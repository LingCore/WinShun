#include "Snapshot.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <windows.h>

#include <compressapi.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <deque>
#include <future>
#include <optional>
#include <span>
#include <tuple>

namespace ws::snapshot {

namespace {

// Layout (little endian):
//   magic[8] version:u32
//   volumeCount:u32 { len:u16 root:wchar[len] serial:u32 journalId:u64 usn:i64 }*
//   4 x { count:u32 { len:u16 text:wchar[len] }* }   crawl rules (version 3 on)
//   nameCount:u32 { length:u16 name:u8[length] }*   (version 5 on)
//   entryCount:u32 { back:leb flags:u8 name:leb }*
//   tableCount:u32 { root:u32 count:u32 { record:u32 folder:u32 }* }*   folder record numbers
//   attachmentSize:u64 attachment:u8[attachmentSize]   (version 4 on)
//   end:u32
// Entries are in pre-order, so a parent always precedes its children: `back`
// is how many entries before this one it is (0 for a root). `name` is the
// number of the entry's name in the list before them, where each name is
// once (only a third are distinct: index.js, __init__.py...), in the order
// the entries first have them. leb: LEB128. Before version 5 an entry was
// parent:u32 flags:u8 nameLength:u16 name:u8[nameLength].
//
// The file holds that packed: packedMagic[8], then blocks of it, each
// rawSize:u32 packedSize:u32 checksum:u64 data[packedSize] compressed on its
// own (XPRESS with Huffman coding, Windows' Compression API; the checksum is
// of the raw bytes), and a block of sizes 0, 0.
// A third of the size (names compress well), so a third written every time
// the index is saved; blocks are packed and unpacked on other threads while
// the index is walked or rebuilt. Files written before are read as they are.
constexpr char kMagic[8] = {'Q', 'F', 'I', 'N', 'D', 'E', 'X', '\0'};
constexpr char kPackedMagic[8] = {'Q', 'F', 'I', 'N', 'D', 'E', 'X', 'Z'};
constexpr std::uint32_t kVersion = 5;
constexpr std::uint32_t kOldestVersion = 2; // without the rules
constexpr std::uint32_t kEndMarker = 0x21444E45; // "END!"
constexpr std::size_t kBlockSize = 1 << 20;
constexpr std::size_t kMaxBlockSize = 16 << 20; // the most a block in a file may hold
constexpr std::size_t kAhead = 8; // blocks packed or unpacked at a time

struct BlockHeader {
    std::uint32_t rawSize;
    std::uint32_t packedSize;
    std::uint64_t checksum;
};

// Tells a damaged block (the coding has no check of its own).
std::uint64_t checksum(std::span<const char> data) noexcept
{
    std::uint64_t h = 0x9E37'79B9'7F4A'7C15ull ^ data.size();
    std::size_t i = 0;
    for (; i + 8 <= data.size(); i += 8) {
        std::uint64_t v = 0;
        std::memcpy(&v, data.data() + i, 8);
        h = (h ^ v) * 0xBF58'476D'1CE4'E5B9ull;
        h ^= h >> 31;
    }
    std::uint64_t tail = 0;
    std::memcpy(&tail, data.data() + i, data.size() - i);
    h = (h ^ tail) * 0x94D0'49BB'1331'11EBull;
    return h ^ (h >> 29);
}

std::optional<std::vector<char>> pack(std::span<const char> raw)
{
    COMPRESSOR_HANDLE compressor = nullptr;
    if (!::CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &compressor))
        return std::nullopt;
    std::vector<char> out(sizeof(BlockHeader) + raw.size() + raw.size() / 8 + 4096);
    SIZE_T packed = 0;
    const bool ok = ::Compress(compressor, raw.data(), raw.size(), out.data() + sizeof(BlockHeader),
        out.size() - sizeof(BlockHeader), &packed);
    ::CloseCompressor(compressor);
    if (!ok)
        return std::nullopt;
    const BlockHeader header {
        static_cast<std::uint32_t>(raw.size()), static_cast<std::uint32_t>(packed), checksum(raw)};
    std::memcpy(out.data(), &header, sizeof header);
    out.resize(sizeof header + packed);
    return out;
}

std::optional<std::vector<char>> unpack(std::span<const uchar> packed, std::size_t rawSize, std::uint64_t sum)
{
    DECOMPRESSOR_HANDLE decompressor = nullptr;
    if (!::CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &decompressor))
        return std::nullopt;
    std::vector<char> out(rawSize);
    SIZE_T size = 0;
    const bool ok = ::Decompress(decompressor, packed.data(), packed.size(), out.data(), out.size(), &size);
    ::CloseDecompressor(decompressor);
    if (!ok || size != rawSize || checksum(out) != sum)
        return std::nullopt;
    return out;
}

// Gathers what is written into blocks and packs them on other threads,
// writing them to the device in order.
class Writer {
public:
    explicit Writer(QIODevice& device)
        : m_device(device)
    {
        m_ok = write(kPackedMagic, sizeof kPackedMagic);
        m_buffer.reserve(kBlockSize);
    }

    template <typename T> void put(T v) { bytes(&v, sizeof v); }
    void leb(std::uint64_t v)
    {
        char out[10];
        std::size_t n = 0;
        for (; v >= 0x80; v >>= 7)
            out[n++] = static_cast<char>(v | 0x80);
        out[n++] = static_cast<char>(v);
        bytes(out, n);
    }
    void bytes(const void* p, std::size_t n)
    {
        const auto* c = static_cast<const char*>(p);
        while (n > 0) {
            const std::size_t room = kBlockSize - m_buffer.size();
            const std::size_t take = std::min(room, n);
            m_buffer.insert(m_buffer.end(), c, c + take);
            c += take;
            n -= take;
            if (m_buffer.size() == kBlockSize)
                flush();
        }
    }
    // Packs what is left, writes every block and the end.
    bool finish()
    {
        flush();
        while (!m_pending.empty())
            writeFront();
        const BlockHeader end {0, 0, 0};
        return m_ok && write(&end, sizeof end);
    }

private:
    void flush()
    {
        if (m_buffer.empty())
            return;
        if (m_pending.size() >= kAhead)
            writeFront();
        m_pending.push_back(std::async(std::launch::async, [block = std::move(m_buffer)] { return pack(block); }));
        m_buffer = {};
        m_buffer.reserve(kBlockSize);
    }
    void writeFront()
    {
        const std::optional<std::vector<char>> packed = m_pending.front().get();
        m_pending.pop_front();
        m_ok = m_ok && packed && write(packed->data(), packed->size());
    }
    bool write(const void* data, std::size_t size)
    {
        return m_device.write(static_cast<const char*>(data), static_cast<qint64>(size)) == static_cast<qint64>(size);
    }

    QIODevice& m_device;
    std::vector<char> m_buffer;
    std::deque<std::future<std::optional<std::vector<char>>>> m_pending;
    bool m_ok = true;
};

// The blocks of a packed file, unpacked a few ahead on other threads.
class Unpacker {
public:
    explicit Unpacker(std::span<const uchar> blocks) // after the magic
        : m_rest(blocks)
    {
        while (m_ahead.size() < kAhead && launch()) {}
    }
    // The next block; false at the end, or when the file is damaged.
    bool next(std::vector<char>& out)
    {
        if (m_ahead.empty())
            return false;
        std::optional<std::vector<char>> block = m_ahead.front().get();
        m_ahead.pop_front();
        if (!block) {
            m_ahead.clear(); // waits for the others
            return false;
        }
        out = std::move(*block);
        launch();
        return true;
    }
    // Every block was read, and then the end: the file is whole.
    bool atEnd() const noexcept { return m_ahead.empty() && m_ended; }

private:
    bool launch()
    {
        BlockHeader header {};
        if (m_rest.size() < sizeof header)
            return false;
        std::memcpy(&header, m_rest.data(), sizeof header);
        m_rest = m_rest.subspan(sizeof header);
        if (header.rawSize == 0 && header.packedSize == 0) {
            m_ended = m_rest.empty();
            return false;
        }
        if (header.rawSize == 0 || header.rawSize > kMaxBlockSize || header.packedSize > m_rest.size()) {
            m_rest = {};
            return false;
        }
        const std::span<const uchar> packed = m_rest.first(header.packedSize);
        m_rest = m_rest.subspan(header.packedSize);
        m_ahead.push_back(std::async(std::launch::async,
            [packed, size = header.rawSize, sum = header.checksum] { return unpack(packed, size, sum); }));
        return true;
    }

    std::span<const uchar> m_rest;
    std::deque<std::future<std::optional<std::vector<char>>>> m_ahead;
    bool m_ended = false;
};

// Reads a file as it is, or a packed one as its blocks come.
class Reader {
public:
    Reader(const uchar* data, std::size_t size)
        : m_p(reinterpret_cast<const char*>(data))
        , m_end(m_p + size)
    {
    }
    explicit Reader(Unpacker& blocks)
        : m_blocks(&blocks)
    {
    }

    template <typename T> T get()
    {
        T v {};
        if (const char* p = bytes(sizeof(T)))
            std::memcpy(&v, p, sizeof(T));
        return v;
    }
    std::uint64_t leb()
    {
        std::uint64_t v = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            const char* p = bytes(1);
            if (!p)
                return 0;
            const auto b = static_cast<unsigned char>(*p);
            v |= std::uint64_t {b & 0x7Fu} << shift;
            if (!(b & 0x80))
                return v;
        }
        m_ok = false;
        return 0;
    }
    std::wstring wide()
    {
        const auto len = get<std::uint16_t>();
        const char* p = bytes(len * sizeof(wchar_t));
        std::wstring s;
        if (p) {
            s.resize(len);
            std::memcpy(s.data(), p, len * sizeof(wchar_t));
        }
        return s;
    }
    // n bytes in a row; null (and failed) past the end.
    const char* bytes(std::size_t n)
    {
        if (static_cast<std::size_t>(m_end - m_p) < n && !fill(n)) {
            m_ok = false;
            m_p = m_end;
            return nullptr;
        }
        const char* p = m_p;
        m_p += n;
        return p;
    }
    bool ok() const noexcept { return m_ok; }

private:
    // Makes n bytes from the current position contiguous: what is left of
    // the window, then as many blocks as it takes.
    bool fill(std::size_t n)
    {
        if (!m_blocks)
            return false;
        m_window.erase(m_window.begin(), m_window.begin() + (m_p - m_window.data()));
        while (m_window.size() < n) {
            if (!m_blocks->next(m_block))
                return false;
            m_window.insert(m_window.end(), m_block.begin(), m_block.end());
        }
        m_p = m_window.data();
        m_end = m_p + m_window.size();
        return true;
    }

    const char* m_p = nullptr;
    const char* m_end = nullptr;
    Unpacker* m_blocks = nullptr;
    std::vector<char> m_window; // a packed file's bytes from the current position on
    std::vector<char> m_block;
    bool m_ok = true;
};

// Numbers for names, by where the index stores them (entries with the same
// name mostly share its storage): open addressing over (offset + 1) << 32 |
// number.
class NameNumbers {
public:
    explicit NameNumbers(std::size_t expected)
        : m_slots(std::bit_ceil(std::max<std::size_t>(expected, 1024)), 0)
    {
    }
    // The name's number, and whether it got it just now.
    std::pair<std::uint32_t, bool> number(std::uint32_t offset)
    {
        std::size_t s = slotOf(offset);
        if (m_slots[s] != 0)
            return {static_cast<std::uint32_t>(m_slots[s]), false};
        if ((m_count + std::size_t {1}) * 4 > m_slots.size() * 3) {
            std::vector<std::uint64_t> old(m_slots.size() * 2, 0);
            old.swap(m_slots);
            for (const std::uint64_t slot : old) {
                if (slot != 0)
                    m_slots[slotOf(static_cast<std::uint32_t>((slot >> 32) - 1))] = slot;
            }
            s = slotOf(offset);
        }
        m_slots[s] = (std::uint64_t {offset} + 1) << 32 | m_count;
        return {m_count++, true};
    }

private:
    std::size_t slotOf(std::uint32_t offset) const noexcept
    {
        const std::uint64_t key = std::uint64_t {offset} + 1;
        const std::size_t mask = m_slots.size() - 1;
        auto s = static_cast<std::size_t>((key * 0x9E37'79B9'7F4A'7C15ull) >> 32) & mask;
        while (m_slots[s] != 0 && (m_slots[s] >> 32) != key)
            s = (s + 1) & mask;
        return s;
    }

    std::vector<std::uint64_t> m_slots;
    std::uint32_t m_count = 0;
};

// The four lists of CrawlRules, in a fixed order.
template <typename Rules, typename F> void forEachList(Rules& rules, F&& f)
{
    f(rules.excludedPaths);
    f(rules.excludedNames);
    f(rules.lowPriorityPaths);
    f(rules.lowPriorityNames);
}

} // namespace

bool save(const FileIndex& index, const std::vector<VolumeInfo>& volumes,
    const std::vector<JournalPosition>& journals, const CrawlRules& rules, const QString& filePath,
    const Attachment& attachment)
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
    forEachList(rules, [&](const std::vector<std::wstring>& list) {
        w.put(static_cast<std::uint32_t>(list.size()));
        for (const std::wstring& s : list) {
            const auto len = static_cast<std::uint16_t>(std::min<std::size_t>(s.size(), 0xFFFF));
            w.put(len);
            w.bytes(s.data(), len * sizeof(wchar_t));
        }
    });
    // Pre-order walks from the roots: parents come before children, and
    // deleted entries are unreachable, so the file is compact.
    const auto forEachEntry = [&](auto&& f) {
        std::vector<EntryId> stack;
        for (const EntryId root : index.roots()) {
            stack.push_back(root);
            while (!stack.empty()) {
                const EntryId id = stack.back();
                stack.pop_back();
                const Entry& e = index.entry(id);
                if (e.isDeleted())
                    continue;
                f(id, e);
                for (EntryId c = e.firstChild; c != kNoEntry; c = index.entry(c).nextSibling)
                    stack.push_back(c);
            }
        }
    };
    std::vector<EntryId> newId(index.slotCount(), kNoEntry);
    std::uint32_t next = 0;
    NameNumbers numbers(index.liveCount() / 2);
    std::vector<const Entry*> named; // an entry with each name, by number
    forEachEntry([&](EntryId id, const Entry& e) {
        newId[id] = next++;
        if (numbers.number(e.nameOffset).second)
            named.push_back(&e);
    });
    w.put(static_cast<std::uint32_t>(named.size()));
    for (const Entry* e : named) {
        const std::string_view name = index.name(*e);
        w.put(static_cast<std::uint16_t>(name.size()));
        w.bytes(name.data(), name.size());
    }
    named = {};
    w.put(next);
    forEachEntry([&](EntryId id, const Entry& e) {
        w.leb(e.parent == kNoEntry ? 0 : newId[id] - newId[e.parent]);
        w.put(static_cast<std::uint8_t>(e.flags & EntryFlag::Persistent));
        w.leb(numbers.number(e.nameOffset).first);
    });

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

    const std::vector<char> attached = attachment ? attachment(newId) : std::vector<char>();
    w.put(static_cast<std::uint64_t>(attached.size()));
    w.bytes(attached.data(), attached.size());

    w.put(kEndMarker);
    if (!w.finish() || next != index.liveCount()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

std::optional<Contents> load(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const qint64 size = file.size();
    if (size < 32)
        return std::nullopt;
    uchar* data = file.map(0, size);
    if (!data)
        return std::nullopt;

    struct Unmap {
        QFile& f;
        uchar* p;
        ~Unmap() { f.unmap(p); }
    } unmap {file, data};

    const auto bytes = static_cast<std::size_t>(size);
    const bool packed = std::memcmp(data, kPackedMagic, sizeof kPackedMagic) == 0;
    std::optional<Unpacker> blocks;
    if (packed)
        blocks.emplace(std::span<const uchar>(data + sizeof kPackedMagic, bytes - sizeof kPackedMagic));
    Reader r = packed ? Reader(*blocks) : Reader(data, bytes);
    const char* magic = r.bytes(sizeof kMagic);
    const auto version = r.get<std::uint32_t>();
    if (!magic || std::memcmp(magic, kMagic, sizeof kMagic) != 0 || version < kOldestVersion || version > kVersion)
        return std::nullopt;

    Contents contents;
    const auto volumeCount = r.get<std::uint32_t>();
    if (!r.ok() || volumeCount > 26)
        return std::nullopt;
    for (std::uint32_t i = 0; i < volumeCount; ++i) {
        VolumeInfo volume;
        volume.root = r.wide();
        volume.serial = r.get<std::uint32_t>();
        JournalPosition journal;
        journal.journalId = r.get<std::uint64_t>();
        journal.usn = r.get<std::int64_t>();
        if (!r.ok())
            return std::nullopt;
        contents.volumes.push_back(std::move(volume));
        contents.journals.push_back(journal);
    }
    if (version >= 3) {
        CrawlRules rules;
        forEachList(rules, [&](std::vector<std::wstring>& list) {
            const auto n = r.get<std::uint32_t>();
            for (std::uint32_t k = 0; k < n && r.ok(); ++k)
                list.push_back(r.wide());
        });
        if (!r.ok())
            return std::nullopt;
        contents.rules = std::move(rules);
    }

    auto index = std::make_unique<FileIndex>();
    // Version 5 on: the names once each, stored as they come.
    std::vector<std::pair<std::uint32_t, std::uint16_t>> names; // offset, length
    if (version >= 5) {
        const auto nameCount = r.get<std::uint32_t>();
        if (!r.ok() || nameCount > (1u << 28))
            return std::nullopt;
        names.resize(nameCount);
        for (auto& [offset, length] : names) {
            length = r.get<std::uint16_t>();
            const char* name = r.bytes(length);
            if (!r.ok() || length == 0)
                return std::nullopt;
            offset = index->storeName({name, length});
        }
    } else {
        index->setInterning(true); // the same name comes many times
    }
    const auto count = r.get<std::uint32_t>();
    if (!r.ok())
        return std::nullopt;
    for (std::uint32_t i = 0; i < count; ++i) {
        EntryId parent = kNoEntry;
        std::uint8_t flags = 0;
        std::uint32_t offset = 0;
        std::uint16_t length = 0;
        if (version >= 5) {
            const std::uint64_t back = r.leb();
            flags = r.get<std::uint8_t>();
            const std::uint64_t number = r.leb();
            if (!r.ok() || back > i || number >= names.size())
                return std::nullopt;
            parent = back == 0 ? kNoEntry : static_cast<EntryId>(i - back);
            std::tie(offset, length) = names[number];
        } else {
            parent = r.get<EntryId>();
            flags = r.get<std::uint8_t>();
            length = r.get<std::uint16_t>();
            const char* name = r.bytes(length);
            if (!r.ok() || length == 0)
                return std::nullopt;
            offset = index->storeName({name, length});
        }
        if (parent == kNoEntry) {
            if (!(flags & EntryFlag::Root))
                return std::nullopt;
            index->addStored(kNoEntry, offset, length, flags);
        } else {
            // Ids are assigned sequentially, so file order == entry id.
            if (parent >= i || index->entry(parent).isDeleted() || !index->entry(parent).isDir())
                return std::nullopt;
            index->addStored(parent, offset, length, flags);
        }
    }

    const auto tables = r.get<std::uint32_t>();
    for (std::uint32_t t = 0; t < tables && r.ok(); ++t) {
        const auto root = r.get<EntryId>();
        const auto n = r.get<std::uint32_t>();
        if (!r.ok() || root >= count || !(index->entry(root).flags & EntryFlag::Root))
            return std::nullopt;
        for (std::uint32_t k = 0; k < n; ++k) {
            const auto record = r.get<std::uint32_t>();
            const auto folder = r.get<EntryId>();
            if (!r.ok() || folder >= count || !index->entry(folder).isDir())
                return std::nullopt;
            index->setFolderRecord(root, record, folder);
        }
    }
    if (version >= 4) {
        const auto attachedSize = static_cast<std::size_t>(r.get<std::uint64_t>());
        const char* attached = r.ok() ? r.bytes(attachedSize) : nullptr;
        if (!attached)
            return std::nullopt;
        contents.attachment.assign(attached, attached + attachedSize);
    }
    if (r.get<std::uint32_t>() != kEndMarker || !r.ok() || (blocks && !blocks->atEnd()))
        return std::nullopt;
    index->setInterning(false);
    contents.index = std::move(index);
    return contents;
}

} // namespace ws::snapshot
