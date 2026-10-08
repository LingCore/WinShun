#include "SystemTheme.h"

#include "platform/WindowEffects.h"

#include <QEvent>
#include <QGuiApplication>
#include <QOperatingSystemVersion>
#include <QPalette>
#include <QStyleHints>

#include <windows.h>

using namespace Qt::StringLiterals;

namespace ws {

SystemTheme::SystemTheme(QObject* parent)
    : QObject(parent)
{
    // Windows 11 ships "Segoe Fluent Icons"; Windows 10 has "Segoe MDL2 Assets".
    // Decided by OS version: enumerating all installed fonts is expensive.
    m_iconFont = QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11 ? u"Segoe Fluent Icons"_s
                                                                                          : u"Segoe MDL2 Assets"_s;

    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &SystemTheme::changed);
    qApp->installEventFilter(this); // accent colour changes arrive as a palette change
    qApp->installNativeEventFilter(this); // transparency effects, high contrast: WM_SETTINGCHANGE
    s_instance = this;
}

bool SystemTheme::backdropSystem() const
{
    return win::backdropSupported();
}

void SystemTheme::setBackdropOn(bool on)
{
    if (s_backdropOn == on)
        return;
    s_backdropOn = on;
    if (s_instance)
        emit s_instance->changed();
}

bool SystemTheme::dark() const
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

QColor SystemTheme::accent() const
{
    const QColor accent = QGuiApplication::palette().color(QPalette::Accent);
    // A grey accent (a popular Windows choice) would make highlighted matches
    // indistinguishable from plain text; use the standard Windows blue then.
    if (!accent.isValid() || accent.hsvSaturationF() < 0.25)
        return QColor(0x00, 0x67, 0xC0);
    return accent;
}

bool SystemTheme::animations() const
{
    BOOL on = TRUE;
    return !::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) || on;
}

bool SystemTheme::materials() const
{
    return win::materialsEnabled();
}

bool SystemTheme::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange)
        emit changed();
    return QObject::eventFilter(watched, event);
}

bool SystemTheme::nativeEventFilter(const QByteArray& eventType, void* message, qintptr*)
{
    // Sent to every top-level window when the colour settings change.
    const auto* msg = static_cast<const MSG*>(message);
    if (eventType != "windows_generic_MSG" || !msg || msg->message != WM_SETTINGCHANGE)
        return false;
    if (msg->wParam == SPI_SETHIGHCONTRAST
        || (msg->lParam && ::lstrcmpiW(reinterpret_cast<LPCWSTR>(msg->lParam), L"ImmersiveColorSet") == 0))
        emit changed();
    return false;
}

} // namespace ws
