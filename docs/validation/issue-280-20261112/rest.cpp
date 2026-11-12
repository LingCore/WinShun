// What getting pages back costs right after they left the working set, and after they sat a while.
#include <windows.h>
#include <psapi.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static double touch(char* base, size_t bytes)
{
    const auto t0 = std::chrono::steady_clock::now();
    volatile char sink = 0;
    for (size_t at = 0; at < bytes; at += 4096)
        sink = sink ^ base[at];
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int main(int argc, char** argv)
{
    const size_t bytes = 80u << 20;
    char* a = static_cast<char*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    for (size_t at = 0; at < bytes; at += 64) // heap-like: text and small numbers, compressible
        std::snprintf(a + at, 64, "entry %zu of the list, flags=%d", at / 64, static_cast<int>(at % 7));
    printf("in memory: %.1f ms for %zu pages\n", touch(a, bytes), bytes / 4096);
    for (int rest : {0, 5, 30, 60, 120}) {
        if (argc > 1 && rest > std::atoi(argv[1]))
            break;
        VirtualUnlock(a, bytes);
        Sleep(rest * 1000);
        const double ms = touch(a, bytes);
        printf("out, %3d s later: %.1f ms (%.1f us a page)\n", rest, ms, ms * 1000 / (bytes / 4096));
        fflush(stdout);
    }
    return 0;
}
