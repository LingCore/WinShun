#pragma once

#include <windows.h>
#include <string>

// Blender's optional extension marks its own separate file-browser windows.
// Requests run through Blender's public API on its UI thread, not through
// keyboard input, the clipboard or remote-process memory.
namespace ws::blenderdialog {

bool mayBe(HWND window);
bool isOne(HWND window);
std::wstring currentFolder(HWND window);
bool goTo(HWND window, const std::wstring& folder);
bool setFileName(HWND window, const std::wstring& name);
bool accept(HWND window);

} // namespace ws::blenderdialog
