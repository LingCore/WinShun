// WinShunExplorerTabs.exe: the winshun.explorer-tabs plugin in the plugin
// library (package/, "资源管理器标签页"; not in the setup). A resident C++
// plugin on the single-header SDK (src/plugins-sdk/winshun.h), started with
// Win顺 and kept running: a folder window that opens while another File
// Explorer window is there becomes a tab of that window instead (as
// ExplorerTabUtility does). The rules are Rules.cpp, the watching and moving
// Engine.cpp. The keyword 标签页 / tabs says whether it is on and pauses it.
//
// Dev-only switches, from runtime.env of a test copy of the manifest (the
// runner gives a plugin the user's default environment, not Win顺's):
//   WINSHUN_TABS_TEST_ROOT  act only on folders under this folder, and only
//                           use windows whose tabs are all under it
//   WINSHUN_TABS_TEST_MARK  a window whose title has this may be hidden
//                           before its folder is known (else only after)
//   WINSHUN_TABS_BYPASS=1   as if Ctrl+Shift were held
//   WINSHUN_TABS_FAIL=1     every navigation of the new tab fails
//   WINSHUN_TABS_TRACE      a file for one timing line per window

#include "winshun.h"

#include "Engine.h"

#include <memory>
#include <string>

using winshun::Item;
using winshun::Items;
using winshun::Json;
using winshun::Query;

namespace {

constexpr const char* kIcon = "glyph:U+E838"; // FolderOpen
constexpr const char* kPauseIcon = "glyph:U+E769"; // Pause
constexpr const char* kPlayIcon = "glyph:U+E768"; // Play

std::wstring g_journal; // for the crash filter
LONG WINAPI onCrash(EXCEPTION_POINTERS*)
{
    // Whatever this process hid is shown again before it goes.
    tabs::Engine::restoreJournal(g_journal);
    return EXCEPTION_CONTINUE_SEARCH;
}

tabs::Settings settingsFrom(const Json& s)
{
    tabs::Settings out;
    if (s["merge"].isBool())
        out.merge = s["merge"].toBool();
    if (s["reuse"].isBool())
        out.reuse = s["reuse"].toBool();
    out.target = s["target"].toString() == "first" ? tabs::Target::First : tabs::Target::Recent;
    return out;
}

class ExplorerTabs : public winshun::Plugin {
protected:
    std::string name() const override { return "winshun.explorer-tabs"; }

    void initialized() override
    {
        tabs::Options o;
        o.settings = settingsFrom(settings());
        o.restriction.root = winshun::wide(winshun::env("WINSHUN_TABS_TEST_ROOT"));
        o.testMark = winshun::wide(winshun::env("WINSHUN_TABS_TEST_MARK"));
        o.forceBypass = winshun::env("WINSHUN_TABS_BYPASS") == "1";
        o.failNavigation = winshun::env("WINSHUN_TABS_FAIL") == "1";
        o.trace = winshun::wide(winshun::env("WINSHUN_TABS_TRACE"));
        if (!dataDir().empty()) {
            ::CreateDirectoryW(winshun::wide(dataDir()).c_str(), nullptr);
            o.journal = winshun::wide(dataDir()) + L"\\hidden.txt";
        }
        o.log = [this](const std::string& text) { log("info", text); };
        if (o.restriction.active())
            log("info", "restricted to " + winshun::utf8(o.restriction.root));
        g_journal = o.journal;
        ::SetUnhandledExceptionFilter(onCrash);
        m_engine = std::make_unique<tabs::Engine>(std::move(o));
        m_engine->start();
    }

    void settingsChanged(const Json& s) override
    {
        if (m_engine)
            m_engine->configure(settingsFrom(s));
    }

    void shuttingDown() override
    {
        if (m_engine)
            m_engine->stop();
    }

    Items query(const Query& q) override
    {
        (void)q;
        const bool zh = chinese();
        const bool on = settingsFrom(settings()).merge;
        const bool paused = m_engine && m_engine->paused();
        const int merged = m_engine ? m_engine->merged() : 0;
        Items items;
        Item status("status", "");
        if (!on) {
            status.title = zh ? "新开的文件夹窗口不并成标签页" : "New folder windows stay windows";
            status.subtitle = zh ? "在设置 → 插件 → 资源管理器标签页里打开" : "Turn it on in Settings → Plugins → Explorer tabs";
            status.icon = kIcon;
            status.valid = false;
        } else if (paused) {
            status.title = zh ? "资源管理器标签页已暂停" : "Explorer tabs paused";
            status.subtitle = zh ? "Enter 恢复：新开的文件夹窗口再并成标签页" : "Enter to resume: new folder windows become tabs again";
            status.icon = kPlayIcon;
        } else {
            status.title = zh ? "新开的文件夹窗口会并成标签页" : "New folder windows become tabs";
            const std::string count = std::to_string(merged);
            status.subtitle = zh ? "这次已并入 " + count + " 个 · 按住 Ctrl+Shift 打开就是新窗口 · Enter 暂停"
                                 : count + " merged so far · hold Ctrl+Shift for a window · Enter to pause";
            status.icon = kPauseIcon;
        }
        items.push_back(status);
        return items;
    }

    winshun::Effects action(const winshun::ActionRequest& a) override
    {
        winshun::Effects effects;
        if (a.item["id"].toString() == "status" && a.action == "default" && m_engine) {
            const bool zh = chinese();
            const bool pause = !m_engine->paused();
            m_engine->setPaused(pause);
            if (pause)
                effects.push_back(winshun::effect::notify(zh ? "已暂停：新开的文件夹窗口照常是窗口，到 Win顺 重启为止"
                                                             : "Paused: folder windows stay windows until Win顺 restarts"));
            else
                effects.push_back(winshun::effect::notify(zh ? "已恢复：新开的文件夹窗口会并成标签页" : "Resumed: new folder windows become tabs"));
        }
        effects.push_back(winshun::effect::hide());
        return effects;
    }

private:
    std::unique_ptr<tabs::Engine> m_engine;
};

} // namespace

int main()
{
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ExplorerTabs plugin;
    const int code = plugin.serve();
    return code;
}
