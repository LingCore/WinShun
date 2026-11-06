#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ws::memory {

// Every process of Win顺's, as Windows counts it, from one look at the
// process list (NtQuerySystemInformation): no process is opened, so the
// service, which runs as LocalSystem, is seen as well from an ordinary user's
// Win顺, as Task Manager sees it. Only the processes whose program is named
// WinShun* are kept (and this one, whatever its name).
struct Process {
    std::uint32_t pid = 0;
    std::uint32_t parent = 0;
    std::int64_t created = 0; // FILETIME, 100 ns
    std::uint64_t privateBytes = 0; // the private working set: Task Manager's Memory column
    std::uint64_t cpu = 0; // user + kernel time, 100 ns
    std::uint64_t readBytes = 0; // all reads since it started (files, the volume, pipes)
    wchar_t exe[40] {}; // "WinShunExtract.exe"; cut if longer
    std::uint64_t workingSetBytes = 0; // all resident pages, including mapped files
    std::uint64_t privateCommitBytes = 0; // committed private memory, resident or not
};

struct Look {
    bool ok = false;
    bool full = false; // the whole list (the service too), not just `known` again
    std::uint32_t self = 0;
    std::int64_t atMs = 0; // wall clock, ms since 1970
    std::int64_t atFt = 0; // the same as a FILETIME (100 ns)
    std::vector<Process> processes;
};

// One look. Windows hands back the whole list (every process and thread: ~2.6 MB
// and ~20 ms with 950 processes), so it is taken off the GUI thread; the
// buffer is the system's own pages, given back at once, not kept between
// looks. `sizeHint`: the size the last one needed, to ask once, not twice.
Look look(std::size_t& sizeHint);

// The same numbers for these processes only, read by handle (a few
// microseconds each, not the whole list): this one and its own, which it may
// open. Not ok if one of them is gone, cannot be opened or Windows does not
// tell its private working set (before Windows 11 22H2): then a whole look.
Look again(const std::vector<Process>& known);

// The family of `root`: it and the WinShun* processes under it, parents
// first. A parent id may name a process that is gone and its number given to
// another: a child is only one if it was made after its parent.
std::vector<const Process*> family(const std::vector<Process>& all, std::uint32_t root);

// How many bytes of these blocks (address, size) are in this process's
// private working set now (QueryWorkingSetEx, page by page, in small steps;
// ~1.3 ms for 128 MB).
std::uint64_t residentBytes(const std::vector<std::pair<const void*, std::size_t>>& blocks);

// This process's private working set (Task Manager's Memory), page by page,
// by what the pages are: program files' own copies (code fixed up, their
// data), the file index's blocks, thread stacks, the rest (heaps: objects of
// the windows, caches, Qt's QML, the graphics driver's). It only asks which
// pages are in (QueryWorkingSetEx), never reads one: a page given back stays
// out. No HeapWalk: it reads every block's header, and took a trimmed Win顺
// from 11 to 270 MB, the health page showing its own walk (pitfalls).
struct Map {
    std::uint64_t total = 0;
    std::uint64_t fileIndex = 0;
    std::uint64_t fileIndexShared = 0; // resident file-backed pages; outside private total
    std::uint64_t ownProgram = 0; // WinShun*.exe/.dll
    std::uint64_t qt = 0; // Qt6*.dll and Qt's plugins
    std::uint64_t graphics = 0; // the graphics driver's and Direct3D's dlls
    std::uint64_t system = 0; // Windows' and others' dlls
    std::uint64_t stacks = 0;
    std::uint64_t other = 0;
};
// `fileIndex`: its blocks, any order. The walk ~20 ms with 600 MB
// committed: off the GUI thread.
Map map(std::vector<std::pair<const void*, std::size_t>> fileIndex);

} // namespace ws::memory
