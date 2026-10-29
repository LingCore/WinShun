#include "Discovery.h"

#include "Util.h"

#include <ws2tcpip.h>

#include <algorithm>

using winshun::Json;
using namespace std::chrono_literals;

namespace localsend {

namespace {

constexpr auto kAnswerGap = 2s; // one register per announcer in this time
constexpr std::size_t kMaxAnswers = 16; // waiting to be sent
constexpr int kAnnouncementsPerSecond = 10; // from one address (LocalSend sends three a search); more are not read
constexpr std::size_t kPruneAt = 256; // a map by address this big: the entries that no longer matter go
constexpr DWORD kReceiveSliceMs = 500; // how soon a listening thread notices it is to end
constexpr auto kSearchTime = 4s; // "正在找…" this long after the user looks
constexpr std::chrono::milliseconds kBursts[] = {100ms, 500ms, 2000ms}; // as LocalSend sends its own
constexpr const char* kRegister = "/api/localsend/v2/register";

std::string announcement(const DeviceInfo& self, bool flag)
{
    Json j = toJson(self, true);
    j.set("announce", flag);
    j.set("announcement", flag); // what LocalSend called it before protocol v2
    return j.dump();
}

std::string key(const std::string& fingerprint)
{
    std::string k = fingerprint;
    for (char& c : k) {
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    }
    return k;
}

} // namespace

struct Discovery::Group {
    DiscoveryNetwork network;
    SOCKET socket = INVALID_SOCKET;
    bool listening = false; // bound to the group's port (sending works without)
    std::atomic<bool> alive {true};
    std::thread thread;
};

Discovery::Discovery(Client& client, std::function<void()> changed)
    : m_client(client)
    , m_changed(std::move(changed))
{
    m_timer = std::thread([this] { timerLoop(); });
    m_answerer = std::thread([this] { answerLoop(); });
}

Discovery::~Discovery()
{
    stop();
    if (m_timer.joinable())
        m_timer.join(); // the only thread that starts scans: none starts after this
    if (m_answerer.joinable())
        m_answerer.join();
    if (m_scanner.joinable())
        m_scanner.join();
    leaveAll();
}

void Discovery::stop()
{
    {
        const std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    m_answerWake.notify_all();
    m_scanCutter.cut();
    m_answerCutter.cut();
}

void Discovery::setSelf(const DeviceInfo& self)
{
    const std::lock_guard lock(m_mutex);
    m_self = self;
}

void Discovery::setNetworks(std::vector<DiscoveryNetwork> networks)
{
    const auto same = [](const std::vector<DiscoveryNetwork>& a, const std::vector<DiscoveryNetwork>& b) {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i].link.index != b[i].link.index || a[i].link.address != b[i].link.address
                || a[i].link.prefix != b[i].link.prefix)
                return false;
        }
        return true;
    };
    bool rejoin = false;
    {
        const std::lock_guard lock(m_mutex);
        rejoin = !same(m_networks, networks) || m_groups.size() != networks.size();
        m_networks = networks;
        if (!rejoin) {
            for (std::size_t i = 0; i < networks.size() && i < m_groups.size(); ++i)
                m_groups[i]->network.receiving = networks[i].receiving;
        }
    }
    if (rejoin) {
        leaveAll();
        joinAll(networks);
    }
}

void Discovery::joinAll(const std::vector<DiscoveryNetwork>& networks)
{
    std::vector<std::unique_ptr<Group>> groups;
    // Whatever fails midway, the groups made are kept (their threads run:
    // dropped, they would end the process).
    guarded("joining the multicast group", [&] {
        groups.reserve(networks.size());
        joinEach(networks, groups);
    });
    const std::lock_guard lock(m_mutex);
    m_groups = std::move(groups);
}

void Discovery::joinEach(const std::vector<DiscoveryNetwork>& networks, std::vector<std::unique_ptr<Group>>& groups)
{
    for (const DiscoveryNetwork& n : networks) {
        auto g = std::make_unique<Group>();
        g->network = n;
        g->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g->socket == INVALID_SOCKET)
            continue;
        // LocalSend itself may run on this PC and hold the port: shared.
        BOOL reuse = TRUE;
        ::setsockopt(g->socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof reuse);
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(kPort));
        g->listening = ::bind(g->socket, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0;
        if (!g->listening) {
            logWarning("multicast port " + std::to_string(kPort) + " could not be had (" + std::to_string(::WSAGetLastError())
                + "): announcements are sent, not heard");
            a.sin_port = 0;
            a.sin_addr.s_addr = htonl(n.link.address);
            ::bind(g->socket, reinterpret_cast<const sockaddr*>(&a), sizeof a);
        }
        ip_mreq membership {};
        ::inet_pton(AF_INET, kMulticastGroup, &membership.imr_multiaddr);
        membership.imr_interface.s_addr = htonl(n.link.address);
        if (g->listening
            && ::setsockopt(g->socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&membership), sizeof membership) != 0) {
            logWarning("joining the multicast group failed (" + std::to_string(::WSAGetLastError()) + ")");
            g->listening = false;
        }
        ::setsockopt(g->socket, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&membership.imr_interface),
            sizeof membership.imr_interface);
        const DWORD ttl = 1, loop = 1;
        ::setsockopt(g->socket, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof ttl);
        ::setsockopt(g->socket, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof loop);
        ::setsockopt(g->socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&kReceiveSliceMs), sizeof kReceiveSliceMs);
        if (g->listening) {
            Group* raw = g.get();
            try {
                g->thread = std::thread([this, raw] { listen(raw); });
            } catch (const std::system_error&) {
                g->listening = false; // announcements sent from it, none heard
            }
        }
        groups.push_back(std::move(g)); // room reserved: never throws with a thread running
    }
}

void Discovery::leaveAll()
{
    std::vector<std::unique_ptr<Group>> groups;
    {
        const std::lock_guard lock(m_mutex);
        groups = std::move(m_groups);
        m_groups.clear();
    }
    for (const auto& g : groups)
        g->alive = false;
    for (const auto& g : groups) {
        if (g->thread.joinable())
            g->thread.join(); // within kReceiveSliceMs
        ::closesocket(g->socket);
    }
}

void Discovery::listen(Group* group)
{
    std::vector<char> buffer(64 * 1024);
    while (group->alive) {
        sockaddr_in from {};
        int size = sizeof from;
        const int got = ::recvfrom(group->socket, buffer.data(), static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from), &size);
        if (!group->alive)
            return;
        if (got <= 0) {
            const int error = ::WSAGetLastError();
            if (error != WSAETIMEDOUT && error != WSAECONNRESET && error != WSAEMSGSIZE)
                ::Sleep(200); // something else: not a spin
            continue;
        }
        guarded("an announcement", [&] {
            onAnnouncement(group, std::string(buffer.data(), static_cast<std::size_t>(got)), ntohl(from.sin_addr.s_addr));
        });
    }
}

void Discovery::onAnnouncement(Group* group, const std::string& text, std::uint32_t from)
{
    const Interface& i = group->network.link;
    // Only from the network it came in on (a datagram for another interface's
    // socket is not ours to read). Our own, looped back, is known by its
    // fingerprint below; from this PC's address otherwise is LocalSend (or
    // another Win顺) open on this PC too.
    if (!sameSubnet(from, i.address, i.prefix))
        return;
    {
        // A flood from one address is not read (parsing is what costs).
        const std::lock_guard lock(m_mutex);
        const Clock::time_point now = Clock::now();
        if (m_heardFrom.size() > kPruneAt)
            std::erase_if(m_heardFrom, [&](const auto& entry) { return now - entry.second.first >= 1s; });
        auto& [second, count] = m_heardFrom[from];
        if (now - second >= 1s) {
            second = now;
            count = 0;
        }
        if (++count > kAnnouncementsPerSecond)
            return;
    }
    const std::optional<Json> json = parsePeerJson(text, kMaxDeviceValues);
    if (!json || !json->isObject())
        return;
    const std::optional<DeviceInfo> info = deviceFromJson(*json);
    if (!info)
        return;
    {
        const std::lock_guard lock(m_mutex);
        if (equalsNoCase(info->fingerprint, m_self.fingerprint))
            return; // ours, back through another interface
    }
    Device d;
    d.info = *info;
    d.ip = ipv4Text(from);
    d.address = from;
    d.interfaceIndex = i.index;
    d.localAddress = i.address;
    remember(d);
    const bool asks = (*json)["announce"].toBool(false) || (*json)["announcement"].toBool(false);
    if (!asks)
        return;
    const std::lock_guard lock(m_mutex);
    if (!group->network.receiving)
        return; // where this PC does not receive, it does not answer either
    const Clock::time_point now = Clock::now();
    if (m_answered.size() > kPruneAt)
        std::erase_if(m_answered, [&](const auto& entry) { return now - entry.second >= kAnswerGap; });
    auto& last = m_answered[d.ip];
    if (m_stopping || m_answers.size() >= kMaxAnswers || (last != Clock::time_point {} && now - last < kAnswerGap))
        return;
    last = now;
    m_answers.push_back({d.info, d.ip, i.address, i.index});
    m_answerWake.notify_one();
}

void Discovery::announce(bool onlyReceiving, bool flag, std::uint32_t onlyLocal)
{
    sockaddr_in to {};
    to.sin_family = AF_INET;
    to.sin_port = htons(static_cast<u_short>(kPort));
    ::inet_pton(AF_INET, kMulticastGroup, &to.sin_addr);
    const std::lock_guard lock(m_mutex);
    if (m_self.fingerprint.empty())
        return;
    const std::string text = announcement(m_self, flag);
    for (const auto& g : m_groups) {
        if ((onlyReceiving && !g->network.receiving) || (onlyLocal && g->network.link.address != onlyLocal))
            continue;
        ::sendto(g->socket, text.data(), static_cast<int>(text.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }
}

void Discovery::remember(Device device)
{
    bool changed = false;
    Clock::time_point recheckAt {};
    {
        const std::lock_guard lock(m_mutex);
        const Clock::time_point now = Clock::now();
        forgetOld();
        device.seen = now;
        const std::string k = key(device.info.fingerprint);
        [&] {
            const auto it = m_devices.find(k);
            if (it != m_devices.end()) {
                Device& known = it->second;
                // Anyone can say a fingerprint. An unchecked word about a device
                // that showed its certificate, or one that would make an HTTPS
                // device HTTP, only says it is still there when it says where it
                // already was; otherwise it is ignored.
                if (!device.verified && (known.verified || (known.info.https() && !device.info.https()))) {
                    if (known.ip != device.ip || known.info.port != device.info.port || known.info.protocol != device.info.protocol)
                        return;
                    known.seen = now;
                } else {
                    device.firstSeen = known.firstSeen;
                    known = std::move(device);
                }
            } else {
                if (m_devices.size() >= kMaxDevices) {
                    auto oldest = m_devices.begin();
                    for (auto d = m_devices.begin(); d != m_devices.end(); ++d) {
                        if (d->second.seen < oldest->second.seen)
                            oldest = d;
                    }
                    m_devices.erase(oldest);
                }
                device.firstSeen = now;
                m_devices.emplace(k, std::move(device));
            }
            if (now >= m_searchStarted)
                m_heardSinceSearch = true;
        }();
        changed = relistLocked(now);
        recheckAt = recheckLocked();
    }
    if (recheckAt != Clock::time_point {})
        at(recheckAt, [this] { recheck(); });
    if (changed)
        tellChanged();
}

bool Discovery::relistLocked(Clock::time_point now)
{
    // What the rows show of each device listed: a new announcement saying
    // the same changes nothing.
    std::string listed;
    for (const auto& [k, d] : m_devices) {
        if (now - d.seen <= kForgetAfter)
            listed += k + '\x1f' + d.info.alias + '\x1f' + d.ip + '\x1f' + d.info.deviceType + '\x1f' + d.info.deviceModel + '\x1e';
    }
    if (listed == m_listed)
        return false;
    m_listed = std::move(listed);
    return true;
}

Discovery::Clock::time_point Discovery::recheckLocked()
{
    // A device unheard of for kForgetAfter leaves the list with no word from
    // anywhere: a look then tells its going too. One look due at a time.
    Clock::time_point first = Clock::time_point::max();
    for (const auto& [k, d] : m_devices)
        first = std::min(first, d.seen + kForgetAfter);
    if (first == Clock::time_point::max())
        return {};
    first += 1s;
    if (m_recheckAt != Clock::time_point {} && m_recheckAt <= first)
        return {};
    m_recheckAt = first;
    return first;
}

void Discovery::recheck()
{
    bool changed = false;
    Clock::time_point next {};
    {
        const std::lock_guard lock(m_mutex);
        m_recheckAt = {};
        forgetOld();
        changed = relistLocked(Clock::now());
        next = recheckLocked();
    }
    if (next != Clock::time_point {})
        at(next, [this] { recheck(); });
    if (changed)
        tellChanged();
}

void Discovery::tellChanged()
{
    if (m_changed)
        guarded("telling the devices changed", m_changed);
}

void Discovery::forgetOld()
{
    const Clock::time_point now = Clock::now();
    for (auto it = m_devices.begin(); it != m_devices.end();) {
        if (now - it->second.seen > kForgetAfter)
            it = m_devices.erase(it);
        else
            ++it;
    }
}

void Discovery::heard(const DeviceInfo& info, const std::string& ip, bool verified)
{
    const std::uint32_t address = parseIpv4(ip);
    Device d;
    {
        const std::lock_guard lock(m_mutex);
        if (equalsNoCase(info.fingerprint, m_self.fingerprint))
            return;
        const auto n = std::find_if(m_networks.begin(), m_networks.end(), [&](const DiscoveryNetwork& network) {
            return sameSubnet(address, network.link.address, network.link.prefix);
        });
        if (n == m_networks.end())
            return;
        d.interfaceIndex = n->link.index;
        d.localAddress = n->link.address;
    }
    d.info = info;
    d.ip = ip;
    d.address = address;
    d.verified = verified;
    remember(d);
}

std::vector<Device> Discovery::devices() const
{
    std::vector<Device> out;
    const Clock::time_point now = Clock::now();
    {
        const std::lock_guard lock(m_mutex);
        for (const auto& [k, d] : m_devices) {
            if (now - d.seen <= kForgetAfter)
                out.push_back(d);
        }
    }
    std::sort(out.begin(), out.end(), [](const Device& a, const Device& b) {
        return a.firstSeen != b.firstSeen ? a.firstSeen < b.firstSeen : a.info.alias < b.info.alias;
    });
    return out;
}

std::optional<Device> Discovery::device(const std::string& fingerprint) const
{
    const std::lock_guard lock(m_mutex);
    const auto it = m_devices.find(key(fingerprint));
    if (it == m_devices.end() || Clock::now() - it->second.seen > kForgetAfter)
        return std::nullopt;
    return it->second;
}

bool Discovery::searching() const
{
    const std::lock_guard lock(m_mutex);
    return m_scanning || Clock::now() < m_searchUntil;
}

std::optional<Target> Discovery::target(const Device& device) const
{
    const std::lock_guard lock(m_mutex);
    const DiscoveryNetwork* best = nullptr;
    for (const DiscoveryNetwork& n : m_networks) {
        if (!sameSubnet(device.address, n.link.address, n.link.prefix))
            continue;
        if (!best || n.link.index == device.interfaceIndex)
            best = &n;
    }
    if (!best)
        return std::nullopt;
    Target t;
    t.ip = device.ip;
    t.port = device.info.port;
    t.https = device.info.https();
    t.fingerprint = device.info.fingerprint;
    t.localAddress = best->link.address;
    t.interfaceIndex = best->link.index;
    return t;
}

void Discovery::search()
{
    const Clock::time_point now = Clock::now();
    {
        const std::lock_guard lock(m_mutex);
        m_searchStarted = now;
        m_searchUntil = now + kSearchTime;
        m_heardSinceSearch = false;
    }
    announce(false, true);
    for (const auto delay : kBursts)
        at(now + delay, [this] { announce(false, true); });
    at(now + kSearchTime + 100ms, [this] {
        if (!searching())
            tellChanged(); // "正在找…" is over (a scan under way tells when it ends)
    });
    at(now + kScanAfter, [this] {
        bool start = false;
        {
            const std::lock_guard lock(m_mutex);
            const Clock::time_point t = Clock::now();
            start = !m_stopping && !m_heardSinceSearch && !m_scanning && (m_lastScan == Clock::time_point {} || t - m_lastScan >= kScanGap);
            if (start) {
                m_scanning = true;
                m_lastScan = t;
            }
        }
        if (!start)
            return;
        if (m_scanner.joinable())
            m_scanner.join(); // the last one, ended
        m_scanCutter.reset();
        bool stopped = false;
        {
            const std::lock_guard lock(m_mutex);
            stopped = m_stopping; // stop() cut before the reset: its cut must hold
        }
        try {
            if (!stopped)
                m_scanner = std::thread([this] {
                    guarded("a scan", [&] { scan(); });
                    m_scanning = false;
                    if (!searching())
                        tellChanged(); // the search's last part
                });
        } catch (const std::system_error&) {
            stopped = true; // no thread: no scan
        }
        if (stopped)
            m_scanning = false;
    });
}

void Discovery::announceReceiving()
{
    const Clock::time_point now = Clock::now();
    for (const auto delay : kBursts)
        at(now + delay, [this] { announce(true, true); });
}

void Discovery::at(Clock::time_point when, std::function<void()> job)
{
    {
        const std::lock_guard lock(m_mutex);
        if (m_stopping)
            return;
        m_jobs.emplace(when, std::move(job));
    }
    m_wake.notify_one();
}

void Discovery::timerLoop()
{
    std::unique_lock lock(m_mutex);
    for (;;) {
        if (m_stopping)
            return;
        if (m_jobs.empty()) {
            m_wake.wait(lock);
            continue;
        }
        const auto first = m_jobs.begin();
        if (Clock::now() < first->first) {
            m_wake.wait_until(lock, first->first);
            continue;
        }
        std::function<void()> job = std::move(first->second);
        m_jobs.erase(first);
        lock.unlock();
        guarded("a discovery job", job);
        lock.lock();
    }
}

void Discovery::answerLoop()
{
    for (;;) {
        Answer a;
        {
            std::unique_lock lock(m_mutex);
            m_answerWake.wait(lock, [&] { return m_stopping || !m_answers.empty(); });
            if (m_stopping)
                return;
            a = std::move(m_answers.front());
            m_answers.pop_front();
        }
        guarded("an answer", [&] { answer(std::move(a.info), std::move(a.ip), a.localAddress, a.interfaceIndex); });
    }
}

void Discovery::answer(DeviceInfo info, std::string ip, std::uint32_t localAddress, unsigned long interfaceIndex)
{
    Target t;
    t.ip = ip;
    t.port = info.port;
    t.https = info.https();
    t.fingerprint = info.fingerprint; // pinned: the certificate must be the announced one
    t.localAddress = localAddress;
    t.interfaceIndex = interfaceIndex;
    std::string self;
    {
        const std::lock_guard lock(m_mutex);
        self = toJson(m_self, true).dump();
    }
    const Reply r = m_client.request(t, "POST", kRegister, self, 3s, &m_answerCutter, 2s);
    if (r.status == 200) {
        Device d;
        d.info = std::move(info);
        d.ip = std::move(ip);
        d.address = parseIpv4(d.ip);
        d.interfaceIndex = interfaceIndex;
        d.localAddress = localAddress;
        d.verified = t.https;
        remember(std::move(d));
        return;
    }
    // Its server is not reachable from here (its firewall, an old version):
    // LocalSend answers on the group instead, and so do we.
    announce(false, false, localAddress);
}

void Discovery::scan()
{
    struct Address {
        std::uint32_t address;
        std::uint32_t local;
        unsigned long index;
    };
    std::vector<Address> addresses;
    std::string body;
    std::string self;
    {
        const std::lock_guard lock(m_mutex);
        for (const DiscoveryNetwork& n : m_networks) {
            // The /24 around our address, as LocalSend scans, whatever the prefix.
            const std::uint32_t base = n.link.address & 0xFFFFFF00u;
            for (std::uint32_t host = 1; host < 255; ++host) {
                const std::uint32_t a = base | host;
                if (a != n.link.address && sameSubnet(a, n.link.address, n.link.prefix))
                    addresses.push_back({a, n.link.address, n.link.index});
            }
            if (addresses.size() > 4 * 254)
                break;
        }
        body = toJson(m_self, true).dump();
        self = m_self.fingerprint;
    }
    std::atomic<std::size_t> next {0};
    const auto work = [&] {
        for (;;) {
            const std::size_t i = next++;
            if (i >= addresses.size() || m_scanCutter.isCut())
                return;
            Target t;
            t.ip = ipv4Text(addresses[i].address);
            t.port = kPort;
            t.https = true;
            t.localAddress = addresses[i].local;
            t.interfaceIndex = addresses[i].index;
            const Reply r = m_client.request(t, "POST", kRegister, body, 2s, &m_scanCutter, 600ms);
            if (r.status != 200)
                continue;
            const std::optional<Json> json = parsePeerJson(r.body, kMaxDeviceValues);
            std::optional<DeviceInfo> info = json ? deviceFromJson(*json) : std::nullopt;
            // Its certificate must be the one its info names: nothing pinned it.
            if (!info || !equalsNoCase(info->fingerprint, r.peerFingerprint) || equalsNoCase(info->fingerprint, self))
                continue;
            info->port = kPort;
            info->protocol = "https";
            Device d;
            d.info = std::move(*info);
            d.ip = t.ip;
            d.address = addresses[i].address;
            d.interfaceIndex = addresses[i].index;
            d.localAddress = addresses[i].local;
            d.verified = true;
            remember(std::move(d));
        }
    };
    std::vector<std::thread> workers;
    const std::size_t count = std::min<std::size_t>(kScanWorkers, addresses.size());
    try {
        workers.reserve(count);
        for (std::size_t w = 0; w < count; ++w)
            workers.emplace_back([&] { guarded("a scan's worker", work); });
    } catch (const std::exception& e) {
        logStopped("starting a scan's workers", e.what()); // the ones running do it
    }
    for (std::thread& w : workers)
        w.join();
}

} // namespace localsend
