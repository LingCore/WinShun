#pragma once

// What is inside an archive (zip, 7z, rar, tar and its compressed kinds, cab,
// iso...): the names, sizes and times of its members, read through
// libarchive; and one member taken out to a file. What a file is comes from
// its bytes, not its name. Untrusted input: the plugin's program runs
// sandboxed (low integrity, a job with a memory limit).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace wsarchive {

struct Member {
    std::string path; // UTF-8, '\' between folders, no leading separator
    std::uint64_t size = 0; // unpacked bytes; 0 when the archive does not say
    std::int64_t modified = 0; // seconds since 1970, UTC; 0: unknown
    bool folder = false;
    bool locked = false; // its data needs a password
};

enum class Outcome : std::uint8_t {
    Listed, // all of it
    Partial, // what was read before a limit (members, time) or damage
    Locked, // the names themselves need a password
    Away, // a cloud file that is not on this computer: not downloaded for this
    Unreadable, // the file could not be opened (in use, no access, gone)
    NotArchive, // not a format read here, or damaged from its start
};
inline constexpr std::uint8_t kOutcomes = 6;

const char* outcomeName(Outcome outcome) noexcept;

struct Limits {
    std::size_t maxMembers = 200'000;
    std::size_t maxNameBytes = std::size_t {16} << 20; // of all the paths together
    std::chrono::milliseconds time {20'000};
    const std::atomic_bool* canceled = nullptr; // set: stops at the next read
    unsigned codePage = 0; // of names that do not say theirs (an old zip); 0: the system's
};

struct Listing {
    Outcome outcome = Outcome::NotArchive;
    std::string format; // as libarchive names it ("ZIP 2.0 (deflation)", "7-Zip", "RAR5")
    std::string problem; // why not all of it, for the log
};

// The members of `file` (a full path), in the archive's order, each handed
// to `member`.
Listing list(const std::wstring& file, const Limits& limits, const std::function<void(Member&&)>& member);

enum class Taken : std::uint8_t {
    Done,
    NotFound, // the archive changed, or is no longer there
    Locked, // needs a password
    TooLarge, // more than the limit
    TimedOut, // deep inside a solid archive, or a slow disk
    Failed, // damaged, a compression not read here, or the target could not be written
};

struct TakeLimits {
    std::uint64_t maxBytes = std::uint64_t {256} << 20;
    std::chrono::milliseconds time {8'000};
    const std::atomic_bool* canceled = nullptr;
    unsigned codePage = 0; // as Limits::codePage when it was listed
};

// The member at `path` (as list() gave it) written to `target` (a full path;
// its folder exists). `index`, its place in list()'s order, tells apart
// members of one name; the first of that name is taken when it does not fit.
// Nothing is left at `target` unless Done.
Taken take(const std::wstring& file, const std::string& path, std::size_t index, const std::wstring& target, const TakeLimits& limits,
    std::string* problem = nullptr);

// A member's path as shown and searched: '/' and '\' both become '\',
// "./" and separators at the start are dropped, as one at the end is
// (`folder` says so then). Empty for the archive's own root.
std::string cleanPath(std::string path, bool& folder);

// Bytes in a code page (0: the system's) as UTF-8; bytes that are UTF-8
// already stay as they are.
std::string textOf(std::string_view bytes, unsigned codePage = 0);

} // namespace wsarchive
