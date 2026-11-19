#include "EditorStub.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

namespace gestures {

namespace {

std::vector<std::wstring> commandLine()
{
    std::vector<std::wstring> out;
    int count = 0;
    if (LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i)
            out.emplace_back(wide[i]);
        ::LocalFree(wide);
    }
    return out;
}

int fail(int code, const char* what, DWORD error = 0)
{
    // stderr goes to plugins.log; stdout is the protocol's alone.
    if (error)
        std::fprintf(stderr, "WinShunGestures --editor: %s (error %lu)\n", what, error);
    else
        std::fprintf(stderr, "WinShunGestures --editor: %s\n", what);
    return code;
}

} // namespace

bool editorRequested()
{
    for (const std::wstring& argument : commandLine()) {
        if (argument == L"--editor")
            return true;
    }
    return false;
}

int runEditor()
{
    // Win顺's folder: the plugin host sets WS_APP_DIR; a preview names it.
    std::wstring appDir;
    const std::vector<std::wstring> arguments = commandLine();
    for (std::size_t i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments[i] == L"--app-dir")
            appDir = arguments[i + 1];
    }
    if (appDir.empty()) {
        appDir.assign(32768, L'\0');
        const DWORD length = ::GetEnvironmentVariableW(L"WS_APP_DIR", appDir.data(), static_cast<DWORD>(appDir.size()));
        if (length == 0 || length >= appDir.size())
            return fail(2, "WS_APP_DIR is not set: Win顺 starts the plugin (a preview takes --app-dir)");
        appDir.resize(length);
    }
    if (::GetFileAttributesW((appDir + L"\\Qt6Core.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
        return fail(3, "no Qt6Core.dll in WS_APP_DIR: this Win顺 cannot run the settings window");
    ::SetEnvironmentVariableW(L"WS_APP_DIR", appDir.c_str());
    // Nothing is searched but the application's folder, System32 and Win顺's:
    // no PATH, no current folder.
    if (!::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) || !::AddDllDirectory(appDir.c_str()))
        return fail(4, "DLL folders not set", ::GetLastError());
    std::wstring path(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return fail(5, "own path unknown", ::GetLastError());
    path.resize(length);
    path.resize(path.find_last_of(L"\\/") + 1);
    path += L"WinShunGesturesEditor.dll";
    const HMODULE module = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!module)
        return fail(6, "WinShunGesturesEditor.dll not loaded (a Win顺 with another Qt?)", ::GetLastError());
    using Entry = int(__cdecl*)(const wchar_t* appDir);
    const auto entry = reinterpret_cast<Entry>(::GetProcAddress(module, "winshunGesturesEditorMain"));
    if (!entry)
        return fail(7, "WinShunGesturesEditor.dll has no entry", ::GetLastError());
    return entry(appDir.c_str());
}

} // namespace gestures
