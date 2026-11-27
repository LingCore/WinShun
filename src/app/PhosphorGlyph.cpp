#include "PhosphorGlyph.h"

#include "Phosphor.h"

#include <QFont>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QQuickWindow>
#include <QRawFont>
#include <QSGImageNode>

#include <cmath>
#include <optional>

namespace ws {

namespace {

// The outlines at a large size, unhinted, scaled down: the shape as designed.
constexpr int kOutlineSize = 1024;
// Around the em square (some icons reach its edge): the anti-aliased fringe.
constexpr int kSpare = 1;

const QRawFont& outlines(const QString& family)
{
    static QHash<QString, QRawFont> fonts; // the GUI thread only
    auto found = fonts.find(family);
    if (found == fonts.end()) {
        QFont font(family);
        font.setPixelSize(kOutlineSize);
        font.setHintingPreference(QFont::PreferNoHinting);
        font.setStyleStrategy(QFont::NoFontMerging);
        found = fonts.insert(family, QRawFont::fromFont(font));
    }
    return *found;
}

// An icon of our own (family kDrawnFamily, its name for text), outlined on
// Phosphor Regular's grid: 256 units, strokes 16 wide with round ends and
// corners, the top left at 0. Empty: no such icon.
// "copyPath": a box holding "\\..", resources/icons/copy-path.svg.
QPainterPath drawnOutline(const QString& name)
{
    if (name != u"copyPath")
        return {};
    QPainterPath lines;
    lines.addRoundedRect(QRectF(32, 48, 192, 160), 8, 8);
    lines.moveTo(72, 96);
    lines.lineTo(96, 160);
    lines.moveTo(112, 96);
    lines.lineTo(136, 160);
    QPainterPathStroker pen;
    pen.setWidth(16);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    QPainterPath outline = pen.createStroke(lines);
    outline.addEllipse(QPointF(168, 160), 8, 8);
    outline.addEllipse(QPointF(200, 160), 8, 8);
    return outline;
}

constexpr qreal kDrawnGrid = 256;

// An icon's outline as it is painted: the same icon at the same size, on the
// same fraction of a pixel, is one path for all the items that show it (a
// QPainterPath shares its elements; a settings page has the same chevron,
// check mark and caret hundreds of times). The GUI thread only.
struct PathKey {
    QString family;
    QString text;
    int em = 0; // device pixels, in 64ths
    int left = 0; // the em's place in the image, in 64ths of a device pixel
    int top = 0;
    bool operator==(const PathKey&) const = default;
};

size_t qHash(const PathKey& key, size_t seed = 0)
{
    return qHashMulti(seed, key.family, key.text, key.em, key.left, key.top);
}

constexpr qsizetype kPathsKept = 2048; // a few hundred icons at a size or two each; emptied beyond that

} // namespace

PhosphorGlyph::PhosphorGlyph(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents);
    setImplicitSize(m_em, m_em);
}

void PhosphorGlyph::setGlyph(const QString& glyph)
{
    if (m_glyph == glyph)
        return;
    m_glyph = glyph;
    resolve();
    emit iconChanged();
}

void PhosphorGlyph::setIconName(const QString& name)
{
    if (m_iconName == name)
        return;
    m_iconName = name;
    resolve();
    emit iconChanged();
}

void PhosphorGlyph::setFilled(bool filled)
{
    if (m_filled == filled)
        return;
    m_filled = filled;
    resolve();
    emit iconChanged();
}

void PhosphorGlyph::setCaption(bool caption)
{
    if (m_caption == caption)
        return;
    m_caption = caption;
    resolve();
    emit iconChanged();
}

void PhosphorGlyph::setOverlay(const QString& overlay)
{
    if (m_overlay == overlay)
        return;
    m_overlay = overlay;
    emit iconChanged();
}

void PhosphorGlyph::setEm(qreal em)
{
    if (m_em == em)
        return;
    m_em = em;
    setImplicitSize(em, em);
    layout();
    emit sizeChanged();
}

void PhosphorGlyph::setColor(const QColor& color)
{
    if (m_color == color)
        return;
    m_color = color;
    m_dirty = true;
    update();
    emit colorChanged();
}

void PhosphorGlyph::setInkScale(qreal scale)
{
    if (m_inkScale == scale)
        return;
    m_inkScale = scale;
    layout();
    emit inkScaleChanged();
}

// Which icon, and what draws it:
//   one outlined here ("copyPath")               this item, kDrawnFamily
//   a Phosphor name, or a Segoe code with one    this item, the Phosphor font
//   a caption button, a Segoe code without one   the system icon font (fontDrawn)
//   a Phosphor name that is not one              nothing: a "ph:..." spec must
//     never reach the system font, as Phosphor and Segoe share the PUA
void PhosphorGlyph::resolve()
{
    if (!isComponentComplete())
        return; // once, when all of them are set
    QString text;
    QString family;
    if (m_glyph == u"copyPath") {
        text = m_glyph;
        family = kDrawnFamily;
    } else if (!m_caption && !(m_glyph.isEmpty() && m_iconName.isEmpty())) {
        const QString spec = m_iconName.isEmpty() ? m_glyph
                                                  : (m_filled ? QStringLiteral("ph-fill:") : QStringLiteral("ph:")) + m_iconName;
        if (const std::optional<phosphor::Icon> icon = phosphor::resolve(spec)) {
            text = icon->text;
            family = icon->family;
        }
    }
    const bool fontDrawn = text.isEmpty() && m_iconName.isEmpty() && !m_glyph.isEmpty();
    if (text != m_text || family != m_family) {
        m_text = text;
        m_family = family;
        layout();
    }
    if (fontDrawn != m_fontDrawn) {
        m_fontDrawn = fontDrawn;
        emit fontDrawnChanged();
    }
}

void PhosphorGlyph::componentComplete()
{
    QQuickItem::componentComplete();
    resolve();
    layout();
}

void PhosphorGlyph::geometryChange(const QRectF& now, const QRectF& before)
{
    QQuickItem::geometryChange(now, before);
    if (now.size() != before.size())
        layout(); // the em is centred on the item
}

void PhosphorGlyph::itemChange(ItemChange change, const ItemChangeData& value)
{
    QQuickItem::itemChange(change, value);
    if (change == ItemDevicePixelRatioHasChanged || (change == ItemSceneChange && value.window))
        layout(); // another screen: other whole pixels
}

void PhosphorGlyph::layout()
{
    if (!isComponentComplete())
        return;
    m_dirty = true;
    update();
    if (m_text.isEmpty() || m_em <= 0 || !window()) {
        m_path = {};
        m_pixels = 0;
        return;
    }
    const qreal dpr = window()->effectiveDevicePixelRatio();
    const qreal full = m_em * dpr; // device pixels; often not whole (18 at 125 %: 22.5)
    // Pressed (inkScale): whole device pixels smaller, so each step of the
    // press is a size of its own, drawn as crisply as the full one.
    const qreal em = m_inkScale == 1 ? full : std::max<qreal>(1, std::round(full * m_inkScale));
    // The icons are symmetric about the em's centre, so that goes on a pixel
    // centre or a pixel edge, the same one across and down: then an x, a plus
    // or a chevron comes out mirror- and diagonal-symmetric in pixels too.
    // Which of the two: the one that keeps a stroke through the centre (em/16
    // wide) crisp, a pixel centre for an odd width, an edge for an even one.
    // The full size's, also while pressed: the centre stays where it was.
    const qreal phase = qRound(full / 16) % 2 ? 0.5 : 0.0;
    const auto middle = [&](qreal box) { return std::round(box * dpr / 2 - phase) + phase; };
    const QPointF top = QPointF(middle(width()), middle(height())) - QPointF(em, em) / 2;
    const QPointF origin(std::floor(top.x() + 1e-6) - kSpare, std::floor(top.y() + 1e-6) - kSpare);
    m_pixels = int(std::ceil(top.x() + em - 1e-6)) + kSpare - int(origin.x()); // the same down: one phase
    // Nudged off the exact half pixel, where a box on one would make the
    // nearest sample a tie that can take a column twice (as ResultRow's icons).
    m_at = origin / dpr + QPointF(1.0 / 64, 1.0 / 64);

    static QHash<PathKey, QPainterPath> paths;
    const PathKey key {m_family, m_text, qRound(em * 64), qRound((top.x() - origin.x()) * 64), qRound((top.y() - origin.y()) * 64)};
    if (const auto known = paths.constFind(key); known != paths.cend()) {
        m_path = *known;
        return;
    }
    QPainterPath path; // the grid's top left at 0
    qreal scale = 0;
    qreal baseline = 0;
    if (m_family == kDrawnFamily) {
        path = drawnOutline(m_text);
        scale = em / kDrawnGrid;
    } else if (const QRawFont& font = outlines(m_family); font.isValid()) {
        for (const quint32 index : font.glyphIndexesForString(m_text))
            path.addPath(font.pathForGlyph(index)); // the baseline at 0
        scale = em / font.pixelSize();
        baseline = font.ascent() * scale;
    }
    if (path.isEmpty()) {
        m_path = {};
        m_pixels = 0;
        return;
    }
    QTransform place;
    place.translate(top.x() - origin.x(), top.y() - origin.y() + baseline);
    place.scale(scale, scale);
    m_path = place.map(path);
    m_path.setFillRule(Qt::WindingFill);
    if (paths.size() >= kPathsKept)
        paths.clear();
    paths.insert(key, m_path);
}

QSGNode* PhosphorGlyph::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    if (m_pixels <= 0 || m_path.isEmpty()) {
        delete old;
        return nullptr;
    }
    auto* node = static_cast<QSGImageNode*>(old);
    if (!node || m_dirty) {
        delete node; // and its texture: a new picture
        QImage image(m_pixels, m_pixels, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillPath(m_path, m_color);
        painter.end();
        node = window()->createImageNode();
        node->setTexture(window()->createTextureFromImage(
            image, QQuickWindow::CreateTextureOptions(QQuickWindow::TextureHasAlphaChannel) | QQuickWindow::TextureCanUseAtlas));
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Nearest); // texel for texel
        m_dirty = false;
    }
    const qreal side = m_pixels / window()->effectiveDevicePixelRatio();
    node->setRect(QRectF(m_at, QSizeF(side, side)));
    return node;
}

} // namespace ws
