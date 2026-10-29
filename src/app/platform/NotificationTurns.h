#pragma once

// Whose notification is up on the tray icon, for MessageWindow (GitHub
// #113). Windows shows one at a time: a newer one replaces it, and the one
// replaced may tell its end right then or never (docs/pitfalls.md), while
// it goes with a short animation; one left alone ends by itself after about
// 6 seconds (NIN_BALLOONTIMEOUT) and is not kept, so no click can come for
// it after that. Pure, the time given (milliseconds, GetTickCount64), for
// tst_notificationturns.

#include <QString>

#include <cstdint>
#include <optional>

namespace ws {

class NotificationTurns {
public:
    // One of Win顺's own counts as up this long at most, its end told or
    // not: about 6 seconds, and a margin (a plugin's waits no longer).
    static constexpr std::uint64_t kUpMs = 8000;
    // One replaced goes with an animation: a click this soon after may be
    // on it, and ends this soon after are taken for the replaced ones'.
    static constexpr std::uint64_t kOverlapMs = 1500;
    // One under the mouse stays as long as it is there; one older than this
    // whose end never came has gone without telling.
    static constexpr std::uint64_t kStaysMostMs = 60000;

    // Shown now. `owner`: empty for Win顺's own, else the plugin's id.
    void shown(const QString& owner, std::uint64_t now)
    {
        if (m_newest && now - m_newest->at < kStaysMostMs) {
            Replaced replaced {m_newest->owner, false, now, 1};
            if (m_replaced && now - m_replaced->at < kOverlapMs) { // one after another: all may still be going
                replaced.owners = m_replaced->owners || m_replaced->owner != replaced.owner;
                replaced.endsToCome += m_replaced->endsToCome;
            }
            m_replaced = replaced;
        } else {
            m_replaced.reset();
        }
        m_newest = Turn {owner, now};
    }

    // NIN_BALLOONHIDE or ...TIMEOUT. True when it is the newest's end (its
    // click action goes with it); right after a replacement it is taken for
    // a replaced one's, which may still be seen going.
    bool ended(std::uint64_t now)
    {
        if (m_replaced && now - m_replaced->at < kOverlapMs && m_replaced->endsToCome > 0) {
            --m_replaced->endsToCome;
            return false;
        }
        clear();
        return true;
    }

    void clear() // clicked away, hidden, or the tray icon made again
    {
        m_newest.reset();
        m_replaced.reset();
    }

    // When one of Win顺's own that is up goes, at the latest (a plugin's
    // waits for it); 0 when none is up.
    std::uint64_t ownUpUntil(std::uint64_t now) const
    {
        if (m_newest && m_newest->owner.isEmpty() && now - m_newest->at < kUpMs)
            return m_newest->at + kUpMs;
        return 0;
    }

    // A click now may be on one just replaced, another owner's, while the
    // newest is a plugin's: which was clicked cannot be told, so its action
    // does not run. (With Win顺's own the newest it runs: those only do
    // Win顺's own harmless things.)
    bool clickUnclear(std::uint64_t now) const
    {
        return m_newest && !m_newest->owner.isEmpty() && m_replaced && now - m_replaced->at < kOverlapMs
            && (m_replaced->owners || m_replaced->owner != m_newest->owner);
    }

private:
    struct Turn {
        QString owner;
        std::uint64_t at = 0; // when it was shown
    };
    // The ones replaced one right after another: the last one's owner,
    // whether they were of more than one, when the last went, and how many
    // ends may still come for them.
    struct Replaced {
        QString owner;
        bool owners = false;
        std::uint64_t at = 0;
        int endsToCome = 0;
    };
    std::optional<Turn> m_newest;
    std::optional<Replaced> m_replaced;
};

} // namespace ws
