#include "Engine.h"

#include <imm.h>

#include <algorithm>
#include <optional>
#include <utility>

namespace expander {

namespace {

constexpr ULONG_PTR kOwnInput = 0x57534558; // "WSEX": keys this plugin sends
constexpr UINT kImeWaitMs = 50; // asking the input method, at most (the hook is waited on)
// WM_IME_CONTROL's questions (the DDK's ime.h, not in the SDK).
constexpr WPARAM kImcGetConversionMode = 0x0001;
constexpr WPARAM kImcGetOpenStatus = 0x0005;

Engine* g_engine = nullptr;

bool held(int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; }

std::optional<DWORD> levelOf(HANDLE token)
{
    alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[64];
    DWORD size = 0;
    if (!::GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof buffer, &size))
        return std::nullopt;
    const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
    const UCHAR count = *::GetSidSubAuthorityCount(sid);
    if (count == 0)
        return std::nullopt;
    return *::GetSidSubAuthority(sid, count - 1);
}

DWORD ownLevel()
{
    static const DWORD level = [] {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
            return DWORD(SECURITY_MANDATORY_MEDIUM_RID);
        const DWORD l = levelOf(token).value_or(DWORD(SECURITY_MANDATORY_MEDIUM_RID));
        ::CloseHandle(token);
        return l;
    }();
    return level;
}

// Whether the process runs above this one (one that cannot be asked counts
// as above): the keys sent there would be dropped.
bool aboveUs(HANDLE process)
{
    if (ownLevel() >= SECURITY_MANDATORY_HIGH_RID)
        return false;
    HANDLE token = nullptr;
    if (!process || !::OpenProcessToken(process, TOKEN_QUERY, &token))
        return true;
    const std::optional<DWORD> level = levelOf(token);
    ::CloseHandle(token);
    return !level || *level > ownLevel();
}

std::wstring programOf(HANDLE process)
{
    wchar_t path[MAX_PATH * 2] {};
    DWORD size = MAX_PATH * 2;
    if (!process || !::QueryFullProcessImageNameW(process, 0, path, &size))
        return {};
    return std::wstring(path, size);
}

// The input method of the window with the keyboard focus writes in its own
// language (中 rather than 英): the letters went to it, not into the text.
bool imeComposes(HWND focus)
{
    const HWND ime = ::ImmGetDefaultIMEWnd(focus);
    if (!ime)
        return false;
    DWORD_PTR open = 0;
    if (!::SendMessageTimeoutW(ime, WM_IME_CONTROL, kImcGetOpenStatus, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, kImeWaitMs, &open) || !open)
        return false;
    DWORD_PTR mode = 0;
    if (!::SendMessageTimeoutW(ime, WM_IME_CONTROL, kImcGetConversionMode, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, kImeWaitMs, &mode))
        return false;
    return (mode & IME_CMODE_NATIVE) != 0;
}

INPUT key(WORD vk, bool up, bool extended = false)
{
    INPUT in {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
    in.ki.dwExtraInfo = kOwnInput;
    return in;
}

INPUT unit(wchar_t c, bool up)
{
    INPUT in {};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = c;
    in.ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0);
    in.ki.dwExtraInfo = kOwnInput;
    return in;
}

void addBackspaces(std::vector<INPUT>& input, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        input.push_back(key(VK_BACK, false));
        input.push_back(key(VK_BACK, true));
    }
}

// The text as typed; a new line as Shift+Enter, which is one in a chat box
// too (Enter alone would send it there).
void addText(std::vector<INPUT>& input, std::wstring_view text)
{
    const bool shift = held(VK_SHIFT);
    for (const wchar_t c : text) {
        if (c == L'\n') {
            if (!shift)
                input.push_back(key(VK_SHIFT, false));
            input.push_back(key(VK_RETURN, false));
            input.push_back(key(VK_RETURN, true));
            if (!shift)
                input.push_back(key(VK_SHIFT, true));
        } else {
            input.push_back(unit(c, false));
            input.push_back(unit(c, true));
        }
    }
}

} // namespace

Engine::Engine(EngineOptions options)
    : m_options(std::move(options))
    , m_shared(std::make_shared<const Settings>())
{
}

Engine::~Engine()
{
    stop();
}

std::shared_ptr<const Settings> Engine::shared() const
{
    const std::lock_guard lock(m_mutex);
    return m_shared;
}

void Engine::configure(Settings settings)
{
    auto next = std::make_shared<const Settings>(std::move(settings));
    const std::lock_guard lock(m_mutex);
    m_shared = std::move(next);
}

void Engine::start()
{
    if (g_engine)
        return;
    g_engine = this;
    std::promise<bool> ready;
    auto started = ready.get_future();
    m_thread = std::thread([this, &ready] { hookThread(&ready); });
    if (!started.get())
        m_options.log("cannot hook the keyboard: error " + std::to_string(::GetLastError()));
}

void Engine::stop()
{
    if (m_thread.joinable()) {
        ::PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
        m_thread.join();
    }
    if (g_engine == this)
        g_engine = nullptr;
}

void Engine::hookThread(std::promise<bool>* ready)
{
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    m_threadId = ::GetCurrentThreadId();
    MSG msg;
    ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the queue, before anyone posts
    // The toggles as they are now; from here on every press is seen.
    m_capsLock = (::GetKeyState(VK_CAPITAL) & 1) != 0;
    m_numLock = (::GetKeyState(VK_NUMLOCK) & 1) != 0;
    const HMODULE module = ::GetModuleHandleW(nullptr);
    const HHOOK keyboard = ::SetWindowsHookExW(WH_KEYBOARD_LL, &Engine::keyboardProc, module, 0);
    const HHOOK mouse = keyboard ? ::SetWindowsHookExW(WH_MOUSE_LL, &Engine::mouseProc, module, 0) : nullptr;
    ready->set_value(keyboard && mouse);
    if (keyboard && mouse) {
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
            ::DispatchMessageW(&msg);
    }
    if (mouse)
        ::UnhookWindowsHookEx(mouse);
    if (keyboard)
        ::UnhookWindowsHookEx(keyboard);
}

LRESULT CALLBACK Engine::keyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && g_engine && g_engine->onKey(wParam, *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam)))
        return 1;
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK Engine::mouseProc(int code, WPARAM wParam, LPARAM lParam)
{
    // A press puts the caret somewhere else, as likely as not.
    if (code == HC_ACTION && g_engine
        && (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN || wParam == WM_XBUTTONDOWN))
        g_engine->forget();
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

void Engine::forget()
{
    m_typed.clear();
    m_undo.reset();
}

void Engine::push(std::wstring_view chars)
{
    m_typed.append(chars);
    if (m_typed.size() > 2 * kLongest)
        m_typed.erase(0, m_typed.size() - 2 * kLongest);
}

std::wstring Engine::translate(const KBDLLHOOKSTRUCT& k, bool& dead) const
{
    BYTE state[256] {};
    for (const int vk : {VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU}) {
        if (held(vk))
            state[vk] = 0x80;
    }
    if (m_capsLock)
        state[VK_CAPITAL] = 0x01;
    if (m_numLock)
        state[VK_NUMLOCK] = 0x01;
    const HKL layout = ::GetKeyboardLayout(m_foreground ? ::GetWindowThreadProcessId(m_foreground, nullptr) : 0);
    wchar_t out[8] {};
    // 0x4: the keyboard state left as it was (a dead key stays the program's).
    const int n = ::ToUnicodeEx(k.vkCode, k.scanCode, state, out, 8, 0x4, layout);
    dead = n < 0;
    return n > 0 ? std::wstring(out, static_cast<std::size_t>(n)) : std::wstring();
}

bool Engine::onKey(WPARAM message, const KBDLLHOOKSTRUCT& k)
{
    if (k.dwExtraInfo == kOwnInput)
        return false;
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const DWORD vk = k.vkCode;
    if (vk == VK_CAPITAL || vk == VK_NUMLOCK) { // toggled on a press, whoever made it (held: repeats)
        bool& on = vk == VK_CAPITAL ? m_capsLock : m_numLock;
        bool& pressed = vk == VK_CAPITAL ? m_capsHeld : m_numHeld;
        if (down && !pressed)
            on = !on;
        pressed = down;
        return false;
    }
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
        // Alt pressed and let go alone opens the menu bar, where letters choose.
        if (down)
            m_altAlone = true;
        else if (std::exchange(m_altAlone, false))
            forget();
        return false;
    }
    if (!down)
        return false;
    m_altAlone = false;
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL)
        return false; // nothing typed yet
    if (k.flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)) { // another program's typing, gone where it went
        forget();
        return false;
    }
    if (const HWND foreground = ::GetForegroundWindow(); foreground != m_foreground) {
        forget();
        m_afterDead = false;
        m_foreground = foreground;
    }
    if (m_paused) {
        forget();
        return false;
    }

    const bool ctrl = held(VK_CONTROL);
    const bool alt = held(VK_MENU);
    const bool win = held(VK_LWIN) || held(VK_RWIN) || vk == VK_LWIN || vk == VK_RWIN;
    const std::optional<Undo> undo = std::exchange(m_undo, std::nullopt);
    if (vk == VK_BACK) {
        if (ctrl || alt || win) {
            forget();
            return false;
        }
        if (undo)
            return takeBack(*undo);
        if (!m_typed.empty()) {
            const bool pair = m_typed.size() >= 2 && IS_LOW_SURROGATE(m_typed.back()) && IS_HIGH_SURROGATE(m_typed[m_typed.size() - 2]);
            m_typed.resize(m_typed.size() - (pair ? 2 : 1));
        }
        return false;
    }
    if (win || ctrl != alt) { // a shortcut (Ctrl+Alt together is AltGr: typing)
        forget();
        return false;
    }
    bool dead = false;
    const std::wstring chars = translate(k, dead);
    if (dead) {
        forget();
        m_afterDead = true;
        return false;
    }
    const bool composed = std::exchange(m_afterDead, false); // made with the dead key: unknown here
    const bool one = chars.size() == 1 || (chars.size() == 2 && IS_HIGH_SURROGATE(chars[0]) && IS_LOW_SURROGATE(chars[1]));
    const wchar_t c = chars.empty() ? L'\0' : chars.front();
    const bool lineKey = c == L'\r' || c == L'\t';
    if (composed || !one || (c < 0x20 && !lineKey) || c == 0x7f) { // an arrow, Esc, a function key…: the caret is elsewhere
        forget();
        return false;
    }

    const std::shared_ptr<const Settings> s = shared();
    if (s->instant) {
        if (lineKey) {
            m_typed.clear();
            return false;
        }
        push(chars);
        if (const std::optional<Match> m = find(s->phrases, m_typed))
            return expand(*m, nullptr, {});
        return false;
    }
    if (chars.size() == 1 && !isWordChar(c)) { // an abbreviation's end, if one was typed
        if (const std::optional<Match> m = find(s->phrases, m_typed); m && expand(*m, &k, chars))
            return true;
    }
    if (lineKey)
        m_typed.clear();
    else
        push(chars);
    return false;
}

bool Engine::eligible(HWND foreground, const Settings& s) const
{
    if (!foreground || held(VK_CONTROL) || held(VK_MENU) || held(VK_LWIN) || held(VK_RWIN))
        return false; // AltGr: what is sent would go with it
    DWORD pid = 0;
    const DWORD thread = ::GetWindowThreadProcessId(foreground, &pid);
    if (pid == 0 || pid == ::GetCurrentProcessId())
        return false;
    GUITHREADINFO gui {};
    gui.cbSize = sizeof gui;
    if (!::GetGUIThreadInfo(thread, &gui))
        gui = {};
    if (gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE))
        return false;
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    const bool above = aboveUs(process);
    const std::wstring program = above ? std::wstring() : programOf(process);
    if (process)
        ::CloseHandle(process);
    static const std::vector<std::wstring> own = {L"winshun.exe"};
    if (above || program.empty() || listed(own, program) || listed(s.excluded, program))
        return false;
    return !imeComposes(gui.hwndFocus ? gui.hwndFocus : foreground);
}

bool Engine::send(std::vector<INPUT>& input)
{
    if (input.empty())
        return true;
    const UINT sent = ::SendInput(static_cast<UINT>(input.size()), input.data(), sizeof(INPUT));
    if (sent == input.size())
        return true;
    m_options.log("SendInput sent " + std::to_string(sent) + " of " + std::to_string(input.size()) + ": error " + std::to_string(::GetLastError()));
    return sent > 0; // some went: the key held back must not go as well
}

bool Engine::expand(const Match& m, const KBDLLHOOKSTRUCT* end, std::wstring_view endChars)
{
    const std::shared_ptr<const Settings> s = shared();
    if (!eligible(m_foreground, *s))
        return false;
    const std::wstring abbreviation = m_typed.substr(m_typed.size() - m.typedLength);
    // With "instant" the abbreviation's last character is this key's, held back: not typed yet.
    const std::size_t typed = codePoints(abbreviation) - (end ? 0 : 1);
    std::vector<INPUT> input;
    addBackspaces(input, typed);
    addText(input, m.text);
    if (end) // the key that ended it, as pressed (its release goes through)
        input.push_back([&] {
            INPUT in {};
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = static_cast<WORD>(end->vkCode);
            in.ki.wScan = static_cast<WORD>(end->scanCode);
            in.ki.dwFlags = (end->flags & LLKHF_EXTENDED) ? KEYEVENTF_EXTENDEDKEY : 0;
            in.ki.dwExtraInfo = kOwnInput;
            return in;
        }());
    if (!send(input))
        return false;
    ++m_expanded;

    // Backspace next takes it back; not after Enter or Tab (they did
    // something more), nor a phrase of several lines (a program may indent them).
    const bool lineKey = !endChars.empty() && (endChars.front() == L'\r' || endChars.front() == L'\t');
    m_typed = lineKey ? std::wstring() : std::wstring(endChars);
    if (!lineKey && m.text.find(L'\n') == std::wstring::npos)
        m_undo = Undo {codePoints(m.text) + codePoints(endChars), abbreviation + std::wstring(endChars)};
    return true;
}

bool Engine::takeBack(const Undo& undo)
{
    std::vector<INPUT> input;
    addBackspaces(input, undo.erase);
    addText(input, undo.typed);
    m_typed.clear(); // not again at the next space: it was taken back
    return send(input);
}

} // namespace expander
