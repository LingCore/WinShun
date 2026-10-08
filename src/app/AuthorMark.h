#pragma once

#include <QColor>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

namespace ws {

// The author's "cursor and rice ear" mark (red version), animated: the cursor
// opens up into the badge; a stalk grows from behind it and sets grain; the
// fuller the ear, the more it bends the stalk, sways twice and settles; a
// gleam of light sweeps across. The last frame is the static icon.
// Ported from MacShun (AuthorMark.swift); geometry in 48-unit icon space.
//
// The stalk and grains reach slightly past the badge, so the item is the
// icon plus `margin` units on every side: an icon of size s needs an item of
// s * (48 + 2 * margin) / 48. `time` is seconds since the animation started
// (AuthorClock.qml); restTime or later draws the static icon.
class AuthorMark : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(qreal time READ time WRITE setTime NOTIFY timeChanged FINAL)
    // The cursor's colour at the start, before the badge opens behind it (it
    // is drawn straight onto the background then).
    Q_PROPERTY(QColor startInk READ startInk WRITE setStartInk NOTIFY startInkChanged FINAL)
    Q_PROPERTY(qreal margin READ margin CONSTANT FINAL)
    Q_PROPERTY(qreal restTime READ restTime CONSTANT FINAL)

public:
    explicit AuthorMark(QQuickItem* parent = nullptr);

    qreal time() const { return m_time; }
    void setTime(qreal time);
    QColor startInk() const { return m_startInk; }
    void setStartInk(const QColor& color);
    static qreal margin();
    static qreal restTime();

    void paint(QPainter* painter) override;

signals:
    void timeChanged();
    void startInkChanged();

private:
    qreal m_time = 7.9;
    QColor m_startInk {242, 237, 227};
};

} // namespace ws
