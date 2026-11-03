#include "Rules.h"

#include <windows.h>

#include <algorithm>
#include <array>

namespace tabs {

namespace {

bool equalNoCase(std::wstring_view a, std::wstring_view b)
{
    return a.size() == b.size()
        && (a.empty()
            || ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL);
}

bool containsNoCase(std::wstring_view text, std::wstring_view part)
{
    if (part.size() > text.size())
        return false;
    for (std::size_t at = 0; at + part.size() <= text.size(); ++at) {
        if (equalNoCase(text.substr(at, part.size()), part))
            return true;
    }
    return false;
}

int hexDigit(wchar_t c)
{
    if (c >= L'0' && c <= L'9')
        return c - L'0';
    if (c >= L'a' && c <= L'f')
        return c - L'a' + 10;
    if (c >= L'A' && c <= L'F')
        return c - L'A' + 10;
    return -1;
}

// "file:///C:/a%20b" -> "C:\a b" (UTF-8 percent escapes).
std::wstring fromFileUrl(std::wstring_view url)
{
    std::wstring_view rest = url.substr(5); // after "file:"
    if (rest.starts_with(L"///"))
        rest.remove_prefix(3); // file:///C:/a: C:/a
    // file://server/share stays //server/share: \\server\share below.
    std::string bytes;
    std::wstring out;
    const auto flush = [&] {
        if (bytes.empty())
            return;
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        std::wstring w(static_cast<std::size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), w.data(), n);
        out += w;
        bytes.clear();
    };
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == L'%' && i + 2 < rest.size() && hexDigit(rest[i + 1]) >= 0 && hexDigit(rest[i + 2]) >= 0) {
            bytes.push_back(static_cast<char>(hexDigit(rest[i + 1]) * 16 + hexDigit(rest[i + 2])));
            i += 2;
            continue;
        }
        flush();
        out.push_back(rest[i]);
    }
    flush();
    return out;
}

} // namespace

std::wstring normalized(std::wstring_view place)
{
    while (!place.empty() && (place.front() == L' ' || place.front() == L'"' || place.front() == L'\''))
        place.remove_prefix(1);
    while (!place.empty() && (place.back() == L' ' || place.back() == L'"' || place.back() == L'\''))
        place.remove_suffix(1);
    std::wstring s = place.size() >= 5 && equalNoCase(place.substr(0, 5), L"file:") ? fromFileUrl(place) : std::wstring(place);
    if (s.size() >= 8 && equalNoCase(std::wstring_view(s).substr(0, 8), L"shell:::"))
        s.erase(0, 6); // shell:::{GUID} is ::{GUID}
    std::replace(s.begin(), s.end(), L'/', L'\\');
    // No trailing '\' -- but a drive's root keeps it ("C:\").
    while (s.size() > 1 && s.back() == L'\\' && !(s.size() == 3 && s[1] == L':'))
        s.pop_back();
    if (s.size() == 2 && s[1] == L':')
        s.push_back(L'\\');
    return s;
}

bool samePlace(std::wstring_view a, std::wstring_view b)
{
    const std::wstring x = normalized(a);
    return !x.empty() && equalNoCase(x, normalized(b));
}

bool isUnder(std::wstring_view place, std::wstring_view root)
{
    const std::wstring p = normalized(place);
    std::wstring r = normalized(root);
    if (p.empty() || r.empty())
        return false;
    if (equalNoCase(p, r))
        return true;
    if (r.back() != L'\\')
        r.push_back(L'\\');
    return p.size() > r.size() && equalNoCase(std::wstring_view(p).substr(0, r.size()), r);
}

bool tabsCannotHost(std::wstring_view place)
{
    static constexpr std::array<std::wstring_view, 3> kWindowsOnly {
        L"{26EE0668-A00A-44D7-9371-BEB064C98683}", // Control Panel (category view and its pages)
        L"{21EC2020-3AEA-1069-A2DD-08002B30309D}", // Control Panel, classic
        L"{ED7BA470-8E54-465E-825C-99712043E01C}", // All Tasks ("God mode")
    };
    return std::ranges::any_of(kWindowsOnly, [&](std::wstring_view guid) { return containsNoCase(place, guid); });
}

bool hideEarly(const Settings& s, bool paused, const Restriction& r, const Candidate& b, int otherWindows, bool namedForTest)
{
    if (!s.merge || paused || b.bypass || b.dragging || b.movedTab || b.aboveUs || b.tabs != 1 || otherWindows < 1)
        return false;
    return !r.active() || namedForTest;
}

namespace {

bool eligible(const Window& w, const Restriction& r)
{
    if (!w.usable || w.aboveUs)
        return false;
    if (!r.active())
        return true;
    // Restricted: a window counts only when each of its tabs is a test folder.
    return !w.tabs.empty() && std::ranges::all_of(w.tabs, [&](const std::wstring& place) { return isUnder(place, r.root); });
}

} // namespace

Decision decide(const Settings& s, bool paused, const Restriction& r, const Candidate& b, const std::vector<Window>& others,
    const std::vector<Closed>& closed, std::int64_t nowMs)
{
    const auto keep = [](Why why) { return Decision {Verdict::Keep, why, 0, -1}; };
    if (!s.merge)
        return keep(Why::Off);
    if (paused)
        return keep(Why::Paused);
    if (b.bypass)
        return keep(Why::Bypass);
    if (b.dragging)
        return keep(Why::Dragging);
    if (b.movedTab)
        return keep(Why::MovedTab);
    if (b.aboveUs)
        return keep(Why::AboveUs);
    if (normalized(b.place).empty())
        return keep(Why::Unknown);
    if (r.active() && !isUnder(b.place, r.root))
        return keep(Why::OutsideTestRoot);
    if (tabsCannotHost(b.place))
        return keep(Why::Special);
    if (b.tabs != 1)
        return keep(Why::MultiTab);
    if (std::ranges::any_of(closed, [&](const Closed& c) { return nowMs - c.atMs <= kDraggedOutMs && samePlace(c.place, b.place); }))
        return keep(Why::DraggedOut);

    std::vector<const Window*> candidates;
    for (const Window& w : others) {
        if (eligible(w, r))
            candidates.push_back(&w);
    }
    if (candidates.empty())
        return keep(Why::NoWindow);
    // The preferred window first: a tab on the same folder there wins over
    // one in another window.
    std::ranges::sort(candidates, [&](const Window* x, const Window* y) {
        return s.target == Target::First ? x->openOrder < y->openOrder : x->zOrder < y->zOrder;
    });
    if (s.reuse) {
        for (const Window* w : candidates) {
            for (std::size_t i = 0; i < w->tabs.size(); ++i) {
                if (samePlace(w->tabs[i], b.place))
                    return {Verdict::SwitchTab, Why::Reused, w->id, static_cast<int>(i)};
            }
        }
    }
    return {Verdict::NewTab, Why::Merged, candidates.front()->id, -1};
}

const char* whyName(Why why)
{
    switch (why) {
    case Why::Merged: return "merged";
    case Why::Off: return "off";
    case Why::Paused: return "paused";
    case Why::Bypass: return "ctrl+shift";
    case Why::Dragging: return "dragging";
    case Why::MovedTab: return "moved tab";
    case Why::DraggedOut: return "dragged out";
    case Why::AboveUs: return "above our integrity";
    case Why::Unknown: return "place unknown";
    case Why::Special: return "special folder";
    case Why::MultiTab: return "several tabs";
    case Why::NoWindow: return "no other window";
    case Why::OutsideTestRoot: return "outside the test root";
    case Why::Reused: return "reused";
    }
    return "?";
}

} // namespace tabs
