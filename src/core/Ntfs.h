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

// How a volume is laid out, as FSCTL_GET_NTFS_VOLUME_DATA reports it.
struct Geometry {
    std::uint32_t sectorSize = 512;
    std::uint32_t clusterSize = 4096; // 512 bytes (volumes converted from FAT32, old ones) to 2 MB
    std::uint32_t recordSize = 1024; // 4096 on 4K-sector disks and with "format /L"
};

// Whether MftReader reads such a volume: any sizes NTFS uses, a cluster
// smaller than a record too.
bool supported(const Geometry& geometry);

// A stretch of the MFT itself.
struct Stretch {
    std::uint64_t at; // bytes from the MFT's start
    std::uint64_t length; // bytes
};

// The stretches of the first `bytes` of the MFT whose records are all free
// by its bitmap (bit i set: record i in use): whole `granule`s (a multiple
// of 8 records), at least `minBytes` long. Records the bitmap does not reach
// count as in use.
std::vector<Stretch> freeStretches(std::span<const std::byte> bitmap, std::uint32_t recordSize, std::uint64_t granule,
    std::uint64_t minBytes, std::uint64_t bytes);

// One read of the MFT.
struct Read {
    std::uint64_t offset; // bytes from the start of the volume
    std::uint64_t length; // bytes
    std::uint64_t at; // where they are in the MFT
};

// The reads that cover the first `bytes` of the MFT but `skip` (sorted, and
// on cluster boundaries), in order: each within one extent, of whole clusters
// (the volume is read unbuffered), and at most `blockBytes` long.
std::vector<Read> planReads(std::span<const Extent> extents, std::uint64_t bytes, std::uint32_t clusterSize,
    std::uint64_t blockBytes, std::span<const Stretch> skip = {});

struct ReadOptions {
    // At least 2, so the disk reads the next block while this one is parsed;
    // 4 was 9-16 % faster than 2 on an NVMe SSD (for 16 MB of buffers).
    int readsInFlight = 4;
    bool skipFree = true; // leave out long stretches of free records (by the MFT's bitmap)
};

// Reads a volume's MFT front to back in large blocks, a few at a time.
class MftReader {
public:
    explicit MftReader(std::wstring_view root, ReadOptions options = {}); // "C:"
    // An MFT in a volume image held in memory (tests). Without `extents`,
    // they are read from the MFT's own record at `mftStart` (bytes).
    MftReader(std::span<const std::byte> image, Geometry geometry, std::vector<Extent> extents, std::uint64_t mftStart,
        std::uint64_t validBytes, ReadOptions options = {});
    ~MftReader();
    MftReader(const MftReader&) = delete;
    MftReader& operator=(const MftReader&) = delete;

    bool valid() const noexcept { return m_error.empty(); }
    const std::wstring& error() const noexcept { return m_error; }
    const Geometry& geometry() const noexcept { return m_geometry; }
    std::uint64_t recordCount() const noexcept { return m_validBytes / m_geometry.recordSize; }
    std::uint64_t bytes() const noexcept { return m_validBytes; } // of the MFT that hold records
    std::uint64_t bytesDone() const noexcept { return m_streamPos; } // of those, handed out or left out so far
    std::uint64_t bytesSkipped() const noexcept { return m_skipped; } // left out: free records only
    std::size_t extentCount() const noexcept { return m_extents.size(); } // diagnostics
    std::span<const std::byte> bitmap() const noexcept { return m_bitmap; } // diagnostics: empty when none was read
    std::uint32_t bitmapRecord() const noexcept { return m_bitmapRecord; } // diagnostics
    const std::wstring& bitmapNote() const noexcept { return m_bitmapNote; } // diagnostics: where it was, or why not
    bool extentsFromRecordZero() const noexcept { return m_fromRecordZero; } // diagnostics

    // Reads the next block. False at the end, or on a read error (error() says).
    bool next();
    std::uint32_t firstRecord() const noexcept { return m_firstRecord; }
    std::size_t blockRecords() const noexcept { return m_blockRecords; }
    bool parse(std::size_t i, FileRecord& out); // record i of the current block

private:
    class Device;
    class VolumeDevice;
    class ImageDevice;
    // A read in flight or done. Its data is preceded by room for the start
    // of a record that the read before it ended with.
    struct Slot {
        std::byte* data = nullptr;
        std::uint32_t bytes = 0;
        std::uint64_t at = 0; // where its data is in the MFT
        std::size_t carried = 0; // bytes of a record copied in front of `data`
        bool pending = false;
    };

    void start(std::uint64_t mftStart);
    bool readRecordZero(std::uint64_t mftStart, std::wstring& error); // into the first buffer
    bool extentsFrom(const std::byte* recordZero);
    std::vector<std::byte> bitmapFrom(const std::byte* recordZero);
    std::vector<std::byte> readRecord(std::uint32_t number); // fixed up; through the second buffer
    std::vector<std::byte> readRuns(std::span<const Extent> runs, std::uint64_t size); // through the second buffer
    bool readInto(int slot, std::uint64_t offset, std::uint32_t bytes); // into that slot's buffer, waited for
    bool issue(int slot); // starts the next planned read in `slot`
    void fail(const wchar_t* what);

    std::unique_ptr<Device> m_device;
    ReadOptions m_options;
    std::wstring m_error;
    Geometry m_geometry;
    std::uint64_t m_validBytes = 0;
    std::vector<Extent> m_extents;
    bool m_fromRecordZero = false;
    std::vector<Read> m_plan;
    std::size_t m_planned = 0; // reads started so far
    std::uint64_t m_skipped = 0;
    std::vector<std::byte> m_bitmap;
    std::uint32_t m_bitmapRecord = 0; // the record its first part was found in
    std::wstring m_bitmapNote;
    std::byte* m_memory = nullptr;
    std::vector<Slot> m_slots; // in a ring: the reads in flight, in the MFT's order
    int m_turn = 0; // the slot whose read comes next in the MFT
    int m_handedOut = -1; // the slot whose records the caller is parsing
    std::uint64_t m_streamPos = 0; // where in the MFT the records still to hand out begin
    std::byte* m_records = nullptr;
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
