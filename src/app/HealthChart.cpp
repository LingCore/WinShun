#include "HealthChart.h"

#include "HealthMonitor.h"

#include <QDateTime>
#include <QPainter>
#include <QPainterPath>
#include <QQuickWindow>

#include <algorithm>
#include <cmath>

namespace ws {

HealthChart::HealthChart(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
}

void HealthChart::setMonitor(HealthMonitor* monitor)
{
    if (m_monitor == monitor)
        return;
    if (m_monitor)
        disconnect(m_monitor, nullptr, this, nullptr);
    m_monitor = monitor;
    if (m_monitor)
        connect(m_monitor, &HealthMonitor::updated, this, &HealthChart::refresh);
    refresh();
    emit monitorChanged();
}

void HealthChart::setKind(int kind)
{
    if (m_kind == kind)
        return;
    m_kind = kind;
    refresh();
    emit kindChanged();
}

void HealthChart::setRangeMs(qint64 ms)
{
    if (m_rangeMs == ms || ms <= 0)
        return;
    m_rangeMs = ms;
    refresh();
    emit rangeMsChanged();
}

void HealthChart::setHoverTime(qint64 ms)
{
    if (m_hoverTime == ms)
        return;
    m_hoverTime = ms;
    readout();
    update();
    emit hoverTimeChanged();
}

void HealthChart::setColors(const QVariantList& colors)
{
    m_colorList = colors;
    m_colors.clear();
    for (const QVariant& c : colors)
        m_colors.push_back(c.value<QColor>());
    update();
    emit colorsChanged();
}

void HealthChart::geometryChange(const QRectF& now, const QRectF& before)
{
    QQuickPaintedItem::geometryChange(now, before);
    if (now.width() != before.width())
        refresh();
}

double HealthChart::niceTop(double peak, double least)
{
    const double v = std::max(peak * 1.08, least); // a little room above the highest
    const double magnitude = std::pow(10.0, std::floor(std::log10(v)));
    for (const double step : {1.0, 2.0, 2.5, 5.0, 10.0}) {
        if (v <= step * magnitude)
            return step * magnitude;
    }
    return 10 * magnitude;
}

void HealthChart::refresh()
{
    m_series = {};
    m_shown.clear();
    if (m_monitor && width() > 0) {
        const health::History& history = m_monitor->history();
        qint64 to = QDateTime::currentMSecsSinceEpoch();
        if (history.fine().size() > 0)
            to = std::max<qint64>(to, history.fine().back().atMs);
        else if (history.coarse().size() > 0)
            to = std::max<qint64>(to, history.coarse().back().atMs);
        const int buckets = std::max(20, static_cast<int>(width() / 2));
        m_series = history.series(to - m_rangeMs, to, buckets);
        m_series.fromMs = to - m_rangeMs;
        m_series.toMs = to;
        m_shown = m_monitor->events().between(to - m_rangeMs, to);
    }
    switch (m_kind) {
    case 0: m_axisMax = niceTop(std::max(m_series.peakTotalMb, m_series.peakServiceMb), 50); break;
    case 1: m_axisMax = niceTop(m_series.peakCpu, 5); break;
    default: m_axisMax = niceTop(m_series.peakRead, 1); break;
    }
    readout();
    update();
}

double HealthChart::xOf(double ms) const
{
    const double span = static_cast<double>(m_series.toMs - m_series.fromMs);
    return span <= 0 ? 0 : (ms - m_series.fromMs) / span * width();
}

qint64 HealthChart::timeAt(double x) const
{
    const double w = std::max(1.0, width());
    return m_series.fromMs + static_cast<qint64>(std::clamp(x / w, 0.0, 1.0) * (m_series.toMs - m_series.fromMs));
}

int HealthChart::indexAt(qint64 ms) const
{
    const auto& t = m_series.t;
    if (t.empty())
        return -1;
    const auto it = std::lower_bound(t.begin(), t.end(), static_cast<double>(ms));
    int i = static_cast<int>(it - t.begin());
    if (i == static_cast<int>(t.size()) || (i > 0 && ms - t[i - 1] < t[i] - ms))
        --i;
    // Over a hole (asleep, not running): nothing there.
    const double bucket = static_cast<double>(m_series.toMs - m_series.fromMs) / std::max(20.0, width() / 2);
    const double slack = std::max(bucket * 1.5, static_cast<double>(health::History::kGapMs));
    return std::abs(t[i] - static_cast<double>(ms)) <= slack ? i : -1;
}

void HealthChart::readout()
{
    m_values.clear();
    m_valueTime = 0;
    const int i = m_hoverTime > 0 ? indexAt(m_hoverTime) : static_cast<int>(m_series.size()) - 1;
    if (i >= 0) {
        m_valueTime = static_cast<qint64>(m_series.t[i]);
        if (m_kind == 0) {
            for (int p = 0; p < health::kParts; ++p)
                m_values.push_back(m_series.partMb[p][i]);
            m_values.push_back(m_series.serviceMb[i]);
        } else if (m_kind == 1) {
            m_values = {m_series.cpuMax[i], m_series.serviceCpuMax[i]};
        } else {
            m_values = {m_series.readMax[i], m_series.serviceReadMax[i]};
        }
    }
    emit readoutChanged();
}

void HealthChart::paint(QPainter* p)
{
    const double w = width();
    const double h = height();
    if (w <= 0 || h <= 0)
        return;
    const double dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const auto snap = [dpr](double v) { return (std::floor(v * dpr) + 0.5) / dpr; }; // the middle of a device pixel
    const double top = 1;
    const double bottom = h - 1;
    const auto yOf = [&](double v) { return bottom - std::clamp(v / m_axisMax, 0.0, 1.0) * (bottom - top); };
    p->setRenderHint(QPainter::Antialiasing, true);

    // What happened, shaded behind.
    for (const health::Event& e : m_shown) {
        if (e.instant())
            continue;
        const double x0 = std::max(0.0, xOf(static_cast<double>(e.startMs)));
        const double x1 = std::min(w, e.open() ? w : xOf(static_cast<double>(e.endMs)));
        p->fillRect(QRectF(x0, 0, std::max(2.0, x1 - x0), h), m_event);
    }

    // The grid: the top, the middle, the bottom.
    p->setPen(QPen(m_grid, 0));
    for (const double y : {top, (top + bottom) / 2, bottom})
        p->drawLine(QPointF(0, snap(y)), QPointF(w, snap(y)));

    const std::size_t n = m_series.size();
    const auto colorAt = [&](std::size_t i) { return i < m_colors.size() ? m_colors[i] : QColor(Qt::gray); };
    // Each run of points without a hole, [from, to).
    std::vector<std::pair<std::size_t, std::size_t>> runs;
    for (std::size_t i = 0; i < n; ++i) {
        if (i == 0 || m_series.breakBefore[i])
            runs.emplace_back(i, i + 1);
        else
            runs.back().second = i + 1;
    }
    const auto area = [&](const std::vector<double>& lower, const std::vector<double>& upper, const QColor& fill) {
        const auto low = [&](std::size_t i) { return lower.empty() ? bottom : yOf(lower[i]); };
        for (const auto& [from, to] : runs) {
            if (to - from == 1) { // a lone point: a sliver, not nothing
                const double x = xOf(m_series.t[from]);
                p->fillRect(QRectF(x - 0.75, yOf(upper[from]), 1.5, low(from) - yOf(upper[from])), fill);
                continue;
            }
            QPainterPath path;
            path.moveTo(xOf(m_series.t[from]), yOf(upper[from]));
            for (std::size_t i = from + 1; i < to; ++i)
                path.lineTo(xOf(m_series.t[i]), yOf(upper[i]));
            for (std::size_t i = to; i-- > from;)
                path.lineTo(xOf(m_series.t[i]), low(i));
            path.closeSubpath();
            p->fillPath(path, fill);
        }
    };
    const auto line = [&](const std::vector<double>& values, const QPen& pen) {
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        for (const auto& [from, to] : runs) {
            QPainterPath path;
            path.moveTo(xOf(m_series.t[from]), yOf(values[from]));
            for (std::size_t i = from + 1; i < to; ++i)
                path.lineTo(xOf(m_series.t[i]), yOf(values[i]));
            if (to - from == 1)
                path.lineTo(xOf(m_series.t[from]) + 1, yOf(values[from]));
            p->drawPath(path);
        }
    };
    QPen dashed(colorAt(m_kind == 0 ? health::kParts : 1), 1.25);
    dashed.setDashPattern({3, 2.5});
    dashed.setCapStyle(Qt::FlatCap);

    if (n > 0) {
        if (m_kind == 0) {
            std::vector<double> lower(n, 0.0), upper(n, 0.0);
            for (int part = 0; part < health::kParts; ++part) {
                for (std::size_t i = 0; i < n; ++i)
                    upper[i] = lower[i] + m_series.partMb[part][i];
                area(lower, upper, colorAt(part));
                lower = upper;
            }
            line(m_series.serviceMb, dashed);
        } else {
            const auto& mine = m_kind == 1 ? m_series.cpuMax : m_series.readMax;
            const auto& service = m_kind == 1 ? m_series.serviceCpuMax : m_series.serviceReadMax;
            QColor fill = colorAt(0);
            fill.setAlphaF(0.22f);
            area({}, mine, fill);
            QPen pen(colorAt(0), 1.5);
            pen.setJoinStyle(Qt::RoundJoin);
            line(mine, pen);
            line(service, dashed);
        }
    }

    // Give-backs: a small mark at the top, pointing down.
    if (m_kind == 0) {
        p->setPen(Qt::NoPen);
        p->setBrush(m_hover);
        for (const health::Event& e : m_shown) {
            if (!e.instant())
                continue;
            const double x = xOf(static_cast<double>(e.startMs));
            const QPointF tri[3] = {{x - 3.5, 0}, {x + 3.5, 0}, {x, 5}};
            p->drawPolygon(tri, 3);
        }
    }

    // The hover: a line and the points on it.
    if (m_hoverTime > 0) {
        const int i = indexAt(m_hoverTime);
        if (i >= 0) {
            const double x = snap(xOf(m_series.t[i]));
            p->setPen(QPen(m_hover, 0));
            p->drawLine(QPointF(x, 0), QPointF(x, h));
        }
    }
}

} // namespace ws
