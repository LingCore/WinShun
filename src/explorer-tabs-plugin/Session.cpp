#include "Session.h"

#include "Rules.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <optional>
#include <ranges>

namespace tabs {

namespace {

constexpr std::string_view kHeader = "WinShun.ExplorerTabs.Session 1";

std::string utf8(std::wstring_view w)
{
    if (w.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wide(std::string_view s)
{
    if (s.empty())
        return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(std::max(n, 0)), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void write(std::string& out, std::string_view keyword, const std::vector<SavedWindow>& windows)
{
    for (const SavedWindow& w : windows) {
        out += keyword;
        for (const int value : {w.left, w.top, w.right, w.bottom, w.maximized ? 1 : 0, w.active})
            out += ' ' + std::to_string(value);
        out += '\n';
        for (const std::wstring& tab : w.tabs)
            out += "tab " + utf8(tab) + '\n';
    }
}

// "window 10 20 810 620 0 1": six whole numbers, nothing else.
std::optional<SavedWindow> windowFrom(std::string_view numbers)
{
    int values[6] {};
    for (int& value : values) {
        while (numbers.starts_with(' '))
            numbers.remove_prefix(1);
        const auto [end, error] = std::from_chars(numbers.data(), numbers.data() + numbers.size(), value);
        if (error != std::errc())
            return std::nullopt;
        numbers.remove_prefix(static_cast<std::size_t>(end - numbers.data()));
    }
    if (!numbers.empty())
        return std::nullopt;
    SavedWindow w;
    w.left = values[0];
    w.top = values[1];
    w.right = values[2];
    w.bottom = values[3];
    w.maximized = values[4] != 0;
    w.active = values[5];
    return w;
}

// Windows without a tab are no windows; the tab in front is one of them.
void tidy(std::vector<SavedWindow>& windows)
{
    std::erase_if(windows, [](const SavedWindow& w) { return w.tabs.empty(); });
    for (SavedWindow& w : windows) {
        if (w.active < -1 || w.active >= static_cast<int>(w.tabs.size()))
            w.active = 0;
    }
}

} // namespace

std::string serialize(const Session& session)
{
    std::string out(kHeader);
    out += '\n';
    out += session.open ? "open 1\n" : "open 0\n";
    write(out, "window", session.windows);
    write(out, "previous", session.previous);
    return out;
}

Session parseSession(std::string_view text)
{
    Session session;
    std::vector<SavedWindow>* list = nullptr; // the one the last window line went to
    bool first = true;
    for (const auto part : text | std::views::split('\n')) {
        std::string_view line(part.begin(), part.end());
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (first) {
            first = false;
            if (line != kHeader)
                return {};
            continue;
        }
        if (line == "open 1" || line == "open 0") {
            session.open = line == "open 1";
        } else if (line.starts_with("window ") || line.starts_with("previous ")) {
            const bool current = line.starts_with("window ");
            list = nullptr;
            if (const auto w = windowFrom(line.substr(line.find(' ')))) {
                list = current ? &session.windows : &session.previous;
                list->push_back(*w);
            }
        } else if (line.starts_with("tab ") && list) {
            if (std::wstring place = wide(line.substr(4)); !place.empty())
                list->back().tabs.push_back(std::move(place));
        }
    }
    tidy(session.windows);
    tidy(session.previous);
    if (session.windows.empty())
        session.open = false;
    return session;
}

Session noted(Session stored, std::vector<SavedWindow> now)
{
    tidy(now);
    stored.open = !now.empty();
    if (stored.open)
        stored.windows = std::move(now);
    return stored;
}

Session newLogon(Session stored)
{
    stored.previous = stored.windows;
    return stored;
}

int tabCount(const std::vector<SavedWindow>& windows)
{
    int count = 0;
    for (const SavedWindow& w : windows)
        count += static_cast<int>(w.tabs.size());
    return count;
}

std::vector<Reopen> planRestore(const std::vector<SavedWindow>& saved, const std::vector<OpenWindow>& open)
{
    const auto holder = [&](const std::wstring& place) -> const OpenWindow* {
        const auto it = std::ranges::find_if(open, [&](const OpenWindow& w) {
            return std::ranges::any_of(w.tabs, [&](const std::wstring& tab) { return !tab.empty() && samePlace(tab, place); });
        });
        return it != open.end() ? &*it : nullptr;
    };
    std::vector<Reopen> plan;
    for (const SavedWindow& window : saved) {
        Reopen reopen;
        reopen.window = window;
        reopen.window.tabs.clear();
        reopen.window.active = -1;
        for (std::size_t i = 0; i < window.tabs.size(); ++i) {
            const std::wstring& place = window.tabs[i];
            if (place.empty() || tabsCannotHost(place))
                continue;
            if (const OpenWindow* there = holder(place)) {
                if (!reopen.into)
                    reopen.into = there->id;
                continue;
            }
            if (static_cast<int>(i) == window.active)
                reopen.window.active = static_cast<int>(reopen.window.tabs.size());
            reopen.window.tabs.push_back(place);
        }
        if (reopen.window.tabs.empty())
            continue;
        if (reopen.into)
            reopen.window.active = -1; // the user's window: what is in front there stays
        plan.push_back(std::move(reopen));
    }
    return plan;
}

} // namespace tabs
