#include "ScrollAutoEngine.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace ws::capture {

namespace {

constexpr int kProbeTimeoutMs = 320; // wait for first visible movement from a probed method
constexpr int kVerifySettleMs = 140; // let smooth-scroll tail or elastic bounce settle at end
constexpr int kVerifyProbeMs = 280; // confirmation wait per method when checking end-of-page
constexpr int kStreamStallMs = 380; // time without forward movement before verifying end
constexpr int kStreamHardStallMs = 260; // with >=3 unacked ticks already in flight
constexpr int kKeysWaitMs = 120;
constexpr int kCursorWaitMs = 150;
constexpr int kMinWheelTickMs = 32;
constexpr int kMaxWheelTickMs = 240;
constexpr int kMinUiaTickMs = 60;
constexpr int kMaxUiaTickMs = 280;
constexpr int kRecoverWaitMs = 320;
constexpr int kRecoverReverseMs = 300;
constexpr int kMaxWaitSliceMs = 40;

bool isWheel(ScrollMethod method)
{
    return method == ScrollMethod::PostWheel || method == ScrollMethod::SendWheel;
}

} // namespace

ScrollAutoEngine::ScrollAutoEngine(Config config)
    : m_config(config)
    , m_ladder(config.elevated, config.prefersSendWheel)
{
}

void ScrollAutoEngine::reconfigure(Config config, bool resetTriedSideways)
{
    m_config = config;
    m_ladder = ScrollLadder(config.elevated, config.prefersSendWheel);
    m_phase = Phase::Idle;
    m_direction = 1;
    if (resetTriedSideways)
        m_triedSideways = false;
    m_userYieldedCursor = false;
    m_pxPerUnit = 0.0;
    m_windowAmount = 0.0;
    m_windowMovedPx = 0.0;
    m_inFlightPx = 0.0;
    m_backoffScale = 1.0;
    m_lastSentAmount = 1.0;
    m_fractionalWorked = false;
    m_fractionalUnsupported = false;
    m_unackedTicks = 0;
    m_otherMethodTicks = 0;
    m_reverseFrames = 0;
    m_probeLostFrames = 0;
    m_probeSentMs = -1;
    m_verifySettled = false;
    m_recoverReversed = false;
    m_recoverReverseSentMs = -1;
}

void ScrollAutoEngine::resetFlowTiming(qint64 nowMs)
{
    m_lastMovedMs = nowMs;
    m_nextTickAtMs = nowMs;
    m_waitUntilMs = 0;
    m_inFlightPx = 0.0;
    m_unackedTicks = 0;
    m_reverseFrames = 0;
    m_probeLostFrames = 0;
    m_probeSentMs = -1;
    m_probeAmount = 0.0;
    m_verifySettled = false;
    m_recoverReversed = false;
    m_recoverReverseSentMs = -1;
}

void ScrollAutoEngine::begin(qint64 nowMs, int direction)
{
    m_direction = direction < 0 ? -1 : 1;
    m_userYieldedCursor = false;
    m_backoffScale = 1.0;
    resetFlowTiming(nowMs);
    if (m_config.elevated) {
        m_ladder = ScrollLadder(true, false);
        m_phase = Phase::Idle;
        return;
    }
    if (m_ladder.atEnd() || m_ladder.method() == ScrollMethod::Manual) {
        m_ladder = ScrollLadder(false, m_config.prefersSendWheel);
        m_pxPerUnit = 0.0;
        m_windowAmount = 0.0;
        m_windowMovedPx = 0.0;
        m_otherMethodTicks = 0;
    }
    m_phase = m_ladder.anyWorked() ? Phase::Streaming : Phase::Probing;
}

void ScrollAutoEngine::stop()
{
    m_phase = Phase::Idle;
    m_probeSentMs = -1;
    m_inFlightPx = 0.0;
    m_unackedTicks = 0;
    m_reverseFrames = 0;
}

double ScrollAutoEngine::targetVelocity(int zonePx) const
{
    const double z = std::max(120.0, double(zonePx));
    double pxPerSec = 0.0;
    switch (m_config.speed) {
    case ScrollSpeed::Slow:
        pxPerSec = std::clamp(z * 0.70, 260.0, 900.0);
        break;
    case ScrollSpeed::Normal:
        pxPerSec = std::clamp(z * 1.45, 520.0, 1900.0);
        break;
    case ScrollSpeed::Fast:
        pxPerSec = std::clamp(z * 2.45, 900.0, 3400.0);
        break;
    }
    return pxPerSec * m_backoffScale;
}

int ScrollAutoEngine::targetWheelTickMs() const
{
    switch (m_config.speed) {
    case ScrollSpeed::Slow:
        return 115;
    case ScrollSpeed::Fast:
        return 65;
    case ScrollSpeed::Normal:
        break;
    }
    return 85;
}

void ScrollAutoEngine::computeTick(int zonePx, ScrollMethod method, double& amount, int& intervalMs) const
{
    const double z = std::max(120.0, double(zonePx));
    const double velocity = targetVelocity(zonePx);
    if (isWheel(method)) {
        const double unitPx = m_pxPerUnit > 0.0 ? m_pxPerUnit : (m_config.pageScrollWheel ? z * 0.8 : 110.0);
        const double maxStepPx = std::max(32.0, z * 0.30);
        const int targetMs = targetWheelTickMs();
        const double desiredStepPx = std::min(maxStepPx, velocity * (targetMs / 1000.0));
        if (!m_fractionalUnsupported && !m_config.pageScrollWheel) {
            // Smooth fractional wheel ticks keep interval well inside smooth-scroll
            // ease curves (~160 ms) so overlapping curves glide at constant velocity.
            const double maxAmount = std::max(0.35, std::min(2.0, maxStepPx / unitPx));
            amount = std::clamp(desiredStepPx / unitPx, 0.35, maxAmount);
            if (amount > 0.85 && amount < 1.0 && 1000.0 * unitPx / velocity <= targetMs + 25)
                amount = 1.0;
        } else if (m_config.pageScrollWheel && unitPx > maxStepPx) {
            amount = std::clamp(maxStepPx / unitPx, 0.25, 1.0);
        } else {
            amount = 1.0;
            if (1000.0 * unitPx / velocity < kMinWheelTickMs) {
                const double wanted = velocity * (kMinWheelTickMs / 1000.0) / unitPx;
                amount = std::clamp(wanted, 1.0, std::min(2.0, maxStepPx / unitPx));
            }
        }
        const double stepPx = amount * unitPx;
        intervalMs = std::clamp(int(std::lround(1000.0 * stepPx / velocity)), kMinWheelTickMs, kMaxWheelTickMs);
        return;
    }
    const double unitPx = m_pxPerUnit > 0.0 ? m_pxPerUnit : z;
    const double targetStepPx = std::max(48.0, z * 0.22);
    amount = std::clamp(targetStepPx / unitPx, 0.05, 0.35);
    intervalMs = std::clamp(int(std::lround(1000.0 * (amount * unitPx) / velocity)), kMinUiaTickMs, kMaxUiaTickMs);
}

bool ScrollAutoEngine::canSwitchSideways(bool extended) const
{
    return !m_triedSideways && !m_config.horizontal && !m_config.elevated && !extended;
}

bool ScrollAutoEngine::canParkCursorFor(ScrollMethod method) const
{
    return method == ScrollMethod::SendWheel && !m_userYieldedCursor && !sustainedWithoutCursor();
}

int ScrollAutoEngine::expectedStepPx() const
{
    if (m_pxPerUnit <= 0.0)
        return 0;
    return int(std::lround(m_pxPerUnit)) * m_direction;
}

void ScrollAutoEngine::onUserMovedCursor(qint64 nowMs, bool stillInsideRegion)
{
    m_userYieldedCursor = true;
    if (!stillInsideRegion && m_ladder.method() == ScrollMethod::SendWheel) {
        m_ladder.refused(ScrollCursorOutside);
        m_probeSentMs = -1;
        m_unackedTicks = 0;
        m_inFlightPx = 0.0;
        m_lastMovedMs = nowMs;
        m_waitUntilMs = nowMs + kCursorWaitMs;
    }
}

void ScrollAutoEngine::onScrollSent(qint64 nowMs, const ScrollResult& result, double amount, int zonePx)
{
    if (m_phase == Phase::Idle || m_phase == Phase::Finished)
        return;
    if (result.code != ScrollOk) {
        if (result.code == ScrollKeysHeld) {
            m_waitUntilMs = nowMs + kKeysWaitMs;
            m_probeSentMs = -1;
            m_unackedTicks = 0;
            m_inFlightPx = 0.0;
            m_lastMovedMs = nowMs;
            return;
        }
        if (m_phase == Phase::Recovering && m_recoverReversed) {
            // Reverse recovery step could not be sent: stop waiting for it.
            m_recoverReverseSentMs = nowMs - kRecoverReverseMs;
            return;
        }
        m_ladder.refused(result.code);
        m_probeSentMs = -1;
        if (result.code == ScrollCursorOutside && m_ladder.hint() == ScrollLadder::Hint::MouseOutside) {
            m_waitUntilMs = nowMs + kCursorWaitMs;
            m_unackedTicks = 0;
            m_inFlightPx = 0.0;
            m_lastMovedMs = nowMs;
        }
        if (m_ladder.method() == ScrollMethod::Manual && m_ladder.hint() == ScrollLadder::Hint::Elevated)
            m_phase = Phase::Idle;
        return;
    }

    if (m_phase == Phase::Recovering) {
        m_recoverReverseSentMs = nowMs;
        return;
    }

    if (m_phase == Phase::Probing || m_phase == Phase::VerifyingEnd) {
        m_probeSentMs = nowMs;
        m_probeAmount = amount;
        return;
    }

    if (m_phase == Phase::Streaming) {
        double dummyAmount = amount;
        int intervalMs = kMinWheelTickMs;
        computeTick(zonePx, result.method, dummyAmount, intervalMs);
        const double unitPx = m_pxPerUnit > 0.0
            ? m_pxPerUnit
            : (isWheel(result.method) ? (m_config.pageScrollWheel ? std::max(120.0, double(zonePx)) * 0.8 : 110.0) : std::max(120.0, double(zonePx)));
        m_lastSentAmount = amount;
        m_windowAmount += amount;
        m_inFlightPx += amount * unitPx;
        ++m_unackedTicks;
        m_nextTickAtMs = nowMs + intervalMs;
    }
}

void ScrollAutoEngine::onFrame(qint64 nowMs, const ScrollStitcher::Step& step, bool extended, int zonePx)
{
    Q_UNUSED(extended);
    if (m_phase == Phase::Idle || m_phase == Phase::Finished)
        return;

    if (step.state == ScrollStitcher::State::Full) {
        m_phase = Phase::Finished;
        return;
    }

    const int forwardPx = step.dy * m_direction;

    if (m_phase == Phase::Recovering) {
        if (step.state != ScrollStitcher::State::Lost) {
            // Reconnected cleanly! Back off target speed slightly if reverse step was needed.
            if (m_recoverReversed)
                m_backoffScale = std::max(0.50, m_backoffScale * 0.75);
            m_phase = m_ladder.anyWorked() ? Phase::Streaming : Phase::Probing;
            resetFlowTiming(nowMs);
            m_nextTickAtMs = nowMs + 60;
        }
        return;
    }

    if (step.state == ScrollStitcher::State::Lost) {
        if (m_phase == Phase::Probing && !m_ladder.anyWorked() && ++m_probeLostFrames < 3)
            return;
        if (m_phase == Phase::Streaming || m_phase == Phase::Probing || m_phase == Phase::VerifyingEnd) {
            m_phase = Phase::Recovering;
            m_recoverStartedMs = nowMs;
            m_recoverReversed = false;
            m_recoverReverseSentMs = -1;
            m_inFlightPx = 0.0;
            m_unackedTicks = 0;
            m_probeSentMs = -1;
        }
        return;
    }
    m_probeLostFrames = 0;

    if (step.state == ScrollStitcher::State::Unchanged)
        return;

    if (forwardPx > 0) {
        const ScrollMethod activeMethod = m_ladder.method();
        if (activeMethod == ScrollMethod::PostWheel || activeMethod == ScrollMethod::Uia)
            ++m_otherMethodTicks;

        if (m_phase == Phase::Probing || m_phase == Phase::VerifyingEnd) {
            const double amt = m_probeAmount > 0.0 ? m_probeAmount : 1.0;
            m_ladder.moved(forwardPx, amt);
            m_windowAmount = amt;
            m_windowMovedPx = double(forwardPx);
            const double defaultUnit = isWheel(activeMethod)
                ? (m_config.pageScrollWheel ? std::max(120.0, double(zonePx)) * 0.8 : 110.0)
                : std::max(120.0, double(zonePx));
            if (m_pxPerUnit <= 0.0) {
                // Smooth-scrolling apps start a notch with a 2..20 px first frame.
                // Never calibrate below defaultUnit from a single ramp-up frame;
                // let the rolling window measure the full distance per notch.
                const double measured = double(forwardPx) / amt;
                m_pxPerUnit = measured >= 60.0
                    ? std::clamp(measured, 60.0, std::max(160.0, double(zonePx) * 1.2))
                    : defaultUnit;
            }
            const double remainingProbePx = std::max(0.0, amt * m_pxPerUnit - double(forwardPx));
            m_phase = Phase::Streaming;
            resetFlowTiming(nowMs);
            m_inFlightPx = remainingProbePx;
            double nextAmt = 1.0;
            int intervalMs = 80;
            computeTick(zonePx, activeMethod, nextAmt, intervalMs);
            m_nextTickAtMs = nowMs + std::min(intervalMs, 90);
            return;
        }

        if (m_phase == Phase::Streaming) {
            m_ladder.moved(forwardPx, 0.0); // clears stall counters without distorting perUnit on partial frames
            if (isWheel(activeMethod) && m_lastSentAmount < 0.99)
                m_fractionalWorked = true;
            m_lastMovedMs = nowMs;
            m_reverseFrames = 0;
            m_unackedTicks = 0;
            m_inFlightPx = std::max(0.0, m_inFlightPx - double(forwardPx));
            m_windowMovedPx += double(forwardPx);
            if (m_windowAmount >= 1.5 && m_windowMovedPx >= 40.0) {
                const double measured = std::clamp(m_windowMovedPx / m_windowAmount, 24.0, std::max(240.0, double(zonePx) * 1.5));
                m_pxPerUnit = m_pxPerUnit > 0.0 ? (0.65 * m_pxPerUnit + 0.35 * measured) : measured;
                m_windowAmount = 0.0;
                m_windowMovedPx = 0.0;
            }
        }
        return;
    }

    // Content moved against auto-scroll direction: if user is manually scrolling
    // back consistently, yield gracefully instead of fighting them.
    if (m_phase == Phase::Streaming && forwardPx <= -24) {
        if (++m_reverseFrames >= 2)
            m_phase = Phase::Idle;
    }
}

ScrollAutoEngine::Command ScrollAutoEngine::next(qint64 nowMs, int zonePx, bool extended)
{
    Command cmd;
    if (m_phase == Phase::Idle) {
        cmd.kind = CommandKind::YieldToManual;
        cmd.hint = m_ladder.hint();
        return cmd;
    }
    if (m_phase == Phase::Finished) {
        cmd.kind = CommandKind::FinishAtEnd;
        return cmd;
    }

    if (m_waitUntilMs > nowMs) {
        cmd.kind = CommandKind::WaitFrame;
        cmd.timeoutMs = int(std::clamp<qint64>(m_waitUntilMs - nowMs, 10, kMaxWaitSliceMs));
        cmd.hint = m_ladder.hint();
        return cmd;
    }

    const auto checkDone = [&]() -> std::optional<Command> {
        if (m_ladder.atEnd()) {
            if (canSwitchSideways(extended)) {
                m_triedSideways = true;
                Command sw;
                sw.kind = CommandKind::SwitchSideways;
                return sw;
            }
            m_phase = Phase::Finished;
            Command fin;
            fin.kind = CommandKind::FinishAtEnd;
            return fin;
        }
        if (m_ladder.method() == ScrollMethod::Manual) {
            if (m_ladder.hint() == ScrollLadder::Hint::NothingWorks && canSwitchSideways(extended)) {
                m_triedSideways = true;
                Command sw;
                sw.kind = CommandKind::SwitchSideways;
                return sw;
            }
            m_phase = Phase::Idle;
            Command yield;
            yield.kind = CommandKind::YieldToManual;
            yield.hint = m_ladder.hint();
            return yield;
        }
        return std::nullopt;
    };

    if (const auto done = checkDone())
        return *done;

    switch (m_phase) {
    case Phase::Probing: {
        if (m_probeSentMs < 0) {
            const ScrollMethod method = m_ladder.method();
            cmd.kind = CommandKind::SendScroll;
            cmd.method = method;
            cmd.amount = isWheel(method) ? (m_config.pageScrollWheel ? 0.25 : 1.0) : 0.20;
            cmd.direction = m_direction;
            cmd.allowParkCursor = canParkCursorFor(method);
            return cmd;
        }
        const qint64 elapsed = nowMs - m_probeSentMs;
        if (elapsed < kProbeTimeoutMs) {
            cmd.kind = CommandKind::WaitFrame;
            cmd.timeoutMs = int(std::clamp<qint64>(kProbeTimeoutMs - elapsed, 10, kMaxWaitSliceMs));
            return cmd;
        }
        // Probe timed out with no movement.
        m_ladder.moved(0, m_probeAmount);
        m_probeSentMs = -1;
        m_pxPerUnit = 0.0;
        m_windowAmount = 0.0;
        m_windowMovedPx = 0.0;
        if (const auto done = checkDone())
            return *done;
        // Immediately probe the next method in the ladder.
        const ScrollMethod method = m_ladder.method();
        cmd.kind = CommandKind::SendScroll;
        cmd.method = method;
        cmd.amount = isWheel(method) ? (m_config.pageScrollWheel ? 0.25 : 1.0) : 0.20;
        cmd.direction = m_direction;
        cmd.allowParkCursor = canParkCursorFor(method);
        return cmd;
    }

    case Phase::Streaming: {
        const double maxInFlight = std::max(80.0, double(zonePx) * 0.42);
        const bool backpressureBlocked = m_inFlightPx >= maxInFlight || m_unackedTicks >= 3;
        const qint64 sinceMoved = nowMs - m_lastMovedMs;
        // Legacy Win32 controls that truncate sub-120 wheel deltas to 0: detect
        // quickly and drop back to integer 1.0 notches without stalling.
        if (!m_fractionalWorked && !m_fractionalUnsupported && isWheel(m_ladder.method())
            && m_lastSentAmount < 0.99 && m_unackedTicks >= 2 && sinceMoved >= 180) {
            m_fractionalUnsupported = true;
            m_unackedTicks = 0;
            m_inFlightPx = 0.0;
            m_windowAmount = 0.0;
            m_windowMovedPx = 0.0;
            m_nextTickAtMs = nowMs;
        }
        if ((m_unackedTicks >= 1 && backpressureBlocked && sinceMoved >= kStreamHardStallMs)
            || (m_unackedTicks >= 2 && sinceMoved >= kStreamStallMs)) {
            m_phase = Phase::VerifyingEnd;
            m_verifySettled = false;
            m_verifyStartedMs = nowMs;
            m_probeSentMs = -1;
            cmd.kind = CommandKind::WaitFrame;
            cmd.timeoutMs = kMaxWaitSliceMs;
            return cmd;
        }
        if (nowMs >= m_nextTickAtMs && !backpressureBlocked) {
            const ScrollMethod method = m_ladder.method();
            double amount = 1.0;
            int intervalMs = kMinWheelTickMs;
            computeTick(zonePx, method, amount, intervalMs);
            cmd.kind = CommandKind::SendScroll;
            cmd.method = method;
            cmd.amount = amount;
            cmd.direction = m_direction;
            cmd.allowParkCursor = canParkCursorFor(method);
            return cmd;
        }
        qint64 waitMs = kMaxWaitSliceMs;
        if (nowMs < m_nextTickAtMs && !backpressureBlocked)
            waitMs = std::clamp<qint64>(m_nextTickAtMs - nowMs, 8, kMaxWaitSliceMs);
        cmd.kind = CommandKind::WaitFrame;
        cmd.timeoutMs = int(waitMs);
        return cmd;
    }

    case Phase::VerifyingEnd: {
        if (!m_verifySettled) {
            const qint64 settled = nowMs - m_verifyStartedMs;
            if (settled < kVerifySettleMs) {
                cmd.kind = CommandKind::WaitFrame;
                cmd.timeoutMs = int(std::clamp<qint64>(kVerifySettleMs - settled, 10, kMaxWaitSliceMs));
                return cmd;
            }
            m_verifySettled = true;
            m_probeSentMs = -1;
        }
        if (m_probeSentMs < 0) {
            const ScrollMethod method = m_ladder.method();
            cmd.kind = CommandKind::SendScroll;
            cmd.method = method;
            cmd.amount = isWheel(method) ? 1.0 : 0.20;
            cmd.direction = m_direction;
            cmd.allowParkCursor = canParkCursorFor(method);
            return cmd;
        }
        const qint64 waited = nowMs - m_probeSentMs;
        if (waited < kVerifyProbeMs) {
            cmd.kind = CommandKind::WaitFrame;
            cmd.timeoutMs = int(std::clamp<qint64>(kVerifyProbeMs - waited, 10, kMaxWaitSliceMs));
            return cmd;
        }
        m_ladder.moved(0, m_probeAmount);
        m_probeSentMs = -1;
        if (const auto done = checkDone())
            return *done;
        const ScrollMethod method = m_ladder.method();
        cmd.kind = CommandKind::SendScroll;
        cmd.method = method;
        cmd.amount = isWheel(method) ? 1.0 : 0.20;
        cmd.direction = m_direction;
        cmd.allowParkCursor = canParkCursorFor(method);
        return cmd;
    }

    case Phase::Recovering: {
        if (!m_recoverReversed) {
            const qint64 elapsed = nowMs - m_recoverStartedMs;
            if (elapsed < kRecoverWaitMs) {
                cmd.kind = CommandKind::WaitFrame;
                cmd.timeoutMs = int(std::clamp<qint64>(kRecoverWaitMs - elapsed, 10, kMaxWaitSliceMs));
                return cmd;
            }
            // Try one gentle reverse notch to reconnect overlap cleanly (only once
            // we have actually scrolled forward in this session).
            m_recoverReversed = true;
            const ScrollMethod method = m_ladder.method();
            if (m_ladder.anyWorked() && method != ScrollMethod::Manual) {
                cmd.kind = CommandKind::SendScroll;
                cmd.method = method;
                cmd.amount = isWheel(method) ? 1.0 : 0.15;
                cmd.direction = -m_direction;
                cmd.allowParkCursor = false;
                return cmd;
            }
        }
        if (m_recoverReverseSentMs >= 0) {
            const qint64 elapsed = nowMs - m_recoverReverseSentMs;
            if (elapsed < kRecoverReverseMs) {
                cmd.kind = CommandKind::WaitFrame;
                cmd.timeoutMs = int(std::clamp<qint64>(kRecoverReverseMs - elapsed, 10, kMaxWaitSliceMs));
                return cmd;
            }
        }
        // Recovery exhausted: hand back to manual cleanly.
        m_phase = Phase::Idle;
        cmd.kind = CommandKind::YieldToManual;
        cmd.lost = true;
        return cmd;
    }

    case Phase::Idle:
    case Phase::Finished:
        break;
    }
    return cmd;
}

} // namespace ws::capture
