// The formula in the forms it is pasted in besides LaTeX: MathML and Typst.

#include "Latex.h"
#include "LatexSymbols.h"

#include <QStringList>

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace ws::capture::latex {

namespace {

using Kind = Node::Kind;

bool isAsciiLetter(QChar c)
{
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
}

bool isDigits(QStringView text)
{
    return !text.isEmpty() && std::ranges::all_of(text, [](QChar c) { return c.isDigit(); });
}

bool isCommandText(QStringView text)
{
    return text.size() > 1 && text.front() == u'\\';
}

// A number: the digits of atoms one after another, with a point between
// two of them (3.14). `i` is moved past it; empty when list[i] is no digit.
QString number(const std::vector<Node>& list, std::size_t& i)
{
    if (list[i].kind != Kind::Atom || !isDigits(list[i].text))
        return {};
    QString digits = list[i].text;
    std::size_t j = i + 1;
    for (; j < list.size() && list[j].kind == Kind::Atom; ++j) {
        if (isDigits(list[j].text)) {
            digits += list[j].text;
        } else if (list[j].text == u"." && j + 1 < list.size() && list[j + 1].kind == Kind::Atom
                   && isDigits(list[j + 1].text)) {
            digits += u'.';
        } else {
            break;
        }
    }
    i = j - 1;
    return digits;
}

// A delimiter after \left, \right or \big: its character; empty for ".".
QString delimiterCharacter(const QString& d)
{
    if (d == u"." || d.isEmpty())
        return {};
    if (d == u"\\backslash")
        return u"\\"_s;
    if (const Symbol* s = symbol(d))
        return s->unicode.toString();
    return isCommandText(d) ? QString() : d;
}

// An upright name with an argument after it (\sin, \operatorname{rank}),
// which MathML follows with the invisible function application.
bool isFunction(const Node& n)
{
    if (n.kind == Kind::Atom) {
        const Class k = classOf(n.text);
        return k == Class::Function || k == Class::Limits;
    }
    if (n.kind == Kind::Command)
        return n.text == u"\\operatorname" || n.text == u"\\operatorname*";
    if (n.kind == Kind::Scripts)
        return isFunction(n.items[0]);
    return false;
}

// What a name command holds: its letters ("rank"), else empty.
QString nameOf(const Node& argument)
{
    QString name;
    for (const Node& n : argument.items) {
        if (n.kind != Kind::Atom || isCommandText(n.text)
            || !std::ranges::all_of(n.text, [](QChar c) { return c.isLetterOrNumber(); }))
            return {};
        name += n.text;
    }
    return name;
}

struct Fenced {
    QStringView environment;
    QStringView open;
    QStringView close;
    QStringView typst; // mat's delim
};
constexpr std::array kMatrices = std::to_array<Fenced>({{u"matrix", {}, {}, u"#none"},
    {u"smallmatrix", {}, {}, u"#none"}, {u"pmatrix", u"(", u")", u"\"(\""}, {u"bmatrix", u"[", u"]", u"\"[\""},
    {u"Bmatrix", u"{", u"}", u"\"{\""}, {u"vmatrix", u"|", u"|", u"\"|\""}, {u"Vmatrix", u"‖", u"‖", u"\"‖\""},
    {u"array", {}, {}, u"#none"}, {u"subarray", {}, {}, u"#none"}, {u"tabular", {}, {}, u"#none"}});
const Fenced* matrix(QStringView environment)
{
    const auto found = std::ranges::find(kMatrices, environment, &Fenced::environment);
    return found == kMatrices.end() ? nullptr : &*found;
}

bool isCases(QStringView environment)
{
    return environment == u"cases" || environment == u"dcases" || environment == u"rcases";
}

bool isAligned(QStringView environment)
{
    return environment == u"aligned" || environment == u"align" || environment == u"align*"
        || environment == u"split" || environment == u"alignedat" || environment == u"eqnarray"
        || environment == u"eqnarray*" || environment == u"flalign" || environment == u"flalign*";
}

struct Arrow {
    QStringView command;
    QStringView unicode;
};
constexpr std::array kArrows = std::to_array<Arrow>({{u"\\xrightarrow", u"→"}, {u"\\xleftarrow", u"←"},
    {u"\\xRightarrow", u"⇒"}, {u"\\xLeftarrow", u"⇐"}, {u"\\xleftrightarrow", u"↔"}, {u"\\xLeftrightarrow", u"⇔"},
    {u"\\xmapsto", u"↦"}, {u"\\xlongequal", u"="}, {u"\\xhookrightarrow", u"↪"}, {u"\\xhookleftarrow", u"↩"},
    {u"\\xtofrom", u"⇄"}, {u"\\xrightleftharpoons", u"⇌"}});
const Arrow* arrow(QStringView command)
{
    const auto found = std::ranges::find(kArrows, command, &Arrow::command);
    return found == kArrows.end() ? nullptr : &*found;
}

// The scripts of a sum, a limit or a brace go under and over it on a line of its own.
bool placesLimits(const Node& scripts, bool display)
{
    const Node& base = scripts.items[0];
    if (base.kind == Kind::Command)
        return base.text == u"\\overbrace" || base.text == u"\\underbrace"
            || (base.text == u"\\operatorname*" && display && scripts.close != u"\\nolimits");
    if (base.kind != Kind::Atom)
        return false;
    if (scripts.close == u"\\limits")
        return true;
    if (!display || scripts.close == u"\\nolimits")
        return false;
    const Class k = classOf(base.text);
    return k == Class::Limits || (k == Class::Op && !isIntegral(base.text));
}

bool primesOnly(const Node& row)
{
    return !row.isEmpty()
        && std::ranges::all_of(row.items, [](const Node& n) { return n.kind == Kind::Atom && n.text == u"\\prime"; });
}

// ---- MathML ------------------------------------------------------------------------------

QString xml(QStringView text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case u'<': out += u"&lt;"_s; break;
        case u'>': out += u"&gt;"_s; break;
        case u'&': out += u"&amp;"_s; break;
        case u'"': out += u"&quot;"_s; break;
        default: out += c;
        }
    }
    return out;
}

class MathMl {
public:
    explicit MathMl(bool display)
        : m_display(display)
    {
    }

    QString take() { return std::move(m_out); }

    // A row as one element: itself when it is one thing, else in <mrow>.
    void row(const Node& row)
    {
        if (row.kind != Kind::Row) {
            node(row);
            return;
        }
        if (row.items.size() == 1 && !isFunction(row.items[0])) {
            node(row.items[0]);
            return;
        }
        m_out += u"<mrow>"_s;
        items(row.items);
        m_out += u"</mrow>"_s;
    }

    void items(const std::vector<Node>& list)
    {
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (const QString digits = number(list, i); !digits.isEmpty()) {
                token(u"mn"_s, digits);
                continue;
            }
            node(list[i]);
            if (isFunction(list[i]) && i + 1 < list.size()) {
                const Node& next = list[i + 1];
                const Class k = next.kind == Kind::Atom ? classOf(next.text) : Class::Ord;
                if (k != Class::Bin && k != Class::Rel && k != Class::Punct && k != Class::Close)
                    m_out += u"<mo>&#x2061;</mo>"_s;
            }
        }
    }

    void node(const Node& n)
    {
        switch (n.kind) {
        case Kind::Row:
            row(n);
            return;
        case Kind::Atom:
            atom(n.text);
            return;
        case Kind::Text: {
            // Spaces at the ends of words are kept as no-break ones: MathML trims the others.
            QString words = n.text;
            for (qsizetype i = 0; i < words.size() && words[i] == u' '; ++i)
                words[i] = QChar(0x00A0);
            for (qsizetype i = words.size() - 1; i >= 0 && words[i] == u' '; --i)
                words[i] = QChar(0x00A0);
            QString variant;
            if (n.open == u"\\textbf")
                variant = u"bold"_s;
            else if (n.open == u"\\textit" || n.open == u"\\textsl")
                variant = u"italic"_s;
            else if (n.open == u"\\textsf")
                variant = u"sans-serif"_s;
            else if (n.open == u"\\texttt")
                variant = u"monospace"_s;
            m_out += variant.isEmpty() ? u"<mtext>"_s : u"<mtext mathvariant=\""_s + variant + u"\">"_s;
            m_out += xml(words) + u"</mtext>"_s;
            return;
        }
        case Kind::Sized: {
            const QString d = delimiterCharacter(n.open);
            if (d.isEmpty())
                return;
            if (n.text == u"\\middle") {
                m_out += u"<mo stretchy=\"true\">"_s + xml(d) + u"</mo>"_s;
                return;
            }
            QString size = n.text.mid(1);
            if (size.endsWith(u'l') || size.endsWith(u'r') || size.endsWith(u'm'))
                size.chop(1);
            const QString em = size == u"big" ? u"1.2em"_s
                : size == u"Big"              ? u"1.623em"_s
                : size == u"bigg"             ? u"2.047em"_s
                                              : u"2.470em"_s;
            m_out += u"<mo minsize=\""_s + em + u"\" maxsize=\""_s + em + u"\">"_s + xml(d) + u"</mo>"_s;
            return;
        }
        case Kind::Command:
            command(n);
            return;
        case Kind::Scripts:
            scripts(n);
            return;
        case Kind::Fence: {
            m_out += u"<mrow>"_s;
            if (const QString open = delimiterCharacter(n.open); !open.isEmpty())
                m_out += u"<mo fence=\"true\" form=\"prefix\">"_s + xml(open) + u"</mo>"_s;
            items(n.items[0].items);
            if (const QString close = delimiterCharacter(n.close); !close.isEmpty())
                m_out += u"<mo fence=\"true\" form=\"postfix\">"_s + xml(close) + u"</mo>"_s;
            m_out += u"</mrow>"_s;
            return;
        }
        case Kind::Environment:
            environment(n);
            return;
        }
    }

private:
    QString m_out;
    bool m_display = true;
    QString m_variant; // \mathbf and the rest, for the letters and digits in them

    void token(const QString& tag, const QString& text, const QString& attributes = {})
    {
        m_out += u'<' + tag;
        if (!m_variant.isEmpty() && tag != u"mo")
            m_out += u" mathvariant=\""_s + m_variant + u'"';
        else if (tag == u"mi" && text.size() == 1 && text.front().unicode() >= 0x391 && text.front().unicode() <= 0x3A9)
            m_out += u" mathvariant=\"normal\""_s; // capital Greek is upright, as TeX has it
        m_out += attributes + u'>' + xml(text) + u"</"_s + tag + u'>';
    }

    void atom(const QString& text)
    {
        if (text == u"\\\\") {
            m_out += u"<mspace linebreak=\"newline\"/>"_s;
            return;
        }
        if (const double em = spaceWidth(text); em >= 0) {
            if (em > 0)
                m_out += u"<mspace width=\""_s + QString::number(em, 'g', 4) + u"em\"/>"_s;
            return;
        }
        const Class kind = classOf(text);
        if (kind == Class::Function || kind == Class::Limits) {
            token(u"mi"_s, text.mid(1));
            return;
        }
        const Symbol* s = symbol(text);
        const QString character = s ? s->unicode.toString() : text;
        if (!s && isCommandText(text)) { // a command this does not know: as it is written
            m_out += u"<mtext>"_s + xml(text) + u"</mtext>"_s;
            return;
        }
        switch (kind) {
        case Class::Op:
        case Class::Bin:
        case Class::Rel:
        case Class::Punct:
            token(u"mo"_s, character);
            return;
        case Class::Open:
        case Class::Close:
            token(u"mo"_s, character, u" stretchy=\"false\""_s);
            return;
        default:
            break;
        }
        if (text == u"\\prime" || text == u"'") {
            token(u"mo"_s, u"′"_s);
            return;
        }
        if (isDigits(character)) {
            token(u"mn"_s, character);
            return;
        }
        if (character.front().isLetter() || character.front().isHighSurrogate()) {
            token(u"mi"_s, character);
            return;
        }
        // |, /, ., ∞, ∂...: ∞ and ∂ are identifiers, the others operators.
        const bool identifier = character == u"∞" || character == u"∂" || character == u"∇" || character == u"∅"
            || character == u"ℏ" || character == u"ℓ" || character == u"℘" || character == u"ℵ";
        token(identifier ? u"mi"_s : u"mo"_s, character,
            identifier ? QString() : u" stretchy=\"false\""_s);
    }

    void scripts(const Node& s)
    {
        const Node& base = s.items[0];
        const Node& sub = s.items[1];
        const Node& sup = s.items[2];
        const bool limits = placesLimits(s, m_display);
        const QString tag = limits ? (!sub.isEmpty() && !sup.isEmpty() ? u"munderover"_s
                                         : !sub.isEmpty()                ? u"munder"_s
                                                                         : u"mover"_s)
                                   : (!sub.isEmpty() && !sup.isEmpty() ? u"msubsup"_s
                                         : !sub.isEmpty()                ? u"msub"_s
                                                                         : u"msup"_s);
        m_out += u'<' + tag + u'>';
        if (base.kind == Kind::Row && base.items.empty())
            m_out += u"<mrow></mrow>"_s;
        else
            row(base);
        if (!sub.isEmpty())
            row(sub);
        if (primesOnly(sup))
            token(u"mo"_s, QString(static_cast<qsizetype>(sup.items.size()), u'′'));
        else if (!sup.isEmpty())
            row(sup);
        m_out += u"</"_s + tag + u'>';
    }

    void command(const Node& c)
    {
        const QString& name = c.text;
        const auto argument = [&c](std::size_t i) -> const Node& {
            static const Node empty;
            return i < c.items.size() ? c.items[i] : empty;
        };
        if (name == u"\\frac" || name == u"\\dfrac" || name == u"\\tfrac" || name == u"\\cfrac") {
            m_out += u"<mfrac>"_s;
            row(argument(0));
            row(argument(1));
            m_out += u"</mfrac>"_s;
            return;
        }
        if (name == u"\\binom" || name == u"\\dbinom" || name == u"\\tbinom") {
            m_out += u"<mrow><mo>(</mo><mfrac linethickness=\"0\">"_s;
            row(argument(0));
            row(argument(1));
            m_out += u"</mfrac><mo>)</mo></mrow>"_s;
            return;
        }
        if (name == u"\\sqrt") {
            if (c.optional) {
                m_out += u"<mroot>"_s;
                row(argument(1));
                row(argument(0));
                m_out += u"</mroot>"_s;
            } else {
                m_out += u"<msqrt>"_s;
                items(argument(0).items);
                m_out += u"</msqrt>"_s;
            }
            return;
        }
        if (const Accent* a = accent(name)) {
            const bool brace = name.endsWith(u"brace") || name.endsWith(u"bracket");
            const QString tag = a->under ? u"munder"_s : u"mover"_s;
            m_out += u'<' + tag;
            if (!brace)
                m_out += a->under ? u" accentunder=\"true\""_s : u" accent=\"true\""_s;
            m_out += u'>';
            row(argument(0));
            m_out += u"<mo stretchy=\""_s + (a->stretchy ? u"true"_s : u"false"_s) + u"\">"_s + xml(a->mark)
                + u"</mo></"_s + tag + u'>';
            return;
        }
        if (const Font* f = font(name)) {
            const QString before = std::exchange(m_variant, f->variant.toString());
            row(argument(0));
            m_variant = before;
            return;
        }
        if (name == u"\\operatorname" || name == u"\\operatorname*") {
            if (const QString n = nameOf(argument(0)); !n.isEmpty()) {
                token(u"mi"_s, n);
            } else {
                const QString before = std::exchange(m_variant, u"normal"_s);
                row(argument(0));
                m_variant = before;
            }
            return;
        }
        if (name == u"\\boxed" || name == u"\\fbox" || name == u"\\cancel" || name == u"\\bcancel"
            || name == u"\\xcancel" || name == u"\\sout") {
            const QString notation = name == u"\\cancel" ? u"updiagonalstrike"_s
                : name == u"\\bcancel"                   ? u"downdiagonalstrike"_s
                : name == u"\\xcancel"                   ? u"updiagonalstrike downdiagonalstrike"_s
                : name == u"\\sout"                      ? u"horizontalstrike"_s
                                                         : u"box"_s;
            m_out += u"<menclose notation=\""_s + notation + u"\">"_s;
            row(argument(0));
            m_out += u"</menclose>"_s;
            return;
        }
        if (name == u"\\phantom" || name == u"\\hphantom" || name == u"\\vphantom") {
            m_out += u"<mphantom>"_s;
            row(argument(0));
            m_out += u"</mphantom>"_s;
            return;
        }
        if (name == u"\\pmod" || name == u"\\pod") {
            m_out += u"<mrow><mspace width=\"1em\"/><mo stretchy=\"false\">(</mo>"_s;
            if (name == u"\\pmod")
                m_out += u"<mi>mod</mi><mspace width=\"0.333em\"/>"_s;
            row(argument(0));
            m_out += u"<mo stretchy=\"false\">)</mo></mrow>"_s;
            return;
        }
        if (name == u"\\overset" || name == u"\\underset" || name == u"\\stackrel") {
            const QString tag = name == u"\\underset" ? u"munder"_s : u"mover"_s;
            m_out += u'<' + tag + u'>';
            row(argument(1));
            row(argument(0));
            m_out += u"</"_s + tag + u'>';
            return;
        }
        if (const Arrow* a = arrow(name)) {
            const Node& above = c.items.empty() ? argument(0) : c.items.back();
            const bool below = c.optional && c.items.size() > 1 && !c.items[0].isEmpty();
            const QString tag = below ? u"munderover"_s : u"mover"_s;
            m_out += u'<' + tag + u"><mo stretchy=\"true\">"_s + xml(a->unicode) + u"</mo>"_s;
            if (below)
                row(c.items[0]);
            row(above);
            m_out += u"</"_s + tag + u'>';
            return;
        }
        if (name == u"\\substack") {
            m_out += u"<mtable>"_s;
            std::vector<Node> line;
            const auto flush = [&] {
                m_out += u"<mtr><mtd>"_s;
                row(Node::row(std::exchange(line, {})));
                m_out += u"</mtd></mtr>"_s;
            };
            for (const Node& n : argument(0).items) {
                if (n.kind == Kind::Atom && n.text == u"\\\\")
                    flush();
                else
                    line.push_back(n);
            }
            flush();
            m_out += u"</mtable>"_s;
            return;
        }
        // \smash and what else holds a formula: what it holds.
        m_out += u"<mrow>"_s;
        for (const Node& part : c.items)
            items(part.items);
        m_out += u"</mrow>"_s;
    }

    void environment(const Node& e)
    {
        const Fenced* m = matrix(e.text);
        QString open = m ? m->open.toString() : QString();
        QString close = m ? m->close.toString() : QString();
        QString align;
        QString spacing;
        if (isCases(e.text)) {
            (e.text == u"rcases" ? close : open) = e.text == u"rcases" ? u"}"_s : u"{"_s;
            align = u"left left"_s;
        } else if (isAligned(e.text)) {
            align = u"right left right left right left"_s;
            spacing = u"0em 2em 0em 2em 0em"_s;
        } else if (e.text == u"array" || e.text == u"subarray" || e.text == u"tabular") {
            for (const QChar c : e.open) {
                if (c == u'l')
                    align += u"left "_s;
                else if (c == u'c')
                    align += u"center "_s;
                else if (c == u'r')
                    align += u"right "_s;
            }
            align = align.trimmed();
        }
        m_out += u"<mrow>"_s;
        if (!open.isEmpty())
            m_out += u"<mo fence=\"true\" form=\"prefix\">"_s + xml(open) + u"</mo>"_s;
        m_out += u"<mtable"_s;
        if (!align.isEmpty())
            m_out += u" columnalign=\""_s + align + u'"';
        if (!spacing.isEmpty())
            m_out += u" columnspacing=\""_s + spacing + u'"';
        if (isAligned(e.text) || e.text == u"gathered" || e.text == u"gather" || e.text == u"gather*")
            m_out += u" displaystyle=\"true\""_s;
        m_out += u'>';
        for (const Node& line : e.items) {
            m_out += u"<mtr>"_s;
            for (const Node& cell : line.items) {
                m_out += u"<mtd>"_s;
                // A cell that starts with = (a &= b): an empty one before it, for the space around it.
                const bool relation = !cell.items.empty() && cell.items[0].kind == Kind::Atom
                    && (classOf(cell.items[0].text) == Class::Rel || classOf(cell.items[0].text) == Class::Bin);
                if (relation) {
                    m_out += u"<mrow><mi></mi>"_s;
                    items(cell.items);
                    m_out += u"</mrow>"_s;
                } else {
                    row(cell);
                }
                m_out += u"</mtd>"_s;
            }
            m_out += u"</mtr>"_s;
        }
        m_out += u"</mtable>"_s;
        if (!close.isEmpty())
            m_out += u"<mo fence=\"true\" form=\"postfix\">"_s + xml(close) + u"</mo>"_s;
        m_out += u"</mrow>"_s;
    }
};

// ---- Typst -------------------------------------------------------------------------------

// The characters Typst's math gives a meaning and that are written with a backslash.
constexpr QStringView kTypstEscapes = u"#$\\\"_^&/@*`[]{}~";

bool typstSimple(const QString& s)
{
    return !s.isEmpty() && !s.contains(u' ') && !s.startsWith(u'(') && !s.contains(u'/');
}

class Typst {
public:
    QString row(const Node& row) { return row.kind == Kind::Row ? items(row.items) : node(row); }

    QString items(const std::vector<Node>& list)
    {
        QString out;
        QString previous;
        bool sign = false; // the part before was a + or - with nothing to join: -x
        Class last = Class::Open; // the class of the part before
        for (std::size_t i = 0; i < list.size(); ++i) {
            const Node& n = list[i];
            QString part = number(list, i);
            if (part.isEmpty())
                part = node(n);
            if (part.isEmpty())
                continue;
            if (!out.isEmpty() && !sign && needsSpace(previous, part))
                out += u' ';
            out += part;
            const Class kind = n.kind == Kind::Atom ? classOf(n.text) : Class::Ord;
            sign = kind == Class::Bin && (n.text == u"-" || n.text == u"+")
                && (last == Class::Open || last == Class::Bin || last == Class::Rel || last == Class::Punct
                    || last == Class::Op);
            last = kind;
            previous = part;
        }
        return out;
    }

    QString node(const Node& n)
    {
        switch (n.kind) {
        case Kind::Row:
            return row(n);
        case Kind::Atom:
            return atom(n.text);
        case Kind::Text:
            return text(n.text);
        case Kind::Sized:
            return delimiter(n.open);
        case Kind::Command:
            return command(n);
        case Kind::Scripts:
            return scripts(n);
        case Kind::Fence:
            return fence(n);
        case Kind::Environment:
            return environment(n);
        }
        return {};
    }

private:
    int m_arguments = 0; // in a function's arguments, where , and ; part them

    static QString text(const QString& words)
    {
        QString out = u"\""_s;
        for (const QChar c : words) {
            if (c == u'"' || c == u'\\')
                out += u'\\';
            out += c;
        }
        return out + u'"';
    }

    // Two parts apart, as Typst needs to tell letters and names from one
    // another (a b, alpha x); none inside brackets or before punctuation.
    static bool needsSpace(const QString& before, const QString& after)
    {
        const QChar last = before.back();
        const QChar first = after.front();
        if (last == u'(' || last == u'[')
            return false;
        if (first == u')' || first == u']' || first == u',' || first == u';' || after == u"!" || first == u'\'')
            return false;
        if (first == u'\\' && after.size() > 1 && (after[1] == u',' || after[1] == u';'))
            return false;
        // f(x), f'(x), 2(x + 1); apart after anything else, which it might
        // otherwise call (alpha (x)) or be taken into (lim_(x -> 0) (...)).
        if (first == u'(' || first == u'[')
            return !((before.size() == 1 && before.front().isLetterOrNumber()) || last == u'\'');
        return true;
    }

    // In a function's arguments a comma or a semicolon is a character
    // only when written with a backslash.
    QString punctuation(const QString& c) const { return m_arguments > 0 ? u'\\' + c : c; }

    // The formula as a function's argument.
    QString argument(const Node& row)
    {
        ++m_arguments;
        QString out = this->row(row);
        --m_arguments;
        return out;
    }

    // A fraction's part or a script: in parentheses (which Typst does not
    // show there) unless it is one thing.
    QString operand(const Node& row)
    {
        const QString s = this->row(row);
        // One thing: one item, or the digits of one number (-b is two).
        const bool one = row.kind != Kind::Row || row.items.size() == 1
            || std::ranges::all_of(row.items, [](const Node& n) {
                   return n.kind == Kind::Atom && (isDigits(n.text) || n.text == u".");
               });
        return one && typstSimple(s) ? s : u'(' + s + u')';
    }

    static QString escaped(const QString& character)
    {
        if (character.size() == 1 && kTypstEscapes.contains(character.front()))
            return u'\\' + character;
        return character;
    }

    QString delimiter(const QString& d) { return escaped(delimiterCharacter(d)); }

    QString atom(const QString& t)
    {
        if (t == u"\\\\")
            return u"\\\n"_s;
        if (const double em = spaceWidth(t); em >= 0) {
            if (em == 0)
                return {};
            if (em < 0.2)
                return u"thin"_s;
            if (em < 0.25)
                return u"med"_s;
            if (em < 0.3)
                return u"thick"_s;
            if (em < 0.5)
                return u"space"_s;
            if (em < 1.5)
                return u"quad"_s;
            return u"wide"_s;
        }
        const Class kind = classOf(t);
        if (kind == Class::Function || kind == Class::Limits)
            return t.mid(1); // Typst has them all by the same names
        if (t == u"\\prime" || t == u"'")
            return u"'"_s;
        if (t == u"," || t == u";")
            return punctuation(t);
        if (const Symbol* s = symbol(t)) {
            if (!s->typst.isEmpty())
                return s->typst.toString();
            if (t == u"-")
                return u"-"_s;
            return escaped(s->unicode.toString());
        }
        if (isCommandText(t)) // a command this does not know: as it is written
            return text(t);
        if (t.size() > 1 && !isDigits(t))
            return text(t); // a name of more letters (\mathrm{kg})
        return escaped(t);
    }

    QString scripts(const Node& s)
    {
        const Node& base = s.items[0];
        const Node& sub = s.items[1];
        const Node& sup = s.items[2];
        // \overbrace{x}^{n}: Typst's overbrace takes its words as an argument.
        if (base.kind == Kind::Command && (base.text == u"\\overbrace" || base.text == u"\\underbrace")) {
            const Node& note = base.text == u"\\overbrace" ? sup : sub;
            if (!note.isEmpty() && !base.items.empty()) {
                const QString body = base.text.mid(1) + u'(' + argument(base.items[0]) + u", "_s + argument(note) + u')';
                const Node& other = base.text == u"\\overbrace" ? sub : sup;
                if (other.isEmpty())
                    return body;
                return body + (base.text == u"\\overbrace" ? u"_"_s : u"^"_s) + operand(other);
            }
        }
        QString out;
        if (base.kind == Kind::Row && base.items.empty()) {
            out = u"\"\""_s; // {}^{14}C: on nothing
        } else if ((base.kind == Kind::Row && (base.items.size() > 1 || base.items[0].kind == Kind::Scripts))
                   || base.kind == Kind::Scripts) {
            // {a + b}^2, {x^2}^3: on the whole, which Typst writes only so.
            out = u"attach("_s + argument(base);
            if (!sub.isEmpty())
                out += u", b: "_s + argument(sub);
            if (!sup.isEmpty())
                out += u", t: "_s + argument(sup);
            return out + u')';
        } else {
            out = node(base);
        }
        if (s.close == u"\\limits")
            out = u"limits("_s + out + u')';
        else if (s.close == u"\\nolimits")
            out = u"scripts("_s + out + u')';
        if (primesOnly(sup))
            out += QString(static_cast<qsizetype>(sup.items.size()), u'\'');
        if (!sub.isEmpty())
            out += u'_' + operand(sub);
        if (!sup.isEmpty() && !primesOnly(sup))
            out += u'^' + operand(sup);
        return out;
    }

    QString call(const QString& function, const Node& row) { return function + u'(' + argument(row) + u')'; }

    QString command(const Node& c)
    {
        const QString& name = c.text;
        const auto argumentAt = [&c](std::size_t i) -> const Node& {
            static const Node empty;
            return i < c.items.size() ? c.items[i] : empty;
        };
        if (name == u"\\frac" || name == u"\\dfrac" || name == u"\\tfrac" || name == u"\\cfrac")
            return operand(argumentAt(0)) + u'/' + operand(argumentAt(1));
        if (name == u"\\binom" || name == u"\\dbinom" || name == u"\\tbinom")
            return u"binom("_s + argument(argumentAt(0)) + u", "_s + argument(argumentAt(1)) + u')';
        if (name == u"\\sqrt") {
            if (c.optional)
                return u"root("_s + argument(argumentAt(0)) + u", "_s + argument(argumentAt(1)) + u')';
            return call(u"sqrt"_s, argumentAt(0));
        }
        if (const Accent* a = accent(name)) {
            if (a->typst.isEmpty())
                return row(argumentAt(0));
            if (a->typst.contains(u'.')) // dot.double: a symbol, given to accent()
                return u"accent("_s + argument(argumentAt(0)) + u", "_s + a->typst.toString() + u')';
            return call(a->typst.toString(), argumentAt(0));
        }
        if (const Font* f = font(name)) {
            // \mathrm{d}: upright(d); \mathrm{kg} is already words.
            const QString inside = row(argumentAt(0));
            if (inside.startsWith(u'"') && inside.endsWith(u'"') && f->typst == u"upright")
                return inside;
            return call(f->typst.toString(), argumentAt(0));
        }
        if (name == u"\\operatorname" || name == u"\\operatorname*") {
            const QString n = nameOf(argumentAt(0));
            const QString label = n.isEmpty() ? row(argumentAt(0)) : text(n);
            return u"op("_s + label + (name.endsWith(u'*') ? u", limits: #true)"_s : u")"_s);
        }
        if (name == u"\\boxed" || name == u"\\fbox")
            return u"#box(stroke: 0.5pt, inset: 3pt, $"_s + row(argumentAt(0)) + u"$)"_s;
        if (name == u"\\cancel")
            return call(u"cancel"_s, argumentAt(0));
        if (name == u"\\bcancel")
            return u"cancel("_s + argument(argumentAt(0)) + u", inverted: #true)"_s;
        if (name == u"\\xcancel")
            return u"cancel("_s + argument(argumentAt(0)) + u", cross: #true)"_s;
        if (name == u"\\phantom" || name == u"\\hphantom" || name == u"\\vphantom")
            return u"#hide($"_s + row(argumentAt(0)) + u"$)"_s;
        if (name == u"\\pmod")
            return u"quad (mod "_s + row(argumentAt(0)) + u')';
        if (name == u"\\pod")
            return u"quad ("_s + row(argumentAt(0)) + u')';
        if (name == u"\\overset" || name == u"\\stackrel")
            return u"limits("_s + argument(argumentAt(1)) + u")^"_s + operand(argumentAt(0));
        if (name == u"\\underset")
            return u"limits("_s + argument(argumentAt(1)) + u")_"_s + operand(argumentAt(0));
        if (const Arrow* a = arrow(name)) {
            QString out = u"stretch("_s + escaped(a->unicode.toString()) + u")"_s;
            if (c.optional && c.items.size() > 1 && !c.items[0].isEmpty())
                out += u'_' + operand(c.items[0]);
            if (!c.items.empty())
                out += u'^' + operand(c.items.back());
            return out;
        }
        if (name == u"\\substack") {
            QString out;
            for (const Node& n : argumentAt(0).items)
                out += n.kind == Kind::Atom && n.text == u"\\\\" ? u" \\ "_s : u' ' + node(n);
            return out.simplified();
        }
        QString out;
        for (const Node& part : c.items)
            out += row(part);
        return out;
    }

    QString fence(const Node& f)
    {
        const QString open = delimiterCharacter(f.open);
        const QString close = delimiterCharacter(f.close);
        const QString inside = items(f.items[0].items);
        if (open == u"|" && close == u"|")
            return u"abs("_s + argument(f.items[0]) + u')';
        if (open == u"‖" && close == u"‖")
            return u"norm("_s + argument(f.items[0]) + u')';
        if (open == u"⌊" && close == u"⌋")
            return u"floor("_s + argument(f.items[0]) + u')';
        if (open == u"⌈" && close == u"⌉")
            return u"ceil("_s + argument(f.items[0]) + u')';
        // Pairs Typst matches by itself, and makes as tall as what is in them.
        if ((open == u"(" && close == u")") || (open == u"[" && close == u"]"))
            return open + inside + close;
        return u"lr("_s + escaped(open) + (open.isEmpty() ? QString() : u" "_s) + argument(f.items[0])
            + (close.isEmpty() ? QString() : u" "_s) + escaped(close) + u')';
    }

    QString cells(const Node& line, const QString& between)
    {
        QStringList parts;
        for (const Node& cell : line.items)
            parts.push_back(argument(cell));
        return parts.join(between);
    }

    QString environment(const Node& e)
    {
        if (isCases(e.text)) {
            QStringList lines;
            for (const Node& line : e.items)
                lines.push_back(cells(line, u" & "_s));
            return u"cases("_s + (e.text == u"rcases" ? u"reverse: #true, "_s : QString()) + lines.join(u", "_s)
                + u')';
        }
        if (isAligned(e.text) || e.text.startsWith(u"gather") || e.text == u"multline" || e.text == u"multline*") {
            QStringList lines;
            for (const Node& line : e.items) {
                QStringList parts;
                for (const Node& cell : line.items)
                    parts.push_back(row(cell));
                lines.push_back(parts.join(u" & "_s));
            }
            return lines.join(u" \\\n"_s);
        }
        const Fenced* m = matrix(e.text);
        QStringList lines;
        for (const Node& line : e.items)
            lines.push_back(cells(line, u", "_s));
        const QString delim = m ? m->typst.toString() : u"#none"_s;
        return u"mat("_s + (delim == u"\"(\"" ? QString() : u"delim: "_s + delim + u", "_s) + lines.join(u"; "_s)
            + u')';
    }
};

} // namespace

QString toMathMl(const Node& formula, bool display)
{
    MathMl m(display);
    if (formula.kind == Kind::Row)
        m.items(formula.items);
    else
        m.node(formula);
    return u"<math xmlns=\"http://www.w3.org/1998/Math/MathML\" display=\""_s
        + (display ? u"block"_s : u"inline"_s) + u"\">"_s + m.take() + u"</math>"_s;
}

QString toTypst(const Node& formula)
{
    return Typst().row(formula);
}

} // namespace ws::capture::latex
