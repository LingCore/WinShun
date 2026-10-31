#pragma once

#include "SwitchProtocol.h"
#include "SwitchBackend.h"
#include "platform/ComWorker.h"

#include <QHash>
#include <QSet>
#include <QTimer>

#include <windows.h>

namespace ws::switchplugin {

// Uses the same native backends that previously lived in the application.
// Tests inject backends through this constructor; production uses the factory.
class Service : public QObject {
public:
    Service(SwitchBackends backends, Protocol& protocol);
    ~Service() override;
    void request(const QJsonValue& id, const QString& method, const QJsonObject& params);
    void shutdown();

private:
    void changed(const QString& id);
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    Protocol& m_protocol;
    SwitchBackends m_backends;
    QHash<QString, SwitchBackend*> m_byId;
    QSet<QString> m_changed;
    QTimer m_notify;
    HWND m_window = nullptr;
    bool m_accepting = true;
    std::unique_ptr<ComWorker> m_worker;
};

} // namespace ws::switchplugin
