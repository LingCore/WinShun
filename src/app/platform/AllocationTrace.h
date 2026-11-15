#pragma once

#include <QStringList>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ws::memory {

// Who holds the heap memory: from start() on, every block handed out through
// the heap functions the loaded modules import (the C runtime's malloc and
// operator new, Qt's, Windows' own dlls') is noted with the calls it came
// from, and forgotten when freed. held() then says what is still held, by
// call stack. For the memory probes only (--foreground-use-trace): it costs
// a stack walk per allocation and memory of its own, so nothing measured
// while it runs is what Win顺 uses. Windows' own tool for this (umdh) does
// not see the heap Windows 11 gives the process.
namespace allocationTrace {

// Puts the notes in place of the modules' imports. Not undone: the process
// is a probe's and ends with it. False if nothing could be patched.
bool start();

// What is held now, by where it was asked for (a number per call stack).
struct Sum {
    std::int64_t bytes = 0;
    std::int64_t blocks = 0;
};
using Totals = std::unordered_map<std::uint32_t, Sum>;
Totals held();
// Blocks that were not noted because the table was full: with any, held() tells too little.
std::int64_t untold() noexcept;

struct Site {
    std::int64_t bytes = 0;
    std::int64_t blocks = 0;
    QStringList frames; // innermost first: "Qt6Core!QArrayData::allocate+0x41", or "module+0x1234" where no name is known
};
// The call stacks that hold the most in `now`, or, with `before`, that hold
// the most more than they did then (what was freed since counts against
// what was allocated: a list rebuilt row for row has grown by nothing).
// `total` (if given): all of them together, the ones past `most` too.
std::vector<Site> sites(const Totals& now, const Totals* before, int most, std::int64_t* total = nullptr);

} // namespace allocationTrace
} // namespace ws::memory
