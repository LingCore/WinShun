#pragma once

// ShunSync v1 (docs/shunsync-protocol.md, shared with Mac顺): what both ends
// must compute alike, byte for byte (docs/shunsync-test-vectors.json is the
// check, tests/tst_shunsync.cpp runs it): the group's public id, the invite
// code, the handshake's transcript, proofs and keys, the six-digit pairing
// code, and the frames sealed with AES-256-GCM under a sequence number.

#include "Crypto.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace shunsync {

inline constexpr int kProtocol = 1;
inline constexpr std::uint16_t kTcpPort = 37219;
inline constexpr std::uint16_t kUdpPort = 37220;
inline constexpr std::size_t kMaxPlainFrame = 20u * 1024 * 1024;
inline constexpr std::size_t kMaxSealedFrame = kMaxPlainFrame + 16;
inline constexpr std::size_t kChunkBytes = 256u * 1024;
inline constexpr std::size_t kMaxImageBytes = 15u * 1024 * 1024;
inline constexpr std::size_t kKeyBytes = 32;
inline constexpr std::size_t kNonceBytes = 16; // the handshake's nonces

// The first byte of a sealed frame's plaintext.
enum class FrameKind : unsigned char { Json = 0x01, FileChunk = 0x02, Image = 0x03 };

// hexLower(SHA256("shunsync:gid:" || groupKey)[0..15])
std::string groupIdOf(std::string_view groupKey);

// shunsync://join?k=<base64url, no padding>[&p=<host:port, percent-encoded>]
std::string inviteCode(std::string_view groupKey, std::string_view hostPort = {});
struct Invite {
    Bytes groupKey;
    std::string hostPort; // "" when the code has none
};
// The code, or a bare 32-byte key in base64 or base64url; spaces round it ignored.
std::optional<Invite> parseInvite(std::string_view text);

// SHA256("shunsync:v1:" || mode || clientNonce || serverNonce || clientEphPub
// || serverEphPub || clientDeviceId || 0x00 || serverDeviceId)
Bytes transcriptOf(std::string_view mode, std::string_view clientNonce, std::string_view serverNonce,
    std::string_view clientEphPub, std::string_view serverEphPub, std::string_view clientDeviceId,
    std::string_view serverDeviceId);

struct SyncKeys {
    Bytes serverProof;
    Bytes clientProof;
    Bytes c2s;
    Bytes s2c;
};
// mode "sync" and "file": both proofs, and the keys from the shared secret and the group key.
SyncKeys syncKeys(std::string_view sharedSecret, std::string_view groupKey, std::string_view transcript);

struct PairKeys {
    std::string sas; // six digits, zeros in front kept
    Bytes c2s;
    Bytes s2c;
};
// mode "pair": no group key yet; the code both screens show.
PairKeys pairKeys(std::string_view sharedSecret, std::string_view transcript);

Bytes nonceOf(std::uint64_t seq); // 0x00000000 || BE64(seq)
Bytes aadOf(std::uint64_t seq); // BE64(seq)

// One direction of a connection: each frame sealed under the next sequence
// number, from 0; the other end opens them in the same order (a frame
// replayed, dropped or reordered does not open).
class FrameSealer {
public:
    explicit FrameSealer(std::string_view key) : m_aes(key) {}
    bool valid() const noexcept { return m_aes.valid(); }
    Bytes seal(std::string_view plaintext)
    {
        const std::uint64_t seq = m_seq++; // not in the call: arguments are evaluated in any order
        return m_aes.seal(nonceOf(seq), aadOf(seq), plaintext);
    }
    std::uint64_t next() const noexcept { return m_seq; }
private:
    AesGcm m_aes;
    std::uint64_t m_seq = 0;
};

class FrameOpener {
public:
    explicit FrameOpener(std::string_view key) : m_aes(key) {}
    bool valid() const noexcept { return m_aes.valid(); }
    std::optional<Bytes> open(std::string_view sealed)
    {
        std::optional<Bytes> plain = m_aes.open(nonceOf(m_seq), aadOf(m_seq), sealed);
        if (plain)
            ++m_seq;
        return plain;
    }
private:
    AesGcm m_aes;
    std::uint64_t m_seq = 0;
};

// Big-endian numbers in frames.
void putBe32(Bytes& out, std::uint32_t v);
void putBe64(Bytes& out, std::uint64_t v);
std::uint32_t be32(std::string_view at) noexcept; // the first 4 bytes
std::uint64_t be64(std::string_view at) noexcept; // the first 8 bytes

// A file's place in a clip_files list: POSIX '/', never a leading '/' or
// '\', a drive, a '..' or an empty part.
bool validRelativePath(std::string_view path) noexcept;

// "host:port" split; IPv6 in brackets. Nothing when the port is not 1-65535.
struct HostPort {
    std::string host;
    std::uint16_t port = 0;
};
std::optional<HostPort> parseHostPort(std::string_view text, std::uint16_t defaultPort = 0);

} // namespace shunsync
