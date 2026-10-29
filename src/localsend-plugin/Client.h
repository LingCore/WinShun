#pragma once

// The client side of LocalSend: one request per connection to a device on
// the local network, out of the interface that network is on (bound to our
// address there, IP_UNICAST_IF: a VPN's default route does not swallow it),
// never through a proxy. HTTPS pins the fingerprint the device gave: a
// certificate that is not that one ends the connection before a byte of
// the request is sent.

#include "Protocol.h"

#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace localsend {

class Credentials;

// Where a request goes, and how.
struct Target {
    std::string ip;
    int port = kPort;
    bool https = true;
    std::string fingerprint; // pinned; "" only while scanning (learnt from the reply, checked by the caller)
    std::uint32_t localAddress = 0; // ours on that network (host order); 0: as routed
    unsigned long interfaceIndex = 0; // 0: as routed
};

struct Reply {
    int status = 0; // 0: no answer (`error` says why)
    std::string body;
    std::string error;
    std::string peerFingerprint; // the certificate the device showed
    bool wrongCertificate = false; // it was not the pinned one: another device is at that address now
};

// Requests under way, cut together (a send job's or a scan's; not each other's).
class Cutter {
public:
    void cut(); // every connection now ends; later ones end at once
    void reset(); // for the next job
    bool isCut() const noexcept { return m_cut; }
    // The client's own: a connection's socket while it is open.
    bool add(SOCKET s); // false: cut already
    void remove(SOCKET s);

private:
    std::mutex m_mutex;
    std::set<SOCKET> m_sockets;
    std::atomic<bool> m_cut {false};
};

class Client {
public:
    static constexpr auto kConnectTime = std::chrono::milliseconds(3000);
    static constexpr std::size_t kMaxReply = 64 * 1024; // a device's answer: more is not taken in

    explicit Client(Credentials& credentials) : m_credentials(credentials) {}

    // `timeout`: for the answer (a prepare-upload waits for a person).
    // `maxReply`: the answer's body at most (longer: status 0).
    Reply request(const Target& target, std::string_view method, const std::string& pathAndQuery, const std::string& body,
        std::chrono::milliseconds timeout, Cutter* cutter = nullptr, std::chrono::milliseconds connectTime = kConnectTime,
        std::size_t maxReply = kMaxReply);
    // A file's bytes with Content-Length; `sent` gets the bytes so far as
    // they go (false from it: stop). A file that is not `size` long (it
    // changed after it was chosen) fails before or as it is sent.
    Reply upload(const Target& target, const std::string& pathAndQuery, const std::wstring& file, std::uint64_t size,
        const std::function<bool(std::uint64_t sent)>& sent, Cutter* cutter = nullptr);

private:
    Credentials& m_credentials;
};

} // namespace localsend
