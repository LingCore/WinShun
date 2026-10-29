#pragma once

#include <QJsonObject>
#include <QString>
#include <QtGlobal>

#include <optional>
#include <vector>

#include <windows.h>

// What a screen capture leaves in the diagnostics journal when the pointer or
// the frames go wrong (Capture records them with diagnostics::record; this
// file only works the facts out, so the tests can run it without the app).
// Numbers, booleans and kinds only: no window titles, no text, no paths (a
// cursor file is named by its file name alone).
namespace ws::capture::diag {

// The capture window's thread is watched while the overlay shows: a tick every
// kTickMs, and one that comes after more than kStallAfterMs is a stall of the
// time over the tick. The first kMaxStallEvents of a capture are recorded one
// by one, the rest only counted.
inline constexpr int kTickMs = 50;
inline constexpr int kStallAfterMs = 150;
inline constexpr int kMaxStallEvents = 5;

class StallWatch {
public:
    struct Stall {
        qint64 ms = 0; // how long the thread was held up
        bool record = false; // within the first kMaxStallEvents
    };

    void reset() { *this = {}; }
    // A tick that came `intervalMs` after the one before, `atMs` after the
    // overlay was shown, in `phase`.
    std::optional<Stall> note(qint64 intervalMs, qint64 atMs, const QString& phase);
    // The pointer was over the overlay and hidden: true the first time only.
    bool noteHidden();
    int count() const { return m_count; }
    qint64 longestMs() const { return m_longestMs; }
    qint64 longestAtMs() const { return m_longestAtMs; }
    QString longestPhase() const { return m_longestPhase; }
    int hiddenTicks() const { return m_hiddenTicks; }
    // {count, longest_ms, longest_at_ms, longest_phase, hidden_ticks}
    QJsonObject summary() const;

private:
    int m_count = 0;
    qint64 m_longestMs = 0;
    qint64 m_longestAtMs = 0;
    QString m_longestPhase;
    int m_hiddenTicks = 0;
};

// GetCursorInfo's flags: the pointer is not shown, or Windows holds it back.
bool cursorHidden(DWORD flags);

// {count, p95_ms, max_ms} of the times between frames (nanoseconds); empty for none.
QJsonObject frameStats(std::vector<qint64> nanoseconds);

// The last part of a path: "C:\Windows\Cursors\cross_rl.cur" -> "cross_rl.cur".
QString fileNameOf(const QString& path);

// "monochrome WxH" (no colour bitmap), "colour <bpp>bpp WxH"; empty when the
// cursor can't be read.
QString describeBitmaps(HCURSOR cursor);

// The pointer settings that decide what a cursor looks like (registry, system
// parameters, the system crosshair and I-beam): read once per capture.
QJsonObject pointerFacts();

// What the pointer is right now: {kind, size, hotspot_x, hotspot_y, flags}.
QJsonObject cursorFacts(HCURSOR cursor, DWORD flags);

// Whole seconds since this process started.
qint64 secondsSinceProcessStart();

} // namespace ws::capture::diag
