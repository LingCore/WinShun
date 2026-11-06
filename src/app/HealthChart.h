#pragma once

#include "HealthHistory.h"

#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace ws {

class HealthMonitor;

// One chart of the 运行状况 page: the plot only (the words around it are
// QML's Text, crisp at any scale). Memory: the parts stacked, the service as a
// dashed line; CPU and reads: Win顺 as a filled line, the service dashed.
// What happened is shaded behind (HealthMonitor's events); a give-back is a
// mark at the top. The page shares one hover time between its charts.
class HealthChart : public QQuickPaintedItem {
    Q_OBJECT
    Q_MOC_INCLUDE("HealthMonitor.h")
    QML_ELEMENT
    Q_PROPERTY(ws::HealthMonitor* monitor READ monitor WRITE setMonitor NOTIFY monitorChanged FINAL)
    Q_PROPERTY(int kind READ kind WRITE setKind NOTIFY kindChanged FINAL) // 0 memory, 1 CPU, 2 reads
    Q_PROPERTY(qint64 rangeMs READ rangeMs WRITE setRangeMs NOTIFY rangeMsChanged FINAL)
    Q_PROPERTY(qint64 hoverTime READ hoverTime WRITE setHoverTime NOTIFY hoverTimeChanged FINAL) // 0: none
    Q_PROPERTY(QVariantList colors READ colors WRITE setColors NOTIFY colorsChanged FINAL) // memory: the parts; else [Win顺]; then the service
    Q_PROPERTY(QColor gridColor MEMBER m_grid WRITE setGridColor NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QColor eventColor MEMBER m_event WRITE setEventColor NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QColor hoverColor MEMBER m_hover WRITE setHoverColor NOTIFY colorsChanged FINAL)
    // What the words show: the axis's top, and the values at the hover time
    // (or the newest), each series in `colors` order; in MB, percent, MB/s.
    Q_PROPERTY(double axisMax READ axisMax NOTIFY readoutChanged FINAL)
    Q_PROPERTY(QVariantList values READ values NOTIFY readoutChanged FINAL)
    Q_PROPERTY(qint64 valueTime READ valueTime NOTIFY readoutChanged FINAL)
    Q_PROPERTY(bool empty READ empty NOTIFY readoutChanged FINAL)

public:
    explicit HealthChart(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

    HealthMonitor* monitor() const { return m_monitor; }
    void setMonitor(HealthMonitor* monitor);
    int kind() const noexcept { return m_kind; }
    void setKind(int kind);
    qint64 rangeMs() const noexcept { return m_rangeMs; }
    void setRangeMs(qint64 ms);
    qint64 hoverTime() const noexcept { return m_hoverTime; }
    void setHoverTime(qint64 ms);
    QVariantList colors() const { return m_colorList; }
    void setColors(const QVariantList& colors);
    void setGridColor(const QColor& c) { m_grid = c; update(); emit colorsChanged(); }
    void setEventColor(const QColor& c) { m_event = c; update(); emit colorsChanged(); }
    void setHoverColor(const QColor& c) { m_hover = c; update(); emit colorsChanged(); }
    double axisMax() const noexcept { return m_axisMax; }
    QVariantList values() const { return m_values; }
    qint64 valueTime() const noexcept { return m_valueTime; }
    bool empty() const noexcept { return m_series.size() == 0; }

    // The time under x (logical pixels from the left), for the hover.
    Q_INVOKABLE qint64 timeAt(double x) const;
    // A round top for an axis: 1, 2, 2.5, 5 × 10ⁿ, at least `least`.
    static double niceTop(double peak, double least);

signals:
    void monitorChanged();
    void kindChanged();
    void rangeMsChanged();
    void hoverTimeChanged();
    void colorsChanged();
    void readoutChanged();

protected:
    void geometryChange(const QRectF& now, const QRectF& before) override;

private:
    void refresh(); // the series again (a new reading, another range or width)
    void readout(); // values at the hover time
    int indexAt(qint64 ms) const; // the point nearest, -1 none
    double xOf(double ms) const;

    QPointer<HealthMonitor> m_monitor;
    int m_kind = 0;
    qint64 m_rangeMs = 10 * 60 * 1000;
    qint64 m_hoverTime = 0;
    QVariantList m_colorList;
    std::vector<QColor> m_colors;
    QColor m_grid {0, 0, 0, 30};
    QColor m_event {0, 0, 0, 16};
    QColor m_hover {0, 0, 0, 120};
    health::Series m_series;
    std::vector<health::Event> m_shown; // the events in the range
    double m_axisMax = 1;
    QVariantList m_values;
    qint64 m_valueTime = 0;
};

} // namespace ws
