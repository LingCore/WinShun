#pragma once

#include <cstddef>
#include <cstdint>

namespace ws::codeaddress {

// An address in the program's code as "Qt6Core.dll+1a2b3c": the module it is
// in (file name, no folder) and how far into it; "0x…" when it is in none
// (code made at run time). In one of Windows' own modules, the exported
// function nearest before it follows: "win32u.dll+1234 NtUserMessageCall+14"
// (no symbols are at hand for whichever Windows build a PC has, but what a
// thread waits in is nearly always an exported call). An offset in the
// program's own modules becomes a function with that version's symbols
// (docs/pitfalls.md has how they are made for a version already out).
//
// Plain buffers only, nothing allocated, nothing locked but the loader's
// look-up of the module: also for a crash handler (CrashRecord). The text is
// UTF-8 and always ends in a 0; returns its length.
std::size_t describe(std::uintptr_t address, char* out, std::size_t room) noexcept;

} // namespace ws::codeaddress
