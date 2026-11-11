#include "Calc.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace tools::calc {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kE = 2.71828182845904523536;

// Full-width and typographic forms of the operators, as typed with a
// Chinese input method, to their ASCII ones; everything else stays.
std::string normalize(const std::string& text)
{
    struct Pair {
        const char* from;
        const char* to;
    };
    static const Pair pairs[] = {
        {"\xC3\x97", "*"}, // ×
        {"\xC3\xB7", "/"}, // ÷
        {"\xE2\x88\x92", "-"}, // −
        {"\xEF\xBC\x8B", "+"}, // ＋
        {"\xEF\xBC\x8D", "-"}, // －
        {"\xEF\xBC\x8A", "*"}, // ＊
        {"\xEF\xBC\x8F", "/"}, // ／
        {"\xEF\xBC\x88", "("}, // （
        {"\xEF\xBC\x89", ")"}, // ）
        {"\xEF\xBC\x8C", ","}, // ，
        {"\xEF\xBC\x8E", "."}, // ．
        {"\xEF\xBC\x85", "%"}, // ％
        {"\xEF\xBC\xBE", "^"}, // ＾
        {"\xEF\xBC\x81", "!"}, // ！
        {"\xCF\x80", "pi"}, // π
        {"\xE3\x80\x80", " "}, // ideographic space
    };
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        bool replaced = false;
        for (const Pair& p : pairs) {
            const std::size_t n = std::strlen(p.from);
            if (text.compare(i, n, p.from) == 0) {
                out += p.to;
                i += n;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            out += text[i++];
    }
    return out;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

class Parser {
public:
    explicit Parser(const std::string& text)
        : m_text(text)
    {
    }

    Result run()
    {
        skipSpace();
        if (!more()) {
            m_result.error = Error::Empty;
            return m_result;
        }
        const double value = expression();
        skipSpace();
        if (m_result.ok() && more())
            fail(Error::Syntax);
        if (m_result.ok())
            m_result.value = check(value);
        return m_result;
    }

private:
    const std::string& m_text;
    std::size_t m_at = 0;
    Result m_result;

    bool more() const { return m_at < m_text.size(); }
    char peek() const { return more() ? m_text[m_at] : '\0'; }
    void skipSpace()
    {
        while (more() && (m_text[m_at] == ' ' || m_text[m_at] == '\t'))
            ++m_at;
    }
    void fail(Error error, std::string detail = {})
    {
        if (m_result.ok()) {
            m_result.error = error;
            m_result.detail = std::move(detail);
        }
    }
    // A value that is not finite is out of range; said once.
    double check(double v)
    {
        if (!std::isfinite(v))
            fail(Error::OutOfRange);
        return v;
    }

    // Whether what follows starts an operand: an implied multiplication
    // after a number or a closing parenthesis.
    bool operandAhead()
    {
        skipSpace();
        const char c = peek();
        return c == '(' || isLetter(c);
    }

    double expression()
    {
        double left = term();
        for (;;) {
            skipSpace();
            const char c = peek();
            if (c == '+' || c == '-') {
                ++m_at;
                const double right = term();
                left = c == '+' ? left + right : left - right;
            } else
                return left;
        }
    }

    double term()
    {
        double left = unary();
        for (;;) {
            skipSpace();
            const char c = peek();
            if (c == '*' || c == '/') {
                ++m_at;
                const double right = unary();
                if (c == '/') {
                    if (right == 0)
                        fail(Error::DivisionByZero);
                    left = right == 0 ? 0 : left / right;
                } else
                    left = left * right;
            } else if (c == '%' && modulo()) {
                ++m_at;
                const double right = unary();
                if (right == 0)
                    fail(Error::DivisionByZero);
                left = right == 0 ? 0 : std::fmod(left, right);
            } else if (c == '(' || isLetter(c)) {
                // Implied multiplication: 2(3+4), 2pi, (1+2)(3+4).
                const double right = unary();
                left = left * right;
            } else
                return left;
        }
    }

    // `%` with an operand after it is the remainder; a trailing one, or one
    // before an operator, is a hundredth (handled in postfix()).
    bool modulo() const
    {
        std::size_t i = m_at + 1;
        while (i < m_text.size() && (m_text[i] == ' ' || m_text[i] == '\t'))
            ++i;
        if (i >= m_text.size())
            return false;
        const char c = m_text[i];
        return isDigit(c) || c == '.' || c == '(' || isLetter(c) || c == '-' || c == '+';
    }

    double unary()
    {
        skipSpace();
        if (peek() == '-') {
            ++m_at;
            return -unary();
        }
        if (peek() == '+') {
            ++m_at;
            return unary();
        }
        return power();
    }

    double power()
    {
        const double base = postfix();
        skipSpace();
        if (peek() == '^') {
            ++m_at;
            const double exponent = unary(); // right-associative: 2^3^2 = 2^9
            if (base == 0 && exponent < 0)
                fail(Error::DivisionByZero);
            return check(std::pow(base, exponent));
        }
        if (m_text.compare(m_at, 2, "**") == 0) {
            m_at += 2;
            const double exponent = unary();
            if (base == 0 && exponent < 0)
                fail(Error::DivisionByZero);
            return check(std::pow(base, exponent));
        }
        return base;
    }

    double postfix()
    {
        double value = primary();
        for (;;) {
            skipSpace();
            if (peek() == '!') {
                ++m_at;
                value = factorial(value);
            } else if (peek() == '%' && !modulo()) {
                ++m_at;
                value = value / 100;
            } else
                return value;
        }
    }

    double factorial(double n)
    {
        if (n < 0 || n != std::floor(n) || n > 170) {
            fail(Error::OutOfRange);
            return 0;
        }
        double out = 1;
        for (int i = 2; i <= static_cast<int>(n); ++i)
            out *= i;
        return out;
    }

    double primary()
    {
        skipSpace();
        const char c = peek();
        if (c == '(') {
            ++m_at;
            const double value = expression();
            skipSpace();
            if (peek() != ')') {
                fail(Error::Syntax);
                return value;
            }
            ++m_at;
            return value;
        }
        if (isDigit(c) || (c == '.' && m_at + 1 < m_text.size() && isDigit(m_text[m_at + 1])))
            return number();
        if (isLetter(c))
            return name();
        fail(Error::Syntax);
        m_at = m_text.size(); // nothing more to read
        return 0;
    }

    double number()
    {
        std::string digits;
        bool seenDot = false;
        while (more()) {
            const char c = peek();
            if (isDigit(c)) {
                digits += c;
                ++m_at;
            } else if (c == '.' && !seenDot) {
                seenDot = true;
                digits += c;
                ++m_at;
            } else if (c == ',' && !seenDot && m_at + 3 < m_text.size() && isDigit(m_text[m_at + 1]) && isDigit(m_text[m_at + 2])
                && isDigit(m_text[m_at + 3]) && (m_at + 4 >= m_text.size() || !isDigit(m_text[m_at + 4]))) {
                ++m_at; // a thousands separator: 1,000
            } else if ((c == 'e' || c == 'E') && m_at + 1 < m_text.size()
                && (isDigit(m_text[m_at + 1]) || ((m_text[m_at + 1] == '-' || m_text[m_at + 1] == '+') && m_at + 2 < m_text.size() && isDigit(m_text[m_at + 2])))) {
                digits += 'e';
                ++m_at;
                if (peek() == '-' || peek() == '+')
                    digits += m_text[m_at++];
                while (more() && isDigit(peek()))
                    digits += m_text[m_at++];
                break;
            } else
                break;
        }
        // 0x1F, 0b101, 0o17
        if (digits == "0" && more() && (peek() == 'x' || peek() == 'X' || peek() == 'b' || peek() == 'B' || peek() == 'o' || peek() == 'O')) {
            const char kind = static_cast<char>(peek() | 0x20);
            const int base = kind == 'x' ? 16 : kind == 'b' ? 2 : 8;
            ++m_at;
            const std::size_t start = m_at;
            while (more() && (isDigit(peek()) || isLetter(peek())))
                ++m_at;
            const std::string literal = m_text.substr(start, m_at - start);
            char* end = nullptr;
            const unsigned long long v = std::strtoull(literal.c_str(), &end, base);
            if (literal.empty() || *end != '\0') {
                fail(Error::Syntax);
                return 0;
            }
            return static_cast<double>(v);
        }
        return check(std::strtod(digits.c_str(), nullptr));
    }

    double name()
    {
        const std::size_t start = m_at;
        while (more() && (isLetter(peek()) || isDigit(peek())))
            ++m_at;
        std::string word = m_text.substr(start, m_at - start);
        for (char& ch : word)
            ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
        skipSpace();
        if (peek() == '(')
            return call(word);
        if (word == "pi")
            return kPi;
        if (word == "e")
            return kE;
        if (word == "tau")
            return 2 * kPi;
        if (word == "inf")
            return std::numeric_limits<double>::infinity();
        fail(Error::UnknownName, word);
        return 0;
    }

    double call(const std::string& fn)
    {
        ++m_at; // (
        std::vector<double> args;
        skipSpace();
        if (peek() == ')') {
            ++m_at;
        } else {
            for (;;) {
                args.push_back(expression());
                skipSpace();
                if (peek() == ',') {
                    ++m_at;
                    continue;
                }
                if (peek() == ')') {
                    ++m_at;
                    break;
                }
                fail(Error::Syntax);
                return 0;
            }
        }
        const auto one = [&](double (*f)(double)) {
            if (args.size() != 1) {
                fail(Error::WrongArguments);
                return 0.0;
            }
            return check(f(args[0]));
        };
        if (fn == "sqrt")
            return one([](double x) { return std::sqrt(x); });
        if (fn == "cbrt")
            return one([](double x) { return std::cbrt(x); });
        if (fn == "abs")
            return one([](double x) { return std::fabs(x); });
        if (fn == "sin")
            return one([](double x) { return std::sin(x); });
        if (fn == "cos")
            return one([](double x) { return std::cos(x); });
        if (fn == "tan")
            return one([](double x) { return std::tan(x); });
        if (fn == "asin")
            return one([](double x) { return std::asin(x); });
        if (fn == "acos")
            return one([](double x) { return std::acos(x); });
        if (fn == "atan")
            return one([](double x) { return std::atan(x); });
        if (fn == "sinh")
            return one([](double x) { return std::sinh(x); });
        if (fn == "cosh")
            return one([](double x) { return std::cosh(x); });
        if (fn == "tanh")
            return one([](double x) { return std::tanh(x); });
        if (fn == "ln")
            return one([](double x) { return std::log(x); });
        if (fn == "log" || fn == "log10")
            return one([](double x) { return std::log10(x); });
        if (fn == "log2")
            return one([](double x) { return std::log2(x); });
        if (fn == "exp")
            return one([](double x) { return std::exp(x); });
        if (fn == "floor")
            return one([](double x) { return std::floor(x); });
        if (fn == "ceil")
            return one([](double x) { return std::ceil(x); });
        if (fn == "round")
            return one([](double x) { return std::round(x); });
        if (fn == "trunc")
            return one([](double x) { return std::trunc(x); });
        if (fn == "deg")
            return one([](double x) { return x * 180 / kPi; });
        if (fn == "rad")
            return one([](double x) { return x * kPi / 180; });
        if (fn == "min" || fn == "max") {
            if (args.empty()) {
                fail(Error::WrongArguments);
                return 0;
            }
            double v = args[0];
            for (const double a : args)
                v = fn == "min" ? std::fmin(v, a) : std::fmax(v, a);
            return v;
        }
        if (fn == "pow" || fn == "hypot" || fn == "atan2" || fn == "mod") {
            if (args.size() != 2) {
                fail(Error::WrongArguments);
                return 0;
            }
            if (fn == "pow")
                return check(std::pow(args[0], args[1]));
            if (fn == "hypot")
                return check(std::hypot(args[0], args[1]));
            if (fn == "atan2")
                return check(std::atan2(args[0], args[1]));
            if (args[1] == 0) {
                fail(Error::DivisionByZero);
                return 0;
            }
            return std::fmod(args[0], args[1]);
        }
        fail(Error::UnknownName, fn);
        return 0;
    }
};

std::string group(std::string digits)
{
    std::string out;
    int count = 0;
    for (std::size_t i = digits.size(); i-- > 0;) {
        out.insert(out.begin(), digits[i]);
        if (++count % 3 == 0 && i > 0)
            out.insert(out.begin(), ',');
    }
    return out;
}

} // namespace

Result evaluate(const std::string& text)
{
    const std::string normalized = normalize(text);
    Parser parser(normalized);
    return parser.run();
}

bool looksLikeExpression(const std::string& text)
{
    const std::string s = normalize(text);
    bool digit = false;
    bool op = false;
    int depth = 0;
    for (const char c : s) {
        if (isDigit(c))
            digit = true;
        else if (c == '+' || c == '*' || c == '/' || c == '^')
            op = true;
        else if (c == '(') {
            op = true;
            ++depth;
        } else if (c == ')')
            --depth;
        else if (c == '-' || c == '.' || c == ',' || c == ' ' || c == '%' || c == '!')
            continue;
        else
            return false; // letters (dates, versions, pinyin) and the rest are names, not sums
    }
    return digit && op && depth == 0;
}

bool takeTrailingEquals(std::string& text)
{
    constexpr std::string_view kWide = "\xEF\xBC\x9D"; // ＝
    std::size_t cut = 0;
    if (text.ends_with('='))
        cut = 1;
    else if (text.ends_with(kWide))
        cut = kWide.size();
    else
        return false;
    text.erase(text.size() - cut);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.pop_back();
    return true;
}

bool looksLikeYearRange(std::string_view text)
{
    if (text.size() != 9 || text[4] != '-')
        return false;
    const auto year = [](std::string_view digits) -> std::optional<int> {
        int value = 0;
        const char* const end = digits.data() + digits.size();
        const auto [stopped, error] = std::from_chars(digits.data(), end, value);
        if (error != std::errc {} || stopped != end || value < 1900 || value > 2099)
            return std::nullopt;
        return value;
    };
    const std::optional<int> from = year(text.substr(0, 4));
    const std::optional<int> to = year(text.substr(5));
    return from && to && *from < *to;
}

std::string format(double value, bool plain)
{
    if (!std::isfinite(value))
        return value > 0 ? "∞" : value < 0 ? "-∞" : "NaN";
    if (value == 0)
        return "0";
    const double magnitude = std::fabs(value);
    if (value == std::floor(value) && magnitude < 1e15) {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%.0f", magnitude);
        return (value < 0 ? "-" : "") + (plain ? std::string(buffer) : group(buffer));
    }
    if (magnitude >= 1e15 || magnitude < 1e-9) {
        char buffer[48];
        std::snprintf(buffer, sizeof buffer, "%.6e", value);
        // 1.500000e+20 -> 1.5e+20
        std::string s = buffer;
        const std::size_t e = s.find('e');
        std::string mantissa = s.substr(0, e);
        std::string exponent = s.substr(e);
        while (mantissa.size() > 1 && mantissa.back() == '0')
            mantissa.pop_back();
        if (mantissa.back() == '.')
            mantissa.pop_back();
        return mantissa + exponent;
    }
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%.12g", value);
    std::string s = buffer;
    if (s.find('e') != std::string::npos) { // %g went scientific for a long fraction
        std::snprintf(buffer, sizeof buffer, "%.12f", value);
        s = buffer;
    }
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
    }
    if (plain)
        return s;
    const std::size_t dot = s.find('.');
    const bool negative = s[0] == '-';
    std::string integer = s.substr(negative ? 1 : 0, dot == std::string::npos ? std::string::npos : dot - (negative ? 1 : 0));
    const std::string fraction = dot == std::string::npos ? "" : s.substr(dot);
    return (negative ? "-" : "") + group(integer) + fraction;
}

namespace {
std::string inBase(double value, int base, const char* prefix)
{
    if (!std::isfinite(value) || value != std::floor(value) || std::fabs(value) >= 9.2e18)
        return {};
    const bool negative = value < 0;
    unsigned long long v = static_cast<unsigned long long>(std::fabs(value));
    std::string digits;
    if (v == 0)
        digits = "0";
    while (v > 0) {
        const int d = static_cast<int>(v % static_cast<unsigned>(base));
        digits.insert(digits.begin(), static_cast<char>(d < 10 ? '0' + d : 'A' + d - 10));
        v /= static_cast<unsigned>(base);
    }
    return (negative ? "-" : "") + std::string(prefix) + digits;
}
} // namespace

std::string hex(double value) { return inBase(value, 16, "0x"); }
std::string binary(double value) { return inBase(value, 2, "0b"); }
std::string octal(double value) { return inBase(value, 8, "0o"); }

} // namespace tools::calc
