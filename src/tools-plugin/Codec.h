#pragma once

// The small encoders of winshun.tools: Base64, hashes (MD5, SHA-1, SHA-256
// through CNG), UUIDs, and timestamps to and from dates. No Qt; Win32 and
// bcrypt only.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tools::codec {

std::string base64Encode(const std::string& bytes);
// The bytes, when `text` is Base64 (standard or URL-safe alphabet, padding
// optional); nullopt otherwise.
std::optional<std::string> base64Decode(const std::string& text);
bool validUtf8(const std::string& bytes);

// Lower-case hex of the digest; empty when the algorithm is unknown or
// fails. `algorithm`: "md5", "sha1", "sha256", "sha384", "sha512".
std::string hashHex(const std::string& algorithm, const std::string& bytes);
// The digest of a file (read whole; `maxBytes` and up refuses, with empty
// and `tooLarge` set); empty when it cannot be read.
std::string hashFileHex(const std::string& algorithm, const std::wstring& path, std::uint64_t maxBytes, bool* tooLarge);

std::string uuid4(); // "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx", lower case

// Times. Epoch seconds may carry a fraction.
struct Moment {
    std::int64_t seconds = 0; // since 1970-01-01T00:00:00Z
    int milliseconds = 0; // 0–999
};
Moment now();
// "1700000000", "1700000000000" (milliseconds), "1700000000000000"
// (microseconds), "1700000000000000000" (nanoseconds), "1700000000.5";
// nullopt when not a timestamp.
std::optional<Moment> parseTimestamp(const std::string& text);
// "2024-01-02", "2024-01-02 15:04", "2024-01-02T15:04:05", with "Z" or
// "+08:00" for a zone (local time otherwise); also "2024/1/2 15:04:05".
std::optional<Moment> parseDate(const std::string& text);
std::string formatLocal(const Moment& m, bool withMs = false); // "2024-01-02 15:04:05"
std::string formatUtc(const Moment& m, bool withMs = false); // "2024-01-02 15:04:05"
std::string formatIsoLocal(const Moment& m); // "2024-01-02T15:04:05+08:00"
std::string formatIsoUtc(const Moment& m); // "2024-01-02T15:04:05Z"
std::string localZoneName(); // "UTC+08:00"
int localOffsetMinutes(const Moment& m);

} // namespace tools::codec
