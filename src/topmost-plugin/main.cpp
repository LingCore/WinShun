// WinShunTopmost.exe: the winshun.topmost plugin in the plugin library
// (package/, "窗口置顶"; not in the setup). A resident C++ plugin on the
// single-header SDK (src/plugins-sdk/winshun.h), started with Win顺 and kept
// running for its shortcut: the window in front goes on top of the others,
// or comes off it again. The keyword 置顶 / top lists the windows to do the
// same from the search box. Pins.cpp is the windows, Note.cpp the word said
// on the window, Border.cpp the line round a window it keeps on top.

#include "winshun.h"

#include "Border.h"
#include "Note.h"
#include "Pins.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using winshun::Item;
using winshun::Items;
using winshun::Json;
using winshun::Query;

namespace {

constexpr const char* kIcon = "ph:push-pin";
constexpr const char* kOffIcon = "ph:push-pin-slash";
constexpr const char* kProblemIcon = "ph:warning";
constexpr std::size_t kTitleMost = 60; // UTF-16 units of a title in a sentence

std::wstring shortened(std::wstring text)
{
    if (text.size() > kTitleMost) {
        text.resize(kTitleMost);
        text += L'…';
    }
    return text;
}

class Topmost : public winshun::Plugin {
protected:
    std::string name() const override { return "winshun.topmost"; }

    void initialized() override
    {
        m_note = std::make_unique<topmost::Note>();
        m_borders = std::make_unique<topmost::Borders>();
        applyLook();
        applyBorder();
        applyHotkey();
    }

    void settingsChanged(const Json&) override
    {
        applyBorder();
        applyHotkey();
    }

    void appearanceChanged() override { applyLook(); }

    // The windows this plugin put on top come off it as it stops (turned off,
    // removed, Win顺 quitting): nothing is left on top with no way to undo it.
    void shuttingDown() override
    {
        for (const HWND hwnd : m_mine)
            topmost::setPinned(hwnd, false);
        m_mine.clear();
        m_borders.reset();
        m_note.reset();
    }

    void hotkey(const std::string&) override
    {
        const HWND hwnd = topmost::current();
        if (!hwnd) {
            say(nullptr, topmost::Tone::Problem, chinese() ? L"前面没有可以置顶的窗口" : L"No window in front to keep on top");
            return;
        }
        toggle(hwnd, !topmost::pinned(hwnd));
    }

    Items query(const Query& q) override
    {
        const bool zh = chinese();
        const std::wstring filter = topmost::lowered(winshun::wide(winshun::trim(q.text)));
        std::vector<topmost::Window> all = topmost::windows();
        // Those on top first, each group front to back.
        std::ranges::stable_partition(all, [](const topmost::Window& w) { return w.pinned; });
        const auto onTop = std::ranges::count_if(all, [](const topmost::Window& w) { return w.pinned; });

        Items items;
        for (const topmost::Window& w : all) {
            if (!topmost::matches(w, filter))
                continue;
            const std::string program = winshun::utf8(w.program);
            Item row("window", winshun::utf8(w.title));
            if (w.pinned)
                row.subtitle = (zh ? "已置顶 · Enter 取消置顶" : "On top · Enter to take it off") + (program.empty() ? "" : " · " + program);
            else
                row.subtitle = (zh ? "Enter 置顶" : "Enter to keep it on top") + (program.empty() ? "" : " · " + program);
            row.icon = w.path.empty() ? std::string(w.pinned ? kIcon : kOffIcon) : "file:" + winshun::utf8(w.path);
            row.data = Json::Object {{"window", std::to_string(reinterpret_cast<std::uintptr_t>(w.hwnd))},
                {"pid", static_cast<unsigned long long>(w.pid)}, {"on", !w.pinned}};
            items.push_back(row);
        }
        if (onTop > 1 && filter.empty()) {
            Item none("none", zh ? "全部取消置顶" : "Take every window off the top",
                zh ? std::to_string(onTop) + " 个窗口置顶着" : std::to_string(onTop) + " windows are on top");
            none.icon = kOffIcon;
            items.push_back(none);
        }
        if (items.empty()) {
            Item nothing("nothing", all.empty() ? (zh ? "没有打开的窗口" : "No open windows") : (zh ? "没有这样的窗口" : "No such window"));
            nothing.icon = kProblemIcon;
            nothing.valid = false;
            items.push_back(nothing);
        }
        return items;
    }

    winshun::Effects action(const winshun::ActionRequest& a) override
    {
        const std::string id = a.item["id"].toString();
        if (id == "none") {
            for (const topmost::Window& w : topmost::windows()) {
                if (w.pinned)
                    topmost::setPinned(w.hwnd, false);
            }
            m_mine.clear();
            if (m_borders)
                m_borders->clear();
            say(nullptr, topmost::Tone::Off, chinese() ? L"已全部取消置顶" : L"No window is on top now");
        } else if (id == "window") {
            const Json& data = a.item["data"];
            const HWND hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(std::strtoull(data["window"].toString().c_str(), nullptr, 10)));
            DWORD pid = 0;
            ::GetWindowThreadProcessId(hwnd, &pid);
            // The same window still: a handle can be another window's by now.
            if (!hwnd || !::IsWindow(hwnd) || pid != static_cast<DWORD>(data["pid"].toInt()))
                say(nullptr, topmost::Tone::Problem, chinese() ? L"这个窗口已经关了" : L"That window has closed");
            else {
                const bool on = data["on"].toBool();
                // Put on top from the list, it is brought to the front too:
                // that is why it was chosen.
                if (toggle(hwnd, on) && on) {
                    if (::IsIconic(hwnd))
                        ::ShowWindowAsync(hwnd, SW_RESTORE);
                    ::SetForegroundWindow(hwnd);
                }
            }
        }
        return {winshun::effect::hide()};
    }

private:
    topmost::Look look() const
    {
        topmost::Look l;
        l.dark = theme() == "dark";
        l.rounded = appearance()["corners"].toString() != "square";
        const Json& native = appearance()["nativeLook"];
        if (!native.isObject())
            return l;
        if (native["rounded"].isBool())
            l.rounded = native["rounded"].toBool();
        if (native["dark"].isBool() && native["dark"].toBool() != l.dark)
            return l; // written for the other theme, a moment before it changes
        l.accent = topmost::argbOf(native["accent"].toString());
        l.accentFill = topmost::argbOf(native["accentFill"].toString());
        l.dangerFill = topmost::argbOf(native["dangerFill"].toString());
        l.background = topmost::argbOf(native["background"].toString());
        l.edge = topmost::argbOf(native["windowEdge"].toString());
        l.text = topmost::argbOf(native["text"].toString());
        return l;
    }

    void applyLook()
    {
        const topmost::Look l = look();
        if (m_note)
            m_note->setLook(l);
        // The accent that reads on the theme's own windows: a line, not a fill.
        if (m_borders)
            m_borders->setColour(l.accent ? (l.accent | 0xFF000000u) : l.dark ? 0xFF4CC2FF : 0xFF0F6CBD);
    }

    // The line round the windows this plugin keeps on top, by its setting
    // (on unless turned off).
    void applyBorder()
    {
        const Json& setting = settings()["border"];
        const bool on = !setting.isBool() || setting.toBool();
        if (on == m_border || !m_borders)
            return;
        m_border = on;
        if (!on) {
            m_borders->clear();
            return;
        }
        for (const HWND hwnd : m_mine)
            m_borders->add(hwnd); // one no longer on top gets none
    }

    void say(HWND window, topmost::Tone tone, std::wstring text)
    {
        if (m_note)
            m_note->show(window, tone, std::move(text));
    }

    // True when the window is as asked now. What could not be done is said
    // on the window, with the reason.
    bool toggle(HWND hwnd, bool on)
    {
        const bool zh = chinese();
        const std::wstring title = shortened(topmost::describe(hwnd).title);
        switch (topmost::setPinned(hwnd, on)) {
        case topmost::Result::Done:
            // Without this one, and without those that closed or were taken
            // off by another program meanwhile.
            std::erase_if(m_mine, [hwnd](HWND mine) { return mine == hwnd || !::IsWindow(mine) || !topmost::pinned(mine); });
            if (on)
                m_mine.push_back(hwnd);
            if (m_borders && on && m_border)
                m_borders->add(hwnd);
            else if (m_borders)
                m_borders->remove(hwnd);
            say(hwnd, on ? topmost::Tone::On : topmost::Tone::Off, on ? (zh ? L"已置顶" : L"On top") : (zh ? L"已取消置顶" : L"No longer on top"));
            return true;
        case topmost::Result::Gone:
            say(nullptr, topmost::Tone::Problem, zh ? L"这个窗口已经关了" : L"That window has closed");
            return false;
        case topmost::Result::Hung:
            say(hwnd, topmost::Tone::Problem, zh ? L"这个程序没有响应，没能改" : L"The program is not responding; nothing changed");
            log("warning", "the window's program does not answer");
            return false;
        case topmost::Result::Refused:
            say(hwnd, topmost::Tone::Problem,
                zh ? L"以管理员身份运行的窗口，Windows 不让改它的置顶" : L"A window run as administrator: Windows will not let it be changed");
            log("warning", "Windows refused the change (a window of higher integrity)");
            return false;
        }
        return false;
    }

    // The one shortcut, held by Win顺 for the plugin. Not had: said in the
    // log and, once per key, on screen.
    void applyHotkey()
    {
        const std::string key = winshun::trim(settings()["hotkey"].toString());
        if (key == m_hotkey)
            return;
        m_hotkey = key;
        const auto refused = registerHotkeys(key.empty() ? std::vector<std::string> {} : std::vector<std::string> {key});
        if (const auto found = refused.find(key); !key.empty() && found != refused.end()) {
            log("warning", "the shortcut " + key + " could not be registered: " + std::to_string(found->second));
            const std::wstring wide = winshun::wide(key);
            if (found->second == ERROR_NOT_SUPPORTED) // a Win顺 from before plugins' shortcuts (minWinShun keeps it from being installed there)
                say(nullptr, topmost::Tone::Problem,
                    chinese() ? L"这个版本的 Win顺 还不能替插件注册快捷键，更新后再用" : L"This WinShun cannot hold a plugin's shortcut yet; update it first");
            else
                say(nullptr, topmost::Tone::Problem,
                    chinese() ? L"窗口置顶的快捷键 " + wide + L" 用不了（多半被别的程序占着），到插件设置里换一个"
                              : L"The shortcut " + wide + L" could not be had (another program may hold it); choose another in the plugin's settings");
        }
    }

    std::unique_ptr<topmost::Note> m_note;
    std::unique_ptr<topmost::Borders> m_borders;
    bool m_border = false; // lines are drawn; set from the setting
    std::vector<HWND> m_mine; // put on top by this plugin, still on top as far as it knows
    std::string m_hotkey = "\x01"; // none asked for yet
};

} // namespace

int main()
{
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Topmost plugin;
    return plugin.serve();
}
