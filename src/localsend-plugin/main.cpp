// WinShunLocalSend.exe: the winshun.localsend plugin that comes with Win顺
// (plugins/winshun.localsend, 局域网传输; GitHub #113). Files and text to
// and from phones and computers nearby that run LocalSend (protocol v2.2),
// a resident plugin on the single-header SDK, no Qt.
//
// Receiving: an HTTPS server on the networks where that is allowed
// (private and domain networks and this PC's own hotspot; a public network
// once the user says so in the 传 list, remembered by network), each
// transfer asked about (a system notification, the 传 list's first row).
// Sending: 传 lists the devices found; Enter sends what is in the box, the
// files chosen with "用局域网发送…" on Win顺's result menu, or the files
// copied in Explorer. While 传 is open its rows are asked for again now and
// then (rerun), and at once when something happens here (refresh()).

#include "Background.h"
#include "Client.h"
#include "Discovery.h"
#include "Firewall.h"
#include "HttpLite.h"
#include "Identity.h"
#include "Network.h"
#include "Protocol.h"
#include "RateLimits.h"
#include "Rows.h"
#include "Saver.h"
#include "Sender.h"
#include "Server.h"
#include "Tls.h"
#include "Util.h"

#include "winshun.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <iphlpapi.h>
#include <netioapi.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace localsend;
using winshun::Effects;
using winshun::Item;
using winshun::Items;
using winshun::Json;
namespace effect = winshun::effect;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto kKeepTexts = std::chrono::minutes(30); // in memory only, never written
constexpr auto kKeepMenuFiles = std::chrono::minutes(10);
constexpr std::size_t kMaxTexts = 5;
// A query after this long without one: 传 opened again. Well above the
// rerun while nothing happens (5 s), or each rerun would look anew.
constexpr auto kNewLook = 15s;
constexpr auto kLookAgain = 30s; // while it stays open, looked again this often
constexpr auto kEvaluateEvery = 5s; // networks (a network made private in Windows' settings shows no other sign)
constexpr auto kFirewallEvery = 60s;

std::wstring expandEnv(const std::wstring& text)
{
    if (text.find(L'%') == std::wstring::npos)
        return text;
    const DWORD size = ::ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
    std::wstring out(size, L'\0');
    if (size == 0 || ::ExpandEnvironmentStringsW(text.c_str(), out.data(), size) == 0)
        return text;
    out.resize(size - 1);
    return out;
}

std::string computerName()
{
    wchar_t name[256] {};
    DWORD size = 256;
    if (::GetComputerNameExW(ComputerNamePhysicalDnsHostname, name, &size) && size > 0)
        return narrow(std::wstring(name, size));
    return "Windows";
}

std::wstring exePath()
{
    std::wstring path(32768, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(n);
    return path;
}

std::string readTextFile(const std::wstring& path)
{
    const Handle f(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    LARGE_INTEGER size {};
    if (!f.valid() || !::GetFileSizeEx(f.get(), &size) || size.QuadPart > 1024 * 1024)
        return {};
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    if (!text.empty() && !::ReadFile(f.get(), text.data(), static_cast<DWORD>(text.size()), &got, nullptr))
        return {};
    text.resize(got);
    return text;
}

bool writeTextFile(const std::wstring& path, const std::string& text)
{
    const std::wstring temp = path + L".new";
    {
        const Handle f(::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr));
        DWORD written = 0;
        if (!f.valid() || !::WriteFile(f.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
            || written != text.size())
            return false;
    }
    return ::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

// " · 12.3 MB/s"; nothing until the first second's speed is known (not "0 B/s").
std::string speedText(double bytesPerSecond)
{
    return bytesPerSecond >= 1 ? " · " + sizeText(static_cast<std::uint64_t>(bytesPerSecond)) + "/s" : std::string();
}

std::size_t characters(const std::string& text)
{
    return wide(text).size();
}

// A chosen file's or folder's own name for the rows, nothing in it turning
// the row around (a name may hold U+202E); "D:\" for a drive.
std::string shownName(const std::wstring& path)
{
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/'))
        p.pop_back();
    const std::size_t slash = p.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos || slash + 1 == p.size() ? p : p.substr(slash + 1);
    return excerpt(narrow(name), 80);
}

// What the clipboard holds that 传 can send: files copied in Explorer, or text.
struct Clip {
    DWORD sequence = 0;
    bool read = false;
    std::vector<std::wstring> files;
    std::string text;
    bool operator==(const Clip&) const = default;
};

// On the clipboard's own thread (Background): a program that renders what
// it copied only when asked answers GetClipboardData in its own time.
std::optional<Clip> readClipboard(DWORD sequence)
{
    Clip c;
    c.sequence = sequence;
    bool open = false;
    for (int tries = 0; tries < 5 && !open; ++tries) {
        open = ::OpenClipboard(nullptr) != 0;
        if (!open)
            ::Sleep(10);
    }
    if (!open)
        return std::nullopt; // busy (another program writing it): read next time
    c.read = true;
    if (const HANDLE h = ::GetClipboardData(CF_HDROP)) {
        const HDROP drop = static_cast<HDROP>(h);
        const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count && i < 1000; ++i) {
            const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            ::DragQueryFileW(drop, i, path.data(), length + 1);
            path.resize(length);
            if (!path.empty())
                c.files.push_back(std::move(path));
        }
    }
    if (c.files.empty()) {
        if (const HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* p = static_cast<const wchar_t*>(::GlobalLock(h))) {
                const std::size_t max = ::GlobalSize(h) / sizeof(wchar_t);
                c.text = narrow(std::wstring(p, ::wcsnlen(p, std::min<std::size_t>(max, 512 * 1024))));
                ::GlobalUnlock(h);
            }
        }
    }
    ::CloseClipboard();
    return c;
}

struct Options {
    std::string alias;
    std::wstring folder; // "" for Downloads
    bool receive = true;
    std::string pin;
    ReceiveNetworks networks = ReceiveNetworks::Trusted;
};

bool boolSetting(const Json& value, bool fallback)
{
    if (value.isBool())
        return value.toBool();
    if (value.isString())
        return value.toString() == "true" || value.toString() == "1";
    return fallback;
}

Options parseOptions(const Json& s)
{
    Options o;
    o.alias = cutUtf8(validUtf8(trimmed(s["alias"].toString())), 64); // never a character cut in two
    o.folder = expandEnv(wide(trimmed(s["folder"].toString())));
    o.receive = boolSetting(s["receive"], true);
    o.pin = trimmed(s["pin"].toString());
    o.networks = s["networks"].toString() == "all" ? ReceiveNetworks::All : ReceiveNetworks::Trusted;
    return o;
}

struct NetState {
    bool evaluated = false;
    std::vector<DiscoveryNetwork> networks; // the usable ones
    int port = 0; // receiving on it; 0: not receiving
    std::vector<std::string> listening;
    bool anyNetwork = false; // connected at all
};

// What of a NetState the rows show (the hints, the status row): when it is
// another, the launcher is told to ask again.
std::string shownOf(const NetState& s)
{
    std::vector<std::string> listening = s.listening;
    std::sort(listening.begin(), listening.end());
    std::string out = std::string(s.evaluated ? "e" : "-") + (s.anyNetwork ? "n" : "-") + std::to_string(s.port) + '\x1e';
    for (const std::string& ip : listening)
        out += ip + '\x1f';
    out += '\x1e';
    for (const DiscoveryNetwork& n : s.networks)
        out += n.link.networkId + '\x1f' + n.link.networkName + '\x1f' + n.link.name + (n.receiving ? "\x1f+" : "\x1f-") + '\x1e';
    return out;
}

struct TextRow {
    std::uint64_t id = 0;
    std::string sender;
    std::string text;
    Clock::time_point at;
};

struct SendOutcomeRow {
    SendOutcome outcome;
    Clock::time_point at;
};

// The last send started: Enter on the row after it sends the same again.
struct LastSend {
    Outgoing what;
    std::string fingerprint; // to whom
    std::uint64_t selection = 0; // the files chosen in the results, when it was those
    Clock::time_point at; // when it started: an outcome before this is an earlier send's
};

// What Enter on a device sends, and how the rows say it.
struct Payload {
    enum class Source { None, Words, Menu, Clipboard };
    Source source = Source::None;
    Outgoing what;
    std::string title; // "报告.docx、照片.jpg 等 5 项" or "文字「晚上七点见」"
    std::string amount; // a device's row: "12.6 MB" or "12 个字"
    std::string enter; // a device's row: "Enter 发送剪贴板里的 3 个文件"
    std::string detail; // with no device: "共 3 个文件 · 12.6 MB · 剪贴板里复制的"
    // What it is, in short (a digest of the words or the paths): a device's
    // row carries it, and Enter sends only when it is still the same (the
    // clipboard or the words may have changed since the row was drawn).
    std::string key;
    std::uint64_t selection = 0; // Menu: which choice of files from the results it is
};

class LocalSendPlugin final : public winshun::Plugin {
public:
    ~LocalSendPlugin() override { stop(); }

protected:
    std::string name() const override { return "WinShunLocalSend"; }
    void initialized() override { start(); }
    // The host ends the process right after (it waits about 2 s): partial
    // files go first. A transfer coming in is cancelled, its partial files
    // deleted; a device we send to is told, so that it deletes its own.
    void shuttingDown() override
    {
        m_clipReader.stop();
        m_counter.stop();
        m_folderCheck.stop();
        if (m_receiver)
            m_receiver->shutDown(500ms);
        SendJob* job = nullptr; // m_job is only replaced on this thread (actions)
        {
            const std::lock_guard lock(m_mutex);
            job = m_job.get();
        }
        if (job)
            job->shutDown(700ms);
        if (m_discovery)
            m_discovery->stop();
    }
    Items query(const winshun::Query&) override { return {}; } // queryResult answers
    Json queryResult(const winshun::Query& q) override;
    Effects action(const winshun::ActionRequest& a) override;
    void settingsChanged(const Json& values) override;

private:
    std::string t(const char* zh, const char* en) const { return chinese() ? zh : en; }

    void start();
    void stop();
    void houseLoop();
    void evaluate(); // on the house thread (COM)
    void evaluateNow(); // asks the house thread, waits a moment for it
    void recheckNow(); // evaluateNow() and the firewall read again
    void loadAllowedNetworks();
    void saveAllowedNetworks();
    DeviceInfo self(int port) const; // m_mutex held
    std::wstring saveFolder() const; // m_mutex held

    void onAsked(const IncomingRequest& r);
    void onText(const TextMessage& m);
    void onFinished(const ReceiveOutcome& o);
    void onSent(const SendOutcome& o, std::uint64_t selection);

    Items rows(const std::string& words);
    // `describe`: the title and the detail too (a look at the files); the key alone without.
    Payload payload(const std::string& words, bool describe = true);
    // What the clipboard holds, read on its own thread and waited for up to
    // `wait`; meanwhile the last one read (clipboardCurrent() says which).
    const Clip& clipboard(std::chrono::milliseconds wait = 300ms);
    bool clipboardCurrent() const;
    std::optional<Summary> summaryOf(const std::vector<std::wstring>& paths); // nothing: still counting
    // `clipText`: the clipboard's text offered too (its textKey), "" for none;
    // `sending`: a send of ours is under way.
    Item deviceRow(const Device& d, const Payload& p, const std::string& clipText, bool sending) const;
    // Receiving's rows: what keeps it from working (`problem`: one row), or
    // the status row with, after it, a network it does not work on.
    Items receiveRows(bool* problem);
    Effects startSend(const Device& device, const Target& target, Outgoing what, std::uint64_t selection);
    std::string folderLabel(const std::wstring& folder) const;
    std::string ago(Clock::time_point at) const;
    std::string filesText(std::size_t files) const;
    Effects refreshed(const std::string& words, Effects before = {});
    std::string keywordSpace() const;

    // Set at start, then only read (the threads that use them end in stop()).
    std::wstring m_dataFolder;
    std::unique_ptr<Identity> m_identity;
    std::unique_ptr<Credentials> m_serverCredentials;
    std::unique_ptr<Credentials> m_clientCredentials;
    std::unique_ptr<Client> m_client;
    std::unique_ptr<Discovery> m_discovery;
    std::unique_ptr<Receiver> m_receiver;
    std::string m_startProblem; // the identity or TLS could not be had: the rows say so
    std::wstring m_exe;

    mutable std::mutex m_mutex; // what follows
    Options m_options;
    std::map<std::string, std::string> m_allowed; // public networks the user receives on: NLM id -> name
    NetState m_net;
    FirewallFacts m_firewall;
    std::deque<TextRow> m_texts;
    LastReceived m_received; // the 收到了 row
    std::optional<SendOutcomeRow> m_sendOutcome;
    std::optional<LastSend> m_lastSend;
    std::unique_ptr<SendJob> m_job;
    std::vector<std::wstring> m_menuFiles;
    Clock::time_point m_menuFilesAt;
    std::uint64_t m_menuSelection = 0; // counts the choices of m_menuFiles
    std::string m_keyword = "传";

    Window m_textToasts {5, std::chrono::minutes(1)}; // m_mutex: texts from many devices ring at most this often

    // The serve() thread's own (queries and actions).
    Clip m_clip;
    Clock::time_point m_lastQuery;
    Clock::time_point m_lastLook;

    // What the serve thread never waits on (a share that sleeps, a program
    // slow to give what it copied): each on a thread of its own, 传 asked
    // again when it is known.
    Background<Clip> m_clipReader {[this] { refresh(); }};
    Background<Summary> m_counter {[this] { refresh(); }};
    Background<bool> m_folderCheck {[this] { refresh(); }}; // can files go where they are saved

    // The house thread: networks, the firewall, the receiver's clock.
    std::thread m_house;
    std::mutex m_houseMutex;
    std::condition_variable m_houseWake;
    std::condition_variable m_houseDone;
    bool m_houseStop = false;
    bool m_evaluateWanted = false;
    bool m_firewallWanted = false;
    std::uint64_t m_evaluations = 0;
    std::uint64_t m_firewallReads = 0;
    HANDLE m_ipNotify = nullptr;
};

// ---- start, stop, the house thread ---------------------------------------------

void LocalSendPlugin::start()
{
    m_exe = exePath();
    m_dataFolder = wide(dataDir());
    if (m_dataFolder.empty())
        m_dataFolder = m_exe.substr(0, m_exe.find_last_of(L'\\'));
    ensureFolder(m_dataFolder);
    {
        const std::lock_guard lock(m_mutex);
        m_options = parseOptions(settings());
    }
    loadAllowedNetworks();
    std::string error;
    m_identity = Identity::loadOrCreate(m_dataFolder, &error);
    if (m_identity) {
        m_serverCredentials = Credentials::acquire(*m_identity, Credentials::Role::Server, &error);
        if (m_serverCredentials)
            m_clientCredentials = Credentials::acquire(*m_identity, Credentials::Role::Client, &error);
    }
    if (!m_clientCredentials) {
        m_startProblem = error;
        logWarning("not started: " + error);
        return;
    }
    m_client = std::make_unique<Client>(*m_clientCredentials);
    // A device found or gone, a search over: 传, when open, asks again now.
    m_discovery = std::make_unique<Discovery>(*m_client, [this] { refresh(); });
    Receiver::Events events;
    events.asked = [this](const IncomingRequest& r) { onAsked(r); };
    events.text = [this](const TextMessage& m) { onText(m); };
    events.finished = [this](const ReceiveOutcome& o) { onFinished(o); };
    events.registered = [this](const DeviceInfo& d, const std::string& ip, bool verified) {
        if (m_discovery)
            m_discovery->heard(d, ip, verified);
    };
    m_receiver = std::make_unique<Receiver>(*m_serverCredentials, std::move(events));
    logInfo(std::string("started: identity ") + (m_identity->created() ? "new" : "kept") + ", TLS "
        + (m_serverCredentials->legacy() ? "1.2" : "1.2 and 1.3"));
    m_house = std::thread([this] { houseLoop(); });
}

void LocalSendPlugin::stop()
{
    // Nothing more told from the workers (they outlive this object).
    m_clipReader.stop();
    m_counter.stop();
    m_folderCheck.stop();
    // The receiver first: its partial files go before anything else is waited for.
    if (m_receiver)
        m_receiver->shutDown(2s);
    {
        const std::lock_guard lock(m_houseMutex);
        m_houseStop = true;
    }
    m_houseWake.notify_all();
    if (m_house.joinable())
        m_house.join(); // it uses the receiver
    m_receiver.reset();
    std::unique_ptr<SendJob> job;
    {
        const std::lock_guard lock(m_mutex);
        job = std::move(m_job);
    }
    if (job)
        job->shutDown(2s); // the device told, then waited for
    job.reset();
    m_discovery.reset();
    m_client.reset();
}

void LocalSendPlugin::houseLoop()
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // An address came or went (Wi-Fi joined, cable out, hotspot on): looked at now.
    ::NotifyIpInterfaceChange(AF_INET, [](PVOID context, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
        auto* self = static_cast<LocalSendPlugin*>(context);
        {
            const std::lock_guard lock(self->m_houseMutex);
            self->m_evaluateWanted = true;
        }
        self->m_houseWake.notify_all();
    }, this, FALSE, &m_ipNotify);
    guarded("sweeping partial files", [&] {
        // Before receiving starts: what a transfer left when the process was ended.
        std::wstring folder;
        {
            const std::lock_guard state(m_mutex);
            folder = saveFolder();
        }
        if (const std::size_t gone = sweepPartials(folder))
            logInfo(std::to_string(gone) + " partial file(s) left by an earlier run deleted");
    });
    Clock::time_point nextEvaluation {};
    Clock::time_point nextFirewall {};
    std::unique_lock lock(m_houseMutex);
    while (!m_houseStop) {
        const bool evaluateWanted = m_evaluateWanted;
        const bool firewallWanted = m_firewallWanted;
        m_evaluateWanted = m_firewallWanted = false;
        lock.unlock();
        const Clock::time_point now = Clock::now();
        const bool evaluated = evaluateWanted || now >= nextEvaluation;
        bool firewallRead = false;
        // One round's trouble (no memory, say) does not end the thread: the next round tries again.
        guarded("the house round", [&] {
            if (m_receiver)
                m_receiver->tick();
            if (evaluated) {
                nextEvaluation = Clock::now() + kEvaluateEvery;
                evaluate();
            }
            if (firewallWanted || now >= nextFirewall) {
                firewallRead = true;
                nextFirewall = Clock::now() + kFirewallEvery;
                const FirewallFacts facts = firewallFacts(m_exe);
                bool changed = false;
                {
                    const std::lock_guard state(m_mutex);
                    changed = facts.known != m_firewall.known || facts.on != m_firewall.on || facts.allowed != m_firewall.allowed
                        || facts.blocked != m_firewall.blocked;
                    m_firewall = facts;
                }
                if (changed)
                    refresh(); // the firewall's hint came or went
            }
        });
        lock.lock();
        if (evaluated)
            ++m_evaluations;
        if (firewallRead)
            ++m_firewallReads;
        m_houseDone.notify_all();
        if (!m_houseStop && !m_evaluateWanted && !m_firewallWanted)
            m_houseWake.wait_for(lock, 1s);
    }
    lock.unlock();
    if (m_ipNotify)
        ::CancelMibChangeNotify2(m_ipNotify);
    if (SUCCEEDED(com))
        ::CoUninitialize();
}

void LocalSendPlugin::evaluateNow()
{
    std::unique_lock lock(m_houseMutex);
    const std::uint64_t before = m_evaluations;
    m_evaluateWanted = true;
    m_houseWake.notify_all();
    m_houseDone.wait_for(lock, 2s, [&] { return m_evaluations > before || m_houseStop; });
}

void LocalSendPlugin::recheckNow()
{
    std::unique_lock lock(m_houseMutex);
    const std::uint64_t evaluations = m_evaluations;
    const std::uint64_t firewallReads = m_firewallReads;
    m_evaluateWanted = m_firewallWanted = true;
    m_houseWake.notify_all();
    m_houseDone.wait_for(lock, 2s, [&] { return (m_evaluations > evaluations && m_firewallReads > firewallReads) || m_houseStop; });
}

void LocalSendPlugin::evaluate()
{
    const std::vector<Interface> all = currentInterfaces();
    Options o;
    std::set<std::string> allowed;
    {
        const std::lock_guard lock(m_mutex);
        o = m_options;
        for (const auto& [id, name] : m_allowed)
            allowed.insert(id);
    }
    NetState s;
    s.evaluated = true;
    std::vector<ListenAddress> listen;
    for (const Interface& i : all) {
        if (!usable(i))
            continue;
        const bool receiving = o.receive && receiveAllowed(i, o.networks, allowed);
        s.networks.push_back({i, receiving});
        if (receiving)
            listen.push_back({i.ip, i.address, i.prefix});
    }
    s.anyNetwork = !s.networks.empty();
    const std::vector<std::string> before = m_receiver->addresses();
    const int portBefore = m_receiver->port();
    bool sending = false;
    {
        const std::lock_guard lock(m_mutex);
        sending = m_job && !m_job->finished();
    }
    // Back to 53317 (LocalSend on this PC let it go) only between transfers:
    // a device in the middle of one knows us by the port we had.
    const bool idle = !sending && !m_receiver->pending() && !m_receiver->progress();
    s.port = m_receiver->listen(listen, kPort, idle);
    s.listening = m_receiver->addresses();
    ReceiverConfig config;
    DeviceInfo me;
    bool shownChanged = false;
    {
        const std::lock_guard lock(m_mutex);
        me = self(s.port);
        config.self = me;
        config.pin = o.pin;
        config.folder = saveFolder();
        shownChanged = shownOf(m_net) != shownOf(s);
        m_net = s;
    }
    m_receiver->setConfig(config);
    m_discovery->setSelf(me);
    m_discovery->setNetworks(s.networks);
    std::vector<std::string> sortedBefore = before, sortedNow = s.listening;
    std::sort(sortedBefore.begin(), sortedBefore.end());
    std::sort(sortedNow.begin(), sortedNow.end());
    if (sortedNow != sortedBefore || s.port != portBefore) { // a new port alone too (LocalSend on this PC took ours)
        logInfo("receiving on " + std::to_string(s.listening.size()) + " network(s), port " + std::to_string(s.port));
        if (s.port)
            m_discovery->announceReceiving(); // the devices nearby learn of us now
    }
    if (shownChanged)
        refresh(); // a hint or the status row is another now
}

DeviceInfo LocalSendPlugin::self(int port) const
{
    DeviceInfo d;
    d.alias = m_options.alias.empty() ? computerName() : m_options.alias;
    d.version = kProtocolVersion;
    d.deviceModel = "Windows";
    d.deviceType = "desktop";
    d.fingerprint = m_identity ? m_identity->fingerprint() : std::string();
    d.port = port ? port : kPort;
    d.protocol = "https";
    d.download = false;
    return d;
}

std::wstring LocalSendPlugin::saveFolder() const
{
    return m_options.folder.empty() ? downloadsFolder() : m_options.folder;
}

void LocalSendPlugin::settingsChanged(const Json& values)
{
    {
        const std::lock_guard lock(m_mutex);
        m_options = parseOptions(values);
    }
    if (m_receiver) {
        evaluateNow();
        refresh(); // the name, the folder or the PIN on the status row
    }
}

void LocalSendPlugin::loadAllowedNetworks()
{
    const std::optional<Json> json = Json::parse(readTextFile(m_dataFolder + L"\\networks.json"));
    const std::lock_guard lock(m_mutex);
    m_allowed.clear();
    if (!json)
        return;
    for (const Json& n : (*json)["allowed"].toArray()) {
        if (n["id"].isString() && !n["id"].toString().empty() && m_allowed.size() < 64)
            m_allowed[n["id"].toString()] = n["name"].toString();
    }
}

void LocalSendPlugin::saveAllowedNetworks()
{
    Json::Array list;
    {
        const std::lock_guard lock(m_mutex);
        for (const auto& [id, name] : m_allowed)
            list.push_back(Json::Object {{"id", id}, {"name", name}});
    }
    Json j;
    j.set("allowed", Json(std::move(list)));
    if (!writeTextFile(m_dataFolder + L"\\networks.json", j.dump()))
        logWarning("networks.json could not be written");
}

// ---- what the receiver tells -------------------------------------------------------

std::string LocalSendPlugin::keywordSpace() const
{
    const std::lock_guard lock(m_mutex);
    return m_keyword + " ";
}

void LocalSendPlugin::onAsked(const IncomingRequest& r)
{
    logInfo("asked: " + std::to_string(r.files.size()) + " file(s), " + std::to_string(r.totalBytes) + " bytes"
        + (r.verified ? "" : ", sender unverified"));
    refresh(); // the receiver had it stored for pending() before it told
    // Who, how many, how much: never the names (a notification stays in the
    // Action Center). Our words first, the name after them and cut short
    // (titled()): a name never reads as the start of the title.
    const std::string title = titled(chinese() ? "想发给你 " + filesText(r.files.size()) : "Wants to send " + filesText(r.files.size()),
        r.sender.alias);
    const std::string text = chinese() ? "共 " + sizeText(r.totalBytes) + " · 点这里查看，2 分钟内不答就拒绝"
                                       : sizeText(r.totalBytes) + " · Click to see; declined after 2 minutes without an answer";
    toast(title, text, {effect::setQuery(keywordSpace())});
}

void LocalSendPlugin::onText(const TextMessage& m)
{
    logInfo("text message: " + std::to_string(characters(m.text)) + " characters");
    bool ring = false;
    {
        const std::lock_guard lock(m_mutex);
        m_texts.push_front({m.id, m.sender.alias, m.text, Clock::now()});
        while (m_texts.size() > kMaxTexts)
            m_texts.pop_back();
        // Each device may send one every few seconds; many devices together
        // ring only so often (the rows have all of them).
        ring = m_textToasts.allow(Clock::now());
    }
    refresh();
    if (!ring) {
        logInfo("text message: no notification (too many lately)");
        return;
    }
    // The click opens 传, where the text's row copies it (Enter): a click
    // never puts a peer's words on the clipboard by itself.
    const std::string keyword = trimmed(keywordSpace());
    const std::string text = chinese() ? std::to_string(characters(m.text)) + " 个字 · 点这里在「" + keyword + "」里查看，Enter 复制"
                                       : std::to_string(characters(m.text)) + " characters · Click to see it in " + keyword + "; Enter copies";
    toast(titled(t("发来一段文字", "Sent you text"), m.sender.alias), text, {effect::setQuery(keywordSpace())});
}

void LocalSendPlugin::onFinished(const ReceiveOutcome& o)
{
    logInfo("receive ended: kind " + std::to_string(static_cast<int>(o.kind)) + ", " + std::to_string(o.saved.size()) + " saved, "
        + std::to_string(o.failed) + " not");
    {
        // Only a batch that left files becomes the 收到了 row (LastReceived).
        const std::lock_guard lock(m_mutex);
        m_received.finished(o, Clock::now());
    }
    refresh(); // the request or progress row is gone (ended before it was told)
    const std::string who = o.sender.alias;
    const std::string where = folderLabel(o.folder);
    switch (o.kind) {
    case ReceiveOutcome::Kind::Received:
        if (o.failed == 0) {
            const bool opens = toastOpens(o.folder); // a share: 传 instead (toastOpens)
            toast(titled(chinese() ? "收到了 " + filesText(o.saved.size()) : "Received " + filesText(o.saved.size()), who),
                chinese() ? "共 " + sizeText(o.bytes) + "，存在「" + where + "」· " + (opens ? "点这里打开文件夹" : "点这里查看")
                          : sizeText(o.bytes) + ", saved in " + where + " · " + (opens ? "Click to open the folder" : "Click to see"),
                {opens ? effect::openPath(narrow(o.folder)) : effect::setQuery(keywordSpace())});
            break;
        }
        [[fallthrough]];
    case ReceiveOutcome::Kind::Failed: {
        // Why, as far as it is known: this PC's disk, what came, or the other side.
        const char* zhWhy = o.fault == Fault::Disk ? "（存不下：盘满了，或者文件写不进去）"
            : o.fault == Fault::Checksum           ? "（收到的内容和对方发的对不上）"
                                                   : "（对方取消了，或者网络断了）";
        const char* enWhy = o.fault == Fault::Disk ? " (could not be saved: the disk is full, or the files could not be written)"
            : o.fault == Fault::Checksum           ? " (what came is not what was sent)"
                                                   : " (cancelled, or the network dropped)";
        toast(titled(t("没收完文件", "Not all files arrived"), who),
            chinese() ? "收到 " + std::to_string(o.saved.size()) + " 个，" + std::to_string(o.failed) + " 个没收完" + zhWhy + "。没收完的已删除"
                      : std::to_string(o.saved.size()) + " arrived, " + std::to_string(o.failed) + " did not" + enWhy
                    + ". The partial ones were deleted",
            o.saved.empty() || !toastOpens(o.folder) ? Effects {effect::setQuery(keywordSpace())} : Effects {effect::openPath(narrow(o.folder))});
        break;
    }
    case ReceiveOutcome::Kind::TimedOut:
        toast(titled(t("没有接收文件", "Files declined"), who),
            chinese() ? "2 分钟没有回答，已经拒绝了。要收的话，请对方再发一次" : "No answer in 2 minutes. Ask them to send again");
        break;
    case ReceiveOutcome::Kind::SenderCancelled:
        toast(titled(t("对方取消了发送", "Cancelled by the sender"), who), chinese() ? "没收完的文件已删除" : "Partial files were deleted");
        break;
    case ReceiveOutcome::Kind::NoSpace:
        toast(titled(t("空间不够，没有接收", "Not enough space"), who),
            chinese() ? "要 " + sizeText(o.bytes) + "，「" + where + "」所在的盘要留出这么多，再加 1 GB"
                      : "They need " + sizeText(o.bytes) + " on the drive of " + where + ", plus 1 GB to spare");
        break;
    case ReceiveOutcome::Kind::NoFolder:
        // Where files go cannot be reached (a disk pulled out, a share gone):
        // refused before the question, else each file would fail once accepted.
        // 传 has a row about it whose Enter opens the settings.
        toast(titled(t("保存位置不可用，没有接收", "Save location unavailable"), who),
            chinese() ? "「" + where + "」现在打不开（盘拔了，或者网络断了）· 点这里查看"
                      : where + " cannot be reached now (a disk pulled out, a share gone) · Click to see",
            {effect::setQuery(keywordSpace())});
        break;
    case ReceiveOutcome::Kind::Declined:
    case ReceiveOutcome::Kind::Cancelled:
        break; // the user's own doing: nothing to tell
    }
}

void LocalSendPlugin::onSent(const SendOutcome& o, std::uint64_t selection)
{
    logInfo("send ended: kind " + std::to_string(static_cast<int>(o.kind)) + ", " + std::to_string(o.files) + " file(s)"
        + (o.detail.empty() ? "" : " (" + o.detail + ")"));
    {
        const std::lock_guard lock(m_mutex);
        m_sendOutcome = SendOutcomeRow {o, Clock::now()};
        // Sent: not offered again. Only when it was these files (not the
        // clipboard's), and nothing else was chosen in the results meanwhile.
        if (o.kind == SendOutcome::Kind::Sent && selection != 0 && selection == m_menuSelection)
            m_menuFiles.clear();
    }
    refresh(); // the job shows no row once finished, before this is called
    const std::string& who = o.device;
    const bool zh = chinese();
    const Effects look = {effect::setQuery(keywordSpace())};
    switch (o.kind) {
    case SendOutcome::Kind::Sent:
        toast(titled(zh ? "对方收下了 " + filesText(o.files) : "Took " + filesText(o.files), who), zh ? "共 " + sizeText(o.bytes) : sizeText(o.bytes));
        break;
    case SendOutcome::Kind::TextSent:
        toast(titled(t("对方收到了文字", "Got the text"), who), t("已经发过去了", "It was sent"));
        break;
    case SendOutcome::Kind::Declined:
        toast(titled(t("对方没有接收", "Declined"), who), t("对方点了拒绝", "They said no"));
        break;
    case SendOutcome::Kind::Busy:
        toast(titled(t("对方正忙", "Busy"), who), t("对方正在收别的文件，等一会儿再发", "It is receiving something else; try again in a moment"));
        break;
    case SendOutcome::Kind::Paused:
        // Win顺 there: it does not ask again right after a no (or takes one text in a few seconds).
        toast(titled(o.text ? t("对方刚收到过文字", "Just got a text") : t("对方刚拒绝过", "Just said no"), who),
            o.text ? t("等几秒再发", "Send again in a few seconds") : t("等一会儿再发", "Send again in a moment"));
        break;
    case SendOutcome::Kind::NoAnswer:
        toast(titled(t("对方没有回应", "No answer"), who), t("3 分钟没人点接收，已经不发了", "Nobody accepted in 3 minutes, so it was not sent"));
        break;
    case SendOutcome::Kind::PinRefused:
        toast(titled(t("没有发出", "Not sent"), who), t("对方要接收密码，没有输入", "It wanted a PIN and none was typed"));
        break;
    case SendOutcome::Kind::TooManyPins:
        toast(titled(t("密码错了太多次", "Too many wrong PINs"), who),
            t("对方暂时不让这台电脑再试，等一会儿再发", "It will not let this PC try again for a while"));
        break;
    case SendOutcome::Kind::Unreachable:
        toast(titled(t("连不上", "Could not reach"), who),
            t("对方还开着 LocalSend 吗？要和这台电脑连着同一个网络", "Is LocalSend still open there, on the same network?"), look);
        break;
    case SendOutcome::Kind::NotTheDevice:
        toast(titled(t("没有发出", "Not sent"), who),
            t("那个地址上现在是别的设备，为了安全没有发。再找一次设备试试",
                "Another device is at that address now, so nothing was sent, to be safe. Look for devices again"),
            look);
        break;
    case SendOutcome::Kind::Interrupted:
        toast(titled(t("没发完", "Not all sent"), who),
            zh ? "发了 " + std::to_string(o.files) + " 个就断了（对方取消了，或者网络断了）"
               : std::to_string(o.files) + " went, then it stopped (cancelled there, or the network dropped)",
            look);
        break;
    case SendOutcome::Kind::NoRoom:
        toast(titled(t("对方存不下", "No room there"), who), t("对方的空间不够，或者保存位置不可用", "Not enough space there, or where it saves cannot be reached"));
        break;
    case SendOutcome::Kind::TooMany:
        toast(titled(t("对方一次收不了这么多文件", "Too many files at once"), who), t("分几次发", "Send them in a few goes"));
        break;
    case SendOutcome::Kind::Nothing:
        toast(chinese() ? "没有可发的文件" : "Nothing to send",
            chinese() ? "选的文件夹是空的，或者文件已经不在了；一次最多 10,000 个文件"
                      : "The folders are empty or the files are gone; at most 10,000 files at a time");
        break;
    case SendOutcome::Kind::Cancelled:
        break;
    }
}

// ---- rows --------------------------------------------------------------------------

std::string LocalSendPlugin::filesText(std::size_t files) const
{
    return chinese() ? std::to_string(files) + " 个文件" : std::to_string(files) + (files == 1 ? " file" : " files");
}

std::string LocalSendPlugin::folderLabel(const std::wstring& folder) const
{
    return folderWords(folder, downloadsFolder(), chinese()); // "下载", or the folder's own name
}

std::string LocalSendPlugin::ago(Clock::time_point at) const
{
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(Clock::now() - at).count();
    if (minutes < 1)
        return t("刚才", "just now");
    return chinese() ? std::to_string(minutes) + " 分钟前" : std::to_string(minutes) + " min ago";
}

const Clip& LocalSendPlugin::clipboard(std::chrono::milliseconds wait)
{
    const DWORD sequence = ::GetClipboardSequenceNumber();
    if (m_clip.read && m_clip.sequence == sequence)
        return m_clip;
    // A clipboard's content never changes under its number: kept for ever.
    if (std::optional<Clip> c = m_clipReader.get(std::to_string(sequence), [sequence] { return readClipboard(sequence); },
            Clock::duration::max(), wait))
        m_clip = std::move(*c);
    return m_clip;
}

bool LocalSendPlugin::clipboardCurrent() const
{
    return m_clip.read && m_clip.sequence == ::GetClipboardSequenceNumber();
}

std::optional<Summary> LocalSendPlugin::summaryOf(const std::vector<std::wstring>& paths)
{
    std::wstring all;
    for (const std::wstring& p : paths)
        all += p + L"|";
    const std::string key = sha256Hex(all.data(), all.size() * sizeof(wchar_t));
    // Counted anew after a while (the files may have changed); the last count meanwhile.
    return m_counter.get(key, [paths] { return std::optional<Summary>(summarize(paths, 1s)); }, 5s, 150ms);
}

Payload LocalSendPlugin::payload(const std::string& words, bool describe)
{
    Payload p;
    const std::string text = trimmed(words);
    if (!text.empty()) {
        p.source = Payload::Source::Words;
        p.what.kind = Outgoing::Kind::Text;
        p.what.text = text;
        p.title = chinese() ? "文字「" + excerpt(text, 20) + "」" : "the text \"" + excerpt(text, 20) + "\"";
        p.amount = chinese() ? std::to_string(characters(text)) + " 个字" : std::to_string(characters(text)) + " characters";
        p.enter = t("Enter 发送这段文字", "Enter sends this text");
        p.detail = p.amount;
        p.key = "w" + sha256Hex(text.data(), text.size()).substr(0, 16);
        return p;
    }
    std::vector<std::wstring> paths;
    {
        const std::lock_guard lock(m_mutex);
        if (!m_menuFiles.empty() && Clock::now() - m_menuFilesAt > kKeepMenuFiles)
            m_menuFiles.clear();
        paths = m_menuFiles;
        p.selection = m_menuSelection;
    }
    p.source = paths.empty() ? Payload::Source::None : Payload::Source::Menu;
    if (paths.empty()) {
        p.selection = 0;
        paths = clipboard().files;
        if (!paths.empty())
            p.source = Payload::Source::Clipboard;
    }
    if (paths.empty())
        return p;
    p.what.kind = Outgoing::Kind::Files;
    p.what.paths = paths;
    {
        std::wstring all;
        for (const std::wstring& path : paths)
            all += path + L'|';
        p.key = (p.source == Payload::Source::Menu ? "m" : "c") + sha256Hex(all.data(), all.size() * sizeof(wchar_t)).substr(0, 16);
    }
    if (!describe)
        return p;
    // The names from the paths alone: the row and its Enter are there while
    // the files are counted (on m_counter's thread).
    std::string names;
    for (std::size_t i = 0; i < paths.size() && i < 2; ++i)
        names += (i ? t("、", ", ") : "") + shownName(paths[i]);
    if (paths.size() > 2)
        names += chinese() ? " 等 " + std::to_string(paths.size()) + " 项" : " and " + std::to_string(paths.size() - 2) + " more";
    p.title = names;
    const bool menu = p.source == Payload::Source::Menu;
    const std::string from = menu ? t("从搜索结果选的", "chosen in the results") : t("剪贴板里复制的", "copied to the clipboard");
    const std::optional<Summary> s = summaryOf(paths);
    if (!s) {
        p.amount = t("正在统计…", "Counting…");
        p.enter = chinese() ? (menu ? "Enter 发送选中的文件" : "Enter 发送剪贴板里的文件") : (menu ? "Enter sends the files chosen" : "Enter sends the files copied");
        p.detail = p.amount + " · " + from;
        return p;
    }
    if (s->remote == paths.size()) {
        // All on shares, not looked at (a NAS that sleeps): how many, no size.
        const std::string items = chinese() ? std::to_string(paths.size()) + " 项" : std::to_string(paths.size()) + (paths.size() == 1 ? " item" : " items");
        p.amount = t("在网络位置上", "on a network share");
        p.enter = chinese() ? (menu ? "Enter 发送选中的 " : "Enter 发送剪贴板里的 ") + items : "Enter sends the " + items + (menu ? " chosen" : " copied");
        p.detail = (chinese() ? "共 " : "") + items + " · " + p.amount + " · " + from;
        return p;
    }
    const std::string count = s->partial ? (chinese() ? "至少 " : "at least ") : "";
    p.amount = count + sizeText(s->bytes);
    p.enter = chinese() ? (menu ? "Enter 发送选中的" : "Enter 发送剪贴板里的") + (s->partial ? "至少 " : std::string(" ")) + filesText(s->files)
                        : "Enter sends " + (s->partial ? std::string("at least ") : std::string("the ")) + filesText(s->files)
            + (menu ? " chosen in the results" : " copied");
    p.detail = (chinese() ? "共 " : "") + count + filesText(s->files) + " · " + count + sizeText(s->bytes) + " · " + from;
    return p;
}

Item LocalSendPlugin::deviceRow(const Device& d, const Payload& p, const std::string& clipText, bool sending) const
{
    // What it is and what Enter does; no address (a phone's user has no use for it).
    const std::string kind = deviceWords(d.info, chinese());
    Item row("dev-" + d.info.fingerprint, d.info.alias); // all of it: two devices never share a row's id
    std::vector<winshun::Action> actions {{"send", t("发送", "Send"), "Enter"}};
    if (p.source != Payload::Source::None) {
        row.title = chinese() ? "发给 " + d.info.alias + "：" + p.title : "Send " + p.title + " to " + d.info.alias;
        row.subtitle = kind + " · " + p.amount + " · " + p.enter;
    } else if (sending) {
        // One send at a time (and a PIN may be typed after 传 now: not words to send).
        row.subtitle = kind + t(" · 等这次发完再发", " · Wait until this send is done");
    } else {
        // Nothing to send yet: Enter says how, in the footer.
        row.subtitle = kind + (chinese() ? " · 先复制文件，或在「" + m_keyword + "」后面打字" : " · Copy files, or type after " + m_keyword);
    }
    if (!clipText.empty())
        actions.push_back({"sendClipText", t("把剪贴板里的文字发过去", "Send the clipboard's text"), "Ctrl+Enter"});
    const std::string& type = d.info.deviceType;
    // "what" and "clip": what Enter and Ctrl+Enter send, checked at the key (the clipboard may change meanwhile).
    row.withIcon(type == "mobile" ? "glyph:U+E8EA" : type == "web" ? "glyph:U+E774" : (type == "headless" || type == "server") ? "glyph:U+E968" : "glyph:U+E7F4")
        .withActions(std::move(actions))
        .withData(Json::Object {{"fp", d.info.fingerprint}, {"what", p.key}, {"clip", clipText}});
    return row;
}

Items LocalSendPlugin::receiveRows(bool* problem)
{
    *problem = true;
    Options o;
    NetState net;
    FirewallFacts firewall;
    std::map<std::string, std::string> allowed;
    {
        const std::lock_guard lock(m_mutex);
        o = m_options;
        net = m_net;
        firewall = m_firewall;
        allowed = m_allowed;
    }
    if (!net.evaluated)
        return {};
    const std::string alias = o.alias.empty() ? computerName() : o.alias;
    if (!o.receive) {
        return {Item("hint-off", t("接收已关闭", "Receiving is off"),
            t("插件设置里打开「接收文件」，附近的设备才能发过来 · Enter 打开设置",
                "Turn on receiving in the plugin's settings so that devices nearby can send · Enter opens them"))
                .withIcon("glyph:U+E946")
                .withActions({{"settings", t("打开设置", "Open settings"), "Enter"}})};
    }
    // Each hint's Enter does something (the launcher has no row that cannot be
    // chosen): what can be set, or one more look once it is put right. Opening
    // Windows' own settings would take the "processes" permission.
    if (!net.anyNetwork) {
        return {Item("hint-nonet", t("没有连上网络", "Not connected"),
            t("连上 Wi-Fi 或网线以后，附近的设备才能找到这台电脑 · Enter 再看一次",
                "Join a Wi-Fi or plug in a cable so that devices nearby can find this PC · Enter looks again"))
                .withIcon("glyph:U+E7BA")
                .withActions({{"recheck", t("再看一次", "Look again"), "Enter"}})};
    }
    // A network Windows calls public, where receiving waits for the user's
    // word: the trouble when it is the only one; with another that receives
    // (Wi-Fi, and a public cable), a row after the status row.
    const DiscoveryNetwork* closed = nullptr;
    bool receiving = false;
    for (const DiscoveryNetwork& n : net.networks) {
        if (n.receiving)
            receiving = true;
        else if (!closed)
            closed = &n;
    }
    std::optional<Item> closedRow;
    if (closed) {
        const Interface& i = closed->link;
        const std::string name = i.networkName.empty() ? i.name : i.networkName;
        // One line at the launcher's width (150 %): the name cut short (it was
        // cut in the middle, the live test found).
        const std::string shown = excerpt(name, 12);
        Item row("hint-public", receiving ? (chinese() ? "在「" + shown + "」上收不到文件" : "No files can come in over " + shown)
                                          : t("这台电脑现在收不到别的设备发来的文件", "This PC cannot receive files here yet"));
        const std::string it = receiving ? t("它", "it") : chinese() ? "「" + shown + "」" : shown;
        if (i.networkId.empty()) {
            row.subtitle = chinese() ? "Windows 没说清" + it + "是什么网络，默认不接收 · Enter 打开设置，改成所有网络都接收"
                                     : "Windows does not say what " + it + " is, so receiving is off · Enter opens the settings to receive on all";
            row.withActions({{"settings", t("打开设置", "Open settings"), "Enter"}});
        } else {
            const std::string enter = receiving ? t("Enter 也在这个网络上接收", "Enter receives on it too") : t("Enter 只在这个网络上接收", "Enter receives on this one only");
            row.subtitle = chinese() ? "Windows 把" + it + "当成公用网络，默认不接收 · " + enter
                                     : "Windows calls " + it + " a public network, so receiving is off · " + enter;
            row.withActions({{"allowNetwork", receiving ? t("也在这个网络上接收", "Receive on it too") : t("只在这个网络上接收", "Receive on this network only"), "Enter"}})
                .withData(Json::Object {{"network", i.networkId}, {"name", name}});
        }
        row.withIcon("glyph:U+E7BA");
        if (!receiving)
            return {std::move(row)};
        closedRow = std::move(row);
    }
    if (net.port == 0) {
        return {Item("hint-port", t("这台电脑现在收不到文件", "This PC cannot receive files"),
            t("端口 53317 到 53320 都被别的程序占着。关掉其中一个，再按 Enter 试一次",
                "Ports 53317 to 53320 are all taken by other programs. Close one, then Enter tries again"))
                .withIcon("glyph:U+E7BA")
                .withActions({{"recheck", t("再试一次", "Try again"), "Enter"}})};
    }
    // Where files go, looked at on its own thread (a share that sleeps takes
    // seconds); not known yet: taken as fine (the receiver looks again for each request).
    const std::wstring saveTo = o.folder.empty() ? downloadsFolder() : o.folder;
    const std::string folder = folderLabel(saveTo);
    std::string folderKey(reinterpret_cast<const char*>(saveTo.data()), saveTo.size() * sizeof(wchar_t));
    if (m_folderCheck.get(folderKey, [saveTo] { return std::optional<bool>(folderUsable(saveTo)); }, 10s, 0ms) == false) {
        return {Item("hint-folder", chinese() ? "收到的文件存不进「" + folder + "」" : "Files cannot be saved in " + folder,
            t("那个位置现在打不开（盘拔了，或者网络断了），别的设备发来的都会被拒绝 · Enter 改设置",
                "It cannot be reached now (a disk pulled out, a share gone): whatever is sent is refused · Enter for settings"))
                .withIcon("glyph:U+E7BA")
                .withActions({{"settings", t("改设置", "Settings"), "Enter"}})};
    }
    if (firewall.known && firewall.on && (firewall.blocked || !firewall.allowed)) {
        return {Item("hint-firewall", t("Windows 防火墙可能挡住别的设备发来的文件", "Windows Firewall may keep files from other devices out"),
            firewall.blocked
                ? t("之前点了「取消」。在「Windows 安全中心 → 防火墙和网络保护 → 允许应用通过防火墙」里勾上「Win顺 局域网传输」· Enter 再看一次",
                    "It was refused once. In Windows Security, Allow an app through firewall, tick WinShun LAN transfer · Enter looks again")
                : t("Windows 问要不要允许「Win顺 局域网传输」时，点「允许」。发给别的设备不受影响 · Enter 再看一次",
                    "When Windows asks about WinShun LAN transfer, choose Allow. Sending works either way · Enter looks again"))
                .withIcon("glyph:U+E7BA")
                .withActions({{"recheck", t("再看一次", "Look again"), "Enter"}})};
    }
    *problem = false;
    // Plain words: the name, the folder's own name, a PIN; no port (the
    // hint above says it when that is the trouble).
    std::string status = chinese() ? "收到的文件存到「" + folder + "」" : "Files received go to " + folder;
    if (!o.pin.empty())
        status += t(" · 要密码", " · PIN set");
    Item row("status", chinese() ? "别的设备看到的名字：" + alias : "Other devices see this PC as " + alias, status + t(" · Enter 改设置", " · Enter for settings"));
    std::vector<winshun::Action> actions {{"settings", t("改设置", "Settings"), "Enter"}};
    Json data = Json::Object {{"port", static_cast<long long>(net.port)}}; // for the tests, not shown
    // A public network the user allowed: the way back.
    for (const DiscoveryNetwork& n : net.networks) {
        if (n.receiving && allowed.count(n.link.networkId)) {
            actions.push_back({"forgetNetwork", t("不在这个网络上接收", "Stop receiving on this network"), "Ctrl+Enter"});
            data.set("network", n.link.networkId);
            break;
        }
    }
    Items out {row.withIcon("glyph:U+E946").withActions(std::move(actions)).withData(std::move(data))};
    if (closedRow)
        out.push_back(std::move(*closedRow));
    return out;
}

Items LocalSendPlugin::rows(const std::string& words)
{
    // Every row's first action is its Enter: the launcher runs the first one,
    // and a row without any would do nothing (it has no rows that cannot be chosen).
    Items out;
    if (!m_receiver) {
        // Off and on again on its card starts it anew.
        out.push_back(Item("broken", t("局域网传输没能启动", "LAN transfer could not start"), m_startProblem + t(" · Enter 打开设置", " · Enter opens the settings"))
                .withIcon("glyph:U+E7BA")
                .withActions({{"settings", t("打开设置", "Open settings"), "Enter"}}));
        return out;
    }
    const Clock::time_point now = Clock::now();
    Items fresh; // texts and outcomes of the last minutes: first, or right after what Enter sends
    Items older;

    // 1. Now: a request waiting for an answer, transfers under way.
    if (const std::optional<IncomingRequest> r = m_receiver->pending()) {
        const auto left = std::max<long long>(0, std::chrono::duration_cast<std::chrono::seconds>(r->deadline - now).count());
        std::string names;
        for (std::size_t i = 0; i < r->files.size() && i < 2; ++i)
            names += (i ? t("、", ", ") : "") + excerpt(r->files[i].fileName, 40);
        if (r->files.size() > 2)
            names += chinese() ? " 等" : "…";
        const std::string title = chinese() ? "接收 " + r->sender.alias + " 的 " + filesText(r->files.size()) + " · " + sizeText(r->totalBytes)
                                            : "Receive " + filesText(r->files.size()) + " from " + r->sender.alias + " · " + sizeText(r->totalBytes);
        const std::string countdown = chinese() ? "还有 " + std::to_string(left) + " 秒，不答就拒绝" : "declined in " + std::to_string(left) + " s";
        // On both lines: a click takes the request from either.
        const std::string unchecked = r->verified ? std::string() : t(" · 对方是老版本，身份没法核对", " · an old version: its identity is not checked");
        const std::string subtitle = names + (chinese() ? " · 存到「" + folderLabel(m_receiver->config().folder) + "」· "
                                                        : " · into " + folderLabel(m_receiver->config().folder) + " · ") + countdown + unchecked;
        std::string preview;
        for (std::size_t i = 0; i < r->files.size() && i < 20; ++i)
            preview += excerpt(r->files[i].fileName, 80) + "  " + sizeText(r->files[i].size) + "\n";
        if (r->files.size() > 20)
            preview += chinese() ? "……还有 " + std::to_string(r->files.size() - 20) + " 个" : "… and " + std::to_string(r->files.size() - 20) + " more";
        // A request that came in while 传 was open is above the keyboard,
        // which stays on its row: the line says how to get there (Win顺 shows
        // it until the keyboard is on it), and an unchecked identity before
        // the countdown, which is what a narrow row cuts.
        const std::string away = t("按 ↑ 选中，再 Enter 接收", "Press ↑ to pick it, then Enter accepts") + unchecked + " · " + countdown;
        out.push_back(Item("req-" + std::to_string(r->id), title, subtitle)
                .withSubtitleAway(away)
                .withIcon("glyph:U+E896")
                .withPreview(preview)
                .withActions({{"accept", t("接收", "Accept"), "Enter"}, {"decline", t("拒绝", "Decline"), "Ctrl+Enter"}})
                .withData(Json::Object {{"request", static_cast<long long>(r->id)}}));
    }
    if (const std::optional<ReceiveProgress> p = m_receiver->progress()) {
        const int percent = p->totalBytes ? static_cast<int>(p->receivedBytes * 100 / p->totalBytes) : 0;
        const std::string title = chinese() ? "正在接收 " + p->sender.alias + " 的 " + filesText(p->files) + " · " + std::to_string(percent) + "%"
                                            : "Receiving " + filesText(p->files) + " from " + p->sender.alias + " · " + std::to_string(percent) + "%";
        // The keys are in the footer (the actions say them): not in the line too.
        const std::string subtitle = sizeText(p->receivedBytes) + " / " + sizeText(p->totalBytes) + speedText(p->bytesPerSecond)
            + (chinese() ? " · 已收完 " + std::to_string(p->filesDone) + " 个" : " · " + std::to_string(p->filesDone) + " done");
        // Enter hides the window; the transfer goes on, and its notification tells how it ended.
        out.push_back(Item("recv", title, subtitle).withIcon("glyph:U+E896").withActions({{"background", t("隐藏", "Hide"), "Enter"}, {"cancelReceive", t("取消接收", "Cancel"), "Ctrl+Enter"}}));
    }
    std::optional<SendView> job;
    {
        const std::lock_guard lock(m_mutex);
        if (m_job)
            job = m_job->view();
    }
    const bool askingPin = job && job->state == SendView::State::NeedsPin;
    if (job) {
        Item row("send", std::string());
        row.withIcon("glyph:U+E724");
        const std::string what = job->text ? t("文字", "the text") : filesText(job->files);
        const std::vector<winshun::Action> going {{"background", t("隐藏", "Hide"), "Enter"}, {"cancelSend", t("取消发送", "Cancel"), "Ctrl+Enter"}};
        switch (job->state) {
        case SendView::State::Preparing:
            row.title = chinese() ? "正在准备发给 " + job->device + "…" : "Getting ready to send to " + job->device + "…";
            row.subtitle = job->text ? what : job->files ? what + " · " + sizeText(job->total) : t("正在看要发的文件", "Looking at the files");
            row.withActions(going);
            break;
        case SendView::State::Waiting:
            row.title = chinese() ? "等 " + job->device + " 接收…" : "Waiting for " + job->device + " to accept…";
            row.subtitle = chinese() ? "对方点了接收就开始发 · " + what + " · " + sizeText(job->total)
                                     : "It starts once they accept · " + what + " · " + sizeText(job->total);
            row.withActions(going);
            break;
        case SendView::State::NeedsPin:
            row.title = chinese() ? job->device + (job->wrongPin ? " 说密码不对" : " 要接收密码")
                                  : job->device + (job->wrongPin ? " says the PIN is wrong" : " wants a PIN");
            row.subtitle = chinese() ? "在「" + m_keyword + "」后面输入对方的密码，再按 Enter" : "Type their PIN after " + m_keyword + ", then Enter";
            row.withActions({{"pin", t("用这个密码发", "Send with this PIN"), "Enter"}, {"cancelSend", t("不发了", "Cancel"), "Ctrl+Enter"}});
            break;
        case SendView::State::Sending: {
            const int percent = job->total ? static_cast<int>(job->sent * 100 / job->total) : 0;
            row.title = chinese() ? "正在发给 " + job->device + " · " + std::to_string(percent) + "%"
                                  : "Sending to " + job->device + " · " + std::to_string(percent) + "%";
            row.subtitle = sizeText(job->sent) + " / " + sizeText(job->total) + speedText(job->bytesPerSecond);
            row.withActions(going);
            break;
        }
        }
        out.push_back(std::move(row));
    }

    // 2. What came in lately: texts (Enter copies), the last transfer.
    {
        const std::lock_guard lock(m_mutex);
        while (!m_texts.empty() && now - m_texts.back().at > kKeepTexts)
            m_texts.pop_back();
        for (const TextRow& text : m_texts) {
            Item row("text-" + std::to_string(text.id), chinese() ? text.sender + " 发来的文字" : "Text from " + text.sender,
                "「" + excerpt(text.text, 60) + "」· " + ago(text.at) + t(" · Enter 复制", " · Enter copies"));
            row.withIcon("glyph:U+E8BD")
                .withPreview(cutUtf8(text.text, 4000))
                .withCopy(text.text)
                .withActions({{"copyText", t("复制", "Copy"), "Enter"}})
                .withData(Json::Object {{"text", static_cast<long long>(text.id)}});
            (now - text.at < kFresh ? fresh : older).push_back(std::move(row));
        }
        // The last batch that left files here, whatever ended it (done, failed,
        // cancelled on either side); one that left none keeps the row before.
        const LastReceived::Row got = m_received.row(now);
        if (got.age != LastReceived::Age::None) {
            Item row("outcome", chinese() ? "收到了 " + got.sender + " 的 " + filesText(got.files) : "Received " + filesText(got.files) + " from " + got.sender,
                (chinese() ? "存在「" + folderLabel(got.folder) + "」· " : "In " + folderLabel(got.folder) + " · ") + ago(got.at)
                    + t(" · Enter 打开文件夹", " · Enter opens the folder"));
            row.withIcon("glyph:U+E8B7").withActions({{"openFolder", t("打开文件夹", "Open the folder"), "Enter"}});
            (got.age == LastReceived::Age::Fresh ? fresh : older).push_back(std::move(row));
        }
        // How our last send ended (its own, not one before it: the job just
        // finished may not have told yet). Enter sends the same to the same device.
        if (m_sendOutcome && m_lastSend && m_sendOutcome->at >= m_lastSend->at && now - m_sendOutcome->at < kFresh && !job) {
            if (const std::optional<SentRow> s = sentRow(m_sendOutcome->outcome, chinese())) {
                const char* enter = s->enter == SentRow::Enter::Search ? "search" : s->enter == SentRow::Enter::Dismiss ? "dismissSent" : "resend";
                fresh.push_back(Item("sent", s->title, ago(m_sendOutcome->at) + " · " + s->subtitle)
                        .withIcon("glyph:U+E724")
                        .withActions({{enter, s->enterTitle, "Enter"}}));
            }
        }
    }
    // 3. Sending: what Enter on a device sends, the devices; and receiving's state.
    // With something to send (files copied or chosen, words typed) the first
    // row says what Enter sends, and what came in lately follows it (the live
    // test of GitHub #113 found a file just received on top of the copied files).
    const auto pushFresh = [&] {
        for (Item& row : fresh)
            out.push_back(std::move(row));
    };
    const Payload p = askingPin ? Payload {} : payload(words);
    // Ctrl+Enter sends the clipboard's text while it is this one (its key in
    // the row). payload() has just waited for the clipboard: not again.
    const std::string clipText = !askingPin && p.source == Payload::Source::None ? textKey(clipboard(0ms).text) : std::string();
    const std::vector<Device> devices = m_discovery->devices();
    const bool searching = m_discovery->searching();
    bool problem = false;
    Items hints = receiveRows(&problem); // a problem, or the status row (and a network that does not receive)
    const auto pushHints = [&] {
        for (Item& row : hints)
            out.push_back(std::move(row));
    };
    Items deviceRows;
    for (const Device& d : devices)
        deviceRows.push_back(deviceRow(d, p, clipText, job.has_value()));
    // No device yet: Enter looks again (during a search too: it starts anew).
    Item none = searching ? Item("search", t("正在找附近的设备…", "Looking for devices nearby…"),
                                t("对方要打开 LocalSend，并且和这台电脑连着同一个网络 · Enter 再找一次",
                                    "They need LocalSend open, on the same network as this PC · Enter looks again"))
                          : Item("search", t("附近没找到设备", "No devices found nearby"),
                                t("对方打开 LocalSend、连上同一个 Wi-Fi 以后，按 Enter 再找一次", "Open LocalSend there, on the same Wi-Fi, then Enter looks again"));
    none.withIcon("glyph:U+E721").withActions({{"search", t("再找一次", "Look again"), "Enter"}});
    if (p.source != Payload::Source::None) {
        if (deviceRows.empty()) {
            // What a device's Enter will send, once one shows up; this Enter looks for one.
            Item what("payload", chinese() ? "要发送：" + p.title : "To send: " + p.title,
                p.detail + t(" · 还没找到设备，Enter 再找一次", " · No device yet: Enter looks again"));
            std::vector<winshun::Action> actions {{"search", t("再找一次", "Look again"), "Enter"}};
            if (p.source == Payload::Source::Menu)
                actions.push_back({"clearFiles", t("不发这些了", "Not these"), "Ctrl+Enter"});
            what.withIcon("glyph:U+E8A5").withActions(std::move(actions));
            out.push_back(std::move(what));
            out.push_back(std::move(none));
        } else {
            for (Item& row : deviceRows)
                out.push_back(std::move(row));
        }
        pushFresh();
        pushHints();
    } else {
        // Nothing to send: a device's Enter says how (in the footer).
        pushFresh();
        if (problem)
            pushHints();
        if (deviceRows.empty())
            out.push_back(std::move(none));
        for (Item& row : deviceRows)
            out.push_back(std::move(row));
        if (!problem)
            pushHints();
    }
    for (Item& row : older)
        out.push_back(std::move(row));
    return out;
}

Json LocalSendPlugin::queryResult(const winshun::Query& q)
{
    const Clock::time_point now = Clock::now();
    if (!q.keyword.empty()) {
        const std::lock_guard lock(m_mutex);
        m_keyword = q.keyword;
    }
    if (m_discovery && (now - m_lastQuery > kNewLook || now - m_lastLook > kLookAgain)) {
        // 传 opened (or open a while): look for devices, and at the network and the firewall anew.
        m_lastLook = now;
        m_discovery->search();
        {
            const std::lock_guard lock(m_houseMutex);
            m_evaluateWanted = m_firewallWanted = true;
        }
        m_houseWake.notify_all();
    }
    m_lastQuery = now;
    const Items items = rows(q.text);
    bool busy = false;
    bool waiting = false;
    if (m_receiver) {
        busy = m_receiver->progress().has_value();
        waiting = m_receiver->pending().has_value();
    }
    {
        const std::lock_guard lock(m_mutex);
        busy = busy || (m_job && m_job->view().has_value());
    }
    // Asked again this soon while 传 is open: the rows change by themselves
    // (a percentage, the seconds left), the rest is pushed (refresh()) as it
    // happens, a search's end and the devices it found too.
    const double rerun = busy ? 0.5 : waiting ? 1.0 : 5.0;
    return Json::Object {{"items", winshun::itemsJson(items)}, {"rerun", rerun}};
}

Effects LocalSendPlugin::refreshed(const std::string& words, Effects before)
{
    before.push_back(effect::replace(rows(words)));
    before.push_back(effect::keep());
    return before;
}

Effects LocalSendPlugin::startSend(const Device& device, const Target& target, Outgoing what, std::uint64_t selection)
{
    if (what.kind == Outgoing::Kind::Text && what.text.size() > kMaxTextBytes)
        return {effect::notify(t("文字太长了（超过 64 KB）：存成文件再发", "The text is too long (over 64 KB): save it as a file and send that")),
            effect::keep()};
    std::unique_ptr<SendJob> old;
    {
        const std::lock_guard lock(m_mutex);
        if (m_job && !m_job->finished())
            return {effect::notify(t("正在发送，等这次发完再发", "Still sending; wait until it is done")), effect::keep()};
        old = std::move(m_job);
    }
    old.reset(); // its thread waited for: how it ended is stored before this one starts
    DeviceInfo me;
    {
        const std::lock_guard lock(m_mutex);
        me = self(m_net.port);
        m_lastSend = LastSend {what, device.info.fingerprint, selection, Clock::now()}; // for the row after it: Enter sends it again
    }
    auto job = std::make_unique<SendJob>(*m_client, target, device.info.alias, me, std::move(what),
        [this, selection](const SendOutcome& o) { onSent(o, selection); });
    {
        const std::lock_guard lock(m_mutex);
        m_job = std::move(job);
    }
    // Nothing in the footer: the window goes, and how it ends comes as a notification.
    return {effect::hide()};
}

// ---- actions -----------------------------------------------------------------------

Effects LocalSendPlugin::action(const winshun::ActionRequest& a)
{
    if (a.trigger == "files" || a.action == "files") {
        // "用局域网发送…" on Win顺's result menu: the files, then the 传 list to pick a device.
        std::vector<std::wstring> files;
        for (const std::string& f : a.files) {
            if (!f.empty() && files.size() < 1000)
                files.push_back(wide(f));
        }
        if (files.empty())
            return {effect::notify(t("没有可发送的文件", "Nothing to send")), effect::keep()};
        {
            const std::lock_guard lock(m_mutex);
            m_menuFiles = std::move(files);
            m_menuFilesAt = Clock::now();
            ++m_menuSelection;
        }
        return {effect::setQuery(keywordSpace()), effect::keep()};
    }
    const std::string& id = a.action;
    if (id == "settings") // this plugin's card in Win顺's settings (the row that says it could not start too)
        return {effect::openUrl("winshun:settings")};
    if (!m_receiver)
        return {effect::notify(t("局域网传输没能启动：", "LAN transfer could not start: ") + m_startProblem), effect::keep()};
    const Json& data = a.item["data"];

    if (id == "accept" || id == "decline") {
        const bool accept = id == "accept";
        if (!m_receiver->decide(static_cast<std::uint64_t>(data["request"].toInt()), accept))
            return refreshed(a.text, {effect::notify(t("这个请求已经过期了", "That request is no longer waiting"))});
        // A yes shows itself (the progress row takes the request's place); a no leaves nothing to see.
        return refreshed(a.text, accept ? Effects {} : Effects {effect::notify(t("已拒绝", "Declined"))});
    }
    if (id == "background") // a transfer's row: it goes on; the notification tells how it ended
        return {effect::hide()};
    if (id == "cancelReceive") {
        m_receiver->cancel();
        return refreshed(a.text, {effect::notify(t("已取消接收，没收完的文件已删除", "Cancelled; partial files were deleted"))});
    }
    if (id == "cancelSend" || id == "pin") {
        SendJob* job = nullptr;
        {
            const std::lock_guard lock(m_mutex);
            job = m_job.get();
        }
        if (!job || job->finished())
            return refreshed(a.text);
        if (id == "pin") {
            const std::string pin = trimmed(a.text);
            if (pin.empty())
                return {effect::notify(chinese() ? "先在「" + m_keyword + "」后面输入密码" : "Type the PIN after " + m_keyword + " first"), effect::keep()};
            job->givePin(pin);
            return {effect::setQuery(keywordSpace()), effect::keep()}; // the send row says how it goes
        }
        job->cancel();
        for (int i = 0; i < 40 && !job->finished(); ++i)
            ::Sleep(25);
        return refreshed(a.text, {effect::notify(t("已取消发送", "Cancelled"))});
    }
    if (id == "send" || id == "sendClipText") {
        {
            // First: while one goes, nothing else can (the words after 传 may be its PIN).
            const std::lock_guard lock(m_mutex);
            if (m_job && !m_job->finished())
                return {effect::notify(t("正在发送，等这次发完再发", "Still sending; wait until it is done")), effect::keep()};
        }
        const std::optional<Device> device = m_discovery->device(data["fp"].toString());
        if (!device)
            return refreshed(a.text, {effect::notify(t("这个设备不见了，再找一次试试", "That device is gone; look again"))});
        const std::optional<Target> target = m_discovery->target(*device);
        if (!target)
            return refreshed(a.text, {effect::notify(t("这个设备已经不在同一个网络上了", "That device is not on this network any more"))});
        Outgoing what;
        std::uint64_t selection = 0; // the files chosen in the results, when it is those
        // What the clipboard holds now, read a while longer than for a row; never
        // the last one read in its place (it may be what was copied before).
        const auto clipboardNow = [&]() -> const Clip* {
            const Clip& clip = clipboard(1000ms);
            return clipboardCurrent() ? &clip : nullptr;
        };
        const Effects notRead {effect::notify(t("剪贴板还没读出来，稍等再按", "The clipboard is not read yet; press again in a moment")), effect::keep()};
        if (id == "sendClipText") {
            const Clip* clip = clipboardNow();
            if (!clip)
                return notRead;
            const std::string key = textKey(clip->text);
            if (key.empty())
                return refreshed(a.text, {effect::notify(t("剪贴板里没有文字", "There is no text on the clipboard"))});
            if (key != data["clip"].toString()) // copied anew since the row was drawn
                return refreshed(a.text, {effect::notify(t("剪贴板里的文字刚变了，看一眼再按 Ctrl+Enter", "The clipboard's text just changed; look, then press Ctrl+Enter"))});
            what.kind = Outgoing::Kind::Text;
            what.text = clip->text;
        } else {
            Payload p = payload(a.text, false); // the key is enough here: no new look at the files
            if ((p.source == Payload::Source::Clipboard || p.source == Payload::Source::None) && !clipboardCurrent()) {
                if (!clipboardNow())
                    return notRead;
                p = payload(a.text, false);
            }
            if (p.source == Payload::Source::None)
                return {effect::notify(chinese() ? "先复制要发的文件，或在「" + m_keyword + "」后面打字" : "Copy files, or type after " + m_keyword + ", first"),
                    effect::keep()};
            if (p.key != data["what"].toString()) // the clipboard or the words changed since the row was drawn
                return refreshed(a.text, {effect::notify(t("要发的东西刚变了，看一眼再按 Enter", "What would be sent just changed; look, then press Enter"))});
            what = p.what;
            if (p.source == Payload::Source::Menu)
                selection = p.selection;
        }
        return startSend(*device, *target, std::move(what), selection);
    }
    if (id == "resend") {
        // The row after a send of ours: the same again, to the same device.
        std::optional<LastSend> last;
        {
            const std::lock_guard lock(m_mutex);
            if (m_job && !m_job->finished())
                return {effect::notify(t("正在发送，等这次发完再发", "Still sending; wait until it is done")), effect::keep()};
            last = m_lastSend;
        }
        if (!last)
            return refreshed(a.text);
        const std::optional<Device> device = m_discovery->device(last->fingerprint);
        if (!device)
            return refreshed(a.text, {effect::notify(t("这个设备不见了，再找一次试试", "That device is gone; look again"))});
        const std::optional<Target> target = m_discovery->target(*device);
        if (!target)
            return refreshed(a.text, {effect::notify(t("这个设备已经不在同一个网络上了", "That device is not on this network any more"))});
        return startSend(*device, *target, std::move(last->what), last->selection);
    }
    if (id == "dismissSent") {
        {
            const std::lock_guard lock(m_mutex);
            m_sendOutcome.reset();
        }
        return refreshed(a.text);
    }
    if (id == "recheck") {
        // A hint's Enter once it was put right: the networks, the ports and the
        // firewall looked at now. Said when it is still the same.
        recheckNow();
        bool problem = false;
        const Items hints = receiveRows(&problem);
        const bool same = problem && !hints.empty() && hints.front().id == a.item["id"].toString();
        return refreshed(a.text, same ? Effects {effect::notify(t("看了一下，还是这样", "Looked again: no change"))} : Effects {});
    }
    if (id == "allowNetwork" || id == "forgetNetwork") {
        const std::string network = data["network"].toString();
        if (network.empty())
            return refreshed(a.text);
        {
            const std::lock_guard lock(m_mutex);
            if (id == "allowNetwork")
                m_allowed[network] = data["name"].toString();
            else
                m_allowed.erase(network);
        }
        saveAllowedNetworks();
        evaluateNow();
        return refreshed(a.text, {effect::notify(id == "allowNetwork" ? t("这个网络上可以接收了，换到别的公用网络还会再问", "Receiving on this network now; other public networks still ask")
                                                                       : t("这个网络上不再接收", "Not receiving on this network any more"))});
    }
    if (id == "copyText") {
        const std::uint64_t textId = static_cast<std::uint64_t>(data["text"].toInt());
        std::string text;
        {
            const std::lock_guard lock(m_mutex);
            for (const TextRow& row : m_texts) {
                if (row.id == textId)
                    text = row.text;
            }
        }
        if (text.empty())
            return refreshed(a.text);
        return {effect::copy(text), effect::notify(t("已复制", "Copied")), effect::hide()};
    }
    if (id == "openFolder") {
        std::wstring folder;
        {
            const std::lock_guard lock(m_mutex);
            folder = m_received.row(Clock::now()).folder;
        }
        if (folder.empty())
            return refreshed(a.text);
        return {effect::openPath(narrow(folder)), effect::hide()};
    }
    if (id == "search") {
        m_discovery->search();
        m_lastLook = Clock::now();
        return refreshed(a.text);
    }
    if (id == "clearFiles") {
        {
            const std::lock_guard lock(m_mutex);
            m_menuFiles.clear();
        }
        return refreshed(a.text);
    }
    return {effect::keep()};
}

} // namespace

int main()
{
    // Nothing but JSON-RPC goes to stdout; the C runtime is never asked to
    // print. Sockets for the receiver, discovery and sending.
    WSADATA wsa {};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        winshun::debug("localsend: WSAStartup failed");
        return 1;
    }
    int code = 0;
    {
        LocalSendPlugin plugin;
        code = plugin.serve();
    }
    ::WSACleanup();
    return code;
}
