// WinShunCapture.exe: the program of the plugin winshun.capture (GitHub #138).
//
// The screenshot is built on Qt, and Win顺 has Qt where it is installed: the
// plugin uses that copy rather than bring its own (some 40 MB more to
// download, and code pages not shared with Win顺's process). This starter,
// without Qt and with the C runtime linked in, adds Win顺's folder (WS_APP_DIR,
// set by the plugin host) to the folders DLLs are looked for in and loads
// WinShunCaptureApp.dll from its own folder: that one imports Qt, which the
// loader then finds there. Nothing else is searched (no PATH, no current
// folder): the application's folder, System32 and Win顺's. With a preview's
// flag (--preview-…, CapturePreview.h) it runs that instead, Win顺's folder
// given as --app-dir <folder> when WS_APP_DIR is not set.

#include <windows.h>

#include <cstdio>
#include <string>
#include <string_view>

namespace {

using Entry = int(__cdecl*)(const wchar_t* appDir);

int fail(int code, const char* what, DWORD error = 0)
{
    // stderr goes to plugins.log; stdout is the protocol's alone.
    if (error)
        std::fprintf(stderr, "WinShunCapture: %s (error %lu)\n", what, error);
    else
        std::fprintf(stderr, "WinShunCapture: %s\n", what);
    return code;
}

std::wstring folderOf(const std::wstring& path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    bool preview = false;
    std::wstring appDir;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        preview = preview || arg.starts_with(L"--preview-");
        if (arg == L"--app-dir" && i + 1 < argc)
            appDir = argv[i + 1];
    }
    if (!preview || appDir.empty()) {
        appDir.assign(32768, L'\0');
        const DWORD length = ::GetEnvironmentVariableW(L"WS_APP_DIR", appDir.data(), static_cast<DWORD>(appDir.size()));
        if (length == 0 || length >= appDir.size())
            return fail(2, "WS_APP_DIR is not set: Win顺 starts this plugin (a preview takes --app-dir)");
        appDir.resize(length);
    }
    if (::GetFileAttributesW((appDir + L"\\Qt6Core.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
        return fail(3, "no Qt6Core.dll in WS_APP_DIR: this Win顺 cannot run the plugin");

    std::wstring self(32768, L'\0');
    const DWORD selfLength = ::GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    if (selfLength == 0 || selfLength >= self.size())
        return fail(4, "own path unknown", ::GetLastError());
    self.resize(selfLength);

    if (!::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) || !::AddDllDirectory(appDir.c_str()))
        return fail(5, "DLL folders not set", ::GetLastError());
    const std::wstring dll = folderOf(self) + L"\\WinShunCaptureApp.dll";
    HMODULE module = ::LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!module)
        return fail(6, "WinShunCaptureApp.dll not loaded (a Win顺 with another Qt?)", ::GetLastError());
    const char* name = preview ? "winshunCapturePreview" : "winshunCaptureMain";
    const auto entry = reinterpret_cast<Entry>(::GetProcAddress(module, name));
    if (!entry)
        return fail(7, preview ? "WinShunCaptureApp.dll has no winshunCapturePreview" : "WinShunCaptureApp.dll has no winshunCaptureMain");
    return entry(appDir.c_str());
}
