// The stretch of the taskbar Win顺's search box goes in (taskbar::stretchFor,
// TaskbarRoom.h): by how Windows lays the icons out, and by the side the user
// chose (GitHub #271).

#include "platform/TaskbarRoom.h"

#include <QTest>

#include <array>

using namespace ws::taskbar;

namespace {
// A 2560 px taskbar, 72 high, as at 150%.
constexpr RECT kBar {0, 1368, 2560, 1440};
constexpr RECT kTray {2130, 1368, 2560, 1440}; // the tray and the clock
constexpr RECT kWidgets {0, 1368, 190, 1440}; // the widgets' button, at the left end with centred icons
constexpr RECT kCentredIcons {980, 1368, 1580, 1440};
constexpr RECT kLeftIcons {0, 1368, 600, 1440};

// Checked while compiling too: the choice is arithmetic alone.
static_assert(stretchFor(kBar, kCentredIcons, true, std::array {kTray}, Side::Auto).leftOfIcons);
static_assert(!stretchFor(kBar, kCentredIcons, true, std::array {kTray}, Side::Tray).leftOfIcons);
} // namespace

class TaskbarRoomTest : public QObject {
    Q_OBJECT

private slots:
    void stretches_data()
    {
        QTest::addColumn<bool>("icons");
        QTest::addColumn<bool>("centred");
        QTest::addColumn<bool>("widgets");
        QTest::addColumn<int>("side");
        QTest::addColumn<bool>("leftOfIcons");
        QTest::addColumn<double>("from");
        QTest::addColumn<double>("to");

        const int automatic = static_cast<int>(Side::Auto);
        const int tray = static_cast<int>(Side::Tray);
        // Windows 11's default: left of the centred icons, from the taskbar's end.
        QTest::newRow("centred, auto") << true << true << false << automatic << true << 0.0 << 980.0;
        // ... or from the widgets' button there.
        QTest::newRow("centred, auto, widgets") << true << true << true << automatic << true << 190.0 << 980.0;
        // #271: beside the tray although the icons are centred; what is at the left end does not count.
        QTest::newRow("centred, tray") << true << true << false << tray << false << 1580.0 << 2130.0;
        QTest::newRow("centred, tray, widgets") << true << true << true << tray << false << 1580.0 << 2130.0;
        // Icons at the left: right of them either way (there is no left of them).
        QTest::newRow("left, auto") << true << false << false << automatic << false << 600.0 << 2130.0;
        QTest::newRow("left, tray") << true << false << false << tray << false << 600.0 << 2130.0;
        // No icons read: the bar up to the tray, the box at its right end.
        QTest::newRow("no icons, auto") << false << true << false << automatic << false << 0.0 << 2130.0;
        QTest::newRow("no icons, tray") << false << false << false << tray << false << 0.0 << 2130.0;
    }

    void stretches()
    {
        QFETCH(bool, icons);
        QFETCH(bool, centred);
        QFETCH(bool, widgets);
        QFETCH(int, side);
        std::vector<RECT> obstacles {kTray};
        if (widgets)
            obstacles.push_back(kWidgets);
        const std::optional<RECT> read = icons ? std::optional<RECT>(centred ? kCentredIcons : kLeftIcons) : std::nullopt;
        const Stretch stretch = stretchFor(kBar, read, centred, obstacles, static_cast<Side>(side));
        QTEST(stretch.leftOfIcons, "leftOfIcons");
        QTEST(stretch.from, "from");
        QTEST(stretch.to, "to");
    }

    // Icons that fill the taskbar up to the tray leave nothing: the stretch
    // is empty, and the caller hides the box (no room) rather than cover them.
    void noRoom()
    {
        const RECT full {980, 1368, 2130, 1440};
        const Stretch stretch = stretchFor(kBar, full, true, std::array {kTray}, Side::Tray);
        QVERIFY(!stretch.leftOfIcons);
        QCOMPARE(stretch.to - stretch.from, 0.0);
    }
};

QTEST_APPLESS_MAIN(TaskbarRoomTest)
#include "tst_taskbarroom.moc"
