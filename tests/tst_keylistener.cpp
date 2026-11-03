// Dispatch/lease regressions use only private window messages: no global
// keystrokes or user files. serviceWorker optionally uses a real SYSTEM
// --console service on WINSHUN_KEY_TEST_PIPE, without installing/replacing it.
#include "KeyListener.h"
#include "KeyListenerProtocol.h"
#include "NumberKeys.h"
#include "OwnInput.h"
#include "ServiceKeyListener.h"
#include "VolumeProtocol.h"
#include "Win32Util.h"

#include <QElapsedTimer>
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
