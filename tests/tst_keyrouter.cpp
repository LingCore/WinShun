#include "platform/HoldWatch.h"
#include "platform/KeyRouter.h"
#include "platform/QtKeys.h"
#include "Diagnostics.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <imm.h>
#include <miniz.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>

using namespace ws;
using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

constexpr ULONG_PTR kTestInput = 0x57535445;
std::array<std::atomic<int>, 256> modifierUps {};

LRESULT CALLBACK modifierWitness(int code, WPARAM message, LPARAM data)
{
    if (code == HC_ACTION && (message == WM_KEYUP || message == WM_SYSKEYUP)) {
        const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        if (event.dwExtraInfo == kTestInput && event.vkCode < modifierUps.size())
            ++modifierUps[event.vkCode];
    }
    return ::CallNextHookEx(nullptr, code, message, data);
}

LRESULT CALLBACK receiverProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_KEYDOWN || message == WM_KEYUP) {
        std::printf("%s:%u\n", message == WM_KEYDOWN ? "down" : "up", static_cast<unsigned>(wParam));
        std::fflush(stdout);
    } else if (message == WM_ACTIVATEAPP && !wParam) {
        std::puts("deactivated");
        std::fflush(stdout);
    } else if (message == WM_DESTROY) {
        ::PostQuitMessage(0);
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

int receiver(bool shellCompanion = false)
{
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSW type {};
    type.lpfnWndProc = receiverProc;
    type.hInstance = instance;
    type.lpszClassName = L"WinShun.KeyRouterTest";
    if (!::RegisterClassW(&type))
        return 1;
    const HWND window = ::CreateWindowW(type.lpszClassName, L"WinShun keyboard routing test (temporary)",
        WS_OVERLAPPEDWINDOW, 100, 100, 520, 220, nullptr, nullptr, instance, nullptr);
    if (!window)
        return 2;
    int shellLeft = 0, shellHeight = 0;
    if (shellCompanion) {
        shellLeft = ::GetSystemMetrics(SM_CXSCREEN) / 2 - 260;
        shellHeight = ::GetSystemMetrics(SM_CYSCREEN) - 100;
        ::SetWindowPos(window, nullptr, shellLeft, 50, 520, shellHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ::ImmAssociateContextEx(window, nullptr, 0); // the fixture records raw letters, independent of the user's IME mode
    ::ShowWindow(window, SW_SHOWNORMAL);
    ::SetForegroundWindow(window);
    std::printf("window:%llu\n", static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(window)));
    if (shellCompanion) {
        const HWND companion = ::CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName, L"WinShun shell companion test",
            WS_POPUP | WS_BORDER, shellLeft - 280, 50, 280, shellHeight, window, nullptr, instance, nullptr);
        ::ShowWindow(companion, SW_SHOWNOACTIVATE);
        std::printf("companion:%llu\n", static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(companion)));
    }
    std::fflush(stdout);
    MSG message;
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    return 0;
}

struct PassiveWindow {
    HWND hwnd = ::CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"Passive clipboard test",
        WS_POPUP, 120, 340, 350, 80, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    ~PassiveWindow() { if (hwnd) ::DestroyWindow(hwnd); }
};

// The temporary window of another process that the keys are typed into: its
// own, in front, saying what reached it.
struct Receiver {
    QProcess process;
    HWND window = nullptr;
    QByteArray output;

    bool start()
    {
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(QCoreApplication::applicationFilePath(), {u"--key-receiver"_s});
        if (!process.waitForStarted(5000))
            return false;
        if (!QTest::qWaitFor([&] { return read().contains("window:"); }, 5000))
            return false;
        const QByteArray handle = output.mid(output.indexOf("window:") + 7).split('\n').front().trimmed();
        window = reinterpret_cast<HWND>(handle.toULongLong());
        return window && ::IsWindow(window) && QTest::qWaitFor([&] { return ::GetForegroundWindow() == window; }, 3000);
    }
    const QByteArray& read()
    {
        output += process.readAll();
        return output;
    }
    ~Receiver()
    {
        if (window)
            ::PostMessageW(window, WM_CLOSE, 0, 0);
        if (process.state() != QProcess::NotRunning && !process.waitForFinished(3000))
            process.kill();
    }
};

bool send(HWND target, WORD vk, bool up)
{
    // Never inject into a user window, or while a physical modifier is held.
    if (::GetForegroundWindow() != target)
        return false;
    for (const int modifier : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN}) {
        if (::GetAsyncKeyState(modifier) & 0x8000)
            return false;
    }
    INPUT input {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    if (vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT)
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    return ::SendInput(1, &input, sizeof input) == 1;
}

} // namespace

class KeyRouterTest : public QObject {
    Q_OBJECT

private slots:
    void groupSearchChordPolicy()
    {
        KeyRouter::Key key {.key = Qt::Key_G, .modifiers = Qt::ControlModifier, .vk = 'G'};
        QVERIFY(KeyRouter::wantsKey(key));
        QVERIFY(!KeyRouter::wantsKey(key, Qt::AltModifier, KeyRouter::Policy::Menu));
        key.modifiers |= Qt::ShiftModifier;
        QVERIFY(!KeyRouter::wantsKey(key));
        key.modifiers = Qt::ControlModifier;
        key.key = Qt::Key_W;
        key.vk = 'W';
        QVERIFY(!KeyRouter::wantsKey(key)); // unrelated app shortcuts still pass through
        // The clipboard menu's keys, pressed without the menu (GitHub #379): theirs while it is open.
        for (const auto& [qtKey, vk] : {std::pair {Qt::Key_E, UINT('E')}, std::pair {Qt::Key_S, UINT('S')}, std::pair {Qt::Key_T, UINT('T')}}) {
            key = {.key = qtKey, .modifiers = Qt::ControlModifier, .vk = vk};
            QVERIFY(KeyRouter::wantsKey(key));
            key.modifiers |= Qt::ShiftModifier;
            QVERIFY(!KeyRouter::wantsKey(key));
        }
        key = {.key = Qt::Key_F3, .modifiers = Qt::NoModifier, .vk = VK_F3};
        QVERIFY(KeyRouter::wantsKey(key));
        QVERIFY(!KeyRouter::wantsKey(key, Qt::AltModifier, KeyRouter::Policy::Menu));
        key.modifiers = Qt::ShiftModifier;
        QVERIFY(!KeyRouter::wantsKey(key));
        key = {.key = Qt::Key_F5, .modifiers = Qt::NoModifier, .vk = VK_F5};
        QVERIFY(!KeyRouter::wantsKey(key)); // the program's (refresh)

        // Ctrl++ / Ctrl+= / Ctrl+- / Ctrl+0 (main and numpad) route for UI & picture zoom.
        for (const UINT vk : {UINT(VK_OEM_PLUS), UINT(VK_ADD), UINT(VK_OEM_MINUS), UINT(VK_SUBTRACT), UINT('0'), UINT(VK_NUMPAD0)}) {
            key = {.key = Qt::Key_Equal, .modifiers = Qt::ControlModifier, .vk = vk};
            QVERIFY2(KeyRouter::wantsKey(key), qPrintable(QString::number(vk, 16)));
            QVERIFY(!KeyRouter::wantsKey(key, Qt::AltModifier, KeyRouter::Policy::Menu));
            // AltGr / Ctrl+Alt without printable text stays free for other programs.
            key.modifiers = Qt::ControlModifier | Qt::AltModifier;
            QVERIFY(!KeyRouter::wantsKey(key));
            // Shift is only allowed with VK_OEM_PLUS (Ctrl+Shift+= -> Ctrl++).
            key.modifiers = Qt::ControlModifier | Qt::ShiftModifier;
            QCOMPARE(KeyRouter::wantsKey(key), vk == UINT(VK_OEM_PLUS));
        }
    }

    // The names of the keys routed, and of those TypeToSearch hands over to
    // the launcher (GitHub #203): Qt's, as its own key events have them.
    void qtKeyNames()
    {
        QCOMPARE(qtKeyOf('A', false, false), int(Qt::Key_A));
        QCOMPARE(qtKeyOf('7', false, true), int(Qt::Key_7));
        QCOMPARE(qtKeyOf(VK_NUMPAD3, false, false), int(Qt::Key_3));
        QCOMPARE(qtKeyOf(VK_TAB, false, false), int(Qt::Key_Tab));
        QCOMPARE(qtKeyOf(VK_TAB, false, true), int(Qt::Key_Backtab));
        QCOMPARE(qtKeyOf(VK_RETURN, false, false), int(Qt::Key_Return));
        QCOMPARE(qtKeyOf(VK_RETURN, true, false), int(Qt::Key_Enter)); // the number pad's
        QCOMPARE(qtKeyOf(VK_BACK, false, false), int(Qt::Key_Backspace));
        QCOMPARE(qtKeyOf(VK_ESCAPE, false, false), int(Qt::Key_Escape));
        QCOMPARE(qtKeyOf(VK_F12, false, false), int(Qt::Key_F12));
        QCOMPARE(qtKeyOf(VK_ADD, false, false), int(Qt::Key_Plus));
        QCOMPARE(qtKeyOf(VK_SUBTRACT, false, false), int(Qt::Key_Minus));
        QCOMPARE(qtKeyOf(VK_NUMPAD0, false, false), int(Qt::Key_0));
        QCOMPARE(qtKeyOf(VK_OEM_1, false, false), 0); // named after its character
        QCOMPARE(qtKeyOf(0xE7, false, false), 0); // VK_PACKET: a character, likewise
    }

    void refreshDoesNotActivateOrResumeDrainedRouting()
    {
        PassiveWindow passive;
        QVERIFY(passive.hwnd);
        const HWND foreground = ::GetForegroundWindow();
        std::atomic<int> keys {0};
        std::atomic<int> drained {0};
        KeyRouter router(nullptr, passive.hwnd, {.key = [&](const auto&) { ++keys; }, .drained = [&] { ++drained; }});
        QVERIFY(router.isActive());
        router.refresh();
        router.drain();
        router.refresh(); // a queued click cannot resume a retired session
        QTRY_COMPARE_WITH_TIMEOUT(drained.load(), 1, 2000);
        QCOMPARE(::GetForegroundWindow(), foreground);
        QVERIFY(!::IsWindowVisible(passive.hwnd));
        QCOMPARE(keys.load(), 0);
    }

    void drainFinishesBeforeCallbackAndIsIdempotent()
    {
        PassiveWindow passive;
        QVERIFY(passive.hwnd);
        std::atomic<int> drained {0};
        std::atomic<bool> finishedInCallback {false};
        std::unique_ptr<KeyRouter> router;
        router = std::make_unique<KeyRouter>(nullptr, passive.hwnd, KeyRouter::Callbacks {.drained = [&] {
            finishedInCallback = router->isFinished();
            ++drained;
        }});
        QVERIFY(router->isActive());
        QVERIFY(!router->isFinished());
        router->drain();
        router->drain(); // hideClipboard + stopKeyRouting call drain() twice
        QTRY_COMPARE_WITH_TIMEOUT(drained.load(), 1, 2000);
        QVERIFY(finishedInCallback.load());
        QVERIFY(router->isFinished());
        const auto start = std::chrono::steady_clock::now();
        router.reset();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        QVERIFY2(elapsed.count() < 500, qPrintable(u"join after finished took %1 ms"_s.arg(elapsed.count())));
        QCOMPARE(drained.load(), 1);

        // Destroying without drain() exits cleanly and never invokes drained.
        std::atomic<int> undrained {0};
        {
            KeyRouter plain(nullptr, passive.hwnd, {.drained = [&] { ++undrained; }});
            QVERIFY(plain.isActive());
            QVERIFY(!plain.isFinished());
        }
        QCOMPARE(undrained.load(), 0);
    }

    // The clipboard hotkey's hold watch (GitHub #107), its keys not down:
    // the hook goes in and out on its own thread, nothing to watch, done at once.
    void holdWatchWithItsKeysUpIsInactive()
    {
        std::atomic<int> notes {0};
        std::atomic<int> drained {0};
        constexpr unsigned all = hotkeysteps::kCtrl | hotkeysteps::kAlt | hotkeysteps::kShift | hotkeysteps::kWin;
        HoldWatch watch(all, VK_F9, {.note = [&](auto, auto) { ++notes; }, .drained = [&] { ++drained; }});
        QVERIFY(!watch.isActive());
        QCOMPARE(QString::fromLatin1(watch.inactiveReason()), u"keys-up"_s);
        QCOMPARE(watch.setupError(), 0ul);
        watch.drain();
        QCOMPARE(drained.load(), 1);
        watch.drain(); // once
        QCOMPARE(drained.load(), 1);
        QCoreApplication::processEvents();
        QCOMPARE(notes.load(), 0);
    }

    void sessionSummarySurvivesDiagnosticExport()
    {
        PassiveWindow passive;
        QVERIFY(passive.hwnd);
        KeyRouter router(nullptr, passive.hwnd, {.key = [](const auto&) {}});
        QVERIFY(router.isActive());
        const QJsonObject status = router.diagnosticStatus();
        // Use the same journal and ZIP exporter as Settings. Session totals
        // must survive export, without a second diagnostics entry point.
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        diagnostics::initialize(profile.path());
        const auto disableLog = qScopeGuard([] { diagnostics::initialize({}); });
        diagnostics::record(u"clipboard-key-routing-stop"_s, {{u"session"_s, 1}, {u"router"_s, status}});
        const auto exported = diagnostics::exportArchive(*diagnostics::current(), profile.path(), {}, {});
        QVERIFY2(exported.error.isEmpty(), qPrintable(exported.error));
        QFile archive(exported.path);
        QVERIFY(archive.open(QIODevice::ReadOnly));
        const QByteArray packed = archive.readAll();
        mz_zip_archive zip {};
        QVERIFY(mz_zip_reader_init_mem(&zip, packed.constData(), static_cast<size_t>(packed.size()), 0));
        const auto closeZip = qScopeGuard([&] { mz_zip_reader_end(&zip); });
        size_t bytes = 0;
        void* journal = mz_zip_reader_extract_file_to_heap(&zip, "events.jsonl", &bytes, 0);
        QVERIFY(journal);
        const auto freeJournal = qScopeGuard([&] { mz_free(journal); });
        const QJsonObject record = QJsonDocument::fromJson(
            QByteArray(static_cast<const char*>(journal), static_cast<qsizetype>(bytes)).trimmed()).object();
        QCOMPARE(record.value(u"event"_s).toString(), u"clipboard-key-routing-stop"_s);
        QCOMPARE(record.value(u"details"_s).toObject().value(u"router"_s).toObject(), status);
    }

    void liveRoutingRefreshAndDrain()
    {
        if (qEnvironmentVariableIntValue("WINSHUN_KEYROUTER_LIVE") != 1)
            QSKIP("Opt-in desktop test: uses guarded keys in its own temporary receiver only.");
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {u"--key-receiver"_s});
        QVERIFY(child.waitForStarted(5000));
        QByteArray output;
        QTRY_VERIFY_WITH_TIMEOUT((output += child.readAll()).contains("window:"), 5000);
        const QByteArray handle = output.mid(output.indexOf("window:") + 7).split('\n').front().trimmed();
        const HWND target = reinterpret_cast<HWND>(handle.toULongLong());
        QVERIFY(target && ::IsWindow(target));
        const auto close = qScopeGuard([&] {
            ::PostMessageW(target, WM_CLOSE, 0, 0);
            child.waitForFinished(3000);
        });
        QTRY_COMPARE_WITH_TIMEOUT(::GetForegroundWindow(), target, 3000);
        PassiveWindow passive;
        QVERIFY(passive.hwnd);
        ::ShowWindow(passive.hwnd, SW_SHOWNOACTIVATE);

        std::array<std::atomic<int>, 256> presses {};
        std::array<std::atomic<int>, 256> releases {};
        const HHOOK witness = ::SetWindowsHookExW(WH_KEYBOARD_LL, modifierWitness, ::GetModuleHandleW(nullptr), 0);
        QVERIFY(witness);
        const auto unhook = qScopeGuard([&] { ::UnhookWindowsHookEx(witness); });
        KeyRouter router(target, passive.hwnd, {.key = [&](const KeyRouter::Key& key) {
            if (key.vk < presses.size())
                ++(key.press ? presses[key.vk] : releases[key.vk]);
        }});
        QVERIFY(router.isActive());
        const auto tap = [&](WORD vk) { return send(target, vk, false) && send(target, vk, true); };
        QVERIFY(tap('A'));
        QTRY_COMPARE_WITH_TIMEOUT(releases['A'].load(), 1, 2000);
        QVERIFY(tap(VK_DOWN));
        QTRY_COMPARE_WITH_TIMEOUT(releases[VK_DOWN].load(), 1, 2000);

        // Refresh with a key held: its release still belongs to this session.
        QVERIFY(send(target, 'B', false));
        QTRY_COMPARE_WITH_TIMEOUT(presses['B'].load(), 1, 2000);
        router.refresh();
        QTest::qWait(50);
        QVERIFY(send(target, 'B', true));
        QTRY_COMPARE_WITH_TIMEOUT(releases['B'].load(), 1, 2000);
        QCOMPARE(::GetForegroundWindow(), target);
        output += child.readAll();
        QVERIFY2(!output.contains("down:"), output.constData());
        QVERIFY2(!output.contains("up:"), output.constData());
        QVERIFY2(!output.contains("deactivated"), output.constData());

        // The session summary must distinguish a missing callback from an
        // intentional pass-through. A hidden panel leaves G in the receiver.
        ::ShowWindow(passive.hwnd, SW_HIDE);
        QVERIFY(tap('G'));
        QTRY_VERIFY_WITH_TIMEOUT((output += child.readAll()).contains("down:71"), 2000);
        const QJsonObject status = router.diagnosticStatus();
        QCOMPARE(status.value(u"routed_presses"_s).toInteger(), 3);
        QCOMPARE(status.value(u"window_hidden"_s).toInteger(), 1);
        QCOMPARE(status.value(u"non_modifier_presses"_s).toInteger(), 4);
        QCOMPARE(status.value(u"refreshes"_s).toInteger(), 1);
        QCOMPARE(status.value(u"refresh_failed"_s).toInteger(), 0);
        ::ShowWindow(passive.hwnd, SW_SHOWNOACTIVATE);
        output.clear();

        // Explicit recovery still routes subsequent typing and leaves the
        // receiver's input field in front. No IME or clipboard is involved.
        QVERIFY(tap('C'));
        QTRY_VERIFY2_WITH_TIMEOUT(releases['C'].load() == 1,
            qPrintable(u"C presses=%1 releases=%2; receiver=%3"_s.arg(presses['C'].load()).arg(releases['C'].load())
                .arg(QString::fromUtf8(output + child.readAll()))), 2000);
        router.refresh();
        QTest::qWait(50);
        QVERIFY(tap('D'));
        QTRY_COMPARE_WITH_TIMEOUT(releases['D'].load(), 1, 2000);
        QCOMPARE(::GetForegroundWindow(), target);
        output += child.readAll();
        QVERIFY2(!output.contains("down:68"), output.constData());
        QVERIFY2(!output.contains("up:68"), output.constData());
        QVERIFY2(!output.contains("deactivated"), output.constData());

        // A real modifier release must reach older hooks unchanged. Replacing
        // it with our own injected release leaves remappers' physical state
        // pressed (GitHub #91: Shift+click, then Enter and Ctrl+V).
        for (const WORD vk : {WORD(VK_LSHIFT), WORD(VK_RSHIFT), WORD(VK_LMENU), WORD(VK_RMENU)}) {
            QCOMPARE(::GetForegroundWindow(), target);
            for (const int modifier : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN})
                QVERIFY((::GetAsyncKeyState(modifier) & 0x8000) == 0);
            std::array<INPUT, 2> input {};
            for (auto& key : input) {
                key.type = INPUT_KEYBOARD;
                key.ki.wVk = vk;
                key.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
                key.ki.dwExtraInfo = kTestInput;
                if (vk == VK_RMENU)
                    key.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            }
            input[1].ki.dwFlags |= KEYEVENTF_KEYUP;
            const auto release = qScopeGuard([&] {
                input[1].ki.dwExtraInfo = kOwnInput;
                ::SendInput(1, &input[1], sizeof(INPUT));
            });
            QCOMPARE(::SendInput(2, input.data(), sizeof(INPUT)), UINT(2));
            QTRY_COMPARE_WITH_TIMEOUT(modifierUps[vk].load(), 1, 2000);
            QTRY_VERIFY_WITH_TIMEOUT((::GetAsyncKeyState(vk) & 0x8000) == 0, 2000);
        }

        router.drain();
        QVERIFY(tap('E'));
        QTRY_VERIFY_WITH_TIMEOUT((output += child.readAll()).contains("down:69"), 2000);
        QCOMPARE(presses['E'].load(), 0);

        output.clear();
        std::array<std::atomic<int>, 256> menuReleases {};
        KeyRouter menuRouter(target, passive.hwnd, {.key = [&](const KeyRouter::Key& key) {
            if (!key.press && key.vk < menuReleases.size())
                ++menuReleases[key.vk];
        }}, Qt::NoModifier, 0, KeyRouter::Policy::Menu);
        QVERIFY(menuRouter.isActive());
        for (const WORD vk : {WORD(VK_LEFT), WORD(VK_RIGHT), WORD(VK_BACK)}) {
            QVERIFY(tap(vk));
            QTRY_COMPARE_WITH_TIMEOUT(menuReleases[vk].load(), 1, 2000);
        }
        QCOMPARE(::GetForegroundWindow(), target);
        output += child.readAll();
        QVERIFY2(!output.contains("down:"), output.constData());
        QVERIFY2(!output.contains("up:"), output.constData());
    }

    // Hooks that are no longer called, with nothing to say so (Windows may
    // take out one that answers late): what is typed goes to the program in
    // front, and none of the window's keys work (GitHub #86). The router
    // must be taking keys again by itself, within a second or so.
    void liveHooksComeBackAfterTheyWereTakenOut()
    {
        if (qEnvironmentVariableIntValue("WINSHUN_KEYROUTER_LIVE") != 1)
            QSKIP("Opt-in desktop test: uses guarded keys in its own temporary receiver only.");
        Receiver receiver;
        QVERIFY(receiver.start());
        PassiveWindow passive;
        QVERIFY(passive.hwnd);
        ::ShowWindow(passive.hwnd, SW_SHOWNOACTIVATE);

        std::array<std::atomic<int>, 256> presses {};
        KeyRouter router(receiver.window, passive.hwnd, {.key = [&](const KeyRouter::Key& key) {
            if (key.press && key.vk < presses.size())
                ++presses[key.vk];
        }});
        QVERIFY(router.isActive());
        const auto tap = [&](WORD vk) { return send(receiver.window, vk, false) && send(receiver.window, vk, true); };
        QVERIFY(tap('A'));
        QTRY_COMPARE_WITH_TIMEOUT(presses['A'].load(), 1, 2000);
        QVERIFY2(!receiver.read().contains("down:"), receiver.output.constData());

        // Out: the next key is the program's, as the report has it.
        router.loseHooksForTest();
        QTest::qWait(50);
        QVERIFY(tap('U'));
        QTRY_VERIFY_WITH_TIMEOUT(receiver.read().contains("down:85"), 2000);
        QCOMPARE(presses['U'].load(), 0);

        // Typing goes on, and is the window's again.
        bool back = false;
        for (int i = 0; i < 15 && !back; ++i) {
            QVERIFY(tap('T'));
            QTest::qWait(200);
            back = presses['T'].load() > 0;
        }
        const QJsonObject status = router.diagnosticStatus();
        QVERIFY2(back, QJsonDocument(status).toJson(QJsonDocument::Compact).constData());
        QCOMPARE(status.value(u"hooks_recovered"_s).toInteger(), 1);
        QCOMPARE(status.value(u"refresh_failed"_s).toInteger(), 0);
        QCOMPARE(::GetForegroundWindow(), receiver.window);

        // And stays so: no key more reaches the program, the hooks are not put in over and over.
        receiver.read();
        receiver.output.clear();
        for (const WORD vk : {WORD('K'), WORD('L'), WORD(VK_DOWN)}) {
            QVERIFY(tap(vk));
            QTRY_COMPARE_WITH_TIMEOUT(presses[vk].load(), 1, 2000);
            QTest::qWait(300);
        }
        QVERIFY2(!receiver.read().contains("down:"), receiver.output.constData());
        QCOMPARE(router.diagnosticStatus().value(u"hooks_recovered"_s).toInteger(), 1);
    }

    // The router started for a window that is not the one in front (another
    // came forward between the look at it and the router's start, with no
    // event to say so): it would stay up taking no key, what is typed going
    // to that program (GitHub #86). The owner is told, as of any other
    // window coming forward, once.
    void liveWindowInFrontUnannouncedIsTold()
    {
        if (qEnvironmentVariableIntValue("WINSHUN_KEYROUTER_LIVE") != 1)
            QSKIP("Opt-in desktop test: uses guarded keys in its own temporary receiver only.");
        Receiver receiver;
        QVERIFY(receiver.start());
        PassiveWindow passive;
        PassiveWindow earlier; // the window that was in front as the owner looked
        QVERIFY(passive.hwnd && earlier.hwnd);
        ::ShowWindow(passive.hwnd, SW_SHOWNOACTIVATE);

        std::atomic<int> keys {0};
        std::atomic<int> told {0};
        std::atomic<HWND> toldOf {nullptr};
        KeyRouter router(earlier.hwnd, passive.hwnd, {.key = [&](const KeyRouter::Key&) { ++keys; },
            .foregroundChanged = [&](HWND window) {
                toldOf = window;
                ++told;
            }});
        QVERIFY(router.isActive());
        QTRY_COMPARE_WITH_TIMEOUT(told.load(), 1, 2000);
        QCOMPARE(toldOf.load(), receiver.window);

        // The keys are that program's, and it is not told again for each.
        const auto tap = [&](WORD vk) { return send(receiver.window, vk, false) && send(receiver.window, vk, true); };
        QVERIFY(tap('G'));
        QVERIFY(tap('H'));
        QTRY_VERIFY_WITH_TIMEOUT(receiver.read().contains("down:72"), 2000);
        QCOMPARE(keys.load(), 0);
        QCOMPARE(told.load(), 1);
        const QJsonObject status = router.diagnosticStatus();
        QCOMPARE(status.value(u"foreground_missed"_s).toInteger(), 1);
        QCOMPARE(status.value(u"other_foreground"_s).toInteger(), 2);
    }

    // An answer that comes too late for Windows (the router's thread held
    // up for more than a second): that press is the program's, whatever the
    // hook says then. Its release must be too, or the key stays down for
    // Windows and its next press goes to the program as well.
    void liveLateAnswerCostsOneKeyOnly()
    {
        if (qEnvironmentVariableIntValue("WINSHUN_KEYROUTER_LIVE") != 1)
            QSKIP("Opt-in desktop test: uses guarded keys in its own temporary receiver only.");
        Receiver receiver;
        QVERIFY(receiver.start());
        PassiveWindow passive;
        QVERIFY(passive.hwnd);
        ::ShowWindow(passive.hwnd, SW_SHOWNOACTIVATE);

        // Whatever comes of it, S is not left down for Windows (nor found so).
        const auto letGo = [] {
            if (::GetAsyncKeyState('S') & 0x8000) {
                INPUT up {};
                up.type = INPUT_KEYBOARD;
                up.ki.wVk = 'S';
                up.ki.wScan = static_cast<WORD>(::MapVirtualKeyW('S', MAPVK_VK_TO_VSC));
                up.ki.dwFlags = KEYEVENTF_KEYUP;
                up.ki.dwExtraInfo = kOwnInput;
                ::SendInput(1, &up, sizeof up);
            }
        };
        letGo();
        const auto after = qScopeGuard(letGo);

        std::array<std::atomic<int>, 256> presses {};
        std::atomic<int> lateMs {1300};
        KeyRouter router(receiver.window, passive.hwnd, {.key = [&](const KeyRouter::Key& key) {
            if (key.press && key.vk < presses.size())
                ++presses[key.vk];
            if (key.press && key.vk == 'S')
                ::Sleep(static_cast<DWORD>(lateMs.exchange(0))); // on the router's thread, inside the hook: once
        }});
        QVERIFY(router.isActive());
        const auto tap = [&](WORD vk) { return send(receiver.window, vk, false) && send(receiver.window, vk, true); };
        QVERIFY(tap('S'));
        QTRY_COMPARE_WITH_TIMEOUT(presses['S'].load(), 1, 3000);
        if (!QTest::qWaitFor([&] { return receiver.read().contains("down:83"); }, 1000))
            QSKIP("Windows waited for the late answer: nothing went to the program.");
        QTRY_VERIFY_WITH_TIMEOUT(receiver.read().contains("up:83"), 2000);
        // Up for Windows too. (Where its limit is short the release went by
        // the hook as well, late_answers 0; where it is a second the release
        // came to the hook, which let it go on, late_answers 1.)
        QVERIFY((::GetAsyncKeyState('S') & 0x8000) == 0);

        // The same key again, once the late answer is through: the window's.
        QTest::qWait(1500);
        receiver.read();
        receiver.output.clear();
        QVERIFY(tap('S'));
        QTRY_COMPARE_WITH_TIMEOUT(presses['S'].load(), 2, 2000);
        QTest::qWait(150);
        QVERIFY2(!receiver.read().contains("down:83"), receiver.output.constData());
        const QJsonObject status = router.diagnosticStatus();
        QCOMPARE(status.value(u"already_held"_s).toInteger(), 0);
        QVERIFY(status.value(u"late_answers"_s).toInteger() <= 1);
    }

    // Draining while a taken key is held keeps eating that key's release,
    // exits as soon as it is let go, and caps out on its own thread if held
    // past the limit.
    void liveDrainWaitsForHeldKeyAndRespectsCap()
    {
        if (qEnvironmentVariableIntValue("WINSHUN_KEYROUTER_LIVE") != 1)
            QSKIP("Opt-in desktop test: uses guarded keys in its own temporary receiver only.");
        Receiver receiver;
        QVERIFY(receiver.start());
        PassiveWindow passive;
        QVERIFY(passive.hwnd);
        ::ShowWindow(passive.hwnd, SW_SHOWNOACTIVATE);

        const auto releaseIfHeld = [](WORD vk) {
            if (::GetAsyncKeyState(vk) & 0x8000) {
                INPUT up {};
                up.type = INPUT_KEYBOARD;
                up.ki.wVk = vk;
                up.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
                up.ki.dwFlags = KEYEVENTF_KEYUP;
                up.ki.dwExtraInfo = kOwnInput;
                ::SendInput(1, &up, sizeof up);
            }
        };
        const auto cleanup = qScopeGuard([&] {
            releaseIfHeld('J');
            releaseIfHeld('K');
        });

        // Case 1: release arrives before the drain cap.
        {
            std::atomic<int> presses {0};
            std::atomic<int> drained {0};
            KeyRouter router(receiver.window, passive.hwnd, {
                .key = [&](const KeyRouter::Key& key) {
                    if (key.press && key.vk == 'J')
                        ++presses;
                },
                .drained = [&] { ++drained; },
            });
            QVERIFY(router.isActive());
            QVERIFY(send(receiver.window, 'J', false));
            QTRY_COMPARE_WITH_TIMEOUT(presses.load(), 1, 2000);
            router.drain(2000ms);
            QTest::qWait(80);
            QCOMPARE(drained.load(), 0);
            QVERIFY(!router.isFinished());
            QVERIFY(send(receiver.window, 'J', true));
            QTRY_COMPARE_WITH_TIMEOUT(drained.load(), 1, 1000);
            QVERIFY(router.isFinished());
            QTest::qWait(50);
            QVERIFY2(!receiver.read().contains("up:74"), receiver.output.constData());
        }

        // Case 2: key stays held past the cap; router finishes on its own thread.
        {
            std::atomic<int> presses {0};
            std::atomic<int> drained {0};
            KeyRouter router(receiver.window, passive.hwnd, {
                .key = [&](const KeyRouter::Key& key) {
                    if (key.press && key.vk == 'K')
                        ++presses;
                },
                .drained = [&] { ++drained; },
            });
            QVERIFY(router.isActive());
            QVERIFY(send(receiver.window, 'K', false));
            QTRY_COMPARE_WITH_TIMEOUT(presses.load(), 1, 2000);
            router.drain(150ms);
            QTRY_COMPARE_WITH_TIMEOUT(drained.load(), 1, 1500);
            QVERIFY(router.isFinished());
            releaseIfHeld('K');
        }
    }
};

int main(int argc, char** argv)
{
    if (argc > 1 && qstrcmp(argv[1], "--key-receiver") == 0)
        return receiver();
    if (argc > 1 && qstrcmp(argv[1], "--shell-receiver") == 0)
        return receiver(true);
    QCoreApplication app(argc, argv);
    KeyRouterTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_keyrouter.moc"
