#include "ForegroundUseProbe.h"

#include "App.h"
#include "IndexService.h"
#include "Launcher.h"
#include "platform/IdleTrim.h"
#include "platform/ProcessUsage.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <bit>
#include <cstdio>
#include <map>
#include <vector>

#include <windows.h>
#include <psapi.h>

using namespace Qt::StringLiterals;

namespace ws::preview {

// --probe-runtime <folder> --foreground-use-check [--renderer d3d11]
//     [--idle-open-index <folder;folder>] [--foreground-use-rounds 2]
// The production App beside the Win顺 that runs, its launcher opened the way
// a hotkey opens it and typed into a key at a time, as somebody looking for a
// few things in a row would: after each query, what the process holds
// (Task Manager's Memory is `private_mb`) and by what it is (memory::map: the
// file index's blocks, the heaps, the program files' own pages), and how long
// each key's search took until its rows were there. Before all that, the
// memory was given back once (IdleTrim, waited for), so what is counted came
// in for the launcher. `allocations`: the largest private allocations
// outside the file index at the end, for telling what the heaps' part is.
// Into foreground-use.json. No input is sent: the queries are set on the
// launcher, whose window is up without the keyboard.
struct ForegroundUseProbe {
    static void pump(int ms)
    {
        QElapsedTimer timer;
        timer.start();
        do
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        while (timer.elapsed() < ms);
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

    // The private allocations outside the file index, largest in memory first.
    static QJsonArray allocations(App& app, int most)
    {
        auto index = indexBlocks(app);
        std::sort(index.begin(), index.end());
        const auto inIndex = [&index](const void* base) {
            auto it = std::upper_bound(index.begin(), index.end(), std::pair<const void*, std::size_t>(base, SIZE_MAX));
            if (it == index.begin())
                return false;
            --it;
            return static_cast<const char*>(base) < static_cast<const char*>(it->first) + it->second;
        };
        std::vector<HANDLE> heaps(256);
        heaps.resize(std::min<DWORD>(256, ::GetProcessHeaps(256, heaps.data())));
        struct Allocation {
            std::uintptr_t base = 0;
            std::size_t reserved = 0;
            std::size_t resident = 0;
            bool heap = false;
            bool stack = false;
        };
        std::vector<Allocation> all;
        static PSAPI_WORKING_SET_EX_INFORMATION info[4096];
        const void* last = nullptr;
        for (std::uintptr_t at = 0;;) {
            MEMORY_BASIC_INFORMATION m;
            if (::VirtualQuery(reinterpret_cast<const void*>(at), &m, sizeof m) == 0)
                break;
            const std::uintptr_t next = reinterpret_cast<std::uintptr_t>(m.BaseAddress) + m.RegionSize;
            if (m.State != MEM_FREE && m.Type != MEM_IMAGE && !inIndex(m.AllocationBase)) {
                if (m.AllocationBase != last) {
                    last = m.AllocationBase;
                    all.push_back({reinterpret_cast<std::uintptr_t>(m.AllocationBase), 0, 0,
                        std::find(heaps.begin(), heaps.end(), m.AllocationBase) != heaps.end(), false});
                }
                Allocation& a = all.back();
                a.reserved += m.RegionSize;
                a.stack = a.stack || (m.Protect & PAGE_GUARD);
                if (m.State == MEM_COMMIT && !(m.Protect & PAGE_GUARD)) {
                    for (std::uintptr_t page = reinterpret_cast<std::uintptr_t>(m.BaseAddress); page < next;) {
                        const std::size_t n = std::min<std::size_t>(std::size(info), (next - page) / 4096);
                        for (std::size_t i = 0; i < n; ++i)
                            info[i].VirtualAddress = reinterpret_cast<void*>(page + i * 4096);
                        if (::QueryWorkingSetEx(::GetCurrentProcess(), info, static_cast<DWORD>(n * sizeof info[0]))) {
                            for (std::size_t i = 0; i < n; ++i)
                                a.resident += info[i].VirtualAttributes.Valid && !info[i].VirtualAttributes.Shared ? 4096 : 0;
                        }
                        page += n * 4096;
                    }
                }
            }
            if (next <= at)
                break;
            at = next;
        }
        std::sort(all.begin(), all.end(), [](const Allocation& a, const Allocation& b) { return a.resident > b.resident; });
        QJsonArray out;
        // By size held, too: many of one size are one kind of thing.
        std::size_t small = 0, medium = 0, large = 0;
        for (const Allocation& a : all) {
            (a.reserved <= (1u << 20) ? small : a.reserved <= (16u << 20) ? medium : large) += a.resident;
        }
        out.append(QJsonObject {{u"up_to_1mb"_s, megabytes(small)}, {u"up_to_16mb"_s, megabytes(medium)}, {u"larger"_s, megabytes(large)},
            {u"count"_s, static_cast<qint64>(all.size())}});
        for (int i = 0; i < most && i < static_cast<int>(all.size()); ++i) {
            const Allocation& a = all[static_cast<std::size_t>(i)];
            out.append(QJsonObject {
                {u"base"_s, QString::number(a.base, 16)},
                {u"reserved_mb"_s, megabytes(a.reserved)},
                {u"resident_mb"_s, megabytes(a.resident)},
                {u"kind"_s, a.heap ? u"heap"_s : a.stack ? u"stack"_s : u"block"_s},
            });
        }
        return out;
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
        std::vector<std::size_t> largest;
        std::map<std::size_t, qint64> exact; // sizes of 64 KB and more, by how many
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
                    ++exact[e.cbData];
                    largest.push_back(e.cbData);
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
                big.append(QJsonObject {{u"bytes"_s, static_cast<qint64>(size)}, {u"count"_s, count}, {u"mb"_s, megabytes(size * static_cast<std::size_t>(count))}});
        }
        return {{u"stage"_s, stage}, {u"heaps"_s, static_cast<qint64>(heaps.size())}, {u"busy_mb"_s, megabytes(busy)},
            {u"overhead_mb"_s, megabytes(overhead)}, {u"uncommitted_mb"_s, megabytes(uncommitted)}, {u"by_size"_s, sizes}, {u"large"_s, big}};
    }

    // The query typed a key at a time; each key's search waited for.
    static QJsonObject type(App& app, const QString& query)
    {
        Launcher& launcher = *app.m_launcher;
        QJsonArray keys;
        double total = 0;
        for (qsizetype length = 1; length <= query.size(); ++length) {
            QElapsedTimer timer;
            timer.start();
            launcher.setQuery(query.left(length));
            // Under way by the next turn of the loop at the latest (the name
            // search is debounced by none; content and plugins by a little).
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            while (launcher.searching() && timer.elapsed() < 10'000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            const double ms = std::round(static_cast<double>(timer.nsecsElapsed()) / 100'000.0) / 10;
            keys.append(ms);
            total += ms;
            pump(90); // between two keys
        }
        pump(600); // read, before the next thing is looked for
        QJsonObject row = reading(app, query);
        row.insert(u"rows"_s, launcher.results()->rowCount());
        row.insert(u"keys_ms"_s, keys);
        row.insert(u"typed_ms"_s, std::round(total * 10) / 10);
        row.insert(u"name_search_ms"_s, static_cast<double>(launcher.elapsedUs()) / 1000.0);
        return row;
    }

    static bool run(App& app, const QStringList& args, const QDir& output)
    {
        QElapsedTimer loading;
        loading.start();
        while (app.m_index->state() != IndexService::State::Ready && loading.elapsed() < 900'000)
            pump(250);
        if (app.m_index->state() != IndexService::State::Ready)
            return false;
        const qint64 loadedMs = loading.elapsed();
        QJsonArray rows;
        const auto add = [&rows](const QJsonObject& row) {
            std::fprintf(stderr, "%s\n", QJsonDocument(row).toJson(QJsonDocument::Compact).constData());
            rows.append(row);
        };
        // Given back once, as after sitting idle: what follows came in for the launcher.
        app.m_idleTrim->setAfterUse(3'000);
        {
            QElapsedTimer waited;
            waited.start();
            while (app.m_idleTrim->lastTrim().atMs == 0 && waited.elapsed() < 40'000)
                pump(100);
            pump(5'000);
        }
        add(reading(app, u"idle"_s));

        const std::uint64_t pressedAt = ::GetTickCount64();
        app.beginTrigger(TriggerSource::Hotkey, pressedAt);
        app.toggleLauncher(true, pressedAt);
        {
            QElapsedTimer limit;
            limit.start();
            while (!app.m_launcherRevealedAt.has_value() && limit.elapsed() < 10'000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        }
        const bool came = app.m_launcherRevealedAt.has_value();
        pump(2'000);
        add(reading(app, u"opened"_s));
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
                app.m_launcher->setQuery(QString());
                pump(300);
            }
            QJsonObject row = reading(app, u"cleared"_s);
            row.insert(u"round"_s, round + 1);
            add(row);
        }
        const QJsonArray held = allocations(app, 40);
        if (walk)
            walks.append(heapBlocks(u"end"_s));
        app.hideLauncher();
        pump(1'500);
        add(reading(app, u"closed"_s));

        QJsonObject report {
            {u"passed"_s, came},
            {u"entries"_s, static_cast<qint64>(app.m_index->index() ? app.m_index->index()->liveCount() : 0)},
            {u"index_ready_ms"_s, loadedMs},
            {u"stages"_s, rows},
            {u"allocations"_s, held},
            {u"heap_blocks"_s, walks},
        };
        QFile file(output.filePath(u"foreground-use.json"_s));
        return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(report).toJson()) > 0 && came;
    }
};

bool checkForegroundUse(App& app, const QStringList& args, const QDir& output)
{
    return ForegroundUseProbe::run(app, args, output);
}

} // namespace ws::preview
