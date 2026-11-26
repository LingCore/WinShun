#include "FormulaView.h"

#include "FormulaPicture.h"

#include <QPainter>
#include <QQuickWindow>

#include <algorithm>
#include <cmath>

namespace ws::capture {

FormulaView::FormulaView(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
}

FormulaView::~FormulaView() = default;

void FormulaView::setLatex(const QString& latex)
{
    if (latex == m_latex)
        return;
    m_latex = latex;
    emit latexChanged();
    layOut();
}

void FormulaView::setColor(const QColor& color)
{
    if (color == m_color)
        return;
    m_color = color;
    emit colorChanged();
    layOut();
}

void FormulaView::setFontSize(qreal size)
{
    if (qFuzzyCompare(size, m_fontSize) || size <= 0)
        return;
    m_fontSize = size;
    emit fontSizeChanged();
    layOut();
}

void FormulaView::layOut()
{
    m_picture.reset();
    m_error.clear();
    if (!m_latex.trimmed().isEmpty()) {
        auto picture = std::make_unique<FormulaPicture>(m_latex, m_fontSize, m_color);
        if (picture->isValid())
            m_picture = std::move(picture);
        else
            m_error = picture->error();
    }
    fit();
    emit drawn();
}

void FormulaView::fit()
{
    qreal scale = 1;
    if (m_picture && !m_picture->size().isEmpty() && width() > 0 && height() > 0)
        scale = std::min({1.0, width() / m_picture->size().width(), height() / m_picture->size().height()});
    if (!qFuzzyCompare(scale, m_scale)) {
        m_scale = scale;
        emit drawn();
    }
    update();
}

void FormulaView::geometryChange(const QRectF& now, const QRectF& before)
{
    QQuickPaintedItem::geometryChange(now, before);
    fit();
}

void FormulaView::paint(QPainter* painter)
{
    if (!m_picture)
        return;
    const QSizeF size = m_picture->size() * m_scale;
    painter->setRenderHint(QPainter::Antialiasing);
    painter->translate((width() - size.width()) / 2, (height() - size.height()) / 2);
    painter->scale(m_scale, m_scale);
    m_picture->draw(*painter, {0, 0});
}

PictureView::PictureView(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
}

void PictureView::setImage(const QImage& image)
{
    m_image = image;
    emit imageChanged();
    update();
}

void PictureView::paint(QPainter* painter)
{
    if (m_image.isNull())
        return;
    // In the item's pixels: one of the picture's to one of the screen's, or fewer.
    const qreal ratio = window() ? window()->effectiveDevicePixelRatio() : 1;
    const QSizeF natural = QSizeF(m_image.size()) / ratio;
    const qreal scale = std::min({1.0, width() / natural.width(), height() / natural.height()});
    const QSizeF size = natural * scale;
    // On whole pixels of the screen: unblurred at its own size.
    const auto snap = [ratio](qreal v) { return std::round(v * ratio) / ratio; };
    const QRectF target(snap((width() - size.width()) / 2), snap((height() - size.height()) / 2), size.width(), size.height());
    painter->setRenderHint(QPainter::SmoothPixmapTransform, scale < 1);
    painter->drawImage(target, m_image);
}

} // namespace ws::capture
