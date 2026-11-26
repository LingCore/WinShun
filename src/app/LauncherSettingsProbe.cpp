#include "LauncherSettingsProbe.h"

#include "App.h"
#include "FilePreview.h"
#include "Launcher.h"
#include "PinnedStacking.h"
#include "Placement.h"
#include "TriggerJournal.h"
#include "WindowFrame.h"
#include "platform/WindowEffects.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include <dwmapi.h>
#include <windows.h>

using namespace Qt::StringLiterals;

namespace ws::preview {

struct LauncherSettingsProbe {
    static void pump(int ms)
    {
        QElapsedTimer timer;
        timer.start();
        do
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        while (timer.elapsed() < ms);
    }

    static bool windowShown(QWindow* window)
    {
        if (!window || !window->handle())
            return false;
        const auto hwnd = reinterpret_cast<HWND>(window->winId());
        if (!hwnd || !::IsWindowVisible(hwnd))
            return false;
        DWORD cloaked = 0;
        ::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
        return !cloaked;
    }

    struct Sample {
        double ms = 0;
        bool launcherShown = false;
        bool launcherActive = false;
        bool transparentBg = false;
        double opacity = 1;
        bool tipShown = false;
        bool settingsShown = false;
        bool settingsActive = false;
        bool sameState(const Sample& other) const
        {
            return launcherShown == other.launcherShown && launcherActive == other.launcherActive
                && transparentBg == other.transparentBg && opacity == other.opacity
                && tipShown == other.tipShown && settingsShown == other.settingsShown
                && settingsActive == other.settingsActive;
        }
    };

    static bool logoTipShown(QQuickWindow* launcher)
    {
        if (!launcher)
            return false;
        if (QObject* tip = launcher->findChild<QObject*>(u"searchLogoHint"_s))
            return tip->property("shown").toBool();
        return false;
    }

    static Sample sample(App& app, double ms)
    {
        QQuickWindow* launcher = app.m_window;
        QQuickWindow* settings = app.m_settingsWindow;
        return {
            std::round(ms * 10.0) / 10.0,
            windowShown(launcher),
            launcher && launcher->isActive(),
            launcher && launcher->color().alpha() == 0,
            launcher ? std::round(launcher->opacity() * 1000.0) / 1000.0 : 0.0,
            logoTipShown(launcher),
            windowShown(settings),
            settings && settings->isActive(),
        };
    }

    static bool openLauncherAndSettle(App& app, const QString& query)
    {
        app.showLauncher(query);
        QElapsedTimer limit;
        limit.start();
        while ((!app.m_launcherRevealedAt || app.m_placement->sliding()) && limit.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        pump(250);
        return app.m_window && windowShown(app.m_window) && app.m_window->opacity() == 1.0;
    }

    static QJsonObject traceAction(App& app, const char* name, bool expectSettings, bool animated,
        const std::function<void()>& trigger, bool& ok)
    {
        QQuickWindow* launcher = app.m_window;
        const bool initialActive = launcher && launcher->isActive();
        const bool initialTransparent = launcher && launcher->color().alpha() == 0;

        QElapsedTimer clock;
        clock.start();
        std::vector<Sample> timeline;
        timeline.push_back(sample(app, 0.0));

        trigger();
        const double callMs = std::round(clock.nsecsElapsed() / 100'000.0) / 10.0;
        {
            const Sample after = sample(app, callMs);
            if (!after.sameState(timeline.back()))
                timeline.push_back(after);
        }

        double launcherGoneMs = -1;
        double settingsShownMs = -1;
        while (clock.elapsed() < 2500) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            const double nowMs = clock.nsecsElapsed() / 1'000'000.0;
            const Sample s = sample(app, nowMs);
            if (!s.sameState(timeline.back()))
                timeline.push_back(s);
            if (launcherGoneMs < 0 && !s.launcherShown)
                launcherGoneMs = s.ms;
            if (settingsShownMs < 0 && s.settingsShown)
                settingsShownMs = s.ms;
            const bool done = launcherGoneMs >= 0 && (!expectSettings || settingsShownMs >= 0);
            if (done && nowMs - std::max(launcherGoneMs, settingsShownMs) > 200)
                break;
        }

        double firstDropMs = -1;
        double prevOpacityMs = 0;
        double maxGapMs = 0;
        int fadeSteps = 0;
        bool deactivatedWhileVisible = false;
        bool opaqueFlashWhileVisible = false;
        bool settingsOverlappedLauncher = false;
        bool tipLingered = false;

        for (std::size_t i = 1; i < timeline.size(); ++i) {
            const Sample& s = timeline[i];
            if (s.launcherShown && s.opacity > 0.0) {
                if (initialActive && !s.launcherActive)
                    deactivatedWhileVisible = true;
                if (initialTransparent && !s.transparentBg)
                    opaqueFlashWhileVisible = true;
                if (s.settingsShown)
                    settingsOverlappedLauncher = true;
                if (s.ms > 30.0 && s.tipShown)
                    tipLingered = true;
            }
            if (s.opacity < 1.0 && s.opacity > 0.0 && s.opacity != timeline[i - 1].opacity) {
                if (firstDropMs < 0)
                    firstDropMs = s.ms;
                ++fadeSteps;
                maxGapMs = std::max(maxGapMs, s.ms - prevOpacityMs);
                prevOpacityMs = s.ms;
            } else if (!s.launcherShown && timeline[i - 1].launcherShown) {
                maxGapMs = std::max(maxGapMs, s.ms - prevOpacityMs);
            }
        }
        maxGapMs = std::round(maxGapMs * 10.0) / 10.0;

        QJsonArray steps;
        for (const Sample& s : timeline) {
            steps.append(QJsonObject {
                {u"ms"_s, s.ms},
                {u"launcher_shown"_s, s.launcherShown},
                {u"launcher_active"_s, s.launcherActive},
                {u"transparent_bg"_s, s.transparentBg},
                {u"opacity"_s, s.opacity},
                {u"tip_shown"_s, s.tipShown},
                {u"settings_shown"_s, s.settingsShown},
                {u"settings_active"_s, s.settingsActive},
            });
        }

        const auto check = [&](bool condition, const char* what) {
            if (!condition) {
                ok = false;
                std::fprintf(stderr, "launcher->settings (%s): FAILED: %s\n", name, what);
            }
        };
        check(launcherGoneMs >= 0, "launcher never hid");
        if (expectSettings) {
            check(settingsShownMs >= 0, "settings never appeared");
            check(!deactivatedWhileVisible, "launcher deactivated while still visible");
            check(!opaqueFlashWhileVisible, "launcher Mica flashed to opaque background while fading");
            check(!settingsOverlappedLauncher, "settings appeared while launcher was still visible");
            check(!tipLingered, "logo hover tip lingered while launcher faded");
            if (animated) {
                check(callMs < 40.0, "click handler stalled GUI thread before fade started");
                check(firstDropMs >= 0 && firstDropMs < 45.0, "fade started too late");
                check(fadeSteps >= 4, "too few fade steps");
                check(maxGapMs < 50.0, "large frame gap during fade-out");
                check(launcherGoneMs >= 90.0 && launcherGoneMs <= 260.0, "launcher hide duration out of range");
            } else {
                check(launcherGoneMs < 40.0, "with animation off, launcher did not hide immediately");
            }
        }

        std::fprintf(stderr,
            "launcher->settings (%s): call=%.1fms firstDrop=%.1fms steps=%d maxGap=%.1fms gone=%.1fms settings=%.1fms "
            "deact=%d opaqueFlash=%d overlap=%d tipLinger=%d\n",
            name, callMs, firstDropMs, fadeSteps, maxGapMs, launcherGoneMs, settingsShownMs,
            int(deactivatedWhileVisible), int(opaqueFlashWhileVisible), int(settingsOverlappedLauncher), int(tipLingered));

        return {
            {u"name"_s, QString::fromLatin1(name)},
            {u"animated"_s, animated},
            {u"initial_active"_s, initialActive},
            {u"initial_transparent_bg"_s, initialTransparent},
            {u"call_ms"_s, callMs},
            {u"first_drop_ms"_s, firstDropMs},
            {u"fade_steps"_s, fadeSteps},
            {u"max_step_gap_ms"_s, maxGapMs},
            {u"launcher_gone_ms"_s, launcherGoneMs},
            {u"settings_shown_ms"_s, settingsShownMs},
            {u"deactivated_while_visible"_s, deactivatedWhileVisible},
            {u"opaque_flash_while_visible"_s, opaqueFlashWhileVisible},
            {u"settings_overlapped_launcher"_s, settingsOverlappedLauncher},
            {u"tip_lingered"_s, tipLingered},
            {u"timeline"_s, steps},
        };
    }

    static void hoverLogo(App& app)
    {
        QQuickItem* logo = app.m_window->findChild<QQuickItem*>(u"searchLogo"_s);
        QObject* tip = app.m_window->findChild<QObject*>(u"searchLogoHint"_s);
        if (logo && tip) {
            tip->setProperty("anchor", QVariant::fromValue(logo));
            tip->setProperty("shownText", u"logo"_s);
            tip->setProperty("shown", true);
            pump(50);
        }
    }

    static bool run(App& app, const QDir& output)
    {
        pump(1500); // let prewarm finish
        app.m_settings.openAnimation = true;
        app.m_placement->setAnimated(true);

        bool ok = true;
        QJsonArray rounds;

        // Round 0: baseline Esc / hideLauncher()
        if (openLauncherAndSettle(app, u"win"_s)) {
            rounds.append(traceAction(app, "baseline-hide", false, true, [&] { app.hideLauncher(); }, ok));
        } else {
            ok = false;
            std::fprintf(stderr, "launcher->settings: FAILED: could not open launcher for baseline\n");
        }
        pump(300);

        // Round 1: first open of Settings via logo handle click (with logo hover tip shown)
        if (openLauncherAndSettle(app, u"win"_s)) {
            hoverLogo(app);
            rounds.append(traceAction(app, "logo-first-open", true, true, [&] {
                emit app.m_frame->handleClicked(Qt::LeftButton);
            }, ok));
            if (app.m_settingsWindow) {
                app.m_settingsWindow->close();
                pump(300);
            }
        } else {
            ok = false;
        }

        // Round 2: second open of Settings via logo handle click (reused SettingsWindow)
        if (openLauncherAndSettle(app, u"win"_s)) {
            hoverLogo(app);
            rounds.append(traceAction(app, "logo-second-open", true, true, [&] {
                emit app.m_frame->handleClicked(Qt::LeftButton);
            }, ok));
            if (app.m_settingsWindow) {
                app.m_settingsWindow->close();
                pump(300);
            }
        } else {
            ok = false;
        }

        // Round 3: openAnimation = false
        app.m_settings.openAnimation = false;
        app.m_placement->setAnimated(false);
        if (openLauncherAndSettle(app, u"win"_s)) {
            rounds.append(traceAction(app, "logo-animation-off", true, false, [&] {
                emit app.m_frame->handleClicked(Qt::LeftButton);
            }, ok));
            if (app.m_settingsWindow) {
                app.m_settingsWindow->close();
                pump(300);
            }
        } else {
            ok = false;
        }

        // Round 4: reopening launcher mid-fade cancels pending Settings
        app.m_settings.openAnimation = true;
        app.m_placement->setAnimated(true);
        if (openLauncherAndSettle(app, u"win"_s)) {
            emit app.m_frame->handleClicked(Qt::LeftButton);
            pump(30);
            app.showLauncher();
            pump(400);
            const bool launcherStillUp = windowShown(app.m_window);
            const bool settingsStayedHidden = !app.m_settingsWindow || !app.m_settingsWindow->isVisible();
            if (!launcherStillUp || !settingsStayedHidden) {
                ok = false;
                std::fprintf(stderr, "launcher->settings (cancel-mid-fade): FAILED: launcherUp=%d settingsHidden=%d\n",
                    int(launcherStillUp), int(settingsStayedHidden));
            } else {
                std::fprintf(stderr, "launcher->settings (cancel-mid-fade): passed\n");
            }
            rounds.append(QJsonObject {
                {u"name"_s, u"cancel-mid-fade"_s},
                {u"launcher_still_up"_s, launcherStillUp},
                {u"settings_stayed_hidden"_s, settingsStayedHidden},
            });
            app.hideLauncher();
            pump(300);
        } else {
            ok = false;
        }

        QFile file(output.filePath(u"launcher-settings.json"_s));
        const bool saved = file.open(QIODevice::WriteOnly)
            && file.write(QJsonDocument(QJsonObject {{u"passed"_s, ok}, {u"rounds"_s, rounds}}).toJson()) > 0;
        std::fprintf(stderr, "launcher->settings: %s\n", ok ? "passed" : "FAILED");
        return ok && saved;
    }

    struct OpenSample {
        double ms = 0;
        bool shown = false;
        bool active = false;
        bool transparentBg = false;
        bool sliding = false;
        double opacity = 1;
        QRect rect;
        double shiftY = 0;
        bool sameState(const OpenSample& o) const
        {
            return shown == o.shown && active == o.active && transparentBg == o.transparentBg
                && sliding == o.sliding && opacity == o.opacity && rect == o.rect && shiftY == o.shiftY;
        }
    };

    static OpenSample openSample(App& app, double ms)
    {
        QQuickWindow* w = app.m_window;
        RECT r {};
        if (w && w->handle())
            ::GetWindowRect(reinterpret_cast<HWND>(w->winId()), &r);
        return {
            std::round(ms * 10.0) / 10.0,
            windowShown(w),
            w && w->isActive(),
            w && w->color().alpha() == 0,
            app.m_placement && app.m_placement->sliding(),
            w ? std::round(w->opacity() * 1000.0) / 1000.0 : 0.0,
            QRect(r.left, r.top, r.right - r.left, r.bottom - r.top),
            app.m_placement ? std::round(app.m_placement->contentShift().y() * 100.0) / 100.0 : 0.0,
        };
    }

    static QJsonObject openRound(App& app, const char* name, bool& ok, bool asPanel = false)
    {
        QQuickWindow* w = app.m_window;
        QElapsedTimer clock;
        clock.start();

        QMutex lock;
        struct FrameInfo { double swapMs; double syncMs; double renderMs; };
        std::vector<FrameInfo> frames;
        qint64 syncStart = -1;
        double lastSyncMs = 0;
        qint64 renderStart = -1;
        double lastRenderMs = 0;

        const auto c1 = QObject::connect(w, &QQuickWindow::beforeSynchronizing, w, [&] {
            syncStart = clock.nsecsElapsed();
        }, Qt::DirectConnection);
        const auto c2 = QObject::connect(w, &QQuickWindow::afterSynchronizing, w, [&] {
            if (syncStart >= 0)
                lastSyncMs = (clock.nsecsElapsed() - syncStart) / 1'000'000.0;
        }, Qt::DirectConnection);
        const auto c3 = QObject::connect(w, &QQuickWindow::beforeRendering, w, [&] {
            renderStart = clock.nsecsElapsed();
        }, Qt::DirectConnection);
        const auto c4 = QObject::connect(w, &QQuickWindow::afterRendering, w, [&] {
            if (renderStart >= 0)
                lastRenderMs = (clock.nsecsElapsed() - renderStart) / 1'000'000.0;
        }, Qt::DirectConnection);
        const auto c5 = QObject::connect(w, &QQuickWindow::frameSwapped, w, [&] {
            const double now = clock.nsecsElapsed() / 1'000'000.0;
            QMutexLocker g(&lock);
            frames.push_back({std::round(now * 10.0) / 10.0, std::round(lastSyncMs * 100.0) / 100.0, std::round(lastRenderMs * 100.0) / 100.0});
        }, Qt::DirectConnection);

        std::vector<OpenSample> timeline;
        std::vector<std::pair<double, double>> guiGaps; // [startMs, gapMs]
        timeline.push_back(openSample(app, 0.0));

        const std::uint64_t pressedAt = ::GetTickCount64();
        app.beginTrigger(TriggerSource::DoubleTap, pressedAt);
        if (asPanel)
            app.panelHotkey(pressedAt);
        else
            app.toggleLauncher(true, pressedAt);
        const double callMs = std::round(clock.nsecsElapsed() / 100'000.0) / 10.0;
        {
            const OpenSample after = openSample(app, callMs);
            if (!after.sameState(timeline.back()))
                timeline.push_back(after);
        }

        double revealedMs = -1;
        double settledMs = -1;
        double prevLoopMs = callMs;
        while (clock.elapsed() < 1500) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            const double nowMs = clock.nsecsElapsed() / 1'000'000.0;
            const double loopGap = nowMs - prevLoopMs;
            if (revealedMs >= 0 && settledMs < 0 && loopGap > 8.0)
                guiGaps.push_back({std::round(prevLoopMs * 10.0) / 10.0, std::round(loopGap * 10.0) / 10.0});
            prevLoopMs = nowMs;

            const OpenSample s = openSample(app, nowMs);
            if (!s.sameState(timeline.back()))
                timeline.push_back(s);
            if (revealedMs < 0 && s.shown)
                revealedMs = s.ms;
            if (revealedMs >= 0 && settledMs < 0 && s.shown && !s.sliding && s.opacity == 1.0 && s.shiftY == 0.0)
                settledMs = s.ms;
            if (settledMs >= 0 && nowMs - settledMs > 150.0)
                break;
        }

        QObject::disconnect(c1);
        QObject::disconnect(c2);
        QObject::disconnect(c3);
        QObject::disconnect(c4);
        QObject::disconnect(c5);

        const OpenSample rest = timeline.back();
        int opacitySteps = 0;
        int moveSteps = 0;
        int shiftSteps = 0;
        int postOpaqueMoves = 0;
        int sizeChangesAfterReveal = 0;
        bool inactiveAfterReveal = false;
        bool opaqueAfterReveal = false;
        double maxAnimGapMs = 0;
        double maxMoveGapMs = 0;
        double prevAnimMs = revealedMs >= 0 ? revealedMs : 0;
        double prevMoveMs = revealedMs >= 0 ? revealedMs : 0;
        double firstShownOpacity = -1;
        int firstShownDy = 0;
        double firstShownShiftY = 0;
        QSize revealedSize;

        for (std::size_t i = 1; i < timeline.size(); ++i) {
            const OpenSample& prev = timeline[i - 1];
            const OpenSample& s = timeline[i];
            if (!s.shown)
                continue;
            if (firstShownOpacity < 0) {
                firstShownOpacity = s.opacity;
                firstShownDy = s.rect.y() - rest.rect.y();
                firstShownShiftY = s.shiftY;
                revealedSize = s.rect.size();
            } else {
                if (s.rect.size() != prev.rect.size() && prev.shown)
                    ++sizeChangesAfterReveal;
                if (s.opacity != prev.opacity) {
                    ++opacitySteps;
                    if (s.sliding || prev.sliding) {
                        maxAnimGapMs = std::max(maxAnimGapMs, s.ms - prevAnimMs);
                        prevAnimMs = s.ms;
                    }
                }
                if (s.rect.y() != prev.rect.y() && s.rect.size() == prev.rect.size()) {
                    ++moveSteps;
                    maxMoveGapMs = std::max(maxMoveGapMs, s.ms - prevMoveMs);
                    prevMoveMs = s.ms;
                    if (prev.opacity >= 1.0 && s.opacity >= 1.0)
                        ++postOpaqueMoves;
                }
                if (s.shiftY != prev.shiftY)
                    ++shiftSteps;
            }
            if (!s.active)
                inactiveAfterReveal = true;
            if (!s.transparentBg)
                opaqueAfterReveal = true;
        }
        maxAnimGapMs = std::round(maxAnimGapMs * 10.0) / 10.0;
        maxMoveGapMs = std::round(maxMoveGapMs * 10.0) / 10.0;

        QJsonArray stepsJson;
        for (const OpenSample& s : timeline) {
            stepsJson.append(QJsonObject {
                {u"ms"_s, s.ms}, {u"shown"_s, s.shown}, {u"active"_s, s.active},
                {u"transparent_bg"_s, s.transparentBg}, {u"sliding"_s, s.sliding},
                {u"opacity"_s, s.opacity}, {u"dy"_s, s.rect.y() - rest.rect.y()},
                {u"w"_s, s.rect.width()}, {u"h"_s, s.rect.height()}, {u"shift_y"_s, s.shiftY},
            });
        }
        QJsonArray framesJson;
        double maxSyncMs = 0, maxRenderMs = 0, maxSwapGapDuringAnimMs = 0;
        double prevSwapMs = -1;
        int swapsDuringAnim = 0;
        int postSettleSwaps = 0;
        {
            QMutexLocker g(&lock);
            for (const FrameInfo& f : frames) {
                framesJson.append(QJsonObject {{u"swap_ms"_s, f.swapMs}, {u"sync_ms"_s, f.syncMs}, {u"render_ms"_s, f.renderMs}});
                if (revealedMs >= 0 && f.swapMs >= revealedMs && (settledMs < 0 || f.swapMs <= settledMs + 10.0)) {
                    ++swapsDuringAnim;
                    maxSyncMs = std::max(maxSyncMs, f.syncMs);
                    maxRenderMs = std::max(maxRenderMs, f.renderMs);
                    if (prevSwapMs >= 0)
                        maxSwapGapDuringAnimMs = std::max(maxSwapGapDuringAnimMs, f.swapMs - prevSwapMs);
                    prevSwapMs = f.swapMs;
                } else if (settledMs >= 0 && f.swapMs > settledMs + 10.0) {
                    ++postSettleSwaps;
                }
            }
        }
        QJsonArray gapsJson;
        double maxGuiGapMs = 0;
        for (const auto& [at, gap] : guiGaps) {
            gapsJson.append(QJsonObject {{u"at_ms"_s, at}, {u"gap_ms"_s, gap}});
            maxGuiGapMs = std::max(maxGuiGapMs, gap);
        }

        const bool fg = w && ::GetForegroundWindow() == reinterpret_cast<HWND>(w->winId());
        const bool busy = app.m_launcher && app.m_launcher->busy();
        const double animMs = revealedMs >= 0 && settledMs >= 0 ? std::round((settledMs - revealedMs) * 10.0) / 10.0 : -1.0;
        std::fprintf(stderr,
            "launcher-open (%s): h=%d busy=%d fg=%d call=%.1fms reveal=%.1fms anim=%.1fms first(op=%.2f dy=%d shift=%.1f) "
            "steps(op=%d move=%d shift=%d postOpMove=%d) maxOpGap=%.1fms maxMoveGap=%.1fms maxGuiGap=%.1fms "
            "swaps=%d postSwaps=%d maxSync=%.2fms maxRender=%.2fms resizes=%d inact=%d opaque=%d\n",
            name, rest.rect.height(), int(busy), int(fg), callMs, revealedMs, animMs, firstShownOpacity, firstShownDy, firstShownShiftY,
            opacitySteps, moveSteps, shiftSteps, postOpaqueMoves, maxAnimGapMs, maxMoveGapMs, maxGuiGapMs,
            swapsDuringAnim, postSettleSwaps, maxSyncMs, maxRenderMs,
            sizeChangesAfterReveal, int(inactiveAfterReveal), int(opaqueAfterReveal));

        const auto check = [&](bool cond, const char* what) {
            if (!cond) {
                ok = false;
                std::fprintf(stderr, "launcher-open (%s): FAILED: %s\n", name, what);
            }
        };
        check(revealedMs >= 0 && settledMs >= 0, "never revealed or settled");
        check(animMs >= 100.0 && animMs <= 220.0, "animation duration outside [100, 220] ms");
        check(firstShownOpacity >= 0.0 && firstShownOpacity < 0.6, "first shown frame not faded");
        check(firstShownDy > 0, "first shown frame not offset below rest");
        check(shiftSteps == 0 && firstShownShiftY == 0.0, "contentShift animated separately from window");
        check(postOpaqueMoves == 0, "window moved after reaching opacity 1.0");
        check(sizeChangesAfterReveal == 0, "window resized after reveal during slide-in");
        check(!opaqueAfterReveal, "window background was opaque after reveal");

        return {
            {u"name"_s, QString::fromLatin1(name)},
            {u"busy"_s, busy},
            {u"call_ms"_s, callMs},
            {u"revealed_ms"_s, revealedMs},
            {u"settled_ms"_s, settledMs},
            {u"anim_ms"_s, animMs},
            {u"first_opacity"_s, firstShownOpacity},
            {u"first_dy"_s, firstShownDy},
            {u"first_shift_y"_s, firstShownShiftY},
            {u"opacity_steps"_s, opacitySteps},
            {u"move_steps"_s, moveSteps},
            {u"shift_steps"_s, shiftSteps},
            {u"post_opaque_moves"_s, postOpaqueMoves},
            {u"max_opacity_gap_ms"_s, maxAnimGapMs},
            {u"max_move_gap_ms"_s, maxMoveGapMs},
            {u"max_gui_gap_ms"_s, maxGuiGapMs},
            {u"swaps_during_anim"_s, swapsDuringAnim},
            {u"post_settle_swaps"_s, postSettleSwaps},
            {u"max_swap_gap_ms"_s, std::round(maxSwapGapDuringAnimMs * 10.0) / 10.0},
            {u"max_sync_ms"_s, maxSyncMs},
            {u"max_render_ms"_s, maxRenderMs},
            {u"size_changes_after_reveal"_s, sizeChangesAfterReveal},
            {u"inactive_after_reveal"_s, inactiveAfterReveal},
            {u"opaque_after_reveal"_s, opaqueAfterReveal},
            {u"gui_gaps"_s, gapsJson},
            {u"frames"_s, framesJson},
            {u"timeline"_s, stepsJson},
        };
    }

    static HWND makeForegroundBacking()
    {
        static const wchar_t* const kClass = L"WinShun.OpenProbeBacking";
        static const bool reg = [] {
            WNDCLASSEXW c {sizeof c};
            c.lpfnWndProc = ::DefWindowProcW;
            c.hInstance = ::GetModuleHandleW(nullptr);
            c.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            c.lpszClassName = kClass;
            return ::RegisterClassExW(&c) != 0;
        }();
        if (!reg)
            return nullptr;
        const HWND hwnd = ::CreateWindowExW(0, kClass, L"Probe Backing", WS_OVERLAPPEDWINDOW,
            120, 120, 640, 420, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (hwnd) {
            ::ShowWindow(hwnd, SW_SHOW);
            ::SetForegroundWindow(hwnd);
        }
        return hwnd;
    }

    static bool runOpen(App& app, const QDir& output)
    {
        pump(1500); // let prewarm finish
        {
            QElapsedTimer wait;
            wait.start();
            while (app.m_launcher && app.m_launcher->busy() && wait.elapsed() < 10000)
                pump(50);
        }
        app.m_settings.openAnimation = true;
        app.m_placement->setAnimated(true);

        const HWND backing = makeForegroundBacking();
        const auto activateBacking = [&] {
            if (backing) {
                ::SetForegroundWindow(backing);
                pump(150);
            }
        };

        bool ok = true;
        QJsonArray rounds;

        // 1. Cold first double-Ctrl (empty query) from another foreground window
        activateBacking();
        rounds.append(openRound(app, "cold-empty", ok));
        app.hideLauncher();
        pump(350);

        // 2. Warm second double-Ctrl (empty query)
        activateBacking();
        rounds.append(openRound(app, "warm-empty", ok));
        app.hideLauncher();
        pump(350);

        // 3. Prime with "report" (8 real file rows + icons, ~850 px tall at 150% DPI), close, then double-Ctrl!
        app.m_settings.clearOnOpen = false;
        app.showLauncher(u"report"_s);
        pump(600);
        app.hideLauncher();
        pump(350);
        activateBacking();
        rounds.append(openRound(app, "warm-8rows-report", ok));
        app.hideLauncher();
        pump(350);

        // 4. Prime with "notes 0000" (1 row + hints), close, then double-Ctrl!
        app.showLauncher(u"notes 0000"_s);
        pump(600);
        app.hideLauncher();
        pump(350);
        activateBacking();
        rounds.append(openRound(app, "warm-1row-notes", ok));
        app.hideLauncher();
        pump(350);

        // 5. Prime with 8 rows ("report"), enable clearOnOpen = true, then double-Ctrl (height shrinks from 8 rows -> 0!)
        app.showLauncher(u"report"_s);
        pump(600);
        app.hideLauncher();
        pump(350);
        app.m_settings.clearOnOpen = true;
        activateBacking();
        rounds.append(openRound(app, "clear-8rows-to-0", ok));
        app.hideLauncher();
        pump(350);

        // 6. Prime with 8 rows ("report"), then simulate IdleTrim (EmptyWorkingSet) before double-Ctrl!
        app.m_settings.clearOnOpen = false;
        app.showLauncher(u"report"_s);
        pump(600);
        app.hideLauncher();
        pump(350);
        ::SetProcessWorkingSetSizeEx(::GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);
        pump(200);
        activateBacking();
        rounds.append(openRound(app, "after-trim-8rows", ok));
        app.hideLauncher();
        pump(350);

        // 7. Prime in Files scope (TypeStrip open) with "report", enable clearOnOpen = true, then double-Ctrl!
        app.m_settings.clearOnOpen = false;
        app.m_launcher->setScope(Launcher::Files);
        app.showLauncher(u"report"_s);
        pump(600);
        app.hideLauncher();
        pump(350);
        app.m_settings.clearOnOpen = true;
        activateBacking();
        rounds.append(openRound(app, "clear-files-scope-to-all", ok));
        app.hideLauncher();
        pump(350);

        // 8. Direct Search Panel open (panelHotkey while hidden: no double-fade, clean slideIn)
        activateBacking();
        rounds.append(openRound(app, "direct-panel-open", ok, true));
        // Collapse panel -> box and re-expand box -> panel (cloaked resize both ways)
        app.togglePanel();
        pump(250);
        const bool collapsedShown = windowShown(app.m_window) && !app.m_launcher->panel();
        app.togglePanel();
        pump(250);
        const bool expandedShown = windowShown(app.m_window) && app.m_launcher->panel();
        if (!collapsedShown || !expandedShown) {
            ok = false;
            std::fprintf(stderr, "launcher-open (panel-toggle): FAILED: collapsed=%d expanded=%d\n",
                int(collapsedShown), int(expandedShown));
        } else {
            std::fprintf(stderr, "launcher-open (panel-toggle): passed (collapsed=%d expanded=%d)\n",
                int(collapsedShown), int(expandedShown));
        }
        app.hideLauncher();
        pump(350);

        if (backing)
            ::DestroyWindow(backing);

        QFile file(output.filePath(u"launcher-open.json"_s));
        const bool saved = file.open(QIODevice::WriteOnly)
            && file.write(QJsonDocument(QJsonObject {{u"passed"_s, ok}, {u"rounds"_s, rounds}}).toJson()) > 0;
        std::fprintf(stderr, "launcher-open: %s\n", ok ? "passed" : "FAILED");
        return ok && saved;
    }

    // `upper` over `lower` among the windows of the desktop.
    static bool isAbove(HWND upper, HWND lower)
    {
        for (HWND next = ::GetWindow(upper, GW_HWNDNEXT); next; next = ::GetWindow(next, GW_HWNDNEXT)) {
            if (next == lower)
                return true;
        }
        return false;
    }

    // Pinned and kept over the other windows, the launcher lets what was
    // opened from it in front (PinnedStacking), its preview with it; over
    // them again once it is in front itself. The "document" is a window of
    // this probe's, laid over the launcher: nothing of the user's is opened.
    static bool runPinned(App& app, const QDir& output)
    {
        pump(1500); // let prewarm finish
        QElapsedTimer wait;
        wait.start();
        while (app.m_launcher && app.m_launcher->busy() && wait.elapsed() < 10000)
            pump(50);

        bool ok = true;
        QJsonArray steps;
        const auto check = [&](const char* what, bool condition) {
            steps.append(QJsonObject {{u"what"_s, QString::fromUtf8(what)}, {u"ok"_s, condition}});
            if (!condition) {
                ok = false;
                std::fprintf(stderr, "pinned-stacking: FAILED: %s\n", what);
            }
        };

        app.m_settings.pinnedOnTop = true;
        app.showLauncher();
        pump(600);
        app.m_launcher->setPinned(true);
        pump(100);
        const HWND launcher = reinterpret_cast<HWND>(app.m_window->winId());
        check("pinned: shown, in front and over the other windows",
            windowShown(app.m_window) && ::GetForegroundWindow() == launcher && win::isOnTop(app.m_window));

        // Its preview, with a text of its own (no file read, no other program).
        FilePreview::Item item;
        item.path = u"winshun-plugin:pinned-stacking-probe"_s;
        item.name = u"pinned stacking"_s;
        item.text = std::make_shared<const QString>(u"What is opened from a pinned box is let in front.\n"_s.repeated(12));
        app.m_preview->show(item);
        pump(500);
        QWindow* preview = nullptr;
        for (QWindow* window : QGuiApplication::topLevelWindows()) {
            if (window != app.m_window && window->isVisible()
                && window->property("preview").value<FilePreview*>() == app.m_preview.get())
                preview = window;
        }
        check("the preview is up", preview != nullptr);
        const HWND previewHwnd = preview ? reinterpret_cast<HWND>(preview->winId()) : nullptr;
        check("the preview is over the other windows with it", preview && win::isOnTop(preview));

        // The "document": over the launcher and the preview, where one opened would be.
        const HWND document = makeForegroundBacking();
        RECT over {};
        ::GetWindowRect(launcher, &over);
        if (previewHwnd) {
            RECT beside {};
            ::GetWindowRect(previewHwnd, &beside);
            ::UnionRect(&over, &over, &beside);
        }
        ::SetWindowPos(document, nullptr, over.left - 40, over.top - 40, over.right - over.left + 80, over.bottom - over.top + 80,
            SWP_NOZORDER | SWP_NOACTIVATE);
        const auto documentToFront = [&] {
            ::SetForegroundWindow(document);
            pump(300);
            return ::GetForegroundWindow() == document;
        };
        const auto launcherToFront = [&] {
            app.showLauncher(); // its shortcut: pinned and up, to the front where it stands
            pump(400);
            return ::GetForegroundWindow() == launcher;
        };

        // A click elsewhere, nothing opened: it stays over that window, as set.
        check("a window clicked took the front", documentToFront());
        check("kept on top: over the window clicked",
            windowShown(app.m_window) && win::isOnTop(app.m_window) && isAbove(launcher, document));
        check("its preview too", previewHwnd && isAbove(previewHwnd, document));

        // Something opened from it (Launcher::dismissRequested is all the App hears of that).
        check("back in front by its shortcut", launcherToFront());
        emit app.m_launcher->dismissRequested();
        pump(50);
        check("something opened: it stays, no longer over the other windows",
            windowShown(app.m_window) && app.m_launcher->pinned() && !win::isOnTop(app.m_window));
        check("in front still, until what it opened comes", ::GetForegroundWindow() == launcher);
        check("the preview stands back with it", previewHwnd && windowShown(preview) && !win::isOnTop(preview));
        check("what was opened took the front", documentToFront());
        check("what was opened is over the launcher", isAbove(document, launcher));
        check("and over its preview", previewHwnd && isAbove(document, previewHwnd));
        check("still up behind it, for the next result",
            windowShown(app.m_window) && ::IsWindowVisible(launcher) && !::IsIconic(launcher));

        // Back to it (its shortcut, as a click on it or its taskbar button): over the others again.
        check("back in front again", launcherToFront());
        check("in front: over the other windows again", win::isOnTop(app.m_window) && isAbove(launcher, document));
        check("its preview beside it again", previewHwnd && win::isOnTop(preview) && isAbove(previewHwnd, document));
        check("a click elsewhere then leaves it over that window", documentToFront() && isAbove(launcher, document));

        // Opened, and nothing came to the front for it: long after, a click
        // elsewhere is only that.
        check("in front once more", launcherToFront());
        emit app.m_launcher->dismissRequested();
        pump(50);
        app.m_pinnedStacking.opened(PinnedStacking::Clock::now() - PinnedStacking::kStandsBackFor - std::chrono::seconds(1));
        check("a click elsewhere long after", documentToFront());
        check("long after what it opened: over the window clicked", win::isOnTop(app.m_window) && isAbove(launcher, document));

        // Not kept on top (the setting off): among the other windows throughout.
        app.m_settings.pinnedOnTop = false;
        check("in front, the setting off", launcherToFront());
        check("not kept on top: not over the other windows", !win::isOnTop(app.m_window) && (!preview || !win::isOnTop(preview)));
        emit app.m_launcher->dismissRequested();
        pump(50);
        check("opened: covered by it",
            documentToFront() && isAbove(document, launcher) && (!previewHwnd || isAbove(document, previewHwnd)));
        check("in front again: still among the others", launcherToFront() && !win::isOnTop(app.m_window));

        // Unpinned: a menu again, over every window.
        app.m_settings.pinnedOnTop = true;
        emit app.m_launcher->dismissRequested();
        pump(50);
        app.m_launcher->setPinned(false);
        pump(100);
        check("unpinned while standing back: over every window", win::isOnTop(app.m_window));
        app.m_preview->close();
        app.hideLauncher();
        pump(350);

        // 一直钉住搜索框 (Settings::keepPinned): pinned as it is turned on,
        // the box hidden too; Ctrl+T unpins it for this run, whatever else
        // is changed in the settings; turned off, unpinned.
        check("not pinned to begin with", !app.m_launcher->pinned());
        app.m_settings.keepPinned = true;
        app.applySettings(false);
        check("kept pinned: pinned while hidden", app.m_launcher->pinned() && !windowShown(app.m_window));
        app.showLauncher();
        pump(600);
        check("opens pinned, over the other windows",
            windowShown(app.m_window) && app.m_launcher->pinned() && win::isOnTop(app.m_window));
        emit app.m_launcher->dismissRequested();
        pump(50);
        check("a result opened: it stays up", windowShown(app.m_window));
        check("and what was opened comes in front", documentToFront() && isAbove(document, launcher));
        check("back in front for the next", launcherToFront());
        app.m_launcher->togglePin(); // Ctrl+T
        app.applySettings(false); // another setting changed meanwhile
        check("Ctrl+T unpins it for this run", !app.m_launcher->pinned());
        app.m_launcher->togglePin();
        app.m_settings.keepPinned = false;
        app.applySettings(false);
        check("turned off: unpinned", !app.m_launcher->pinned());
        app.hideLauncher();
        pump(350);
        if (document)
            ::DestroyWindow(document);

        QFile file(output.filePath(u"pinned-stacking.json"_s));
        const bool saved = file.open(QIODevice::WriteOnly)
            && file.write(QJsonDocument(QJsonObject {{u"passed"_s, ok}, {u"steps"_s, steps}}).toJson()) > 0;
        std::fprintf(stderr, "pinned-stacking: %s (%d checks)\n", ok ? "passed" : "FAILED", int(steps.size()));
        return ok && saved;
    }
};

bool checkPinnedStacking(App& app, const QStringList&, const QDir& output)
{
    return LauncherSettingsProbe::runPinned(app, output);
}

bool checkLauncherToSettings(App& app, const QStringList&, const QDir& output)
{
    return LauncherSettingsProbe::run(app, output);
}

bool checkLauncherOpen(App& app, const QStringList&, const QDir& output)
{
    return LauncherSettingsProbe::runOpen(app, output);
}

} // namespace ws::preview
