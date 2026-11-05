#include "NtfsBulk.h"

#include <windows.h>
#include <winioctl.h>

#include <aclapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace ws::ntfs {

namespace {

template <typename T> T load(const std::byte* p) noexcept
{
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

// File record layout (as Ntfs.cpp has it; the few more fields read here).
constexpr std::uint32_t kFileSignature = 0x454C4946; // "FILE"
constexpr std::size_t kLsn = 0x08;
constexpr std::size_t kSequence = 0x10;
constexpr std::size_t kLinks = 0x12;
constexpr std::size_t kFirstAttribute = 0x14;
constexpr std::size_t kRecordFlags = 0x16;
constexpr std::size_t kBytesInUse = 0x18;
constexpr std::size_t kBaseRecord = 0x20;
constexpr std::uint16_t kInUse = 0x0001;
constexpr std::uint16_t kIsDirectory = 0x0002;

constexpr std::uint32_t kStandardInformation = 0x10;
constexpr std::uint32_t kAttributeList = 0x20;
constexpr std::uint32_t kFileName = 0x30;
constexpr std::uint32_t kSecurityDescriptor = 0x50;
constexpr std::uint8_t kDosNamespace = 2;
constexpr std::uint32_t kData = 0x80;
constexpr std::uint32_t kReparsePoint = 0xC0;
constexpr std::uint32_t kEndOfAttributes = 0xFFFF'FFFF;

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

std::uint64_t roundUp(std::uint64_t v, std::uint64_t to) noexcept
{
    return (v + to - 1) / to * to;
}

double msSince(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

} // namespace

std::optional<std::vector<DataRun>> decodeDataRuns(std::span<const std::byte> pairs)
{
    std::vector<DataRun> runs;
    std::int64_t lcn = 0;
    std::size_t i = 0;
    while (i < pairs.size()) {
        const auto header = static_cast<std::uint8_t>(pairs[i++]);
        if (header == 0)
            return runs;
        const unsigned lengthBytes = header & 0x0F;
        const unsigned offsetBytes = header >> 4;
        if (lengthBytes == 0 || lengthBytes > 8 || offsetBytes > 8 || lengthBytes + offsetBytes > pairs.size() - i)
            return std::nullopt;
        std::uint64_t length = 0;
        for (unsigned b = 0; b < lengthBytes; ++b)
            length |= std::uint64_t {static_cast<std::uint8_t>(pairs[i + b])} << (8 * b);
        i += lengthBytes;
        if (length == 0)
            return std::nullopt;
        if (offsetBytes == 0) {
            runs.push_back({length, -1}); // sparse: no clusters, and the position on disk does not move
            continue;
        }
        std::uint64_t delta = 0;
        for (unsigned b = 0; b < offsetBytes; ++b)
            delta |= std::uint64_t {static_cast<std::uint8_t>(pairs[i + b])} << (8 * b);
        if (offsetBytes < 8 && (static_cast<std::uint8_t>(pairs[i + offsetBytes - 1]) & 0x80))
            delta |= ~std::uint64_t {0} << (8 * offsetBytes); // relative to the previous run: signed
        i += offsetBytes;
        lcn += static_cast<std::int64_t>(delta);
        if (lcn < 0)
            return std::nullopt;
        runs.push_back({length, lcn});
    }
    return std::nullopt; // no end marker
}

bool parseFileInfo(std::span<const std::byte> record, FileInfo& out)
{
    out = FileInfo {};
    const std::byte* const d = record.data();
    const std::size_t size = record.size();
    if (size < 0x30 || load<std::uint32_t>(d) != kFileSignature)
        return false;
    out.lsn = load<std::uint64_t>(d + kLsn);
    out.sequence = load<std::uint16_t>(d + kSequence);
    out.links = load<std::uint16_t>(d + kLinks);
    const auto flags = load<std::uint16_t>(d + kRecordFlags);
    out.inUse = flags & kInUse;
    out.directory = flags & kIsDirectory;
    out.baseRecord = recordOf(load<std::uint64_t>(d + kBaseRecord));
    if (!out.inUse)
        return true;
    return forEachAttribute(d, size, [&](std::uint32_t type, const std::byte* attribute, std::size_t length) {
        if (length < 0x18)
            return;
        const bool nonResident = attribute[8] != std::byte {0};
        const auto nameLength = static_cast<std::uint8_t>(attribute[9]);
        switch (type) {
        case kAttributeList:
            out.hasAttributeList = true;
            return;
        case kSecurityDescriptor:
            out.hasSecurityDescriptor = true;
            return;
        case kFileName: {
            // The link count at 0x12 counts the DOS 8.3 alias too: the names
            // a user sees are those in the other namespaces.
            const auto value = residentValue(attribute, length);
            if (value.size() >= 0x42 && static_cast<std::uint8_t>(value[0x41]) != kDosNamespace)
                ++out.names;
            return;
        }
        case kStandardInformation: {
            const auto value = residentValue(attribute, length);
            out.standardInfoBytes = static_cast<std::uint32_t>(value.size());
            if (value.size() >= 0x24) {
                out.modified = load<std::int64_t>(value.data() + 0x08);
                out.mftChanged = load<std::int64_t>(value.data() + 0x10);
                out.attributes = load<std::uint32_t>(value.data() + 0x20);
            }
            if (value.size() >= 0x38)
                out.securityId = load<std::uint32_t>(value.data() + 0x34);
            return;
        }
        case kReparsePoint: {
            out.hasReparse = true;
            const auto value = residentValue(attribute, length);
            if (value.size() >= 4)
                out.reparseTag = load<std::uint32_t>(value.data());
            return;
        }
        case kData:
            break;
        default:
            return;
        }
        if (nameLength != 0)
            return; // a named stream (WofCompressedData, Zone.Identifier...)
        if (out.data != FileInfo::Data::None) {
            out.data = FileInfo::Data::Several;
            return;
        }
        out.dataFlags = load<std::uint16_t>(attribute + 0x0C);
        if (!nonResident) {
            out.data = FileInfo::Data::Resident;
            out.resident = residentValue(attribute, length);
            out.size = out.initialized = out.allocated = out.resident.size();
            return;
        }
        out.data = FileInfo::Data::NonResident;
        if (length < 0x40)
            return; // runList stays empty: not readable
        out.lowestVcn = load<std::uint64_t>(attribute + 0x10);
        out.highestVcn = load<std::uint64_t>(attribute + 0x18);
        const auto pairsOffset = load<std::uint16_t>(attribute + 0x20);
        out.allocated = load<std::uint64_t>(attribute + 0x28);
        out.size = load<std::uint64_t>(attribute + 0x30);
        out.initialized = load<std::uint64_t>(attribute + 0x38);
        if (pairsOffset < length)
            out.runList = {attribute + pairsOffset, length - pairsOffset};
    });
}

const char* bulkSkipName(BulkSkip skip) noexcept
{
    switch (skip) {
    case BulkSkip::None:
        return "readable";
    case BulkSkip::NotFound:
        return "not found";
    case BulkSkip::Directory:
        return "directory";
    case BulkSkip::NoData:
        return "no $DATA";
    case BulkSkip::SeveralParts:
        return "attribute list / several parts";
    case BulkSkip::Compressed:
        return "compressed";
    case BulkSkip::Encrypted:
        return "encrypted";
    case BulkSkip::Reparse:
        return "reparse point";
    case BulkSkip::RunList:
        return "run list";
    case BulkSkip::TooLarge:
        return "too large";
    }
    return "?";
}

// ---- BulkReader ---------------------------------------------------------------

struct BulkReader::Op {
    std::size_t job;
    std::uint64_t volumeOffset;
    std::uint32_t bytes;
    std::byte* into;
};

BulkReader::BulkReader(std::wstring_view root, Options options)
    : m_root(root)
    , m_options(options)
{
    m_options.readsInFlight = std::clamp(m_options.readsInFlight, 1, 64);
    m_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const std::wstring path = L"\\\\.\\" + m_root;
    const HANDLE volume = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, nullptr);
    if (volume == INVALID_HANDLE_VALUE) {
        m_error = L"cannot open the volume " + path + L" (error " + std::to_wstring(::GetLastError())
            + L"; administrator rights are needed)";
        return;
    }
    m_volume = volume;
    NTFS_VOLUME_DATA_BUFFER data {};
    if (!control(FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &data, sizeof data)) {
        m_error = L"FSCTL_GET_NTFS_VOLUME_DATA failed, error " + std::to_wstring(::GetLastError()) + L" (not NTFS?)";
        return;
    }
    m_geometry = {static_cast<std::uint32_t>(data.BytesPerSector), static_cast<std::uint32_t>(data.BytesPerCluster),
        static_cast<std::uint32_t>(data.BytesPerFileRecordSegment)};
    if (!supported(m_geometry)) {
        m_error = L"unsupported layout: " + std::to_wstring(m_geometry.sectorSize) + L"-byte sectors, "
            + std::to_wstring(m_geometry.clusterSize) + L"-byte clusters, " + std::to_wstring(m_geometry.recordSize)
            + L"-byte records";
        return;
    }
    m_mftStart = static_cast<std::uint64_t>(data.MftStartLcn.QuadPart) * m_geometry.clusterSize;
    m_mftExtents = mftExtents(m_root, m_geometry.clusterSize);
    m_options.blockBytes = static_cast<std::uint32_t>(
        std::max<std::uint64_t>(roundUp(m_options.blockBytes, m_geometry.clusterSize), m_geometry.clusterSize));
    if (m_options.quiet) {
        // A hard disk someone is using: one read at a time, after everything
        // else (as the content indexer's file handles are; not background
        // mode, which would lower the buffers' memory priority too).
        m_options.readsInFlight = 1;
        FILE_IO_PRIORITY_HINT_INFO hint {};
        hint.PriorityHint = IoPriorityHintVeryLow;
        ::SetFileInformationByHandle(m_volume, FileIoPriorityHintInfo, &hint, sizeof hint);
    }
}

BulkReader::~BulkReader()
{
    if (m_volume)
        ::CloseHandle(m_volume);
    if (m_event)
        ::CloseHandle(m_event);
}

bool BulkReader::control(unsigned long code, const void* in, unsigned long inBytes, void* out, unsigned long outBytes)
{
    OVERLAPPED o {};
    o.hEvent = m_event;
    ::ResetEvent(m_event);
    DWORD bytes = 0;
    if (!::DeviceIoControl(m_volume, code, const_cast<void*>(in), inBytes, out, outBytes, nullptr, &o)
        && ::GetLastError() != ERROR_IO_PENDING)
        return false;
    return ::GetOverlappedResult(m_volume, &o, &bytes, TRUE);
}

bool BulkReader::readSectors(std::uint64_t offset, std::uint32_t bytes, std::byte* into)
{
    OVERLAPPED o {};
    o.hEvent = m_event;
    o.Offset = static_cast<DWORD>(offset);
    o.OffsetHigh = static_cast<DWORD>(offset >> 32);
    ::ResetEvent(m_event);
    if (!::ReadFile(m_volume, into, bytes, nullptr, &o) && ::GetLastError() != ERROR_IO_PENDING)
        return false;
    DWORD read = 0;
    return ::GetOverlappedResult(m_volume, &o, &read, TRUE) && read == bytes;
}

namespace {

// Found, from a parsed record: what keeps it from being read, or its runs.
void classify(BulkReader::Found& f, std::uint32_t number, const FileInfo& info, std::uint64_t maxBytes, bool directory,
    std::uint32_t clusterSize)
{
    f.record = number;
    f.info = info;
    f.info.resident = {};
    f.info.runList = {};
    f.want = std::min(info.size, maxBytes);
    using Data = FileInfo::Data;
    if (directory) {
        f.skip = BulkSkip::Directory;
    } else if (info.data == Data::None) {
        f.skip = BulkSkip::NoData;
    } else if (info.data == Data::Several) {
        f.skip = BulkSkip::SeveralParts;
    } else if (info.hasReparse || (info.attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        f.skip = BulkSkip::Reparse;
    } else if ((info.dataFlags & kDataEncrypted) || (info.attributes & FILE_ATTRIBUTE_ENCRYPTED)) {
        f.skip = BulkSkip::Encrypted;
    } else if ((info.dataFlags & kDataCompressed) || (info.attributes & FILE_ATTRIBUTE_COMPRESSED)) {
        f.skip = BulkSkip::Compressed;
    } else if (info.hasAttributeList) {
        f.skip = BulkSkip::SeveralParts; // the $DATA may go on in an extension record
    } else if (info.data == Data::Resident) {
        f.resident.assign(info.resident.begin(), info.resident.end());
        f.skip = info.size > maxBytes ? BulkSkip::TooLarge : BulkSkip::None;
    } else {
        auto runs = decodeDataRuns(info.runList);
        std::uint64_t clusters = 0;
        if (runs) {
            for (const DataRun& r : *runs)
                clusters += r.clusters;
        }
        // The whole attribute, in this record: from VCN 0 to its last cluster,
        // and the clusters hold the data (a shorter run list would be the
        // first part of several).
        if (!runs || info.lowestVcn != 0 || clusters != info.highestVcn + 1 || clusters * clusterSize < info.size) {
            f.skip = BulkSkip::RunList;
        } else {
            f.runs = std::move(*runs);
            f.skip = info.size > maxBytes ? BulkSkip::TooLarge : BulkSkip::None;
        }
    }
}

struct U16Hash {
    std::size_t operator()(std::u16string_view s) const noexcept { return std::hash<std::u16string_view> {}(s); }
};

} // namespace

bool BulkReader::scan(std::span<const Want> wants, std::vector<Found>& found, ScanStats* stats)
{
    found.assign(wants.size(), Found {});
    if (!valid())
        return false;
    // By name; the folder is checked among those (a sample has few names twice).
    std::unordered_map<std::u16string_view, std::vector<std::uint32_t>, U16Hash> byName;
    byName.reserve(wants.size());
    for (std::size_t i = 0; i < wants.size(); ++i)
        byName[wants[i].name].push_back(static_cast<std::uint32_t>(i));

    const auto started = std::chrono::steady_clock::now();
    ScanStats s;
    MftReader reader(m_root);
    if (!reader.valid()) {
        m_error = reader.error();
        return false;
    }
    FileRecord record;
    FileInfo info;
    while (reader.next()) {
        for (std::size_t i = 0; i < reader.blockRecords(); ++i) {
            ++s.records;
            if (!reader.parse(i, record) || !record.inUse)
                continue;
            ++s.inUse;
            if (record.baseRecord != 0)
                continue; // an extension: its names belong to the base record (not matched here)
            const auto number = static_cast<std::uint32_t>(reader.firstRecord() + i);
            bool parsed = false;
            for (const FileRecord::Name& n : record.names) {
                const auto it = byName.find(n.name);
                if (it == byName.end())
                    continue;
                const std::uint32_t parent = recordOf(n.parent);
                for (const std::uint32_t w : it->second) {
                    if (wants[w].parentRecord != parent || found[w].skip != BulkSkip::NotFound)
                        continue;
                    if (!parsed) {
                        if (!parseFileInfo(reader.recordBytes(i), info))
                            break;
                        parsed = true;
                    }
                    classify(found[w], number, info, wants[w].maxBytes, record.directory, m_geometry.clusterSize);
                    ++s.matched;
                }
            }
        }
    }
    if (!reader.valid()) {
        m_error = reader.error();
        return false;
    }
    s.ms = msSince(started);
    if (stats)
        *stats = s;
    return true;
}

std::int64_t BulkReader::firstLcn(const Found& found) noexcept
{
    for (const DataRun& r : found.runs) {
        if (r.lcn >= 0)
            return r.lcn;
    }
    return std::numeric_limits<std::int64_t>::max();
}

bool BulkReader::readAll(std::span<const Found> found,
    const std::function<void(std::size_t index, std::span<const std::byte> bytes)>& sink, ReadStats* stats,
    std::span<const std::uint32_t> only, void* abort)
{
    if (!valid())
        return false;
    const auto started = std::chrono::steady_clock::now();
    const std::uint32_t cluster = m_geometry.clusterSize;
    ReadStats s;

    // The files to read, by their first cluster on the disk.
    struct Job {
        std::size_t index;
        std::int64_t firstLcn;
        std::byte* buffer = nullptr; // VirtualAlloc: page aligned (the volume is read unbuffered), zeroed
        std::size_t bufferBytes = 0;
        std::vector<Op> ops;
        std::size_t issued = 0;
        std::size_t completed = 0;
        bool planned = false;
    };
    std::vector<Job> jobs;
    const auto consider = [&](std::size_t i) {
        const Found& f = found[i];
        if (f.skip != BulkSkip::None || f.info.data != FileInfo::Data::NonResident || f.want == 0)
            return;
        jobs.push_back({i, firstLcn(f)});
    };
    if (only.data()) {
        for (const std::uint32_t i : only) {
            if (i < found.size())
                consider(i);
        }
    } else {
        for (std::size_t i = 0; i < found.size(); ++i)
            consider(i);
    }
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.firstLcn < b.firstLcn; });

    const auto plan = [&](Job& job) {
        const Found& f = found[job.index];
        // Nothing past the valid data length is read: those bytes are zeros
        // (and on the disk, whatever was there before).
        const std::uint64_t readBytes = roundUp(std::min(f.want, f.info.initialized), cluster);
        job.bufferBytes = static_cast<std::size_t>(std::max(roundUp(f.want, cluster), readBytes));
        job.buffer = static_cast<std::byte*>(
            ::VirtualAlloc(nullptr, job.bufferBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        job.planned = true;
        if (!job.buffer)
            return false;
        // Runs that are next to each other on the disk become one read; each
        // read is at most blockBytes long.
        struct Segment {
            std::uint64_t volumeOffset;
            std::uint64_t bufferOffset;
            std::uint64_t bytes;
        };
        std::vector<Segment> segments;
        std::uint64_t vcn = 0;
        for (const DataRun& r : f.runs) {
            const std::uint64_t start = vcn * cluster;
            if (start >= readBytes)
                break;
            const std::uint64_t bytes = std::min(r.clusters * cluster, readBytes - start);
            vcn += r.clusters;
            if (r.lcn < 0)
                continue; // sparse: zeros, which the buffer holds already
            const std::uint64_t at = static_cast<std::uint64_t>(r.lcn) * cluster;
            if (!segments.empty() && segments.back().volumeOffset + segments.back().bytes == at
                && segments.back().bufferOffset + segments.back().bytes == start)
                segments.back().bytes += bytes;
            else
                segments.push_back({at, start, bytes});
        }
        for (const Segment& seg : segments) {
            for (std::uint64_t pos = 0; pos < seg.bytes; pos += m_options.blockBytes) {
                const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(m_options.blockBytes, seg.bytes - pos));
                job.ops.push_back({0, seg.volumeOffset + pos, n, job.buffer + seg.bufferOffset + pos});
                s.diskBytes += n;
            }
        }
        return true;
    };
    const auto deliver = [&](Job& job) {
        const Found& f = found[job.index];
        // The rest of the last cluster read, past the valid data length, is
        // whatever the disk held there: zeros, as ReadFile gives.
        if (f.info.initialized < f.want)
            std::memset(job.buffer + f.info.initialized, 0, static_cast<std::size_t>(f.want - f.info.initialized));
        sink(job.index, std::span<const std::byte>(job.buffer, static_cast<std::size_t>(f.want)));
        ::VirtualFree(job.buffer, 0, MEM_RELEASE);
        job.buffer = nullptr;
        ++s.files;
        s.bytes += f.want;
    };

    // A ring of reads in flight, issued in the jobs' order and waited for in
    // the same order; a job is delivered once its last read is in.
    struct Slot {
        OVERLAPPED overlapped {};
        std::size_t job = 0;
        bool pending = false;
    };
    std::vector<Slot> slots(static_cast<std::size_t>(m_options.readsInFlight));
    for (Slot& slot : slots)
        slot.overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = true;
    std::size_t nextJob = 0;
    std::size_t head = 0; // the slot whose read completes next
    std::size_t tail = 0; // the slot the next read goes in
    std::size_t inFlight = 0;
    // The next read to issue, planning jobs as it reaches them; jobs with
    // nothing to read (all sparse, or nothing initialized) are delivered here.
    const auto nextOp = [&](std::size_t& job) -> const Op* {
        while (nextJob < jobs.size()) {
            Job& j = jobs[nextJob];
            if (!j.planned && !plan(j)) {
                ok = false;
                return nullptr;
            }
            if (j.issued < j.ops.size()) {
                job = nextJob;
                return &j.ops[j.issued++];
            }
            if (j.ops.empty())
                deliver(j);
            ++nextJob;
        }
        return nullptr;
    };
    while (ok) {
        if (abort && ::WaitForSingleObject(abort, 0) == WAIT_OBJECT_0) {
            m_error = L"the read was given up";
            ok = false;
            break;
        }
        while (inFlight < slots.size()) {
            std::size_t job = 0;
            const Op* op = nextOp(job);
            if (!op)
                break;
            Slot& slot = slots[tail];
            const HANDLE event = slot.overlapped.hEvent;
            slot.overlapped = OVERLAPPED {};
            slot.overlapped.hEvent = event;
            slot.overlapped.Offset = static_cast<DWORD>(op->volumeOffset);
            slot.overlapped.OffsetHigh = static_cast<DWORD>(op->volumeOffset >> 32);
            slot.job = job;
            if (!::ReadFile(m_volume, op->into, op->bytes, nullptr, &slot.overlapped) && ::GetLastError() != ERROR_IO_PENDING) {
                m_error = L"reading the volume failed, error " + std::to_wstring(::GetLastError());
                ok = false;
                break;
            }
            slot.pending = true;
            ++s.reads;
            tail = (tail + 1) % slots.size();
            ++inFlight;
        }
        if (inFlight == 0)
            break;
        Slot& slot = slots[head];
        DWORD read = 0;
        const BOOL done = ::GetOverlappedResult(m_volume, &slot.overlapped, &read, TRUE);
        slot.pending = false;
        head = (head + 1) % slots.size();
        --inFlight;
        Job& job = jobs[slot.job];
        if (!done || read != job.ops[job.completed].bytes) {
            m_error = L"reading the volume failed, error " + std::to_wstring(::GetLastError());
            ok = false;
            break;
        }
        if (++job.completed == job.ops.size())
            deliver(job);
    }
    // On an error: reads still in flight write into buffers that must stay until they end.
    if (inFlight > 0) {
        ::CancelIoEx(m_volume, nullptr);
        for (Slot& slot : slots) {
            DWORD ignored = 0;
            if (slot.pending)
                ::GetOverlappedResult(m_volume, &slot.overlapped, &ignored, TRUE);
        }
    }
    for (Job& job : jobs) {
        if (job.buffer)
            ::VirtualFree(job.buffer, 0, MEM_RELEASE);
    }
    for (Slot& slot : slots)
        ::CloseHandle(slot.overlapped.hEvent);
    s.ms = msSince(started);
    if (stats)
        *stats = s;
    return ok;
}

bool BulkReader::readOne(const Found& found, std::vector<std::byte>& out)
{
    out.clear();
    if (found.skip != BulkSkip::None)
        return false;
    if (found.info.data == FileInfo::Data::Resident) {
        out.assign(found.resident.begin(), found.resident.begin() + static_cast<std::ptrdiff_t>(found.want));
        return true;
    }
    bool got = false;
    const bool ok = readAll(std::span<const Found>(&found, 1), [&](std::size_t, std::span<const std::byte> bytes) {
        out.assign(bytes.begin(), bytes.end());
        got = true;
    });
    return ok && (got || found.want == 0);
}

std::vector<std::byte> BulkReader::readRecordRaw(std::uint32_t record)
{
    if (!valid() || m_mftExtents.empty())
        return {};
    // Its bytes in the MFT, through the extents, in whole sectors (as
    // MftReader::readRecord does: a cluster may be smaller than a record).
    const std::uint32_t recordSize = m_geometry.recordSize;
    const std::uint32_t sector = m_geometry.sectorSize;
    const auto pieceBytes = static_cast<std::uint32_t>(roundUp(recordSize, sector) + sector);
    std::byte* const piece = static_cast<std::byte*>(::VirtualAlloc(nullptr, pieceBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!piece)
        return {};
    std::vector<std::byte> out(recordSize);
    std::uint64_t at = std::uint64_t {record} * recordSize;
    std::uint64_t start = 0;
    std::size_t done = 0;
    bool ok = true;
    for (const Extent& e : m_mftExtents) {
        while (ok && done < recordSize && at >= start && at < start + e.length) {
            const std::uint64_t n = std::min<std::uint64_t>(recordSize - done, start + e.length - at);
            const std::uint64_t offset = e.offset + (at - start);
            const std::uint64_t first = offset / sector * sector;
            const auto bytes = static_cast<std::uint32_t>(roundUp(offset + n, sector) - first);
            ok = bytes <= pieceBytes && readSectors(first, bytes, piece);
            if (!ok)
                break;
            std::memcpy(out.data() + done, piece + (offset - first), static_cast<std::size_t>(n));
            done += static_cast<std::size_t>(n);
            at += n;
        }
        start += e.length;
    }
    ::VirtualFree(piece, 0, MEM_RELEASE);
    FileRecord parsed;
    if (!ok || done < recordSize || !parseFileRecord(out.data(), out.size(), parsed))
        return {};
    return out;
}

bool BulkReader::readRecordCached(std::uint32_t record, Cached& out)
{
    out = Cached {};
    if (!valid())
        return false;
    NTFS_FILE_RECORD_INPUT_BUFFER in {};
    in.FileReferenceNumber.QuadPart = record;
    std::vector<std::byte> buffer(sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER) + m_geometry.recordSize);
    if (!control(FSCTL_GET_NTFS_FILE_RECORD, &in, sizeof in, buffer.data(), static_cast<DWORD>(buffer.size())))
        return false;
    const auto* answer = reinterpret_cast<const NTFS_FILE_RECORD_OUTPUT_BUFFER*>(buffer.data());
    out.reference = static_cast<std::uint64_t>(answer->FileReferenceNumber.QuadPart);
    const std::size_t length = std::min<std::size_t>(answer->FileRecordLength, m_geometry.recordSize);
    out.record.assign(reinterpret_cast<const std::byte*>(answer->FileRecordBuffer),
        reinterpret_cast<const std::byte*>(answer->FileRecordBuffer) + length);
    // With its update sequence array still in place, the check values sit at
    // the end of every 512 bytes and parseFileRecord puts the data back;
    // without (NTFS applied them), the values there are data and it refuses.
    std::vector<std::byte> fixed = out.record;
    FileRecord parsed;
    if (parseFileRecord(fixed.data(), fixed.size(), parsed)) {
        out.record = std::move(fixed);
        out.fixupsApplied = true;
        return true;
    }
    // Without the array, the record must still read as one: a damaged or
    // cut-short one is not handed out as if NTFS had fixed it up.
    FileInfo info;
    if (!parseFileInfo(out.record, info) || (info.inUse && info.standardInfoBytes != 0x30 && info.standardInfoBytes != 0x48)) {
        out.record.clear();
        return false;
    }
    return true;
}

// ---- AccessGate -----------------------------------------------------------------

namespace {

// Attributes that keep a file from being opened even for its descriptor: a
// reparse point may be a cloud placeholder (an open could recall it).
constexpr DWORD kNeverOpened = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN
    | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
// A record changed within this long of now (as NTFS has it) does not settle
// its security id's verdict for other files: its descriptor may just have
// been changed, and the id in the record NTFS hands out may lag.
constexpr std::int64_t kSettledTicks = 10'000'000ll * 10; // 10 s in FILETIME units

std::uint64_t referenceOf(const BulkReader::Found& found) noexcept
{
    return std::uint64_t {found.record} | (std::uint64_t {found.info.sequence} << 48);
}

} // namespace

AccessGate::AccessGate(std::wstring_view root, void* token, BulkReader& reader)
    : m_token(token)
    , m_reader(reader)
{
    if (!m_token)
        return; // no check
    // OpenFileById needs a handle on the volume opened plainly (the reader's
    // is unbuffered and overlapped).
    const std::wstring path = L"\\\\.\\" + std::wstring(root);
    const HANDLE volume = ::CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) {
        m_error = L"cannot open the volume for the access checks (error " + std::to_wstring(::GetLastError()) + L")";
        return;
    }
    m_volume = volume;
}

AccessGate::~AccessGate()
{
    if (m_volume)
        ::CloseHandle(m_volume);
}

bool AccessGate::allowed(const BulkReader::Found& found)
{
    if (!m_token)
        return true;
    if (!valid() || found.skip == BulkSkip::NotFound || (found.info.attributes & kNeverOpened) || found.info.hasReparse)
        return false;
    const std::uint32_t securityId = found.info.securityId;
    const bool keyed = found.info.standardInfoBytes >= 0x38 && securityId != 0;
    if (keyed) {
        if (const auto it = m_bySecurityId.find(securityId); it != m_bySecurityId.end())
            return it->second;
    }

    // The file by its reference (record and sequence: a reused record does
    // not open), for its descriptor only.
    FILE_ID_DESCRIPTOR id {};
    id.dwSize = sizeof id;
    id.Type = FileIdType;
    id.FileId.QuadPart = static_cast<LONGLONG>(referenceOf(found));
    const HANDLE file = ::OpenFileById(m_volume, &id, READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, FILE_FLAG_OPEN_REPARSE_POINT);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    bool allowed = false;
    FILE_ID_INFO idInfo {};
    FILE_BASIC_INFO basic {};
    std::uint64_t opened = 0;
    if (::GetFileInformationByHandleEx(file, FileIdInfo, &idInfo, sizeof idInfo))
        std::memcpy(&opened, idInfo.FileId.Identifier, sizeof opened); // NTFS: the 64-bit reference, then zeros
    if (opened == referenceOf(found) && ::GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof basic)
        && !(basic.FileAttributes & kNeverOpened)) {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        // The label too: AccessCheck weighs the integrity policy when it is there (as src/mcp/Visibility.cpp).
        if (::GetSecurityInfo(file, SE_FILE_OBJECT,
                OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
                nullptr, nullptr, nullptr, nullptr, &descriptor)
            == ERROR_SUCCESS) {
            GENERIC_MAPPING mapping {FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
            DWORD access = GENERIC_READ; // what CreateFileW(GENERIC_READ) asks: FILE_GENERIC_READ
            ::MapGenericMask(&access, &mapping);
            PRIVILEGE_SET privileges {};
            DWORD privilegesSize = sizeof privileges;
            DWORD granted = 0;
            BOOL status = FALSE;
            allowed = ::AccessCheck(descriptor, m_token, access, &mapping, &privileges, &privilegesSize, &granted, &status)
                && status;
            ::LocalFree(descriptor);
            ++m_checks;
        }
    }
    ::CloseHandle(file);
    if (!keyed)
        return allowed;

    // The verdict stands for every file with this security id, once the
    // record (as NTFS has it now) is seen with the id and settled: a
    // descriptor changed a moment ago might otherwise be charged to the old id.
    BulkReader::Cached cached;
    FileInfo now;
    FILETIME ft {};
    ::GetSystemTimeAsFileTime(&ft);
    const auto nowTicks = static_cast<std::int64_t>((std::uint64_t {ft.dwHighDateTime} << 32) | ft.dwLowDateTime);
    if (m_reader.readRecordCached(found.record, cached) && parseFileInfo(cached.record, now) && now.inUse
        && recordOf(cached.reference) == found.record && now.sequence == found.info.sequence
        && now.securityId == securityId && nowTicks - now.mftChanged > kSettledTicks)
        m_bySecurityId.emplace(securityId, allowed);
    return allowed;
}

void* clientIdentificationToken(void* pipe, std::wstring* why)
{
    const auto fail = [&](const wchar_t* what) {
        if (why)
            *why = std::wstring(what) + L" (error " + std::to_wstring(::GetLastError()) + L")";
        return static_cast<void*>(nullptr);
    };
    if (!::ImpersonateNamedPipeClient(pipe))
        return fail(L"ImpersonateNamedPipeClient");
    HANDLE thread = nullptr;
    const bool opened = ::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE, TRUE, &thread);
    ::RevertToSelf();
    if (!opened)
        return fail(L"OpenThreadToken");
    // Identification at least (anonymous says nothing), and at most: the
    // copy is made at that level whatever the client allowed.
    TOKEN_STATISTICS statistics {};
    DWORD size = 0;
    bool ok = ::GetTokenInformation(thread, TokenStatistics, &statistics, sizeof statistics, &size)
        && statistics.TokenType == TokenImpersonation && statistics.ImpersonationLevel >= SecurityIdentification;
    if (!ok) {
        ::CloseHandle(thread);
        return fail(L"the client's token is anonymous");
    }
    // Bypass traverse checking: with it (Everyone has it by default), what a
    // file's own descriptor says is what its open would say; without it, the
    // folders on the way would count too, and the file is not served.
    LUID traverse {};
    ::LookupPrivilegeValueW(nullptr, SE_CHANGE_NOTIFY_NAME, &traverse);
    PRIVILEGE_SET wanted {};
    wanted.PrivilegeCount = 1;
    wanted.Control = PRIVILEGE_SET_ALL_NECESSARY;
    wanted.Privilege[0].Luid = traverse;
    wanted.Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL has = FALSE;
    ok = ::PrivilegeCheck(thread, &wanted, &has) && has;
    if (!ok) {
        ::CloseHandle(thread);
        return fail(L"the client has no SeChangeNotifyPrivilege");
    }
    HANDLE token = nullptr;
    if (!::DuplicateTokenEx(thread, TOKEN_QUERY, nullptr, SecurityIdentification, TokenImpersonation, &token))
        token = nullptr;
    ::CloseHandle(thread);
    return token ? token : fail(L"DuplicateTokenEx");
}

// ---- the volume itself as a BulkSource (DirectVolumeAccess) ----------------------------

namespace {

// An elevated program reading its own volumes: BulkReader in this process,
// no access check (it reads everything anyway).
class DirectBulkSource final : public BulkSource {
public:
    DirectBulkSource(std::wstring_view root, bool quiet)
        : m_reader(root, BulkReader::Options {.quiet = quiet})
    {
    }
    bool valid() const noexcept override { return m_reader.valid() && m_error.empty(); }
    const std::wstring& error() const noexcept override { return m_error.empty() ? m_reader.error() : m_error; }

    bool want(std::span<const BulkWant> wants) override
    {
        m_wants.insert(m_wants.end(), wants.begin(), wants.end());
        return true;
    }
    bool scan(std::vector<BulkFile>& files) override
    {
        files.clear();
        const bool scanned = m_reader.scan(m_wants, m_found);
        std::vector<BulkWant>().swap(m_wants); // found[i] answers wants[i]: the names are not needed any more
        if (!scanned)
            return false;
        m_fetched.assign(m_found.size(), false);
        files.reserve(m_found.size());
        std::vector<std::uint32_t> data; // in disk order
        for (std::size_t i = 0; i < m_found.size(); ++i) {
            const BulkReader::Found& f = m_found[i];
            BulkFile file {static_cast<std::uint32_t>(i), BulkStatus::Fallback, f.skip, f.info.size, f.info.modified};
            if (f.skip == BulkSkip::None) {
                data.push_back(static_cast<std::uint32_t>(i));
                continue;
            }
            if (f.skip == BulkSkip::NotFound)
                file.status = BulkStatus::NotFound;
            else if (f.skip == BulkSkip::TooLarge)
                file.status = BulkStatus::TooLarge;
            files.push_back(file);
        }
        std::sort(data.begin(), data.end(), [&](std::uint32_t a, std::uint32_t b) {
            return BulkReader::firstLcn(m_found[a]) < BulkReader::firstLcn(m_found[b]);
        });
        for (const std::uint32_t i : data)
            files.push_back({i, BulkStatus::Data, BulkSkip::None, m_found[i].info.size, m_found[i].info.modified});
        return true;
    }
    bool fetch(std::span<const std::uint32_t> tickets,
        const std::function<void(std::uint32_t ticket, std::vector<std::byte>&& bytes)>& sink) override
    {
        std::vector<std::uint32_t> only;
        for (const std::uint32_t t : tickets) {
            if (t >= m_found.size() || m_fetched[t] || m_found[t].skip != BulkSkip::None)
                continue;
            m_fetched[t] = true;
            const BulkReader::Found& f = m_found[t];
            if (f.info.data == FileInfo::Data::Resident) {
                sink(t, std::vector<std::byte>(f.resident.begin(), f.resident.begin() + static_cast<std::ptrdiff_t>(f.want)));
                continue;
            }
            only.push_back(t);
        }
        if (only.empty())
            return true;
        return m_reader.readAll(
            m_found,
            [&](std::size_t index, std::span<const std::byte> bytes) {
                sink(static_cast<std::uint32_t>(index), std::vector<std::byte>(bytes.begin(), bytes.end()));
            },
            nullptr, only);
    }

private:
    BulkReader m_reader;
    std::wstring m_error;
    std::vector<BulkWant> m_wants;
    std::vector<BulkReader::Found> m_found;
    std::vector<bool> m_fetched;
};

} // namespace

std::unique_ptr<BulkSource> DirectVolumeAccess::openBulkRead(std::wstring_view root, std::uint32_t, bool quiet)
{
    return std::make_unique<DirectBulkSource>(root, quiet);
}

} // namespace ws::ntfs
