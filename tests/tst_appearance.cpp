#include "Phosphor.h"
#include "Settings.h"
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
        const QList<QQuickItem*> texts = eye->findChildren<QQuickItem*>();
        int drawn = 0;
        for (const QQuickItem* text : texts) {
            if (!text->isVisible() || text->property("text").toString().isEmpty())
                continue;
            ++drawn;
            QCOMPARE(text->property("font").value<QFont>().family(), regular.familyName());
            QVERIFY(qAbs(text->implicitHeight() - 20 * phosphor::kScale) <= 1);
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
};

QTEST_MAIN(AppearanceTest)
#include "tst_appearance.moc"
