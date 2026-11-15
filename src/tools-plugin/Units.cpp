#include "Units.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tools::units {

namespace {

// clang-format off
const Unit kUnits[] = {
    // length (base: metre)
    {"mm", Category::Length, 0.001, "mm|millimeter|millimetre|millimeters|millimetres|毫米"},
    {"cm", Category::Length, 0.01, "cm|centimeter|centimetre|centimeters|centimetres|厘米|公分"},
    {"m", Category::Length, 1, "m|meter|metre|meters|metres|米|公尺"},
    {"km", Category::Length, 1000, "km|kilometer|kilometre|kilometers|kilometres|千米|公里"},
    {"inch", Category::Length, 0.0254, "in|inch|inches|\"|″|英寸|吋"},
    {"ft", Category::Length, 0.3048, "ft|foot|feet|'|′|英尺|呎"},
    {"yd", Category::Length, 0.9144, "yd|yard|yards|码"},
    {"mile", Category::Length, 1609.344, "mi|mile|miles|英里"},
    {"nmi", Category::Length, 1852, "nmi|nautical mile|海里"},
    {"里", Category::Length, 500, "里|华里|市里"},
    {"丈", Category::Length, 10.0 / 3.0, "丈"},
    {"尺", Category::Length, 1.0 / 3.0, "尺|市尺"},
    {"寸", Category::Length, 1.0 / 30.0, "寸|市寸"},
    // mass (base: gram)
    {"mg", Category::Mass, 0.001, "mg|milligram|milligrams|毫克"},
    {"g", Category::Mass, 1, "g|gram|grams|克|公克"},
    {"kg", Category::Mass, 1000, "kg|kilogram|kilograms|千克|公斤"},
    {"t", Category::Mass, 1000000, "t|ton|tons|tonne|tonnes|吨|公吨"},
    {"lb", Category::Mass, 453.59237, "lb|lbs|pound|pounds|磅"},
    {"oz", Category::Mass, 28.349523125, "oz|ounce|ounces|盎司"},
    {"st", Category::Mass, 6350.29318, "st|stone|英石"},
    {"斤", Category::Mass, 500, "斤|市斤"},
    {"两", Category::Mass, 50, "两|市两"},
    // temperature (its own rule)
    {"°C", Category::Temperature, 1, "c|°c|℃|celsius|centigrade|摄氏度|摄氏|度"},
    {"°F", Category::Temperature, 1, "f|°f|℉|fahrenheit|华氏度|华氏"},
    {"K", Category::Temperature, 1, "k|kelvin|开尔文|开氏度"},
    // data (base: byte; KB/MB/… are 1024-based, as Windows counts, with a
    // 1000-based row added; KiB/MiB/… are strictly 1024-based, no rows)
    {"B", Category::Data, 1, "b|byte|bytes|字节"},
    {"KB", Category::Data, 1024, "kb|kilobyte|kilobytes|千字节"},
    {"MB", Category::Data, 1048576, "mb|megabyte|megabytes|兆|兆字节"},
    {"GB", Category::Data, 1073741824, "gb|gigabyte|gigabytes|吉字节"},
    {"TB", Category::Data, 1099511627776.0, "tb|terabyte|terabytes|太字节"},
    {"PB", Category::Data, 1125899906842624.0, "pb|petabyte|petabytes|拍字节"},
    {"KiB", Category::Data, 1024, "kib|kibibyte|kibibytes"},
    {"MiB", Category::Data, 1048576, "mib|mebibyte|mebibytes"},
    {"GiB", Category::Data, 1073741824, "gib|gibibyte|gibibytes"},
    {"TiB", Category::Data, 1099511627776.0, "tib|tebibyte|tebibytes"},
    {"PiB", Category::Data, 1125899906842624.0, "pib|pebibyte|pebibytes"},
    // time (base: second)
    {"ms", Category::Time, 0.001, "ms|millisecond|milliseconds|毫秒"},
    {"s", Category::Time, 1, "s|sec|secs|second|seconds|秒"},
    {"min", Category::Time, 60, "min|mins|minute|minutes|分钟|分"},
    {"h", Category::Time, 3600, "h|hr|hrs|hour|hours|小时|时"},
    {"day", Category::Time, 86400, "d|day|days|天|日"},
    {"week", Category::Time, 604800, "w|wk|week|weeks|周|星期"},
    // area (base: square metre)
    {"m²", Category::Area, 1, "m2|m²|sqm|平方米|平米"},
    {"km²", Category::Area, 1000000, "km2|km²|平方千米|平方公里"},
    {"ha", Category::Area, 10000, "ha|hectare|hectares|公顷"},
    {"亩", Category::Area, 10000.0 / 15.0, "亩|市亩"},
    {"ft²", Category::Area, 0.09290304, "ft2|ft²|sqft|平方英尺"},
    {"acre", Category::Area, 4046.8564224, "acre|acres|英亩"},
    // volume (base: litre)
    {"mL", Category::Volume, 0.001, "ml|milliliter|millilitre|毫升"},
    {"L", Category::Volume, 1, "l|liter|litre|liters|litres|升|公升"},
    {"m³", Category::Volume, 1000, "m3|m³|立方米"},
    {"gal", Category::Volume, 3.785411784, "gal|gallon|gallons|加仑"},
    {"fl oz", Category::Volume, 0.0295735295625, "floz|fl oz|液量盎司"},
    {"cup", Category::Volume, 0.2365882365, "cup|cups|杯"},
};
// clang-format on

std::string lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return s;
}

std::string trim(const std::string& s)
{
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
        --b;
    return s.substr(a, b - a);
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Degrees with a space, as typed: "100 ° F" -> "100 °f".
std::string compact(const std::string& s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == ' ' && i + 1 < s.size() && (s.compare(i + 1, 2, "\xC2\xB0") == 0)) // " °" -> "°"
            continue;
        if (s.compare(i, 2, "\xC2\xB0") == 0 && i + 2 < s.size() && s[i + 2] == ' ') { // "° F" -> "°F"
            out += "\xC2\xB0";
            i += 2;
            continue;
        }
        out += s[i];
    }
    return out;
}

const char* kSeparators[] = {" to ", " in ", " as ", " into ", "->", "=", "\xE2\x86\x92" /* → */, "\xE8\xBD\xAC\xE6\x88\x90" /* 转成 */,
    "\xE6\x8D\xA2\xE6\x88\x90" /* 换成 */, "\xE7\xAD\x89\xE4\xBA\x8E" /* 等于 */, "\xE8\xBD\xAC" /* 转 */, "\xE6\x8D\xA2" /* 换 */,
    "\xE5\x88\xB0" /* 到 */, "\xE6\x88\x90" /* 成 */, "\xE6\x98\xAF" /* 是 */};

} // namespace

const Unit* find(const std::string& name)
{
    const std::string key = lower(trim(name));
    if (key.empty())
        return nullptr;
    for (const Unit& u : kUnits) {
        const char* p = u.names;
        while (*p) {
            const char* end = std::strchr(p, '|');
            const std::size_t n = end ? static_cast<std::size_t>(end - p) : std::strlen(p);
            if (key.size() == n && key.compare(0, n, p, n) == 0)
                return &u;
            p += n;
            if (*p == '|')
                ++p;
        }
    }
    return nullptr;
}

std::optional<Request> parse(const std::string& given)
{
    std::string text = compact(trim(given));
    if (text.empty() || !(isDigit(text[0]) || text[0] == '-' || text[0] == '.' || text[0] == '+'))
        return std::nullopt;
    // The number.
    std::size_t i = 0;
    if (text[i] == '-' || text[i] == '+')
        ++i;
    bool digits = false;
    while (i < text.size() && (isDigit(text[i]) || text[i] == '.' || (text[i] == ',' && i + 1 < text.size() && isDigit(text[i + 1])))) {
        digits = digits || isDigit(text[i]);
        ++i;
    }
    if (!digits)
        return std::nullopt;
    std::string number = text.substr(0, i);
    number.erase(std::remove(number.begin(), number.end(), ','), number.end());
    Request r;
    r.value = std::strtod(number.c_str(), nullptr);
    std::string rest = trim(text.substr(i));
    if (rest.empty())
        return std::nullopt;
    // A separator splits the units; without one, the first space does, or
    // there is one unit only.
    std::string from;
    std::string to;
    bool split = false;
    const std::string lowered = lower(rest);
    for (const char* sep : kSeparators) {
        const std::size_t at = lowered.find(sep);
        if (at == std::string::npos)
            continue;
        from = trim(rest.substr(0, at));
        to = trim(rest.substr(at + std::strlen(sep)));
        split = true;
        break;
    }
    if (!split) {
        const std::size_t space = rest.find(' ');
        if (space == std::string::npos) {
            from = rest;
        } else {
            from = trim(rest.substr(0, space));
            to = trim(rest.substr(space + 1));
        }
    }
    r.from = find(from);
    if (!r.from)
        return std::nullopt;
    if (!to.empty()) {
        r.to = find(to);
        if (!r.to || r.to->category != r.from->category)
            return std::nullopt;
    }
    return r;
}

std::string formatValue(double value)
{
    if (!std::isfinite(value))
        return "?";
    char buffer[64];
    const double magnitude = std::fabs(value);
    if (magnitude != 0 && (magnitude >= 1e15 || magnitude < 1e-7)) {
        std::snprintf(buffer, sizeof buffer, "%.6g", value);
        return buffer;
    }
    std::snprintf(buffer, sizeof buffer, "%.10g", value);
    std::string s = buffer;
    if (s.find('e') != std::string::npos) {
        std::snprintf(buffer, sizeof buffer, "%.10f", value);
        s = buffer;
    }
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
    }
    if (s == "-0")
        s = "0";
    return s;
}

namespace {

double toBase(double value, const Unit& u)
{
    if (u.category != Category::Temperature)
        return value * u.factor;
    if (std::strcmp(u.id, "°C") == 0)
        return value;
    if (std::strcmp(u.id, "°F") == 0)
        return (value - 32) * 5 / 9;
    return value - 273.15; // K
}

double fromBase(double celsiusOrBase, const Unit& u)
{
    if (u.category != Category::Temperature)
        return celsiusOrBase / u.factor;
    if (std::strcmp(u.id, "°C") == 0)
        return celsiusOrBase;
    if (std::strcmp(u.id, "°F") == 0)
        return celsiusOrBase * 9 / 5 + 32;
    return celsiusOrBase + 273.15;
}

// The 1000-based factor of a data unit (KB = 1000 bytes).
double decimalFactor(const Unit& u)
{
    double f = 1;
    double p = u.factor;
    while (p >= 1024) {
        p /= 1024;
        f *= 1000;
    }
    return f;
}

// True for the strictly 1024-based spellings (KiB, MiB, …): no 1000-based
// row next to them, they mean 1024 and nothing else.
bool binaryName(const Unit& u)
{
    const std::string id = u.id;
    return id.size() > 1 && id.back() == 'B' && id[id.size() - 2] == 'i';
}

} // namespace

std::vector<Row> convert(const Request& request, bool chinese)
{
    // The note on a data-size row: how the step was counted. English says
    // the sizes themselves; Chinese says the rule in a word.
    const char* note1024 = chinese ? "按 1024 算" : "1 KB = 1024 B";
    const char* note1000 = chinese ? "按 1000 算" : "1 KB = 1000 B";
    std::vector<Row> rows;
    if (!request.from)
        return rows;
    const double base = toBase(request.value, *request.from);
    const auto one = [&](const Unit& to) {
        const bool sized = to.category == Category::Data
            && (to.factor >= 1024 || request.from->factor >= 1024)
            && !binaryName(to) && !binaryName(*request.from);
        Row row;
        row.value = formatValue(fromBase(base, to));
        row.unit = to.id;
        if (sized)
            row.note = note1024;
        rows.push_back(row);
        if (sized) {
            Row alt;
            alt.value = formatValue(request.value * decimalFactor(*request.from) / decimalFactor(to));
            alt.unit = to.id;
            alt.note = note1000;
            alt.alternate = true;
            rows.push_back(alt);
        }
    };
    if (request.to) {
        one(*request.to);
        return rows;
    }
    for (const Unit& u : kUnits) {
        if (u.category == request.from->category && &u != request.from)
            one(u);
    }
    return rows;
}

} // namespace tools::units
