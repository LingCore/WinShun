#pragma once

#include <QList>
#include <QString>

#include <optional>

namespace ws::phosphor {

// Win顺's icons are Phosphor Icons (2.1.2, MIT, phosphoricons.com; the fonts
// are in the resources, from third_party/phosphor). The code still names
// them by their Segoe Fluent / MDL2 code points (Glyph.qml, the menus, a
// plugin's "glyph:U+E8E5"), mapped here. An older Windows 10's Segoe MDL2
// Assets lacks the newer icons, and the text took a Chinese character from
// another font there; Phosphor is the same on every Windows.

struct Icon {
    QString text;
    QString family; // Phosphor, or Phosphor-Fill for an "on" state
};

// Registers the two fonts (Regular, Fill): before the first window, and in
// the tests that draw icons.
void loadFonts();
// The Phosphor icon for a Segoe code point (one character), when the table
// has one and the fonts loaded; otherwise the system icon font draws it.
// Phosphor's code points are in the same private use area as Segoe's: one is
// only ever read through this.
std::optional<Icon> forSegoe(const QString& glyph);
// The Segoe code points mapped (the tests check the fonts have each).
QList<QChar> segoeCodes();

// Phosphor draws within about 82% of its em square, Segoe within nearly all
// of it: drawn this much larger in the same box, both weigh the same.
inline constexpr double kScale = 1.125;

} // namespace ws::phosphor
