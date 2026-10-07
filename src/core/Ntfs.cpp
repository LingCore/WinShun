#include "Ntfs.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>

namespace qf::ntfs {

namespace {

template <typename T> T load(const std::byte* p) noexcept
{
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

// File record layout (offsets into the record).
constexpr std::uint32_t kFileSignature = 0x454C4946; // "FILE"
constexpr std::size_t kUsaOffset = 0x04;
constexpr std::size_t kUsaCount = 0x06;
constexpr std::size_t kFirstAttribute = 0x14;
constexpr std::size_t kRecordFlags = 0x16;
constexpr std::size_t kBytesInUse = 0x18;
constexpr std::size_t kBaseRecord = 0x20;
constexpr std::uint16_t kInUse = 0x0001;
constexpr std::uint16_t kIsDirectory = 0x0002;
constexpr std::size_t kStride = 512; // update sequence stride, whatever the sector size

// Attribute types, and the fields we read.
constexpr std::uint32_t kStandardInformation = 0x10;
constexpr std::uint32_t kAttributeList = 0x20;
constexpr std::uint32_t kFileName = 0x30;
constexpr std::uint32_t kData = 0x80;
constexpr std::uint32_t kEndOfAttributes = 0xFFFF'FFFF;
constexpr std::uint8_t kDosNamespace = 2; // the 8.3 alias of a long name

constexpr std::size_t kBlockBytes = 4u << 20;
constexpr DWORD kJournalBufferBytes = 128 * 1024;
constexpr DWORD kReasonMask = USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE | USN_REASON_RENAME_OLD_NAME
    | USN_REASON_RENAME_NEW_NAME | USN_REASON_BASIC_INFO_CHANGE | USN_REASON_HARD_LINK_CHANGE;

HANDLE openVolume(std::wstring_view root, DWORD access, DWORD flags)
{
    const std::wstring path = L"\\\\.\\" + std::wstring(root);
    const HANDLE h = ::CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, flags, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}

void closeHandle(void*& h)
{
    if (h)
        ::CloseHandle(h);
    h = nullptr;
}

// Every 512-byte stride of a record ends with a check value; the bytes it
// replaced are kept in the update sequence array. A mismatch means the
// record was torn while being written.
bool applyFixups(std::byte* data, std::size_t size)
{
    if (size < 0x30 || load<std::uint32_t>(data) != kFileSignature)
        return false;
    const auto usaOffset = load<std::uint16_t>(data + kUsaOffset);
    const auto usaCount = load<std::uint16_t>(data + kUsaCount);
    if (usaCount < 2 || (usaCount - 1u) * kStride > size || usaOffset + usaCount * 2u > size)
        return false;
    const auto check = load<std::uint16_t>(data + usaOffset);
    for (std::size_t i = 1; i < usaCount; ++i) {
        std::byte* tail = data + i * kStride - 2;
        if (load<std::uint16_t>(tail) != check)
            return false;
        std::memcpy(tail, data + usaOffset + i * 2, 2);
    }
    return true;
}

// Calls f(type, header, length) for each attribute of a fixed-up record.
template <typename F> bool forEachAttribute(const std::byte* data, std::size_t size, F&& f)
{
    const std::size_t used = std::min<std::size_t>(load<std::uint32_t>(data + kBytesInUse), size);
    std::size_t pos = load<std::uint16_t>(data + kFirstAttribute);
    while (pos + 4 <= used) {
        const auto type = load<std::uint32_t>(data + pos);
        if (type == kEndOfAttributes)
            break;
        const auto length = pos + 8 <= used ? load<std::uint32_t>(data + pos + 4) : 0;
        if (length < 16 || length > used - pos)
            return false;
        f(type, data + pos, static_cast<std::size_t>(length));
        pos += length;
    }
    return true;
}

// The value of a resident attribute, or an empty span.
std::span<const std::byte> residentValue(const std::byte* attribute, std::size_t length)
{
    if (length < 0x18 || attribute[8] != std::byte {0})
        return {};
    const auto valueLength = load<std::uint32_t>(attribute + 0x10);
    const auto valueOffset = load<std::uint16_t>(attribute + 0x14);
    if (valueOffset > length || valueLength > length - valueOffset)
        return {};
    return {attribute + valueOffset, valueLength};
}

} // namespace

bool parseFileRecord(std::byte* data, std::size_t size, FileRecord& out)
{
    out.inUse = false;
    out.directory = false;
    out.baseRecord = 0;
    out.attributes = 0;
    out.hasAttributeList = false;
    out.names.clear();
    if (!applyFixups(data, size))
        return false;
    const auto flags = load<std::uint16_t>(data + kRecordFlags);
    out.inUse = flags & kInUse;
    out.directory = flags & kIsDirectory;
    out.baseRecord = recordOf(load<std::uint64_t>(data + kBaseRecord));
    if (!out.inUse)
        return true;
    return forEachAttribute(data, size, [&](std::uint32_t type, const std::byte* attribute, std::size_t length) {
        if (type == kAttributeList) {
            out.hasAttributeList = true;
            return;
        }
        const auto value = residentValue(attribute, length);
        if (type == kStandardInformation && value.size() >= 0x24) {
            out.attributes = load<std::uint32_t>(value.data() + 0x20);
        } else if (type == kFileName && value.size() >= 0x42) {
            const auto nameLength = static_cast<std::uint8_t>(value[0x40]);
            const auto nameSpace = static_cast<std::uint8_t>(value[0x41]);
            if (nameSpace == kDosNamespace || nameLength == 0 || 0x42 + nameLength * 2u > value.size())
                return;
            out.names.push_back({load<std::uint64_t>(value.data()),
                {reinterpret_cast<const char16_t*>(value.data() + 0x42), nameLength}});
        }
    });
}

std::optional<std::vector<Extent>> decodeRunList(std::span<const std::byte> pairs, std::uint64_t clusterSize)
{
    std::vector<Extent> extents;
    std::int64_t lcn = 0;
    std::size_t i = 0;
    while (i < pairs.size()) {
        const auto header = static_cast<std::uint8_t>(pairs[i++]);
        if (header == 0)
            return extents;
        const unsigned lengthBytes = header & 0x0F;
        const unsigned offsetBytes = header >> 4;
        // A run without an offset is sparse: never part of the MFT.
        if (lengthBytes == 0 || lengthBytes > 8 || offsetBytes == 0 || offsetBytes > 8
            || lengthBytes + offsetBytes > pairs.size() - i)
            return std::nullopt;
        std::uint64_t length = 0;
        for (unsigned b = 0; b < lengthBytes; ++b)
            length |= std::uint64_t {static_cast<std::uint8_t>(pairs[i + b])} << (8 * b);
        i += lengthBytes;
        std::uint64_t delta = 0;
        for (unsigned b = 0; b < offsetBytes; ++b)
            delta |= std::uint64_t {static_cast<std::uint8_t>(pairs[i + b])} << (8 * b);
        if (offsetBytes < 8 && (static_cast<std::uint8_t>(pairs[i + offsetBytes - 1]) & 0x80))
            delta |= ~std::uint64_t {0} << (8 * offsetBytes); // relative to the previous run: signed
        i += offsetBytes;
        lcn += static_cast<std::int64_t>(delta);
        if (lcn < 0 || length == 0)
            return std::nullopt;
        extents.push_back({static_cast<std::uint64_t>(lcn) * clusterSize, length * clusterSize});
    }
    return std::nullopt; // no end marker
}

// ---- MftReader --------------------------------------------------------------

MftReader::MftReader(std::wstring_view root)
{
    // Unbuffered: the MFT is read once, so it should not crowd the file cache.
    m_volume = openVolume(root, GENERIC_READ, FILE_FLAG_NO_BUFFERING);
    if (!m_volume) {
        fail(L"open volume");
        return;
    }
    NTFS_VOLUME_DATA_BUFFER data {};
    DWORD bytes = 0;
    if (!::DeviceIoControl(m_volume, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &data, sizeof data, &bytes, nullptr)) {
        fail(L"FSCTL_GET_NTFS_VOLUME_DATA");
        return;
    }
    m_recordSize = data.BytesPerFileRecordSegment;
    m_clusterSize = data.BytesPerCluster;
    m_validBytes = static_cast<std::uint64_t>(data.MftValidDataLength.QuadPart);
    // Records never straddle extents only when a cluster holds whole records.
    if (m_recordSize < kStride || (m_recordSize & (m_recordSize - 1)) != 0 || m_clusterSize < m_recordSize
        || kBlockBytes % m_clusterSize != 0) {
        m_error = L"unsupported record or cluster size";
        return;
    }
    m_buffer = static_cast<std::byte*>(::VirtualAlloc(nullptr, kBlockBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!m_buffer) {
        fail(L"VirtualAlloc");
        return;
    }
    if (!findExtents(root, static_cast<std::uint64_t>(data.MftStartLcn.QuadPart), data.BytesPerSector))
        return;
}

MftReader::~MftReader()
{
    if (m_buffer)
        ::VirtualFree(m_buffer, 0, MEM_RELEASE);
    closeHandle(m_volume);
}

void MftReader::fail(const wchar_t* what)
{
    m_error = std::wstring(what) + L" failed, error " + std::to_wstring(::GetLastError());
}

bool MftReader::findExtents(std::wstring_view root, std::uint64_t mftStart, std::uint32_t sectorSize)
{
    // Where the MFT lies on the volume: ask the file system...
    const std::wstring mftPath = L"\\\\?\\" + std::wstring(root) + L"\\$MFT";
    if (const HANDLE mft = ::CreateFileW(mftPath.c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
        mft != INVALID_HANDLE_VALUE) {
        std::vector<std::uint64_t> buffer(8192); // RETRIEVAL_POINTERS_BUFFER, 64 KB
        STARTING_VCN_INPUT_BUFFER in {};
        bool ok = true;
        for (;;) {
            DWORD bytes = 0;
            const BOOL done = ::DeviceIoControl(mft, FSCTL_GET_RETRIEVAL_POINTERS, &in, sizeof in, buffer.data(),
                static_cast<DWORD>(buffer.size() * sizeof(std::uint64_t)), &bytes, nullptr);
            if (!done && ::GetLastError() != ERROR_MORE_DATA) {
                ok = false;
                break;
            }
            const auto* rp = reinterpret_cast<const RETRIEVAL_POINTERS_BUFFER*>(buffer.data());
            std::int64_t vcn = rp->StartingVcn.QuadPart;
            for (DWORD k = 0; k < rp->ExtentCount; ++k) {
                const std::int64_t nextVcn = rp->Extents[k].NextVcn.QuadPart;
                const std::int64_t lcn = rp->Extents[k].Lcn.QuadPart;
                if (lcn < 0 || nextVcn <= vcn) {
                    ok = false;
                    break;
                }
                m_extents.push_back({static_cast<std::uint64_t>(lcn) * m_clusterSize,
                    static_cast<std::uint64_t>(nextVcn - vcn) * m_clusterSize});
                vcn = nextVcn;
            }
            if (!ok || done || rp->ExtentCount == 0)
                break;
            in.StartingVcn.QuadPart = vcn;
        }
        ::CloseHandle(mft);
        if (ok && !m_extents.empty())
            return true;
        m_extents.clear();
    }

    // ...or read it from the MFT's own record (record 0), at its known start.
    m_fromRecordZero = true;
    const std::size_t readBytes = (m_recordSize + sectorSize - 1) / sectorSize * sectorSize;
    OVERLAPPED at {};
    const std::uint64_t offset = mftStart * m_clusterSize;
    at.Offset = static_cast<DWORD>(offset);
    at.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD read = 0;
    if (!::ReadFile(m_volume, m_buffer, static_cast<DWORD>(readBytes), &read, &at) || read < m_recordSize) {
        fail(L"reading MFT record 0");
        return false;
    }
    if (!applyFixups(m_buffer, m_recordSize)) {
        m_error = L"MFT record 0 is damaged";
        return false;
    }
    std::optional<std::vector<Extent>> runs;
    std::uint64_t clusters = 0;
    forEachAttribute(m_buffer, m_recordSize, [&](std::uint32_t type, const std::byte* attribute, std::size_t length) {
        // The unnamed, non-resident $DATA attribute that starts at the first cluster.
        if (type != kData || runs || length < 0x40 || attribute[8] == std::byte {0} || attribute[9] != std::byte {0}
            || load<std::uint64_t>(attribute + 0x10) != 0)
            return;
        const auto pairsOffset = load<std::uint16_t>(attribute + 0x20);
        if (pairsOffset >= length)
            return;
        clusters = load<std::uint64_t>(attribute + 0x18) + 1; // highest VCN + 1
        runs = decodeRunList({attribute + pairsOffset, length - pairsOffset}, m_clusterSize);
    });
    // A very fragmented MFT continues its run list in other records: not handled here.
    if (!runs || clusters * m_clusterSize < m_validBytes) {
        m_error = L"cannot locate the MFT";
        return false;
    }
    m_extents = std::move(*runs);
    return true;
}

bool MftReader::next()
{
    m_blockRecords = 0;
    while (valid() && m_consumed < m_validBytes && m_extent < m_extents.size()) {
        const Extent& e = m_extents[m_extent];
        if (m_extentPos >= e.length) {
            ++m_extent;
            m_extentPos = 0;
            continue;
        }
        // Whole clusters only (unbuffered reads), stopping near the valid end.
        const std::uint64_t wanted = (m_validBytes - m_consumed + m_clusterSize - 1) / m_clusterSize * m_clusterSize;
        const auto bytes = static_cast<DWORD>(std::min({std::uint64_t {kBlockBytes}, e.length - m_extentPos, wanted}));
        OVERLAPPED at {};
        const std::uint64_t offset = e.offset + m_extentPos;
        at.Offset = static_cast<DWORD>(offset);
        at.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD read = 0;
        if (!::ReadFile(m_volume, m_buffer, bytes, &read, &at) || read == 0) {
            fail(L"reading the MFT");
            return false;
        }
        m_firstRecord = static_cast<std::uint32_t>(m_consumed / m_recordSize);
        m_blockRecords = static_cast<std::size_t>(std::min<std::uint64_t>(read, m_validBytes - m_consumed) / m_recordSize);
        m_consumed += read;
        m_extentPos += read;
        return true;
    }
    return false;
}

bool MftReader::parse(std::size_t i, FileRecord& out)
{
    return i < m_blockRecords && parseFileRecord(m_buffer + i * m_recordSize, m_recordSize, out);
}

// ---- change journal ----------------------------------------------------------

std::optional<JournalInfo> queryJournal(std::wstring_view root)
{
    const auto query = [&]() -> std::optional<JournalInfo> {
        void* volume = openVolume(root, GENERIC_READ, 0);
        if (!volume)
            return std::nullopt;
        USN_JOURNAL_DATA_V0 data {};
        DWORD bytes = 0;
        const BOOL ok = ::DeviceIoControl(volume, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &data, sizeof data, &bytes, nullptr);
        const DWORD error = ::GetLastError();
        closeHandle(volume);
        if (!ok) {
            ::SetLastError(error);
            return std::nullopt;
        }
        return JournalInfo {data.UsnJournalID, std::max(data.FirstUsn, data.LowestValidUsn), data.NextUsn};
    };
    if (auto info = query())
        return info;
    if (::GetLastError() != ERROR_JOURNAL_NOT_ACTIVE)
        return std::nullopt;
    // No journal yet (rare on data volumes): create one with Windows' default size.
    void* volume = openVolume(root, GENERIC_READ | GENERIC_WRITE, 0);
    if (!volume)
        return std::nullopt;
    CREATE_USN_JOURNAL_DATA create {32ull << 20, 8ull << 20};
    DWORD bytes = 0;
    const BOOL created
        = ::DeviceIoControl(volume, FSCTL_CREATE_USN_JOURNAL, &create, sizeof create, nullptr, 0, &bytes, nullptr);
    closeHandle(volume);
    return created ? query() : std::nullopt;
}

std::int64_t parseUsnRecords(std::span<const std::byte> data, std::vector<UsnRecord>& out)
{
    if (data.size() < sizeof(USN))
        return -1;
    const auto next = load<std::int64_t>(data.data());
    std::size_t pos = sizeof(USN);
    constexpr std::size_t kHeader = 60; // USN_RECORD_V2 up to FileName
    while (data.size() - pos >= kHeader) {
        const std::byte* r = data.data() + pos;
        const auto length = load<std::uint32_t>(r);
        if (length < kHeader || length > data.size() - pos)
            return -1;
        if (load<std::uint16_t>(r + 4) == 2) {
            const auto nameLength = load<std::uint16_t>(r + 56);
            const auto nameOffset = load<std::uint16_t>(r + 58);
            if (std::uint32_t {nameOffset} + nameLength <= length && nameLength % 2 == 0) {
                out.push_back({load<std::uint64_t>(r + 8), load<std::uint64_t>(r + 16), load<std::int64_t>(r + 24),
                    load<std::uint32_t>(r + 40), load<std::uint32_t>(r + 52),
                    {reinterpret_cast<const char16_t*>(r + nameOffset), nameLength / 2u}});
            }
        }
        pos += length;
    }
    return next;
}

JournalReader::JournalReader(std::wstring_view root, std::uint64_t journalId)
    : m_journalId(journalId)
    , m_buffer(kJournalBufferBytes / sizeof(std::uint64_t))
    , m_overlapped(std::make_unique<OVERLAPPED>())
{
    m_volume = openVolume(root, GENERIC_READ, FILE_FLAG_OVERLAPPED);
    m_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

JournalReader::~JournalReader()
{
    cancel();
    closeHandle(m_volume);
    closeHandle(m_event);
}

bool JournalReader::valid() const noexcept
{
    return m_volume && m_event;
}

void JournalReader::start(std::int64_t usn, bool wait)
{
    cancel();
    READ_USN_JOURNAL_DATA_V0 in {};
    in.StartUsn = usn;
    in.ReasonMask = kReasonMask;
    in.ReturnOnlyOnClose = FALSE; // rename records carry the old name only before the close
    in.Timeout = 0;
    in.BytesToWaitFor = wait ? 1 : 0;
    in.UsnJournalID = m_journalId;
    ::ResetEvent(m_event);
    *m_overlapped = OVERLAPPED {};
    m_overlapped->hEvent = m_event;
    m_startError = 0;
    if (::DeviceIoControl(m_volume, FSCTL_READ_USN_JOURNAL, &in, sizeof in, m_buffer.data(), kJournalBufferBytes,
            nullptr, m_overlapped.get())
        || ::GetLastError() == ERROR_IO_PENDING) {
        m_pending = true;
        return;
    }
    m_startError = ::GetLastError();
    ::SetEvent(m_event); // finish() reports the error
}

bool JournalReader::finish(std::vector<UsnRecord>& records, std::int64_t& nextUsn)
{
    records.clear();
    if (!m_pending)
        return false;
    m_pending = false;
    DWORD bytes = 0;
    if (!::GetOverlappedResult(m_volume, m_overlapped.get(), &bytes, TRUE))
        return false;
    const std::int64_t next = parseUsnRecords({reinterpret_cast<const std::byte*>(m_buffer.data()), bytes}, records);
    if (next < 0)
        return false;
    nextUsn = next;
    return true;
}

void JournalReader::cancel()
{
    if (!m_pending)
        return;
    ::CancelIoEx(m_volume, m_overlapped.get());
    DWORD ignored = 0;
    ::GetOverlappedResult(m_volume, m_overlapped.get(), &ignored, TRUE);
    m_pending = false;
}

} // namespace qf::ntfs
