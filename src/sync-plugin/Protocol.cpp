#include "Protocol.h"

#include <charconv>
#include <cstdio>

namespace shunsync {

namespace {

bool unreservedInHostPort(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'
        || c == '~' || c == ':' || c == '[' || c == ']';
}

std::string percentEncode(std::string_view text)
{
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        if (unreservedInHostPort(c)) {
            out += c;
        } else {
            out += '%';
            out += digits[(static_cast<unsigned char>(c) >> 4) & 15];
            out += digits[static_cast<unsigned char>(c) & 15];
        }
    }
    return out;
}

std::string percentDecode(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            if (const auto byte = fromHex(text.substr(i + 1, 2))) {
                out += *byte;
                i += 2;
                continue;
            }
        }
        out += text[i] == '+' ? ' ' : text[i];
    }
    return out;
}

std::string_view trimmed(std::string_view s) noexcept
{
    while (!s.empty() && static_cast<unsigned char>(s.front()) <= ' ')
        s.remove_prefix(1);
    while (!s.empty() && static_cast<unsigned char>(s.back()) <= ' ')
        s.remove_suffix(1);
    return s;
}

} // namespace

std::string groupIdOf(std::string_view groupKey)
{
    Bytes input = "shunsync:gid:";
    input.append(groupKey);
    return hexLower(sha256(input).substr(0, 16));
}

std::string inviteCode(std::string_view groupKey, std::string_view hostPort)
{
    std::string code = "shunsync://join?k=" + base64UrlNoPad(groupKey);
    if (!hostPort.empty())
        code += "&p=" + percentEncode(hostPort);
    return code;
}

std::optional<Invite> parseInvite(std::string_view text)
{
    text = trimmed(text);
    constexpr std::string_view scheme = "shunsync://join?";
    Invite invite;
    if (text.size() > scheme.size() && std::equal(scheme.begin(), scheme.end(), text.begin(), [](char a, char b) {
            return a == (b >= 'A' && b <= 'Z' ? static_cast<char>(b - 'A' + 'a') : b);
        })) {
        std::string_view query = text.substr(scheme.size());
        if (const std::size_t hash = query.find('#'); hash != std::string_view::npos)
            query = query.substr(0, hash);
        while (!query.empty()) {
            const std::size_t amp = query.find('&');
            const std::string_view pair = query.substr(0, amp);
            query = amp == std::string_view::npos ? std::string_view() : query.substr(amp + 1);
            const std::size_t eq = pair.find('=');
            if (eq == std::string_view::npos)
                continue;
            const std::string_view name = pair.substr(0, eq);
            const std::string value = percentDecode(pair.substr(eq + 1));
            if (name == "k") {
                if (auto key = fromBase64(value))
                    invite.groupKey = std::move(*key);
            } else if (name == "p") {
                invite.hostPort = std::string(trimmed(value));
            }
        }
    } else if (auto key = fromBase64(text)) {
        invite.groupKey = std::move(*key);
    }
    if (invite.groupKey.size() != kKeyBytes)
        return std::nullopt;
    if (!invite.hostPort.empty() && !parseHostPort(invite.hostPort, kTcpPort))
        invite.hostPort.clear(); // the key is good; a place that is not one is left out
    return invite;
}

Bytes transcriptOf(std::string_view mode, std::string_view clientNonce, std::string_view serverNonce,
    std::string_view clientEphPub, std::string_view serverEphPub, std::string_view clientDeviceId,
    std::string_view serverDeviceId)
{
    Bytes input = "shunsync:v1:";
    input.append(mode);
    input.append(clientNonce);
    input.append(serverNonce);
    input.append(clientEphPub);
    input.append(serverEphPub);
    input.append(clientDeviceId);
    input += '\0';
    input.append(serverDeviceId);
    return sha256(input);
}

SyncKeys syncKeys(std::string_view sharedSecret, std::string_view groupKey, std::string_view transcript)
{
    SyncKeys keys;
    keys.serverProof = hmacSha256(groupKey, Bytes("server:").append(transcript));
    keys.clientProof = hmacSha256(groupKey, Bytes("client:").append(transcript));
    const Bytes ikm = Bytes(sharedSecret).append(groupKey);
    keys.c2s = hkdfSha256(ikm, transcript, "shunsync:v1:c2s", 32);
    keys.s2c = hkdfSha256(ikm, transcript, "shunsync:v1:s2c", 32);
    return keys;
}

PairKeys pairKeys(std::string_view sharedSecret, std::string_view transcript)
{
    PairKeys keys;
    const std::uint32_t raw = be32(transcript);
    char digits[8] {};
    std::snprintf(digits, sizeof digits, "%06u", static_cast<unsigned>(raw % 1000000u));
    keys.sas = digits;
    keys.c2s = hkdfSha256(sharedSecret, transcript, "shunsync:v1:pair-c2s", 32);
    keys.s2c = hkdfSha256(sharedSecret, transcript, "shunsync:v1:pair-s2c", 32);
    return keys;
}

void putBe32(Bytes& out, std::uint32_t v)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        out += static_cast<char>((v >> shift) & 0xFF);
}

void putBe64(Bytes& out, std::uint64_t v)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        out += static_cast<char>((v >> shift) & 0xFF);
}

std::uint32_t be32(std::string_view at) noexcept
{
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4 && i < at.size(); ++i)
        v = (v << 8) | static_cast<unsigned char>(at[i]);
    return v;
}

std::uint64_t be64(std::string_view at) noexcept
{
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8 && i < at.size(); ++i)
        v = (v << 8) | static_cast<unsigned char>(at[i]);
    return v;
}

Bytes nonceOf(std::uint64_t seq)
{
    Bytes out(4, '\0');
    putBe64(out, seq);
    return out;
}

Bytes aadOf(std::uint64_t seq)
{
    Bytes out;
    putBe64(out, seq);
    return out;
}

bool validRelativePath(std::string_view path) noexcept
{
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos
        || path.find('\0') != std::string_view::npos)
        return false;
    if (path.size() >= 2 && path[1] == ':')
        return false; // "C:…"
    while (!path.empty()) {
        const std::size_t slash = path.find('/');
        const std::string_view part = path.substr(0, slash);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (slash == std::string_view::npos)
            break;
        path.remove_prefix(slash + 1);
        if (path.empty())
            return false; // a trailing '/'
    }
    return true;
}

std::optional<HostPort> parseHostPort(std::string_view text, std::uint16_t defaultPort)
{
    text = trimmed(text);
    HostPort out;
    std::string_view portText;
    if (!text.empty() && text.front() == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos)
            return std::nullopt;
        out.host = std::string(text.substr(1, close - 1));
        const std::string_view rest = text.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':')
                return std::nullopt;
            portText = rest.substr(1);
        }
    } else if (const std::size_t colon = text.rfind(':'); colon != std::string_view::npos && text.find(':') == colon) {
        out.host = std::string(text.substr(0, colon));
        portText = text.substr(colon + 1);
    } else {
        out.host = std::string(text); // a bare name or an IPv6 address without a port
    }
    if (out.host.empty())
        return std::nullopt;
    for (const char c : out.host) {
        if (static_cast<unsigned char>(c) <= ' ' || c == '/' || c == '?' || c == '#' || c == '@')
            return std::nullopt;
    }
    if (portText.empty()) {
        if (!defaultPort)
            return std::nullopt;
        out.port = defaultPort;
        return out;
    }
    unsigned port = 0;
    const auto [end, error] = std::from_chars(portText.data(), portText.data() + portText.size(), port);
    if (error != std::errc() || end != portText.data() + portText.size() || port == 0 || port > 65535)
        return std::nullopt;
    out.port = static_cast<std::uint16_t>(port);
    return out;
}

} // namespace shunsync
