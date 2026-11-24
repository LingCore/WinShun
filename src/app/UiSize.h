#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringView>

#include <algorithm>
#include <array>

namespace ws::uisize {

// How large Win顺's interface is drawn (Settings::uiSize, 外观 > 界面大小;
// Settings::clipboardUiSize, 剪贴板 > 剪贴板大小): text sizes and row heights.
// Theme.qml and UiScale.qml mirror these steps for QML; C++ uses Metrics for
// window placement and step() / feedback() for Ctrl++ / Ctrl+- / Ctrl+0.
// All dimensions are whole logical pixels.
enum class Size { Dense, Compact, Standard, Roomy };

inline constexpr int kCount = 4;
inline constexpr std::array<QStringView, kCount> kNames {u"dense", u"compact", u"standard", u"roomy"};

constexpr int index(Size size) noexcept
{
    return static_cast<int>(size);
}

constexpr QStringView name(Size size) noexcept
{
    return kNames[static_cast<std::size_t>(std::clamp(index(size), 0, kCount - 1))];
}

constexpr bool isValid(QStringView text) noexcept
{
    for (const QStringView candidate : kNames) {
        if (candidate == text)
            return true;
    }
    return false;
}

constexpr Size parse(QStringView name) noexcept
{
    return name == u"dense" ? Size::Dense
        : name == u"compact" ? Size::Compact
        : name == u"roomy" ? Size::Roomy
                           : Size::Standard;
}

// Coarse typography step shared by SettingsWindow, tips, dialogs and DropPie
// (Theme.step): 0 for dense/compact, 1 for standard, 2 for roomy.
constexpr int fontStep(Size size) noexcept
{
    return size == Size::Roomy ? 2 : size == Size::Standard ? 1 : 0;
}

struct StepResult {
    Size size = Size::Standard;
    bool changed = false;
    bool atLimit = false;
};

// Move one step larger (delta > 0), smaller (delta < 0), or back to Standard
// (delta == 0). Clamps cleanly at Dense / Roomy and reports atLimit when
// already at the requested bound.
constexpr StepResult step(Size current, int delta) noexcept
{
    if (delta == 0)
        return {Size::Standard, current != Size::Standard, false};
    const int from = index(current);
    const int to = std::clamp(from + (delta > 0 ? 1 : -1), 0, kCount - 1);
    return {static_cast<Size>(to), to != from, to == from};
}

inline QString label(Size size)
{
    switch (size) {
    case Size::Dense: return QCoreApplication::translate("UiSize", "密集");
    case Size::Compact: return QCoreApplication::translate("UiSize", "紧凑");
    case Size::Roomy: return QCoreApplication::translate("UiSize", "宽松");
    case Size::Standard: break;
    }
    return QCoreApplication::translate("UiSize", "标准");
}

inline QString feedback(StepResult result, int delta, bool clipboard = false, bool following = false)
{
    const QString sizeName = label(result.size);
    if (clipboard) {
        if (delta == 0 && following) {
            return result.changed
                ? QCoreApplication::translate("UiSize", "剪贴板大小：跟随界面大小（%1）").arg(sizeName)
                : QCoreApplication::translate("UiSize", "剪贴板大小：已跟随界面大小（%1）").arg(sizeName);
        }
        if (result.atLimit) {
            return delta > 0
                ? QCoreApplication::translate("UiSize", "剪贴板大小：%1（已是最大）").arg(sizeName)
                : QCoreApplication::translate("UiSize", "剪贴板大小：%1（已是最小）").arg(sizeName);
        }
        return QCoreApplication::translate("UiSize", "剪贴板大小：%1").arg(sizeName);
    }
    if (delta == 0) {
        return result.changed
            ? QCoreApplication::translate("UiSize", "界面大小：%1（已恢复默认）").arg(sizeName)
            : QCoreApplication::translate("UiSize", "界面大小：%1（已是默认）").arg(sizeName);
    }
    if (result.atLimit) {
        return delta > 0
            ? QCoreApplication::translate("UiSize", "界面大小：%1（已是最大）").arg(sizeName)
            : QCoreApplication::translate("UiSize", "界面大小：%1（已是最小）").arg(sizeName);
    }
    return QCoreApplication::translate("UiSize", "界面大小：%1").arg(sizeName);
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
