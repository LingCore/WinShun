#pragma once

// LocalSend's HTTPS with Windows' own TLS (Schannel; no OpenSSL): a
// credential per role and a channel over a connected socket. TLS 1.2 and
// 1.3 through SCH_CREDENTIALS; where Windows does not know that structure
// (Windows 10 before 1809) SCHANNEL_CRED with TLS 1.2.
//
// Both sides show the identity's certificate: LocalSend 1.18's server wants
// a client certificate and checks that it is the one a register names;
// ours asks for one but takes connections without (older senders have
// none). Certificates are self-signed, so nothing is checked against a CA:
// a client pins the fingerprint it expects, and a server learns the
// client's from the handshake.

#include "HttpLite.h"

#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#ifndef SCHANNEL_USE_BLACKLISTS
#define SCHANNEL_USE_BLACKLISTS // SCH_CREDENTIALS
#endif
#include <winsock2.h>

#include <windows.h>

#include <subauth.h>
#include <security.h>
#include <schannel.h>

#include <memory>
#include <string>

namespace localsend {

class Identity;

class Credentials {
public:
    enum class Role { Server, Client };
    static std::unique_ptr<Credentials> acquire(const Identity& identity, Role role, std::string* error);
    ~Credentials();
    Credentials(const Credentials&) = delete;
    Credentials& operator=(const Credentials&) = delete;

    CredHandle* handle() noexcept { return &m_handle; }
    Role role() const noexcept { return m_role; }
    bool legacy() const noexcept { return m_legacy; } // SCHANNEL_CRED: TLS 1.2 only

private:
    Credentials() = default;
    CredHandle m_handle {};
    Role m_role = Role::Server;
    bool m_legacy = false;
};

class TlsChannel final : public http::Stream {
public:
    // `socket` stays the caller's (closed by it, after this is gone).
    TlsChannel(SOCKET socket, Credentials& credentials);
    ~TlsChannel() override;
    TlsChannel(const TlsChannel&) = delete;
    TlsChannel& operator=(const TlsChannel&) = delete;

    bool handshake(std::string* error);
    // The peer's certificate fingerprint (SHA-256, upper-case hex), "" when
    // it showed none. A client compares it with the one it pins.
    const std::string& peerFingerprint() const noexcept { return m_peerFingerprint; }

    int receive(char* buffer, int size) override; // plaintext
    bool sendAll(const char* data, std::size_t size) override;
    void shutdown(); // close_notify, best effort

private:
    long step(); // one turn of the handshake; output tokens sent
    bool receiveMore();
    bool decryptSome(); // more plaintext into m_plain; false at the end or on errors
    void describe(); // the peer's fingerprint, the record sizes

    SOCKET m_socket;
    Credentials& m_credentials;
    CtxtHandle m_context {};
    bool m_haveContext = false;
    bool m_ready = false;
    bool m_closed = false;
    SecPkgContext_StreamSizes m_sizes {};
    std::string m_in; // encrypted bytes not used yet
    std::string m_plain; // decrypted bytes not read yet
    std::size_t m_plainAt = 0;
    std::string m_peerFingerprint;
};

} // namespace localsend
