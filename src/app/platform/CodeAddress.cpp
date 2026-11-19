#include "CodeAddress.h"

#include <windows.h>

#include <array>
#include <cstdio>

namespace ws::codeaddress {

namespace {

constexpr DWORD kNearExport = 0x1000; // further past a name than this: not that function

// The exported function of `module` that starts closest before `rva`, and how
// far in. Plain data in and out, structured exceptions around it: the module
// may go meanwhile.
bool exportBefore(HMODULE module, DWORD rva, char* name, std::size_t room, DWORD* into) noexcept
{
    __try {
        const auto* base = reinterpret_cast<const BYTE*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT)
            return false;
        const IMAGE_DATA_DIRECTORY& where = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!where.VirtualAddress || where.Size < sizeof(IMAGE_EXPORT_DIRECTORY))
            return false;
        const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + where.VirtualAddress);
        const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
        const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
        const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
        DWORD best = 0;
        const char* found = nullptr;
        for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
            if (ordinals[i] >= exports->NumberOfFunctions)
                continue;
            const DWORD start = functions[ordinals[i]];
            // In the export table itself: a forwarder's text, no code. Of two
            // names for one address the first stays (Nt… before Zw…).
            if (start > rva || (found && start <= best)
                || (start >= where.VirtualAddress && start - where.VirtualAddress < where.Size))
                continue;
            best = start;
            found = reinterpret_cast<const char*>(base + names[i]);
        }
        if (!found || rva - best > kNearExport)
            return false;
        std::size_t length = 0;
        while (length + 1 < room && found[length]) {
            name[length] = found[length];
            ++length;
        }
        name[length] = 0;
        *into = rva - best;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Windows' folder with the separator after it, once; empty when it cannot be told.
const wchar_t* windowsFolder() noexcept
{
    static const std::array<wchar_t, MAX_PATH> folder = [] {
        std::array<wchar_t, MAX_PATH> path {};
        const UINT length = ::GetSystemWindowsDirectoryW(path.data(), static_cast<UINT>(path.size() - 1));
        if (length == 0 || length >= path.size() - 1)
            return std::array<wchar_t, MAX_PATH> {};
        if (path[length - 1] != L'\\')
            path[length] = L'\\';
        return path;
    }();
    return folder.data();
}

} // namespace

std::size_t describe(std::uintptr_t address, char* out, std::size_t room) noexcept
{
    if (!out || room == 0)
        return 0;
    const auto finish = [&](int written) -> std::size_t {
        if (written < 0) {
            out[0] = 0;
            return 0;
        }
        return static_cast<std::size_t>(written) < room ? static_cast<std::size_t>(written) : room - 1;
    };
    HMODULE module = nullptr;
    std::array<wchar_t, MAX_PATH> path {};
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module)
        || !module) {
        return finish(std::snprintf(out, room, "0x%llx", static_cast<unsigned long long>(address)));
    }
    const DWORD length = ::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return finish(std::snprintf(out, room, "0x%llx", static_cast<unsigned long long>(address)));
    const wchar_t* file = path.data();
    for (const wchar_t* c = path.data(); *c; ++c) {
        if (*c == L'\\' || *c == L'/')
            file = c + 1;
    }
    std::array<char, MAX_PATH * 3> name {};
    if (::WideCharToMultiByte(CP_UTF8, 0, file, -1, name.data(), static_cast<int>(name.size()), nullptr, nullptr) <= 0)
        name[0] = 0;
    const std::uintptr_t offset = address - reinterpret_cast<std::uintptr_t>(module);
    std::array<char, 96> function {};
    DWORD into = 0;
    const wchar_t* windows = windowsFolder();
    const int windowsLength = static_cast<int>(::wcslen(windows));
    const bool own = windowsLength > 0 && static_cast<int>(length) > windowsLength
        && ::CompareStringOrdinal(path.data(), windowsLength, windows, windowsLength, TRUE) == CSTR_EQUAL;
    if (own && offset <= MAXDWORD && exportBefore(module, static_cast<DWORD>(offset), function.data(), function.size(), &into))
        return finish(std::snprintf(out, room, "%s+%llx %s+%lx", name.data(), static_cast<unsigned long long>(offset),
            function.data(), static_cast<unsigned long>(into)));
    return finish(std::snprintf(out, room, "%s+%llx", name.data(), static_cast<unsigned long long>(offset)));
}

} // namespace ws::codeaddress
