#include "Favorites.h"

#include "History.h"
#include "IndexService.h"
#include "SearchEngine.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>
#include <shellapi.h>

using namespace ws;
using namespace Qt::StringLiterals;

namespace {

Favorite folder(const QString& path, const QString& name = {}, const QString& keyword = {})
{
    return {Favorite::Kind::Folder, path, name, keyword};
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeAll(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

} // namespace

// The favorites (src/core/Favorites): their file, the store that keeps it,
// and what the search engine lists of them with nothing typed.
class TestFavorites : public QObject {
    Q_OBJECT

private slots:
    void commandRoundTripAndSearch()
    {
        const Favorite command {Favorite::Kind::Command, u"winshun-command:test"_s, u"打开编辑器"_s, u"code"_s,
            u"\"C:\\Tools With Space\\Code.exe\" --new-window {query}"_s, u"D:\\工作"_s};
        const FavoriteList favorites {command, folder(u"D:\\Reports"_s, u"报告"_s, u"bg"_s)};
        const QByteArray written = writeFavoritesFile(favorites);
        QVERIFY(written.contains("\"version\": 2"));
        const auto loaded = readFavoritesFile(written);
        QVERIFY(loaded);
        QCOMPARE(loaded->favorites, favorites);
        QCOMPARE(typedCommandKeyword(favorites, u" CODE D:\\新 项目 "), 0);
        QCOMPARE(typedCommandKeyword(favorites, u"codex a"), -1);
        QCOMPARE(typedCommandKeyword(favorites, u"bg a"), -1);
        QCOMPARE(commandWords(command, u" CODE D:\\新 项目 "), u"D:\\新 项目"_s);
        QTemporaryDir temporary;
        IndexService service(temporary.path(), {});
        SearchEngine engine(&service);
        QSignalSpy spy(&engine, &SearchEngine::resultsReady);
        SearchEngine::Request request;
        request.favorites = favorites;
        request.text = u"code D:\\新 项目"_s;
        engine.submit(request);
        QVERIFY(spy.wait(5000));
        const SearchResults results = spy.at(0).at(1).value<SearchResults>();
        QVERIFY(!results.isEmpty());
        QVERIFY(results.first().isCommand());
        QVERIFY(results.first().favorite && results.first().exact && !results.first().missing);
        QCOMPARE(results.first().target, command.command);
        QCOMPARE(results.first().words, u"D:\\新 项目"_s);
        QCOMPARE(results.first().workingDirectory, command.workingDirectory);
        QVERIFY(!scopeShows(Scope::Files, command.kind));
        QVERIFY(!scopeShows(Scope::Folders, command.kind));
        QCOMPARE(favoriteOf(results.first())->command, command.command);
    }

    void commandArgumentRoundTrip()
    {
        for (const QString& words : {u""_s, u"中文 with spaces"_s, u"C:\\folder\\"_s,
                 u"say \"hello\" \\"_s, u"a\\\"b\\\\\"c"_s, u"{query} %TEMP% & more"_s}) {
            for (const QString& pattern : {u"{query}"_s, u"\"{query}\""_s}) {
                const QString line = u"probe.exe --before "_s + commandArguments(pattern, words) + u" --after"_s;
                int count = 0;
                wchar_t** args = ::CommandLineToArgvW(line.toStdWString().c_str(), &count);
                QVERIFY(args);
                QStringList actual;
                for (int i = 0; i < count; ++i)
                    actual.append(QString::fromWCharArray(args[i]));
                ::LocalFree(args);
                QCOMPARE(actual, (QStringList {u"probe.exe"_s, u"--before"_s, words, u"--after"_s}));
            }
        }
    }
    void fileRoundTrip()
    {
        const FavoriteList favorites {
            folder(u"D:\\工作文档"_s, u"工作"_s, u"gz"_s),
            {Favorite::Kind::File, u"C:\\Users\\me\\notes.txt"_s, u"notes.txt"_s, {}},
            {Favorite::Kind::App, u"shell:AppsFolder\\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"_s, u"计算器"_s, u"js"_s},
            {Favorite::Kind::Place, u"winshun-place:tool:taskmgr"_s, u"任务管理器"_s, {}},
            folder(u"C:\\"_s, u"C:\\"_s),
        };
        bool newer = true;
        const auto read = readFavoritesFile(writeFavoritesFile(favorites), &newer);
        QVERIFY(read);
        QVERIFY(!newer);
        QCOMPARE(read->skipped, 0);
        QCOMPARE(read->favorites, favorites);
    }

    // What a file may hold that is no favorite, or not quite one.
    void fileCleansUp()
    {
        // ' for " and / for \ (a JSON "\\"), to read.
        const QByteArray json = QByteArray("{'format': 'winshun-favorites', 'version': 1, 'favorites': ["
                                           "{'kind': 'folder', 'path': 'D:/工作文档/', 'name': '', 'keyword': 'gz'},"
                                           "{'kind': 'folder', 'path': 'd:||工作文档'},"
                                           "{'kind': 'file', 'path': ''},"
                                           "{'kind': 'shortcut', 'path': 'D:||x.lnk'},"
                                           "{'kind': 'file', 'path': 'shell:AppsFolder||x!App'},"
                                           "{'kind': 'app', 'path': 'D:||x.exe'},"
                                           "{'kind': 'file', 'path': 'D:||a.txt', 'keyword': 'GZ'},"
                                           "{'kind': 'file', 'path': 'D:||b.txt', 'keyword': 'a b'},"
                                           "{'kind': 'folder', 'path': 'E:||', 'keyword': 'e:'}"
                                           "]}")
                                    .replace('\'', '"')
                                    .replace('|', '\\');
        const auto read = readFavoritesFile(json);
        QVERIFY(read);
        QCOMPARE(read->skipped, 5); // twice, no path, no such kind, an app's path as a file, a file as an app
        QCOMPARE(read->favorites.size(), 4);
        QCOMPARE(read->favorites[0], folder(u"D:\\工作文档"_s, u"工作文档"_s, u"gz"_s));
        QCOMPARE(read->favorites[1].keyword, QString()); // gz is taken, whatever its case
        QCOMPARE(read->favorites[2].keyword, QString()); // not one word
        QCOMPARE(read->favorites[3].path, u"E:\\"_s); // a drive keeps its backslash
        QCOMPARE(read->favorites[3].keyword, QString()); // like a path
    }

    void fileOfOtherKinds()
    {
        bool newer = false;
        QVERIFY(!readFavoritesFile("not json", &newer));
        QVERIFY(!newer);
        QVERIFY(!readFavoritesFile("{\"format\": \"something\", \"version\": 1, \"favorites\": []}", &newer));
        QVERIFY(!newer);
        QVERIFY(!readFavoritesFile("{\"format\": \"winshun-favorites\", \"version\": 3, \"favorites\": []}", &newer));
        QVERIFY(newer);
    }

    void keywords()
    {
        const FavoriteList favorites {folder(u"D:\\a"_s, {}, u"gz"_s), folder(u"D:\\b"_s)};
        QCOMPARE(typedFavoriteKeyword(favorites, u"gz"), 0);
        QCOMPARE(typedFavoriteKeyword(favorites, u" GZ "), 0);
        QCOMPARE(typedFavoriteKeyword(favorites, u"gzx"), -1);
        QCOMPARE(typedFavoriteKeyword(favorites, u"gz 报告"), -1);
        QCOMPARE(typedFavoriteKeyword(favorites, u""), -1);
        QCOMPARE(typedFavoriteKeyword(favorites, u"  "), -1);

        QVERIFY(isValidFavoriteKeyword(u"gz"));
        QVERIFY(isValidFavoriteKeyword(u"工作"));
        QVERIFY(!isValidFavoriteKeyword(u""));
        QVERIFY(!isValidFavoriteKeyword(u"a b"));
        QVERIFY(!isValidFavoriteKeyword(u"a/b"));
        QVERIFY(!isValidFavoriteKeyword(u"a\\b"));
        QVERIFY(!isValidFavoriteKeyword(u"c:"));
    }

    void favoriteOfRows()
    {
        SearchResult dir {.name = u"工作文档"_s, .path = u"D:\\工作文档"_s, .isDir = true};
        QVERIFY(favoriteOf(dir));
        QCOMPARE(*favoriteOf(dir), folder(u"D:\\工作文档"_s, u"工作文档"_s));

        // A content match: its file, by the file's own name.
        SearchResult line {.name = u"明天要做的事"_s, .path = u"D:/notes/todo.txt"_s};
        const auto file = favoriteOf(line);
        QVERIFY(file);
        QCOMPARE(file->kind, Favorite::Kind::File);
        QCOMPARE(file->path, u"D:\\notes\\todo.txt"_s);
        QCOMPARE(file->name, u"todo.txt"_s);

        SearchResult app {.name = u"X"_s, .path = u"shell:AppsFolder\\x!App"_s, .app = AppKind::Store};
        QCOMPARE(favoriteOf(app)->kind, Favorite::Kind::App);
        SearchResult place {.name = u"任务管理器"_s, .path = u"winshun-place:tool:taskmgr"_s, .app = AppKind::Tool};
        QCOMPARE(favoriteOf(place)->kind, Favorite::Kind::Place);

        QVERIFY(!favoriteOf(SearchResult {.path = u"winshun-web:gh"_s, .app = AppKind::Web}));
        QVERIFY(!favoriteOf(SearchResult {.path = u"winshun-plugin:x"_s, .app = AppKind::Plugin}));
        QVERIFY(!favoriteOf(SearchResult {}));
    }

    void scopes()
    {
        using K = Favorite::Kind;
        for (const K kind : {K::Folder, K::File, K::App, K::Place}) {
            QVERIFY(scopeShows(Scope::All, kind));
            QVERIFY(!scopeShows(Scope::Content, kind));
        }
        QVERIFY(scopeShows(Scope::Files, K::File));
        QVERIFY(scopeShows(Scope::Files, K::Folder));
        QVERIFY(!scopeShows(Scope::Files, K::App));
        QVERIFY(scopeShows(Scope::Folders, K::Folder));
        QVERIFY(!scopeShows(Scope::Folders, K::File));
        QVERIFY(!scopeShows(Scope::Folders, K::Place));
    }

    void storeChangesAndKeepsThem()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(u"data/favorites.json"_s); // the folder made when first saved
        Favorites store(path);
        QSignalSpy changed(&store, &Favorites::changed);
        store.load();
        QVERIFY(store.items().isEmpty());

        QVERIFY(store.add(folder(u"D:\\a"_s, {}, u"aa"_s)));
        QCOMPARE(store.items().constFirst().name, u"a"_s); // its own name
        QVERIFY(!store.add(folder(u"d:\\A"_s))); // there already
        QVERIFY(!store.add(folder({})));
        QVERIFY(store.add(folder(u"D:\\b"_s, u"乙"_s, u"AA"_s))); // the keyword taken: dropped
        QCOMPARE(store.items()[1].keyword, QString());
        QVERIFY(store.add(folder(u"D:\\c"_s, {}, u"a b"_s), 0)); // first; a keyword no one can type: dropped
        QCOMPARE(store.items()[0].path, u"D:\\c"_s);
        QCOMPARE(store.items()[0].keyword, QString());

        QVERIFY(store.move(0, 2));
        QVERIFY(!store.move(0, 0));
        QVERIFY(!store.move(0, 3));
        QCOMPARE(store.items()[2].path, u"D:\\c"_s);

        Favorite renamed = store.items()[1];
        renamed.name = u"乙方"_s;
        renamed.keyword = u"yf"_s;
        QVERIFY(store.update(1, renamed));
        QVERIFY(!store.update(1, renamed)); // nothing changed
        renamed.path = u"D:\\A"_s;
        QVERIFY(!store.update(1, renamed)); // another one's path

        QCOMPARE(store.addAll({folder(u"D:\\a"_s), folder(u"D:\\d"_s, {}, u"yf"_s), folder(u"D:\\e"_s, u"e"_s)}), 2);
        QCOMPARE(store.items()[3].keyword, QString()); // yf is 乙方's
        QVERIFY(store.remove(u"d:\\E"));
        QVERIFY(!store.remove(u"D:\\e"));
        QCOMPARE(store.removeAll({u"D:\\nothing"_s}), 0);

        const FavoriteList kept = store.items();
        QCOMPARE(kept.size(), 4);
        Favorites again(path);
        again.load();
        QCOMPARE(again.items(), kept);
        QCOMPARE(changed.count(), 8); // load, 3 adds, move, update, addAll, remove
    }

    // 导入: the new ones added, those there already given what they lack.
    void importCompletes()
    {
        QTemporaryDir dir;
        Favorites store(dir.filePath(u"favorites.json"_s));
        store.load();
        QVERIFY(store.add(folder(u"D:\\a"_s)));
        QVERIFY(store.add(folder(u"D:\\b"_s, u"乙"_s, u"yi"_s)));
        QVERIFY(store.add(folder(u"D:\\c"_s, {}, u"cc"_s)));
        const Favorites::Merged merged = store.merge({
            folder(u"d:\\A"_s, u"甲"_s, u"gz"_s), // its own name here and no keyword: both taken
            folder(u"D:\\b"_s, u"别的名字"_s, u"bb"_s), // renamed here, with a keyword: kept as it is
            folder(u"D:\\c"_s, u"丙"_s, u"cc"_s), // its own name: renamed
            folder(u"D:\\d"_s, {}, u"yi"_s), // new; yi is 乙's
        });
        QCOMPARE(merged.added, 1);
        QCOMPARE(merged.completed, 2);
        QCOMPARE(store.items()[0], folder(u"D:\\a"_s, u"甲"_s, u"gz"_s));
        QCOMPARE(store.items()[1], folder(u"D:\\b"_s, u"乙"_s, u"yi"_s));
        QCOMPARE(store.items()[2], folder(u"D:\\c"_s, u"丙"_s, u"cc"_s));
        QCOMPARE(store.items()[3], folder(u"D:\\d"_s, u"d"_s));
        Favorites again(dir.filePath(u"favorites.json"_s));
        again.load();
        QCOMPARE(again.items(), store.items());
    }

    // The dialog box's pinned folders, once: taken over and the old file set aside.
    void pinsTakenOver()
    {
        QTemporaryDir dir;
        const QString pinsFile = dir.filePath(u"dialog-pins.txt"_s);
        {
            History pins(pinsFile, 1000);
            pins.record(u"D:\\older"_s, 1);
            pins.record(u"D:\\newer"_s, 2);
        }
        const QString path = dir.filePath(u"favorites.json"_s);
        Favorites store(path, pinsFile);
        store.load();
        QCOMPARE(store.items(), (FavoriteList {folder(u"D:\\newer"_s, u"newer"_s), folder(u"D:\\older"_s, u"older"_s)}));
        QVERIFY(QFile::exists(path));
        QVERIFY(!QFile::exists(pinsFile));
        QVERIFY(QFile::exists(pinsFile + u".migrated"_s));

        // Not again: removed favorites stay removed.
        QVERIFY(store.remove(u"D:\\newer"));
        QVERIFY(QFile::copy(pinsFile + u".migrated"_s, pinsFile));
        Favorites later(path, pinsFile);
        later.load();
        QCOMPARE(later.items().size(), 1);
    }

    // A file that cannot be read is set aside, not overwritten by the next change.
    void badFileKeptAside()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(u"favorites.json"_s);
        const QByteArray bad = "{\"format\": \"winshun-favorites\", \"favori";
        QVERIFY(writeAll(path, bad));
        Favorites store(path);
        store.load();
        QVERIFY(store.items().isEmpty());
        QCOMPARE(readAll(path + u".bad"_s), bad);
        QVERIFY(!QFile::exists(path));
        QVERIFY(store.add(folder(u"D:\\a"_s)));
        QCOMPARE(readAll(path + u".bad"_s), bad);
        QVERIFY(readFavoritesFile(readAll(path)));
    }

    // A newer Win顺's file: read-only until that one runs again.
    void newerFileLeftAlone()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(u"favorites.json"_s);
        const QByteArray newer = "{\"format\": \"winshun-favorites\", \"version\": 7, \"favorites\": [], \"groups\": []}";
        QVERIFY(writeAll(path, newer));
        Favorites store(path);
        store.load();
        QVERIFY(store.add(folder(u"D:\\a"_s)));
        QCOMPARE(readAll(path), newer);
        QVERIFY(!QFile::exists(path + u".bad"_s));
    }

    // Nothing typed: every favorite first, in its user's order, one that is
    // gone marked so, one on a server that does not answer neither waited
    // for nor called gone; then the recent items, not twice.
    void listedFirstWithNothingTyped()
    {
        QTemporaryDir dir;
        const QString kept = QDir::toNativeSeparators(dir.filePath(u"收藏的文件夹"_s));
        QVERIFY(QDir().mkpath(kept));
        const QString file = QDir::toNativeSeparators(dir.filePath(u"报告.txt"_s));
        QVERIFY(writeAll(file, "x"));
        const QString recent = QDir::toNativeSeparators(dir.filePath(u"最近.txt"_s));
        QVERIFY(writeAll(recent, "y"));
        const QString gone = QDir::toNativeSeparators(dir.filePath(u"已删除"_s));
        const QString remote = u"\\\\10.255.255.1\\share\\far"_s; // nobody there

        const FavoriteList favorites {
            {Favorite::Kind::File, file, u"周报"_s, u"zb"_s},
            folder(kept, u"收藏的文件夹"_s),
            folder(gone, u"已删除"_s),
            folder(remote, u"far"_s),
            {Favorite::Kind::App, u"shell:AppsFolder\\x!App"_s, u"X"_s, {}}, // no apps read: left out
        };

        SearchEngine engine(nullptr);
        QSignalSpy spy(&engine, &SearchEngine::resultsReady);
        qint64 total = 0;
        const auto listed = [&](Scope scope, qint64& elapsedUs) {
            spy.clear();
            SearchEngine::Request request;
            request.scope = scope;
            request.history = {file, recent, gone};
            request.favorites = favorites;
            engine.submit(std::move(request));
            if (!spy.wait(5000))
                return SearchResults();
            elapsedUs = spy.at(0).at(3).toLongLong();
            total = spy.at(0).at(2).toLongLong();
            return spy.at(0).at(1).value<SearchResults>();
        };

        qint64 us = 0;
        const SearchResults all = listed(Scope::All, us);
        QCOMPARE(total, all.size());
        qInfo("all: %lld us", us);
        QVERIFY2(us < 400'000, qPrintable(QString::number(us))); // at most the one wait (100 ms)
        QStringList paths;
        for (const SearchResult& r : all)
            paths.append(r.path);
        QCOMPARE(paths, (QStringList {file, kept, gone, remote, recent}));
        QVERIFY(all[0].favorite && !all[0].missing && !all[0].isDir);
        QCOMPARE(all[0].name, u"周报"_s);
        QCOMPARE(all[0].favoriteKeyword, u"zb"_s);
        QVERIFY(all[0].modified > 0);
        QVERIFY(all[1].favorite && all[1].isDir && !all[1].missing);
        QVERIFY(all[2].favorite && all[2].missing);
        QVERIFY(all[3].favorite && !all[3].missing); // not known: shown as it is
        QVERIFY(!all[4].favorite);

        const SearchResults folders = listed(Scope::Folders, us);
        paths.clear();
        for (const SearchResult& r : folders)
            paths.append(r.path);
        QCOMPARE(paths, (QStringList {kept, gone, remote}));
    }

    // A share that answers "no such share" (or a server that is not there)
    // has not said the favorite is gone: not marked so once it has answered.
    void notGoneWhenTheDiskWillNotSay()
    {
        const QString share = u"\\\\127.0.0.1\\winshun-no-such-share\\x"_s;
        SearchEngine engine(nullptr);
        QSignalSpy spy(&engine, &SearchEngine::resultsReady);
        for (int round = 0; round < 2; ++round) {
            spy.clear();
            SearchEngine::Request request;
            request.favorites = {folder(share, u"远方共享"_s)};
            engine.submit(std::move(request));
            QVERIFY(spy.wait(5000));
            const SearchResults rows = spy.at(0).at(1).value<SearchResults>();
            QCOMPARE(rows.size(), 1);
            QCOMPARE(rows[0].name, u"远方共享"_s);
            QVERIFY2(!rows[0].missing, qPrintable(u"round %1"_s.arg(round)));
            QTest::qWait(2000); // the answer in by now
        }
    }
};

QTEST_GUILESS_MAIN(TestFavorites)
#include "tst_favorites.moc"
