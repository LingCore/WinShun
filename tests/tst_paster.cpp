#include "platform/Paster.h"

#include <QtTest>
#include <commctrl.h>

using namespace ws;

class PasterTest : public QObject {
    Q_OBJECT
    HWND m_root = nullptr;
    HWND m_edit = nullptr;
    HWND m_previous = nullptr;
    int m_pastes = 0;
    int m_keys = 0;

    static LRESULT CALLBACK editProc(HWND hwnd, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR context)
    {
        auto* self = reinterpret_cast<PasterTest*>(context);
        if (message == WM_KEYDOWN && w == 'V' && (::GetKeyState(VK_CONTROL) & 0x8000)) {
            ++self->m_keys;
            return 0; // don't read or change the user's clipboard
        }
        if (message == WM_PASTE) {
            ++self->m_pastes;
            // Exercise the real Edit's selection/replacement, without touching
            // the desktop clipboard. Only the source bytes are a fixture.
            ::SendMessageW(hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"pasted"));
            return 0;
        }
        return ::DefSubclassProc(hwnd, message, w, l);
    }
    static UINT WINAPI refused(UINT, LPINPUT, int) { ::SetLastError(ERROR_ACCESS_DENIED); return 0; }
    static UINT WINAPI partial(UINT, LPINPUT, int) { return 1; }

private slots:
    void init()
    {
        m_previous = ::GetForegroundWindow();
        m_root = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"WinShun paste regression",
            WS_OVERLAPPEDWINDOW, 80, 80, 360, 160, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        QVERIFY(m_root);
        m_edit = ::CreateWindowExW(0, L"EDIT", L"before selected after", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            12, 12, 320, 60, m_root, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        QVERIFY(m_edit);
        QVERIFY(::SetWindowSubclass(m_edit, editProc, 1, reinterpret_cast<DWORD_PTR>(this)));
        ::ShowWindow(m_root, SW_SHOWNORMAL);
        ::SetForegroundWindow(m_root);
        ::SetFocus(m_edit);
        QTRY_VERIFY_WITH_TIMEOUT(paste::isForeground(m_root), 1000);
        ::SendMessageW(m_edit, EM_SETSEL, 7, 15);
        m_pastes = m_keys = 0;
    }
    void cleanup()
    {
        if (m_root) ::DestroyWindow(m_root);
        if (::IsWindow(m_previous)) ::SetForegroundWindow(m_previous);
        m_root = m_edit = nullptr;
    }
    void refusedKeysUseEditPasteOnce()
    {
        const auto result = paste::sendPasteKeys(m_root, nullptr, refused);
        QCOMPARE(result.method, paste::Result::Method::EditMessage);
        QCOMPARE(result.sent, UINT(0));
        QCOMPARE(result.error, DWORD(ERROR_ACCESS_DENIED));
        QCOMPARE(m_pastes, 1);
        wchar_t text[80] {};
        ::GetWindowTextW(m_edit, text, 80);
        QCOMPARE(QString::fromWCharArray(text), QStringLiteral("before pasted after"));
    }
    void partialSendNeverRepeatsPaste()
    {
        const auto result = paste::sendPasteKeys(m_root, nullptr, partial);
        QVERIFY(!result);
        QCOMPARE(result.sent, UINT(1));
        QCOMPARE(m_pastes, 0);
    }
    void readonlyAndUnknownControlsDoNotReceiveFallback()
    {
        ::SendMessageW(m_edit, EM_SETREADONLY, TRUE, 0);
        QVERIFY(!paste::sendPasteKeys(m_root, nullptr, refused));
        QCOMPARE(m_pastes, 0);
        ::SetFocus(m_root);
        QVERIFY(!paste::sendPasteKeys(m_root, nullptr, refused));
        QVERIFY(!paste::sendPasteKeys(nullptr, nullptr, refused));
    }
    void realKeysReachFocusedEdit()
    {
        const auto result = paste::sendPasteKeys(m_root);
        QCOMPARE(result.method, paste::Result::Method::Keys);
        QTRY_COMPARE_WITH_TIMEOUT(m_keys, 1, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(m_pastes, 1, 1000); // Edit translates Ctrl+V to WM_PASTE
    }
};

QTEST_GUILESS_MAIN(PasterTest)
#include "tst_paster.moc"
