#include "Snapshot.h"

#include "Win32Util.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <cstring>

namespace qf::snapshot {

namespace {

// Layout (little endian):
//   magic[8] version:u32
//   volumeCount:u32 { len:u16 root:wchar[len] serial:u32 journalId:u64 usn:i64 }*
//   entryCount:u32 { parent:u32 flags:u8 nameLength:u16 name:u8[nameLength] }*
//   tableCount:u32 { root:u32 count:u32 { record:u32 folder:u32 }* }*   folder record numbers
//   end:u32
// Entries are in pre-order, so a parent always precedes its children.
constexpr char kMagic[8] = {'Q', 'F', 'I', 'N', 'D', 'E', 'X', '\0'};
constexpr std::uint32_t kVersion = 2;
constexpr std::uint32_t kEndMarker = 0x21444E45; // "END!"

class Writer {
public:
    explicit Writer(QIODevice& device)
        : m_device(device)
    {
        m_buffer.reserve(kCapacity);
    }

    template <typename T> void put(T v) { bytes(&v, sizeof v); }
    void bytes(const void* p, std::size_t n)
    {
        if (m_buffer.size() + n > kCapacity)
            flush();
        const auto* c = static_cast<const char*>(p);
        m_buffer.insert(m_buffer.end(), c, c + n);
    }
    void flush()
    {
        if (!m_buffer.empty()
            && m_device.write(m_buffer.data(), static_cast<qint64>(m_buffer.size()))
                != static_cast<qint64>(m_buffer.size()))
            m_ok = false;
        m_buffer.clear();
    }
    bool ok() const noexcept { return m_ok; }

private:
    static constexpr std::size_t kCapacity = 1 << 20;
    QIODevice& m_device;
    std::vector<char> m_buffer;
    bool m_ok = true;
};

class Reader {
public:
    Reader(const uchar* data, std::size_t size)
        : m_p(data)
        , m_end(data + size)
    {
    }
    template <typename T> T get()
    {
        T v {};
        if (m_end - m_p < static_cast<std::ptrdiff_t>(sizeof(T))) {
            m_ok = false;
            return v;
        }
        std::memcpy(&v, m_p, sizeof(T));
        m_p += sizeof(T);
        return v;
    }
    const char* bytes(std::size_t n)
    {
        if (m_end - m_p < static_cast<std::ptrdiff_t>(n)) {
            m_ok = false;
            return nullptr;
        }
        const auto* p = reinterpret_cast<const char*>(m_p);
        m_p += n;
        return p;
    }
    bool ok() const noexcept { return m_ok; }

private:
    const uchar* m_p;
    const uchar* m_end;
    bool m_ok = true;
};

} // namespace

bool save(const FileIndex& index, const std::vector<VolumeInfo>& volumes,
    const std::vector<JournalPosition>& journals, const QString& filePath)
{
    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QSaveFile file(filePath); // atomic replace: a crash never leaves half a snapshot
    if (!file.open(QIODevice::WriteOnly))
        return false;

    Writer w(file);
    w.bytes(kMagic, sizeof kMagic);
    w.put(kVersion);
    w.put(static_cast<std::uint32_t>(volumes.size()));
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        const auto& v = volumes[i];
        const JournalPosition journal = i < journals.size() ? journals[i] : JournalPosition {};
        w.put(static_cast<std::uint16_t>(v.root.size()));
        w.bytes(v.root.data(), v.root.size() * sizeof(wchar_t));
        w.put(v.serial);
        w.put(journal.journalId);
        w.put(journal.usn);
    }
    const auto expected = static_cast<std::uint32_t>(index.liveCount());
    w.put(expected);

    // Pre-order walk from the roots: parents come before children, and
    // deleted entries are unreachable, so the file is compact.
    std::vector<EntryId> newId(index.slotCount(), kNoEntry);
    std::uint32_t next = 0;
    std::vector<EntryId> stack;
    for (const EntryId root : index.roots()) {
        stack.push_back(root);
        while (!stack.empty()) {
            const EntryId id = stack.back();
            stack.pop_back();
            const Entry& e = index.entry(id);
            if (e.isDeleted())
                continue;
            newId[id] = next++;
            w.put(e.parent == kNoEntry ? kNoEntry : newId[e.parent]);
            w.put(static_cast<std::uint8_t>(e.flags & EntryFlag::Persistent));
            const std::string_view name = index.name(e);
            w.put(static_cast<std::uint16_t>(name.size()));
            w.bytes(name.data(), name.size());
            for (EntryId c = e.firstChild; c != kNoEntry; c = index.entry(c).nextSibling)
                stack.push_back(c);
        }
    }

    // Folder record numbers, for the folders that made it into the file.
    const auto saved = [&](EntryId id) { return id < newId.size() && newId[id] != kNoEntry && index.entry(id).isDir(); };
    std::uint32_t tables = 0;
    for (const EntryId root : index.roots())
        tables += index.folderRecords(root) && saved(root) ? 1 : 0;
    w.put(tables);
    for (const EntryId root : index.roots()) {
        const RecordTable* table = index.folderRecords(root);
        if (!table || !saved(root))
            continue;
        std::uint32_t count = 0;
        table->forEach([&](std::uint32_t, EntryId id) { count += saved(id) ? 1 : 0; });
        w.put(newId[root]);
        w.put(count);
        table->forEach([&](std::uint32_t record, EntryId id) {
            if (saved(id)) {
                w.put(record);
                w.put(newId[id]);
            }
        });
    }

    w.put(kEndMarker);
    w.flush();
    if (!w.ok() || next != expected) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

std::unique_ptr<FileIndex> load(const QString& filePath, const std::vector<VolumeInfo>& expectedVolumes,
    std::vector<JournalPosition>* journals)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return nullptr;
    const qint64 size = file.size();
    if (size < 32)
        return nullptr;
    uchar* data = file.map(0, size);
    if (!data)
        return nullptr;

    struct Unmap {
        QFile& f;
        uchar* p;
        ~Unmap() { f.unmap(p); }
    } unmap {file, data};

    Reader r(data, static_cast<std::size_t>(size));
    const char* magic = r.bytes(sizeof kMagic);
    if (!magic || std::memcmp(magic, kMagic, sizeof kMagic) != 0 || r.get<std::uint32_t>() != kVersion)
        return nullptr;

    const auto volumeCount = r.get<std::uint32_t>();
    if (!r.ok() || volumeCount != expectedVolumes.size())
        return nullptr;
    std::vector<JournalPosition> positions(volumeCount);
    for (std::uint32_t i = 0; i < volumeCount; ++i) {
        const auto len = r.get<std::uint16_t>();
        const char* root = r.bytes(len * sizeof(wchar_t));
        const auto serial = r.get<std::uint32_t>();
        positions[i].journalId = r.get<std::uint64_t>();
        positions[i].usn = r.get<std::int64_t>();
        if (!r.ok())
            return nullptr;
        std::wstring rootName(len, L'\0');
        std::memcpy(rootName.data(), root, len * sizeof(wchar_t));
        if (!win32::equalsIgnoreCase(rootName, expectedVolumes[i].root) || serial != expectedVolumes[i].serial)
            return nullptr; // drives changed (reformatted, swapped, ...): rebuild instead
    }

    const auto count = r.get<std::uint32_t>();
    if (!r.ok())
        return nullptr;

    auto index = std::make_unique<FileIndex>();
    index->setInterning(true);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto parent = r.get<EntryId>();
        const auto flags = r.get<std::uint8_t>();
        const auto len = r.get<std::uint16_t>();
        const char* name = r.bytes(len);
        if (!r.ok() || len == 0)
            return nullptr;
        const std::string_view nameView(name, len);
        if (parent == kNoEntry) {
            if (!(flags & EntryFlag::Root))
                return nullptr;
            index->addRoot(nameView);
        } else {
            // Ids are assigned sequentially, so file order == entry id.
            if (parent >= i || index->entry(parent).isDeleted() || !index->entry(parent).isDir())
                return nullptr;
            index->add(parent, nameView, flags);
        }
    }

    const auto tables = r.get<std::uint32_t>();
    for (std::uint32_t t = 0; t < tables && r.ok(); ++t) {
        const auto root = r.get<EntryId>();
        const auto n = r.get<std::uint32_t>();
        if (!r.ok() || root >= count || !(index->entry(root).flags & EntryFlag::Root))
            return nullptr;
        for (std::uint32_t k = 0; k < n; ++k) {
            const auto record = r.get<std::uint32_t>();
            const auto folder = r.get<EntryId>();
            if (!r.ok() || folder >= count || !index->entry(folder).isDir())
                return nullptr;
            index->setFolderRecord(root, record, folder);
        }
    }
    if (r.get<std::uint32_t>() != kEndMarker || !r.ok())
        return nullptr;
    index->setInterning(false);
    if (journals)
        *journals = std::move(positions);
    return index;
}

} // namespace qf::snapshot
