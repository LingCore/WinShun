#pragma once

#include <QList>
#include <QString>

#include <optional>

namespace ws::phosphor {

// Win顺's icons are Phosphor Icons (2.1.2, MIT, phosphoricons.com; the fonts
// are in the resources, from third_party/phosphor). New code names an icon
// ("ph:heart"), while the legacy code still uses Segoe Fluent / MDL2 code
// points (Glyph.qml, the menus, a plugin's "glyph:U+E8E5"), translated by
// PhosphorAliases.h. An older Windows 10's Segoe MDL2 Assets lacks the newer
// icons, and the text took a Chinese character from another font there;
// Phosphor is the same on every Windows.

struct Icon {
    QString text;
    QString family; // Phosphor, or Phosphor-Fill for an "on" state
};

// The two bundled fonts. Other Phosphor weights are not shipped.
enum class Weight { Regular, Fill };

// Registers the two fonts (Regular, Fill): before the first window, and in
// the tests that draw icons.
void loadFonts();
// The named icon, when the name is in the generated table and the font for
// the requested weight is loaded. Names are official Phosphor 2.1.2 names
// such as "heart" and "arrow-square-out": see PhosphorIcons.generated.h.
std::optional<Icon> forName(const QString& name, Weight weight = Weight::Regular);
// "ph:heart" (Regular), "ph-fill:heart" (Fill), or one legacy Segoe
// character. Anything else resolves to nothing: Phosphor's code points are
// in the same private use area as Segoe's, and one must never reach the
// system icon font (docs/pitfalls.md, 2026-10-29).
std::optional<Icon> resolve(const QString& spec);
// The Phosphor icon for a Segoe code point (one character), when there is an
// alias and the fonts are loaded; otherwise the system icon font draws it.
std::optional<Icon> forSegoe(const QString& glyph);
// The Segoe code points mapped (the tests check the fonts have each).
QList<QChar> segoeCodes();

// Phosphor draws within about 82% of its em square, Segoe within nearly all
// of it: drawn this much larger in the same box, both weigh the same.
inline constexpr double kScale = 1.125;

} // namespace ws::phosphor
