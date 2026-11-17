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

    // GitHub #361: a folder the user chose (the arrows, a click), or one
    // completing the path typed, is stepped into; one the list put under the
    // keyboard by itself keeps Tab for the scopes (GitHub #81).
    void chosenFoldersStepIn()
    {
        SearchResult folder {.path = u"D:\\Work\\合同"_s, .isDir = true};
        for (const Scope scope : {Scope::All, Scope::Files, Scope::Folders}) {
            QCOMPARE(tabAction(scope, false, false, &folder, false, true), TabAction::BrowseFolder);
            QCOMPARE(tabAction(scope, false, false, &folder, false, false), TabAction::SwitchScope);
        }
        // 内容 keeps Tab for its scopes.
        QCOMPARE(tabAction(Scope::Content, false, false, &folder, false, true), TabAction::SwitchScope);
        // Ahead of a keyword's hint row elsewhere in the list; that row itself still completes.
        QCOMPARE(tabAction(Scope::All, false, false, &folder, true, true), TabAction::BrowseFolder);
        QCOMPARE(tabAction(Scope::All, false, false, nullptr, true, true), TabAction::Complete);
        // A plugin's keyword: its completion, never a folder.
        QCOMPARE(tabAction(Scope::All, true, false, &folder, true, true), TabAction::Complete);
        QCOMPARE(tabAction(Scope::All, true, false, &folder, false, true), TabAction::None);
        // What cannot be browsed is not stepped into: a file, offline, missing, an app.
        const SearchResult file {.path = u"D:\\Work\\合同.docx"_s};
        QCOMPARE(tabAction(Scope::All, false, false, &file, false, true), TabAction::SwitchScope);
        folder.offline = true;
        QCOMPARE(tabAction(Scope::All, false, false, &folder, false, true), TabAction::SwitchScope);
        folder.offline = false;
        folder.missing = true;
        QCOMPARE(tabAction(Scope::All, false, false, &folder, false, true), TabAction::SwitchScope);
        folder.missing = false;
        folder.app = AppKind::Desktop;
        QCOMPARE(tabAction(Scope::All, false, false, &folder, false, true), TabAction::SwitchScope);
    }

    void pathCompletion()
    {
        using pathtext::splitTyped;
        // A folder right in the folder typed, while a part of a name is typed
        // (whatever the list matched it by: pinyin, a part of the name).
        QVERIFY(completesPath(splitTyped(u"D:\\Wo"_s), u"D:\\Work"_s));
        QVERIFY(completesPath(splitTyped(u"D:\\Work\\合"_s), u"D:\\Work\\合同"_s));
        QVERIFY(completesPath(splitTyped(u"d:/work/ht"_s), u"D:\\Work\\合同"_s));
        QVERIFY(completesPath(splitTyped(u"C:\\Program F"_s), u"C:\\Program Files"_s));
        QVERIFY(completesPath(splitTyped(u"\\\\server\\share\\do"_s), u"\\\\server\\share\\docs"_s));
        // Not the folder's own listing, nor words after a space, nor deeper or elsewhere.
        QVERIFY(!completesPath(splitTyped(u"D:\\Work\\"_s), u"D:\\Work\\合同"_s));
        QVERIFY(!completesPath(splitTyped(u"D:\\Work\\ 合同"_s), u"D:\\Work\\合同"_s));
        QVERIFY(!completesPath(splitTyped(u"D:\\Work\\合"_s), u"D:\\Work\\合同\\2026"_s));
        QVERIFY(!completesPath(splitTyped(u"D:\\Work\\合"_s), u"D:\\Other\\合同"_s));
        QVERIFY(!completesPath(splitTyped(u"D:\\Work\\合"_s), u"D:\\Work"_s));
        // Words that are no path.
        QVERIFY(!completesPath(splitTyped(u"合同"_s), u"D:\\Work\\合同"_s));
        QVERIFY(!completesPath(splitTyped(u"tools/sc"_s), u"D:\\tools\\screenshots"_s));
    }

    void stepsUpALevel()
    {
        const auto up = [](const QString& text) { return upFrom(pathtext::splitTyped(text)); };
        // A name being typed goes first (as DialogBar::up), nothing to choose.
        std::optional<Up> u = up(u"D:\\Work\\合"_s);
        QVERIFY(u);
        QCOMPARE(u->query, u"D:\\Work\\"_s);
        QVERIFY(u->row.isEmpty());
        u = up(u"D:\\Work"_s);
        QVERIFY(u);
        QCOMPARE(u->query, u"D:\\"_s);
        QVERIFY(u->row.isEmpty());
        // Then the folder above, the one left chosen there.
        u = up(u"D:\\Work\\合同\\"_s);
        QVERIFY(u);
        QCOMPARE(u->query, u"D:\\Work\\"_s);
        QCOMPARE(u->row, u"D:\\Work\\合同"_s);
        u = up(u"D:\\Work\\"_s);
        QVERIFY(u);
        QCOMPARE(u->query, u"D:\\"_s);
        QCOMPARE(u->row, u"D:\\Work"_s);
        u = up(u"d:/work/合同/"_s); // slashes, as typed
        QVERIFY(u);
        QCOMPARE(u->query, u"d:\\work\\"_s);
        QCOMPARE(u->row, u"d:\\work\\合同"_s);
        u = up(u"\"D:\\Work\\合同\\\""_s); // in quotes, as Explorer copies a path
        QVERIFY(u);
        QCOMPARE(u->query, u"D:\\Work\\"_s);
        QCOMPARE(u->row, u"D:\\Work\\合同"_s);
        u = up(u"\\\\server\\share\\dir\\"_s);
        QVERIFY(u);
        QCOMPARE(u->query, u"\\\\server\\share\\"_s);
        QCOMPARE(u->row, u"\\\\server\\share\\dir"_s);
        // Nothing above a drive's or a share's root, and none in what is no
        // path: the key edits the text as ever.
        QVERIFY(!up(u"D:\\"_s));
        QVERIFY(!up(u"D:"_s));
        QVERIFY(!up(u"\\\\server\\share\\"_s));
        QVERIFY(!up(u"\\\\server\\share"_s));
        QVERIFY(!up(u"合同"_s));
        QVERIFY(!up(u"tools/out/"_s));
        QVERIFY(!up(QString()));
    }

    void folderPaths()
    {
        QCOMPARE(browseQuery(u"D:\\Work"_s), u"D:\\Work\\"_s);
        QCOMPARE(browseQuery(u"D:\\"_s), u"D:\\"_s);
        QCOMPARE(browseQuery(u"\\\\server\\share"_s), u"\\\\server\\share\\"_s);
        QVERIFY(within(u"D:\\Work"_s, u"D:\\Work"_s));
        QVERIFY(within(u"D:\\Work"_s, u"d:\\work\\合同"_s));
        QVERIFY(within(u"D:\\Work\\"_s, u"D:\\Work\\合同\\2026"_s));
        QVERIFY(within(u"D:\\"_s, u"D:\\Work"_s));
        QVERIFY(within(u"\\\\server\\share"_s, u"\\\\server\\share\\dir"_s));
        QVERIFY(!within(u"D:\\Work"_s, u"D:\\Workshop"_s));
        QVERIFY(!within(u"D:\\Work\\合同"_s, u"D:\\Work"_s));
        QVERIFY(!within(u"D:\\Work"_s, u"E:\\Work"_s));
        QVERIFY(!within(QString(), u"D:\\Work"_s));
        QVERIFY(!within(u"D:\\Work"_s, u"合同"_s));
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
