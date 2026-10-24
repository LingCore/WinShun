#include "Text.h"

#include <windows.h>

#include <atomic>
#include <cwctype>

namespace ws::cli {

namespace {

std::atomic<Lang> g_language {Lang::En};

bool startsWithIgnoringCase(std::wstring_view text, std::wstring_view prefix) noexcept
{
    if (text.size() < prefix.size())
        return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::towlower(text[i]) != std::towlower(prefix[i]))
            return false;
    }
    return true;
}

} // namespace

void setLanguage(Lang lang) noexcept
{
    g_language.store(lang, std::memory_order_relaxed);
}

Lang language() noexcept
{
    return g_language.load(std::memory_order_relaxed);
}

Lang languageNamed(std::wstring_view name) noexcept
{
    if (startsWithIgnoringCase(name, L"zh"))
        return Lang::Zh;
    if (startsWithIgnoringCase(name, L"en"))
        return Lang::En;
    // As Win顺 does (Settings::resolveLanguage): the language Windows itself
    // is shown in, not the first of the preferred languages.
    return PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_CHINESE ? Lang::Zh : Lang::En;
}

std::wstring_view tr(const Text& text) noexcept
{
    return language() == Lang::Zh ? text.zh : text.en;
}

} // namespace ws::cli
