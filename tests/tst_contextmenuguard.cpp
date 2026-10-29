#include "ContextMenuGuard.h"

#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTest>

namespace {

// Counts the context-menu events that get past the application's filters to the window.
class Arrivals : public QObject {
public:
    int count = 0;

protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::ContextMenu)
            ++count;
        return false;
    }
};

// A right-click at `pos` (the window's own logical coordinates), as Windows
// delivers one: Qt makes the context-menu event from the release. Sent
// straight to the window: QTest's helpers take a null point for "the middle".
void rightClick(QQuickWindow& window, QPointF pos)
{
    const QPointF global = window.mapToGlobal(pos);
    QMouseEvent press(QEvent::MouseButtonPress, pos, pos, global, Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, pos, pos, global, Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &release);
}

// A window that never takes the focus, as the menus are; it is not shown:
// the events are sent to it directly.
void prepare(QQuickWindow& window, Arrivals& arrivals)
{
    window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    window.setGeometry(100, 100, 240, 160);
    window.installEventFilter(&arrivals);
}

} // namespace

class ContextMenuGuardTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Windows' trigger, whatever the platform plugin says.
        QGuiApplication::styleHints()->setContextMenuTrigger(Qt::ContextMenuTrigger::Release);
        // WINSHUN_TEST_UNGUARDED=1 leaves the guard out: the test then dies as
        // long as Qt has the bug. After a Qt upgrade: can the guard go?
        if (!qEnvironmentVariableIsSet("WINSHUN_TEST_UNGUARDED"))
            qApp->installEventFilter(&m_guard);
    }

    // QTBUG-146671: without the guard Qt dereferences the missing active
    // focus item here and the test process dies.
    void topLeftPixelWithoutFocusItem()
    {
        QQuickWindow window;
        Arrivals arrivals;
        prepare(window, arrivals);
        QVERIFY(!window.activeFocusItem());
        rightClick(window, QPointF(0, 0));
        rightClick(window, QPointF(0.3, 0.4)); // rounds to (0, 0): at 150 % the device pixel (0, 0) only
        QCOMPARE(arrivals.count, 0);
    }

    // The keyboard's kind (the Menu key: no position on Windows) is dropped
    // there too: Qt would crash on it the same way.
    void keyboardEventWithoutFocusItem()
    {
        QQuickWindow window;
        Arrivals arrivals;
        prepare(window, arrivals);
        QContextMenuEvent event(QContextMenuEvent::Keyboard, QPoint(), QPoint());
        QCoreApplication::sendEvent(&window, &event);
        QCOMPARE(arrivals.count, 0);
    }

    // Anywhere else Qt delivers the event as usual.
    void otherPixelsStillArrive()
    {
        QQuickWindow window;
        Arrivals arrivals;
        prepare(window, arrivals);
        rightClick(window, QPointF(1, 1));
        rightClick(window, QPointF(0, 30));
        rightClick(window, QPointF(239, 159));
        QCOMPARE(arrivals.count, 3);
    }

    // With an active focus item Qt does not crash, so nothing is dropped.
    void windowWithFocusItemStillGetsThem()
    {
        QQuickWindow window;
        Arrivals arrivals;
        prepare(window, arrivals);
        QQuickItem field(window.contentItem());
        field.setSize(QSizeF(100, 30));
        field.setPosition(QPointF(40, 40));
        field.forceActiveFocus(); // as when its window has the keyboard
        QCOMPARE(window.activeFocusItem(), &field);
        rightClick(window, QPointF(0, 0));
        QContextMenuEvent key(QContextMenuEvent::Keyboard, QPoint(), QPoint());
        QCoreApplication::sendEvent(&window, &key);
        QCOMPARE(arrivals.count, 2);
    }

    // Context-menu events for items (Qt delivers them one by one) are not the guard's.
    void itemsAreNotFiltered()
    {
        QQuickWindow window;
        Arrivals arrivals;
        prepare(window, arrivals);
        QQuickItem item(window.contentItem());
        item.installEventFilter(&arrivals);
        QContextMenuEvent event(QContextMenuEvent::Mouse, QPoint(), QPoint());
        QCoreApplication::sendEvent(&item, &event);
        QCOMPARE(arrivals.count, 1);
    }

private:
    ws::ContextMenuGuard m_guard;
};

QTEST_MAIN(ContextMenuGuardTest)
#include "tst_contextmenuguard.moc"
