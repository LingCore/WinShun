#include "TrayMenu.h"
#include "CountdownWindow.h"
#include "PinManager.h"
#include "Phosphor.h"
#include "SystemTheme.h"
#include "platform/WindowEffects.h"

#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QScopeGuard>
#include <QScreen>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWindow>

#include <dwmapi.h>
#include <oleacc.h>
#include <wrl/client.h>

#include <string_view>

using namespace ws;
using namespace Qt::StringLiterals;

namespace {
std::vector<shell::MenuItem> entries(bool english)
{
    std::vector<shell::MenuItem> items {
        {1, english ? u"Open WinShun\tDouble Shift"_s : u"打开 Win顺\t双击 Shift"_s, false, true, true},
        {2, english ? u"Clipboard history\tWin+Alt+V"_s : u"剪贴板历史\tWin+Alt+V"_s},
        {3, english ? u"Pause clipboard recording"_s : u"暂停记录剪贴板"_s},
        shell::MenuItem::separator(),
        {4, english ? u"14,723 items indexed"_s : u"已索引 14,723 项"_s, false, false},
        {5, english ? u"Rebuild index"_s : u"重建索引"_s},
        shell::MenuItem::separator(),
        {6, english ? u"Start at sign-in"_s : u"开机自动启动"_s, true},
        {7, english ? u"Settings…"_s : u"设置…"_s},
        {8, english ? u"Check for updates…\t0.16.0"_s : u"检查更新…\t0.16.0"_s},
        shell::MenuItem::separator(),
        {9, english ? u"Restart"_s : u"重新启动"_s},
        {10, english ? u"E&xit"_s : u"退出"_s},
        {0, english ? u"Screenshot and recording plugin with a deliberately long title"_s : u"截图与录屏"_s, false,
            true, false, {{21, u"Capture"_s}, {22, u"Unavailable"_s, false, false}, {23, u"Record"_s}}},
    };
    constexpr char16_t glyphs[] {
        0xE721, 0xE77F, 0xE769, 0, 0xE968, 0xE895, 0, 0xE7E8, 0xE713, 0xE896, 0, 0xE72C, 0xE8BB, 0xE722};
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (glyphs[i])
            items[i].glyph = QString(QChar(glyphs[i]));
    }
    items[4].secondary = true;
    return items;
}

} // namespace

class TrayMenuTest : public QObject {
    Q_OBJECT
    HRESULT m_com = E_FAIL;
private slots:
    void initTestCase()
    {
        m_com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        QVERIFY(SUCCEEDED(m_com));
        phosphor::loadFonts();
        const int id = QFontDatabase::addApplicationFont(
            QCoreApplication::applicationDirPath() + u"/fonts/AlibabaPuHuiTi-3-55-Regular.ttf"_s);
        const auto families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty())
            QGuiApplication::setFont(QFont(families.front()));
        qmlRegisterSingletonType<SystemTheme>(
            "WinShun", 1, 0, "SystemTheme", [](QQmlEngine*, QJSEngine*) -> QObject* { return new SystemTheme; });
        qmlRegisterSingletonType(
            QUrl::fromLocalFile(QStringLiteral(WINSHUN_QML_DIR) + u"/Theme.qml"_s), "WinShun", 1, 0, "Theme");
    }

    void appearance_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("round");
        QTest::addColumn<bool>("english");
        QTest::addColumn<bool>("selected");
        QTest::newRow("light-round") << false << true << false << false;
        QTest::newRow("dark-round") << true << true << false << false;
        QTest::newRow("light-square") << false << false << false << false;
        QTest::newRow("dark-square") << true << false << false << false;
        QTest::newRow("english-long") << false << true << true << false;
        QTest::newRow("dark-selected") << true << true << false << true;
    }

    void appearance()
    {
        if (qEnvironmentVariableIntValue("WINSHUN_TRAYMENU_LIVE") != 1)
            QSKIP("Opt in with WINSHUN_TRAYMENU_LIVE=1");
        QFETCH(bool, dark);
        QFETCH(bool, round);
        QFETCH(bool, english);
        QFETCH(bool, selected);
        const HWND previous = ::GetForegroundWindow();
        const auto restore = qScopeGuard([&] {
            if (::IsWindow(previous))
                ::SetForegroundWindow(previous);
        });
        QGuiApplication::styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
        win::setMenuTheme(dark);
        SystemTheme::setRoundedCorners(round);
        QQmlEngine engine;
        QObject* theme = engine.singletonInstance<QObject*>(u"WinShun"_s, u"Theme"_s);
        QVERIFY(theme);
        QWindow owner;
        owner.setTitle(u"WinShun tray menu verification"_s);
        owner.setGeometry(100, 100, 320, 160);
        owner.show();
        QVERIFY(QTest::qWaitForWindowExposed(&owner));
        const HWND handle = reinterpret_cast<HWND>(owner.winId());
        const auto items = entries(english);
        QObject lifetime;
        bool observed = false, timedOut = false;
        QTimer watchdog;
        connect(&watchdog, &QTimer::timeout, &lifetime, [&] {
            timedOut = true;
            ::EndMenu();
        });
        watchdog.start(4000);
        const int chosen = traymenu::popup(handle, items, POINT {200, 180}, theme, [&](HWND window, HMENU menu) {
            if (selected)
                ::PostMessageW(handle, WM_KEYDOWN, VK_UP, 0);
            QTimer::singleShot(120, &lifetime, [&, window, menu] {
                const auto dismiss = qScopeGuard([&] { ::PostMessageW(handle, WM_KEYDOWN, VK_ESCAPE, 0); });
                observed = true;
                QCOMPARE(::GetMenuItemCount(menu), int(items.size()) + 2);
                QVERIFY(::GetMenuState(menu, 5, MF_BYPOSITION) & MF_DISABLED);
                QVERIFY(::GetMenuState(menu, 8, MF_BYPOSITION) & MF_CHECKED);
                RECT top {}, bottom {};
                QVERIFY(::GetMenuItemRect(nullptr, menu, 0, &top));
                QVERIFY(::GetMenuItemRect(nullptr, menu, int(items.size()) + 1, &bottom));
                QCOMPARE(top.bottom - top.top, qRound(6 * ::GetDpiForWindow(window) / 96.0));
                QCOMPARE(bottom.bottom - bottom.top, top.bottom - top.top);
                for (const int position : {0, int(items.size()) + 1}) {
                    const UINT state = ::GetMenuState(menu, position, MF_BYPOSITION);
                    QVERIFY(state & MF_SEPARATOR);
                    QVERIFY(state & MF_DISABLED);
                }
                MENUITEMINFOW entry {sizeof entry};
                entry.fMask = MIIM_DATA | MIIM_FTYPE;
                QVERIFY(::GetMenuItemInfoW(menu, 1, TRUE, &entry));
                QVERIFY(entry.fType & MFT_OWNERDRAW);
                const auto* accessible = reinterpret_cast<const MSAAMENUINFO*>(entry.dwItemData);
                QCOMPARE(accessible->dwMSAASignature, DWORD(MSAA_MENU_SIG));
                QCOMPARE(QString::fromWCharArray(accessible->pszWText, accessible->cchWText), items[0].text);
                Microsoft::WRL::ComPtr<IAccessible> object;
                QVERIFY(SUCCEEDED(
                    ::AccessibleObjectFromWindow(window, static_cast<DWORD>(OBJID_CLIENT), IID_PPV_ARGS(&object))));
                VARIANT child {};
                child.vt = VT_I4;
                child.lVal = 2;
                BSTR name = nullptr;
                QVERIFY(SUCCEEDED(object->get_accName(child, &name)));
                const QString exposedName = QString::fromWCharArray(name);
                ::SysFreeString(name);
                QVERIFY2(exposedName.startsWith(items[0].text.section(u'\t', 0, 0)), qPrintable(exposedName));
                VARIANT state {};
                child.lVal = 9;
                QVERIFY(SUCCEEDED(object->get_accState(child, &state)));
                QVERIFY(state.lVal & STATE_SYSTEM_CHECKED);
                child.lVal = 6;
                QVERIFY(SUCCEEDED(object->get_accState(child, &state)));
                QVERIFY(state.lVal & STATE_SYSTEM_UNAVAILABLE);
                if (selected)
                    QVERIFY(::GetMenuState(menu, 14, MF_BYPOSITION) & MF_HILITE);
                RECT row {};
                QVERIFY(::GetMenuItemRect(handle, menu, 1, &row));
                QCOMPARE(row.bottom - row.top, qRound(28 * ::GetDpiForWindow(window) / 96.0));
                int corners = 0;
                QVERIFY(SUCCEEDED(::DwmGetWindowAttribute(window, 33, &corners, sizeof corners)));
                QCOMPARE(corners, round ? 2 : 1);
                const QImage image = owner.screen()->grabWindow(reinterpret_cast<WId>(window)).toImage();
                QVERIFY(!image.isNull());
                RECT plugin {};
                RECT outer {};
                QVERIFY(::GetMenuItemRect(nullptr, menu, 14, &plugin));
                QVERIFY(::GetWindowRect(window, &outer));
                const qreal scale = ::GetDpiForWindow(window) / 96.0;
                const QRect arrow(plugin.right - outer.left - qRound(26 * scale), plugin.top - outer.top,
                    qRound(18 * scale), plugin.bottom - plugin.top);
                const QColor foreground = theme->property("text").value<QColor>();
                const QColor background = theme->property(selected ? "menuHover" : "menuBackground").value<QColor>();
                int inkPixels = 0;
                for (int y = arrow.top(); y < arrow.bottom(); ++y) {
                    for (int x = arrow.left(); x < arrow.right(); ++x) {
                        if (!image.valid(x, y))
                            continue;
                        const QColor pixel = image.pixelColor(x, y);
                        // A thin diagonal is antialiased: require at least
                        // half the expected contrast in the correct direction.
                        const int expectedContrast = foreground.lightness() - background.lightness();
                        const int actualContrast = pixel.lightness() - background.lightness();
                        if (actualContrast * expectedContrast > expectedContrast * expectedContrast / 2)
                            ++inkPixels;
                    }
                }
                const QString folder = qEnvironmentVariable("WINSHUN_TRAY_EVIDENCE");
                if (!folder.isEmpty()) {
                    QVERIFY(QDir().mkpath(folder));
                    QVERIFY(
                        image.save(QDir(folder).filePath(QString::fromLatin1(QTest::currentDataTag()) + u".png"_s)));
                }
                QVERIFY2(
                    inkPixels > 2, "Submenu chevron must contrast with the background in the theme text direction");
                qInfo() << "Menu native DPI:" << ::GetDpiForWindow(window) << "dimensions:" << image.size();
            });
        });
        QVERIFY(!timedOut);
        QVERIFY(observed);
        QCOMPARE(chosen, 0);
    }

    void pinMenu_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("collapsed");
        QTest::newRow("pin-light") << false << false;
        QTest::newRow("pin-dark") << true << false;
        QTest::newRow("pin-collapsed") << true << true;
    }

    void pinMenu()
    {
        if (qEnvironmentVariableIntValue("WINSHUN_TRAYMENU_LIVE") != 1)
            QSKIP("Opt in with WINSHUN_TRAYMENU_LIVE=1");
        QFETCH(bool, dark);
        QFETCH(bool, collapsed);
        QGuiApplication::styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
        win::setMenuTheme(dark);
        SystemTheme::setRoundedCorners(true);
        QQmlEngine engine;
        QObject* theme = engine.singletonInstance<QObject*>(u"WinShun"_s, u"Theme"_s);
        QVERIFY(theme);
        QImage fixture(160, 100, QImage::Format_RGB32);
        fixture.fill(QColor("#e7edf5"));
        QImage copied;
        bool observed = false, styled = false, timedOut = false;
        capture::PinManager::Callbacks callbacks;
        callbacks.copy = [&](const QImage& image, bool) { copied = image; };
        callbacks.dark = [dark] { return dark; };
        callbacks.ocrAvailable = [] { return false; };
        QObject lifetime;
        callbacks.popupMenu = [&](HWND owner, const std::vector<shell::MenuItem>& items, POINT at) {
            const auto iconsPresent = [](const auto& self, const auto& rows) -> bool {
                for (const auto& row : rows) {
                    if (row.text.isEmpty())
                        continue;
                    if (!phosphor::forSegoe(row.glyph) || !self(self, row.children))
                        return false;
                }
                return true;
            };
            styled = iconsPresent(iconsPresent, items);
            QTimer watchdog;
            connect(&watchdog, &QTimer::timeout, &lifetime, [&] {
                timedOut = true;
                ::EndMenu();
            });
            watchdog.start(4000);
            return traymenu::popup(owner, items, at, theme, [&](HWND window, HMENU menu) {
                QTimer::singleShot(120, &lifetime, [&, window, menu, owner] {
                    observed = true;
                    RECT row {};
                    if (!::GetMenuItemRect(nullptr, menu, 1, &row)
                        || row.bottom - row.top != qRound(28 * ::GetDpiForWindow(window) / 96.0))
                        styled = false;
                    const QString folder = qEnvironmentVariable("WINSHUN_TRAY_EVIDENCE");
                    if (!folder.isEmpty()) {
                        const QImage image
                            = QGuiApplication::primaryScreen()->grabWindow(reinterpret_cast<WId>(window)).toImage();
                        const QString file = QDir(folder).filePath(QString::fromLatin1(QTest::currentDataTag()) + u".png"_s);
                        if (image.isNull() || !image.save(file))
                            styled = false;
                    }
                    ::PostMessageW(owner, WM_KEYDOWN, VK_DOWN, 0);
                    QTimer::singleShot(70, &lifetime, [owner] { ::PostMessageW(owner, WM_KEYDOWN, VK_RETURN, 0); });
                });
            });
        };
        capture::PinManager pins(std::move(callbacks));
        pins.pin(fixture, QPoint(650, 200));
        QCOMPARE(pins.count(), 1);
        HWND pin = nullptr;
        ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
            wchar_t name[64] {};
            ::GetClassNameW(window, name, 64);
            if (std::wstring_view(name) == L"WinShun.Pin") {
                *reinterpret_cast<HWND*>(data) = window;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&pin));
        QVERIFY(pin);
        if (collapsed)
            ::SendMessageW(pin, WM_KEYDOWN, VK_SPACE, 0);
        ::SendMessageW(pin, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(pin), MAKELPARAM(700, 300));
        QVERIFY(!timedOut);
        QVERIFY(observed);
        QVERIFY(styled);
        QCOMPARE(copied, fixture); // a real menu choice reaches the pin's copy callback
        QCOMPARE(pins.count(), 1);
    }

    void keyboard_data()
    {
        QTest::addColumn<QList<int>>("keys");
        QTest::addColumn<int>("expected");
        QTest::newRow("action") << QList<int> {VK_DOWN, VK_DOWN, VK_RETURN} << 2;
        // Native menus focus disabled rows, but cannot execute them.
        QTest::newRow("disabled-inert") << QList<int> {VK_DOWN, VK_DOWN, VK_DOWN, VK_DOWN, VK_RETURN} << 0;
        QTest::newRow("past-disabled") << QList<int> {VK_DOWN, VK_DOWN, VK_DOWN, VK_DOWN, VK_DOWN, VK_RETURN} << 5;
        // Native submenus with a leading separator open without a focused
        // command; Down enters the first command, skipping the padding.
        QTest::newRow("submenu-first-command") << QList<int> {VK_UP, VK_RIGHT, VK_DOWN, VK_RETURN} << 21;
        QTest::newRow("plugin-submenu") << QList<int> {VK_UP, VK_RIGHT, VK_DOWN, VK_DOWN, VK_DOWN, VK_RETURN} << 23;
        QTest::newRow("cancel-submenu") << QList<int> {VK_UP, VK_RIGHT, VK_ESCAPE, VK_ESCAPE} << 0;
        QTest::newRow("explicit-mnemonic") << QList<int> {'X'} << 10;
        QTest::newRow("duplicate-prefix") << QList<int> {'C', 'C', VK_RETURN} << 8;
    }

    void keyboard()
    {
        if (qEnvironmentVariableIntValue("WINSHUN_TRAYMENU_LIVE") != 1)
            QSKIP("Opt in with WINSHUN_TRAYMENU_LIVE=1");
        QFETCH(QList<int>, keys);
        QFETCH(int, expected);
        const HWND previous = ::GetForegroundWindow();
        const auto restore = qScopeGuard([&] {
            if (::IsWindow(previous))
                ::SetForegroundWindow(previous);
        });
        QQmlEngine engine;
        QObject* theme = engine.singletonInstance<QObject*>(u"WinShun"_s, u"Theme"_s);
        QWindow owner;
        owner.setTitle(u"WinShun menu keyboard verification"_s);
        owner.show();
        QVERIFY(QTest::qWaitForWindowExposed(&owner));
        const HWND handle = reinterpret_cast<HWND>(owner.winId());
        QObject lifetime;
        bool timedOut = false;
        QTimer watchdog;
        connect(&watchdog, &QTimer::timeout, &lifetime, [&] {
            timedOut = true;
            ::EndMenu();
        });
        watchdog.start(4000);
        const int chosen = traymenu::popup(handle, entries(true), POINT {200, 180}, theme, [&](HWND, HMENU) {
            for (int i = 0; i < keys.size(); ++i)
                QTimer::singleShot(80 + i * 70, &lifetime, [handle, key = keys[i]] {
                    ::PostMessageW(handle, key >= 'A' && key <= 'Z' ? WM_CHAR : WM_KEYDOWN, key, 0);
                });
        });
        QVERIFY(!timedOut);
        QCOMPARE(chosen, expected);
    }

    // Theme writes the look C++ draws by (SystemTheme::nativeLook): every
    // style's, squared by the 直角 setting; fluent's the very colours the
    // native surfaces had before it (the break notice, the pins' edge).
    void nativeLook_data()
    {
        QTest::addColumn<QString>("style");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("round");
        for (const char* style : {"fluent", "brand", "tactile"}) {
            for (const bool dark : {false, true}) {
                QTest::addRow("%s-%s", style, dark ? "dark" : "light") << QString::fromLatin1(style) << dark << true;
                QTest::addRow("%s-%s-square", style, dark ? "dark" : "light") << QString::fromLatin1(style) << dark << false;
            }
        }
    }

    void nativeLook()
    {
        QFETCH(QString, style);
        QFETCH(bool, dark);
        QFETCH(bool, round);
        QGuiApplication::styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
        SystemTheme::setRoundedCorners(round);
        SystemTheme::setUiStyle(u"fluent"_s);
        int told = 0;
        QObject receiver;
        SystemTheme::onNativeLookChanged(&receiver, [&] { ++told; });
        QQmlEngine engine;
        QObject* theme = engine.singletonInstance<QObject*>(u"WinShun"_s, u"Theme"_s);
        QVERIFY(theme);
        QCOMPARE(SystemTheme::nativeLook().value(u"style"_s).toString(), u"fluent"_s);
        SystemTheme::setUiStyle(style);
        QCoreApplication::processEvents();
        told = 0;
        SystemTheme::setUiStyle(style == u"fluent" ? u"brand"_s : u"fluent"_s);
        SystemTheme::setUiStyle(style); // one change and back: told once at most, after the loop turns
        QVERIFY(told == 0);
        QCoreApplication::processEvents();
        QVERIFY(told <= 1);

        const QVariantMap look = SystemTheme::nativeLook();
        QCOMPARE(look.value(u"style"_s).toString(), style);
        QCOMPARE(look.value(u"dark"_s).toBool(), dark);
        QCOMPARE(look.value(u"rounded"_s).toBool(), round);
        for (const char* radius : {"cardRadius", "controlRadius", "menuRadius", "menuItemRadius"}) {
            const qreal value = SystemTheme::nativeReal(radius, -1);
            QVERIFY2(value >= 0, radius);
            if (!round)
                QVERIFY2(value == 0, radius);
        }
        const auto rgb = [](const char* name) { return SystemTheme::nativeColor(name, QColor()).name(QColor::HexArgb); };
        for (const char* name : {"background", "windowEdge", "text", "tagFill", "menuBackground", "menuHover", "accentFill",
                 "accentText", "textSelection", "controlBorder"})
            QVERIFY2(SystemTheme::nativeColor(name, QColor()).isValid(), name);
        if (style == u"fluent") {
            QCOMPARE(rgb("background"), dark ? u"#ff202020"_s : u"#fff9f9f9"_s);
            QCOMPARE(rgb("windowEdge"), dark ? u"#ff6a6a6a"_s : u"#ffb4b4b4"_s);
            QCOMPARE(rgb("text"), dark ? u"#fff3f3f3"_s : u"#ff1b1b1b"_s);
            QCOMPARE(rgb("tagFill"), dark ? u"#12ffffff"_s : u"#0d000000"_s);
            QCOMPARE(SystemTheme::nativeReal("menuItemRadius", -1), round ? 4.0 : 0.0);
            QCOMPARE(SystemTheme::nativeReal("controlRadius", -1), round ? 4.0 : 0.0);
            QCOMPARE(SystemTheme::nativeReal("menuRadius", -1), round ? 8.0 : 0.0);
            QCOMPARE(SystemTheme::nativeReal("menuPadding", -1), 4.0);
            QCOMPARE(SystemTheme::nativeColor("caret", Qt::red).alpha(), 0); // none of its own
        } else {
            // A menu window's corner is DWM's in every style.
            QCOMPARE(SystemTheme::nativeReal("menuRadius", -1), round ? 8.0 : 0.0);
            QCOMPARE(SystemTheme::nativeReal("menuItemRadius", -1), round ? 4.0 : 0.0);
            QVERIFY(SystemTheme::nativeColor("caret", QColor()).alpha() > 0);
        }
        const QJsonObject json = SystemTheme::nativeLookJson();
        QCOMPARE(json.value(u"background"_s).toString(), rgb("background"));
        QCOMPARE(json.value(u"rounded"_s).toBool(), round);
        if (const QString folder = qEnvironmentVariable("WINSHUN_NATIVE_LOOK_DUMP"); !folder.isEmpty()) {
            QVERIFY(QDir().mkpath(folder));
            QFile file(QDir(folder).filePath(QString::fromLatin1(QTest::currentDataTag()) + u".json"_s));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(json).toJson());
        }
        SystemTheme::setUiStyle(u"fluent"_s);
        SystemTheme::setRoundedCorners(true);
    }

    // The menu in each style, drawn off screen (traymenu::render): the
    // style's menu colours, its item radius and inset; a hover that would
    // hardly show on the menu (tactile's key face) gets the key's rim.
    void styledRows_data()
    {
        QTest::addColumn<QString>("style");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<qreal>("scale");
        for (const char* style : {"fluent", "brand", "tactile"}) {
            for (const bool dark : {false, true}) {
                for (const qreal scale : {1.0, 1.5})
                    QTest::addRow("%s-%s-%d", style, dark ? "dark" : "light", int(scale * 100))
                        << QString::fromLatin1(style) << dark << scale;
            }
        }
    }

    void styledRows()
    {
        QFETCH(QString, style);
        QFETCH(bool, dark);
        QFETCH(qreal, scale);
        QGuiApplication::styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
        SystemTheme::setUiStyle(style);
        QQmlEngine engine;
        QObject* theme = engine.singletonInstance<QObject*>(u"WinShun"_s, u"Theme"_s);
        QVERIFY(theme);
        const QImage image = traymenu::render(entries(false), theme, scale, 1);
        QVERIFY(!image.isNull());
        if (const QString folder = qEnvironmentVariable("WINSHUN_TRAY_EVIDENCE"); !folder.isEmpty()) {
            QVERIFY(QDir().mkpath(folder));
            QVERIFY(image.save(QDir(folder).filePath(QString::fromLatin1(QTest::currentDataTag()) + u".png"_s)));
        }
        const QColor background = theme->property("menuBackground").value<QColor>();
        const QColor hover = theme->property("menuHover").value<QColor>();
        const auto opaque = [&background](const QColor& c) {
            const double t = c.alphaF();
            return QColor::fromRgbF(float(c.redF() * t + background.redF() * (1 - t)),
                float(c.greenF() * t + background.greenF() * (1 - t)), float(c.blueF() * t + background.blueF() * (1 - t)));
        };
        const auto similar = [](const QColor& a, const QColor& b) {
            return std::abs(a.red() - b.red()) <= 2 && std::abs(a.green() - b.green()) <= 2 && std::abs(a.blue() - b.blue()) <= 2;
        };
        // Row 2 (剪贴板历史) is highlighted: 6 px padding, then a 28 px row.
        const int top = qRound(6 * scale), rowHeight = qRound(28 * scale);
        const int middle = top + rowHeight + rowHeight / 2;
        QVERIFY2(similar(image.pixelColor(1, middle), background), "the menu's own background left of the hover");
        const int inset = qRound(theme->property("menuPadding").toReal() * scale);
        QVERIFY2(similar(image.pixelColor(inset + qRound(2 * scale) + 1, middle), opaque(hover)), "the hover's fill");
        QVERIFY2(similar(image.pixelColor(inset - 2, middle), background), "the hover starts at menuPadding");
        SystemTheme::setUiStyle(u"fluent"_s);
    }

    // The delayed capture's countdown as the capture plugin makes it: the
    // style's accentFill and menuRadius (0 in the 直角 setting).
    void countdown_data() { nativeLook_data(); }

    void countdown()
    {
        QFETCH(QString, style);
        QFETCH(bool, dark);
        QFETCH(bool, round);
        QGuiApplication::styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
        SystemTheme::setRoundedCorners(round);
        SystemTheme::setUiStyle(style);
        QQmlEngine engine;
        QVERIFY(engine.singletonInstance<QObject*>(u"WinShun"_s, u"Theme"_s));
        const QColor fill = SystemTheme::nativeColor("accentFill", QColor());
        const qreal radius = SystemTheme::nativeReal("menuRadius", -1);
        QVERIFY(fill.isValid() && radius >= 0);
        for (const double dpr : {1.0, 1.5}) {
            const QImage image = capture::CountdownWindow::picture(3, fill, radius, dpr);
            QCOMPARE(image.pixelColor(0, 0).alpha(), round ? 0 : 255); // the corner
            const QColor middle = image.pixelColor(image.width() - 4, image.height() / 2);
            QCOMPARE(middle.rgb(), fill.rgb());
            if (const QString folder = qEnvironmentVariable("WINSHUN_TRAY_EVIDENCE"); !folder.isEmpty())
                QVERIFY(image.save(QDir(folder).filePath(u"countdown-%1-%2.png"_s.arg(QString::fromLatin1(QTest::currentDataTag())).arg(int(dpr * 100)))));
        }
        SystemTheme::setUiStyle(u"fluent"_s);
        SystemTheme::setRoundedCorners(true);
    }

    void cleanupTestCase()
    {
        if (SUCCEEDED(m_com))
            ::CoUninitialize();
    }
};

QTEST_MAIN(TrayMenuTest)
#include "tst_traymenu.moc"
