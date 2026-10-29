#pragma once

// This computer as LocalSend peers know it: a self-signed certificate
// (CN=WinShun, RSA-2048, SHA-256, valid from yesterday to 2099) whose
// private key is a named CNG key of the user's (the Microsoft Software Key
// Storage Provider, not exportable). Schannel needs a key it can open by
// name: neither a PFX imported without persisting it nor an ephemeral key
// works (AcquireCredentialsHandle fails: 0x8009030E as a server, 0x8009030D
// as a client; the #113 probe). The certificate and the key's name are kept
// in the plugin's data folder. Peers pin the fingerprint: the SHA-256 of
// the certificate, upper-case hex.

#include <windows.h>

#include <wincrypt.h>

#include <memory>
#include <string>

namespace localsend {

class Identity {
public:
    // The one in `folder`, else a new one (its files written there).
    static std::unique_ptr<Identity> loadOrCreate(const std::wstring& folder, std::string* error);
    // Deletes the key and the files: a new identity next time (peers then
    // see a new device). True when nothing of it is left.
    static bool forget(const std::wstring& folder);

    ~Identity();
    Identity(const Identity&) = delete;
    Identity& operator=(const Identity&) = delete;

    PCCERT_CONTEXT certificate() const noexcept { return m_cert; }
    const std::string& fingerprint() const noexcept { return m_fingerprint; }
    bool created() const noexcept { return m_created; } // made just now (not loaded)

private:
    Identity() = default;
    PCCERT_CONTEXT m_cert = nullptr;
    std::string m_fingerprint;
    bool m_created = false;
};

} // namespace localsend
