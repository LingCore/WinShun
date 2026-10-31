#pragma once

#include "Switches.h"

#include <QObject>
#include <QPointer>
#include <QString>

namespace ws::plugin { class Host; class Catalog; }

namespace ws {

// The switch cache sees only asynchronous requests. All native backends,
// device subscriptions and power handles belong to the provider process.
class SwitchPlugin : public QObject {
    Q_OBJECT
public:
    SwitchPlugin(plugin::Host* host, QString id, QObject* parent = nullptr);
    QString id() const { return m_id; }
    Switches::Remote remote();
    static QString provider(const plugin::Catalog& catalog);

signals:
    void changed(const QString& id); // empty: provider connection/state changed

private:
    SwitchState unavailable() const;
    QPointer<plugin::Host> m_host;
    QString m_id;
};

} // namespace ws
