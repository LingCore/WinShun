#include "LineHighlighter.h"

#include "SyntaxSpans.h"

#include <QFont>
#include <QTextCharFormat>
#include <QTextDocument>

#include <algorithm>

namespace ws {

LineHighlighter::LineHighlighter(QObject* parent)
    : QSyntaxHighlighter(parent)
{
}

void LineHighlighter::setTextDocument(QQuickTextDocument* document)
{
    if (m_quick == document)
        return;
    m_quick = document;
    setDocument(document ? document->textDocument() : nullptr);
    emit textDocumentChanged();
}

void LineHighlighter::setSpans(const QList<int>& spans)
{
    if (m_spans == spans)
        return;
    m_spans = spans;
    emit spansChanged();
    rehighlight();
}

void LineHighlighter::setMarks(const QList<int>& marks)
{
    if (m_marks == marks)
        return;
    m_marks = marks;
    emit marksChanged();
    rehighlight();
}

void LineHighlighter::setColors(const QList<QColor>& colors)
{
    if (m_colors == colors)
        return;
    m_colors = colors;
    emit colorsChanged();
    rehighlight();
}

void LineHighlighter::setMarkColor(const QColor& color)
{
    if (m_markColor == color)
        return;
    m_markColor = color;
    emit markColorChanged();
    rehighlight();
}

void LineHighlighter::setBasePixelSize(int size)
{
    if (m_basePixelSize == size)
        return;
    m_basePixelSize = size;
    emit basePixelSizeChanged();
    rehighlight();
}

void LineHighlighter::highlightBlock(const QString& text)
{
    using syntax::Kind;
    const int length = static_cast<int>(text.size());
    for (qsizetype i = 0; i + 2 < m_spans.size(); i += 3) {
        const int start = m_spans[i];
        const int count = std::min(m_spans[i + 1], length - start);
        const int kind = m_spans[i + 2];
        if (start < 0 || count <= 0 || kind < 0 || kind >= syntax::kKinds)
            continue;
        QTextCharFormat format;
        if (kind < m_colors.size() && m_colors[kind].isValid())
            format.setForeground(m_colors[kind]);
        switch (static_cast<Kind>(kind)) {
        case Kind::Heading1:
        case Kind::Heading2:
        case Kind::Heading3:
            format.setFontWeight(QFont::Bold);
            if (m_basePixelSize > 0) {
                const int percent = kind == static_cast<int>(Kind::Heading1) ? 150 : kind == static_cast<int>(Kind::Heading2) ? 125 : 110;
                format.setProperty(QTextFormat::FontPixelSize, m_basePixelSize * percent / 100);
            }
            break;
        case Kind::Strong:
            format.setFontWeight(QFont::Bold);
            break;
        case Kind::Emphasis:
        case Kind::Quote:
            format.setFontItalic(true);
            break;
        default:
            break;
        }
        setFormat(start, count, format);
    }
    // The words searched for: their colour and weight over whatever the span is.
    for (qsizetype i = 0; i + 1 < m_marks.size(); i += 2) {
        const int start = m_marks[i];
        const int end = std::min(start + m_marks[i + 1], length);
        for (int at = std::max(start, 0); at < end; ++at) {
            QTextCharFormat marked = format(at);
            if (m_markColor.isValid())
                marked.setForeground(m_markColor);
            marked.setFontWeight(QFont::Bold);
            setFormat(at, 1, marked);
        }
    }
}

} // namespace ws
