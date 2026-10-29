#include "KeyRouter.h"

#include <QtLogging>

#include <array>
#include <latch>

namespace ws {

namespace {

// Low-level hooks and out-of-context events are called on the thread that set them.
thread_local KeyRouter* t_router = nullptr;

constexpr UINT kDrainMessage = WM_APP; // to the router's thread (drain)
constexpr UINT kRefreshMessage = WM_APP + 1; // the search field was clicked
constexpr UINT kToUnicodeKeepState = 0x4; // ToUnicodeEx: leave the dead key state alone (Windows 10 1607)

bool held(int vk)
{
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}

Qt::KeyboardModifiers heldModifiers()
{
    Qt::KeyboardModifiers modifiers;
    if (held(VK_SHIFT))
        modifiers |= Qt::ShiftModifier;
    if (held(VK_CONTROL))
        modifiers |= Qt::ControlModifier;
    if (held(VK_MENU))
        modifiers |= Qt::AltModifier;
    return modifiers;
}

// Shift, Ctrl or Alt (a hook is told which one of the two), else none.
Qt::KeyboardModifier modifierOf(UINT vk)
{
    switch (vk) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return Qt::ShiftModifier;
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return Qt::ControlModifier;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return Qt::AltModifier;
    default:
        return Qt::NoModifier;
    }
}

// Keys that type a character in some layout: letters, digits, punctuation,
// the number pad, space. A dead key (^ on a French keyboard) as well.
bool typesCharacter(UINT vk)
{
    return (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE)
        || (vk >= VK_OEM_1 && vk <= VK_OEM_3) || (vk >= VK_OEM_4 && vk <= VK_OEM_8) || vk == VK_OEM_102
        || vk == VK_SPACE || vk == VK_PACKET;
}

// The key as Qt names it; 0 for those named after the character they type.
int qtKey(UINT vk, bool extended, bool shift)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return static_cast<int>(vk); // Qt::Key_A… and Qt::Key_0… have the same codes
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return Qt::Key_0 + static_cast<int>(vk - VK_NUMPAD0);
    if (vk >= VK_F1 && vk <= VK_F24)
        return Qt::Key_F1 + static_cast<int>(vk - VK_F1);
    switch (vk) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return Qt::Key_Shift;
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return Qt::Key_Control;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return Qt::Key_Alt;
    case VK_BACK:
        return Qt::Key_Backspace;
    case VK_TAB:
        return shift ? Qt::Key_Backtab : Qt::Key_Tab; // as Qt spells Shift+Tab
    case VK_RETURN:
        return extended ? Qt::Key_Enter : Qt::Key_Return; // the number pad's is extended
    case VK_ESCAPE:
        return Qt::Key_Escape;
    case VK_SPACE:
        return Qt::Key_Space;
    case VK_PRIOR:
        return Qt::Key_PageUp;
    case VK_NEXT:
        return Qt::Key_PageDown;
    case VK_END:
        return Qt::Key_End;
    case VK_HOME:
        return Qt::Key_Home;
    case VK_LEFT:
        return Qt::Key_Left;
    case VK_UP:
        return Qt::Key_Up;
    case VK_RIGHT:
        return Qt::Key_Right;
    case VK_DOWN:
        return Qt::Key_Down;
    case VK_INSERT:
        return Qt::Key_Insert;
    case VK_DELETE:
        return Qt::Key_Delete;
    case VK_APPS:
        return Qt::Key_Menu;
    default:
        return 0;
    }
}

// Whether the clipboard window acts on the key (ClipboardPage.qml, its
// search field's TextInput); the rest goes on to the program in front.
// `numberKeys`: the modifier that pastes a row with a digit.
// A menu's keys: to move, choose, close and jump by a first letter.
bool wantedByMenu(const KeyRouter::Key& key)
{
    if (key.modifiers.testFlag(Qt::ControlModifier) || key.modifiers.testFlag(Qt::AltModifier))
        return false;
    switch (key.vk) {
    case VK_RETURN:
    case VK_ESCAPE:
    case VK_UP:
    case VK_DOWN:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_APPS:
        return true;
    default:
        return typesCharacter(key.vk);
    }
}

bool wanted(const KeyRouter::Key& key, Qt::KeyboardModifier numberKeys)
{
    const UINT vk = key.vk;
    const bool shift = key.modifiers.testFlag(Qt::ShiftModifier);
    const bool ctrl = key.modifiers.testFlag(Qt::ControlModifier);
    const bool alt = key.modifiers.testFlag(Qt::AltModifier);
    if (ctrl && alt) // AltGr: a character of the layout (€ on a German keyboard); else another program's hotkey
        return !key.text.isEmpty() && key.text.at(0).isPrint();
    const bool digit = vk >= '1' && vk <= '9';
    if (alt) // pastes the nth row (Shift: as plain text), the preview, closes
        return (digit && numberKeys == Qt::AltModifier) || vk == 'P' || vk == VK_F4;
    if (ctrl) {
        if (digit)
            return numberKeys == Qt::ControlModifier; // likewise
        switch (vk) {
        case 'A': // select all, copy, paste, cut, redo; Ctrl+C and Ctrl+P act on the row
        case 'C':
        case 'V':
        case 'X':
        case 'Y':
        case 'P':
            return !shift; // Ctrl+Shift+V and the like stay free for other programs' hotkeys
        case 'Z':
            return true; // undo, redo
        case VK_BACK:
        case VK_DELETE:
        case VK_INSERT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_RETURN:
            return true;
        default:
            return false; // Ctrl+S, Ctrl+W, Ctrl+Tab…: the program's
        }
    }
    switch (vk) {
    case VK_RETURN:
    case VK_ESCAPE:
    case VK_TAB:
    case VK_BACK:
    case VK_DELETE:
    case VK_INSERT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_APPS:
        return true;
    case VK_F10:
        return shift; // the context menu
    default:
        return typesCharacter(vk);
    }
}

INPUT keyInput(WORD vk, WORD scanCode, DWORD flags, ULONG_PTR mark = kOwnInput)
{
    INPUT input {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = scanCode;
    input.ki.dwFlags = flags;
    input.ki.dwExtraInfo = mark;
    return input;
}

// A key the program in front sees between a modifier's press and release,
// so that it does not count the modifier as pressed alone.
void sendMask(ULONG_PTR mark = kOwnInput)
{
    std::array keys {keyInput(kMaskKey, 0, 0, mark), keyInput(kMaskKey, 0, KEYEVENTF_KEYUP, mark)};
    ::SendInput(static_cast<UINT>(keys.size()), keys.data(), sizeof(INPUT));
}

} // namespace

KeyRouter::KeyRouter(HWND target, HWND window, Callbacks callbacks, Qt::KeyboardModifier numberKeys, Policy policy)
    : m_target(target)
    , m_window(window)
    , m_numberKeys(numberKeys)
    , m_policy(policy)
    , m_process(::GetCurrentProcessId())
    , m_callbacks(std::move(callbacks))
{
    std::latch ready(1);
    m_thread = std::jthread([this, &ready] {
        t_router = this;
        MSG msg;
        ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the thread's message queue, for WM_QUIT
        m_threadId = ::GetCurrentThreadId();
        // Every key press and mouse move in the system waits for the hooks' answer.
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        HHOOK keyboard = ::SetWindowsHookExW(WH_KEYBOARD_LL, &KeyRouter::keyboardProc, instance, 0);
        if (!keyboard) {
            m_setupStep = "keyboard";
            m_setupError = ::GetLastError();
        }
        const HHOOK mouse = keyboard ? ::SetWindowsHookExW(WH_MOUSE_LL, &KeyRouter::mouseProc, instance, 0) : nullptr;
        if (keyboard && !mouse) {
            m_setupStep = "mouse";
            m_setupError = ::GetLastError();
        }
        const HWINEVENTHOOK foreground = mouse
            ? ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &KeyRouter::foregroundProc,
                                0, 0, WINEVENT_OUTOFCONTEXT)
            : nullptr;
        if (mouse && !foreground) {
            m_setupStep = "foreground";
            m_setupError = ::GetLastError();
        }
        m_active = foreground != nullptr;
        ready.count_down();
        if (m_active) {
            // The hooks are called from inside GetMessage; the events too.
            while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (msg.hwnd == nullptr && msg.message == kDrainMessage) {
                    checkDrained(); // nothing held now: done at once
                    continue;
                }
                if (msg.hwnd == nullptr && msg.message == kRefreshMessage) {
                    if (!m_draining) {
                        count(Refreshes);
                        // Installing succeeded once, but Windows can remove a
                        // timed-out hook without notifying us. Keep the old
                        // one if its replacement cannot be installed.
                        if (const HHOOK replacement = ::SetWindowsHookExW(
                                WH_KEYBOARD_LL, &KeyRouter::keyboardProc, instance, 0)) {
                            m_refreshError = 0;
                            m_refreshUnhookError = 0;
                            if (!::UnhookWindowsHookEx(keyboard)) {
                                m_refreshUnhookError = ::GetLastError();
                                qWarning("KeyRouter: replaced an unavailable keyboard hook (error %lu)", m_refreshUnhookError.load());
                            }
                            keyboard = replacement;
                        } else {
                            m_refreshError = ::GetLastError();
                            count(RefreshFailed);
                            qWarning("KeyRouter: keyboard hook not refreshed (error %lu)", m_refreshError.load());
                        }
                    }
                    continue;
                }
                ::DispatchMessageW(&msg);
            }
        }
        if (foreground)
            ::UnhookWinEvent(foreground);
        if (mouse)
            ::UnhookWindowsHookEx(mouse);
        if (keyboard)
            ::UnhookWindowsHookEx(keyboard);
    });
    ready.wait();
}

KeyRouter::~KeyRouter()
{
    ::PostThreadMessageW(m_threadId.load(), WM_QUIT, 0, 0); // ends the message loop
    if (m_thread.joinable())
        m_thread.join();
}

void KeyRouter::refresh()
{
    if (!m_draining && m_active && !::PostThreadMessageW(m_threadId.load(), kRefreshMessage, 0, 0)) {
        m_refreshError = ::GetLastError();
        count(RefreshFailed);
    }
}

QJsonObject KeyRouter::diagnosticStatus() const
{
    QJsonObject status {{QStringLiteral("initialized"), isActive()},
        {QStringLiteral("setup_step"), QString::fromLatin1(m_setupStep)},
        {QStringLiteral("setup_error"), static_cast<qint64>(m_setupError)},
        {QStringLiteral("refresh_error"), static_cast<qint64>(m_refreshError.load())},
        {QStringLiteral("refresh_unhook_error"), static_cast<qint64>(m_refreshUnhookError.load())}};
    static constexpr std::array<const char*, CounterCount> names {
        "non_modifier_presses", "routed_presses", "other_foreground", "window_hidden", "window_hung",
        "win_held", "already_held", "not_wanted", "global_hotkey", "other_hook", "refreshes", "refresh_failed"};
    for (std::size_t i = 0; i < names.size(); ++i)
        status.insert(QString::fromLatin1(names[i]), static_cast<qint64>(m_counts[i].load(std::memory_order_relaxed)));
    return status;
}

void KeyRouter::drain()
{
    m_draining = true;
    ::PostThreadMessageW(m_threadId.load(), kDrainMessage, 0, 0);
}

void KeyRouter::checkDrained()
{
    if (m_draining && m_taken.none() && m_callbacks.drained)
        m_callbacks.drained();
}

LRESULT CALLBACK KeyRouter::keyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && t_router)
        return t_router->handleKey(code, wParam, lParam);
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK KeyRouter::mouseProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && t_router)
        t_router->handleMouse(wParam, *reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam));
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

void CALLBACK KeyRouter::foregroundProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD)
{
    if (t_router && hwnd && !t_router->m_draining && t_router->m_callbacks.foregroundChanged)
        t_router->m_callbacks.foregroundChanged(::GetAncestor(hwnd, GA_ROOT));
}

KeyRouter::Availability KeyRouter::availability() const
{
    // Not while the window is gone or hung (its thread has not answered for
    // five seconds): the keys would be lost.
    const HWND front = ::GetForegroundWindow();
    if (!front || ::GetAncestor(front, GA_ROOT) != m_target)
        return Availability::OtherForeground;
    if (!::IsWindowVisible(m_window))
        return Availability::Hidden;
    return ::IsHungAppWindow(m_window) ? Availability::Hung : Availability::Ready;
}

LRESULT KeyRouter::handleKey(int code, WPARAM message, LPARAM lParam)
{
    const KBDLLHOOKSTRUCT& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    // The rest of the chain: the hooks installed before this one (the newest
    // is called first), then the program. One that takes the key answers
    // nonzero. A key it sends in its place (a remapper's) comes here at once,
    // inside this call, ahead of the one it replaces.
    const auto next = [&] { return ::CallNextHookEx(nullptr, code, message, lParam); };
    if (isOwnInput(event.dwExtraInfo))
        return next();
    const UINT vk = event.vkCode;
    // Any other key while a Shift is down: not a Shift pressed alone (see
    // handleModifier). Ours, the masking keys among them, went on above.
    if (modifierOf(vk) != Qt::ShiftModifier)
        m_lonelyShift = 0;
    if (vk >= m_taken.size())
        return next();
    const bool press = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    if (!m_draining && press && modifierOf(vk) == Qt::NoModifier)
        count(Presses);
    if (m_taken.test(vk) && !(press && m_draining)) {
        // The release and auto-repeat of a taken key are taken too, wherever
        // the focus went. The hooks after this one saw its press: they see
        // these as well (AutoHotkey keeps a key's state from them).
        next();
        if (!press) {
            m_taken.reset(vk);
            if (m_draining)
                checkDrained();
            else
                m_callbacks.key(translate(event, false, false));
            return 1;
        }
        const Key key = translate(event, true, true);
        count(Routed);
        if (key.modifiers & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier))
            sendMask();
        m_callbacks.key(key);
        return 1;
    }
    if (m_draining)
        return next();
    if (modifierOf(vk) != Qt::NoModifier) {
        handleModifier(press, event);
        return next(); // physical transitions must also reach remappers' hooks
    }
    if (!press)
        return next(); // its press went to the program: so does its release
    // Win combinations are Windows' (Win+V again closes the window). A key
    // down since before the window came (this is its auto-repeat) goes on to
    // the program up to its release.
    switch (availability()) {
    case Availability::OtherForeground:
        count(OtherForeground);
        return next();
    case Availability::Hidden:
        count(Hidden);
        return next();
    case Availability::Hung:
        count(Hung);
        return next();
    case Availability::Ready:
        break;
    }
    if (held(VK_LWIN) || held(VK_RWIN)) {
        count(WinHeld);
        return next();
    }
    if (held(static_cast<int>(vk))) {
        count(AlreadyHeld);
        return next();
    }
    const Key key = translate(event, true, false);
    if (!(m_policy == Policy::Menu ? wantedByMenu(key) : wanted(key, m_numberKeys))) {
        count(NotWanted);
        return next();
    }
    if (heldAsHotkey(key)) {
        count(GlobalHotkey);
        return next();
    }
    // A key the window wants goes to the hooks after this one first, as if
    // the window were an ordinary one in front: a remapper there (AutoHotkey,
    // PowerToys' Keyboard Manager) turns CapsLock+J into Down. It takes the
    // J (it answers nonzero), and its Down comes here as a key of its own.
    // A key it leaves alone is the window's.
    if (const LRESULT taken = next(); taken != 0) {
        count(OtherHook);
        return taken;
    }
    m_taken.set(vk);
    count(Routed);
    if (key.modifiers & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier))
        sendMask(); // in place of the key: the modifier was not pressed alone (a double Ctrl neither)
    m_callbacks.key(key);
    return 1; // the program does not see it
}

// A chord with Ctrl or Alt that another program holds as a global hotkey
// (RegisterHotKey refuses it; or one of Win顺's own) is that program's:
// PixPin's Ctrl+1 and up, with the clipboard's number keys set to Ctrl.
// Asked once per chord while the window is up: a few microseconds.
bool KeyRouter::heldAsHotkey(const Key& key)
{
    UINT modifiers = 0;
    if (key.modifiers.testFlag(Qt::ControlModifier))
        modifiers |= MOD_CONTROL;
    if (key.modifiers.testFlag(Qt::AltModifier))
        modifiers |= MOD_ALT;
    if (modifiers == 0)
        return false; // typing, moving: a hotkey on those would leave nothing to type with anyway
    if (key.modifiers.testFlag(Qt::ShiftModifier))
        modifiers |= MOD_SHIFT;
    const UINT chord = modifiers << 16 | key.vk;
    if (const auto it = m_hotkeys.find(chord); it != m_hotkeys.end())
        return it->second;
    constexpr int kProbeId = 0xBF01; // a thread's own hotkey ids go up to 0xBFFF
    bool held = false;
    if (::RegisterHotKey(nullptr, kProbeId, modifiers | MOD_NOREPEAT, key.vk))
        ::UnregisterHotKey(nullptr, kProbeId); // free: let go at once
    else
        held = ::GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED;
    m_hotkeys.emplace(chord, held);
    return held;
}

void KeyRouter::handleModifier(bool press, const KBDLLHOOKSTRUCT& event)
{
    const Qt::KeyboardModifier modifier = modifierOf(event.vkCode);
    // Alt held shows the rows' numbers. Windows counts the key as held only
    // once this hook is through: the modifiers are those before it.
    const Key key = translate(event, press, press && held(static_cast<int>(event.vkCode)));
    m_callbacks.key(key);
    // A Shift pressed alone: nothing else held as it goes down (the other
    // Shift neither, nor Win), no other key or click before it is let go.
    // Its own key repeats change nothing.
    bool tapped = false;
    if (modifier == Qt::ShiftModifier && !key.autoRepeat) {
        const bool alone = key.modifiers == Qt::NoModifier && !held(VK_LWIN) && !held(VK_RWIN);
        tapped = !press && m_lonelyShift == event.vkCode;
        m_lonelyShift = press && alone ? event.vkCode : 0;
    }
    // Mask after the press, while it is held. Replacing the original release
    // with SendInput left older hooks' physical Shift state pressed (#91).
    // Ctrl alone must stay a tap for the double-Ctrl shortcut.
    if (!taking())
        return;
    if (press && !key.autoRepeat && modifier != Qt::ControlModifier)
        sendMask(kOwnMask);
    if (tapped && m_policy == Policy::Clipboard && m_callbacks.shiftTapped)
        m_callbacks.shiftTapped();
}

void KeyRouter::handleMouse(WPARAM message, const MSLLHOOKSTRUCT& event)
{
    if (message != WM_MOUSEMOVE)
        m_lonelyShift = 0; // Shift+click, Shift+wheel, on Win顺's windows too: not a Shift alone
    if (m_draining)
        return;
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        break;
    default:
        return; // moves, above all: nothing to do
    }
    // Hit-testing windows of other threads sends them nothing (only this
    // thread's own would be asked, and it has none).
    const HWND hit = ::WindowFromPoint(event.pt);
    DWORD process = 0;
    if (hit)
        ::GetWindowThreadProcessId(hit, &process);
    if (process != m_process && m_callbacks.clickedAway)
        m_callbacks.clickedAway(); // the click goes on where it was meant to
}

KeyRouter::Key KeyRouter::translate(const KBDLLHOOKSTRUCT& event, bool press, bool autoRepeat) const
{
    const UINT vk = event.vkCode;
    Key key;
    key.vk = vk;
    key.scanCode = event.scanCode;
    key.press = press;
    key.autoRepeat = autoRepeat;
    key.modifiers = heldModifiers();
    const bool shift = key.modifiers.testFlag(Qt::ShiftModifier);
    const bool ctrl = key.modifiers.testFlag(Qt::ControlModifier);
    const bool alt = key.modifiers.testFlag(Qt::AltModifier);
    key.key = qtKey(vk, (event.flags & LLKHF_EXTENDED) != 0, shift);
    if (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE)
        key.modifiers |= Qt::KeypadModifier;
    // What it types, in the keyboard layout of the program in front (the
    // one the user is typing in), as Windows would make it: with Shift,
    // Caps Lock, AltGr. Not with Ctrl or Alt alone, which make shortcuts.
    const HKL layout = ::GetKeyboardLayout(::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr));
    if (vk == VK_PACKET) { // typed as a character (SendInput with KEYEVENTF_UNICODE: the on-screen keyboard, voice typing)
        key.text = QString(QChar(static_cast<char16_t>(event.scanCode)));
    } else if (press && ctrl == alt) {
        std::array<BYTE, 256> state {};
        for (const int down : {VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU,
                 VK_RMENU}) {
            if (held(down))
                state[static_cast<std::size_t>(down)] = 0x80;
        }
        state[VK_CAPITAL] = static_cast<BYTE>(::GetKeyState(VK_CAPITAL) & 1);
        state[VK_NUMLOCK] = static_cast<BYTE>(::GetKeyState(VK_NUMLOCK) & 1);
        std::array<wchar_t, 8> chars {};
        const int n = ::ToUnicodeEx(vk, event.scanCode, state.data(), chars.data(), static_cast<int>(chars.size()),
                                    kToUnicodeKeepState, layout);
        if (n > 0)
            key.text = QString::fromWCharArray(chars.data(), n);
    }
    if (key.key == 0) {
        // Named after its character, upper case, as Qt does: the typed one,
        // or the one the key has without modifiers (a dead key's too).
        const QChar c = !key.text.isEmpty() ? key.text.at(0)
                                            : QChar(static_cast<char16_t>(::MapVirtualKeyExW(vk, MAPVK_VK_TO_CHAR, layout)));
        key.key = c.isNull() ? int(Qt::Key_unknown) : int(c.toUpper().unicode());
    }
    return key;
}

} // namespace ws
