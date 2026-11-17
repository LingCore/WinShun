#include "MonoLogo.h"

#include <QOperatingSystemVersion>
#include <QPainter>
#include <QPainterPath>

namespace ws::monologo {

namespace {

// The panes as in resources/logo.svg, in its 1024 units; the top right one
// grows into a leaf.
QPainterPath panes()
{
    QPainterPath p;
    p.moveTo(512, 512); // top left
    p.lineTo(512, 72);
    p.lineTo(204, 72);
    p.quadTo(72, 72, 72, 204);
    p.lineTo(72, 512);
    p.closeSubpath();
    p.moveTo(512, 512); // bottom left
    p.lineTo(72, 512);
    p.lineTo(72, 820);
    p.quadTo(72, 952, 204, 952);
    p.lineTo(512, 952);
    p.closeSubpath();
    p.moveTo(512, 512); // bottom right
    p.lineTo(512, 952);
    p.lineTo(820, 952);
    p.quadTo(952, 952, 952, 820);
    p.lineTo(952, 512);
    p.closeSubpath();
    p.moveTo(512, 512); // the leaf
    p.lineTo(677, 512);
    p.quadTo(952, 512, 952, 237);
    p.lineTo(952, 72);
    p.lineTo(787, 72);
    p.quadTo(512, 72, 512, 347);
    p.closeSubpath();
    return p;
}

// Thicker than the coloured icon's 72: one colour has no fill to hold the
// shape, and at 16 pixels this is about one and a half.
constexpr double kStroke = 96;
constexpr double kVein = 72;

} // namespace

QImage image(int pixels, const QColor& color)
{
    QImage image(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    // The outer edge of the stroke on the image's edge.
    const double from = 72 - kStroke / 2;
    const double scale = pixels / (952 + kStroke / 2 - from);
    painter.scale(scale, scale);
    painter.translate(-from, -from);
    painter.setPen(QPen(color, kStroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(panes());
    painter.setPen(QPen(color, kVein, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(661.6, 362.4), QPointF(784.8, 239.2));
    return image;
}

HICON icon(int pixels, const QColor& color)
{
    return image(pixels, color).toHICON();
}

bool lightTaskbar()
{
    // Not set: light, Windows 11's default. The value came with Windows 10
    // 1903; before it (LTSC 2019 is 1809) the taskbar is always dark and
    // the value is missing: black on it, the tray icon was all but invisible.
    DWORD value = QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows10_1903 ? 1 : 0;
    DWORD size = sizeof value;
    ::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value != 0;
}

QColor taskbarInk()
{
    HIGHCONTRASTW contrast {sizeof contrast};
    if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
        const COLORREF ink = ::GetSysColor(COLOR_WINDOWTEXT);
        return QColor(GetRValue(ink), GetGValue(ink), GetBValue(ink));
    }
    // Windows 11's taskbar icons: white on dark, near black on light.
    return lightTaskbar() ? QColor(0x1B, 0x1B, 0x1B) : QColor(Qt::white);
}

} // namespace ws::monologo
