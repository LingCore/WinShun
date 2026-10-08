#pragma once

#include <QColor>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

namespace ws {

// A rounded square in a vertical gradient with an icon-font glyph on it,
// optionally tilted (MacShun: IconBadge + rotationEffect). The tilt is drawn
// by QPainter into an upright texture: rotating a Text item instead blurs a
// natively rendered glyph, and the curve renderer's antialiasing frays its
// one-pixel lines. The item must be big enough for the tilted square.
class IconBadge : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString glyph MEMBER m_glyph NOTIFY changed FINAL)
    Q_PROPERTY(QString iconFont MEMBER m_iconFont NOTIFY changed FINAL)
    Q_PROPERTY(qreal badgeSize MEMBER m_badgeSize NOTIFY changed FINAL) // the square's side
    Q_PROPERTY(qreal angle MEMBER m_angle NOTIFY changed FINAL) // degrees, clockwise
    Q_PROPERTY(QColor topColor MEMBER m_topColor NOTIFY changed FINAL)
    Q_PROPERTY(QColor bottomColor MEMBER m_bottomColor NOTIFY changed FINAL)
    Q_PROPERTY(QColor glyphColor MEMBER m_glyphColor NOTIFY changed FINAL)

public:
    explicit IconBadge(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

signals:
    void changed();

private:
    QString m_glyph;
    QString m_iconFont;
    qreal m_badgeSize = 32;
    qreal m_angle = 0;
    QColor m_topColor;
    QColor m_bottomColor;
    QColor m_glyphColor {Qt::white};
};

} // namespace ws
