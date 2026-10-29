#include "KeyListener.h"
#include "KeyListenerProtocol.h"
#include "OwnInput.h"
#include "SearchTyping.h" // searchtyping::RawInputAside

#include <shellscalingapi.h>

#include <algorithm>
#include <format>
#include <latch>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ws {

namespace {

constexpr const wchar_t* kClassName = keyproto::kWindowClass;
constexpr USHORT kGenericDesktop = 0x01; // HID usage page
constexpr USHORT kMouse = 0x02; // HID usage
constexpr USHORT kKeyboard = 0x06;
constexpr USHORT kAnyButtonDown = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_DOWN
    | RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN;
// Ctrl+wheel zooms. A touchpad pinch is sent as Ctrl+wheel too, by Windows.
constexpr USHORT kWheel = RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL;
constexpr UINT_PTR kMouseOffTimer = 1;
// The key under Esc, left of 1, by where it is: ` and ~ on US and Chinese
// layouts (· in a Chinese input method), ^ on German ones, ² on French ones.
constexpr USHORT kDragKeyScan = 0x29;
// Its hotkeys while a drag is under way: alone, with Shift, Ctrl, both.
constexpr int kDragKeyHotkey = 0x5750;
constexpr std::array<UINT, 4> kDragKeyMods {0, MOD_SHIFT, MOD_CONTROL, MOD_CONTROL | MOD_SHIFT};
constexpr std::uint32_t kDragLookMs = 30;
// The key pressed on a file before it was dragged: the drag started for it,
// looked for every 20 ms, for up to 300.
constexpr UINT_PTR kStartDragTimer = 2;
constexpr UINT kStartDragTickMs = 20;
constexpr int kStartDragTicks = 15;
// While the service's UIAccess listener tells of the taps, this process's
// own word of one stands in for that listener's after this long. Both see
// the same events, with the same times; its word comes within a
// millisecond or so of this one's, unless it is late.
constexpr UINT_PTR kBackupTimer = 3;
constexpr UINT kBackupMs = 120;
// GetLastInputInfo's time for the tap's own last release: the same tick, or the next.
constexpr std::int32_t kSameInputMs = 32;

thread_local KeyListener* t_listener = nullptr; // on the listener thread, for its WinEvent callback

// The windows that list files, where a press may become a drag of them:
// File Explorer's view and the file dialogs (DirectUIHWND), the desktop and
// older lists (SysListView32), folder trees, Total Commander's panels (64
// and 32 bit). Pressed elsewhere, the key is left alone until a drag begins.
bool listsFiles(HWND window)
{
    wchar_t name[32] {};
    if (!window || !::GetClassNameW(window, name, 32))
        return false;
    const std::wstring_view cls(name);
    return cls == L"DirectUIHWND" || cls == L"SysListView32" || cls == L"SysTreeView32" || cls == L"LCLListBox"
        || cls == L"TMyListBox";
}

using Key = DoubleTapDetector::Key;

// The key pressed while dragging: Ctrl switches between moving and copying,
// Alt drags a column selection or (AltSnap) the window.
bool mouseButtonHeld()
{
    for (const int button : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2}) {
        if (::GetAsyncKeyState(button) < 0)
            return true;
    }
    return false;
}

// As long as a double click may take (Control Panel → Mouse), so a slow
// setting there is honoured; within limits, so two separate presses do not
// make one.
std::uint32_t maxGapMs()
{
    return std::clamp<std::uint32_t>(::GetDoubleClickTime(), 400, 900);
}

bool held(int vk)
{
    return ::GetAsyncKeyState(vk) < 0;
}

// Alt+double click is File Explorer's Properties, Ctrl and Shift select.
bool modifierHeld()
{
    return held(VK_SHIFT) || held(VK_CONTROL) || held(VK_MENU) || held(VK_LWIN) || held(VK_RWIN);
}

bool isDown(std::uint32_t vk)
{
    return held(static_cast<int>(vk));
}

DoubleTapDetector detectorFor(Key key)
{
    return DoubleTapDetector({.key = key, .isDown = &isDown});
}

bool takes(KeyListener::Keys keys, Key key)
{
    switch (keys) {
    case KeyListener::Keys::All:
        return true;
    case KeyListener::Keys::Ctrl:
        return key == Key::Ctrl;
    case KeyListener::Keys::Alt:
        return key == Key::Alt;
    case KeyListener::Keys::Shift:
        return key == Key::Shift;
    }
    return false;
}

bool keyboardRegistered(HWND window)
{
    // ShortcutCapture temporarily removes this registration while recording
    // a hotkey, TypeToSearch while it holds keys for the launcher
    // (KeyboardRawInputAside). A hotkey's Ctrl presses must not wake the
    // launcher via the helper.
    const KeyboardRawInput keyboard = keyboardRawInput();
    return keyboard.registration && keyboard.registration->hwndTarget == window;
}

// KeyboardRawInputAside's, shared by its holders on their threads and
// KeyListener's own registering and removing.
std::mutex g_asideMutex;
searchtyping::RawInputAside g_aside; // guarded by g_asideMutex

using Registration = searchtyping::RawInputAside::Registration;

// Raw Input names Ctrl, Shift and Alt without their side: the right Ctrl and
// Alt come with the E0 prefix, the right Shift with its own scan code.
std::uint32_t sidedKey(const RAWKEYBOARD& key)
{
    const bool e0 = key.Flags & RI_KEY_E0;
    switch (key.VKey) {
    case VK_CONTROL:
        return e0 ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        return e0 ? VK_RMENU : VK_LMENU;
    case VK_SHIFT:
        return key.MakeCode == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    default:
        return key.VKey;
    }
}

constexpr const char* kRecentNames[] {"", "LCtrl", "RCtrl", "LShift", "RShift", "LAlt", "RAlt", "LWin", "RWin", "key"};

std::uint8_t recentKey(std::uint32_t vk)
{
    switch (vk) {
    case VK_LCONTROL:
        return 1;
    case VK_RCONTROL:
        return 2;
    case VK_LSHIFT:
        return 3;
    case VK_RSHIFT:
        return 4;
    case VK_LMENU:
        return 5;
    case VK_RMENU:
        return 6;
    case VK_LWIN:
        return 7;
    case VK_RWIN:
        return 8;
    default:
        return 9;
    }
}

// WINSHUN_KEYLOG=1: every key event in the log (Callbacks::log).
bool keyLogWanted()
{
    wchar_t value[4] {};
    return ::GetEnvironmentVariableW(L"WINSHUN_KEYLOG", value, 4) == 1 && value[0] == L'1';
}

const char* keysName(KeyListener::Keys keys)
{
    switch (keys) {
    case KeyListener::Keys::Ctrl:
        return "Ctrl";
    case KeyListener::Keys::Alt:
        return "Alt";
    case KeyListener::Keys::Shift:
        return "Shift";
    case KeyListener::Keys::All:
        break;
    }
    return "Ctrl, Alt and Shift";
}

const char* sideName(DoubleTapDetector::Side side)
{
    return side == DoubleTapDetector::Side::Left ? "left" : side == DoubleTapDetector::Side::Right ? "right" : "both";
}

const char* yesNo(bool yes)
{
    return yes ? "yes" : "no";
}

// For the key log: a modifier (or KeyRouter's E8) with what Raw Input said
// of it, any other key only as "key" (nothing typed is kept); when, and from
// where.
std::string describeKey(const RAWKEYBOARD& key, std::uint32_t vk, bool up, bool device, bool sent, std::uint32_t time)
{
    const std::uint8_t name = recentKey(vk);
    std::string text = std::format("Key log: {}{}", kRecentNames[name], up ? "↑" : "↓");
    if (name != 9 || key.VKey == 0xE8)
        text += std::format(" (vk {:#x}, scan {:#x}, flags {:#x})", key.VKey, key.MakeCode, key.Flags);
    return text
        + std::format(" at {}, {}, extra {:#x}", time,
            device  ? "from a keyboard"
            : sent  ? "sent by a program"
                    : "sent by Win顺",
            key.ExtraInformation);
}

} // namespace

KeyListener::KeyListener(Callbacks callbacks)
    : m_callbacks(std::move(callbacks))
    , m_detectors {detectorFor(Key::Ctrl), detectorFor(Key::Alt), detectorFor(Key::Shift)}
    , m_keyLog(m_callbacks.log && keyLogWanted())
{
    std::latch ready(1);
    m_thread = std::jthread([this, &ready] {
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW wc {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = &KeyListener::wndProc;
        wc.hInstance = instance;
        wc.lpszClassName = kClassName;
        ::RegisterClassExW(&wc);
        // Message-only: never shown, and still the target of raw input.
        const HWND hwnd = ::CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
        m_window = hwnd;
        // Every keyboard, also while other programs have the focus; the mouse
        // only around a press of the key (listenToMouse), or all the time for
        // double clicks.
        bool active = hwnd != nullptr;
        if (hwnd && keyboardWanted())
            active = registerKeyboard(hwnd);
        if (hwnd && mouseWanted()) {
            listenToMouse(true);
            active = active && m_mouseOn;
        }
        // Which window is in front, as it changes: whether a tap told late
        // was made over the one in front now (quietSince).
        if (hwnd && m_callbacks.doubleCtrl) {
            t_listener = this;
            m_foregroundAt = static_cast<std::uint32_t>(::GetTickCount());
            m_foregroundHook = ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                &KeyListener::onForeground, 0, 0, WINEVENT_OUTOFCONTEXT);
        }
        m_active = active;
        ready.count_down();
        if (!hwnd)
            return;
        // Above normal: the second tap is timed from the message's own clock,
        // but it should not wait behind other work for long either.
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
            ::DispatchMessageW(&msg);
    });
    ready.wait();
}

KeyListener::~KeyListener()
{
    if (const HWND hwnd = m_window.load())
        ::PostMessageW(hwnd, WM_CLOSE, 0, 0); // the window ends the thread's message loop
    if (m_thread.joinable())
        m_thread.join();
}

LRESULT CALLBACK KeyListener::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<KeyListener*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case keyproto::kReady:
        if (self && wParam != 0 && wParam == self->m_servicePid.load()) {
            const LPARAM reports = lParam & (keyproto::kReportsAlt | keyproto::kReportsShift);
            if (self->m_keyLog && (self->m_readyPid.load() != wParam || self->m_serviceReports.load() != reports))
                self->log(std::format("Key log: UIAccess listener {} ready, it tells of Ctrl{}{}", wParam,
                    (reports & keyproto::kReportsAlt) ? ", Alt" : "", (reports & keyproto::kReportsShift) ? ", Shift" : ""));
            self->m_readyPid = static_cast<DWORD>(wParam);
            self->m_serviceReports = reports;
            self->m_heartbeat = ::GetTickCount64();
            self->followService();
        }
        return 0;
    case keyproto::kTap:
        if (self && static_cast<DWORD>(wParam >> 32) == self->m_servicePid.load())
            self->onServiceTap(static_cast<std::uint32_t>(wParam & 0xFF), keyproto::tapTime(lParam),
                keyproto::tapSpan(lParam), (wParam & 0x100) != 0);
        return 0;
    case WM_INPUT:
        if (self)
            self->onInput(reinterpret_cast<HRAWINPUT>(lParam));
        return ::DefWindowProcW(hwnd, msg, wParam, lParam); // lets Windows free the input
    case WM_TIMER:
        if (wParam == kBackupTimer && self)
            self->onBackupTimer();
        if (wParam == kStartDragTimer && self)
            self->onStartDragTimer();
        if (wParam == kMouseOffTimer && self) {
            ::KillTimer(hwnd, kMouseOffTimer);
            if (self->keyHeld()) // still down: wait for its release
                ::SetTimer(hwnd, kMouseOffTimer, maxGapMs(), nullptr);
            else
                self->listenToMouse(false);
        }
        return 0;
    case WM_HOTKEY: // the drop pie's key during a drag; once a press (MOD_NOREPEAT)
        if (self && wParam >= WPARAM {kDragKeyHotkey} && wParam < kDragKeyHotkey + kDragKeyMods.size())
            self->onDragKeyPressed();
        return 0;
    case WM_CLOSE: {
        if (self && self->m_foregroundHook) {
            ::UnhookWinEvent(self->m_foregroundHook);
            self->m_foregroundHook = nullptr;
        }
        if (self)
            self->guardDragKey(false);
        if (self && self->keyboardWanted())
            self->removeKeyboard(hwnd);
        if (self) {
            self->m_callbacks.doubleClick = nullptr; // so the mouse goes too
            self->m_callbacks.dragMenu = nullptr;
            self->m_callbacks.leftButtonUp = nullptr;
            self->m_callbacks.mousePress = nullptr;
            self->listenToMouse(false);
            self->m_active = false;
        }
        ::DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    default:
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void KeyListener::onInput(HRAWINPUT input)
{
    RAWINPUT raw {};
    UINT size = sizeof raw;
    if (::GetRawInputData(input, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return;
    if (raw.header.dwType == RIM_TYPEMOUSE) { // mostly moves, which tell only whether a drag began
        if (m_callbacks.dragMenu)
            onDrag(raw.data.mouse);
        if (raw.data.mouse.usButtonFlags & (kAnyButtonDown | kWheel))
            onMouse(raw.data.mouse);
        return;
    }
    if (raw.header.dwType != RIM_TYPEKEYBOARD)
        return;
    const RAWKEYBOARD& key = raw.data.keyboard;
    if (m_callbacks.dragMenu)
        onDragKey(key);
    if (!m_callbacks.doubleCtrl)
        return;
    const bool up = key.Flags & RI_KEY_BREAK;
    const std::uint32_t vk = sidedKey(key);
    // A program's has no device. Not Win顺's own: the Alt KeyRouter lets go
    // after its mask was pressed by a finger.
    const bool device = raw.header.hDevice != nullptr;
    const bool sent = !device && !isOwnInput(key.ExtraInformation);
    const auto time = static_cast<std::uint32_t>(::GetMessageTime()); // when it was pressed, not when we got to it
    // 0xFF: the extra part of an escaped key sequence, not a key. kOwnMask:
    // KeyRouter's mask just before it lets a Shift or Alt pressed alone go
    // on to the program: no key in between, the Alt was alone.
    if (key.VKey == 0xFF || key.ExtraInformation == kOwnMask) {
        if (m_keyLog)
            log(describeKey(key, vk, up, device, sent, time) + ", passed over");
        return;
    }
    followService();
    m_recent[m_recentNext] = {recentKey(vk), up, sent, time};
    m_recentNext = (m_recentNext + 1) % m_recent.size();
    if (recentKey(vk) == 9 && !isOwnInput(key.ExtraInformation)) { // typing, which Windows may hide the pointer for
        const std::scoped_lock lock(m_pressesMutex);
        if (up)
            m_presses.up(vk);
        else
            m_presses.down(vk, time);
    }
    const Keys keys = m_keys.load();
    const bool tapped
        = std::ranges::any_of(m_detectors, [&](const auto& d) { return takes(keys, d.key()) && d.isKey(vk); });
    POINT pt {};
    if (tapped) {
        ::GetCursorPos(&pt);
        if (!up)
            listenToMouse(true);
        // Until a second tap can no longer follow. Armed on the press too: a
        // release can be lost (Ctrl+Alt+Del to lock the screen), and the timer
        // then finds the key up.
        ::SetTimer(m_window.load(), kMouseOffTimer, maxGapMs() + 100, nullptr);
    }
    std::optional<Tap> tap;
    std::string states; // for the key log
    for (DoubleTapDetector& d : m_detectors) {
        if (!takes(keys, d.key()))
            continue;
        const bool own = d.isKey(vk);
        if (own && !up) {
            d.setMaxGap(maxGapMs());
            d.setSide(m_side.load());
        }
        const bool fire = up ? d.keyUp(vk, time, pt.x, pt.y, sent) : d.keyDown(vk, time, pt.x, pt.y, sent);
        if (own && !up && mouseButtonHeld())
            d.mouseUsed();
        // While the UIAccess listener tells of the taps, this one's own word
        // waits for its (holdBackup).
        if (fire)
            tap = Tap {.sent = sentTrace(d.key()),
                .key = vk,
                .time = time,
                .firstPress = d.firstPressTime(),
                .from = m_usingService ? Tap::From::OwnBackup : Tap::From::Own,
                .queuedMs = d.queuedMs()};
        const DoubleTapDetector::Miss miss = up ? d.takeMiss() : DoubleTapDetector::Miss {};
        if (miss.reason != DoubleTapDetector::MissReason::None)
            note({Note::Kind::NearMiss, vk, miss.reason, miss.ms, miss.by});
        if (m_keyLog)
            states += std::format("; {} {}{}{}{}", d.key() == Key::Alt ? "Alt" : d.key() == Key::Shift ? "Shift" : "Ctrl", d.clean() ? "clean" : "not clean",
                d.armed() ? ", armed" : "", fire ? ", DOUBLE TAP" : "",
                miss.reason != DoubleTapDetector::MissReason::None
                    ? std::format(", near miss: {} {} ms{}", DoubleTapDetector::name(miss.reason), miss.ms,
                          miss.by != DoubleTapDetector::Cause::None
                              ? std::format(" (by {})", DoubleTapDetector::name(miss.by))
                              : std::string())
                    : std::string());
    }
    if (m_keyLog)
        log(std::format("{}; keys {}, side {}; service active {}, tells {}, used {}{}",
            describeKey(key, vk, up, device, sent, time), keysName(keys), sideName(m_side.load()),
            yesNo(serviceActive()), yesNo(serviceTells()), yesNo(m_usingService), states));
    // The UIAccess listener's word of it may be in already: a posted message
    // is taken before the input queued ahead of it. Then nothing is held for
    // it, and deliver passes it over as told.
    if (tap && tap->from == Tap::From::OwnBackup
        && !m_teller.toldAlready(tap->time, static_cast<std::uint32_t>(::GetTickCount())))
        holdBackup(std::move(*tap));
    else if (tap)
        deliver(std::move(*tap));
}

bool KeyListener::serviceActive() const noexcept
{
    const DWORD pid = m_servicePid.load();
    return pid && pid == m_readyPid.load() && ::GetTickCount64() - m_heartbeat.load() < keyproto::kLeaseMs;
}

// The UIAccess listener tells of the key's taps, and then only it, so that
// none comes twice. One from before double Alt (or Shift) never tells of
// that key's: they are left to this one.
bool KeyListener::serviceTells() const noexcept
{
    if (!serviceActive())
        return false;
    switch (m_keys.load()) {
    case Keys::Alt:
        return (m_serviceReports.load() & keyproto::kReportsAlt) != 0;
    case Keys::Shift:
        return (m_serviceReports.load() & keyproto::kReportsShift) != 0;
    case Keys::Ctrl:
    case Keys::All:
        break;
    }
    return true;
}

// Never joins half a gesture seen here to one from before the service came
// or went (a lost connection, an elevated window that stopped delivering).
void KeyListener::followService()
{
    // Said once: Alt is then this process's own, and over administrator
    // windows its Raw Input brings nothing (UIPI, docs/pitfalls.md).
    if (!m_oldServiceLogged && (m_keys.load() == Keys::Alt || m_keys.load() == Keys::Shift) && serviceActive()
        && !serviceTells()) {
        m_oldServiceLogged = true;
        log("Double Alt or Shift: the service's UIAccess listener is from before that key and does not tell of it. "
            "Over administrator windows it does nothing until the service is updated.");
    }
    const bool external = serviceTells();
    if (external == m_usingService)
        return;
    for (DoubleTapDetector& d : m_detectors)
        d.reset();
    m_usingService = external;
    if (m_keyLog)
        log(external ? "Key log: double taps now from the UIAccess listener"
                     : "Key log: double taps now from this process's Raw Input");
}

// A key listened for is down: its release is still to come, or was lost.
bool KeyListener::keyHeld() const
{
    const Keys keys = m_keys.load();
    return (takes(keys, Key::Ctrl) && held(VK_CONTROL)) || (takes(keys, Key::Alt) && held(VK_MENU))
        || (takes(keys, Key::Shift) && held(VK_SHIFT));
}

// A double tap of the key and side set. The UIAccess listener tells of all.
bool KeyListener::wanted(std::uint32_t key) const noexcept
{
    const bool alt = key == VK_LMENU || key == VK_RMENU;
    const bool shift = key == VK_LSHIFT || key == VK_RSHIFT;
    if ((!alt && !shift && key != VK_LCONTROL && key != VK_RCONTROL)
        || !takes(m_keys.load(), alt ? Key::Alt : shift ? Key::Shift : Key::Ctrl))
        return false;
    switch (m_side.load()) {
    case DoubleTapDetector::Side::Left:
        return key == VK_LCONTROL || key == VK_LMENU || key == VK_LSHIFT;
    case DoubleTapDetector::Side::Right:
        return key == VK_RCONTROL || key == VK_RMENU || key == VK_RSHIFT;
    case DoubleTapDetector::Side::Both:
        break;
    }
    return true;
}

// The UIAccess listener's word of a double tap: taken while that listener is
// the one to tell of them (serviceTells; else this process's own word is)
// and the keyboard's Raw Input is this listener's (not set aside while a
// shortcut is recorded or keys are held for the launcher). This process's
// own word of the same tap, waiting for it, is let go: told once. `span`:
// how long before `time` its first press was; 0 from a listener from before
// that (keyproto::tapTimes), which the word waiting here then tells instead.
void KeyListener::onServiceTap(std::uint32_t key, std::uint32_t time, std::uint32_t span, bool sent)
{
    const bool tells = serviceTells();
    const bool registered = keyboardRegistered(m_window.load());
    const bool waiting = m_backup && m_backup->time == time;
    if (m_keyLog)
        log(std::format("Key log: UIAccess listener: double {} at {}, {} ms ago, {}{}", kRecentNames[recentKey(key)],
            time, static_cast<DWORD>(::GetTickCount()) - time,
            span ? std::format("begun {} ms before", span) : std::string("its first press not told"),
            !tells        ? ", passed over: not its key now"
                : !registered ? ", passed over: a shortcut being recorded or keys held"
                              : ""));
    if (!tells || (waiting && !registered))
        return; // this process's own word counts (or counts still: holdBackup)
    if (!registered) {
        const auto now = static_cast<std::uint32_t>(::GetTickCount());
        if (wanted(key) && !m_teller.toldAlready(time, now))
            dropped({Note::Kind::SetAside, key, {}, now - time}, time);
        return;
    }
    std::optional<std::uint32_t> firstPress = span ? std::optional(time - span) : std::nullopt;
    std::uint32_t queuedMs = 0; // that listener does not say: this one's own word does
    if (waiting) {
        if (!firstPress)
            firstPress = m_backup->firstPress;
        queuedMs = m_backup->queuedMs;
        m_backup.reset();
        ::KillTimer(m_window.load(), kBackupTimer);
    }
    deliver({.sent = sent ? "UIAccess listener: partly simulated" : "",
        .key = key,
        .time = time,
        .firstPress = firstPress,
        .from = Tap::From::Helper,
        .queuedMs = queuedMs});
}

// This process's own word of a double tap while the UIAccess listener tells
// of them: told should that listener's not come within kBackupMs. Its word
// can take seconds (GitHub #111), and over a window of this process's
// integrity this one sees just as much.
void KeyListener::holdBackup(Tap tap)
{
    if (m_backup) // two taps within kBackupMs cannot be; told, not lost
        deliver(*std::exchange(m_backup, std::nullopt));
    m_backup = std::move(tap);
    ::SetTimer(m_window.load(), kBackupTimer, kBackupMs, nullptr);
}

void KeyListener::onBackupTimer()
{
    ::KillTimer(m_window.load(), kBackupTimer);
    std::optional<Tap> backup = std::exchange(m_backup, std::nullopt);
    if (!backup)
        return;
    if (m_keyLog)
        log(std::format("Key log: no word from the UIAccess listener of double {} at {}: this process's own told",
            kRecentNames[recentKey(backup->key)], backup->time));
    deliver(std::move(*backup));
}

void KeyListener::deliver(Tap tap)
{
    const char* const name = kRecentNames[recentKey(tap.key)];
    if (!wanted(tap.key)) {
        if (m_keyLog)
            log(std::format("Key log: double {} passed over: not the key or side set", name));
        return;
    }
    // Both sources can finish the same gesture: during a handover, and while
    // this process's own word stands in for the UIAccess listener's
    // (holdBackup). WM_INPUT uses the same event timestamp in both processes:
    // late duplicates lose. Told late, the moment has passed, unless nothing
    // came since (TapTeller, GitHub #111).
    const auto now = static_cast<std::uint32_t>(::GetTickCount());
    switch (m_teller.judge(tap.time, now, [&] { return quietSince(tap.time); })) {
    case TapTeller::Verdict::Duplicate:
        if (m_keyLog)
            log(std::format("Key log: double {} passed over: already told", name));
        return;
    case TapTeller::Verdict::Stale:
        if (m_keyLog)
            log(std::format("Key log: double {} at {} passed over: {} ms old", name, tap.time, now - tap.time));
        dropped({Note::Kind::Late, tap.key, {}, now - tap.time}, tap.time);
        return;
    case TapTeller::Verdict::Tell:
        break;
    }
    tap.toldAt = now;
    if (m_keyLog)
        log(std::format("Key log: double {} at {} told{}, {}{}", name, tap.time,
            tap.from == Tap::From::Helper          ? " (UIAccess listener)"
                : tap.from == Tap::From::OwnBackup ? " (in its place)"
                                                   : "",
            tap.firstPress ? std::format("begun {} ms before", tap.time - *tap.firstPress)
                           : std::string("its first press not known"),
            tap.queuedMs ? std::format(", its first press seemingly {} ms, let go as the second was pressed", tap.queuedMs)
                         : std::string()));
    m_callbacks.doubleCtrl(tap);
}

// Nothing came since a tap (its time, the input's clock): no other input in
// the session, which GetLastInputInfo counts also when it goes to an
// administrator's window (UIPI keeps that from this process's Raw Input),
// and no window came to the front (EVENT_SYSTEM_FOREGROUND).
bool KeyListener::quietSince(std::uint32_t time) const
{
    LASTINPUTINFO last {sizeof last};
    return m_foregroundHook && ::GetLastInputInfo(&last)
        && static_cast<std::int32_t>(last.dwTime - time) <= kSameInputMs
        && static_cast<std::int32_t>(m_foregroundAt - time) < 0;
}

void CALLBACK KeyListener::onForeground(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD time)
{
    if (KeyListener* self = t_listener)
        self->m_foregroundAt = time;
}

// A tap dropped, noted once: both listeners may tell of it.
void KeyListener::dropped(const Note& note, std::uint32_t time)
{
    if (m_teller.firstDrop(time))
        this->note(note);
}

void KeyListener::note(const Note& note) const
{
    if (m_callbacks.note)
        m_callbacks.note(note);
}

std::optional<std::uint32_t> KeyListener::keyDownAtOrBefore(std::uint32_t time) const
{
    const std::scoped_lock lock(m_pressesMutex);
    return m_presses.atOrBefore(time);
}

void KeyListener::log(const std::string& line) const
{
    if (m_callbacks.log)
        m_callbacks.log(line);
}

// The last keys, oldest first, when a program sent any of the last four
// events of the key tapped: "LAlt↓ +0, LCtrl↓ sent +812, …". Empty when the
// taps were all typed.
std::string KeyListener::sentTrace(Key key) const
{
    const int left = key == Key::Alt ? 5 : key == Key::Shift ? 3 : 1; // its left one among the names, the right one next
    bool sent = false;
    int events = 0;
    for (std::size_t i = 0; i < m_recent.size() && events < 4; ++i) {
        const Recent& r = m_recent[(m_recentNext + m_recent.size() - 1 - i) % m_recent.size()];
        if (r.key == left || r.key == left + 1) {
            ++events;
            sent = sent || r.sent;
        }
    }
    if (!sent)
        return {};
    std::string trace;
    std::uint32_t first = 0;
    for (std::size_t i = 0; i < m_recent.size(); ++i) {
        const Recent& r = m_recent[(m_recentNext + i) % m_recent.size()];
        if (r.key == 0)
            continue;
        if (trace.empty())
            first = r.time;
        else
            trace += ", ";
        trace
            += std::format("{}{}{} +{}", kRecentNames[r.key], r.up ? "↑" : "↓", r.sent ? " sent" : "", r.time - first);
    }
    return trace;
}

void KeyListener::onMouse(const RAWMOUSE& mouse)
{
    if (m_callbacks.mousePress && (mouse.usButtonFlags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN))) {
        POINT point {};
        if (::GetPhysicalCursorPos(&point)) {
            const bool swapped = ::GetSystemMetrics(SM_SWAPBUTTON) != 0;
            const auto inputTime = static_cast<std::uint32_t>(::GetMessageTime());
            if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) m_callbacks.mousePress(point, swapped, inputTime);
            if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) m_callbacks.mousePress(point, !swapped, inputTime);
        }
    }
    for (DoubleTapDetector& d : m_detectors)
        d.mouseUsed();
    if (!m_callbacks.doubleClick)
        return;
    // The device's buttons: with them swapped (left-handed), its right one
    // is the one that clicks.
    const USHORT primary = ::GetSystemMetrics(SM_SWAPBUTTON) ? RI_MOUSE_RIGHT_BUTTON_DOWN : RI_MOUSE_LEFT_BUTTON_DOWN;
    if ((mouse.usButtonFlags & kAnyButtonDown) != primary) {
        m_clicks.otherInput(); // another button, the wheel, or more than one at once
        return;
    }
    // Where the pointer was when the press was sent, by its own clock, as
    // Windows judges a double click.
    const DWORD at = ::GetMessagePos();
    const POINT pt {static_cast<short>(LOWORD(at)), static_cast<short>(HIWORD(at))};
    const auto time = static_cast<std::uint32_t>(::GetMessageTime());
    // Safe here: this thread has no window that WindowFromPoint would ask.
    const HWND window = ::WindowFromPoint(pt);
    m_clicks.setConfig(
        {::GetDoubleClickTime(), ::GetSystemMetrics(SM_CXDOUBLECLK) / 2, ::GetSystemMetrics(SM_CYDOUBLECLK) / 2});
    if (!m_clicks.leftDown(time, pt.x, pt.y, reinterpret_cast<std::uintptr_t>(window), m_menuOpen.load()))
        return;
    if (m_clicks.firstClosedMenu() || modifierHeld())
        return;
    const int other = ::GetSystemMetrics(SM_SWAPBUTTON) ? VK_LBUTTON : VK_RBUTTON;
    if (held(other) || held(VK_MBUTTON) || held(VK_XBUTTON1) || held(VK_XBUTTON2))
        return;
    m_callbacks.doubleClick(window, pt);
}

// The primary button and the pointer while it is held: whether a drag has
// begun, for the drop pie's key. Kept apart from onMouse: a move or a release there
// would spoil a double tap (mouseUsed) or the double click's pair.
void KeyListener::onDrag(const RAWMOUSE& mouse)
{
    const USHORT flags = mouse.usButtonFlags;
    const auto time = static_cast<std::uint32_t>(::GetMessageTime());
    if (flags != 0) {
        // The device's buttons: swapped (left-handed), its right one drags.
        const bool swapped = ::GetSystemMetrics(SM_SWAPBUTTON) != 0;
        const USHORT down = swapped ? RI_MOUSE_RIGHT_BUTTON_DOWN : RI_MOUSE_LEFT_BUTTON_DOWN;
        const USHORT up = swapped ? RI_MOUSE_RIGHT_BUTTON_UP : RI_MOUSE_LEFT_BUTTON_UP;
        if (flags & down) {
            POINT pt {};
            ::GetPhysicalCursorPos(&pt);
            // The drag rectangle and the scale of the monitor pressed on.
            UINT dpiX = 96;
            UINT dpiY = 96;
            if (FAILED(::GetDpiForMonitor(::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
                dpiX = dpiY = 96;
            m_dragRect = {::GetSystemMetricsForDpi(SM_CXDRAG, dpiX), ::GetSystemMetricsForDpi(SM_CYDRAG, dpiY)};
            m_pressed = true;
            m_dragging = false;
            m_pressedAt = pt;
            m_dragFound = false;
            m_dragLooked = time;
            // Held on a list of files, the key is the pie's from now on: it
            // may come before the hand moves (startDrag).
            if (listsFiles(::WindowFromPoint(pt)))
                guardDragKey(true);
        } else if (flags & (kAnyButtonDown & ~down)) {
            m_pressed = false; // another button too: not a plain drag
            guardDragKey(false);
            m_starting = false;
        }
        if (flags & up) {
            const bool dragged = m_pressed && (m_dragging || m_dragFound);
            m_pressed = false;
            guardDragKey(false);
            m_starting = false;
            if (dragged && m_callbacks.leftButtonUp)
                m_callbacks.leftButtonUp();
        }
        return;
    }
    if (!m_pressed)
        return;
    POINT pt {};
    ::GetPhysicalCursorPos(&pt);
    // Windows starts a drag once the pointer leaves the rectangle centred on
    // the press (DragDetect): before that it is a click.
    if (!m_dragging)
        m_dragging = std::abs(pt.x - m_pressedAt.x) * 2 > m_dragRect.cx || std::abs(pt.y - m_pressedAt.y) * 2 > m_dragRect.cy;
    // Once a drag has begun, the pie's key is held back from it.
    if (!m_dragFound && m_dragging && time - m_dragLooked >= kDragLookMs) {
        m_dragLooked = time;
        if (oleDrag().capture) {
            m_dragFound = true;
            guardDragKey(true);
        }
    }
}

// The drop pie's key pressed: during an OLE drag it brings up the pie (or
// takes it away again); typed at any other time it is left alone. Once a
// drag is under way it is a hotkey (guardDragKey), which raw input does not
// see: WM_HOTKEY tells of it then.
void KeyListener::onDragKey(const RAWKEYBOARD& key)
{
    if (key.MakeCode != kDragKeyScan || (key.Flags & (RI_KEY_E0 | RI_KEY_E1)) || isOwnInput(key.ExtraInformation))
        return; // replayDragKey's own too
    if (key.Flags & RI_KEY_BREAK) {
        m_dragKeyDown = false;
        return;
    }
    if (!m_dragKeyGuarded && !std::exchange(m_dragKeyDown, true)) // held down: its repeats
        onDragKeyPressed();
}

void KeyListener::onDragKeyPressed()
{
    if (m_starting)
        return;
    // Held on a file, not moved yet: no drag to bring the pie into.
    if (m_dragKeyGuarded && m_pressed && !m_dragging && !m_dragFound) {
        startDrag();
        return;
    }
    const DragSource source = oleDrag();
    if (!source.capture) {
        if (m_dragKeyGuarded)
            replayDragKey(); // not a drag after all: typed, as it would have been
        if (m_keyLog)
            log("Key log: the drop pie's key, but no OLE drag is under way");
        return;
    }
    POINT pt {};
    ::GetPhysicalCursorPos(&pt);
    m_callbacks.dragMenu(pt, source.capture, source.pid);
}

// A drag of files is an OLE drag: while DoDragDrop runs, the source's thread
// has captured the mouse with OLE's own window. Its thread is the one of the
// window pressed on; else the one in front. A window dragged by its title
// bar (Aero Shake), a selection rectangle, a slider: no. (Safe here: this
// thread has no window that WindowFromPoint would ask.) Total Commander
// drags files inside its own window by itself, its main window holding the
// capture, and starts an OLE drag only once the pointer is over another
// program's window: such as the pie, which is shown (unseen until
// DragEnter) all the same. Its title bar dragged (Windows' own move, Aero
// Shake) or a menu open is not that.
KeyListener::DragSource KeyListener::oleDrag() const
{
    const auto captureOf = [](DWORD thread, bool totalCommander) -> HWND {
        GUITHREADINFO info {sizeof info};
        if (!::GetGUIThreadInfo(thread, &info) || !info.hwndCapture
            || (info.flags & (GUI_INMOVESIZE | GUI_INMENUMODE | GUI_POPUPMENUMODE)))
            return nullptr;
        wchar_t name[32] {};
        ::GetClassNameW(info.hwndCapture, name, 32);
        return std::wstring_view(name) == (totalCommander ? L"TTOTAL_CMD" : L"CLIPBRDWNDCLASS") ? info.hwndCapture : nullptr;
    };
    const HWND pressed = m_pressed ? ::WindowFromPoint(m_pressedAt) : nullptr;
    const DWORD pressedThread = pressed ? ::GetWindowThreadProcessId(pressed, nullptr) : 0;
    HWND capture = pressed ? captureOf(pressedThread, false) : nullptr;
    if (!capture)
        capture = captureOf(0, false); // the foreground thread
    if (!capture && pressed)
        capture = captureOf(pressedThread, true);
    DWORD pid = 0;
    if (capture)
        ::GetWindowThreadProcessId(capture, &pid);
    return {capture, pid};
}

// The drop pie's key as a hotkey of this window while a file list is pressed
// on or a drag is under way:
// Windows takes a hotkey before the input method and the program in front,
// so neither types it (OLE's drag loop leaves keys alone, Total Commander's
// own drag does not: "·" went into its command line). Raw input does not see
// it then either. By the layout of the program in front, where the key is
// under Esc. One another program holds already stays its own.
void KeyListener::guardDragKey(bool on)
{
    const HWND window = m_window.load();
    if (on == m_dragKeyGuarded || !window)
        return;
    if (!on) {
        for (int i = 0; i < static_cast<int>(kDragKeyMods.size()); ++i)
            ::UnregisterHotKey(window, kDragKeyHotkey + i);
        m_dragKeyGuarded = false;
        return;
    }
    const HKL layout = ::GetKeyboardLayout(::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr));
    const UINT vk = ::MapVirtualKeyExW(kDragKeyScan, MAPVK_VSC_TO_VK, layout);
    std::string held;
    for (int i = 0; vk && i < static_cast<int>(kDragKeyMods.size()); ++i) {
        const UINT mods = kDragKeyMods[static_cast<std::size_t>(i)];
        if (!::RegisterHotKey(window, kDragKeyHotkey + i, mods | MOD_NOREPEAT, vk))
            continue;
        m_dragKeyGuarded = true;
        held += std::format(" {}{}key", (mods & MOD_CONTROL) ? "Ctrl+" : "", (mods & MOD_SHIFT) ? "Shift+" : "");
    }
    if (m_keyLog)
        log(std::format("Key log: a file list pressed or a drag begun: the drop pie's key (vk 0x{:02X}) held back as:{}", vk,
            held.empty() ? " nothing" : held));
}

// The key pressed while a file is held but not moved: the program has not
// begun a drag (it waits for the pointer to leave the drag rectangle), so the
// pointer is moved just past it, and put back (SetCursorPos, which OLE
// follows) once the drag has begun.
void KeyListener::startDrag()
{
    ::GetPhysicalCursorPos(&m_startFrom);
    // Six drag rectangles: Total Commander begins its own past about four.
    const LONG slop = std::max(m_dragRect.cx, m_dragRect.cy);
    POINT to {m_startFrom.x + 6 * slop, m_startFrom.y};
    if (!::MonitorFromPoint(to, MONITOR_DEFAULTTONULL))
        to.x = m_startFrom.x - 6 * slop; // by the screen's right edge
    m_starting = true;
    m_startTicks = 0;
    // As input, in steps: Total Commander takes no drag from SetCursorPos.
    const int left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = std::max(2, ::GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int height = std::max(2, ::GetSystemMetrics(SM_CYVIRTUALSCREEN));
    std::array<INPUT, 4> moves {};
    for (std::size_t i = 0; i < moves.size(); ++i) {
        const LONG x = m_startFrom.x + (to.x - m_startFrom.x) * static_cast<LONG>(i + 1) / static_cast<LONG>(moves.size());
        moves[i].type = INPUT_MOUSE;
        moves[i].mi.dx = MulDiv(x - left, 65535, width - 1);
        moves[i].mi.dy = MulDiv(to.y - top, 65535, height - 1);
        moves[i].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        moves[i].mi.dwExtraInfo = kOwnInput;
    }
    ::SendInput(static_cast<UINT>(moves.size()), moves.data(), sizeof(INPUT));
    ::SetTimer(m_window.load(), kStartDragTimer, kStartDragTickMs, nullptr);
}

void KeyListener::onStartDragTimer()
{
    const HWND window = m_window.load();
    if (!m_starting || !m_pressed) {
        ::KillTimer(window, kStartDragTimer);
        m_starting = false;
        return;
    }
    ++m_startTicks;
    // Total Commander holds its capture from the press on: its own drag
    // begins at the move, given a moment.
    const DragSource source = m_startTicks >= 2 ? oleDrag() : DragSource {};
    if (!source.capture && m_startTicks < kStartDragTicks)
        return;
    ::KillTimer(window, kStartDragTimer);
    m_starting = false;
    ::SetCursorPos(m_startFrom.x, m_startFrom.y);
    if (!source.capture) {
        if (m_keyLog)
            log("Key log: the drop pie's key on a file held, but no drag began");
        replayDragKey();
        return;
    }
    m_dragFound = true;
    if (m_keyLog)
        log(std::format("Key log: the drop pie's key on a file held: a drag begun for it in {} ms",
            m_startTicks * kStartDragTickMs));
    m_callbacks.dragMenu(m_startFrom, source.capture, source.pid);
}

// The key was held back, but no drag came of it: typed after all, where it
// would have gone (marked as Win顺's, so not taken for the pie's again).
void KeyListener::replayDragKey()
{
    guardDragKey(false);
    std::array<INPUT, 2> keys {};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        keys[i].type = INPUT_KEYBOARD;
        keys[i].ki.wScan = kDragKeyScan;
        keys[i].ki.dwFlags = KEYEVENTF_SCANCODE | (i == 1 ? KEYEVENTF_KEYUP : 0);
        keys[i].ki.dwExtraInfo = kOwnInput;
    }
    ::SendInput(static_cast<UINT>(keys.size()), keys.data(), sizeof(INPUT));
}

// The mouse only while it can spoil a tap: from a press of the key until a
// second tap can no longer follow. Listened to all the time, every move wakes this
// thread, up to thousands of times a second with a gaming mouse: only when
// double clicks are asked for (cheap still: nothing waits on it, and a move
// is one look at the button flags).
void KeyListener::listenToMouse(bool on)
{
    if (!on && mouseWanted())
        return;
    if (on == m_mouseOn)
        return;
    const RAWINPUTDEVICE mouse {kGenericDesktop, kMouse, static_cast<DWORD>(on ? RIDEV_INPUTSINK : RIDEV_REMOVE),
        on ? m_window.load() : nullptr};
    if (::RegisterRawInputDevices(&mouse, 1, sizeof mouse) || !on)
        m_mouseOn = on;
}

// The keyboard's registration, this listener's; while a hook has it set
// aside (KeyboardRawInputAside), it is what goes back when the last of those
// is done (true: it will). What Windows refused goes in the log, outside the
// lock: a hook waits on that lock, and every key press in the system on it.
bool KeyListener::registerKeyboard(HWND window)
{
    const RAWINPUTDEVICE keyboard {kGenericDesktop, kKeyboard, RIDEV_INPUTSINK, window};
    DWORD error = 0;
    {
        const std::scoped_lock lock(g_asideMutex);
        if (!g_aside.registers({reinterpret_cast<std::uintptr_t>(window), keyboard.dwFlags})
            || ::RegisterRawInputDevices(&keyboard, 1, sizeof keyboard))
            return true;
        error = ::GetLastError();
    }
    log(std::format("Key listener: keyboard Raw Input not registered (error {}): no double tap, no drop pie key", error));
    return false;
}

// ... removed as the window goes: only its own (the process has one keyboard
// registration, and the next listener may have it already), and while set
// aside, not put back either. One Windows cannot say whose is removed all the
// same: most likely this one's, and left, it would take the keys away from the
// hooks for nothing (docs/pitfalls.md).
void KeyListener::removeKeyboard(HWND window)
{
    std::string line;
    {
        const std::scoped_lock lock(g_asideMutex);
        if (!g_aside.removes(reinterpret_cast<std::uintptr_t>(window)))
            return;
        const KeyboardRawInput keyboard = keyboardRawInput();
        if (!keyboard.error && (!keyboard.registration || keyboard.registration->hwndTarget != window))
            return;
        const RAWINPUTDEVICE remove {kGenericDesktop, kKeyboard, RIDEV_REMOVE, nullptr};
        if (::RegisterRawInputDevices(&remove, 1, sizeof remove)) {
            if (keyboard.error)
                line = std::format("Key listener: keyboard Raw Input removed unseen (error {} asking for it)", keyboard.error);
        } else {
            line = std::format("Key listener: keyboard Raw Input not removed (error {})", ::GetLastError());
        }
    }
    if (!line.empty())
        log(line);
}

KeyboardRawInput keyboardRawInput()
{
    const auto isKeyboard = [](const RAWINPUTDEVICE& d) { return d.usUsagePage == kGenericDesktop && d.usUsage == kKeyboard; };
    // Asked for the count first, then the list. Another thread of the process
    // can register a device between the two (KeyListener's its mouse, around
    // each press of the key): too small then, the list is asked for again with
    // the count Windows gave, a few times at most.
    std::vector<RAWINPUTDEVICE> devices;
    for (int tries = 0; tries < 4; ++tries) {
        UINT count = static_cast<UINT>(devices.size());
        const UINT got = ::GetRegisteredRawInputDevices(devices.empty() ? nullptr : devices.data(), &count, sizeof(RAWINPUTDEVICE));
        if (got == static_cast<UINT>(-1)) {
            if (const DWORD error = ::GetLastError(); error != ERROR_INSUFFICIENT_BUFFER)
                return {.error = error};
        } else if (!devices.empty()) {
            const auto end = devices.begin() + (std::min)(std::size_t {got}, devices.size());
            const auto keyboard = std::find_if(devices.begin(), end, isKeyboard);
            return {.registration = keyboard == end ? std::nullopt : std::optional(*keyboard)};
        }
        if (count == 0)
            return {}; // none registered at all
        devices.resize(count);
    }
    return {.error = ERROR_INSUFFICIENT_BUFFER};
}

KeyboardRawInputAside::KeyboardRawInputAside(Log log)
    : m_log(log)
{
    std::string line;
    bool warning = false;
    {
        const std::scoped_lock lock(g_asideMutex);
        if (g_aside.take()) {
            const KeyboardRawInput keyboard = keyboardRawInput();
            const RAWINPUTDEVICE remove {kGenericDesktop, kKeyboard, RIDEV_REMOVE, nullptr};
            if (keyboard.error) {
                // Not taken away blind: what to put back would not be known.
                warning = true;
                line = std::format("keyboard Raw Input not looked up (error {}): if registered, the hook misses keys "
                                   "typed into Win顺's windows",
                    keyboard.error);
            } else if (!keyboard.registration) {
                // None (no double tap, no drop pie): nothing to set aside.
            } else if (::RegisterRawInputDevices(&remove, 1, sizeof remove)) {
                g_aside.taken(Registration {
                    reinterpret_cast<std::uintptr_t>(keyboard.registration->hwndTarget), keyboard.registration->dwFlags});
                line = "keyboard Raw Input set aside";
            } else {
                warning = true;
                line = std::format("keyboard Raw Input not set aside (error {}): the hook misses keys typed into "
                                   "Win顺's windows",
                    ::GetLastError());
            }
        }
    }
    if (m_log && !line.empty())
        m_log(warning, line); // outside the lock: the other holders and KeyListener do not wait on it
}

KeyboardRawInputAside::~KeyboardRawInputAside()
{
    std::string line;
    bool warning = false;
    {
        const std::scoped_lock lock(g_asideMutex);
        if (const std::optional<Registration> back = g_aside.give()) {
            const auto window = reinterpret_cast<HWND>(back->window);
            const RAWINPUTDEVICE keyboard {kGenericDesktop, kKeyboard, static_cast<DWORD>(back->flags), window};
            if (!::IsWindow(window)) {
                warning = true; // KeyListener's window went without removing it (WM_CLOSE): nothing to put back
                line = "keyboard Raw Input not put back: its window is gone";
            } else if (::RegisterRawInputDevices(&keyboard, 1, sizeof keyboard)) {
                line = "keyboard Raw Input put back";
            } else {
                warning = true;
                line = std::format("keyboard Raw Input not put back (error {}): no double tap until the next hold or "
                                   "recording ends",
                    ::GetLastError());
                g_aside.notPutBack(*back); // the next holder that goes tries again
            }
        }
    }
    if (m_log && !line.empty())
        m_log(warning, line);
}

} // namespace ws
