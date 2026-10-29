#include "Phosphor.h"
#include "Settings.h"
#include "SystemTheme.h"
#include "platform/WindowEffects.h"

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
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>

#include <dwmapi.h>

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
        const QColor border = values->property("edge").value<QColor>();
        for (const QPoint& p : {QPoint(0, 0), QPoint(pixels.width() - 1, 0),
                 QPoint(0, pixels.height() - 1), QPoint(pixels.width() - 1, pixels.height() - 1),
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
