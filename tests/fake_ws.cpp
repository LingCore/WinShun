// A stand-in for ws.exe (tst_commandline): only what the settings page asks of
// it, `ws skill --install claude`. It writes a SKILL.md into
// %CLAUDE_CONFIG_DIR%\skills\winshun (else the user's .claude), prints the
// path as UTF-8 and exits 0, as the real one does. FAKE_WS_FAIL=<text> makes
// it fail with that text on standard error and exit code 7; FAKE_WS_BOTH=1
// has it write to both streams first.
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::wstring variable(const wchar_t* name)
{
    wchar_t buffer[32768];
    const DWORD n = ::GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    return n > 0 && n < std::size(buffer) ? std::wstring(buffer, n) : std::wstring();
}

void write(HANDLE handle, const std::wstring& text)
{
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
    DWORD written = 0;
    ::WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
    if (argc != 4 || std::wstring(argv[1]) != L"skill" || std::wstring(argv[2]) != L"--install" || std::wstring(argv[3]) != L"claude") {
        write(err, L"usage: ws skill --install claude\n");
        return 2;
    }
    if (!variable(L"FAKE_WS_BOTH").empty())
        write(out, L"to standard output first\n");
    if (const std::wstring why = variable(L"FAKE_WS_FAIL"); !why.empty()) {
        write(err, why + L"\n");
        return 7;
    }
    std::filesystem::path folder = variable(L"CLAUDE_CONFIG_DIR");
    if (folder.empty())
        folder = std::filesystem::path(variable(L"USERPROFILE")) / L".claude";
    folder /= L"skills";
    folder /= L"winshun";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    const std::filesystem::path file = folder / L"SKILL.md";
    std::ofstream(file) << "---\nname: winshun\ndescription: fake\n---\n";
    write(out, file.wstring() + L"\n");
    return 0;
}
