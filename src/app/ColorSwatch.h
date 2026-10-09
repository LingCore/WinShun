#pragma once

#include <QColor>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

namespace ws {

// A colour on a rounded tile, as design tools show one: a translucent colour
// is split, opaque on the left and over a checkerboard on the right, so that
// its transparency shows instead of passing for a paler colour.
class ColorSwatch : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QColor color MEMBER m_color NOTIFY changed FINAL)
    Q_PROPERTY(qreal radius MEMBER m_radius NOTIFY changed FINAL)
    Q_PROPERTY(QColor borderColor MEMBER m_borderColor NOTIFY changed FINAL) // one pixel, inside
    // The checkerboard: its squares' side (whole device pixels at 125–200 %
    // scaling when a multiple of 4) and its two colours.
    Q_PROPERTY(qreal cellSize MEMBER m_cellSize NOTIFY changed FINAL)
    Q_PROPERTY(QColor checkerColor MEMBER m_checkerColor NOTIFY changed FINAL)
    Q_PROPERTY(QColor checkerAltColor MEMBER m_checkerAltColor NOTIFY changed FINAL)

public:
    explicit ColorSwatch(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

signals:
    void changed();

private:
    QColor m_color;
    qreal m_radius = 0;
    QColor m_borderColor {Qt::transparent};
    qreal m_cellSize = 4;
    QColor m_checkerColor {0xFF, 0xFF, 0xFF};
    QColor m_checkerAltColor {0xD6, 0xD6, 0xD6};
};

} // namespace ws
