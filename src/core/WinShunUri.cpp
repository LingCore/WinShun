#include "WinShunUri.h"

#include <QByteArray>
#include <QList>
#include <QStringView>

#include <utility>

namespace ws::winshunuri {

namespace {

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Percent-decoded as UTF-8; `form`: "+" is a space (a form's value). A "%" that
// is not followed by two hex digits stays as typed (QByteArray::fromPercentEncoding
// makes something of "%ZZ").
QString decode(QStringView encoded, bool form)
{
    const QByteArray bytes = encoded.toUtf8();
    QByteArray decoded;
    decoded.reserve(bytes.size());
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const char c = bytes[i];
        if (c == '+' && form) { // before decoding, as it is here: %2B stays a plus
            decoded += ' ';
        } else if (c == '%' && i + 2 < bytes.size() &&hexValue(bytes[i + 1]) >= 0 && hexValue(bytes[i + 2]) >= 0) {
            decoded += static_cast<char>(hexValue(bytes[i + 1]) * 16 + hexValue(bytes[i + 2]));
            i += 2;
        } else {
            decoded += c;
        }
    }
    return QString::fromUtf8(decoded);
}

bool isLineBreak(char16_t c)
{
    return c == u'\t' || c == u'\n' || c == u'\r' || c == 0x0B || c == 0x0C || c == 0x85 || c == 0x2028 || c == 0x2029;
}

// Controls that change how text reads rather than what it says (the direction of what follows).
bool isDirectionControl(char16_t c)
{
    return c == 0x200E || c == 0x200F || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
}

// What may be typed from a link: no control characters (a line break or a tab
// becomes a space), no direction controls, at most `limit` units.
QString cleaned(const QString& text, int limit)
{
    QString out;
    out.reserve(qMin<qsizetype>(text.size(), limit + 1));
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        if (isLineBreak(u))
            out += u' ';
        else if (u < 0x20 || (u >= 0x7F && u <= 0x9F) || isDirectionControl(u))
            continue;
        else
            out += c;
        if (out.size() > limit)
            break;
    }
    if (out.size() > limit) {
        out.truncate(limit);
        if (out.back().isHighSurrogate())
            out.chop(1); // never half a pair
    }
    return out;
}

// The value of `name` (q, query) in "a=1&q=x&b=2", decoded as a form.
QString formValue(QStringView form, QStringView first, QStringView second)
{
    for (const QStringView pair : form.split(u'&', Qt::SkipEmptyParts)) {
        const qsizetype eq = pair.indexOf(u'=');
        if (eq < 0)
            continue;
        const QString key = decode(pair.left(eq), true);
        if (key.compare(first, Qt::CaseInsensitive) == 0 || key.compare(second, Qt::CaseInsensitive) == 0)
            return decode(pair.mid(eq + 1), true);
    }
    return {};
}

Request search(QString text)
{
    text = cleaned(text, kMaxTextChars);
    if (text.trimmed().isEmpty())
        return {}; // nothing to type: the search box as it is
    return {Kind::Search, text, {}};
}

// The rest of a path up to a form or an anchor, decoded, in lower case, without slashes at its end.
QString lastPart(QStringView text)
{
    const qsizetype stop = text.indexOf(u'?');
    if (stop >= 0)
        text = text.left(stop);
    const qsizetype hash = text.indexOf(u'#');
    if (hash >= 0)
        text = text.left(hash);
    QString part = decode(text, false).trimmed().toLower();
    while (part.endsWith(u'/'))
        part.chop(1);
    return part;
}

// "Clipboard", "files-and-dialogs" and "files_and_dialogs/" all name a page;
// anything with other characters in it names none.
QString pageId(QStringView text)
{
    QString page = lastPart(text);
    page.replace(u'-', u'_');
    if (page.isEmpty() || page.size() > kMaxPageChars)
        return {};
    for (const QChar c : std::as_const(page)) {
        const char16_t u = c.unicode();
        if (!((u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9') || u == u'_'))
            return {};
    }
    return page;
}

// "winshun.capture" and "WinShun.Capture/" name a plugin (as plugin::validPluginId reads ids); "a/b" none.
QString pluginId(QStringView text)
{
    const QString id = lastPart(text);
    if (id.isEmpty() || id.size() > kMaxPageChars || id.startsWith(u'.') || id.endsWith(u'.'))
        return {};
    for (const QChar c : id) {
        const char16_t u = c.unicode();
        if (!((u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9') || u == u'.' || u == u'_' || u == u'-'))
            return {};
    }
    return id;
}

} // namespace

Request parse(const QString& link)
{
    QStringView rest = QStringView(link).trimmed();
    constexpr QStringView scheme = u"winshun:";
    if (!rest.startsWith(scheme, Qt::CaseInsensitive))
        return {};
    rest = rest.mid(scheme.size());
    if (rest.startsWith(u"//"))
        rest = rest.mid(2);

    qsizetype end = 0;
    while (end < rest.size() && rest[end] != u'/' && rest[end] != u'?' && rest[end] != u'#')
        ++end;
    const QStringView word = rest.left(end);
    const QStringView tail = rest.mid(end); // "", or begins with "/", "?" or "#"

    if (word.compare(u"search", Qt::CaseInsensitive) == 0) {
        QStringView text;
        if (tail.startsWith(u'/'))
            text = tail.mid(1);
        else if (tail.startsWith(u'?'))
            text = tail; // the form, below
        else
            return {};
        if (text.startsWith(u'?')) // "search?q=" and "search/?q="
            return search(formValue(text.mid(1), u"q", u"query"));
        return search(decode(text, false)); // after "search/": all of it, "?" and "#" too
    }
    if (word.compare(u"settings", Qt::CaseInsensitive) == 0) {
        Request request;
        request.kind = Kind::Settings;
        if (!tail.startsWith(u'/'))
            return request;
        const QStringView path = tail.mid(1);
        constexpr QStringView plugin = u"plugin/";
        if (path.startsWith(plugin, Qt::CaseInsensitive))
            request.plugin = pluginId(path.mid(plugin.size()));
        else
            request.page = pageId(path);
        return request;
    }
    return {};
}

} // namespace ws::winshunuri
