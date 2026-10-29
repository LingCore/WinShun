#pragma once

// Finding devices the way LocalSend does: announcements on the multicast
// group 224.0.0.167:53317 of every usable interface (ours when the user
// looks, and once when receiving starts; theirs answered with a register,
// or a multicast "announce": false when that fails), and, when nobody has
// answered within a second of looking, a scan of each network's /24 on
// port 53317 (outgoing connections: it finds devices even when this PC's
// firewall keeps theirs out). The devices found, at most 64, each
// forgotten five minutes after it was last heard of.

#include "Client.h"
#include "Network.h"
#include "Protocol.h"

#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace localsend {

struct Device {
    DeviceInfo info;
    std::string ip;
    std::uint32_t address = 0; // host order
    unsigned long interfaceIndex = 0; // where it was heard
    std::uint32_t localAddress = 0; // our address there
    bool verified = false; // a TLS contact showed the certificate its fingerprint names
    std::chrono::steady_clock::time_point firstSeen;
    std::chrono::steady_clock::time_point seen;
};

// A network to work on: a usable interface, and whether this PC receives
// there (then announcements are answered, and ours go out when receiving
// starts; elsewhere only while the user looks).
struct DiscoveryNetwork {
    Interface link; // ("interface" is a macro of the COM headers)
    bool receiving = false;
};

class Discovery {
public:
    static constexpr std::size_t kMaxDevices = 64;
    static constexpr auto kForgetAfter = std::chrono::minutes(5);
    static constexpr auto kScanAfter = std::chrono::seconds(1);
    static constexpr auto kScanGap = std::chrono::seconds(20); // no new scan before this
    static constexpr int kScanWorkers = 32;

    // `changed`: what devices() lists changed (a device found, gone, renamed
    // or moved), or a search ended; never for an announcement that changes
    // nothing. Called on discovery's threads, with nothing locked.
    explicit Discovery(Client& client, std::function<void()> changed = {});
    ~Discovery(); // stop(), and its threads waited for
    Discovery(const Discovery&) = delete;
    Discovery& operator=(const Discovery&) = delete;

    // Nothing more sent or started; the scan and the answer under way cut.
    // Does not wait.
    void stop();

    // What we say we are (the port the receiver has, or 53317 when it has none).
    void setSelf(const DeviceInfo& self);
    // The networks now; the multicast group joined anew when they changed.
    void setNetworks(std::vector<DiscoveryNetwork> networks);
    // The user looks: our announcement on every network, a scan if nobody answers.
    void search();
    // Receiving began (or its networks changed): announced on those networks.
    void announceReceiving();
    // A register reached our server: a device nearby. A word nobody checked
    // (`verified` false: no certificate, or an announcement) never moves a
    // device that showed its certificate, nor makes an HTTPS one HTTP.
    void heard(const DeviceInfo& info, const std::string& ip, bool verified);

    std::vector<Device> devices() const; // in the order they were found
    std::optional<Device> device(const std::string& fingerprint) const;
    bool searching() const; // a search's first seconds, or a scan
    // How to reach `device` now: its address on an interface we still have.
    std::optional<Target> target(const Device& device) const;

private:
    struct Group;
    using Clock = std::chrono::steady_clock;

    void joinAll(const std::vector<DiscoveryNetwork>& networks); // m_mutex not held
    void joinEach(const std::vector<DiscoveryNetwork>& networks, std::vector<std::unique_ptr<Group>>& groups);
    void leaveAll();
    void listen(Group* group);
    void onAnnouncement(Group* group, const std::string& text, std::uint32_t from);
    void announce(bool onlyReceiving, bool flag, std::uint32_t onlyLocal = 0);
    void remember(Device device);
    void forgetOld(); // m_mutex held
    bool relistLocked(Clock::time_point now); // true when what devices() lists is not what it was
    Clock::time_point recheckLocked(); // when to look for devices to forget next; {}: none, or one due already
    void recheck();
    void tellChanged();
    void timerLoop();
    void at(Clock::time_point when, std::function<void()> job);
    void answerLoop();
    void answer(DeviceInfo info, std::string ip, std::uint32_t localAddress, unsigned long interfaceIndex);
    void scan();

    Client& m_client;
    const std::function<void()> m_changed;
    mutable std::mutex m_mutex;
    std::string m_listed; // what devices() listed when last told
    Clock::time_point m_recheckAt; // the look for devices to forget that is due
    DeviceInfo m_self;
    std::vector<DiscoveryNetwork> m_networks;
    std::vector<std::unique_ptr<Group>> m_groups;
    std::map<std::string, Device> m_devices; // by fingerprint (upper case)
    std::map<std::string, Clock::time_point> m_answered; // by peer address: the last register sent it
    std::map<std::uint32_t, std::pair<Clock::time_point, int>> m_heardFrom; // announcements by address: this second's count
    Clock::time_point m_searchStarted;
    Clock::time_point m_searchUntil;
    Clock::time_point m_lastScan;
    bool m_heardSinceSearch = false;
    std::atomic<bool> m_scanning {false};
    bool m_stopping = false;

    std::condition_variable m_wake;
    std::multimap<Clock::time_point, std::function<void()>> m_jobs;
    std::thread m_timer;

    std::condition_variable m_answerWake;
    struct Answer {
        DeviceInfo info;
        std::string ip;
        std::uint32_t localAddress = 0;
        unsigned long interfaceIndex = 0;
    };
    std::deque<Answer> m_answers;
    std::thread m_answerer;

    std::thread m_scanner;
    Cutter m_scanCutter;
    Cutter m_answerCutter;
};

} // namespace localsend
