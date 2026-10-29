#include "IsoTime.h"

#include <QDateTime>
#include <QTimeZone>

#include <array>
#include <chrono>
#include <utility>

namespace ws::mcp {

namespace {

constexpr qint64 kDay = 86'400;

qint64 floorDiv(qint64 a, qint64 b) noexcept
{
    return a / b - (a % b != 0 && (a < 0) != (b < 0) ? 1 : 0);
}

char* putDigits(char* at, int value, int digits) noexcept
{
    for (int i = digits - 1; i >= 0; --i) {
        at[i] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    return at + digits;
}

} // namespace

QJsonValue isoTime(qint64 secs)
{
    if (secs <= 0)
        return QJsonValue::Null;
    const QDateTime time = QDateTime::fromSecsSinceEpoch(secs);
    return time.toOffsetFromUtc(time.offsetFromUtc()).toString(Qt::ISODate);
}

QString isoText(qint64 secs, int offset)
{
    using namespace std::chrono;
    const qint64 local = secs + offset;
    const qint64 day = floorDiv(local, kDay);
    const year_month_day date {sys_days {days {day}}};
    const qint64 inDay = local - day * kDay;
    std::array<char, 32> text {};
    char* at = text.data();
    at = putDigits(at, static_cast<int>(date.year()), 4);
    *at++ = '-';
    at = putDigits(at, static_cast<int>(static_cast<unsigned>(date.month())), 2);
    *at++ = '-';
    at = putDigits(at, static_cast<int>(static_cast<unsigned>(date.day())), 2);
    *at++ = 'T';
    at = putDigits(at, static_cast<int>(inDay / 3600), 2);
    *at++ = ':';
    at = putDigits(at, static_cast<int>(inDay / 60 % 60), 2);
    *at++ = ':';
    at = putDigits(at, static_cast<int>(inDay % 60), 2);
    if (offset == 0) {
        *at++ = 'Z'; // toOffsetFromUtc(0) is UTC
    } else {
        const int minutes = (offset < 0 ? -offset : offset) / 60;
        *at++ = offset < 0 ? '-' : '+';
        at = putDigits(at, minutes / 60, 2);
        *at++ = ':';
        at = putDigits(at, minutes % 60, 2);
    }
    return QString::fromLatin1(text.data(), at - text.data());
}

LocalTimes::LocalTimes(std::function<int(qint64)> offsetAt)
    : m_offsetAt(std::move(offsetAt))
{
}

int LocalTimes::offsetAt(qint64 secs) const
{
    if (m_offsetAt)
        return m_offsetAt(secs);
    return QDateTime::fromSecsSinceEpoch(secs).offsetFromUtc();
}

int LocalTimes::dayStart(qint64 day)
{
    if (const auto it = m_days.find(day); it != m_days.end())
        return it->second;
    const int offset = offsetAt(day * kDay);
    m_days.emplace(day, offset);
    return offset;
}

QJsonValue LocalTimes::iso(qint64 secs)
{
    if (secs <= 0)
        return QJsonValue::Null;
    const qint64 day = floorDiv(secs, kDay);
    const int start = dayStart(day);
    const int offset = start == dayStart(day + 1) ? start : offsetAt(secs);
    if (offset % 60 != 0)
        return QDateTime::fromSecsSinceEpoch(secs, QTimeZone::UTC).toOffsetFromUtc(offset).toString(Qt::ISODate);
    return isoText(secs, offset);
}

} // namespace ws::mcp
