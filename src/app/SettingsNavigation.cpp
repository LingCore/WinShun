#include "SettingsNavigation.h"

#include <algorithm>
#include <cmath>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws {

bool SettingsHistory::Entry::samePlace(const Entry& other) const noexcept
{
    if (query != other.query)
        return false;
    if (!query.isEmpty())
        return true;
    return pageId == other.pageId && tab == other.tab && pluginId == other.pluginId;
}

QVariantMap SettingsHistory::Entry::toVariantMap(int historyIndex) const
{
    QVariantMap map {
        {u"pageId"_s, pageId},
        {u"tab"_s, tab},
        {u"pluginId"_s, pluginId},
        {u"query"_s, query},
        {u"scrollY"_s, scrollY},
    };
    if (historyIndex >= 0)
        map.insert(u"historyIndex"_s, historyIndex);
    return map;
}

SettingsHistory::SettingsHistory(QObject* parent)
    : QObject(parent)
{
}

QVariantMap SettingsHistory::currentEntry() const
{
    if (m_index < 0 || m_index >= count())
        return {};
    return m_entries[static_cast<std::size_t>(m_index)].toVariantMap(m_index);
}

QVariantMap SettingsHistory::backEntry() const
{
    if (!canBack())
        return {};
    return m_entries[static_cast<std::size_t>(m_index - 1)].toVariantMap(m_index - 1);
}

QVariantMap SettingsHistory::forwardEntry() const
{
    if (!canForward())
        return {};
    return m_entries[static_cast<std::size_t>(m_index + 1)].toVariantMap(m_index + 1);
}

QVariantList SettingsHistory::backEntries() const
{
    QVariantList list;
    if (!canBack())
        return list;
    const int stop = std::max(0, m_index - kDropdownLimit);
    list.reserve(m_index - stop);
    for (int i = m_index - 1; i >= stop; --i)
        list.append(m_entries[static_cast<std::size_t>(i)].toVariantMap(i));
    return list;
}

QVariantList SettingsHistory::forwardEntries() const
{
    QVariantList list;
    if (!canForward())
        return list;
    const int stop = std::min(count() - 1, m_index + kDropdownLimit);
    list.reserve(stop - m_index);
    for (int i = m_index + 1; i <= stop; ++i)
        list.append(m_entries[static_cast<std::size_t>(i)].toVariantMap(i));
    return list;
}

void SettingsHistory::setRestoring(bool on)
{
    if (on == m_restoring)
        return;
    m_restoring = on;
    emit restoringChanged();
}

void SettingsHistory::clear()
{
    if (m_entries.empty() && m_index == -1)
        return;
    m_entries.clear();
    m_index = -1;
    emit changed();
}

void SettingsHistory::reset(const QString& pageId, int tab, const QString& pluginId,
    const QString& query, qreal scrollY)
{
    m_entries.clear();
    if (!pageId.isEmpty() || !query.isEmpty()) {
        m_entries.push_back(Entry {pageId, std::max(0, tab), pluginId, query, std::max<qreal>(0, scrollY)});
        m_index = 0;
    } else {
        m_index = -1;
    }
    emit changed();
}

void SettingsHistory::updateScroll(qreal scrollY)
{
    if (m_restoring || m_index < 0 || m_index >= count())
        return;
    m_entries[static_cast<std::size_t>(m_index)].scrollY = std::max<qreal>(0, scrollY);
}

bool SettingsHistory::push(const QString& pageId, int tab, const QString& pluginId,
    const QString& query, qreal scrollY)
{
    if (m_restoring || (pageId.isEmpty() && query.isEmpty()))
        return false;
    const Entry incoming {pageId, std::max(0, tab), pluginId, query, std::max<qreal>(0, scrollY)};
    if (m_index >= 0 && m_index < count() && m_entries[static_cast<std::size_t>(m_index)].samePlace(incoming)) {
        m_entries[static_cast<std::size_t>(m_index)].scrollY = incoming.scrollY;
        return false;
    }
    if (m_index + 1 < count())
        m_entries.erase(m_entries.begin() + m_index + 1, m_entries.end());
    m_entries.push_back(incoming);
    if (count() > kMaxEntries) {
        const int overflow = count() - kMaxEntries;
        m_entries.erase(m_entries.begin(), m_entries.begin() + overflow);
    }
    m_index = count() - 1;
    emit changed();
    return true;
}

void SettingsHistory::replaceCurrent(const QString& pageId, int tab, const QString& pluginId,
    const QString& query, qreal scrollY)
{
    if (m_restoring || (pageId.isEmpty() && query.isEmpty()))
        return;
    const Entry replacement {pageId, std::max(0, tab), pluginId, query, std::max<qreal>(0, scrollY)};
    if (m_index < 0 || m_index >= count()) {
        m_entries.push_back(replacement);
        m_index = count() - 1;
    } else {
        m_entries[static_cast<std::size_t>(m_index)] = replacement;
    }
    emit changed();
}

QVariantMap SettingsHistory::back()
{
    if (!canBack())
        return {};
    --m_index;
    emit changed();
    return currentEntry();
}

QVariantMap SettingsHistory::forward()
{
    if (!canForward())
        return {};
    ++m_index;
    emit changed();
    return currentEntry();
}

QVariantMap SettingsHistory::jumpTo(int index)
{
    if (index < 0 || index >= count() || index == m_index)
        return {};
    m_index = index;
    emit changed();
    return currentEntry();
}

EdgeScrollGate::EdgeScrollGate(QObject* parent)
    : QObject(parent)
{
    m_clock.start();
    m_disarmTimer.setSingleShot(true);
    connect(&m_disarmTimer, &QTimer::timeout, this, &EdgeScrollGate::cancel);
}

void EdgeScrollGate::setEnabled(bool on)
{
    if (on == m_enabled)
        return;
    m_enabled = on;
    if (!on)
        cancel();
    emit configChanged();
}

void EdgeScrollGate::setCanPrevious(bool on)
{
    if (on == m_canPrevious)
        return;
    m_canPrevious = on;
    if (!on && m_direction < 0)
        cancel();
    emit configChanged();
}

void EdgeScrollGate::setCanNext(bool on)
{
    if (on == m_canNext)
        return;
    m_canNext = on;
    if (!on && m_direction > 0)
        cancel();
    emit configChanged();
}

void EdgeScrollGate::setThreshold(int delta)
{
    const int clamped = std::max(1, delta);
    if (clamped == m_threshold)
        return;
    m_threshold = clamped;
    emit configChanged();
}

void EdgeScrollGate::setSettleMs(int ms)
{
    const int clamped = std::max(0, ms);
    if (clamped == m_settleMs)
        return;
    m_settleMs = clamped;
    emit configChanged();
}

void EdgeScrollGate::setSwitchCooldownMs(int ms)
{
    const int clamped = std::max(0, ms);
    if (clamped == m_switchCooldownMs)
        return;
    m_switchCooldownMs = clamped;
    emit configChanged();
}

void EdgeScrollGate::setTrailingLockMs(int ms)
{
    const int clamped = std::max(0, ms);
    if (clamped == m_trailingLockMs)
        return;
    m_trailingLockMs = clamped;
    emit configChanged();
}

void EdgeScrollGate::setDisarmMs(int ms)
{
    const int clamped = std::max(100, ms);
    if (clamped == m_disarmMs)
        return;
    m_disarmMs = clamped;
    emit configChanged();
}

void EdgeScrollGate::setClock(Clock clock)
{
    m_clockOverride = std::move(clock);
    m_settleUntil = 0;
    m_trailingLockUntil = 0;
    m_trailingLockDir = 0;
    cancel();
}

qint64 EdgeScrollGate::nowMs() const
{
    return m_clockOverride ? m_clockOverride() : m_clock.elapsed();
}

bool EdgeScrollGate::canMove(int direction) const noexcept
{
    return (direction > 0 && m_canNext) || (direction < 0 && m_canPrevious);
}

void EdgeScrollGate::setState(int direction, qreal accumulated)
{
    const qreal clamped = direction == 0 ? 0.0 : std::max<qreal>(0.0, accumulated);
    const qreal nextProgress = direction == 0 ? 0.0 : std::clamp(clamped / static_cast<qreal>(m_threshold), 0.0, 1.0);
    const int prevDir = m_direction;
    const bool changed = (m_direction != direction) || (std::abs(m_progress - nextProgress) > 1e-6);
    m_direction = direction;
    m_accumulated = clamped;
    m_progress = nextProgress;
    if (changed)
        emit stateChanged();
    if (prevDir != m_direction)
        emit armedDirectionChanged(m_direction);
}

void EdgeScrollGate::noteBoundaryReached()
{
    const qint64 now = nowMs();
    m_settleUntil = std::max(m_settleUntil, now + m_settleMs);
    cancel();
}

void EdgeScrollGate::noteSwitched(int direction)
{
    const qint64 now = nowMs();
    m_settleUntil = std::max(m_settleUntil, now + m_switchCooldownMs);
    if (direction != 0) {
        m_trailingLockDir = direction > 0 ? 1 : -1;
        m_trailingLockUntil = now + m_trailingLockMs;
    } else {
        m_trailingLockDir = 0;
        m_trailingLockUntil = 0;
    }
    cancel();
}

bool EdgeScrollGate::shouldConsumeWheel(int direction)
{
    if (direction == 0 || m_trailingLockDir == 0)
        return false;
    const int dir = direction > 0 ? 1 : -1;
    const qint64 now = nowMs();
    if (now >= m_trailingLockUntil) {
        m_trailingLockDir = 0;
        return false;
    }
    if (dir != m_trailingLockDir) {
        // Reversing direction immediately ends the trailing lock.
        m_trailingLockDir = 0;
        m_trailingLockUntil = 0;
        return false;
    }
    // Keep the post-switch settle window alive across trailing ticks of the same flick.
    m_settleUntil = std::max(m_settleUntil, now + m_settleMs);
    return true;
}

void EdgeScrollGate::cancel()
{
    m_disarmTimer.stop();
    setState(0, 0);
}

bool EdgeScrollGate::overscroll(int direction, qreal delta)
{
    if (!m_enabled || direction == 0 || delta <= 0) {
        cancel();
        return false;
    }
    const int dir = direction > 0 ? 1 : -1;
    const qint64 now = nowMs();

    // Still inside the post-boundary / post-switch settle window: treat this as
    // the tail of the previous flick and extend the quiet period so continuous
    // fast wheel spins slamming into the bottom never flip pages by accident.
    if (now < m_settleUntil) {
        m_settleUntil = std::max(m_settleUntil, now + m_settleMs);
        cancel();
        return false;
    }

    if (!canMove(dir)) {
        cancel();
        return false;
    }

    // Stale armed progress (older than disarmMs) starts fresh rather than completing.
    if (m_direction != 0 && now - m_lastOverscrollAt >= m_disarmMs)
        cancel();

    // Reversing direction while armed cancels the previous direction before starting fresh.
    if (m_direction != 0 && m_direction != dir) {
        cancel();
        return false;
    }

    m_lastOverscrollAt = now;
    const qreal nextAccumulated = m_accumulated + delta;
    if (nextAccumulated + 1e-6 >= static_cast<qreal>(m_threshold)) {
        noteSwitched(dir);
        emit triggered(dir);
        return true;
    }

    setState(dir, nextAccumulated);
    if (!m_clockOverride)
        m_disarmTimer.start(m_disarmMs);
    return false;
}

} // namespace ws
