#include "LaserTrail.h"

#include <QLineF>

#include <algorithm>
#include <cmath>

namespace ws::capture::laser {

void Trail::add(QPointF at, qint64 now, bool joined)
{
    prune(now);
    if (joined && !m_points.empty() && QLineF(m_points.back().at, at).length() < kMinStep) {
        m_points.back().time = now; // still here: the point stays fresh
        return;
    }
    if (joined && m_points.empty())
        joined = false;
    m_points.push_back(Point {at, now, joined});
    if (m_points.size() > std::size_t(kMaxPoints))
        m_points.erase(m_points.begin(), m_points.begin() + std::ptrdiff_t(m_points.size() - kMaxPoints));
    if (!m_points.empty())
        m_points.front().joined = false;
}

void Trail::prune(qint64 now)
{
    const auto alive = std::ranges::find_if(m_points, [&](const Point& p) { return now - p.time < kLifeMs; });
    m_points.erase(m_points.begin(), alive);
    if (!m_points.empty())
        m_points.front().joined = false;
}

bool Trail::visible(qint64 now) const
{
    return std::ranges::any_of(m_points, [&](const Point& p) { return now - p.time < kLifeMs; });
}

qreal Trail::widthAt(const Point& point, qint64 now) const
{
    const qreal age = qreal(std::clamp<qint64>(now - point.time, 0, kLifeMs)) / qreal(kLifeMs);
    return m_width * (1.0 - age);
}

QPainterPath Trail::outline(qint64 now) const
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    const Point* before = nullptr;
    for (const Point& p : m_points) {
        const qreal w = widthAt(p, now);
        if (w > 0.05)
            path.addEllipse(p.at, w / 2, w / 2);
        if (before && p.joined) {
            const qreal wb = widthAt(*before, now);
            const QLineF line(before->at, p.at);
            if (line.length() > 0 && (w > 0.05 || wb > 0.05)) {
                const QPointF n = QPointF(-line.dy(), line.dx()) / line.length();
                QPolygonF band;
                band << before->at + n * (wb / 2) << p.at + n * (w / 2) << p.at - n * (w / 2) << before->at - n * (wb / 2);
                path.addPolygon(band);
                path.closeSubpath();
            }
        }
        before = &p;
    }
    return path;
}

QRectF Trail::bounds(qint64 now) const
{
    QRectF r;
    for (const Point& p : m_points) {
        const qreal w = widthAt(p, now);
        if (w <= 0)
            continue;
        const QRectF disc(p.at - QPointF(w / 2, w / 2), QSizeF(w, w));
        r = r.isNull() ? disc : r.united(disc);
    }
    return r;
}

} // namespace ws::capture::laser
