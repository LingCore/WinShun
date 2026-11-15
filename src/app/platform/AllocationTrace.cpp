#include "AllocationTrace.h"

#include <algorithm>
#include <atomic>

#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#include <psapi.h>

namespace ws::memory::allocationTrace {
namespace {

constexpr int kFrames = 28;
constexpr int kSkipped = 2; // note() and the function in the import's place

using Alloc = LPVOID(WINAPI*)(HANDLE, DWORD, SIZE_T);
using ReAlloc = LPVOID(WINAPI*)(HANDLE, DWORD, LPVOID, SIZE_T);
using Free = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID);
using RtlFree = BOOLEAN(NTAPI*)(PVOID, ULONG, PVOID);

Alloc g_alloc = nullptr;
ReAlloc g_reAlloc = nullptr;
Free g_free = nullptr;
RtlFree g_rtlFree = nullptr; // what Windows' own dlls import; HeapFree is a function of its own before it
std::atomic<bool> g_on {false};

// What a thread asks for while it is in the notes (held() and sites()
// allocate) is not noted. A slot of the thread's own block (TlsAlloc, one of
// the first 64), read there directly: a thread_local is not there yet when
// Windows' dlls allocate as a thread starts (so no function's static either,
// which asks the thread's storage whether it is made), and TlsGetValue would
// reset the caller's last error.
DWORD g_slot = TLS_OUT_OF_INDEXES;
constexpr DWORD kTlsSlots = 0x1480; // TEB::TlsSlots, x64
bool inside() noexcept { return __readgsqword(kTlsSlots + g_slot * sizeof(void*)) != 0; }
struct Inside {
    Inside() noexcept { __writegsqword(kTlsSlots + g_slot * sizeof(void*), 1); }
    ~Inside() { __writegsqword(kTlsSlots + g_slot * sizeof(void*), 0); }
    Inside(const Inside&) = delete;
    Inside& operator=(const Inside&) = delete;
};

// The notes: two tables of fixed size in pages of their own (VirtualAlloc),
// under one lock. Nothing is allocated from a heap while it is held, and no
// C++ runs there that could throw: a note is made inside any heap call of
// any thread, Windows' own included, and a lock still held after one of
// those was unwound past, or one waiting for the heap, stops the process.
// (The first version kept std::unordered_maps under a std::mutex: one run in
// three hung with every thread waiting for it.)
struct BlockSlot {
    std::uintptr_t block = 0; // 0: empty
    std::uint32_t size = 0;
    std::uint32_t stack = 0;
};
struct StackSlot {
    std::uint64_t key = 0; // 0: empty
    std::uint32_t depth = 0;
    void* frames[kFrames] {};
};
constexpr std::size_t kBlockSlots = std::size_t {1} << 25; // 512 MB reserved, touched as used: reading a disk's folders holds millions of blocks at once
constexpr std::size_t kStackSlots = std::size_t {1} << 19; // 120 MB reserved
constexpr std::uint32_t kNoStack = 0xFFFF'FFFFu; // more call stacks than the table holds: told together

SRWLOCK g_lock = SRWLOCK_INIT;
BlockSlot* g_blocks = nullptr;
StackSlot* g_stacks = nullptr;
std::size_t g_blockCount = 0;
std::size_t g_stackCount = 0;
std::atomic<std::int64_t> g_untold {0};

std::size_t homeOf(std::uintptr_t block) noexcept
{
    return static_cast<std::size_t>(((block >> 4) * 0x9E37'79B9'7F4A'7C15ull) >> 39) & (kBlockSlots - 1); // the product's top 25 bits
}

// g_lock held.
std::uint32_t stackSlot(std::uint64_t key, void* const* frames, int depth) noexcept
{
    for (std::size_t i = key & (kStackSlots - 1);; i = (i + 1) & (kStackSlots - 1)) {
        StackSlot& slot = g_stacks[i];
        if (slot.key == key)
            return static_cast<std::uint32_t>(i);
        if (slot.key == 0) {
            if (g_stackCount >= kStackSlots / 4 * 3)
                return kNoStack;
            slot.key = key;
            slot.depth = static_cast<std::uint32_t>(depth);
            std::copy_n(frames, depth, slot.frames);
            ++g_stackCount;
            return static_cast<std::uint32_t>(i);
        }
    }
}

void note(const void* block, std::size_t size) noexcept
{
    if (!block || !g_on.load(std::memory_order_relaxed) || inside())
        return;
    const Inside in;
    void* frames[kFrames];
    const int depth = ::RtlCaptureStackBackTrace(kSkipped, kFrames, frames, nullptr);
    std::uint64_t key = 1469598103934665603ull;
    for (int i = 0; i < depth; ++i)
        key = (key ^ reinterpret_cast<std::uintptr_t>(frames[i])) * 1099511628211ull;
    key |= 1; // never the empty slot's
    const auto address = reinterpret_cast<std::uintptr_t>(block);
    ::AcquireSRWLockExclusive(&g_lock);
    if (g_blockCount < kBlockSlots / 4 * 3) { // past that, new blocks go untold
        for (std::size_t i = homeOf(address);; i = (i + 1) & (kBlockSlots - 1)) {
            BlockSlot& slot = g_blocks[i];
            if (slot.block != 0 && slot.block != address)
                continue;
            g_blockCount += slot.block == 0;
            slot = {address, static_cast<std::uint32_t>(std::min<std::size_t>(size, 0xFFFF'FFFFu)), stackSlot(key, frames, depth)};
            break;
        }
    } else {
        g_untold.fetch_add(1, std::memory_order_relaxed);
    }
    ::ReleaseSRWLockExclusive(&g_lock);
}

void forget(const void* block) noexcept
{
    if (!block || !g_on.load(std::memory_order_relaxed) || inside())
        return;
    const Inside in;
    const auto address = reinterpret_cast<std::uintptr_t>(block);
    constexpr std::size_t mask = kBlockSlots - 1;
    ::AcquireSRWLockExclusive(&g_lock);
    std::size_t i = homeOf(address);
    while (g_blocks[i].block != 0 && g_blocks[i].block != address)
        i = (i + 1) & mask;
    if (g_blocks[i].block == address) {
        // Backward-shift deletion: later slots of the same run move into the
        // gap when their home lies at or before it, so no lookup stops early.
        for (std::size_t j = (i + 1) & mask; g_blocks[j].block != 0; j = (j + 1) & mask) {
            const std::size_t home = homeOf(g_blocks[j].block);
            if (i <= j ? (home <= i || home > j) : (home <= i && home > j)) {
                g_blocks[i] = g_blocks[j];
                i = j;
            }
        }
        g_blocks[i] = {};
        --g_blockCount;
    }
    ::ReleaseSRWLockExclusive(&g_lock);
}

LPVOID WINAPI tracedAlloc(HANDLE heap, DWORD flags, SIZE_T size)
{
    const LPVOID block = g_alloc(heap, flags, size);
    note(block, size);
    return block;
}

LPVOID WINAPI tracedReAlloc(HANDLE heap, DWORD flags, LPVOID old, SIZE_T size)
{
    // Forgotten first: once it is free, its address may be another thread's new block.
    forget(old);
    const LPVOID block = g_reAlloc(heap, flags, old, size);
    note(block ? block : old, size);
    return block;
}

BOOL WINAPI tracedFree(HANDLE heap, DWORD flags, LPVOID block)
{
    forget(block);
    return g_free(heap, flags, block);
}

BOOLEAN NTAPI tracedRtlFree(PVOID heap, ULONG flags, PVOID block)
{
    forget(block);
    return g_rtlFree(heap, flags, block);
}

// Every import of `module` that is one of the heap functions: ours instead.
int patch(HMODULE module)
{
    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0)
        return 0;
    int patched = 0;
    for (const auto* dll = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress); dll->Name; ++dll) {
        for (auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(const_cast<std::uint8_t*>(base) + dll->FirstThunk); thunk->u1.Function; ++thunk) {
            const auto target = reinterpret_cast<void*>(thunk->u1.Function);
            void* ours = target == reinterpret_cast<void*>(g_alloc) ? reinterpret_cast<void*>(&tracedAlloc)
                : target == reinterpret_cast<void*>(g_reAlloc) ? reinterpret_cast<void*>(&tracedReAlloc)
                : target == reinterpret_cast<void*>(g_free) ? reinterpret_cast<void*>(&tracedFree)
                : target == reinterpret_cast<void*>(g_rtlFree) ? reinterpret_cast<void*>(&tracedRtlFree)
                : nullptr;
            DWORD before = 0;
            if (!ours || !::VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &before))
                continue;
            thunk->u1.Function = reinterpret_cast<ULONG_PTR>(ours);
            ::VirtualProtect(&thunk->u1.Function, sizeof(void*), before, &before);
            ++patched;
        }
    }
    return patched;
}

QString nameOf(HANDLE process, void* address)
{
    const auto at = reinterpret_cast<DWORD64>(address);
    alignas(SYMBOL_INFOW) char buffer[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)] {};
    auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
    symbol->MaxNameLen = 512;
    wchar_t path[MAX_PATH] {};
    HMODULE module = nullptr;
    QString moduleName = QStringLiteral("?");
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module)
        && ::GetModuleFileNameW(module, path, MAX_PATH)) {
        moduleName = QString::fromWCharArray(path);
        moduleName = moduleName.mid(moduleName.lastIndexOf(u'\\') + 1);
        moduleName.chop(moduleName.endsWith(u".dll", Qt::CaseInsensitive) || moduleName.endsWith(u".exe", Qt::CaseInsensitive) ? 4 : 0);
    }
    DWORD64 offset = 0;
    if (::SymFromAddrW(process, at, &offset, symbol))
        return QStringLiteral("%1!%2+0x%3").arg(moduleName, QString::fromWCharArray(symbol->Name)).arg(offset, 0, 16);
    return QStringLiteral("%1+0x%2").arg(moduleName).arg(at - reinterpret_cast<DWORD64>(module), 0, 16);
}

} // namespace

bool start()
{
    // kernel32's HeapAlloc and the like are forwarded to ntdll's: what an
    // import of either holds once resolved.
    const HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll");
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    g_alloc = reinterpret_cast<Alloc>(::GetProcAddress(kernel, "HeapAlloc"));
    g_reAlloc = reinterpret_cast<ReAlloc>(::GetProcAddress(kernel, "HeapReAlloc"));
    g_free = reinterpret_cast<Free>(::GetProcAddress(kernel, "HeapFree"));
    g_rtlFree = reinterpret_cast<RtlFree>(::GetProcAddress(ntdll, "RtlFreeHeap"));
    if (!g_alloc || !g_reAlloc || !g_free || !g_rtlFree)
        return false;
    g_slot = ::TlsAlloc();
    if (g_slot >= 64)
        return false;
    g_blocks = static_cast<BlockSlot*>(::VirtualAlloc(nullptr, kBlockSlots * sizeof(BlockSlot), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    g_stacks = static_cast<StackSlot*>(::VirtualAlloc(nullptr, kStackSlots * sizeof(StackSlot), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_blocks || !g_stacks)
        return false;
    HMODULE modules[2048];
    DWORD needed = 0;
    if (!::EnumProcessModules(::GetCurrentProcess(), modules, sizeof modules, &needed))
        return false;
    int patched = 0;
    for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 2048); ++i) {
        if (modules[i] != ntdll)
            patched += patch(modules[i]);
    }
    g_on.store(patched > 0);
    return patched > 0;
}

std::int64_t untold() noexcept { return g_untold.load(std::memory_order_relaxed); }

Totals held()
{
    // The table copied while the lock is held, counted after: counting
    // allocates, and the heap is not to be waited for under the lock.
    const Inside in;
    auto* copy = static_cast<BlockSlot*>(::VirtualAlloc(nullptr, kBlockSlots * sizeof(BlockSlot), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!copy)
        return {};
    ::AcquireSRWLockExclusive(&g_lock);
    std::copy_n(g_blocks, kBlockSlots, copy);
    ::ReleaseSRWLockExclusive(&g_lock);
    Totals by;
    for (std::size_t i = 0; i < kBlockSlots; ++i) {
        if (copy[i].block == 0)
            continue;
        Sum& sum = by[copy[i].stack];
        sum.bytes += copy[i].size;
        ++sum.blocks;
    }
    ::VirtualFree(copy, 0, MEM_RELEASE);
    return by;
}

std::vector<Site> sites(const Totals& now, const Totals* before, int most, std::int64_t* total)
{
    const Inside in;
    std::vector<std::pair<std::uint32_t, Sum>> sums;
    sums.reserve(now.size());
    std::int64_t all = 0;
    for (const auto& [stack, sum] : now) {
        Sum grown = sum;
        if (before) {
            if (const auto was = before->find(stack); was != before->end())
                grown = {sum.bytes - was->second.bytes, sum.blocks - was->second.blocks};
        }
        all += grown.bytes;
        sums.emplace_back(stack, grown);
    }
    if (before) { // gone altogether since
        for (const auto& [stack, sum] : *before) {
            if (!now.contains(stack))
                all -= sum.bytes;
        }
    }
    if (total)
        *total = all;
    std::ranges::sort(sums, [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
    if (sums.size() > static_cast<std::size_t>(std::max(most, 0)))
        sums.resize(static_cast<std::size_t>(std::max(most, 0)));
    const HANDLE process = ::GetCurrentProcess();
    ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    ::SymInitializeW(process, nullptr, TRUE); // again: fails, the first one stands
    std::vector<Site> out;
    out.reserve(sums.size());
    for (const auto& [index, sum] : sums) {
        Site site {sum.bytes, sum.blocks, {}};
        if (index == kNoStack) {
            site.frames.push_back(QStringLiteral("(more call stacks than the trace holds)"));
        } else {
            // A slot is written once, under the lock, before a block names it.
            const StackSlot& stack = g_stacks[index];
            for (std::uint32_t i = 0; i < stack.depth; ++i)
                site.frames.push_back(nameOf(process, stack.frames[i]));
        }
        out.push_back(std::move(site));
    }
    return out;
}

} // namespace ws::memory::allocationTrace
