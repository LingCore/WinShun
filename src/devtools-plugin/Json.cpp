#include "Json.h"

#include <QCoreApplication>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::devtools {

namespace {

struct Text {
    Q_DECLARE_TR_FUNCTIONS(Json)
};

constexpr int kDeepest = 1000;
constexpr qsizetype kMostEmbedded = 4 * 1024 * 1024; // a longer string is not looked into
constexpr int kEmbedLevels = 4; // a string's JSON in a string's JSON ...
constexpr int kMostCandidates = 64; // the braces tried in a log line

bool isSpace(QChar c)
{
    const char16_t u = c.unicode();
    return u == u' ' || u == u'\t' || u == u'\n' || u == u'\r' || u == 0xFEFF || u == 0x00A0 || u == 0x3000;
}

bool isDigit(QChar c) { return c >= u'0' && c <= u'9'; }
bool identifierStart(QChar c) { return c.isLetter() || c == u'_' || c == u'$'; }
bool identifierPart(QChar c) { return c.isLetterOrNumber() || c == u'_' || c == u'$' || c == u'-'; }

qsizetype skipSpaces(QStringView text, qsizetype at)
{
    while (at < text.size() && isSpace(text[at]))
        ++at;
    return at;
}

bool onlySpacesAfter(QStringView text, qsizetype at) { return skipSpaces(text, at) >= text.size(); }

QString shortened(const QString& text)
{
    return text.size() <= 24 ? text : text.first(23) + u'…';
}

int hexValue(QChar c)
{
    const char16_t u = c.unicode();
    if (u >= u'0' && u <= u'9')
        return u - u'0';
    if (u >= u'a' && u <= u'f')
        return u - u'a' + 10;
    if (u >= u'A' && u <= u'F')
        return u - u'A' + 10;
    return -1;
}

// `digits` hex digits at `at`, or -1.
int hexAt(QStringView text, qsizetype at, int digits)
{
    if (at + digits > text.size())
        return -1;
    int value = 0;
    for (int i = 0; i < digits; ++i) {
        const int v = hexValue(text[at + i]);
        if (v < 0)
            return -1;
        value = value * 16 + v;
    }
    return value;
}

void link(Document& d, int parent, int child)
{
    Node& p = d.nodes[parent];
    d.nodes[child].parent = parent;
    d.nodes[child].index = p.count;
    d.nodes[child].next = -1;
    if (p.last >= 0)
        d.nodes[p.last].next = child;
    else
        p.first = child;
    p.last = child;
    ++p.count;
}

// `from`'s nodes appended to `into`, its root under `parent` (not linked as a
// child: the caller says how it hangs there). The root's index.
int graft(Document& into, int parent, Document&& from)
{
    const int offset = int(into.nodes.size());
    into.nodes.reserve(into.nodes.size() + from.nodes.size());
    for (Node& n : from.nodes) {
        n.parent = n.parent >= 0 ? n.parent + offset : parent;
        if (n.first >= 0)
            n.first += offset;
        if (n.last >= 0)
            n.last += offset;
        if (n.next >= 0)
            n.next += offset;
        into.nodes.push_back(std::move(n));
    }
    return offset;
}

bool isContainer(const Document& d) { return !d.isEmpty() && (d.nodes[0].kind == Kind::Object || d.nodes[0].kind == Kind::Array); }

void locate(QStringView text, Error& error)
{
    error.offset = std::clamp<qsizetype>(error.offset, 0, text.size());
    int line = 1;
    qsizetype lineStart = 0;
    for (qsizetype i = 0; i < error.offset; ++i) {
        if (text[i] == u'\n') {
            ++line;
            lineStart = i + 1;
        }
    }
    error.line = line;
    error.column = int(error.offset - lineStart) + 1;
}

class Parser {
public:
    Parser(QStringView text, qsizetype from, Document& document)
        : m_s(text)
        , m_i(from)
        , m_d(document)
    {
    }

    bool run() { return value(-1, {}, 0); }
    qsizetype at() const { return m_i; }
    const Error& error() const { return m_error; }

private:
    bool fail(const QString& message, qsizetype at)
    {
        if (!m_failed) {
            m_failed = true;
            m_error.message = message;
            m_error.offset = at;
        }
        return false;
    }

    // Whitespace, and the comments of JSONC: // to the line's end, /* ... */.
    void skip()
    {
        while (m_i < m_s.size()) {
            if (isSpace(m_s[m_i])) {
                ++m_i;
            } else if (m_s[m_i] == u'/' && m_i + 1 < m_s.size() && m_s[m_i + 1] == u'/') {
                m_d.relaxed |= Comments;
                while (m_i < m_s.size() && m_s[m_i] != u'\n')
                    ++m_i;
            } else if (m_s[m_i] == u'/' && m_i + 1 < m_s.size() && m_s[m_i + 1] == u'*') {
                m_d.relaxed |= Comments;
                const qsizetype close = m_s.indexOf(u"*/", m_i + 2);
                m_i = close < 0 ? m_s.size() : close + 2;
            } else {
                return;
            }
        }
    }

    // The next member starts a line of its own after a value that ended
    // the one before: the comma that is missing belongs at that line's end
    // (where an editor's eye looks), not before the next one.
    bool nextLineStarts(qsizetype after, bool memberStarts) const
    {
        return memberStarts && m_s.sliced(after, m_i - after).contains(u'\n');
    }

    int add(Kind kind, int parent, QString&& key, QString&& text = {})
    {
        const int index = int(m_d.nodes.size());
        Node& node = m_d.nodes.emplace_back();
        node.kind = kind;
        node.key = std::move(key);
        node.text = std::move(text);
        if (parent >= 0)
            link(m_d, parent, index);
        return index;
    }

    bool value(int parent, QString key, int depth)
    {
        if (depth > kDeepest)
            return fail(Text::tr("嵌套太深（超过 %1 层）").arg(kDeepest), m_i);
        skip();
        if (m_i >= m_s.size())
            return fail(Text::tr("这里应该有一个值，但内容已经结束"), m_i);
        const QChar c = m_s[m_i];
        if (c == u'{')
            return object(parent, std::move(key), depth);
        if (c == u'[')
            return array(parent, std::move(key), depth);
        if (c == u'"' || c == u'\'') {
            QString text;
            if (!string(text))
                return false;
            add(Kind::String, parent, std::move(key), std::move(text));
            return true;
        }
        if (c == u'-' || isDigit(c))
            return number(parent, std::move(key));
        if (identifierStart(c))
            return word(parent, std::move(key));
        return fail(Text::tr("这里应该是一个值：对象、数组、字符串、数字、true、false 或 null"), m_i);
    }

    bool object(int parent, QString key, int depth)
    {
        const int node = add(Kind::Object, parent, std::move(key));
        ++m_i;
        skip();
        if (m_i < m_s.size() && m_s[m_i] == u'}') {
            ++m_i;
            return true;
        }
        for (;;) {
            skip();
            if (m_i >= m_s.size())
                return fail(Text::tr("对象没有结束，缺少 }"), m_i);
            const QChar c = m_s[m_i];
            QString name;
            if (c == u'"' || c == u'\'') {
                if (!string(name))
                    return false;
            } else if (identifierStart(c)) {
                const qsizetype start = m_i;
                while (m_i < m_s.size() && identifierPart(m_s[m_i]))
                    ++m_i;
                name = m_s.sliced(start, m_i - start).toString();
                m_d.relaxed |= BareKeys;
            } else if (c == u'}') { // after a comma
                m_d.relaxed |= TrailingCommas;
                ++m_i;
                return true;
            } else {
                return fail(Text::tr("这里应该是一个键，用双引号括起来"), m_i);
            }
            skip();
            if (m_i >= m_s.size() || m_s[m_i] != u':')
                return fail(Text::tr("键“%1”后面缺少冒号").arg(shortened(name)), m_i);
            ++m_i;
            if (!value(node, std::move(name), depth + 1))
                return false;
            const qsizetype after = m_i;
            skip();
            if (m_i >= m_s.size())
                return fail(Text::tr("对象没有结束，缺少 }"), m_i);
            if (m_s[m_i] == u',') {
                ++m_i;
                continue;
            }
            if (m_s[m_i] == u'}') {
                ++m_i;
                return true;
            }
            if (nextLineStarts(after, m_s[m_i] == u'"' || m_s[m_i] == u'\'' || identifierStart(m_s[m_i])))
                return fail(Text::tr("这一行末尾缺少逗号"), after);
            return fail(Text::tr("缺少逗号或 }"), m_i);
        }
    }

    bool array(int parent, QString key, int depth)
    {
        const int node = add(Kind::Array, parent, std::move(key));
        ++m_i;
        skip();
        if (m_i < m_s.size() && m_s[m_i] == u']') {
            ++m_i;
            return true;
        }
        for (;;) {
            skip();
            if (m_i < m_s.size() && m_s[m_i] == u']') { // after a comma
                m_d.relaxed |= TrailingCommas;
                ++m_i;
                return true;
            }
            if (!value(node, {}, depth + 1))
                return false;
            const qsizetype after = m_i;
            skip();
            if (m_i >= m_s.size())
                return fail(Text::tr("数组没有结束，缺少 ]"), m_i);
            if (m_s[m_i] == u',') {
                ++m_i;
                continue;
            }
            if (m_s[m_i] == u']') {
                ++m_i;
                return true;
            }
            if (nextLineStarts(after, true))
                return fail(Text::tr("这一行末尾缺少逗号"), after);
            return fail(Text::tr("缺少逗号或 ]"), m_i);
        }
    }

    bool string(QString& out)
    {
        const qsizetype start = m_i;
        const QChar quote = m_s[m_i];
        if (quote == u'\'')
            m_d.relaxed |= SingleQuotes;
        ++m_i;
        qsizetype run = m_i;
        while (m_i < m_s.size()) {
            const QChar c = m_s[m_i];
            if (c == quote) {
                out += m_s.sliced(run, m_i - run);
                ++m_i;
                return true;
            }
            if (c == u'\\') {
                out += m_s.sliced(run, m_i - run);
                if (m_i + 1 >= m_s.size())
                    break;
                const char16_t e = m_s[m_i + 1].unicode();
                qsizetype length = 2;
                switch (e) {
                case u'"': out += u'"'; break;
                case u'\'': out += u'\''; break;
                case u'\\': out += u'\\'; break;
                case u'/': out += u'/'; break;
                case u'b': out += u'\b'; break;
                case u'f': out += u'\f'; break;
                case u'n': out += u'\n'; break;
                case u'r': out += u'\r'; break;
                case u't': out += u'\t'; break;
                case u'u': {
                    const int code = hexAt(m_s, m_i + 2, 4);
                    if (code < 0)
                        return fail(Text::tr("\\u 后面要有 4 位十六进制数"), m_i);
                    out += QChar(char16_t(code)); // a surrogate pair comes as two of these
                    length = 6;
                    break;
                }
                case u'x': { // Python's repr
                    const int code = hexAt(m_s, m_i + 2, 2);
                    if (code < 0)
                        return fail(Text::tr("\\x 后面要有 2 位十六进制数"), m_i);
                    out += QChar(char16_t(code));
                    m_d.relaxed |= SingleQuotes;
                    length = 4;
                    break;
                }
                default:
                    return fail(Text::tr("转义写法不对：\\%1").arg(QChar(e)), m_i);
                }
                m_i += length;
                run = m_i;
                continue;
            }
            if (c == u'\n' || c == u'\r')
                return fail(Text::tr("字符串没有结束，缺少 %1").arg(quote), start);
            if (c.unicode() < 0x20 && c != u'\t')
                return fail(Text::tr("字符串里的控制字符要写成转义"), m_i);
            ++m_i;
        }
        return fail(Text::tr("字符串没有结束，缺少 %1").arg(quote), start);
    }

    bool number(int parent, QString key)
    {
        const qsizetype start = m_i;
        if (m_s[m_i] == u'-') {
            ++m_i;
            if (m_s.sliced(m_i).startsWith(u"Infinity")) {
                m_i += 8;
                m_d.relaxed |= NonFinite;
                add(Kind::Number, parent, std::move(key), m_s.sliced(start, m_i - start).toString());
                return true;
            }
        }
        const auto digits = [&] {
            const qsizetype from = m_i;
            while (m_i < m_s.size() && isDigit(m_s[m_i]))
                ++m_i;
            return m_i > from;
        };
        if (m_i >= m_s.size() || !isDigit(m_s[m_i]))
            return fail(Text::tr("数字写法不对"), start);
        if (m_s[m_i] == u'0')
            ++m_i;
        else
            digits();
        if (m_i < m_s.size() && m_s[m_i] == u'.') {
            ++m_i;
            if (!digits())
                return fail(Text::tr("数字写法不对：小数点后面要有数字"), start);
        }
        if (m_i < m_s.size() && (m_s[m_i] == u'e' || m_s[m_i] == u'E')) {
            ++m_i;
            if (m_i < m_s.size() && (m_s[m_i] == u'+' || m_s[m_i] == u'-'))
                ++m_i;
            if (!digits())
                return fail(Text::tr("数字写法不对：指数要有数字"), start);
        }
        if (m_i < m_s.size() && (isDigit(m_s[m_i]) || identifierStart(m_s[m_i])))
            return fail(Text::tr("数字写法不对"), start);
        add(Kind::Number, parent, std::move(key), m_s.sliced(start, m_i - start).toString());
        return true;
    }

    bool word(int parent, QString key)
    {
        const qsizetype start = m_i;
        while (m_i < m_s.size() && (m_s[m_i].isLetterOrNumber() || m_s[m_i] == u'_' || m_s[m_i] == u'$'))
            ++m_i;
        const QStringView w = m_s.sliced(start, m_i - start);
        if (w == u"true" || w == u"false") {
            add(Kind::Bool, parent, std::move(key), w.toString());
        } else if (w == u"null") {
            add(Kind::Null, parent, std::move(key), u"null"_s);
        } else if (w == u"True" || w == u"False") {
            m_d.relaxed |= PythonWords;
            add(Kind::Bool, parent, std::move(key), w.toString().toLower());
        } else if (w == u"None" || w == u"undefined") {
            m_d.relaxed |= PythonWords;
            add(Kind::Null, parent, std::move(key), u"null"_s);
        } else if (w == u"NaN" || w == u"Infinity") {
            m_d.relaxed |= NonFinite;
            add(Kind::Number, parent, std::move(key), w.toString());
        } else {
            return fail(Text::tr("“%1”不是 JSON 的值，文字要用引号括起来").arg(shortened(w.toString())), start);
        }
        return true;
    }

    QStringView m_s;
    qsizetype m_i = 0;
    Document& m_d;
    Error m_error;
    bool m_failed = false;
};

// Strings that are JSON themselves, a few levels down: each carries its
// document under it (Node::embedded). Not merged into `relaxed`: that says
// how the text itself was read.
void embed(Document& d)
{
    for (std::size_t n = 0; n < d.nodes.size(); ++n) {
        if (d.nodes[n].kind != Kind::String)
            continue;
        const QStringView t = QStringView(d.nodes[n].text).trimmed();
        if (t.size() < 2 || t.size() > kMostEmbedded || !((t.front() == u'{' && t.back() == u'}') || (t.front() == u'[' && t.back() == u']')))
            continue;
        int levels = 0;
        for (int p = d.nodes[n].parent; p >= 0; p = d.nodes[p].parent)
            levels += d.nodes[p].embedded ? 1 : 0;
        if (levels >= kEmbedLevels)
            continue;
        qsizetype end = 0;
        std::optional<Document> inner = parse(t, 0, &end);
        if (!inner || !onlySpacesAfter(t, end) || !isContainer(*inner) || inner->nodes[0].count == 0)
            continue;
        const int root = graft(d, int(n), std::move(*inner));
        d.nodes[n].embedded = true;
        d.nodes[n].first = root;
    }
}

// JSON string escapes undone, as if `text` stood between quotes: a log's
// {\"a\":1}. What is not an escape stays as it is.
QString unescape(QStringView text)
{
    if (text.size() >= 2 && text.front() == u'"' && text.back() == u'"')
        text = text.sliced(1, text.size() - 2);
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c != u'\\' || i + 1 >= text.size()) {
            out += c;
            continue;
        }
        const char16_t e = text[i + 1].unicode();
        switch (e) {
        case u'"': out += u'"'; ++i; break;
        case u'\\': out += u'\\'; ++i; break;
        case u'/': out += u'/'; ++i; break;
        case u'b': out += u'\b'; ++i; break;
        case u'f': out += u'\f'; ++i; break;
        case u'n': out += u'\n'; ++i; break;
        case u'r': out += u'\r'; ++i; break;
        case u't': out += u'\t'; ++i; break;
        case u'u':
            if (const int code = hexAt(text, i + 2, 4); code >= 0) {
                out += QChar(char16_t(code));
                i += 5;
            } else {
                out += c;
            }
            break;
        default: out += c; break;
        }
    }
    return out;
}

// The largest object or array that starts at one of the first braces from
// `from` on: the JSON in a log line, after its time and level. Braces inside
// one found are not tried again.
std::optional<Document> within(QStringView text, qsizetype from, qsizetype* start, qsizetype* end)
{
    std::optional<Document> best;
    qsizetype bestStart = 0;
    qsizetype bestEnd = 0;
    int tried = 0;
    for (qsizetype k = from; k < text.size() && tried < kMostCandidates; ++k) {
        if (text[k] != u'{' && text[k] != u'[')
            continue;
        ++tried;
        qsizetype stop = 0;
        std::optional<Document> d = parse(text, k, &stop);
        if (!d || !isContainer(*d) || d->nodes[0].count == 0)
            continue;
        if (!best || stop - k > bestEnd - bestStart) {
            best = std::move(d);
            bestStart = k;
            bestEnd = stop;
        }
        k = stop - 1;
    }
    if (best) {
        *start = bestStart;
        *end = bestEnd;
    }
    return best;
}

// One document a line (JSON Lines, or a log whose lines each hold one),
// gathered into an array; nullopt unless at least two lines, and at least
// half of those not blank, hold one.
std::optional<Document> perLine(QStringView text, QStringList& notes)
{
    Document merged;
    merged.nodes.emplace_back().kind = Kind::Array;
    int lines = 0;
    int found = 0;
    int afterWords = 0;
    for (QStringView line : text.tokenize(u'\n')) {
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        ++lines;
        qsizetype end = 0;
        std::optional<Document> d = parse(line, 0, &end);
        if (!d || !onlySpacesAfter(line, end) || !isContainer(*d)) {
            qsizetype start = 0;
            d = within(line, 0, &start, &end);
            if (!d)
                continue;
            ++afterWords;
        }
        merged.relaxed |= d->relaxed;
        link(merged, 0, graft(merged, 0, std::move(*d)));
        ++found;
    }
    if (found < 2 || found * 2 < lines)
        return std::nullopt;
    notes << (afterWords > 0 ? Text::tr("从 %n 行日志里各取出一段 JSON，合成数组显示", "", found)
                             : Text::tr("共 %n 行，每行一段 JSON，合成数组显示", "", found));
    if (lines > found)
        notes << Text::tr("%n 行不是 JSON，没有读", "", lines - found);
    return merged;
}

// JSON in quotes: what is in them, read in turn (a string of a string ...).
void unwrap(Reading& r)
{
    for (int round = 0; round < 3 && r.document.nodes[0].kind == Kind::String; ++round) {
        const QString text = r.document.nodes[0].text;
        const QStringView t = QStringView(text).trimmed();
        if (t.isEmpty() || (t.front() != u'{' && t.front() != u'['))
            return;
        qsizetype end = 0;
        std::optional<Document> inner = parse(t, 0, &end);
        if (!inner || !onlySpacesAfter(t, end) || !isContainer(*inner))
            return;
        r.document = std::move(*inner);
        if (round == 0)
            r.notes << Text::tr("去掉了外面的引号和转义");
    }
}

Reading finish(Reading r)
{
    embed(r.document);
    if (r.document.relaxed)
        r.notes << Text::tr("按宽松写法读取：%1").arg(relaxations(r.document.relaxed));
    return r;
}

void write(const Document& d, int n, Layout layout, const QString& indent, int level, QString& out)
{
    const Node& node = d.nodes[n];
    const bool pretty = layout == Layout::Pretty;
    switch (node.kind) {
    case Kind::Object:
    case Kind::Array: {
        const bool object = node.kind == Kind::Object;
        if (node.count == 0) {
            out += object ? u"{}" : u"[]";
            return;
        }
        out += object ? u'{' : u'[';
        for (int c = node.first; c >= 0; c = d.nodes[c].next) {
            if (c != node.first)
                out += u',';
            if (pretty) {
                out += u'\n';
                for (int i = 0; i <= level; ++i)
                    out += indent;
            }
            if (object) {
                out += quoted(d.nodes[c].key);
                out += pretty ? u": " : u":";
            }
            write(d, c, layout, indent, level + 1, out);
        }
        if (pretty) {
            out += u'\n';
            for (int i = 0; i < level; ++i)
                out += indent;
        }
        out += object ? u'}' : u']';
        return;
    }
    case Kind::String:
        out += quoted(node.text);
        return;
    case Kind::Number:
    case Kind::Bool:
    case Kind::Null:
        out += node.text;
        return;
    }
}

} // namespace

bool Document::expandable(int node) const
{
    const Node& n = nodes[node];
    if (n.embedded)
        return true;
    return (n.kind == Kind::Object || n.kind == Kind::Array) && n.count > 0;
}

std::optional<Document> parse(QStringView text, qsizetype from, qsizetype* end, Error* error)
{
    Document d;
    Parser parser(text, from, d);
    if (!parser.run()) {
        if (error) {
            *error = parser.error();
            locate(text, *error);
        }
        return std::nullopt;
    }
    if (end)
        *end = parser.at();
    return d;
}

Reading read(const QString& input)
{
    Reading r;
    if (input.size() > kMostCharacters) {
        r.error = Error {Text::tr("内容太大，最多读 %1 MB").arg(kMostCharacters / 1024 / 1024)};
        return r;
    }
    const QStringView text(input);
    const qsizetype start = skipSpaces(text, 0);
    if (start >= text.size()) {
        r.empty = true;
        return r;
    }
    Error first;
    qsizetype end = 0;
    std::optional<Document> whole = parse(text, start, &end, &first);
    if (whole && onlySpacesAfter(text, end)) {
        r.document = std::move(*whole);
        unwrap(r);
        return finish(std::move(r));
    }
    if (!whole && text.contains(u"\\\"")) {
        const QString plain = unescape(text.sliced(start));
        qsizetype stop = 0;
        const qsizetype from = skipSpaces(plain, 0);
        if (std::optional<Document> d = parse(plain, from, &stop); d && onlySpacesAfter(plain, stop) && isContainer(*d)) {
            r.document = std::move(*d);
            r.notes << Text::tr("去掉了转义（\\\"）");
            return finish(std::move(r));
        }
    }
    // A document that starts the text but breaks is shown as its error, not
    // as a guess made of its pieces: those lie before where reading failed.
    // A log line's "[2026-11-18 10:00] INFO {...}" fails before its JSON.
    const bool startsAsJson = text[start] == u'{' || text[start] == u'[';
    const qsizetype lineEnd = text.indexOf(u'\n', start);
    qsizetype from = 0;
    qsizetype stop = 0;
    const bool firstLineHolds = startsAsJson && !whole
        && within(text.sliced(start, lineEnd < 0 ? text.size() - start : lineEnd - start), 0, &from, &stop).has_value();
    if (lineEnd >= 0 && (whole || !startsAsJson || firstLineHolds)) {
        if (std::optional<Document> lines = perLine(text, r.notes)) {
            r.document = std::move(*lines);
            return finish(std::move(r));
        }
    }
    if (std::optional<Document> d = within(text, whole || !startsAsJson ? start : first.offset, &from, &stop)) {
        r.document = std::move(*d);
        if (from > start)
            r.notes << Text::tr("跳过了前面 %n 个字符", "", int(from - start));
        if (!onlySpacesAfter(text, stop))
            r.notes << Text::tr("后面还有 %n 个字符没有读", "", int(text.size() - skipSpaces(text, stop)));
        return finish(std::move(r));
    }
    if (whole) { // a value with words after it, not an object or array: those are the error
        first.message = Text::tr("值的后面还有多余的内容");
        first.offset = skipSpaces(text, end);
        locate(text, first);
    }
    r.error = first;
    return r;
}

QString write(const Document& document, int node, Layout layout, const QString& indent)
{
    QString out;
    if (node >= 0 && node < int(document.nodes.size()))
        write(document, node, layout, indent, 0, out);
    return out;
}

QString quoted(QStringView text)
{
    QString out;
    out.reserve(text.size() + 2);
    out += u'"';
    for (const QChar c : text) {
        switch (c.unicode()) {
        case u'"': out += u"\\\""; break;
        case u'\\': out += u"\\\\"; break;
        case u'\b': out += u"\\b"; break;
        case u'\f': out += u"\\f"; break;
        case u'\n': out += u"\\n"; break;
        case u'\r': out += u"\\r"; break;
        case u'\t': out += u"\\t"; break;
        case 0x2028: out += u"\\u2028"; break; // a line break to JavaScript
        case 0x2029: out += u"\\u2029"; break;
        default:
            if (c.unicode() < 0x20)
                out += u"\\u00" + QString::number(c.unicode(), 16).rightJustified(2, u'0');
            else
                out += c;
        }
    }
    out += u'"';
    return out;
}

QString escaped(QStringView json)
{
    const QString q = quoted(json);
    return q.sliced(1, q.size() - 2);
}

QString describe(const Document& document, int node)
{
    if (node < 0 || node >= int(document.nodes.size()))
        return {};
    switch (document.shape(node)) {
    case Kind::Object: return Text::tr("对象 · %n 个键", "", document.childCount(node));
    case Kind::Array: return Text::tr("数组 · %n 项", "", document.childCount(node));
    case Kind::String: return Text::tr("字符串 · %n 个字", "", int(document.nodes[node].text.size()));
    case Kind::Number: return Text::tr("数字");
    case Kind::Bool: return Text::tr("布尔值");
    case Kind::Null: return u"null"_s;
    }
    return {};
}

QString relaxations(unsigned relaxed)
{
    QStringList parts;
    if (relaxed & Comments)
        parts << Text::tr("注释");
    if (relaxed & SingleQuotes)
        parts << Text::tr("单引号");
    if (relaxed & BareKeys)
        parts << Text::tr("没加引号的键");
    if (relaxed & TrailingCommas)
        parts << Text::tr("多余的逗号");
    if (relaxed & PythonWords)
        parts << u"True / False / None"_s;
    if (relaxed & NonFinite)
        parts << u"NaN / Infinity"_s;
    return parts.join(Text::tr("、"));
}

} // namespace ws::devtools
