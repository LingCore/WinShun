#include "KeyRouter.h"

#include <array>
#include <latch>

namespace ws {

namespace {

// Low-level hooks and out-of-context events are called on the thread that set them.
thread_local KeyRouter* t_router = nullptr;

constexpr WORD kMaskKey = 0xE8; // unassigned: no program acts on it
constexpr UINT kDrainMessage = WM_APP; // to the router's thread (drain)
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

INPUT keyInput(WORD vk, WORD scanCode, DWORD flags)
{
    INPUT input {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = scanCode;
    input.ki.dwFlags = flags;
    input.ki.dwExtraInfo = kOwnInput;
    return input;
}

// A key the program in front sees between a modifier's press and release,
// so that it does not count the modifier as pressed alone.
void sendMask()
{
    std::array keys {keyInput(kMaskKey, 0, 0), keyInput(kMaskKey, 0, KEYEVENTF_KEYUP)};
    ::SendInput(static_cast<UINT>(keys.size()), keys.data(), sizeof(INPUT));
}

// The release of a modifier, after a masking key.
void sendMaskedRelease(const KBDLLHOOKSTRUCT& event)
{
    const DWORD extended = (event.flags & LLKHF_EXTENDED) ? KEYEVENTF_EXTENDEDKEY : 0;
    std::array keys {keyInput(kMaskKey, 0, 0), keyInput(kMaskKey, 0, KEYEVENTF_KEYUP),
        keyInput(static_cast<WORD>(event.vkCode), static_cast<WORD>(event.scanCode), KEYEVENTF_KEYUP | extended)};
    ::SendInput(static_cast<UINT>(keys.size()), keys.data(), sizeof(INPUT));
}

} // namespace

KeyRouter::KeyRouter(HWND target, HWND window, Callbacks callbacks, Qt::KeyboardModifier numberKeys)
    : m_target(target)
    , m_window(window)
    , m_numberKeys(numberKeys)
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
        const HHOOK keyboard = ::SetWindowsHookExW(WH_KEYBOARD_LL, &KeyRouter::keyboardProc, instance, 0);
        const HHOOK mouse = keyboard ? ::SetWindowsHookExW(WH_MOUSE_LL, &KeyRouter::mouseProc, instance, 0) : nullptr;
        const HWINEVENTHOOK foreground = mouse
            ? ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &KeyRouter::foregroundProc,
                                0, 0, WINEVENT_OUTOFCONTEXT)
            : nullptr;
        m_active = foreground != nullptr;
        ready.count_down();
        if (m_active) {
            // The hooks are called from inside GetMessage; the events too.
            while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (msg.hwnd == nullptr && msg.message == kDrainMessage) {
                    checkDrained(); // nothing held now: done at once
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
    if (code == HC_ACTION && t_router && t_router->handleKey(wParam, *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam)))
        return 1; // seen by nobody else
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

bool KeyRouter::taking() const
{
    // Not while the window is gone or hung (its thread has not answered for
    // five seconds): the keys would be lost.
    const HWND front = ::GetForegroundWindow();
    return front && ::GetAncestor(front, GA_ROOT) == m_target && ::IsWindowVisible(m_window)
        && !::IsHungAppWindow(m_window);
}

bool KeyRouter::handleKey(WPARAM message, const KBDLLHOOKSTRUCT& event)
{
    if (event.dwExtraInfo == kOwnInput)
        return false;
    const UINT vk = event.vkCode;
    if (vk >= m_taken.size())
        return false;
    const bool press = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    if (!press) {
        if (m_taken.test(vk)) {
            m_taken.reset(vk);
            if (m_draining)
                checkDrained();
            else
                m_callbacks.key(translate(event, false, false));
            return true;
        }
        // Its press went to the program: so does its release.
    }
    if (m_draining)
        return false;
    if (modifierOf(vk) != Qt::NoModifier)
        return handleModifier(press, event);
    if (!press)
        return false;
    const bool autoRepeat = m_taken.test(vk); // a taken key stays taken, wherever the focus went
    if (!autoRepeat) {
        // Win combinations are Windows' (Win+V again closes the window). A
        // key down since before the window came (this is its auto-repeat)
        // goes on to the program up to its release.
        if (!taking() || held(VK_LWIN) || held(VK_RWIN) || held(static_cast<int>(vk)))
            return false;
    }
    const Key key = translate(event, true, autoRepeat);
    if (!autoRepeat) {
        if (!wanted(key, m_numberKeys))
            return false;
        m_taken.set(vk);
    }
    if (key.modifiers & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier))
        sendMask(); // in place of the key: the modifier was not pressed alone (a double Ctrl neither)
    m_callbacks.key(key);
    return true;
}

bool KeyRouter::handleModifier(bool press, const KBDLLHOOKSTRUCT& event)
{
    const Qt::KeyboardModifier modifier = modifierOf(event.vkCode);
    // Alt held shows the rows' numbers. Windows counts the key as held only
    // once this hook is through: the modifiers are those before it.
    m_callbacks.key(translate(event, press, press && held(static_cast<int>(event.vkCode))));
    // Shift or Alt let go with nothing between for the program (taken keys
    // are not, a click is not): it would switch the input method or open
    // the menu bar. Ctrl alone does neither, and must stay a tap for the
    // double Ctrl.
    if (press || modifier == Qt::ControlModifier || !taking())
        return false;
    sendMaskedRelease(event);
    return true;
}

void KeyRouter::handleMouse(WPARAM message, const MSLLHOOKSTRUCT& event)
{
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
