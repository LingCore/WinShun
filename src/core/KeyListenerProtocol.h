#pragma once

#include <windows.h>

#include <cstdint>

namespace ws::keyproto {

// Only notifications, to the requesting process's KeyListener window.
// No key injection, window activation or arbitrary commands in this channel.
constexpr wchar_t kWindowClass[] = L"WinShun.KeyListener";
constexpr UINT kReady = WM_APP + 71;
constexpr UINT kTap = WM_APP + 72;
constexpr DWORD kHeartbeatMs = 1000;
constexpr DWORD kLeaseMs = 3000;

// Ready's lParam: which taps the listener tells of besides Ctrl's. One from
// before double Alt sends 0: the GUI's own Raw Input then tells of Alt's.
constexpr LPARAM kReportsAlt = 1;
// Likewise for double Shift: an older one tells of Ctrl and Alt only.
constexpr LPARAM kReportsShift = 2;

// WinShun is x64. The sender PID is in the upper half; the lower half is
// VK_LCONTROL / VK_RCONTROL / VK_LMENU / VK_RMENU / VK_LSHIFT / VK_RSHIFT, plus whether any tap
// came from SendInput.
constexpr WPARAM tapWord(DWORD pid, DWORD key, bool sent) noexcept
{
    return (static_cast<WPARAM>(pid) << 32) | key | (sent ? 0x100 : 0);
}

// kTap's lParam: the second release's time on the input's clock in the lower
// half, as every version sends it; in the upper half how long before it the
// first press was, where the double tap began (seenBy, GitHub #111). A
// listener from before that sends 0 there: not told. A GUI from before reads
// the lower half alone.
constexpr LPARAM tapTimes(std::uint32_t time, std::uint32_t span) noexcept
{
    return static_cast<LPARAM>((static_cast<std::uint64_t>(span) << 32) | time);
}
constexpr std::uint32_t tapTime(LPARAM times) noexcept
{
    return static_cast<std::uint32_t>(times);
}
constexpr std::uint32_t tapSpan(LPARAM times) noexcept
{
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(times) >> 32);
}

} // namespace ws::keyproto
