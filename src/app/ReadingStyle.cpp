#include "ReadingStyle.h"

#include <QFont>
#include <QQuickItem>
#include <QScopedValueRollback>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextList>
#include <QTextTable>

#include <cmath>

using namespace Qt::StringLiterals;

namespace ws {
namespace {
// A line of this font before any spacing, as QTextDocumentLayout measures the
// one it multiplies for a ProportionalHeight: ascent, descent and leading,
// rounded up; with the Chinese fallback in it, as the text has.
qreal naturalLine(const QFont& font)
{
    QTextLayout layout(u"中Ag"_s, font);
    layout.beginLayout();
    const QTextLine line = layout.createLine();
    layout.endLayout();
    return std::ceil(line.ascent() + line.descent() + line.leading());
}

// `em` lines of the font's size apart, as the share of its own line that
// QTextBlockFormat::ProportionalHeight takes; never closer than the font has them.
qreal lineShare(const QFont& font, qreal em)
{
    const qreal natural = naturalLine(font);
    return natural > 0 ? std::max<qreal>(100, std::round(em * font.pixelSize() / natural * 100)) : 100;
}

bool fixedPitch(const QTextCharFormat& format)
{
    return format.fontFixedPitch() || format.fontFamilies().toStringList().contains(u"Consolas"_s, Qt::CaseInsensitive);
}

// The headings, a step over the body each: 20, 17 and 15 px over 14.
int headingSize(int level, int body)
{
    switch (level) {
    case 1: return qRound(body * 1.43);
    case 2: return qRound(body * 1.21);
    case 3: return qRound(body * 1.07);
    default: return body;
    }
}
} // namespace

void ReadingStyle::setTextDocument(QQuickTextDocument* document)
{
    if (m_quick == document)
        return;
    disconnect(m_changed);
    m_quick = document;
    if (QTextDocument* text = document ? document->textDocument() : nullptr) {
        text->setUndoRedoEnabled(false); // read only: nothing to undo, no copies of each format change kept
        m_changed = connect(text, &QTextDocument::contentsChanged, this, &ReadingStyle::schedule);
    }
    emit textDocumentChanged();
    schedule();
}

void ReadingStyle::setPixelSize(int size)
{
    if (m_pixelSize == size)
        return;
    m_pixelSize = size;
    emit pixelSizeChanged();
    schedule();
}

void ReadingStyle::setColor(QColor& field, const QColor& color)
{
    if (field == color)
        return;
    field = color;
    emit colorsChanged();
    schedule();
}

void ReadingStyle::setLinkColor(const QColor& color) { setColor(m_link, color); }
void ReadingStyle::setQuietColor(const QColor& color) { setColor(m_quiet, color); }
void ReadingStyle::setRuleColor(const QColor& color) { setColor(m_rule, color); }
void ReadingStyle::setCodeColor(const QColor& color) { setColor(m_code, color); }

// After the TextEdit is done with a new text: it marks it for drawing in part
// only once the text is in (QQuickTextEdit::setText), after the document has
// said it changed. Several changes in a row are laid out once.
void ReadingStyle::schedule()
{
    if (m_applying || m_scheduled)
        return;
    m_scheduled = true;
    QMetaObject::invokeMethod(this, [this] {
        m_scheduled = false;
        apply();
    }, Qt::QueuedConnection);
}

void ReadingStyle::apply()
{
    QTextDocument* document = m_quick ? m_quick->textDocument() : nullptr;
    if (!document || m_applying)
        return;
    // All of it drawn (see the class comment). The edits below mark every
    // block for drawing again, so what was drawn in part is drawn whole.
    if (auto* item = qobject_cast<QQuickItem*>(m_quick->parent()))
        item->setFlag(QQuickItem::ItemObservesViewport, false);
    if (document->isEmpty())
        return;
    const QScopedValueRollback guard(m_applying, true); // a format change is a change of contents too

    QFont body = document->defaultFont();
    const int px = m_pixelSize > 0 ? m_pixelSize : body.pixelSize() > 0 ? body.pixelSize() : 14;
    body.setPixelSize(px);
    const auto em = [px](qreal share) { return qreal(qRound(px * share)); };
    const qreal bodyLine = lineShare(body, 1.7); // the lines of a page, for reading Chinese
    const qreal codeLine = lineShare(body, 1.45);
    const qreal paragraph = em(0.75); // under a paragraph, a list, a code block
    const qreal item = em(0.3); // between the items of a list
    document->setIndentWidth(em(1.6)); // a list's indent, with its marker in it

    struct Run {
        int start;
        int length;
        QTextCharFormat format;
    };
    QList<Run> runs;
    QTextCursor edit(document);
    edit.beginEditBlock();
    bool previousCode = false;
    bool previousRule = false;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat format = block.blockFormat();
        const QTextBlock next = block.next();
        const int level = format.headingLevel();
        const bool rule = format.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth);
        const bool code = format.nonBreakableLines() && level == 0;
        const bool nextCode = next.isValid() && next.blockFormat().nonBreakableLines() && next.blockFormat().headingLevel() == 0;
        const bool listed = block.textList() != nullptr || format.indent() > 0;
        const bool nextListed = next.isValid() && (next.textList() != nullptr || next.blockFormat().indent() > 0)
            && next.blockFormat().headingLevel() == 0;
        // A quote: by its level, or from HTML by the margins a Markdown quote
        // had in it (40 px on the left a level, 40 on the right; toHtml keeps
        // those, not the level). Its level kept for the next time.
        int quote = format.intProperty(QTextFormat::BlockQuoteLevel);
        if (quote == 0 && level == 0 && format.rightMargin() > 0 && format.leftMargin() > 0) {
            quote = std::max(1, qRound(format.leftMargin() / 40));
            format.setProperty(QTextFormat::BlockQuoteLevel, quote);
        }
        QTextCursor at(block);
        const bool first = block == document->begin();

        qreal top = 0;
        qreal bottom = paragraph;
        qreal line = bodyLine;
        int size = 0; // a heading's pixel size
        if (at.currentTable()) { // its own lines; the cell's padding is the room round them, even above and under
            bottom = 0;
            line = 100;
        } else if (level > 0) { // room above it, its own lines close under it
            size = headingSize(level, px);
            QFont heading = body;
            heading.setPixelSize(size);
            heading.setBold(true);
            // Under a rule, the rule is what parts it from what was before.
            top = first ? 0 : previousRule ? em(1.0) : em(level == 1 ? 2.0 : level == 2 ? 1.7 : level == 3 ? 1.3 : 1.1);
            bottom = em(level <= 2 ? 0.5 : 0.35);
            line = lineShare(heading, 1.3);
        } else if (rule) {
            top = bottom = em(0.5);
            line = 100;
            // Across the column (from HTML it came as a length of nothing), in
            // the rule's colour: QQuickTextNodeEngine takes the block's background for it.
            format.setProperty(QTextFormat::BlockTrailingHorizontalRulerWidth, QTextLength(QTextLength::PercentageLength, 100));
            if (m_rule.isValid())
                format.setBackground(m_rule);
        } else if (code) {
            top = previousCode ? 0 : em(0.25);
            bottom = nextCode ? 0 : paragraph;
            line = codeLine;
            format.setLeftMargin(em(1));
        } else if (listed) {
            bottom = nextListed ? item : paragraph;
        } else if (block.text().isEmpty()) {
            bottom = 0;
        }
        if (quote > 0) {
            format.setLeftMargin(quote * em(1.2));
            format.setRightMargin(0);
        }
        format.setTopMargin(top);
        format.setBottomMargin(bottom);
        format.setLineHeight(line, QTextBlockFormat::ProportionalHeight);
        at.setBlockFormat(format);
        previousCode = code;
        previousRule = rule;

        // Its characters: changed once all of the block is read, as changing
        // one can merge or split the fragments being read.
        runs.clear();
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            QTextCharFormat chars = fragment.charFormat();
            if (size > 0) {
                chars.clearProperty(QTextFormat::FontSizeAdjustment);
                chars.clearProperty(QTextFormat::FontPointSize);
                chars.setProperty(QTextFormat::FontPixelSize, size);
                chars.setFontWeight(QFont::Bold);
            }
            const bool link = chars.isAnchor() && !chars.anchorHref().isEmpty();
            if (link && m_link.isValid())
                chars.setForeground(m_link);
            else if (quote > 0 && m_quiet.isValid())
                chars.setForeground(m_quiet);
            if (!code && fixedPitch(chars) && m_code.isValid() && m_code.alpha() > 0)
                chars.setBackground(m_code);
            runs.append({fragment.position(), fragment.length(), chars});
        }
        for (const Run& run : std::as_const(runs)) {
            QTextCursor span(document);
            span.setPosition(run.start);
            span.setPosition(run.start + run.length, QTextCursor::KeepAnchor);
            span.setCharFormat(run.format);
        }
    }

    // Tables: lines in the rule's colour, one between two cells.
    const std::function<void(QTextFrame*)> tables = [&](QTextFrame* frame) {
        for (QTextFrame* child : frame->childFrames()) {
            if (auto* table = qobject_cast<QTextTable*>(child)) {
                QTextTableFormat format = table->format();
                format.setBorder(1);
                format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
                if (m_rule.isValid())
                    format.setBorderBrush(m_rule);
                format.setBorderCollapse(true);
                format.setCellSpacing(0);
                format.setCellPadding(em(0.5));
                format.setTopMargin(em(0.25));
                format.setBottomMargin(paragraph);
                table->setFormat(format);
            }
            tables(child);
        }
    };
    tables(document->rootFrame());
    edit.endEditBlock();
}

} // namespace ws
