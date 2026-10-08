#include "SystemTheme.h"

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

bool SystemTheme::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange)
        emit changed();
    return QObject::eventFilter(watched, event);
}

} // namespace ws
