#pragma once

// Times as the tools give them: ISO 8601 in local time with the offset
// ("2026-10-24T20:15:36+08:00", "…Z" where the offset is 0).

#include <QJsonValue>
#include <QString>

#include <functional>
#include <unordered_map>

namespace ws::mcp {

// One time, as QDateTime writes it: some 15 µs (the local offset worked
// out, the text made). Null for 0 and before (not known).
QJsonValue isoTime(qint64 secs);

// The same for the many items of one answer: 50 000 times through QDateTime
// took most of ws's longest answers. The offset is asked once per UTC day,
// at the day's first second: a day that starts with the same offset as the
// next has it all through (time zones change it at most once a day, so it
// did not change and change back in between), and the text is made here.
// A day with a change in it, and an offset that is not whole minutes (a
// local mean time, as Monrovia kept until 1972), go item by item through
// QDateTime as before.
// Not for sharing between threads: one per piece of work.
class LocalTimes {
public:
    // `offsetAt`: the local offset from UTC in seconds at a time (seconds
    // since 1970); empty: the system's, as QDateTime has it (tests give a
    // time zone of their own).
    explicit LocalTimes(std::function<int(qint64 secs)> offsetAt = {});

    QJsonValue iso(qint64 secs);

private:
    int offsetAt(qint64 secs) const;
    int dayStart(qint64 day); // the offset at the first second of that UTC day, remembered

    std::function<int(qint64)> m_offsetAt;
    std::unordered_map<qint64, int> m_days;
};

// The text of a time `offset` seconds east of UTC (whole minutes), as
// QDateTime's Qt::ISODate writes it after toOffsetFromUtc(offset).
QString isoText(qint64 secs, int offset);

} // namespace ws::mcp
