#include "CaptureSurfaceBridge.h"
#include "CaptureSurfaceProtocol.h"

#include <QCursor>
#include <QEvent>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>

#include <windows.h>

#include <cstring>
#include <thread>
#include <vector>

using namespace ws::capsurface;
using namespace ws::capture;
using namespace Qt::StringLiterals;

namespace {

bool writeAll(HANDLE pipe, std::span<const std::byte> data)
{
    std::size_t off = 0;
    while (off < data.size()) {
        DWORD n = 0;
        if (!::WriteFile(pipe, data.data() + off, static_cast<DWORD>(data.size() - off), &n, nullptr) || n == 0)
            return false;
        off += n;
    }
    return true;
}

bool readFrame(HANDLE pipe, Type& type, std::vector<std::byte>& payload)
{
    std::byte hdr[kHeaderBytes] {};
    std::size_t off = 0;
    while (off < kHeaderBytes) {
        DWORD n = 0;
        if (!::ReadFile(pipe, hdr + off, static_cast<DWORD>(kHeaderBytes - off), &n, nullptr) || n == 0)
            return false;
        off += n;
    }
    std::uint32_t len = 0;
    if (!parseHeader(std::span<const std::byte, kHeaderBytes>(hdr, kHeaderBytes), type, len))
        return false;
    payload.resize(len);
    off = 0;
    while (off < len) {
        DWORD n = 0;
        if (!::ReadFile(pipe, payload.data() + off, static_cast<DWORD>(len - off), &n, nullptr) || n == 0)
            return false;
        off += n;
    }
    return true;
}

} // namespace

class CaptureSurfaceTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }

    void pipeNameValidation()
    {
        QVERIFY(isValidPipeName(L"WinShun.CaptureSurface.42.0123456789abcdef0123456789abcdef", 42));
        QVERIFY(!isValidPipeName(L"WinShun.CaptureSurface.42.0123456789abcdef0123456789abcdef", 43));
        QVERIFY(!isValidPipeName(L"WinShun.CaptureSurface.0.0123456789abcdef0123456789abcdef", 0));
        QVERIFY(!isValidPipeName(L"WinShun.CaptureSurface.42.0123456789ABCDEF0123456789abcdef", 42));
        QVERIFY(!isValidPipeName(L"WinShun.CaptureSurface.42.0123456789abcdef", 42));
        QVERIFY(!isValidPipeName(L"..\\WinShun.CaptureSurface.42.0123456789abcdef0123456789abcdef", 42));
        QVERIFY(!isValidPipeName(L"WinShun.Clipboard.42.0123456789abcdef0123456789abcdef", 42));
    }

    void protocolRoundTripsAndBounds()
    {
        {
            InitMsg in {1, {{0x1111, 0, 0, 2560, 1600}, {0x2222, 2560, 0, 1920, 1080}}};
            auto raw = encode(in);
            Type t {};
            std::uint32_t len = 0;
            QVERIFY(parseHeader(std::span<const std::byte, kHeaderBytes>(raw.data(), kHeaderBytes), t, len));
            QCOMPARE(t, Type::Init);
            InitMsg out;
            QVERIFY(decode(std::span<const std::byte>(raw.data() + kHeaderBytes, len), out));
            QCOMPARE(out, in);

            // Out-of-range activeIndex and zero monitor dimensions must be rejected.
            InitMsg bad = in;
            bad.activeIndex = 5;
            auto badRaw = encode(bad);
            QVERIFY(!decode(std::span<const std::byte>(badRaw.data() + kHeaderBytes, badRaw.size() - kHeaderBytes), out));
            bad = {0, {{0x1111, 0, 0, 0, 100}}};
            badRaw = encode(bad);
            QVERIFY(!decode(std::span<const std::byte>(badRaw.data() + kHeaderBytes, badRaw.size() - kHeaderBytes), out));
        }
        {
            ReadyMsg in {1, 0, {0xAA01, 0xBB02}};
            auto raw = encode(in);
            ReadyMsg out;
            QVERIFY(decode(std::span<const std::byte>(raw.data() + kHeaderBytes, raw.size() - kHeaderBytes), out));
            QCOMPARE(out, in);
        }
        {
            CursorMsg in {CursorKind::Rgba, 0, 0x123456789abcdef0ull, 2, 2, 1, 1,
                {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255}};
            auto raw = encode(in);
            CursorMsg out;
            QVERIFY(decode(std::span<const std::byte>(raw.data() + kHeaderBytes, raw.size() - kHeaderBytes), out));
            QCOMPARE(out, in);

            // Truncated RGBA bytes must fail.
            QVERIFY(!decode(std::span<const std::byte>(raw.data() + kHeaderBytes, raw.size() - kHeaderBytes - 2), out));
        }
        {
            ImeStateMsg in {true, 0, 120, 240, 2, 24};
            auto raw = encode(in);
            ImeStateMsg out;
            QVERIFY(decode(std::span<const std::byte>(raw.data() + kHeaderBytes, raw.size() - kHeaderBytes), out));
            QCOMPARE(out, in);
        }
        {
            MouseMsg in {MouseKind::Wheel, MouseButton::None, kBtnLeft, kModShift | kModCtrl, 512, 384, -120};
            auto raw = encode(in);
            MouseMsg out;
            QVERIFY(decode(std::span<const std::byte>(raw.data() + kHeaderBytes, raw.size() - kHeaderBytes), out));
            QCOMPARE(out, in);
        }
        {
            KeyMsg in {true, false, true, kModCtrl, VK_ESCAPE, 1, u"\x1b"};
            auto raw = encode(in);
            KeyMsg out;
            QVERIFY(decode(std::span<const std::byte>(raw.data() + kHeaderBytes, raw.size() - kHeaderBytes), out));
            QCOMPARE(out, in);
        }
        {
            ImeEventMsg in {ImeKind::Commit, 2, u"测试"};
            auto raw = encode(in);
            ImeEventMsg out;
            QVERIFY(decode(std::span<const std::byte>(raw.data() + kHeaderBytes, raw.size() - kHeaderBytes), out));
            QCOMPARE(out, in);
        }
        {
            ClosedMsg in {CloseReason::Deactivated};
            auto raw = encode(in);
            ClosedMsg out;
            QVERIFY(decode(std::span<const std::byte>(raw.data() + kHeaderBytes, raw.size() - kHeaderBytes), out));
            QCOMPARE(out, in);
        }
    }

    void keyMappingCoversCaptureShortcuts()
    {
        QCOMPARE(qtKeyFromSurface(VK_ESCAPE, false, false, {}), int(Qt::Key_Escape));
        QCOMPARE(qtKeyFromSurface(VK_RETURN, false, false, {}), int(Qt::Key_Return));
        QCOMPARE(qtKeyFromSurface(VK_RETURN, true, false, {}), int(Qt::Key_Enter));
        QCOMPARE(qtKeyFromSurface(VK_TAB, false, false, {}), int(Qt::Key_Tab));
        QCOMPARE(qtKeyFromSurface(VK_TAB, false, true, {}), int(Qt::Key_Backtab));
        QCOMPARE(qtKeyFromSurface(VK_SHIFT, false, true, {}), int(Qt::Key_Shift));
        QCOMPARE(qtKeyFromSurface(VK_OEM_COMMA, false, false, u","_s), int(Qt::Key_Comma));
        QCOMPARE(qtKeyFromSurface(VK_OEM_PERIOD, false, false, u"."_s), int(Qt::Key_Period));
        QCOMPARE(qtKeyFromSurface(VK_OEM_2, false, false, u"/"_s), int(Qt::Key_Slash));
        QCOMPARE(qtKeyFromSurface(VK_OEM_2, false, true, u"?"_s), int(Qt::Key_Question));
        QCOMPARE(qtKeyFromSurface(VK_OEM_4, false, false, u"["_s), int(Qt::Key_BracketLeft));
        QCOMPARE(qtKeyFromSurface(VK_OEM_4, false, true, u"{"_s), int(Qt::Key_BraceLeft));
        QCOMPARE(qtKeyFromSurface(VK_OEM_6, false, false, u"]"_s), int(Qt::Key_BracketRight));
        QCOMPARE(qtKeyFromSurface(VK_OEM_6, false, true, u"}"_s), int(Qt::Key_BraceRight));
        QCOMPARE(qtKeyFromSurface(VK_SNAPSHOT, false, false, {}), int(Qt::Key_Print));
        QCOMPARE(qtKeyFromSurface('W', false, false, u"w"_s), int(Qt::Key_W));
        QCOMPARE(qtKeyFromSurface(VK_F1, false, false, {}), int(Qt::Key_F1));
    }

    void bridgeRelaysMouseKeyImeAndCursorToInactiveWindow()
    {
        QQmlEngine engine;
        QQmlComponent comp(&engine);
        comp.setData(R"(
            import QtQuick
            Window {
                id: win
                width: 400
                height: 300
                property int presses: 0
                property int releases: 0
                property int dblClicks: 0
                property int moves: 0
                property int wheels: 0
                property int lastKey: 0
                property int escCount: 0
                property real lastX: -1
                property real lastY: -1
                property alias editText: editor.text
                function focusEditor() { editor.forceActiveFocus() }
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    onPositionChanged: (m) => { win.moves++; win.lastX = m.x; win.lastY = m.y }
                    onPressed: (m) => { win.presses++; win.lastX = m.x; win.lastY = m.y }
                    onReleased: (m) => { win.releases++ }
                    onDoubleClicked: (m) => { win.dblClicks++ }
                    onWheel: (w) => { win.wheels += w.angleDelta.y }
                }
                Item {
                    id: keyRoot
                    focus: true
                    Keys.onPressed: (e) => {
                        win.lastKey = e.key
                        if (e.key === Qt.Key_Escape) win.escCount++
                        e.accepted = true
                    }
                }
                TextInput {
                    id: editor
                    x: 40; y: 150; width: 200; height: 30
                }
            }
        )", QUrl(u"qrc:/test.qml"_s));
        std::unique_ptr<QQuickWindow> win(qobject_cast<QQuickWindow*>(comp.create()));
        QVERIFY2(win != nullptr, qPrintable(comp.errorString()));
        win->setProperty("_q_showWithoutActivating", true);
        win->show();
        QCoreApplication::processEvents();

        // Set a custom RGBA cursor before start to test RGBA cursor sync.
        QImage curImg(16, 16, QImage::Format_ARGB32_Premultiplied);
        curImg.fill(QColor(255, 255, 255, 255));
        win->setCursor(QCursor(QPixmap::fromImage(curImg), 8, 8));

        CaptureSurfaceBridge bridge;
        QSignalSpy readySpy(&bridge, &CaptureSurfaceBridge::ready);
        QSignalSpy closedSpy(&bridge, &CaptureSurfaceBridge::closedExternally);

        QString capturedPipe;
        bool closeCalled = false;
        std::thread mockHelper;
        const auto cleanup = qScopeGuard([&] {
            bridge.stop();
            if (mockHelper.joinable())
                mockHelper.join();
        });
        bool helperGotInit = false;
        bool helperGotRgbaCursor = false;
        bool helperGotImeOn = false;

        const bool started = bridge.start(
            reinterpret_cast<HWND>(static_cast<std::uintptr_t>(0x1234)),
            {{win.get(), QRect(100, 200, 400, 300), 1.0}},
            0,
            [&](HWND, const QString& pipeName, std::function<void(CaptureSurfaceBridge::OpenReply)> done) {
                capturedPipe = pipeName;
                // Connect from a worker thread BEFORE calling done() to verify
                // race-free handling of early pipe connections.
                mockHelper = std::thread([pipeName, &helperGotInit, &helperGotRgbaCursor, &helperGotImeOn] {
                    const std::wstring full = L"\\\\.\\pipe\\" + pipeName.toStdWString();
                    HANDLE p = ::CreateFileW(full.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                    if (p == INVALID_HANDLE_VALUE)
                        return;
                    Type t {};
                    std::vector<std::byte> payload;
                    if (readFrame(p, t, payload) && t == Type::Init) {
                        InitMsg init;
                        if (decode(payload, init) && init.monitors.size() == 1) {
                            helperGotInit = true;
                            ReadyMsg ready {1, 0, {0xBEEF01}};
                            writeAll(p, encode(ready));
                        }
                    }
                    // Send mouse move, press, release, dblclick, wheel, Esc key.
                    writeAll(p, encode(MouseMsg {MouseKind::Move, MouseButton::None, 0, 0, 150, 260, 0}));
                    writeAll(p, encode(MouseMsg {MouseKind::Press, MouseButton::Left, kBtnLeft, 0, 150, 260, 0}));
                    writeAll(p, encode(MouseMsg {MouseKind::Release, MouseButton::Left, 0, 0, 150, 260, 0}));
                    writeAll(p, encode(MouseMsg {MouseKind::DblClick, MouseButton::Left, kBtnLeft, 0, 150, 260, 0}));
                    writeAll(p, encode(MouseMsg {MouseKind::Release, MouseButton::Left, 0, 0, 150, 260, 0}));
                    writeAll(p, encode(MouseMsg {MouseKind::Wheel, MouseButton::None, 0, 0, 150, 260, 120}));
                    writeAll(p, encode(KeyMsg {true, false, false, 0, VK_ESCAPE, 1, u"\x1b"}));

                    // Drain cursor/IME messages from plugin until Close or IME enable.
                    while (readFrame(p, t, payload)) {
                        if (t == Type::SetCursor) {
                            CursorMsg cm;
                            if (decode(payload, cm) && cm.kind == CursorKind::Rgba && cm.width == 16 && cm.height == 16)
                                helperGotRgbaCursor = true;
                        } else if (t == Type::SetIme) {
                            ImeStateMsg im;
                            if (decode(payload, im) && im.enabled && !helperGotImeOn) {
                                helperGotImeOn = true;
                                writeAll(p, encode(ImeEventMsg {ImeKind::Commit, 0, u"你好"}));
                            }
                        } else if (t == Type::Close) {
                            break;
                        }
                    }
                    ::CloseHandle(p);
                });
                QTimer::singleShot(30, &bridge, [done = std::move(done)] {
                    done({true, ::GetCurrentProcessId(), {}, ERROR_SUCCESS});
                });
            },
            [&] { closeCalled = true; });
        QVERIFY(started);
        QVERIFY(isValidPipeName(capturedPipe.toStdWString(), ::GetCurrentProcessId()));

        bridge.setOverlaysReady();
        QTRY_COMPARE(readySpy.count(), 1);
        QVERIFY(bridge.isSurfaceHwnd(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(0xBEEF01))));

        QTRY_VERIFY(win->property("escCount").toInt() == 1);
        QVERIFY(win->property("moves").toInt() >= 1);
        QCOMPARE(win->property("lastX").toReal(), 50.0);
        QCOMPARE(win->property("lastY").toReal(), 60.0);
        QVERIFY(win->property("presses").toInt() >= 2);
        QCOMPARE(win->property("releases").toInt(), 2);
        QCOMPARE(win->property("dblClicks").toInt(), 1);
        QCOMPARE(win->property("wheels").toInt(), 120);

        // Focus QML TextInput -> bridge sends SetIme(enabled=true) -> helper sends Chinese IME commit.
        QMetaObject::invokeMethod(win.get(), "focusEditor");
        QTRY_COMPARE(win->property("editText").toString(), u"你好"_s);

        bridge.stop();
        if (mockHelper.joinable())
            mockHelper.join();
        QVERIFY(helperGotInit);
        QVERIFY(helperGotRgbaCursor);
        QVERIFY(helperGotImeOn);
        QVERIFY(closeCalled);
        QCOMPARE(closedSpy.count(), 0);
    }

    void rejectsMismatchedPeerPid()
    {
        QQuickWindow win;
        win.create();
        CaptureSurfaceBridge bridge;
        QSignalSpy failedSpy(&bridge, &CaptureSurfaceBridge::failed);
        std::thread rogue;
        const auto cleanup = qScopeGuard([&] {
            bridge.stop();
            if (rogue.joinable())
                rogue.join();
        });

        QVERIFY(bridge.start(
            reinterpret_cast<HWND>(static_cast<std::uintptr_t>(0x1234)),
            {{&win, QRect(0, 0, 200, 200), 1.0}},
            0,
            [&](HWND, const QString& pipeName, std::function<void(CaptureSurfaceBridge::OpenReply)> done) {
                rogue = std::thread([pipeName] {
                    const std::wstring full = L"\\\\.\\pipe\\" + pipeName.toStdWString();
                    HANDLE p = ::CreateFileW(full.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                    if (p != INVALID_HANDLE_VALUE) {
                        ::Sleep(200);
                        ::CloseHandle(p);
                    }
                });
                // Report a different expected PID than the connecting process's actual PID.
                QTimer::singleShot(20, &bridge, [done = std::move(done)] {
                    done({true, ::GetCurrentProcessId() + 999999u, {}, ERROR_SUCCESS});
                });
            },
            [] {}));
        bridge.setOverlaysReady();
        QTRY_COMPARE(failedSpy.count(), 1);
        QCOMPARE(failedSpy.at(0).at(0).toString(), u"peer-mismatch"_s);
        if (rogue.joinable())
            rogue.join();
    }
};

QTEST_MAIN(CaptureSurfaceTest)
#include "tst_capturesurface.moc"
