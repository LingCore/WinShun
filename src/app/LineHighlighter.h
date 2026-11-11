#pragma once

#include <QColor>
#include <QList>
#include <QPointer>
#include <QQuickTextDocument>
#include <QSyntaxHighlighter>
#include <QtQml/qqmlregistration.h>

namespace ws {

// The colours of one row of the preview's text (GitHub #291): the spans the
// scanner found for the line (core/SyntaxSpans) and the places of the words
// searched for, drawn over a TextEdit's plain text, so positions in the row
// stay those of the file (the mouse's pick, Ctrl+C).
class LineHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument* textDocument READ textDocument WRITE setTextDocument NOTIFY textDocumentChanged FINAL)
    // start, length, kind (syntax::Kind), start, length, kind …
    Q_PROPERTY(QList<int> spans READ spans WRITE setSpans NOTIFY spansChanged FINAL)
    // The words searched for: start, length, start, length …
    Q_PROPERTY(QList<int> marks READ marks WRITE setMarks NOTIFY marksChanged FINAL)
    // By syntax::Kind; an invalid one leaves the text's own colour.
    Q_PROPERTY(QList<QColor> colors READ colors WRITE setColors NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QColor markColor READ markColor WRITE setMarkColor NOTIFY markColorChanged FINAL)
    Q_PROPERTY(int basePixelSize READ basePixelSize WRITE setBasePixelSize NOTIFY basePixelSizeChanged FINAL) // headings are larger

public:
    explicit LineHighlighter(QObject* parent = nullptr);

    QQuickTextDocument* textDocument() const { return m_quick; }
    void setTextDocument(QQuickTextDocument* document);
    QList<int> spans() const { return m_spans; }
    void setSpans(const QList<int>& spans);
    QList<int> marks() const { return m_marks; }
    void setMarks(const QList<int>& marks);
    QList<QColor> colors() const { return m_colors; }
    void setColors(const QList<QColor>& colors);
    QColor markColor() const { return m_markColor; }
    void setMarkColor(const QColor& color);
    int basePixelSize() const { return m_basePixelSize; }
    void setBasePixelSize(int size);

signals:
    void textDocumentChanged();
    void spansChanged();
    void marksChanged();
    void colorsChanged();
    void markColorChanged();
    void basePixelSizeChanged();

protected:
    void highlightBlock(const QString& text) override;

private:
    QPointer<QQuickTextDocument> m_quick;
    QList<int> m_spans;
    QList<int> m_marks;
    QList<QColor> m_colors;
    QColor m_markColor;
    int m_basePixelSize = 0;
};

} // namespace ws
