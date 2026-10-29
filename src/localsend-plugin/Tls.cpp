#include "Tls.h"

#include "Identity.h"
#include "Util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace localsend {

namespace {

std::string statusText(const char* what, SECURITY_STATUS st)
{
    char text[96];
    std::snprintf(text, sizeof text, "%s 0x%08lX", what, static_cast<unsigned long>(st));
    return text;
}

bool sendRaw(SOCKET s, const char* data, std::size_t size)
{
    while (size > 0) {
        const int sent = ::send(s, data, static_cast<int>(std::min<std::size_t>(size, 1 << 20)), 0);
        if (sent <= 0)
            return false;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

} // namespace

std::unique_ptr<Credentials> Credentials::acquire(const Identity& identity, Role role, std::string* error)
{
    std::unique_ptr<Credentials> c(new Credentials);
    c->m_role = role;
    const bool server = role == Role::Server;
    PCCERT_CONTEXT certs[1] = {identity.certificate()};
    TimeStamp expiry {};
    // A client checks the server's certificate itself (pinning), and never
    // offers a certificate of the user's from the Windows store.
    const DWORD flags = server ? (SCH_USE_STRONG_CRYPTO | SCH_CRED_NO_SYSTEM_MAPPER)
                               : (SCH_USE_STRONG_CRYPTO | SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS);
    TLS_PARAMETERS tls {};
    tls.grbitDisabledProtocols = server ? (SP_PROT_SSL3_SERVER | SP_PROT_TLS1_0_SERVER | SP_PROT_TLS1_1_SERVER)
                                        : (SP_PROT_SSL3_CLIENT | SP_PROT_TLS1_0_CLIENT | SP_PROT_TLS1_1_CLIENT);
    SCH_CREDENTIALS modern {};
    modern.dwVersion = SCH_CREDENTIALS_VERSION;
    modern.cCreds = 1;
    modern.paCred = certs;
    modern.cTlsParameters = 1;
    modern.pTlsParameters = &tls;
    modern.dwFlags = flags;
    SECURITY_STATUS st = ::AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W),
        server ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND, nullptr, &modern, nullptr, nullptr, &c->m_handle, &expiry);
    if (st == SEC_E_UNKNOWN_CREDENTIALS || st == SEC_E_INTERNAL_ERROR || st == SEC_E_UNSUPPORTED_FUNCTION) {
        // Windows 10 before 1809 has no SCH_CREDENTIALS.
        SCHANNEL_CRED legacy {};
        legacy.dwVersion = SCHANNEL_CRED_VERSION;
        legacy.cCreds = 1;
        legacy.paCred = certs;
        legacy.grbitEnabledProtocols = server ? SP_PROT_TLS1_2_SERVER : SP_PROT_TLS1_2_CLIENT;
        legacy.dwFlags = flags;
        st = ::AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), server ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND,
            nullptr, &legacy, nullptr, nullptr, &c->m_handle, &expiry);
        c->m_legacy = st == SEC_E_OK;
    }
    if (st != SEC_E_OK) {
        *error = statusText("AcquireCredentialsHandle", st);
        SecInvalidateHandle(&c->m_handle);
        return nullptr;
    }
    return c;
}

Credentials::~Credentials()
{
    if (SecIsValidHandle(&m_handle))
        ::FreeCredentialsHandle(&m_handle);
}

TlsChannel::TlsChannel(SOCKET socket, Credentials& credentials)
    : m_socket(socket)
    , m_credentials(credentials)
{
}

TlsChannel::~TlsChannel()
{
    if (m_haveContext)
        ::DeleteSecurityContext(&m_context);
}

bool TlsChannel::receiveMore()
{
    char buffer[32 * 1024];
    const int got = ::recv(m_socket, buffer, sizeof buffer, 0);
    if (got <= 0)
        return false;
    m_in.append(buffer, static_cast<std::size_t>(got));
    return true;
}

// One turn of the handshake for either side, on what came so far (m_in);
// what it wants sent is sent. The status.
long TlsChannel::step()
{
    const bool server = m_credentials.role() == Credentials::Role::Server;
    SecBuffer in[2] = {{static_cast<ULONG>(m_in.size()), SECBUFFER_TOKEN, m_in.empty() ? nullptr : m_in.data()}, {0, SECBUFFER_EMPTY, nullptr}};
    SecBufferDesc inDesc {SECBUFFER_VERSION, 2, in};
    SecBuffer out[2] = {{0, SECBUFFER_TOKEN, nullptr}, {0, SECBUFFER_ALERT, nullptr}};
    SecBufferDesc outDesc {SECBUFFER_VERSION, 2, out};
    ULONG got = 0;
    TimeStamp expiry {};
    SECURITY_STATUS st;
    if (server) {
        constexpr ULONG kFlags = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY | ASC_REQ_EXTENDED_ERROR
            | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_MUTUAL_AUTH; // asks for the client's certificate; takes none too
        st = ::AcceptSecurityContext(m_credentials.handle(), m_haveContext ? &m_context : nullptr, &inDesc, kFlags, 0,
            m_haveContext ? nullptr : &m_context, &outDesc, &got, &expiry);
    } else {
        constexpr ULONG kFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR
            | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_USE_SUPPLIED_CREDS;
        st = ::InitializeSecurityContextW(m_credentials.handle(), m_haveContext ? &m_context : nullptr, nullptr, kFlags, 0, 0,
            m_haveContext ? &inDesc : nullptr, 0, m_haveContext ? nullptr : &m_context, &outDesc, &got, &expiry);
    }
    if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED || st == SEC_I_INCOMPLETE_CREDENTIALS || (FAILED(st) && out[0].cbBuffer))
        m_haveContext = true;
    if (out[0].cbBuffer && out[0].pvBuffer) {
        sendRaw(m_socket, static_cast<const char*>(out[0].pvBuffer), out[0].cbBuffer); // an alert too, on failures
        ::FreeContextBuffer(out[0].pvBuffer);
    }
    if (out[1].pvBuffer)
        ::FreeContextBuffer(out[1].pvBuffer);
    if (st == SEC_E_INCOMPLETE_MESSAGE)
        return st;
    if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer && in[1].cbBuffer <= m_in.size())
        m_in.erase(0, m_in.size() - in[1].cbBuffer); // the start of what comes next
    else if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED || st == SEC_I_INCOMPLETE_CREDENTIALS)
        m_in.clear();
    return st;
}

bool TlsChannel::handshake(std::string* error)
{
    const bool server = m_credentials.role() == Credentials::Role::Server;
    if (!server) {
        const long st = step(); // the ClientHello
        if (st != SEC_I_CONTINUE_NEEDED) {
            *error = statusText("InitializeSecurityContext", st);
            return false;
        }
    }
    for (int turns = 0; turns < 64; ++turns) {
        if (m_in.empty() && !receiveMore()) {
            *error = "the connection closed during the handshake";
            return false;
        }
        const long st = step();
        if (st == SEC_E_INCOMPLETE_MESSAGE) {
            if (!receiveMore()) {
                *error = "the connection closed during the handshake";
                return false;
            }
            continue;
        }
        if (st == SEC_I_CONTINUE_NEEDED || st == SEC_I_INCOMPLETE_CREDENTIALS) // asked for a certificate: ours is in the credential
            continue;
        if (st == SEC_E_OK) {
            describe();
            m_ready = true;
            return true;
        }
        *error = statusText(server ? "AcceptSecurityContext" : "InitializeSecurityContext", st);
        return false;
    }
    *error = "the handshake did not end";
    return false;
}

void TlsChannel::describe()
{
    PCCERT_CONTEXT remote = nullptr;
    if (::QueryContextAttributesW(&m_context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remote) == SEC_E_OK && remote) {
        m_peerFingerprint = sha256Hex(remote->pbCertEncoded, remote->cbCertEncoded);
        ::CertFreeCertificateContext(remote);
    }
    ::QueryContextAttributesW(&m_context, SECPKG_ATTR_STREAM_SIZES, &m_sizes);
}

bool TlsChannel::sendAll(const char* data, std::size_t size)
{
    if (!m_ready || m_closed)
        return false;
    std::vector<char> buffer(m_sizes.cbHeader + m_sizes.cbMaximumMessage + m_sizes.cbTrailer);
    while (size > 0) {
        const std::size_t chunk = std::min<std::size_t>(size, m_sizes.cbMaximumMessage);
        std::memcpy(buffer.data() + m_sizes.cbHeader, data, chunk);
        SecBuffer b[4] = {{m_sizes.cbHeader, SECBUFFER_STREAM_HEADER, buffer.data()},
            {static_cast<ULONG>(chunk), SECBUFFER_DATA, buffer.data() + m_sizes.cbHeader},
            {m_sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, buffer.data() + m_sizes.cbHeader + chunk}, {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc desc {SECBUFFER_VERSION, 4, b};
        if (::EncryptMessage(&m_context, 0, &desc, 0) != SEC_E_OK)
            return false;
        if (!sendRaw(m_socket, buffer.data(), b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer))
            return false;
        data += chunk;
        size -= chunk;
    }
    return true;
}

bool TlsChannel::decryptSome()
{
    for (;;) {
        if (m_in.empty() && !receiveMore()) {
            m_closed = true;
            return false;
        }
        SecBuffer b[4] = {{static_cast<ULONG>(m_in.size()), SECBUFFER_DATA, m_in.data()}, {0, SECBUFFER_EMPTY, nullptr},
            {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc desc {SECBUFFER_VERSION, 4, b};
        const SECURITY_STATUS st = ::DecryptMessage(&m_context, &desc, 0, nullptr);
        if (st == SEC_E_INCOMPLETE_MESSAGE) {
            if (!receiveMore()) {
                m_closed = true;
                return false;
            }
            continue;
        }
        if (st != SEC_E_OK && st != SEC_I_RENEGOTIATE && st != SEC_I_CONTEXT_EXPIRED)
            return false;
        bool data = false;
        std::string extra;
        for (const SecBuffer& x : b) {
            if (x.BufferType == SECBUFFER_DATA && x.cbBuffer) {
                if (m_plainAt > 0) {
                    m_plain.erase(0, m_plainAt);
                    m_plainAt = 0;
                }
                m_plain.append(static_cast<const char*>(x.pvBuffer), x.cbBuffer);
                data = true;
            } else if (x.BufferType == SECBUFFER_EXTRA && x.cbBuffer) {
                extra.assign(static_cast<const char*>(x.pvBuffer), x.cbBuffer);
            }
        }
        m_in = std::move(extra);
        if (st == SEC_I_CONTEXT_EXPIRED) { // close_notify
            m_closed = true;
            return data;
        }
        if (st == SEC_I_RENEGOTIATE) {
            // TLS 1.3's messages after the handshake (a session ticket, a key
            // update): they go back through the handshake call.
            const long r = step();
            if (r != SEC_E_OK && r != SEC_I_CONTINUE_NEEDED)
                return false;
        }
        if (data)
            return true;
    }
}

int TlsChannel::receive(char* buffer, int size)
{
    if (!m_ready)
        return -1;
    if (m_plainAt >= m_plain.size()) {
        m_plain.clear();
        m_plainAt = 0;
        if (!decryptSome())
            return m_closed ? 0 : -1;
    }
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(size), m_plain.size() - m_plainAt);
    std::memcpy(buffer, m_plain.data() + m_plainAt, n);
    m_plainAt += n;
    return static_cast<int>(n);
}

void TlsChannel::shutdown()
{
    if (!m_haveContext || !m_ready)
        return;
    DWORD type = SCHANNEL_SHUTDOWN;
    SecBuffer b {sizeof type, SECBUFFER_TOKEN, &type};
    SecBufferDesc desc {SECBUFFER_VERSION, 1, &b};
    if (::ApplyControlToken(&m_context, &desc) != SEC_E_OK)
        return;
    m_in.clear();
    step(); // sends close_notify
    m_ready = false;
}

} // namespace localsend
