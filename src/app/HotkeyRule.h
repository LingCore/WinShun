#pragma once

#include "platform/HotkeyText.h"

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace ws {

// hotkeys::allowed (Settings::hotkeyAllowed) for the shortcut recorders, so
// that what it takes and what AI may set are one rule.
class HotkeyRule : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit HotkeyRule(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

    Q_INVOKABLE bool allowed(const QString& shortcut) const { return hotkeys::allowed(shortcut); }
};

} // namespace ws
