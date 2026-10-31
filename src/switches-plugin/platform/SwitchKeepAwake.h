#pragma once

#include "SwitchBackend.h"
#include "KeepAwakeText.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QString>
#include <QStringView>

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>

class QTimer;

namespace ws {

// Keep awake: the switch "keep-awake" (docs/design-system-switches.md 5.5).
// Win顺 holds a power request (PowerCreateRequest) with the system and the
// display required: the computer does not sleep and the screen stays on,
// until the user ends it, the end time asked for comes, or Win顺 exits (the
// request goes with the process; it is not remembered across a restart).
//
// The state is Win顺's own, so nothing is read from the system: read() says
// what is held here. Written on the switch thread (a request is made and
// cleared there, quickly). The end time is kept as a wall-clock time; a
// single-shot timer on the GUI thread (made in watch(), armed by written())
// ends it, and every read looks at the wall clock too, so that a computer
// that slept past the end time ends it as soon as it is read again
// (Switches::refresh when the system resumed).
class SwitchKeepAwake final : public SwitchBackend {
public:
    static constexpr QStringView kId = u"keep-awake";
    static constexpr int kMaxMinutes = 99 * 60; // "保持唤醒 99小时": the longest
    using Clock = std::function<QDateTime()>; // local time; tests put in their own

    explicit SwitchKeepAwake(Clock now = [] { return QDateTime::currentDateTime(); });
    ~SwitchKeepAwake() override;

    QStringList ids() const override;
    void watch(Notify changed) override;
    SwitchState read(const QString& id) override;
    SwitchWrite write(const QString& id, const SwitchIntent& intent) override;
    void written(const QString& id) override;
    void close() override;

    // GUI thread: ends it if its time has come, and tells Switches; otherwise
    // the timer is armed again for what is left (the timer's slot).
    void checkEnd();

    // Whether the power request is held now (tests).
    [[nodiscard]] bool held() const;
    // Whether the end timer is armed (tests).
    [[nodiscard]] bool timerActive() const;

    // When `intent` ends keep awake, counted from `now`: AwakeFor in minutes
    // (within 1 to kMaxMinutes), AwakeUntil at that time of day, tomorrow's if
    // it is past (or now). Invalid for any other intent.
    [[nodiscard]] static QDateTime endOf(const SwitchIntent& intent, const QDateTime& now)
    {
        using Kind = SwitchIntent::Kind;
        if (intent.kind == Kind::AwakeFor)
            return now.addSecs(std::clamp(intent.value, 1, kMaxMinutes) * 60LL).addMSecs(-now.time().msec());
        if (intent.kind != Kind::AwakeUntil)
            return {};
        const int minutes = std::clamp(intent.value, 0, 24 * 60 - 1);
        QDateTime end(now.date(), QTime(minutes / 60, minutes % 60));
        if (end <= now)
            end = end.addDays(1);
        return end;
    }

    // The second line: "到 16:42 · 还剩 42 分钟" (the minutes round up), or
    // that it goes on until it is ended. `until` invalid: no end time.
    [[nodiscard]] static QString noteOf(const QDateTime& until, const QDateTime& now)
    {
        return KeepAwakeText::noteOf(until, now);
    }

    // How long until the timer should look at the clock: the time left, at
    // least 1 ms, within what a QTimer takes (about 24 days).
    [[nodiscard]] static int msUntil(const QDateTime& until, const QDateTime& now)
    {
        return static_cast<int>(std::clamp<qint64>(now.msecsTo(until), 1, 2'000'000'000));
    }

private:
    struct Request; // the power request (RAII)

    static QString tr(const char* source, const char* disambiguation = nullptr, int n = -1)
    {
        return QCoreApplication::translate("ws::SwitchKeepAwake", source, disambiguation, n);
    }
    [[nodiscard]] SwitchState stateLocked(const QDateTime& now) const; // m_mutex held
    bool endedLocked(const QDateTime& now); // m_mutex held: let go of it if its time has come
    void arm(); // GUI thread

    Clock m_now;
    mutable std::mutex m_mutex; // the switch thread and the GUI thread (checkEnd)
    std::unique_ptr<Request> m_request; // null: not held
    QDateTime m_until; // invalid: no end time
    std::unique_ptr<QTimer> m_timer; // GUI thread; made in watch()
    Notify m_changed;
};

} // namespace ws
