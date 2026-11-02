// Resolve Qt from the host, as the capture plugin does. No Qt or models are
// copied into this optional plugin; no DLLs are loaded from PATH/the CWD.
#include <windows.h>
#include <cstdio>
#include <string>
#include <string_view>

int wmain(int argc, wchar_t** argv)
{
    bool preview = false;
    bool worker = false;
    std::wstring appDir;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view argument(argv[i]);
        if (argument == L"--preview-ocr-search")
            preview = true;
        if (argument == L"--ocr-worker")
            worker = true;
        if (argument == L"--app-dir" && i + 1 < argc)
            appDir = argv[++i];
    }
    if (appDir.empty()) {
        appDir.resize(32768);
        const DWORD length = ::GetEnvironmentVariableW(L"WS_APP_DIR", appDir.data(), DWORD(appDir.size()));
        if (!length || length >= appDir.size()) {
            std::fputs("WS_APP_DIR is required\n", stderr);
            return 2;
        }
        appDir.resize(length);
    }
    ::SetEnvironmentVariableW(L"WS_APP_DIR", appDir.c_str());
    if (!::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) || !::AddDllDirectory(appDir.c_str()))
        return 3;
    std::wstring path(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!length || length >= path.size())
        return 4;
    path.resize(length);
    path.resize(path.find_last_of(L"\\/") + 1);
    path += L"WinShunOcrSearchApp.dll";
    const HMODULE module
        = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!module) {
        std::fprintf(stderr, "Cannot load WinShunOcrSearchApp.dll: %lu\n", ::GetLastError());
        return 5;
    }
    using Main = int(__cdecl*)(const wchar_t*, int);
    const auto entry = reinterpret_cast<Main>(::GetProcAddress(module, "winshunOcrSearchMain"));
    return entry ? entry(appDir.c_str(), worker ? 2 : preview ? 1 : 0) : 6;
}
