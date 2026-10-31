#pragma once

#include <QCoreApplication>
#include <QDateTime>
#include <QString>

namespace ws {

// Shared presentation only: no native power handles, timers or worker.
class KeepAwakeText {
    Q_DECLARE_TR_FUNCTIONS(ws::SwitchKeepAwake)
public:
    [[nodiscard]] static QString noteOf(const QDateTime& until, const QDateTime& now)
    {
        if (!until.isValid())
            return tr("直到手动结束或退出 Win顺");
        const QString clock = until.time().toString(u"HH:mm");
        const qint64 seconds = now.secsTo(until);
        if (seconds <= 0)
            return tr("到 %1").arg(clock);
        const int minutes = static_cast<int>((seconds + 59) / 60);
        const int hours = minutes / 60;
        const int rest = minutes % 60;
        QString left;
        if (hours == 0)
            left = tr("%n 分钟", nullptr, rest);
        else if (rest == 0)
            left = tr("%n 小时", nullptr, hours);
        else
            left = tr("%1 %2").arg(tr("%n 小时", nullptr, hours), tr("%n 分钟", nullptr, rest));
        return tr("到 %1 · 还剩 %2").arg(clock, left);
    }
};

} // namespace ws
