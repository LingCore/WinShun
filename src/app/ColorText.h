#pragma once

#include <QColor>
#include <QString>
#include <QStringView>

#include <optional>
#include <vector>

// Colour values written as text, the way CSS and design tools copy them.
namespace ws::colortext {

constexpr qsizetype kMaxLength = 48; // longer text is never just a colour

// #rgb, #rgba, #rrggbb, #rrggbbaa (alpha last, as in CSS), rgb() / rgba()
// with numbers or percentages, hsl() / hsla(); commas or spaces, alpha after
// a comma or a slash (0.5 or 50%), and a semicolon at the end, as copied
// from a style sheet.
std::optional<QColor> parse(QStringView text);

struct Notation {
    QString label; // "HEX", "ARGB", "RGB", "HSL"
    QString value;
    bool readsBack = true; // false for #aarrggbb: parse() takes it for CSS's #rrggbbaa
};
// The colour in each notation, with its alpha when it has one; a translucent
// colour also as #AARRGGBB, alpha first, as Qt, Android and XAML write it.
std::vector<Notation> notations(const QColor& color);

} // namespace ws::colortext
