#pragma once

// LocalSend's protocol v2 as plain values (localsend/protocol, v2.2; what
// LocalSend 1.18 sends and wants): the device a peer says it is, the files
// of a prepare-upload, and their JSON. Values from the network are checked
// here once: lengths, counts, ids.

#include "winshun.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace localsend {

inline constexpr int kPort = 53317; // LocalSend's, for HTTPS and the multicast group alike
inline constexpr int kLastPort = 53320; // taken (LocalSend itself runs here too): the next ones, up to this
inline constexpr char kMulticastGroup[] = "224.0.0.167";
inline constexpr char kProtocolVersion[] = "2.2";
inline constexpr std::size_t kMaxFiles = 10'000; // in one prepare-upload
inline constexpr std::size_t kMaxNameBytes = 4096; // a file name as sent (with its folders)
inline constexpr std::size_t kMaxTextBytes = 64 * 1024; // a text message's preview
inline constexpr std::size_t kMaxFingerprintBytes = 128;
// JSON values a peer's text may make: "[0,0,0,…]" of a megabyte would be
// half a million parsed. A file of LocalSend 1.18's prepare-upload makes 10
// (its fields, its dates' object), of its answer 1 (a token).
inline constexpr std::size_t kMaxValues = 12 * kMaxFiles + 100; // a prepare-upload, its answer
inline constexpr std::size_t kMaxDeviceValues = 1'000; // a device's info, register, announcement

struct DeviceInfo {
    std::string alias;
    std::string version = kProtocolVersion;
    std::string deviceModel; // "Windows", "Pixel 8", …; may be empty
    std::string deviceType = "desktop"; // mobile | desktop | web | headless | server
    std::string fingerprint; // HTTPS: the certificate's SHA-256, upper-case hex
    int port = kPort;
    std::string protocol = "https"; // or "http"
    bool download = false; // the web download API (not offered)
    bool https() const { return protocol != "http"; }
};

// `withPort`: register and multicast carry port and protocol; info does not.
winshun::Json toJson(const DeviceInfo& info, bool withPort);
// A peer's: alias and fingerprint wanted, the rest defaulted; texts made
// UTF-8 and cut on a character. A fingerprint is letters, digits, '-' and
// '_' (SHA-256 hex for HTTPS, a random string for HTTP), at most
// kMaxFingerprintBytes: any other is no device.
std::optional<DeviceInfo> deviceFromJson(const winshun::Json& json);

// Few enough values in a JSON text to parse it (counted on the commas and
// brackets outside strings, without parsing).
bool fewValues(std::string_view json, std::size_t maxValues);
// A peer's JSON text; nothing when it is not JSON or makes too many values.
std::optional<winshun::Json> parsePeerJson(const std::string& text, std::size_t maxValues);

struct FileMeta {
    std::string id;
    std::string fileName; // as sent: may have folders ("相册/IMG_1.jpg"), never trusted as a path
    std::uint64_t size = 0;
    std::string fileType; // a MIME type ("image/jpeg", "text/plain")
    std::string sha256; // may be empty
    std::string preview; // a text message's words
};

struct PrepareUpload {
    DeviceInfo info;
    std::vector<FileMeta> files;
    std::uint64_t totalBytes() const;
};

// A prepare-upload body. Nothing, `why` saying it, when it is not one we
// can take: no files, more than kMaxFiles, ids repeated, no name.
std::optional<PrepareUpload> parsePrepareUpload(const winshun::Json& json, std::string* why);
winshun::Json toJson(const PrepareUpload& request);

// One text file with its words in the preview: shown, never written
// anywhere (LocalSend answers it with 204).
bool isTextMessage(const PrepareUpload& request);

// The device types LocalSend knows; anything else is shown as a desktop.
std::string normalizedDeviceType(std::string_view type);

} // namespace localsend
