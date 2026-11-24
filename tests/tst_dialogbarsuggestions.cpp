#include "DialogBarSuggestions.h"

#include <QFileInfo>
#include <QtTest>

using namespace ws;
using namespace ws::dialogbar;
using namespace Qt::StringLiterals;

namespace {

QString leafName(const QString& path)
{
    const QFileInfo info(path);
    const QString name = info.fileName();
    return name.isEmpty() ? path : name;
}

SuggestionInputs sampleInputs()
{
    SuggestionInputs in;
    in.explorer = {u"D:\\Work\\Alpha"_s, u"D:\\Work\\Beta"_s, u"D:\\Work\\Gamma"_s};
    in.explorerTags = {u"资源管理器"_s, u"Directory Opus"_s, u"Total Commander"_s};
    in.clipboard = u"D:\\Inbox\\spec.pdf"_s;
    in.clipboardIsDir = false;
    in.clipboardTag = u"剪贴板"_s;

    SearchResult favDir;
    favDir.path = u"D:\\Work\\Beta"_s; // duplicate of explorer (case-insensitive)
    favDir.name = u"Beta Fav"_s;
    favDir.isDir = true;
    favDir.favorite = true;

    SearchResult favDir2;
    favDir2.path = u"E:\\Archive"_s;
    favDir2.name = u"归档"_s;
    favDir2.isDir = true;
    favDir2.favorite = true;

    SearchResult favFile;
    favFile.path = u"E:\\Notes\\todo.md"_s;
    favFile.name = u"Todo"_s;
    favFile.isDir = false;
    favFile.favorite = true;

    in.favorites = {favDir, favDir2, favFile};
    in.favoriteTags = {u"收藏"_s, u"收藏 · arc"_s, u"收藏"_s};
    in.recent = {u"d:\\work\\alpha"_s, u"C:\\Temp\\RecentOne"_s};
    in.recentTag = u"最近"_s;
    in.maxExplorer = 2;
    return in;
}

} // namespace

class DialogBarSuggestionsTest : public QObject {
    Q_OBJECT

private slots:
    void allSourcesOrderCapAndDedup()
    {
        const SuggestionInputs in = sampleInputs();
        const SuggestionSources all;
        QVERIFY(all.any());

        const SuggestionList list = buildSuggestions(in, all, leafName);
        QCOMPARE(list.rows.size(), 6);
        QCOMPARE(list.tags.size(), 6);

        // Open file managers capped at maxExplorer=2.
        QCOMPARE(list.rows[0].path, u"D:\\Work\\Alpha"_s);
        QCOMPARE(list.rows[0].name, u"Alpha"_s);
        QCOMPARE(list.tags[0], u"资源管理器"_s);
        QCOMPARE(list.rows[1].path, u"D:\\Work\\Beta"_s);
        QCOMPARE(list.tags[1], u"Directory Opus"_s);

        // Clipboard item next.
        QCOMPARE(list.rows[2].path, u"D:\\Inbox\\spec.pdf"_s);
        QVERIFY(!list.rows[2].isDir);
        QCOMPARE(list.tags[2], u"剪贴板"_s);

        // Favorites: duplicate D:\Work\Beta skipped, Archive and todo.md kept with custom names.
        QCOMPARE(list.rows[3].path, u"E:\\Archive"_s);
        QCOMPARE(list.rows[3].name, u"归档"_s);
        QCOMPARE(list.tags[3], u"收藏 · arc"_s);
        QCOMPARE(list.rows[4].path, u"E:\\Notes\\todo.md"_s);
        QCOMPARE(list.rows[4].name, u"Todo"_s);
        QVERIFY(!list.rows[4].isDir);

        // Recent: duplicate d:\work\alpha skipped case-insensitively, RecentOne kept.
        QCOMPARE(list.rows[5].path, u"C:\\Temp\\RecentOne"_s);
        QVERIFY(list.rows[5].recent);
        QCOMPARE(list.tags[5], u"最近"_s);
    }

    // GitHub #438: choosing only open folders + favorites omits recent and clipboard completely.
    void selectiveSourcesPrivacyFiltering()
    {
        const SuggestionInputs in = sampleInputs();
        const SuggestionSources openAndFav {.open = true, .clipboard = false, .favorites = true, .recent = false};
        const SuggestionList list = buildSuggestions(in, openAndFav, leafName);
        QCOMPARE(list.rows.size(), 4);
        QCOMPARE(list.rows[0].path, u"D:\\Work\\Alpha"_s);
        QCOMPARE(list.rows[1].path, u"D:\\Work\\Beta"_s);
        QCOMPARE(list.rows[2].path, u"E:\\Archive"_s);
        QCOMPARE(list.rows[3].path, u"E:\\Notes\\todo.md"_s);

        // Only favorites enabled: Beta is no longer deduplicated against disabled explorer.
        const SuggestionSources favOnly {.open = false, .clipboard = false, .favorites = true, .recent = false};
        const SuggestionList favList = buildSuggestions(in, favOnly, leafName);
        QCOMPARE(favList.rows.size(), 3);
        QCOMPARE(favList.rows[0].path, u"D:\\Work\\Beta"_s);
        QCOMPARE(favList.rows[0].name, u"Beta Fav"_s);

        // Folder picker omits favorite files even when favorites are enabled.
        SuggestionInputs folderPicker = in;
        folderPicker.foldersOnly = true;
        const SuggestionList folderList = buildSuggestions(folderPicker, favOnly, leafName);
        QCOMPARE(folderList.rows.size(), 2);
        QCOMPARE(folderList.rows[0].path, u"D:\\Work\\Beta"_s);
        QCOMPARE(folderList.rows[1].path, u"E:\\Archive"_s);
    }

    // All four unchecked: neither focus nor Down key shows suggestions.
    void allDisabledBlocksFocusAndDownKey()
    {
        const SuggestionInputs in = sampleInputs();
        const SuggestionSources none {.open = false, .clipboard = false, .favorites = false, .recent = false};
        QVERIFY(!none.any());
        QVERIFY(!shouldSuggest(none, true, false));
        QVERIFY(!shouldSuggest(none, false, true));
        QVERIFY(!canAskSuggestions(none, false, false, u""_s));
        QVERIFY(buildSuggestions(in, none, leafName).rows.isEmpty());

        // With at least one source enabled, Down works when focus auto-list is off and query is empty.
        const SuggestionSources one {.open = false, .clipboard = false, .favorites = true, .recent = false};
        QVERIFY(shouldSuggest(one, true, false));
        QVERIFY(!shouldSuggest(one, false, false));
        QVERIFY(canAskSuggestions(one, false, false, u""_s));
        QVERIFY(!canAskSuggestions(one, false, false, u"abc"_s));
        QVERIFY(!canAskSuggestions(one, true, false, u""_s));
        QVERIFY(shouldSuggest(one, false, true));
        QVERIFY(!canAskSuggestions(one, false, true, u""_s));
    }
};

QTEST_GUILESS_MAIN(DialogBarSuggestionsTest)
#include "tst_dialogbarsuggestions.moc"
