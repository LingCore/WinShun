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
//   WINSHUN_TABS_LOGON=1    this start counts as the first after a logon
//                           (the tabs kept from before are opened again)

#include "winshun.h"

#include "Engine.h"
#include "Keeper.h"

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
constexpr const char* kRestoreIcon = "glyph:U+E81C"; // History

// The Windows build (RtlGetVersion does not lie to a program without a
// manifest), 0 when it cannot be read.
unsigned windowsBuild()
{
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    const auto get = ntdll ? reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
    OSVERSIONINFOW v {sizeof v};
    return get && get(&v) == 0 ? v.dwBuildNumber : 0;
}


tabs::Settings settingsFrom(const Json& s)
{
    tabs::Settings out;
    if (s["merge"].isBool())
        out.merge = s["merge"].toBool();
    if (s["reuse"].isBool())
        out.reuse = s["reuse"].toBool();
    out.target = s["target"].toString() == "first" ? tabs::Target::First : tabs::Target::Recent;
    out.windowsFor = tabs::programList(winshun::wide(s["windowsFor"].toString()));
    return out;
}

class ExplorerTabs : public winshun::Plugin {
protected:
    std::string name() const override { return "winshun.explorer-tabs"; }

    void initialized() override
    {
        // Explorer has no tabs before Windows 11 22H2: the new-tab command does
        // nothing there, and waiting for a tab would only keep every new
        // window hidden for seconds. Touch nothing, as if not installed.
        const unsigned build = windowsBuild();
        if (!tabs::explorerHasTabs(build)) {
            m_unsupported = true;
            log("info", "File Explorer has no tabs on this Windows (build " + std::to_string(build)
                    + ", tabs need 22621 = Windows 11 22H2); not touching any window");
            return;
        }
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
            m_pauseFile = winshun::wide(dataDir()) + L"\\paused";
        }
        o.log = [this](const std::string& text) { log("info", text); };
        if (o.restriction.active())
            log("info", "restricted to " + winshun::utf8(o.restriction.root));
        m_engine = std::make_unique<tabs::Engine>(std::move(o));
        // Paused stays paused (a file in the data folder) until resumed.
        if (!m_pauseFile.empty() && ::GetFileAttributesW(m_pauseFile.c_str()) != INVALID_FILE_ATTRIBUTES)
            m_engine->setPaused(true);
        m_engine->start();
        // The open tabs are kept whether or not they come back by themselves:
        // the keyword's second row brings them back when asked.
        if (!dataDir().empty()) {
            tabs::KeeperOptions k;
            k.file = winshun::wide(dataDir()) + L"\\session.txt";
            k.restoreAtLogon = settings()["restore"].toBool();
            k.restriction.root = winshun::wide(winshun::env("WINSHUN_TABS_TEST_ROOT"));
            k.asIfLogon = winshun::env("WINSHUN_TABS_LOGON") == "1";
            k.hold = [engine = m_engine.get()](bool held) { engine->setHeld(held); };
            k.log = [this](const std::string& text) { log("info", text); };
            m_keeper = std::make_unique<tabs::Keeper>(std::move(k));
            m_keeper->start();
        }
    }

    void settingsChanged(const Json& s) override
    {
        if (m_engine)
            m_engine->configure(settingsFrom(s));
    }

    void shuttingDown() override
    {
        if (m_keeper)
            m_keeper->stop(); // first: it may be holding the engine
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
        if (m_unsupported) {
            status.title = zh ? "资源管理器标签页需要 Windows 11 22H2 或更新" : "Explorer tabs need Windows 11 22H2 or newer";
            status.subtitle = zh ? "这台电脑上不会生效，新开的文件夹窗口照常是窗口" : "Not active on this computer: folder windows stay windows";
            status.icon = kIcon;
            status.valid = false;
        } else if (!on) {
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
            status.subtitle = zh ? "Win顺 启动后已并入 " + count + " 个 · 按住 Ctrl+Shift 打开就是新窗口 · Enter 暂停"
                                 : count + " merged since WinShun started · hold Ctrl+Shift for a window · Enter to pause";
            status.icon = kPauseIcon;
        }
        items.push_back(status);
        if (const tabs::Keeper::Left left = m_keeper ? m_keeper->left() : tabs::Keeper::Left {}; left.tabs > 0) {
            const std::string windows = std::to_string(left.windows);
            const std::string count = std::to_string(left.tabs);
            Item restore("restore", zh ? "打开上次的标签页" : "Open the tabs from last time");
            restore.subtitle = zh ? "上次登录留下 " + windows + " 个窗口、" + count + " 个标签页 · 已经开着的不重复打开"
                                  : windows + " window(s), " + count + " tab(s) left by the last logon · those open already are skipped";
            restore.icon = kRestoreIcon;
            items.push_back(restore);
        }
        return items;
    }

    winshun::Effects action(const winshun::ActionRequest& a) override
    {
        winshun::Effects effects;
        if (a.item["id"].toString() == "restore" && a.action == "default" && m_keeper) {
            if (!m_keeper->restoring())
                m_keeper->restore();
        } else if (a.item["id"].toString() == "status" && a.action == "default" && m_engine) {
            const bool zh = chinese();
            const bool pause = !m_engine->paused();
            m_engine->setPaused(pause);
            if (!m_pauseFile.empty()) {
                if (pause) {
                    if (const HANDLE f = ::CreateFileW(m_pauseFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        f != INVALID_HANDLE_VALUE)
                        ::CloseHandle(f);
                } else
                    ::DeleteFileW(m_pauseFile.c_str());
            }
            if (pause)
                effects.push_back(winshun::effect::notify(zh ? "已暂停：新开的文件夹窗口照常是窗口，直到你恢复"
                                                             : "Paused: folder windows stay windows until you resume"));
            else
                effects.push_back(winshun::effect::notify(zh ? "已恢复：新开的文件夹窗口会并成标签页" : "Resumed: new folder windows become tabs"));
        }
        effects.push_back(winshun::effect::hide());
        return effects;
    }

private:
    std::unique_ptr<tabs::Engine> m_engine;
    std::unique_ptr<tabs::Keeper> m_keeper; // after the engine: gone before it
    bool m_unsupported = false; // Explorer here has no tabs (before Windows 11 22H2)
    std::wstring m_pauseFile; // <dataDir>\paused: there while paused (survives a restart)
};

} // namespace

int main()
{
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ExplorerTabs plugin;
    const int code = plugin.serve();
    return code;
}
