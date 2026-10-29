#include "platform/HoldWatch.h"
#include "platform/KeyRouter.h"
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
#include <cstdio>

using namespace ws;
using namespace Qt::StringLiterals;

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

int receiver()
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
    ::ImmAssociateContextEx(window, nullptr, 0); // the fixture records raw letters, independent of the user's IME mode
    ::ShowWindow(window, SW_SHOWNORMAL);
    ::SetForegroundWindow(window);
    std::printf("window:%llu\n", static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(window)));
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
    if (vk == VK_DOWN)
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    return ::SendInput(1, &input, sizeof input) == 1;
}

} // namespace

class KeyRouterTest : public QObject {
    Q_OBJECT

private slots:
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
    }
};

int main(int argc, char** argv)
{
    if (argc > 1 && qstrcmp(argv[1], "--key-receiver") == 0)
        return receiver();
    QCoreApplication app(argc, argv);
    KeyRouterTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_keyrouter.moc"
