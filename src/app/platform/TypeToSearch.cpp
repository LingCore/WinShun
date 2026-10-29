#include "TypeToSearch.h"

#include "FolderViewHit.h"
#include "Integrity.h"
#include "KeyListener.h" // KeyboardRawInputAside
#include "KeyRouter.h" // kOwnInput
#include "PasteOffer.h"
#include "SearchTyping.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <imm.h> // IMN_ (WIN32_LEAN_AND_MEAN leaves it out of windows.h)
#include <msctf.h>
#include <wrl/client.h>

#include <QCoreApplication>
#include <QtLogging>

#include <algorithm>
#include <cwchar>
#include <format>
#include <latch>
#include <utility>

namespace ws {

namespace {

// Low-level hooks and out-of-context events are called on the thread that set them.
thread_local TypeToSearch* t_typing = nullptr;
std::atomic<TypeToSearch::RequestId> g_nextFolderRequest = 0;

constexpr UINT kDeliverMessage = WM_APP + 1; // to the hook's thread (deliver)
constexpr UINT kGiveBackMessage = WM_APP + 2; // to the hook's thread (giveBack)
constexpr UINT kPassedMessage = WM_APP + 3; // to the hook's thread: the keys typed again have gone by
constexpr UINT kLookAgainMessage = WM_APP + 4; // to the hook's thread: the front changed while keys wait for it
// Keys for the launcher wait while Win顺 is not in front (GitHub #66): how often
// to look again, besides at each foreground event.
constexpr UINT kFrontPollMs = 20;

bool held(int vk)
{
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}

// The Start menu, or Windows' search, which takes it over when typed in.
// Windows 11 has them in SearchHost.exe (the Start menu in front, on 25H2)
// and StartMenuExperienceHost.exe, Windows 10 in SearchApp.exe or SearchUI.exe.
bool isStartMenu(HWND window)
{
    DWORD pid = 0;
    if (!window || !::GetWindowThreadProcessId(window, &pid) || pid == 0)
        return false;
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    wchar_t path[MAX_PATH] {};
    DWORD size = MAX_PATH;
    const bool named = ::QueryFullProcessImageNameW(process, 0, path, &size);
    ::CloseHandle(process);
    if (!named)
        return false;
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    const wchar_t* file = slash ? slash + 1 : path;
    for (const wchar_t* host : {L"SearchHost.exe", L"StartMenuExperienceHost.exe", L"SearchApp.exe", L"SearchUI.exe"}) {
        if (_wcsicmp(file, host) == 0)
            return true;
    }
    return false;
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

// One stroke of searchtyping::retype as input to send.
INPUT inputOf(const searchtyping::Stroke& stroke)
{
    const DWORD up = stroke.release ? KEYEVENTF_KEYUP : 0;
    if (stroke.vk == searchtyping::kVkPacket) // a program's character: as it came, not as a key with that code
        return keyInput(0, static_cast<WORD>(stroke.scanCode), KEYEVENTF_UNICODE | up);
    if (stroke.vk == kMaskKey)
        return keyInput(kMaskKey, 0, up, kOwnMask);
    const WORD scanCode = stroke.scanCode ? static_cast<WORD>(stroke.scanCode)
                                          : static_cast<WORD>(::MapVirtualKeyW(stroke.vk, MAPVK_VK_TO_VSC));
    return keyInput(static_cast<WORD>(stroke.vk), scanCode, (stroke.extended ? KEYEVENTF_EXTENDEDKEY : 0) | up);
}

// KeyRouter's mask, to the window in front (see TypeToSearch::handleKey).
void sendMask()
{
    INPUT keys[] {keyInput(kMaskKey, 0, 0, kOwnMask), keyInput(kMaskKey, 0, KEYEVENTF_KEYUP, kOwnMask)};
    ::SendInput(static_cast<UINT>(std::size(keys)), keys, sizeof(INPUT));
}

// The strokes of a Step, typed as Win顺's own: how many went in.
std::size_t sendStrokes(const std::vector<searchtyping::Stroke>& strokes)
{
    std::vector<INPUT> inputs;
    inputs.reserve(strokes.size());
    for (const searchtyping::Stroke& stroke : strokes)
        inputs.push_back(inputOf(stroke));
    const UINT inserted = ::SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    if (inserted < inputs.size())
        qWarning("TypeToSearch: %u of %zu keys typed again went in (error %lu)", inserted, inputs.size(), ::GetLastError());
    return inserted;
}

searchtyping::Hold::KeyEvent keyEvent(WPARAM message, const KBDLLHOOKSTRUCT& event)
{
    return {.vk = event.vkCode,
        .scanCode = event.scanCode,
        .extended = (event.flags & LLKHF_EXTENDED) != 0,
        .press = message == WM_KEYDOWN || message == WM_SYSKEYDOWN,
        .own = isOwnInput(event.dwExtraInfo)};
}

// What a key types with AltGr held (searchtyping::Hold asks only then), on
// the keyboard layout of the window in front: Shift as the user holds it, Caps
// Lock as it will be once the presses of it held before are typed again.
// Empty for none, a dead key (left pending as it is: flag 4, Windows 10 1607
// on) or a control character: the key goes on then.
std::u16string altGrChars(const searchtyping::Hold::KeyEvent& key, searchtyping::Shifts shifts, bool capsLockFlipped)
{
    if (key.vk == searchtyping::kVkPacket)
        return std::u16string(1, static_cast<char16_t>(key.scanCode)); // a program's character, as it came
    BYTE state[256] {};
    state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_MENU] = state[VK_RMENU] = 0x80;
    if (shifts.left)
        state[VK_LSHIFT] = 0x80;
    if (shifts.right)
        state[VK_RSHIFT] = 0x80;
    if (shifts.any())
        state[VK_SHIFT] = 0x80;
    if (((::GetKeyState(VK_CAPITAL) & 1) != 0) != capsLockFlipped)
        state[VK_CAPITAL] = 0x01;
    const HKL layout = ::GetKeyboardLayout(::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr));
    wchar_t chars[8] {};
    const int count = ::ToUnicodeEx(key.vk, key.scanCode & 0xFF, state, chars, static_cast<int>(std::size(chars)), 0x4, layout);
    if (count <= 0)
        return {};
    std::u16string text(chars, chars + count);
    if (std::ranges::any_of(text, [](char16_t c) { return c < 0x20 || c == 0x7F; }))
        return {};
    return text;
}

// This thread's lines, KeyboardRawInputAside's among them: info ones (the
// set-aside itself, every hold) with WINSHUN_LOG_INFO (main.cpp's logToFile
// drops info lines without it), what Windows refused always. Written on the
// GUI thread: they come from inside the hook, which every key press in the
// system waits for, and the log is a file. With no application (a test), here.
void logLine(bool warning, const std::string& line)
{
    static const bool info = qEnvironmentVariableIsSet("WINSHUN_LOG_INFO");
    if (!warning && !info)
        return;
    auto write = [warning, line] {
        if (warning)
            qWarning("TypeToSearch: %s", line.c_str());
        else
            qInfo("TypeToSearch: %s", line.c_str());
    };
    if (QCoreApplication* app = QCoreApplication::instance())
        QMetaObject::invokeMethod(app, std::move(write), Qt::QueuedConnection);
    else
        write();
}

} // namespace

TypeToSearch::TypeToSearch(Surfaces surfaces, Callbacks callbacks)
    : m_surfaces(surfaces)
    , m_callbacks(std::move(callbacks))
{
    std::latch ready(1);
    m_thread = std::jthread([this, &ready] {
        t_typing = this;
        MSG msg;
        ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the thread's message queue, for WM_QUIT
        m_threadId = ::GetCurrentThreadId();
        // While the hook is there, every key press in the system waits for
        // it: at once, and not slowed down when the system saves power
        // (Windows drops a hook that keeps it waiting too long).
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        THREAD_POWER_THROTTLING_STATE throttling {THREAD_POWER_THROTTLING_CURRENT_VERSION,
            THREAD_POWER_THROTTLING_EXECUTION_SPEED, 0};
        ::SetThreadInformation(::GetCurrentThread(), ThreadPowerThrottling, &throttling, sizeof throttling);
        const HWINEVENTHOOK foreground = ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
            &TypeToSearch::foregroundProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        ready.count_down();
        // Open already. A folder window that opened behind another before
        // this started raised its event unseen: kept as one seen behind is,
        // foregroundChanged keeping it while it is still the active window
        // of its own thread.
        const HWND inFront = ::GetForegroundWindow();
        m_explorerBehind = openedBehind(inFront);
        foregroundChanged(inFront, nullptr);
        if (m_explorerBehind)
            logLine(false, std::format("folder window {:#x} opened behind another before the start: kept",
                reinterpret_cast<std::uintptr_t>(m_explorerBehind)));
        // The hook and the events are called from inside GetMessage.
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.hwnd == nullptr && msg.message == kDeliverMessage)
                apply(m_hold.deliver(msg.wParam != 0, front(), ::GetTickCount()));
            else if (msg.hwnd == nullptr && msg.message == kGiveBackMessage) {
                if (!msg.wParam || pending(static_cast<RequestId>(msg.wParam)))
                    apply(m_hold.giveBack(front(), ::GetTickCount()));
            } else if (msg.hwnd == nullptr && msg.message == kPassedMessage)
                apply(m_hold.passed(front(), ::GetTickCount()));
            else if (msg.hwnd == nullptr
                && ((msg.message == WM_TIMER && m_frontTimer && msg.wParam == m_frontTimer)
                    || (msg.message == kLookAgainMessage && m_frontTimer)))
                apply(m_hold.lookAgain(front(), ::GetTickCount())); // whether Win顺 is in front now
            else if (msg.hwnd == nullptr && msg.message == WM_TIMER && m_timer && msg.wParam == m_timer)
                apply(m_hold.timeout(front(), ::GetTickCount()));
            else
                ::DispatchMessageW(&msg);
        }
        // Ended while holding: Shift as the user holds it, typed once the hook
        // is gone (no one would answer it now, and nothing is waited for),
        // the Raw Input put back after both.
        const searchtyping::Hold::Step last = m_hold.stop();
        setHooked(false);
        if (!last.send.empty())
            sendStrokes(last.send);
        m_rawInputAside.reset();
        if (foreground)
            ::UnhookWinEvent(foreground);
    });
    ready.wait();
}

TypeToSearch::~TypeToSearch()
{
    ::PostThreadMessageW(m_threadId.load(), WM_QUIT, 0, 0); // ends the message loop
    if (m_thread.joinable())
        m_thread.join();
}

void TypeToSearch::deliver(bool ready)
{
    ::PostThreadMessageW(m_threadId.load(), kDeliverMessage, ready ? 1 : 0, 0);
}

void TypeToSearch::giveBack(RequestId request)
{
    ::PostThreadMessageW(m_threadId.load(), kGiveBackMessage, static_cast<WPARAM>(request), 0);
}

TypeToSearch::Timing TypeToSearch::timing() const noexcept
{
    using Clock = std::chrono::steady_clock;
    const auto at = [](Clock::rep ticks) { return ticks ? Clock::time_point(Clock::duration(ticks)) : Clock::time_point {}; };
    return {at(m_firstKeyAt.load()), at(m_sentAt.load()), m_sentPresses.load()};
}

LRESULT CALLBACK TypeToSearch::keyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code != HC_ACTION || !t_typing)
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    const KBDLLHOOKSTRUCT event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    if (t_typing->handleKey(wParam, event))
        return 1; // seen by nobody else
    const LRESULT result = ::CallNextHookEx(nullptr, code, wParam, lParam);
    // What went on is counted after the rest of the chain, not before: a hook
    // later in it (AutoHotkey, PowerToys, PixPin) that sends keys sends them
    // from inside that call, and they come here (this hook called again) and
    // get to the window ahead of this one; a hook that takes the key (returns
    // nonzero) keeps it from the window. Counted before, Shift as the window
    // has it came out wrong, and so did the order the keys typed again went by
    // in. Once through, a key is on its way to the window, ahead of any that
    // comes here after it.
    t_typing->passedOn(wParam, event, result == 0);
    return result;
}

void CALLBACK TypeToSearch::foregroundProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD)
{
    // The window really in front now, not the one the event names: that
    // can be a part of it, or already gone. That one counts too when it is
    // an Explorer window left behind (foregroundChanged).
    if (t_typing)
        t_typing->foregroundChanged(::GetForegroundWindow(), hwnd);
}

// An Explorer folder window typing or pasting can count in: told by its
// classes alone; whether it is elevated is asked here, once, not at every key.
bool TypeToSearch::watchedExplorer(HWND window) const
{
    return (m_surfaces.explorer || m_surfaces.pasteFile) && window
        && folderview::classifyWindow(window).surface == folderview::Surface::Explorer
        && !integrity::higherThanOurs(window);
}

// Still the active window of its thread while not in front: brought forward
// now, it comes with no foreground event. No message sent.
bool TypeToSearch::activeBehind(HWND window)
{
    GUITHREADINFO info {sizeof info};
    const DWORD thread = ::GetWindowThreadProcessId(window, nullptr);
    return thread && ::GetGUIThreadInfo(thread, &info) && info.hwndActive == window;
}

// The folder window to keep apart as the hook starts (searchtyping::openedBehind):
// the top-level windows looked at by their classes and their threads' state
// alone, as at the events, with no message sent to Explorer. EnumWindows lists
// them top down and calls back on this thread, from inside the call.
HWND TypeToSearch::openedBehind(HWND front) const
{
    struct Search {
        const TypeToSearch* typing;
        std::vector<searchtyping::OpenFolder<HWND>> folders;
    } search {this, {}};
    ::EnumWindows(
        [](HWND window, LPARAM data) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(data);
            if (search.typing->watchedExplorer(window))
                search.folders.push_back({window, activeBehind(window)});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return searchtyping::openedBehind(front, search.folders);
}

void TypeToSearch::foregroundChanged(HWND window, HWND named)
{
    m_startInFront = m_surfaces.startMenu && isStartMenu(window);
    if (!m_startInFront)
        m_windowsSearch = false; // Left Alt counts for one opening
    // Explorer raises this event when it goes to another folder too
    // (docs/pitfalls.md), so a window that was something else (Home) and now
    // shows a folder is seen as often as needed; which folder is asked only
    // when typed in. A window that raises it while another stays in front
    // (opened by a program that may not take the front) is kept apart: it
    // comes to the front later, when clicked, with no event (measured), so
    // the hook stays and the key looks at what is in front then
    // (searchtyping::typingWindow). It stays kept through later events
    // (the launcher up and away, another program) while it is still the
    // active window of its own thread: that is what makes its coming to
    // the front silent, and it ends when it really comes forward or closes.
    const HWND behind = m_explorerBehind;
    m_explorer = nullptr;
    m_explorerBehind = nullptr;
    m_opus = nullptr;
    m_desktop = nullptr;
    m_pasteTaken = false; // V's release goes where the front went
    if (!m_startInFront) {
        if (m_surfaces.opus && folderview::isOpusLister(window) && !integrity::higherThanOurs(window))
            m_opus = window;
        if (watchedExplorer(window))
            m_explorer = window;
        if (named != window && watchedExplorer(named))
            m_explorerBehind = named; // behind another window, Explorer's own too
        else if (behind && behind != window && activeBehind(behind))
            m_explorerBehind = behind;
        if (m_surfaces.pasteFile && folderview::classifyWindow(window).surface == folderview::Surface::Desktop
            && !integrity::higherThanOurs(window))
            m_desktop = window;
    }
    setHooked(wantsHook());
    // Keys waiting for Win顺 to come to the front: maybe now. Looked at from
    // the loop, as the timer's are, not from here: the event can come while
    // the hook waits in CallNextHookEx (as the keys a later hook sends do),
    // in the middle of a key the hold has not counted yet (went()), and a
    // batch typed again then started from Shift as it was before that key.
    if (m_frontTimer)
        ::PostThreadMessageW(m_threadId.load(), kLookAgainMessage, 0, 0);
}

bool TypeToSearch::wantsHook() const
{
    return m_startInFront || m_explorer || m_explorerBehind || m_opus || m_desktop || m_hold.holding();
}

void TypeToSearch::setHooked(bool hooked)
{
    if (hooked == (m_hook != nullptr))
        return;
    if (hooked) {
        m_hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, &TypeToSearch::keyboardProc, ::GetModuleHandleW(nullptr), 0);
    } else {
        ::UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
    }
}

// Whether the files and folders shown in m_explorer have the keyboard focus
// (not a name being renamed, the address bar, the search box, the navigation
// pane), with no menu of the window's open over them (GitHub #37), and which
// tab they are. GetGUIThreadInfo reads the other thread's state without
// sending it anything: safe inside the hook, microseconds.
bool TypeToSearch::explorerFocused(HWND& tab) const
{
    GUITHREADINFO info {sizeof info};
    if (!::GetGUIThreadInfo(::GetWindowThreadProcessId(m_explorer, nullptr), &info) || !info.hwndFocus)
        return false;
    const folderview::Hit hit = folderview::classify(info.hwndFocus, {});
    const bool itemsView = hit.surface == folderview::Surface::Explorer && hit.root == m_explorer;
    if (searchtyping::focusOf(static_cast<unsigned>(info.flags), itemsView) != searchtyping::Focus::ItemsView)
        return false;
    tab = hit.tab;
    return true;
}

// Only Opus's file display itself, not its child editors or an open menu.
bool TypeToSearch::opusFocused(HWND& tab) const
{
    GUITHREADINFO info {sizeof info};
    if (!m_opus || ::GetForegroundWindow() != m_opus
        || !::GetGUIThreadInfo(::GetWindowThreadProcessId(m_opus, nullptr), &info))
        return false;
    const bool items = folderview::isOpusFileDisplay(m_opus, info.hwndFocus);
    if (searchtyping::focusOf(static_cast<unsigned>(info.flags), items) != searchtyping::Focus::ItemsView)
        return false;
    tab = info.hwndFocus;
    return true;
}

bool TypeToSearch::sourceFocused() const
{
    GUITHREADINFO info {sizeof info};
    return m_sourceWindow && ::GetForegroundWindow() == m_sourceWindow
        && ::GetGUIThreadInfo(::GetWindowThreadProcessId(m_sourceWindow, nullptr), &info)
        && info.hwndFocus == m_sourceFocus
        && searchtyping::focusOf(static_cast<unsigned>(info.flags), true) == searchtyping::Focus::ItemsView;
}

bool TypeToSearch::desktopFocused() const
{
    if (!m_desktop || ::GetForegroundWindow() != m_desktop)
        return false;
    GUITHREADINFO info {sizeof info};
    if (!::GetGUIThreadInfo(::GetWindowThreadProcessId(m_desktop, nullptr), &info) || !info.hwndFocus)
        return false;
    const folderview::Hit hit = folderview::classify(info.hwndFocus, {});
    const bool icons = hit.surface == folderview::Surface::Desktop && hit.root == m_desktop;
    return searchtyping::focusOf(static_cast<unsigned>(info.flags), icons) == searchtyping::Focus::ItemsView;
}

// Whichever Explorer window seen is really in front, made m_explorer (the
// one left behind may have come forward with no event).
bool TypeToSearch::explorerInFront()
{
    if (!m_explorer && !m_explorerBehind)
        return false;
    const HWND front = searchtyping::typingWindow(::GetForegroundWindow(), m_explorer, m_explorerBehind);
    if (!front)
        return false;
    if (front != m_explorer)
        std::swap(m_explorer, m_explorerBehind); // the one left behind is in front now, the other may come back
    return true;
}

// Ctrl+V on the files of an Explorer folder or the desktop's icons, with words
// or a picture on the clipboard and no files: made a file there by the stub.
// The clipboard is asked by its formats alone (no program is asked), only once
// the focus is right.
bool TypeToSearch::handlePaste(bool press)
{
    if (!press) {
        const bool taken = std::exchange(m_pasteTaken, false);
        return taken; // V's release, after its press was taken
    }
    if (m_pasteTaken)
        return true; // held down: one file for one press
    HWND view = nullptr;
    HWND tab = nullptr;
    if (explorerInFront() && explorerFocused(tab))
        view = tab;
    else if (desktopFocused())
        view = m_desktop;
    if (!view || !pastefile::takesCtrlV(pastefile::clipboardOffer()))
        return false;
    m_pasteTaken = true;
    if (m_callbacks.pastedInto)
        m_callbacks.pastedInto(view);
    return true;
}

bool TypeToSearch::handleKey(WPARAM message, const KBDLLHOOKSTRUCT& event)
{
    const UINT vk = event.vkCode;
    const bool press = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    // Ours too: the clipboard panel's paste, an entry picked with a folder in
    // front, is a paste into it all the same.
    if (vk == 'V' && m_surfaces.pasteFile && !m_hold.holding() && !m_startInFront
        && (m_pasteTaken
            || (press && held(VK_CONTROL) && !held(VK_SHIFT) && !held(VK_MENU) && !held(VK_LWIN) && !held(VK_RWIN)))
        && handlePaste(press))
        return true;
    // Ours: being typed again, given back, the Esc that closes the Start
    // menu for the launcher (win::bringToFront). Other programs' are typing
    // all the same (keys remapped by AutoHotkey or PowerToys, the on-screen
    // keyboard).
    if (isOwnInput(event.dwExtraInfo))
        return false;
    if (!m_hold.holding() && !startsHold(vk, press))
        return false;
    switch (m_hold.key(keyEvent(message, event), &altGrChars)) {
    case searchtyping::Hold::Take::No:
        return false;
    case searchtyping::Hold::Take::YesMask:
        // The window in front saw Shift go down, and with the key held back
        // it would see it come up alone: a tap, which input methods take for
        // their switch between Chinese and English ("Desktop" with Sogou or
        // Microsoft Pinyin in Chinese: the mode switched for the user, GitHub
        // #103). The mask goes to it now; the key keeps its Shift for when it
        // is typed again (searchtyping::retype).
        sendMask();
        return true;
    case searchtyping::Hold::Take::Yes:
        return true;
    }
    return false;
}

// No hold on: whether the key starts one (begun then; handleKey takes the key).
bool TypeToSearch::startsHold(UINT vk, bool press)
{
    if (searchtyping::isModifier(vk)) {
        if (press && vk == VK_LMENU && m_startInFront)
            m_windowsSearch = true;
        return false;
    }
    const bool combination = held(VK_CONTROL) || held(VK_MENU) || held(VK_LWIN) || held(VK_RWIN);
    if (!press || combination)
        return false; // AltGr too: its left Ctrl is down
    HWND tab = nullptr;
    HWND source = nullptr;
    bool opus = false;
    if (m_startInFront) {
        if (m_windowsSearch || !searchtyping::startsTyping(vk))
            return false;
    } else if (m_opus && ::GetForegroundWindow() == m_opus) {
        if (!searchtyping::opusStarts(vk, press, combination, held(VK_SHIFT), searchtyping::Focus::ItemsView, false)
            || !opusFocused(tab))
            return false;
        source = m_opus;
        opus = true;
    } else if (m_surfaces.explorer && (m_explorer || m_explorerBehind)) {
        // The key alone first; what is in front and what has the focus
        // are asked only for one that would start a search.
        if (!searchtyping::explorerStarts(vk, press, combination, searchtyping::Focus::ItemsView, false))
            return false;
        if (!explorerInFront() || !explorerFocused(tab))
            return false;
        source = m_explorer;
    } else {
        return false;
    }
    // Until the hold ends the hook must see every key: also those typed
    // into the launcher once it is in front, and its own keys typed again
    // as they go by. While the process takes the keyboard's Raw Input (the
    // double Ctrl, the drop pie: on by default), Windows does not call the
    // hook while a window of Win顺's is in front, so keys typed then went
    // straight in, ahead of those still held ("baidu" came out "dbaiu":
    // the launcher came to the front between "i" and "d", GitHub #66;
    // docs/pitfalls.md). Set aside now, before the launcher is asked for.
    if (!m_rawInputAside)
        m_rawInputAside.emplace(&logLine);
    // From here on Shift is the one the hook sees go by, not
    // GetAsyncKeyState's: that one also has the Shift typed again with
    // the keys held while it is on its way, and a key typed just then was
    // taken for a capital ("Desktop" came out "DEsktop"). Nothing of
    // Win顺's is on its way now.
    apply(m_hold.begin(source != nullptr, {.left = held(VK_LSHIFT), .right = held(VK_RSHIFT)}, ::GetTickCount()));
    m_firstKeyAt = std::chrono::steady_clock::now().time_since_epoch().count();
    m_sentAt = 0;
    m_sentPresses = 0;
    m_sourceWindow = source;
    GUITHREADINFO focus {sizeof focus};
    m_sourceFocus = source && ::GetGUIThreadInfo(::GetWindowThreadProcessId(source, nullptr), &focus)
        ? focus.hwndFocus
        : nullptr;
    if (source) {
        const RequestId request = ++g_nextFolderRequest;
        m_pendingRequest = request;
        const auto& callback = opus ? m_callbacks.typedInOpus : m_callbacks.typedInExplorer;
        if (callback)
            callback(source, tab, vk, request);
    } else if (m_callbacks.typed) {
        m_callbacks.typed();
    }
    return true;
}

// After the rest of the chain (keyboardProc): one of Win顺's own keys typed
// again went by, the last of them letting the keys typed meanwhile follow,
// once out of the hook (a posted message comes after the hooks' calls already
// waiting); and a key that got to the window counts as the window has it.
void TypeToSearch::passedOn(WPARAM message, const KBDLLHOOKSTRUCT& event, bool delivered)
{
    if (!m_hold.holding())
        return;
    const searchtyping::Hold::KeyEvent key = keyEvent(message, event);
    if (key.own && m_hold.ownPassed(key.vk, !key.press))
        ::PostThreadMessageW(m_threadId.load(), kPassedMessage, 0, 0);
    if (delivered)
        m_hold.went(key);
}

// What is in front, for the hold. Qt can call the launcher active while the
// Start menu still is in front (its Esc came while it was still opening),
// and the Start menu can take the front back a few milliseconds after the
// launcher got it (Windows slow to start it): typed then, the keys went into
// Windows' search (GitHub #66). So the keys for the launcher wait for a window
// of Win顺's, looked for at each foreground event and every kFrontPollMs, and
// with the Start menu in front again the launcher is asked for once more
// (searchtyping::delivery), until the hold runs out.
searchtyping::Hold::Front TypeToSearch::front() const
{
    const HWND window = ::GetForegroundWindow();
    DWORD pid = 0;
    if (window)
        ::GetWindowThreadProcessId(window, &pid);
    return {.ours = window && pid == ::GetCurrentProcessId(),
        .startMenu = isStartMenu(window),
        .sourceFocused = !m_hold.forFolder() || sourceFocused()};
}

// Does what the hold says: the keys typed again (in order, each with Shift as
// it was pressed, as Win顺's own; the hold goes on until the last of them
// has gone down the hook chain, so a key typed or let go meanwhile follows
// them: let through at once, it went in among them, q down, z down, z up,
// q up, and "qz" came out "zq", GitHub #39), the timer, asking for the
// launcher again, looking at the front again shortly; and the end.
void TypeToSearch::apply(searchtyping::Hold::Step step)
{
    if (step.askAgain && m_callbacks.typed)
        m_callbacks.typed(); // over the taskbar again; its Esc closes the Start menu
    if (step.timerMs)
        restartTimer(*step.timerMs);
    if (step.lookAgain) {
        if (!m_frontTimer)
            m_frontTimer = ::SetTimer(nullptr, 0, kFrontPollMs, nullptr);
    } else if (!step.send.empty() || step.ended) {
        ::KillTimer(nullptr, m_frontTimer);
        m_frontTimer = 0;
    }
    if (!step.send.empty()) {
        if (step.intoLauncher && m_sentAt.load() == 0) { // the first into the launcher in this hold (timing)
            m_sentPresses = static_cast<int>(std::ranges::count_if(step.send, [](const searchtyping::Stroke& stroke) {
                return !stroke.release && !searchtyping::isShift(stroke.vk) && stroke.vk != kMaskKey;
            }));
            m_sentAt = std::chrono::steady_clock::now().time_since_epoch().count();
        }
        // Not all went in (blocked: the secure desktop, a window of higher
        // integrity in front): what went down among those is let go, and
        // Shift set right, tried once (searchtyping::Hold::sent).
        if (const std::size_t inserted = sendStrokes(step.send); inserted < step.send.size())
            apply(m_hold.sent(inserted));
    }
    if (step.ended)
        endHolding();
}

void TypeToSearch::restartTimer(UINT ms)
{
    ::KillTimer(nullptr, m_timer);
    m_timer = ::SetTimer(nullptr, 0, ms, nullptr);
}

void TypeToSearch::endHolding()
{
    m_pendingRequest = 0;
    m_sourceWindow = nullptr;
    m_sourceFocus = nullptr;
    ::KillTimer(nullptr, m_timer);
    m_timer = 0;
    ::KillTimer(nullptr, m_frontTimer);
    m_frontTimer = 0;
    setHooked(wantsHook()); // the launcher is in front: gone
    m_rawInputAside.reset(); // after the hook went, if it did: both with the launcher in front blind every hook
}

namespace {

constexpr UINT_PTR kKeysWatchId = 0x5748; // "WH": LauncherKeysWatch (shellcontext's is "WK")

} // namespace

void LauncherKeysWatch::start(HWND window)
{
    stop();
    m_changed = false;
    m_inFrame = false;
    m_drawn = 0;
    if (window && ::SetWindowSubclass(window, &LauncherKeysWatch::proc, kKeysWatchId, reinterpret_cast<DWORD_PTR>(this)))
        m_window = window;
}

LauncherKeysWatch::Seen LauncherKeysWatch::stop()
{
    if (m_window) {
        ::RemoveWindowSubclass(m_window, &LauncherKeysWatch::proc, kKeysWatchId);
        m_window = nullptr;
        m_changed = false;
        m_inFrame = false;
        if (const Clock::rep drawn = m_drawn.exchange(0))
            m_seen.drawn = Clock::time_point(Clock::duration(drawn));
    }
    return std::exchange(m_seen, {});
}

void LauncherKeysWatch::fieldGot(bool text)
{
    if (!m_window)
        return;
    Clock::time_point& at = text ? m_seen.text : m_seen.preedit;
    if (at != Clock::time_point {})
        return;
    at = Clock::now();
    changed();
}

void LauncherKeysWatch::changed()
{
    if (m_drawn.load() == 0)
        m_changed = true;
}

void LauncherKeysWatch::synchronized() noexcept
{
    if (m_changed.exchange(false))
        m_inFrame = true;
}

void LauncherKeysWatch::frameSwapped() noexcept
{
    if (m_inFrame.exchange(false) && m_drawn.load() == 0)
        m_drawn = Clock::now().time_since_epoch().count();
}

LRESULT CALLBACK LauncherKeysWatch::proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
{
    auto& watch = *reinterpret_cast<LauncherKeysWatch*>(data);
    Seen& seen = watch.m_seen;
    const auto first = [](Clock::time_point& at) {
        if (at != Clock::time_point {})
            return false;
        at = Clock::now();
        return true;
    };
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (seen.keys++ == 0) {
            seen.key = Clock::now();
            seen.imeTook = wParam == VK_PROCESSKEY; // only that: never which key
        }
        break;
    case WM_IME_STARTCOMPOSITION:
        first(seen.composition);
        break;
    case WM_IME_NOTIFY:
        if (wParam == IMN_OPENCANDIDATE || wParam == IMN_CHANGECANDIDATE)
            first(seen.candidates);
        break;
    case WM_NCDESTROY: // a subclass must go before its window (never while watching, normally)
        ::RemoveWindowSubclass(hwnd, &LauncherKeysWatch::proc, kKeysWatchId);
        watch.m_window = nullptr;
        break;
    default:
        break;
    }
    return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

InputMethodInfo activeInputMethod()
{
    InputMethodInfo info;
    info.layout = reinterpret_cast<std::uintptr_t>(::GetKeyboardLayout(0));
    Microsoft::WRL::ComPtr<ITfInputProcessorProfileMgr> manager;
    if (FAILED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager))))
        return info;
    TF_INPUTPROCESSORPROFILE profile {};
    if (FAILED(manager->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &profile))
        || profile.dwProfileType != TF_PROFILETYPE_INPUTPROCESSOR)
        return info; // a keyboard layout alone: its HKL says it all
    wchar_t clsid[40] {};
    if (::StringFromGUID2(profile.clsid, clsid, static_cast<int>(std::size(clsid))))
        info.service = clsid;
    Microsoft::WRL::ComPtr<ITfInputProcessorProfiles> profiles;
    BSTR description = nullptr;
    if (SUCCEEDED(manager.As(&profiles))
        && SUCCEEDED(profiles->GetLanguageProfileDescription(profile.clsid, profile.langid, profile.guidProfile, &description))
        && description) {
        info.name.assign(description, ::SysStringLen(description));
        ::SysFreeString(description);
    }
    return info;
}

bool isCloaked(HWND window)
{
    DWORD cloaked = 0;
    return window && SUCCEEDED(::DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked != 0;
}

} // namespace ws
