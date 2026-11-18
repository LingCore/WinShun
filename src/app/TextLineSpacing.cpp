#include "TextLineSpacing.h"

#include <QScopedValueRollback>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>

namespace ws {

void TextLineSpacing::setTextDocument(QQuickTextDocument* document)
{
    if (m_quick == document)
        return;
    disconnect(m_changed);
    m_quick = document;
    if (QTextDocument* text = document ? document->textDocument() : nullptr) {
        text->setUndoRedoEnabled(false); // read only: nothing to undo, no copies of each format change kept
        m_changed = connect(text, &QTextDocument::contentsChanged, this, &TextLineSpacing::apply);
    }
    emit textDocumentChanged();
    apply();
}

void TextLineSpacing::setFactor(qreal factor)
{
    if (qFuzzyCompare(m_factor, factor))
        return;
    m_factor = factor;
    emit factorChanged();
    apply();
}

void TextLineSpacing::apply()
{
    QTextDocument* document = m_quick ? m_quick->textDocument() : nullptr;
    if (!document || m_applying || document->isEmpty())
        return;
    const QScopedValueRollback guard(m_applying, true); // the format change is a change of contents too
    QTextBlockFormat format;
    format.setLineHeight(m_factor * 100, QTextBlockFormat::ProportionalHeight);
    QTextCursor cursor(document);
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(format);
}

} // namespace ws
