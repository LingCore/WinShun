#include "VolumeProtocol.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace ws::volproto {

namespace {

template <typename T> void append(std::vector<std::byte>& out, T v)
{
    const std::size_t at = out.size();
    out.resize(at + sizeof v);
    std::memcpy(out.data() + at, &v, sizeof v);
}

template <typename T> bool take(std::span<const std::byte> data, std::size_t& pos, T& v) noexcept
{
    if (data.size() - pos < sizeof v)
        return false;
    std::memcpy(&v, data.data() + pos, sizeof v);
    pos += sizeof v;
    return true;
}

bool validRoot(std::wstring_view root) noexcept
{
    return root.size() == 2 && root[1] == L':'
        && ((root[0] >= L'A' && root[0] <= L'Z') || (root[0] >= L'a' && root[0] <= L'z'));
}

} // namespace

std::wstring fullPipePath(std::wstring_view name)
{
    return L"\\\\.\\pipe\\" + std::wstring(name);
}

// ---- Writer / Reader ------------------------------------------------------------

Writer::Writer()
{
    m_data.resize(kHeaderBytes);
}

void Writer::u8(std::uint8_t v)
{
    append(m_data, v);
}

void Writer::u16(std::uint16_t v)
{
    append(m_data, v);
}

void Writer::u32(std::uint32_t v)
{
    append(m_data, v);
}

void Writer::u64(std::uint64_t v)
{
    append(m_data, v);
}

void Writer::i64(std::int64_t v)
{
    append(m_data, v);
}

void Writer::string(std::u16string_view s)
{
    const auto n = static_cast<std::uint16_t>(std::min<std::size_t>(s.size(), 0xFFFF));
    u16(n);
    bytes({reinterpret_cast<const std::byte*>(s.data()), n * 2u});
}

void Writer::string(std::wstring_view s)
{
    string(std::u16string_view(reinterpret_cast<const char16_t*>(s.data()), s.size()));
}

void Writer::bytes(std::span<const std::byte> b)
{
    m_data.insert(m_data.end(), b.begin(), b.end());
}

std::span<const std::byte> Writer::finish(Type type)
{
    const auto length = static_cast<std::uint32_t>(m_data.size() - kHeaderBytes);
    const auto t = static_cast<std::uint16_t>(type);
    const std::uint16_t flags = 0;
    std::memcpy(m_data.data(), &length, 4);
    std::memcpy(m_data.data() + 4, &t, 2);
    std::memcpy(m_data.data() + 6, &flags, 2);
    return m_data;
}

void Writer::reset()
{
    m_data.resize(kHeaderBytes);
}

void Writer::patchU32(std::size_t at, std::uint32_t v)
{
    std::memcpy(m_data.data() + kHeaderBytes + at, &v, sizeof v);
}

void Writer::patchU64(std::size_t at, std::uint64_t v)
{
    std::memcpy(m_data.data() + kHeaderBytes + at, &v, sizeof v);
}

bool Reader::u8(std::uint8_t& v) noexcept
{
    return take(m_data, m_pos, v);
}

bool Reader::u16(std::uint16_t& v) noexcept
{
    return take(m_data, m_pos, v);
}

bool Reader::u32(std::uint32_t& v) noexcept
{
    return take(m_data, m_pos, v);
}

bool Reader::u64(std::uint64_t& v) noexcept
{
    return take(m_data, m_pos, v);
}

bool Reader::i64(std::int64_t& v) noexcept
{
    return take(m_data, m_pos, v);
}

bool Reader::boolean(bool& v) noexcept
{
    std::uint8_t b = 0;
    if (!u8(b))
        return false;
    v = b != 0;
    return true;
}

bool Reader::string(std::u16string_view& s) noexcept
{
    std::uint16_t n = 0;
    std::span<const std::byte> b;
    if (!u16(n) || !bytes(b, n * 2u))
        return false;
    s = {reinterpret_cast<const char16_t*>(b.data()), n};
    return true;
}

bool Reader::string(std::wstring& s)
{
    std::u16string_view v;
    if (!string(v))
        return false;
    s.assign(reinterpret_cast<const wchar_t*>(v.data()), v.size());
    return true;
}

bool Reader::bytes(std::span<const std::byte>& b, std::size_t n) noexcept
{
    if (left() < n)
        return false;
    b = m_data.subspan(m_pos, n);
    m_pos += n;
    return true;
}

bool parseHeader(std::span<const std::byte, 8> header, Type& type, std::uint32_t& length) noexcept
{
    std::uint16_t t = 0;
    std::memcpy(&length, header.data(), 4);
    std::memcpy(&t, header.data() + 4, 2);
    type = static_cast<Type>(t);
    return length <= kMaxPayloadBytes;
}

// ---- payloads ----------------------------------------------------------------

void encode(Writer& w, const Hello& m)
{
    w.u16(m.major);
    w.u16(m.minor);
    for (const std::uint16_t v : m.version)
        w.u16(v);
    w.u32(m.pid);
}

bool decode(std::span<const std::byte> payload, Hello& m)
{
    Reader r(payload);
    return r.u16(m.major) && r.u16(m.minor) && r.u16(m.version[0]) && r.u16(m.version[1]) && r.u16(m.version[2])
        && r.u32(m.pid);
}

void encode(Writer& w, const HelloOk& m)
{
    w.u16(m.major);
    w.u16(m.minor);
    for (const std::uint16_t v : m.version)
        w.u16(v);
    w.u32(m.capabilities);
}

bool decode(std::span<const std::byte> payload, HelloOk& m)
{
    Reader r(payload);
    return r.u16(m.major) && r.u16(m.minor) && r.u16(m.version[0]) && r.u16(m.version[1]) && r.u16(m.version[2])
        && r.u32(m.capabilities);
}

void encode(Writer& w, const HelloRefused& m)
{
    w.u32(m.reason);
}

bool decode(std::span<const std::byte> payload, HelloRefused& m)
{
    Reader r(payload);
    return r.u32(m.reason);
}

void encode(Writer& w, const OpenMft& m)
{
    w.string(m.root);
    w.u32(m.serial);
    w.u8(m.lowPriority ? 1 : 0);
}

bool decode(std::span<const std::byte> payload, OpenMft& m)
{
    Reader r(payload);
    return r.string(m.root) && r.u32(m.serial) && r.boolean(m.lowPriority) && validRoot(m.root);
}

void encode(Writer& w, const MftInfo& m)
{
    w.u32(m.geometry.sectorSize);
    w.u32(m.geometry.clusterSize);
    w.u32(m.geometry.recordSize);
    w.u64(m.recordCount);
    w.u64(m.validBytes);
}

bool decode(std::span<const std::byte> payload, MftInfo& m)
{
    Reader r(payload);
    return r.u32(m.geometry.sectorSize) && r.u32(m.geometry.clusterSize) && r.u32(m.geometry.recordSize)
        && r.u64(m.recordCount) && r.u64(m.validBytes);
}

void encode(Writer& w, const MftDone& m)
{
    w.u32(m.status);
    w.string(m.error);
}

bool decode(std::span<const std::byte> payload, MftDone& m)
{
    Reader r(payload);
    return r.u32(m.status) && r.string(m.error);
}

void encode(Writer& w, const QueryJournal& m)
{
    w.string(m.root);
    w.u32(m.serial);
}

bool decode(std::span<const std::byte> payload, QueryJournal& m)
{
    Reader r(payload);
    return r.string(m.root) && r.u32(m.serial) && validRoot(m.root);
}

void encode(Writer& w, const ntfs::JournalInfo& m)
{
    w.u64(m.id);
    w.i64(m.firstUsn);
    w.i64(m.nextUsn);
}

bool decode(std::span<const std::byte> payload, ntfs::JournalInfo& m)
{
    Reader r(payload);
    return r.u64(m.id) && r.i64(m.firstUsn) && r.i64(m.nextUsn);
}

void encode(Writer& w, const FollowJournal& m)
{
    w.string(m.root);
    w.u32(m.serial);
    w.u64(m.journalId);
}

bool decode(std::span<const std::byte> payload, FollowJournal& m)
{
    Reader r(payload);
    return r.string(m.root) && r.u32(m.serial) && r.u64(m.journalId) && validRoot(m.root);
}

void encode(Writer& w, const Read& m)
{
    w.i64(m.usn);
    w.u8(m.wait ? 1 : 0);
}

bool decode(std::span<const std::byte> payload, Read& m)
{
    Reader r(payload);
    return r.i64(m.usn) && r.boolean(m.wait);
}

void encode(Writer& w, const JournalLost& m)
{
    w.u32(m.error);
}

bool decode(std::span<const std::byte> payload, JournalLost& m)
{
    Reader r(payload);
    return r.u32(m.error);
}

void encode(Writer& w, const CloseVolume& m)
{
    w.string(m.root);
}

bool decode(std::span<const std::byte> payload, CloseVolume& m)
{
    Reader r(payload);
    return r.string(m.root) && validRoot(m.root);
}

void encode(Writer& w, const Closed& m)
{
    w.u32(m.closed);
}

bool decode(std::span<const std::byte> payload, Closed& m)
{
    Reader r(payload);
    return r.u32(m.closed);
}

void encode(Writer& w, const Error& m)
{
    w.u32(m.code);
    w.string(m.message);
}

bool decode(std::span<const std::byte> payload, Error& m)
{
    Reader r(payload);
    return r.u32(m.code) && r.string(m.message);
}

void encode(Writer& w, const OpenBulkRead& m)
{
    w.string(m.root);
    w.u32(m.serial);
    w.u8(m.quiet ? 1 : 0);
}

bool decode(std::span<const std::byte> payload, OpenBulkRead& m)
{
    Reader r(payload);
    return r.string(m.root) && r.u32(m.serial) && r.boolean(m.quiet) && validRoot(m.root);
}

void encode(Writer& w, const BulkScanned& m)
{
    w.u32(m.matched);
    w.u32(m.checks);
}

bool decode(std::span<const std::byte> payload, BulkScanned& m)
{
    Reader r(payload);
    return r.u32(m.matched) && r.u32(m.checks);
}

void encode(Writer& w, std::span<const ntfs::BulkWant> wants)
{
    w.u32(static_cast<std::uint32_t>(wants.size()));
    for (const ntfs::BulkWant& want : wants) {
        w.u32(want.parentRecord);
        w.u64(want.maxBytes);
        w.string(std::u16string_view(want.name));
    }
}

bool decode(std::span<const std::byte> payload, std::vector<ntfs::BulkWant>& out)
{
    Reader r(payload);
    std::uint32_t count = 0;
    if (!r.u32(count) || count > payload.size() / 14) // at least 14 bytes each
        return false;
    out.reserve(out.size() + count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ntfs::BulkWant want;
        std::u16string_view name;
        if (!r.u32(want.parentRecord) || !r.u64(want.maxBytes) || !r.string(name) || name.empty())
            return false;
        want.name.assign(name);
        out.push_back(std::move(want));
    }
    return r.done();
}

void encode(Writer& w, std::span<const ntfs::BulkFile> files)
{
    w.u32(static_cast<std::uint32_t>(files.size()));
    for (const ntfs::BulkFile& f : files) {
        w.u32(f.ticket);
        w.u8(static_cast<std::uint8_t>(f.status));
        w.u8(static_cast<std::uint8_t>(f.reason));
        w.u64(f.size);
        w.i64(f.modified);
    }
}

bool decode(std::span<const std::byte> payload, std::vector<ntfs::BulkFile>& out)
{
    Reader r(payload);
    std::uint32_t count = 0;
    if (!r.u32(count) || count > payload.size() / 22)
        return false;
    out.reserve(out.size() + count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ntfs::BulkFile f {};
        std::uint8_t status = 0, reason = 0;
        if (!r.u32(f.ticket) || !r.u8(status) || !r.u8(reason) || !r.u64(f.size) || !r.i64(f.modified)
            || status > static_cast<std::uint8_t>(ntfs::BulkStatus::TooLarge)
            || reason > static_cast<std::uint8_t>(ntfs::BulkSkip::TooLarge))
            return false;
        f.status = static_cast<ntfs::BulkStatus>(status);
        f.reason = static_cast<ntfs::BulkSkip>(reason);
        out.push_back(f);
    }
    return r.done();
}

void encode(Writer& w, std::span<const std::uint32_t> tickets)
{
    w.u32(static_cast<std::uint32_t>(tickets.size()));
    for (const std::uint32_t t : tickets)
        w.u32(t);
}

bool decode(std::span<const std::byte> payload, std::vector<std::uint32_t>& out)
{
    Reader r(payload);
    std::uint32_t count = 0;
    if (!r.u32(count) || count > payload.size() / 4)
        return false;
    out.reserve(out.size() + count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t t = 0;
        if (!r.u32(t))
            return false;
        out.push_back(t);
    }
    return r.done();
}

void encode(Writer& w, const BulkDataHeader& m, std::span<const std::byte> bytes)
{
    w.u32(m.ticket);
    w.u64(m.offset);
    w.u64(m.total);
    w.bytes(bytes);
}

bool decode(std::span<const std::byte> payload, BulkDataHeader& m, std::span<const std::byte>& bytes)
{
    Reader r(payload);
    return r.u32(m.ticket) && r.u64(m.offset) && r.u64(m.total) && r.bytes(bytes, r.left()) && m.offset <= m.total
        && bytes.size() <= m.total - m.offset;
}

// ---- MftBlock ----------------------------------------------------------------

void BlockWriter::begin(std::uint32_t firstRecord, bool withSizes, bool withCreated)
{
    m_writer.reset();
    m_writer.u32(firstRecord);
    m_writer.u32(0); // count, patched
    m_writer.u64(0); // bytesDone, patched
    m_count = 0;
    m_withSizes = withSizes;
    m_withCreated = withSizes && withCreated;
}

void BlockWriter::add(const ntfs::FileRecord& record, bool parsed)
{
    ++m_count;
    if (!parsed) {
        m_writer.u8(static_cast<std::uint8_t>(RecordKind::Invalid));
        return;
    }
    if (!record.inUse) {
        m_writer.u8(static_cast<std::uint8_t>(RecordKind::Free));
        return;
    }
    m_writer.u8(static_cast<std::uint8_t>(RecordKind::InUse));
    m_writer.u32(record.baseRecord);
    m_writer.u32(record.attributes);
    m_writer.i64(record.modified);
    m_writer.u8(static_cast<std::uint8_t>((record.directory ? kRecordDirectory : 0)
        | (record.hasAttributeList ? kRecordAttributeList : 0)
        | (m_withSizes && record.sizeKnown ? kRecordSizeKnown : 0)
        | (record.nameSurrogate ? kRecordNameSurrogate : 0)));
    const std::size_t names = std::min(record.names.size(), kMaxNames);
    m_writer.u8(static_cast<std::uint8_t>(names));
    for (std::size_t i = 0; i < names; ++i) {
        m_writer.u64(record.names[i].parent);
        m_writer.string(record.names[i].name); // NTFS names are at most 255 units: never cut
    }
    if (m_withSizes)
        m_writer.u64(record.sizeKnown ? record.size : 0);
    if (m_withCreated)
        m_writer.i64(record.created);
}

std::span<const std::byte> BlockWriter::finish(std::uint64_t bytesDone)
{
    m_writer.patchU32(4, m_count);
    m_writer.patchU64(8, bytesDone);
    return m_writer.finish(Type::MftBlock);
}

bool BlockReader::load(std::vector<std::byte>&& payload, bool withSizes, bool withCreated)
{
    m_offsets.clear();
    m_payload.clear();
    m_withSizes = withSizes;
    m_withCreated = withSizes && withCreated;
    Reader r(payload);
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    std::uint64_t done = 0;
    if (!r.u32(first) || !r.u32(count) || !r.u64(done) || count > r.left())
        return false; // at least a byte per record
    std::vector<std::uint32_t> offsets;
    offsets.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        offsets.push_back(static_cast<std::uint32_t>(r.position()));
        std::uint8_t kind = 0;
        if (!r.u8(kind) || kind > static_cast<std::uint8_t>(RecordKind::InUse))
            return false;
        if (kind != static_cast<std::uint8_t>(RecordKind::InUse))
            continue;
        std::uint32_t u = 0;
        std::int64_t t = 0;
        std::uint8_t flags = 0;
        std::uint8_t names = 0;
        if (!r.u32(u) || !r.u32(u) || !r.i64(t) || !r.u8(flags) || !r.u8(names))
            return false;
        for (std::uint8_t n = 0; n < names; ++n) {
            std::uint64_t parent = 0;
            std::u16string_view name;
            if (!r.u64(parent) || !r.string(name) || name.empty())
                return false;
        }
        std::uint64_t size = 0;
        if (withSizes && !r.u64(size))
            return false;
        if (m_withCreated && !r.i64(t))
            return false;
    }
    if (!r.done())
        return false; // bytes after the last record: not ours
    m_payload = std::move(payload);
    m_offsets = std::move(offsets);
    m_firstRecord = first;
    m_bytesDone = done;
    return true;
}

bool BlockReader::record(std::size_t i, ntfs::FileRecord& out) const
{
    out.inUse = false;
    out.directory = false;
    out.baseRecord = 0;
    out.attributes = 0;
    out.modified = 0;
    out.created = 0;
    out.hasAttributeList = false;
    out.nameSurrogate = false;
    out.size = 0;
    out.sizeKnown = false;
    out.names.clear();
    if (i >= m_offsets.size())
        return false;
    // Checked whole by load(): every read below has its bytes.
    Reader r(std::span<const std::byte>(m_payload).subspan(m_offsets[i]));
    std::uint8_t kind = 0;
    r.u8(kind);
    if (kind == static_cast<std::uint8_t>(RecordKind::Invalid))
        return false;
    if (kind != static_cast<std::uint8_t>(RecordKind::InUse))
        return true;
    out.inUse = true;
    std::uint8_t flags = 0;
    std::uint8_t names = 0;
    r.u32(out.baseRecord);
    r.u32(out.attributes);
    r.i64(out.modified);
    r.u8(flags);
    r.u8(names);
    out.directory = flags & kRecordDirectory;
    out.hasAttributeList = flags & kRecordAttributeList;
    out.nameSurrogate = flags & kRecordNameSurrogate;
    out.names.reserve(names);
    for (std::uint8_t n = 0; n < names; ++n) {
        ntfs::FileRecord::Name name {};
        r.u64(name.parent);
        r.string(name.name);
        out.names.push_back(name);
    }
    if (m_withSizes) {
        r.u64(out.size);
        out.sizeKnown = flags & kRecordSizeKnown;
    }
    if (m_withCreated)
        r.i64(out.created);
    return true;
}

// ---- Channel -------------------------------------------------------------------

Channel::Channel(void* pipe)
    : m_pipe(pipe == INVALID_HANDLE_VALUE ? nullptr : pipe)
    , m_readEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
    , m_writeEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
    , m_overlapped(std::make_unique<OVERLAPPED>())
{
}

Channel::~Channel()
{
    close();
    if (m_readEvent)
        ::CloseHandle(m_readEvent);
    if (m_writeEvent)
        ::CloseHandle(m_writeEvent);
}

Channel::Channel(Channel&& other) noexcept
    : m_pipe(std::exchange(other.m_pipe, nullptr))
    , m_readEvent(std::exchange(other.m_readEvent, nullptr))
    , m_writeEvent(std::exchange(other.m_writeEvent, nullptr))
    , m_overlapped(std::move(other.m_overlapped))
    , m_pending(std::exchange(other.m_pending, false))
    , m_error(other.m_error)
{
    std::memcpy(m_header, other.m_header, sizeof m_header);
}

Channel& Channel::operator=(Channel&& other) noexcept
{
    if (this != &other) {
        close();
        if (m_readEvent)
            ::CloseHandle(m_readEvent);
        if (m_writeEvent)
            ::CloseHandle(m_writeEvent);
        m_pipe = std::exchange(other.m_pipe, nullptr);
        m_readEvent = std::exchange(other.m_readEvent, nullptr);
        m_writeEvent = std::exchange(other.m_writeEvent, nullptr);
        m_overlapped = std::move(other.m_overlapped);
        m_pending = std::exchange(other.m_pending, false);
        m_error = other.m_error;
        std::memcpy(m_header, other.m_header, sizeof m_header);
    }
    return *this;
}

bool Channel::valid() const noexcept
{
    return m_pipe && m_readEvent && m_writeEvent && m_overlapped;
}

void Channel::close()
{
    if (!m_pipe)
        return;
    cancelRead();
    ::CloseHandle(m_pipe);
    m_pipe = nullptr;
}

bool Channel::write(Type type, std::span<const std::byte> payload, unsigned long timeoutMs)
{
    Writer w;
    w.bytes(payload);
    return write(w.finish(type), timeoutMs);
}

bool Channel::write(std::span<const std::byte> frame, unsigned long timeoutMs)
{
    if (!valid()) {
        m_error = ERROR_INVALID_HANDLE;
        return false;
    }
    const std::byte* p = frame.data();
    std::size_t n = frame.size();
    while (n > 0) {
        OVERLAPPED o {};
        o.hEvent = m_writeEvent;
        ::ResetEvent(m_writeEvent);
        DWORD put = 0;
        if (!::WriteFile(m_pipe, p, static_cast<DWORD>(std::min<std::size_t>(n, 1u << 20)), nullptr, &o)) {
            if (::GetLastError() != ERROR_IO_PENDING) {
                m_error = ::GetLastError();
                return false;
            }
            if (::WaitForSingleObject(m_writeEvent, timeoutMs) != WAIT_OBJECT_0) {
                // Not taken in time: whoever is at the other end is not reading.
                ::CancelIoEx(m_pipe, &o);
                ::GetOverlappedResult(m_pipe, &o, &put, TRUE);
                m_error = ERROR_TIMEOUT;
                return false;
            }
        }
        if (!::GetOverlappedResult(m_pipe, &o, &put, TRUE)) {
            m_error = ::GetLastError();
            return false;
        }
        if (put == 0) {
            m_error = ERROR_NO_DATA;
            return false;
        }
        p += put;
        n -= put;
    }
    return true;
}

bool Channel::readAll(std::byte* into, std::size_t n, unsigned long timeoutMs, void* abort)
{
    while (n > 0) {
        OVERLAPPED o {};
        o.hEvent = m_readEvent;
        ::ResetEvent(m_readEvent);
        DWORD got = 0;
        if (!::ReadFile(m_pipe, into, static_cast<DWORD>(std::min<std::size_t>(n, 1u << 20)), nullptr, &o)) {
            if (::GetLastError() != ERROR_IO_PENDING) {
                m_error = ::GetLastError();
                return false;
            }
            HANDLE handles[2] {m_readEvent, abort};
            const DWORD r = ::WaitForMultipleObjects(abort ? 2 : 1, handles, FALSE, timeoutMs);
            if (r != WAIT_OBJECT_0) {
                ::CancelIoEx(m_pipe, &o);
                ::GetOverlappedResult(m_pipe, &o, &got, TRUE);
                m_error = r == WAIT_OBJECT_0 + 1 ? ERROR_OPERATION_ABORTED : ERROR_TIMEOUT;
                return false;
            }
        }
        if (!::GetOverlappedResult(m_pipe, &o, &got, TRUE)) {
            m_error = ::GetLastError();
            return false;
        }
        if (got == 0) {
            m_error = ERROR_BROKEN_PIPE;
            return false;
        }
        into += got;
        n -= got;
    }
    return true;
}

bool Channel::read(Type& type, std::vector<std::byte>& payload, unsigned long timeoutMs, void* abort)
{
    if (!valid() || m_pending) {
        m_error = ERROR_INVALID_HANDLE;
        return false;
    }
    if (!readAll(m_header, sizeof m_header, timeoutMs, abort))
        return false;
    std::uint32_t length = 0;
    if (!parseHeader(m_header, type, length)) {
        m_error = ERROR_INVALID_DATA;
        return false;
    }
    payload.resize(length);
    return length == 0 || readAll(payload.data(), length, timeoutMs, abort);
}

bool Channel::beginRead()
{
    if (!valid()) {
        m_error = ERROR_INVALID_HANDLE;
        return false;
    }
    if (m_pending)
        return true;
    *m_overlapped = OVERLAPPED {};
    m_overlapped->hEvent = m_readEvent;
    ::ResetEvent(m_readEvent);
    m_pending = true;
    m_error = 0;
    if (!::ReadFile(m_pipe, m_header, sizeof m_header, nullptr, m_overlapped.get())
        && ::GetLastError() != ERROR_IO_PENDING) {
        // finishRead() reports it; the event is set so a waiter comes to ask.
        m_error = ::GetLastError();
        m_pending = false;
        ::SetEvent(m_readEvent);
        return false;
    }
    return true;
}

bool Channel::finishRead(Type& type, std::vector<std::byte>& payload, unsigned long timeoutMs)
{
    if (!m_pending) {
        if (m_error == 0)
            m_error = ERROR_INVALID_HANDLE;
        return false;
    }
    DWORD got = 0;
    if (::WaitForSingleObject(m_readEvent, timeoutMs) != WAIT_OBJECT_0) {
        cancelRead();
        m_error = ERROR_TIMEOUT;
        return false;
    }
    m_pending = false;
    if (!::GetOverlappedResult(m_pipe, m_overlapped.get(), &got, TRUE)) {
        m_error = ::GetLastError();
        return false;
    }
    if (got == 0) {
        m_error = ERROR_BROKEN_PIPE;
        return false;
    }
    // A byte-mode pipe may hand over less than asked for: the rest follows.
    if (got < sizeof m_header && !readAll(m_header + got, sizeof m_header - got, timeoutMs, nullptr))
        return false;
    std::uint32_t length = 0;
    if (!parseHeader(m_header, type, length)) {
        m_error = ERROR_INVALID_DATA;
        return false;
    }
    payload.resize(length);
    return length == 0 || readAll(payload.data(), length, timeoutMs, nullptr);
}

void Channel::cancelRead()
{
    if (!m_pending)
        return;
    m_pending = false;
    ::CancelIoEx(m_pipe, m_overlapped.get());
    DWORD ignored = 0;
    ::GetOverlappedResult(m_pipe, m_overlapped.get(), &ignored, TRUE);
}

} // namespace ws::volproto
