#include "CapturePointer.h"

#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QSettings>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace ws::capture {

namespace {

int systemBaseSize()
{
    const QSettings cursors(u"HKEY_CURRENT_USER\\Control Panel\\Cursors"_s, QSettings::NativeFormat);
    return cursors.value(u"CursorBaseSize"_s, 32).toInt();
}

// White strokes (screen pixels) edged with black, the rest see-through. The
// hotspot is in logical pixels: Qt multiplies it by the window's dpr.
QCursor alphaCursor(QSize size, const QList<QRect>& white, int edge, qreal dpr, QPoint hotspot)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        for (const QRect& r : white)
            painter.fillRect(r.adjusted(-edge, -edge, edge, edge), Qt::black);
        for (const QRect& r : white)
            painter.fillRect(r, Qt::white);
    }
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(dpr);
    return QCursor(pixmap, hotspot.x(), hotspot.y());
}

} // namespace

QCursor brushCursor(QColor color, qreal pixels, qreal dpr, int baseSize)
{
    dpr = std::isfinite(dpr) && dpr > 0 ? dpr : 1;
    const qreal scale = std::clamp(baseSize, 32, 256) / 32.0;
    const int edge = std::max(1, qRound(dpr * std::sqrt(scale)));
    // Wider than this Windows would not show whole; narrower is not seen.
    const qreal radius = std::clamp(pixels, 1.0, 256.0) / 2;
    const qreal white = radius + edge;
    const qreal dark = white + edge;
    // A tip under 12 logical px is hard to find on a busy picture: ticks.
    const bool ticks = 2 * radius < 12 * dpr * scale;
    const int gap = 2 * edge;
    const int tick = qRound(5 * dpr * scale);
    const int reach = int(std::ceil(dark)) + (ticks ? gap + tick + edge : 0) + 1;
    // The hotspot is given in whole logical pixels, which Qt multiplies by
    // the dpr: the middle is put where that lands.
    const int hot = int(std::ceil(reach / dpr));
    const int c = qRound(hot * dpr);
    const int n = 2 * c + 1;
    QImage image(n, n, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        if (ticks) {
            const int off = int(std::ceil(dark)) + gap; // from the middle to where a tick starts
            const int top = c - (edge - 1) / 2;
            const QList<QRect> bars {QRect(c + off, top, tick, edge), QRect(c - off - tick + 1, top, tick, edge),
                QRect(top, c + off, edge, tick), QRect(top, c - off - tick + 1, edge, tick)};
            for (const QRect& bar : bars)
                painter.fillRect(bar.adjusted(-edge, -edge, edge, edge), Qt::black);
            for (const QRect& bar : bars)
                painter.fillRect(bar, Qt::white);
        }
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        const QPointF centre(c + 0.5, c + 0.5); // a pixel's middle, as a dot's centre is
        painter.setBrush(QColor(0, 0, 0, 120));
        painter.drawEllipse(centre, dark, dark);
        painter.setBrush(Qt::white);
        painter.drawEllipse(centre, white, white);
        // Its own colour, see-through where the ink is (the marker's).
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.setBrush(color);
        painter.drawEllipse(centre, radius, radius);
    }
    QPixmap pixmap = QPixmap::fromImage(image);
    pixmap.setDevicePixelRatio(dpr);
    return QCursor(pixmap, hot, hot);
}

CapturePointer::CapturePointer(QQuickItem* parent)
    : QQuickItem(parent)
    , m_baseSize(systemBaseSize())
{
    refreshCursor();
}

void CapturePointer::setShape(int shape)
{
    if (m_shape == shape)
        return;
    m_shape = shape;
    refreshCursor();
    emit shapeChanged();
}

void CapturePointer::setDpr(qreal dpr)
{
    dpr = std::isfinite(dpr) && dpr > 0 ? dpr : 1;
    if (qFuzzyCompare(m_dpr, dpr))
        return;
    m_dpr = dpr;
    refreshCursor();
    emit dprChanged();
}

void CapturePointer::setBaseSize(int size)
{
    if (m_baseSize == size)
        return;
    m_baseSize = size;
    refreshCursor();
    emit baseSizeChanged();
}

void CapturePointer::setBrushSize(qreal pixels)
{
    pixels = std::isfinite(pixels) && pixels > 0 ? pixels : 0;
    if (qFuzzyCompare(m_brushSize + 1, pixels + 1))
        return;
    m_brushSize = pixels;
    refreshCursor();
    emit brushChanged();
}

void CapturePointer::setBrushColor(const QColor& color)
{
    if (m_brushColor == color)
        return;
    m_brushColor = color;
    refreshCursor();
    emit brushChanged();
}

void CapturePointer::refreshCursor()
{
    if (m_shape != Qt::CrossCursor && m_shape != Qt::IBeamCursor) {
        setCursor(QCursor(static_cast<Qt::CursorShape>(m_shape)));
        return;
    }
    if (m_shape == Qt::CrossCursor && m_brushSize > 0 && m_brushColor.isValid()) {
        const Tip tip {m_brushSize, m_brushColor.rgba(), m_dpr, m_baseSize};
        if (tip != m_drawnTip) {
            m_tip = brushCursor(m_brushColor, m_brushSize, m_dpr, m_baseSize);
            m_drawnTip = tip;
        }
        setCursor(m_tip);
        return;
    }
    if (!qFuzzyCompare(m_drawnDpr, m_dpr) || m_drawnSize != m_baseSize) {
        // Windows' sizes go 32, 48, … 256: half steps of these lengths.
        const qreal scale = std::clamp(m_baseSize, 32, 256) / 32.0;
        const int edge = std::max(1, qRound(m_dpr * std::sqrt(scale)));
        const auto onScreen = [this](int logical) { return qRound(logical * m_dpr); }; // where Qt puts the hotspot
        {
            const int arm = qRound(10 * scale);
            const int c = onScreen(arm);
            const int n = 2 * c + 1;
            m_cross = alphaCursor(QSize(n, n), {QRect(edge, c, n - 2 * edge, edge), QRect(c, edge, edge, n - 2 * edge)},
                edge, m_dpr, QPoint(arm, arm));
        }
        {
            const int half = qRound(8 * scale);
            const int serif = qRound(3 * scale);
            const int cx = onScreen(serif);
            const int cy = onScreen(half);
            const int w = 2 * cx + 1;
            const int h = 2 * cy + 1;
            m_beam = alphaCursor(QSize(w, h),
                {QRect(cx, edge, edge, h - 2 * edge), QRect(edge, edge, w - 2 * edge, edge), QRect(edge, h - 2 * edge, w - 2 * edge, edge)},
                edge, m_dpr, QPoint(serif, half));
        }
        m_drawnDpr = m_dpr;
        m_drawnSize = m_baseSize;
    }
    setCursor(m_shape == Qt::IBeamCursor ? m_beam : m_cross);
}

} // namespace ws::capture
