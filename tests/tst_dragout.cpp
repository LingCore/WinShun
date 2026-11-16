// What a drop target gets when results are dragged out (dragout::dataObject):
// the files as CF_HDROP and as the shell's item list, a copy preferred;
// and, opt-in, a real drag with either button (dragout::drag).
#include "DragOut.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;
using namespace ws;

namespace {

QStringList dropped(IDataObject* data)
{
    FORMATETC format {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium {};
    if (FAILED(data->GetData(&format, &medium)))
        return {u"(no CF_HDROP)"_s};
    QStringList out;
    const auto drop = static_cast<HDROP>(medium.hGlobal);
    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        wchar_t path[MAX_PATH] {};
        ::DragQueryFileW(drop, i, path, MAX_PATH);
        out.append(QString::fromWCharArray(path));
    }
    ::ReleaseStgMedium(&medium);
    return out;
}

DWORD preferredEffect(IDataObject* data)
{
    FORMATETC format {static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)), nullptr,
        DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium {};
    if (FAILED(data->GetData(&format, &medium)))
        return 0xFFFFFFFF;
    const DWORD effect = *static_cast<DWORD*>(::GlobalLock(medium.hGlobal));
    ::GlobalUnlock(medium.hGlobal);
    ::ReleaseStgMedium(&medium);
    return effect;
}

// The formats it offers, by name.
QStringList formats(IDataObject* data)
{
    QStringList out;
    IEnumFORMATETC* list = nullptr;
    if (FAILED(data->EnumFormatEtc(DATADIR_GET, &list)))
        return out;
    FORMATETC f {};
    while (list->Next(1, &f, nullptr) == S_OK) {
        wchar_t name[128] {};
        out.append(::GetClipboardFormatNameW(f.cfFormat, name, 128) ? QString::fromWCharArray(name)
                                                                    : QString::number(f.cfFormat));
        if (f.ptd)
            ::CoTaskMemFree(f.ptd);
    }
    list->Release();
    return out;
}

// A drop target of the test's own: what a drag offered it and with which
// button, taking a copy of what is dropped (nothing is written anywhere).
class Target final : public IDropTarget {
public:
    DWORD allowed = 0; // what DragEnter was offered
    DWORD keys = 0; // the buttons seen while over it
    bool droppedIn = false;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
    {
        if (!out)
            return E_POINTER;
        if (id == IID_IUnknown || id == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --m_refs; } // on the test's stack
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*, DWORD state, POINTL, DWORD* effect) override
    {
        allowed = *effect;
        keys |= state;
        *effect &= DROPEFFECT_COPY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD state, POINTL, DWORD* effect) override
    {
        keys |= state;
        *effect &= DROPEFFECT_COPY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINTL, DWORD* effect) override
    {
        droppedIn = true;
        *effect &= DROPEFFECT_COPY;
        return S_OK;
    }

private:
    ULONG m_refs = 1;
};

// The button releases a window was posted, by message.
std::vector<UINT> g_released;

LRESULT CALLBACK recordingProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_LBUTTONUP || message == WM_RBUTTONUP)
        g_released.push_back(message);
    return ::DefWindowProcW(window, message, w, l);
}

HWND topWindow(const wchar_t* className, WNDPROC proc, const RECT& at)
{
    WNDCLASSW wc {};
    wc.lpfnWndProc = proc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(GRAY_BRUSH));
    wc.lpszClassName = className;
    ::RegisterClassW(&wc);
    const HWND window = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, className, L"", WS_POPUP | WS_VISIBLE,
        at.left, at.top, at.right - at.left, at.bottom - at.top, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    ::UpdateWindow(window);
    return window;
}

void pump()
{
    MSG message {};
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
}

POINT centre(const RECT& r) { return {(r.left + r.right) / 2, (r.top + r.bottom) / 2}; }

void click(DWORD flags)
{
    INPUT input {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    ::SendInput(1, &input, sizeof input);
}

void key(WORD vk)
{
    INPUT input[2] {};
    input[0].type = input[1].type = INPUT_KEYBOARD;
    input[0].ki.wVk = input[1].ki.wVk = vk;
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(2, input, sizeof input[0]);
}

} // namespace

class TestDragOut : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(SUCCEEDED(::OleInitialize(nullptr))); }
    void cleanupTestCase() { ::OleUninitialize(); }

    void itemsOfSeveralFolders()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(u"甲/文件夹"_s));
        QVERIFY(QDir(dir.path()).mkpath(u"乙"_s));
        const QString a = QDir::toNativeSeparators(dir.filePath(u"甲/报告 2026.pdf"_s));
        const QString b = QDir::toNativeSeparators(dir.filePath(u"乙/notes.txt"_s));
        const QString folder = QDir::toNativeSeparators(dir.filePath(u"甲/文件夹"_s));
        for (const QString& f : {a, b}) {
            QFile file(f);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
        }
        const auto data = dragout::dataObject({a, b, folder});
        QVERIFY(data);
        QCOMPARE(dropped(data.Get()), (QStringList {a, b, folder})); // as picked, from different folders
        const QStringList offered = formats(data.Get());
        QVERIFY2(offered.contains(QString::fromWCharArray(CFSTR_SHELLIDLIST)), qPrintable(offered.join(u", "))); // what Explorer reads
        QCOMPARE(preferredEffect(data.Get()), DWORD(DROPEFFECT_COPY));
    }

    void qtImagePath()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(u"图片 export"_s));
        // Clipboard exports start as Qt paths, including spaces and Unicode.
        const QString path = dir.filePath(u"图片 export/image_261.png"_s);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("image bytes");
        file.close();
        const auto data = dragout::dataObject({path});
        QVERIFY(data);
        QCOMPARE(dropped(data.Get()), QStringList {QDir::toNativeSeparators(path)});
        QCOMPARE(preferredEffect(data.Get()), DWORD(DROPEFFECT_COPY));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("image bytes")); // data object never changes the source
    }

    void missingOnes()
    {
        QTemporaryDir dir;
        const QString kept = QDir::toNativeSeparators(dir.filePath(u"kept.txt"_s));
        QFile file(kept);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        // Deleted since the search: left out, the rest still goes.
        const auto data = dragout::dataObject({QDir::toNativeSeparators(dir.filePath(u"gone.txt"_s)), kept});
        QVERIFY(data);
        QCOMPARE(dropped(data.Get()), QStringList {kept});
        QVERIFY(!dragout::dataObject({QDir::toNativeSeparators(dir.filePath(u"gone.txt"_s))}));
        QVERIFY(!dragout::dataObject({}));
    }

    // Copied (Ctrl+C): the files and nothing a program could paste as text
    // instead. WPS pasted a copied picture's path from the text (GitHub #130).
    void copiedFilesOnly()
    {
        // Never looked up: a server that went away must not hold the copy up.
        const QString offline = u"\\\\192.0.2.1\\share\\照片 1.jpg"_s;
        const QString local = u"C:/no such folder/b.png"_s;
        const auto data = dragout::fileNamesObject({offline, local});
        QVERIFY(data);
        QCOMPARE(dropped(data.Get()), (QStringList {offline, QDir::toNativeSeparators(local)}));
        QCOMPARE(preferredEffect(data.Get()), DWORD(DROPEFFECT_COPY));
        const QStringList offered = formats(data.Get());
        QCOMPARE(offered.size(), 2); // CF_HDROP and Preferred DropEffect
        QVERIFY2(offered.contains(QString::number(CF_HDROP)), qPrintable(offered.join(u", ")));
        for (const UINT text : {CF_UNICODETEXT, CF_TEXT, CF_OEMTEXT}) {
            FORMATETC f {static_cast<CLIPFORMAT>(text), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            QVERIFY(data->QueryGetData(&f) != S_OK);
        }
        QVERIFY(!dragout::fileNamesObject({}));
    }

    // A drop moves only when Explorer's menu said so, after a right drag (#338).
    void allowedEffects()
    {
        using dragout::Button;
        QCOMPARE(dragout::allowedEffects(Button::Left, true), DWORD(DROPEFFECT_COPY | DROPEFFECT_LINK));
        QCOMPARE(dragout::allowedEffects(Button::Left, false), DWORD(DROPEFFECT_COPY));
        QCOMPARE(dragout::allowedEffects(Button::Right, true), DWORD(DROPEFFECT_COPY | DROPEFFECT_LINK | DROPEFFECT_MOVE));
        QCOMPARE(dragout::allowedEffects(Button::Right, false), DWORD(DROPEFFECT_COPY | DROPEFFECT_MOVE));
    }

    // A real drag with the real mouse, into a window of the test's own: it goes
    // on with the button that started it, the right one offers a move, and the
    // source is posted that button's release. Opt-in: it moves the pointer.
    void realDrag_data()
    {
        QTest::addColumn<bool>("right");
        QTest::addColumn<bool>("picture");
        QTest::newRow("right, own picture") << true << true;
        QTest::newRow("right, shell's picture") << true << false;
        QTest::newRow("left, own picture") << false << true;
    }

    void realDrag()
    {
        if (qEnvironmentVariableIsEmpty("WINSHUN_TEST_REAL_INPUT"))
            QSKIP("Moves the real mouse: set WINSHUN_TEST_REAL_INPUT=1, hands off the mouse");
        QFETCH(bool, right);
        QFETCH(bool, picture);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir::toNativeSeparators(dir.filePath(u"拖出 test.txt"_s));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();

        RECT work {};
        QVERIFY(::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0));
        const RECT sourceAt {work.left + 100, work.top + 100, work.left + 300, work.top + 300};
        const RECT targetAt {work.left + 400, work.top + 100, work.left + 600, work.top + 300};
        const HWND source = topWindow(L"WinShunTestDragSource", recordingProc, sourceAt);
        const HWND targetWindow = topWindow(L"WinShunTestDropTarget", ::DefWindowProcW, targetAt);
        Target target;
        QVERIFY(SUCCEEDED(::RegisterDragDrop(targetWindow, &target)));
        POINT before {};
        ::GetCursorPos(&before);
        const auto restore = qScopeGuard([&] {
            ::RevokeDragDrop(targetWindow);
            ::DestroyWindow(targetWindow);
            ::DestroyWindow(source);
            ::SetCursorPos(before.x, before.y);
        });
        pump();
        const POINT from = centre(sourceAt);
        const POINT to = centre(targetAt);
        ::SetCursorPos(from.x, from.y);
        pump();
        if (::WindowFromPoint(from) != source || ::WindowFromPoint(to) != targetWindow)
            QSKIP("Another topmost window covers the test's windows");

        g_released.clear();
        const int vk = right ? VK_RBUTTON : VK_LBUTTON;
        click(right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN);
        for (int i = 0; i < 200 && ::GetKeyState(vk) >= 0; ++i) { // as a press reaches the row: by message
            pump();
            ::Sleep(5);
        }
        QVERIFY(::GetKeyState(vk) < 0);
        std::atomic<bool> over = false;
        std::thread mover([&] {
            ::Sleep(150);
            for (int step = 1; step <= 10; ++step) {
                ::SetCursorPos(from.x + (to.x - from.x) * step / 10, from.y + (to.y - from.y) * step / 10);
                ::Sleep(30);
            }
            ::Sleep(150);
            over = ::WindowFromPoint(to) == targetWindow;
            if (!over)
                key(VK_ESCAPE); // never drop on someone else's window
            click(right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP);
        });
        QImage image;
        if (picture) {
            image = QImage(48, 48, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::darkCyan);
        }
        const dragout::Outcome outcome = dragout::drag({path}, source, true, image, QPoint(24, 24));
        mover.join();
        pump();

        QVERIFY2(over, "the pointer left the test's window (was the mouse moved?)");
        QCOMPARE(int(outcome), int(dragout::Outcome::Dropped));
        QVERIFY(target.droppedIn);
        QCOMPARE((target.allowed & DROPEFFECT_MOVE) != 0, right);
        QVERIFY(target.keys & (right ? MK_RBUTTON : MK_LBUTTON));
        QCOMPARE(g_released, std::vector<UINT> {UINT(right ? WM_RBUTTONUP : WM_LBUTTONUP)});
        QVERIFY(QFile::exists(path)); // a copy at most: the file stays
    }
};

QTEST_GUILESS_MAIN(TestDragOut)
#include "tst_dragout.moc"
