#pragma once

#include <QString>
#include <QStringView>

namespace ws::capture::latex {

// What a symbol is to the layout (TeX's classes): what spacing it takes and
// how MathML writes it. Function: an upright name that takes an argument
// after it (\sin x); Limits: one whose scripts go under and over it in a
// formula on its own line (\lim, \max).
enum class Class : quint8 { Ord, Op, Bin, Rel, Open, Close, Punct, Function, Limits };

struct Symbol {
    QStringView name; // the command without its backslash ("alpha"), or the character ("+")
    QStringView unicode; // what it is in MathML and on the screen
    Class kind = Class::Ord;
    QStringView typst; // its name in Typst's math; empty: the Unicode character itself
};

// The command ("\\alpha", "\\{") or the character ("+", "x"); nothing for
// one not in the table (a letter or a digit is not: they are Ord as they are).
const Symbol* symbol(QStringView text);
// The class of an atom's text: the table's, else a character's own (letters
// and digits are Ord, an unknown command too).
Class classOf(QStringView text);

// Upright names that take an argument (\sin, \log) or limits (\lim, \max):
// "sin" for \operatorname{sin} or \mathrm{s i n} to become \sin.
bool isFunctionName(QStringView name);
// \sum, \int, \lim...: an operator whose scripts \underset and \overset may
// have put under and over it (the model writes \underset{i=1}{\overset{n}{\sum}}).
bool takesLimits(QStringView command);
bool isIntegral(QStringView command);

// The accents, as MathML writes them over (or under) the base, and Typst's
// function for them. Nothing for a command that is not one.
struct Accent {
    QStringView name;
    QStringView mark; // the character over the base
    bool under = false;
    bool stretchy = false;
    QStringView typst;
};
const Accent* accent(QStringView command);

// \mathbf and the rest: MathML's mathvariant and Typst's function.
struct Font {
    QStringView name;
    QStringView variant;
    QStringView typst;
};
const Font* font(QStringView command);

// The spaces (\, \quad): their width in em, 0 for one that takes room back (\!).
double spaceWidth(QStringView command); // < 0: not a space

} // namespace ws::capture::latex
