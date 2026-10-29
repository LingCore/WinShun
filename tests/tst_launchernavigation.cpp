#include "LauncherNavigation.h"

#include <QtTest>

using namespace ws;
using namespace ws::navigation;
using namespace Qt::StringLiterals;

class LauncherNavigationTest : public QObject {
    Q_OBJECT

private slots:
    void favoritesKeepScopeByDefault()
    {
        SearchResult folder {.name = u"Work"_s, .path = u"C:\\Work"_s, .isDir = true, .favorite = true};
        for (const Scope scope : {Scope::All, Scope::Files, Scope::Folders, Scope::Content}) {
            QCOMPARE(tabAction(scope, false, false, &folder, false), TabAction::SwitchScope);
            QCOMPARE(tabAction(scope, false, false, nullptr, false), TabAction::SwitchScope);
            folder.favorite = false;
            QCOMPARE(tabAction(scope, false, false, &folder, false), TabAction::SwitchScope);
            folder.favorite = true;
        }
    }

    void favoriteBrowsingIsExplicitAndScoped()
    {
        SearchResult folder {.path = u"C:\\Work"_s, .isDir = true, .favorite = true};
        QCOMPARE(tabAction(Scope::All, false, true, &folder, false), TabAction::BrowseFolder);
        // Turning the setting off applies to the same selected row at once.
        QCOMPARE(tabAction(Scope::All, false, false, &folder, false), TabAction::SwitchScope);
        for (const Scope scope : {Scope::Files, Scope::Folders, Scope::Content})
            QCOMPARE(tabAction(scope, false, true, &folder, false), TabAction::SwitchScope);
        folder.favorite = false;
        QVERIFY(canBrowseFolder(folder)); // an ordinary folder still has the menu action
        QCOMPARE(tabAction(Scope::All, false, true, &folder, false), TabAction::SwitchScope);
        folder.favorite = true;
        folder.isDir = false;
        QCOMPARE(tabAction(Scope::All, false, true, &folder, false), TabAction::SwitchScope);
        QVERIFY(!canBrowseFolder(folder));
        QCOMPARE(tabAction(Scope::All, false, true, nullptr, false), TabAction::SwitchScope);
    }

    void unavailableFoldersAreNotEntered()
    {
        SearchResult folder {.path = u"\\\\server\\share"_s, .isDir = true, .favorite = true};
        folder.offline = true;
        QVERIFY(!canBrowseFolder(folder));
        QCOMPARE(tabAction(Scope::All, false, true, &folder, false), TabAction::SwitchScope);
        folder.offline = false;
        folder.missing = true;
        QVERIFY(!canBrowseFolder(folder));
        QCOMPARE(tabAction(Scope::All, false, true, &folder, false), TabAction::SwitchScope);
        folder.missing = false;
        folder.app = AppKind::Desktop;
        QVERIFY(!canBrowseFolder(folder));
        QCOMPARE(tabAction(Scope::All, false, true, &folder, false), TabAction::SwitchScope);
    }

    void completionKeepsItsOwnBehavior()
    {
        const SearchResult folder {.path = u"C:\\Work"_s, .isDir = true, .favorite = true};
        for (const bool enterFolder : {false, true}) {
            for (const Scope scope : {Scope::All, Scope::Files, Scope::Folders, Scope::Content}) {
                QCOMPARE(tabAction(scope, true, enterFolder, &folder, true), TabAction::Complete);
                QCOMPARE(tabAction(scope, true, enterFolder, &folder, false), TabAction::None);
            }
            // A web suggestion or keyword hint outside keyword mode still completes.
            QCOMPARE(tabAction(Scope::All, false, enterFolder, nullptr, true), TabAction::Complete);
        }
        QCOMPARE(tabAction(Scope::All, false, false, &folder, true), TabAction::Complete);
        QCOMPARE(tabAction(Scope::All, false, true, &folder, true), TabAction::BrowseFolder);
    }

    // GitHub #113: a list changing by itself (传's request coming in on top)
    // never puts another row under the keyboard's Enter, the first row
    // included; only right after the list opened from that plugin's
    // notification does the first row stay first.
    void keyboardFollowsItsRow()
    {
        const QStringList before {u"recv"_s, u"dev-a"_s, u"dev-b"_s};
        const QStringList request {u"req"_s, u"recv"_s, u"dev-a"_s, u"dev-b"_s};
        // On the first row, a request on top: the keyboard goes down with its row.
        RowFollow f = followRow(before, request, 0, u"recv"_s, false, false);
        QCOMPARE(f.kind, RowFollow::Kind::Move);
        QCOMPARE(f.row, 1);
        // On another row too.
        f = followRow(before, request, 2, u"dev-b"_s, false, false);
        QCOMPARE(f.kind, RowFollow::Kind::Move);
        QCOMPARE(f.row, 3);
        // Opened from the notification: the first row stays first, Enter waits.
        QCOMPARE(followRow(before, request, 0, u"recv"_s, false, true).kind, RowFollow::Kind::Wait);
        // ... but only the first row: elsewhere the keyboard still follows its row.
        QCOMPARE(followRow(before, request, 1, u"dev-a"_s, false, true).kind, RowFollow::Kind::Move);
        // Nothing changed, or its row did not move: it stays.
        QCOMPARE(followRow(before, before, 1, u"dev-a"_s, false, false).kind, RowFollow::Kind::Stay);
        QCOMPARE(followRow(before, QStringList {u"recv"_s, u"dev-a"_s, u"dev-c"_s}, 1, u"dev-a"_s, false, false).kind,
            RowFollow::Kind::Stay);
        // Its row went: it stays put, Enter waits.
        QCOMPARE(followRow(before, QStringList {u"recv"_s, u"dev-b"_s}, 1, u"dev-a"_s, false, false).kind, RowFollow::Kind::Wait);
        // What stood in for a row that went a moment ago was never chosen: it waits.
        QCOMPARE(followRow(before, request, 1, u"dev-a"_s, true, false).kind, RowFollow::Kind::Wait);
        // No row, or no keyboard: nothing.
        QCOMPARE(followRow(before, request, -1, u"recv"_s, false, false).kind, RowFollow::Kind::Stay);
        QCOMPARE(followRow(before, request, 0, QString(), false, false).kind, RowFollow::Kind::Stay);
    }
};

QTEST_GUILESS_MAIN(LauncherNavigationTest)
#include "tst_launchernavigation.moc"
