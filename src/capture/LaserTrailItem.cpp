#include "LaserTrailItem.h"

#include <QPainter>
#include <QQuickWindow>

namespace ws::capture {

namespace {
// Beyond the trail's own reach: antialiasing and the soft rim.
constexpr qreal kMargin = 3;
} // namespace

LaserTrailItem::LaserTrailItem(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    m_clock.start();
    setAntialiasing(true);
    setVisible(false);
    setAcceptedMouseButtons(Qt::NoButton);
}

void LaserTrailItem::setColor(const QColor& color)
{
    if (color == m_color)
        return;
    m_color = color;
    update();
    emit colorChanged();
}

void LaserTrailItem::setLineWidth(qreal width)
{
    if (qFuzzyCompare(width, m_trail.width()))
        return;
    m_trail.setWidth(width);
    emit lineWidthChanged();
}

void LaserTrailItem::press(qreal x, qreal y)
{
    m_down = true;
    m_trail.add(QPointF(x, y), now(), false);
    place(now());
}

void LaserTrailItem::move(qreal x, qreal y)
{
    if (!m_down)
        return;
    m_trail.add(QPointF(x, y), now(), true);
    place(now());
}

void LaserTrailItem::release()
{
    m_down = false;
}

void LaserTrailItem::clear()
{
    m_down = false;
    m_trail.clear();
    place(now());
}

void LaserTrailItem::itemChange(ItemChange change, const ItemChangeData& data)
{
    if (change == ItemSceneChange) {
        disconnect(m_frames);
        if (data.window)
            m_frames = connect(data.window, &QQuickWindow::frameSwapped, this, &LaserTrailItem::tick, Qt::QueuedConnection);
    }
    QQuickPaintedItem::itemChange(change, data);
}

void LaserTrailItem::tick()
{
    if (m_showing)
        place(now());
}

void LaserTrailItem::place(qint64 t)
{
    if (m_down)
        m_trail.hold(t); // while the button is held the newest point stays (the pointer's dot)
    m_trail.prune(t);
    const bool show = m_trail.visible(t);
    if (show) {
        const QRectF r = m_trail.bounds(t).adjusted(-kMargin, -kMargin, kMargin, kMargin);
        m_origin = r.topLeft();
        setPosition(r.topLeft());
        setSize(r.size());
        update();
    }
    setVisible(show);
    if (show != m_showing) {
        m_showing = show;
        emit showingChanged();
    }
}

void LaserTrailItem::paint(QPainter* painter)
{
    const qint64 t = now();
    const QPainterPath path = m_trail.outline(t);
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->translate(-m_origin);
    painter->fillPath(path, m_color);
}

} // namespace ws::capture
