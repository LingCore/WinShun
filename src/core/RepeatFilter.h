#pragma once

#include <QHash>
#include <QString>

#include <chrono>
#include <vector>

namespace ws::diagnostics {

// Collapses a log message that comes again and again into its first few
// copies and a count. A Qt warning in a loop costs a synchronous write to
// WinShun.log and the diagnostics journal per copy (~1 ms), on the thread
// that warns, and pushes everything before it out of the bounded files.
// GitHub #96: "QFontEngineFT: Failed to create FreeType font engine" ~800
// times a second held the GUI thread ("not responding") and left nothing else
// in either log (they kept its last 24 s). Pure logic (the caller passes the time and
// locks), so it is unit-tested.
//
// Per message text: the first `copies` within `window` of its first one are
// written; the others are only counted. The count comes back when the window
// is over: with the message's next copy, or when any other message is
// written, or from takeAll() at exit. A message that comes less often than
// that is never held back.
class RepeatFilter {
public:
    using Clock = std::chrono::steady_clock;

    struct Limits {
        int copies = 3;
        Clock::duration window = std::chrono::seconds(10);
        int tracked = 64; // messages followed at a time
        // Beyond them (the table full of live messages, a storm whose text
        // varies): this many other copies are written per window, the rest
        // only counted, as one "other messages" line.
        int untracked = 20;
    };

    struct Repeat {
        QString message;
        int kind = 0; // the caller's (QtMsgType)
        qint64 times = 0; // copies left out
        Clock::duration span {}; // from the window's first copy to its last
    };

    struct Verdict {
        bool write = true; // write this copy...
        std::vector<Repeat> repeats; // ...after these counts, whose windows are over
    };

    explicit RepeatFilter(Limits limits = {})
        : m_limits(limits)
    {
    }

    // A fatal message is always written: it is the crash's cause.
    Verdict check(const QString& message, int kind, Clock::time_point now)
    {
        if (kind == QtFatalMsg)
            return {true, sweep(now)};
        const auto at = m_entries.find(message);
        if (at != m_entries.end() && now - at->first < m_limits.window) {
            at->last = now;
            if (at->written >= m_limits.copies) {
                ++at->leftOut; // the storm itself: nothing else to do
                return {false, {}};
            }
            ++at->written;
            return {true, sweep(now)};
        }
        Verdict verdict {true, sweep(now)}; // this message's last window too
        if (m_entries.size() < m_limits.tracked) {
            m_entries.insert(message, Entry {now, now, kind, 1, 0});
            return verdict;
        }
        if (now - m_otherFirst >= m_limits.window) { // sweep() handed the last bucket over
            m_otherFirst = now;
            m_otherWritten = 0;
        }
        m_otherLast = now;
        if (m_otherWritten >= m_limits.untracked) {
            ++m_otherLeftOut;
            m_otherKind = kind;
            verdict.write = false;
        } else {
            ++m_otherWritten;
        }
        return verdict;
    }

    // Every count not handed over yet: before exit.
    std::vector<Repeat> takeAll()
    {
        std::vector<Repeat> repeats;
        for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
            if (it->leftOut > 0)
                repeats.push_back({it.key(), it->kind, it->leftOut, it->last - it->first});
        }
        m_entries.clear();
        takeOther(repeats);
        return repeats;
    }

    // What the count of copies beyond the table is called.
    static QString otherMessages() { return QStringLiteral("(other repeated messages, more kinds than are followed)"); }

private:
    struct Entry {
        Clock::time_point first;
        Clock::time_point last;
        int kind = 0;
        int written = 0;
        qint64 leftOut = 0;
    };

    // Ends the windows that are over, with the counts they left out.
    std::vector<Repeat> sweep(Clock::time_point now)
    {
        std::vector<Repeat> repeats;
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (now - it->first < m_limits.window) {
                ++it;
                continue;
            }
            if (it->leftOut > 0)
                repeats.push_back({it.key(), it->kind, it->leftOut, it->last - it->first});
            it = m_entries.erase(it);
        }
        if (now - m_otherFirst >= m_limits.window)
            takeOther(repeats);
        return repeats;
    }

    void takeOther(std::vector<Repeat>& repeats)
    {
        if (m_otherLeftOut > 0)
            repeats.push_back({otherMessages(), m_otherKind, m_otherLeftOut, m_otherLast - m_otherFirst});
        m_otherLeftOut = 0;
    }

    Limits m_limits;
    QHash<QString, Entry> m_entries;
    Clock::time_point m_otherFirst {}; // the window of copies beyond the table
    Clock::time_point m_otherLast {};
    int m_otherWritten = 0;
    int m_otherKind = 0;
    qint64 m_otherLeftOut = 0;
};

} // namespace ws::diagnostics
