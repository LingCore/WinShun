#pragma once

#include "ScrollInput.h"
#include "ScrollStitcher.h"

#include <QtGlobal>

namespace ws::capture {

enum class ScrollSpeed {
    Slow = 0,
    Normal = 1,
    Fast = 2,
};

// Pure, deterministic closed-loop controller for continuous long-screenshot
// auto-scrolling. Drives smooth rhythmic wheel/UIA ticks while frames stream
// in, adapts cadence to measured scroll speed and stitcher backpressure,
// probes fallback methods quickly, verifies end-of-page without long stalls,
// and recovers from transient lost frames before yielding to manual control.
class ScrollAutoEngine {
public:
    enum class Phase {
        Idle,
        Probing,
        Streaming,
        VerifyingEnd,
        Recovering,
        Finished,
    };

    enum class CommandKind {
        WaitFrame,
        SendScroll,
        SwitchSideways,
        FinishAtEnd,
        YieldToManual,
    };

    struct Command {
        CommandKind kind = CommandKind::WaitFrame;
        int timeoutMs = 40;
        ScrollMethod method = ScrollMethod::Manual;
        double amount = 1.0;
        int direction = 1; // +1 down/right, -1 up/left
        bool allowParkCursor = false;
        ScrollLadder::Hint hint = ScrollLadder::Hint::None;
        bool lost = false;
    };

    struct Config {
        bool elevated = false;
        bool prefersSendWheel = false;
        bool pageScrollWheel = false;
        bool horizontal = false;
        ScrollSpeed speed = ScrollSpeed::Normal;
    };

    explicit ScrollAutoEngine(Config config = {});

    void reconfigure(Config config, bool resetTriedSideways = false);
    void setSpeed(ScrollSpeed speed) { m_config.speed = speed; }
    ScrollSpeed speed() const { return m_config.speed; }

    // Begin or resume auto-scrolling in `direction` (+1 down/right, -1 up/left).
    void begin(qint64 nowMs, int direction = 1);
    // Pause auto-scrolling (user toggled switch off).
    void stop();

    // Feed runtime observations from the capture loop.
    void onScrollSent(qint64 nowMs, const ScrollResult& result, double amount, int zonePx);
    void onFrame(qint64 nowMs, const ScrollStitcher::Step& step, bool extended, int zonePx);
    // User moved the mouse away from our parked cursor while SendWheel was active.
    void onUserMovedCursor(qint64 nowMs, bool stillInsideRegion);

    // Decide the next action for the capture loop at `nowMs`.
    Command next(qint64 nowMs, int zonePx, bool extended);

    Phase phase() const { return m_phase; }
    bool active() const { return m_phase != Phase::Idle && m_phase != Phase::Finished; }
    bool canAutoScroll() const { return m_ladder.method() != ScrollMethod::Manual || m_phase == Phase::Finished; }
    ScrollLadder::Hint hint() const { return m_ladder.hint(); }
    int direction() const { return m_direction; }
    // Expected displacement for the next frame, if useful as a tie-breaker.
    int expectedStepPx() const;
    // True once a non-SendWheel method has sustained several ticks (so end-of-page
    // probing does not warp the user's cursor unnecessarily).
    bool sustainedWithoutCursor() const { return m_otherMethodTicks >= 3; }

private:
    double targetVelocity(int zonePx) const;
    int targetWheelTickMs() const;
    void computeTick(int zonePx, ScrollMethod method, double& amount, int& intervalMs) const;
    bool canSwitchSideways(bool extended) const;
    bool canParkCursorFor(ScrollMethod method) const;
    void resetFlowTiming(qint64 nowMs);

    Config m_config;
    ScrollLadder m_ladder;
    Phase m_phase = Phase::Idle;
    int m_direction = 1;
    bool m_triedSideways = false;
    bool m_userYieldedCursor = false;

    // Pacing & calibration
    double m_pxPerUnit = 0.0;
    double m_windowAmount = 0.0;
    double m_windowMovedPx = 0.0;
    double m_inFlightPx = 0.0;
    double m_backoffScale = 1.0;
    double m_lastSentAmount = 1.0;
    bool m_fractionalWorked = false;
    bool m_fractionalUnsupported = false;
    int m_unackedTicks = 0;
    int m_otherMethodTicks = 0; // successful ticks by PostWheel or Uia
    int m_reverseFrames = 0;
    int m_probeLostFrames = 0;
    qint64 m_lastMovedMs = 0;
    qint64 m_nextTickAtMs = 0;
    qint64 m_waitUntilMs = 0;

    // Probing & VerifyingEnd state
    qint64 m_probeSentMs = -1;
    double m_probeAmount = 0.0;
    bool m_verifySettled = false;
    qint64 m_verifyStartedMs = 0;

    // Lost recovery state
    qint64 m_recoverStartedMs = 0;
    bool m_recoverReversed = false;
    qint64 m_recoverReverseSentMs = -1;
};

} // namespace ws::capture
