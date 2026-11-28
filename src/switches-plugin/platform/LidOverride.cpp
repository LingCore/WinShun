#include "LidOverride.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <windows.h>
#include <powrprof.h>

#include <utility>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

// GUID_SYSTEM_BUTTON_SUBGROUP and GUID_LIDCLOSE_ACTION (winnt.h), here so
// that no INITGUID is needed.
constexpr GUID kButtons {0x4f971e89, 0xeebd, 0x4455, {0xa8, 0xde, 0x9e, 0x59, 0x04, 0x0e, 0x73, 0x47}};
constexpr GUID kLidClose {0x5ca83367, 0x6e45, 0x459f, {0xa2, 0x7b, 0x47, 0x6b, 0x1d, 0x01, 0xc9, 0x36}};

class SystemPlans final : public PowerPlans {
public:
    bool lidPresent() override
    {
        SYSTEM_POWER_CAPABILITIES caps {};
        return ::GetPwrCapabilities(&caps) && caps.LidPresent;
    }

    // "Select the lid switch action" (Computer Configuration > Administrative
    // Templates > System > Power Management > Button Settings).
    bool lidByPolicy() override
    {
        static constexpr const wchar_t* kKey =
            L"SOFTWARE\\Policies\\Microsoft\\Power\\PowerSettings\\5ca83367-6e45-459f-a27b-476b1d01c936";
        for (const wchar_t* value : {L"ACSettingIndex", L"DCSettingIndex"}) {
            if (::RegGetValueW(HKEY_LOCAL_MACHINE, kKey, value, RRF_RT_DWORD, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
                return true;
        }
        return false;
    }

    QUuid active() override
    {
        GUID* scheme = nullptr;
        if (::PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme)
            return {};
        const QUuid id(*scheme);
        ::LocalFree(scheme);
        return id;
    }

    std::optional<LidActions> lid(const QUuid& scheme) override
    {
        const GUID id = scheme;
        DWORD ac = 0;
        DWORD dc = 0;
        if (::PowerReadACValueIndex(nullptr, &id, &kButtons, &kLidClose, &ac) != ERROR_SUCCESS
            || ::PowerReadDCValueIndex(nullptr, &id, &kButtons, &kLidClose, &dc) != ERROR_SUCCESS)
            return std::nullopt;
        return LidActions {ac, dc};
    }

    bool setLid(const QUuid& scheme, const LidActions& actions) override
    {
        const GUID id = scheme;
        DWORD error = ::PowerWriteACValueIndex(nullptr, &id, &kButtons, &kLidClose, actions.ac);
        if (error == ERROR_SUCCESS)
            error = ::PowerWriteDCValueIndex(nullptr, &id, &kButtons, &kLidClose, actions.dc);
        // A change to the active scheme counts once it is made active again.
        if (error == ERROR_SUCCESS && active() == scheme)
            error = ::PowerSetActiveScheme(nullptr, &id);
        if (error != ERROR_SUCCESS)
            qWarning() << "LidOverride: the lid's action not written" << scheme << error;
        return error == ERROR_SUCCESS;
    }
};

} // namespace

std::unique_ptr<PowerPlans> PowerPlans::system()
{
    return std::make_unique<SystemPlans>();
}

LidOverride::LidOverride(std::unique_ptr<PowerPlans> plans, QString record)
    : m_plans(std::move(plans)), m_record(std::move(record))
{
    m_lidPresent = m_plans->lidPresent();
    recover();
}

LidOverride::~LidOverride()
{
    release();
}

LidOverride::Outcome LidOverride::hold()
{
    if (m_held)
        return Outcome::Held;
    if (!m_lidPresent)
        return Outcome::NoLid;
    if (m_plans->lidByPolicy())
        return Outcome::ByPolicy;
    const QUuid scheme = m_plans->active();
    const std::optional<LidActions> was = scheme.isNull() ? std::nullopt : m_plans->lid(scheme);
    if (!was)
        return Outcome::Failed;
    Change change {.scheme = scheme};
    if (was->ac != 0)
        change.ac = was->ac;
    if (was->dc != 0)
        change.dc = was->dc;
    if (change.ac || change.dc) {
        if (!save(change))
            return Outcome::Failed; // not changed, as it could not be put back after a crash
        if (!m_plans->setLid(scheme, {})) {
            if (putBack(change) && !m_record.isEmpty()) // one of the two may have been written
                QFile::remove(m_record);
            return Outcome::Failed;
        }
        m_change = change;
    }
    m_held = true;
    m_scheme = scheme;
    return Outcome::Held;
}

void LidOverride::release()
{
    if (!std::exchange(m_held, false))
        return;
    m_scheme = {};
    if (const std::optional<Change> change = std::exchange(m_change, std::nullopt);
        change && putBack(*change) && !m_record.isEmpty())
        QFile::remove(m_record);
}

LidOverride::Outcome LidOverride::follow()
{
    if (!m_held)
        return Outcome::Held;
    const QUuid now = m_plans->active();
    if (now.isNull() || now == m_scheme)
        return Outcome::Held;
    release();
    return hold();
}

// Each action back where it still does nothing. False when the scheme cannot
// be written (the record stays, to try again); true when it is gone.
bool LidOverride::putBack(const Change& change)
{
    const std::optional<LidActions> now = m_plans->lid(change.scheme);
    if (!now) {
        qWarning() << "LidOverride: the scheme is gone, nothing to put back" << change.scheme;
        return true;
    }
    LidActions back = *now;
    if (change.ac && now->ac == 0)
        back.ac = *change.ac;
    if (change.dc && now->dc == 0)
        back.dc = *change.dc;
    return back == *now || m_plans->setLid(change.scheme, back);
}

void LidOverride::recover()
{
    if (m_record.isEmpty() || !QFileInfo::exists(m_record))
        return;
    const std::optional<Change> change = load();
    if (!change)
        qWarning() << "LidOverride: an unreadable record dropped" << m_record;
    if (!change || putBack(*change))
        QFile::remove(m_record);
}

bool LidOverride::save(const Change& change) const
{
    if (m_record.isEmpty())
        return true;
    QJsonObject o {{u"scheme"_s, change.scheme.toString(QUuid::WithoutBraces)}};
    if (change.ac)
        o.insert(u"ac"_s, static_cast<qint64>(*change.ac));
    if (change.dc)
        o.insert(u"dc"_s, static_cast<qint64>(*change.dc));
    QDir().mkpath(QFileInfo(m_record).absolutePath());
    QSaveFile file(m_record);
    if (file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(o).toJson(QJsonDocument::Compact)) >= 0
        && file.commit())
        return true;
    qWarning() << "LidOverride: the record not written" << m_record << file.errorString();
    return false;
}

std::optional<LidOverride::Change> LidOverride::load() const
{
    QFile file(m_record);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096)
        return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
    Change change {.scheme = QUuid::fromString(o.value(u"scheme"_s).toString())};
    // An action is 0 to 3; anything else was not written by Win顺.
    const auto action = [&o](QStringView key) -> std::optional<quint32> {
        const QJsonValue v = o.value(key);
        if (!v.isDouble() || v.toInteger(-1) < 1 || v.toInteger(-1) > 3)
            return std::nullopt;
        return static_cast<quint32>(v.toInteger());
    };
    change.ac = action(u"ac");
    change.dc = action(u"dc");
    if (change.scheme.isNull() || (!change.ac && !change.dc))
        return std::nullopt;
    return change;
}

} // namespace ws
