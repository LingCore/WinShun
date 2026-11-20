#include "Crypto.h"

#include <windows.h>

#include <bcrypt.h>
#include <dpapi.h>

#include <array>
#include <stdexcept>

namespace shunsync {

namespace {

bool ok(NTSTATUS status) noexcept { return status >= 0; }

const UCHAR* bytesOf(std::string_view s) noexcept { return reinterpret_cast<const UCHAR*>(s.data()); }

Bytes hashWith(BCRYPT_ALG_HANDLE algorithm, std::string_view key, std::string_view data)
{
    Bytes out(32, '\0');
    // BCryptHash takes no null pointer for empty input on every Windows: one byte stands in.
    static const UCHAR none = 0;
    const NTSTATUS status = ::BCryptHash(algorithm, key.empty() ? nullptr : const_cast<UCHAR*>(bytesOf(key)),
        static_cast<ULONG>(key.size()), data.empty() ? const_cast<UCHAR*>(&none) : const_cast<UCHAR*>(bytesOf(data)),
        static_cast<ULONG>(data.size()), reinterpret_cast<UCHAR*>(out.data()), static_cast<ULONG>(out.size()));
    if (!ok(status))
        throw std::runtime_error("BCryptHash failed");
    return out;
}

constexpr char kHexDigits[] = "0123456789abcdef";

int hexValue(char c) noexcept
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::string encode64(std::string_view bytes, const char* alphabet, bool pad)
{
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8)
            | static_cast<unsigned char>(bytes[i + 2]);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }
    if (const std::size_t rest = bytes.size() - i; rest > 0) {
        unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        if (rest == 2)
            v |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        if (rest == 2)
            out += alphabet[(v >> 6) & 63];
        else if (pad)
            out += '=';
        if (pad)
            out += '=';
    }
    return out;
}

} // namespace

Bytes sha256(std::string_view data) { return hashWith(BCRYPT_SHA256_ALG_HANDLE, {}, data); }

Bytes hmacSha256(std::string_view key, std::string_view message)
{
    // HMAC pads the key with zero bytes to its block: an empty key is the
    // same as one zero byte, which CNG takes (it wants a key).
    return hashWith(BCRYPT_HMAC_SHA256_ALG_HANDLE, key.empty() ? std::string_view("\0", 1) : key, message);
}

Bytes hkdfSha256(std::string_view ikm, std::string_view salt, std::string_view info, std::size_t length)
{
    // RFC 5869: extract with the salt (zero bytes when there is none: the
    // same key to HMAC, see above), then expand.
    const Bytes prk = hmacSha256(salt, ikm);
    Bytes out;
    Bytes block;
    for (unsigned char counter = 1; out.size() < length; ++counter) {
        Bytes input = block;
        input.append(info);
        input += static_cast<char>(counter);
        block = hmacSha256(prk, input);
        out += block;
    }
    out.resize(length);
    return out;
}

Bytes randomBytes(std::size_t size)
{
    Bytes out(size, '\0');
    if (size > 0
        && !ok(::BCryptGenRandom(nullptr, reinterpret_cast<UCHAR*>(out.data()), static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        throw std::runtime_error("BCryptGenRandom failed");
    return out;
}

bool sameBytes(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

Sha256Stream::Sha256Stream()
{
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!ok(::BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0)))
        throw std::runtime_error("BCryptCreateHash failed");
    m_hash = hash;
}

Sha256Stream::~Sha256Stream()
{
    if (m_hash)
        ::BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(m_hash));
}

void Sha256Stream::add(std::string_view data)
{
    if (!data.empty() && m_hash)
        ::BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(m_hash), const_cast<UCHAR*>(bytesOf(data)), static_cast<ULONG>(data.size()), 0);
}

Bytes Sha256Stream::finish()
{
    Bytes out(32, '\0');
    if (!m_hash || !ok(::BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(m_hash), reinterpret_cast<UCHAR*>(out.data()), 32, 0)))
        throw std::runtime_error("BCryptFinishHash failed");
    ::BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(m_hash));
    m_hash = nullptr;
    return out;
}

AesGcm::AesGcm(std::string_view key)
{
    if (key.size() != 32)
        return;
    BCRYPT_KEY_HANDLE handle = nullptr;
    if (ok(::BCryptGenerateSymmetricKey(BCRYPT_AES_GCM_ALG_HANDLE, &handle, nullptr, 0, const_cast<UCHAR*>(bytesOf(key)),
            static_cast<ULONG>(key.size()), 0)))
        m_key = handle;
}

AesGcm::~AesGcm()
{
    if (m_key)
        ::BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(m_key));
}

Bytes AesGcm::seal(std::string_view nonce, std::string_view aad, std::string_view plaintext) const
{
    if (!m_key || nonce.size() != 12)
        throw std::runtime_error("AES-GCM: no key");
    std::array<UCHAR, 16> tag {};
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<UCHAR*>(bytesOf(nonce));
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbAuthData = aad.empty() ? nullptr : const_cast<UCHAR*>(bytesOf(aad));
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = tag.data();
    info.cbTag = static_cast<ULONG>(tag.size());
    Bytes out(plaintext.size() + tag.size(), '\0');
    ULONG written = 0;
    static UCHAR none = 0;
    if (!ok(::BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(m_key), plaintext.empty() ? &none : const_cast<UCHAR*>(bytesOf(plaintext)),
            static_cast<ULONG>(plaintext.size()), &info, nullptr, 0, reinterpret_cast<UCHAR*>(out.data()),
            static_cast<ULONG>(plaintext.size()), &written, 0)))
        throw std::runtime_error("BCryptEncrypt failed");
    std::copy(tag.begin(), tag.end(), reinterpret_cast<UCHAR*>(out.data()) + plaintext.size());
    return out;
}

std::optional<Bytes> AesGcm::open(std::string_view nonce, std::string_view aad, std::string_view sealed) const
{
    if (!m_key || nonce.size() != 12 || sealed.size() < 16)
        return std::nullopt;
    const std::size_t size = sealed.size() - 16;
    std::array<UCHAR, 16> tag {};
    std::copy(sealed.begin() + static_cast<std::ptrdiff_t>(size), sealed.end(), reinterpret_cast<char*>(tag.data()));
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<UCHAR*>(bytesOf(nonce));
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbAuthData = aad.empty() ? nullptr : const_cast<UCHAR*>(bytesOf(aad));
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = tag.data();
    info.cbTag = static_cast<ULONG>(tag.size());
    Bytes out(size, '\0');
    ULONG written = 0;
    static UCHAR none = 0;
    UCHAR spare = 0;
    if (!ok(::BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(m_key), size ? const_cast<UCHAR*>(bytesOf(sealed)) : &none,
            static_cast<ULONG>(size), &info, nullptr, 0, size ? reinterpret_cast<UCHAR*>(out.data()) : &spare,
            static_cast<ULONG>(size), &written, 0)))
        return std::nullopt;
    return out;
}

std::string hexLower(std::string_view bytes)
{
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char c : bytes) {
        out += kHexDigits[(static_cast<unsigned char>(c) >> 4) & 15];
        out += kHexDigits[static_cast<unsigned char>(c) & 15];
    }
    return out;
}

std::optional<Bytes> fromHex(std::string_view text)
{
    if (text.size() % 2)
        return std::nullopt;
    Bytes out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = hexValue(text[i]);
        const int lo = hexValue(text[i + 1]);
        if (hi < 0 || lo < 0)
            return std::nullopt;
        out += static_cast<char>((hi << 4) | lo);
    }
    return out;
}

std::string base64(std::string_view bytes)
{
    return encode64(bytes, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", true);
}

std::string base64UrlNoPad(std::string_view bytes)
{
    return encode64(bytes, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", false);
}

std::optional<Bytes> fromBase64(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1);
    while (!text.empty() && text.back() == '=')
        text.remove_suffix(1);
    if (text.size() % 4 == 1)
        return std::nullopt;
    Bytes out;
    out.reserve(text.size() * 3 / 4);
    unsigned v = 0;
    int bits = 0;
    for (const char c : text) {
        int d;
        if (c >= 'A' && c <= 'Z')
            d = c - 'A';
        else if (c >= 'a' && c <= 'z')
            d = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            d = c - '0' + 52;
        else if (c == '+' || c == '-')
            d = 62;
        else if (c == '/' || c == '_')
            d = 63;
        else
            return std::nullopt;
        v = (v << 6) | static_cast<unsigned>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((v >> bits) & 0xFF);
        }
    }
    return out;
}

std::string newUuid()
{
    Bytes b = randomBytes(16);
    b[6] = static_cast<char>((static_cast<unsigned char>(b[6]) & 0x0F) | 0x40);
    b[8] = static_cast<char>((static_cast<unsigned char>(b[8]) & 0x3F) | 0x80);
    return uuidText(b);
}

std::optional<Bytes> uuidBytes(std::string_view uuid)
{
    std::string digits;
    for (const char c : uuid) {
        if (c != '-')
            digits += c;
    }
    if (uuid.size() != 36 || digits.size() != 32 || uuid[8] != '-' || uuid[13] != '-' || uuid[18] != '-' || uuid[23] != '-')
        return std::nullopt;
    return fromHex(digits);
}

std::string uuidText(std::string_view bytes)
{
    if (bytes.size() != 16)
        return {};
    const std::string h = hexLower(bytes);
    return h.substr(0, 8) + '-' + h.substr(8, 4) + '-' + h.substr(12, 4) + '-' + h.substr(16, 4) + '-' + h.substr(20);
}

std::optional<Bytes> protectForUser(std::string_view secret)
{
    DATA_BLOB in {static_cast<DWORD>(secret.size()), const_cast<BYTE*>(reinterpret_cast<const BYTE*>(secret.data()))};
    DATA_BLOB out {};
    if (!::CryptProtectData(&in, L"ShunSync group key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return std::nullopt;
    Bytes blob(reinterpret_cast<const char*>(out.pbData), out.cbData);
    ::LocalFree(out.pbData);
    return blob;
}

std::optional<Bytes> unprotectForUser(std::string_view blob)
{
    DATA_BLOB in {static_cast<DWORD>(blob.size()), const_cast<BYTE*>(reinterpret_cast<const BYTE*>(blob.data()))};
    DATA_BLOB out {};
    if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return std::nullopt;
    Bytes secret(reinterpret_cast<const char*>(out.pbData), out.cbData);
    ::SecureZeroMemory(out.pbData, out.cbData);
    ::LocalFree(out.pbData);
    return secret;
}

} // namespace shunsync
