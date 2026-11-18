#pragma once

#include "Json.h"
#include "JsonTree.h"

#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>
#include <optional>

namespace ws::devtools {

// A text file's words: UTF-8 (a BOM aside), or UTF-16/32 with a BOM. Empty
// with `why` set when it cannot be read or is past kMostCharacters' bytes.
std::optional<QString> readTextFile(const QString& path, QString* why);

// What the JSON window shows: the text (the box on the left) and the tree
// it reads as (on the right), with what was done to read it or where it
// failed. Typing reads again after a pause; a paste, a file, the search
// box's json read at once.
class JsonTool : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Made by the plugin")
    Q_PROPERTY(QString input READ input NOTIFY inputChanged)
    Q_PROPERTY(bool editable READ editable NOTIFY inputChanged) // short enough for the box
    Q_PROPERTY(QString inputSize READ inputSize NOTIFY inputChanged)
    Q_PROPERTY(QString source READ source NOTIFY inputChanged) // the file's name, when it came from one
    Q_PROPERTY(ws::devtools::JsonTree* tree READ tree CONSTANT)
    Q_PROPERTY(bool empty READ empty NOTIFY readChanged)
    Q_PROPERTY(bool valid READ valid NOTIFY readChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY readChanged)
    Q_PROPERTY(QString notes READ notes NOTIFY readChanged)
    Q_PROPERTY(QString error READ error NOTIFY readChanged)
    Q_PROPERTY(int errorLine READ errorLine NOTIFY readChanged)
    Q_PROPERTY(int errorColumn READ errorColumn NOTIFY readChanged)
    Q_PROPERTY(int errorOffset READ errorOffset NOTIFY readChanged)

public:
    explicit JsonTool(QObject* parent = nullptr);

    // From the search box or a file: the text replaced and read now.
    void open(const QString& text, const QString& source = {});
    void setIndent(const QString& indent);
    const Document* document() const { return m_document.get(); }
    // Let go of a big text when the window closes (kept while it is small,
    // for 打开 JSON 工具 to show it again).
    void release();

    Q_INVOKABLE void edit(const QString& text); // typed in the box
    Q_INVOKABLE void paste();
    Q_INVOKABLE bool openFile(const QUrl& file); // dropped on the window; false: `failed` says why
    Q_INVOKABLE void reformat(bool pretty); // the box's text written out again
    Q_INVOKABLE bool copy(const QString& form); // "pretty", "compact", "escaped"
    Q_INVOKABLE void copyText(const QString& text, const QString& what);

    QString input() const { return m_input; }
    bool editable() const { return m_input.size() <= kMostEditable; }
    QString inputSize() const;
    QString source() const { return m_source; }
    JsonTree* tree() { return &m_tree; }
    bool empty() const { return m_reading.empty; }
    bool valid() const { return m_document && !m_document->isEmpty(); }
    QString summary() const;
    QString notes() const { return m_reading.notes.join(QStringLiteral(" · ")); }
    QString error() const { return m_reading.error ? m_reading.error->message : QString(); }
    int errorLine() const { return m_reading.error ? m_reading.error->line : 0; }
    int errorColumn() const { return m_reading.error ? m_reading.error->column : 0; }
    int errorOffset() const { return m_reading.error ? int(m_reading.error->offset) : -1; }

    // Characters the box takes: a longer text, often one line of compact
    // JSON, lays out too slowly to type in (the tree still shows it).
    static constexpr qsizetype kMostEditable = 512 * 1024;

signals:
    void inputChanged(); // replaced from outside the box: the box takes it
    void readChanged();
    void copied(const QString& what); // for the footer
    void failed(const QString& why); // a file that could not be read

private:
    void readNow();

    QString m_input;
    QString m_source;
    QString m_indent = QStringLiteral("  ");
    Reading m_reading; // its document moved to m_document, which the tree shares
    std::shared_ptr<const Document> m_document;
    JsonTree m_tree;
    QTimer m_pause;
};

} // namespace ws::devtools
