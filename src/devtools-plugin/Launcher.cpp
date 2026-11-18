#include "Launcher.h"

#include <QCoreApplication>
#include <QLocale>

using namespace Qt::StringLiterals;

namespace ws::devtools {

namespace {

struct Text {
    Q_DECLARE_TR_FUNCTIONS(Launcher)
};

constexpr qsizetype kMostCopiedInRow = 64 * 1024; // a row's own Ctrl+C; past this only Enter copies (from the plugin)

QString origin(From from)
{
    switch (from) {
    case From::Typed: return Text::tr("输入的内容");
    case From::Clipboard: return Text::tr("剪贴板");
    case From::Pasted: return Text::tr("粘贴的内容");
    case From::File: return Text::tr("文件");
    }
    return {};
}

// The start of `text` on one line, for a subtitle.
QString sample(const QString& text, int most)
{
    QString line = text.first(std::min<qsizetype>(text.size(), most + 1)).simplified();
    if (line.size() > most || text.size() > most + 1)
        line = line.first(std::min<qsizetype>(line.size(), most)) + u'…';
    return line;
}

QJsonObject row(const QString& id, const QString& title, const QString& subtitle, const QString& icon)
{
    return {{u"id"_s, id}, {u"title"_s, title}, {u"subtitle"_s, subtitle}, {u"icon"_s, icon}, {u"data"_s, QJsonObject {{u"do"_s, id}}}};
}

} // namespace

QString sizeText(qsizetype bytes)
{
    if (bytes < 1024)
        return u"%1 B"_s.arg(bytes);
    if (bytes < 1024 * 1024)
        return u"%1 KB"_s.arg(QLocale::c().toString(double(bytes) / 1024, 'f', 1));
    return u"%1 MB"_s.arg(QLocale::c().toString(double(bytes) / 1024 / 1024, 'f', 1));
}

QJsonArray rows(const QString& text, From from, const Reading& reading, bool full, const QString& indent)
{
    QJsonArray out;
    if (reading.empty) {
        if (full)
            out.append(row(u"view"_s, Text::tr("打开 JSON 工具"),
                from == From::Clipboard ? Text::tr("剪贴板里没有文字，打开后粘贴或输入 JSON") : Text::tr("在 json 后面粘贴 JSON，或者先复制再输入 json"),
                u"ph:brackets-curly"_s));
        return out;
    }
    if (reading.error) {
        if (!full)
            return out;
        if (!text.contains(u'{') && !text.contains(u'[')) {
            out.append(row(u"view"_s, Text::tr("%1里不是 JSON").arg(origin(from)), Text::tr("Enter 打开 JSON 工具，粘贴或输入 JSON"),
                u"ph:brackets-curly"_s));
            return out;
        }
        out.append(row(u"view"_s, Text::tr("JSON 有错误：%1").arg(reading.error->message),
            Text::tr("%1 · 第 %2 行第 %3 列 · Enter 打开窗口查看和修改").arg(origin(from)).arg(reading.error->line).arg(reading.error->column),
            u"ph:warning-circle"_s));
        return out;
    }
    const Document& d = reading.document;
    QStringList facts {origin(from), describe(d, 0), sizeText(text.toUtf8().size())};
    if (!reading.notes.isEmpty())
        facts << reading.notes.first();
    QJsonObject view = row(u"view"_s, Text::tr("查看 JSON"), facts.join(u" · "), u"ph:tree-view"_s);
    view.insert(u"details"_s, true); // the preview (space) shows it formatted
    if (!full) {
        view.insert(u"score"_s, 90);
        out.append(view);
        return out;
    }
    out.append(view);

    const QString pretty = write(d, 0, Layout::Pretty, indent);
    const QString compact = write(d, 0, Layout::Compact);
    const QString inString = escaped(compact);
    const int lines = int(pretty.count(u'\n')) + 1;
    const QString indentName = indent == u"\t" ? Text::tr("Tab 缩进") : Text::tr("%n 个空格缩进", "", int(indent.size()));
    QJsonObject prettyRow = row(u"pretty"_s, Text::tr("复制格式化后的 JSON"), Text::tr("%n 行", "", lines) + u" · " + indentName, u"ph:text-indent"_s);
    QJsonObject compactRow = row(u"compact"_s, Text::tr("复制压缩后的 JSON"), sample(compact, 80), u"ph:arrows-in-line-horizontal"_s);
    QJsonObject escapedRow = row(u"escaped"_s, Text::tr("复制转义后的 JSON"), Text::tr("放进字符串里用：%1").arg(sample(inString, 72)), u"ph:quotes"_s);
    if (pretty.size() <= kMostCopiedInRow) {
        prettyRow.insert(u"copy"_s, pretty);
        compactRow.insert(u"copy"_s, compact);
        escapedRow.insert(u"copy"_s, inString);
    }
    out.append(prettyRow);
    out.append(compactRow);
    out.append(escapedRow);
    return out;
}

QString copied(const Reading& reading, const QString& form, const QString& indent)
{
    if (reading.document.isEmpty())
        return {};
    if (form == u"pretty")
        return write(reading.document, 0, Layout::Pretty, indent);
    const QString compact = write(reading.document, 0, Layout::Compact);
    return form == u"escaped" ? escaped(compact) : compact;
}

} // namespace ws::devtools
