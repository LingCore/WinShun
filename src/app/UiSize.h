#pragma once

#include <QStringView>

namespace ws::uisize {

// How large the launcher, the clipboard window and the bar by file dialogs
// are drawn (Settings::uiSize, 外观 > 界面大小): their type and the heights of
// their rows. Widths, icons and the settings window stay as they are.
// Theme.qml has the same steps for what QML draws; these are the heights C++
// places windows by. All in logical pixels, whole numbers.
enum class Size { Dense, Compact, Standard, Roomy };

inline Size parse(QStringView name)
{
    return name == u"dense" ? Size::Dense
        : name == u"compact" ? Size::Compact
        : name == u"roomy" ? Size::Roomy
                           : Size::Standard;
}

struct Metrics {
    int header; // the search field's row on top (Theme.headerHeight)
    int barHeight; // the bar by a file dialog: its box ...
    int barRow; // ... its rows (two lines each)
    int askHeight; // ... the line that asks a question under it
};

constexpr Metrics metrics(Size size)
{
    switch (size) {
    case Size::Dense: // one line per result; the dialog bar's rows stay two lines, as 紧凑 has them
        return {44, 40, 44, 36};
    case Size::Compact:
        return {56, 40, 44, 36};
    case Size::Roomy:
        return {72, 48, 60, 44};
    case Size::Standard:
        break;
    }
    return {64, 44, 52, 40};
}

inline Metrics metrics(QStringView name) { return metrics(parse(name)); }

} // namespace ws::uisize
