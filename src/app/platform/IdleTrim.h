#pragma once

#include "JournalBudget.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <functional>

namespace ws {

// Hands Win顺's memory back to Windows while nobody uses it. Unused, the app
// touches ~5 MB, yet everything it ever touched (the file index, the windows'
// graphics) stays in its working set: Windows trims a process only when memory
// runs short, so Task Manager shows ~230 MB. A while after the last window
// lost the focus, the working set is emptied: the pages go to the standby
// list, counted as available memory, and come back by soft faults when used
// (measured 2026-10-12: a first name search ~10 ms instead of ~2.3 ms; the
// launcher warms the index when shown, see FileIndex::warm).
// Nothing is freed or rebuilt, so nothing has to load again.
class IdleTrim : public QObject {
public:
    // `busy` names work under way that would touch the pages again at once
    // (the index loading or being built, files being read for content), or
    // returns null. `up`: a window of Win顺's is up and in use without the
    // focus (the clipboard, which leaves the keyboard focus where it was; a
    // pinned launcher), so not idle (GitHub #111: emptied 1.7 s after the
    // clipboard came, while it was in use).
    explicit IdleTrim(std::function<const char*()> busy, std::function<bool()> up, QObject* parent = nullptr);

    // Work `busy` reports may have ended (the index is loaded, a catch-up is
    // done): emptied a few seconds later, not at the next recheck.
    void poke();
    // A window was opened: it is in use from now on (the clipboard never
    // takes the focus, which is what tells otherwise).
    void used();
    // Back from sleep or hibernation (also noticed by itself, at the first
    // check after it). Not emptied until Win顺 is next used: the pages the
    // first opening needs (brought back ahead, App's prewarm) stay. Emptied
    // at wake, they went to the standby list just as every program woke up
    // and read its own back; taken for those, the first opening read them
    // from the disk again: seconds (GitHub #111, "the first hotkey after an
    // hour away took several seconds"). Speed before a few MB.
    void resumed();
    // Whether the working set was emptied since last asked.
    bool takeTrimmed() noexcept { return m_trimmed.exchange(false); }
    // The same, without asking (for the diagnostics journal's "trigger").
    bool trimmed() const noexcept { return m_trimmed.load(); }

private:
    void check();
    bool inUse() const; // a window of Win顺's has the focus, or is up and in use (m_up)
    void startAfter(int ms);
    bool takeSlept(); // since the call before; measured from now on
    void journal(QJsonObject details, bool notable);

    std::function<const char*()> m_busy;
    std::function<bool()> m_up;
    bool m_heldAfterResume = false;
    // At the last check, on clocks with and without the time asleep: when
    // they part, the system slept in between.
    std::uint64_t m_lastTickMs = 0;
    std::uint64_t m_lastAwakeMs = 0;
    const char* m_waitingFor = nullptr; // logged once per reason
    QTimer m_timer;
    QElapsedTimer m_inactive; // since the last window lost the focus
    std::atomic<bool> m_trimmed {false};
    // "idle-trim" entries: the first few, then one every ten minutes; each
    // resume's hold (notable) up to 30 an hour.
    diagnostics::JournalBudget m_journal;
};

} // namespace ws
