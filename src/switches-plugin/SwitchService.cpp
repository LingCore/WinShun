#include "SwitchService.h"

#include "SwitchWire.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QPointer>

#include <utility>

using namespace Qt::StringLiterals;

namespace ws::switchplugin {

Service::Service(SwitchBackends backends, Protocol& protocol)
    : m_protocol(protocol), m_backends(std::move(backends))
{
    m_worker = std::make_unique<ComWorker>(ComWorker::Apartment::MultiThreaded, [this] {
        for (const auto& backend : m_backends)
            backend->close();
    });
    const QPointer self(this);
    for (const auto& backend : m_backends) {
        for (const QString& id : backend->ids()) {
            if (switchwire::known(id))
                m_byId.insert(id, backend.get());
        }
        backend->setStopToken(m_worker->stopToken());
        backend->watch([self](const QString& id) {
            QMetaObject::invokeMethod(self, [self, id] { if (self) self->changed(id); }, Qt::QueuedConnection);
        });
    }
    m_notify.setSingleShot(true);
    connect(&m_notify, &QTimer::timeout, this, [this] {
        const QStringList ids(m_changed.cbegin(), m_changed.cend());
        m_changed.clear();
        if (!ids.isEmpty() && m_accepting)
            m_protocol.notify(u"systemSwitches/changed"_s, {{u"ids"_s, QJsonArray::fromStringList(ids)}});
    });
    // A real, hidden top-level window receives system broadcasts. Core-only
    // Qt needs no renderer or visible window for the radio/audio service.
    WNDCLASSW cls {};
    cls.lpfnWndProc = &Service::windowProc;
    cls.hInstance = ::GetModuleHandleW(nullptr);
    cls.lpszClassName = L"WinShun.Switches.Events";
    ::RegisterClassW(&cls);
    m_window = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, cls.hInstance, this);
}

Service::~Service() { shutdown(); }

LRESULT CALLBACK Service::windowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_NCCREATE)
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* service = reinterpret_cast<Service*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (service && message == WM_SETTINGCHANGE)
        service->changed(u"theme"_s);
    return ::DefWindowProcW(window, message, wp, lp);
}

void Service::changed(const QString& id)
{
    if (!m_accepting || !switchwire::known(id))
        return;
    m_changed.insert(id);
    if (!m_notify.isActive())
        m_notify.start(0);
}

void Service::request(const QJsonValue& id, const QString& method, const QJsonObject& params)
{
    if (method == u"shutdown") {
        shutdown();
        m_protocol.respond(id, QJsonObject {});
        QCoreApplication::quit();
        return;
    }
    if (id.isUndefined() || id.isNull())
        return;
    if (method == u"query") {
        m_protocol.respond(id, QJsonObject {{u"items"_s, QJsonArray {}}});
        return;
    }
    if (method != u"systemSwitches/read" && method != u"systemSwitches/write") {
        m_protocol.fail(id, -32601, u"Unknown method"_s);
        return;
    }
    const QString key = params.value(u"id"_s).toString();
    SwitchBackend* backend = m_byId.value(key);
    if (!m_accepting || !backend || !switchwire::known(key)) {
        m_protocol.fail(id, -32602, u"Unknown switch or provider stopped"_s);
        return;
    }
    const QPointer self(this);
    if (method == u"systemSwitches/read") {
        m_worker->post([self, backend, key, id] {
            const SwitchState state = backend->read(key);
            QMetaObject::invokeMethod(self, [self, id, state] {
                if (self && self->m_accepting)
                    self->m_protocol.respond(id, switchwire::state(state));
            }, Qt::QueuedConnection);
        });
        return;
    }
    const auto intent = switchwire::intent(params.value(u"intent"_s));
    if (!intent || intent->kind == SwitchIntent::Kind::Toggle || intent->kind == SwitchIntent::Kind::OpenSettings) {
        m_protocol.fail(id, -32602, u"An explicit switch intent is required"_s);
        return;
    }
    m_worker->post([self, backend, key, id, intent = *intent] {
        const SwitchWrite result = backend->write(key, intent);
        QMetaObject::invokeMethod(self, [self, backend, key, id, result] {
            if (!self || !self->m_accepting)
                return;
            // The host broadcasts a theme write after updating its own row,
            // so its windows change immediately. Keep-awake timers live here.
            if (result.wrote && key != u"theme")
                backend->written(key);
            self->m_protocol.respond(id, switchwire::write(result));
        }, Qt::QueuedConnection);
    });
}

void Service::shutdown()
{
    if (!std::exchange(m_accepting, false))
        return;
    m_notify.stop();
    m_worker.reset(); // stop and close COM on its owner thread
    m_backends.clear(); // release the provider's power request now
    m_byId.clear();
    if (m_window)
        ::DestroyWindow(std::exchange(m_window, nullptr));
}

} // namespace ws::switchplugin
