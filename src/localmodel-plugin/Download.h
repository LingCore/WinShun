// Downloads one big file (a recommended model) over HTTPS with WinHTTP and
// the system's proxy settings: into `<target>.part` in pieces over a few
// connections at once (ModelScope's CDN slows each connection down, not
// all of them), `<target>.part.map` saying which pieces are in, so a later
// try goes on with the rest; then checked by size and SHA-256 before it is
// renamed to `target`. A file that fails the check is deleted. A server
// without ranges gets one stream from the start.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace lm {

struct DownloadResult {
    bool ok = false;
    bool canceled = false; // stopped on request: the .part is kept to go on later
    std::string error; // a sentence for the user when neither
};

// `progress(done, total, checking)`: about four times a second while bytes
// come, then with `checking` while the SHA-256 is read.
using DownloadProgress = std::function<void(std::uint64_t done, std::uint64_t total, bool checking)>;

struct DownloadTuning {
    std::uint64_t pieceBytes = 8ull << 20; // what one request asks for (and what a broken one loses)
    int connections = 4;
};

DownloadResult download(const std::string& url, const std::filesystem::path& target, std::uint64_t size,
    std::string_view sha256, const std::atomic_bool& cancel, const DownloadProgress& progress, bool chinese,
    const DownloadTuning& tuning = {});

// The SHA-256 of a file, lower-case hex; empty when it cannot be read or
// `cancel` was set.
std::string sha256Of(const std::filesystem::path& file, const std::atomic_bool& cancel,
    const std::function<void(std::uint64_t done)>& progress = {});

} // namespace lm
