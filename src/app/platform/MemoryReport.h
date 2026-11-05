#pragma once

#include <QtGlobal>

#include <cstdint>

namespace ws::memory {

// What Task Manager's Memory column (the private working set) counts for Win顺:
// this process and every process below it that is Win顺's own. Those in the
// tree whose program is not named WinShun* (a program started from the
// launcher) are not Win顺's and are left out.
struct Use {
    std::uint64_t main = 0; // bytes
    std::uint64_t plugins = 0; // plugin hosts and the processes they run
    std::uint64_t documents = 0; // WinShunExtract: documents being read
    int pluginProcesses = 0;
    int documentProcesses = 0;
    std::uint64_t total() const noexcept { return main + plugins + documents; }
};
Use measure();

// Empties the working set of this process and, where Windows allows, of those
// under it (the pages go to the standby list and come back when used).
// Returns the MB given back, all together.
qint64 trimAll();

} // namespace ws::memory
