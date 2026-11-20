#pragma once

// ShunSync v1's engine (docs/shunsync-protocol.md): one device of a group of
// this user's computers (Windows and Mac顺) that share the clipboard.
//
// The group is a 32-byte key every member holds; nothing else is trusted.
// Each TCP connection is authenticated by it (X25519 per connection, HMAC
// proofs, AES-256-GCM both ways); a new device gets the key either by
// pairing (the other device opens a two-minute window, both screens show
// the same six digits, the user agrees there) or by an invite code that
// holds it. Removing a device makes a new key and gives it to the others.
//
// Who is where: UDP beacons on the LAN (port 37220, private networks only by
// default), the addresses that worked before, ones typed in, and what each
// member says it knows (auth_ok.peers, peer_update) -- so three or more
// devices end up all connected to each other.
//
// What is copied here goes to every member at once and only from here
// (one hop: what came from elsewhere is never sent on). What comes is put on
// this PC's clipboard when it is the newest (timestamp, then device id) and
// "收到就放进剪贴板" is on; a copy that only puts it back (the clipboard
// watcher seeing our own write) is not sent again. Files: the list goes at
// once, the bytes are pulled from the device they were copied on over a
// connection of their own (mode "file"): small ones by themselves, larger
// ones when the user asks (the 同步 list), into <data>\SharedFiles.
//
// Threads: one accepting, two per connection (reading; writing a queue),
// one for beacons, one ticking (pings, reconnects, expiry). Events come on
// any of them.

#include "Crypto.h"
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
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace winshun {
class Json;
}

namespace shunsync {

// Where what comes from other devices goes: the system clipboard, or the
// tests' record of it. Called on the engine's threads, never under its lock.
class ClipboardSink {
public:
    virtual ~ClipboardSink() = default;
    virtual bool putText(const std::string& text, const std::string& originDeviceId) = 0; // '\n' line ends
    virtual bool putImage(const Bytes& png, const std::string& originDeviceId) = 0;
    virtual bool putFiles(const std::vector<std::wstring>& paths, const std::string& originDeviceId) = 0;
};

struct Options {
    std::string deviceName;
    std::string platform = "windows";
    std::wstring dataDir; // identity.json and SharedFiles
    std::string bindAddress = "0.0.0.0"; // the tests: 127.0.0.1
    std::uint16_t preferredPort = kTcpPort; // taken: one Windows picks, told in beacons
    bool discovery = true; // UDP beacons (off in the tests)
    bool autoWrite = true; // what comes goes on the clipboard
    bool shareFiles = true;
    std::uint64_t prefetchBytes = 0; // files up to this come by themselves (0: none, Mac顺's default)
    bool fileNotice = true; // a notification when files copied elsewhere wait here
    bool acceptPairing = true; // pairing asked for answered without the 2-minute window (Mac顺's default)
    bool paused = false; // nothing sent, nothing taken
    std::vector<std::string> addresses; // "host:port" typed in
    bool allNetworks = false; // beacons on public networks too
    bool directFiles = true; // the tests: false, every pull over the sync link (the fallback)
    bool chinese = true; // the notifications' language
};

struct Events {
    std::function<void()> changed; // what the 同步 list shows changed
    std::function<void(const std::string& title, const std::string& text)> toast;
    std::function<void(const std::string& line)> log; // nothing private: no text, no file names
};

enum class PeerState { Offline, Connecting, Online };

struct MemberView {
    std::string deviceId;
    std::string name;
    std::string platform;
    std::string host;
    std::uint16_t port = 0;
    PeerState state = PeerState::Offline;
    std::int64_t lastSeenMs = 0;
};

struct NearbyView { // a beacon from a device of another group
    std::string deviceId;
    std::string name;
    std::string platform;
    std::string host;
    std::uint16_t port = 0;
    bool pairingOpen = false;
};

struct PairView {
    std::uint64_t id = 0;
    bool incoming = false; // another device asks to join ours
    std::string name;
    std::string platform;
    std::string sas; // "" until the handshake is done
    enum class Stage { Connecting, Confirm, Done, Failed } stage = Stage::Connecting;
    std::string problem; // Failed: "pairing_closed", "declined", "unreachable", "lost"
};

struct FilesView {
    std::string transferId;
    std::string originName;
    std::size_t files = 0; // copied as such (top level), not the files in the folders
    std::size_t folders = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t receivedBytes = 0;
    enum class Stage { Offered, Pulling, Ready, Failed } stage = Stage::Offered;
    std::string problem;
    std::int64_t timestampMs = 0;
};

struct ReceivedView {
    std::string eventId;
    std::string originName;
    std::int64_t timestampMs = 0;
    enum class Kind { Text, Image, Files } kind = Kind::Text;
    std::string preview; // text: its start; image: "1200 × 800"
    std::string transferId; // files
    bool written = false; // put on the clipboard
};

struct Snapshot {
    bool running = false;
    std::string problem; // why not running
    std::string deviceId;
    std::string deviceName;
    std::string groupId;
    std::uint16_t port = 0;
    bool paused = false;
    std::int64_t pairingLeftMs = 0; // the window open for new devices: how long still
    bool acceptingPairing = false; // pairing asked for is answered now (always, or the window)
    std::vector<MemberView> members;
    std::vector<NearbyView> nearby;
    std::vector<PairView> pairs;
    std::vector<FilesView> files;
    std::vector<ReceivedView> received; // newest first
};

class Engine {
public:
    Engine(Options options, ClipboardSink& sink, Events events);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool start(std::string* problem = nullptr);
    void stop();
    void setOptions(const Options& options); // name, switches, addresses (dataDir and the port stay)

    // This PC's clipboard changed: what it holds now. `target`: to that member only.
    void localText(std::string text, const std::string& target = {});
    void localImage(Bytes png, int width, int height, const std::string& target = {});
    void localFiles(std::vector<std::wstring> paths, const std::string& target = {});

    Snapshot snapshot() const;
    std::uint16_t port() const noexcept { return m_port.load(); }
    std::string deviceId() const;

    void openPairing(std::chrono::seconds length = std::chrono::seconds(120));
    void closePairing();
    // Ask to join the group of a device seen nearby (its id) or at "host:port".
    std::uint64_t pairWith(const std::string& deviceIdOrAddress);
    void answerPair(std::uint64_t id, bool accept);
    void cancelPair(std::uint64_t id); // ours, or forget a finished one
    std::string invite() const; // the code, with this PC's address
    bool joinWithInvite(const std::string& text);
    void removeDevice(const std::string& deviceId); // a new key for the others
    // Onto the clipboard when here, or (toClipboard false: 存到下载) only into SharedFiles.
    void pullFiles(const std::string& transferId, bool toClipboard = true);
    void cancelPull(const std::string& transferId); // what came so far deleted
    // A received text or image put on the clipboard again (Enter in the list).
    bool rewrite(const std::string& eventId);

private:
    struct Conn;
    struct Peer {
        std::string deviceId;
        std::string name;
        std::string platform;
        std::string host;
        std::uint16_t port = 0;
        std::int64_t lastSeenMs = 0;
        std::int64_t nextTryMs = 0;
        int failures = 0;
        bool connecting = false;
    };
    struct Nearby {
        NearbyView view;
        std::string groupId;
        std::int64_t seenMs = 0;
    };
    struct Pair {
        PairView view;
        std::shared_ptr<Conn> conn;
        std::string peerId;
        std::string host;
        std::uint16_t peerPort = 0;
        std::int64_t updatedMs = 0;
    };
    struct OutgoingFile {
        std::wstring path;
        std::uint64_t size = 0;
    };
    struct Outgoing { // files copied here, offered to the others
        std::vector<OutgoingFile> files; // by fileIndex (folders: empty path)
        std::int64_t createdMs = 0;
    };
    struct EntryIn {
        std::uint32_t index = 0;
        std::string relativePath;
        bool directory = false;
        std::uint64_t size = 0;
        std::int64_t modifiedMs = 0;
    };
    struct Incoming { // files copied elsewhere
        FilesView view;
        std::string originId;
        std::vector<EntryIn> entries;
        std::string eventKey; // timestamp + origin, for "is it still the newest"
        std::int64_t eventTs = 0;
        bool writeWhenDone = false;
        std::shared_ptr<std::atomic_bool> cancel;
    };
    // A pull over a member's sync connection (§4.4's fallback: a file
    // connection to it could not be made -- NAT, a second router): what
    // comes for its transferId (0x02 chunks, file_pull_done / _err) is handed
    // over from that connection's reader.
    struct Relay {
        std::shared_ptr<Conn> link;
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<Bytes> frames; // plaintexts, kind byte first
        std::size_t bytes = 0;
        bool lost = false; // the link ended, or the pull fell too far behind
    };
    struct Received {
        ReceivedView view;
        std::string text;
        Bytes png;
        std::string originId;
    };

    // Connections
    void acceptLoop(SOCKET listener);
    void serve(std::shared_ptr<Conn> conn); // an accepted one: the server's handshake, then frames
    void dial(std::string host, std::uint16_t port, std::string expectId); // sync, to a member
    void runSync(const std::shared_ptr<Conn>& conn); // frames until it ends
    bool clientHandshake(Conn& conn, const std::string& mode, std::string* problem);
    void adopt(const std::shared_ptr<Conn>& conn); // an authenticated sync connection
    void dropped(const std::shared_ptr<Conn>& conn);
    void onJson(const std::shared_ptr<Conn>& conn, const std::string& json);
    void onImage(const std::shared_ptr<Conn>& conn, std::string_view payload);
    void serveFiles(const std::shared_ptr<Conn>& conn); // a file connection's requests
    // One file_pull_req answered through `emit` (chunks, then done or err);
    // false when `emit` failed (the connection is gone).
    bool serveFile(const std::string& transferId, long long index, long long offset, const std::function<bool(Bytes)>& emit);
    bool sendPaced(const std::shared_ptr<Conn>& conn, Bytes plaintext); // waits while much is queued
    void relayed(const std::string& transferId, Bytes plaintext);
    void send(const std::shared_ptr<Conn>& conn, Bytes plaintext);
    void sendJson(const std::shared_ptr<Conn>& conn, const winshun::Json& message);
    void broadcast(const Bytes& plaintext, const std::string& target);
    std::vector<std::shared_ptr<Conn>> links() const; // authenticated sync connections
    void spawn(const char* what, std::function<void()> work);
    void reap(bool all);

    // Group
    void loadIdentity();
    void saveIdentityLocked() const;
    void newGroupLocked(Bytes key, bool keepPeers);
    void rotate(const Bytes& key, const std::vector<std::string>& revoked);
    void closeAllLinks();
    winshun::Json peersLocked(const std::string& except) const; // for auth_ok, peer_update, pair_grant
    void learnPeers(const winshun::Json& peers);
    void notePeerLocked(const std::string& id, const std::string& name, const std::string& platform,
        const std::string& host, std::uint16_t port);

    // Pairing
    void pairClient(std::uint64_t id, std::string host, std::uint16_t port);
    void serverPair(const std::shared_ptr<Conn>& conn, const std::string& peerId, const std::string& name,
        const std::string& platform, const std::string& sas, std::uint16_t listenPort);

    // Clipboard
    bool newest(std::int64_t ts, const std::string& origin);
    void recordReceived(Received received);
    bool echoOf(const std::string& fingerprint);
    std::string originName(const std::string& id) const;

    // Files
    void filesManifest(std::vector<std::wstring> paths, std::string target);
    void pull(const std::string& transferId);
    void sweepCache();

    // Discovery and the clock
    void beaconLoop();
    void sendBeacons(SOCKET udp);
    void tick();
    void tickLoop();

    void changed();
    void log(const std::string& line);
    void toast(const std::string& title, const std::string& text);
    bool zh() const; // never with m_mutex held

    Options m_options;
    ClipboardSink& m_sink;
    Events m_events;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::atomic_bool m_stopping {false};
    bool m_running = false;
    std::string m_problem;
    std::atomic<std::uint16_t> m_port {0};
    SOCKET m_listener = INVALID_SOCKET;
    SOCKET m_udp = INVALID_SOCKET;

    std::string m_deviceId;
    Bytes m_groupKey;
    std::string m_groupId;
    std::vector<std::string> m_revoked;
    std::map<std::string, Peer> m_peers; // members known, by id (not this PC)
    std::map<std::string, std::shared_ptr<Conn>> m_links; // online members
    std::set<std::shared_ptr<Conn>> m_conns; // every open connection
    std::map<std::string, Nearby> m_nearby;
    std::map<std::string, std::int64_t> m_dialing; // "host:port" being dialed -> since
    std::int64_t m_pairingUntil = 0;
    std::map<std::string, std::int64_t> m_pairAsked; // who asked to pair (device id, or address), when
    std::map<std::uint64_t, Pair> m_pairs;
    std::uint64_t m_nextPair = 1;
    std::map<std::string, Outgoing> m_outgoing;
    std::map<std::string, Incoming> m_incoming;
    std::map<std::string, std::shared_ptr<Relay>> m_relays; // by transferId
    std::deque<Received> m_received;
    std::deque<std::pair<std::string, std::int64_t>> m_seenEvents; // event ids, for duplicates
    std::int64_t m_lastTs = 0; // the newest clip, here or anywhere
    std::string m_lastOrigin;
    std::string m_echoFingerprint; // what we just wrote, for 5 seconds
    std::int64_t m_echoUntil = 0;
    std::string m_lastSentFingerprint;
    std::int64_t m_lastSentMs = 0;

    std::mutex m_threadsMutex;
    std::vector<std::pair<std::thread, std::shared_ptr<std::atomic_bool>>> m_threads;
    std::thread m_acceptThread;
    std::thread m_beaconThread;
    std::thread m_tickThread;
};

std::int64_t nowMs();

} // namespace shunsync
