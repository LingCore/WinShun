#pragma once

// Unit conversion for winshun.tools: length, mass, temperature, data size
// and time, with English, Chinese and symbol names ("12 inch to cm",
// "100 f to c", "1.5 GB in MB", "3 斤 换成 kg"). No Qt.

#include <optional>
#include <string>
#include <vector>

namespace tools::units {

enum class Category { Length, Mass, Temperature, Data, Time, Area, Volume };

struct Unit {
    const char* id; // the name shown ("cm", "inch", "°C", "斤")
    Category category;
    double factor; // to the category's base unit (m, g, °C, byte, s); temperature uses its own rule
    const char* names; // the names it answers to, '|' between them, all lower case
};

struct Request {
    double value = 0;
    const Unit* from = nullptr;
    const Unit* to = nullptr; // null: "12 inch" alone, every unit of the category wanted
};

// "12 inch to cm", "12inch cm", "100 °F → °C", "1.5 GB in MB", "12 inch";
// nullopt when the text is not a conversion (an unknown unit, two
// categories, no number). Separators: to, in, as, =, ->, →, 转, 换成, 换,
// 到, 成, 等于, or just a space.
std::optional<Request> parse(const std::string& text);

struct Row {
    std::string value; // "30.48"
    std::string unit; // "cm"
    std::string note; // "按 1000 算" and the like; empty otherwise
    bool alternate = false; // the 1000-based reading of a data size
};

// The conversions a request asks for: one row (two for data sizes, 1024
// and 1000), or, with no target, one per unit of the category. `chinese`
// says the language of the notes.
std::vector<Row> convert(const Request& request, bool chinese = false);

// A number for the rows: up to 10 significant digits, no trailing zeros.
std::string formatValue(double value);

// The unit named, if any (for tests and the keyword mode's help).
const Unit* find(const std::string& name);

} // namespace tools::units
