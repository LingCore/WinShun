#include "JsonTool.h"
#include "Launcher.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QStringDecoder>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace ws::devtools {

namespace {
struct Text {
    Q_DECLARE_TR_FUNCTIONS(JsonFile)
};

constexpr qsizetype kKeptOnClose = 1024 * 1024; // characters
} // namespace

std::optional<QString> readTextFile(const QString& path, QString* why)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (why)
            *why = Text::tr("打不开“%1”：%2").arg(QFileInfo(path).fileName(), file.errorString());
        return std::nullopt;
    }
    if (file.size() > kMostCharacters) {
        if (why)
            *why = Text::tr("“%1”有 %2，太大了：最多 32 MB").arg(QFileInfo(path).fileName(), sizeText(file.size()));
        return std::nullopt;
    }
    const QByteArray bytes = file.readAll();
    // A BOM says which; without one it is UTF-8 (what JSON is), wrong bytes
    // shown as U+FFFD rather than read as some code page.
    QStringDecoder decoder(QStringConverter::encodingForData(bytes).value_or(QStringConverter::Utf8));
    QString text = decoder(bytes);
    return text;
}

JsonTool::JsonTool(QObject* parent)
    : QObject(parent)
{
    m_reading.empty = true; // nothing yet: the window asks for JSON
    m_pause.setSingleShot(true);
    m_pause.setInterval(250);
    connect(&m_pause, &QTimer::timeout, this, &JsonTool::readNow);
}

void JsonTool::open(const QString& text, const QString& source)
{
    m_pause.stop();
    m_input = text;
    m_source = source;
    emit inputChanged();
    readNow();
}

void JsonTool::setIndent(const QString& indent)
{
    m_indent = indent.isEmpty() ? u"  "_s : indent;
    m_tree.setIndent(m_indent);
}

void JsonTool::edit(const QString& text)
{
    if (text == m_input)
        return;
    m_input = text;
    m_pause.start();
}

void JsonTool::paste()
{
    const QMimeData* data = QGuiApplication::clipboard()->mimeData();
    if (!data || !data->hasText())
        return;
    open(data->text());
}

bool JsonTool::openFile(const QUrl& file)
{
    const QString path = file.toLocalFile();
    QString why;
    const std::optional<QString> text = path.isEmpty() ? std::nullopt : readTextFile(path, &why);
    if (!text) {
        emit failed(why.isEmpty() ? tr("只能打开本机的文件") : why);
        return false;
    }
    open(*text, QFileInfo(path).fileName());
    return true;
}

void JsonTool::release()
{
    if (m_input.size() <= kKeptOnClose)
        return;
    open(QString());
}

void JsonTool::reformat(bool pretty)
{
    if (!valid())
        return;
    m_pause.stop();
    m_input = write(*m_document, 0, pretty ? Layout::Pretty : Layout::Compact, m_indent);
    // Plain JSON now: what was done to read it (the quotes taken off, the
    // log lines) is no longer so. The tree stays as it was opened.
    m_reading.notes.clear();
    emit inputChanged();
    emit readChanged();
}

bool JsonTool::copy(const QString& form)
{
    if (!valid())
        return false;
    QString text;
    if (form == u"pretty")
        text = write(*m_document, 0, Layout::Pretty, m_indent);
    else if (form == u"escaped")
        text = escaped(write(*m_document, 0, Layout::Compact));
    else
        text = write(*m_document, 0, Layout::Compact);
    copyText(text, form);
    return true;
}

void JsonTool::copyText(const QString& text, const QString& what)
{
    QGuiApplication::clipboard()->setText(text);
    emit copied(what);
}

QString JsonTool::inputSize() const
{
    return sizeText(m_input.toUtf8().size());
}

QString JsonTool::summary() const
{
    return valid() ? describe(*m_document, 0) : QString();
}

void JsonTool::readNow()
{
    Reading reading = read(m_input);
    auto document = std::make_shared<Document>(std::move(reading.document));
    reading.document = {};
    m_reading = std::move(reading);
    m_document = document;
    m_tree.setDocument(document);
    emit readChanged();
}

} // namespace ws::devtools
