// Uses an owned window and temporary commands. No global keystrokes, user
// files or shell configuration are changed; foreground is restored afterward.
#include "FolderMenu.h"
#include "Favorites.h"
#include "FileIconProvider.h"
#include "Phosphor.h"
#include "PhosphorGlyph.h"
#include "SystemTheme.h"
#include "platform/WindowEffects.h"

#include <QDir>
#include <memory>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>
#include <QScopeGuard>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickItemGrabResult>
#include <QScreen>
#include <QStyleHints>
#include <QFontDatabase>
#include <QCursor>
#include <QPixmap>
#include <QPointer>
#include <QWheelEvent>
#include <cmath>
#include <commctrl.h>
#include <dwmapi.h>

namespace {
// Owned desktop-shaped windows: never changes Explorer or desktop settings.
class DesktopFixture {
public:
    HWND root = nullptr, view = nullptr, list = nullptr;
    DesktopFixture(const wchar_t* host)
    {
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        for (const wchar_t* name : {host, L"SHELLDLL_DefView"}) {
            WNDCLASSW wc {};
            wc.lpfnWndProc = ::DefWindowProcW;
            wc.hInstance = instance;
            wc.lpszClassName = name;
            ::RegisterClassW(&wc);
        }
        INITCOMMONCONTROLSEX controls {sizeof controls, ICC_LISTVIEW_CLASSES};
        ::InitCommonControlsEx(&controls);
        root = ::CreateWindowExW(
            0, host, L"WinShun owned desktop test", WS_POPUP, 120, 120, 480, 320, nullptr, nullptr, instance, nullptr);
        view = ::CreateWindowExW(
            0, L"SHELLDLL_DefView", L"", WS_CHILD | WS_VISIBLE, 0, 0, 480, 320, root, nullptr, instance, nullptr);
        list = ::CreateWindowExW(
            0, WC_LISTVIEWW, L"", WS_CHILD | LVS_ICON, 0, 0, 480, 320, view, nullptr, instance, nullptr);
    }
    ~DesktopFixture() { ::DestroyWindow(root); }
    POINT point() const
    {
        POINT p {300, 220};
        ::ClientToScreen(root, &p);
        return p;
    }
    void show(bool viewVisible)
    {
        ::ShowWindow(view, viewVisible ? SW_SHOW : SW_HIDE);
        ::SetWindowPos(root, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
};
} // namespace

using namespace ws;
using namespace Qt::StringLiterals;

class FolderMenuTest : public QObject {
    Q_OBJECT

    static QQuickItem* visualItem(QQuickItem* root, const QString& name)
    {
        if (root->objectName() == name)
            return root;
        for (auto* child : root->childItems()) {
            if (auto* found = visualItem(child, name))
                return found;
        }
        return nullptr;
    }

    static int entryIn(const QVariantList& entries, const QString& title)
    {
        for (const auto& value : entries) {
            const auto row = value.toMap();
            if (row.value(u"text"_s).toString() == title && row.contains(u"action"_s))
                return row.value(u"action"_s).toInt();
            if (const int child = entryIn(row.value(u"children"_s).toList(), title); child >= 0)
                return child;
        }
        return -1;
    }
    static int entry(const FolderMenu& menu, const QString& title) { return entryIn(menu.entries(), title); }

    // Where the pointer is, for the menu (FolderMenu::setCursorSource) and
    // for Qt: QTest moves Qt's pointer, never the system's. Going from one
    // window to another, the one left is told, as Windows tells it.
    struct Pointer {
        QPoint at {-30000, -30000};
        QPointer<QWindow> over;
        void leave()
        {
            if (over) {
                QEvent left(QEvent::Leave);
                QCoreApplication::sendEvent(over, &left);
            }
            over = nullptr;
        }
        void moveTo(QWindow* window, QPoint local)
        {
            if (over != window)
                leave();
            over = window;
            at = window->mapToGlobal(local);
            QTest::mouseMove(window, local);
        }
        // A window came under the pointer, which has not moved: Windows tells
        // that one where the pointer is.
        void cameUnder(QWindow* window)
        {
            if (over != window)
                leave();
            over = window;
            QTest::mouseMove(window, window->mapFromGlobal(at));
        }
        void click(QWindow* window, QPoint local)
        {
            moveTo(window, local);
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, local);
        }
    };

    // The production menu (FolderMenuWindow.qml) for `menu`.
    static std::unique_ptr<QObject> load(QQmlEngine& engine, FolderMenu& menu)
    {
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(WINSHUN_QML_DIR) + u"/FolderMenuWindow.qml"_s));
        std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"folderMenu"_s, QVariant::fromValue(&menu)}}));
        if (!object)
            qWarning().noquote() << component.errorString();
        return object;
    }
    static QVariant value(QObject* object, const QString& script)
    {
        QQmlExpression expression(qmlContext(object), object, script);
        const QVariant result = expression.evaluate();
        if (expression.hasError())
            qWarning().noquote() << expression.error().toString();
        return result;
    }
    // The window showing column `visual` of the menu (0: the menu's own).
    static QQuickWindow* column(QObject* menu, int visual)
    {
        return visual == 0 ? qobject_cast<QQuickWindow*>(menu)
                           : menu->findChild<QQuickWindow*>(u"folderMenuColumn%1"_s.arg(visual));
    }
    static QQuickItem* rowItem(QQuickWindow* window, int index)
    {
        return window ? visualItem(window->contentItem(), u"folderMenuEntry%1"_s.arg(index)) : nullptr;
    }
    // A point on row `index`, `across` of the way along it.
    static QPoint on(QQuickWindow* window, int index, qreal across = 0.3)
    {
        auto* item = rowItem(window, index);
        return item ? item->mapToScene(QPointF(item->width() * across, item->height() / 2)).toPoint() : QPoint(-1, -1);
    }
    static QPoint arrowOf(QQuickWindow* window, int index)
    {
        auto* item = window ? visualItem(window->contentItem(), u"folderMenuArrow%1"_s.arg(index)) : nullptr;
        return item ? item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint() : QPoint(-1, -1);
    }
    static qreal topOf(QQuickWindow* window, int index)
    {
        auto* item = rowItem(window, index);
        return item ? item->mapToGlobal(QPointF(0, 0)).y() : -1e6;
    }
    static bool aligned(qreal a, qreal b) { return std::abs(a - b) <= 1.01; }
    // Inside the work area `area`, 8 px from its edges (a pixel either way).
    static bool inside(const QRect& area, const QWindow* window)
    {
        const QRect r = window->geometry();
        return r.x() >= area.x() + 7 && r.y() >= area.y() + 7 && r.x() + r.width() <= area.x() + area.width() - 7
            && r.y() + r.height() <= area.y() + area.height() - 7;
    }
    static HWND handle(QWindow* window) { return reinterpret_cast<HWND>(window->winId()); }
    // A window on the screen's own pixels (Qt's whole ones are 1.5 of them at 150 %).
    static QRect screenRect(QWindow* window)
    {
        RECT r {};
        ::GetWindowRect(handle(window), &r);
        return QRect(QPoint(r.left, r.top), QSize(r.right - r.left, r.bottom - r.top));
    }
    // The edge Windows draws inside a window, in the screen's pixels: DWM's on
    // Windows 11 (DWMWA_VISIBLE_FRAME_BORDER_THICKNESS), WindowEdge's on 10.
    static int frameBorder(QWindow* window)
    {
        UINT thickness = 0;
        if (SUCCEEDED(::DwmGetWindowAttribute(handle(window), 37, &thickness, sizeof thickness)) && thickness > 0)
            return static_cast<int>(thickness);
        return std::max(1, qRound(window->devicePixelRatio()));
    }
    // `next` beside `before` (side 1: on its right), the border they share
    // one line on the screen's pixels (#348: one off, it was a 3 px seam).
    // Empty when so, else what is not.
    static QByteArray seam(QWindow* before, QWindow* next, int side)
    {
        const QRect a = screenRect(before), b = screenRect(next);
        const int border = frameBorder(before);
        const bool shared = side > 0 ? b.x() == a.x() + a.width() - border : b.x() + b.width() == a.x() + border;
        return shared ? QByteArray()
                      : u"side %1: %2..%3 then %4..%5, border %6"_s.arg(side).arg(a.x()).arg(a.x() + a.width())
                            .arg(b.x()).arg(b.x() + b.width()).arg(border).toUtf8();
    }
    // Where row `index` starts on the screen's own pixels.
    static qreal screenTop(QQuickWindow* window, int index)
    {
        auto* item = rowItem(window, index);
        return item ? screenRect(window).y() + item->mapToScene(QPointF(0, 0)).y() * window->devicePixelRatio() : -1e6;
    }
    // Whether `top` is in front of `below` on the screen.
    static bool inFront(QWindow* top, QWindow* below)
    {
        for (HWND h = ::GetWindow(handle(top), GW_HWNDNEXT); h; h = ::GetWindow(h, GW_HWNDNEXT)) {
            if (h == handle(below))
                return true;
        }
        return false;
    }
    // The wheel turned over `local` of `window` (towards the user: negative).
    static void wheel(QWindow* window, QPoint local, int notches)
    {
        QWheelEvent event(QPointF(local), QPointF(window->mapToGlobal(local)), QPoint(), QPoint(0, 120 * notches),
            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QTest::lastMouseTimestamp += 300; // on QTest's clock, after its pointer's moves
        event.setTimestamp(QTest::lastMouseTimestamp);
        QCoreApplication::sendEvent(window, &event);
    }
    static QVariantMap group(const QString& text, const QVariantList& children)
    {
        return {{u"text"_s, text}, {u"glyph"_s, u"\uE8B7"_s}, {u"children"_s, children}};
    }
    static QVariantMap leaf(const QString& text) { return {{u"text"_s, text}, {u"glyph"_s, u"\uE8B7"_s}, {u"action"_s, 0}}; }
    // Somewhere on the screen away from the system's pointer, which the tests
    // never move: a window that came under it would get its moves too.
    static QRect awayFromPointer(QSize size)
    {
        const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
        const QPoint cursor = QCursor::pos();
        const int left = screen.x() + 16, right = screen.x() + screen.width() - size.width() - 16;
        const int top = screen.y() + 16, bottom = screen.y() + screen.height() - size.height() - 16;
        for (const QPoint corner : {QPoint(left, top), QPoint(right, top), QPoint(left, bottom), QPoint(right, bottom)}) {
            if (!QRect(corner, size).adjusted(-24, -24, 24, 24).contains(cursor))
                return QRect(corner, size);
        }
        return QRect(QPoint(left, top), size);
    }
    // A plain window under the menu for screenEvidence; none unless
    // WINSHUN_MENU_EVIDENCE asks for evidence.
    static std::unique_ptr<QQuickWindow> backdrop(const QRect& area, bool dark)
    {
        if (qEnvironmentVariableIsEmpty("WINSHUN_MENU_EVIDENCE"))
            return {};
        auto window = std::make_unique<QQuickWindow>();
        window->setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
        window->setProperty("_q_showWithoutActivating", true);
        window->setColor(dark ? QColor(0x1f, 0x23, 0x29) : QColor(0xe3, 0xe8, 0xee));
        window->setGeometry(area.adjusted(-40, -40, 40, 40));
        window->show();
        return window;
    }
    // The screen where the columns are, as Windows shows them (DWM's corners,
    // edges and shadows), over the backdrop: WINSHUN_MENU_EVIDENCE only.
    static bool screenEvidence(const QList<QQuickWindow*>& windows, const QString& name)
    {
        const QString folder = qEnvironmentVariable("WINSHUN_MENU_EVIDENCE");
        if (folder.isEmpty())
            return true;
        QTest::qWait(400); // revealed (FolderMenu::showColumn), the highlights where they go
        QRect bounds;
        for (const QQuickWindow* window : windows)
            bounds |= window->geometry();
        QScreen* screen = windows.first()->screen();
        const QRect whole = screen->geometry();
        bounds = bounds.adjusted(-20, -20, 20, 20) & whole;
        const QPixmap shot = screen->grabWindow(0, bounds.x() - whole.x(), bounds.y() - whole.y(), bounds.width(), bounds.height());
        const QString scale = QString::number(qRound(screen->devicePixelRatio() * 100));
        return QDir().mkpath(folder) && shot.save(folder + u'/' + name + u'-' + scale + u".png"_s);
    }

private slots:
    void initTestCase()
    {
        phosphor::loadFonts();
        qmlRegisterUncreatableType<FolderMenu>("WinShun", 1, 0, "FolderMenu", u"Provided by the application"_s);
        qmlRegisterSingletonType<SystemTheme>(
            "WinShun", 1, 0, "SystemTheme", [](QQmlEngine*, QJSEngine*) -> QObject* { return new SystemTheme; });
        qmlRegisterType<PhosphorGlyph>("WinShun", 1, 0, "PhosphorGlyph");
        const QUrl folder = QUrl::fromLocalFile(QStringLiteral(WINSHUN_QML_DIR) + u'/');
        qmlRegisterSingletonType(folder.resolved(QUrl(u"Theme.qml"_s)), "WinShun", 1, 0, "Theme");
        QFile data(QCoreApplication::applicationDirPath() + u"/fonts/AlibabaPuHuiTi-3-55-Regular.ttf"_s);
        QVERIFY(data.open(QIODevice::ReadOnly));
        const int font = QFontDatabase::addApplicationFontFromData(data.readAll());
        if (font >= 0)
            QGuiApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font).first()));
    }

    void hiddenDesktopIcons_data()
    {
        QTest::addColumn<QString>("host");
        QTest::addColumn<bool>("viewVisible");
        QTest::newRow("progman-background") << QStringLiteral("Progman") << false;
        QTest::newRow("progman-defview") << QStringLiteral("Progman") << true;
        QTest::newRow("worker-background") << QStringLiteral("WorkerW") << false;
        QTest::newRow("worker-defview") << QStringLiteral("WorkerW") << true;
    }

    void hiddenDesktopIcons()
    {
        QFETCH(QString, host);
        QFETCH(bool, viewVisible);
        const HWND previous = ::GetForegroundWindow();
        const auto restore = qScopeGuard([&] {
            if (::IsWindow(previous))
                ::SetForegroundWindow(previous);
        });
        DesktopFixture desktop(reinterpret_cast<const wchar_t*>(host.utf16()));
        QVERIFY(desktop.root && desktop.view && desktop.list);
        desktop.show(viewVisible);
        const POINT pt = desktop.point();
        const HWND clicked = ::WindowFromPoint(pt);
        QCOMPARE(clicked, viewVisible ? desktop.view : desktop.root);
        const auto hit = folderview::classifyMenu(clicked, pt);
        QCOMPARE(hit.surface, folderview::Surface::Desktop);
        QCOMPARE(hit.view, clicked);
        folderview::BlankTrace trace;
        QVERIFY(folderview::isBlank(hit, &trace));
        QCOMPARE(QByteArray(trace.by), QByteArray("desktop-background"));
        QWindow menuWindow;
        FolderMenu menu(nullptr, nullptr);
        menu.setWindow(&menuWindow);
        menu.setOptions(false, true, false);
        QSignalSpy popup(&menu, &FolderMenu::popupRequested);
        menu.doubleClicked(clicked, pt);
        QTest::qWait(30);
        QCOMPARE(popup.size(), 0); // the desktop switch still takes priority
    }

    void desktopCanvasGuards()
    {
        const HWND previous = ::GetForegroundWindow();
        const auto restore = qScopeGuard([&] {
            if (::IsWindow(previous))
                ::SetForegroundWindow(previous);
        });
        DesktopFixture desktop(L"Progman");
        desktop.show(true);
        const POINT pt = desktop.point();
        auto hit = folderview::classifyMenu(desktop.view, pt);
        QCOMPARE(hit.surface, folderview::Surface::Desktop);
        QVERIFY(folderview::isBlank(hit));
        // Showing the icons covers the background. Keep the original icon
        // list path, and reject a delayed background hit after that change.
        ::ShowWindow(desktop.list, SW_SHOW);
        QCOMPARE(::WindowFromPoint(pt), desktop.list);
        QVERIFY(!folderview::isBlank(hit));
        QCOMPARE(folderview::classifyMenu(desktop.list, pt).view, desktop.list);
        QCOMPARE(folderview::classify(desktop.view, pt).surface, folderview::Surface::None);
        ::ShowWindow(desktop.list, SW_HIDE);
        const HWND edit = ::CreateWindowExW(0, L"Edit", L"", WS_CHILD | WS_VISIBLE, 0, 0, 480, 320, desktop.view,
            nullptr, ::GetModuleHandleW(nullptr), nullptr);
        QVERIFY(edit);
        QCOMPARE(::WindowFromPoint(pt), edit);
        QCOMPARE(folderview::classifyMenu(edit, pt).surface, folderview::Surface::None);
        QVERIFY(!folderview::isBlank(hit));
        ::DestroyWindow(edit);
        QVERIFY(folderview::isBlank(hit));
        ::DestroyWindow(desktop.view);
        desktop.view = desktop.list = nullptr;
        // A similarly named application window without a desktop view is
        // not accepted; the actual shell's wallpaper hosts are separate.
        QCOMPARE(folderview::classifyMenu(desktop.root, pt).surface, folderview::Surface::None);
        QVERIFY(!folderview::sameDesktop(desktop.root, ::GetShellWindow()));
    }

    void commandsAndSettings()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const HWND previous = ::GetForegroundWindow();
        auto restore = qScopeGuard([previous] {
            if (previous && ::IsWindow(previous))
                ::SetForegroundWindow(previous);
        });
        QWindow source;
        source.setTitle(u"WinShun folder menu test"_s);
        source.resize(320, 160);
        source.show();
        QVERIFY(QTest::qWaitForWindowExposed(&source));
        win::bringToFront(&source);
        QTRY_COMPARE_WITH_TIMEOUT(::GetForegroundWindow(), reinterpret_cast<HWND>(source.winId()), 3000);

        Favorites favorites(temporary.filePath(u"favorites.json"_s));
        const QString output = temporary.filePath(u"command-ran.txt"_s);
        QString quotedOutput = QDir::toNativeSeparators(output);
        quotedOutput.replace(u'\'', u"''"_s);
        Favorite command {Favorite::Kind::Command, u"winshun-command:menu-test"_s, u"Test command"_s, u"test"_s,
            u"powershell.exe -NoProfile -NonInteractive -Command \"[IO.File]::WriteAllText('"_s + quotedOutput
                + u"',(Get-Location).Path)\""_s,
            temporary.path()};
        command.silent = true;
        QVERIFY(favorites.add(command));
        QWindow menuWindow;
        FolderMenu menu(nullptr, &favorites);
        menu.setWindow(&menuWindow);
        menu.setRecordHistory(false);
        menu.setGroups(true, false, false);
        QSignalSpy changed(&menu, &FolderMenu::entriesChanged);
        QSignalSpy settings(&menu, &FolderMenu::settingsRequested);
        QSignalSpy notice(&menu, &FolderMenu::notice);

        const auto open = [&] {
            changed.clear();
            menu.hotkeyPressed();
            return changed.wait(5000);
        };
        QVERIFY(open());
        const int settingsIndex = entry(menu, u"Win顺 设置…"_s);
        const int commandIndex = entry(menu, command.name);
        QVERIFY(settingsIndex >= 0);
        QVERIFY(commandIndex >= 0);
        menu.trigger(settingsIndex);
        QCOMPARE(settings.size(), 1);

        QVERIFY(open());
        menu.trigger(entry(menu, command.name));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(output), 10000);
        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QDir::cleanPath(QString::fromUtf8(file.readAll())), QDir::cleanPath(temporary.path()));
        QCOMPARE(notice.size(), 0);

        // The visible entry keeps the command's identity, not its old row.
        QVERIFY(open());
        const int removedIndex = entry(menu, command.name);
        QVERIFY(removedIndex >= 0);
        QVERIFY(favorites.remove(command.path));
        menu.trigger(removedIndex);
        QCOMPARE(notice.size(), 1);
        QVERIFY(notice.first().first().toString().contains(u"已移除"));
    }

    // Copy path (GitHub #137): on the desktop the folder is the Desktop
    // directory; the entry puts it on the clipboard as plain native text, and
    // its switch hides it. Where there is no folder (the hotkey over a window
    // that is not a file manager) the entry is not offered at all.
    void copyPath()
    {
        const HWND previous = ::GetForegroundWindow();
        const auto restore = qScopeGuard([&] {
            if (::IsWindow(previous))
                ::SetForegroundWindow(previous);
        });
        DesktopFixture desktop(L"WorkerW");
        QVERIFY(desktop.root && desktop.view && desktop.list);
        desktop.show(true);
        const POINT pt = desktop.point();
        const HWND clicked = ::WindowFromPoint(pt);
        QCOMPARE(clicked, desktop.view);
        // The menu only comes up where it was asked for: put the fixture in front.
        std::unique_ptr<QWindow> wrapped(QWindow::fromWinId(reinterpret_cast<WId>(desktop.root)));
        win::bringToFront(wrapped.get());
        QTRY_COMPARE_WITH_TIMEOUT(::GetForegroundWindow(), desktop.root, 3000);

        QWindow menuWindow;
        FolderMenu menu(nullptr, nullptr);
        menu.setWindow(&menuWindow);
        menu.setRecordHistory(false);
        QString copied; // never the real clipboard: it feeds Win+V and Win顺 history
        menu.setClipboardWriter([&](const QString& text) { copied = text; });
        menu.setOptions(false, false, true);
        menu.setGroups(false, true, false);
        QSignalSpy changed(&menu, &FolderMenu::entriesChanged);
        const auto open = [&] {
            changed.clear();
            menu.doubleClicked(clicked, pt);
            return changed.wait(5000);
        };

        QVERIFY(open());
        const int copy = entry(menu, u"复制路径"_s);
        QVERIFY(copy >= 0);
        QVERIFY(entry(menu, u"Win顺 设置…"_s) > copy); // between search and the shells, above Settings
        menu.trigger(copy);
        QCOMPARE(copied,
            QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)));

        menu.setItems(true, false, true, true, true, true);
        QVERIFY(open());
        QCOMPARE(entry(menu, u"复制路径"_s), -1);
        QVERIFY(entry(menu, u"Win顺 设置…"_s) >= 0);
    }

    void noFolderNoCopyPath()
    {
        QWindow source;
        source.setTitle(u"WinShun folder menu test"_s);
        source.resize(320, 160);
        source.show();
        QVERIFY(QTest::qWaitForWindowExposed(&source));
        win::bringToFront(&source);
        QTRY_COMPARE_WITH_TIMEOUT(::GetForegroundWindow(), reinterpret_cast<HWND>(source.winId()), 3000);
        QWindow menuWindow;
        FolderMenu menu(nullptr, nullptr);
        menu.setWindow(&menuWindow);
        menu.setRecordHistory(false);
        QSignalSpy changed(&menu, &FolderMenu::entriesChanged);
        menu.hotkeyPressed();
        QVERIFY(changed.wait(5000));
        QCOMPARE(entry(menu, u"复制路径"_s), -1);
        QVERIFY(entry(menu, u"Win顺 设置…"_s) >= 0);
    }
    void directFolderGroups()
    {
        QVariantList children;
        for (int i = 0; i < 12; ++i)
            children.append(QVariantMap {{u"text"_s, u"项目 %1"_s.arg(i)}, {u"action"_s, i}, {u"tooltip"_s, u"C:\\项目\\%1"_s.arg(i)}});
        const QString title = u"最近的文件夹"_s;
        for (const int count : {-1, 0, 1, 3, 10, 50}) {
            const auto rows = FolderMenu::groupEntries(title, u"folder"_s, children, count);
            if (count <= 0) {
                QCOMPARE(rows.size(), 1);
                QCOMPARE(rows.first().toMap().value(u"children"_s).toList(), children);
                continue;
            }
            const int shown = std::min(count, 10);
            QCOMPARE(rows.size(), shown + 2); // non-selectable heading, direct rows, More
            QVERIFY(rows.first().toMap().value(u"disabled"_s).toBool());
            QVERIFY(rows.first().toMap().value(u"heading"_s).toBool());
            QVariantList reconstructed = rows.sliced(1, shown);
            const auto more = rows.last().toMap();
            QCOMPARE(more.value(u"pageTitle"_s).toString(), title);
            reconstructed.append(more.value(u"children"_s).toList());
            QCOMPARE(reconstructed, children); // every action preserved once, in order
        }
        const auto exact = FolderMenu::groupEntries(title, {}, children.first(3), 3);
        QCOMPARE(exact.size(), 4); // no empty More page
        QVERIFY(!exact.last().toMap().contains(u"children"_s));
        const auto empty = FolderMenu::groupEntries(title, {}, {}, 5);
        QCOMPARE(empty.size(), 1);
        QVERIFY(empty.first().toMap().value(u"disabled"_s).toBool());
    }

    void shortPagePathTip_data()
    {
        QTest::addColumn<QPoint>("position");
        QTest::newRow("top-left") << QPoint(8, 8);
        QTest::newRow("bottom-right") << QPoint(432, 505);
    }

    // Run with QT_QPA_PLATFORM=offscreen: this uses the production menu QML
    // without shell discovery, global keys or foreground-window changes.
    void shortPagePathTip()
    {
        QFETCH(QPoint, position);
        FolderMenu menu(nullptr, nullptr);
        const QString path = u"C:\\项目资料\\"_s + QString(90, u'长') + u"\\outputs"_s;
        menu.previewEntries({QVariantMap {{u"text"_s, u"outputs"_s}, {u"tooltip"_s, path}}});
        QQmlEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine,
            QUrl::fromLocalFile(QStringLiteral(WINSHUN_QML_DIR) + u"/FolderMenuWindow.qml"_s));
        std::unique_ptr<QObject> object(component.createWithInitialProperties({
            {u"folderMenu"_s, QVariant::fromValue(&menu)}, {u"availableArea"_s, QRectF(0, 0, 800, 600)},
            {u"popupAnchor"_s, position}}));
        QVERIFY2(object, qPrintable(component.errorString()));
        auto* window = qobject_cast<QQuickWindow*>(object.get());
        QVERIFY(window);
        window->setPosition(position);
        window->show();
        QTRY_VERIFY(visualItem(window->contentItem(), u"folderMenuEntry0"_s));
        auto* row = visualItem(window->contentItem(), u"folderMenuEntry0"_s);
        object->setProperty("hoveredText", path);
        object->setProperty("hoveredEntry", QVariant::fromValue(row));
        auto* tip = object->findChild<QQuickWindow*>(u"menuPathTip"_s);
        QVERIFY(tip);
        QTRY_VERIFY(tip->isVisible());
        QCOMPARE(tip->transientParent(), window);
        QVERIFY(tip->flags().testFlag(Qt::WindowDoesNotAcceptFocus));
        QVERIFY(tip->flags().testFlag(Qt::WindowTransparentForInput));
        QVERIFY(QRect(8, 8, 784, 584).contains(tip->geometry()));
        const auto* label = visualItem(tip->contentItem(), u"menuPathLabel"_s);
        QVERIFY(label);
        QCOMPARE(label->property("text").toString(), path);
        QVERIFY(label->property("paintedHeight").toReal() <= label->height() + 1);
        QVERIFY(!label->property("truncated").toBool());
        const QString evidence = qEnvironmentVariable("WINSHUN_MENU_EVIDENCE");
        if (!evidence.isEmpty()) {
            QVERIFY(QDir().mkpath(evidence));
            QTest::qWait(50);
            QVERIFY(tip->grabWindow().save(evidence + u"/path-tip-%1.png"_s.arg(QLatin1String(QTest::currentDataTag()))));
        }
        window->hide();
        QTRY_VERIFY(!tip->isVisible());
        QCOMPARE(warnings.count(), 0);
    }

    void pagesAndLongLists_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("round");
        QTest::newRow("light") << false << true;
        QTest::newRow("dark") << true << true;
        QTest::newRow("light-square") << false << false;
        QTest::newRow("dark-square") << true << false;
    }

    void pagesAndLongLists()
    {
        QFETCH(bool, dark);
        QFETCH(bool, round);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const HWND previous = ::GetForegroundWindow();
        const auto scheme = QGuiApplication::styleHints()->colorScheme();
        const bool previousCorners = win::roundedCorners();
        auto restore = qScopeGuard([&] {
            QGuiApplication::styleHints()->setColorScheme(scheme);
            SystemTheme::setRoundedCorners(previousCorners);
            if (previous && ::IsWindow(previous))
                ::SetForegroundWindow(previous);
        });
        QGuiApplication::styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
        SystemTheme::setRoundedCorners(round);
        QWindow source;
        source.setTitle(u"WinShun menu page test"_s);
        source.setGeometry(QRect(awayFromPointer(QSize(900, 680)).topLeft(), QSize(320, 160)));
        source.show();
        QVERIFY(QTest::qWaitForWindowExposed(&source));
        win::bringToFront(&source);
        const HWND sourceHandle = reinterpret_cast<HWND>(source.winId());
        QTRY_COMPARE_WITH_TIMEOUT(::GetForegroundWindow(), sourceHandle, 3000);

        Favorites favorites(temporary.filePath(u"favorites.json"_s));
        const QStringList labels {u"项目资料"_s, u"年度报告"_s, u"设计素材"_s, u"客户合同"_s,
            u"会议记录"_s, u"归档备份"_s, u"财务报表"_s, u"产品文档"_s};
        for (int i = 0; i < 50; ++i) {
            const QString path = temporary.filePath(u"folder-%1"_s.arg(i));
            QVERIFY(QDir().mkpath(path));
            QVERIFY(favorites.add({Favorite::Kind::Folder, QDir::toNativeSeparators(path),
                i < labels.size() ? labels[i] : u"文件夹 %1：一段很长的名称，用来检查省略和滚动"_s.arg(i), {}}));
        }
        const QString output = temporary.filePath(u"menu-command-ran.txt"_s);
        QString quoted = QDir::toNativeSeparators(output);
        quoted.replace(u'\'', u"''"_s);
        Favorite command {Favorite::Kind::Command, u"winshun-command:page-test"_s, u"写入验证文件"_s, u"verify"_s,
            u"powershell.exe -NoProfile -NonInteractive -Command \"[IO.File]::WriteAllText('"_s + quoted
                + u"','menu-ok')\""_s,
            temporary.path()};
        command.silent = true;
        QVERIFY(favorites.add(command));
        QVERIFY(favorites.add({Favorite::Kind::File, temporary.filePath(u"readme.txt"_s), u"使用说明"_s, {}}));
        QVERIFY(favorites.add({Favorite::Kind::App, u"shell:AppsFolder\\notepad.exe"_s, u"记事本"_s, {}}));
        QVERIFY(favorites.add({Favorite::Kind::Place, u"winshun-place:test"_s, u"系统设置"_s, {}}));

        FolderMenu menu(nullptr, &favorites);
        menu.setRecordHistory(false);
        menu.setGroups(true, false, false);
        Pointer pointer;
        menu.setCursorSource([&pointer] { return pointer.at; });
        QQmlEngine engine;
        engine.addImageProvider(u"fileicon"_s, new FileIconProvider);
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(
            &engine, QUrl::fromLocalFile(QStringLiteral(WINSHUN_QML_DIR) + u"/FolderMenuWindow.qml"_s));
        std::unique_ptr<QObject> object(
            component.createWithInitialProperties({{u"folderMenu"_s, QVariant::fromValue(&menu)}}));
        QVERIFY2(object, qPrintable(component.errorString()));
        auto* window = qobject_cast<QQuickWindow*>(object.get());
        QVERIFY(window);
        menu.setWindow(window);
        QSignalSpy changed(&menu, &FolderMenu::entriesChanged);
        const auto open = [&] {
            changed.clear();
            menu.hotkeyPressed();
            return changed.wait(5000) && menu.isOpen();
        };
        const auto shot = [&](QQuickWindow* shown, const QString& name) {
            const QString folder = qEnvironmentVariable("WINSHUN_MENU_EVIDENCE");
            if (folder.isEmpty())
                return true;
            return QDir().mkpath(folder)
                && shown->grabWindow().save(folder + u'/' + name + (round ? QString() : u"-square"_s)
                    + (dark ? u"-dark.png"_s : u"-light.png"_s));
        };
        const auto depth = [&] { return object->property("depth").toInt(); };
        const auto highlighted = [&] { return object->property("highlighted").toInt(); };
        const auto key = [&](Qt::Key k) { emit menu.keyPressed(k, {}); };
        QVERIFY(open());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QCOMPARE(::GetForegroundWindow(), sourceHandle);
        const auto root = menu.entries();
        QCOMPARE(root.first().toMap().value(u"text"_s).toString(), u"收藏夹"_s);
        const auto children = root.first().toMap().value(u"children"_s).toList();
        QCOMPARE(children.size(), 54);
        QCOMPARE(children[50].toMap().value(u"detail"_s).toString(), u"verify"_s);
        QVERIFY(children[50].toMap().value(u"icon"_s).toString().startsWith(u"image://fileicon/command/"));
        QCOMPARE(children.first().toMap().value(u"detail"_s).toString(), QString());
        QVERIFY(!children.first().toMap().value(u"tooltip"_s).toString().isEmpty());
        QVERIFY(shot(window, u"root"_s));

        // A click on a group's row opens its column beside the menu, which
        // stays where it is; nothing runs, and the window in front keeps it.
        const QPoint topLeft = window->position();
        QTRY_VERIFY(rowItem(window, 0));
        pointer.click(window, on(window, 0));
        QCOMPARE(depth(), 2);
        auto* favoritesColumn = column(object.get(), 1);
        QVERIFY(favoritesColumn);
        QVERIFY(favoritesColumn->isVisible());
        QCOMPARE(window->position(), topLeft);
        QCOMPARE(favoritesColumn->transientParent(), window);
        QVERIFY(favoritesColumn->flags().testFlag(Qt::WindowDoesNotAcceptFocus));
        QCOMPARE(favoritesColumn->width(), window->width());
        QVERIFY(!QFile::exists(output)); // browsing never executes a command
        QCOMPARE(::GetForegroundWindow(), sourceHandle);
        pointer.leave();
        // Left closes it, back on the row it came from; the menu stays.
        key(Qt::Key_Left);
        QCOMPARE(depth(), 1);
        QVERIFY(!favoritesColumn->isVisible());
        QCOMPARE(highlighted(), 0);
        QVERIFY(menu.isOpen());
        // Right opens it again, its first row chosen; the row it came from
        // stays highlighted in the menu, fainter.
        key(Qt::Key_Right);
        QCOMPARE(depth(), 2);
        QVERIFY(favoritesColumn->isVisible());
        QCOMPARE(highlighted(), 0);
        QCOMPARE(value(object.get(), u"rowShown(0)"_s).toInt(), 0);
        QVERIFY(value(object.get(), u"fainter(0)"_s).toBool());
        auto* scroll = visualItem(favoritesColumn->contentItem(), u"folderMenuScroll"_s);
        QVERIFY(scroll);
        QTRY_VERIFY(scroll->property("contentHeight").toReal() > scroll->height());
        const QRectF area = menu.screenArea(window->position());
        QVERIFY(favoritesColumn->height() <= area.height() - 16);
        QVERIFY(area.contains(QRectF(favoritesColumn->geometry())));
        QTest::qWait(150); // asynchronous shell icons before visual evidence
        QVERIFY(shot(favoritesColumn, u"favorites"_s));
        key(Qt::Key_End);
        QCOMPARE(highlighted(), 53);
        QTRY_VERIFY(scroll->property("contentY").toReal() > 0);
        QTRY_VERIFY(visualItem(favoritesColumn->contentItem(), u"folderMenuEntry53"_s));
        const auto* last = visualItem(favoritesColumn->contentItem(), u"folderMenuEntry53"_s);
        const auto bottom = last->mapToItem(scroll, QPointF(0, last->height())).y();
        QVERIFY(bottom <= scroll->height() + 1);
        QVERIFY(shot(favoritesColumn, u"favorites-end"_s));
        key(Qt::Key_Left);
        QCOMPARE(depth(), 1);
        key(Qt::Key_Right);
        key(Qt::Key_Backspace);
        QCOMPARE(depth(), 1);
        // Esc closes the whole menu, its columns with it.
        key(Qt::Key_Right);
        QCOMPARE(depth(), 2);
        key(Qt::Key_Escape);
        QVERIFY(!menu.isOpen());
        QVERIFY(!window->isVisible());
        QVERIFY(!favoritesColumn->isVisible());
        QVERIFY(open());
        QCOMPARE(depth(), 1);
        key(Qt::Key_Down);
        key(Qt::Key_Return);
        QCOMPARE(depth(), 2);
        object->setProperty("highlighted", 50);
        key(Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(output), 10000);
        QVERIFY(!menu.isOpen());
        QVERIFY(!favoritesColumn->isVisible());
        QFile written(output);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QCOMPARE(written.readAll(), QByteArray("menu-ok"));
        QCOMPARE(::GetForegroundWindow(), sourceHandle);
        QCOMPARE(warnings.count(), 0);

        menu.setGroups(false, false, false);
        QVERIFY(open());
        QCOMPARE(menu.entries().size(), 1); // disabled groups are not leaked as commands
        QCOMPARE(entry(menu, command.name), -1);
        menu.dismiss();
        QVERIFY(favorites.remove(command.path));
        menu.setGroups(true, false, false);
        QVERIFY(open());
        QCOMPARE(entry(menu, command.name), -1);
        menu.dismiss();
    }

    // The folders inside a folder (GitHub #170): by name with numbers by
    // value, hidden ones and files left out.
    void subfolders()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir root(temporary.path());
        for (const QString& name : {u"10月"_s, u"2月"_s, u"24年"_s, u"25年"_s, u"藏起来的"_s})
            QVERIFY(root.mkpath(name));
        QVERIFY(::SetFileAttributesW(reinterpret_cast<const wchar_t*>(
                                         QDir::toNativeSeparators(root.filePath(u"藏起来的"_s)).utf16()),
            FILE_ATTRIBUTE_HIDDEN));
        QFile file(root.filePath(u"3月.txt"_s));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const QStringList expected {u"2月"_s, u"10月"_s, u"24年"_s, u"25年"_s};
        QCOMPARE(FolderMenu::subfolders(temporary.path()), expected);
        QCOMPARE(FolderMenu::subfolders(temporary.path() + u'/'), expected);
        QCOMPARE(FolderMenu::subfolders(temporary.path(), 2), expected.first(2));
        QCOMPARE(FolderMenu::subfolders(root.filePath(u"2月"_s)), QStringList());
        QCOMPARE(FolderMenu::subfolders(root.filePath(u"没有这个"_s)), QStringList());
        QCOMPARE(FolderMenu::subfolders({}), QStringList());
    }

    // A folder's row leads to the folders inside it, column after column: by
    // the Right key, a click on its arrow, or the pointer resting on it. The
    // row itself still goes to the folder; a column that comes while the
    // pointer rests opens nothing further.
    void folderPages()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir root(temporary.path());
        for (const QString& name : {u"工作/2月"_s, u"工作/10月"_s, u"工作/24年"_s, u"工作/25年/a一月"_s, u"工作/25年/b二月"_s})
            QVERIFY(root.mkpath(name));
        const QString work = QDir::toNativeSeparators(root.filePath(u"工作"_s));
        const HWND previous = ::GetForegroundWindow();
        auto restore = qScopeGuard([previous] {
            if (previous && ::IsWindow(previous))
                ::SetForegroundWindow(previous);
        });
        QWindow source;
        source.setTitle(u"WinShun folder pages test"_s);
        source.setGeometry(QRect(awayFromPointer(QSize(900, 400)).topLeft(), QSize(320, 160)));
        source.show();
        QVERIFY(QTest::qWaitForWindowExposed(&source));
        win::bringToFront(&source);
        const HWND sourceHandle = reinterpret_cast<HWND>(source.winId());
        QTRY_COMPARE_WITH_TIMEOUT(::GetForegroundWindow(), sourceHandle, 3000);

        Favorites favorites(temporary.filePath(u"favorites.json"_s));
        QVERIFY(favorites.add({Favorite::Kind::Folder, work, u"工作"_s, {}}));
        QVERIFY(favorites.add({Favorite::Kind::File, root.filePath(u"readme.txt"_s), u"使用说明"_s, {}}));
        FolderMenu menu(nullptr, &favorites);
        menu.setRecordHistory(false);
        menu.setGroups(true, false, false);
        QStringList opened; // never the real file manager
        menu.setOpener([&](const QString& folder) { opened.append(QDir::toNativeSeparators(folder)); });
        Pointer pointer;
        menu.setCursorSource([&pointer] { return pointer.at; });
        QQmlEngine engine;
        engine.addImageProvider(u"fileicon"_s, new FileIconProvider);
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        std::unique_ptr<QObject> object = load(engine, menu);
        QVERIFY(object);
        auto* window = qobject_cast<QQuickWindow*>(object.get());
        QVERIFY(window);
        menu.setWindow(window);
        QSignalSpy changed(&menu, &FolderMenu::entriesChanged);
        const auto open = [&] {
            changed.clear();
            menu.hotkeyPressed();
            return changed.wait(5000) && menu.isOpen();
        };
        const auto depth = [&] { return object->property("depth").toInt(); };
        const auto highlighted = [&] { return object->property("highlighted").toInt(); };
        const auto folder = [&] { return value(object.get(), u"page === null ? '' : (page.folder ?? '')"_s).toString(); };
        const auto texts = [&] { return value(object.get(), u"entries.map(e => e.text ?? '').join('|')"_s).toString(); };
        const auto key = [&](Qt::Key k) { emit menu.keyPressed(k, {}); };
        const auto shot = [&](QQuickWindow* shown, const QString& name) {
            const QString evidence = qEnvironmentVariable("WINSHUN_MENU_EVIDENCE");
            return evidence.isEmpty() || (QDir().mkpath(evidence) && shown->grabWindow().save(evidence + u'/' + name + u".png"_s));
        };

        // Keys: Right opens the column a row leads to, its first row chosen,
        // the rows it came from still highlighted; Enter goes there; Left
        // closes the last column, back on the row it came from.
        QVERIFY(open());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        key(Qt::Key_Down);
        key(Qt::Key_Right);
        QCOMPARE(texts(), u"工作|使用说明"_s);
        QCOMPARE(highlighted(), 0);
        key(Qt::Key_Right);
        QTRY_COMPARE(folder(), work);
        QCOMPARE(depth(), 3);
        QCOMPARE(texts(), u"2月|10月|24年|25年"_s);
        QCOMPARE(highlighted(), 0);
        QCOMPARE(value(object.get(), u"rowShown(0)"_s).toInt(), 0);
        QCOMPARE(value(object.get(), u"rowShown(1)"_s).toInt(), 0);
        QTest::qWait(150); // asynchronous shell icons before visual evidence
        QVERIFY(shot(column(object.get(), 2), u"folder-page"_s));
        key(Qt::Key_End);
        key(Qt::Key_Right);
        QTRY_COMPARE(folder(), work + u"\\25年"_s);
        QCOMPARE(texts(), u"a一月|b二月"_s);
        key(Qt::Key_Right); // nothing inside: says so, and nothing to choose
        QTRY_COMPARE(folder(), work + u"\\25年\\a一月"_s);
        QCOMPARE(texts(), u"没有子文件夹"_s);
        QCOMPARE(highlighted(), -1);
        QCOMPARE(depth(), 5);
        key(Qt::Key_Left);
        QCOMPARE(depth(), 4);
        QCOMPARE(folder(), work + u"\\25年"_s);
        QCOMPARE(highlighted(), 0);
        QVERIFY(!column(object.get(), 4)->isVisible());
        key(Qt::Key_Left);
        QCOMPARE(folder(), work);
        QCOMPARE(highlighted(), 3);
        // Read once while the menu is up, a folder's column comes at once.
        key(Qt::Key_Right);
        QCOMPARE(folder(), work + u"\\25年"_s);
        QVERIFY(value(object.get(), u"pending === null"_s).toBool());
        key(Qt::Key_Down);
        key(Qt::Key_Return);
        QCOMPARE(opened, QStringList {work + u"\\25年\\b二月"_s});
        QVERIFY(!menu.isOpen());
        QVERIFY(!column(object.get(), 3)->isVisible());
        opened.clear();

        // A file among the favorites leads nowhere: Right on it does nothing.
        QVERIFY(open());
        QCOMPARE(depth(), 1);
        key(Qt::Key_Down);
        key(Qt::Key_Right);
        QCOMPARE(texts(), u"工作|使用说明"_s);
        key(Qt::Key_Down);
        key(Qt::Key_Right);
        QTest::qWait(300);
        QCOMPARE(depth(), 2);
        QCOMPARE(texts(), u"工作|使用说明"_s);

        // The pointer: a click on a row's arrow opens its column; while the
        // pointer rests there, nothing further opens.
        auto* favoritesColumn = column(object.get(), 1);
        QVERIFY(favoritesColumn);
        QTRY_VERIFY(arrowOf(favoritesColumn, 0).x() > 0);
        pointer.click(favoritesColumn, arrowOf(favoritesColumn, 0));
        QTRY_COMPARE(folder(), work);
        QCOMPARE(depth(), 3);
        QVERIFY(opened.isEmpty());
        QVERIFY(menu.isOpen());
        QTest::qWait(700);
        QCOMPARE(depth(), 3);
        // Resting on a row opens its column after a moment, nothing chosen in
        // it; the one that comes opens nothing further.
        auto* foldersColumn = column(object.get(), 2);
        QVERIFY(foldersColumn);
        QTRY_VERIFY(on(foldersColumn, 3).x() > 0);
        pointer.moveTo(foldersColumn, on(foldersColumn, 3, 0.2));
        pointer.moveTo(foldersColumn, on(foldersColumn, 3, 0.4));
        QTest::qWait(150);
        QCOMPARE(depth(), 3); // not at once
        QTRY_COMPARE(folder(), work + u"\\25年"_s);
        QCOMPARE(depth(), 4);
        QCOMPARE(highlighted(), -1);
        QTest::qWait(700);
        QCOMPARE(depth(), 4);
        QVERIFY(shot(column(object.get(), 3), u"folder-page-pointer"_s));
        // Back on the row of a column before whose column is up: the columns
        // after that one close.
        pointer.moveTo(favoritesColumn, on(favoritesColumn, 0));
        QTRY_COMPARE(depth(), 3);
        QCOMPARE(folder(), work);
        QCOMPARE(highlighted(), 3);
        // Onto a row there that leads nowhere: every column after its own closes.
        pointer.moveTo(favoritesColumn, on(favoritesColumn, 1, 0.1));
        QTRY_COMPARE(depth(), 2);
        QCOMPARE(highlighted(), 1);
        QVERIFY(!foldersColumn->isVisible());
        // A click on a folder's row goes there.
        pointer.click(favoritesColumn, on(favoritesColumn, 0, 0.1));
        QVERIFY(!menu.isOpen());
        QCOMPARE(opened, QStringList {work});
        QCOMPARE(::GetForegroundWindow(), sourceHandle);

        // Closed before the folder answered: nothing comes for the next menu.
        QVERIFY(open());
        QSignalSpy listed(&menu, &FolderMenu::folderListed);
        menu.listFolder(work);
        menu.dismiss();
        QVERIFY(open());
        QTest::qWait(300);
        QCOMPARE(listed.size(), 0);
        QCOMPARE(folder(), QString());
        menu.dismiss();
        QCOMPARE(warnings.count(), 0);
    }

    void cascadeColumns_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    // Each column beside the one before it (GitHub #348): on that one's side
    // while there is room, flipped where the screen ends and on that way
    // after, against the edge (over the one before) where neither side has
    // room; its first row level with the row it came from; all inside the
    // work area. Past eight columns the last window shows the last column.
    void cascadeColumns()
    {
        QFETCH(bool, dark);
        const auto scheme = QGuiApplication::styleHints()->colorScheme();
        const auto restore = qScopeGuard([&] { QGuiApplication::styleHints()->setColorScheme(scheme); });
        QGuiApplication::styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
        QVariantList chain {leaf(u"底"_s)};
        for (int level = 10; level >= 1; --level)
            chain = {group(u"层 %1"_s.arg(level), chain), leaf(u"文件 a"_s), leaf(u"文件 b"_s)};
        QVariantList many;
        for (int i = 1; i <= 14; ++i)
            many.append(leaf(u"文件夹 %1"_s.arg(i)));
        FolderMenu menu(nullptr, nullptr);
        menu.previewEntries({chain.first(),
            group(u"项目资料"_s, {leaf(u"2024"_s),
                group(u"2025"_s, {leaf(u"01 一月"_s), leaf(u"02 二月"_s), leaf(u"03 三月：一段很长的名称，用来检查省略"_s)}),
                leaf(u"客户合同"_s), leaf(u"设计素材"_s)}),
            leaf(u"年度报告"_s), leaf(u"会议记录"_s), leaf(u"归档备份"_s), leaf(u"财务报表"_s), leaf(u"产品文档"_s),
            group(u"很多"_s, many)});
        Pointer pointer; // never over the menu here: the keys only
        menu.setCursorSource([&pointer] { return pointer.at; });
        QQmlEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        std::unique_ptr<QObject> object = load(engine, menu);
        QVERIFY(object);
        auto* window = qobject_cast<QQuickWindow*>(object.get());
        QVERIFY(window);
        const int width = window->width();
        const QRect area = awayFromPointer(QSize(3 * width + 60, 460));
        const auto behind = backdrop(area, dark);
        const auto popup = [&](const QRect& within, QPoint at) {
            emit menu.popupRequested(QPointF(at));
            object->setProperty("availableArea", QRectF(within));
            QMetaObject::invokeMethod(object.get(), "place");
        };
        const auto key = [&](Qt::Key k) { emit menu.keyPressed(k, {}); };
        const auto depth = [&] { return object->property("depth").toInt(); };
        const auto highlighted = [&] { return object->property("highlighted").toInt(); };
        const auto tops = [](qreal a, qreal b) { return u"row tops %1 and %2"_s.arg(a).arg(b).toUtf8(); };

        // To the right while there is room, each first row level with the row
        // it came from, which stays highlighted, fainter.
        popup(area, area.topLeft() + QPoint(8, 8));
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QCOMPARE(window->position(), area.topLeft() + QPoint(8, 8));
        key(Qt::Key_Down);
        key(Qt::Key_Down);
        key(Qt::Key_Right);
        QCOMPARE(depth(), 2);
        auto* first = column(object.get(), 1);
        QVERIFY(first);
        QVERIFY(first->isVisible());
        QTRY_VERIFY(rowItem(first, 1));
        QCOMPARE(first->transientParent(), window);
        QCOMPARE(::GetWindow(handle(first), GW_OWNER), handle(window)); // in front of the menu, always
        QVERIFY(first->flags().testFlag(Qt::WindowDoesNotAcceptFocus));
        QVERIFY(!first->isActive());
        QCOMPARE(first->width(), width);
        QVERIFY2(aligned(first->x(), window->x() + width - 1), qPrintable(u"x %1"_s.arg(first->x())));
        QVERIFY2(aligned(topOf(first, 0), topOf(window, 1)), tops(topOf(first, 0), topOf(window, 1)).constData());
        QCOMPARE(highlighted(), 0);
        QCOMPARE(value(object.get(), u"rowShown(0)"_s).toInt(), 1);
        QVERIFY(value(object.get(), u"fainter(0)"_s).toBool());
        key(Qt::Key_Down);
        key(Qt::Key_Right);
        QCOMPARE(depth(), 3);
        auto* second = column(object.get(), 2);
        QVERIFY(second);
        QVERIFY(second->isVisible());
        QTRY_VERIFY(rowItem(second, 0));
        QVERIFY2(aligned(second->x(), first->x() + width - 1), qPrintable(u"x %1"_s.arg(second->x())));
        QVERIFY2(aligned(topOf(second, 0), topOf(first, 1)), tops(topOf(second, 0), topOf(first, 1)).constData());
        QCOMPARE(value(object.get(), u"rowShown(1)"_s).toInt(), 1);
        // On the screen's own pixels: each border the columns share one line,
        // every row level with the one it came from (half a pixel at 150 %).
        QByteArray wrong = seam(window, first, 1);
        QVERIFY2(wrong.isEmpty(), wrong.constData());
        wrong = seam(first, second, 1);
        QVERIFY2(wrong.isEmpty(), wrong.constData());
        QVERIFY2(std::abs(screenTop(first, 0) - screenTop(window, 1)) <= 0.51,
            tops(screenTop(first, 0), screenTop(window, 1)).constData());
        QVERIFY2(std::abs(screenTop(second, 0) - screenTop(first, 1)) <= 0.51,
            tops(screenTop(second, 0), screenTop(first, 1)).constData());
        QVERIFY(screenEvidence({window, first, second}, dark ? u"cascade-dark"_s : u"cascade-light"_s));

        // The full path in front of every column, those shown after it too.
        key(Qt::Key_Left);
        QCOMPARE(depth(), 2);
        auto* tip = object->findChild<QQuickWindow*>(u"menuPathTip"_s);
        QVERIFY(tip);
        object->setProperty("hoveredText", u"C:\\项目资料\\2025"_s);
        object->setProperty("hoveredEntry", QVariant::fromValue(rowItem(first, 1)));
        QTRY_VERIFY(tip->isVisible());
        QVERIFY(inFront(tip, first));
        key(Qt::Key_Right);
        QCOMPARE(depth(), 3);
        QVERIFY(second->isVisible());
        QVERIFY2(inFront(tip, second) && inFront(tip, first) && inFront(tip, window), "the path behind a column");
        QTRY_VERIFY(rowItem(second, 0));
        object->setProperty("hoveredText", u"C:\\项目资料\\2025\\01 一月"_s);
        object->setProperty("hoveredEntry", QVariant::fromValue(rowItem(second, 0)));
        QTRY_COMPARE(tip->property("shownText").toString(), u"C:\\项目资料\\2025\\01 一月"_s);
        QVERIFY(tip->isVisible());
        QVERIFY2(inFront(tip, second) && inFront(tip, first), "the path behind its own column");
        object->setProperty("hoveredText", QString());
        QTRY_VERIFY(!tip->isVisible());
        key(Qt::Key_Escape);
        QVERIFY(!window->isVisible());
        QVERIFY(!first->isVisible());
        QVERIFY(!second->isVisible());

        // Where the screen ends on the right: to the left, and on that way after.
        const QPoint rightmost(area.x() + area.width() - 8 - width - 20, area.y() + 8);
        popup(area, rightmost);
        QCOMPARE(window->position(), rightmost);
        key(Qt::Key_Down);
        key(Qt::Key_Down);
        key(Qt::Key_Right);
        QVERIFY(first->isVisible());
        QVERIFY2(aligned(first->x(), window->x() - width + 1), qPrintable(u"x %1"_s.arg(first->x())));
        QCOMPARE(first->property("direction").toInt(), -1);
        QVERIFY2(aligned(topOf(first, 0), topOf(window, 1)), tops(topOf(first, 0), topOf(window, 1)).constData());
        key(Qt::Key_Down);
        key(Qt::Key_Right);
        QVERIFY(second->isVisible());
        QVERIFY2(aligned(second->x(), first->x() - width + 1), qPrintable(u"x %1"_s.arg(second->x())));
        QVERIFY(inside(area, second));
        wrong = seam(window, first, -1);
        QVERIFY2(wrong.isEmpty(), wrong.constData());
        wrong = seam(first, second, -1);
        QVERIFY2(wrong.isEmpty(), wrong.constData());
        key(Qt::Key_Escape);

        // No room on either side: against the edge, over the menu.
        const QRect narrow(area.topLeft(), QSize(width + 150, area.height()));
        popup(narrow, narrow.topLeft() + QPoint(8, 8));
        key(Qt::Key_Down);
        key(Qt::Key_Down);
        key(Qt::Key_Right);
        QVERIFY(first->isVisible());
        QVERIFY(inside(narrow, first));
        QVERIFY(first->x() < window->x() + width - 1);
        QVERIFY2(aligned(topOf(first, 0), topOf(window, 1)), tops(topOf(first, 0), topOf(window, 1)).constData());
        key(Qt::Key_Escape);

        // Level with its row only as far as the work area lets it.
        popup(area, area.topLeft() + QPoint(8, 8));
        key(Qt::Key_End);
        QCOMPARE(highlighted(), 7);
        key(Qt::Key_Right);
        QVERIFY(first->isVisible());
        QTRY_VERIFY(rowItem(first, 0));
        QVERIFY(inside(area, first));
        QVERIFY2(topOf(first, 0) < topOf(window, 7) - 1, tops(topOf(first, 0), topOf(window, 7)).constData());
        key(Qt::Key_Escape);

        // Deeper than eight columns: the last window shows the last column.
        popup(area, area.topLeft() + QPoint(8, 8));
        key(Qt::Key_Down);
        for (int i = 0; i < 10; ++i)
            key(Qt::Key_Right);
        QCOMPARE(depth(), 11);
        QCOMPARE(value(object.get(), u"windows.length"_s).toInt(), 7);
        QVERIFY(!column(object.get(), 8));
        QCOMPARE(value(object.get(), u"windows[6].page.text"_s).toString(), u"层 10"_s);
        for (int visual = 0; visual < 8; ++visual) {
            auto* shown = column(object.get(), visual);
            QVERIFY(shown && shown->isVisible());
            QVERIFY2(inside(area, shown), qPrintable(u"column %1 at %2"_s.arg(visual).arg(shown->x())));
            if (visual == 0)
                continue;
            // Right, right, then left over them, and so on: every seam one line.
            wrong = seam(column(object.get(), visual - 1), shown, shown->property("direction").toInt());
            QVERIFY2(wrong.isEmpty(), qPrintable(u"column %1: "_s.arg(visual) + QString::fromUtf8(wrong)));
        }
        key(Qt::Key_Left);
        QCOMPARE(depth(), 10);
        QCOMPARE(value(object.get(), u"windows[6].page.text"_s).toString(), u"层 9"_s);
        QCOMPARE(highlighted(), 0);
        key(Qt::Key_Escape);
        for (int visual = 0; visual < 8; ++visual)
            QVERIFY(!column(object.get(), visual)->isVisible());
        QCOMPARE(warnings.count(), 0);
    }

    // The pointer (GitHub #348): resting on a row opens its column after a
    // moment; on its way there it passes rows that open nothing (menu aim)
    // unless it stops on one; back on a row of a column before, the columns
    // after that one close; a column that comes over a pointer at rest
    // chooses nothing under it until the pointer moves.
    void cascadePointer()
    {
        FolderMenu menu(nullptr, nullptr);
        menu.previewEntries({
            group(u"项目资料"_s, {group(u"2024"_s, {leaf(u"一月"_s), leaf(u"二月"_s)}), group(u"2025"_s, {leaf(u"三月"_s)}),
                leaf(u"说明"_s), leaf(u"合同"_s), leaf(u"报告"_s), leaf(u"素材"_s)}),
            group(u"归档"_s, {leaf(u"旧项目"_s), leaf(u"备份"_s), leaf(u"其他"_s), leaf(u"杂项"_s)}),
            leaf(u"设置"_s),
            group(u"图片"_s, {group(u"截图"_s, {leaf(u"今天"_s), leaf(u"昨天"_s)}), group(u"壁纸"_s, {leaf(u"风景"_s)}),
                leaf(u"图标"_s)}),
            leaf(u"Repos"_s)});
        Pointer pointer;
        menu.setCursorSource([&pointer] { return pointer.at; });
        QQmlEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        std::unique_ptr<QObject> object = load(engine, menu);
        QVERIFY(object);
        auto* window = qobject_cast<QQuickWindow*>(object.get());
        QVERIFY(window);
        const int width = window->width();
        const QRect area = awayFromPointer(QSize(3 * width + 60, 460));
        const auto popup = [&](const QRect& within, QPoint at) {
            emit menu.popupRequested(QPointF(at));
            object->setProperty("availableArea", QRectF(within));
            QMetaObject::invokeMethod(object.get(), "place");
        };
        const auto key = [&](Qt::Key k, const QString& text = {}) { emit menu.keyPressed(k, text); };
        const auto depth = [&] { return object->property("depth").toInt(); };
        const auto highlighted = [&] { return object->property("highlighted").toInt(); };
        const auto from = [&](int level) {
            return value(object.get(), u"pages.length > %1 ? pages[%1].from : -1"_s.arg(level)).toInt();
        };
        const auto aiming = [&](int row) { return value(object.get(), u"aim.running && aimRow === %1"_s.arg(row)).toBool(); };
        const auto state = [&] {
            return value(object.get(), u"JSON.stringify({aimRow, hoverRow, pointerRow, depth, rest: restingCursor, trace})"_s)
                .toString().toUtf8();
        };

        // Resting on a row opens its column after a moment, nothing chosen in it.
        popup(area, area.topLeft() + QPoint(8, 8));
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(rowItem(window, 4));
        pointer.moveTo(window, on(window, 0));
        QCOMPARE(highlighted(), 0);
        QTest::qWait(150);
        QCOMPARE(depth(), 1); // not at once
        QTRY_COMPARE(depth(), 2);
        QCOMPARE(from(0), 0);
        QCOMPARE(highlighted(), -1);
        auto* first = column(object.get(), 1);
        QVERIFY(first);
        QTRY_VERIFY(rowItem(first, 2));

        // On its way to that column, across the rows below: they open nothing.
        pointer.moveTo(window, on(window, 1, 0.8));
        QVERIFY2(aiming(1), state().constData());
        QCOMPARE(value(object.get(), u"rowShown(0)"_s).toInt(), 0);
        pointer.moveTo(first, on(first, 2, 0.1));
        QVERIFY(!aiming(1));
        QCOMPARE(highlighted(), 2);
        QTest::qWait(500);
        QCOMPARE(depth(), 2);
        QCOMPARE(from(0), 0);

        // Stopped on one of them: chosen after all, and its column opens.
        pointer.moveTo(window, on(window, 0));
        QTest::qWait(400); // on the row its column came from: as it was
        QCOMPARE(depth(), 2);
        QCOMPARE(from(0), 0);
        pointer.moveTo(window, on(window, 1, 0.8));
        QVERIFY2(aiming(1), state().constData());
        QTest::qWait(100);
        QCOMPARE(from(0), 0);
        QTRY_COMPARE(from(0), 1);
        QCOMPARE(depth(), 2);

        // Deeper, then back on the row in the menu that the column came from:
        // the columns after that one close.
        pointer.moveTo(window, on(window, 0, 0.1)); // up its left side, not towards the column
        QTRY_COMPARE(from(0), 0);
        QTRY_VERIFY(rowItem(first, 0));
        pointer.moveTo(first, on(first, 0));
        QTRY_COMPARE(depth(), 3);
        auto* second = column(object.get(), 2);
        QVERIFY(second && second->isVisible());
        pointer.moveTo(window, on(window, 0));
        QTRY_COMPARE(depth(), 2);
        QCOMPARE(highlighted(), 0);
        QVERIFY(!second->isVisible());
        // A row there that leads nowhere: every column after the menu closes.
        pointer.moveTo(window, on(window, 2, 0.1));
        QTRY_COMPARE(depth(), 1);
        QCOMPARE(highlighted(), 2);
        QVERIFY(!first->isVisible());

        // Letters choose in the last column.
        pointer.leave();
        key(Qt::Key_R, u"r"_s);
        QCOMPARE(highlighted(), 4);
        key(Qt::Key_Home);
        key(Qt::Key_Right);
        QCOMPARE(depth(), 2);
        QCOMPARE(highlighted(), 0);
        key(Qt::Key_2, u"2"_s);
        QCOMPARE(highlighted(), 1);
        key(Qt::Key_Escape);

        // No room beside the menu: the column comes over the pointer resting
        // on its row, and chooses nothing under it until the pointer moves.
        const QRect narrow(area.topLeft(), QSize(width + 150, area.height()));
        popup(narrow, narrow.topLeft() + QPoint(8, 8));
        pointer.moveTo(window, on(window, 3, 0.8));
        QTRY_COMPARE(depth(), 2);
        QVERIFY(first->isVisible());
        QVERIFY2(first->geometry().contains(pointer.at),
            qPrintable(u"pointer %1,%2 column x %3"_s.arg(pointer.at.x()).arg(pointer.at.y()).arg(first->x())));
        pointer.cameUnder(first);
        QTest::qWait(700);
        QCOMPARE(depth(), 2);
        QCOMPARE(highlighted(), -1);
        pointer.moveTo(first, first->mapFromGlobal(pointer.at) + QPoint(3, 0));
        QTRY_COMPARE(depth(), 3);
        key(Qt::Key_Escape);
        QCOMPARE(warnings.count(), 0);
    }

    // A column scrolled (GitHub #348): the columns after it follow the row
    // they came from while it is in sight and close once it is not; the rows
    // the list brings under the pointer at rest open nothing until it moves.
    void cascadeScroll()
    {
        QVariantList folders;
        for (int i = 1; i <= 30; ++i)
            folders.append(group(u"文件夹 %1"_s.arg(i), {leaf(u"文件 %1"_s.arg(i))}));
        FolderMenu menu(nullptr, nullptr);
        menu.previewEntries({group(u"很多文件夹"_s, folders), leaf(u"设置"_s)});
        Pointer pointer;
        menu.setCursorSource([&pointer] { return pointer.at; });
        QQmlEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        std::unique_ptr<QObject> object = load(engine, menu);
        QVERIFY(object);
        auto* window = qobject_cast<QQuickWindow*>(object.get());
        QVERIFY(window);
        const QRect area = awayFromPointer(QSize(3 * window->width() + 60, 460));
        emit menu.popupRequested(QPointF(area.topLeft() + QPoint(8, 8)));
        object->setProperty("availableArea", QRectF(area));
        QMetaObject::invokeMethod(object.get(), "place");
        QVERIFY(QTest::qWaitForWindowExposed(window));
        const auto key = [&](Qt::Key k) { emit menu.keyPressed(k, {}); };
        const auto depth = [&] { return object->property("depth").toInt(); };
        const auto from = [&](int level) {
            return value(object.get(), u"pages.length > %1 ? pages[%1].from : -1"_s.arg(level)).toInt();
        };
        const auto inSight = [&](int row) { return value(object.get(), u"columnAt(1).rowInSight(%1)"_s.arg(row)).toBool(); };
        const auto state = [&] {
            return value(object.get(), u"JSON.stringify({hoverRow, pointerRow, highlighted, depth, rest: restingCursor})"_s)
                .toString().toUtf8();
        };

        // Row 8 of the long column opens its own beside it.
        key(Qt::Key_Down);
        key(Qt::Key_Right);
        auto* first = column(object.get(), 1);
        QVERIFY(first && first->isVisible());
        QTRY_VERIFY(rowItem(first, 8));
        for (int i = 0; i < 8; ++i)
            key(Qt::Key_Down);
        key(Qt::Key_Right);
        QCOMPARE(depth(), 3);
        QCOMPARE(from(1), 8);
        auto* second = column(object.get(), 2);
        QVERIFY(second && second->isVisible());
        QTRY_VERIFY(rowItem(second, 0));
        auto* list = visualItem(first->contentItem(), u"folderMenuScroll"_s);
        QVERIFY(list);
        pointer.moveTo(first, on(first, 8)); // on that row, then at rest
        QTest::qWait(400);
        QCOMPARE(depth(), 3);
        QCOMPARE(from(1), 8);

        // Two rows down: its column follows the row.
        const qreal was = screenTop(first, 8);
        list->setProperty("contentY", 2 * object->property("itemHeight").toReal());
        QVERIFY(screenTop(first, 8) < was - 10);
        QVERIFY(inSight(8));
        QCOMPARE(depth(), 3);
        QVERIFY2(std::abs(screenTop(second, 0) - screenTop(first, 8)) <= 0.51,
            qPrintable(u"row tops %1 and %2"_s.arg(screenTop(second, 0)).arg(screenTop(first, 8))));
        QTest::qWait(700); // the row now under the pointer: nothing
        QVERIFY2(depth() == 3 && from(1) == 8, state().constData());

        // The wheel on until the row is out of sight: its column closes, the
        // row stays chosen, and nothing under the pointer opens.
        const QPoint local = first->mapFromGlobal(pointer.at);
        for (int i = 0; i < 12 && depth() == 3; ++i) {
            wheel(first, local, -1);
            QTest::qWait(200);
        }
        QVERIFY2(depth() == 2, state().constData());
        QVERIFY(!second->isVisible());
        QVERIFY(!inSight(8));
        QCOMPARE(object->property("highlighted").toInt(), 8);
        QTRY_VERIFY(!list->property("moving").toBool());
        QTest::qWait(700);
        QVERIFY2(depth() == 2, state().constData());

        // The pointer moves: the row under it opens its column.
        pointer.moveTo(first, local + QPoint(3, 0));
        QTRY_COMPARE(depth(), 3);
        QVERIFY(from(1) > 8);
        key(Qt::Key_Escape);
        QVERIFY(!first->isVisible());
        QCOMPARE(warnings.count(), 0);
    }
};

QTEST_MAIN(FolderMenuTest)
#include "tst_foldermenu.moc"
