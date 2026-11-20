// WinShunSync.exe: the winshun.sync plugin in the plugin library (剪贴板同步),
// a resident plugin on the single-header SDK, no Qt. What is copied on this
// PC appears on this user's other Windows PCs and Macs (Mac顺) and the other
// way round: text, pictures, files and folders, end to end encrypted over
// ShunSync v1 (docs/shunsync-protocol.md; the engine in Engine.cpp), no
// account and no server.
//
// The 同步 list: requests to join (the six digits to compare, Enter agrees;
// another device may ask whenever sync is on, as on Mac顺, unless 别的设备可以请求加入
// is off: then only in the two minutes of "允许新设备加入"), files copied elsewhere
// (offered: Enter receives them onto the clipboard, Ctrl+Enter saves them to
// Downloads; none come by themselves unless 自动接收不超过 is set), the devices
// of the group (Enter sends what the clipboard holds to that one; Ctrl+Enter
// removes it), devices nearby, the invite code, what came lately. An invite
// code or an address typed after the keyword joins or asks to join. While it
// is open the rows are asked for again now and then (rerun), and at once when
// something happens (refresh()).

#include "ClipboardWin.h"
#include "Engine.h"
#include "Firewall.h"
#include "Protocol.h"
#include "Saver.h"
#include "Util.h"

#include "winshun.h"

#include <winsock2.h>

#include <windows.h>

#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace shunsync;
using winshun::Action;
using winshun::Effects;
using winshun::Item;
using winshun::Items;
using winshun::Json;
namespace effect = winshun::effect;

constexpr std::int64_t kConfirmRemoveMs = 10'000;

std::string computerName()
{
    wchar_t name[256] {};
    DWORD size = 256;
    if (::GetComputerNameExW(ComputerNamePhysicalDnsHostname, name, &size) && size > 0)
        return localsend::narrow(std::wstring(name, size));
    return "Windows";
}

std::wstring exePath()
{
    std::wstring path(32768, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(n);
    return path;
}

bool boolSetting(const Json& value, bool fallback)
{
    if (value.isBool())
        return value.toBool();
    if (value.isString())
        return value.toString() == "true" || value.toString() == "1";
    return fallback;
}

double numberSetting(const Json& value, double fallback)
{
    if (value.isNumber())
        return value.toNumber();
    if (value.isString()) {
        try {
            return std::stod(value.toString());
        } catch (...) {
        }
    }
    return fallback;
}

std::vector<std::string> addressList(const std::string& text)
{
    std::vector<std::string> out;
    std::string word;
    const auto flush = [&] {
        if (!word.empty() && parseHostPort(word, kTcpPort) && std::ranges::find(out, word) == out.end())
            out.push_back(word);
        word.clear();
    };
    for (const char c : text) {
        if (c == ',' || c == ';' || c == ' ' || c == '\n' || c == '\r' || c == '\t')
            flush();
        else
            word += c;
    }
    flush();
    return out;
}

// Words that are an address ("192.168.1.5", "mac.local:37219", "[fe80::1]"), not just any word.
bool looksLikeAddress(const std::string& words)
{
    if (words.empty() || words.find(' ') != std::string::npos || !parseHostPort(words, kTcpPort))
        return false;
    return words.find('.') != std::string::npos || words.find(':') != std::string::npos || words == "localhost";
}

std::string spacedSas(const std::string& sas)
{
    return sas.size() == 6 ? sas.substr(0, 3) + " " + sas.substr(3) : sas;
}

std::string shown(const std::string& name, std::size_t bytes = 60)
{
    std::string out;
    for (const char c : localsend::validUtf8(name))
        out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    // No marks that turn the row around (a name may hold U+202E).
    std::wstring wide = localsend::wide(out);
    std::erase_if(wide, [](wchar_t c) { return localsend::directionMark(c); });
    return localsend::cutUtf8(localsend::narrow(wide), bytes);
}

class SyncPlugin final : public winshun::Plugin {
public:
    ~SyncPlugin() override { stop(); }

protected:
    std::string name() const override { return "WinShunSync"; }
    void initialized() override { start(); }
    void shuttingDown() override { stop(); }
    Items query(const winshun::Query&) override { return {}; } // queryResult answers
    Json queryResult(const winshun::Query& q) override;
    Effects action(const winshun::ActionRequest& a) override;
    void settingsChanged(const Json& values) override;

private:
    std::string t(const char* zh, const char* en) const { return chinese() ? zh : en; }
    Options optionsOf(const Json& settings) const;
    void start();
    void stop();
    void firewallLoop();
    Items rows(const std::string& words);
    Effects refreshed(const std::string& words, Effects before = {});
    std::string keywordSpace() const;
    std::string ago(std::int64_t ms) const;
    std::string pairProblem(const std::string& code) const;
    std::string filesProblem(const std::string& code) const;
    std::wstring sharedFolder(const std::string& transferId) const;
    Effects sendTo(const std::string& deviceId, const std::string& name);
    void saveLoop();
    void saveToDownloads(const std::string& transferId);
    bool saving(const std::string& transferId) const;

    std::unique_ptr<Engine> m_engine;
    std::unique_ptr<ClipboardWin> m_clipboard;
    std::string m_startProblem;
    std::wstring m_dataFolder;
    std::wstring m_exe;

    mutable std::mutex m_mutex; // what follows
    std::string m_keyword = "同步";
    localsend::FirewallFacts m_firewall;
    std::string m_confirmRemove; // a member's id: the next Ctrl+Enter on it removes it
    std::int64_t m_confirmRemoveAt = 0;

    std::thread m_firewallThread;
    std::mutex m_firewallMutex;
    std::condition_variable m_firewallWake;
    bool m_firewallStop = false;

    // 存到下载: transfers to copy into Downloads once they are here.
    std::thread m_saveThread;
    mutable std::mutex m_saveMutex;
    std::condition_variable m_saveWake;
    std::set<std::string> m_saving;
    bool m_saveStop = false;
};

// ---- start, stop, settings ---------------------------------------------------------

Options SyncPlugin::optionsOf(const Json& s) const
{
    Options o;
    o.deviceName = shown(localsend::trimmed(s["name"].toString()), 64);
    if (o.deviceName.empty())
        o.deviceName = computerName();
    o.dataDir = m_dataFolder;
    o.autoWrite = boolSetting(s["autoWrite"], true);
    o.shareFiles = boolSetting(s["files"], true);
    o.fileNotice = boolSetting(s["fileNotice"], true);
    o.acceptPairing = boolSetting(s["pairingRequests"], true);
    const double megabytes = std::clamp(numberSetting(s["prefetchMB"], 0), 0.0, 2048.0);
    o.prefetchBytes = static_cast<std::uint64_t>(megabytes * 1024 * 1024);
    o.addresses = addressList(s["addresses"].toString());
    o.allNetworks = s["networks"].toString() == "all";
    o.paused = boolSetting(s["paused"], false);
    o.chinese = chinese();
    return o;
}

void SyncPlugin::start()
{
    m_exe = exePath();
    m_dataFolder = localsend::wide(dataDir());
    if (m_dataFolder.empty())
        m_dataFolder = m_exe.substr(0, m_exe.find_last_of(L'\\'));
    ::CreateDirectoryW(m_dataFolder.c_str(), nullptr);
    const Options options = optionsOf(settings());
    Events events;
    events.changed = [this] { refresh(); };
    events.toast = [this](const std::string& title, const std::string& text) { toast(title, text, {effect::setQuery(keywordSpace())}); };
    events.log = [this](const std::string& line) { log("info", line); };
    m_clipboard = std::make_unique<ClipboardWin>(
        [this](Copied c) {
            if (!m_engine)
                return;
            if (c.kind == Copied::Kind::Text)
                m_engine->localText(std::move(c.text));
            else if (c.kind == Copied::Kind::Image)
                m_engine->localImage(std::move(c.png), c.width, c.height);
            else
                m_engine->localFiles(std::move(c.files));
        },
        [this](const std::string& line) { log("info", line); });
    m_engine = std::make_unique<Engine>(options, *m_clipboard, std::move(events));
    std::string problem;
    if (!m_engine->start(&problem)) {
        m_startProblem = problem;
        log("warning", "not started: " + problem);
        return;
    }
    if (!m_clipboard->start())
        m_startProblem = "clipboard";
    m_clipboard->setWatching(!options.paused);
    m_firewallThread = std::thread([this] { firewallLoop(); });
    m_saveThread = std::thread([this] { saveLoop(); });
}

void SyncPlugin::stop()
{
    {
        const std::lock_guard lock(m_firewallMutex);
        m_firewallStop = true;
    }
    m_firewallWake.notify_all();
    if (m_firewallThread.joinable())
        m_firewallThread.join();
    {
        const std::lock_guard lock(m_saveMutex);
        m_saveStop = true;
    }
    m_saveWake.notify_all();
    if (m_saveThread.joinable())
        m_saveThread.join();
    if (m_clipboard)
        m_clipboard->setWatching(false);
    if (m_engine)
        m_engine->stop(); // its threads end before the clipboard they write to goes
    if (m_clipboard)
        m_clipboard->stop();
}

void SyncPlugin::firewallLoop()
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::unique_lock lock(m_firewallMutex);
    while (!m_firewallStop) {
        lock.unlock();
        const localsend::FirewallFacts facts = localsend::firewallFacts(m_exe);
        bool changed = false;
        {
            const std::lock_guard state(m_mutex);
            changed = facts.known != m_firewall.known || facts.on != m_firewall.on || facts.allowed != m_firewall.allowed
                || facts.blocked != m_firewall.blocked;
            m_firewall = facts;
        }
        if (changed)
            refresh();
        lock.lock();
        m_firewallWake.wait_for(lock, std::chrono::seconds(60), [this] { return m_firewallStop; });
    }
    lock.unlock();
    if (SUCCEEDED(com))
        ::CoUninitialize();
}

void SyncPlugin::settingsChanged(const Json& values)
{
    if (!m_engine)
        return;
    const Options options = optionsOf(values);
    m_engine->setOptions(options);
    if (m_clipboard)
        m_clipboard->setWatching(!options.paused);
}

// ---- the rows ----------------------------------------------------------------------------

std::string SyncPlugin::keywordSpace() const
{
    const std::lock_guard lock(m_mutex);
    return m_keyword + " ";
}

std::string SyncPlugin::ago(std::int64_t ms) const
{
    const std::int64_t seconds = std::max<std::int64_t>(0, (nowMs() - ms) / 1000);
    if (seconds < 60)
        return t("刚刚", "just now");
    if (seconds < 3600)
        return chinese() ? std::to_string(seconds / 60) + " 分钟前" : std::to_string(seconds / 60) + " min ago";
    if (seconds < 86400)
        return chinese() ? std::to_string(seconds / 3600) + " 小时前" : std::to_string(seconds / 3600) + " h ago";
    return chinese() ? std::to_string(seconds / 86400) + " 天前" : std::to_string(seconds / 86400) + " d ago";
}

std::string SyncPlugin::pairProblem(const std::string& code) const
{
    if (code == "pairing_closed")
        return t("对方没有开放加入：在对方的「同步」里选「允许新设备加入」后再试", "It is not letting new devices in: choose Let a new device in under sync there, then try again");
    if (code == "pairing_rate_limited")
        return t("请求太频繁，等几秒再试", "Asked too often; try again in a few seconds");
    if (code == "wrong_group" || code == "unknown_group")
        return t("对方不在这个组里", "It is not in this group");
    if (code == "declined")
        return t("对方没有同意", "It said no");
    if (code == "declined_here")
        return t("已拒绝", "Declined");
    if (code == "canceled")
        return t("对方取消了", "Canceled on the other device");
    if (code == "canceled_here")
        return t("已取消", "Canceled");
    if (code == "pairing_busy" || code == "busy")
        return t("对方正有别的设备在请求，稍后再试", "Another device is asking it right now; try again in a moment");
    if (code == "timeout")
        return t("等太久没人确认，已取消", "Nobody agreed in time; canceled");
    if (code == "self")
        return t("这是本机", "That is this PC");
    if (code == "lost")
        return t("连接断了，再试一次", "The connection broke; try again");
    return t("连不上：两台设备在同一个网络吗？对方的防火墙放行了吗？", "Could not reach it: same network? Its firewall?");
}

std::string SyncPlugin::filesProblem(const std::string& code) const
{
    if (code == "canceled")
        return t("已取消", "Canceled");
    if (code == "not_found")
        return t("对方那边的文件不在了（移走、删掉，或者对方重启过）", "The files are gone on the other device (moved, deleted, or it restarted)");
    if (code == "disk")
        return t("这台电脑写不下（磁盘满了？）", "Could not write them here (disk full?)");
    if (code == "checksum" || code == "sender")
        return t("收到的内容不对，再试一次", "What came was not right; try again");
    if (code == "unreachable")
        return t("连不上复制文件的那台设备", "Could not reach the device they were copied on");
    return t("传输断了，再试一次", "The transfer broke off; try again");
}

std::wstring SyncPlugin::sharedFolder(const std::string& transferId) const
{
    return m_dataFolder + L"\\SharedFiles\\" + localsend::wide(transferId);
}

bool SyncPlugin::saving(const std::string& transferId) const
{
    const std::lock_guard lock(m_saveMutex);
    return m_saving.count(transferId) > 0;
}

// 存到下载 (Mac顺's 存到下载): once a transfer asked for so is here, its files and
// folders are copied into Downloads (free names: "报告 (1).docx") and shown
// selected in a File Explorer window.
void SyncPlugin::saveLoop()
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::unique_lock lock(m_saveMutex);
    while (!m_saveStop) {
        if (m_saving.empty() || !m_engine) {
            m_saveWake.wait(lock, [this] { return m_saveStop || !m_saving.empty(); });
            continue;
        }
        lock.unlock();
        const Snapshot s = m_engine->snapshot();
        std::vector<std::string> ready;
        lock.lock();
        std::erase_if(m_saving, [&](const std::string& id) {
            const auto f = std::ranges::find(s.files, id, &FilesView::transferId);
            if (f == s.files.end() || f->stage == FilesView::Stage::Failed)
                return true; // gone, canceled or failed: its row says so
            if (f->stage != FilesView::Stage::Ready)
                return false;
            ready.push_back(id);
            return true;
        });
        lock.unlock();
        for (const std::string& id : ready)
            saveToDownloads(id);
        lock.lock();
        m_saveWake.wait_for(lock, std::chrono::milliseconds(300), [this] { return m_saveStop; });
    }
    lock.unlock();
    if (SUCCEEDED(com))
        ::CoUninitialize();
}

void SyncPlugin::saveToDownloads(const std::string& transferId)
{
    namespace fs = std::filesystem;
    const std::wstring downloads = localsend::downloadsFolder();
    std::vector<std::wstring> saved;
    std::error_code error;
    if (!downloads.empty() && localsend::ensureFolder(downloads)) {
        localsend::Placement placement(downloads);
        for (const auto& entry : fs::directory_iterator(sharedFolder(transferId), error)) {
            const std::wstring target = placement.place(entry.path().filename().wstring());
            std::error_code copied;
            fs::copy(entry.path(), target, fs::copy_options::recursive, copied);
            if (!copied)
                saved.push_back(target);
        }
    }
    if (saved.empty()) {
        toast(t("没能存到「下载」", "Could not save to Downloads"),
            t("文件还在「同步」里，可以按 Enter 放进剪贴板", "They are still in sync: Enter puts them on the clipboard"),
            {effect::setQuery(keywordSpace())});
        return;
    }
    // Selected in File Explorer (the shell does it: no program started here).
    if (PIDLIST_ABSOLUTE folder = ::ILCreateFromPathW(downloads.c_str())) {
        std::vector<PIDLIST_ABSOLUTE> items;
        std::vector<PCUITEMID_CHILD> children;
        for (const std::wstring& path : saved) {
            if (PIDLIST_ABSOLUTE item = ::ILCreateFromPathW(path.c_str())) {
                items.push_back(item);
                children.push_back(::ILFindLastID(item));
            }
        }
        if (!children.empty())
            ::SHOpenFolderAndSelectItems(folder, static_cast<UINT>(children.size()), children.data(), 0);
        for (PIDLIST_ABSOLUTE item : items)
            ::ILFree(item);
        ::ILFree(folder);
    }
    const std::string first = localsend::narrow(fs::path(saved.front()).filename().wstring());
    const std::string what = saved.size() == 1 ? "「" + first + "」"
                                               : (chinese() ? "「" + first + "」等 " + std::to_string(saved.size()) + " 项"
                                                            : first + " and " + std::to_string(saved.size() - 1) + " more");
    toast(t("已存到「下载」", "Saved to Downloads"), what, {effect::openPath(localsend::narrow(downloads))});
}

Items SyncPlugin::rows(const std::string& rawWords)
{
    Items out;
    if (!m_engine || !m_startProblem.empty()) {
        const std::string why = m_startProblem == "bind" || m_startProblem == "listen"
            ? t("端口打不开", "no port could be opened")
            : m_startProblem == "clipboard" ? t("剪贴板监听没能打开", "the clipboard could not be watched") : m_startProblem;
        out.push_back(Item("problem", t("剪贴板同步没能启动", "Clipboard sync could not start"), why)
                .withIcon("ph:warning-circle")
                .withActions({{"settings", t("打开设置", "Open settings"), "Enter"}}));
        return out;
    }
    const std::string words = localsend::trimmed(rawWords);
    const Snapshot s = m_engine->snapshot();
    localsend::FirewallFacts firewall;
    std::string confirmRemove;
    {
        const std::lock_guard lock(m_mutex);
        firewall = m_firewall;
        if (nowMs() - m_confirmRemoveAt < kConfirmRemoveMs)
            confirmRemove = m_confirmRemove;
    }
    std::size_t online = 0;
    for (const MemberView& m : s.members)
        online += m.state == PeerState::Online;

    // Requests to join this PC's group: first, they wait for an answer.
    for (const PairView& p : s.pairs) {
        if (!p.incoming || p.stage != PairView::Stage::Confirm)
            continue;
        const std::string who = p.name.empty() ? t("新设备", "A new device") : shown(p.name);
        out.push_back(Item("pair-" + std::to_string(p.id),
            chinese() ? "「" + who + "」想加入剪贴板同步" : who + " asks to join clipboard sync",
            chinese() ? "确认码 " + spacedSas(p.sas) + " · 和对方屏幕上的一样就按 Enter 同意，Ctrl+Enter 拒绝"
                      : "Code " + spacedSas(p.sas) + " · the same on its screen? Enter agrees, Ctrl+Enter declines")
                .withSubtitleAway(t("按 ↑ 选中它，核对确认码后同意", "Press ↑ to it, compare the code, then agree"))
                .withIcon("ph:handshake")
                .withData(Json::Object {{"pair", static_cast<double>(p.id)}})
                .withActions({{"accept", t("同意加入", "Agree"), "Enter"}, {"decline", t("拒绝", "Decline"), "Ctrl+Enter"}}));
    }
    // Ours, asking another device.
    for (const PairView& p : s.pairs) {
        if (p.incoming)
            continue;
        const std::string who = shown(p.name);
        Item row("ask-" + std::to_string(p.id), "", "");
        row.withIcon("ph:link").withData(Json::Object {{"pair", static_cast<double>(p.id)}});
        if (p.stage == PairView::Stage::Connecting) {
            row.title = chinese() ? "正在联系「" + who + "」…" : "Reaching " + who + "…";
            row.subtitle = t("Ctrl+Enter 取消", "Ctrl+Enter cancels");
            row.withActions({{"hide", t("在后台等", "Wait in the background"), "Enter"}, {"cancelPair", t("取消", "Cancel"), "Ctrl+Enter"}});
        } else if (p.stage == PairView::Stage::Confirm) {
            row.title = chinese() ? "确认码 " + spacedSas(p.sas) : "Code " + spacedSas(p.sas);
            row.subtitle = chinese() ? "在「" + who + "」上核对这串数字并同意 · Ctrl+Enter 取消"
                                     : "Compare it on " + who + " and agree there · Ctrl+Enter cancels";
            row.withActions({{"hide", t("在后台等", "Wait in the background"), "Enter"}, {"cancelPair", t("取消", "Cancel"), "Ctrl+Enter"}});
        } else if (p.stage == PairView::Stage::Done) {
            row.title = chinese() ? "已加入「" + who + "」的剪贴板同步" : "Joined " + who + "'s clipboard sync";
            row.subtitle = t("复制的东西会出现在组里每台设备上 · Enter 关掉这行", "What you copy appears on every device of the group · Enter hides this");
            row.withIcon("ph:check-circle").withActions({{"cancelPair", t("知道了", "OK"), "Enter"}});
        } else {
            row.title = chinese() ? "没能加入「" + who + "」" : "Could not join " + who;
            row.subtitle = pairProblem(p.problem) + t(" · Enter 关掉这行", " · Enter hides this");
            row.withIcon("ph:x-circle").withActions({{"cancelPair", t("知道了", "OK"), "Enter"}});
        }
        out.push_back(std::move(row));
    }

    // Words typed after the keyword: an invite code, or an address to ask.
    if (!words.empty()) {
        if (parseInvite(words)) {
            out.push_back(Item("join", t("用这个邀请码加入", "Join with this invite code"),
                s.members.empty() ? t("不用配对，加入后自动连上组里的设备", "No pairing: the devices of the group are reached by themselves")
                                  : t("这台电脑会离开现在的组，加入新的组", "This PC leaves its group for that one"))
                    .withIcon("ph:key")
                    .withData(Json::Object {{"code", words}})
                    .withActions({{"join", t("加入", "Join"), "Enter"}}));
        } else if (looksLikeAddress(words)) {
            out.push_back(Item("address", chinese() ? "请求加入 " + words + " 的剪贴板同步" : "Ask to join " + words + "'s clipboard sync",
                t("对方要先打开「允许新设备加入」 · Ctrl+Enter 记住这个地址（跨网段、Tailscale 时用）",
                    "It must let new devices in first · Ctrl+Enter remembers the address (another subnet, Tailscale)"))
                    .withIcon("ph:link")
                    .withData(Json::Object {{"address", words}})
                    .withActions({{"pairAddress", t("请求加入", "Ask to join"), "Enter"}, {"addAddress", t("记住地址", "Remember"), "Ctrl+Enter"}}));
        } else {
            out.push_back(Item("hint", t("输入邀请码，或另一台设备的地址", "Type an invite code, or another device's address"),
                t("比如 shunsync://join?k=… 或 192.168.1.20", "Like shunsync://join?k=… or 192.168.1.20"))
                    .withIcon("ph:info")
                    .invalid());
        }
    }

    // Files copied elsewhere.
    for (const FilesView& f : s.files) {
        const std::string who = shown(f.originName);
        std::string what;
        if (chinese()) {
            if (f.files)
                what += std::to_string(f.files) + " 个文件";
            if (f.folders)
                what += (what.empty() ? "" : "、") + std::to_string(f.folders) + " 个文件夹";
        } else {
            if (f.files)
                what += std::to_string(f.files) + (f.files == 1 ? " file" : " files");
            if (f.folders)
                what += (what.empty() ? "" : ", ") + std::to_string(f.folders) + (f.folders == 1 ? " folder" : " folders");
        }
        const std::string size = localsend::sizeText(f.totalBytes);
        Item row("files-" + f.transferId, "", "");
        row.withIcon("ph:files").withData(Json::Object {{"transfer", f.transferId}});
        switch (f.stage) {
        case FilesView::Stage::Offered:
            row.title = chinese() ? who + " 复制了 " + what : who + " copied " + what;
            row.subtitle = chinese() ? size + " · " + ago(f.timestampMs) + " · Enter 接收并复制，Ctrl+Enter 存到「下载」"
                                     : size + " · " + ago(f.timestampMs) + " · Enter receives and copies them, Ctrl+Enter saves them to Downloads";
            row.withIcon("ph:download-simple").withActions({{"pull", t("接收并复制", "Receive and copy"), "Enter"},
                {"save", t("存到下载", "Save to Downloads"), "Ctrl+Enter"}});
            break;
        case FilesView::Stage::Pulling: {
            const int percent = f.totalBytes ? static_cast<int>(f.receivedBytes * 100 / std::max<std::uint64_t>(f.totalBytes, 1)) : 0;
            row.title = chinese() ? "正在接收 " + who + " 的 " + what + " · " + std::to_string(percent) + "%"
                                  : "Receiving " + who + "'s " + what + " · " + std::to_string(percent) + "%";
            row.subtitle = localsend::sizeText(f.receivedBytes) + " / " + size
                + (saving(f.transferId) ? t(" · 收完存到「下载」 · Ctrl+Enter 取消", " · into Downloads when done · Ctrl+Enter cancels")
                                        : t(" · 收完自动放进剪贴板 · Ctrl+Enter 取消", " · onto the clipboard when done · Ctrl+Enter cancels"));
            row.withActions({{"hide", t("后台接收", "In the background"), "Enter"}, {"cancelPull", t("取消", "Cancel"), "Ctrl+Enter"}});
            break;
        }
        case FilesView::Stage::Ready:
            row.title = chinese() ? "收到了 " + who + " 的 " + what : "Received " + who + "'s " + what;
            row.subtitle = size
                + t(" · Enter 再放进剪贴板，Ctrl+Enter 存到「下载」，Shift+Enter 打开文件夹",
                    " · Enter puts them on the clipboard again, Ctrl+Enter saves them to Downloads, Shift+Enter opens the folder");
            row.withIcon("ph:check-circle").withActions({{"pull", t("放进剪贴板", "Onto the clipboard"), "Enter"},
                {"save", t("存到下载", "Save to Downloads"), "Ctrl+Enter"}, {"openFolder", t("打开文件夹", "Open folder"), "Shift+Enter"}});
            break;
        case FilesView::Stage::Failed:
            row.title = chinese() ? "没收到 " + who + " 的 " + what : who + "'s " + what + " did not come";
            row.subtitle = filesProblem(f.problem) + t(" · Enter 重试", " · Enter tries again");
            row.withIcon("ph:x-circle").withActions({{"pull", t("重试", "Try again"), "Enter"}});
            break;
        }
        out.push_back(std::move(row));
        if (out.size() > 12)
            break;
    }

    // This PC.
    {
        std::string state;
        if (s.paused)
            state = t("已暂停：不发也不收 · Enter 继续", "Paused: nothing sent or taken · Enter resumes");
        else if (s.members.empty())
            state = t("还没有别的设备 · 在另一台设备上请求加入这台，或用邀请码", "No other device yet · ask to join this PC from another one, or use an invite code");
        else
            state = chinese() ? std::to_string(online) + " 台在线，共 " + std::to_string(s.members.size()) + " 台 · Enter 暂停"
                              : std::to_string(online) + " of " + std::to_string(s.members.size()) + " online · Enter pauses";
        out.push_back(Item("self", chinese() ? "本机「" + shown(s.deviceName) + "」" : "This PC, " + shown(s.deviceName), state)
                .withIcon(s.paused ? "ph:pause" : "ph:arrows-clockwise")
                .withActions({{s.paused ? "resume" : "pause", s.paused ? t("继续同步", "Resume") : t("暂停同步", "Pause"), "Enter"}}));
    }

    // The group's devices.
    for (const MemberView& m : s.members) {
        const std::string who = m.name.empty() ? t("未命名设备", "Unnamed device") : shown(m.name);
        const bool mac = m.platform == "macOS";
        Item row("member-" + m.deviceId, who, "");
        row.withIcon(mac ? "ph:laptop" : "ph:desktop").withData(Json::Object {{"device", m.deviceId}, {"name", who}});
        std::string where = m.platform;
        if (!m.host.empty())
            where += (where.empty() ? "" : " · ") + m.host;
        if (confirmRemove == m.deviceId) {
            row.title = chinese() ? "再按一次 Ctrl+Enter 移除「" + who + "」" : "Ctrl+Enter again removes " + who;
            row.subtitle = t("组会换一把新密钥，它拿不到；以后要再用就得重新加入", "The group gets a new key it does not have; to use it again it joins anew");
            row.withIcon("ph:trash").withActions({{"keepMember", t("不移除", "Keep it"), "Enter"}, {"remove", t("移除", "Remove"), "Ctrl+Enter"}});
        } else if (m.state == PeerState::Online) {
            row.subtitle = t("在线 · ", "Online · ") + where + t(" · Enter 把剪贴板发给它，Ctrl+Enter 移除", " · Enter sends it the clipboard, Ctrl+Enter removes");
            row.withActions({{"sendTo", t("发送剪贴板", "Send the clipboard"), "Enter"}, {"remove", t("移除", "Remove"), "Ctrl+Enter"}});
        } else {
            const std::string when = m.lastSeenMs > 0 ? t(" · 上次在线 ", " · last seen ") + ago(m.lastSeenMs) : std::string();
            row.subtitle = (m.state == PeerState::Connecting ? t("正在连接", "Connecting") : t("离线", "Offline")) + when
                + (where.empty() ? "" : " · " + where) + t(" · Ctrl+Enter 移除", " · Ctrl+Enter removes");
            row.withActions({{"sendTo", t("发送剪贴板", "Send the clipboard"), "Enter"}, {"remove", t("移除", "Remove"), "Ctrl+Enter"}});
        }
        out.push_back(std::move(row));
    }

    // Devices nearby, of another group.
    for (const NearbyView& n : s.nearby) {
        const std::string who = n.name.empty() ? t("未命名设备", "Unnamed device") : shown(n.name);
        out.push_back(Item("nearby-" + n.deviceId, chinese() ? "附近的「" + who + "」" : who + " nearby",
            n.platform + " · " + n.host
                + (n.pairingOpen ? t(" · Enter 请求加入它的组", " · Enter asks to join its group")
                                 : t(" · 它没有开放加入：先在它上面选「允许新设备加入」，再按 Enter", " · it is not letting new devices in: choose that there first, then press Enter")))
                .withIcon("ph:broadcast")
                .withData(Json::Object {{"device", n.deviceId}})
                .withActions({{"pairNearby", t("请求加入", "Ask to join"), "Enter"}}));
    }

    // Letting a new device in, the invite code.
    if (s.pairingLeftMs > 0) {
        const std::int64_t left = (s.pairingLeftMs + 999) / 1000;
        const std::string clock = std::to_string(left / 60) + ":" + (left % 60 < 10 ? "0" : "") + std::to_string(left % 60);
        out.push_back(Item("pairing", t("正在等新设备加入 · 还剩 ", "Letting new devices in · ") + clock,
            t("在另一台设备上打开「同步」，选这台电脑请求加入 · Enter 停止", "On the other device open sync and ask to join this PC · Enter stops"))
                .withIcon("ph:user-plus")
                .withActions({{"closePairing", t("停止等待", "Stop"), "Enter"}}));
    } else if (!s.acceptingPairing) {
        out.push_back(Item("pairing", t("允许新设备加入", "Let a new device in"),
            t("打开 2 分钟：另一台设备就能请求加入，两边显示同一串数字时同意", "For 2 minutes: another device can ask to join; agree when both show the same digits"))
                .withIcon("ph:user-plus")
                .withActions({{"openPairing", t("允许 2 分钟", "For 2 minutes"), "Enter"}}));
    }
    out.push_back(Item("invite", t("复制邀请码", "Copy the invite code"),
        t("在另一台设备的「同步」里粘贴就能加入，不用配对（不在同一网段、用 Tailscale 时方便）；里面有组的密钥，只发给自己的设备",
            "Pasted into sync on another device it joins without pairing (handy across subnets, Tailscale); it holds the group's key: only for your own devices"))
            .withIcon("ph:key")
            .withActions({{"copyInvite", t("复制", "Copy"), "Enter"}}));

    // What came lately.
    int shownReceived = 0;
    for (const ReceivedView& r : s.received) {
        if (r.kind == ReceivedView::Kind::Files || ++shownReceived > 5)
            continue; // files have their own rows
        const std::string from = (chinese() ? "来自 " : "From ") + shown(r.originName) + " · " + ago(r.timestampMs);
        const bool text = r.kind == ReceivedView::Kind::Text;
        out.push_back(Item("received-" + r.eventId,
            text ? (chinese() ? "文字「" + shown(r.preview, 90) + "」" : "Text \"" + shown(r.preview, 90) + "\"")
                 : (chinese() ? "图片 " : "Picture ") + r.preview,
            from + t(" · Enter 再放进剪贴板", " · Enter puts it on the clipboard again"))
                .withIcon(text ? "ph:clipboard-text" : "ph:image")
                .withData(Json::Object {{"event", r.eventId}})
                .withActions({{"rewrite", t("放进剪贴板", "Onto the clipboard"), "Enter"}}));
    }

    // Windows Firewall keeping the other devices out.
    if (firewall.known && firewall.on && (firewall.blocked || (!firewall.allowed && online == 0))) {
        out.push_back(Item("firewall",
            firewall.blocked ? t("Windows 防火墙挡住了别的设备连进来", "Windows Firewall keeps other devices from connecting")
                             : t("别的设备可能连不进来：Windows 防火墙还没放行", "Other devices may not get in: Windows Firewall has not allowed it"),
            t("Windows 问的时候选「允许」；已经点了「取消」的话，到「允许应用通过 Windows 防火墙」里给 WinShunSync 勾上「专用」。这台电脑主动去连别的设备不受影响",
                "Choose Allow when Windows asks; if you chose Cancel, tick Private for WinShunSync under Allow an app through Windows Firewall. Connecting from this PC to others is not affected"))
                .withIcon("ph:shield-warning")
                .invalid());
    }
    return out;
}

Json SyncPlugin::queryResult(const winshun::Query& q)
{
    if (!q.keyword.empty()) {
        const std::lock_guard lock(m_mutex);
        m_keyword = q.keyword;
    }
    const Items items = rows(q.text);
    bool busy = false;
    if (m_engine) {
        const Snapshot s = m_engine->snapshot();
        busy = s.pairingLeftMs > 0;
        for (const PairView& p : s.pairs)
            busy = busy || p.stage == PairView::Stage::Connecting || p.stage == PairView::Stage::Confirm;
        for (const FilesView& f : s.files)
            busy = busy || f.stage == FilesView::Stage::Pulling;
    }
    return Json::Object {{"items", winshun::itemsJson(items)}, {"rerun", busy ? 1.0 : 5.0}};
}

Effects SyncPlugin::refreshed(const std::string& words, Effects before)
{
    before.push_back(effect::replace(rows(words)));
    before.push_back(effect::keep());
    return before;
}

// ---- actions ------------------------------------------------------------------------------

Effects SyncPlugin::sendTo(const std::string& deviceId, const std::string& name)
{
    const std::optional<Copied> c = m_clipboard ? m_clipboard->current() : std::nullopt;
    if (!c)
        return {effect::notify(t("剪贴板里没有能发的东西（文字、图片或文件）", "Nothing on the clipboard to send (text, a picture or files)")), effect::keep()};
    if (c->kind == Copied::Kind::Text)
        m_engine->localText(c->text, deviceId);
    else if (c->kind == Copied::Kind::Image)
        m_engine->localImage(c->png, c->width, c->height, deviceId);
    else
        m_engine->localFiles(c->files, deviceId);
    return {effect::notify(chinese() ? "已发给「" + name + "」" : "Sent to " + name), effect::hide()};
}

Effects SyncPlugin::action(const winshun::ActionRequest& a)
{
    const std::string& id = a.action;
    if (id == "settings")
        return {effect::openUrl("winshun:settings")};
    if (!m_engine || !m_startProblem.empty())
        return {effect::notify(t("剪贴板同步没能启动", "Clipboard sync could not start")), effect::keep()};
    const Json& data = a.item["data"];
    const auto pairId = static_cast<std::uint64_t>(data["pair"].toInt());

    if (id == "accept" || id == "decline") {
        m_engine->answerPair(pairId, id == "accept");
        return refreshed(a.text, {effect::notify(id == "accept" ? t("已同意，正在连接", "Agreed; connecting") : t("已拒绝", "Declined"))});
    }
    if (id == "cancelPair") {
        m_engine->cancelPair(pairId);
        return refreshed(a.text);
    }
    if (id == "join") {
        if (!m_engine->joinWithInvite(data["code"].toString()))
            return {effect::notify(t("这不是邀请码", "That is not an invite code")), effect::keep()};
        return {effect::notify(t("已加入，正在连接组里的设备", "Joined; reaching the group's devices")), effect::setQuery(keywordSpace()), effect::keep()};
    }
    if (id == "pairAddress" || id == "pairNearby") {
        const std::string target = id == "pairAddress" ? data["address"].toString() : data["device"].toString();
        if (!m_engine->pairWith(target))
            return refreshed(a.text, {effect::notify(t("这个设备不见了", "That device is gone"))});
        return {effect::setQuery(keywordSpace()), effect::keep()};
    }
    if (id == "addAddress") {
        std::string addresses = settings()["addresses"].toString();
        const std::string address = data["address"].toString();
        const std::vector<std::string> known = addressList(addresses);
        if (std::ranges::find(known, address) == known.end())
            addresses = localsend::trimmed(addresses).empty() ? address : localsend::trimmed(addresses) + ", " + address;
        updateSettings(Json::Object {{"addresses", addresses}});
        return {effect::notify(t("已记住，会一直试着连它", "Remembered; it will keep being tried")), effect::setQuery(keywordSpace()), effect::keep()};
    }
    if (id == "pull") {
        m_engine->pullFiles(data["transfer"].toString());
        return refreshed(a.text);
    }
    if (id == "save") {
        const std::string transfer = data["transfer"].toString();
        {
            const std::lock_guard lock(m_saveMutex);
            m_saving.insert(transfer);
        }
        const Snapshot s = m_engine->snapshot();
        const auto f = std::ranges::find(s.files, transfer, &FilesView::transferId);
        const bool here = f != s.files.end() && f->stage == FilesView::Stage::Ready;
        if (!here)
            m_engine->pullFiles(transfer, false);
        m_saveWake.notify_all();
        return refreshed(a.text, {effect::notify(here ? t("正在存到「下载」", "Saving to Downloads")
                                                      : t("正在接收，收完存到「下载」", "Receiving; into Downloads when done"))});
    }
    if (id == "cancelPull") {
        m_engine->cancelPull(data["transfer"].toString());
        return refreshed(a.text, {effect::notify(t("已取消，收到一半的文件已删除", "Canceled; what came so far was deleted"))});
    }
    if (id == "hide")
        return {effect::hide()};
    if (id == "openFolder")
        return {effect::openPath(localsend::narrow(sharedFolder(data["transfer"].toString()))), effect::hide()};
    if (id == "openPairing" || id == "closePairing") {
        if (id == "openPairing")
            m_engine->openPairing();
        else
            m_engine->closePairing();
        return refreshed(a.text);
    }
    if (id == "pause" || id == "resume") {
        const bool paused = id == "pause";
        Options options = optionsOf(settings());
        options.paused = paused;
        m_engine->setOptions(options); // at once; the setting follows
        if (m_clipboard)
            m_clipboard->setWatching(!paused);
        updateSettings(Json::Object {{"paused", paused}});
        return refreshed(a.text, {effect::notify(paused ? t("已暂停", "Paused") : t("已继续", "Resumed"))});
    }
    if (id == "sendTo") {
        const Snapshot s = m_engine->snapshot();
        const auto m = std::ranges::find(s.members, data["device"].toString(), &MemberView::deviceId);
        if (m != s.members.end() && m->state != PeerState::Online)
            return {effect::notify(chinese() ? "「" + data["name"].toString() + "」现在不在线，上线后会自动连上"
                                             : data["name"].toString() + " is not online now; it is reached when it is"),
                effect::keep()};
        return sendTo(data["device"].toString(), data["name"].toString());
    }
    if (id == "keepMember") {
        {
            const std::lock_guard lock(m_mutex);
            m_confirmRemove.clear();
            m_confirmRemoveAt = 0;
        }
        return refreshed(a.text);
    }
    if (id == "remove") {
        const std::string device = data["device"].toString();
        bool confirmed = false;
        {
            const std::lock_guard lock(m_mutex);
            confirmed = m_confirmRemove == device && nowMs() - m_confirmRemoveAt < kConfirmRemoveMs;
            m_confirmRemove = confirmed ? std::string() : device;
            m_confirmRemoveAt = confirmed ? 0 : nowMs();
        }
        if (!confirmed)
            return refreshed(a.text);
        m_engine->removeDevice(device);
        return refreshed(a.text, {effect::notify(t("已移除，组换了新密钥", "Removed; the group has a new key"))});
    }
    if (id == "copyInvite") {
        if (!m_clipboard || !m_clipboard->putSecret(m_engine->invite()))
            return {effect::notify(t("剪贴板正忙，再试一次", "The clipboard is busy; try again")), effect::keep()};
        return {effect::notify(t("已复制邀请码（不进剪贴板历史）", "Invite code copied (kept out of clipboard history)")), effect::hide()};
    }
    if (id == "rewrite") {
        if (!m_engine->rewrite(data["event"].toString()))
            return refreshed(a.text, {effect::notify(t("这条已经不在了", "That one is gone"))});
        return {effect::notify(t("已放进剪贴板", "On the clipboard")), effect::hide()};
    }
    return {effect::keep()};
}

} // namespace

int main()
{
    // Nothing but JSON-RPC goes to stdout.
    SyncPlugin plugin;
    return plugin.serve();
}
