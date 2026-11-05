#include "ActivityText.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>

namespace ws::activity {

namespace {

// Over this much of the wait seen is the speed from the last stretch: older
// stretches weigh less, so a slow start does not stay in the figure for good.
constexpr double kSmoothing = 0.3;

} // namespace

void RateEstimator::feed(std::int64_t nowMs, std::size_t done)
{
    if (!m_started || done < m_lastDone || nowMs < m_lastMs) {
        *this = RateEstimator();
        m_started = true;
        m_startMs = m_lastMs = nowMs;
        m_lastDone = done;
        return;
    }
    const std::int64_t span = nowMs - m_lastMs;
    if (span < 2000)
        return; // too short a stretch to say anything
    const double now = static_cast<double>(done - m_lastDone) * 1000.0 / static_cast<double>(span);
    m_rate = m_rate <= 0 ? now : m_rate + (now - m_rate) * kSmoothing;
    m_lastMs = nowMs;
    m_lastDone = done;
}

QString pauseText(Pause pause)
{
    switch (pause) {
    case Pause::SearchBox:
    case Pause::ContentSearch:
        return QCoreApplication::translate("Activity", "你在搜索，先停一下");
    case Pause::Battery:
        return QCoreApplication::translate("Activity", "在用电池，先暂停");
    case Pause::UserPresent:
        return QCoreApplication::translate("Activity", "硬盘上慢慢读，不卡你");
    case Pause::None:
        break;
    }
    return {};
}

QString rangeText(double seconds)
{
    const double low = seconds * 0.75;
    const double high = seconds * 1.25;
    if (high >= 100 * 60) { // hours
        const int a = std::max(1, static_cast<int>(std::lround(low / 3600)));
        const int b = std::max(a, static_cast<int>(std::lround(high / 3600)));
        return a == b ? QCoreApplication::translate("Activity", "约 %1 小时").arg(a) : QCoreApplication::translate("Activity", "约 %1–%2 小时").arg(a).arg(b);
    }
    const int a = std::max(1, static_cast<int>(std::lround(low / 60)));
    const int b = std::max(a, static_cast<int>(std::lround(high / 60)));
    return a == b ? QCoreApplication::translate("Activity", "约 %1 分钟").arg(a) : QCoreApplication::translate("Activity", "约 %1–%2 分钟").arg(a).arg(b);
}

QString remainingText(const Progress& p)
{
    if (p.finishing)
        return QCoreApplication::translate("Activity", "正在整理，快好了");
    if (p.pause == Pause::SearchBox || p.pause == Pause::ContentSearch || p.pause == Pause::Battery)
        return pauseText(p.pause); // no time to give: it does not know when it goes on
    QString eta;
    if (p.sampleMs >= RateEstimator::kMinSampleMs && p.perSecond > 0 && p.total > p.done) {
        const double seconds = static_cast<double>(p.total - p.done) / p.perSecond;
        eta = seconds < 60 ? QCoreApplication::translate("Activity", "快好了") : rangeText(seconds);
    }
    if (p.pause == Pause::UserPresent)
        return eta.isEmpty() ? pauseText(p.pause) : eta + QCoreApplication::translate("Activity", "（%1）").arg(pauseText(p.pause));
    return eta;
}

QString tookText(std::int64_t ms)
{
    const std::int64_t minutes = std::max<std::int64_t>(1, (ms + 30'000) / 60'000);
    if (minutes >= 90)
        return QCoreApplication::translate("Activity", "%1 小时").arg(QString::number(static_cast<double>(minutes) / 60.0, 'f', 1));
    return QCoreApplication::translate("Activity", "%1 分钟").arg(minutes);
}

bool takeNotice(std::int64_t& startedMs, std::int64_t nowMs) noexcept
{
    const bool due = startedMs != 0 && nowMs - startedMs >= kNoticeAfterMs;
    startedMs = 0;
    return due;
}

bool takeContentNotice(std::int64_t& startedMs, std::int64_t nowMs, std::size_t done, std::size_t total) noexcept
{
    if (done < total)
        return false;
    return takeNotice(startedMs, nowMs) && total >= kBigRound;
}

} // namespace ws::activity
