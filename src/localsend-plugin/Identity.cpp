#include "Identity.h"

#include "Util.h"

#include <ncrypt.h>
#include <objbase.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace localsend {

namespace {

constexpr wchar_t kCertFile[] = L"\\identity.der";
constexpr wchar_t kKeyNameFile[] = L"\\identity.key";
constexpr wchar_t kKeyPrefix[] = L"WinShun.LocalSend.";

std::string readAll(const std::wstring& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

bool writeAll(const std::wstring& path, const void* data, std::size_t size)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return f.good();
}

struct Provider {
    NCRYPT_PROV_HANDLE h = 0;
    Provider() { ::NCryptOpenStorageProvider(&h, MS_KEY_STORAGE_PROVIDER, 0); }
    ~Provider()
    {
        if (h)
            ::NCryptFreeObject(h);
    }
    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;
};

struct Key {
    NCRYPT_KEY_HANDLE h = 0;
    Key() = default;
    Key(const Key&) = delete; // a copy would free the handle twice
    Key& operator=(const Key&) = delete;
    ~Key()
    {
        if (h)
            ::NCryptFreeObject(h);
    }
};

std::string status(const char* what, SECURITY_STATUS st)
{
    char text[96];
    std::snprintf(text, sizeof text, "%s 0x%08lX", what, static_cast<unsigned long>(st));
    return text;
}

PCCERT_CONTEXT selfSign(NCRYPT_KEY_HANDLE key, const std::wstring& keyName, std::string* error)
{
    BYTE name[256];
    DWORD size = sizeof name;
    if (!::CertStrToNameW(X509_ASN_ENCODING, L"CN=WinShun", CERT_X500_NAME_STR, nullptr, name, &size, nullptr)) {
        *error = status("CertStrToName", static_cast<SECURITY_STATUS>(::GetLastError()));
        return nullptr;
    }
    CERT_NAME_BLOB subject {size, name};
    CRYPT_ALGORITHM_IDENTIFIER algorithm {};
    algorithm.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);
    // From a day back: the peer's clock may be behind ours.
    FILETIME now;
    ::GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t {{now.dwLowDateTime, now.dwHighDateTime}};
    t.QuadPart -= 24ull * 3600 * 10'000'000;
    now = {t.LowPart, t.HighPart};
    SYSTEMTIME start {};
    ::FileTimeToSystemTime(&now, &start);
    SYSTEMTIME end {2099, 12, 0, 31, 23, 59, 59, 0};
    CRYPT_KEY_PROV_INFO provider {const_cast<LPWSTR>(keyName.c_str()), const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER), 0, 0, 0, nullptr, 0};
    PCCERT_CONTEXT cert = ::CertCreateSelfSignCertificate(static_cast<HCRYPTPROV_OR_NCRYPT_KEY_HANDLE>(key), &subject, 0, &provider,
        &algorithm, &start, &end, nullptr);
    if (!cert)
        *error = status("CertCreateSelfSignCertificate", static_cast<SECURITY_STATUS>(::GetLastError()));
    return cert;
}

} // namespace

std::unique_ptr<Identity> Identity::loadOrCreate(const std::wstring& folder, std::string* error)
{
    ::CreateDirectoryW(folder.c_str(), nullptr);
    Provider provider;
    if (!provider.h) {
        *error = "no key storage provider";
        return nullptr;
    }
    std::unique_ptr<Identity> identity(new Identity);
    std::wstring keyName = wide(readAll(folder + kKeyNameFile));
    const std::string der = readAll(folder + kCertFile);
    Key key;
    if (!keyName.empty() && !der.empty() && ::NCryptOpenKey(provider.h, &key.h, keyName.c_str(), 0, NCRYPT_SILENT_FLAG) == ERROR_SUCCESS) {
        identity->m_cert = ::CertCreateCertificateContext(X509_ASN_ENCODING, reinterpret_cast<const BYTE*>(der.data()),
            static_cast<DWORD>(der.size()));
        if (identity->m_cert) {
            // Schannel finds the key through this property: by its name.
            CRYPT_KEY_PROV_INFO info {keyName.data(), const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER), 0, 0, 0, nullptr, 0};
            ::CertSetCertificateContextProperty(identity->m_cert, CERT_KEY_PROV_INFO_PROP_ID, 0, &info);
        }
    }
    if (!identity->m_cert) {
        // None yet (or its key is gone): a new one. A key left behind under
        // the old name goes first.
        if (!keyName.empty() && keyName.rfind(kKeyPrefix, 0) == 0) {
            Key old;
            if (::NCryptOpenKey(provider.h, &old.h, keyName.c_str(), 0, NCRYPT_SILENT_FLAG) == ERROR_SUCCESS
                && ::NCryptDeleteKey(old.h, NCRYPT_SILENT_FLAG) == ERROR_SUCCESS)
                old.h = 0; // deleting frees it
        }
        GUID guid {};
        ::CoCreateGuid(&guid);
        wchar_t text[64];
        ::StringFromGUID2(guid, text, 64);
        keyName = std::wstring(kKeyPrefix) + text;
        Key fresh;
        SECURITY_STATUS st = ::NCryptCreatePersistedKey(provider.h, &fresh.h, BCRYPT_RSA_ALGORITHM, keyName.c_str(), 0, 0);
        if (st != ERROR_SUCCESS) {
            *error = status("NCryptCreatePersistedKey", st);
            return nullptr;
        }
        DWORD bits = 2048;
        ::NCryptSetProperty(fresh.h, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&bits), sizeof bits, 0);
        st = ::NCryptFinalizeKey(fresh.h, NCRYPT_SILENT_FLAG);
        if (st != ERROR_SUCCESS) {
            *error = status("NCryptFinalizeKey", st); // 0x80090010 at low integrity: the plugin needs the normal sandbox
            ::NCryptDeleteKey(fresh.h, NCRYPT_SILENT_FLAG);
            fresh.h = 0;
            return nullptr;
        }
        identity->m_cert = selfSign(fresh.h, keyName, error);
        if (!identity->m_cert) {
            ::NCryptDeleteKey(fresh.h, NCRYPT_SILENT_FLAG);
            fresh.h = 0;
            return nullptr;
        }
        const std::string name = narrow(keyName);
        if (!writeAll(folder + kKeyNameFile, name.data(), name.size())
            || !writeAll(folder + kCertFile, identity->m_cert->pbCertEncoded, identity->m_cert->cbCertEncoded)) {
            *error = "the identity could not be saved in the data folder";
            ::NCryptDeleteKey(fresh.h, NCRYPT_SILENT_FLAG);
            fresh.h = 0;
            return nullptr;
        }
        identity->m_created = true;
    }
    identity->m_fingerprint = sha256Hex(identity->m_cert->pbCertEncoded, identity->m_cert->cbCertEncoded);
    return identity;
}

bool Identity::forget(const std::wstring& folder)
{
    const std::wstring keyName = wide(readAll(folder + kKeyNameFile));
    bool gone = true;
    if (!keyName.empty() && keyName.rfind(kKeyPrefix, 0) == 0) {
        Provider provider;
        Key key;
        if (provider.h && ::NCryptOpenKey(provider.h, &key.h, keyName.c_str(), 0, NCRYPT_SILENT_FLAG) == ERROR_SUCCESS) {
            if (::NCryptDeleteKey(key.h, NCRYPT_SILENT_FLAG) == ERROR_SUCCESS)
                key.h = 0;
            else
                gone = false;
        }
    }
    ::DeleteFileW((folder + kKeyNameFile).c_str());
    ::DeleteFileW((folder + kCertFile).c_str());
    return gone;
}

Identity::~Identity()
{
    if (m_cert)
        ::CertFreeCertificateContext(m_cert);
}

} // namespace localsend
