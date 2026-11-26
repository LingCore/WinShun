#pragma once

#include <QString>
#include <QStringView>

#include <vector>

namespace ws::capture::latex {

// A formula in LaTeX's math mode, as 公式识别 (FormulaReader) reads it from
// a picture and the user edits it, taken apart so that it can be written
// again tidily and in the other forms a formula is pasted in: MathML (Word's
// equations, web pages), Typst. Pure logic with no window in it (tst_formula).
//
// What the parser knows is what such formulas use: fractions, roots,
// scripts, \left \right, accents, fonts, \text, the matrix, cases and
// aligned environments, and the old plain-TeX forms (\over, \root \of, \bf).
// A command it does not know is kept as it is, its braces after it groups.
// Nothing is ever refused: a stray brace or \right is taken as a character.
struct Node {
    enum class Kind : quint8 {
        Row, // `items`, one after another; `braced`: written in braces ({...})
        Atom, // `text`: one character ("x", "+", "("), or a command that takes nothing ("\\alpha", "\\,", "\\infty")
        Command, // `text` ("\\frac", "\\sqrt", "\\mathbf", "\\text"...) and its arguments in `items`, each a Row;
                 // `optional`: the first of them was in brackets (\sqrt[3]{x})
        Scripts, // `items`: the base (any node), the subscript and the superscript, both Rows (empty: none);
                 // `close`: "\\limits" or "\\nolimits" when the base was given one
        Fence, // \left `open` ... \right `close`, with what is between in items[0]
        Sized, // a delimiter made larger (\bigl( \Big|): `text` the size command, `open` the delimiter
        Environment, // \begin{`text`}{`open`, its columns}: `items` its rows, each a Row of cells, each a Row
        Text, // the words of `text`, in the command `open` (\text, \textbf, \mbox...)
    };
    Kind kind = Kind::Row;
    bool braced = false;
    bool optional = false;
    QString text;
    QString open;
    QString close;
    std::vector<Node> items;

    static Node row(std::vector<Node> items = {}, bool braced = false);
    static Node atom(const QString& text);
    bool isEmpty() const { return kind == Kind::Row && items.empty(); }
    bool operator==(const Node&) const = default;
};

// How the source is to be read: as anyone writes LaTeX, or as the formula
// model spells it, every token apart and a space in words written as ~
// ("\\mathrm { ~ o r ~ }"), where white space means nothing.
enum class Source : quint8 { Written, Model };

Node parse(QStringView source, Source as = Source::Written);

// The formula as the model said it made into what a person would write:
// redundant braces and negative spaces gone, \operatorname*{l i m} as \lim,
// \underset{i=1}{\overset{n}{\sum}} as \sum_{i=1}^{n}, \root 3 \of x as
// \sqrt[3]{x}, {\bf 1} as \mathbf{1}, \mathrm{~or~} as \text{ or }, an
// unmatched \left or \right as the delimiter alone, an aligned environment
// of one row as that row.
Node tidy(Node formula);
// parse(Model) then tidy, written: what the source field shows first.
QString tidyModel(QStringView raw);

// LaTeX again: spaces around relations and operators that join two things
// (x = -1), none in scripts (x_{n+1}), braces only where they are needed.
// An environment of more than one row has a line for each.
QString write(const Node& formula);

// Presentation MathML, the <math> element with its namespace: what Word
// takes as an equation when it is pasted, and what browsers show. `display`:
// a formula on a line of its own (sums with their limits under and over).
QString toMathMl(const Node& formula, bool display = true);
// Typst's math (without the $ around it): frac as a / b, letters apart
// (4 a c), symbols by their Typst names, matrices as mat(...).
QString toTypst(const Node& formula);

} // namespace ws::capture::latex
