#pragma once

#include "Ntfs.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What goes over the pipe between Win顺 and its index service
// (WinShunService.exe, src/service; the client is ServiceVolumeAccess): the
// frames, their payloads, and a channel that reads and writes them on a pipe
// handle. No Qt: the service has none. Little-endian, both ends on the same
// x64 machine. Neither end trusts the other's lengths: every one is checked
// before it is used.
//
// Each operation is a connection of its own. The first frame is Hello /
// HelloOk; then one of:
//   OpenMft       -> MftInfo, MftBlock..., MftDone
//   QueryJournal  -> JournalInfo | Error
//   FollowJournal -> Following | Error; then Read -> UsnBatch | JournalLost,
//                    Cancel -> Cancelled, any number of times
//   CloseVolume   -> Closed
//   OpenBulkRead  -> BulkReady | Error; then BulkWant... BulkScan -> BulkFiles... BulkScanned,
//                    then BulkFetch -> BulkData... BulkFetched, any number of times
//                    (the content index's bulk path, Ntfs.h BulkSource; the
//                    service's HelloOk says whether it has it: kCapabilityBulkRead)
// VolumeLost comes from the service on an MFT, journal or bulk connection
// whose volume it had to let go of (an eject, a lock, or the client's own
// CloseVolume on another connection).
namespace ws::volproto {

constexpr std::uint16_t kMajor = 1; // different on the two ends: they cannot talk
// Only ever adds fields at the end of a payload; 1: bulk reads; 2: each
// in-use record of an MftBlock ends with the file's size (both ends at 2 or
// more: the writer is told the other end's minor, see BlockWriter::begin).
// Minor 3 appends the creation time after the size, on both ends at 3+.
// Minor 4 adds the dialog-navigation capability (kCapabilityDialogNav) and the
// OpenDialogNav request; an older service without the bit leaves the UI's
// copy-and-paste fallback in place. A service that has the navigator but
// keeps it for the WinShun.exe beside it tells any other client so
// (kCapabilityDialogNavBesideOnly), for that copy's hint.
constexpr std::uint16_t kMinor = 4;
constexpr std::uint16_t kMinorSizes = 2;
constexpr std::uint16_t kMinorCreated = 3;
constexpr std::uint32_t kCapabilityBulkRead = 1; // HelloOk::capabilities
constexpr std::uint32_t kCapabilityKeyListener = 2; // UIAccess listener, on the caller's desktop
constexpr std::uint32_t kCapabilityDialogNav = 4; // minor 4: navigate an elevated file dialog (GitHub #104)
constexpr std::uint32_t kCapabilityDialogNavBesideOnly = 8; // minor 4: has it, for the WinShun.exe beside it only
constexpr std::size_t kMaxBulkWants = 4u << 20; // files one bulk session may ask about
constexpr wchar_t kPipeName[] = L"WinShun.Volumes"; // one service for the whole machine
constexpr wchar_t kPipeEnvironment[] = L"WINSHUN_PIPE"; // another pipe's name, for tests (--console --pipe)
constexpr wchar_t kServiceName[] = L"WinShun";
constexpr std::size_t kMaxPayloadBytes = 4u << 20; // a larger frame is refused unread
constexpr std::size_t kBlockBytes = 1u << 20; // the service ends an MftBlock past this
constexpr std::size_t kMaxNames = 255; // per record in an MftBlock (a 4 KB record holds some 60)

enum class Type : std::uint16_t {
    Hello = 1,
    HelloOk = 2,
    HelloRefused = 3,
    OpenMft = 10,
    MftInfo = 11,
    MftBlock = 12,
    MftDone = 13,
    QueryJournal = 20,
    JournalInfo = 21,
    FollowJournal = 30,
    Following = 31,
    Read = 32,
    UsnBatch = 33,
    JournalLost = 34,
    Cancel = 35,
    Cancelled = 36,
    CloseVolume = 40,
    Closed = 41,
    VolumeLost = 50,
    Error = 60,
    OpenBulkRead = 70,
    BulkReady = 71,
    BulkWant = 72,
    BulkScan = 73,
    BulkFiles = 74,
    BulkScanned = 75,
    BulkFetch = 76,
    BulkData = 77,
    BulkFetched = 78,
    OpenKeyListener = 80, // u64: caller's KeyListener HWND; no path or session supplied by the client
    KeyListenerReady = 81, // u32: child PID; connection owns the child's lifetime
    KeyListenerPing = 82, // empty request and reply, once a second
    OpenDialogNav = 83, // { u64 dialog HWND, string folder }: navigate a higher-integrity file dialog (#104)
    DialogNavDone = 84, // empty reply: the helper took the dialog to the folder (else Error)
};

std::wstring fullPipePath(std::wstring_view name); // "\\.\pipe\" + name

// ---- payloads ----------------------------------------------------------------

struct Hello {
    std::uint16_t major = kMajor;
    std::uint16_t minor = kMinor;
    std::uint16_t version[3] {}; // of the program
    std::uint32_t pid = 0; // for the log only: the service asks the pipe who the client is
};

struct HelloOk {
    std::uint16_t major = kMajor;
    std::uint16_t minor = kMinor;
    std::uint16_t version[3] {};
    std::uint32_t capabilities = 0; // none yet
};

enum class Refusal : std::uint32_t { Protocol = 1, TooManyConnections = 2 };

struct HelloRefused {
    std::uint32_t reason = 0; // Refusal
};

struct OpenMft {
    std::wstring root; // "C:"
    std::uint32_t serial = 0; // 0: not checked
    bool lowPriority = false;
};

struct MftInfo {
    ntfs::Geometry geometry;
    std::uint64_t recordCount = 0;
    std::uint64_t validBytes = 0;
};

enum class MftStatus : std::uint32_t { Done = 0, Failed = 1, VolumeLost = 2 };

struct MftDone {
    std::uint32_t status = 0; // MftStatus
    std::wstring error; // when Failed
};

struct QueryJournal {
    std::wstring root;
    std::uint32_t serial = 0;
};

struct FollowJournal {
    std::wstring root;
    std::uint32_t serial = 0;
    std::uint64_t journalId = 0;
};

struct Read {
    std::int64_t usn = 0;
    bool wait = false;
};

struct JournalLost {
    std::uint32_t error = 0; // the Win32 error of the read
};

struct CloseVolume {
    std::wstring root;
};

struct Closed {
    std::uint32_t closed = 0; // connections let go of
};

struct Error {
    std::uint32_t code = 0; // Win32
    std::wstring message;
};

struct OpenBulkRead {
    std::wstring root; // "C:"
    std::uint32_t serial = 0; // 0: not checked
    bool quiet = false; // a hard disk someone is using: one read at a time, very low I/O priority
};

struct BulkScanned {
    std::uint32_t matched = 0; // wants with a record
    std::uint32_t checks = 0; // access checks made (descriptors fetched)
};

// A BulkData frame: the header, then the bytes (the rest of the payload). A
// file longer than a frame comes in several, `offset` moving on; the client
// has the whole once offset + bytes == total.
struct BulkDataHeader {
    std::uint32_t ticket = 0;
    std::uint64_t offset = 0;
    std::uint64_t total = 0;
};

// ---- encoding ----------------------------------------------------------------

// Builds one frame: the header in front, patched on finish().
class Writer {
public:
    Writer();
    void u8(std::uint8_t v);
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void i64(std::int64_t v);
    void string(std::u16string_view s); // u16 length (code units), then them
    void string(std::wstring_view s);
    void bytes(std::span<const std::byte> b);
    std::size_t payloadBytes() const noexcept { return m_data.size() - kHeaderBytes; }
    // The frame, header and all. The writer can go on being used for the next one (reset()).
    std::span<const std::byte> finish(Type type);
    void reset();
    // Patches a u32 / u64 written earlier at `at` (payload offset).
    void patchU32(std::size_t at, std::uint32_t v);
    void patchU64(std::size_t at, std::uint64_t v);

    static constexpr std::size_t kHeaderBytes = 8;

private:
    std::vector<std::byte> m_data;
};

// Reads a payload field by field; every read says whether the bytes were there.
class Reader {
public:
    explicit Reader(std::span<const std::byte> data) noexcept
        : m_data(data)
    {
    }
    bool u8(std::uint8_t& v) noexcept;
    bool u16(std::uint16_t& v) noexcept;
    bool u32(std::uint32_t& v) noexcept;
    bool u64(std::uint64_t& v) noexcept;
    bool i64(std::int64_t& v) noexcept;
    bool boolean(bool& v) noexcept;
    bool string(std::wstring& s); // as Writer::string wrote it
    bool string(std::u16string_view& s) noexcept; // a view into the payload
    bool bytes(std::span<const std::byte>& b, std::size_t n) noexcept;
    bool done() const noexcept { return m_pos == m_data.size(); }
    std::size_t position() const noexcept { return m_pos; }
    std::size_t left() const noexcept { return m_data.size() - m_pos; }

private:
    std::span<const std::byte> m_data;
    std::size_t m_pos = 0;
};

// The header of a frame: false when it is not one we would read (too long).
bool parseHeader(std::span<const std::byte, 8> header, Type& type, std::uint32_t& length) noexcept;

void encode(Writer& w, const Hello& m);
void encode(Writer& w, const HelloOk& m);
void encode(Writer& w, const HelloRefused& m);
void encode(Writer& w, const OpenMft& m);
void encode(Writer& w, const MftInfo& m);
void encode(Writer& w, const MftDone& m);
void encode(Writer& w, const QueryJournal& m);
void encode(Writer& w, const ntfs::JournalInfo& m);
void encode(Writer& w, const FollowJournal& m);
void encode(Writer& w, const Read& m);
void encode(Writer& w, const JournalLost& m);
void encode(Writer& w, const CloseVolume& m);
void encode(Writer& w, const Closed& m);
void encode(Writer& w, const Error& m);

// Each false on a payload that is cut short. Bytes past the fields known
// here are left alone: a later minor version may add some.
bool decode(std::span<const std::byte> payload, Hello& m);
bool decode(std::span<const std::byte> payload, HelloOk& m);
bool decode(std::span<const std::byte> payload, HelloRefused& m);
bool decode(std::span<const std::byte> payload, OpenMft& m);
bool decode(std::span<const std::byte> payload, MftInfo& m);
bool decode(std::span<const std::byte> payload, MftDone& m);
bool decode(std::span<const std::byte> payload, QueryJournal& m);
bool decode(std::span<const std::byte> payload, ntfs::JournalInfo& m);
bool decode(std::span<const std::byte> payload, FollowJournal& m);
bool decode(std::span<const std::byte> payload, Read& m);
bool decode(std::span<const std::byte> payload, JournalLost& m);
bool decode(std::span<const std::byte> payload, CloseVolume& m);
bool decode(std::span<const std::byte> payload, Closed& m);
bool decode(std::span<const std::byte> payload, Error& m);
void encode(Writer& w, const OpenBulkRead& m);
bool decode(std::span<const std::byte> payload, OpenBulkRead& m);
void encode(Writer& w, const BulkScanned& m);
bool decode(std::span<const std::byte> payload, BulkScanned& m);

// The bulk frames with lists: each { u32 count } then the items. A BulkWant
// item is { u32 parentRecord, u64 maxBytes, string name }; the i-th want of
// the session is ticket i. A BulkFiles item is { u32 ticket, u8 status, u8
// reason, u64 size, i64 modified }. A BulkFetch item is a u32 ticket. The
// decoders append to `out`; false for a payload cut short or over
// kMaxPayloadBytes worth of items.
void encode(Writer& w, std::span<const ntfs::BulkWant> wants);
bool decode(std::span<const std::byte> payload, std::vector<ntfs::BulkWant>& out);
void encode(Writer& w, std::span<const ntfs::BulkFile> files);
bool decode(std::span<const std::byte> payload, std::vector<ntfs::BulkFile>& out);
void encode(Writer& w, std::span<const std::uint32_t> tickets);
bool decode(std::span<const std::byte> payload, std::vector<std::uint32_t>& out);
void encode(Writer& w, const BulkDataHeader& m, std::span<const std::byte> bytes);
bool decode(std::span<const std::byte> payload, BulkDataHeader& m, std::span<const std::byte>& bytes);

// An MftBlock: { u32 firstRecord, u32 count, u64 bytesDone }, then `count`
// records in order, each { u8 kind } and, for a record in use, { u32
// baseRecord, u32 attributes, i64 modified, u8 flags, u8 nameCount } and
// each name { u64 parent, u16 length, the UTF-16 units }. Record kinds:
enum class RecordKind : std::uint8_t { Invalid = 0, Free = 1, InUse = 2 }; // Invalid: not an intact record
constexpr std::uint8_t kRecordDirectory = 1;
constexpr std::uint8_t kRecordAttributeList = 2;
constexpr std::uint8_t kRecordSizeKnown = 4; // minor 2: the size that follows is the file's (FileRecord::sizeKnown)

// Writes an MftBlock record by record, as the service parses them.
class BlockWriter {
public:
    // `withSizes`: the reader speaks minor 2 or more (its Hello said so):
    // every in-use record carries the file's size.
    // `withCreated`: both ends speak minor 3 or more; creation follows size.
    void begin(std::uint32_t firstRecord, bool withSizes = true, bool withCreated = true);
    // The next record in order: parsed, or not one (`parsed` false).
    void add(const ntfs::FileRecord& record, bool parsed);
    std::uint32_t count() const noexcept { return m_count; }
    std::size_t payloadBytes() const noexcept { return m_writer.payloadBytes(); }
    std::span<const std::byte> finish(std::uint64_t bytesDone); // the frame

private:
    Writer m_writer;
    std::uint32_t m_count = 0;
    bool m_withSizes = true;
    bool m_withCreated = true;
};

// Reads an MftBlock: checked whole on load(), then any record by number.
class BlockReader {
public:
    // Takes the payload; false when it is malformed (nothing is kept then).
    // `withSizes`: the writer speaks minor 2 or more (its HelloOk said so).
    // `withCreated`: the writer speaks minor 3 or more.
    bool load(std::vector<std::byte>&& payload, bool withSizes = true, bool withCreated = true);
    std::uint32_t firstRecord() const noexcept { return m_firstRecord; }
    std::uint32_t count() const noexcept { return static_cast<std::uint32_t>(m_offsets.size()); }
    std::uint64_t bytesDone() const noexcept { return m_bytesDone; }
    // Record i of the block, as parseFileRecord would have given it. False
    // for one that was not an intact record. Names point into the payload.
    bool record(std::size_t i, ntfs::FileRecord& out) const;

private:
    std::vector<std::byte> m_payload;
    std::vector<std::uint32_t> m_offsets; // of each record's kind byte
    std::uint32_t m_firstRecord = 0;
    std::uint64_t m_bytesDone = 0;
    bool m_withSizes = true;
    bool m_withCreated = true;
};

// ---- the pipe -------------------------------------------------------------------

// Frames on one pipe handle (either end). Reads and writes are overlapped,
// so that a read can be waited on among other events (event()), and so
// that either can be given up after a while or on an event.
class Channel {
public:
    Channel() = default;
    explicit Channel(void* pipe); // takes the handle
    ~Channel();
    Channel(Channel&& other) noexcept;
    Channel& operator=(Channel&& other) noexcept;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    bool valid() const noexcept;
    void* handle() const noexcept { return m_pipe; }
    void close(); // cancels what is in flight

    // Writes a whole frame. False when the other end is gone, or did not
    // take it within `timeoutMs` (the connection is then no good: close it).
    bool write(std::span<const std::byte> frame, unsigned long timeoutMs);
    bool write(Type type, std::span<const std::byte> payload, unsigned long timeoutMs);
    // Reads a whole frame: false when the other end is gone, the header is
    // not a frame's, or nothing came within `timeoutMs` / `abort` was set
    // (the read is cancelled then; the connection is no good).
    bool read(Type& type, std::vector<std::byte>& payload, unsigned long timeoutMs, void* abort = nullptr);

    // Starts reading the next frame: event() is set once its first bytes are
    // in (or the pipe broke). finishRead() then reads the rest. One at a time.
    bool beginRead();
    bool readPending() const noexcept { return m_pending; }
    void* event() const noexcept { return m_readEvent; }
    bool finishRead(Type& type, std::vector<std::byte>& payload, unsigned long timeoutMs);
    void cancelRead();

    unsigned long lastError() const noexcept { return m_error; }

private:
    bool readAll(std::byte* into, std::size_t n, unsigned long timeoutMs, void* abort);
    bool waitRead(unsigned long timeoutMs, void* abort); // the overlapped read in m_overlapped, into m_got

    void* m_pipe = nullptr;
    void* m_readEvent = nullptr;
    void* m_writeEvent = nullptr;
    std::unique_ptr<_OVERLAPPED> m_overlapped; // the read in flight
    std::byte m_header[8] {};
    bool m_pending = false;
    unsigned long m_error = 0;
};

} // namespace ws::volproto
