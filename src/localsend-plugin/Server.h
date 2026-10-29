#pragma once

// Receiving: the HTTPS server LocalSend senders talk to (register, info,
// prepare-upload, upload, cancel), on the addresses of the networks where
// receiving is allowed (never 0.0.0.0), and only from those subnets.
//
// Nothing is saved without the user's yes: a prepare-upload waits, its
// connection held, until the user accepts or declines in the 传 list, or
// two minutes pass (LocalSend's sender waits for ever, so the deadline is
// ours), and is then answered 200 with tokens or 403. One transfer at a
// time: another request, a text message too, is answered 409 (as LocalSend
// does). A text message is shown, never written (204). Limits on
// everything a peer controls (see Server.cpp).

#include "RateLimits.h"
#include "Protocol.h"
#include "Saver.h"

#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace localsend {

class Credentials;
class TlsChannel;
namespace http {
class Reader;
struct Head;
} // namespace http

struct ReceiverConfig {
    DeviceInfo self; // what peers are told (port: set by listen())
    std::string pin; // "" for none
    std::wstring folder; // where files go
    std::uint64_t spareBytes = 1ull << 30; // left free on the disk: more than that is refused up front
};

struct IncomingRequest {
    std::uint64_t id = 0;
    DeviceInfo sender;
    bool verified = false; // it showed the certificate its fingerprint names
    std::vector<FileMeta> files;
    std::uint64_t totalBytes = 0;
    std::chrono::steady_clock::time_point deadline;
};

struct ReceiveProgress {
    DeviceInfo sender;
    std::size_t files = 0;
    std::size_t filesDone = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t receivedBytes = 0;
    double bytesPerSecond = 0;
};

struct ReceiveOutcome {
    // NoSpace, NoFolder: refused before anyone was asked (the folder's
    // drive is full, or not there: a USB disk pulled out, a share gone).
    enum class Kind { Received, Declined, TimedOut, SenderCancelled, Cancelled, Failed, NoSpace, NoFolder };
    Kind kind = Kind::Received;
    DeviceInfo sender;
    std::vector<std::wstring> saved; // full paths, in the order they came in
    std::size_t failed = 0; // files that did not come in whole
    std::uint64_t bytes = 0;
    std::wstring folder;
    Fault fault = Fault::Sender; // Failed: the worst a file failed for (this PC's disk before a checksum before the sender)
};

struct TextMessage {
    std::uint64_t id = 0;
    DeviceInfo sender;
    std::string text;
};

// One address to listen on: an interface's, with its subnet.
struct ListenAddress {
    std::string ip;
    std::uint32_t address = 0; // host order
    int prefix = 24;
};

class Receiver {
public:
    static constexpr auto kAnswerTime = std::chrono::seconds(120);
    static constexpr auto kStallTime = std::chrono::seconds(120); // no bytes for this long: the transfer failed
    // A request that ended untaken (declined, unanswered): its address is
    // answered 429 for this long, nobody asked, so a sender asking again and
    // again cannot keep the 传 list ringing. One given up by the sender (the
    // wrong file picked) may be sent again at once: only another untaken one
    // from there within kRepeatTime starts the pause. A refusal for want of
    // space or of a place to save is told once in kCooldown.
    static constexpr auto kCooldown = std::chrono::seconds(15);
    static constexpr auto kRepeatTime = std::chrono::seconds(60);
    static constexpr std::size_t kMaxConnections = 32;
    static constexpr int kMaxConnectionsPerPeer = 8;
    static constexpr int kPinAttempts = 3; // then 429, as LocalSend does
    // Requests from one address, any route: more in a second cuts its
    // connection off. Uploads into the transfer under way are not counted (a
    // folder of small files is many a second; their tokens are checked).
    static constexpr int kRequestsPerSecond = 20;
    // A prepare-upload's file list: kMaxFiles as LocalSend 1.18 words them
    // are some 2.8 MB (279 bytes each, ids and dates included), the rest is
    // room for longer names; more is 413, as more than kMaxFiles is. A body
    // this big is tens of megabytes parsed: at most ReadSlots::kInAll are read
    // at a time.
    static constexpr std::size_t kMaxPrepareBytes = 4 * 1024 * 1024;

    // Called on the server's threads; never with the receiver's lock held.
    struct Events {
        std::function<void(const IncomingRequest&)> asked; // waits for decide()
        std::function<void(const TextMessage&)> text;
        std::function<void(const ReceiveOutcome&)> finished;
        // A register: a device nearby (`verified`: it showed the certificate it names).
        std::function<void(const DeviceInfo& device, const std::string& ip, bool verified)> registered;
    };

    Receiver(Credentials& credentials, Events events);
    ~Receiver(); // stop(), and the connections' threads waited for
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    void setConfig(ReceiverConfig config);
    ReceiverConfig config() const;
    // Listening on these (none: not at all), on `port` or the next free one
    // up to kLastPort (LocalSend itself may hold 53317). The port in use,
    // 0 when none could be had. Listening already on the same: kept; on a
    // later port, back to `port` once it is free again, when `idle` (the
    // caller sends nothing either) and nothing is asked or received here.
    int listen(const std::vector<ListenAddress>& addresses, int port, bool idle = false);
    void stopListening();
    int port() const;
    std::vector<std::string> addresses() const;

    // The user's answer to the request waiting; false when it waits no more.
    bool decide(std::uint64_t requestId, bool accept);
    // The transfer under way or waiting ends; its partial files are deleted
    // (whole ones are kept). False when there was none.
    bool cancel();
    std::optional<IncomingRequest> pending() const;
    std::optional<ReceiveProgress> progress() const;
    void tick(); // about every second: deadlines, stalled transfers, the speed
    // Before the process ends (the host ends it soon after): nothing more
    // taken, the transfer cancelled, and up to `wait` for its uploads to
    // have deleted their partial files.
    void shutDown(std::chrono::milliseconds wait);

private:
    using Clock = std::chrono::steady_clock;
    struct Listener;
    struct Session;

    void acceptLoop(Listener* listener);
    bool admit(SOCKET socket, const std::string& peerIp); // false: not taken (the caller closes it)
    void connection(SOCKET socket, const std::string& peerIp); // a connection's thread
    void serve(SOCKET socket, const std::string& peerIp);
    bool allowed(const http::Head& head, const std::string& peerIp); // kRequestsPerSecond, the request counted
    bool route(TlsChannel& tls, http::Reader& reader, const http::Head& head, const std::string& peerIp, SOCKET socket);
    bool registerPeer(TlsChannel& tls, http::Reader& reader, const http::Head& head, const std::string& peerIp);
    bool prepareUpload(TlsChannel& tls, http::Reader& reader, const http::Head& head, const std::string& peerIp, SOCKET socket);
    bool upload(TlsChannel& tls, http::Reader& reader, const http::Head& head, const std::string& peerIp, SOCKET socket);
    bool cancelRequest(TlsChannel& tls, http::Reader& reader, const http::Head& head, const std::string& peerIp);
    // The connection is cut (tick()) when it is still at it at `until`.
    void deadline(SOCKET socket, Clock::time_point until);
    // Ends the session with `kind`, the outcome to tell in `outcome`; false
    // when it had ended already. m_mutex held.
    bool endLocked(Session& session, ReceiveOutcome::Kind kind, ReceiveOutcome& outcome);
    // endLocked() and told; m_mutex not held.
    void finish(const std::shared_ptr<Session>& session, ReceiveOutcome::Kind kind);
    void tell(const std::shared_ptr<Session>& session, const ReceiveOutcome& outcome);
    void settleIfDone(const std::shared_ptr<Session>& session);
    void release(const std::shared_ptr<Session>& session) noexcept; // a thread done with it
    // Once it ended early and nothing uses it any more: the folders it made, the empty ones, gone.
    void tidyIfIdle(const std::shared_ptr<Session>& session);

    Credentials& m_credentials;
    Events m_events;
    mutable std::mutex m_mutex;
    std::condition_variable m_answered;
    ReceiverConfig m_config;
    std::vector<std::unique_ptr<Listener>> m_listeners;
    int m_port = 0;
    std::shared_ptr<Session> m_session; // the one transfer, asked or under way
    std::uint64_t m_nextId = 1;
    std::map<std::string, int> m_perPeer; // connections by peer address
    std::map<SOCKET, Clock::time_point> m_connections; // and when each is cut
    struct Attempts {
        int count = 0;
        Clock::time_point last;
    };
    std::map<std::string, Attempts> m_pinAttempts;
    std::map<std::string, Clock::time_point> m_lastText; // by peer address
    PerSecond m_requests {kRequestsPerSecond}; // by peer address
    ReadSlots m_reading; // prepare-upload bodies being read
    struct PeerState {
        Clock::time_point coolUntil; // its requests answered 429 till then
        Clock::time_point untaken; // its last request that ended untaken
        Clock::time_point refusalTold; // last told "not enough space" or "no place to save"
        bool stale(Clock::time_point now) const
        {
            return now >= coolUntil && now - untaken >= kRepeatTime && now - refusalTold >= kCooldown;
        }
    };
    std::map<std::string, PeerState> m_peers; // by peer address
    bool m_stopping = false;
    int m_threads = 0; // connections being served
    std::condition_variable m_threadsDone; // and a session's threads letting go of it
};

} // namespace localsend
