#pragma once

#include <QPainterPath>
#include <QPointF>
#include <QRectF>

#include <vector>

namespace ws::capture::laser {

// The presenter's laser pointer (GitHub #189, docs/design-presenter.md 2.7):
// while the button is held, a trail that thins from full width at the newest
// point to nothing at points kLifeMs old, and is gone kLifeMs after the last
// one. It is never part of the drawing (no undo). Pure geometry: the item
// that shows it (LaserTrailItem) fills outline() once, so where the trail
// crosses itself it does not get darker. Units are the caller's (the ink
// layer's logical px); times are milliseconds on one clock.
inline constexpr qint64 kLifeMs = 600;
inline constexpr int kMaxPoints = 512; // memory only: the life sets the length
inline constexpr qreal kMinStep = 1.0; // closer points than this add nothing

class Trail {
public:
    struct Point {
        QPointF at;
        qint64 time = 0;
        bool joined = false; // to the point before it
    };

    explicit Trail(qreal width = 8.0)
        : m_width(width)
    {
    }

    qreal width() const { return m_width; }
    void setWidth(qreal width) { m_width = width > 0 ? width : 1.0; }

    // A point of the stroke under way; `joined` false starts a new stroke
    // (the button pressed again). Points older than the life go first.
    void add(QPointF at, qint64 now, bool joined);
    // Lets go of the points older than the life.
    void prune(qint64 now);
    // The button held still: the newest point stays new (the pointer's dot).
    void hold(qint64 now)
    {
        if (!m_points.empty())
            m_points.back().time = now;
    }
    // Anything left to draw at `now`.
    bool visible(qint64 now) const;
    const std::vector<Point>& points() const { return m_points; }
    void clear() { m_points.clear(); }

    // A point's width at `now`: full when new, 0 at the life's end.
    qreal widthAt(const Point& point, qint64 now) const;
    // The whole trail at `now`: a disc at each point and a tapered band
    // between joined neighbours, one path (winding fill).
    QPainterPath outline(qint64 now) const;
    // What outline(now) can reach.
    QRectF bounds(qint64 now) const;

private:
    std::vector<Point> m_points;
    qreal m_width = 8.0;
};

} // namespace ws::capture::laser
