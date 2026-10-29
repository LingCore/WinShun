#pragma once

// Where received files go, and how. A name from another device is only
// ever a name: made into a path under the save folder or refused (no "..",
// no drive, no share, no device names); written to a hidden "<name>.wspart"
// first (straight in the save folder, whatever folder the file goes to:
// one left by a process that was ended is found there at the next start),
// marked as from the internet (Zone.Identifier, ZoneId=3, which LocalSend
// itself does not do) and renamed into place once it is whole (its size the
// one declared and, when the sender gave one, its SHA-256 agreeing), never
// over a file that is there ("名字 (1).ext").

#include "Util.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace localsend {

inline constexpr std::size_t kMaxSegmentChars = 200;
inline constexpr std::size_t kMaxPathChars = 240; // the whole path; Explorer and many programs trip over more
inline constexpr int kMaxDepth = 32;

// `name` as sent ("相册/IMG_1.jpg") as a relative path ("相册\IMG_1.jpg"):
// split on '/' and '\'; a ".." anywhere, a drive ("C:"), a share or a root
// refuses the whole name (nullopt, `why` saying so); "." and empty parts
// are dropped; <>:"|?*, control characters and the marks that turn text
// around (U+202E: "报告<RLO>fdp.exe" shown as "报告exe.pdf") become '_'; trailing dots and
// spaces go; CON, PRN, AUX, NUL, COM1-9, LPT1-9 (any extension) get a '_'
// in front; a part is at most kMaxSegmentChars (the extension kept); at
// most kMaxDepth levels. An empty result is "文件".
std::optional<std::wstring> safeRelativePath(std::string_view name, std::string* why = nullptr);

// Case-insensitive as NTFS compares names.
struct NoCaseLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const;
};

// Where the files of one transfer go under the save folder: a folder sent
// along keeps its name unless one of that name was there before ("相册 (1)",
// once for the whole transfer); a file that would land on another gets
// " (1)" before its extension. Names handed out are kept until the
// transfer ends, so two of its files never get the same.
class Placement {
public:
    explicit Placement(std::wstring folder) : m_folder(std::move(folder)) {}
    std::wstring place(const std::wstring& relative); // the full path
    // The folders sent along, as placed (full paths): none was there before.
    std::vector<std::wstring> folders() const;

private:
    bool taken(const std::wstring& path) const;
    std::wstring m_folder;
    std::map<std::wstring, std::wstring, NoCaseLess> m_tops; // first folder as sent -> as placed
    std::set<std::wstring, NoCaseLess> m_given;
};

// Why a received file was not kept: the sender's side (it came short, or
// more than declared), what came is not what was sent, or this PC's disk
// (full, or the file could not be made, written or renamed).
enum class Fault { Sender, Checksum, Disk };

// A file being received.
class IncomingFile {
public:
    // The hidden partial file goes in `partialFolder` ("": the final one's).
    static std::unique_ptr<IncomingFile> create(std::wstring finalPath, std::uint64_t declaredSize, std::string sha256,
        std::string* error, const std::wstring& partialFolder = {});
    ~IncomingFile(); // abort() unless finished
    IncomingFile(const IncomingFile&) = delete;
    IncomingFile& operator=(const IncomingFile&) = delete;

    // False when it would be more than declared, or the disk took no more.
    bool write(const char* data, std::size_t size);
    // Whole: marked and renamed into place (a free name if its own was
    // taken meanwhile); the path it got. Nothing when it is not whole, the
    // checksum disagrees or the rename fails (`why`, `fault`), and then it
    // is deleted.
    std::optional<std::wstring> finish(const std::string& hostUrl, std::string* why, Fault* fault = nullptr);
    void abort(); // the partial file deleted

private:
    IncomingFile() = default;
    std::wstring m_final;
    std::wstring m_temp;
    Handle m_file;
    std::uint64_t m_declared = 0;
    std::uint64_t m_written = 0;
    std::string m_expectedSha;
    std::unique_ptr<Sha256> m_hash; // only when the sender gave a checksum
    bool m_done = false;
};

// Zone.Identifier: ZoneId=3 (the internet) and where it came from. False
// (`error` the system's code) when the disk has no named streams (FAT32,
// exFAT) or it could not be written.
bool writeMarkOfTheWeb(const std::wstring& path, const std::string& hostUrl, DWORD* error = nullptr);
std::uint64_t freeBytes(const std::wstring& folder); // 0 when unknown
std::wstring downloadsFolder();
bool ensureFolder(const std::wstring& folder); // made with its parents when missing
// Files can go to `folder`: it is a folder, or the nearest one above it that
// is there is (it is made when a file comes). False when its drive or share
// is not there (a USB disk pulled out, a NAS gone), or it is a file. Made
// nothing; a share that sleeps may take seconds to say.
bool folderUsable(const std::wstring& folder);
// Hidden "*.wspart" files straight in `folder`, left by a transfer whose
// process was ended: deleted, unless a program still writes one (it is
// open without delete sharing). How many went.
std::size_t sweepPartials(const std::wstring& folder);
// `folder` and the folders under it, the ones that are empty (links not
// followed): a transfer that ended early made them.
void removeEmptyFolders(const std::wstring& folder);

} // namespace localsend
