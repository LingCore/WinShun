#pragma once

namespace ws {

// Written atomically with the pixels, including when the history is off.
// An opaque per-capture identity, never a file path or an authorization token.
inline constexpr auto kImageOriginFormat = L"WinShun.ImageOrigin";
inline constexpr auto kImageOriginMime = "application/x-qt-windows-mime;value=\"WinShun.ImageOrigin\"";

} // namespace ws
