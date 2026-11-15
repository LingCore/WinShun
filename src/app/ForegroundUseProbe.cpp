#include "ForegroundUseProbe.h"

#include "App.h"
#include "IndexService.h"
#include "Launcher.h"
#include "platform/AllocationTrace.h"
#include "platform/IdleTrim.h"
#include "platform/ProcessUsage.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QAbstractItemModel>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <bit>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <windows.h>
#include <psapi.h>

using namespace Qt::StringLiterals;

namespace ws::preview {

// --probe-runtime <folder> --foreground-use-check [--renderer d3d11]
//     [--idle-open-index <folder;folder>] [--foreground-use-rounds 2]
//     [--foreground-use-walk] [--foreground-use-trace <from>[:<to>]]
// The production App beside the Win顺 that runs, its launcher opened the way
// a hotkey opens it and typed into a key at a time, as somebody looking for a
// few things in a row would: after each query, what the process holds
// (Task Manager's Memory is `private_mb`) and by what it is (memory::map: the
// file index's blocks, the heaps, the program files' own pages), and how long
// each key's search took until its rows were there. Before all that, the
// memory was given back once (IdleTrim, waited for), so what is counted came
// in for the launcher. `anatomy`: what the memory outside the file index is
// at the end, heap by heap, and what is no heap's.
// Into foreground-use.json. No input is sent: the queries are set on the
// launcher, whose window is up without the keyboard.
struct ForegroundUseProbe {
    // Waits in an event loop of its own, as the App waits in its: objects
    // put away with deleteLater (a list's rows when the next search's come)
    // go only when control is back in a loop that runs. Turning the events
    // over by hand (processEvents) left every one of them in memory.
    static void pump(int ms)
    {
        QEventLoop loop;
        QTimer::singleShot(ms, Qt::PreciseTimer, &loop, &QEventLoop::quit);
        loop.exec();
    }

    template <typename Done> static bool waitFor(Done&& done, int limitMs)
    {
        QElapsedTimer waited;
        waited.start();
        while (!done() && waited.elapsed() < limitMs)
            pump(1);
        return done();
    }

    static double megabytes(std::uint64_t bytes) { return std::round(static_cast<double>(bytes) / 1048576.0 * 10) / 10; }

    static std::vector<std::pair<const void*, std::size_t>> indexBlocks(App& app)
    {
        const auto index = app.m_index->index();
        if (!index)
            return {};
        const auto lock = index->readLock();
        return index->memoryBlocks();
    }

    static QJsonObject reading(App& app, const QString& stage)
    {
        PROCESS_MEMORY_COUNTERS_EX2 c {};
        c.cb = sizeof c;
        ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof c);
        const memory::Map map = memory::map(indexBlocks(app));
        return {
            {u"stage"_s, stage},
            {u"private_mb"_s, megabytes(c.PrivateWorkingSetSize)},
            {u"working_mb"_s, megabytes(c.WorkingSetSize)},
            {u"commit_mb"_s, megabytes(c.PrivateUsage)},
            {u"index_mb"_s, megabytes(map.fileIndex)},
            {u"heaps_mb"_s, megabytes(map.other)},
            {u"program_mb"_s, megabytes(map.ownProgram)},
            {u"qt_mb"_s, megabytes(map.qt)},
            {u"graphics_mb"_s, megabytes(map.graphics)},
            {u"system_mb"_s, megabytes(map.system)},
            {u"stacks_mb"_s, megabytes(map.stacks)},
        };
    }

    // What the private memory outside the file index is, at the end: each
    // heap's part (what is in use of it, what is free but still committed,
    // what of its pages are in memory), and the allocations that are no
    // heap's, by kind and size (a script engine's, the graphics driver's,
    // thread stacks). The pages are counted first, the heaps walked after:
    // walking reads every block's header and brings its page in.
    static QJsonObject anatomy(App& app)
    {
        auto index = indexBlocks(app);
        std::sort(index.begin(), index.end());
        std::size_t indexBytes = 0;
        for (const auto& [block, size] : index)
            indexBytes += size;
        const auto inIndex = [&index](std::uintptr_t page) {
            auto it = std::upper_bound(index.begin(), index.end(),
                std::pair<const void*, std::size_t>(reinterpret_cast<const void*>(page), SIZE_MAX));
            if (it == index.begin())
                return false;
            --it;
            return page + 4096 > reinterpret_cast<std::uintptr_t>(it->first)
                && page < reinterpret_cast<std::uintptr_t>(it->first) + it->second;
        };
        struct Allocation {
            std::uintptr_t base = 0;
            std::size_t reserved = 0;
            std::size_t resident = 0; // private pages in memory, the index's apart
            std::size_t residentIndex = 0;
            DWORD type = 0;
            DWORD protect = 0; // the first committed region's
            bool stack = false;
            int heap = -1;
        };
        std::vector<Allocation> all;
        static PSAPI_WORKING_SET_EX_INFORMATION info[4096];
        const void* last = nullptr;
        for (std::uintptr_t at = 0;;) {
            MEMORY_BASIC_INFORMATION m;
            if (::VirtualQuery(reinterpret_cast<const void*>(at), &m, sizeof m) == 0)
                break;
            const std::uintptr_t next = reinterpret_cast<std::uintptr_t>(m.BaseAddress) + m.RegionSize;
            if (m.State != MEM_FREE && m.Type != MEM_IMAGE) {
                if (m.AllocationBase != last) {
                    last = m.AllocationBase;
                    all.push_back({.base = reinterpret_cast<std::uintptr_t>(m.AllocationBase), .type = m.Type});
                }
                Allocation& a = all.back();
                a.reserved += m.RegionSize;
                a.stack = a.stack || (m.Protect & PAGE_GUARD);
                if (m.State == MEM_COMMIT && !(m.Protect & PAGE_GUARD)) {
                    if (a.protect == 0)
                        a.protect = m.Protect;
                    for (std::uintptr_t page = reinterpret_cast<std::uintptr_t>(m.BaseAddress); page < next;) {
                        const std::size_t n = std::min<std::size_t>(std::size(info), (next - page) / 4096);
                        for (std::size_t i = 0; i < n; ++i)
                            info[i].VirtualAddress = reinterpret_cast<void*>(page + i * 4096);
                        if (::QueryWorkingSetEx(::GetCurrentProcess(), info, static_cast<DWORD>(n * sizeof info[0]))) {
                            for (std::size_t i = 0; i < n; ++i) {
                                if (info[i].VirtualAttributes.Valid && !info[i].VirtualAttributes.Shared)
                                    (inIndex(page + i * 4096) ? a.residentIndex : a.resident) += 4096;
                            }
                        }
                        page += n * 4096;
                    }
                }
            }
            if (next <= at)
                break;
            at = next;
        }
        const auto allocationOf = [&all](std::uintptr_t address) -> Allocation* {
            auto it = std::upper_bound(all.begin(), all.end(), address, [](std::uintptr_t a, const Allocation& b) { return a < b.base; });
            if (it == all.begin())
                return nullptr;
            --it;
            return address < it->base + it->reserved ? &*it : nullptr;
        };

        std::vector<HANDLE> heaps(256);
        heaps.resize(std::min<DWORD>(256, ::GetProcessHeaps(256, heaps.data())));
        struct Heap {
            std::size_t busy = 0, free = 0, overhead = 0, blocks = 0;
        };
        std::vector<Heap> sums(heaps.size());
        for (std::size_t h = 0; h < heaps.size(); ++h) {
            if (!::HeapLock(heaps[h]))
                continue;
            PROCESS_HEAP_ENTRY e {};
            while (::HeapWalk(heaps[h], &e)) {
                if (Allocation* a = allocationOf(reinterpret_cast<std::uintptr_t>(e.lpData)); a && a->heap < 0)
                    a->heap = static_cast<int>(h);
                if (e.wFlags & (PROCESS_HEAP_UNCOMMITTED_RANGE | PROCESS_HEAP_REGION))
                    continue;
                if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
                    sums[h].busy += e.cbData;
                    ++sums[h].blocks;
                } else {
                    sums[h].free += e.cbData;
                }
                sums[h].overhead += e.cbOverhead;
            }
            ::HeapUnlock(heaps[h]);
        }
        QJsonArray heapRows;
        for (std::size_t h = 0; h < heaps.size(); ++h) {
            std::size_t resident = 0, residentIndex = 0, reserved = 0;
            int parts = 0;
            for (const Allocation& a : all) {
                if (a.heap == static_cast<int>(h)) {
                    resident += a.resident;
                    residentIndex += a.residentIndex;
                    reserved += a.reserved;
                    ++parts;
                }
            }
            heapRows.append(QJsonObject {{u"heap"_s, static_cast<int>(h)}, {u"default"_s, heaps[h] == ::GetProcessHeap()},
                {u"resident_mb"_s, megabytes(resident)}, {u"resident_index_mb"_s, megabytes(residentIndex)},
                {u"busy_mb"_s, megabytes(sums[h].busy)}, {u"free_mb"_s, megabytes(sums[h].free)}, {u"overhead_mb"_s, megabytes(sums[h].overhead)},
                {u"blocks"_s, static_cast<qint64>(sums[h].blocks)}, {u"reserved_mb"_s, megabytes(reserved)}, {u"allocations"_s, parts}});
        }
        // The rest by kind and by size reserved: many of one size are one kind of thing.
        struct Kind {
            std::size_t resident = 0, reserved = 0;
            int count = 0;
        };
        std::map<QString, Kind> kinds;
        for (const Allocation& a : all) {
            if (a.heap >= 0)
                continue;
            const QString protect = a.stack ? u"stack"_s
                : a.protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY) ? u"code"_s
                : a.protect & PAGE_WRITECOMBINE ? u"write-combined"_s
                : a.protect & (PAGE_READWRITE | PAGE_WRITECOPY) ? u"data"_s : u"read-only"_s;
            const double mb = static_cast<double>(a.reserved) / 1048576.0;
            const QString size = mb <= 0.07 ? u"64 KB"_s : mb <= 1.2 ? u"up to 1 MB"_s : u"%1 MB"_s.arg(std::round(mb));
            Kind& kind = kinds[u"%1, %2, %3"_s.arg(a.type == MEM_MAPPED ? u"mapped"_s : u"private"_s, protect, size)];
            kind.resident += a.resident + a.residentIndex;
            kind.reserved += a.reserved;
            ++kind.count;
        }
        std::vector<std::pair<QString, Kind>> sorted(kinds.begin(), kinds.end());
        std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.resident > b.second.resident; });
        QJsonArray others;
        for (const auto& [name, kind] : sorted) {
            if (kind.resident >= 256 * 1024)
                others.append(QJsonObject {{u"kind"_s, name}, {u"resident_mb"_s, megabytes(kind.resident)}, {u"reserved_mb"_s, megabytes(kind.reserved)},
                    {u"count"_s, kind.count}});
        }
        return {{u"index_blocks_mb"_s, megabytes(indexBytes)}, {u"heaps"_s, heapRows}, {u"others"_s, others}};
    }

    // --foreground-use-walk: every block of every heap by size (HeapWalk),
    // the largest ones singly. Reading the blocks' headers brings their pages
    // in: the readings after it are not what the launcher needs any more, so
    // only for telling what the heaps hold, not for measuring.
    static QJsonObject heapBlocks(const QString& stage)
    {
        std::vector<HANDLE> heaps(256);
        heaps.resize(std::min<DWORD>(256, ::GetProcessHeaps(256, heaps.data())));
        struct Bin {
            qint64 count = 0;
            quint64 bytes = 0;
        };
        Bin bins[40] {};
        std::map<std::size_t, qint64> exact; // sizes of 64 KB and more, by how many
        std::map<std::size_t, QByteArray> starts;
        quint64 busy = 0, overhead = 0, uncommitted = 0;
        for (const HANDLE heap : heaps) {
            if (!::HeapLock(heap))
                continue;
            PROCESS_HEAP_ENTRY e {};
            while (::HeapWalk(heap, &e)) {
                if (e.wFlags & PROCESS_HEAP_UNCOMMITTED_RANGE) {
                    uncommitted += e.cbData;
                    continue;
                }
                if (!(e.wFlags & PROCESS_HEAP_ENTRY_BUSY))
                    continue;
                busy += e.cbData;
                overhead += e.cbOverhead;
                Bin& bin = bins[std::bit_width(std::max<std::size_t>(e.cbData, 1)) - 1];
                ++bin.count;
                bin.bytes += e.cbData;
                if (e.cbData >= 65536) {
                    // What a size's first block starts with: often enough to tell whose it is.
                    if (++exact[e.cbData] == 1)
                        starts[e.cbData] = QByteArray(static_cast<const char*>(e.lpData), 96);
                }
            }
            ::HeapUnlock(heap);
        }
        QJsonArray sizes;
        for (int i = 0; i < 40; ++i) {
            if (bins[i].count)
                sizes.append(QJsonObject {{u"from"_s, static_cast<qint64>(std::size_t {1} << i)}, {u"count"_s, bins[i].count}, {u"mb"_s, megabytes(bins[i].bytes)}});
        }
        QJsonArray big;
        for (const auto& [size, count] : exact) {
            if (size * static_cast<std::size_t>(count) >= (1u << 20))
                big.append(QJsonObject {{u"bytes"_s, static_cast<qint64>(size)}, {u"count"_s, count}, {u"mb"_s, megabytes(size * static_cast<std::size_t>(count))},
                    {u"starts"_s, QString::fromLatin1(starts[size].toHex(' '))}});
        }
        return {{u"stage"_s, stage}, {u"heaps"_s, static_cast<qint64>(heaps.size())}, {u"busy_mb"_s, megabytes(busy)},
            {u"overhead_mb"_s, megabytes(overhead)}, {u"uncommitted_mb"_s, megabytes(uncommitted)}, {u"by_size"_s, sizes}, {u"large"_s, big}};
    }

    // The query typed a key at a time: for each key, how long until the list
    // had its rows (the model's first change), then the rest of the search
    // (content, plugins) waited for.
    static QJsonObject type(App& app, const QString& query)
    {
        Launcher& launcher = *app.m_launcher;
        const QAbstractItemModel* model = launcher.results();
        QElapsedTimer timer;
        qint64 changedNs = -1;
        const auto note = [&] {
            if (changedNs < 0)
                changedNs = timer.nsecsElapsed();
        };
        const QMetaObject::Connection connections[] {
            QObject::connect(model, &QAbstractItemModel::modelReset, model, note),
            QObject::connect(model, &QAbstractItemModel::rowsInserted, model, note),
            QObject::connect(model, &QAbstractItemModel::rowsRemoved, model, note),
            QObject::connect(model, &QAbstractItemModel::dataChanged, model, note),
        };
        QJsonArray keys;
        double total = 0;
        for (qsizetype length = 1; length <= query.size(); ++length) {
            changedNs = -1;
            timer.start();
            launcher.setQuery(query.left(length));
            waitFor([&] { return changedNs >= 0; }, 10'000);
            const double ms = std::round(static_cast<double>(changedNs) / 100'000.0) / 10;
            keys.append(ms);
            total += ms;
            pump(90); // between two keys
        }
        waitFor([&] { return !launcher.searching(); }, 10'000);
        for (const QMetaObject::Connection& connection : connections)
            QObject::disconnect(connection);
        pump(600); // read, before the next thing is looked for
        QJsonObject row = reading(app, query);
        row.insert(u"rows"_s, launcher.results()->rowCount());
        row.insert(u"keys_ms"_s, keys);
        row.insert(u"typed_ms"_s, std::round(total * 10) / 10);
        row.insert(u"name_search_ms"_s, static_cast<double>(launcher.elapsedUs()) / 1000.0);
        return row;
    }

    // --foreground-use-trace <from>[:<to>]: who asked for the heap memory
    // that stage <to> (the last one, if not given) holds more of than stage
    // <from> did ("start": all it holds; else "idle", "opened", a query of
    // the first round, "round1", ...), by call stack (allocationTrace,
    // started before the App is made). Not for measuring: see there.
    static QJsonArray sites(const memory::allocationTrace::Totals* before)
    {
        std::int64_t total = 0;
        QJsonArray out;
        const auto held = memory::allocationTrace::sites(memory::allocationTrace::held(), before, 400, &total);
        out.append(QJsonObject {{u"total_mb"_s, std::round(static_cast<double>(total) / 1048576.0 * 10) / 10},
            {u"untold_blocks"_s, memory::allocationTrace::untold()}});
        for (const auto& site : held) {
            out.append(QJsonObject {{u"mb"_s, std::round(static_cast<double>(site.bytes) / 1048576.0 * 100) / 100},
                {u"blocks"_s, site.blocks}, {u"frames"_s, QJsonArray::fromStringList(site.frames)}});
        }
        return out;
    }

    static bool run(App& app, const QStringList& args, const QDir& output)
    {
        const qsizetype traceAt = args.indexOf(u"--foreground-use-trace"_s);
        const QStringList traced = traceAt >= 0 && traceAt + 1 < args.size() ? args[traceAt + 1].split(u':') : QStringList();
        const QString traceFrom = traced.value(0), traceTo = traced.value(1, u"closed"_s);
        QJsonArray traceSites;
        std::optional<memory::allocationTrace::Totals> tracedBefore;
        const auto trace = [&](const QString& stage) {
            if (traced.isEmpty())
                return;
            if (stage == traceFrom)
                tracedBefore = memory::allocationTrace::held();
            else if (stage == traceTo && traceSites.isEmpty())
                traceSites = sites(tracedBefore ? &*tracedBefore : nullptr);
        };
        QElapsedTimer loading;
        loading.start();
        if (!waitFor([&app] { return app.m_index->state() == IndexService::State::Ready; }, 900'000))
            return false;
        const qint64 loadedMs = loading.elapsed();
        QJsonArray rows;
        const auto add = [&rows](const QJsonObject& row) {
            std::fprintf(stderr, "%s\n", QJsonDocument(row).toJson(QJsonDocument::Compact).constData());
            rows.append(row);
        };
        // Given back once, as after sitting idle: what follows came in for the launcher.
        app.m_idleTrim->setAfterUse(3'000);
        waitFor([&app] { return app.m_idleTrim->lastTrim().atMs != 0; }, 40'000);
        pump(5'000);
        add(reading(app, u"idle"_s));
        trace(u"idle"_s);

        const std::uint64_t pressedAt = ::GetTickCount64();
        app.beginTrigger(TriggerSource::Hotkey, pressedAt);
        app.toggleLauncher(true, pressedAt);
        const bool came = waitFor([&app] { return app.m_launcherRevealedAt.has_value(); }, 10'000);
        pump(2'000);
        add(reading(app, u"opened"_s));
        trace(u"opened"_s);
        const bool walk = args.contains(u"--foreground-use-walk"_s);
        QJsonArray walks;
        if (walk)
            walks.append(heapBlocks(u"opened"_s));

        const qsizetype roundsAt = args.indexOf(u"--foreground-use-rounds"_s);
        const int rounds = std::clamp(roundsAt >= 0 && roundsAt + 1 < args.size() ? args[roundsAt + 1].toInt() : 2, 1, 20);
        const QStringList queries {u"report"_s, u"wx"_s, u"项目"_s, u".pdf"_s, u"a"_s, u"setup exe"_s, u"zhaopian"_s, u"2026 notes"_s};
        for (int round = 0; round < rounds; ++round) {
            for (const QString& query : queries) {
                QJsonObject row = type(app, query);
                row.insert(u"round"_s, round + 1);
                add(row);
                if (walk && round == 0 && query == queries.front())
                    walks.append(heapBlocks(query));
                if (round == 0)
                    trace(query);
                app.m_launcher->setQuery(QString());
                pump(300);
            }
            QJsonObject row = reading(app, u"cleared"_s);
            row.insert(u"round"_s, round + 1);
            add(row);
            trace(u"round%1"_s.arg(round + 1));
        }
        app.hideLauncher();
        pump(1'500);
        add(reading(app, u"closed"_s));
        trace(u"closed"_s);
        // Last: both walk the heaps, which brings their pages in.
        const QJsonObject held = anatomy(app);
        if (walk)
            walks.append(heapBlocks(u"end"_s));

        QJsonObject report {
            {u"passed"_s, came},
            {u"entries"_s, static_cast<qint64>(app.m_index->index() ? app.m_index->index()->liveCount() : 0)},
            {u"index_ready_ms"_s, loadedMs},
            {u"stages"_s, rows},
            {u"anatomy"_s, held},
            {u"heap_blocks"_s, walks},
            {u"held_since_%1"_s.arg(traceFrom), traceSites},
        };
        QFile file(output.filePath(u"foreground-use.json"_s));
        return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(report).toJson()) > 0 && came;
    }
};

bool checkAllocationTrace()
{
    namespace trace = memory::allocationTrace;
    const auto heldBytes = [] {
        std::int64_t bytes = 0;
        for (const auto& [stack, sum] : trace::held())
            bytes += sum.bytes;
        return bytes;
    };
    const std::int64_t before = heldBytes();
    constexpr int kThreads = 8;
    constexpr std::size_t kSizes[] {24, 200, 4'000, 70'000, 300'000, 1'114'112};
    using Block = std::pair<std::unique_ptr<char[]>, std::size_t>;
    std::vector<std::vector<Block>> kept(kThreads);
    std::atomic<std::int64_t> expected {0};
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&kept, &expected, t, &kSizes] {
                std::vector<Block>& mine = kept[static_cast<std::size_t>(t)];
                for (int i = 0; i < 20'000; ++i) {
                    const std::size_t size = kSizes[static_cast<std::size_t>(i) % (i % 400 == 0 ? 6 : 4)];
                    Block block {std::make_unique_for_overwrite<char[]>(size), size};
                    if (i % 3 == 0) { // kept; the rest freed, at once or in place of one kept
                        expected += static_cast<std::int64_t>(size);
                        mine.push_back(std::move(block));
                    } else if (i % 3 == 1 && !mine.empty()) {
                        expected += static_cast<std::int64_t>(size) - static_cast<std::int64_t>(mine.back().second);
                        std::swap(block, mine.back());
                    }
                }
            });
        }
    }
    const std::int64_t held = heldBytes() - before;
    kept.clear();
    const std::int64_t after = heldBytes() - before;
    // The lists' own blocks and what else the process allocated meanwhile: a little either way.
    const bool ok = std::abs(held - expected) < expected / 50 && std::abs(after) < expected / 50 && trace::untold() == 0;
    std::fprintf(stderr, "allocation trace check: %s (kept %.1f MB, told %.1f MB, after freeing %.1f MB)\n", ok ? "ok" : "FAILED",
        static_cast<double>(expected) / 1048576.0, static_cast<double>(held) / 1048576.0, static_cast<double>(after) / 1048576.0);
    return ok;
}

bool checkForegroundUse(App& app, const QStringList& args, const QDir& output)
{
    return ForegroundUseProbe::run(app, args, output);
}

} // namespace ws::preview
