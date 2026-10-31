#include "SwitchPlugin.h"

#include "Host.h"
#include "SwitchWire.h"
#include "platform/SettingBroadcast.h"

using namespace Qt::StringLiterals;

namespace ws {

SwitchPlugin::SwitchPlugin(plugin::Host* host, QString id, QObject* parent)
    : QObject(parent), m_host(host), m_id(std::move(id))
{
    connect(host, &plugin::Host::stateChanged, this, [this](const QString& id) {
        if (id == m_id)
            emit changed({});
    });
    connect(host, &plugin::Host::systemSwitchesChanged, this, [this](const QString& plugin, const QStringList& ids) {
        if (plugin != m_id)
            return;
        for (const QString& id : ids) {
            if (switchwire::known(id))
                emit changed(id);
        }
    });
}

QString SwitchPlugin::provider(const plugin::Catalog& catalog)
{
    QString first;
    for (const plugin::Entry& e : catalog.entries()) {
        if (!e.manifest || !e.manifest->systemSwitches || !catalog.enabled(e.id))
            continue;
        if (e.id == switchwire::kPlugin)
            return e.id;
        if (first.isEmpty())
            first = e.id;
    }
    return first;
}

SwitchState SwitchPlugin::unavailable() const
{
    SwitchState state;
    state.availability = SwitchState::Availability::Blocked;
    state.note = tr("系统开关插件暂不可用，可打开 Windows 设置");
    return state;
}

Switches::Remote SwitchPlugin::remote()
{
    Switches::Remote remote;
    remote.ids = switchwire::ids();
    const QPointer self(this);
    remote.read = [self](const QString& id, std::function<void(SwitchState)> done) {
        if (!self)
            return;
        if (!self->m_host) {
            done(self->unavailable());
            return;
        }
        self->m_host->request(self->m_id, u"systemSwitches/read"_s, {{u"id"_s, id}},
            [self, done = std::move(done)](bool ok, const QJsonValue& result) {
                if (!self)
                    return;
                const auto state = ok ? switchwire::state(result) : std::nullopt;
                done(state.value_or(self->unavailable()));
            });
    };
    remote.write = [self](const QString& id, SwitchIntent intent, std::function<void(SwitchWrite)> done) {
        if (!self)
            return;
        const auto failed = [self, done](const QJsonValue& = {}) {
            if (self) {
                SwitchWrite result;
                result.after = self->unavailable();
                result.error = result.after.note;
                done(std::move(result));
            }
        };
        if (!self->m_host) {
            failed();
            return;
        }
        self->m_host->request(self->m_id, u"systemSwitches/write"_s,
            {{u"id"_s, id}, {u"intent"_s, switchwire::intent(intent)}},
            [self, done = std::move(done), failed](bool ok, const QJsonValue& value) {
                if (!self)
                    return;
                const auto result = ok ? switchwire::write(value) : std::nullopt;
                if (result)
                    done(*result);
                else
                    failed();
            });
    };
    remote.written = [](const QString& id) {
        if (id == u"theme")
            settingbroadcast::send(settingbroadcast::Area::ImmersiveColorSet);
    };
    return remote;
}

} // namespace ws
