#pragma once

// The calculator of winshun.tools: arithmetic with parentheses, powers,
// percentages, factorials and the usual functions, read and evaluated
// without Qt (the plugin is a plain Win32 program). Errors are kinds, not
// words: the plugin says them in the interface's language.

#include <cstdint>
#include <string>
#include <string_view>

namespace tools::calc {

enum class Error {
    None,
    Empty, // nothing to compute
    Syntax, // unbalanced parentheses, an operator without operands …
    UnknownName, // an identifier that is no function or constant (`detail` says which)
    DivisionByZero,
    OutOfRange, // infinite, too large, or no real result (sqrt(-1), log(0), 171!)
    WrongArguments, // a function given the wrong number of arguments
};

struct Result {
    Error error = Error::None;
    double value = 0;
    std::string detail; // UnknownName: the name
    bool ok() const { return error == Error::None; }
};

// Evaluates `text` (UTF-8). Understands + - * / % ^ ( ), × ÷ and the
// full-width forms, unary minus, `15%` (a hundredth) and `a % b` (the
// remainder), `n!`, `2(3+4)` and `2pi` (implied multiplication),
// thousands separators in numbers (1,000), and these names: pi, e, sqrt,
// cbrt, abs, sin, cos, tan, asin, acos, atan, sinh, cosh, tanh, ln, log
// (base 10), log2, exp, floor, ceil, round, trunc, min, max, pow, hypot,
// deg, rad.
Result evaluate(const std::string& text);

// True when `text` looks like an expression worth evaluating without the
// keyword: digits with at least one operator among + * / ^ × ÷ or a
// parenthesis, nothing but arithmetic characters (a date like 2024-01-02
// or a phone number does not qualify).
bool looksLikeExpression(const std::string& text);

// "2024-1987=": a sum typed as on a calculator, the "=" (or its full-width
// form) after it. Taken off `text`, with the blanks before it; true if it
// was there: the sum was asked for, whatever it looks like.
bool takeTrailingEquals(std::string& text);

// True for two years with the later one second, "1998-2003": a span of
// years, searched for as it stands. The earlier one second, "2024-1987", is
// a sum (an age), and so is anything spaced out or outside 1900 to 2099.
bool looksLikeYearRange(std::string_view text);

// The value as text: integers in full with thousands separators (plain:
// without them), others with up to 12 significant digits, very large or
// small ones in scientific notation.
std::string format(double value, bool plain = false);
// Of an integer that fits 64 bits: "0x1F", "0b11111", "0o37"; empty otherwise.
std::string hex(double value);
std::string binary(double value);
std::string octal(double value);

} // namespace tools::calc
