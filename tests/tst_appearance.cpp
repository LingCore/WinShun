#include "Phosphor.h"
#include "PhosphorIcons.generated.h"
#include "Settings.h"
#include "PhosphorGlyph.h"
#include "SystemTheme.h"
#include "WindowFrame.h"
#include "platform/WindowEffects.h"
#include "platform/Foreground.h"

#include <QDir>
#include <QFile>
#include <QFontInfo>
#include <QGuiApplication>
#include <QRawFont>
#include <QOperatingSystemVersion>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QScreen>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>

#include <dwmapi.h>
#include <cmath>

using namespace ws;
using namespace Qt::StringLiterals;

namespace {

// The app's QML folder, with SystemTheme and Theme registered and the icon
// fonts loaded (once).
QUrl qmlFolder()
{
    static const QUrl folder = [] {
        phosphor::loadFonts();
        qmlRegisterSingletonType<SystemTheme>("WinShun", 1, 0, "SystemTheme",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new SystemTheme; });
        qmlRegisterType<PhosphorGlyph>("WinShun", 1, 0, "PhosphorGlyph");
        const QUrl url = QUrl::fromLocalFile(QStringLiteral(WINSHUN_QML_DIR) + u'/');
        qmlRegisterSingletonType(url.resolved(QUrl(u"Theme.qml"_s)), "WinShun", 1, 0, "Theme");
        return url;
    }();
    return folder;
}

} // namespace

class AppearanceTest : public QObject {
    Q_OBJECT

private slots:
    void inactiveBackdrop()
    {
        if (!win::backdropSupported()) QSKIP("Native backdrop needs Windows 11 22H2");
        const auto restore = qScopeGuard([] { win::setBackdropMaterial(win::BackdropMaterial::Mica); });
        QQuickWindow window;
        QQuickWindow plain;
        window.setFlags(Qt::Window | Qt::FramelessWindowHint);
        window.setColor(Qt::transparent);
        const HWND hwnd = reinterpret_cast<HWND>(window.winId());
        const HWND other = reinterpret_cast<HWND>(plain.winId());
        const HWND foreground = ::GetForegroundWindow();
        win::setBackdropMaterial(win::BackdropMaterial::Acrylic);
        QVERIFY(!win::backdropFrameActive(hwnd, false)); // unregistered windows stay opaque
        win::styleFramelessWindow(&window, true);
        QCOMPARE(win::backdropFrameActive(hwnd, false), win::materialsEnabled());
        QVERIFY(win::backdropFrameActive(hwnd, true));
        QVERIFY(!win::backdropFrameActive(other, false));
        int material = 0;
        QCOMPARE(::DwmGetWindowAttribute(hwnd, 38, &material, sizeof material), S_OK);
        QCOMPARE(material, 3);
        win::setBackdropMaterial(win::BackdropMaterial::Mica);
        QVERIFY(!win::backdropFrameActive(hwnd, false));
        QCOMPARE(::DwmGetWindowAttribute(hwnd, 38, &material, sizeof material), S_OK);
        QCOMPARE(material, 2);
        win::setBackdropMaterial(win::BackdropMaterial::Acrylic);
        QCOMPARE(win::backdropFrameActive(hwnd, false), win::materialsEnabled());
        QCOMPARE(::GetForegroundWindow(), foreground);
        QVERIFY(!window.isActive()); // visual activation never grants input focus
        win::styleFramelessWindow(&window, false);
        QVERIFY(!win::backdropFrameActive(hwnd, false));
    }

    void disabledFields_data()
    {
        QTest::addColumn<QString>("file");
        QTest::newRow("input") << u"InputBox.qml"_s;
        QTest::newRow("toggle") << u"ToggleSwitch.qml"_s;
    }

    void disabledFields()
    {
        QFETCH(QString, file);
        QTest::failOnWarning();
        const auto restore = qScopeGuard([] { SystemTheme::setUiStyle(u"fluent"_s); });
        SystemTheme::setUiStyle(u"liquid"_s);
        QQmlEngine engine;
        {
            QQmlComponent component(&engine, qmlFolder().resolved(QUrl(file)));
            std::unique_ptr<QObject> object(component.create());
            QVERIFY2(object, qPrintable(component.errorString()));
            auto* item = qobject_cast<QQuickItem*>(object.get());
            QVERIFY(item);
            if (file.startsWith(u"Input")) object->setProperty("text", u"Saved value"_s);
            else object->setProperty("checked", true);
            QCOMPARE(item->opacity(), 1.0);
            item->setEnabled(false);
            QTRY_VERIFY_WITH_TIMEOUT(item->opacity() < 1.0, 500);
            if (file.startsWith(u"Input")) QCOMPARE(object->property("text").toString(), u"Saved value"_s);
            else QVERIFY(object->property("checked").toBool());
            item->setEnabled(true);
            QTRY_COMPARE_WITH_TIMEOUT(item->opacity(), 1.0, 500);
        }
    }

    void glassAppearance()
    {
        QTest::failOnWarning();
        QTemporaryDir profile;
        Settings::useProfile(profile.path());
        Settings original = Settings::defaults();
        QCOMPARE(original.glassTransparency, 50);
        QCOMPARE(original.glassClarity, 50);
        original.glassTransparency = 81;
        original.glassClarity = 23;
        QVERIFY(original.saveChecked());
        Settings restored;
        restored.load();
        QCOMPARE(restored.glassTransparency, 81);
        QCOMPARE(restored.glassClarity, 23);
        QSettings ini(Settings::filePath(), QSettings::IniFormat);
        ini.setValue(u"Appearance/GlassTransparency"_s, -30);
        ini.setValue(u"Appearance/GlassClarity"_s, 150);
        ini.sync();
        restored.load();
        QCOMPARE(restored.glassTransparency, 0);
        QCOMPARE(restored.glassClarity, 100);
        const auto cleanup = qScopeGuard([] {
            SystemTheme::setGlassAppearance(50, 50);
            SystemTheme::setBackdropAvailable(false);
        });
        SystemTheme::setBackdropAvailable(true);
        SystemTheme::setBackdropOn(true);
        QQmlEngine engine;
        QQmlComponent source(&engine, qmlFolder().resolved(QUrl(u"themes/liquid/Look.qml"_s)));
        std::unique_ptr<QObject> look(source.create());
        QVERIFY2(look, qPrintable(source.errorString()));
        const QColor ink = look->property("text").value<QColor>();
        SystemTheme::setGlassAppearance(0, 50);
        QCOMPARE(look->property("windowTint").value<QColor>().alphaF(), 1.0);
        SystemTheme::setGlassAppearance(50, 50);
        const auto defaultTint = look->property("windowTint").value<QColor>();
        QVERIFY(defaultTint.alphaF() > 0 && defaultTint.alphaF() < 1);
        const auto defaultCard = look->property("cardFill").value<QColor>();
        SystemTheme::setGlassAppearance(100, 50);
        QCOMPARE(look->property("windowTint").value<QColor>().alphaF(), 0.0);
        QCOMPARE(look->property("cardFill").value<QColor>(), defaultCard);
        if (win::materialsEnabled()) {
            SystemTheme::setGlassAppearance(100, 0);
            const auto softCard = look->property("cardFill").value<QColor>();
            SystemTheme::setGlassAppearance(100, 100);
            QVERIFY(softCard.alphaF() < defaultCard.alphaF());
            QCOMPARE(look->property("cardFill").value<QColor>().alphaF(), 1.0);
        }
        QCOMPARE(look->property("text").value<QColor>(), ink);
        SystemTheme::setBackdropOn(false);
        SystemTheme::setGlassAppearance(0, 0);
        const auto solid = look->property("cardFill").value<QColor>();
        SystemTheme::setGlassAppearance(100, 100);
        QCOMPARE(look->property("cardFill").value<QColor>(), solid);
        QCOMPARE(solid.alphaF(), 1.0);
    }

    void styleRoundTrip()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        Settings::useProfile(profile.path());
        for (const QString& style : {u"fluent"_s, u"brand"_s, u"tactile"_s, u"liquid"_s}) {
            Settings settings = Settings::defaults();
            settings.uiStyle = style;
            QVERIFY(settings.saveChecked());
            Settings restored;
            restored.load();
            QCOMPARE(restored.uiStyle, style);
        }
        for (const QString& value : {u" LIQUID "_s, u"invalid"_s}) {
            QSettings ini(Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Appearance/UiStyle"_s, value);
            ini.sync();
            Settings restored;
            restored.load();
            QCOMPARE(restored.uiStyle, value == u" LIQUID " ? u"liquid"_s : u"fluent"_s);
        }
    }

    void liveBackdropSwitch()
    {
        if (!win::backdropSupported())
            QSKIP("Native materials require Windows 11 22H2 or later");
        const auto restore = qScopeGuard([] { SystemTheme::setUiStyle(u"fluent"_s); });
        QWindow translucent, opaque;
        translucent.setFlags(Qt::Tool | Qt::FramelessWindowHint);
        opaque.setFlags(translucent.flags());
        SystemTheme::setUiStyle(u"fluent"_s);
        win::styleFramelessWindow(&translucent, true);
        win::styleFramelessWindow(&opaque, false);
        const auto material = [](QWindow& window) {
            int value = -1;
            ::DwmGetWindowAttribute(reinterpret_cast<HWND>(window.winId()), 38, &value, sizeof value);
            return value;
        };
        for (const QString& style : {u"liquid"_s, u"brand"_s, u"liquid"_s, u"tactile"_s, u"fluent"_s}) {
            SystemTheme::setUiStyle(style);
            QCOMPARE(SystemTheme::uiStyle(), style);
            const int expected = style == u"liquid" ? 3 : 2;
            QCOMPARE(material(translucent), expected);
            QCOMPARE(material(opaque), 1);
            QWindow createdAfter;
            createdAfter.setFlags(translucent.flags());
            win::styleFramelessWindow(&createdAfter, true);
            QCOMPARE(material(createdAfter), expected);
        }
        // Reconfiguring an existing window as opaque must remove its opt-in.
        win::styleFramelessWindow(&translucent, false);
        SystemTheme::setUiStyle(u"liquid"_s);
        QCOMPARE(material(translucent), 1);
    }

    void liquidParts_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("round");
        QTest::addColumn<bool>("backdrop");
        for (const bool dark : {false, true})
            for (const bool round : {false, true})
                for (const bool backdrop : {false, true})
                    QTest::addRow("%s-%s-%s", dark ? "dark" : "light", round ? "round" : "square", backdrop ? "native" : "solid")
                        << dark << round << backdrop;
    }

    void liquidParts()
    {
        QFETCH(bool, dark);
        QFETCH(bool, round);
        QFETCH(bool, backdrop);
        QTest::failOnWarning();
        const auto restore = qScopeGuard([] {
            SystemTheme::setUiStyle(u"fluent"_s);
            SystemTheme::setRoundedCorners(true);
            SystemTheme::setBackdropAvailable(false);
        });
        QGuiApplication::styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
        SystemTheme::setRoundedCorners(round);
        SystemTheme::setBackdropAvailable(backdrop);
        SystemTheme::setBackdropOn(true);
        QQmlEngine engine;
        QQmlComponent source(&engine, qmlFolder().resolved(QUrl(u"themes/liquid/Look.qml"_s)));
        std::unique_ptr<QObject> look(source.create());
        QVERIFY2(look, qPrintable(source.errorString()));
        QCOMPARE(look->property("name").toString(), u"liquid"_s);
        QCOMPARE(look->property("backdrop").toBool(), backdrop && win::materialsEnabled());
        for (const char* hook : {"windowFrame", "searchField", "inputField", "keySurface", "segmented", "chipSurface",
                 "rowHighlight", "menuFrame", "toggle", "keyHint", "separator", "scrollBar", "card"}) {
            auto* component = look->property(hook).value<QQmlComponent*>();
            QVERIFY2(component, hook);
            std::unique_ptr<QObject> part(component->create(component->creationContext()));
            QVERIFY2(part, qPrintable(component->errorString()));
            const auto checkRadii = [&](auto&& self, QObject* object) -> void {
                if (!round && object->property("radius").isValid())
                    QCOMPARE(object->property("radius").toReal(), 0.0);
                for (QObject* child : object->children()) self(self, child);
            };
            checkRadii(checkRadii, part.get());
            // Exercise live state changes, not just the default object.
            for (const char* state : {"focused", "hovered", "pressed", "checked", "current", "error"})
                if (part->metaObject()->indexOfProperty(state) >= 0)
                    QVERIFY(part->setProperty(state, true));
            checkRadii(checkRadii, part.get());
        }
    }

    void liveComposedOutline_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("backdrop");
        for (const bool dark : {false, true}) {
            for (int kind = 0; kind < 3; ++kind) {
                const QByteArray name = QByteArray(dark ? "dark-" : "light-") + QByteArray::number(kind);
                QTest::newRow(name.constData()) << kind << dark << false;
            }
        }
        QTest::newRow("light-backdrop") << 0 << false << true;
        QTest::newRow("dark-backdrop") << 0 << true << true;
    }

    void liveComposedOutline()
    {
        QFETCH(int, kind);
        QFETCH(bool, dark);
        QFETCH(bool, backdrop);
        const QString output = qEnvironmentVariable("WINSHUN_APPEARANCE_LIVE");
        if (output.isEmpty())
            QSKIP("Opt in to desktop-composed window checks with WINSHUN_APPEARANCE_LIVE=<output folder>");
        if (backdrop && !win::backdropSupported())
            QSKIP("Mica requires Windows 11 22H2 or later");
        QVERIFY(QDir().mkpath(output));
        QGuiApplication::styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
        SystemTheme::setRoundedCorners(false);
        QQmlEngine engine;
        QQmlComponent edge(&engine, qmlFolder().resolved(QUrl(u"WindowEdge.qml"_s)));
        // Keep a known, owned background behind the complete window rectangle:
        // grabWindow() on the client alone cannot see a missing DWM outline.
        QQuickWindow background;
        background.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
        background.setColor(QColor("#CC3399"));
        background.setGeometry(100, 100, 420, 280);
        background.show();
        QVERIFY(QTest::qWaitForWindowExposed(&background));
        QQuickWindow window;
        window.setFlags(background.flags());
        window.setTransientParent(&background);
        window.setColor(backdrop ? QColor(Qt::transparent) : QColor(dark ? "#202020" : "#F9F9F9"));
        if (backdrop) {
            auto format = window.requestedFormat();
            format.setAlphaBufferSize(8);
            window.setFormat(format);
        }
        window.setGeometry(160, 160, 240, 120);
        std::unique_ptr<QObject> object(edge.create());
        QVERIFY2(object, qPrintable(edge.errorString()));
        auto* item = qobject_cast<QQuickItem*>(object.get());
        QVERIFY(item);
        item->setParentItem(window.contentItem());
        // Menu, resizable clipboard/launcher, and settings with its own title bar.
        WindowFrame frame(kind == 2);
        if (kind != 0) {
            frame.setWindow(&window);
            frame.setResizable(true);
        }
        win::styleFramelessWindow(&window, backdrop);
        win::setDarkFrame(&window, dark);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        qInfo() << "Desktop composition DPR:" << window.devicePixelRatio();
        int step = 0;
        for (const bool round : {false, true, false}) {
            if (kind == 1 && step == 2)
                frame.setResizable(false); // Leaving a resizable panel must retain its shadow.
            SystemTheme::setRoundedCorners(round);
            const bool native = QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11;
            QTRY_COMPARE(item->isVisible(), !native);
            QTest::qWait(120);
            ::DwmFlush();
            const QImage pixels = window.screen()->grabWindow(0, window.x() - 8, window.y() - 8,
                window.width() + 16, window.height() + 16).toImage();
            QVERIFY(!pixels.isNull());
            const QString name = u"%1-%2-%3-%4.png"_s.arg(dark ? u"dark"_s : u"light"_s)
                .arg(backdrop ? u"backdrop"_s : QString::number(kind))
                .arg(round ? u"round"_s : u"square"_s).arg(step++);
            QVERIFY(pixels.save(QDir(output).filePath(name)));
            const qreal desktopScale = pixels.devicePixelRatio();
            const QPoint shadowPoint(qRound(6 * desktopScale), pixels.height() / 2);
            QVERIFY2(pixels.pixelColor(shadowPoint) != background.color(), "Native shadow missing outside the window");
            RECT client {};
            QVERIFY(::GetClientRect(reinterpret_cast<HWND>(window.winId()), &client));
            QCOMPARE(client.right, qRound(window.width() * window.devicePixelRatio()));
            QCOMPARE(client.bottom, qRound(window.height() * window.devicePixelRatio()));
            if (kind == 0 || (kind == 1 && step == 3)) {
                QVERIFY(!(::GetWindowLongPtrW(reinterpret_cast<HWND>(window.winId()), GWL_STYLE) & WS_THICKFRAME));
                RECT bounds {};
                QVERIFY(::GetWindowRect(reinterpret_cast<HWND>(window.winId()), &bounds));
                QCOMPARE(::SendMessageW(reinterpret_cast<HWND>(window.winId()), WM_NCHITTEST, 0,
                    MAKELPARAM(bounds.left + 1, (bounds.top + bounds.bottom) / 2)), LRESULT(HTCLIENT));
            }
            if (!round) {
                const qreal scale = pixels.devicePixelRatio();
                const int left = qRound(8 * scale), top = qRound(8 * scale);
                const int right = left + qRound(window.width() * scale) - 1;
                const int bottom = top + qRound(window.height() * scale) - 1;
                const int middleX = (left + right) / 2, middleY = (top + bottom) / 2;
                const QList<QPoint> sides {QPoint(middleX, top), QPoint(middleX, bottom),
                    QPoint(left, middleY), QPoint(right, middleY)};
                if (native) {
                    for (const QPoint& point : sides)
                        QVERIFY(pixels.pixelColor(point) != window.color());
                    // Prove these pixels belong to DWM, rather than a matching
                    // client stroke. BORDER_COLOR is set-only; do not read it.
                    constexpr COLORREF probeColor = RGB(17, 181, 91);
                    const HWND hwnd = reinterpret_cast<HWND>(window.winId());
                    QVERIFY(SUCCEEDED(::DwmSetWindowAttribute(hwnd, 34, &probeColor, sizeof probeColor)));
                    QTest::qWait(120);
                    ::DwmFlush();
                    const QImage nativePixels = window.screen()->grabWindow(0, window.x() - 8, window.y() - 8,
                        window.width() + 16, window.height() + 16).toImage();
                    QVERIFY(nativePixels.save(QDir(output).filePath(u"native-"_s + name)));
                    constexpr COLORREF defaultColor = 0xFFFFFFFF;
                    QVERIFY(SUCCEEDED(::DwmSetWindowAttribute(hwnd, 34, &defaultColor, sizeof defaultColor)));
                    for (const QPoint& point : sides)
                        QCOMPARE(nativePixels.pixelColor(point), QColor(17, 181, 91));
                } else {
                    const QColor outline(dark ? "#6A6A6A" : "#B4B4B4");
                    for (const QPoint& point : sides)
                        QCOMPARE(pixels.pixelColor(point), outline);
                }
                if (!backdrop)
                    QCOMPARE(pixels.pixelColor(left + int(std::ceil(scale)) + 1, middleY), window.color());
            }
        }
    }

    void shellHosts()
    {
        for (const auto name : {u"SearchHost.exe", u"StartMenuExperienceHost.exe", u"SearchApp.exe", u"searchui.EXE"}) {
            QVERIFY(foreground::isSearchHost(QStringView(name)));
            QVERIFY(foreground::isShellFlyout(QStringView(name)));
        }
        for (const auto name : {u"ShellHost.exe", u"ShellExperienceHost.exe"}) {
            QVERIFY(!foreground::isSearchHost(QStringView(name)));
            QVERIFY(foreground::isShellFlyout(QStringView(name)));
        }
        for (const auto name : {u"", u"explorer.exe", u"notepad.exe", u"SearchApp.exe.bak"}) {
            QVERIFY(!foreground::isSearchHost(QStringView(name)));
            QVERIFY(!foreground::isShellFlyout(QStringView(name)));
        }
    }

    void settingsRoundTrip()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        Settings::useProfile(profile.path());
        const QString expected = QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11
            ? u"round"_s : u"square"_s;
        QCOMPARE(Settings::defaults().corners, expected);
        Settings settings;
        settings.load();
        QCOMPARE(settings.corners, expected);
        for (const QString& value : {u"square"_s, u"round"_s}) {
            settings.corners = value;
            settings.save();
            Settings loaded;
            loaded.load();
            QCOMPARE(loaded.corners, value);
        }
        for (const QString& value : {u"invalid"_s, u""_s, u" SQUARE "_s}) {
            QSettings ini(Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Appearance/Corners"_s, value);
            ini.sync();
            settings.load();
            QCOMPARE(settings.corners, value == u" SQUARE " ? u"square"_s : expected);
        }
    }

    void liveWindows()
    {
        if (QOperatingSystemVersion::current() < QOperatingSystemVersion::Windows11)
            QSKIP("DWM corner attributes require Windows 11");
        QWindow normal;
        normal.setFlags(Qt::Tool | Qt::FramelessWindowHint);
        win::styleFramelessWindow(&normal);
        const HWND page = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP,
            0, 0, 80, 60, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        QVERIFY(page);
        win::setWindowCorners(page, true);
        const auto preference = [](HWND hwnd) {
            int value = -1;
            return SUCCEEDED(::DwmGetWindowAttribute(hwnd, 33, &value, sizeof value)) ? value : -1;
        };
        for (bool round : {false, true, false, true}) {
            SystemTheme::setRoundedCorners(round);
            QCOMPARE(preference(reinterpret_cast<HWND>(normal.winId())), round ? 2 : 1);
            QCOMPARE(preference(page), round ? 3 : 1);
            QWindow createdAfter;
            createdAfter.setFlags(Qt::Tool | Qt::FramelessWindowHint);
            win::styleFramelessWindow(&createdAfter);
            QCOMPARE(preference(reinterpret_cast<HWND>(createdAfter.winId())), round ? 2 : 1);
        }
        ::DestroyWindow(page);
    }

    void liveTokensAndSquarePixels()
    {
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
        const QUrl folder = qmlFolder();
        QQmlEngine engine;
        QQmlComponent probe(&engine);
        probe.setData(R"(
            import QtQuick
            import WinShun 1.0
            QtObject {
                readonly property real radius: Theme.controlRadius
                readonly property real pill: Theme.pillRadius(26)
                readonly property color edge: Theme.windowEdge
                readonly property int header: Theme.headerHeight
                readonly property int row: Theme.rowHeight
                readonly property int body: Theme.uiBody
            }
        )", folder);
        std::unique_ptr<QObject> values(probe.create());
        QVERIFY2(values, qPrintable(probe.errorString()));
        QSignalSpy changed(values.get(), SIGNAL(radiusChanged()));
        for (bool round : {true, false, true, false}) {
            SystemTheme::setRoundedCorners(round);
            QCOMPARE(values->property("radius").toDouble(), round ? 4.0 : 0.0);
            QCOMPARE(values->property("pill").toDouble(), round ? 13.0 : 0.0);
        }
        QVERIFY(changed.count() >= 3);
        QSignalSpy resized(values.get(), SIGNAL(rowChanged()));
        const QStringList sizes {u"compact"_s, u"standard"_s, u"roomy"_s, u"standard"_s};
        for (int i = 0; i < sizes.size(); ++i) {
            SystemTheme::setUiSize(sizes[i]);
            const int step = i == 3 ? 1 : i;
            QCOMPARE(values->property("header").toInt(), 56 + 8 * step);
            QCOMPARE(values->property("row").toInt(), 52 + 8 * step);
            QCOMPARE(values->property("body").toInt(), 14 + 2 * step);
        }
        QCOMPARE(resized.count(), 4);

        QQuickWindow window;
        window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        window.resize(160, 90);
        window.setColor(QColor("#F9F9F9"));
        QQmlComponent edge(&engine, folder.resolved(QUrl(u"WindowEdge.qml"_s)));
        std::unique_ptr<QObject> object(edge.create());
        QVERIFY2(object, qPrintable(edge.errorString()));
        auto* item = qobject_cast<QQuickItem*>(object.get());
        QVERIFY(item);
        item->setParentItem(window.contentItem());
        win::styleFramelessWindow(&window);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const QImage pixels = window.grabWindow();
        QVERIFY(!pixels.isNull());
        const bool native = QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11;
        QCOMPARE(item->isVisible(), !native);
        const QColor border = native ? window.color() : values->property("edge").value<QColor>();
        // Exclude the final partially covered clear pixel at fractional DPR.
        const int right = native ? int(std::floor(window.width() * window.devicePixelRatio())) - 1 : pixels.width() - 1;
        const int bottom = native ? int(std::floor(window.height() * window.devicePixelRatio())) - 1 : pixels.height() - 1;
        for (const QPoint& p : {QPoint(0, 0), QPoint(right, 0),
                 QPoint(0, bottom), QPoint(right, bottom),
                 QPoint(1, 0), QPoint(0, 1)})
            QCOMPARE(pixels.pixelColor(p), border);
        QCOMPARE(item->property("antialiasing").toBool(), false);
    }

    // An icon the font lacks is told (an older Windows 10's Segoe MDL2
    // Assets has not every icon, and the text took a Chinese character from
    // another font there); one it has is not, nor anything in a font that is
    // not installed (drawn as it ever was).
    void iconFontGlyphs()
    {
        const QString view(QChar(0xE890));
        QVERIFY(SystemTheme::fontHas(u"Segoe MDL2 Assets"_s, view));
        QVERIFY(!SystemTheme::fontHas(u"Arial"_s, view));
        QVERIFY(!SystemTheme::fontHas(u"Arial"_s, u"A"_s + view));
        QVERIFY(SystemTheme::fontHas(u"Arial"_s, u"A"_s));
        QVERIFY(SystemTheme::fontHas(u"No Such Font 7f3a"_s, view));
        SystemTheme theme;
        QVERIFY(theme.hasGlyph(QString(QChar(0xED1A)))); // Hide: in this Windows' icon font
        QVERIFY(theme.hasGlyph(view));
        QVERIFY(theme.hasGlyph(QString()));

        // Glyph.qml, a code point Phosphor has no icon for: one the icon font
        // has as it is; one it lacks (U+E000 is in neither icon font) not at
        // all, in the room of one.
        QQmlEngine engine;
        QQmlComponent glyph(&engine, qmlFolder().resolved(QUrl(u"Glyph.qml"_s)));
        const QString minimize(QChar(0xE921)); // ChromeMinimize
        QVERIFY(!phosphor::forSegoe(minimize));
        std::unique_ptr<QObject> shown(glyph.createWithInitialProperties({{u"glyph"_s, minimize}}));
        QVERIFY2(shown, qPrintable(glyph.errorString()));
        QCOMPARE(shown->property("shown").toString(), minimize);
        std::unique_ptr<QObject> lacking(glyph.createWithInitialProperties({{u"glyph"_s, QString(QChar(0xE000))}, {u"size"_s, 20}}));
        QVERIFY2(lacking, qPrintable(glyph.errorString()));
        QCOMPARE(lacking->property("shown").toString(), QString());
        QCOMPARE(lacking->property("implicitWidth").toReal(), 20.0);
        QCOMPARE(lacking->property("implicitHeight").toReal(), 20.0);
    }

    // Every mapped icon is in the bundled font it is drawn with (read from
    // the resources, not by name: an installed "Phosphor" would hide a
    // missing one), and Glyph.qml draws it from there, in its box.
    void phosphorIcons()
    {
        const QUrl folder = qmlFolder(); // loads the fonts
        const auto fontFile = [](const QString& name) {
            QFile file(u":/fonts/"_s + name);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        const QRawFont regular(fontFile(u"Phosphor.ttf"_s), 16);
        const QRawFont fill(fontFile(u"Phosphor-Fill.ttf"_s), 16);
        QVERIFY(regular.isValid() && fill.isValid());
        const QList<QChar> codes = phosphor::segoeCodes();
        QVERIFY(codes.size() >= 80);
        for (const QChar code : codes) {
            const std::optional<phosphor::Icon> icon = phosphor::forSegoe(QString(code));
            QVERIFY2(icon, qPrintable(u"U+%1"_s.arg(uint(code.unicode()), 4, 16)));
            const QRawFont& font = icon->family == fill.familyName() ? fill : regular;
            QCOMPARE(icon->family, font.familyName());
            QVERIFY2(font.supportsCharacter(icon->text.front()), qPrintable(u"U+%1"_s.arg(uint(code.unicode()), 4, 16)));
        }
        QCOMPARE(phosphor::forSegoe(QString(QChar(0xE841)))->family, fill.familyName()); // PinnedFill
        QCOMPARE(phosphor::forSegoe(QString(QChar(0xE718)))->family, regular.familyName()); // Pin
        // SearchBar's filter keys (#279): the shown state is the filled funnel,
        // the idle one the outline. Both are Segoe code points that resolve
        // only through the alias table, so an upstream rebase must keep them.
        const auto filterShown = phosphor::forSegoe(QString(QChar(0xE16E)));
        const auto filterIdle = phosphor::forSegoe(QString(QChar(0xE71C)));
        QVERIFY(filterShown && filterIdle);
        QCOMPARE(filterShown->family, fill.familyName());
        QCOMPARE(filterIdle->family, regular.familyName());
        QCOMPARE(filterShown->text, filterIdle->text);
        QVERIFY(!phosphor::forSegoe(QString(2, QChar(0xE718))));
        QVERIFY(!phosphor::forSegoe(QString()));

        // View: Phosphor's eye, in a box of its size, the overlay left out;
        // the font's line box is its em square (the drawn size, no line gap).
        QQmlEngine engine;
        QQmlComponent glyph(&engine, folder.resolved(QUrl(u"Glyph.qml"_s)));
        const QString view(QChar(0xE890));
        std::unique_ptr<QObject> eye(glyph.createWithInitialProperties({{u"glyph"_s, view}, {u"size"_s, 20}, {u"overlay"_s, QString(QChar(0xE718))}}));
        QVERIFY2(eye, qPrintable(glyph.errorString()));
        QCOMPARE(eye->property("shown").toString(), phosphor::forSegoe(view)->text);
        QCOMPARE(eye->property("implicitWidth").toReal(), 20.0);
        QCOMPARE(eye->property("implicitHeight").toReal(), 20.0);
        int drawn = 0;
        for (const QQuickItem* item : eye->findChildren<QQuickItem*>()) {
            if (!item->isVisible() || item->property("text").toString().isEmpty())
                continue;
            ++drawn;
            const auto* outline = qobject_cast<const PhosphorGlyph*>(item); // not text: see PhosphorGlyph.h
            QVERIFY2(outline, item->metaObject()->className());
            QCOMPARE(outline->family(), regular.familyName());
            QVERIFY(qAbs(outline->em() - 20 * phosphor::kScale) <= 1);
        }
        QCOMPARE(drawn, 1);
        // A caption button keeps Windows' own close glyph.
        const QString close(QChar(0xE8BB));
        std::unique_ptr<QObject> caption(glyph.createWithInitialProperties({{u"glyph"_s, close}, {u"caption"_s, true}}));
        QVERIFY2(caption, qPrintable(glyph.errorString()));
        QCOMPARE(caption->property("shown").toString(), close);

        // Both families are what Qt picks by those names, and drawn: the
        // pinned pin (Fill) has much more ink than the outlined one.
        for (const QRawFont* font : {&regular, &fill})
            QCOMPARE(QFontInfo(QFont(font->familyName())).family(), font->familyName());
        QQmlComponent pins(&engine);
        pins.setData(R"(
            import QtQuick
            Row {
                Glyph { glyph: String.fromCharCode(0xE841); size: 32; color: "black" } // PinnedFill
                Glyph { glyph: String.fromCharCode(0xE718); size: 32; color: "black" } // Pin
            }
        )", folder);
        std::unique_ptr<QObject> row(pins.create());
        QVERIFY2(row, qPrintable(pins.errorString()));
        QQuickWindow window;
        window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        window.resize(64, 32);
        window.setColor(Qt::white);
        qobject_cast<QQuickItem*>(row.get())->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const QImage pixels = window.grabWindow();
        QVERIFY(!pixels.isNull());
        const auto ink = [&](int from, int to) {
            int dark = 0;
            for (int y = 0; y < pixels.height(); ++y)
                for (int x = from; x < to; ++x)
                    dark += qGray(pixels.pixel(x, y)) < 128;
            return dark;
        };
        const int half = pixels.width() / 2;
        const int pinned = ink(0, half);
        const int outline = ink(half, pixels.width());
        QVERIFY2(outline > 0 && pinned > outline * 3 / 2, qPrintable(u"pinned %1, outline %2"_s.arg(pinned).arg(outline)));
    }

    // The generated name table is the whole bundled icon set: every name
    // resolves in both weights, and a name nobody has never reaches the
    // system icon font (Glyph.qml keeps the room but draws nothing).
    void phosphorNames()
    {
        const QUrl folder = qmlFolder(); // loads the fonts
        const auto fontFile = [](const QString& name) {
            QFile file(u":/fonts/"_s + name);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        const QRawFont regular(fontFile(u"Phosphor.ttf"_s), 16);
        const QRawFont fill(fontFile(u"Phosphor-Fill.ttf"_s), 16);
        QVERIFY(regular.isValid() && fill.isValid());

        qsizetype named = 0;
        for (const phosphor::generated::Entry& entry : phosphor::generated::kNames) {
            const QString name = QString::fromLatin1(entry.name.data(), qsizetype(entry.name.size()));
            const std::optional<phosphor::Icon> icon = phosphor::forName(name);
            QVERIFY2(icon, qPrintable(name));
            QCOMPARE(icon->text, QString(QChar(entry.code)));
            QCOMPARE(icon->family, regular.familyName());
            QVERIFY2(regular.supportsCharacter(icon->text.front()), qPrintable(name));
            QVERIFY2(fill.supportsCharacter(icon->text.front()), qPrintable(name));
            const std::optional<phosphor::Icon> filled = phosphor::forName(name, phosphor::Weight::Fill);
            QVERIFY2(filled, qPrintable(name));
            QCOMPARE(filled->family, fill.familyName());
            QCOMPARE(filled->text, icon->text);
            ++named;
        }
        QCOMPARE(named, qsizetype(phosphor::generated::kNames.size()));
        QVERIFY(phosphor::generated::kIconCount == 1512);
        QVERIFY(!phosphor::forName(u"no-such-icon"_s));
        QVERIFY(!phosphor::forName(QString()));

        const std::optional<phosphor::Icon> heart = phosphor::forName(u"heart"_s);
        QVERIFY(heart);
        const std::optional<phosphor::Icon> bySpec = phosphor::resolve(u"ph:heart"_s);
        QVERIFY(bySpec);
        QCOMPARE(bySpec->text, heart->text);
        const std::optional<phosphor::Icon> filledSpec = phosphor::resolve(u"ph-fill:heart"_s);
        QVERIFY(filledSpec);
        QCOMPARE(filledSpec->family, fill.familyName());
        QVERIFY(!phosphor::resolve(u"ph:no-such-icon"_s));
        QVERIFY(!phosphor::resolve(u"ph:"_s));
        QVERIFY(!phosphor::resolve(u"heart"_s)); // a bare name is not a spec
        const QString pin(QChar(0xE718));
        QCOMPARE(phosphor::resolve(pin)->text, phosphor::forSegoe(pin)->text);

        QQmlEngine engine;
        QQmlComponent glyph(&engine, folder.resolved(QUrl(u"Glyph.qml"_s)));
        std::unique_ptr<QObject> byName(glyph.createWithInitialProperties({{u"iconName"_s, u"heart"_s}, {u"size"_s, 20}}));
        QVERIFY2(byName, qPrintable(glyph.errorString()));
        QCOMPARE(byName->property("shown").toString(), heart->text);
        std::unique_ptr<QObject> unknown(glyph.createWithInitialProperties({{u"iconName"_s, u"no-such-icon"_s}, {u"size"_s, 20}}));
        QVERIFY2(unknown, qPrintable(glyph.errorString()));
        QCOMPARE(unknown->property("shown").toString(), QString());
        QCOMPARE(unknown->property("missing").toBool(), true);
        QCOMPARE(unknown->property("implicitWidth").toReal(), 20.0);
    }

    // A symmetric icon is drawn symmetric in pixels at this scale (the
    // ctest runs 1, 1.25, 1.5 and 2; the em, 18 px, is 22.5 device pixels at
    // 1.25): the cancel x and the plus mirror onto themselves about their
    // diagonal, and their middle is as far right as it is down, within half
    // a device pixel of the box's; those and the save button's chevron
    // mirror left to right about a pixel centre or edge. Native text put the
    // x a fraction of a pixel lower than across and its axis between pixels
    // ("warped" at 150 %).
    void phosphorAxesAlike()
    {
        const QUrl folder = qmlFolder();
        QQmlEngine engine;
        QQmlComponent scene(&engine);
        scene.setData(R"(
            import QtQuick
            Rectangle {
                width: 120; height: 40; color: "white"
                Glyph { x: 8; y: 8; glyph: String.fromCharCode(0xE711); size: 16; color: "black" } // Cancel: x
                Glyph { x: 48; y: 8; glyph: String.fromCharCode(0xE710); size: 16; color: "black" } // Add: plus
                Glyph { x: 88; y: 8; glyph: String.fromCharCode(0xE70D); size: 16; color: "black" } // ChevronDown
            }
        )", folder);
        std::unique_ptr<QObject> root(scene.create());
        QVERIFY2(root, qPrintable(scene.errorString()));
        QQuickWindow window;
        window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        window.resize(120, 40);
        window.setColor(Qt::white);
        qobject_cast<QQuickItem*>(root.get())->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const QImage pixels = window.grabWindow();
        QVERIFY(!pixels.isNull());
        const qreal dpr = window.effectiveDevicePixelRatio();
        for (const int left : {8, 48, 88}) {
            const int from = qRound(left * dpr) - 4, top = qRound(8 * dpr) - 4, side = qRound(16 * dpr) + 8;
            const auto at = [&](int x, int y) {
                return x < 0 || x >= side ? 0.0 : 1 - qGray(pixels.pixel(from + x, top + y)) / 255.0;
            };
            double ink = 0, across = 0, down = 0, unlike = 0;
            for (int y = 0; y < side; ++y) {
                for (int x = 0; x < side; ++x) {
                    const double a = at(x, y);
                    ink += a;
                    across += a * (x + 0.5);
                    down += a * (y + 0.5);
                    unlike += std::abs(a - at(y, x));
                }
            }
            QVERIFY(ink > 0);
            // Left to right about the ink's own middle: on a pixel centre or edge.
            const int axis = qRound(2 * across / ink); // twice the middle
            double sideways = 0;
            for (int y = 0; y < side; ++y)
                for (int x = 0; x < side; ++x)
                    sideways += std::abs(at(x, y) - at(axis - 1 - x, y));
            const double middleX = across / ink - side / 2.0, middleY = down / ink - side / 2.0;
            const QString what = u"at %1: diagonal %2, sideways %3, middle %4, %5"_s.arg(left).arg(unlike / ink)
                                     .arg(sideways / ink).arg(middleX).arg(middleY);
            QVERIFY2(sideways / ink < 0.05, qPrintable(what));
            if (left == 88)
                continue; // the chevron: no diagonal, its ink not in the em's middle
            QVERIFY2(unlike / ink < 0.05, qPrintable(what));
            QVERIFY2(std::abs(middleX - middleY) < 0.05, qPrintable(what));
            QVERIFY2(std::abs(middleX) <= 0.55, qPrintable(what));
        }
    }
};

QTEST_MAIN(AppearanceTest)
#include "tst_appearance.moc"
