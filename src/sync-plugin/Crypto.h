#pragma once

// ShunSync v1's primitives (docs/shunsync-protocol.md), with Windows' own
// CNG (bcrypt): SHA-256, HMAC-SHA256, HKDF-SHA256 (RFC 5869, on HMAC),
// AES-256-GCM and the system's random bytes. X25519 (RFC 7748) is written out
// in X25519.cpp: CNG's curve25519 takes its keys only in a blob of its own,
// and the test vectors' fixed private keys must go in as they are.
//
// Bytes are std::string here: what goes on a socket and into JSON as base64.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace shunsync {

using Bytes = std::string;

Bytes sha256(std::string_view data);
Bytes hmacSha256(std::string_view key, std::string_view message);
Bytes hkdfSha256(std::string_view ikm, std::string_view salt, std::string_view info, std::size_t length);
Bytes randomBytes(std::size_t size);

// X25519(scalar, u-coordinate), both 32 bytes; the shared secret or, with
// the base point 9, the public key. The scalar is clamped as RFC 7748 says.
Bytes x25519(std::string_view scalar, std::string_view point);
Bytes x25519Public(std::string_view scalar);

// Same size and same bytes, in time that does not depend on where they differ.
bool sameBytes(std::string_view a, std::string_view b) noexcept;

// SHA-256 of what comes in pieces (a file being sent or received).
class Sha256Stream {
public:
    Sha256Stream();
    ~Sha256Stream();
    Sha256Stream(const Sha256Stream&) = delete;
    Sha256Stream& operator=(const Sha256Stream&) = delete;
    void add(std::string_view data);
    Bytes finish(); // once
private:
    void* m_hash = nullptr; // BCRYPT_HASH_HANDLE
};

// AES-256-GCM with one key; a 12-byte nonce and a 16-byte tag after the ciphertext.
class AesGcm {
public:
    explicit AesGcm(std::string_view key);
    ~AesGcm();
    AesGcm(const AesGcm&) = delete;
    AesGcm& operator=(const AesGcm&) = delete;
    bool valid() const noexcept { return m_key != nullptr; }
    Bytes seal(std::string_view nonce, std::string_view aad, std::string_view plaintext) const; // ciphertext || tag
    std::optional<Bytes> open(std::string_view nonce, std::string_view aad, std::string_view sealed) const; // nothing: forged or damaged
private:
    void* m_key = nullptr; // BCRYPT_KEY_HANDLE
};

std::string hexLower(std::string_view bytes);
std::optional<Bytes> fromHex(std::string_view text);
std::string base64(std::string_view bytes); // standard alphabet, padded
std::string base64UrlNoPad(std::string_view bytes);
// Standard or URL-safe, with or without padding, spaces round it ignored.
std::optional<Bytes> fromBase64(std::string_view text);

std::string newUuid(); // random (version 4), lower case
std::optional<Bytes> uuidBytes(std::string_view uuid); // its 16 bytes, in the order written
std::string uuidText(std::string_view bytes); // 16 bytes -> "xxxxxxxx-xxxx-…", lower case

// Protects a secret for this Windows user (DPAPI), and back.
std::optional<Bytes> protectForUser(std::string_view secret);
std::optional<Bytes> unprotectForUser(std::string_view blob);

} // namespace shunsync
