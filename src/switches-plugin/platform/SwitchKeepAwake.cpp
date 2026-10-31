#include "SwitchKeepAwake.h"

#include <QDebug>
#include <QTimer>

#include <windows.h>

#include <utility>

namespace ws {

// The power request, made with the system and the display required. Cleared
// and closed with it, whichever way it goes (ended, replaced, Win顺 exits).
struct SwitchKeepAwake::Request {
    HANDLE handle = nullptr;

    Request() = default;
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
    ~Request()
    {
        if (!handle)
            return;
        ::PowerClearRequest(handle, PowerRequestSystemRequired);
        ::PowerClearRequest(handle, PowerRequestDisplayRequired);
        ::CloseHandle(handle);
    }

    // Null (and logged) when Windows does not take it.
    static std::unique_ptr<Request> make()
    {
        static wchar_t reason[] = L"Win顺：保持唤醒"; // shown by powercfg /requests
        REASON_CONTEXT context {};
        context.Version = POWER_REQUEST_CONTEXT_VERSION;
        context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        context.Reason.SimpleReasonString = reason;
        auto request = std::make_unique<Request>();
        request->handle = ::PowerCreateRequest(&context);
        if (request->handle == INVALID_HANDLE_VALUE) {
            request->handle = nullptr;
            qWarning() << "SwitchKeepAwake: PowerCreateRequest failed" << ::GetLastError();
            return nullptr;
        }
        for (const POWER_REQUEST_TYPE type : {PowerRequestSystemRequired, PowerRequestDisplayRequired}) {
            if (!::PowerSetRequest(request->handle, type)) {
                qWarning() << "SwitchKeepAwake: PowerSetRequest failed" << type << ::GetLastError();
                return nullptr; // ~Request clears and closes
            }
        }
        return request;
    }
};

SwitchKeepAwake::SwitchKeepAwake(Clock now)
    : m_now(std::move(now))
{
}

SwitchKeepAwake::~SwitchKeepAwake() = default; // the request and the timer go with it

QStringList SwitchKeepAwake::ids() const
{
    return {kId.toString()};
}

void SwitchKeepAwake::watch(Notify changed)
{
    m_changed = std::move(changed);
    m_timer = std::make_unique<QTimer>();
    m_timer->setSingleShot(true);
    m_timer->setTimerType(Qt::PreciseTimer); // one wake-up at the end; the coarse timer is 5 % off
    QObject::connect(m_timer.get(), &QTimer::timeout, m_timer.get(), [this] { checkEnd(); });
}

bool SwitchKeepAwake::endedLocked(const QDateTime& now)
{
    if (!m_request || !m_until.isValid() || now < m_until)
        return false;
    m_request.reset();
    m_until = {};
    return true;
}

SwitchState SwitchKeepAwake::stateLocked(const QDateTime& now) const
{
    SwitchState state;
    state.availability = SwitchState::Availability::Ready;
    state.on = m_request ? SwitchState::Value::On : SwitchState::Value::Off;
    state.until = m_until;
    state.note = m_request ? noteOf(m_until, now) : tr("电脑不睡眠、屏幕不关；合盖仍会睡眠");
    return state;
}

SwitchState SwitchKeepAwake::read(const QString&)
{
    const QDateTime now = m_now();
    std::lock_guard lock(m_mutex);
    endedLocked(now); // the computer slept past the end time, or the timer is late
    return stateLocked(now);
}

SwitchWrite SwitchKeepAwake::write(const QString&, const SwitchIntent& intent)
{
    using Kind = SwitchIntent::Kind;
    SwitchWrite result;
    const QDateTime now = m_now();
    std::lock_guard lock(m_mutex);
    endedLocked(now);
    result.after = stateLocked(now);

    QDateTime until; // what this asks of the end time
    switch (intent.kind) {
    case Kind::Off:
        result.ok = true;
        if (m_request) {
            m_request.reset();
            m_until = {};
            result.wrote = true;
        }
        result.after = stateLocked(now);
        return result;
    case Kind::On: // keeps an end time already set
        until = m_until;
        break;
    case Kind::AwakeFor:
    case Kind::AwakeUntil:
        until = endOf(intent, now);
        break;
    default:
        qWarning() << "SwitchKeepAwake: not for keep awake:" << static_cast<int>(intent.kind);
        return result;
    }

    if (!m_request) {
        std::unique_ptr<Request> request = Request::make();
        if (!request) {
            result.error = tr("没能保持唤醒：Windows 没有接受请求");
            return result;
        }
        m_request = std::move(request);
        result.wrote = true;
    }
    if (m_until != until) {
        m_until = until;
        result.wrote = true;
    }
    result.ok = true;
    result.after = stateLocked(now);
    return result;
}

void SwitchKeepAwake::written(const QString&)
{
    arm();
}

void SwitchKeepAwake::arm()
{
    if (!m_timer)
        return;
    QDateTime until;
    {
        std::lock_guard lock(m_mutex);
        if (m_request)
            until = m_until;
    }
    if (until.isValid())
        m_timer->start(msUntil(until, m_now()));
    else
        m_timer->stop();
}

void SwitchKeepAwake::checkEnd()
{
    bool ended = false;
    {
        std::lock_guard lock(m_mutex);
        ended = endedLocked(m_now());
    }
    if (ended) {
        if (m_timer)
            m_timer->stop();
        if (m_changed)
            m_changed(kId.toString());
        return;
    }
    arm(); // early (a timer that fired before the wall clock got there): once more
}

void SwitchKeepAwake::close()
{
    std::lock_guard lock(m_mutex);
    m_request.reset(); // Win顺 is exiting: the computer may sleep again
    m_until = {};
}

bool SwitchKeepAwake::held() const
{
    std::lock_guard lock(m_mutex);
    return m_request != nullptr;
}

bool SwitchKeepAwake::timerActive() const
{
    return m_timer && m_timer->isActive();
}

} // namespace ws
