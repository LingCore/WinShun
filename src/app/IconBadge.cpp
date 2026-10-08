#include "IconBadge.h"

#include <QFont>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

namespace ws {

IconBadge::IconBadge(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    connect(this, &IconBadge::changed, this, [this] { update(); });
}

void IconBadge::paint(QPainter* painter)
{
    const qreal s = m_badgeSize;
    // Relative to the scaling the scene graph has set up for the texture.
    painter->setRenderHint(QPainter::Antialiasing);
    painter->translate(width() / 2, height() / 2);
    painter->rotate(m_angle);

    QLinearGradient fill(0, -s / 2, 0, s / 2);
    fill.setColorAt(0, m_topColor);
    fill.setColorAt(1, m_bottomColor);
    painter->setPen(Qt::NoPen);
    painter->setBrush(fill);
    painter->drawRoundedRect(QRectF(-s / 2, -s / 2, s, s), s * 0.27, s * 0.27);

    if (m_glyph.isEmpty())
        return;
    // The glyph's outline taken at a large size, where hinting cannot bend
    // it, and scaled down to half the badge. Icon fonts draw within their em
    // square, so that is what gets centred.
    constexpr qreal kOutlineSize = 256;
    QFont font(m_iconFont);
    font.setPixelSize(int(kOutlineSize));
    font.setHintingPreference(QFont::PreferNoHinting);
    const QFontMetricsF metrics(font);
    QPainterPath glyph;
    glyph.addText(0, 0, font, m_glyph);
    const qreal scale = s * 0.5 / kOutlineSize;
    painter->scale(scale, scale);
    painter->translate(-metrics.horizontalAdvance(m_glyph) / 2, (metrics.ascent() - metrics.descent()) / 2);
    painter->setBrush(m_glyphColor);
    painter->drawPath(glyph);
}

} // namespace ws
