#include "Release.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

using namespace Qt::StringLiterals;

namespace ws::release {

namespace {

QList<int> numbers(const QString& version)
{
    const QString core = normalized(version).section(u'-', 0, 0);
    QList<int> parts;
    for (const QString& part : core.split(u'.'))
        parts.append(part.toInt()); // not a number: 0
    return parts;
}

bool isHan(char32_t c)
{
    return c >= 0x3400 && c <= 0x9FFF;
}

bool containsHan(QStringView text)
{
    for (const char32_t c : text.toUcs4())
        if (isHan(c))
            return true;
    return false;
}

// "🔍", "🖥️" (with a variation selector or zero-width joiners). Plain ASCII
// (#, *, digits) does not count, though Unicode calls some of it emoji.
bool isEmoji(QStringView text)
{
    if (text.isEmpty())
        return false;
    for (const char32_t c : text.toUcs4()) {
        const bool joiner = c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F);
        const bool pictograph = (c >= 0x2190 && c <= 0x2BFF) || (c >= 0x1F000 && c <= 0x1FAFF);
        if (!joiner && !pictograph)
            return false;
    }
    return true;
}

// "中文……。English…": split after a Chinese full stop when no Han
// character follows any more.
std::optional<std::pair<QString, QString>> splitLanguages(const QString& text)
{
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] != u'。' && text[i] != u'！' && text[i] != u'？')
            continue;
        const QString rest = text.mid(i + 1).trimmed();
        if (!rest.isEmpty() && !containsHan(rest))
            return std::pair {text.left(i + 1).trimmed(), rest};
    }
    return std::nullopt;
}

} // namespace

QString normalized(const QString& tag)
{
    const QString t = tag.trimmed();
    return t.startsWith(u'v', Qt::CaseInsensitive) ? t.mid(1) : t;
}

bool isNewer(const QString& candidate, const QString& base)
{
    const QList<int> a = numbers(candidate);
    const QList<int> b = numbers(base);
    for (qsizetype i = 0; i < std::max(a.size(), b.size()); ++i) {
        const int x = a.value(i);
        const int y = b.value(i);
        if (x != y)
            return x > y;
    }
    return false;
}

std::optional<Info> parse(const QByteArray& json)
{
    const QJsonObject release = QJsonDocument::fromJson(json).object();
    const QString tag = release.value(u"tag_name"_s).toString();
    if (tag.isEmpty())
        return std::nullopt;
    Info info;
    info.version = normalized(tag);
    info.pageUrl = release.value(u"html_url"_s).toString();
    info.notes = release.value(u"body"_s).toString();
    return info;
}

QString summary(const QString& notes, bool chinese)
{
    const QStringList lines = notes.split(u'\n');
    qsizetype start = 0;
    while (start < lines.size() && lines[start].trimmed().isEmpty())
        ++start;
    if (start == lines.size() || lines[start].startsWith(u'#'))
        return {};
    QStringList paragraph;
    for (qsizetype i = start; i < lines.size() && !lines[i].trimmed().isEmpty(); ++i)
        paragraph.append(lines[i].trimmed());
    const QString text = paragraph.join(u' ');
    const auto halves = splitLanguages(text);
    if (!halves)
        return text;
    return chinese ? halves->first : halves->second;
}

QList<Highlight> highlights(const QString& notes, bool chinese)
{
    const QStringList lines = notes.split(u'\n');
    qsizetype start = -1;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        if (lines[i].startsWith(u"## "_s) && lines[i].contains(u"what's new"_s, Qt::CaseInsensitive)) {
            start = i;
            break;
        }
    }
    if (start < 0)
        return {};

    struct Item {
        QString zh;
        QStringList en;
    };
    QList<Item> items;
    for (qsizetype i = start + 1; i < lines.size(); ++i) {
        QString line = lines[i];
        if (line.endsWith(u'\r'))
            line.chop(1);
        if (line.startsWith(u'#'))
            break;
        if (line.startsWith(u"- "_s))
            items.append({line.mid(2).trimmed(), {}});
        else if (!items.isEmpty() && (line.startsWith(u' ') || line.startsWith(u'\t')) && !line.trimmed().isEmpty())
            items.last().en.append(line.trimmed());
    }

    QList<Highlight> result;
    for (const Item& item : items) {
        Highlight h;
        QString zh = item.zh;
        // The leading emoji becomes the bullet, for the English line as well.
        if (const qsizetype space = zh.indexOf(u' '); space > 0 && isEmoji(QStringView(zh).left(space))) {
            h.symbol = zh.left(space);
            zh = zh.mid(space + 1);
        }
        h.text = chinese || item.en.isEmpty() ? zh : item.en.join(u' ');
        result.append(h);
    }
    return result;
}

} // namespace ws::release
