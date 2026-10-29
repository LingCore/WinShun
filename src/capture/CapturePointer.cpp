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

void CapturePointer::refreshCursor()
{
    if (m_shape != Qt::CrossCursor && m_shape != Qt::IBeamCursor) {
        setCursor(QCursor(static_cast<Qt::CursorShape>(m_shape)));
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
