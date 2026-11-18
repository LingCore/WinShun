// What else is loaded in the plugin's process (GitHub #331): on one PC every
// Explorer window answered GetWindowLongPtr with ERROR_INVALID_WINDOW_HANDLE
// while its title and process could be read and Explorer itself said it was
// shown. No other desktop, job or token reproduced that; a hook put into the
// process (software guarding explorer.exe) would. The log names such DLLs.
#pragma once

#include <string>

namespace tabs {

// The full paths of the DLLs in this process that are neither under the
// Windows folder nor next to the program, "; " between them ("" for none,
// "?" when they cannot be listed).
std::string foreignModules();

} // namespace tabs
