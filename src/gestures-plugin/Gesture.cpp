#include "Gesture.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cwctype>

namespace gestures {

namespace {

std::wstring trimmed(std::wstring_view text)
{
    const auto space = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000; };
    std::size_t start = 0;
    std::size_t end = text.size();
    while (start < end && space(text[start]))
        ++start;
    while (end > start && space(text[end - 1]))
        --end;
    return std::wstring(text.substr(start, end - start));
}

// Lower case for ASCII letters only: what the names and keys are written in.
std::wstring lowered(std::wstring_view text)
{
    std::wstring out(text);
    for (wchar_t& c : out) {
        if (c >= L'A' && c <= L'Z')
            c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return out;
}

struct Name {
    const wchar_t* text;
    Builtin builtin;
};

const Name kNames[] = {
    {L"后退", Builtin::Back}, {L"返回", Builtin::Back}, {L"back", Builtin::Back},
    {L"前进", Builtin::Forward}, {L"forward", Builtin::Forward},
    {L"回到顶部", Builtin::Top}, {L"顶部", Builtin::Top}, {L"top", Builtin::Top},
    {L"到底部", Builtin::Bottom}, {L"底部", Builtin::Bottom}, {L"bottom", Builtin::Bottom},
    {L"刷新", Builtin::Refresh}, {L"refresh", Builtin::Refresh},
    {L"关闭窗口", Builtin::Close}, {L"关闭", Builtin::Close}, {L"close", Builtin::Close},
    {L"关闭标签页", Builtin::CloseTab}, {L"关闭标签", Builtin::CloseTab}, {L"closetab", Builtin::CloseTab},
    {L"最小化", Builtin::Minimize}, {L"minimize", Builtin::Minimize},
    {L"最大化", Builtin::Maximize}, {L"maximize", Builtin::Maximize},
    {L"搜索框", Builtin::Search}, {L"打开搜索框", Builtin::Search}, {L"win顺", Builtin::Search}, {L"search", Builtin::Search},
};

struct KeyName {
    const wchar_t* text;
    std::uint16_t vk;
};

const KeyName kKeys[] = {
    {L"left", VK_LEFT}, {L"right", VK_RIGHT}, {L"up", VK_UP}, {L"down", VK_DOWN},
    {L"home", VK_HOME}, {L"end", VK_END}, {L"pgup", VK_PRIOR}, {L"pageup", VK_PRIOR}, {L"pgdn", VK_NEXT},
    {L"pagedown", VK_NEXT}, {L"enter", VK_RETURN}, {L"return", VK_RETURN}, {L"esc", VK_ESCAPE},
    {L"escape", VK_ESCAPE}, {L"tab", VK_TAB}, {L"space", VK_SPACE}, {L"backspace", VK_BACK},
    {L"delete", VK_DELETE}, {L"del", VK_DELETE}, {L"insert", VK_INSERT}, {L"ins", VK_INSERT},
    {L"apps", VK_APPS}, {L"menu", VK_APPS}, {L"printscreen", VK_SNAPSHOT}, {L"prtsc", VK_SNAPSHOT},
    {L"browserback", VK_BROWSER_BACK}, {L"browserforward", VK_BROWSER_FORWARD},
    {L"volumeup", VK_VOLUME_UP}, {L"volumedown", VK_VOLUME_DOWN}, {L"mute", VK_VOLUME_MUTE},
    {L"playpause", VK_MEDIA_PLAY_PAUSE}, {L"nexttrack", VK_MEDIA_NEXT_TRACK}, {L"prevtrack", VK_MEDIA_PREV_TRACK},
    {L"-", VK_OEM_MINUS}, {L"=", VK_OEM_PLUS}, {L",", VK_OEM_COMMA}, {L".", VK_OEM_PERIOD},
    {L"/", VK_OEM_2}, {L";", VK_OEM_1}, {L"[", VK_OEM_4}, {L"]", VK_OEM_6}, {L"`", VK_OEM_3},
};

std::uint16_t modifierKey(const std::wstring& token)
{
    if (token == L"ctrl" || token == L"control" || token == L"ctl")
        return VK_CONTROL;
    if (token == L"shift")
        return VK_SHIFT;
    if (token == L"alt")
        return VK_MENU;
    if (token == L"win" || token == L"windows")
        return VK_LWIN;
    return 0;
}

std::uint16_t plainKey(const std::wstring& token)
{
    if (token.size() == 1) {
        const wchar_t c = token[0];
        if (c >= L'a' && c <= L'z')
            return static_cast<std::uint16_t>(c - L'a' + 'A');
        if (c >= L'0' && c <= L'9')
            return static_cast<std::uint16_t>(c);
    }
    if (token.size() >= 2 && token[0] == L'f' && std::all_of(token.begin() + 1, token.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
        const int n = std::wcstol(token.c_str() + 1, nullptr, 10);
        if (n >= 1 && n <= 24)
            return static_cast<std::uint16_t>(VK_F1 + n - 1);
    }
    for (const KeyName& k : kKeys) {
        if (token == k.text)
            return k.vk;
    }
    return 0;
}

// "Ctrl+Shift+T" -> {VK_CONTROL, VK_SHIFT, 'T'}: modifiers, then one key.
bool parseKeys(const std::wstring& text, std::vector<std::uint16_t>& keys)
{
    std::vector<std::wstring> tokens;
    std::wstring token;
    for (const wchar_t c : text) {
        if (c == L'+') {
            tokens.push_back(lowered(trimmed(token)));
            token.clear();
        } else {
            token += c;
        }
    }
    tokens.push_back(lowered(trimmed(token)));
    keys.clear();
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].empty())
            return false;
        const bool last = i + 1 == tokens.size();
        if (const std::uint16_t m = modifierKey(tokens[i]); m != 0 && !last) {
            if (std::ranges::find(keys, m) != keys.end())
                return false;
            keys.push_back(m);
            continue;
        }
        if (!last)
            return false;
        const std::uint16_t vk = plainKey(tokens[i]);
        if (vk == 0)
            return false;
        keys.push_back(vk);
    }
    return !keys.empty();
}

bool endsWith(const std::wstring& text, std::wstring_view end)
{
    return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}

} // namespace

void Recognizer::reset(int x, int y, int stroke)
{
    m_x = x;
    m_y = y;
    m_stroke = std::max(stroke, 4);
    m_directions.clear();
}

bool Recognizer::add(int x, int y)
{
    const int dx = x - m_x;
    const int dy = y - m_y;
    const int ax = std::abs(dx);
    const int ay = std::abs(dy);
    if (std::max(ax, ay) < m_stroke)
        return false;
    char d = 0;
    // Clearly more along one axis: a quarter more (about 39 degrees off it
    // at most). A slant in between moves the point on once it is long.
    if (ax * 4 > ay * 5)
        d = dx > 0 ? 'R' : 'L';
    else if (ay * 4 > ax * 5)
        d = dy > 0 ? 'D' : 'U';
    if (d == 0) {
        if (std::max(ax, ay) >= m_stroke * 3) {
            m_x = x;
            m_y = y;
        }
        return false;
    }
    m_x = x;
    m_y = y;
    if (!m_directions.empty() && m_directions.back() == d)
        return false;
    m_directions += d;
    return true;
}

std::string arrows(std::string_view directions)
{
    std::string out;
    for (const char d : directions) {
        switch (d) {
        case 'L': out += "\xE2\x86\x90"; break; // ←
        case 'U': out += "\xE2\x86\x91"; break; // ↑
        case 'R': out += "\xE2\x86\x92"; break; // →
        case 'D': out += "\xE2\x86\x93"; break; // ↓
        default: break;
        }
    }
    return out;
}

std::string parseDirections(std::wstring_view text)
{
    std::string out;
    for (const wchar_t c : text) {
        char d = 0;
        switch (c) {
        case L'L': case L'l': case 0x2190: case L'左': d = 'L'; break;
        case L'U': case L'u': case 0x2191: case L'上': d = 'U'; break;
        case L'R': case L'r': case 0x2192: case L'右': d = 'R'; break;
        case L'D': case L'd': case 0x2193: case L'下': d = 'D'; break;
        case L' ': case L'\t': case L'-': case 0x3000: continue;
        default: return {};
        }
        if (!out.empty() && out.back() == d)
            return {}; // the same twice is one direction: a typo
        out += d;
    }
    return out.size() <= kMaxDirections ? out : std::string();
}

Action parseAction(std::wstring_view written)
{
    Action a;
    a.text = trimmed(written);
    if (a.text.empty())
        return a;
    const std::wstring low = lowered(a.text);
    for (const Name& n : kNames) {
        if (low == n.text) {
            a.kind = Action::Kind::Builtin;
            a.builtin = n.builtin;
            return a;
        }
    }
    const bool pathLike = low.find_first_of(L"\\/:") != std::wstring::npos || low.front() == L'"' || endsWith(low, L".exe")
        || endsWith(low, L".lnk") || endsWith(low, L".bat") || endsWith(low, L".cmd");
    // A lone / or : is a key; "Ctrl+/" too.
    if (pathLike && a.text.size() > 1 && a.text.find(L'+') == std::wstring::npos) {
        a.kind = Action::Kind::Open;
        a.target = a.text;
        if (a.target.size() >= 2 && a.target.front() == L'"' && a.target.back() == L'"')
            a.target = trimmed(std::wstring_view(a.target).substr(1, a.target.size() - 2));
        return a;
    }
    if (parseKeys(a.text, a.keys)) {
        a.kind = Action::Kind::Keys;
        return a;
    }
    if (pathLike) {
        a.kind = Action::Kind::Open;
        a.target = a.text;
        return a;
    }
    a.kind = Action::Kind::Invalid;
    return a;
}

std::wstring actionTitle(const Action& action, bool chinese)
{
    switch (action.kind) {
    case Action::Kind::Nothing:
        return {};
    case Action::Kind::Builtin:
        switch (action.builtin) {
        case Builtin::Back: return chinese ? L"后退" : L"Back";
        case Builtin::Forward: return chinese ? L"前进" : L"Forward";
        case Builtin::Top: return chinese ? L"回到顶部" : L"To the top";
        case Builtin::Bottom: return chinese ? L"到底部" : L"To the bottom";
        case Builtin::Refresh: return chinese ? L"刷新" : L"Refresh";
        case Builtin::Close: return chinese ? L"关闭窗口" : L"Close the window";
        case Builtin::CloseTab: return chinese ? L"关闭标签页" : L"Close the tab";
        case Builtin::Minimize: return chinese ? L"最小化" : L"Minimize";
        case Builtin::Maximize: return chinese ? L"最大化" : L"Maximize";
        case Builtin::Search: return chinese ? L"Win顺 搜索框" : L"WinShun search";
        }
        return {};
    case Action::Kind::Keys:
        return action.text;
    case Action::Kind::Open: {
        const std::size_t slash = action.target.find_last_of(L"\\/");
        const bool web = action.target.find(L"://") != std::wstring::npos;
        return (chinese ? L"打开 " : L"Open ") + (slash == std::wstring::npos || web ? action.target : action.target.substr(slash + 1));
    }
    case Action::Kind::Invalid:
        return (chinese ? L"看不懂：" : L"Not understood: ") + action.text;
    }
    return {};
}

std::vector<std::pair<std::string, std::wstring>> parseMore(std::wstring_view text)
{
    std::vector<std::pair<std::string, std::wstring>> out;
    std::wstring item;
    const auto flush = [&] {
        const std::size_t eq = item.find(L'=');
        if (eq != std::wstring::npos) {
            const std::string directions = parseDirections(trimmed(std::wstring_view(item).substr(0, eq)));
            const std::wstring action = trimmed(std::wstring_view(item).substr(eq + 1));
            if (!directions.empty() && !action.empty())
                out.emplace_back(directions, action);
        }
        item.clear();
    };
    for (const wchar_t c : text) {
        if (c == L';' || c == L'；' || c == L'\n' || c == L'\r')
            flush();
        else
            item += c;
    }
    flush();
    return out;
}

std::vector<std::wstring> programList(std::wstring_view text)
{
    std::vector<std::wstring> out;
    std::wstring item;
    const auto flush = [&] {
        std::wstring name = trimmed(item);
        while (!name.empty() && name.back() == L'"')
            name.pop_back();
        while (!name.empty() && name.front() == L'"')
            name.erase(0, 1);
        if (const std::size_t slash = name.find_last_of(L"\\/"); slash != std::wstring::npos)
            name.erase(0, slash + 1);
        name = lowered(trimmed(name));
        if (!name.empty()) {
            if (name.find(L'.') == std::wstring::npos)
                name += L".exe";
            if (std::ranges::find(out, name) == out.end())
                out.push_back(name);
        }
        item.clear();
    };
    for (const wchar_t c : text) {
        if (c == L';' || c == L'；' || c == L',' || c == L'，' || c == L'\n' || c == L'\r')
            flush();
        else
            item += c;
    }
    flush();
    return out;
}

bool listed(const std::vector<std::wstring>& list, std::wstring_view exe)
{
    std::wstring name(exe);
    if (const std::size_t slash = name.find_last_of(L"\\/"); slash != std::wstring::npos)
        name.erase(0, slash + 1);
    name = lowered(name);
    return std::ranges::find(list, name) != list.end();
}

const std::vector<Slot>& defaultSlots()
{
    static const std::vector<Slot> list = {
        {"left", "L", L"后退"},
        {"right", "R", L"前进"},
        {"up", "U", L"回到顶部"},
        {"down", "D", L"到底部"},
        {"downRight", "DR", L"关闭窗口"},
        {"downLeft", "DL", L"最小化"},
        {"upDown", "UD", L"搜索框"},
    };
    return list;
}

} // namespace gestures
