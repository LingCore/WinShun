#include "ColorSwatch.h"

#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace ws {

ColorSwatch::ColorSwatch(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    // The texture has a pixel for each device pixel; at half a pixel off (the
    // preview pane can be) linear filtering would blur every edge between
    // squares into a seam. It is never scaled.
    setSmooth(false);
    connect(this, &ColorSwatch::changed, this, [this] { update(); });
}

void ColorSwatch::paint(QPainter* painter)
{
    // In logical pixels: the scene graph has scaled the painter for the texture.
    const QRectF box(0, 0, width(), height());
    const bool translucent = m_color.alpha() < 255;
    const qreal split = translucent ? std::round(box.width() / 2) : box.width();
    painter->setRenderHint(QPainter::Antialiasing, false); // squares on whole pixels

    QColor opaque = m_color;
    opaque.setAlpha(255);
    painter->fillRect(QRectF(0, 0, split, box.height()), opaque);
    if (translucent && m_cellSize > 0) {
        const QRectF rest(split, 0, box.width() - split, box.height());
        painter->fillRect(rest, m_checkerColor);
        // Squares counted from the corner, so both sizes of tile start alike.
        const int columns = int(std::ceil(box.width() / m_cellSize));
        const int rows = int(std::ceil(box.height() / m_cellSize));
        for (int row = 0; row < rows; ++row) {
            for (int column = row % 2 == 0 ? 1 : 0; column < columns; column += 2) {
                const QRectF cell = QRectF(column * m_cellSize, row * m_cellSize, m_cellSize, m_cellSize) & rest;
                if (!cell.isEmpty())
                    painter->fillRect(cell, m_checkerAltColor);
            }
        }
        painter->fillRect(rest, m_color);
    }

    // Rounded corners: what lies outside them is cleared, antialiased (a clip
    // path would leave them jagged).
    painter->setRenderHint(QPainter::Antialiasing);
    QPainterPath outside;
    outside.setFillRule(Qt::OddEvenFill);
    outside.addRect(box);
    outside.addRoundedRect(box, m_radius, m_radius);
    painter->setCompositionMode(QPainter::CompositionMode_DestinationOut);
    painter->fillPath(outside, Qt::black);
    painter->setCompositionMode(QPainter::CompositionMode_SourceOver);

    if (m_borderColor.alpha() > 0) {
        painter->setPen(QPen(m_borderColor, 1));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), m_radius - 0.5, m_radius - 0.5);
    }
}

} // namespace ws
