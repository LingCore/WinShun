// Dispatch/lease regressions use only private window messages: no global
// keystrokes or user files. serviceWorker optionally uses a real SYSTEM
// --console service on WINSHUN_KEY_TEST_PIPE, without installing/replacing it.
#include "KeyListener.h"
#include "KeyListenerProtocol.h"
#include "NumberKeys.h"
#include "OwnInput.h"
#include "PlainHotkeys.h"
#include "ServiceKeyListener.h"
#include "VolumeProtocol.h"
#include "Win32Util.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QScopeGuard>
#include <QTest>
#include <QUuid>

#include <algorithm>
#include <array>
#include <atomic>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace ws;
using namespace ws::volproto;
using namespace Qt::StringLiterals;

namespace {
// A message-only window counting WM_HOTKEY, for plainkeys.
struct HotkeyWindow {
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_HOTKEY) {
            auto* self = reinterpret_cast<HotkeyWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (self) {
                ++self->count;
                self->lastId = static_cast<int>(wParam);
                self->lastVk = HIWORD(lParam);
                if (self->pressed)
                    self->pressed();
            }
            return 0;
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    HotkeyWindow()
    {
        WNDCLASSW type {};
        type.lpfnWndProc = proc;
        type.hInstance = ::GetModuleHandleW(nullptr);
        type.lpszClassName = L"WinShun.Test.PlainHotkeys";
        ::RegisterClassW(&type);
        hwnd = ::CreateWindowExW(0, type.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, nullptr);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    ~HotkeyWindow()
    {
        plainkeys::unregisterAll(hwnd);
        ::DestroyWindow(hwnd);
    }
    HWND hwnd = nullptr;
    int count = 0;
    int lastId = 0;
    UINT lastVk = 0;
    std::function<void()> pressed;
};

// A function key from F24 down that nobody holds alone, or 0.
UINT freePlainKey()
{
    for (UINT vk = VK_F24; vk >= VK_F13; --vk) {
        if (::RegisterHotKey(nullptr, 0x7243, MOD_NOREPEAT, vk)) {
            ::UnregisterHotKey(nullptr, 0x7243);
            return vk;
        }
    }
    return 0;
}
} // namespace

class KeyListenerTest : public QObject {
    Q_OBJECT

    // `reports`: 0 for a listener from before double Alt, kReportsAlt for one from before double Shift.
    static void ready(KeyListener& listener, DWORD pid, LPARAM reports = keyproto::kReportsAlt | keyproto::kReportsShift)
    {
        listener.setServiceProcess(pid);
        ::SendMessageW(listener.window(), keyproto::kReady, pid, reports);
    }

    static void tap(KeyListener& listener, DWORD pid, DWORD key, DWORD time)
    {
        ::SendMessageW(listener.window(), keyproto::kTap, keyproto::tapWord(pid, key, false), time);
    }

private slots:
    void routingAndHandover()
    {
        std::atomic<int> count {0};
        KeyListener listener({.doubleCtrl = [&](const KeyListener::Tap&) { ++count; }});
        QVERIFY(listener.isActive());
        constexpr DWORD first = 12345, second = 54321;
        DWORD time = ::GetTickCount() - 100;
        tap(listener, first, VK_LCONTROL, time);
        QCOMPARE(count.load(), 0); // no ready/lease yet
        ready(listener, first);
        QVERIFY(listener.serviceActive());
        tap(listener, second, VK_LCONTROL, time);
        QCOMPARE(count.load(), 0); // a stale or unrelated worker
        tap(listener, first, VK_LCONTROL, time);
        QCOMPARE(count.load(), 1);
        tap(listener, first, VK_LCONTROL, time);
        QCOMPARE(count.load(), 1); // same physical gesture, one notification
        listener.setSide(DoubleTapDetector::Side::Right);
        tap(listener, first, VK_LCONTROL, ++time);
        QCOMPARE(count.load(), 1);
        tap(listener, first, VK_RCONTROL, ++time);
        QCOMPARE(count.load(), 2);
        ready(listener, second);
        tap(listener, first, VK_RCONTROL, ++time);
        QCOMPARE(count.load(), 2);
        tap(listener, second, VK_RCONTROL, ++time);
        QCOMPARE(count.load(), 3);
        tap(listener, second, VK_RCONTROL, time - 2000);
        QCOMPARE(count.load(), 3); // old queued events cannot toggle later
        listener.setServiceProcess(0);
        QVERIFY(!listener.serviceActive());
        tap(listener, second, VK_RCONTROL, ++time);
        QCOMPARE(count.load(), 3);
    }

    // GitHub #111: the UIAccess listener tells where the double tap began
    // (keyproto::tapTimes), for "pressed again"; one from before, nothing.
    void serviceTapFirstPress()
    {
        std::vector<KeyListener::Tap> taps; // the listener's thread adds them while the test waits in SendMessage
        KeyListener listener({.doubleCtrl = [&](const KeyListener::Tap& tap) { taps.push_back(tap); }});
        QVERIFY(listener.isActive());
        constexpr DWORD pid = 12345;
        ready(listener, pid);
        const auto time = static_cast<std::uint32_t>(::GetTickCount() - 100);
        ::SendMessageW(listener.window(), keyproto::kTap, keyproto::tapWord(pid, VK_LCONTROL, false),
            keyproto::tapTimes(time, 310));
        QCOMPARE(taps.size(), std::size_t {1});
        QCOMPARE(taps[0].time, time);
        QVERIFY(taps[0].firstPress == std::optional<std::uint32_t>(time - 310));
        QVERIFY(taps[0].from == KeyListener::Tap::From::Helper);
        tap(listener, pid, VK_LCONTROL, time + 1); // a listener from before: the time alone
        QCOMPARE(taps.size(), std::size_t {2});
        QCOMPARE(taps[1].time, time + 1);
        QVERIFY(!taps[1].firstPress);
    }

    void altTaps()
    {
        std::atomic<int> count {0};
        std::vector<std::string> lines; // the listener's thread writes them while the test waits in SendMessage
        KeyListener listener({.doubleCtrl = [&](const KeyListener::Tap&) { ++count; },
            .log = [&](const std::string& line) { lines.push_back(line); }});
        constexpr DWORD pid = 12345;
        DWORD time = ::GetTickCount() - 100;
        ready(listener, pid); // it tells of Ctrl's and Alt's taps; only the key set counts
        tap(listener, pid, VK_LMENU, ++time);
        QCOMPARE(count.load(), 0);
        listener.setKeys(KeyListener::Keys::Alt);
        tap(listener, pid, VK_LCONTROL, ++time);
        QCOMPARE(count.load(), 0);
        tap(listener, pid, VK_LMENU, ++time);
        QCOMPARE(count.load(), 1);
        listener.setSide(DoubleTapDetector::Side::Right);
        tap(listener, pid, VK_LMENU, ++time);
        QCOMPARE(count.load(), 1);
        tap(listener, pid, VK_RMENU, ++time);
        QCOMPARE(count.load(), 2);
        // One from before double Alt: Alt's are this process's own Raw Input's
        // to tell of, Ctrl's still its. The log says so, once.
        const auto oldService = [&] {
            return std::ranges::count_if(lines, [](const std::string& l) { return l.starts_with("Double Alt or Shift:"); });
        };
        QCOMPARE(oldService(), 0);
        ready(listener, pid, 0);
        ready(listener, pid, 0);
        QCOMPARE(oldService(), 1);
        QVERIFY(listener.serviceActive());
        tap(listener, pid, VK_RMENU, ++time);
        QCOMPARE(count.load(), 2);
        listener.setKeys(KeyListener::Keys::Ctrl);
        listener.setSide(DoubleTapDetector::Side::Both);
        tap(listener, pid, VK_LCONTROL, ++time);
        QCOMPARE(count.load(), 3);
    }

    void shiftTaps()
    {
        std::atomic<int> count {0};
        std::vector<std::string> lines;
        KeyListener listener({.doubleCtrl = [&](const KeyListener::Tap&) { ++count; },
            .log = [&](const std::string& line) { lines.push_back(line); }});
        constexpr DWORD pid = 12345;
        DWORD time = ::GetTickCount() - 100;
        ready(listener, pid);
        tap(listener, pid, VK_LSHIFT, ++time);
        QCOMPARE(count.load(), 0); // Ctrl is the key set
        listener.setKeys(KeyListener::Keys::Shift);
        tap(listener, pid, VK_LCONTROL, ++time);
        QCOMPARE(count.load(), 0);
        tap(listener, pid, VK_LMENU, ++time);
        QCOMPARE(count.load(), 0);
        tap(listener, pid, VK_LSHIFT, ++time);
        QCOMPARE(count.load(), 1);
        listener.setSide(DoubleTapDetector::Side::Right);
        tap(listener, pid, VK_LSHIFT, ++time);
        QCOMPARE(count.load(), 1);
        tap(listener, pid, VK_RSHIFT, ++time);
        QCOMPARE(count.load(), 2);
        // One from before double Shift (it tells of Ctrl and Alt): Shift's are
        // this process's own Raw Input's to tell of; the log says so, once.
        const auto oldService = [&] {
            return std::ranges::count_if(lines, [](const std::string& l) { return l.starts_with("Double Alt or Shift:"); });
        };
        QCOMPARE(oldService(), 0);
        ready(listener, pid, keyproto::kReportsAlt);
        ready(listener, pid, keyproto::kReportsAlt);
        QCOMPARE(oldService(), 1);
        QVERIFY(listener.serviceActive());
        tap(listener, pid, VK_RSHIFT, ++time);
        QCOMPARE(count.load(), 2);
        listener.setKeys(KeyListener::Keys::Alt); // Alt's it still tells of
        listener.setSide(DoubleTapDetector::Side::Both);
        tap(listener, pid, VK_LMENU, ++time);
        QCOMPARE(count.load(), 3);
    }

    void recordingShortcutPausesRemoteTaps()
    {
        std::atomic<int> count {0};
        KeyListener listener({.doubleCtrl = [&](const KeyListener::Tap&) { ++count; }});
        ready(listener, 12345);
        const RAWINPUTDEVICE remove {1, 6, RIDEV_REMOVE, nullptr};
        QVERIFY(::RegisterRawInputDevices(&remove, 1, sizeof remove));
        tap(listener, 12345, VK_LCONTROL, ::GetTickCount() - 10);
        QCOMPARE(count.load(), 0);
        const RAWINPUTDEVICE restore {1, 6, RIDEV_INPUTSINK, listener.window()};
        QVERIFY(::RegisterRawInputDevices(&restore, 1, sizeof restore));
        tap(listener, 12345, VK_LCONTROL, ::GetTickCount());
        QCOMPARE(count.load(), 1);
    }

    void stalledWorkerFallsBack()
    {
        KeyListener listener({.doubleCtrl = [](const KeyListener::Tap&) { }});
        ready(listener, 12345);
        QVERIFY(listener.serviceActive());
        QTRY_VERIFY_WITH_TIMEOUT(!listener.serviceActive(), keyproto::kLeaseMs + 1000);
        QVERIFY(listener.isActive()); // local Raw Input remains registered
    }

    void noServiceDoesNotBlockOrDisableLocalListener()
    {
        const QByteArray pipe = "WinShun.Test.Missing." + QUuid::createUuid().toByteArray(QUuid::Id128);
        qputenv("WINSHUN_PIPE", pipe);
        KeyListener listener({.doubleCtrl = [](const KeyListener::Tap&) { }});
        QElapsedTimer elapsed;
        elapsed.start();
        {
            ServiceKeyListener service(listener.window(), [&](DWORD pid) { listener.setServiceProcess(pid); });
            QTest::qWait(50);
            QVERIFY(!listener.serviceActive());
            QVERIFY(listener.isActive());
        }
        QVERIFY(elapsed.elapsed() < 1500); // cancellation interrupts the retry delay
        qunsetenv("WINSHUN_PIPE");
    }

    void ordinaryServiceRefusesWorker()
    {
        const QByteArray name = "WinShun.Test.Keys." + QUuid::createUuid().toByteArray(QUuid::Id128);
        QProcess server;
        server.start(QString::fromUtf8(WINSHUN_SERVICE_EXE),
            {QStringLiteral("--console"), QStringLiteral("--pipe"), QString::fromLatin1(name),
                QStringLiteral("--no-privileges")});
        QVERIFY(server.waitForStarted());
        QVERIFY(server.waitForReadyRead());
        Channel channel(::CreateFileW(fullPipePath(QString::fromLatin1(name).toStdWString()).c_str(),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        QVERIFY(channel.valid());
        Writer w;
        encode(w, Hello {});
        QVERIFY(channel.write(w.finish(Type::Hello), 1000));
        Type type {};
        std::vector<std::byte> payload;
        QVERIFY(channel.read(type, payload, 1000));
        HelloOk ok;
        QVERIFY(type == Type::HelloOk && decode(payload, ok));
        QCOMPARE(ok.capabilities & kCapabilityKeyListener, 0u);
        QCOMPARE(ok.capabilities & kCapabilityHotkeyRelay, 0u);
        w.reset();
        w.u64(1);
        QVERIFY(channel.write(w.finish(Type::OpenKeyListener), 1000));
        QVERIFY(channel.read(type, payload, 1000));
        Error error;
        QVERIFY(type == Type::Error && decode(payload, error));
        QCOMPARE(error.code, DWORD(ERROR_NOT_SUPPORTED));
        channel.close();
        server.kill(); // only the isolated child this test started
        QVERIFY(server.waitForFinished());
    }

    void workerCannotBeStartedWithoutUiAccess()
    {
        QProcess child;
        child.start(QString::fromUtf8(WINSHUN_SERVICE_EXE),
            {QStringLiteral("--listen-keys"), QString::number(::GetCurrentProcessId()), QStringLiteral("1")});
        QVERIFY(child.waitForFinished());
        QCOMPARE(child.exitCode(), int(ERROR_ACCESS_DENIED));
    }

    // GitHub #243: only F1-F24 and PrintScreen, at most 32, each as the
    // service checked it; the UIAccess check comes after.
    void workerRelaysPlainKeysOnly()
    {
        const auto run = [](const QString& keys) {
            QProcess child;
            child.start(QString::fromUtf8(WINSHUN_SERVICE_EXE),
                {QStringLiteral("--listen-keys"), QString::number(::GetCurrentProcessId()), QStringLiteral("1"), keys});
            child.waitForFinished();
            return child.exitCode();
        };
        QCOMPARE(run(QStringLiteral("112,44,135")), int(ERROR_ACCESS_DENIED)); // F1, PrintScreen, F24: no UIAccess here
        QCOMPARE(run(QStringLiteral("65")), int(ERROR_INVALID_PARAMETER)); // A types
        QCOMPARE(run(QStringLiteral("13")), int(ERROR_INVALID_PARAMETER)); // Enter
        QCOMPARE(run(QStringLiteral("112,x")), int(ERROR_INVALID_PARAMETER));
        QStringList many;
        for (int i = 0; i < 33; ++i)
            many << QString::number(VK_F1 + i % 24);
        QCOMPARE(run(many.join(u',')), int(ERROR_INVALID_PARAMETER));
        static_assert(keyproto::relayable(VK_F1) && keyproto::relayable(VK_F24) && keyproto::relayable(VK_SNAPSHOT));
        static_assert(!keyproto::relayable('A') && !keyproto::relayable(VK_RETURN) && !keyproto::relayable(VK_ESCAPE)
            && !keyproto::relayable(VK_SPACE) && !keyproto::relayable(VK_OEM_3));
    }

    // Registered through plainkeys: a plain function key is relayed, one with
    // a modifier is not (Windows gives it over administrators' windows too);
    // deliver() posts the WM_HOTKEY Windows would have, to its holder.
    void plainKeysRegistry()
    {
        const UINT vk = freePlainKey();
        if (!vk)
            QSKIP("Every function key F13-F24 is held by some program here.");
        HotkeyWindow window;
        int changes = 0;
        plainkeys::setChanged([&] { ++changes; });
        auto cleanup = qScopeGuard([] { plainkeys::setChanged(nullptr); });
        QVERIFY(plainkeys::registerHotkey(window.hwnd, 7, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, vk));
        QCOMPARE(changes, 0);
        QVERIFY(plainkeys::relayed().empty());
        QVERIFY(plainkeys::registerHotkey(window.hwnd, 9, MOD_NOREPEAT, vk));
        QCOMPARE(changes, 1);
        QCOMPARE(plainkeys::relayed(), std::vector<std::uint32_t> {vk});
        QVERIFY(!plainkeys::registerHotkey(window.hwnd, 10, MOD_NOREPEAT, vk)); // Windows: one holder
        QCOMPARE(::GetLastError(), DWORD(ERROR_HOTKEY_ALREADY_REGISTERED));
        QCOMPARE(changes, 1);
        QVERIFY(plainkeys::deliver(vk));
        QTRY_COMPARE(window.count, 1);
        QCOMPARE(window.lastId, 9);
        QCOMPARE(window.lastVk, vk);
        QVERIFY(!plainkeys::deliver(vk == VK_F24 ? VK_F23 : VK_F24));
        plainkeys::unregisterHotkey(window.hwnd, 9);
        QCOMPARE(changes, 2);
        QVERIFY(plainkeys::relayed().empty());
        QVERIFY(!plainkeys::deliver(vk));
        plainkeys::unregisterHotkey(window.hwnd, 7);
        QCOMPARE(changes, 2);
        QVERIFY(freePlainKey() != 0); // let go of
    }

    // The UIAccess listener's word of a plain hotkey: from the listener this
    // process has now only, with the key and whether a program sent it.
    void plainHotkeyFromTheListener()
    {
        std::vector<std::pair<std::uint32_t, bool>> told; // the listener's thread adds while the test waits in SendMessage
        KeyListener listener({.plainHotkey = [&](std::uint32_t vk, bool sent) { told.emplace_back(vk, sent); }});
        QVERIFY(listener.isActive());
        constexpr DWORD pid = 12345, other = 54321;
        ::SendMessageW(listener.window(), keyproto::kHotkey, keyproto::hotkeyWord(pid, VK_F1, false), ::GetTickCount());
        QVERIFY(told.empty()); // no listener yet
        ready(listener, pid);
        ::SendMessageW(listener.window(), keyproto::kHotkey, keyproto::hotkeyWord(other, VK_F1, false), ::GetTickCount());
        QVERIFY(told.empty());
        ::SendMessageW(listener.window(), keyproto::kHotkey, keyproto::hotkeyWord(pid, VK_SNAPSHOT, true), ::GetTickCount());
        QCOMPARE(told.size(), std::size_t {1});
        QCOMPARE(told[0].first, std::uint32_t(VK_SNAPSHOT));
        QVERIFY(told[0].second);
        // No double tap wanted: the listener is there for the hotkeys alone.
        ::SendMessageW(listener.window(), keyproto::kTap, keyproto::tapWord(pid, VK_LCONTROL, false), ::GetTickCount());
        QCOMPARE(told.size(), std::size_t {1});
    }

    void serviceWorker()
    {
        const QByteArray pipe = qgetenv("WINSHUN_KEY_TEST_PIPE");
        if (pipe.isEmpty())
            QSKIP("Set WINSHUN_KEY_TEST_PIPE to an isolated SYSTEM --console service for the real launch test.");
        qputenv("WINSHUN_PIPE", pipe);
        KeyListener listener({.doubleCtrl = [](const KeyListener::Tap&) { }});
        std::atomic<DWORD> worker {0};
        auto service = std::make_unique<ServiceKeyListener>(listener.window(), [&](DWORD pid) {
            worker = pid;
            listener.setServiceProcess(pid);
        });
        QTRY_VERIFY_WITH_TIMEOUT(listener.serviceActive(), 10000);
        const win32::UniqueHandle process(
            ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, worker.load()));
        QVERIFY(process.valid());
        HANDLE raw = nullptr;
        QVERIFY(::OpenProcessToken(process.get(), TOKEN_QUERY, &raw));
        const win32::UniqueHandle token(raw);
        DWORD uiAccess = 0, size = 0;
        QVERIFY(::GetTokenInformation(token.get(), TokenUIAccess, &uiAccess, sizeof uiAccess, &size));
        QCOMPARE(uiAccess, 1u);
        alignas(TOKEN_MANDATORY_LABEL) BYTE label[128] {};
        QVERIFY(::GetTokenInformation(token.get(), TokenIntegrityLevel, label, sizeof label, &size));
        const PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label)->Label.Sid;
        QCOMPARE(*::GetSidSubAuthority(sid, *::GetSidSubAuthorityCount(sid) - 1), DWORD(SECURITY_MANDATORY_HIGH_RID));
        TOKEN_ELEVATION elevated {};
        QVERIFY(::GetTokenInformation(token.get(), TokenElevation, &elevated, sizeof elevated, &size));
        QCOMPARE(elevated.TokenIsElevated, 0u);
        DWORD session = 0, ownSession = 0;
        QVERIFY(::ProcessIdToSessionId(worker.load(), &session));
        QVERIFY(::ProcessIdToSessionId(::GetCurrentProcessId(), &ownSession));
        QCOMPARE(session, ownSession);
        service.reset();
        QVERIFY(!listener.serviceActive());
        QCOMPARE(::WaitForSingleObject(process.get(), 5000), DWORD(WAIT_OBJECT_0));
        QVERIFY(listener.isActive());
        qunsetenv("WINSHUN_PIPE");
    }

    void elevatedForegroundInput()
    {
        const QByteArray pipe = qgetenv("WINSHUN_KEY_TEST_PIPE");
        const QString eventName = qEnvironmentVariable("WINSHUN_KEY_INPUT_EVENT");
        if (pipe.isEmpty() || eventName.isEmpty())
            QSKIP("Live desktop test: needs the isolated SYSTEM service and an elevated input sender.");
        qputenv("WINSHUN_PIPE", pipe);
        const win32::UniqueHandle readyEvent(::CreateEventW(nullptr, TRUE, FALSE, eventName.toStdWString().c_str()));
        QVERIFY(readyEvent.valid());
        struct TestWindow {
            HWND hwnd = ::CreateWindowExW(0, L"STATIC", L"WinShun issue #31 input test", WS_OVERLAPPEDWINDOW, 100, 100,
                400, 100, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
            ~TestWindow()
            {
                if (hwnd)
                    ::DestroyWindow(hwnd);
            }
        } window;
        QVERIFY(window.hwnd);
        std::atomic<int> count {0};
        KeyListener listener({.doubleCtrl = [&](const KeyListener::Tap&) { ++count; }});
        ServiceKeyListener service(listener.window(), [&](DWORD pid) { listener.setServiceProcess(pid); });
        QTRY_VERIFY_WITH_TIMEOUT(listener.serviceActive(), 10000);
        ::SetEvent(readyEvent.get());
        for (int expected = 1; expected <= 3; ++expected) {
            QTRY_COMPARE_WITH_TIMEOUT(count.load(), expected, 10000);
            // The production helper grants this process foreground access.
            // Verify that a normal-IL UI can actually take it, not just that
            // a callback arrived while the elevated sender had focus.
            ::ShowWindow(window.hwnd, SW_SHOWNORMAL);
            QVERIFY(::SetForegroundWindow(window.hwnd));
            QCOMPARE(::GetForegroundWindow(), window.hwnd);
        }
        QTest::qWait(250);
        QCOMPARE(count.load(), 3);
        qunsetenv("WINSHUN_PIPE");
    }

    // GitHub #243, live: a plain function key of this process's, pressed
    // while a window run as administrator is in front, comes through the
    // service's UIAccess listener (held back from that window), and this
    // process may then take the front; over its own window its own
    // RegisterHotKey hears it, never both. The sender (elevated) fronts its
    // window and presses the key (WINSHUN_KEY_RELAY_VK) three times, then
    // fronts this test's window once, then csrss's error box if it can, and
    // logs what its own window got. WINSHUN_KEY_TEST_PIPE: a SYSTEM service
    // installed-from this exe; WINSHUN_KEY_OTHER_PIPE: one that is not.
    void elevatedForegroundPlainHotkey()
    {
        const QByteArray pipe = qgetenv("WINSHUN_KEY_TEST_PIPE");
        const QString readyName = qEnvironmentVariable("WINSHUN_KEY_INPUT_EVENT");
        const QString doneName = qEnvironmentVariable("WINSHUN_KEY_DONE_EVENT");
        const UINT vk = qEnvironmentVariable("WINSHUN_KEY_RELAY_VK").toUInt(nullptr, 16);
        if (pipe.isEmpty() || readyName.isEmpty() || doneName.isEmpty() || !keyproto::relayable(vk))
            QSKIP("Live desktop test: needs the isolated SYSTEM services and an elevated sender.");
        // Another client than Win顺's own: no relay offered, and refused when asked.
        if (const QByteArray other = qgetenv("WINSHUN_KEY_OTHER_PIPE"); !other.isEmpty()) {
            Channel channel(::CreateFileW(fullPipePath(QString::fromLatin1(other).toStdWString()).c_str(),
                GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
            QVERIFY(channel.valid());
            Writer w;
            encode(w, Hello {});
            QVERIFY(channel.write(w.finish(Type::Hello), 1000));
            Type type {};
            std::vector<std::byte> payload;
            HelloOk ok;
            QVERIFY(channel.read(type, payload, 1000) && type == Type::HelloOk && decode(payload, ok));
            QVERIFY(ok.capabilities & kCapabilityKeyListener);
            QCOMPARE(ok.capabilities & kCapabilityHotkeyRelay, 0u);
            const HWND target = ::CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
            w.reset();
            w.u64(reinterpret_cast<std::uintptr_t>(target));
            w.u32(1);
            w.u32(vk);
            QVERIFY(channel.write(w.finish(Type::OpenKeyListener), 1000));
            Error error;
            QVERIFY(channel.read(type, payload, 2000) && type == Type::Error && decode(payload, error));
            QCOMPARE(error.code, DWORD(ERROR_ACCESS_DENIED));
            ::DestroyWindow(target);
        }
        qputenv("WINSHUN_PIPE", pipe);
        auto unset = qScopeGuard([] { qunsetenv("WINSHUN_PIPE"); });
        const win32::UniqueHandle readyEvent(::CreateEventW(nullptr, TRUE, FALSE, readyName.toStdWString().c_str()));
        const win32::UniqueHandle doneEvent(::CreateEventW(nullptr, TRUE, FALSE, doneName.toStdWString().c_str()));
        QVERIFY(readyEvent.valid() && doneEvent.valid());
        struct TestWindow {
            HWND hwnd = ::CreateWindowExW(0, L"STATIC", L"WinShun issue #243 relay test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                120, 120, 420, 120, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
            ~TestWindow()
            {
                if (hwnd)
                    ::DestroyWindow(hwnd);
            }
        } shown;
        QVERIFY(shown.hwnd);
        HotkeyWindow hotkeys;
        QVERIFY2(plainkeys::registerHotkey(hotkeys.hwnd, 1, MOD_NOREPEAT, vk), "the key is held by another program");
        QStringList timeline;
        QElapsedTimer clock;
        clock.start();
        int relayed = 0, fronted = 0;
        hotkeys.pressed = [&] {
            // As Win顺 does for a hotkey: to the front, over the administrator's window.
            const bool front = ::SetForegroundWindow(shown.hwnd) && ::GetForegroundWindow() == shown.hwnd;
            fronted += front ? 1 : 0;
            timeline << u"%1 WM_HOTKEY front=%2"_s.arg(clock.elapsed()).arg(front);
        };
        KeyListener listener({.plainHotkey = [&](std::uint32_t key, bool) {
            QMetaObject::invokeMethod(qApp, [&, key] {
                ++relayed;
                timeline << u"%1 relayed vk=%2"_s.arg(clock.elapsed()).arg(key, 0, 16);
                plainkeys::deliver(key);
            }, Qt::QueuedConnection);
        }});
        ServiceKeyListener service(listener.window(), [&](DWORD pid) { listener.setServiceProcess(pid); },
            plainkeys::relayed());
        QTRY_VERIFY_WITH_TIMEOUT(listener.serviceActive() && service.relaying(), 10000);
        ::SetEvent(readyEvent.get());
        QTRY_VERIFY_WITH_TIMEOUT(::WaitForSingleObject(doneEvent.get(), 0) == WAIT_OBJECT_0, 90000);
        QTest::qWait(500);
        const QString path = qEnvironmentVariable("WINSHUN_KEY_RELAY_LOG");
        if (QFile file(path); !path.isEmpty() && file.open(QIODevice::WriteOnly | QIODevice::Text))
            file.write(timeline.join(u'\n').toUtf8());
        qInfo().noquote() << timeline.join(u"; "_s);
        QVERIFY(relayed >= 3); // three over the administrator's window (four with csrss's box)
        QCOMPARE(hotkeys.count, relayed + 1); // and one over this test's window, by its own registration
        QVERIFY(fronted >= 3);
    }

    // The number keys' hook sets the keyboard's Raw Input aside while it
    // lives; drained with no digit down, it goes at once and gives it back
    // before it is destroyed (App then drops it).
    void numberKeyHookDrainsAtOnce()
    {
        KeyListener listener({.doubleCtrl = [](const KeyListener::Tap&) {}});
        QVERIFY(listener.isActive());
        QTRY_VERIFY(keyboardRawInput().registration.has_value());
        std::atomic<int> keys {0};
        std::atomic<int> drained {0};
        numberkeys::Hook hook(numberkeys::Modifier::Alt, 1u << 5, [&](const auto&) { ++keys; }, [&] { ++drained; });
        QVERIFY(hook.isActive());
        QVERIFY(!keyboardRawInput().registration.has_value());
        hook.drain();
        QTRY_COMPARE_WITH_TIMEOUT(drained.load(), 1, 2000);
        QVERIFY(keyboardRawInput().registration.has_value());
        QCOMPARE(keys.load(), 0);
    }

    // A digit the hook took, still down when it is done (Ctrl+1 opened a row
    // and hid the launcher): its repeats and release stay its own, never the
    // other program's hotkey; then it goes. Opt-in (WINSHUN_NUMBERKEYS_LIVE=1):
    // injects Alt+5 into this test's own window, in front, only.
    void numberKeyHookDrainsAHeldDigit()
    {
        if (qEnvironmentVariableIntValue("WINSHUN_NUMBERKEYS_LIVE") != 1)
            QSKIP("Opt-in desktop test: injects Alt+5 into its own window only.");
        // The other program: Alt+5 held as a hotkey, on a thread of its own
        // (Qt's event loop here would take its WM_HOTKEY first).
        std::atomic<int> hotkeys {0};
        std::atomic<DWORD> hotkeyThread {0};
        std::latch registered(1);
        bool held = false;
        std::jthread other([&] {
            MSG msg;
            ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
            hotkeyThread = ::GetCurrentThreadId();
            held = ::RegisterHotKey(nullptr, 1, MOD_ALT, '5');
            registered.count_down();
            if (!held)
                return;
            while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
                hotkeys += msg.message == WM_HOTKEY;
            ::UnregisterHotKey(nullptr, 1);
        });
        registered.wait();
        const auto stop = qScopeGuard([&] { ::PostThreadMessageW(hotkeyThread.load(), WM_QUIT, 0, 0); });
        if (!held)
            QSKIP("Alt+5 is another program's hotkey here.");
        struct TestWindow {
            HWND hwnd = ::CreateWindowExW(0, L"STATIC", L"WinShun number keys drain test (temporary)", WS_OVERLAPPEDWINDOW,
                100, 100, 420, 100, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
            ~TestWindow() { if (hwnd) ::DestroyWindow(hwnd); }
        } window;
        QVERIFY(window.hwnd);
        ::ShowWindow(window.hwnd, SW_SHOWNORMAL);
        // A key of our own first (the unassigned masking key): Windows lets the
        // process that sent the last input take the foreground.
        std::array<INPUT, 2> mask {};
        for (INPUT& key : mask) {
            key.type = INPUT_KEYBOARD;
            key.ki.wVk = kMaskKey;
            key.ki.dwExtraInfo = kOwnInput;
        }
        mask[1].ki.dwFlags = KEYEVENTF_KEYUP;
        ::SendInput(UINT(mask.size()), mask.data(), sizeof(INPUT));
        ::SetForegroundWindow(window.hwnd);
        QTRY_COMPARE_WITH_TIMEOUT(::GetForegroundWindow(), window.hwnd, 2000);
        for (const int modifier : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN})
            QVERIFY((::GetAsyncKeyState(modifier) & 0x8000) == 0);
        bool altDown = false, fiveDown = false;
        const auto send = [&](WORD vk, bool up, bool own = false) {
            if (!own && ::GetForegroundWindow() != window.hwnd)
                return false; // never into another window
            INPUT input {};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = vk;
            input.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
            input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
            input.ki.dwExtraInfo = own ? kOwnInput : 0;
            if (::SendInput(1, &input, sizeof input) != 1)
                return false;
            (vk == VK_MENU ? altDown : fiveDown) = !up;
            return true;
        };
        const auto release = qScopeGuard([&] { // past any hook, whatever failed
            if (fiveDown)
                send('5', true, true);
            if (altDown)
                send(VK_MENU, true, true);
        });
        std::atomic<int> presses {0};
        std::atomic<int> drained {0};
        auto hook = std::make_unique<numberkeys::Hook>(numberkeys::Modifier::Alt, 1u << 5,
            [&](const numberkeys::Hook::Key& key) { presses += key.press; }, [&] { ++drained; });
        QVERIFY(hook->isActive());
        QVERIFY(send(VK_MENU, false) && send('5', false));
        QTRY_COMPARE_WITH_TIMEOUT(presses.load(), 1, 2000); // taken: no hotkey
        hook->drain();
        for (int repeat = 0; repeat < 3; ++repeat) {
            QVERIFY(send('5', false)); // auto-repeat, the launcher gone
            QTest::qWait(30);
        }
        QCOMPARE(drained.load(), 0);
        QVERIFY(send('5', true));
        QTRY_COMPARE_WITH_TIMEOUT(drained.load(), 1, 2000);
        QVERIFY(send(VK_MENU, true));
        QTest::qWait(100);
        QCOMPARE(presses.load(), 1); // nothing told after drain()
        QCOMPARE(hotkeys.load(), 0);
        // Drained, the hook takes nothing: Alt+5 is the hotkey again.
        QVERIFY(send(VK_MENU, false) && send('5', false) && send('5', true) && send(VK_MENU, true));
        QTRY_COMPARE_WITH_TIMEOUT(hotkeys.load(), 1, 2000);
        hook.reset();
    }
};

QTEST_GUILESS_MAIN(KeyListenerTest)
#include "tst_keylistener.moc"
