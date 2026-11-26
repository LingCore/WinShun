#pragma once

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QPointF>
#include <QSizeF>
#include <QString>

#include <memory>

class QPainter;

namespace microtex {
class Render;
}

namespace ws::capture {

// A formula's LaTeX laid out as TeX lays it out (MicroTeX with Latin Modern
// Math, third_party/microtex): 公式识别's preview, and the picture and the
// SVG it copies and saves. LaTeX that cannot be laid out gives no picture
// and the reason in error(). From any thread: MicroTeX's one state is
// behind a lock.
class FormulaPicture {
public:
    // `size`: the letters' size in pixels (1 em); `color` their colour.
    FormulaPicture(const QString& latex, qreal size, const QColor& color);
    ~FormulaPicture();
    FormulaPicture(FormulaPicture&&) noexcept;
    FormulaPicture& operator=(FormulaPicture&&) noexcept;

    bool isValid() const { return m_render != nullptr; }
    // Why the LaTeX could not be laid out, as MicroTeX says it (English);
    // empty when it was.
    QString error() const { return m_error; }
    // The formula's width and height (above and below the baseline) in pixels.
    QSizeF size() const { return m_size; }

    void draw(QPainter& painter, QPointF topLeft) const;
    // The formula on `background` with `margin` pixels around it, at `scale`
    // device pixels to a pixel (the screen's, or more for a sharp copy).
    QImage image(qreal scale, qreal margin, const QColor& background) const;
    // The formula as SVG: its letters as outlines, so it looks the same
    // everywhere; `margin` pixels around it, no background.
    QByteArray svg(qreal margin) const;

private:
    std::unique_ptr<microtex::Render> m_render;
    QString m_error;
    QSizeF m_size;
};

} // namespace ws::capture
