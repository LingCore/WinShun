#include "Gesture.h"

#include "winshun.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <span>
#include <utility>

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

const std::vector<BuiltinInfo> kBuiltins = {
    {Builtin::Back, "back", Category::Page, L"后退", L"Back"},
    {Builtin::Forward, "forward", Category::Page, L"前进", L"Forward"},
    {Builtin::Refresh, "refresh", Category::Page, L"刷新", L"Refresh"},
    {Builtin::Top, "top", Category::Page, L"回到顶部", L"To the top"},
    {Builtin::Bottom, "bottom", Category::Page, L"到底部", L"To the bottom"},
    {Builtin::NewTab, "newtab", Category::Tabs, L"新建标签页", L"New tab"},
    {Builtin::CloseTab, "closetab", Category::Tabs, L"关闭标签页", L"Close the tab"},
    {Builtin::ReopenTab, "reopentab", Category::Tabs, L"重新打开关掉的标签页", L"Reopen the closed tab"},
    {Builtin::PrevTab, "prevtab", Category::Tabs, L"上一个标签页", L"Previous tab"},
    {Builtin::NextTab, "nexttab", Category::Tabs, L"下一个标签页", L"Next tab"},
    {Builtin::Close, "close", Category::Window, L"关闭窗口", L"Close the window"},
    {Builtin::Minimize, "minimize", Category::Window, L"最小化", L"Minimize"},
    {Builtin::Maximize, "maximize", Category::Window, L"最大化或还原", L"Maximize or restore"},
    {Builtin::Topmost, "topmost", Category::Window, L"置顶或取消置顶", L"Keep on top, or not"},
    {Builtin::Desktop, "desktop", Category::Window, L"显示桌面", L"Show the desktop"},
    {Builtin::TaskView, "taskview", Category::Window, L"任务视图", L"Task view"},
    {Builtin::LastWindow, "lastwindow", Category::Window, L"切到上一个窗口", L"The window before"},
    {Builtin::Copy, "copy", Category::Text, L"复制", L"Copy"},
    {Builtin::Cut, "cut", Category::Text, L"剪切", L"Cut"},
    {Builtin::Paste, "paste", Category::Text, L"粘贴", L"Paste"},
    {Builtin::Undo, "undo", Category::Text, L"撤销", L"Undo"},
    {Builtin::Redo, "redo", Category::Text, L"重做", L"Redo"},
    {Builtin::SelectAll, "selectall", Category::Text, L"全选", L"Select all"},
    {Builtin::WebSearch, "websearch", Category::Text, L"网页搜索", L"Search the web"},
    {Builtin::FindInWinShun, "findwinshun", Category::Text, L"用 Win顺 搜", L"Find with WinShun"},
    {Builtin::Translate, "translate", Category::Text, L"翻译", L"Translate"},
    {Builtin::Search, "search", Category::WinShun, L"Win顺 搜索框", L"WinShun search"},
    {Builtin::Mute, "mute", Category::System, L"静音", L"Mute"},
    {Builtin::VolumeUp, "volumeup", Category::System, L"音量加", L"Volume up"},
    {Builtin::VolumeDown, "volumedown", Category::System, L"音量减", L"Volume down"},
    {Builtin::PlayPause, "playpause", Category::System, L"播放或暂停", L"Play or pause"},
    {Builtin::NextTrack, "nexttrack", Category::System, L"下一首", L"Next track"},
    {Builtin::PrevTrack, "prevtrack", Category::System, L"上一首", L"Previous track"},
    {Builtin::Lock, "lock", Category::System, L"锁屏", L"Lock"},
    {Builtin::ScreenOff, "screenoff", Category::System, L"关闭屏幕", L"Turn the screen off"},
    {Builtin::Sleep, "sleep", Category::System, L"睡眠", L"Sleep"},
    {Builtin::Shutdown, "shutdown", Category::System, L"关机", L"Shut down"},
    {Builtin::Restart, "restart", Category::System, L"重启", L"Restart"},
    {Builtin::SignOut, "signout", Category::System, L"注销", L"Sign out"},
};

// Other names 1.0's settings took (and some one may well write).
struct Alias {
    const wchar_t* text;
    Builtin builtin;
};
const Alias kAliases[] = {
    {L"返回", Builtin::Back}, {L"顶部", Builtin::Top}, {L"底部", Builtin::Bottom}, {L"关闭", Builtin::Close},
    {L"关闭标签", Builtin::CloseTab}, {L"最大化", Builtin::Maximize}, {L"搜索框", Builtin::Search},
    {L"打开搜索框", Builtin::Search}, {L"win顺", Builtin::Search}, {L"置顶", Builtin::Topmost},
    {L"新标签页", Builtin::NewTab}, {L"重新打开标签页", Builtin::ReopenTab}, {L"播放", Builtin::PlayPause},
    {L"暂停", Builtin::PlayPause}, {L"搜索", Builtin::WebSearch}, {L"关屏", Builtin::ScreenOff},
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
    {L"←", VK_LEFT}, {L"→", VK_RIGHT}, {L"↑", VK_UP}, {L"↓", VK_DOWN},
    {L"-", VK_OEM_MINUS}, {L"=", VK_OEM_PLUS}, {L",", VK_OEM_COMMA}, {L".", VK_OEM_PERIOD},
    {L"/", VK_OEM_2}, {L";", VK_OEM_1}, {L"[", VK_OEM_4}, {L"]", VK_OEM_6}, {L"`", VK_OEM_3},
    {L"'", VK_OEM_7}, {L"\\", VK_OEM_5},
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
        if (c == L'+' && !trimmed(token).empty()) {
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
        const std::uint16_t vk = tokens[i] == L"+" ? std::uint16_t(VK_OEM_PLUS) : plainKey(tokens[i]);
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

std::wstring withoutQuotes(std::wstring text)
{
    text = trimmed(text);
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"')
        text = trimmed(std::wstring_view(text).substr(1, text.size() - 2));
    return text;
}

// A stroke as kept: one to kMaxDirections of L R U D, no letter twice in a
// row; or a circle.
bool validStroke(std::string_view on)
{
    if (on == kClockwise || on == kCounterClockwise)
        return true;
    if (on.empty() || on.size() > kMaxDirections)
        return false;
    for (std::size_t i = 0; i < on.size(); ++i) {
        if (on[i] != 'L' && on[i] != 'R' && on[i] != 'U' && on[i] != 'D')
            return false;
        if (i > 0 && on[i] == on[i - 1])
            return false;
    }
    return true;
}

std::optional<MouseButton> buttonByShortId(std::string_view id)
{
    if (id == "r")
        return MouseButton::Right;
    if (id == "m")
        return MouseButton::Middle;
    if (id == "x1")
        return MouseButton::X1;
    if (id == "x2")
        return MouseButton::X2;
    if (id == "l")
        return MouseButton::Left;
    return std::nullopt;
}

std::uint8_t buttonBit(MouseButton button)
{
    return button == MouseButton::Any ? 0 : static_cast<std::uint8_t>(1u << static_cast<unsigned>(button));
}

bool sameApps(std::vector<std::wstring> a, std::vector<std::wstring> b)
{
    std::ranges::sort(a);
    std::ranges::sort(b);
    return a == b;
}

int indexOf(std::span<const std::string_view> list, std::string_view value)
{
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i] == value)
            return static_cast<int>(i);
    }
    return -1;
}

Rule plainRule(std::string on, Action action)
{
    Rule r;
    r.on = std::move(on);
    r.action = std::move(action);
    return r;
}

// The rule for `on` everywhere, with nothing more to it, made or replaced;
// a rule that does nothing is taken out instead (nothing set: the same).
void setPlain(std::vector<Rule>& rules, const std::string& on, Action action)
{
    const auto plain = [&](const Rule& r) { return r.on == on && r.where == Zone::Anywhere && !r.selection && r.apps.empty(); };
    const auto found = std::ranges::find_if(rules, plain);
    if (action.kind == Action::Kind::Nothing) {
        if (found != rules.end())
            rules.erase(found);
        return;
    }
    if (found != rules.end())
        found->action = std::move(action);
    else
        rules.push_back(plainRule(on, std::move(action)));
}

} // namespace

// ---- what is drawn ----

void Recognizer::reset(int x, int y, int stroke)
{
    m_x = m_startX = m_lastX = m_segmentX = m_left = m_right = x;
    m_y = m_startY = m_lastY = m_segmentY = m_top = m_bottom = y;
    m_stroke = std::max(stroke, 4);
    m_directions.clear();
    m_hasHeading = false;
    m_heading = 0;
    m_turned = 0;
}

bool Recognizer::add(int x, int y)
{
    const std::string before = gesture();
    m_lastX = x;
    m_lastY = y;
    m_left = std::min(m_left, x);
    m_right = std::max(m_right, x);
    m_top = std::min(m_top, y);
    m_bottom = std::max(m_bottom, y);

    // The turning, from segment to segment of half a stroke: a circle turns
    // a little at each; a sharp turn back (a zigzag) is no part of one.
    const int sx = x - m_segmentX;
    const int sy = y - m_segmentY;
    const int half = m_stroke / 2;
    if (sx * sx + sy * sy >= half * half) {
        const double heading = std::atan2(static_cast<double>(sy), static_cast<double>(sx));
        if (m_hasHeading) {
            double turn = heading - m_heading;
            while (turn > std::numbers::pi)
                turn -= 2 * std::numbers::pi;
            while (turn <= -std::numbers::pi)
                turn += 2 * std::numbers::pi;
            if (std::abs(turn) < 2 * std::numbers::pi / 3)
                m_turned += turn;
        }
        m_heading = heading;
        m_hasHeading = true;
        m_segmentX = x;
        m_segmentY = y;
    }

    const int dx = x - m_x;
    const int dy = y - m_y;
    const int ax = std::abs(dx);
    const int ay = std::abs(dy);
    if (std::max(ax, ay) >= m_stroke) {
        char d = 0;
        // Clearly more along one axis: a quarter more (about 39 degrees off
        // it at most). A slant in between moves the point on once it is long.
        if (ax * 4 > ay * 5)
            d = dx > 0 ? 'R' : 'L';
        else if (ay * 4 > ax * 5)
            d = dy > 0 ? 'D' : 'U';
        if (d != 0) {
            m_x = x;
            m_y = y;
            if (m_directions.empty() || m_directions.back() != d)
                m_directions += d;
        } else if (std::max(ax, ay) >= m_stroke * 3) {
            m_x = x;
            m_y = y;
        }
    }
    return gesture() != before;
}

std::string_view Recognizer::circle() const
{
    constexpr double kFullTurn = 300.0 / 180.0 * std::numbers::pi;
    if (std::abs(m_turned) < kFullTurn)
        return {};
    const int w = m_right - m_left;
    const int h = m_bottom - m_top;
    const int small = std::min(w, h);
    const int large = std::max(w, h);
    if (small < 2 * m_stroke || large > 3 * small)
        return {};
    const double away = std::hypot(static_cast<double>(m_lastX - m_startX), static_cast<double>(m_lastY - m_startY));
    if (away > 0.45 * large)
        return {};
    return m_turned > 0 ? kClockwise : kCounterClockwise;
}

std::string Recognizer::gesture() const
{
    if (const std::string_view c = circle(); !c.empty())
        return std::string(c);
    return m_directions;
}

std::string arrows(std::string_view gesture)
{
    if (gesture == kClockwise)
        return "\xE2\x86\xBB"; // ↻
    if (gesture == kCounterClockwise)
        return "\xE2\x86\xBA"; // ↺
    std::string out;
    for (const char d : gesture) {
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
    const std::wstring whole = lowered(trimmed(text));
    if (whole == L"\x21BB" || whole == L"cw" || whole == L"顺时针" || whole == L"顺时针画圈" || whole == L"画圈")
        return std::string(kClockwise);
    if (whole == L"\x21BA" || whole == L"ccw" || whole == L"逆时针" || whole == L"逆时针画圈")
        return std::string(kCounterClockwise);
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

// ---- what a rule does ----

const std::vector<BuiltinInfo>& builtins()
{
    return kBuiltins;
}

const BuiltinInfo& info(Builtin builtin)
{
    for (const BuiltinInfo& b : kBuiltins) {
        if (b.builtin == builtin)
            return b;
    }
    return kBuiltins.front();
}

std::optional<Builtin> builtinById(std::string_view id)
{
    for (const BuiltinInfo& b : kBuiltins) {
        if (id == b.id)
            return b.builtin;
    }
    return std::nullopt;
}

const wchar_t* categoryTitle(Category category, bool chinese)
{
    switch (category) {
    case Category::Page: return chinese ? L"网页" : L"Pages";
    case Category::Tabs: return chinese ? L"标签页" : L"Tabs";
    case Category::Window: return chinese ? L"窗口" : L"Windows";
    case Category::Text: return chinese ? L"文字" : L"Text";
    case Category::WinShun: return L"Win顺";
    case Category::System: return chinese ? L"系统" : L"System";
    }
    return L"";
}

bool takesSelection(Builtin builtin)
{
    return builtin == Builtin::WebSearch || builtin == Builtin::FindInWinShun || builtin == Builtin::Translate;
}

bool asksFirst(Builtin builtin)
{
    return builtin == Builtin::Shutdown || builtin == Builtin::Restart || builtin == Builtin::SignOut || builtin == Builtin::Sleep;
}

Action builtinAction(Builtin builtin)
{
    Action a;
    a.kind = Action::Kind::Builtin;
    a.builtin = builtin;
    return a;
}

Action keysAction(std::wstring_view written)
{
    Action a;
    a.text = trimmed(written);
    if (a.text.empty())
        return a;
    a.kind = parseKeys(a.text, a.keys) ? Action::Kind::Keys : Action::Kind::Invalid;
    if (a.kind == Action::Kind::Invalid)
        a.keys.clear();
    return a;
}

Action openAction(std::wstring_view written)
{
    Action a;
    a.text = trimmed(written);
    a.target = withoutQuotes(a.text);
    if (!a.target.empty())
        a.kind = Action::Kind::Open;
    return a;
}

Action typeAction(std::wstring_view words)
{
    Action a;
    if (words.empty())
        return a;
    a.kind = Action::Kind::Type;
    a.target = std::wstring(words);
    a.text = a.target;
    return a;
}

Action parseAction(std::wstring_view written)
{
    Action a;
    a.text = trimmed(written);
    if (a.text.empty())
        return a;
    const std::wstring low = lowered(a.text);
    for (const BuiltinInfo& b : kBuiltins) {
        if (low == lowered(b.zh) || low == winshun::wide(b.id))
            return builtinAction(b.builtin);
    }
    for (const Alias& n : kAliases) {
        if (low == n.text)
            return builtinAction(n.builtin);
    }
    const bool pathLike = low.find_first_of(L"\\/:") != std::wstring::npos || low.front() == L'"' || endsWith(low, L".exe")
        || endsWith(low, L".lnk") || endsWith(low, L".bat") || endsWith(low, L".cmd");
    // A lone / or : is a key; "Ctrl+/" too.
    if (pathLike && a.text.size() > 1 && a.text.find(L'+') == std::wstring::npos)
        return openAction(a.text);
    if (Action keys = keysAction(a.text); keys.kind == Action::Kind::Keys)
        return keys;
    if (pathLike)
        return openAction(a.text);
    a.kind = Action::Kind::Invalid;
    return a;
}

std::wstring actionTitle(const Action& action, bool chinese)
{
    switch (action.kind) {
    case Action::Kind::Nothing:
        return {};
    case Action::Kind::Builtin:
        return chinese ? info(action.builtin).zh : info(action.builtin).en;
    case Action::Kind::Keys:
        return action.text;
    case Action::Kind::Open: {
        const std::size_t slash = action.target.find_last_of(L"\\/");
        const bool web = action.target.find(L"://") != std::wstring::npos;
        return (chinese ? L"打开 " : L"Open ") + (slash == std::wstring::npos || web ? action.target : action.target.substr(slash + 1));
    }
    case Action::Kind::Type: {
        std::wstring words;
        for (const wchar_t c : action.target) {
            if (words.size() >= 12) {
                words += L"…";
                break;
            }
            words += c == L'\r' || c == L'\n' || c == L'\t' ? L' ' : c;
        }
        return chinese ? L"输入「" + words + L"」" : L"Type “" + words + L"”";
    }
    case Action::Kind::Invalid:
        return (chinese ? L"看不懂：" : L"Not understood: ") + action.text;
    }
    return {};
}

// ---- mouse buttons ----

const char* buttonId(MouseButton button)
{
    switch (button) {
    case MouseButton::Any: return "";
    case MouseButton::Right: return "right";
    case MouseButton::Middle: return "middle";
    case MouseButton::X1: return "x1";
    case MouseButton::X2: return "x2";
    case MouseButton::Left: return "left";
    }
    return "";
}

const char* buttonShortId(MouseButton button)
{
    switch (button) {
    case MouseButton::Any: return "";
    case MouseButton::Right: return "r";
    case MouseButton::Middle: return "m";
    case MouseButton::X1: return "x1";
    case MouseButton::X2: return "x2";
    case MouseButton::Left: return "l";
    }
    return "";
}

std::optional<MouseButton> buttonById(std::string_view id)
{
    std::string low;
    low.reserve(id.size());
    for (const char c : id) {
        if (c >= 'A' && c <= 'Z')
            low += static_cast<char>(c - 'A' + 'a');
        else if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            low += c;
    }
    if (low == "right" || low == "r")
        return MouseButton::Right;
    if (low == "middle" || low == "m")
        return MouseButton::Middle;
    if (low == "x1" || low == "back")
        return MouseButton::X1;
    if (low == "x2" || low == "forward")
        return MouseButton::X2;
    if (low == "left" || low == "l")
        return MouseButton::Left;
    return std::nullopt;
}

bool isTriggerButton(MouseButton button)
{
    return button == MouseButton::Right || button == MouseButton::Middle || button == MouseButton::X1 || button == MouseButton::X2;
}

std::vector<MouseButton> defaultTriggerButtons()
{
    return {MouseButton::Right};
}

std::vector<MouseButton> parseTriggerButtons(std::string_view text)
{
    std::uint8_t seen = 0;
    std::string token;
    const auto flush = [&] {
        if (const auto btn = buttonById(token); btn && isTriggerButton(*btn))
            seen |= buttonBit(*btn);
        token.clear();
    };
    for (const char c : text) {
        if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n')
            flush();
        else
            token += c;
    }
    flush();
    std::vector<MouseButton> out;
    for (const MouseButton b : {MouseButton::Right, MouseButton::Middle, MouseButton::X1, MouseButton::X2}) {
        if (seen & buttonBit(b))
            out.push_back(b);
    }
    if (out.empty())
        return defaultTriggerButtons();
    return out;
}

std::string formatTriggerButtons(const std::vector<MouseButton>& buttons)
{
    std::uint8_t seen = 0;
    for (const MouseButton b : buttons) {
        if (isTriggerButton(b))
            seen |= buttonBit(b);
    }
    std::string out;
    for (const MouseButton b : {MouseButton::Right, MouseButton::Middle, MouseButton::X1, MouseButton::X2}) {
        if (seen & buttonBit(b)) {
            if (!out.empty())
                out += ',';
            out += buttonId(b);
        }
    }
    return out.empty() ? std::string(buttonId(MouseButton::Right)) : out;
}

bool hasTriggerButton(const std::vector<MouseButton>& buttons, MouseButton button)
{
    return isTriggerButton(button) && std::ranges::find(buttons, button) != buttons.end();
}

const wchar_t* buttonTitle(MouseButton button, bool chinese)
{
    switch (button) {
    case MouseButton::Any: return chinese ? L"任意触发键" : L"Any trigger button";
    case MouseButton::Right: return chinese ? L"右键" : L"Right button";
    case MouseButton::Middle: return chinese ? L"中键" : L"Middle button";
    case MouseButton::X1: return chinese ? L"侧键（后退）" : L"Side 1 (Back)";
    case MouseButton::X2: return chinese ? L"侧键（前进）" : L"Side 2 (Forward)";
    case MouseButton::Left: return chinese ? L"左键" : L"Left button";
    }
    return L"";
}

const wchar_t* buttonShortTitle(MouseButton button, bool chinese)
{
    switch (button) {
    case MouseButton::Any: return L"";
    case MouseButton::Right: return chinese ? L"右" : L"R";
    case MouseButton::Middle: return chinese ? L"中" : L"M";
    case MouseButton::X1: return chinese ? L"侧1" : L"X1";
    case MouseButton::X2: return chinese ? L"侧2" : L"X2";
    case MouseButton::Left: return chinese ? L"左" : L"L";
    }
    return L"";
}

// ---- the rules ----

const char* zoneId(Zone zone)
{
    switch (zone) {
    case Zone::Anywhere: return "";
    case Zone::TopLeft: return "tl";
    case Zone::TopRight: return "tr";
    case Zone::BottomLeft: return "bl";
    case Zone::BottomRight: return "br";
    }
    return "";
}

std::optional<Zone> zoneById(std::string_view id)
{
    for (const Zone z : {Zone::Anywhere, Zone::TopLeft, Zone::TopRight, Zone::BottomLeft, Zone::BottomRight}) {
        if (id == zoneId(z))
            return z;
    }
    return std::nullopt;
}

const wchar_t* zoneTitle(Zone zone, bool chinese)
{
    switch (zone) {
    case Zone::Anywhere: return L"";
    case Zone::TopLeft: return chinese ? L"左上角" : L"Top left";
    case Zone::TopRight: return chinese ? L"右上角" : L"Top right";
    case Zone::BottomLeft: return chinese ? L"左下角" : L"Bottom left";
    case Zone::BottomRight: return chinese ? L"右下角" : L"Bottom right";
    }
    return L"";
}

std::optional<ParsedGesture> parseGestureTrigger(std::string_view on)
{
    if (on.empty())
        return std::nullopt;
    ParsedGesture out;
    if (const std::size_t colon = on.find(':'); colon != std::string_view::npos) {
        const auto btn = buttonByShortId(on.substr(0, colon));
        if (!btn || !isTriggerButton(*btn))
            return std::nullopt;
        out.trigger = *btn;
        on.remove_prefix(colon + 1);
        if (on.find(':') != std::string_view::npos)
            return std::nullopt;
    }
    if (const std::size_t plus = on.find('+'); plus != std::string_view::npos) {
        const std::string_view suffix = on.substr(plus + 1);
        if (suffix.find('+') != std::string_view::npos)
            return std::nullopt;
        const auto chord = buttonByShortId(suffix);
        if (!chord)
            return std::nullopt;
        out.chord = *chord;
        on = on.substr(0, plus);
    }
    if (out.trigger != MouseButton::Any && out.chord == out.trigger)
        return std::nullopt;
    if (out.chord == MouseButton::Any) {
        if (!validStroke(on))
            return std::nullopt;
    } else if (!on.empty() && !validStroke(on)) {
        return std::nullopt;
    }
    out.stroke = std::string(on);
    return out;
}

std::string formatGestureTrigger(MouseButton trigger, std::string_view stroke, MouseButton chord)
{
    if (trigger != MouseButton::Any && !isTriggerButton(trigger))
        return {};
    if (trigger != MouseButton::Any && chord == trigger)
        return {};
    if (chord == MouseButton::Any) {
        if (!validStroke(stroke))
            return {};
    } else if (!stroke.empty() && !validStroke(stroke)) {
        return {};
    }
    std::string out;
    if (trigger != MouseButton::Any) {
        out += buttonShortId(trigger);
        out += ':';
    }
    out += stroke;
    if (chord != MouseButton::Any) {
        out += '+';
        out += buttonShortId(chord);
    }
    return out;
}

std::wstring gestureBadge(std::string_view on, bool chinese)
{
    const auto parsed = parseGestureTrigger(on);
    if (!parsed)
        return winshun::wide(arrows(on));
    std::wstring out;
    if (parsed->trigger != MouseButton::Any)
        out += buttonShortTitle(parsed->trigger, chinese);
    if (!parsed->stroke.empty()) {
        if (!out.empty())
            out += L' ';
        out += winshun::wide(arrows(parsed->stroke));
    }
    if (parsed->chord != MouseButton::Any) {
        out += L'+';
        out += buttonShortTitle(parsed->chord, chinese);
    }
    return out;
}

std::optional<ParsedWheel> parseWheelTrigger(std::string_view on)
{
    if (on == kWheelUp)
        return ParsedWheel {MouseButton::Any, true};
    if (on == kWheelDown)
        return ParsedWheel {MouseButton::Any, false};
    const std::size_t colon = on.find(':');
    if (colon == std::string_view::npos)
        return std::nullopt;
    const auto btn = buttonByShortId(on.substr(0, colon));
    if (!btn || !isTriggerButton(*btn))
        return std::nullopt;
    const std::string_view rest = on.substr(colon + 1);
    if (rest == kWheelUp)
        return ParsedWheel {*btn, true};
    if (rest == kWheelDown)
        return ParsedWheel {*btn, false};
    return std::nullopt;
}

std::string formatWheelTrigger(MouseButton trigger, bool up)
{
    if (trigger != MouseButton::Any && !isTriggerButton(trigger))
        return {};
    std::string out;
    if (trigger != MouseButton::Any) {
        out += buttonShortId(trigger);
        out += ':';
    }
    out += up ? kWheelUp : kWheelDown;
    return out;
}

TriggerKind triggerKind(std::string_view on)
{
    if (indexOf(kCorners, on) >= 0)
        return TriggerKind::Corner;
    if (indexOf(kEdges, on) >= 0)
        return TriggerKind::Edge;
    if (parseWheelTrigger(on).has_value())
        return TriggerKind::Wheel;
    if (on.starts_with(kKeyPrefix)) // only as keyTrigger writes it
        return keyTrigger(winshun::wide(std::string(on.substr(kKeyPrefix.size())))) == on ? TriggerKind::Key : TriggerKind::Unknown;
    return parseGestureTrigger(on).has_value() ? TriggerKind::Gesture : TriggerKind::Unknown;
}

std::string keyTrigger(std::wstring_view keys)
{
    std::vector<std::uint16_t> vks;
    if (!parseKeys(std::wstring(keys), vks))
        return {};
    const std::uint16_t key = vks.back();
    const auto held = [&](std::uint16_t vk) { return std::find(vks.begin(), vks.end() - 1, vk) != vks.end() - 1; };
    if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU || key == VK_LWIN)
        return {}; // a modifier alone
    const bool function = key >= VK_F1 && key <= VK_F24;
    if (!function && !held(VK_CONTROL) && !held(VK_MENU) && !held(VK_LWIN))
        return {};
    // The key as it was written (parseKeys read it), its first letter up: "T", "F5", "Space", "/".
    const std::size_t plus = keys.size() > 1 && keys.back() == L'+' ? keys.size() - 1 : keys.find_last_of(L'+');
    std::wstring name = trimmed(plus == std::wstring_view::npos ? keys : keys.substr(plus + 1));
    if (name.empty())
        name = L"+";
    name = lowered(name);
    if (!name.empty() && name[0] >= L'a' && name[0] <= L'z')
        name[0] = static_cast<wchar_t>(name[0] - L'a' + L'A');
    std::wstring out;
    for (const auto& [vk, word] : {std::pair {std::uint16_t(VK_LWIN), L"Win"}, std::pair {std::uint16_t(VK_CONTROL), L"Ctrl"},
             std::pair {std::uint16_t(VK_MENU), L"Alt"}, std::pair {std::uint16_t(VK_SHIFT), L"Shift"}}) {
        if (held(vk))
            out += std::wstring(word) + L"+";
    }
    return std::string(kKeyPrefix) + winshun::utf8(out + name);
}

bool hotkeyOf(std::string_view on, unsigned* modifiers, unsigned* vk)
{
    if (triggerKind(on) != TriggerKind::Key)
        return false;
    std::vector<std::uint16_t> vks;
    if (!parseKeys(winshun::wide(std::string(on.substr(kKeyPrefix.size()))), vks))
        return false;
    unsigned mods = 0;
    for (std::size_t i = 0; i + 1 < vks.size(); ++i) {
        switch (vks[i]) {
        case VK_CONTROL: mods |= MOD_CONTROL; break;
        case VK_SHIFT: mods |= MOD_SHIFT; break;
        case VK_MENU: mods |= MOD_ALT; break;
        case VK_LWIN: mods |= MOD_WIN; break;
        default: break;
        }
    }
    *modifiers = mods;
    *vk = vks.back();
    return true;
}

bool shortcutNeedsMask(unsigned modifiers) noexcept
{
    return (modifiers & (MOD_ALT | MOD_WIN | MOD_SHIFT)) != 0;
}

std::wstring triggerTitle(std::string_view on, bool chinese)
{
    static constexpr const wchar_t* kCornerTitles[2][4] = {
        {L"Top left corner", L"Top right corner", L"Bottom left corner", L"Bottom right corner"},
        {L"左上角", L"右上角", L"左下角", L"右下角"},
    };
    static constexpr const wchar_t* kEdgeTitles[2][4] = {
        {L"Top edge", L"Bottom edge", L"Left edge", L"Right edge"},
        {L"上边", L"下边", L"左边", L"右边"},
    };
    if (const int i = indexOf(kCorners, on); i >= 0)
        return kCornerTitles[chinese][i];
    if (const int i = indexOf(kEdges, on); i >= 0)
        return kEdgeTitles[chinese][i];
    if (const auto w = parseWheelTrigger(on)) {
        if (w->trigger == MouseButton::Any) {
            if (w->up)
                return chinese ? L"按住手势键向上滚" : L"Hold trigger + wheel up";
            return chinese ? L"按住手势键向下滚" : L"Hold trigger + wheel down";
        }
        const std::wstring btn = buttonTitle(w->trigger, chinese);
        if (w->up)
            return chinese ? L"按住" + btn + L"向上滚" : btn + L" + wheel up";
        return chinese ? L"按住" + btn + L"向下滚" : btn + L" + wheel down";
    }
    if (on.starts_with(kKeyPrefix))
        return winshun::wide(std::string(on.substr(kKeyPrefix.size())));
    return gestureBadge(on, chinese);
}

MouseButton Rule::triggerButton() const
{
    if (const auto g = parseGestureTrigger(on))
        return g->trigger;
    if (const auto w = parseWheelTrigger(on))
        return w->trigger;
    return MouseButton::Any;
}

bool Rule::sameCase(const Rule& other) const
{
    return on == other.on && where == other.where && selection == other.selection && sameApps(apps, other.apps);
}

const Rule* match(const std::vector<Rule>& rules, std::string_view on, const Context& context)
{
    const auto g = parseGestureTrigger(on);
    const auto w = !g ? parseWheelTrigger(on) : std::nullopt;
    const Rule* best = nullptr;
    for (const Rule& r : rules) {
        if (g) {
            const auto rg = parseGestureTrigger(r.on);
            if (!rg || rg->stroke != g->stroke || rg->chord != g->chord)
                continue;
            const MouseButton active = g->trigger != MouseButton::Any ? g->trigger : context.button;
            if (rg->trigger != MouseButton::Any && active != MouseButton::Any && rg->trigger != active)
                continue;
            if (rg->trigger != MouseButton::Any && active == MouseButton::Any)
                continue;
            if (rg->chord != MouseButton::Any && active != MouseButton::Any && rg->chord == active)
                continue;
        } else if (w) {
            const auto rw = parseWheelTrigger(r.on);
            if (!rw || rw->up != w->up)
                continue;
            const MouseButton active = w->trigger != MouseButton::Any ? w->trigger : context.button;
            if (rw->trigger != MouseButton::Any && active != MouseButton::Any && rw->trigger != active)
                continue;
            if (rw->trigger != MouseButton::Any && active == MouseButton::Any)
                continue;
        } else if (r.on != on) {
            continue;
        }
        if (r.where != Zone::Anywhere && r.where != context.zone)
            continue;
        if (r.selection && !context.selection)
            continue;
        if (!r.apps.empty() && !listed(r.apps, context.program))
            continue;
        if (!best || r.specificity() > best->specificity())
            best = &r;
    }
    return best;
}

bool chordMatches(const std::vector<Rule>& rules, std::string_view stroke, MouseButton chord, const Context& context)
{
    if (chord == MouseButton::Any)
        return false;
    if (context.button != MouseButton::Any && chord == context.button)
        return false;
    const std::string probe = formatGestureTrigger(MouseButton::Any, stroke, chord);
    if (probe.empty())
        return false;
    Context plain = context;
    plain.selection = false;
    if (const Rule* r = match(rules, probe, plain); r && r->action.kind != Action::Kind::Nothing)
        return true;
    Context withWords = context;
    withWords.selection = true;
    if (const Rule* r = match(rules, probe, withWords); r && r->action.kind != Action::Kind::Nothing)
        return true;
    return false;
}

bool selectionMatters(const std::vector<Rule>& rules, std::string_view prefix, std::wstring_view program, MouseButton button)
{
    return std::ranges::any_of(rules, [&](const Rule& r) {
        if (!r.selection || r.action.kind == Action::Kind::Nothing)
            return false;
        const auto parsed = parseGestureTrigger(r.on);
        if (!parsed)
            return false;
        if (parsed->trigger != MouseButton::Any && button != MouseButton::Any && parsed->trigger != button)
            return false;
        if (!r.apps.empty() && !listed(r.apps, program))
            return false;
        if (prefix.empty())
            return true;
        const bool circle = parsed->stroke == kClockwise || parsed->stroke == kCounterClockwise;
        return circle || std::string_view(parsed->stroke).starts_with(prefix);
    });
}

bool anyFor(const std::vector<Rule>& rules, TriggerKind kind)
{
    return std::ranges::any_of(rules, [&](const Rule& r) { return r.action.kind != Action::Kind::Nothing && triggerKind(r.on) == kind; });
}

std::string rulesJson(const std::vector<Rule>& rules)
{
    winshun::Json list = winshun::Json::Array {};
    for (const Rule& r : rules) {
        winshun::Json o = winshun::Json::Object {};
        o.set("on", r.on);
        if (r.where != Zone::Anywhere)
            o.set("where", zoneId(r.where));
        if (r.selection)
            o.set("selection", true);
        if (!r.apps.empty()) {
            winshun::Json apps = winshun::Json::Array {};
            for (const std::wstring& app : r.apps)
                apps.push(winshun::utf8(app));
            o.set("apps", apps);
        }
        switch (r.action.kind) {
        case Action::Kind::Nothing:
            break;
        case Action::Kind::Builtin:
            o.set("do", info(r.action.builtin).id);
            break;
        case Action::Kind::Keys:
            o.set("keys", winshun::utf8(r.action.text));
            break;
        case Action::Kind::Open:
            o.set("open", winshun::utf8(r.action.text));
            break;
        case Action::Kind::Type:
            o.set("type", winshun::utf8(r.action.target));
            break;
        case Action::Kind::Invalid:
            o.set("do", winshun::utf8(r.action.text)); // kept as written, shown as not understood
            break;
        }
        list.push(o);
    }
    winshun::Json root = winshun::Json::Object {};
    root.set("version", 2);
    root.set("rules", list);
    return root.dump();
}

std::optional<std::vector<Rule>> parseRules(std::string_view json)
{
    const std::optional<winshun::Json> root = winshun::Json::parse(std::string(json));
    if (!root || !root->isObject() || !(*root)["rules"].isArray())
        return std::nullopt;
    std::vector<Rule> rules;
    for (const winshun::Json& o : (*root)["rules"].toArray()) {
        if (!o.isObject())
            continue;
        Rule r;
        r.on = o["on"].toString();
        // A shortcut as written by hand: "key:alt+ctrl+t" is "key:Ctrl+Alt+T".
        if (std::string_view(r.on).starts_with(kKeyPrefix))
            r.on = keyTrigger(winshun::wide(r.on.substr(kKeyPrefix.size())));
        const TriggerKind kind = triggerKind(r.on);
        if (kind == TriggerKind::Unknown)
            continue;
        if (kind == TriggerKind::Gesture)
            r.where = zoneById(o["where"].toString()).value_or(Zone::Anywhere);
        if (kind == TriggerKind::Gesture || kind == TriggerKind::Key)
            r.selection = o["selection"].toBool();
        if (kind == TriggerKind::Gesture || kind == TriggerKind::Wheel || kind == TriggerKind::Key) {
            for (const winshun::Json& app : o["apps"].toArray()) {
                for (std::wstring& name : programList(winshun::wide(app.toString()))) {
                    if (std::ranges::find(r.apps, name) == r.apps.end())
                        r.apps.push_back(std::move(name));
                }
            }
        }
        if (o["do"].isString()) {
            if (const std::optional<Builtin> b = builtinById(o["do"].toString()))
                r.action = builtinAction(*b);
            else
                r.action = {.kind = Action::Kind::Invalid, .text = winshun::wide(o["do"].toString())};
        } else if (o["keys"].isString()) {
            r.action = keysAction(winshun::wide(o["keys"].toString()));
        } else if (o["open"].isString()) {
            r.action = openAction(winshun::wide(o["open"].toString()));
        } else if (o["type"].isString()) {
            r.action = typeAction(winshun::wide(o["type"].toString()));
        }
        // The same case twice: the later one, as it would be saved.
        std::erase_if(rules, [&](const Rule& other) { return other.sameCase(r); });
        rules.push_back(std::move(r));
    }
    return rules;
}

std::vector<Rule> defaultRules()
{
    std::vector<Rule> rules;
    for (const Slot& slot : legacySlots())
        rules.push_back(plainRule(slot.directions, parseAction(slot.fallback)));
    const auto selected = [&](const char* on, Builtin b) {
        Rule r = plainRule(on, builtinAction(b));
        r.selection = true;
        rules.push_back(std::move(r));
    };
    selected("D", Builtin::Copy);
    selected("U", Builtin::WebSearch);
    selected("R", Builtin::Translate);
    rules.push_back(plainRule(std::string(kWheelUp), builtinAction(Builtin::PrevTab)));
    rules.push_back(plainRule(std::string(kWheelDown), builtinAction(Builtin::NextTab)));
    return rules;
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

// ---- 1.0's settings ----

const std::vector<Slot>& legacySlots()
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

std::optional<std::vector<Rule>> migrate(const std::vector<std::pair<std::string, std::wstring>>& slots, std::wstring_view more)
{
    std::vector<Rule> rules = defaultRules();
    bool changed = false;
    for (const Slot& slot : legacySlots()) {
        const auto found = std::ranges::find_if(slots, [&](const auto& s) { return s.first == slot.key; });
        if (found == slots.end() || trimmed(found->second) == slot.fallback)
            continue;
        changed = true;
        setPlain(rules, slot.directions, parseAction(found->second));
    }
    for (const auto& [directions, text] : parseMore(more)) {
        changed = true;
        setPlain(rules, directions, parseAction(text));
    }
    if (!changed)
        return std::nullopt;
    return rules;
}

// ---- the screen's corners and edges ----

Zone zoneAt(int x, int y, const Rect& screen)
{
    const int fx = (screen.right - screen.left) / 5;
    const int fy = (screen.bottom - screen.top) / 5;
    const bool left = x < screen.left + fx;
    const bool right = x >= screen.right - fx;
    const bool top = y < screen.top + fy;
    const bool bottom = y >= screen.bottom - fy;
    if (top && left)
        return Zone::TopLeft;
    if (top && right)
        return Zone::TopRight;
    if (bottom && left)
        return Zone::BottomLeft;
    if (bottom && right)
        return Zone::BottomRight;
    return Zone::Anywhere;
}

bool CornerWatch::move(int x, int y, const Screen& screen, std::int64_t ms)
{
    const Rect& b = screen.bounds;
    // Two pixels: where a pointer pushed into the corner stops (on its last pixel).
    const bool left = screen.hardLeft && x - b.left < 2;
    const bool right = screen.hardRight && b.right - 1 - x < 2;
    const bool top = screen.hardTop && y - b.top < 2;
    const bool bottom = screen.hardBottom && b.bottom - 1 - y < 2;
    int corner = -1;
    if (top && left)
        corner = 0;
    else if (top && right)
        corner = 1;
    else if (bottom && left)
        corner = 2;
    else if (bottom && right)
        corner = 3;

    if (m_spent >= 0) {
        const int rearm = static_cast<int>(std::lround(kRearm * screen.scale));
        if (std::abs(x - m_spentX) > rearm || std::abs(y - m_spentY) > rearm)
            m_spent = -1;
    }
    if (corner < 0 || corner == m_spent) {
        m_corner = -1;
        return false;
    }
    m_cornerX = x;
    m_cornerY = y;
    if (corner == m_corner)
        return false;
    m_corner = corner;
    m_since = ms;
    return true;
}

std::string_view CornerWatch::due(std::int64_t ms)
{
    if (m_corner < 0 || m_corner == m_spent || ms - m_since < kDwellMs)
        return {};
    m_spent = std::exchange(m_corner, -1);
    m_spentX = m_cornerX;
    m_spentY = m_cornerY;
    return kCorners[m_spent];
}

void CornerWatch::reset()
{
    m_corner = -1;
}

std::string_view RubWatch::move(int x, int y, const Screen& screen, std::int64_t ms)
{
    if (ms < m_quietUntil) {
        m_edge = -1;
        return {};
    }
    const Rect& b = screen.bounds;
    const int close = std::max(1, static_cast<int>(std::lround(kNear * screen.scale)));
    const bool on[4] = {
        screen.hardTop && y - b.top < close,
        screen.hardBottom && b.bottom - 1 - y < close,
        screen.hardLeft && x - b.left < close,
        screen.hardRight && b.right - 1 - x < close,
    };
    int edge = m_edge >= 0 && on[m_edge] ? m_edge : -1;
    for (int i = 0; edge < 0 && i < 4; ++i) {
        if (on[i])
            edge = i;
    }
    if (edge < 0) {
        m_edge = -1;
        return {};
    }
    const int along = edge < 2 ? x : y; // top and bottom: across; left and right: up and down
    if (edge != m_edge) {
        m_edge = edge;
        m_direction = 0;
        m_anchor = m_extreme = along;
        m_turns.clear();
        return {};
    }
    const int leg = static_cast<int>(std::lround(kLeg * screen.scale));
    if (m_direction == 0) {
        if (std::abs(along - m_anchor) >= leg) {
            m_direction = along > m_anchor ? 1 : -1;
            m_extreme = along;
        }
        return {};
    }
    if ((along - m_extreme) * m_direction > 0) {
        m_extreme = along;
        return {};
    }
    if (std::abs(along - m_extreme) < leg)
        return {};
    // Back the other way, a whole leg: a turn.
    m_direction = -m_direction;
    m_extreme = along;
    m_turns.push_back(ms);
    std::erase_if(m_turns, [&](std::int64_t t) { return ms - t > kWithinMs; });
    if (static_cast<int>(m_turns.size()) < kTurns)
        return {};
    const std::string_view rubbed = kEdges[edge];
    reset();
    m_quietUntil = ms + kQuietMs;
    return rubbed;
}

void RubWatch::reset()
{
    m_edge = -1;
    m_direction = 0;
    m_turns.clear();
}

// ---- mouse button & chord state machine ----

bool GestureSession::wantsTriggerCheck(MouseButton btn, const std::vector<MouseButton>& enabledButtons, bool activeStillDown) const
{
    if (!hasTriggerButton(enabledButtons, btn))
        return false;
    if (m_state == State::Idle)
        return true;
    if (m_state == State::Through)
        return !activeStillDown || btn == m_activeButton;
    return btn == m_activeButton;
}

SessionResult GestureSession::onButtonDown(MouseButton btn, int x, int y, int startDistance, int stroke, bool eligible,
    const Settings& settings, const Context& context, bool activeStillDown)
{
    SessionResult res;
    if (wantsTriggerCheck(btn, settings.buttons, activeStillDown)) {
        if (m_state != State::Idle && m_state != State::Through) {
            // A release never seen (the hook was late or an elevated window took focus): start over.
            res.stopHoldTimer = true;
            res.cancelBefore = true;
        }
        reset();
        res.resetScreenWatches = true;
        if (!eligible)
            return res;
        m_state = State::Pressed;
        m_activeButton = btn;
        m_downX = x;
        m_downY = y;
        m_startDistance = std::max(startDistance, 1);
        m_wheelDelta = 0;
        m_recognizer.reset(x, y, stroke);
        res.startHoldTimer = true;
        res.swallow = true;
        return res;
    }

    if ((m_state == State::Pressed || m_state == State::Gesturing || m_state == State::Rocked)
        && btn != MouseButton::Any && btn != m_activeButton && m_chordButton == MouseButton::Any) {
        const bool tooLong = m_state == State::Gesturing && m_recognizer.tooLong();
        const std::string strokeStr = (m_state == State::Gesturing && !tooLong) ? m_recognizer.gesture() : std::string();
        Context ctx = context;
        ctx.button = m_activeButton;
        if (!tooLong && chordMatches(settings.rules, strokeStr, btn, ctx)) {
            m_swallowedButtons |= buttonBit(btn);
            res.stopHoldTimer = true;
            res.startBeforeMove = m_state != State::Gesturing;
            m_state = State::Gesturing;
            m_chordButton = btn;
            m_chordStroke = strokeStr;
            res.event = SessionResult::Event::Move;
            res.button = m_activeButton;
            res.gesture = formatGestureTrigger(MouseButton::Any, m_chordStroke, m_chordButton);
            res.swallow = true;
            return res;
        }
        // Unmatched left click while drawing: keep the existing cancel-on-left-click behavior.
        if (btn == MouseButton::Left && (m_state == State::Pressed || m_state == State::Gesturing)) {
            res.stopHoldTimer = true;
            m_state = State::Canceled;
            res.event = SessionResult::Event::Cancel;
            return res;
        }
    }
    return res;
}

SessionResult GestureSession::onButtonUp(MouseButton btn, int x, int y)
{
    SessionResult res;
    if (const std::uint8_t bit = buttonBit(btn); bit && (m_swallowedButtons & bit)) {
        m_swallowedButtons &= static_cast<std::uint8_t>(~bit);
        res.swallow = true;
        if (m_state == State::Gesturing && m_chordButton == btn) {
            res.event = SessionResult::Event::End;
            res.button = m_activeButton;
            res.gesture = formatGestureTrigger(MouseButton::Any, m_chordStroke, m_chordButton);
            m_chordButton = MouseButton::Any;
            m_chordStroke.clear();
            m_state = State::Rocked;
        }
        return res;
    }

    if (btn != MouseButton::Any && btn == m_activeButton) {
        res.stopHoldTimer = true;
        const MouseButton released = std::exchange(m_activeButton, MouseButton::Any);
        const MouseButton chord = std::exchange(m_chordButton, MouseButton::Any);
        const std::string chordStroke = std::exchange(m_chordStroke, {});
        switch (std::exchange(m_state, State::Idle)) {
        case State::Pressed:
            res.event = SessionResult::Event::Click;
            res.button = released;
            res.swallow = true;
            break;
        case State::Gesturing:
            res.event = SessionResult::Event::End;
            res.button = released;
            if (chord != MouseButton::Any) {
                res.gesture = formatGestureTrigger(MouseButton::Any, chordStroke, chord);
            } else {
                m_recognizer.add(x, y);
                res.gesture = m_recognizer.gesture();
            }
            res.swallow = true;
            break;
        case State::Wheeling:
        case State::Rocked:
        case State::Canceled:
            res.swallow = true;
            break;
        case State::Idle:
        case State::Through:
            break;
        }
    }
    return res;
}

SessionResult GestureSession::onMove(int x, int y)
{
    SessionResult res;
    if (m_state == State::Idle) {
        res.watchScreen = true;
        return res;
    }
    if (m_state == State::Pressed) {
        const int dx = x - m_downX;
        const int dy = y - m_downY;
        if (dx * dx + dy * dy < m_startDistance * m_startDistance)
            return res;
        res.stopHoldTimer = true;
        m_state = State::Gesturing;
        res.startBeforeMove = true;
    }
    if (m_state == State::Gesturing) {
        if (m_chordButton != MouseButton::Any)
            return res; // freeze stroke once a secondary chord button is armed
        m_recognizer.add(x, y);
        res.event = SessionResult::Event::Move;
        res.button = m_activeButton;
        res.gesture = m_recognizer.gesture();
    }
    return res;
}

SessionResult GestureSession::onWheel(int delta, bool hasWheelRules, const Settings& settings, const Context& context)
{
    SessionResult res;
    if (m_state != State::Pressed && m_state != State::Gesturing && m_state != State::Wheeling && m_state != State::Rocked)
        return res;
    if (!hasWheelRules || delta == 0 || m_chordButton != MouseButton::Any)
        return res;
    const bool up = delta > 0;
    Context ctx = context;
    ctx.button = m_activeButton;
    const Rule* rule = match(settings.rules, up ? kWheelUp : kWheelDown, ctx);
    if (!rule || rule->action.kind == Action::Kind::Nothing)
        return res;
    if (m_state != State::Wheeling) {
        res.stopHoldTimer = true;
        if (m_state == State::Gesturing)
            res.cancelBefore = true;
        m_state = State::Wheeling;
        m_wheelDelta = 0;
    }
    if (m_wheelDelta != 0 && (m_wheelDelta > 0) != up)
        m_wheelDelta = 0;
    m_wheelDelta += delta;
    int notches = 0;
    while (std::abs(m_wheelDelta) >= WHEEL_DELTA) {
        ++notches;
        m_wheelDelta += up ? -WHEEL_DELTA : WHEEL_DELTA;
    }
    if (notches > 0) {
        res.event = SessionResult::Event::Wheel;
        res.button = m_activeButton;
        res.wheelUp = up;
        res.wheelNotches = notches;
    }
    res.swallow = true;
    return res;
}

SessionResult GestureSession::onHoldTimer()
{
    SessionResult res;
    res.stopHoldTimer = true;
    if (m_state != State::Pressed)
        return res;
    m_state = State::Through;
    res.event = SessionResult::Event::PressThrough;
    res.button = m_activeButton;
    return res;
}

SessionResult GestureSession::onAbort()
{
    SessionResult res;
    if (m_state == State::Pressed || m_state == State::Gesturing) {
        res.stopHoldTimer = true;
        m_chordButton = MouseButton::Any;
        m_chordStroke.clear();
        m_state = State::Canceled;
        res.event = SessionResult::Event::Cancel;
    }
    return res;
}

void GestureSession::reset()
{
    m_state = State::Idle;
    m_activeButton = MouseButton::Any;
    m_chordButton = MouseButton::Any;
    m_chordStroke.clear();
    m_swallowedButtons = 0;
    m_wheelDelta = 0;
}

} // namespace gestures
