#include "Latex.h"

#include "LatexSymbols.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws::capture::latex {

Node Node::row(std::vector<Node> items, bool braced)
{
    Node n;
    n.kind = Kind::Row;
    n.items = std::move(items);
    n.braced = braced;
    return n;
}

Node Node::atom(const QString& text)
{
    Node n;
    n.kind = Kind::Atom;
    n.text = text;
    return n;
}

namespace {

using Kind = Node::Kind;

bool isAsciiLetter(QChar c)
{
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
}

template <std::size_t N>
bool oneOf(QStringView text, const std::array<QStringView, N>& list)
{
    return std::ranges::find(list, text) != list.end();
}

constexpr std::array kTwoArguments = std::to_array<QStringView>({u"\\frac", u"\\dfrac", u"\\tfrac", u"\\cfrac",
    u"\\binom", u"\\dbinom", u"\\tbinom", u"\\overset", u"\\underset", u"\\stackrel", u"\\textcolor", u"\\colorbox"});
constexpr std::array kOneArgument = std::to_array<QStringView>({u"\\operatorname", u"\\operatorname*", u"\\mathop",
    u"\\mathbin", u"\\mathrel", u"\\mathord", u"\\mathopen", u"\\mathclose", u"\\mathpunct", u"\\mathinner",
    u"\\boxed", u"\\fbox", u"\\phantom", u"\\hphantom", u"\\vphantom", u"\\smash", u"\\cancel", u"\\bcancel",
    u"\\xcancel", u"\\sout", u"\\pmod", u"\\pod", u"\\substack", u"\\tag", u"\\tag*", u"\\hspace", u"\\hspace*",
    u"\\vspace", u"\\mspace", u"\\ensuremath", u"\\overbrace", u"\\underbrace", u"\\label", u"\\color"});
constexpr std::array kArrows = std::to_array<QStringView>({u"\\xrightarrow", u"\\xleftarrow", u"\\xRightarrow",
    u"\\xLeftarrow", u"\\xleftrightarrow", u"\\xLeftrightarrow", u"\\xmapsto", u"\\xlongequal",
    u"\\xhookrightarrow", u"\\xhookleftarrow", u"\\xtofrom", u"\\xrightleftharpoons"});
constexpr std::array kTextCommands = std::to_array<QStringView>({u"\\text", u"\\textrm", u"\\textbf", u"\\textit",
    u"\\textsf", u"\\texttt", u"\\textnormal", u"\\textup", u"\\textmd", u"\\mbox", u"\\hbox", u"\\textsl"});
constexpr std::array kSizes = std::to_array<QStringView>({u"\\big", u"\\Big", u"\\bigg", u"\\Bigg", u"\\bigl",
    u"\\Bigl", u"\\biggl", u"\\Biggl", u"\\bigr", u"\\Bigr", u"\\biggr", u"\\Biggr", u"\\bigm", u"\\Bigm",
    u"\\biggm", u"\\Biggm", u"\\middle"});
// Switches that change what follows them in the group, as TeX had them before \mathbf.
struct Switch {
    QStringView name;
    QStringView command;
};
constexpr std::array kSwitches = std::to_array<Switch>({{u"\\bf", u"\\mathbf"}, {u"\\rm", u"\\mathrm"},
    {u"\\it", u"\\mathit"}, {u"\\cal", u"\\mathcal"}, {u"\\sf", u"\\mathsf"}, {u"\\tt", u"\\mathtt"},
    {u"\\mit", u"\\mathit"}, {u"\\boldmath", u"\\boldsymbol"}});
struct Infix {
    QStringView name;
    QStringView command;
};
constexpr std::array kInfix = std::to_array<Infix>({{u"\\over", u"\\frac"}, {u"\\atop", u"\\frac"},
    {u"\\choose", u"\\binom"}, {u"\\above", u"\\frac"}, {u"\\brace", u"\\binom"}, {u"\\brack", u"\\binom"}});
// What changes only the size or the style: gone in a tidied formula.
constexpr std::array kStyles = std::to_array<QStringView>({u"\\displaystyle", u"\\textstyle", u"\\scriptstyle",
    u"\\scriptscriptstyle", u"\\tiny", u"\\scriptsize", u"\\footnotesize", u"\\small", u"\\normalsize", u"\\large",
    u"\\Large", u"\\LARGE", u"\\huge", u"\\Huge", u"\\nonumber", u"\\notag", u"\\limits", u"\\displaylimits",
    u"\\strut", u"\\mathstrut", u"\\allowbreak", u"\\nobreak", u"\\relax", u"\\protect", u"\\normalfont",
    u"\\unboldmath", u"\\hfill", u"\\hfil", u"\\centering", u"\\noindent"});
// Lines in tables, which a formula's matrix does not draw.
constexpr std::array kRules = std::to_array<QStringView>({u"\\hline", u"\\hdashline", u"\\toprule", u"\\midrule",
    u"\\bottomrule"});
// The characters \text{...} writes with a backslash (\%).
constexpr QStringView kTextEscapes = u"{}%&#_$";

struct Token {
    enum class Type : quint8 { Command, Char, Open, Close, Sup, Sub, Align, Prime, End };
    Type type = Type::End;
    QString text;
};
using T = Token::Type;

class Parser {
public:
    Parser(QStringView source, Source as)
        : m_s(source)
        , m_model(as == Source::Model)
    {
    }

    Node document()
    {
        std::vector<Node> items;
        for (;;) {
            appendRow(items, row());
            if (next().type == T::End) // else a } that closes nothing: left out
                break;
        }
        return Node::row(std::move(items));
    }

private:
    QStringView m_s;
    qsizetype m_pos = 0;
    bool m_model = false;
    int m_depth = 0;
    int m_fences = 0; // \left ... being read: \right closes it
    int m_environments = 0; // \begin ... being read: & \\ \end end its cells

    void skipSpace()
    {
        while (m_pos < m_s.size()) {
            const QChar c = m_s[m_pos];
            if (c.isSpace()) {
                ++m_pos;
            } else if (c == u'%') {
                while (m_pos < m_s.size() && m_s[m_pos] != u'\n')
                    ++m_pos;
            } else {
                break;
            }
        }
    }

    Token next()
    {
        skipSpace();
        if (m_pos >= m_s.size())
            return {};
        const QChar c = m_s[m_pos];
        if (c == u'\\') {
            if (m_pos + 1 >= m_s.size()) {
                ++m_pos;
                return {T::Char, u"\\"_s};
            }
            const QChar d = m_s[m_pos + 1];
            if (isAsciiLetter(d)) {
                qsizetype end = m_pos + 1;
                while (end < m_s.size() && isAsciiLetter(m_s[end]))
                    ++end;
                QString name = m_s.mid(m_pos, end - m_pos).toString();
                // \operatorname* and the like; spaces between them in the model's spelling.
                qsizetype after = end;
                while (m_model && after < m_s.size() && m_s[after].isSpace())
                    ++after;
                if (after < m_s.size() && m_s[after] == u'*'
                    && (name == u"\\operatorname" || name == u"\\tag" || name == u"\\hspace")) {
                    name += u'*';
                    end = after + 1;
                }
                m_pos = end;
                return {T::Command, name};
            }
            const qsizetype length = d.isHighSurrogate() && m_pos + 2 < m_s.size() ? 3 : 2;
            const QString name = m_s.mid(m_pos, length).toString();
            m_pos += length;
            return {T::Command, name};
        }
        ++m_pos;
        switch (c.unicode()) {
        case u'{': return {T::Open, u"{"_s};
        case u'}': return {T::Close, u"}"_s};
        case u'^': return {T::Sup, u"^"_s};
        case u'_': return {T::Sub, u"_"_s};
        case u'&': return {T::Align, u"&"_s};
        case u'\'': return {T::Prime, u"'"_s};
        case u'~': return {T::Command, u"~"_s};
        default: break;
        }
        if (c.isHighSurrogate() && m_pos < m_s.size() && m_s[m_pos].isLowSurrogate()) {
            ++m_pos;
            return {T::Char, m_s.mid(m_pos - 2, 2).toString()};
        }
        return {T::Char, QString(c)};
    }

    Token peek()
    {
        const qsizetype at = m_pos;
        Token t = next();
        m_pos = at;
        return t;
    }

    // What stops a row: the end, a closing brace, a cell's or a row's end,
    // \end, \right; and ] in a [optional] argument. Those of them that
    // close nothing being read are not stops: row() takes them in.
    bool stops(const Token& t, bool optional) const
    {
        switch (t.type) {
        case T::End:
        case T::Close:
            return true;
        case T::Align:
            return m_environments > 0;
        case T::Command:
            if (t.text == u"\\right")
                return m_fences > 0;
            return m_environments > 0 && (t.text == u"\\\\" || t.text == u"\\cr" || t.text == u"\\end");
        case T::Char:
            return optional && t.text == u"]";
        default:
            return false;
        }
    }

    static void appendRow(std::vector<Node>& items, Node part)
    {
        if (part.kind == Kind::Row && !part.braced) {
            for (Node& n : part.items)
                items.push_back(std::move(n));
        } else {
            items.push_back(std::move(part));
        }
    }

    // Items up to what stops the row, which is left for the caller.
    Node row(bool optional = false)
    {
        std::vector<Node> items;
        if (++m_depth > 200) { // a formula is never this deep: whatever it is, it is not read further
            --m_depth;
            m_pos = m_s.size();
            return Node::row();
        }
        for (;;) {
            const Token t = peek();
            if (stops(t, optional))
                break;
            next();
            if (t.type == T::Align) // a cell's end with no matrix around it
                continue;
            if (t.type == T::Command) {
                // \right with no \left: the delimiter as it is; \end with no \begin: nothing.
                if (t.text == u"\\right") {
                    if (const QString d = delimiter(); d != u".")
                        items.push_back(Node::atom(d));
                    continue;
                }
                if (t.text == u"\\end") {
                    rawGroup();
                    continue;
                }
                if (t.text == u"\\cr") {
                    items.push_back(Node::atom(u"\\\\"_s));
                    continue;
                }
                if (const auto sw = std::ranges::find(kSwitches, QStringView(t.text), &Switch::name); sw != kSwitches.end()) {
                    Node rest = row(optional);
                    Node c;
                    c.kind = Kind::Command;
                    c.text = sw->command.toString();
                    rest.braced = false;
                    c.items.push_back(std::move(rest));
                    items.push_back(std::move(c));
                    break;
                }
                if (const auto in = std::ranges::find(kInfix, QStringView(t.text), &Infix::name); in != kInfix.end()) {
                    if (t.text == u"\\above")
                        dimension();
                    Node c;
                    c.kind = Kind::Command;
                    c.text = in->command.toString();
                    c.items.push_back(Node::row(std::move(items)));
                    Node denominator = row(optional);
                    denominator.braced = false;
                    c.items.push_back(std::move(denominator));
                    items.clear();
                    items.push_back(std::move(c));
                    break;
                }
                if (oneOf(QStringView(t.text), kRules))
                    continue;
                if (t.text == u"\\cline") {
                    rawGroup();
                    continue;
                }
                if (t.text == u"\\color") { // a colour for what follows: not kept
                    rawGroup();
                    continue;
                }
            }
            std::optional<Node> item = this->item(t);
            if (!item)
                continue;
            Node withScripts = scripts(std::move(*item));
            appendRow(items, std::move(withScripts));
        }
        --m_depth;
        return Node::row(std::move(items));
    }

    // One thing, its scripts not yet.
    std::optional<Node> item(const Token& t)
    {
        switch (t.type) {
        case T::Open: {
            Node group = row();
            if (peek().type == T::Close)
                next();
            group.braced = true;
            return group;
        }
        case T::Char:
            return Node::atom(t.text);
        case T::Sup:
        case T::Sub:
        case T::Prime: {
            // A script with nothing before it: on an empty base ({}^{14}C).
            m_pos -= t.text.size(); // given back to scripts(), which reads it
            return Node::row({}, true);
        }
        case T::Command:
            return command(t.text);
        default:
            return std::nullopt;
        }
    }

    Node scripts(Node base)
    {
        Node s;
        bool any = false;
        QString limits;
        for (;;) {
            const Token t = peek();
            if (t.type == T::Command && (t.text == u"\\limits" || t.text == u"\\nolimits")) {
                next();
                limits = t.text;
                continue;
            }
            if (t.type != T::Sup && t.type != T::Sub && t.type != T::Prime)
                break;
            if (!any) {
                s.kind = Kind::Scripts;
                s.items.push_back(std::move(base));
                s.items.push_back(Node::row());
                s.items.push_back(Node::row());
                any = true;
            }
            next();
            Node& sub = s.items[1];
            Node& sup = s.items[2];
            if (t.type == T::Prime) {
                sup.items.push_back(Node::atom(u"\\prime"_s));
                continue;
            }
            Node argument = this->argument();
            const bool primesOnly = std::ranges::all_of(sup.items, [](const Node& n) { return n.kind == Kind::Atom && n.text == u"\\prime"; });
            Node& target = t.type == T::Sub ? sub : sup;
            if (!target.isEmpty() && !(t.type == T::Sup && primesOnly)) {
                // A second one (x^2^3): the scripts so far are the base of new ones.
                Node outer;
                outer.kind = Kind::Scripts;
                outer.items.push_back(std::move(s));
                outer.items.push_back(Node::row());
                outer.items.push_back(Node::row());
                s = std::move(outer);
                appendRow(s.items[t.type == T::Sub ? 1 : 2].items, std::move(argument));
                continue;
            }
            for (Node& n : argument.items)
                target.items.push_back(std::move(n));
        }
        if (!any) {
            if (!limits.isEmpty() && base.kind == Kind::Atom)
                base.close = limits;
            return base;
        }
        s.close = limits;
        return s;
    }

    // A command's argument, or a script's: a group, or the one thing that comes next.
    Node argument()
    {
        const Token t = peek();
        switch (t.type) {
        case T::Open: {
            next();
            Node group = row();
            if (peek().type == T::Close)
                next();
            return group;
        }
        case T::Char:
            next();
            return Node::row({Node::atom(t.text)});
        case T::Command: {
            if (stops(t, false))
                return Node::row();
            next();
            Node inner = command(t.text);
            if (inner.kind == Kind::Row && !inner.braced)
                return inner;
            return Node::row({std::move(inner)});
        }
        default:
            return Node::row();
        }
    }

    // {...} as it is written, for words and names; empty when no brace follows.
    QString rawGroup()
    {
        skipSpace();
        if (m_pos >= m_s.size() || m_s[m_pos] != u'{')
            return {};
        int depth = 0;
        const qsizetype start = m_pos + 1;
        for (; m_pos < m_s.size(); ++m_pos) {
            const QChar c = m_s[m_pos];
            if (c == u'\\') {
                ++m_pos;
                continue;
            }
            if (c == u'{') {
                ++depth;
            } else if (c == u'}' && --depth == 0) {
                const QString inside = m_s.mid(start, m_pos - start).toString();
                ++m_pos;
                return inside;
            }
        }
        return m_s.mid(start).toString();
    }

    // \kern 3mu and the like: the size after the command, read and dropped.
    void dimension()
    {
        skipSpace();
        if (m_pos < m_s.size() && m_s[m_pos] == u'{') {
            rawGroup();
            return;
        }
        while (m_pos < m_s.size() && (m_s[m_pos].isDigit() || m_s[m_pos] == u'.' || m_s[m_pos] == u'-'
                                         || m_s[m_pos] == u'+' || m_s[m_pos].isSpace()))
            ++m_pos;
        while (m_pos < m_s.size() && isAsciiLetter(m_s[m_pos]))
            ++m_pos;
    }

    QString delimiter()
    {
        const Token t = next();
        switch (t.type) {
        case T::Char:
        case T::Command:
            return t.text;
        case T::Open:
            return u"\\{"_s;
        case T::Close:
            return u"\\}"_s;
        default:
            return u"."_s;
        }
    }

    Node command(const QString& name)
    {
        Node c;
        c.kind = Kind::Command;
        c.text = name;
        if (name == u"\\left") {
            Node fence;
            fence.kind = Kind::Fence;
            fence.open = delimiter();
            ++m_fences;
            Node inside = row();
            --m_fences;
            const Token t = peek();
            if (t.type == T::Command && t.text == u"\\right") {
                next();
                fence.close = delimiter();
                inside.braced = false;
                fence.items.push_back(std::move(inside));
                return fence;
            }
            // No \right before the group ends: the delimiter alone.
            std::vector<Node> items;
            if (fence.open != u".")
                items.push_back(Node::atom(fence.open));
            appendRow(items, std::move(inside));
            return Node::row(std::move(items));
        }
        if (oneOf(QStringView(name), kSizes)) {
            Node sized;
            sized.kind = Kind::Sized;
            sized.text = name;
            sized.open = delimiter();
            return sized;
        }
        if (name == u"\\begin")
            return environment();
        if (oneOf(QStringView(name), kTextCommands)) {
            Node text;
            text.kind = Kind::Text;
            text.open = name;
            skipSpace();
            if (m_pos < m_s.size() && m_s[m_pos] == u'{')
                text.text = words(rawGroup());
            else if (const Token t = next(); t.type == T::Char) // \text x
                text.text = t.text;
            return text;
        }
        if (name == u"\\sqrt" || oneOf(QStringView(name), kArrows)) {
            if (peek().type == T::Char && peek().text == u"[") {
                next();
                Node index = row(true);
                if (peek().type == T::Char && peek().text == u"]")
                    next();
                index.braced = false;
                c.items.push_back(std::move(index));
                c.optional = true;
            }
            c.items.push_back(argument());
            return c;
        }
        if (name == u"\\root") { // \root 3 \of x
            std::vector<Node> index;
            for (;;) {
                const Token t = peek();
                if (stops(t, false))
                    break;
                next();
                if (t.type == T::Command && t.text == u"\\of")
                    break;
                if (std::optional<Node> part = item(t))
                    appendRow(index, scripts(std::move(*part)));
            }
            c.text = u"\\sqrt"_s;
            c.optional = true;
            c.items.push_back(Node::row(std::move(index)));
            c.items.push_back(argument());
            return c;
        }
        if (name == u"\\genfrac") { // \genfrac{(}{)}{0pt}{}{n}{k}: the last two, as a fraction
            for (int i = 0; i < 4; ++i)
                rawGroup();
            c.text = u"\\frac"_s;
            c.items.push_back(argument());
            c.items.push_back(argument());
            return c;
        }
        if (name == u"\\kern" || name == u"\\mkern" || name == u"\\hskip" || name == u"\\mskip") {
            dimension();
            return Node::atom(u"\\,"_s);
        }
        if (oneOf(QStringView(name), kTwoArguments)) {
            c.items.push_back(argument());
            c.items.push_back(argument());
            return c;
        }
        if (oneOf(QStringView(name), kOneArgument) || accent(name) || font(name)) {
            c.items.push_back(argument());
            return c;
        }
        return Node::atom(name);
    }

    // What \text{...} says, its escapes (\{ \%) as the characters: ~ is a
    // space, a run of white space one; in the model's spelling white space
    // means nothing (only ~ is a space there).
    QString words(const QString& raw) const
    {
        QString out;
        out.reserve(raw.size());
        const auto space = [&out] {
            if (!out.endsWith(u' '))
                out += u' ';
        };
        for (qsizetype i = 0; i < raw.size(); ++i) {
            const QChar c = raw[i];
            if (c == u'\\' && i + 1 < raw.size()) {
                const QChar d = raw[i + 1];
                if (d == u' ' || d == u'~' || d == u',' || d == u';') {
                    space();
                    ++i;
                    continue;
                }
                if (kTextEscapes.contains(d)) {
                    out += d;
                    ++i;
                    continue;
                }
                out += c;
                continue;
            }
            if (c == u'~') {
                space();
            } else if (c.isSpace()) {
                if (!m_model)
                    space();
            } else {
                out += c;
            }
        }
        return out;
    }

    Node environment()
    {
        Node env;
        env.kind = Kind::Environment;
        env.text = rawGroup().remove(u' ');
        if (env.text == u"array" || env.text == u"subarray" || env.text == u"tabular") {
            skipSpace();
            if (m_pos < m_s.size() && m_s[m_pos] == u'[') { // [t]
                while (m_pos < m_s.size() && m_s[m_pos] != u']')
                    ++m_pos;
                ++m_pos;
            }
            env.open = rawGroup().remove(u' ');
        }
        Node line = Node::row();
        ++m_environments;
        const int fences = std::exchange(m_fences, 0); // a \right in a cell closes nothing outside it
        for (;;) {
            Node cell = row();
            cell.braced = false;
            line.items.push_back(std::move(cell));
            const Token t = peek();
            if (t.type == T::Align) {
                next();
                continue;
            }
            if (t.type == T::Command && (t.text == u"\\\\" || t.text == u"\\cr")) {
                next();
                skipSpace();
                if (m_pos < m_s.size() && m_s[m_pos] == u'[') { // \\[2pt]
                    while (m_pos < m_s.size() && m_s[m_pos] != u']')
                        ++m_pos;
                    ++m_pos;
                }
                env.items.push_back(std::exchange(line, Node::row()));
                continue;
            }
            if (t.type == T::Command && t.text == u"\\end") {
                next();
                rawGroup();
            }
            // \end, or the end of the source or of a group before one.
            break;
        }
        --m_environments;
        m_fences = fences;
        env.items.push_back(std::move(line));
        return env;
    }
};

// ---- tidy ------------------------------------------------------------------------------

bool isSpace(const Node& n)
{
    return n.kind == Kind::Atom && spaceWidth(n.text) >= 0;
}

// The letters of an argument (\operatorname{s i n}), with its spaces as
// they are in `spaced`; empty when it holds anything else.
QString lettersOf(const Node& row, bool* spaced = nullptr)
{
    QString name;
    if (spaced)
        *spaced = false;
    for (const Node& n : row.items) {
        if (n.kind == Kind::Atom && !n.text.isEmpty() && !n.text.startsWith(u'\\')
            && std::ranges::all_of(n.text, [](QChar c) { return c.isLetterOrNumber(); })) {
            name += n.text;
        } else if (isSpace(n) && n.text != u"\\!") {
            name += u' ';
            if (spaced)
                *spaced = true;
        } else if (n.kind == Kind::Text) {
            name += n.text;
        } else {
            return {};
        }
    }
    return name;
}

// A node standing alone: a Row of one item as that item.
const Node& single(const Node& n)
{
    return n.kind == Kind::Row && n.items.size() == 1 ? single(n.items.front()) : n;
}

bool limitTaker(const Node& n)
{
    return n.kind == Kind::Atom && takesLimits(n.text);
}

bool unknownCommand(const Node& n)
{
    return n.kind == Kind::Atom && n.text.size() > 1 && n.text.front() == u'\\' && !symbol(n.text)
        && spaceWidth(n.text) < 0 && classOf(n.text) == Class::Ord && !oneOf(QStringView(n.text), kStyles);
}

Node tidyNode(Node n);

std::vector<Node> tidyItems(std::vector<Node> in)
{
    std::vector<Node> items;
    items.reserve(in.size());
    for (Node& raw : in) {
        Node n = tidyNode(std::move(raw));
        if (n.kind == Kind::Atom && oneOf(QStringView(n.text), kStyles))
            continue;
        if (n.kind == Kind::Atom && (n.text == u"\\!" || n.text == u"\\negthinspace" || n.text == u"\\negmedspace"
                                        || n.text == u"\\negthickspace"))
            continue;
        if (n.kind == Kind::Row) {
            // Braces around what follows a command nothing here knows stay: they may be its argument.
            if (n.braced && !items.empty() && unknownCommand(items.back())) {
                items.push_back(std::move(n));
                continue;
            }
            for (Node& inner : n.items)
                items.push_back(std::move(inner));
            continue;
        }
        items.push_back(std::move(n));
    }
    // \not= as \neq, \not\in as \notin.
    for (std::size_t i = 0; i + 1 < items.size(); ++i) {
        if (items[i].kind == Kind::Atom && items[i].text == u"\\not" && items[i + 1].kind == Kind::Atom) {
            const QString& next = items[i + 1].text;
            const QString joined = next == u"=" ? u"\\neq"_s : next == u"\\in" ? u"\\notin"_s : QString();
            if (!joined.isEmpty()) {
                items[i] = Node::atom(joined);
                items.erase(items.begin() + static_cast<std::ptrdiff_t>(i) + 1);
            }
        }
    }
    // Spaces: none at the ends of a row, one for those that come together
    // (the widest), and no thin one (narrower than \quad) where TeX puts
    // its own space or none belongs: after an operator, a relation, a
    // punctuation mark or an opening, and before an operator, a relation,
    // a punctuation mark or a closing. The model writes them there
    // (x \, = \, 1); a thin one elsewhere is meant (f(x) \, dx).
    const auto classIs = [](const Node& n, std::initializer_list<Class> classes) {
        return n.kind == Kind::Atom && std::ranges::find(classes, classOf(n.text)) != classes.end();
    };
    const auto thin = [](const Node& n) { return spaceWidth(n.text) < 1; };
    std::vector<Node> out;
    out.reserve(items.size());
    for (Node& n : items) {
        if (isSpace(n)) {
            if (out.empty())
                continue;
            if (isSpace(out.back())) {
                if (spaceWidth(n.text) > spaceWidth(out.back().text))
                    out.back() = std::move(n);
                continue;
            }
            if (thin(n) && classIs(out.back(), {Class::Bin, Class::Rel, Class::Punct, Class::Open}))
                continue;
        } else if (classIs(n, {Class::Bin, Class::Rel, Class::Punct, Class::Close})) {
            while (!out.empty() && isSpace(out.back()) && thin(out.back()))
                out.pop_back();
        }
        out.push_back(std::move(n));
    }
    while (!out.empty() && isSpace(out.back()))
        out.pop_back();
    return out;
}

Node tidyRow(Node row)
{
    row.items = tidyItems(std::move(row.items));
    return row;
}

// \underset{a}{X} and \overset{b}{X}, X an operator with limits: X_a^b.
std::optional<Node> asLimits(const QString& command, Node script, Node base)
{
    const Node& b = single(base);
    const bool under = command == u"\\underset";
    if (limitTaker(b)) {
        Node s;
        s.kind = Kind::Scripts;
        s.items = {b, Node::row(), Node::row()};
        s.items[under ? 1 : 2] = std::move(script);
        return s;
    }
    if (b.kind == Kind::Scripts && limitTaker(b.items[0]) && b.items[under ? 1 : 2].isEmpty()) {
        Node s = b;
        s.items[under ? 1 : 2] = std::move(script);
        return s;
    }
    return std::nullopt;
}

Node tidyCommand(Node c)
{
    const QString& name = c.text;
    // An upright name, read from the argument as it was written, its spaces
    // still there: \mathrm{~or~} is words, \mathrm{l n} the logarithm.
    const bool naming = !c.items.empty()
        && (name == u"\\operatorname" || name == u"\\operatorname*" || name == u"\\mathrm" || name == u"\\mathop");
    bool spaced = false;
    const QString letters = naming ? lettersOf(c.items[0], &spaced) : QString();
    for (Node& argument : c.items) {
        argument = tidyRow(std::move(argument));
        argument.braced = false;
    }
    if (naming) {
        const QString joined = QString(letters).remove(u' ');
        const bool operatorName = name == u"\\operatorname" || name == u"\\operatorname*";
        if (!spaced && isFunctionName(joined))
            return Node::atom(u'\\' + joined);
        if (!joined.isEmpty() && std::ranges::all_of(joined, [](QChar ch) { return ch.isDigit(); }))
            return Node::row(std::move(c.items[0].items)); // digits are upright anyway
        if (!spaced && joined.size() > 1) {
            // A name of more letters (\mathrm{kg}, \operatorname{rank}): one atom of them.
            c.items[0] = Node::row({Node::atom(joined)});
            if (name == u"\\mathop")
                c.text = u"\\operatorname"_s;
            return c;
        }
        if (spaced && !joined.isEmpty() && name == u"\\mathrm") { // \mathrm{~or~}: words
            Node text;
            text.kind = Kind::Text;
            text.open = u"\\text"_s;
            text.text = letters;
            return text;
        }
        if ((name == u"\\mathrm" && !joined.isEmpty()) || operatorName)
            return c;
        // \mathop{=}, \mathop{d x}, \mathrm{ x = ... }: what is inside, as it is.
        return Node::row(std::move(c.items[0].items));
    }
    if (name == u"\\hspace" || name == u"\\hspace*" || name == u"\\mspace")
        return Node::atom(u"\\quad"_s);
    if (name == u"\\mathbin" || name == u"\\mathrel" || name == u"\\mathord" || name == u"\\mathopen"
        || name == u"\\mathclose" || name == u"\\mathpunct" || name == u"\\mathinner" || name == u"\\ensuremath"
        || name == u"\\textcolor" || name == u"\\colorbox") {
        if (c.items.empty())
            return Node::row();
        return Node::row(std::move(c.items.back().items));
    }
    if (name == u"\\label" || name == u"\\tag" || name == u"\\tag*" || name == u"\\vspace" || name == u"\\color")
        return Node::row();
    if ((name == u"\\underset" || name == u"\\overset" || name == u"\\stackrel") && c.items.size() == 2) {
        if (std::optional<Node> limits = asLimits(name == u"\\stackrel" ? u"\\overset"_s : name, c.items[0], c.items[1]))
            return std::move(*limits);
        return c;
    }
    // An accent, a font or a box over nothing: nothing.
    if ((accent(name) || font(name) || name == u"\\boxed") && c.items.size() == 1 && c.items[0].isEmpty())
        return Node::row();
    return c;
}

// The environment a person writes for an array in delimiters, as the model
// writes them: \left\{ \begin{array}{ll} ... \end{array} \right. is cases,
// \left( \begin{array}{cc} ... \end{array} \right) a pmatrix. Empty: none.
QString namedEnvironment(const QString& open, const QString& close, const Node& environment)
{
    if (environment.text != u"array" && environment.text != u"matrix")
        return {};
    if ((open == u"\\{" || open == u"\\lbrace") && close == u".")
        return u"cases"_s;
    // A matrix's columns are centred: an array with others is left as it is.
    if (!std::ranges::all_of(environment.open, [](QChar c) { return c == u'c'; }))
        return {};
    struct Pair {
        QStringView open;
        QStringView close;
        QStringView environment;
    };
    static constexpr std::array kPairs = std::to_array<Pair>({{u"(", u")", u"pmatrix"}, {u"[", u"]", u"bmatrix"},
        {u"\\lbrack", u"\\rbrack", u"bmatrix"}, {u"\\{", u"\\}", u"Bmatrix"}, {u"\\lbrace", u"\\rbrace", u"Bmatrix"},
        {u"|", u"|", u"vmatrix"}, {u"\\vert", u"\\vert", u"vmatrix"}, {u"\\lvert", u"\\rvert", u"vmatrix"},
        {u"\\|", u"\\|", u"Vmatrix"}, {u"\\Vert", u"\\Vert", u"Vmatrix"}, {u"\\lVert", u"\\rVert", u"Vmatrix"}});
    for (const Pair& p : kPairs) {
        if (open == p.open && close == p.close)
            return p.environment.toString();
    }
    return {};
}

Node tidyNode(Node n)
{
    switch (n.kind) {
    case Kind::Row:
        return tidyRow(std::move(n));
    case Kind::Atom:
        n.close.clear(); // \limits with no scripts after it
        return n;
    case Kind::Text:
    case Kind::Sized:
        return n;
    case Kind::Command:
        return tidyCommand(std::move(n));
    case Kind::Scripts: {
        Node base = tidyNode(std::move(n.items[0]));
        // A group of one thing as the base: that thing ({x}^2 is x^2).
        if (base.kind == Kind::Row && base.items.size() == 1 && base.items[0].kind != Kind::Scripts) {
            Node inner = std::move(base.items[0]); // not moved into its own parent straight
            base = std::move(inner);
        }
        Node sub = tidyRow(std::move(n.items[1]));
        Node sup = tidyRow(std::move(n.items[2]));
        sub.braced = sup.braced = false;
        if (sub.isEmpty() && sup.isEmpty()) // x^{}: x
            return base;
        if (base.kind == Kind::Row && base.items.empty())
            base.braced = true; // {}^{14}C
        n.items = {std::move(base), std::move(sub), std::move(sup)};
        if (n.close == u"\\limits" && !isIntegral(n.items[0].text))
            n.close.clear(); // what a sum has anyway
        return n;
    }
    case Kind::Fence: {
        Node inside = tidyRow(std::move(n.items[0]));
        inside.braced = false;
        if (n.open == u"." && n.close == u".")
            return inside;
        if (inside.items.size() == 1 && inside.items[0].kind == Kind::Environment) {
            if (const QString named = namedEnvironment(n.open, n.close, inside.items[0]); !named.isEmpty()) {
                Node environment = std::move(inside.items[0]);
                environment.text = named;
                environment.open.clear();
                return environment;
            }
        }
        n.items = {std::move(inside)};
        return n;
    }
    case Kind::Environment: {
        for (Node& line : n.items) {
            for (Node& cell : line.items) {
                cell = tidyRow(std::move(cell));
                cell.braced = false;
            }
        }
        // A last row with nothing in it (the \\ before \end).
        while (n.items.size() > 1
               && std::ranges::all_of(n.items.back().items, [](const Node& cell) { return cell.isEmpty(); }))
            n.items.pop_back();
        const bool aligning = n.text == u"aligned" || n.text == u"align" || n.text == u"align*" || n.text == u"gathered"
            || n.text == u"gather" || n.text == u"gather*" || n.text == u"split" || n.text == u"eqnarray"
            || n.text == u"eqnarray*" || n.text == u"array" || n.text == u"alignedat";
        if (aligning && n.items.size() == 1) {
            // One row only: the row itself, its cells one after the other.
            std::vector<Node> items;
            for (Node& cell : n.items[0].items) {
                for (Node& part : cell.items)
                    items.push_back(std::move(part));
            }
            return Node::row(tidyItems(std::move(items)));
        }
        return n;
    }
    }
    return n;
}

// ---- LaTeX again -----------------------------------------------------------------------

class Writer {
public:
    QString take() { return std::move(m_out); }

    void node(const Node& n, bool compact)
    {
        switch (n.kind) {
        case Kind::Row:
            if (n.braced) {
                put(u"{"_s);
                items(n.items, compact);
                put(u"}"_s);
            } else {
                items(n.items, compact);
            }
            return;
        case Kind::Atom:
            put(n.text);
            if (!n.close.isEmpty())
                put(n.close);
            return;
        case Kind::Text: {
            QString words;
            words.reserve(n.text.size());
            for (const QChar c : n.text) {
                if (kTextEscapes.contains(c))
                    words += u'\\';
                words += c;
            }
            put(n.open + u'{' + words + u'}');
            return;
        }
        case Kind::Sized:
            put(n.text);
            put(n.open);
            return;
        case Kind::Command:
            command(n, compact);
            return;
        case Kind::Scripts:
            scripts(n);
            return;
        case Kind::Fence:
            put(u"\\left"_s);
            put(n.open);
            items(n.items[0].items, compact);
            put(u"\\right"_s);
            put(n.close);
            return;
        case Kind::Environment:
            environment(n);
            return;
        }
    }

    // In scripts (compact) only the operators and relations written as
    // commands are spaced: x_{n+1}, but x \to 0 and i \in S. Elsewhere
    // words and spaces stand apart (x \, dx, 1 \text{ or } 2), and so does
    // what an operator with scripts applies to (\sum_{i=1}^n a_i).
    void items(const std::vector<Node>& list, bool compact)
    {
        for (std::size_t i = 0; i < list.size(); ++i) {
            const Node& n = list[i];
            const Node* next = i + 1 < list.size() ? &list[i + 1] : nullptr;
            const Class kind = spacingClass(list, i);
            const bool spaced = (kind == Class::Bin || kind == Class::Rel)
                && (!compact || (n.kind == Kind::Atom && n.text.size() > 1 && n.text.front() == u'\\'));
            const bool apart = !compact && (n.kind == Kind::Text || isSpace(n));
            if (spaced || apart)
                space();
            node(n, compact);
            if (!next)
                continue;
            const Class nextKind = next->kind == Kind::Atom ? classOf(next->text) : Class::Ord;
            const bool nextJoins = nextKind == Class::Open || nextKind == Class::Close || nextKind == Class::Punct;
            if (spaced || apart || (!compact && kind == Class::Punct) || (!compact && appliesToNext(n) && !nextJoins))
                m_pendingSpace = true;
        }
    }

    void environment(const Node& n)
    {
        put(u"\\begin{"_s + n.text + u'}');
        if (!n.open.isEmpty())
            put(u'{' + n.open + u'}');
        const bool lines = n.items.size() > 1;
        for (std::size_t r = 0; r < n.items.size(); ++r) {
            m_out += lines ? u'\n' : u' ';
            m_pendingSpace = false;
            const Node& line = n.items[r];
            for (std::size_t c = 0; c < line.items.size(); ++c) {
                if (c > 0) {
                    if (!m_out.endsWith(u' '))
                        m_out += u' ';
                    m_out += u"& "_s;
                    m_pendingSpace = false;
                }
                items(line.items[c].items, false);
                m_pendingSpace = false;
            }
            if (r + 1 < n.items.size())
                m_out += u" \\\\"_s;
        }
        m_out += lines ? u'\n' : u' ';
        m_pendingSpace = false;
        put(u"\\end{"_s + n.text + u'}');
    }

    // The class that decides the spaces around an item: a + or - with
    // nothing before it to join (at the start, after an operator, a
    // relation or an opening) is a sign, not an operator.
    static Class spacingClass(const std::vector<Node>& list, std::size_t i)
    {
        const Node& n = list[i];
        if (n.kind != Kind::Atom)
            return n.kind == Kind::Scripts && n.items[0].kind == Kind::Atom && classOf(n.items[0].text) == Class::Rel
                ? Class::Rel
                : Class::Ord;
        const Class kind = classOf(n.text);
        if (kind != Class::Bin)
            return kind;
        std::size_t p = i;
        while (p > 0 && isSpace(list[p - 1]))
            --p;
        if (p == 0)
            return Class::Ord;
        const Node& before = list[p - 1];
        if (before.kind == Kind::Atom) {
            const Class b = classOf(before.text);
            if (b == Class::Bin || b == Class::Rel || b == Class::Open || b == Class::Punct || b == Class::Op)
                return Class::Ord;
        }
        if (before.kind == Kind::Sized && !before.text.endsWith(u'r'))
            return Class::Ord;
        if (before.kind == Kind::Scripts && before.items[0].kind == Kind::Atom && takesLimits(before.items[0].text))
            return Class::Ord;
        return Class::Bin;
    }

private:
    QString m_out;
    bool m_pendingSpace = false;

    // An operator with its scripts or an upright name in braces, followed
    // by what it applies to: \sum_{i=1}^n a_i, \operatorname{rank} A.
    // (\sin x has its space already, after the command's letters.)
    static bool appliesToNext(const Node& n)
    {
        if (n.kind == Kind::Command)
            return n.text == u"\\operatorname" || n.text == u"\\operatorname*";
        if (n.kind != Kind::Scripts)
            return false;
        const Node& base = n.items[0];
        if (base.kind == Kind::Command)
            return appliesToNext(base);
        return base.kind == Kind::Atom && (takesLimits(base.text) || classOf(base.text) == Class::Function);
    }

    void space()
    {
        if (!m_out.isEmpty() && !m_out.endsWith(u' ') && !m_out.endsWith(u'{') && !m_out.endsWith(u'\n'))
            m_out += u' ';
        m_pendingSpace = false;
    }

    // What ends in a command's letters ("\\alpha") needs a space before a letter or a digit.
    void put(const QString& text)
    {
        if (text.isEmpty())
            return;
        if (m_pendingSpace) {
            m_pendingSpace = false;
            if (!text.startsWith(u'}') && !text.startsWith(u'^') && !text.startsWith(u'_'))
                space();
        }
        if (!m_out.isEmpty() && !m_out.endsWith(u' ')) {
            const QChar first = text.front();
            qsizetype i = m_out.size() - 1;
            while (i >= 0 && isAsciiLetter(m_out[i]))
                --i;
            const bool afterCommand = i >= 0 && i < m_out.size() - 1 && m_out[i] == u'\\'
                && (i == 0 || m_out[i - 1] != u'\\');
            if (afterCommand && (first.isLetterOrNumber() || (first == u'\\' && isFunctionName(m_out.mid(i + 1)))))
                m_out += u' ';
        }
        m_out += text;
    }

    void argument(const Node& row, bool compact)
    {
        put(u"{"_s);
        items(row.items, compact);
        m_pendingSpace = false;
        put(u"}"_s);
    }

    void command(const Node& c, bool compact)
    {
        put(c.text);
        std::size_t i = 0;
        if (c.optional && !c.items.empty()) {
            m_out += u'[';
            items(c.items[0].items, true);
            m_pendingSpace = false;
            m_out += u']';
            i = 1;
        }
        for (; i < c.items.size(); ++i)
            argument(c.items[i], compact);
    }

    // A script as it is (x^2) when it is one letter or digit, else in braces.
    void script(QChar mark, const Node& row)
    {
        m_out += mark;
        const bool bare = row.items.size() == 1 && row.items[0].kind == Kind::Atom && row.items[0].text.size() == 1
            && row.items[0].text.front().isLetterOrNumber() && row.items[0].text.front().unicode() < 0x80;
        if (bare)
            m_out += row.items[0].text;
        else
            argument(row, true);
    }

    void scripts(const Node& s)
    {
        const Node& base = s.items[0];
        if (base.kind == Kind::Scripts || (base.kind == Kind::Row && (base.items.size() != 1 || base.braced))) {
            put(u"{"_s);
            if (base.kind == Kind::Row)
                items(base.items, false);
            else
                node(base, false);
            put(u"}"_s);
        } else {
            node(base, false);
        }
        put(s.close); // \limits, \nolimits
        const Node& sub = s.items[1];
        const Node& sup = s.items[2];
        const bool primes = !sup.isEmpty() && std::ranges::all_of(sup.items, [](const Node& n) {
            return n.kind == Kind::Atom && n.text == u"\\prime";
        });
        if (primes)
            m_out += QString(static_cast<qsizetype>(sup.items.size()), u'\'');
        if (!sub.isEmpty())
            script(u'_', sub);
        if (!sup.isEmpty() && !primes)
            script(u'^', sup);
    }
};

} // namespace

Node parse(QStringView source, Source as)
{
    return Parser(source, as).document();
}

Node tidy(Node formula)
{
    return tidyNode(std::move(formula));
}

QString tidyModel(QStringView raw)
{
    return write(tidy(parse(raw, Source::Model)));
}

QString write(const Node& formula)
{
    Writer w;
    w.node(formula, false);
    return w.take().trimmed();
}

} // namespace ws::capture::latex
