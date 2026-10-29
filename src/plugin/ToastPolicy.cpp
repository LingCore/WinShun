#include "ToastPolicy.h"

using namespace Qt::StringLiterals;

namespace ws::plugin {
namespace {

// See toastWords(). The zero width non-joiner and joiner (U+200C, U+200D)
// stay: emoji and some scripts need them, and alone they show nothing that
// could pass for words.
bool hidden(char32_t c)
{
    if (c == 0x200C || c == 0x200D)
        return false;
    const QChar::Category category = QChar::category(c);
    if (category == QChar::Other_Control || category == QChar::Other_Format)
        return true; // also U+00AD, U+061C, U+180E, U+200B, U+200E/F, U+202A-202E, U+2060-2064, U+2066-206F, U+FEFF, the tags
    return c == 0x034F // combining grapheme joiner
        || c == 0x115F || c == 0x1160 || c == 0x3164 || c == 0xFFA0 // Hangul fillers: letters that draw nothing
        || c == 0x17B4 || c == 0x17B5 // Khmer inherent vowels
        || (c >= 0x180B && c <= 0x180F) // Mongolian variation selectors
        || c == 0x2800 // braille blank
        || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF); // variation selectors
}

// Something of it shows: a letter, a number, a punctuation mark or a symbol.
bool seen(QStringView text)
{
    for (const char32_t c : text.toUcs4()) {
        if (QChar::isLetterOrNumber(c) || QChar::isPunct(c) || QChar::isSymbol(c))
            return true;
    }
    return false;
}

QString cut(const QString& text, qsizetype most)
{
    if (text.size() <= most)
        return text;
    qsizetype keep = most - 1;
    if (keep > 0 && text.at(keep - 1).isHighSurrogate())
        --keep;
    return text.left(keep) + u'…';
}

bool chinese(const QString& locale)
{
    return locale.startsWith(u"zh"_s, Qt::CaseInsensitive);
}

} // namespace

QString toastWords(const QString& text, bool oneLine)
{
    QString out;
    out.reserve(text.size());
    for (const char32_t c : text.toUcs4()) {
        if (c == U'\n' && !oneLine)
            out += u'\n';
        else if (c == U'\n' || c == U'\r' || c == U'\t')
            out += u' ';
        else if (!hidden(c))
            out += QStringView(QChar::fromUcs4(c));
    }
    return oneLine ? out.simplified() : out.trimmed();
}

QString toastName(ToastFrom from, const QString& pluginId, const QString& pluginName, const QString& locale)
{
    const QString open = chinese(locale) ? u"插件「"_s : u"Plugin “"_s;
    const QString close = chinese(locale) ? u"」"_s : u"”"_s;
    const bool marked = from == ToastFrom::User;
    const QString words = toastWords(pluginName);
    const QString name = cut(words, kToastNameMost - (marked ? open.size() + close.size() : 0));
    // What shows of it is what was kept, not the "…" after it.
    const QStringView kept = name.size() < words.size() ? QStringView(name).chopped(1) : QStringView(name);
    if (seen(kept))
        return marked ? open + name + close : name;
    // Its id: letters, digits, ".", "_" and "-" (Manifest), always seen.
    return open + cut(toastWords(pluginId), kToastNameMost - open.size() - close.size()) + close;
}

QString toastTitle(ToastFrom from, const QString& pluginId, const QString& pluginName, const QString& title, const QString& locale)
{
    const QString name = toastName(from, pluginId, pluginName, locale);
    const QString words = toastWords(title);
    if (words.isEmpty())
        return name;
    return cut(name + (chinese(locale) ? u"："_s : u": "_s) + words, kToastTitleMost);
}

bool plainLocalPath(QStringView p)
{
    if (p.size() < 3 || p[1] != u':' || (p[2] != u'\\' && p[2] != u'/'))
        return false;
    if (!((p[0] >= u'A' && p[0] <= u'Z') || (p[0] >= u'a' && p[0] <= u'z')))
        return false;
    // A second colon is a stream ("notes.txt:payload.exe"), which can carry a program.
    return p.indexOf(u':', 2) < 0;
}

Effects toastClick(const Effects& asked, const std::function<bool(const QString& query)>& ownQuery, bool mayCopy)
{
    Effects allowed;
    bool query = false; // one of each: a click opens one list, one place, makes one copy
    bool open = false;
    bool copy = false;
    for (const Effect& e : asked) {
        switch (e.kind) {
        case Effect::Kind::SetQuery:
            if (!query && !e.text.trimmed().isEmpty() && ownQuery && ownQuery(e.text)) {
                allowed.push_back(e);
                query = true;
            }
            break;
        case Effect::Kind::Open:
            if (!open && e.open.kind == Open::Kind::Path && e.open.args.isEmpty() && e.open.cwd.isEmpty()
                && plainLocalPath(e.open.target)) {
                allowed.push_back(e);
                open = true;
            }
            break;
        case Effect::Kind::Copy:
            if (!copy && mayCopy && !e.text.isEmpty()) {
                allowed.push_back(e);
                copy = true;
            }
            break;
        case Effect::Kind::Hide:
        case Effect::Kind::Keep:
        case Effect::Kind::Notify:
        case Effect::Kind::Replace:
            break;
        }
    }
    return allowed;
}

} // namespace ws::plugin
