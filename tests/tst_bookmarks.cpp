// The logic of the winshun.bookmarks plugin (src/bookmarks-plugin/Bookmarks):
// the Bookmarks file format, the folder text, percent-decoding like Python's
// unquote, Unicode lower case, matching, scoring and the rows. Files are
// made in a temporary folder; nothing of the real browsers is read.

#include "Bookmarks.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace bm;

namespace {

QString q(const std::string& s)
{
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QString Q(const char* s)
{
    return QString::fromUtf8(s);
}

std::string url(const char* title, const char* address)
{
    return std::string("{\"type\":\"url\",\"name\":\"") + title + "\",\"url\":\"" + address + "\"}";
}

std::string file(const std::string& bar, const std::string& other = "{\"type\":\"folder\",\"name\":\"Other\",\"children\":[]}")
{
    return "{\"roots\":{\"bookmark_bar\":{\"type\":\"folder\",\"name\":\"Bar\",\"children\":[" + bar + "]},\"other\":" + other
        + ",\"sync_transaction_version\":\"1\"},\"version\":1}";
}

std::vector<Entry> entries(const std::string& content)
{
    return parseFile(content, "Chrome");
}

Entry entry(const std::string& title, const std::string& address, const std::string& folder = "Bar")
{
    return {title, address, folder, "Chrome"};
}

} // namespace

class TstBookmarks : public QObject {
    Q_OBJECT

private slots:
    void nestedFolders()
    {
        const std::string inner = url("Deep", "https://deep.example/");
        const std::string content = file(url("Top", "https://top.example/") + ",{\"type\":\"folder\",\"name\":\"Work\",\"children\":[{\"type\":\"folder\",\"name\":\"Docs\",\"children\":["
                                         + inner + "]}]}",
                                         "{\"type\":\"folder\",\"name\":\"\",\"children\":[" + url("Nameless", "https://n.example/") + "]}");
        const auto found = entries(content);
        QCOMPARE(found.size(), size_t(3));
        QCOMPARE(q(found[0].title), Q("Top"));
        QCOMPARE(q(found[0].folder), Q("Bar")); // the root's own name is in
        QCOMPARE(q(found[1].folder), Q("Bar › Work › Docs"));
        QCOMPARE(q(found[2].folder), Q("")); // a nameless root and nothing above
        QCOMPARE(q(found[1].browser), Q("Chrome"));
    }
    void badFiles()
    {
        QVERIFY(entries("").empty());
        QVERIFY(entries("not json").empty());
        QVERIFY(entries("[1,2]").empty());
        QVERIFY(entries("{\"roots\":5}").empty());
        QVERIFY(entries("{\"roots\":{\"a\":3,\"b\":\"x\"}}").empty());
        // A child that is not an object is skipped, the rest is read.
        const auto found = entries("{\"roots\":{\"a\":{\"type\":\"folder\",\"name\":\"A\",\"children\":[7," + url("T", "https://t.example/") + "]}}}");
        QCOMPARE(found.size(), size_t(1));
    }
    void byteOrderMark()
    {
        const auto found = entries("\xEF\xBB\xBF" + file(url("Top", "https://top.example/")));
        QCOMPARE(found.size(), size_t(1));
    }
    void readFileShared()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("Bookmarks"));
        // Held open for writing, as a running browser does.
        QFile held(path);
        QVERIFY(held.open(QIODevice::WriteOnly));
        held.write(file(url("Top", "https://top.example/")).c_str());
        held.flush();
        const std::string content = readFile(path.toUtf8().toStdString());
        QCOMPARE(entries(content).size(), size_t(1));
        QVERIFY(readFile((dir.filePath(QStringLiteral("missing"))).toUtf8().toStdString()).empty());
    }
    void findsFiles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString chrome = dir.filePath(QStringLiteral("Google/Chrome/User Data"));
        const QString edge = dir.filePath(QStringLiteral("Microsoft/Edge/User Data"));
        for (const QString& p : {chrome + u"/Default", chrome + u"/Profile 1", chrome + u"/NoFile", chrome + u"/.hidden", edge + u"/Default"})
            QVERIFY(QDir().mkpath(p));
        for (const QString& p : {chrome + u"/Default/Bookmarks", chrome + u"/Profile 1/Bookmarks", chrome + u"/.hidden/Bookmarks", edge + u"/Default/Bookmarks"}) {
            QFile f(p);
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        QVERIFY(QDir().mkpath(edge + u"/Profile 2/Bookmarks")); // a folder is not a file
        const QString extra = dir.filePath(QStringLiteral("Brave.Bookmarks"));
        QFile f(extra);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();

        const std::string base = QDir::toNativeSeparators(dir.path()).toUtf8().toStdString();
        const auto files = bookmarkFiles(base, QDir::toNativeSeparators(extra).toUtf8().toStdString());
        QCOMPARE(files.size(), size_t(4));
        QVERIFY(q(files[0]).endsWith(u"Default\\Bookmarks") && q(files[0]).contains(u"Chrome"));
        QVERIFY(q(files[1]).endsWith(u"Profile 1\\Bookmarks"));
        QVERIFY(q(files[2]).contains(u"Edge"));
        QCOMPARE(q(files[3]), QDir::toNativeSeparators(extra));
        QCOMPARE(bookmarkFiles(base, std::string()).size(), size_t(3));
        QCOMPARE(bookmarkFiles(base, QDir::toNativeSeparators(dir.filePath(QStringLiteral("nope"))).toUtf8().toStdString()).size(), size_t(3));
        QCOMPARE(bookmarkFiles(std::string(), std::string()).size(), size_t(0));
    }
    // The setting names one browser's Bookmarks file; the account list beside
    // it is read too, or that browser loses its account bookmarks as well.
    void extraFileWithAccountBeside()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString brave = dir.filePath(QStringLiteral("Brave"));
        QVERIFY(QDir().mkpath(brave));
        // The named file, its account list, and a decoy that is not it.
        for (const QString& p : {brave + u"/Bookmarks", brave + u"/AccountBookmarks", brave + u"/NotBookmarks"}) {
            QFile f(p);
            QVERIFY(f.open(QIODevice::WriteOnly));
        }

        const std::string named = QDir::toNativeSeparators(brave + u"/Bookmarks").toUtf8().toStdString();
        const auto files = bookmarkFiles(std::string(), named);
        QCOMPARE(files.size(), size_t(2));
        QCOMPARE(q(files[0]), QDir::toNativeSeparators(brave + u"/Bookmarks"));
        QCOMPARE(q(files[1]), QDir::toNativeSeparators(brave + u"/AccountBookmarks"));

        // Naming the account file itself must not read it twice, in any case:
        // Windows compares names without case.
        const std::string account = QDir::toNativeSeparators(brave + u"/AccountBookmarks").toUtf8().toStdString();
        QCOMPARE(bookmarkFiles(std::string(), account).size(), size_t(1));
        std::string upper = account;
        for (char& c : upper) {
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
        }
        QCOMPARE(bookmarkFiles(std::string(), upper).size(), size_t(1));

        // A bare file name has no directory to look in: nothing is read
        // beside it, and it is not resolved against the working directory.
        QCOMPARE(bookmarkFiles(std::string(), std::string("no-such-bookmarks-file-3f9a")).size(), size_t(0));

        // No account list beside it: just the named file.
        QFile::remove(brave + u"/AccountBookmarks");
        QCOMPARE(bookmarkFiles(std::string(), named).size(), size_t(1));
    }

    // AccountBookmarks — sometimes with no Bookmarks at all.
    void findsAccountBookmarks()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString chrome = dir.filePath(QStringLiteral("Google/Chrome/User Data"));
        const QString edge = dir.filePath(QStringLiteral("Microsoft/Edge/User Data"));
        for (const QString& p : {chrome + u"/Default", chrome + u"/Profile 1", edge + u"/Default"})
            QVERIFY(QDir().mkpath(p));
        // A signed-in profile whose local list is gone: the account's is the only one.
        for (const QString& p : {chrome + u"/Default/AccountBookmarks", chrome + u"/Profile 1/Bookmarks", chrome + u"/Profile 1/AccountBookmarks", edge + u"/Default/AccountBookmarks"}) {
            QFile f(p);
            QVERIFY(f.open(QIODevice::WriteOnly));
        }

        const std::string base = QDir::toNativeSeparators(dir.path()).toUtf8().toStdString();
        const auto files = bookmarkFiles(base, std::string());
        QCOMPARE(files.size(), size_t(4));
        QVERIFY(q(files[0]).endsWith(u"Default\\AccountBookmarks") && q(files[0]).contains(u"Chrome"));
        QVERIFY(q(files[1]).endsWith(u"Profile 1\\Bookmarks")); // both of a profile, its list first
        QVERIFY(q(files[2]).endsWith(u"Profile 1\\AccountBookmarks"));
        QVERIFY(q(files[3]).contains(u"Edge") && q(files[3]).endsWith(u"AccountBookmarks"));
        QCOMPARE(q(browserOf(files[0], true)), Q("Chrome"));
        QCOMPARE(q(browserOf(files[3], true)), Q("Edge"));
    }
    void browserNames()
    {
        QCOMPARE(q(browserOf("C:\\U\\a\\Local\\Google\\Chrome\\User Data\\Default\\Bookmarks", true)), Q("Chrome"));
        QCOMPARE(q(browserOf("C:\\u\\LOCAL\\MICROSOFT\\EDGE\\user data\\Default\\Bookmarks", true)), Q("Edge"));
        QCOMPARE(q(browserOf("C:\\x\\BraveSoftware\\Brave-Browser\\Bookmarks", true)), Q("Brave"));
        QCOMPARE(q(browserOf("C:\\x\\Vivaldi\\User Data\\Bookmarks", false)), Q("Vivaldi"));
        QCOMPARE(q(browserOf("C:\\x\\Opera Software\\Opera Stable\\Bookmarks", false)), Q("Opera"));
        QCOMPARE(q(browserOf("D:\\mine\\Bookmarks", true)), Q("书签文件"));
        QCOMPARE(q(browserOf("D:\\mine\\Bookmarks", false)), Q("Bookmarks file"));
    }
    void percentDecoding()
    {
        QCOMPARE(q(unquote("https://a.example/dotFiles#%E5%AE%89%E8%A3%85")), Q("https://a.example/dotFiles#安装"));
        QCOMPARE(q(unquote("a+b%20c")), Q("a+b c")); // "+" stays
        QCOMPARE(q(unquote("100%")), Q("100%"));
        QCOMPARE(q(unquote("%zz%4")), Q("%zz%4"));
        QCOMPARE(q(unquote("%41%62")), Q("Ab"));
        QCOMPARE(q(unquote("x%FFy")), Q("x\uFFFDy")); // a byte that is never UTF-8
        QCOMPARE(q(unquote("x%E5%AEy")), Q("x\uFFFDy")); // one cut sequence, one U+FFFD
        QCOMPARE(q(unquote("%E5%E5%AE%89")), Q("\uFFFD安"));
        QCOMPARE(q(unquote("%C0%AF")), Q("\uFFFD\uFFFD")); // overlong
        QCOMPARE(q(unquote("%ED%A0%80")), Q("\uFFFD\uFFFD\uFFFD")); // a surrogate
        QCOMPARE(q(unquote("%F0%9F%98%80")), Q("😀"));
        QCOMPARE(q(unquote("安%E5%AE%89")), Q("安安"));
    }
    void lowerCase()
    {
        QCOMPARE(q(lower("ÄÖÜ ABC")), Q("äöü abc"));
        QCOMPARE(q(lower("ПРИВЕТ Мир")), Q("привет мир"));
        QCOMPARE(q(lower("安装 Ω")), Q("安装 ω"));
        QCOMPARE(q(lower("")), Q(""));
    }
    void splitWords()
    {
        const auto w = words("  Foo\tBAR\u3000Äö  baz\n");
        QCOMPARE(w.size(), size_t(4));
        QCOMPARE(q(w[0]), Q("foo"));
        QCOMPARE(q(w[1]), Q("bar"));
        QCOMPARE(q(w[2]), Q("äö"));
        QCOMPARE(q(w[3]), Q("baz"));
        QVERIFY(words("").empty());
        QVERIFY(words(" \u3000\t").empty());
    }
    void matchesEveryWord()
    {
        const std::vector<Entry> all {entry("Qt docs", "https://doc.qt.io/", "Bar › Dev"), entry("Cats", "https://cats.example/qt"), entry("Dogs", "https://dogs.example/")};
        const auto rows = search(all, "QT", true);
        QCOMPARE(rows.size(), size_t(2));
        QCOMPARE(q(rows[0].title), Q("Qt docs"));
        QCOMPARE(q(rows[1].title), Q("Cats"));
        QCOMPARE(search(all, "qt dev", true).size(), size_t(1)); // the folder counts
        QCOMPARE(search(all, "dogs cats", true).front().id, std::string("none")); // every word must be there
    }
    void scores()
    {
        const std::vector<Entry> all {entry("Other", "https://x.example/ab"), entry("Alpha Beta", "https://y.example/"), entry("Alpha", "https://z.example/beta")};
        const auto rows = search(all, "alpha beta", true);
        QCOMPARE(rows.size(), size_t(2));
        // Both words in the title: 80, ahead; the address-only match: 60. Stable otherwise.
        QCOMPARE(q(rows[0].title), Q("Alpha Beta"));
        QCOMPARE(rows[0].score, 80);
        QCOMPARE(q(rows[1].title), Q("Alpha"));
        QCOMPARE(rows[1].score, 60);
        // The ids follow the order found, not the order shown.
        QCOMPARE(rows[0].id, std::string("bm-0"));
        QCOMPARE(rows[1].id, std::string("bm-1"));
        QCOMPARE(search({entry("Zed", "https://q.example/")}, "", true).front().score, 80);
    }
    void unicodeMatching()
    {
        const std::vector<Entry> all {entry("ÄPFEL und Öl", "https://a.example/"), entry("Привет МИР", "https://b.example/"), entry("x", "https://c.example/%E5%AE%89%E8%A3%85")};
        QCOMPARE(search(all, "äpfel öl", true).size(), size_t(1));
        QCOMPARE(search(all, "ÄPFEL", true).front().score, 80);
        QCOMPARE(q(search(all, "мир", false).front().title), Q("Привет МИР"));
        const auto decoded = search(all, "安装", true); // matched decoded, opened as it is
        QCOMPARE(decoded.size(), size_t(1));
        QCOMPARE(decoded.front().score, 60);
        QCOMPARE(q(decoded.front().open["url"].toString()), Q("https://c.example/%E5%AE%89%E8%A3%85"));
        QCOMPARE(q(decoded.front().subtitle), Q("Chrome · Bar · https://c.example/安装"));
        QCOMPARE(q(decoded.front().match), Q("x https://c.example/安装"));
    }
    void rowFields()
    {
        const auto rows = search({entry("Site", "https://site.example/a%20b", "Bar › Dev"), entry("", "ftp://files.example/%E5%AE%89"), entry("Page", "HTTP://UPPER.example/", "")}, "", true);
        QCOMPARE(rows.size(), size_t(3));
        QCOMPARE(q(rows[0].subtitle), Q("Chrome · Bar › Dev · https://site.example/a b"));
        QCOMPARE(q(rows[0].icon), Q("site:https://site.example/a%20b"));
        QCOMPARE(q(rows[0].copy), Q("https://site.example/a%20b"));
        QCOMPARE(q(rows[0].match), Q("Site https://site.example/a b"));
        QCOMPARE(q(rows[1].title), Q("ftp://files.example/安")); // no title: the shown address
        QCOMPARE(q(rows[1].icon), Q("glyph:U+E734"));
        QCOMPARE(q(rows[2].icon), Q("site:HTTP://UPPER.example/"));
        QCOMPARE(q(rows[2].subtitle), Q("Chrome · HTTP://UPPER.example/")); // an empty folder is left out
        QVERIFY(rows[0].valid);
    }
    void duplicates()
    {
        const auto rows = search({entry("One", "https://d.example/A"), entry("Two", "https://D.example/a", "Other"), entry("Three", "https://d.example/b")}, "", true);
        QCOMPARE(rows.size(), size_t(2));
        QCOMPARE(q(rows[0].title), Q("One"));
        QCOMPARE(q(rows[1].title), Q("Three"));
    }
    void capAtFifty()
    {
        std::vector<Entry> many;
        for (int i = 0; i < 200; ++i)
            many.push_back(entry("Mark " + std::to_string(i), "https://m.example/" + std::to_string(i)));
        const auto rows = search(many, "", true);
        QCOMPARE(rows.size(), size_t(50));
        QCOMPARE(q(rows[49].title), Q("Mark 49"));
        QCOMPARE(rows[49].id, std::string("bm-49"));
        // The cap counts the matches, in file order, before the sort.
        many.push_back(entry("Mark Z", "https://z.example/", "Bar"));
        QCOMPARE(search(many, "z.example", true).size(), size_t(1));
        QCOMPARE(search(many, "mark", true).size(), size_t(50));
    }
    void nothingFound()
    {
        const auto none = search({}, "abc", true);
        QCOMPARE(none.size(), size_t(1));
        QCOMPARE(none[0].id, std::string("none"));
        QVERIFY(!none[0].valid);
        QCOMPARE(q(none[0].title), Q("没有找到书签文件"));
        QCOMPARE(q(none[0].subtitle), Q("装了 Chrome 或 Edge 就会自动读；其他浏览器的在设置里指定"));
        QCOMPARE(q(search({}, "", false)[0].title), Q("No bookmarks file found"));
        QCOMPARE(q(search({}, "", false)[0].subtitle), Q("Chrome's and Edge's are read on their own; name another browser's in the settings"));

        const auto miss = search({entry("A", "https://a.example/")}, "zzz", true);
        QCOMPARE(miss.size(), size_t(1));
        QVERIFY(!miss[0].valid);
        QCOMPARE(q(miss[0].title), Q("没有匹配的书签"));
        QCOMPARE(q(miss[0].subtitle), Q("zzz"));
        QCOMPARE(q(search({entry("A", "https://a.example/")}, "zzz", false)[0].title), Q("No matching bookmark"));
        QCOMPARE(q(miss[0].icon), Q("glyph:U+E734"));
    }
    void pinyinMatching()
    {
        const std::vector<Entry> all {entry("西安交通大学", "https://xjtu.example/"), entry("在线视频", "https://video.example/"),
                                      entry("Plain English", "https://english.example/")};
        // Initials, whole syllables and mixes all find it ...
        QCOMPARE(search(all, "xjtu", true).size(), size_t(1));
        QCOMPARE(q(search(all, "xian", true).front().title), Q("西安交通大学"));
        QCOMPARE(q(search(all, "xajt", true).front().title), Q("西安交通大学"));
        // ... and a title that says the word outranks one that only spells it:
        // 西安 literally reads "xian", 在线 is zai**xian**.
        const auto rows = search(all, "xian", true);
        QCOMPARE(rows.size(), size_t(2));
        QCOMPARE(q(rows[0].title), Q("西安交通大学"));
        QCOMPARE(rows[0].score, 76); // pinyin, and it starts the title
        QCOMPARE(q(rows[1].title), Q("在线视频"));
        QCOMPARE(rows[1].score, 70); // pinyin in the middle of the title
        // Chinese typed as Chinese still matches literally and ranks first.
        QCOMPARE(search(all, "西安", true).front().score, 80);
        // English stays literal: "plain" is not the pinyin of anything.
        QCOMPARE(search(all, "plain", true).size(), size_t(1));
        // One letter is a literal search, so it does not turn into pinyin: "x"
        // appears in every address here, and it never reads 西.
        QCOMPARE(search(all, "x", true).size(), size_t(3));
        // Pinyin only applies to text that has Chinese in it. "xian" is in the
        // title, so it matches literally; "xa" only would as pinyin, which an
        // ASCII name does not get. (The address must not contain "xa" itself.)
        const std::vector<Entry> ascii {entry("xian", "https://q.test/")};
        QCOMPARE(search(ascii, "xian", true).size(), size_t(1));
        QCOMPARE(search(ascii, "xa", true).front().id, std::string("none"));
    }
};

QTEST_APPLESS_MAIN(TstBookmarks)
#include "tst_bookmarks.moc"
