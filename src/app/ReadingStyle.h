#pragma once

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QtQml/qqmlregistration.h>

namespace ws {

// A TextEdit's rich text laid out for reading: a plugin's 使用说明 and the
// 更新内容 of a release (PluginReadme). Headings a step or two over the body
// with room above them, paragraphs and list items spaced as on a page, lines
// as far apart as the font allows to read them, links in the accent, quotes
// quieter, tables and rules in the divider's colour. Laid out again whenever
// the text changes, as a new text starts with the plain formats.
//
// It also has the TextEdit draw all of its text at once. Over 10 000
// characters (a README's HTML soon is) Qt's TextEdit draws only the blocks in
// its viewport and keeps a "rendered region" that it grows past what it drew
// when the view jumps: a jump made by dragging the scroll bar then showed an
// empty page, and Qt drew nothing more there.
class ReadingStyle : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument* textDocument READ textDocument WRITE setTextDocument NOTIFY textDocumentChanged FINAL)
    Q_PROPERTY(int pixelSize READ pixelSize WRITE setPixelSize NOTIFY pixelSizeChanged FINAL) // the body's: the TextEdit's font.pixelSize
    Q_PROPERTY(QColor linkColor READ linkColor WRITE setLinkColor NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QColor quietColor READ quietColor WRITE setQuietColor NOTIFY colorsChanged FINAL) // block quotes
    Q_PROPERTY(QColor ruleColor READ ruleColor WRITE setRuleColor NOTIFY colorsChanged FINAL) // --- and table lines
    Q_PROPERTY(QColor codeColor READ codeColor WRITE setCodeColor NOTIFY colorsChanged FINAL) // under `code`; transparent: none

public:
    using QObject::QObject;

    QQuickTextDocument* textDocument() const { return m_quick; }
    void setTextDocument(QQuickTextDocument* document);
    int pixelSize() const { return m_pixelSize; }
    void setPixelSize(int size);
    QColor linkColor() const { return m_link; }
    void setLinkColor(const QColor& color);
    QColor quietColor() const { return m_quiet; }
    void setQuietColor(const QColor& color);
    QColor ruleColor() const { return m_rule; }
    void setRuleColor(const QColor& color);
    QColor codeColor() const { return m_code; }
    void setCodeColor(const QColor& color);

signals:
    void textDocumentChanged();
    void pixelSizeChanged();
    void colorsChanged();

private:
    void schedule();
    void apply();
    void setColor(QColor& field, const QColor& color);

    QPointer<QQuickTextDocument> m_quick;
    QMetaObject::Connection m_changed;
    int m_pixelSize = 0;
    QColor m_link;
    QColor m_quiet;
    QColor m_rule;
    QColor m_code;
    bool m_applying = false;
    bool m_scheduled = false;
};

} // namespace ws
