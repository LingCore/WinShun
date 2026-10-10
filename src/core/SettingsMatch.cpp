#include "SettingsMatch.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws::settingsmatch {

namespace {

// Punctuation that separates words, after NFKC has turned the full-width
// forms ("，", "（", "；") into ASCII. Not "+", ".", "_", "-", ":", "\" and
// "/": they belong to words such as Win+V, KeePass.exe, node_modules, C:\.
bool separates(QChar c)
{
    switch (c.unicode()) {
    case u',': case u';': case u'?': case u'!': case u'(': case u')': case u'[': case u']':
    case u'{': case u'}': case u'<': case u'>': case u'"': case u'\'': case u'|':
    case u'、': case u'。': case u'“': case u'”': case u'‘': case u'’': case u'《': case u'》':
    case u'【': case u'】': case u'「': case u'」': case u'『': case u'』': case u'…': case u'·':
        return true;
    default:
        return c.isSpace();
    }
}

bool isAsciiAlnum(char16_t c)
{
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9');
}

// Whether a word starts at `at`: at the start, after a space, punctuation or
// a Chinese character, at a camelCase hump, between letters and digits.
// Next to Chinese text every position counts: it has no spaces.
bool startsWord(QStringView text, qsizetype at)
{
    if (at == 0)
        return true;
    const char16_t prev = text[at - 1].unicode();
    const char16_t cur = text[at].unicode();
    if (!isAsciiAlnum(prev) || !isAsciiAlnum(cur))
        return true;
    if ((prev <= u'9') != (cur <= u'9'))
        return true;
    return prev >= u'a' && prev <= u'z' && cur >= u'A' && cur <= u'Z';
}

enum How { Whole, Start, Word, Inside, Pinyin, Initials, HowCount };
enum Field { Title, Keyword, Option, Context, Description, Value, FieldCount };

// A word found in the title outranks any other field; one spelled out in a
// keyword outranks the title by pinyin initials. The page and section rank
// below the option's own words, so that "剪贴板 图片" puts 记录图片 first.
constexpr int kScore[FieldCount][HowCount] = {
    // Whole Start Word Inside Pinyin Initials
    {104, 100, 90, 80, 74, 68}, // Title
    {88, 78, 72, 62, 58, 54}, // Keyword
    {57, 57, 55, 52, 48, 46}, // Option
    {42, 42, 40, 38, 35, 33}, // Context
    {20, 20, 20, 20, 0, 0}, // Description
    {12, 12, 12, 12, 0, 0}, // Value
};

struct Found {
    Span span;
    How how = Inside;

    int score(Field field) const
    {
        // A little more when the pinyin starts the text ("kj" in 快捷键).
        const bool leading = (how == Pinyin || how == Initials) && span.start == 0;
        return kScore[field][how] + (leading ? 2 : 0);
    }
};

std::optional<Found> findWord(const Term& term, QStringView text)
{
    qsizetype inside = -1;
    for (qsizetype at = text.indexOf(term.text, 0, Qt::CaseInsensitive); at >= 0;
         at = text.indexOf(term.text, at + 1, Qt::CaseInsensitive)) {
        if (startsWord(text, at)) {
            const How how = at > 0 ? Word : term.text.size() == text.size() ? Whole : Start;
            return Found {{at, term.text.size()}, how};
        }
        if (inside < 0)
            inside = at;
    }
    if (inside >= 0 && !term.single)
        return Found {{inside, term.text.size()}, Inside};
    return std::nullopt;
}

std::optional<Found> findPinyin(const Term& term, QStringView text)
{
    if (!term.pinyin.valid())
        return std::nullopt;
    const std::u16string_view view(reinterpret_cast<const char16_t*>(text.utf16()), static_cast<std::size_t>(text.size()));
    const auto span = term.pinyin.findUtf16(view);
    if (!span)
        return std::nullopt;
    const Span found {static_cast<qsizetype>(span->start), static_cast<qsizetype>(span->length)};
    // One letter for each character: by the initials alone.
    return Found {found, found.length == term.text.size() ? Initials : Pinyin};
}

// The word in a field that may be matched by pinyin: as written first.
std::optional<Found> find(const Term& term, QStringView text)
{
    if (auto found = findWord(term, text))
        return found;
    return findPinyin(term, text);
}

void merge(QList<Span>& spans)
{
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.start < b.start; });
    QList<Span> merged;
    for (const Span& span : std::as_const(spans)) {
        if (!merged.isEmpty() && span.start <= merged.last().start + merged.last().length) {
            Span& last = merged.last();
            last.length = std::max(last.start + last.length, span.start + span.length) - last.start;
        } else {
            merged.append(span);
        }
    }
    spans = merged;
}

} // namespace

Query parse(const QString& text)
{
    QString normalized = text.normalized(QString::NormalizationForm_KC);
    for (QChar& c : normalized) {
        if (separates(c))
            c = u' ';
    }

    Query query;
    QString word;
    const auto finish = [&] {
        if (word.isEmpty())
            return;
        word = word.toCaseFolded();
        const bool ascii = std::all_of(word.cbegin(), word.cend(), [](QChar c) { return c.unicode() < 0x80; });
        const std::string folded = ascii ? word.toStdString() : std::string();
        query.terms.push_back({word, pinyin::Matcher(folded), word.size() == 1 && ascii});
        word.clear();
    };
    for (qsizetype i = 0; i < normalized.size(); ++i) {
        const QChar c = normalized[i];
        if (c == u' ') {
            // "Win + V": the spaces around a plus belong to the word.
            qsizetype next = i;
            while (next < normalized.size() && normalized[next] == u' ')
                ++next;
            const bool beforePlus = next < normalized.size() && normalized[next] == u'+' && !word.isEmpty();
            const bool afterPlus = word.endsWith(u'+');
            if (beforePlus || (afterPlus && next < normalized.size())) {
                i = next - 1;
                continue;
            }
            finish();
            continue;
        }
        word.append(c);
    }
    finish();
    return query;
}

Match match(const Query& query, const Fields& fields)
{
    Match result;
    if (query.isEmpty())
        return result;

    int total = 0;
    for (const Term& term : query.terms) {
        int best = -1;
        const auto consider = [&best](int score) { best = std::max(best, score); };

        if (const auto found = find(term, fields.title)) {
            consider(found->score(Title));
            result.title.append(found->span);
        }
        for (const QString& keyword : fields.keywords) {
            if (const auto found = find(term, keyword))
                consider(found->score(Keyword));
        }
        for (const QString& option : fields.options) {
            if (const auto found = find(term, option))
                consider(found->score(Option));
        }
        // The page brings up all its options: by pinyin only for three
        // letters or more, as two fit too much ("zt": 主题, but also 粘贴).
        for (const QString& context : fields.context) {
            if (const auto found = term.text.size() >= 3 ? find(term, context) : findWord(term, context))
                consider(found->score(Context));
        }
        // A lone letter in a sentence would find nearly everything.
        if (!term.single) {
            if (const auto found = findWord(term, fields.description)) {
                consider(found->score(Description));
                result.description.append(found->span);
            }
            for (qsizetype i = 0; i < fields.values.size(); ++i) {
                if (const auto found = findWord(term, fields.values[i])) {
                    consider(found->score(Value));
                    if (!result.values.contains(static_cast<int>(i)))
                        result.values.append(static_cast<int>(i));
                }
            }
        }

        if (best < 0)
            return Match {};
        total += best;
    }

    result.score = total;
    merge(result.title);
    merge(result.description);
    std::sort(result.values.begin(), result.values.end());
    return result;
}

QString highlight(const QString& text, const QList<Span>& spans, const QString& color)
{
    QString out;
    qsizetype at = 0;
    for (const Span& span : spans) {
        if (span.start < at || span.length <= 0 || span.start + span.length > text.size())
            continue;
        out += text.mid(at, span.start - at).toHtmlEscaped();
        out += u"<font color=\""_s + color + u"\">"_s + text.mid(span.start, span.length).toHtmlEscaped() + u"</font>"_s;
        at = span.start + span.length;
    }
    out += text.mid(at).toHtmlEscaped();
    // StyledText runs lines together as HTML does.
    out.replace(u'\n', u"<br>"_s);
    return out;
}

} // namespace ws::settingsmatch
