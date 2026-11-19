// WinShunGestures.exe: the winshun.gestures plugin in the plugin library
// (package/, "鼠标手势"; not in the setup). A resident C++ plugin on the
// single-header SDK (src/plugins-sdk/winshun.h), started with Win顺 and
// kept running: hold the right button and draw, push the pointer into a
// corner of the screen or rub it along an edge, and its rule does what it
// says (Gesture.h holds the rules, Engine.cpp hooks the mouse and draws;
// docs/design-gestures.md). The keyword 手势 / gestures lists them and
// pauses them.

#include "winshun.h"

#include "EditorHost.h"
#include "EditorStub.h"
#include "Engine.h"

#include <algorithm>
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

std::optional<std::uint32_t> argb(const Json& value)
{
    const std::string s = value.toString();
    if (s.size() != 9 || s[0] != '#')
        return std::nullopt;
    try {
        return static_cast<std::uint32_t>(std::stoul(s.substr(1), nullptr, 16));
    } catch (...) {
        return std::nullopt;
    }
}

// `top` laid over the opaque `under`: what it looks like there.
std::uint32_t over(std::uint32_t top, std::uint32_t under)
{
    const std::uint32_t a = top >> 24;
    if (a == 0xff)
        return top;
    std::uint32_t out = 0xff000000;
    for (const int shift : {0, 8, 16}) {
        const std::uint32_t t = (top >> shift) & 0xff;
        const std::uint32_t u = (under >> shift) & 0xff;
        out |= ((t * a + u * (255 - a) + 127) / 255) << shift;
    }
    return out;
}

bool light(std::uint32_t c)
{
    const double r = (c >> 16) & 0xff;
    const double g = (c >> 8) & 0xff;
    const double b = c & 0xff;
    return 0.299 * r + 0.587 * g + 0.114 * b > 150;
}

// 1.0's seven gesture settings, by their keys.
std::vector<std::pair<std::string, std::wstring>> oldSlots(const Json& s)
{
    std::vector<std::pair<std::string, std::wstring>> slots;
    for (const gestures::Slot& slot : gestures::legacySlots()) {
        if (s[slot.key].isString())
            slots.emplace_back(slot.key, winshun::wide(s[slot.key].toString()));
    }
    return slots;
}

// The rules: the setting "rules" once written; before that 1.0's seven
// slots and "more" when they were changed (main() keeps them as rules once,
// Gestures::keepOldSettings); else the defaults.
std::vector<gestures::Rule> rulesFrom(const Json& s)
{
    if (const std::string text = s["rules"].toString(); !text.empty()) {
        if (auto rules = gestures::parseRules(text))
            return std::move(*rules);
    }
    if (auto rules = gestures::migrate(oldSlots(s), winshun::wide(s["more"].toString())))
        return std::move(*rules);
    return gestures::defaultRules();
}

gestures::Settings settingsFrom(const Json& s)
{
    gestures::Settings out;
    out.rules = rulesFrom(s);
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
        keepOldSettings(settings());
        gestures::EngineOptions o;
        o.log = [this](const std::string& text) { log("info", text); };
        o.showSearch = [this] { showSearch(); };
        o.open = [this](const std::wstring& target) { open(target); };
        o.findInWinShun = [this](const std::wstring& words) { askHost("show_launcher", Json::Object {{"text", winshun::utf8(words)}}); };
        o.webSearch = [this](const std::wstring& words) { askHost("web_search", Json::Object {{"text", winshun::utf8(words)}}); };
        o.translate = [this](const std::wstring& words, std::function<void(gestures::Translation)> done) {
            std::thread([this, text = winshun::utf8(words), done = std::move(done)] { done(translate(text)); }).detach();
        };
        o.translateSetup = [this] { askHost("translate_text", Json::Object {{"setup", true}}); };
        o.copy = [this](const std::wstring& words) {
            std::thread([this, text = winshun::utf8(words)] {
                const Json answer = tool("clipboard_copy", Json::Object {{"text", text}}, &m_stopping);
                if (answer.has("error"))
                    log("warning", "clipboard_copy: " + answer["error"]["message"].toString());
            }).detach();
        };
        o.selectedText = [this](HWND root) -> std::wstring {
            const Json answer = tool("selected_text", Json::Object {{"window", static_cast<double>(reinterpret_cast<std::uintptr_t>(root))}}, &m_stopping);
            if (answer.has("error")) {
                log("warning", "selected_text: " + answer["error"]["message"].toString());
                return {};
            }
            return winshun::wide(answer["text"].toString());
        };
        o.power = [this](const std::string& what) -> std::wstring {
            const Json answer = tool("power", Json::Object {{"action", what}}, &m_stopping);
            if (!answer.has("error"))
                return {};
            const std::string message = answer["error"]["message"].toString();
            log("warning", "power " + what + ": " + message);
            return winshun::wide(message.empty() ? "?" : message);
        };
        m_engine = std::make_unique<gestures::Engine>(std::move(o));
        m_engine->configure(settingsFrom(settings()), look());
        if (!dataDir().empty()) {
            ::CreateDirectoryW(winshun::wide(dataDir()).c_str(), nullptr);
            m_pauseFile = winshun::wide(dataDir()) + L"\\paused";
            if (::GetFileAttributesW(m_pauseFile.c_str()) != INVALID_FILE_ATTRIBUTES)
                m_engine->setPaused(true);
        }
        m_engine->start();

        // The settings window (设置 on the card): its own process, started when wanted (EditorHost.h).
        gestures::EditorHost::Hooks hooks;
        hooks.log = [this](const std::string& text) { log("info", text); };
        // What it changed (on its reader thread): kept by Win顺, which sends the merged settings back.
        hooks.save = [this](Json values) { updateSettings(std::move(values)); };
        hooks.pause = [this](bool paused) { setPaused(paused); };
        // The engine leaves its windows alone: a right drag there is for drawing the gesture it records.
        hooks.own = [this](DWORD pid) {
            if (m_engine)
                m_engine->setOwnWindowsOf(pid);
        };
        m_editor = std::make_unique<gestures::EditorHost>(std::move(hooks));
    }

    void settingsChanged(const Json& s) override
    {
        if (m_engine)
            m_engine->configure(settingsFrom(s), look());
        if (m_editor)
            m_editor->update(editorState(s));
    }

    void appearanceChanged() override
    {
        if (m_engine)
            m_engine->configure(settingsFrom(settings()), look());
        if (m_editor)
            m_editor->appearance(appearance());
    }

    void showSettings() override
    {
        if (m_editor && !m_editor->show(editorState(settings())))
            notifyUser(chinese() ? "没能打开鼠标手势的设置窗口" : "The mouse gestures settings window could not be opened");
    }

    void shuttingDown() override
    {
        m_stopping = true;
        if (m_editor)
            m_editor->close(); // before the engine goes: it is told to leave the window's process alone
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
            status.subtitle = zh ? "Enter 恢复" : "Enter to resume";
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
        int n = 0;
        for (const gestures::Rule& rule : s.rules) {
            if (rule.action.kind != gestures::Action::Kind::Nothing)
                items.push_back(row(rule, n++, zh));
        }
        return items;
    }

    winshun::Effects action(const winshun::ActionRequest& a) override
    {
        winshun::Effects effects;
        if (a.item["id"].toString() == "status" && a.action == "default" && m_engine) {
            const bool zh = chinese();
            const bool pause = !m_engine->paused();
            setPaused(pause);
            effects.push_back(winshun::effect::notify(pause ? (zh ? "鼠标手势已暂停，右键照常" : "Mouse gestures paused: the right button is plain again")
                                                            : (zh ? "鼠标手势已恢复" : "Mouse gestures resumed")));
        }
        effects.push_back(winshun::effect::hide());
        return effects;
    }

private:
    // 暂停鼠标手势 on or off, from the launcher's row or the settings window (its reader thread):
    // the engine, the file that keeps it across a restart, and the window that shows it.
    void setPaused(bool pause)
    {
        if (!m_engine)
            return;
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
        if (m_editor)
            m_editor->pausedChanged(pause);
    }

    // What the settings window is given: the rules that hold (the setting's, or 1.0's, or the
    // defaults), the other options and the look. Read on the serve thread.
    Json editorState(const Json& s) const
    {
        const gestures::Settings g = settingsFrom(s);
        Json state = Json::Object {};
        state.set("rules", gestures::rulesJson(g.rules));
        state.set("exclude", s["exclude"].toString());
        state.set("trail", g.trail);
        state.set("fullScreen", g.fullScreen);
        state.set("paused", m_engine && m_engine->paused());
        state.set("appearance", appearance());
        return state;
    }

    // 1.0's gestures, changed from what they came as: kept as the rules
    // once (2.0 reads only those), the old settings put back as they came so
    // that nothing reads them again.
    void keepOldSettings(const Json& s)
    {
        if (!s["rules"].toString().empty())
            return;
        const auto rules = gestures::migrate(oldSlots(s), winshun::wide(s["more"].toString()));
        if (!rules)
            return;
        Json values = Json::Object {{"rules", gestures::rulesJson(*rules)}, {"more", ""}};
        for (const gestures::Slot& slot : gestures::legacySlots())
            values.set(slot.key, winshun::utf8(slot.fallback));
        updateSettings(std::move(values));
        log("info", "1.0's gestures kept as rules");
    }

    gestures::Look look() const
    {
        gestures::Look l;
        l.chinese = chinese();
        const Json& native = appearance()["nativeLook"];
        if (!native.isObject())
            return l;
        if (native["dark"].isBool())
            l.dark = native["dark"].toBool();
        else
            l.dark = theme() == "dark";
        if (native["rounded"].isBool())
            l.rounded = native["rounded"].toBool();
        const auto pick = [&](const char* key, std::uint32_t& into) {
            if (const auto c = argb(native[key]))
                into = *c;
        };
        pick("background", l.background);
        l.background |= 0xff000000; // a card is opaque: nothing behind it to blur
        pick("accent", l.accent);
        pick("accentFill", l.accentFill);
        l.accentFill = over(l.accentFill, l.background);
        pick("accentText", l.onAccent);
        pick("text", l.text);
        pick("subtext", l.subtext);
        if (!argb(native["windowEdge"]))
            pick("menuEdge", l.edge);
        else
            pick("windowEdge", l.edge);
        pick("control", l.control);
        l.control = over(l.control, l.background);
        pick("controlHover", l.controlHover);
        l.controlHover = over(l.controlHover, l.background);
        pick("controlBorder", l.controlBorder);
        // The hint of a gesture that does nothing: grey, the words as on the accent.
        l.muted = light(l.onAccent) ? 0xf0505050 : 0xf0b4b4b4;
        if (native["cardRadius"].isNumber())
            l.cardRadius = static_cast<int>(native["cardRadius"].toInt());
        if (native["controlRadius"].isNumber())
            l.controlRadius = static_cast<int>(native["controlRadius"].toInt());
        return l;
    }

    Item row(const gestures::Rule& rule, int n, bool zh) const
    {
        std::wstring title = gestures::triggerTitle(rule.on, zh);
        std::wstring when;
        const auto add = [&](const std::wstring& part) { when += (when.empty() ? L"" : L" · ") + part; };
        if (rule.where != gestures::Zone::Anywhere)
            add(std::wstring(zh ? L"从" : L"started in the ") + gestures::zoneTitle(rule.where, zh) + (zh ? L"画" : L""));
        if (rule.selection)
            add(zh ? L"选中文字时" : L"with words selected");
        if (!rule.apps.empty()) {
            std::wstring apps;
            for (const std::wstring& app : rule.apps)
                apps += (apps.empty() ? L"" : L", ") + app;
            add((zh ? L"在 " : L"in ") + apps + (zh ? L" 里" : L""));
        }
        Item item("r" + std::to_string(n), winshun::utf8(title + L"  " + gestures::actionTitle(rule.action, zh)));
        item.subtitle = winshun::utf8(when);
        if (rule.action.kind == gestures::Action::Kind::Invalid)
            item.subtitle = zh ? "这项设置看不懂，请在设置里重新选一个动作" : "This rule is not understood: choose its action again in the settings";
        item.icon = kIcon;
        item.valid = false;
        return item;
    }

    // A tool of Win顺's that answers at once; on a thread of its own so the
    // engine's window thread does not wait for it.
    void askHost(std::string name, Json arguments)
    {
        std::thread([this, name = std::move(name), arguments = std::move(arguments)]() mutable {
            const Json answer = tool(name, std::move(arguments), &m_stopping);
            if (answer.has("error"))
                log("warning", name + ": " + answer["error"]["message"].toString());
        }).detach();
    }

    gestures::Translation translate(const std::string& text)
    {
        gestures::Translation t;
        const Json answer = tool("translate_text", Json::Object {{"text", text}}, &m_stopping);
        if (answer.has("error")) {
            t.text = winshun::wide(answer["error"]["message"].toString());
            return t;
        }
        const std::string state = answer["state"].toString();
        if (state == "ok") {
            t.state = gestures::Translation::State::Ok;
            t.text = winshun::wide(answer["text"].toString());
        } else if (state == "service") {
            t.state = gestures::Translation::State::NoService;
        } else if (state == "plugin") {
            t.state = gestures::Translation::State::NoPlugin;
        } else {
            t.text = winshun::wide(answer["message"].toString());
        }
        return t;
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
        askHost("show_launcher", Json::Object {{"text", ""}});
    }

    // A program, shortcut, file, folder or web address, opened by Win顺 (with
    // the user's rights, outside this plugin's job).
    void open(const std::wstring& target)
    {
        std::thread([this, t = winshun::utf8(target)] {
            const bool web = t.rfind("http://", 0) == 0 || t.rfind("https://", 0) == 0;
            const Json answer = web ? tool("open_url", Json::Object {{"url", t}}, &m_stopping)
                                    : tool("open_item", Json::Object {{"path", t}}, &m_stopping);
            if (answer.has("error")) {
                log("warning", "open " + t + ": " + answer["error"]["message"].toString());
                notifyUser((chinese() ? "鼠标手势没能打开 " : "The gesture could not open ") + t);
            }
        }).detach();
    }

    std::unique_ptr<gestures::Engine> m_engine;
    std::unique_ptr<gestures::EditorHost> m_editor; // after the engine: gone before it
    std::wstring m_pauseFile; // <dataDir>\paused: there while paused (survives a restart)
    std::atomic_bool m_stopping {false};
};

} // namespace

int main()
{
    // The settings window's start (EditorStub.cpp), none of the plugin's: before anything is set up here.
    if (gestures::editorRequested())
        return gestures::runEditor();
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Gestures plugin;
    return plugin.serve();
}
