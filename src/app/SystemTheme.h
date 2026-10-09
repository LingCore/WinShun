#pragma once

#include <QAbstractNativeEventFilter>
#include <QColor>
#include <QObject>
#include <QtQml/qqmlregistration.h>

namespace ws {

// System appearance for QML: light/dark mode, accent colour, icon font.
// Colours derived from these live in Theme.qml.
class SystemTheme : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool dark READ dark NOTIFY changed FINAL)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed FINAL)
    Q_PROPERTY(QString iconFont READ iconFont CONSTANT FINAL)
    // Settings > Accessibility > Visual effects > Animation effects.
    Q_PROPERTY(bool animations READ animations NOTIFY changed FINAL)
    // Whether our windows can stand on Mica (see WindowEffects): Windows 11
    // 22H2 or later and the GPU renderer. Decided once at startup.
    Q_PROPERTY(bool backdropAvailable READ backdropAvailable CONSTANT FINAL)
    // Windows has Mica (11 22H2 or later), whatever the renderer.
    Q_PROPERTY(bool backdropSystem READ backdropSystem CONSTANT FINAL)
    // They do (the 透明效果 setting): their surfaces are translucent then.
    Q_PROPERTY(bool backdrop READ backdrop NOTIFY changed FINAL)
    // Transparency effects on and no high contrast: otherwise Windows draws
    // no Mica.
    Q_PROPERTY(bool materials READ materials NOTIFY changed FINAL)
    // Windows rounds the corners of our windows (11 or later).
    Q_PROPERTY(bool roundedCorners READ roundedCorners CONSTANT FINAL)

public:
    explicit SystemTheme(QObject* parent = nullptr);

    bool dark() const;
    QColor accent() const;
    QString iconFont() const { return m_iconFont; }
    bool animations() const;
    bool materials() const;
    bool backdropSystem() const;
    bool roundedCorners() const;
    static bool backdropAvailable() { return s_backdropAvailable; }
    static void setBackdropAvailable(bool on) { s_backdropAvailable = on; } // before the first window
    static bool backdrop() { return s_backdropAvailable && s_backdropOn; }
    static void setBackdropOn(bool on);

signals:
    void changed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    static inline bool s_backdropAvailable = false;
    static inline bool s_backdropOn = true;
    static inline SystemTheme* s_instance = nullptr; // the QML singleton
    QString m_iconFont;
};

} // namespace ws
