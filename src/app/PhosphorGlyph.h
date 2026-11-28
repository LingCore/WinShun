#pragma once

#include <QColor>
#include <QPainterPath>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

namespace ws {

// An icon (Glyph.qml is this item, with the theme's colour): a Phosphor icon
// by name (iconName) or by its legacy Segoe Fluent / MDL2 code point (glyph),
// see phosphor::resolve, or one outlined here (kDrawnFamily).
//
// The glyph's outline is filled by QPainter into an image exactly as many
// device pixels wide as it needs, the centre of the em square on a pixel
// centre or edge, the same across and down, shown texel for texel (nearest
// filtering), so a symmetric icon is symmetric in pixels. Native text can't
// do that: it puts the pen (the em's left edge) on a whole pixel, and also
// the baseline, 15/16 em below the em's top, so at most sizes an x or chevron
// is drawn a fraction of a pixel lower than across, and its axis falls
// between pixels ("warped"). See docs/pitfalls.md (Phosphor 图标斜线一边虚一边实).
//
// One item per icon, no children and no bindings of its own: a settings
// window has some two thousand icons, and as an Item holding this, a Text for
// the system font and a Loader (five objects and a dozen bindings each) they
// were a fifth of all its objects. What Phosphor does not draw (fontDrawn: a
// caption button's glyph, any icon where the fonts did not load) is a Text
// made for that icon alone (GlyphFont.qml).
class PhosphorGlyph : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    // A legacy Segoe code point, or the name of an icon outlined here ("copyPath").
    Q_PROPERTY(QString glyph READ glyph WRITE setGlyph NOTIFY iconChanged FINAL)
    // A Phosphor icon by name, e.g. "heart"; filled picks the Fill weight.
    // Prefer this for new icons: the name is stable, a Segoe code is not.
    Q_PROPERTY(QString iconName READ iconName WRITE setIconName NOTIFY iconChanged FINAL)
    Q_PROPERTY(bool filled READ filled WRITE setFilled NOTIFY iconChanged FINAL)
    // A window's caption button: Windows' own glyph, as its own windows have.
    Q_PROPERTY(bool caption READ caption WRITE setCaption NOTIFY iconChanged FINAL)
    // A second font glyph drawn over the first, as MDL2 fills pair with their
    // outlines: PinnedFill is only the head, the needle is in Pin. Only where
    // the system font draws (fontDrawn): a Phosphor icon is whole in one glyph.
    Q_PROPERTY(QString overlay READ overlay WRITE setOverlay NOTIFY iconChanged FINAL)
    // The icon's container: an even square (14, 16, 18, 20, 24), and the
    // item's implicit size. A Phosphor icon's em square is exactly this box,
    // its ink inside it with the margin Phosphor's grid gives every icon
    // (about 82% of the box drawn on), so what stands beside an icon keeps
    // its distance from the box and never looks at the ink. In logical
    // pixels, whatever the font DPI. An item made wider or taller than that
    // (a slot) has the em centred in it, to the nearest half device pixel.
    Q_PROPERTY(qreal size READ em WRITE setEm NOTIFY sizeChanged FINAL)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged FINAL)
    // A pressed key's icon, a little smaller (KeySurface's contentScale): the
    // box stays, so nothing beside it moves; the outline is drawn afresh at a
    // whole number of device pixels, on the same centre. A scaled picture
    // would drop pixels (nearest filtering) and blur.
    Q_PROPERTY(qreal inkScale READ inkScale WRITE setInkScale NOTIFY inkScaleChanged FINAL)
    // Not drawn here: the system icon font's glyph (GlyphFont.qml).
    Q_PROPERTY(bool fontDrawn READ fontDrawn NOTIFY fontDrawnChanged FINAL)

public:
    explicit PhosphorGlyph(QQuickItem* parent = nullptr);

    // Not a font: `text` names an icon outlined here on Phosphor's grid
    // (copyPath), drawn the same way as a font's.
    static constexpr QLatin1StringView kDrawnFamily {"ws-drawn"};

    QString glyph() const { return m_glyph; }
    void setGlyph(const QString& glyph);
    QString iconName() const { return m_iconName; }
    void setIconName(const QString& name);
    bool filled() const { return m_filled; }
    void setFilled(bool filled);
    bool caption() const { return m_caption; }
    void setCaption(bool caption);
    QString overlay() const { return m_overlay; }
    void setOverlay(const QString& overlay);
    qreal em() const { return m_em; }
    void setEm(qreal em);
    QColor color() const { return m_color; }
    void setColor(const QColor& color);
    qreal inkScale() const { return m_inkScale; }
    void setInkScale(qreal scale);
    bool fontDrawn() const { return m_fontDrawn; }

    // What is drawn here: the icon's character and its font (kDrawnFamily:
    // its name); empty where nothing is (no icon, or fontDrawn).
    QString text() const { return m_text; }
    QString family() const { return m_family; }

signals:
    void iconChanged();
    void sizeChanged();
    void colorChanged();
    void inkScaleChanged();
    void fontDrawnChanged();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF& now, const QRectF& before) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;

private:
    void resolve(); // which icon, by what: after glyph, iconName, filled or caption changed
    void layout(); // the GUI thread: the font; updatePaintNode only paints

    QString m_glyph;
    QString m_iconName;
    QString m_overlay;
    QString m_text;
    QString m_family;
    QColor m_color;
    qreal m_em = 16;
    qreal m_inkScale = 1;
    QPainterPath m_path; // in the image's device pixels; shared by the icons that look the same
    QPointF m_at;        // the image's top left in the item, logical pixels
    int m_pixels = 0;    // the image's side
    bool m_filled = false;
    bool m_caption = false;
    bool m_fontDrawn = false;
    bool m_dirty = true;
};

} // namespace ws
