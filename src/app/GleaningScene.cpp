#include "GleaningScene.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace std::chrono_literals;

namespace ws {

namespace {

// A fixed pseudo-random number in [0, 1): the same arguments always give the
// same value, so the stalks keep their places from frame to frame.
double rnd(int i, int salt)
{
    const double x = std::sin(double(i * 127 + salt * 311)) * 43758.5453;
    return x - std::floor(x);
}

double degrees(double radians) { return radians * 180 / std::numbers::pi; }

QColor faded(QColor c, double opacity)
{
    c.setAlphaF(static_cast<float>(c.alphaF() * opacity));
    return c;
}

struct Clearing {
    double center;
    double half;
};

// No wheat in the middle of the field, behind the centred feedback note and
// footer: stalks there would only make the text look busy.
Clearing clearing(QSizeF size, double leadingInset)
{
    const double contentWidth = size.width() - leadingInset;
    return {leadingInset + contentWidth / 2, std::min(170.0, contentWidth * 0.32)};
}

// One layer of the field: 0 is the shorter, denser one at the back. Sparse,
// each stalk's place and height varied so they don't stand in a row.
void drawStalks(QPainter& p, QSizeF size, double t, int layer, const QColor& color, Clearing gap)
{
    static const QPainterPath grain = [] {
        QPainterPath path;
        path.addEllipse(QRectF(-2.6, -5.5, 5.2, 11));
        return path;
    }();

    const double spacing = layer == 0 ? 60 : 84;
    const int count = int(size.width() / spacing) + 2;
    const double maxHeight = std::min(size.height() * (layer == 0 ? 0.2 : 0.25), layer == 0 ? 150.0 : 190.0);
    const int salt = layer * 100;
    const QPen stemPen(color, layer == 0 ? 1.2 : 1.7, Qt::SolidLine, Qt::FlatCap);
    const QPen awnPen(faded(color, 0.6), 0.6, Qt::SolidLine, Qt::FlatCap);
    for (int i = 0; i < count; ++i) {
        const double x = (i - 0.5 + rnd(i, salt + 1) * 1.3) * spacing;
        // None in the clearing; the ones next to it get shorter, so its edge isn't abrupt.
        const double distance = std::abs(x - gap.center) - gap.half;
        if (distance <= 0)
            continue;
        const double taper = std::min(distance / 90, 1.0);
        const double height = maxHeight * (0.5 + rnd(i, salt + 2) * 0.5) * (0.45 + 0.55 * taper);
        const double sway = std::sin(t * (0.7 + rnd(i, salt + 3) * 0.4) + x * 0.012) * 0.08 + 0.04;
        const QPointF base(x, size.height() + 8);
        const QPointF tip(x + height * std::sin(sway), base.y() - height * std::cos(sway));
        const QPointF control(x, base.y() - height * 0.55);

        QPainterPath stem(base);
        stem.quadTo(control, tip);
        p.setPen(stemPen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(stem);

        // The ear: grains alternating left and right along the top of the stalk.
        const double angle = sway * 1.4;
        constexpr int grains = 7;
        const double headLength = std::min(height * 0.24, 44.0);
        const double scale = layer == 0 ? 0.85 : 1.1;
        for (int k = 0; k <= grains; ++k) {
            const double f = double(k) / grains;
            const QPointF at(tip.x() - std::sin(angle) * headLength * (1 - f), tip.y() + std::cos(angle) * headLength * (1 - f));
            const double side = k == grains ? 0 : (k % 2 == 0 ? 1 : -1);
            p.save();
            p.translate(at);
            p.rotate(degrees(angle) + side * 28);
            p.scale(scale, scale);
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawPath(grain);
            if (side != 0 && layer == 1) { // too small to see on the back row
                p.setPen(awnPen);
                p.drawLine(QPointF(0, -5.5), QPointF(side * 2.5, -16));
            }
            p.restore();
        }
    }
}

// Motes of light rising from the field, twinkling, fainter the higher they get.
void drawMotes(QPainter& p, QSizeF size, double t, const QColor& color)
{
    const int count = int(size.width() / 30);
    const double travel = size.height() * 0.75;
    if (travel <= 0)
        return;
    p.setPen(Qt::NoPen);
    for (int i = 0; i < count; ++i) {
        const double speed = 6 + rnd(i, 7) * 10;
        const double rise = std::fmod(t * speed + rnd(i, 8) * travel, travel);
        const double y = size.height() - rise;
        const double x = rnd(i, 9) * size.width() + std::sin(t * 0.5 + i) * 12;
        const double r = 0.8 + rnd(i, 10) * 1.4;
        const double twinkle = 0.5 + 0.5 * std::sin(t * (1.2 + rnd(i, 11)) + i);
        const double fade = 1 - rise / travel;
        p.setBrush(faded(color, (0.35 + 0.65 * twinkle) * fade));
        p.drawEllipse(QPointF(x, y), r, r);
    }
}

// A few small birds flying slowly across the sky, flapping their wings.
void drawBirds(QPainter& p, QSizeF size, double t, const QColor& color)
{
    p.setBrush(Qt::NoBrush);
    for (int i = 0; i < 3; ++i) {
        const double speed = 14 + rnd(i, 21) * 8;
        const double travel = size.width() + 120;
        const double x = std::fmod(t * speed + rnd(i, 22) * travel, travel) - 60;
        const double y = size.height() * 0.1 + i * 22 + std::sin(t * 0.6 + i * 2) * 8;
        // The title is on the left: they fade out before they reach it.
        const double fade = std::clamp((x - size.width() * 0.4) / (size.width() * 0.15), 0.0, 1.0);
        if (fade <= 0)
            continue;
        const double flap = std::sin(t * 5 + i * 1.7);
        const double span = 7.0 - i;
        QPainterPath bird(QPointF(x - span, y - flap * 4));
        bird.quadTo(QPointF(x - span * 0.4, y - 3 - flap * 2), QPointF(x, y));
        bird.quadTo(QPointF(x + span * 0.4, y - 3 - flap * 2), QPointF(x + span, y - flap * 4));
        p.setPen(QPen(faded(color, fade), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(bird);
    }
}

} // namespace

GleaningScene::GleaningScene(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    // 30 frames a second: everything drifts slowly, and each frame repaints
    // most of the window on the CPU.
    m_timer.setInterval(33ms);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &GleaningScene::tick);
    connect(this, &GleaningScene::colorsChanged, this, [this] { update(); });
}

void GleaningScene::setRunning(bool running)
{
    if (running == m_running)
        return;
    m_running = running;
    if (running) {
        m_clock.start();
        m_timer.start();
    } else {
        m_timer.stop();
    }
    emit runningChanged();
}

void GleaningScene::setLeadingInset(qreal inset)
{
    if (qFuzzyCompare(inset, m_leadingInset))
        return;
    m_leadingInset = inset;
    emit leadingInsetChanged();
    update();
}

void GleaningScene::tick()
{
    // A stalled frame (window dragged, machine busy) must not make it jump.
    m_time += std::min(m_clock.restart() / 1000.0, 0.1);
    update();
}

void GleaningScene::paint(QPainter* painter)
{
    const QSizeF size(width(), height());
    if (size.isEmpty())
        return;
    painter->setRenderHint(QPainter::Antialiasing);
    const Clearing gap = clearing(size, m_leadingInset);
    drawBirds(*painter, size, m_time, m_bird);
    drawStalks(*painter, size, m_time, 0, m_wheatBack, gap);
    drawMotes(*painter, size, m_time, m_mote);
    drawStalks(*painter, size, m_time, 1, m_wheatFront, gap);
}

} // namespace ws
