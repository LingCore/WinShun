#pragma once

#include <QColor>
#include <QImage>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace ws::capture {

class FormulaPicture;

// The formula window's drawing of the LaTeX being edited (FormulaPicture,
// MicroTeX): centred, made smaller to fit and never larger than
// `fontSize`. Laid out again as the text changes; a command MicroTeX does
// not know is drawn in red, a formula it cannot lay out at all (a brace
// left open) leaves `error` saying so.
class FormulaView : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString latex READ latex WRITE setLatex NOTIFY latexChanged FINAL)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged FINAL)
    Q_PROPERTY(qreal fontSize READ fontSize WRITE setFontSize NOTIFY fontSizeChanged FINAL)
    Q_PROPERTY(QString error READ error NOTIFY drawn FINAL)
    Q_PROPERTY(qreal scale READ drawnScale NOTIFY drawn FINAL) // < 1: made smaller to fit

public:
    explicit FormulaView(QQuickItem* parent = nullptr);
    ~FormulaView() override;

    QString latex() const { return m_latex; }
    void setLatex(const QString& latex);
    QColor color() const { return m_color; }
    void setColor(const QColor& color);
    qreal fontSize() const { return m_fontSize; }
    void setFontSize(qreal size);
    QString error() const { return m_error; }
    qreal drawnScale() const { return m_scale; }

    void paint(QPainter* painter) override;

signals:
    void latexChanged();
    void colorChanged();
    void fontSizeChanged();
    void drawn();

protected:
    void geometryChange(const QRectF& now, const QRectF& before) override;

private:
    void layOut();
    void fit(); // the scale for the item's size
    QString m_latex;
    QColor m_color = Qt::black;
    qreal m_fontSize = 26;
    QString m_error;
    qreal m_scale = 1;
    std::unique_ptr<FormulaPicture> m_picture;
};

// The picture the formula was read from: centred, made smaller to fit, at
// most one of its pixels to one of the screen's (a capture's own size).
class PictureView : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QImage image READ image WRITE setImage NOTIFY imageChanged FINAL)

public:
    explicit PictureView(QQuickItem* parent = nullptr);
    QImage image() const { return m_image; }
    void setImage(const QImage& image);
    void paint(QPainter* painter) override;

signals:
    void imageChanged();

private:
    QImage m_image;
};

} // namespace ws::capture
