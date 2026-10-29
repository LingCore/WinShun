#include "HoldWatch.h"

#include "OwnInput.h"

#include <algorithm>
#include <iterator>
#include <latch>
#include <utility>
#include <vector>

namespace ws {

namespace {

// A low-level hook is called on the thread that set it.
thread_local HoldWatch* t_watch = nullptr;

constexpr UINT kDrainMessage = WM_APP; // to the watch's thread (drain)
constexpr UINT kNoteMessage = WM_APP + 1; // to the window on the GUI thread: a Note, the key's time
constexpr UINT kPasteMessage = WM_APP + 2; // to the watch's thread: the paste's guard gone unused, or fewer keys went
constexpr wchar_t kWindowClass[] = L"WinShun.HoldWatch";
constexpr std::uint64_t kPassingMs = 1000; // the paste's keys past the hook by then, or lost

bool down(int vk)
{
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}

// KeyRouter's masking key, to the program in front: one it does not act on.
void sendMask()
{
    INPUT keys[2] {};
    for (INPUT& key : keys) {
        key.type = INPUT_KEYBOARD;
        key.ki.wVk = kMaskKey;
        key.ki.dwExtraInfo = kOwnMask;
    }
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(static_cast<UINT>(std::size(keys)), keys, sizeof(INPUT));
}

} // namespace

// The paste the hold ends with, between the GUI thread (its guard) and the
// watch's thread (the hook).
struct HoldWatch::Paste {
    enum class State {
        None, // no paste
        Coming, // its keys not sent yet
        Watching, // sent: `keys` (not written again), the hook judges them
        Through, // all past the hook
        Gone, // none coming (the guard went unused), or the hook gone
    };
    std::atomic<State> state {State::None};
    std::vector<pastekeys::Key> keys;
    std::atomic<std::size_t> count {0}; // of `keys`, those that went
    std::atomic<std::uint64_t> watchedAt {0};
    std::atomic<DWORD> thread {0}; // the watch's, while its hook is in
};

class HoldWatch::PasteGuard final : public paste::Guard {
public:
    explicit PasteGuard(std::shared_ptr<Paste> paste)
        : m_paste(std::move(paste))
    {
    }
    ~PasteGuard() override
    {
        // Never watched: no keys coming (the paste failed, or went into a
        // field of Win顺's own).
        auto coming = Paste::State::Coming;
        if (m_paste->state.compare_exchange_strong(coming, Paste::State::Gone))
            wake();
    }
    PasteGuard(const PasteGuard&) = delete;
    PasteGuard& operator=(const PasteGuard&) = delete;

    bool watch(std::span<const pastekeys::Key> keys) override
    {
        // The hook goes on the GUI thread only, as this runs: still in now,
        // it is in until the keys are past it (pastePassing).
        if (!m_paste->thread.load() || m_paste->state.load() != Paste::State::Coming)
            return false;
        m_paste->keys.assign(keys.begin(), keys.end());
        m_paste->count.store(keys.size());
        m_paste->watchedAt.store(::GetTickCount64());
        m_paste->state.store(Paste::State::Watching, std::memory_order_release);
        return true;
    }
    void went(std::size_t count) override
    {
        if (count < m_paste->count.load()) {
            m_paste->count.store(count);
            wake(); // those that went may all be past it already
        }
    }

private:
    void wake() const
    {
        if (const DWORD thread = m_paste->thread.load())
            ::PostThreadMessageW(thread, kPasteMessage, 0, 0);
    }

    const std::shared_ptr<Paste> m_paste;
};

HoldWatch::HoldWatch(unsigned modifiers, unsigned vk, Callbacks callbacks)
    : m_modifiers(modifiers & hotkeysteps::kModifiers)
    , m_vk(vk)
    , m_callbacks(std::move(callbacks))
    , m_paste(std::make_shared<Paste>())
{
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    // Registered once (made on the GUI thread only); tried again next time
    // if it failed.
    static ATOM windowClass = 0;
    if (!windowClass) {
        WNDCLASSW wc {};
        wc.lpfnWndProc = &HoldWatch::windowProc;
        wc.hInstance = instance;
        wc.lpszClassName = kWindowClass;
        windowClass = ::RegisterClassW(&wc);
    }
    if (!windowClass) {
        m_setupError = ::GetLastError();
        m_failed = "no-class";
        return;
    }
    m_window = ::CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (!m_window) {
        m_setupError = ::GetLastError();
        m_failed = "no-window";
        return;
    }
    ::SetWindowLongPtrW(m_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    std::latch ready(1);
    m_thread = std::jthread([this, &ready, instance] {
        t_watch = this;
        MSG msg;
        ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // the thread's message queue, for WM_QUIT
        m_threadId = ::GetCurrentThreadId();
        // Every key press in the system waits for the hook's answer.
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        const HHOOK hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, &HoldWatch::keyboardProc, instance, 0);
        if (hook) {
            // The keys as they are now; those pressed or let go since the
            // hook went in come to it next, in order (the system state
            // follows each key only once the hooks are through with it).
            hotkeysteps::Sides sides;
            unsigned user = 0;
            for (const std::uint16_t side : pastekeys::kSides) {
                if (down(side)) {
                    sides.set(side, true);
                    user |= pastekeys::sideBit(side);
                }
            }
            const DWORD now = ::GetTickCount();
            m_watch.emplace(m_modifiers, m_vk, sides, down(static_cast<int>(m_vk)), now);
            m_guard = pastekeys::Guard(user);
            m_active = m_watch->holding();
            if (m_active)
                m_paste->thread.store(m_threadId.load());
            if (m_active)
                post(m_watch->start(), now); // the key let go already: before any key's note
        } else {
            m_setupError = ::GetLastError();
            m_failed = "no-hook";
        }
        // The GUI thread goes on (the window opens) while the mask goes
        // through every hook in the system, this one's too.
        ready.count_down();
        if (m_active) {
            // The program in front saw none of the hotkey's press (it is
            // Windows'): an Alt or Shift let go before the key is one
            // pressed alone there. Its menu bar opens and takes the paste;
            // the input method switches between Chinese and English. A key
            // in between that no one acts on, as KeyRouter sends for those
            // pressed while it routes.
            if (m_modifiers & (hotkeysteps::kAlt | hotkeysteps::kShift | hotkeysteps::kWin))
                sendMask();
            // The hook is called from inside GetMessage.
            while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (msg.hwnd == nullptr && msg.message == kDrainMessage) {
                    m_watch->stop();
                    checkDrained(); // nothing taken held, no paste ahead: done at once
                    continue;
                }
                if (msg.hwnd == nullptr && msg.message == kPasteMessage) {
                    settlePaste();
                    checkDrained();
                    continue;
                }
                ::DispatchMessageW(&msg);
            }
        }
        if (hook)
            ::UnhookWindowsHookEx(hook);
    });
    ready.wait();
}

HoldWatch::~HoldWatch()
{
    m_paste->thread.store(0); // a guard still unused watches nothing now
    if (const DWORD thread = m_threadId.load())
        ::PostThreadMessageW(thread, WM_QUIT, 0, 0); // ends the message loop (if it is still in it)
    if (m_thread.joinable())
        m_thread.join();
    if (m_window) {
        ::SetWindowLongPtrW(m_window, GWLP_USERDATA, 0); // notes still queued for it go nowhere
        ::DestroyWindow(m_window);
    }
}

void HoldWatch::drain()
{
    if (m_draining.exchange(true))
        return;
    if (!m_active || !::PostThreadMessageW(m_threadId.load(), kDrainMessage, 0, 0)) {
        if (m_callbacks.drained)
            m_callbacks.drained();
    }
}

void HoldWatch::checkDrained()
{
    if (m_draining && !m_watch->holdsTaken() && !pasteAhead() && !std::exchange(m_drainedTold, true)
        && m_callbacks.drained)
        m_callbacks.drained();
}

std::unique_ptr<paste::Guard> HoldWatch::guardPaste()
{
    if (!m_active || m_draining)
        return nullptr;
    auto none = Paste::State::None;
    if (!m_paste->state.compare_exchange_strong(none, Paste::State::Coming))
        return nullptr; // one paste a hold
    return std::make_unique<PasteGuard>(m_paste);
}

bool HoldWatch::pastePassing() const noexcept
{
    return m_paste->state.load() == Paste::State::Watching
        && ::GetTickCount64() - m_paste->watchedAt.load() < kPassingMs;
}

bool HoldWatch::pasteAhead() const noexcept
{
    const Paste::State state = m_paste->state.load();
    return state == Paste::State::Coming || state == Paste::State::Watching;
}

bool HoldWatch::passOwn(std::uint16_t side, bool up)
{
    if (m_paste->state.load(std::memory_order_acquire) != Paste::State::Watching)
        return true;
    const std::size_t count = std::min(m_paste->count.load(), m_paste->keys.size());
    return m_guard.own(std::span(m_paste->keys).first(count), side, up);
}

void HoldWatch::settlePaste()
{
    if (m_paste->state.load(std::memory_order_acquire) == Paste::State::Watching
        && m_guard.passed() >= m_paste->count.load()) {
        m_guard.finish();
        m_paste->state.store(Paste::State::Through);
        checkDrained();
    }
}

LRESULT CALLBACK HoldWatch::keyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && t_watch && t_watch->m_watch)
        return t_watch->handleKey(code, wParam, lParam);
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT HoldWatch::handleKey(int code, WPARAM message, LPARAM lParam)
{
    const KBDLLHOOKSTRUCT& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    const bool press = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const std::uint16_t side = pastekeys::sided(static_cast<std::uint16_t>(event.vkCode),
        (event.flags & LLKHF_EXTENDED) != 0, static_cast<std::uint16_t>(event.scanCode));
    if (isOwnInput(event.dwExtraInfo)) {
        // Win顺's own: the paste keys, KeyRouter's masking key. A paste key
        // that would put a modifier back as the hand no longer has it goes
        // to no one, the hooks before this one included; nor does one the
        // hand's keys overtook while those hooks had it (sent meanwhile,
        // they come here before this returns).
        const std::size_t before = m_guard.passed();
        LRESULT answer = 1;
        if (passOwn(side, !press)) {
            const std::size_t after = m_guard.passed();
            answer = ::CallNextHookEx(nullptr, code, message, lParam);
            if (after != before && m_guard.passed() == after && !m_guard.agrees())
                answer = 1;
        }
        settlePaste(); // once the hooks before are through: the last key's done, the hook may go
        return answer;
    }
    // The hooks installed before this one first (the newest is called
    // first): a key one of them takes (a remapper's) is theirs; its
    // replacement comes here as a key of its own. Modifiers are followed
    // whatever happens to them.
    const LRESULT next = ::CallNextHookEx(nullptr, code, message, lParam);
    // The paste's guard: the hand's modifiers as the system has them (not
    // those a hook before took). A let-go of the side the paste holds down
    // is kept back: the paste's own let-go goes for it, after its key.
    const bool keptBack = next == 0 && m_guard.user(side, !press);
    if (next != 0 && hotkeysteps::modifierOf(event.vkCode) == 0)
        return next;
    if (m_draining)
        m_watch->stop(); // takes nothing new from now on, before its thread hears of it
    const hotkeysteps::Watch::Answer answer = m_watch->key(event.vkCode, press, event.time);
    post(answer.note, event.time);
    post(answer.then, event.time);
    if (m_draining)
        checkDrained();
    return answer.take || keptBack ? 1 : next;
}

void HoldWatch::post(hotkeysteps::Note note, std::uint32_t time) const
{
    // Posted from inside the hook: ahead of the WM_HOTKEY Windows posts for
    // the same key once the hooks are through with it.
    if (note != hotkeysteps::Note::None && !m_draining)
        ::PostMessageW(m_window, kNoteMessage, static_cast<WPARAM>(note), static_cast<LPARAM>(time));
}

LRESULT CALLBACK HoldWatch::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kNoteMessage) {
        auto* watch = reinterpret_cast<HoldWatch*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (watch && !watch->m_draining && watch->m_callbacks.note)
            watch->m_callbacks.note(static_cast<hotkeysteps::Note>(wParam), static_cast<std::uint32_t>(lParam));
        return 0;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace ws
