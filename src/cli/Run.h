#pragma once

// A whole run of ws: the arguments in, the exit code out.

#include <span>
#include <string>

namespace ws::cli {

int run(std::span<const std::wstring> args);

} // namespace ws::cli
