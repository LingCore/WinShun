#pragma once

#include <QColor>
#include <QPainterPath>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

namespace ws {

// A Phosphor icon in Glyph.qml: the glyph's outline filled by QPainter into
// an image exactly as many device pixels wide as the item, the centre of the
// em square on a pixel centre or edge, the same across and down, shown texel
// for texel (nearest filtering), so a symmetric icon is symmetric in pixels.
// Native text can't do that: it puts the pen (the em's left edge) on a whole
// pixel, and also the baseline, 15/16 em below the em's top, so at most sizes
// an x or chevron is drawn a fraction of a pixel lower than across, and its
// axis falls between pixels ("warped"). See docs/pitfalls.md (Phosphor
// 图标斜线一边虚一边实).
class PhosphorGlyph : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY changed FINAL)
    Q_PROPERTY(QString family READ family WRITE setFamily NOTIFY changed FINAL)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY changed FINAL)
    // The em square and the icon's box (Glyph's size), in logical pixels;
    // the em is centred on the box, to the nearest half device pixel. The
    // item places and sizes itself (the em and a pixel spare).
    Q_PROPERTY(qreal em READ em WRITE setEm NOTIFY changed FINAL)
    Q_PROPERTY(QSizeF box READ box WRITE setBox NOTIFY changed FINAL)

public:
    explicit PhosphorGlyph(QQuickItem* parent = nullptr);

    QString text() const { return m_text; }
    void setText(const QString& text);
    QString family() const { return m_family; }
    void setFamily(const QString& family);
    QColor color() const { return m_color; }
    void setColor(const QColor& color);
    qreal em() const { return m_em; }
    void setEm(qreal em);
    QSizeF box() const { return m_box; }
    void setBox(const QSizeF& box);

signals:
    void changed();

protected:
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;

private:
    void layout(); // the GUI thread: the font; updatePaintNode only paints

    QString m_text;
    QString m_family;
    QColor m_color;
    qreal m_em = 0;
    QSizeF m_box;
    QPainterPath m_path; // in the image's device pixels
    int m_pixels = 0;    // the image's side
    bool m_dirty = true;
};

} // namespace ws
