#include "platform/WindowEffects.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>
#include <QTest>
#include <QWindow>

#include <windows.h>

namespace {

// As the clipboard window: frameless, created long before its first show.
void prepare(QWindow& window)
{
    window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    window.setGeometry(12000, 12000, 320, 240);
    window.create();
}

// The application palette changes and the colour scheme does not: another
// accent colour, as when Windows' own colours come in after sign-in. Qt
// posts the change to every top-level window.
void changeAccent()
{
    QPalette palette = QGuiApplication::palette();
    const QColor one(0x12, 0x34, 0x56);
    palette.setColor(QPalette::Accent, palette.color(QPalette::Accent) == one ? QColor(0x65, 0x43, 0x21) : one);
    QGuiApplication::setPalette(palette);
    QCoreApplication::sendPostedEvents();
}

// As the clipboard opens over the program it pastes into.
void showWithoutFocus(QWindow& window)
{
    window.setProperty("_q_showWithoutActivating", true);
    window.show();
    window.setProperty("_q_showWithoutActivating", QVariant());
}

void setScheme(Qt::ColorScheme scheme)
{
    QGuiApplication::styleHints()->setColorScheme(scheme);
    QCoreApplication::sendPostedEvents();
}

} // namespace

class DarkFrameTest : public QObject {
    Q_OBJECT

private slots:
    void init() { setScheme(Qt::ColorScheme::Dark); }

    // GitHub #120's cause, Qt's own: a change of the palette alone sets a
    // frameless window's dark mode to light, while the scheme stays dark.
    // Failing here, Qt no longer does.
    void paletteChangeSetsLight()
    {
        QWindow window;
        prepare(window);
        ws::win::setDarkFrame(&window, true);
        QVERIFY(ws::win::isDarkFrame(&window));
        changeAccent();
        QCOMPARE(QGuiApplication::styleHints()->colorScheme(), Qt::ColorScheme::Dark);
        QVERIFY(!ws::win::isDarkFrame(&window));
    }

    // Set back as the window is shown, before the native window is: its
    // first frame is dark.
    void shownInTheScheme()
    {
        QWindow window;
        prepare(window);
        ws::win::keepDarkFrameOnShow(&window);
        ws::win::setDarkFrame(&window, true);
        changeAccent();
        QVERIFY(!ws::win::isDarkFrame(&window));
        const HWND hwnd = reinterpret_cast<HWND>(window.winId());
        int checked = 0;
        connect(&window, &QWindow::visibleChanged, &window, [&](bool visible) { // after keepDarkFrameOnShow's
            if (!visible)
                return;
            QVERIFY(!::IsWindowVisible(hwnd));
            QCOMPARE(ws::win::isDarkFrame(&window),
                QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
            ++checked;
        });
        showWithoutFocus(window);
        QCOMPARE(checked, 1);
        QVERIFY(ws::win::isDarkFrame(&window));
        window.hide();

        // Again, after another change while hidden.
        changeAccent();
        QVERIFY(!ws::win::isDarkFrame(&window));
        showWithoutFocus(window);
        QCOMPARE(checked, 2);
        window.hide();

        // Light the other way round, should it be left dark.
        setScheme(Qt::ColorScheme::Light);
        ws::win::setDarkFrame(&window, true);
        showWithoutFocus(window);
        QCOMPARE(checked, 3);
        QVERIFY(!ws::win::isDarkFrame(&window));
        window.hide();
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    DarkFrameTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_darkframe.moc"
