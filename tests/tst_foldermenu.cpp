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
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>
#include <QScopeGuard>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickItemGrabResult>
#include <QScreen>
#include <QStyleHints>
#include <QFontDatabase>
#include <commctrl.h>

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
        const int font = QFontDatabase::addApplicationFont(
            QCoreApplication::applicationDirPath() + u"/fonts/AlibabaPuHuiTi-3-55-Regular.ttf"_s);
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
        source.resize(320, 160);
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
        const auto shot = [&](const QString& name) {
            const QString folder = qEnvironmentVariable("WINSHUN_MENU_EVIDENCE");
            if (folder.isEmpty())
                return true;
            return QDir().mkpath(folder)
                && window->grabWindow().save(folder + u'/' + name + (round ? QString() : u"-square"_s)
                    + (dark ? u"-dark.png"_s : u"-light.png"_s));
        };
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
        QVERIFY(shot(u"root"_s));

        // Mouse entry and keyboard return use the production QML handlers.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, QPoint(90, 22));
        QTRY_VERIFY(object->property("headingHeight").toInt() > 0);
        QVERIFY(!QFile::exists(output)); // browsing never executes a command
        QCOMPARE(::GetForegroundWindow(), sourceHandle);
        emit menu.keyPressed(Qt::Key_Escape, {});
        QTRY_COMPARE(object->property("headingHeight").toInt(), 0);
        QVERIFY(menu.isOpen());
        object->setProperty("highlighted", 0);
        emit menu.keyPressed(Qt::Key_Right, {});
        QTRY_VERIFY(object->property("headingHeight").toInt() > 0);
        auto* scroll = visualItem(window->contentItem(), u"folderMenuScroll"_s);
        QVERIFY(scroll);
        QTRY_VERIFY(scroll->property("contentHeight").toReal() > scroll->height());
        QVERIFY(window->height() <= menu.screenArea(window->position()).height() - 16);
        QTest::qWait(150); // asynchronous shell icons before visual evidence
        QVERIFY(shot(u"favorites"_s));
        emit menu.keyPressed(Qt::Key_End, {});
        QTRY_COMPARE(object->property("highlighted").toInt(), 53);
        QTRY_VERIFY(scroll->property("contentY").toReal() > 0);
        const auto* last = visualItem(window->contentItem(), u"folderMenuEntry53"_s);
        QVERIFY(last);
        const auto bottom = last->mapToItem(scroll, QPointF(0, last->height())).y();
        QVERIFY(bottom <= scroll->height() + 1);
        QVERIFY(shot(u"favorites-end"_s));
        emit menu.keyPressed(Qt::Key_Left, {});
        QTRY_COMPARE(object->property("headingHeight").toInt(), 0);
        emit menu.keyPressed(Qt::Key_Right, {});
        emit menu.keyPressed(Qt::Key_Backspace, {});
        QTRY_COMPARE(object->property("headingHeight").toInt(), 0);
        emit menu.keyPressed(Qt::Key_Escape, {});
        QVERIFY(!menu.isOpen());
        QVERIFY(open());
        QCOMPARE(object->property("headingHeight").toInt(), 0);
        emit menu.keyPressed(Qt::Key_Down, {});
        emit menu.keyPressed(Qt::Key_Return, {});
        QTRY_VERIFY(object->property("headingHeight").toInt() > 0);
        object->setProperty("highlighted", 50);
        emit menu.keyPressed(Qt::Key_Return, {});
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(output), 10000);
        QVERIFY(!menu.isOpen());
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
};

QTEST_MAIN(FolderMenuTest)
#include "tst_foldermenu.moc"
