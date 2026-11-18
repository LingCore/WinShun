// The rules of the winshun.gestures plugin ("鼠标手势"), as plain C++ (no
// windows: only the VK_ names from windows.h) so tests/tst_gestures.cpp can
// drive them: the directions a drag with the right button draws, what the
// user wrote for each gesture, and the programs left alone.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace gestures {

// A gesture is its directions in order, one letter each: L R U D ("DR" =
// down, then right). Longer than this, it is no gesture.
constexpr std::size_t kMaxDirections = 6;

// Directions from the points of a drag (screen pixels). A direction counts
// once the pointer has gone `stroke` pixels along it from where the last
// one was confirmed, clearly more along one axis than the other; the same
// direction again only moves that point on, a slant counts for nothing.
class Recognizer {
public:
    void reset(int x, int y, int stroke);
    // True when the directions changed.
    bool add(int x, int y);
    const std::string& directions() const { return m_directions; }
    bool tooLong() const { return m_directions.size() > kMaxDirections; }

private:
    int m_x = 0;
    int m_y = 0;
    int m_stroke = 20;
    std::string m_directions;
};

// "DR" -> "↓→" (UTF-8), for the hint and the settings.
std::string arrows(std::string_view directions);
// What the user wrote for a gesture's directions: arrows (↓→), letters
// (DR, dr) or 上下左右; spaces and dashes between are fine. Empty when it
// is none of these or too long.
std::string parseDirections(std::wstring_view text);

enum class Builtin {
    Back,
    Forward,
    Top,
    Bottom,
    Refresh,
    Close,
    CloseTab,
    Minimize,
    Maximize, // or back from maximized
    Search, // Win顺's search box
};

struct Action {
    enum class Kind {
        Nothing, // left empty: the gesture does nothing
        Builtin,
        Keys, // pressed in order, let go in reverse
        Open, // a program, shortcut, file, folder or web address
        Invalid, // none of these: shown as such in the hint
    };
    Kind kind = Kind::Nothing;
    Builtin builtin = Builtin::Back;
    std::vector<std::uint16_t> keys; // virtual keys, modifiers first
    std::wstring target; // Open: the path or address, quotes taken off
    std::wstring text; // as written, trimmed
};

// One gesture's action, as written in its setting:
// - a name: 后退 前进 回到顶部 到底部 刷新 关闭窗口 关闭标签页 最小化 最大化
//   搜索框 (or back forward top bottom refresh close closetab minimize
//   maximize search);
// - keys joined by +: Ctrl+W, Alt+Left, Ctrl+Shift+T, F5, Win+D;
// - anything with a \ / or : in it, or a name ending in .exe .lnk .bat .cmd:
//   opened, as a double click would.
Action parseAction(std::wstring_view text);

// Its title in the hint: the builtin's name in the language, the keys or the
// target as written. Empty for Nothing.
std::wstring actionTitle(const Action& action, bool chinese);

// "↑←=Ctrl+Shift+T; DU = notepad.exe" -> {"UL": Ctrl+Shift+T, "DU": …}:
// gestures of the "more" setting, separated by ; or line breaks, each
// directions = action. Pairs that do not read are left out.
std::vector<std::pair<std::string, std::wstring>> parseMore(std::wstring_view text);

// "a.exe; B.EXE, c" -> {"a.exe", "b.exe", "c.exe"}: separated by ; , or
// line breaks, trimmed, lower case (ASCII), ".exe" added when there is no
// dot; a path counts by its file name.
std::vector<std::wstring> programList(std::wstring_view text);

// Whether `exe` (a file name or a path) is in the list.
bool listed(const std::vector<std::wstring>& list, std::wstring_view exe);

struct Settings {
    std::map<std::string, Action> gestures; // by directions; Nothing left out
    std::vector<std::wstring> excluded; // programs whose windows keep the right button
    bool trail = true; // draw the trail and the hint
    bool fullScreen = false; // gestures over a full-screen window too
};

// The default gestures' setting keys and directions, in the order shown.
struct Slot {
    const char* key;
    const char* directions;
    const wchar_t* fallback; // the default action
};
const std::vector<Slot>& defaultSlots();

} // namespace gestures
