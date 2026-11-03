#pragma once

#include "LaserTrail.h"

#include <QColor>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

class QQuickWindow;

namespace ws::capture {

// The laser pointer's trail on the presenter's ink layer (laser::Trail),
// in the layer's logical px. The item covers only what the trail reaches,
// moved and resized each frame, so a full-screen layer never paints a
// full-screen picture; it asks for the next frame while anything shows and
// for none after.
class LaserTrailItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(LaserTrail)

    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged FINAL)
    // The trail's width where it is newest, logical px.
    Q_PROPERTY(qreal lineWidth READ lineWidth WRITE setLineWidth NOTIFY lineWidthChanged FINAL)
    // Anything on the screen now (for the tests and the cursor).
    Q_PROPERTY(bool showing READ showing NOTIFY showingChanged FINAL)

public:
    explicit LaserTrailItem(QQuickItem* parent = nullptr);

    QColor color() const { return m_color; }
    void setColor(const QColor& color);
    qreal lineWidth() const { return m_trail.width(); }
    void setLineWidth(qreal width);
    bool showing() const { return m_showing; }

    // Positions in the parent's coordinates (the layer's).
    Q_INVOKABLE void press(qreal x, qreal y);
    Q_INVOKABLE void move(qreal x, qreal y);
    Q_INVOKABLE void release();
    Q_INVOKABLE void clear();

    void paint(QPainter* painter) override;

signals:
    void colorChanged();
    void lineWidthChanged();
    void showingChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData& data) override;

private:
    void tick(); // a frame went out: the next one while anything shows
    void place(qint64 now);
    qint64 now() const { return m_clock.elapsed(); }

    laser::Trail m_trail;
    QColor m_color = QColor(0xFF, 0x2D, 0x2D);
    QElapsedTimer m_clock;
    bool m_down = false;
    bool m_showing = false;
    QPointF m_origin; // the item's top left in the parent, when last placed
    QMetaObject::Connection m_frames;
};

} // namespace ws::capture
