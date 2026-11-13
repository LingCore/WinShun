#include "IdleOpenProbe.h"

#include "App.h"
#include "IndexService.h"
#include "platform/HotPages.h"
#include "platform/IdleTrim.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <windows.h>
#include <psapi.h>

using namespace Qt::StringLiterals;

namespace ws::preview {

// --probe-runtime <folder> --idle-open-check [--with-clipboard] [--renderer d3d11]
//     [--idle-open-index <folder;folder>] [--idle-open-cycles 8] [--idle-open-after-use 3000]
//     [--idle-open-rest 0] [--idle-open-pressure <folder>]
// The production App beside the Win顺 that runs, its windows opened the way a
// hotkey opens them (beginTrigger, then the toggle), each opening timed from
// that call until the window is there to be seen (uncloaked after its first
// frame), with the page faults it took and the memory held just before.
// First of all, once the memory was given back for the first time since the
// start (waited for):
//   first    each window's first opening
// Then each cycle, for the launcher and then the clipboard: opened once and
// closed (in use), then
//   idle     after IdleTrim gave the memory back by itself (`trimmed`; waited
//            for: --idle-open-after-use after the last use instead of 20 s,
//            so that many openings can be timed), or found too little to;
//            then left alone for --idle-open-rest more. With
//            --idle-open-pressure, Windows is then short of memory
//            (`pressed`): what was given back is no longer in memory and
//            comes from the disk. An elevated helper does that, asked through
//            <folder>\purge.request and answering with purge.done: it writes
//            the modified pages out and empties the standby list.
//   warm     opened again a moment after that one closed
// and at the end, three times each:
//   windows  after Windows emptied the working set (EmptyWorkingSet, untold)
// `cpu` is how busy the whole machine was from a second before the opening
// until it was seen, 0 to 1: other work under way spoils a time.
// Into idle-open.json. Nothing is typed and no input is sent; each opening
// waits until the keyboard and mouse were left alone for three seconds, as
// the launcher takes the keyboard while it is up (about half a second).
struct IdleOpenProbe {
    static void pump(int ms)
    {
        QElapsedTimer timer;
        timer.start();
        do
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        while (timer.elapsed() < ms);
    }

    static PROCESS_MEMORY_COUNTERS_EX2 counters()
    {
        PROCESS_MEMORY_COUNTERS_EX2 c {};
        c.cb = sizeof c;
        ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof c);
        return c;
    }

    struct MachineTime {
        quint64 idle = 0;
        quint64 all = 0;
    };
    static MachineTime machineTime()
    {
        FILETIME idle {}, kernel {}, user {}; // kernel time includes the idle time
        ::GetSystemTimes(&idle, &kernel, &user);
        const auto ticks = [](const FILETIME& t) { return (static_cast<quint64>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        return {ticks(idle), ticks(kernel) + ticks(user)};
    }

    static void waitForQuietInput()
    {
        QElapsedTimer waited;
        waited.start();
        for (;;) {
            LASTINPUTINFO last {};
            last.cbSize = sizeof last;
            if (!::GetLastInputInfo(&last) || ::GetTickCount() - last.dwTime >= 3000 || waited.elapsed() > 120'000)
                return;
            pump(200);
        }
    }

    static bool seen(const App& app, bool clipboard)
    {
        return clipboard ? app.m_clipRevealedAt.has_value() : app.m_launcherRevealedAt.has_value();
    }

    static double megabytes(std::size_t bytes) { return std::round(static_cast<double>(bytes) / 1048576.0 * 10) / 10; }

    static QJsonObject open(App& app, bool clipboard, const char* kind)
    {
        waitForQuietInput();
        const MachineTime machineBefore = machineTime();
        pump(1'000);
        const PROCESS_MEMORY_COUNTERS_EX2 before = counters();
        LARGE_INTEGER frequency {}, from {}, to {};
        ::QueryPerformanceFrequency(&frequency);
        const auto since = [&] {
            ::QueryPerformanceCounter(&to);
            return std::round(10'000.0 * static_cast<double>(to.QuadPart - from.QuadPart) / static_cast<double>(frequency.QuadPart)) / 10;
        };
        ::QueryPerformanceCounter(&from);
        const std::uint64_t pressedAt = ::GetTickCount64();
        app.beginTrigger(clipboard ? TriggerSource::ClipboardHotkey : TriggerSource::Hotkey, pressedAt);
        const double beginMs = since();
        if (clipboard)
            app.toggleClipboard(pressedAt);
        else
            app.toggleLauncher(true, pressedAt);
        const double callMs = since();
        QElapsedTimer limit;
        limit.start();
        while (!seen(app, clipboard) && limit.elapsed() < 10'000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        const double seenMs = since();
        const bool came = seen(app, clipboard);
        const PROCESS_MEMORY_COUNTERS_EX2 after = counters();
        const MachineTime machineAfter = machineTime();
        pump(400); // up for a moment, as when used
        const PROCESS_MEMORY_COUNTERS_EX2 settled = counters();
        if (clipboard)
            app.hideClipboard(false);
        else
            app.hideLauncher();
        pump(600); // the way out, and what closing sets going
        const quint64 all = machineAfter.all - machineBefore.all;
        QJsonObject row {
            {u"window"_s, clipboard ? u"clipboard"_s : u"launcher"_s},
            {u"kind"_s, QString::fromLatin1(kind)},
            {u"came"_s, came},
            {u"seen_ms"_s, seenMs},
            {u"begin_ms"_s, beginMs}, // beginTrigger alone: the working set looked at, the read-ahead started
            {u"call_ms"_s, callMs},
            {u"faults"_s, static_cast<qint64>(after.PageFaultCount - before.PageFaultCount)},
            {u"private_mb_before"_s, megabytes(before.PrivateWorkingSetSize)},
            {u"working_mb_before"_s, megabytes(before.WorkingSetSize)},
            {u"private_mb_up"_s, megabytes(settled.PrivateWorkingSetSize)},
            {u"hot_mb"_s, megabytes((clipboard ? app.m_clipPages : app.m_launcherPages)->bytes())},
            {u"cpu"_s, all ? std::round(100.0 * (1.0 - static_cast<double>(machineAfter.idle - machineBefore.idle) / static_cast<double>(all))) / 100 : 0.0},
        };
        return row;
    }

    // IdleTrim's own giving back, a while after the last use and whenever
    // its rechecks find enough: the one after `since`, if one comes.
    static bool waitForIdleTrim(App& app, qint64 since, int ms)
    {
        QElapsedTimer waited;
        waited.start();
        while (app.m_idleTrim->lastTrim().atMs <= since && waited.elapsed() < ms)
            pump(100);
        const bool trimmed = app.m_idleTrim->lastTrim().atMs > since;
        pump(1'500); // what it sets going has ended
        return trimmed;
    }

    static bool run(App& app, const QStringList& args, const QDir& output)
    {
        const auto number = [&args](QStringView flag, int fallback) {
            const qsizetype at = args.indexOf(flag);
            return at >= 0 && at + 1 < args.size() ? args[at + 1].toInt() : fallback;
        };
        const int cycles = std::clamp(number(u"--idle-open-cycles", 8), 1, 100);
        const int afterUse = std::clamp(number(u"--idle-open-after-use", 3'000), 1'000, 20'000);
        const int rest = std::clamp(number(u"--idle-open-rest", 0), 0, 600'000);
        QElapsedTimer loading;
        loading.start();
        while (app.m_index->state() != IndexService::State::Ready && loading.elapsed() < 900'000)
            pump(250);
        if (app.m_index->state() != IndexService::State::Ready)
            return false;
        const qint64 loadedMs = loading.elapsed();
        app.m_idleTrim->setAfterUse(afterUse);
        std::vector<bool> windows {false};
        if (app.m_settings.clipboard)
            windows.push_back(true);
        QJsonArray rows;
        const auto add = [&rows](QJsonObject row, std::optional<bool> trimmed = {}) {
            if (trimmed)
                row.insert(u"trimmed"_s, *trimmed);
            std::fprintf(stderr, "%s\n", QJsonDocument(row).toJson(QJsonDocument::Compact).constData());
            rows.append(row);
        };
        const qsizetype pressureAt = args.indexOf(u"--idle-open-pressure"_s);
        const QDir pressure(pressureAt >= 0 && pressureAt + 1 < args.size() ? args[pressureAt + 1] : QString());
        const auto press = [&pressure] {
            QFile::remove(pressure.filePath(u"purge.done"_s));
            QFile request(pressure.filePath(u"purge.request"_s));
            if (!request.open(QIODevice::WriteOnly))
                return false;
            request.close();
            QElapsedTimer waited;
            waited.start();
            while (!QFile::exists(pressure.filePath(u"purge.done"_s)) && waited.elapsed() < 30'000)
                pump(50);
            pump(500);
            return QFile::remove(pressure.filePath(u"purge.done"_s));
        };
        {
            const bool trimmed = waitForIdleTrim(app, 0, 40'000);
            pump(4'000); // what the App does about it has ended
            for (const bool clipboard : windows) {
                const std::optional<bool> pressed = pressureAt >= 0 ? std::optional(press()) : std::nullopt;
                QJsonObject row = open(app, clipboard, "first");
                if (pressed)
                    row.insert(u"pressed"_s, *pressed);
                add(row, trimmed);
            }
        }
        for (int i = 0; i < cycles; ++i) {
            for (const bool clipboard : windows) {
                open(app, clipboard, "use");
                const bool trimmed = waitForIdleTrim(app, app.m_idleTrim->lastTrim().atMs, afterUse + 6'000);
                pump(rest);
                const std::optional<bool> pressed = pressureAt >= 0 ? std::optional(press()) : std::nullopt;
                QJsonObject row = open(app, clipboard, "idle");
                if (pressed)
                    row.insert(u"pressed"_s, *pressed);
                add(row, trimmed);
                add(open(app, clipboard, "warm"));
            }
        }
        for (int i = 0; i < 3; ++i) {
            for (const bool clipboard : windows) {
                open(app, clipboard, "use");
                ::SetProcessWorkingSetSizeEx(::GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);
                add(open(app, clipboard, "windows"));
            }
        }
        waitForIdleTrim(app, app.m_idleTrim->lastTrim().atMs, afterUse + 6'000);
        pump(10'000);
        const PROCESS_MEMORY_COUNTERS_EX2 idle = counters();
        bool passed = true;
        for (const QJsonValue row : std::as_const(rows))
            passed = passed && row[u"came"_s].toBool();
        QJsonObject report {
            {u"passed"_s, passed},
            {u"entries"_s, static_cast<qint64>(app.m_index->index() ? app.m_index->index()->liveCount() : 0)},
            {u"index_ready_ms"_s, loadedMs},
            {u"idle_private_mb"_s, megabytes(idle.PrivateWorkingSetSize)},
            {u"idle_working_mb"_s, megabytes(idle.WorkingSetSize)},
            {u"openings"_s, rows},
        };
        QFile file(output.filePath(u"idle-open.json"_s));
        return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(report).toJson()) > 0 && passed;
    }
};

bool checkIdleOpen(App& app, const QStringList& args, const QDir& output)
{
    return IdleOpenProbe::run(app, args, output);
}

} // namespace ws::preview
