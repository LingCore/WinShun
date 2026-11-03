#pragma once

#include <QtGlobal>

namespace ws::capture {

// When the capture's key hints (CaptureKeyHints.qml) show by themselves:
// there for whoever stops to look, never in the way of whoever knows the
// keys. Times are the caller's clock, ms; CaptureSession feeds it and wakes
// it at nextDeadline(). The rules:
//  1. A capture begins with none. After kStartMs with no mouse button
//     pressed they fade in; moving the mouse, hovering windows, the wheel
//     and keys meanwhile (trying the keys out) leave that alone.
//  2. A mouse button pressed on the picture (any button) takes them away at
//     once, also a manual show. While a drag, an adjustment, a stroke or a
//     text is under way there are no rows (CaptureSession::keyHintRows).
//  3. A selection made (or recalled with R, or brought back with , and .):
//     none. Only when the user hesitates (kIdleMs with no move, key, click,
//     wheel or tool) do the selection's rows fade in; any of those takes
//     them away again. Once per capture, so they never nag.
//  4. Placement (the pointer, selection, magnifier or toolbar near) is the
//     panel's own.
//  5. ? or F1: shown until pressed again, a button pressed or the capture's
//     end, with no timer; hidden: nothing shows by itself for the rest of
//     this capture.
//  6. Off in the settings (不再显示): nothing shows by itself; ? still
//     shows them. 一直显示 (or the settings switch) turns that back on.
//  7. A capture begun by the delay's countdown (quiet): nothing by itself;
//     it is for a moment that passes. Long screenshots and recordings take
//     the overlay away; R, , and . count as input (rule 3).
//  8. Each capture begins afresh.
class KeyHintPolicy {
public:
    static constexpr qint64 kStartMs = 400;
    static constexpr qint64 kIdleMs = 2000;
    enum class Stage { None, Choosing, Selected }; // None: not capturing, or busy (no rows)

    void begin(qint64 now)
    {
        reset();
        m_active = true;
        m_since = now;
    }
    void end() { reset(); }
    void setQuiet() { m_quiet = true; } // rule 7: this capture shows nothing by itself
    void setEnabled(bool on)
    {
        m_enabled = on;
        if (!on)
            m_auto = false;
    }
    bool enabled() const { return m_enabled; }

    void setStage(Stage stage, qint64 now)
    {
        if (stage == m_stage)
            return;
        m_stage = stage;
        if (stage != Stage::Choosing)
            m_auto = false; // rules 2 and 3: a selection, or busy
        if (stage == Stage::Selected)
            m_since = now;
    }
    void press() // rule 2
    {
        m_auto = false;
        m_choosingDone = true;
        if (m_manual == Manual::Shown)
            m_manual = Manual::None;
    }
    void input(qint64 now) // a move, a key, the wheel, a tool: rule 3 (rule 1 leaves them be)
    {
        if (m_stage != Stage::Selected)
            return;
        m_auto = false;
        m_since = now;
    }
    void toggle() { m_manual = shown() ? Manual::Hidden : Manual::Shown; } // rule 5
    void keep(bool on) // the panel's button, rule 6
    {
        setEnabled(on);
        if (!on)
            m_manual = Manual::None;
    }
    // The timers due by now (rules 1 and 3).
    void tick(qint64 now)
    {
        if (!m_active || !m_enabled || m_quiet || m_manual != Manual::None)
            return;
        if (m_stage == Stage::Choosing && !m_choosingDone && now - m_since >= kStartMs) {
            m_choosingDone = true;
            m_auto = true;
        } else if (m_stage == Stage::Selected && !m_selectedDone && now - m_since >= kIdleMs) {
            m_selectedDone = true;
            m_auto = true;
        }
    }
    // When tick() may show them next; -1: nothing to wait for.
    qint64 nextDeadline() const
    {
        if (!m_active || !m_enabled || m_quiet || m_manual != Manual::None || m_auto)
            return -1;
        if (m_stage == Stage::Choosing && !m_choosingDone)
            return m_since + kStartMs;
        if (m_stage == Stage::Selected && !m_selectedDone)
            return m_since + kIdleMs;
        return -1;
    }
    bool shown() const { return m_active && (m_manual == Manual::None ? m_auto : m_manual == Manual::Shown); }

private:
    enum class Manual { None, Shown, Hidden };
    void reset() // all but the setting
    {
        const bool enabled = m_enabled;
        *this = KeyHintPolicy {};
        m_enabled = enabled;
    }

    bool m_enabled = true; // the setting ([Capture] KeyHints); kept across captures
    bool m_active = false;
    bool m_quiet = false;
    Stage m_stage = Stage::None;
    qint64 m_since = 0; // the capture's start, or the last input on a selection
    bool m_auto = false; // shown by a timer
    bool m_choosingDone = false; // rule 1 done (shown, or a button pressed first)
    bool m_selectedDone = false; // rule 3 done
    Manual m_manual = Manual::None;
};

} // namespace ws::capture
