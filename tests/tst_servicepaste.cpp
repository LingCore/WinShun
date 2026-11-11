// The paste into a program run as administrator, pressed by the index
// service's helper (GitHub #277): which keys a window takes, and, against an
// isolated SYSTEM service and a real administrator's window, the request
// itself (docs/validation/issue277-20261111/harness: it sets the variables
// the live part waits for; without them that part is skipped).

#include "PasteInput.h"
#include "PasteKeys.h"
#include "platform/ServicePaste.h"

#include <QElapsedTimer>
#include <QTest>

#include <windows.h>

using namespace ws;

namespace {
HWND windowFrom(const char* variable)
{
    bool ok = false;
    const qulonglong value = qEnvironmentVariable(variable).toULongLong(&ok);
    return ok ? reinterpret_cast<HWND>(static_cast<quintptr>(value)) : nullptr;
}
} // namespace

class ServicePasteTest : public QObject {
    Q_OBJECT

private slots:
    void keysByWindowClass()
    {
        using pastekeys::Keys;
        QCOMPARE(pastekeys::forWindowClass(L"mintty"), Keys::ShiftInsert);
        QCOMPARE(pastekeys::forWindowClass(L"PuTTY"), Keys::ShiftInsert);
        QCOMPARE(pastekeys::forWindowClass(L"Notepad"), Keys::CtrlV);
        QCOMPARE(pastekeys::forWindowClass(L"ConsoleWindowClass"), Keys::CtrlV);
        QCOMPARE(pastekeys::forWindowClass(L"putty"), Keys::CtrlV); // a class name, as registered
        QCOMPARE(pastekeys::forWindowClass({}), Keys::CtrlV);
    }

    // What the helper sends with no modifier held: the chord and nothing
    // else, every key marked as Win顺's own.
    void theChordAsInput()
    {
        const std::vector<pastekeys::Key> keys = pastekeys::sequence(pastekeys::Keys::CtrlV, 0, false);
        const std::vector<INPUT> inputs = pastekeys::inputs(keys);
        QCOMPARE(inputs.size(), std::size_t {4});
        const WORD expected[] = {VK_CONTROL, 'V', 'V', VK_CONTROL};
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            QCOMPARE(inputs[i].type, DWORD {INPUT_KEYBOARD});
            QCOMPARE(inputs[i].ki.wVk, expected[i]);
            QCOMPARE(bool(inputs[i].ki.dwFlags & KEYEVENTF_KEYUP), i >= 2);
            QVERIFY(isOwnInput(inputs[i].ki.dwExtraInfo));
        }
        const std::vector<INPUT> insert = pastekeys::inputs(pastekeys::sequence(pastekeys::Keys::ShiftInsert, 0, false));
        QCOMPARE(insert.size(), std::size_t {4});
        QCOMPARE(insert[1].ki.wVk, WORD {VK_INSERT});
        QVERIFY(insert[1].ki.dwFlags & KEYEVENTF_EXTENDEDKEY); // not the number pad's 0
    }

    void outcomesByName()
    {
        QCOMPARE(servicepaste::name(servicepaste::Outcome::Pasted), "pasted");
        QCOMPARE(servicepaste::name(servicepaste::Outcome::NoService), "no-service");
        QCOMPARE(servicepaste::name(servicepaste::Outcome::NotOffered), "not-offered");
        QCOMPARE(servicepaste::name(servicepaste::Outcome::Failed), "failed");
    }

    // No service on the pipe asked: said so at once, nothing waited for.
    void noServiceIsSaidAtOnce()
    {
        const QByteArray before = qgetenv("WINSHUN_PIPE");
        qputenv("WINSHUN_PIPE", "WinShun.NoSuchPipe.tst_servicepaste");
        QElapsedTimer timer;
        timer.start();
        const servicepaste::Result result = servicepaste::paste(::GetDesktopWindow());
        const qint64 took = timer.elapsed();
        if (before.isEmpty())
            qunsetenv("WINSHUN_PIPE");
        else
            qputenv("WINSHUN_PIPE", before);
        QCOMPARE(result.outcome, servicepaste::Outcome::NoService);
        QVERIFY(!result);
        QVERIFY2(took < 500, qPrintable(QString::number(took)));
    }

    // Live: WINSHUN_PIPE names an isolated SYSTEM service that takes this
    // program for Win顺's UI; WINSHUN_PASTE_FRONT is an administrator's
    // window in front, WINSHUN_PASTE_BEHIND one that is not.
    void administratorWindowLive()
    {
        const HWND front = windowFrom("WINSHUN_PASTE_FRONT");
        const HWND behind = windowFrom("WINSHUN_PASTE_BEHIND");
        if (!front || !behind || qEnvironmentVariableIsEmpty("WINSHUN_PIPE"))
            QSKIP("needs the elevated harness (an isolated service and administrator windows)");
        QVERIFY(::IsWindow(front) && ::IsWindow(behind));

        // A window of this process's own level: the UI presses its own keys there.
        const HWND own = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"WinShun #277 own level",
            WS_POPUP | WS_VISIBLE, 0, 0, 40, 40, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        QVERIFY(own);
        servicepaste::Result result = servicepaste::paste(own);
        ::DestroyWindow(own);
        QCOMPARE(result.outcome, servicepaste::Outcome::Failed);
        QCOMPARE(result.error, DWORD {ERROR_NOT_SUPPORTED});

        // An administrator's window that is not in front: no key goes anywhere.
        QVERIFY(::GetAncestor(::GetForegroundWindow(), GA_ROOT) == front);
        result = servicepaste::paste(behind);
        QCOMPARE(result.outcome, servicepaste::Outcome::Failed);
        QCOMPARE(result.error, DWORD {ERROR_NOT_READY});

        // The one in front: pressed. The harness's window says what it got.
        QVERIFY(::GetAncestor(::GetForegroundWindow(), GA_ROOT) == front);
        QElapsedTimer timer;
        timer.start();
        result = servicepaste::paste(front);
        qInfo("paste through the service: %s, error %lu, %lld ms", servicepaste::name(result.outcome), result.error,
            timer.elapsed());
        QCOMPARE(result.outcome, servicepaste::Outcome::Pasted);
        timer.restart();
        result = servicepaste::paste(front);
        qInfo("again: %s, error %lu, %lld ms", servicepaste::name(result.outcome), result.error, timer.elapsed());
        QCOMPARE(result.outcome, servicepaste::Outcome::Pasted);
    }
};

QTEST_GUILESS_MAIN(ServicePasteTest)
#include "tst_servicepaste.moc"
