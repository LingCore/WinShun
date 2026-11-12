// What VirtualUnlock on pages that are not locked does to the working set.
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <vector>
#include <chrono>

static size_t residentIn(void* base, size_t bytes)
{
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> info(bytes / 4096);
    for (size_t i = 0; i < info.size(); ++i)
        info[i].VirtualAddress = static_cast<char*>(base) + i * 4096;
    QueryWorkingSetEx(GetCurrentProcess(), info.data(), static_cast<DWORD>(info.size() * sizeof info[0]));
    size_t n = 0;
    for (auto& i : info)
        n += i.VirtualAttributes.Valid;
    return n;
}

static void touch(void* base, size_t bytes, bool write)
{
    volatile char* p = static_cast<volatile char*>(base);
    for (size_t at = 0; at < bytes; at += 4096)
        if (write) p[at] = 1; else (void)p[at];
}

int main()
{
    const size_t mb = 1 << 20;
    // 1. one private region
    void* a = VirtualAlloc(nullptr, 64 * mb, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    touch(a, 64 * mb, true);
    printf("private: resident %zu pages\n", residentIn(a, 64 * mb));
    auto t0 = std::chrono::steady_clock::now();
    BOOL ok = VirtualUnlock(static_cast<char*>(a) + 8 * mb, 16 * mb);
    auto t1 = std::chrono::steady_clock::now();
    printf("unlock middle 16 MB: ok=%d err=%lu, resident %zu pages (expect 12288), %.2f ms\n", ok, GetLastError(),
        residentIn(a, 64 * mb), std::chrono::duration<double, std::milli>(t1 - t0).count());
    touch(static_cast<char*>(a) + 8 * mb, 16 * mb, false);
    printf("touched again: resident %zu pages; first byte %d (content kept)\n", residentIn(a, 64 * mb), static_cast<char*>(a)[8 * mb]);

    // 2. a range over two allocations next to each other
    void* r = VirtualAlloc(nullptr, 8 * mb, MEM_RESERVE, PAGE_NOACCESS);
    VirtualFree(r, 0, MEM_RELEASE);
    void* b1 = VirtualAlloc(r, 4 * mb, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    void* b2 = VirtualAlloc(static_cast<char*>(r) + 4 * mb, 4 * mb, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (b1 && b2) {
        touch(b1, 4 * mb, true);
        touch(b2, 4 * mb, true);
        ok = VirtualUnlock(b1, 8 * mb);
        printf("two allocations in one call: ok=%d err=%lu, resident %zu + %zu\n", ok, GetLastError(), residentIn(b1, 4 * mb), residentIn(b2, 4 * mb));
        ok = VirtualUnlock(b1, 4 * mb);
        DWORD e1 = GetLastError();
        ok = VirtualUnlock(b2, 4 * mb);
        printf("one call each: err=%lu/%lu, resident %zu + %zu\n", e1, GetLastError(), residentIn(b1, 4 * mb), residentIn(b2, 4 * mb));
    } else
        printf("could not place two allocations side by side\n");

    // 3. a range with a hole (decommitted pages) in it
    void* c = VirtualAlloc(nullptr, 8 * mb, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    touch(c, 8 * mb, true);
    VirtualFree(static_cast<char*>(c) + 2 * mb, mb, MEM_DECOMMIT);
    ok = VirtualUnlock(c, 8 * mb);
    printf("range with a decommitted hole: ok=%d err=%lu, resident %zu (of 1792 committed)\n", ok, GetLastError(),
        residentIn(c, 2 * mb) + residentIn(static_cast<char*>(c) + 3 * mb, 5 * mb));

    // 4. image pages (this module's code) and a guard page
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    MODULEINFO mi {};
    GetModuleInformation(GetCurrentProcess(), k, &mi, sizeof mi);
    touch(mi.lpBaseOfDll, mi.SizeOfImage, false);
    size_t before = residentIn(mi.lpBaseOfDll, mi.SizeOfImage);
    ok = VirtualUnlock(mi.lpBaseOfDll, mi.SizeOfImage);
    printf("kernel32 image: %zu -> %zu pages, ok=%d err=%lu\n", before, residentIn(mi.lpBaseOfDll, mi.SizeOfImage), ok, GetLastError());

    void* g = VirtualAlloc(nullptr, 4 * 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    touch(g, 4 * 4096, true);
    DWORD old = 0;
    VirtualProtect(static_cast<char*>(g) + 4096, 4096, PAGE_READWRITE | PAGE_GUARD, &old);
    ok = VirtualUnlock(g, 4 * 4096);
    MEMORY_BASIC_INFORMATION m {};
    VirtualQuery(static_cast<char*>(g) + 4096, &m, sizeof m);
    printf("with a guard page: ok=%d err=%lu, guard still set: %d, resident %zu\n", ok, GetLastError(), (m.Protect & PAGE_GUARD) != 0, residentIn(g, 4 * 4096));

    // 5. many small ranges: the cost of a call
    void* d = VirtualAlloc(nullptr, 256 * mb, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    touch(d, 256 * mb, true);
    t0 = std::chrono::steady_clock::now();
    for (size_t at = 0; at < 256 * mb; at += 8192)
        VirtualUnlock(static_cast<char*>(d) + at, 4096);
    t1 = std::chrono::steady_clock::now();
    printf("32768 one-page calls: %.1f ms, resident %zu (expect 32768)\n", std::chrono::duration<double, std::milli>(t1 - t0).count(), residentIn(d, 256 * mb));
    t0 = std::chrono::steady_clock::now();
    VirtualUnlock(d, 256 * mb);
    t1 = std::chrono::steady_clock::now();
    printf("one call for the 128 MB left: %.1f ms, resident %zu\n", std::chrono::duration<double, std::milli>(t1 - t0).count(), residentIn(d, 256 * mb));
    touch(d, 256 * mb, true);
    t0 = std::chrono::steady_clock::now();
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);
    t1 = std::chrono::steady_clock::now();
    printf("emptying everything (256 MB+): %.1f ms\n", std::chrono::duration<double, std::milli>(t1 - t0).count());
    return 0;
}
