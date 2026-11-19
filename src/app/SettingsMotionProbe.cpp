#include "SettingsMotionProbe.h"

#include "App.h"
#include "Settings.h"
#include "SettingsEditor.h"

#include <QAccessible>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QMutex>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QScreen>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

#include <windows.h>

using namespace Qt::StringLiterals;

namespace ws::preview {

namespace {

// --probe-runtime <folder> --settings-motion-check [--all-pages] [--activate] [--plugins-only] [--gallery-early]
//     [--other-windows]
// The production App beside the Win顺 that runs, its settings window shown
// without being activated, on 插件. Two things are measured.
//
// How the window answers UI Automation: its accessibility tree walked the
// way Windows walks it through Qt, on the 插件 page alone and (--all-pages)
// with every page made, as after a search or a while of use. Qt lists an
// item's accessible children by going down through every item that is not
// accessible itself, visible or not, once for each child asked for; with no
// accessible parts in between, every text of every page stood under the
// window and one walk took a quarter of a second. Screen readers, input
// methods and assistants that read the window in front walk it over and
// over: the GUI thread was busy most of the time while the window was in
// front (docs/pitfalls.md, “设置窗口在前台时卡”). It fails when more than 20
// elements stand under the window or a walk takes more than 20 ms.
// --activate puts the window in front for a while, so that the UIA clients
// of this PC look at it, and writes down how long the GUI thread was held.
// The other windows' roots are counted too (not judged).
//
// How smoothly it moves: each step (a tab or a page shown, the wheel turned
// over the page) done as the window does it, and while it plays out every
// frame the render thread presents is written down, with the GUI thread's
// stalls (a 1 ms timer that came late) and the render thread's sync and
// render times. Into settings-motion.json and settings-motion.log; nothing
// is typed and no real input is sent (the wheel and the pointer are Qt
// events to the window).
struct Recorder {
    QElapsedTimer clock;
    QMutex lock;
    std::vector<qint64> swaps; // ns, when each frame was presented
    std::vector<std::pair<qint64, qint64>> syncs; // ns, render thread: [start, end]
    std::vector<std::pair<qint64, qint64>> renders;
    qint64 syncStart = -1;
    qint64 renderStart = -1;
    std::vector<qint64> frameStarts; // ns, GUI thread: each frame's afterAnimating
    std::vector<std::pair<qint64, qint64>> stalls; // ns, GUI thread: [start, length] of each tick that came late
    qint64 lastTick = -1;

    qint64 now() const { return clock.nsecsElapsed(); }
};

QFile* g_log = nullptr;

void say(const QString& line)
{
    std::fprintf(stderr, "%s\n", qUtf8Printable(line));
    if (g_log) {
        g_log->write((line + u'\n').toUtf8());
        g_log->flush();
    }
}

QQuickWindow* settingsWindow()
{
    for (QWindow* candidate : QGuiApplication::allWindows()) {
        if (candidate->property("editor").value<SettingsEditor*>())
            return qobject_cast<QQuickWindow*>(candidate);
    }
    return nullptr;
}

QVariant js(QQuickWindow* window, const QString& expression)
{
    QQmlExpression e(qmlContext(window), window, expression);
    QVariant value = e.evaluate();
    if (e.hasError()) {
        say(u"WARNING: %1: %2"_s.arg(expression, e.error().toString()));
        return {};
    }
    if (value.metaType() == QMetaType::fromType<QJSValue>())
        value = value.value<QJSValue>().toVariant();
    return value;
}

double ms(qint64 ns) { return static_cast<double>(ns) / 1e6; }
double round1(double v) { return std::round(v * 10) / 10; }

double percentile(std::vector<double> values, double p)
{
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    const auto at = static_cast<std::size_t>(std::clamp(p * static_cast<double>(values.size() - 1) + 0.5, 0.0,
        static_cast<double>(values.size() - 1)));
    return values[at];
}

// The accessibility tree walked as QWindowsUiaMainProvider::Navigate walks it:
// the first visible child, then each next visible sibling (the parent's
// indexOfChild + 1, then child(i) until one is not invisible).
struct UiaWalk {
    int visited = 0;
    int childCalls = 0;
};

QAccessibleInterface* firstVisible(QAccessibleInterface* parent, int from, UiaWalk& walk)
{
    for (int count = parent->childCount(), i = from; i >= 0 && i < count; ++i) {
        ++walk.childCalls;
        if (QAccessibleInterface* child = parent->child(i); child && child->isValid() && !child->state().invisible)
            return child;
    }
    return nullptr;
}

void walkUia(QAccessibleInterface* node, int depth, UiaWalk& walk)
{
    ++walk.visited;
    if (depth > 64)
        return;
    for (QAccessibleInterface* child = firstVisible(node, 0, walk); child;) {
        walkUia(child, depth + 1, walk);
        QAccessibleInterface* parent = child->parent();
        if (!parent)
            break;
        child = firstVisible(parent, parent->indexOfChild(child) + 1, walk);
    }
}

QJsonObject timeUiaWalk(QWindow* window)
{
    QAccessibleInterface* root = QAccessible::queryAccessibleInterface(window);
    if (!root)
        return {{u"error"_s, u"no accessible window"_s}};
    UiaWalk walk;
    QElapsedTimer took;
    took.start();
    const int rootChildren = root->childCount();
    walkUia(root, 0, walk);
    return {{u"ms"_s, round1(took.nsecsElapsed() / 1e6)}, {u"visible_elements"_s, walk.visited},
        {u"child_calls"_s, walk.childCalls}, {u"root_children"_s, rootChildren}};
}

} // namespace

void copyGalleryCacheForMotionCheck()
{
    const QString kept = qEnvironmentVariable("LOCALAPPDATA") + u"/WinShun/plugin-index.json"_s;
    if (!QFile::exists(kept))
        return;
    QDir().mkpath(Settings::dataDir());
    const QString to = Settings::dataDir() + u"/plugin-index.json"_s;
    QFile::remove(to);
    QFile::copy(kept, to);
}

bool checkSettingsMotion(App& app, const QStringList& args, const QDir& output)
{
    QFile log(output.filePath(u"settings-motion.log"_s));
    if (log.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        g_log = &log;
    const auto closeLog = qScopeGuard([] { g_log = nullptr; });

    const auto pump = [](int ms) {
        QElapsedTimer timer;
        timer.start();
        do
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        while (timer.elapsed() < ms);
    };

    QElapsedTimer opening;
    opening.start();
    app.showSettings([](QQuickWindow* w) { QMetaObject::invokeMethod(w, "showPage", Q_ARG(QVariant, u"plugins"_s), Q_ARG(QVariant, 0)); });
    QQuickWindow* w = nullptr;
    while (opening.elapsed() < 8000 && !(w = settingsWindow()))
        pump(10);
    if (!w) {
        say(u"FAILED: no settings window"_s);
        return false;
    }

    Recorder rec;
    rec.clock.start();
    QObject::connect(w, &QQuickWindow::beforeSynchronizing, w, [&] { rec.syncStart = rec.now(); }, Qt::DirectConnection);
    QObject::connect(w, &QQuickWindow::afterSynchronizing, w, [&] {
        QMutexLocker locked(&rec.lock);
        rec.syncs.emplace_back(rec.syncStart, rec.now());
    }, Qt::DirectConnection);
    QObject::connect(w, &QQuickWindow::beforeRendering, w, [&] { rec.renderStart = rec.now(); }, Qt::DirectConnection);
    QObject::connect(w, &QQuickWindow::afterRendering, w, [&] {
        QMutexLocker locked(&rec.lock);
        rec.renders.emplace_back(rec.renderStart, rec.now());
    }, Qt::DirectConnection);
    QObject::connect(w, &QQuickWindow::frameSwapped, w, [&] {
        QMutexLocker locked(&rec.lock);
        rec.swaps.push_back(rec.now());
    }, Qt::DirectConnection);
    QObject::connect(w, &QQuickWindow::afterAnimating, w, [&] { rec.frameStarts.push_back(rec.now()); });
    QTimer tick;
    tick.setTimerType(Qt::PreciseTimer);
    tick.setInterval(1);
    QObject::connect(&tick, &QTimer::timeout, [&] {
        const qint64 at = rec.now();
        if (rec.lastTick >= 0 && at - rec.lastTick > 4'000'000)
            rec.stalls.emplace_back(rec.lastTick, at - rec.lastTick);
        rec.lastTick = at;
    });
    tick.start();
    while (opening.elapsed() < 8000 && app.settingsOpening().firstFrameMs < 0)
        pump(10);
    const qint64 firstFrameAt = rec.now();

    const double refresh = w->screen() ? w->screen()->refreshRate() : 60.0;
    const double period = 1000.0 / std::max(1.0, refresh);
    QJsonObject report {{u"refresh_hz"_s, refresh}, {u"dpr"_s, w->devicePixelRatio()},
        {u"window"_s, u"%1x%2"_s.arg(w->width()).arg(w->height())}, {u"style"_s, js(w, u"SystemTheme.uiStyle"_s).toString()}};
    QJsonArray steps;
    bool passed = true;

    // One step: `action` (timed), then `watchMs` of watching, `during` called every `everyMs` meanwhile.
    const auto measure = [&](const QString& name, const std::function<void()>& action, int watchMs,
                             const std::function<void(int)>& during = {}, int everyMs = 0) {
        pump(250); // what the step before left settles
        const qint64 from = rec.now();
        QElapsedTimer took;
        took.start();
        if (action)
            action();
        const double actionMs = took.nsecsElapsed() / 1e6;
        const qint64 actionEnd = rec.now();
        QTimer stepper;
        int stepIndex = 0;
        if (during && everyMs > 0) {
            stepper.setTimerType(Qt::PreciseTimer);
            stepper.setInterval(everyMs);
            QObject::connect(&stepper, &QTimer::timeout, [&] { during(stepIndex++); });
            stepper.start();
        }
        rec.lastTick = rec.now();
        pump(watchMs);
        stepper.stop();
        const qint64 to = rec.now();

        std::vector<qint64> swaps;
        std::vector<double> renders;
        double syncMax = 0;
        qint64 firstSync = -1;
        const auto frameStart = std::lower_bound(rec.frameStarts.begin(), rec.frameStarts.end(), actionEnd);
        const qint64 firstFrameStart = frameStart != rec.frameStarts.end() && *frameStart <= to ? *frameStart : -1;
        {
            QMutexLocker locked(&rec.lock);
            for (qint64 at : rec.swaps)
                if (at >= from && at <= to)
                    swaps.push_back(at);
            for (const auto& [start, end] : rec.syncs) {
                if (start < from || start > to)
                    continue;
                syncMax = std::max(syncMax, ms(end - start));
                if (firstSync < 0 && firstFrameStart >= 0 && start >= firstFrameStart)
                    firstSync = start;
            }
            for (const auto& [start, end] : rec.renders)
                if (start >= from && start <= to)
                    renders.push_back(ms(end - start));
        }
        double worstStall = 0;
        for (const auto& [start, length] : rec.stalls)
            if (start + length >= from && start <= to)
                worstStall = std::max(worstStall, ms(length));
        std::vector<double> intervals;
        for (std::size_t i = 1; i < swaps.size(); ++i)
            intervals.push_back(ms(swaps[i] - swaps[i - 1]));
        const double first = swaps.empty() ? -1 : ms(swaps.front() - from);
        const auto longestAt = std::max_element(intervals.begin(), intervals.end());
        const double longest = intervals.empty() ? 0 : *longestAt;
        // When the longest began, from the step's start, and how many frames came before:
        // after the motion it is only the wait for something new (an icon read), not a stutter.
        const double longestFrom = intervals.empty() ? -1 : ms(swaps[static_cast<std::size_t>(longestAt - intervals.begin())] - from);
        const int framesBeforeLongest = intervals.empty() ? 0 : static_cast<int>(longestAt - intervals.begin()) + 1;
        const auto longCount = std::count_if(intervals.begin(), intervals.end(), [&](double v) { return v > 1.5 * period; });
        steps.append(QJsonObject {
            {u"step"_s, name},
            {u"action_ms"_s, round1(actionMs)},
            {u"first_frame_ms"_s, round1(first)},
            // The first frame after the action: until the GUI thread began it, then until its sync.
            {u"first_frame_begun_ms"_s, firstFrameStart >= 0 ? round1(ms(firstFrameStart - actionEnd)) : -1},
            {u"first_frame_to_sync_ms"_s, firstFrameStart >= 0 && firstSync >= 0 ? round1(ms(firstSync - firstFrameStart)) : -1},
            {u"frames"_s, static_cast<int>(swaps.size())},
            {u"interval_p50"_s, round1(percentile(intervals, 0.5))},
            {u"interval_p95"_s, round1(percentile(intervals, 0.95))},
            {u"interval_max"_s, round1(longest)},
            {u"interval_max_from_ms"_s, round1(longestFrom)},
            {u"frames_before_interval_max"_s, framesBeforeLongest},
            {u"long_intervals"_s, static_cast<int>(longCount)},
            {u"sync_max"_s, round1(syncMax)},
            {u"render_p50"_s, round1(percentile(renders, 0.5))},
            {u"render_max"_s, round1(renders.empty() ? 0 : *std::max_element(renders.begin(), renders.end()))},
            {u"gui_stall_max"_s, round1(worstStall)},
        });
        say(u"%1: action %2 ms, first frame %3 ms, %4 frames, interval p50 %5 p95 %6 max %7 (after frame %8), long %9, sync max %10, worst GUI stall %11"_s
                .arg(name).arg(round1(actionMs)).arg(round1(first)).arg(swaps.size()).arg(round1(percentile(intervals, 0.5)))
                .arg(round1(percentile(intervals, 0.95))).arg(round1(longest)).arg(framesBeforeLongest).arg(longCount)
                .arg(round1(syncMax)).arg(round1(worstStall)));
    };

    const auto tab = [&](int index) { return [&, index] { js(w, u"clearFocus(); showTab(%1)"_s.arg(index)); }; };
    const auto page = [&](const QString& id, int tabIndex) {
        return [&, id, tabIndex] { js(w, u"clearFocus(); showPage('%1', %2)"_s.arg(id).arg(tabIndex)); };
    };
    // The wheel over the middle of the page, `notches` down then as many up,
    // one every `everyMs`; the pointer there first (the rows under it hover).
    const auto scroll = [&](const QString& name, int notches, int everyMs) {
        const QPointF over = js(w, u"flick.mapToItem(null, flick.width / 2, flick.height / 2)"_s).toPointF();
        QMouseEvent move(QEvent::MouseMove, over, w->mapToGlobal(over), Qt::NoButton, Qt::NoButton, {});
        QCoreApplication::sendEvent(w, &move);
        const auto wheel = [&, over](int delta) {
            QWheelEvent event(over, w->mapToGlobal(over), {}, QPoint(0, delta), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(w, &event);
        };
        const double height = js(w, u"flick.contentHeight - flick.height"_s).toDouble();
        measure(name + u" (scrollable %1 px)"_s.arg(round1(height)), {}, 2 * notches * everyMs + 400,
            [&, notches, wheel](int i) {
                if (i < 2 * notches)
                    wheel(i < notches ? -120 : 120);
            }, everyMs);
    };

    // The 插件库's entries are made from 已安装 a moment after it shows, a few
    // at a time between frames (SettingsWindow, PluginGallerySection): how
    // long that held the GUI thread. --gallery-early: its tab shown while they
    // are being made, as a quick click does; all of them must be there then.
    const auto entriesMade = [&] {
        int made = 0;
        std::function<void(QQuickItem*)> count = [&](QQuickItem* item) {
            if (item->objectName().startsWith(u"installPlugin-"))
                ++made;
            for (QQuickItem* child : item->childItems())
                count(child);
        };
        count(w->contentItem());
        return made;
    };
    if (args.contains(u"--gallery-early"_s)) {
        pump(300);
        int atClick = -1;
        int afterClick = -1;
        measure(u"插件: 已安装 → 插件库 while its entries are being made"_s, [&] {
            atClick = entriesMade();
            tab(1)();
            afterClick = entriesMade();
        }, 700);
        const int listed = js(w, u"editor.plugins.gallery.count"_s).toInt();
        say(u"  entries made when the tab was clicked: %1, right after: %2, of %3 listed"_s.arg(atClick).arg(afterClick).arg(listed));
        report.insert(u"gallery_early"_s, QJsonObject {{u"at_click"_s, atClick}, {u"after_click"_s, afterClick}, {u"listed"_s, listed}});
        if (listed == 0 || afterClick != listed) {
            say(u"FAILED: the 插件库 shown without all its entries"_s);
            passed = false;
        }
        if (atClick == listed)
            say(u"  (all were made before the click: the early path was not tried)"_s);
        js(w, u"showTab(0)"_s);
    } else {
        rec.lastTick = rec.now();
        pump(std::max<qint64>(0, 1500 - (rec.now() - firstFrameAt) / 1'000'000));
        double stall = 0;
        for (const auto& [start, length] : rec.stalls)
            if (start >= firstFrameAt)
                stall = std::max(stall, ms(length));
        const int made = entriesMade();
        say(u"on 已安装 for 1.5 s after the window's first frame: worst GUI stall %1 ms; %2 entries of the 插件库 made meanwhile"_s
                .arg(round1(stall)).arg(made));
        report.insert(u"on_installed_gui_stall_max"_s, round1(stall));
        report.insert(u"gallery_made_beforehand"_s, made);
    }
    pump(300);

    // UI Automation: the settings window's tree, then every other window's root.
    const auto uia = [&](const QString& key) {
        const QJsonObject walked = timeUiaWalk(w);
        say(u"%1: UIA walk %2 ms, %3 visible elements, %4 child() calls, %5 under the window"_s.arg(key)
                .arg(walked.value(u"ms"_s).toDouble()).arg(walked.value(u"visible_elements"_s).toInt())
                .arg(walked.value(u"child_calls"_s).toInt()).arg(walked.value(u"root_children"_s).toInt()));
        report.insert(key, walked);
        if (walked.contains(u"error"_s) || walked.value(u"root_children"_s).toInt() > 20 || walked.value(u"ms"_s).toDouble() > 20) {
            say(u"FAILED: %1: the window's accessibility tree is flat or slow to walk"_s.arg(key));
            passed = false;
        }
    };
    uia(u"uia_plugins_page"_s);
    if (args.contains(u"--all-pages"_s)) {
        QMetaObject::invokeMethod(w, "loadAllPages");
        pump(500);
        js(w, u"showPage('plugins', 0)"_s);
        pump(300);
        uia(u"uia_all_pages_installed"_s);
        js(w, u"showTab(1)"_s);
        pump(1500);
        uia(u"uia_all_pages_gallery"_s);
        js(w, u"showTab(0)"_s);
        pump(300);
    }
    QJsonArray others;
    for (QWindow* other : QGuiApplication::allWindows()) {
        if (other == w || !qobject_cast<QQuickWindow*>(other))
            continue;
        QAccessibleInterface* root = QAccessible::queryAccessibleInterface(other);
        if (!root)
            continue;
        QElapsedTimer took;
        took.start();
        const int children = root->childCount();
        for (int i = 0; i < children; ++i)
            root->child(i);
        const double askedMs = round1(took.nsecsElapsed() / 1e6);
        const QString type = QString::fromLatin1(other->metaObject()->className()).section(u"_QMLTYPE_"_s, 0, 0);
        others.append(QJsonObject {{u"window"_s, type}, {u"title"_s, other->title()}, {u"visible"_s, other->isVisible()},
            {u"root_children"_s, children}, {u"all_children_ms"_s, askedMs}});
        say(u"other window %1 \"%2\"%3: %4 under the window, each asked for once in %5 ms"_s.arg(type, other->title(),
            other->isVisible() ? u""_s : u" (hidden)"_s).arg(children).arg(askedMs));
    }
    report.insert(u"uia_other_windows"_s, others);

    if (args.contains(u"--activate"_s)) {
        const HWND hwnd = reinterpret_cast<HWND>(w->winId());
        const DWORD frontThread = ::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr);
        const DWORD me = ::GetCurrentThreadId();
        ::AttachThreadInput(me, frontThread, TRUE);
        ::SetForegroundWindow(hwnd);
        ::BringWindowToTop(hwnd);
        ::AttachThreadInput(me, frontThread, FALSE);
        pump(500);
        report.insert(u"activated"_s, ::GetForegroundWindow() == hwnd);
        say(u"in front: %1"_s.arg(::GetForegroundWindow() == hwnd));
        measure(u"in front, idle on 已安装 (this PC's UIA clients look)"_s, {}, 3000);
        js(w, u"showTab(1)"_s);
        pump(1500);
        measure(u"in front, idle on 插件库 (this PC's UIA clients look)"_s, {}, 3000);
        js(w, u"showTab(0)"_s);
        pump(300);
    }

    measure(u"插件: 已安装 → 插件库 (first time)"_s, tab(1), 700);
    pump(2500); // the list fetched meanwhile, if it was
    report.insert(u"gallery_count"_s, js(w, u"editor.plugins.gallery.count"_s).toInt());
    for (int round = 0; round < 3; ++round) {
        measure(u"插件: 插件库 → 已安装"_s, tab(0), 500);
        measure(u"插件: 已安装 → 插件库"_s, tab(1), 500);
    }
    scroll(u"wheel on 插件库"_s, 12, 60);
    measure(u"插件: 插件库 → 已安装 (scrolled)"_s, tab(0), 500);
    scroll(u"wheel on 已安装"_s, 12, 60);

    // The other pages, each shown once (made then, if not yet), then again
    // with each of its tabs and the wheel. --plugins-only: not them.
    const QStringList ids = args.contains(u"--plugins-only"_s) ? QStringList() : js(w, u"pages.map(p => p.id)"_s).toStringList();
    for (const QString& id : ids) {
        if (id == u"plugins" || id == u"gleaning")
            continue;
        measure(u"page → %1 (first)"_s.arg(id), page(id, 0), 500);
    }
    for (const QString& id : ids) {
        if (id == u"gleaning")
            continue;
        measure(u"page → %1"_s.arg(id), page(id, 0), 500);
        const int tabs = js(w, u"pageTabs[currentPage].length"_s).toInt();
        for (int t = 1; t < tabs; ++t)
            measure(u"%1: tab %2 → %3"_s.arg(id).arg(t - 1).arg(t), tab(t), 500);
        if (tabs > 1)
            measure(u"%1: tab %2 → 0"_s.arg(id).arg(tabs - 1), tab(0), 500);
        scroll(u"wheel on %1"_s.arg(id), 8, 60);
    }

    // --other-windows: the search box with results, then the clipboard window,
    // each shown in front for a while: their trees walked as above, and the
    // GUI thread's longest stall while this PC's UIA clients look at them.
    if (args.contains(u"--other-windows"_s)) {
        w->close();
        pump(500);
        QJsonArray shown;
        const auto lookAt = [&](const QString& what) {
            const qint64 since = rec.now();
            rec.lastTick = since;
            pump(3000);
            double stall = 0;
            for (const auto& [start, length] : rec.stalls)
                if (start >= since)
                    stall = std::max(stall, ms(length));
            for (QWindow* other : QGuiApplication::allWindows()) {
                if (!other->isVisible() || !qobject_cast<QQuickWindow*>(other))
                    continue;
                QJsonObject walked = timeUiaWalk(other);
                const QString type = QString::fromLatin1(other->metaObject()->className()).section(u"_QMLTYPE_"_s, 0, 0);
                walked.insert(u"window"_s, type);
                walked.insert(u"shown_for"_s, what);
                walked.insert(u"in_front"_s, ::GetForegroundWindow() == reinterpret_cast<HWND>(other->winId()));
                walked.insert(u"gui_stall_max"_s, round1(stall));
                say(u"%1: window %2%3: UIA walk %4 ms, %5 visible elements, %6 child() calls, %7 under the window; worst GUI stall in 3 s %8 ms"_s
                        .arg(what, type, walked.value(u"in_front"_s).toBool() ? u" (in front)"_s : u""_s)
                        .arg(walked.value(u"ms"_s).toDouble()).arg(walked.value(u"visible_elements"_s).toInt())
                        .arg(walked.value(u"child_calls"_s).toInt()).arg(walked.value(u"root_children"_s).toInt()).arg(round1(stall)));
                shown.append(walked);
            }
        };
        app.showLauncher(u"win"_s);
        lookAt(u"search box, \"win\" typed"_s);
        app.toggleLauncher();
        pump(500);
        app.showClipboard();
        lookAt(u"clipboard"_s);
        app.toggleClipboard();
        pump(500);
        report.insert(u"uia_shown_windows"_s, shown);
    }

    tick.stop();
    report.insert(u"steps"_s, steps);
    report.insert(u"passed"_s, passed);
    QFile json(output.filePath(u"settings-motion.json"_s));
    const bool saved = json.open(QIODevice::WriteOnly) && json.write(QJsonDocument(report).toJson()) > 0;
    say(u"settings motion: %1"_s.arg(passed ? u"passed"_s : u"FAILED"_s));
    w->close();
    pump(300);
    return passed && saved;
}

} // namespace ws::preview
