#pragma once

#include <windows.h>

namespace ws::win {

namespace detail {

constexpr UINT kZOrderOnly = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;

inline bool isTopmost(HWND hwnd)
{
    return ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST;
}

inline void setOwner(HWND hwnd, HWND owner)
{
    ::SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
}

// Into the topmost band or out of it. Demoting an owned window also demotes
// its owners: detached for that, then owned again.
inline void setTopmost(HWND hwnd, bool topmost, UINT flags)
{
    if (isTopmost(hwnd) == topmost)
        return;
    const HWND owner = topmost ? nullptr : ::GetWindow(hwnd, GW_OWNER);
    if (owner)
        setOwner(hwnd, nullptr);
    ::SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    if (owner)
        setOwner(hwnd, owner);
}

// Right above `target`. No other window moves, but the ones `hwnd` owns
// unless `flags` has SWP_NOOWNERZORDER.
inline void stackAbove(HWND hwnd, HWND target, UINT flags)
{
    const HWND above = ::GetWindow(target, GW_HWNDPREV);
    if (above == hwnd)
        return;
    // First in its band: the top of that band is right above it.
    const HWND after = above && isTopmost(above) == isTopmost(hwnd) ? above : HWND_TOP;
    if (!::SetWindowPos(hwnd, after, 0, 0, 0, 0, flags) && after != HWND_TOP)
        ::SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, flags); // that window just went away
}

} // namespace detail

// The bar by another program's dialog: right above it, topmost only when the
// dialog is, and never owned by it. An owner on another thread joins the two
// threads' input: while that program hung, the bar took 2.9 s to come to the
// front, unowned 30 ms (#95). Windows does not keep it above the dialog then:
// DialogBar does this again whenever the dialog comes to the front, shows,
// moves or is let go of. Without SWP_NOOWNERZORDER the bar's own logo and
// menus come along, and with no owner nothing else moves. An elevated
// dialog: topmost, as before #95, the bar showing only while it is in front;
// its program can raise windows over it that we may not hear of across UIPI.
inline void followDialog(HWND companion, HWND dialog, bool elevated)
{
    using namespace detail;
    if (!::IsWindow(companion))
        return;
    if (!::IsWindow(dialog))
        dialog = nullptr;
    if (::GetWindow(companion, GW_OWNER))
        setOwner(companion, nullptr); // first: a demotion would take an owner along
    setTopmost(companion, dialog && (elevated || isTopmost(dialog)), kZOrderOnly);
    if (dialog && !elevated)
        stackAbove(companion, dialog, kZOrderOnly);
}

// The logo and the menus by the bar: owned by it, on the same thread (so no
// input is joined), right above it, topmost only when it is. The bar stays
// where followDialog put it (SWP_NOOWNERZORDER).
inline void followOwner(HWND companion, HWND owner)
{
    using namespace detail;
    if (!::IsWindow(companion))
        return;
    if (!::IsWindow(owner))
        owner = nullptr;
    if (::GetWindow(companion, GW_OWNER) != owner)
        setOwner(companion, owner);
    constexpr UINT flags = kZOrderOnly | SWP_NOOWNERZORDER;
    setTopmost(companion, owner && isTopmost(owner), flags);
    if (owner)
        stackAbove(companion, owner, flags);
}

} // namespace ws::win
