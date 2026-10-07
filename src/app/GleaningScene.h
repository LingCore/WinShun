#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

namespace qf {

// The moving part of the 拾穗计划 page's backdrop: a wheat field along the
// bottom, motes of light drifting up from it and a few birds crossing the
// sky (the sky gradient itself is plain QML). Ported from MacShun
// (Gleaning.swift, SceneCanvas). Still unless `running`; it then continues
// where it stopped.
class GleaningScene : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged FINAL)
    // Width on the left covered by the settings window's sidebar: the gap in
    // the middle of the field lines up with the content area's centre.
    Q_PROPERTY(qreal leadingInset READ leadingInset WRITE setLeadingInset NOTIFY leadingInsetChanged FINAL)
    Q_PROPERTY(QColor wheatBack MEMBER m_wheatBack NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QColor wheatFront MEMBER m_wheatFront NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QColor mote MEMBER m_mote NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QColor bird MEMBER m_bird NOTIFY colorsChanged FINAL)

public:
    explicit GleaningScene(QQuickItem* parent = nullptr);

    bool running() const { return m_running; }
    void setRunning(bool running);
    qreal leadingInset() const { return m_leadingInset; }
    void setLeadingInset(qreal inset);

    void paint(QPainter* painter) override;

signals:
    void runningChanged();
    void leadingInsetChanged();
    void colorsChanged();

private:
    void tick();

    bool m_running = false;
    qreal m_leadingInset = 0;
    double m_time = 0; // seconds of animation so far
    QElapsedTimer m_clock; // since the last tick
    QTimer m_timer;
    QColor m_wheatBack;
    QColor m_wheatFront;
    QColor m_mote;
    QColor m_bird;
};

} // namespace qf
