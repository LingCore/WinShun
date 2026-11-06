#pragma once

#include <windows.h>

namespace tools {

// The winshun.tools plugin over these ends: requests in, answers out, until
// `in` closes or shutdown comes. Its exit code.
int serve(HANDLE in, HANDLE out);

} // namespace tools
