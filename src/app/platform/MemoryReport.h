#pragma once

#include <QtGlobal>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ws::memory {

// Empties the working set of this process and, where Windows allows, of those
// under it (the pages go to the standby list and come back when used).
// Returns the MB given back, all together.
qint64 trimAll();

} // namespace ws::memory
