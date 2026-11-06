// WinShunSwitches.exe: winshun.switches on its own, when Win顺 keeps plugins
// apart (WINSHUN_PLUGINS_APART=1); it otherwise runs inside the plugin host
// (src/plugin/runner/InProcess.cpp). Qt is shared with the host, using the
// same loader pattern as capture.
#include <windows.h>

#include <cstdio>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    std::wstring app(32768, L'\0');
    const DWORD length = ::GetEnvironmentVariableW(L"WS_APP_DIR", app.data(), static_cast<DWORD>(app.size()));
    if (!length || length >= app.size()) {
        std::fprintf(stderr, "WinShunSwitches: WS_APP_DIR is required\n");
        return 2;
    }
    app.resize(length);
    std::wstring self(32768, L'\0');
    const DWORD own = ::GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    if (!own || own >= self.size())
        return 3;
    self.resize(own);
    if (!::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) || !::AddDllDirectory(app.c_str()))
        return 4;
    const auto slash = self.find_last_of(L"\\/");
    const std::wstring dll = self.substr(0, slash) + L"\\WinShunSwitchesApp.dll";
    const HMODULE module = ::LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!module) {
        std::fprintf(stderr, "WinShunSwitches: cannot load provider, error %lu\n", ::GetLastError());
        return 5;
    }
    using Entry = int(__cdecl*)(const wchar_t*);
    const auto entry = reinterpret_cast<Entry>(::GetProcAddress(module, "winshunSwitchesMain"));
    return entry ? entry(app.c_str()) : 6;
}
