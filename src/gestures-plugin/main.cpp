// WinShunGestures.exe: the winshun.gestures plugin in the plugin library
// (package/, "鼠标手势"; not in the setup). A resident C++ plugin on the
// single-header SDK (src/plugins-sdk/winshun.h), started with Win顺 and
// kept running: hold the right button and draw, and the gesture does what
// its setting says (Gesture.h reads them, Engine.cpp hooks the mouse and
// draws). The keyword 手势 / gestures lists them and pauses them.

#include "winshun.h"

#include "Engine.h"

#include <atomic>
#include <memory>
#include <string>

using winshun::Item;
using winshun::Items;
using winshun::Json;
using winshun::Query;

namespace {

constexpr const char* kIcon = "ph:mouse-right-click";
constexpr const char* kPauseIcon = "ph:pause";
constexpr const char* kPlayIcon = "ph:play";

constexpr wchar_t kSearchEvent[] = L"Local\\WinShun.TaskbarSearch"; // src/stub/main.cpp
constexpr wchar_t kMessageWindow[] = L"WinShun.MessageWindow";

std::uint32_t argb(const Json& value, std::uint32_t fallback)
{
    const std::string s = value.toString();
    if (s.size() != 9 || s[0] != '#')
        return fallback;
    try {
        return static_cast<std::uint32_t>(std::stoul(s.substr(1), nullptr, 16));
    } catch (...) {
        return fallback;
    }
}

gestures::Settings settingsFrom(const Json& s)
{
    gestures::Settings out;
    for (const gestures::Slot& slot : gestures::defaultSlots()) {
        const Json& value = s[slot.key];
        const std::wstring text = value.isString() ? winshun::wide(value.toString()) : slot.fallback;
        gestures::Action action = gestures::parseAction(text);
        if (action.kind != gestures::Action::Kind::Nothing)
            out.gestures[slot.directions] = std::move(action);
    }
    for (const auto& [directions, text] : gestures::parseMore(winshun::wide(s["more"].toString()))) {
        gestures::Action action = gestures::parseAction(text);
        if (action.kind != gestures::Action::Kind::Nothing)
            out.gestures[directions] = std::move(action);
    }
    out.excluded = gestures::programList(winshun::wide(s["exclude"].toString()));
    if (s["trail"].isBool())
        out.trail = s["trail"].toBool();
    if (s["fullScreen"].isBool())
        out.fullScreen = s["fullScreen"].toBool();
    return out;
}

class Gestures : public winshun::Plugin {
protected:
    std::string name() const override { return "winshun.gestures"; }

    void initialized() override
    {
        gestures::EngineOptions o;
        o.log = [this](const std::string& text) { log("info", text); };
        o.showSearch = [this] { showSearch(); };
        o.open = [this](const std::wstring& target) { open(target); };
        m_engine = std::make_unique<gestures::Engine>(std::move(o));
        m_engine->configure(settingsFrom(settings()), look());
        if (!dataDir().empty()) {
            ::CreateDirectoryW(winshun::wide(dataDir()).c_str(), nullptr);
            m_pauseFile = winshun::wide(dataDir()) + L"\\paused";
            if (::GetFileAttributesW(m_pauseFile.c_str()) != INVALID_FILE_ATTRIBUTES)
                m_engine->setPaused(true);
        }
        m_engine->start();
    }

    void settingsChanged(const Json& s) override
    {
        if (m_engine)
            m_engine->configure(settingsFrom(s), look());
    }

    void appearanceChanged() override
    {
        if (m_engine)
            m_engine->configure(settingsFrom(settings()), look());
    }

    void shuttingDown() override
    {
        m_stopping = true;
        if (m_engine)
            m_engine->stop();
    }

    Items query(const Query& q) override
    {
        (void)q;
        const bool zh = chinese();
        const bool paused = m_engine && m_engine->paused();
        Items items;
        Item status("status", "");
        if (paused) {
            status.title = zh ? "鼠标手势已暂停" : "Mouse gestures paused";
            status.subtitle = zh ? "Enter 恢复：按住右键画出手势" : "Enter to resume: hold the right button and draw";
            status.icon = kPlayIcon;
        } else {
            status.title = zh ? "按住右键画出手势" : "Hold the right button and draw";
            const std::string count = std::to_string(m_engine ? m_engine->performed() : 0);
            status.subtitle = zh ? "Win顺 启动后用了 " + count + " 次 · 右键按住停一下再拖，就是平常的右键拖动 · Enter 暂停"
                                 : "Used " + count + " time(s) since WinShun started · hold still, then drag, for a plain right drag · Enter to pause";
            status.icon = kPauseIcon;
        }
        items.push_back(status);
        const gestures::Settings s = settingsFrom(settings());
        for (const gestures::Slot& slot : gestures::defaultSlots()) {
            if (const auto found = s.gestures.find(slot.directions); found != s.gestures.end())
                items.push_back(row(slot.directions, found->second, zh));
        }
        for (const auto& [directions, action] : s.gestures) {
            const bool slotted = std::ranges::any_of(gestures::defaultSlots(), [&](const gestures::Slot& slot) { return directions == slot.directions; });
            if (!slotted)
                items.push_back(row(directions, action, zh));
        }
        return items;
    }

    winshun::Effects action(const winshun::ActionRequest& a) override
    {
        winshun::Effects effects;
        if (a.item["id"].toString() == "status" && a.action == "default" && m_engine) {
            const bool zh = chinese();
            const bool pause = !m_engine->paused();
            m_engine->setPaused(pause);
            if (!m_pauseFile.empty()) {
                if (pause) {
                    if (const HANDLE f = ::CreateFileW(m_pauseFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        f != INVALID_HANDLE_VALUE)
                        ::CloseHandle(f);
                } else {
                    ::DeleteFileW(m_pauseFile.c_str());
                }
            }
            effects.push_back(winshun::effect::notify(pause ? (zh ? "鼠标手势已暂停，右键照常" : "Mouse gestures paused: the right button is plain again")
                                                            : (zh ? "鼠标手势已恢复" : "Mouse gestures resumed")));
        }
        effects.push_back(winshun::effect::hide());
        return effects;
    }

private:
    gestures::Look look() const
    {
        gestures::Look l;
        l.chinese = chinese();
        const Json& native = appearance()["nativeLook"];
        l.accent = argb(native["accentFill"], l.accent);
        return l;
    }

    Item row(const std::string& directions, const gestures::Action& action, bool zh) const
    {
        Item item("g-" + directions, gestures::arrows(directions) + "  " + winshun::utf8(gestures::actionTitle(action, zh)));
        if (action.kind == gestures::Action::Kind::Invalid)
            item.subtitle = zh ? "这项设置看不懂：填动作名、按键（如 Ctrl+W）或要打开的程序" : "This setting is not understood: a name, keys (such as Ctrl+W) or something to open";
        item.icon = kIcon;
        item.valid = false;
        return item;
    }

    // Win顺's search box, shown or hidden as its taskbar button does (the
    // event it waits on); the host's show_launcher when that is not there.
    void showSearch()
    {
        if (const HANDLE event = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, kSearchEvent)) {
            DWORD pid = 0;
            if (const HWND window = ::FindWindowW(kMessageWindow, nullptr))
                ::GetWindowThreadProcessId(window, &pid);
            ::AllowSetForegroundWindow(pid ? pid : ASFW_ANY);
            const bool set = ::SetEvent(event);
            ::CloseHandle(event);
            if (set)
                return;
        }
        const Json answer = tool("show_launcher", Json::Object {{"text", ""}}, &m_stopping);
        if (answer.has("error"))
            log("warning", "show_launcher: " + answer["error"]["message"].toString());
    }

    // A program, shortcut, file, folder or web address, opened by Win顺 (with
    // the user's rights, outside this plugin's job).
    void open(const std::wstring& target)
    {
        const std::string t = winshun::utf8(target);
        const bool web = t.rfind("http://", 0) == 0 || t.rfind("https://", 0) == 0;
        const Json answer = web ? tool("open_url", Json::Object {{"url", t}}, &m_stopping)
                                : tool("open_item", Json::Object {{"path", t}}, &m_stopping);
        if (answer.has("error")) {
            const std::string message = answer["error"]["message"].toString();
            log("warning", "open " + t + ": " + message);
            notifyUser((chinese() ? "鼠标手势没能打开 " : "The gesture could not open ") + t);
        }
    }

    std::unique_ptr<gestures::Engine> m_engine;
    std::wstring m_pauseFile; // <dataDir>\paused: there while paused (survives a restart)
    std::atomic_bool m_stopping {false};
};

} // namespace

int main()
{
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Gestures plugin;
    return plugin.serve();
}
