#include "PhosphorGlyph.h"

#include <QFont>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QQuickWindow>
#include <QRawFont>
#include <QSGImageNode>

#include <cmath>

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

} // namespace

PhosphorGlyph::PhosphorGlyph(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents);
}

void PhosphorGlyph::setText(const QString& text)
{
    if (m_text == text)
        return;
    m_text = text;
    layout();
    emit changed();
}

void PhosphorGlyph::setFamily(const QString& family)
{
    if (m_family == family)
        return;
    m_family = family;
    layout();
    emit changed();
}

void PhosphorGlyph::setColor(const QColor& color)
{
    if (m_color == color)
        return;
    m_color = color;
    m_dirty = true;
    update();
    emit changed();
}

void PhosphorGlyph::setEm(qreal em)
{
    if (m_em == em)
        return;
    m_em = em;
    layout();
    emit changed();
}

void PhosphorGlyph::setBox(const QSizeF& box)
{
    if (m_box == box)
        return;
    m_box = box;
    layout();
    emit changed();
}

void PhosphorGlyph::setInkScale(qreal scale)
{
    if (m_inkScale == scale)
        return;
    m_inkScale = scale;
    layout();
    emit changed();
}

void PhosphorGlyph::itemChange(ItemChange change, const ItemChangeData& value)
{
    QQuickItem::itemChange(change, value);
    if (change == ItemDevicePixelRatioHasChanged || (change == ItemSceneChange && value.window))
        layout(); // another screen: other whole pixels
}

void PhosphorGlyph::layout()
{
    m_dirty = true;
    update();
    const QRawFont& font = m_family.isEmpty() ? QRawFont() : outlines(m_family);
    if (m_text.isEmpty() || m_em <= 0 || !font.isValid() || !window()) {
        m_path = {};
        m_pixels = 0;
        if (m_inkLeft != 0) {
            m_inkLeft = 0;
            emit inkChanged();
        }
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
    const QPointF top = QPointF(middle(m_box.width()), middle(m_box.height())) - QPointF(em, em) / 2;
    const QPointF origin(std::floor(top.x() + 1e-6) - kSpare, std::floor(top.y() + 1e-6) - kSpare);
    m_pixels = int(std::ceil(top.x() + em - 1e-6)) + kSpare - int(origin.x()); // the same down: one phase
    // Nudged off the exact half pixel, where a box on one would make the
    // nearest sample a tie that can take a column twice (as ResultRow's icons).
    setPosition(origin / dpr + QPointF(1.0 / 64, 1.0 / 64));
    setSize(QSizeF(m_pixels, m_pixels) / dpr);

    QPainterPath path;
    for (const quint32 index : font.glyphIndexesForString(m_text))
        path.addPath(font.pathForGlyph(index)); // the baseline at 0
    const qreal scale = em / font.pixelSize();
    QTransform place;
    place.translate(top.x() - origin.x(), top.y() - origin.y() + font.ascent() * scale);
    place.scale(scale, scale);
    m_path = place.map(path);
    m_path.setFillRule(Qt::WindingFill);
    const qreal inkLeft = x() + m_path.boundingRect().left() / dpr;
    if (m_inkScale == 1 && !qFuzzyCompare(1 + inkLeft, 1 + m_inkLeft)) {
        m_inkLeft = inkLeft;
        emit inkChanged();
    }
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
    node->setRect(QRectF(0, 0, width(), height()));
    return node;
}

} // namespace ws
