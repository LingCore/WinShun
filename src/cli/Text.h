#pragma once

// The two languages ws speaks (docs/design-cli.md §5): Win顺's own (its
// settings file), else the one Windows is shown in; WS_LANG overrides. Field
// names of --json and the exit codes never change with it.

#include <format>
#include <string>
#include <string_view>

namespace ws::cli {

enum class Lang { Zh, En };

// One message in both languages. std::format fields ({}) are allowed.
struct Text {
    std::wstring_view zh;
    std::wstring_view en;
};

void setLanguage(Lang lang) noexcept;
Lang language() noexcept;

// "zh", "zh-CN", "en", "EN-us"…: that language; anything else ("system",
// empty) the one Windows is shown in.
Lang languageNamed(std::wstring_view name) noexcept;

std::wstring_view tr(const Text& text) noexcept;

template <typename... Args>
std::wstring fmt(const Text& text, const Args&... args)
{
    return std::vformat(tr(text), std::make_wformat_args(args...));
}

} // namespace ws::cli
