#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct _OVERLAPPED;

// Direct access to NTFS volumes: the master file table (MFT), which lists
// every file and folder, and the change journal, which logs every change to
// them. Both need administrator rights (they open the volume itself).
namespace ws::ntfs {

inline constexpr std::uint32_t kRootRecord = 5; // the root folder's record number

// A record number is the low 48 bits of a file reference number (the high 16
// count reuses of the record). 0 when it does not fit in 32 bits, which would
// take a volume with over 4 billion files.
constexpr std::uint32_t recordOf(std::uint64_t fileReference) noexcept
{
    const std::uint64_t record = fileReference & 0xFFFF'FFFF'FFFFull;
    return record <= 0xFFFF'FFFFull ? static_cast<std::uint32_t>(record) : 0;
}

// ---- MFT -------------------------------------------------------------------

// One file record of the MFT, parsed.
struct FileRecord {
    struct Name {
        std::uint64_t parent; // file reference number of the folder holding it
        std::u16string_view name; // points into the record
    };
    bool inUse = false;
    bool directory = false;
    std::uint32_t baseRecord = 0; // non-zero: an extension of that record
    std::uint32_t attributes = 0; // FILE_ATTRIBUTE_*, from $STANDARD_INFORMATION (base records only)
    bool hasAttributeList = false; // some attributes live in extension records
    std::vector<Name> names; // one per hard link; DOS 8.3 aliases are left out
};

// Parses one record, undoing its update sequence protection in place.
// Returns false for anything that is not an intact file record.
bool parseFileRecord(std::byte* data, std::size_t size, FileRecord& out);

struct Extent {
    std::uint64_t offset; // bytes from the start of the volume
    std::uint64_t length; // bytes
};

// Decodes the MFT's run list (the mapping pairs of its $DATA attribute).
// Returns nothing for malformed or sparse runs.
std::optional<std::vector<Extent>> decodeRunList(std::span<const std::byte> pairs, std::uint64_t clusterSize);

// Reads a volume's MFT front to back in large blocks.
class MftReader {
public:
    explicit MftReader(std::wstring_view root); // "C:"
    ~MftReader();
    MftReader(const MftReader&) = delete;
    MftReader& operator=(const MftReader&) = delete;

    bool valid() const noexcept { return m_error.empty(); }
    const std::wstring& error() const noexcept { return m_error; }
    std::uint64_t recordCount() const noexcept { return m_validBytes / m_recordSize; }
    bool extentsFromRecordZero() const noexcept { return m_fromRecordZero; } // diagnostics

    // Reads the next block. False at the end, or on a read error (error() says).
    bool next();
    std::uint32_t firstRecord() const noexcept { return m_firstRecord; }
    std::size_t blockRecords() const noexcept { return m_blockRecords; }
    bool parse(std::size_t i, FileRecord& out); // record i of the current block

private:
    bool findExtents(std::wstring_view root, std::uint64_t mftStart, std::uint32_t sectorSize);
    void fail(const wchar_t* what);

    void* m_volume = nullptr;
    std::wstring m_error;
    std::uint32_t m_recordSize = 1024;
    std::uint32_t m_clusterSize = 0;
    std::uint64_t m_validBytes = 0;
    std::vector<Extent> m_extents;
    bool m_fromRecordZero = false;
    std::size_t m_extent = 0;
    std::uint64_t m_extentPos = 0;
    std::uint64_t m_consumed = 0; // bytes of the MFT read so far
    std::byte* m_buffer = nullptr;
    std::uint32_t m_firstRecord = 0;
    std::size_t m_blockRecords = 0;
};

// ---- change journal ----------------------------------------------------------

struct JournalInfo {
    std::uint64_t id = 0;
    std::int64_t firstUsn = 0; // the oldest record still kept
    std::int64_t nextUsn = 0; // where the next record will go
};

// The volume's change journal, created if the volume has none.
std::optional<JournalInfo> queryJournal(std::wstring_view root);

struct UsnRecord {
    std::uint64_t file; // file reference number
    std::uint64_t parent; // of its folder
    std::int64_t usn;
    std::uint32_t reason; // USN_REASON_*
    std::uint32_t attributes; // FILE_ATTRIBUTE_*
    std::u16string_view name; // points into the read buffer
};

// Parses what FSCTL_READ_USN_JOURNAL returns. Returns the USN to continue
// from, or -1 when the buffer is malformed.
std::int64_t parseUsnRecords(std::span<const std::byte> data, std::vector<UsnRecord>& out);

// Reads one volume's change journal with overlapped I/O, so that one thread
// can wait on several volumes. Only name, attribute, link and content
// changes are read.
class JournalReader {
public:
    JournalReader(std::wstring_view root, std::uint64_t journalId);
    ~JournalReader();
    JournalReader(const JournalReader&) = delete;
    JournalReader& operator=(const JournalReader&) = delete;

    bool valid() const noexcept;
    void* event() const noexcept { return m_event; } // signaled when a read completes

    // Starts reading from `usn` on. With `wait` the read completes once there
    // is something new; without, at once (possibly with nothing).
    void start(std::int64_t usn, bool wait);
    // Waits for the read to complete. False when the journal no longer has
    // those records (it wrapped, or was deleted) or the volume is gone.
    // Records point into this reader's buffer until the next start().
    bool finish(std::vector<UsnRecord>& records, std::int64_t& nextUsn);
    void cancel();

private:
    void* m_volume = nullptr;
    void* m_event = nullptr;
    std::uint64_t m_journalId;
    std::vector<std::uint64_t> m_buffer; // 8-byte aligned
    std::unique_ptr<_OVERLAPPED> m_overlapped;
    bool m_pending = false;
    unsigned long m_startError = 0;
};

} // namespace ws::ntfs
