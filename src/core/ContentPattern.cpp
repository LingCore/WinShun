#include "ContentPattern.h"

#include <QObject>

#include <algorithm>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws {

QStringList queryWords(const QString& text)
{
    QStringList tokens;
    QString current;
    bool quoted = false;
    for (const QChar c : text) {
        if (c == u'"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && c.isSpace()) {
            if (!current.isEmpty())
                tokens.append(std::exchange(current, QString()));
            continue;
        }
        current.append(c);
    }
    if (!current.isEmpty())
        tokens.append(current);
    return tokens;
}

bool looksLikeCode(QStringView text)
{
    static constexpr QStringView kSigns[] {u"=", u";", u"(", u")", u"{", u"}", u"<", u">", u"[", u"]", u"->", u"::", u"&&", u"||"};
    return std::ranges::any_of(kSigns, [&](QStringView s) { return text.contains(s); });
}

QStringList contentTerms(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return {};
    if (!looksLikeCode(trimmed))
        return queryWords(trimmed); // an unclosed quote: to the end
    // Quotes around the whole line only make it one phrase, as for words.
    if (trimmed.count(u'"') == 2 && trimmed.size() > 2 && trimmed.startsWith(u'"') && trimmed.endsWith(u'"'))
        return {trimmed.mid(1, trimmed.size() - 2).trimmed()};
    return {trimmed};
}

// ---- RegexNeed ------------------------------------------------------------

namespace {

constexpr qsizetype kMaxExact = 16; // strings an expression is known to match, at most
constexpr qsizetype kMaxExactLength = 96; // and how long one may get
constexpr int kMaxClassRange = 16; // [a-f] is spelled out, [a-z] is any character
constexpr int kMaxDepth = 64; // groups within groups

RegexNeed literal(QString text)
{
    RegexNeed need;
    if (!text.isEmpty()) {
        need.kind = RegexNeed::Kind::Literal;
        need.text = std::move(text);
    }
    return need;
}

void addDistinct(std::vector<RegexNeed>& parts, RegexNeed part)
{
    if (std::ranges::find(parts, part) == parts.end())
        parts.push_back(std::move(part));
}

RegexNeed allOf(RegexNeed a, RegexNeed b)
{
    if (a.isAny())
        return b;
    if (b.isAny())
        return a;
    RegexNeed out;
    out.kind = RegexNeed::Kind::All;
    for (RegexNeed* side : {&a, &b}) {
        if (side->kind == RegexNeed::Kind::All) {
            for (RegexNeed& part : side->parts)
                addDistinct(out.parts, std::move(part));
        } else {
            addDistinct(out.parts, std::move(*side));
        }
    }
    return out.parts.size() == 1 ? std::move(out.parts.front()) : out;
}

RegexNeed oneOf(RegexNeed a, RegexNeed b)
{
    if (a.isAny() || b.isAny())
        return {};
    RegexNeed out;
    out.kind = RegexNeed::Kind::OneOf;
    for (RegexNeed* side : {&a, &b}) {
        if (side->kind == RegexNeed::Kind::OneOf) {
            for (RegexNeed& part : side->parts)
                addDistinct(out.parts, std::move(part));
        } else {
            addDistinct(out.parts, std::move(*side));
        }
    }
    return out.parts.size() == 1 ? std::move(out.parts.front()) : out;
}

// What is known of a part of an expression: every string it can match
// (`exact`, in lower case), or, when those are too many to list or not known,
// what each of them has in it (`need`).
struct Part {
    bool known = false;
    QStringList exact;
    RegexNeed need;

    static Part strings(QStringList exact)
    {
        Part p;
        p.known = true;
        p.exact = std::move(exact);
        return p;
    }
    static Part empty() { return strings({QString()}); } // matches without taking a character (^, \b, a lookahead)
    static Part unknown(RegexNeed need = {})
    {
        Part p;
        p.need = std::move(need);
        return p;
    }
};

RegexNeed needOf(const Part& part)
{
    if (!part.known)
        return part.need;
    if (part.exact.isEmpty() || part.exact.contains(QString()))
        return {};
    RegexNeed need = literal(part.exact.front());
    for (qsizetype i = 1; i < part.exact.size(); ++i)
        need = oneOf(std::move(need), literal(part.exact[i]));
    return need;
}

// a then b, when every string of both is known and they stay few.
std::optional<Part> joined(const Part& a, const Part& b)
{
    if (!a.known || !b.known || a.exact.size() * b.exact.size() > kMaxExact)
        return std::nullopt;
    QStringList out;
    for (const QString& x : a.exact) {
        for (const QString& y : b.exact) {
            if (x.size() + y.size() > kMaxExactLength)
                return std::nullopt;
            if (const QString s = x + y; !out.contains(s))
                out.append(s);
        }
    }
    return Part::strings(std::move(out));
}

// The parts one after another: the stretches whose strings are known are
// taken whole ("abc" of a, b, c), and what is between them breaks them.
Part sequence(const std::vector<Part>& parts)
{
    Part run = Part::empty();
    RegexNeed need;
    bool broken = false;
    for (const Part& part : parts) {
        if (std::optional<Part> longer = joined(run, part)) {
            run = std::move(*longer);
            continue;
        }
        need = allOf(std::move(need), needOf(run));
        broken = true;
        if (part.known) {
            run = part;
        } else {
            need = allOf(std::move(need), part.need);
            run = Part::empty();
        }
    }
    if (!broken)
        return run;
    return Part::unknown(allOf(std::move(need), needOf(run)));
}

Part either(const Part& a, const Part& b)
{
    if (a.known && b.known) {
        QStringList out = a.exact;
        for (const QString& s : b.exact) {
            if (!out.contains(s))
                out.append(s);
        }
        if (out.size() <= kMaxExact)
            return Part::strings(std::move(out));
    }
    return Part::unknown(oneOf(needOf(a), needOf(b)));
}

// `max` < 0: without end.
Part repeated(const Part& part, int min, int max)
{
    if (max == 0)
        return Part::empty();
    if (min == 0)
        return max == 1 ? either(part, Part::empty()) : Part::unknown();
    if (min == max && min <= 8) {
        Part out = part;
        for (int i = 1; i < min; ++i) {
            std::optional<Part> longer = joined(out, part);
            if (!longer)
                return Part::unknown(needOf(part));
            out = std::move(*longer);
        }
        return out;
    }
    return Part::unknown(needOf(part));
}

// A character of the expression that stands for itself: in lower case; none
// when its other case is not ASCII's to fold, or it is half a character.
std::optional<QChar> plainChar(QChar c)
{
    if (c.unicode() < 0x80)
        return c.toLower();
    if (c.isSurrogate() || c.toLower() != c || c.toUpper() != c || c.toCaseFolded() != c)
        return std::nullopt;
    return c;
}

Part charPart(QChar c)
{
    const std::optional<QChar> plain = plainChar(c);
    return plain ? Part::strings({QString(*plain)}) : Part::unknown();
}

// Reads PCRE's syntax as far as the text needed goes. Whatever it does not
// make out fails the whole reading (RegexNeed::of gives Any): a wrong guess
// would leave out files that match.
class Reader {
public:
    explicit Reader(QStringView pattern)
        : m_text(pattern)
    {
    }

    std::optional<Part> read()
    {
        Part part = alternation(0);
        if (m_failed || m_at != m_text.size())
            return std::nullopt;
        return part;
    }

private:
    bool atEnd() const { return m_at >= m_text.size(); }
    QChar peek(qsizetype ahead = 0) const { return m_at + ahead < m_text.size() ? m_text[m_at + ahead] : QChar(); }
    bool take(QChar c)
    {
        if (peek() != c || atEnd())
            return false;
        ++m_at;
        return true;
    }
    bool take(QStringView s)
    {
        if (!m_text.sliced(m_at).startsWith(s))
            return false;
        m_at += s.size();
        return true;
    }
    Part fail()
    {
        m_failed = true;
        m_at = m_text.size();
        return Part::unknown();
    }
    // Up to and past `close`: a name, a number in braces.
    bool skipPast(QChar close)
    {
        const qsizetype end = m_text.indexOf(close, m_at);
        if (end < 0)
            return false;
        m_at = end + 1;
        return true;
    }

    Part alternation(int depth)
    {
        if (depth > kMaxDepth)
            return fail();
        Part out = branch(depth);
        while (!m_failed && take(u'|'))
            out = either(out, branch(depth));
        return out;
    }

    Part branch(int depth)
    {
        std::vector<Part> parts;
        while (!m_failed && !atEnd() && peek() != u'|' && peek() != u')') {
            Part part = atom(depth);
            if (m_failed)
                break;
            int min = 1;
            int max = 1;
            while (quantifier(min, max)) { // a{2}{3} is not written, but a quantifier after one is read as PCRE reads it
                part = repeated(part, min, max);
                if (peek() == u'?' || peek() == u'+')
                    ++m_at; // lazy, possessive: the same strings
            }
            parts.push_back(std::move(part));
        }
        return sequence(parts);
    }

    // ?, *, +, {n}, {n,}, {n,m}, {,m}. A brace that is none of these is a character.
    bool quantifier(int& min, int& max)
    {
        if (m_failed || atEnd())
            return false;
        const QChar c = peek();
        if (c == u'?' || c == u'*' || c == u'+') {
            ++m_at;
            min = c == u'+' ? 1 : 0;
            max = c == u'?' ? 1 : -1;
            return true;
        }
        if (c != u'{')
            return false;
        qsizetype i = m_at + 1;
        const auto number = [&](int& out) {
            const qsizetype from = i;
            qint64 value = 0;
            while (i < m_text.size() && m_text[i].unicode() >= u'0' && m_text[i].unicode() <= u'9' && i - from < 6)
                value = value * 10 + (m_text[i++].unicode() - u'0');
            out = static_cast<int>(value);
            return i > from;
        };
        int low = 0;
        int high = -1;
        const bool hasLow = number(low);
        bool comma = false;
        bool hasHigh = false;
        if (i < m_text.size() && m_text[i] == u',') {
            comma = true;
            ++i;
            hasHigh = number(high);
        }
        if (i >= m_text.size() || m_text[i] != u'}' || (!hasLow && !hasHigh)) {
            // Not a quantifier as written here. Newer PCRE also takes spaces
            // within the braces: not told apart, so not read at all.
            if (i < m_text.size() && (m_text[i] == u' ' || m_text[i] == u'\t'))
                fail();
            return false;
        }
        m_at = i + 1;
        min = hasLow ? low : 0;
        max = !comma ? low : hasHigh ? high : -1;
        return true;
    }

    Part atom(int depth)
    {
        const QChar c = peek();
        ++m_at;
        switch (c.unicode()) {
        case u'(':
            return group(depth);
        case u'[':
            return charClass();
        case u'.':
            return Part::unknown();
        case u'^':
        case u'$':
            return Part::empty();
        case u'\\':
            return escape();
        case u'*':
        case u'+':
        case u'?':
            return fail(); // nothing to repeat
        default:
            return charPart(c);
        }
    }

    Part group(int depth)
    {
        bool lookaround = false;
        if (take(u'?')) {
            if (take(u':') || take(u'>') || take(u'|')) {
                // not kept, atomic, numbered alike: what is inside
            } else if (take(u'=') || take(u'!') || take(u"<="_s) || take(u"<!"_s)) {
                lookaround = true;
            } else if (take(u"P<"_s) || take(u'<')) {
                if (!skipPast(u'>'))
                    return fail();
            } else if (take(u'\'')) {
                if (!skipPast(u'\''))
                    return fail();
            } else {
                // Options: (?i) for what follows, (?i:...) for what is inside.
                // Case and how . and ^ behave change nothing here; (?x)
                // changes what a space is.
                const qsizetype from = m_at;
                while (!atEnd() && QStringView(u"imsnUJ-^").contains(peek()))
                    ++m_at;
                if (m_at == from && peek() != u':' && peek() != u')')
                    return fail(); // a comment, a condition, a call, (?x)...
                if (take(u')'))
                    return Part::empty();
                if (!take(u':'))
                    return fail();
            }
        } else if (peek() == u'*') {
            return fail(); // (*VERB)
        }
        Part inside = alternation(depth + 1);
        if (m_failed || !take(u')'))
            return fail();
        return lookaround ? Part::empty() : inside;
    }

    Part escape()
    {
        if (atEnd())
            return fail();
        const QChar c = peek();
        ++m_at;
        const auto braced = [&] { return peek() == u'{' ? skipPast(u'}') : true; };
        switch (c.unicode()) {
        case u'd': case u'D': case u'w': case u'W': case u's': case u'S': case u'h': case u'H':
        case u'v': case u'V': case u'R': case u'X': case u'C':
        case u'f': case u'e': case u'a':
            return Part::unknown();
        case u'N': // not a newline, or \N{U+hhhh}
        case u'o': // \o{...}
            return braced() ? Part::unknown() : fail();
        case u'b': case u'B': case u'A': case u'z': case u'Z': case u'G': case u'K': case u'E':
            return Part::empty();
        case u'n':
            return Part::strings({u"\n"_s});
        case u'r':
            return Part::strings({u"\r"_s});
        case u't':
            return Part::strings({u"\t"_s});
        case u'p':
        case u'P': // \pL, \p{Han}
            if (peek() == u'{')
                return skipPast(u'}') ? Part::unknown() : fail();
            if (atEnd())
                return fail();
            ++m_at;
            return Part::unknown();
        case u'x': // \x41, \x{4e2d}
            if (peek() == u'{')
                return skipPast(u'}') ? Part::unknown() : fail();
            for (int i = 0; i < 2 && isHex(peek()); ++i)
                ++m_at;
            return Part::unknown();
        case u'c': // \cA
            if (atEnd())
                return fail();
            ++m_at;
            return Part::unknown();
        case u'g': // what a group matched: \g1, \g-1, \g{name}, \g<name>
        case u'k':
            if (peek() == u'{')
                return skipPast(u'}') ? Part::unknown() : fail();
            if (peek() == u'<')
                return skipPast(u'>') ? Part::unknown() : fail();
            if (peek() == u'\'') {
                ++m_at;
                return skipPast(u'\'') ? Part::unknown() : fail();
            }
            if (peek() == u'-' || peek() == u'+')
                ++m_at;
            [[fallthrough]];
        case u'0': case u'1': case u'2': case u'3': case u'4':
        case u'5': case u'6': case u'7': case u'8': case u'9':
            while (peek().unicode() >= u'0' && peek().unicode() <= u'9' && !atEnd())
                ++m_at; // a group again, or a character by its number
            return Part::unknown();
        default:
            if (c.unicode() < 0x80 && c.isLetterOrNumber())
                return fail(); // \Q, and what PCRE may come to mean by a letter
            return charPart(c);
        }
    }

    static bool isHex(QChar c)
    {
        const char16_t u = c.unicode();
        return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') || (u >= u'A' && u <= u'F');
    }

    // After the [. A few characters listed are each of them; anything else
    // (a range too wide, [^...], \d or [:alpha:] within) is any character.
    Part charClass()
    {
        bool listed = !take(u'^');
        QStringList chars;
        const auto add = [&](QChar c) {
            const std::optional<QChar> plain = plainChar(c);
            if (!plain)
                listed = false;
            else if (const QString s(*plain); !chars.contains(s))
                chars.append(s);
        };
        bool first = true;
        for (;; first = false) {
            if (atEnd())
                return fail();
            const QChar c = peek();
            ++m_at;
            if (c == u']' && !first)
                break;
            if (c == u'\\') {
                if (atEnd() || peek() == u'Q')
                    return fail();
                ++m_at;
                listed = false;
                continue;
            }
            if (c == u'[' && (peek() == u':' || peek() == u'.' || peek() == u'=')) {
                const qsizetype end = m_text.indexOf(QString(peek()) + u']', m_at + 1);
                if (end < 0)
                    return fail();
                m_at = end + 2;
                listed = false;
                continue;
            }
            if (peek() == u'-' && peek(1) != u']' && m_at + 1 < m_text.size()) {
                const QChar last = peek(1);
                m_at += 2;
                if (last == u'\\' || last == u'[') {
                    --m_at; // read on from it: an escape, or a class by name
                    listed = false;
                    continue;
                }
                const int span = last.unicode() - c.unicode();
                if (span < 0 || span >= kMaxClassRange || c.unicode() >= 0x80 || last.unicode() >= 0x80) {
                    listed = false;
                    continue;
                }
                for (int i = 0; i <= span; ++i)
                    add(QChar(c.unicode() + i));
                continue;
            }
            add(c);
        }
        if (!listed || chars.isEmpty() || chars.size() > kMaxExact)
            return Part::unknown();
        return Part::strings(std::move(chars));
    }

    QStringView m_text;
    qsizetype m_at = 0;
    bool m_failed = false;
};

} // namespace

RegexNeed RegexNeed::of(QStringView pattern)
{
    const std::optional<Part> part = Reader(pattern).read();
    return part ? needOf(*part) : RegexNeed();
}

bool RegexNeed::in(QStringView haystack) const
{
    switch (kind) {
    case Kind::Any:
        return true;
    case Kind::Literal:
        return haystack.contains(text, Qt::CaseInsensitive);
    case Kind::All:
        return std::ranges::all_of(parts, [&](const RegexNeed& p) { return p.in(haystack); });
    case Kind::OneOf:
        return std::ranges::any_of(parts, [&](const RegexNeed& p) { return p.in(haystack); });
    }
    return true;
}

QString RegexNeed::toString() const
{
    const auto list = [&](QLatin1StringView name) {
        QStringList inner;
        for (const RegexNeed& p : parts)
            inner.append(p.toString());
        return name + u'(' + inner.join(u", "_s) + u')';
    };
    switch (kind) {
    case Kind::Any:
        return u"any"_s;
    case Kind::Literal:
        return u'"' + text + u'"';
    case Kind::All:
        return list("all"_L1);
    case Kind::OneOf:
        return list("oneOf"_L1);
    }
    return {};
}

// ---- ContentPattern ---------------------------------------------------------

bool ContentPattern::hasRegex() const noexcept
{
    return std::ranges::any_of(needles, &ContentNeedle::regex);
}

QStringList ContentPattern::words() const
{
    QStringList out;
    for (const ContentNeedle& n : needles) {
        if (!n.regex)
            out.append(n.text);
    }
    return out;
}

namespace {

// A word as typed: quotes taken off, spaces within them kept.
struct Word {
    QString text;
    bool quoted = false; // any of it
    bool quotedStart = false; // from its first character: taken as it is
};

std::vector<Word> wordsOf(const QString& text)
{
    std::vector<Word> words;
    Word current;
    bool open = false; // within a word
    bool quoted = false;
    const auto end = [&] {
        if (open && !current.text.isEmpty())
            words.push_back(std::move(current));
        current = {};
        open = false;
    };
    for (const QChar c : text) {
        if (c == u'"') {
            if (!open)
                current.quotedStart = true;
            open = true;
            current.quoted = true;
            quoted = !quoted;
            continue;
        }
        if (!quoted && c.isSpace()) {
            end();
            continue;
        }
        open = true;
        current.text.append(c);
    }
    end(); // an unclosed quote: to the end
    return words;
}

qsizetype regexPrefix(const Word& word)
{
    if (word.quotedStart)
        return 0;
    return word.text.startsWith(u"regex:", Qt::CaseInsensitive) ? 6 : word.text.startsWith(u"re:", Qt::CaseInsensitive) ? 3 : 0;
}

// Collects the needles (each once) and the ways.
class Builder {
public:
    // The needle's number; none when there are too many already.
    std::optional<std::size_t> plain(const QString& text)
    {
        for (std::size_t i = 0; i < m_out.needles.size(); ++i) {
            if (!m_out.needles[i].regex && m_out.needles[i].text.compare(text, Qt::CaseInsensitive) == 0)
                return i;
        }
        if (m_out.needles.size() == ContentPattern::kMaxNeedles)
            return std::nullopt;
        m_out.needles.push_back({text, false, {}, {}});
        return m_out.needles.size() - 1;
    }

    std::optional<std::size_t> regex(const QString& pattern)
    {
        for (std::size_t i = 0; i < m_out.needles.size(); ++i) {
            if (m_out.needles[i].regex && m_out.needles[i].text == pattern)
                return i;
        }
        if (m_out.needles.size() == ContentPattern::kMaxNeedles)
            return std::nullopt;
        // A line at a time, whatever ends it: $ before "\r\n" too. An
        // expression that runs away on a long line gives up on it.
        QRegularExpression expression(u"(*LIMIT_MATCH=200000)(*LIMIT_DEPTH=200)(*ANYCRLF)"_s + pattern,
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption
                | QRegularExpression::MultilineOption);
        if (pattern.isEmpty() || !expression.isValid()) {
            if (m_out.error.isEmpty()) {
                m_out.error = QObject::tr("正则表达式无效：%1")
                    .arg(pattern.isEmpty() ? QObject::tr("请输入表达式") : expression.errorString());
                m_out.errorReason = pattern.isEmpty() ? u"no expression after regex:"_s : expression.errorString();
            }
            return std::nullopt;
        }
        expression.optimize();
        m_out.needles.push_back({pattern, true, std::move(expression), RegexNeed::of(pattern)});
        return m_out.needles.size() - 1;
    }

    // Words one after another, each with the needles any one of which will
    // do: every choice among them is a way.
    void addWays(const std::vector<std::vector<std::size_t>>& picks)
    {
        std::vector<std::uint32_t> ways {0};
        for (const std::vector<std::size_t>& slot : picks) {
            std::vector<std::uint32_t> next;
            for (const std::uint32_t way : ways) {
                for (const std::size_t needle : slot) {
                    if (const std::uint32_t with = way | (std::uint32_t {1} << needle); std::ranges::find(next, with) == next.end())
                        next.push_back(with);
                }
            }
            ways = std::move(next);
        }
        for (const std::uint32_t way : ways) {
            if (way != 0)
                m_ways.push_back(way);
        }
    }

    std::size_t wayCount() const noexcept { return m_ways.size(); }

    ContentPattern finish()
    {
        if (!m_out.error.isEmpty()) {
            m_out.needles.clear();
            return std::move(m_out);
        }
        // A way that asks for all of another and more adds no file.
        for (const std::uint32_t way : m_ways) {
            const bool more = std::ranges::any_of(m_ways, [&](std::uint32_t other) { return other != way && (other & way) == other; });
            if (!more && std::ranges::find(m_out.ways, way) == m_out.ways.end())
                m_out.ways.push_back(way);
        }
        if (m_out.ways.empty())
            m_out.needles.clear();
        return std::move(m_out);
    }

private:
    ContentPattern m_out;
    std::vector<std::uint32_t> m_ways;
};

} // namespace

ContentPattern ContentPattern::allOf(const QStringList& terms)
{
    Builder builder;
    std::vector<std::vector<std::size_t>> picks;
    for (const QString& term : terms) {
        if (term.isEmpty())
            continue;
        if (const auto needle = builder.plain(term))
            picks.push_back({*needle});
    }
    builder.addWays(picks);
    return builder.finish();
}

ContentPattern contentPattern(const QString& text, bool regexMode)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return {};
    const std::vector<Word> words = wordsOf(trimmed);
    const bool anyRegex = regexMode || std::ranges::any_of(words, [](const Word& w) { return regexPrefix(w) > 0; });
    // A line of code is looked for as it is: its bars and brackets are its own.
    if (!anyRegex && looksLikeCode(trimmed))
        return ContentPattern::allOf(contentTerms(trimmed));

    Builder builder;
    std::vector<std::vector<std::size_t>> picks; // of the words since the last " | "
    const auto endWay = [&] {
        builder.addWays(picks);
        picks.clear();
    };
    for (const Word& word : words) {
        if (!word.quoted && word.text == u"|") {
            endWay();
            continue;
        }
        if (const qsizetype prefix = regexPrefix(word); prefix > 0 || regexMode) {
            if (const auto needle = builder.regex(word.text.sliced(prefix)))
                picks.push_back({*needle});
            continue;
        }
        // 马上|立即: either. What a file with "马上|立即" in it has too, so
        // nothing is lost by reading it so; too many to be a choice, as it is.
        // A bar with a space on one side only parts ways too: "马上| 立即".
        const bool barAfter = !word.quoted && word.text.endsWith(u'|');
        if (!word.quoted && word.text.startsWith(u'|'))
            endWay();
        std::vector<std::size_t> slot;
        const QStringList choices = word.quoted ? QStringList() : word.text.split(u'|', Qt::SkipEmptyParts);
        auto ways = static_cast<std::size_t>(choices.size()); // of this " | " part, with the choice
        for (const std::vector<std::size_t>& s : picks)
            ways *= s.size();
        if (choices.size() > 1 && builder.wayCount() + ways <= ContentPattern::kMaxWays) {
            for (const QString& choice : choices) {
                if (const auto needle = builder.plain(choice))
                    slot.push_back(*needle);
            }
        } else if (const auto needle = builder.plain(choices.size() == 1 ? choices.front() : word.text)) {
            slot.push_back(*needle);
        }
        if (!slot.empty())
            picks.push_back(std::move(slot));
        if (barAfter)
            endWay();
    }
    endWay();
    return builder.finish();
}

} // namespace ws
