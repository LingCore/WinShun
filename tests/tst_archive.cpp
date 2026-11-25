// The archive search plugin (src/archive-plugin). What it keeps of the
// archives and how it searches that (Index) is built in again here; the
// reading of archives (Lister, on libarchive, linked into the plugin's
// program with the C runtime) is asked of WinShunArchive.exe itself, on the
// small archives of tests/fixtures/archives (tools/make_archive_fixtures.py).

#include "Index.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;

using namespace wsarchive;

namespace {

const QString kFixtures = QStringLiteral(WINSHUN_ARCHIVE_FIXTURES);

// What the plugin's program says of an archive (--list), or of taking a
// member out (--take). Names without a code page of their own are read as
// GBK whatever this computer's is.
QJsonObject run(const QStringList& arguments)
{
    QProcess process;
    process.start(QStringLiteral(WINSHUN_ARCHIVE_EXE), arguments);
    if (!process.waitForFinished(30000))
        return {{u"problem"_s, u"did not end"_s}};
    if (process.exitStatus() != QProcess::NormalExit)
        return {{u"problem"_s, u"crashed"_s}};
    return QJsonDocument::fromJson(process.readAllStandardOutput()).object();
}

QJsonObject listed(const QString& name)
{
    return run({u"--list"_s, kFixtures + u'/' + name, u"936"_s});
}

QStringList paths(const QJsonObject& listing)
{
    QStringList out;
    for (const QJsonValue& v : listing.value(u"members"_s).toArray()) {
        const QJsonObject m = v.toObject();
        out.push_back(m.value(u"path"_s).toString() + (m.value(u"folder"_s).toBool() ? u"\\"_s : QString()));
    }
    return out;
}

// An archive as a pass would put it; the bytes of names it added.
std::size_t put(Index& index, const std::string& path, std::initializer_list<Member> members, Outcome outcome = Outcome::Listed,
    std::uint64_t size = 100, std::int64_t modified = 1'700'000'000)
{
    Entry entry;
    entry.path = path;
    entry.size = size;
    entry.modified = modified;
    entry.outcome = outcome;
    Contents contents;
    for (const Member& m : members)
        contents.add(m);
    return index.put(std::move(entry), std::move(contents));
}

Member file(const char* path, std::uint64_t size = 10, std::int64_t modified = 1'600'000'000)
{
    return {path, size, modified, false, false};
}

QStringList found(const Found& f)
{
    QStringList out;
    for (const Hit& hit : f.hits)
        out.push_back(QString::fromStdString(hit.archive.substr(hit.archive.find_last_of('\\') + 1) + " > " + hit.member));
    return out;
}

// Three archives with something in them, and one whose names are locked.
void fill(Index& index)
{
    put(index, "D:\\备份\\2025备份.zip",
        {{"合同", 0, 0, true, false}, file("合同\\2026 年度采购合同.docx", 2048), file("合同\\附件\\报价单.xlsx", 512), file("notes\\Read Me.TXT"),
            {"notes\\密件.pdf", 77, 1'650'000'000, false, true}});
    put(index, "D:\\下载\\project-src.7z", {file("src\\main.cpp"), file("src\\report\\builder.cpp"), file("docs\\报价说明.md"), file("README.md")});
    put(index, "C:\\Users\\me\\report.tar.gz", {file("report\\summary.txt"), file("report\\data.csv"), file("old\\report-2019.txt")});
    put(index, "D:\\备份\\秘密.7z", {}, Outcome::Locked);
}

} // namespace

class TestArchive : public QObject {
    Q_OBJECT

private slots:
    void words()
    {
        QCOMPARE(wordsOf("  报价  单.xlsx\t"), (std::vector<std::string> {"报价", "单.xlsx"}));
        QCOMPARE(wordsOf("\"Read Me\" txt"), (std::vector<std::string> {"Read Me", "txt"}));
        QCOMPARE(wordsOf("合同\xE3\x80\x80附件"), (std::vector<std::string> {"合同", "附件"})); // a full-width space
        QVERIFY(wordsOf(" \t ").empty());
        QCOMPARE(findFolded("Read Me.TXT", "me.txt"), std::size_t {5});
        QCOMPARE(findFolded("Read Me.TXT", "txt", 9), std::string_view::npos);
        QCOMPARE(findFolded("ab", "abc"), std::string_view::npos);
        QCOMPARE(folded("AbC 报价.XLSX"), std::string("abc 报价.xlsx"));
    }

    void searchByName()
    {
        Index index;
        fill(index);
        // Part of a name, any case; Chinese as it is.
        QCOMPARE(found(index.search({"报价单"}, 50)), QStringList {u"2025备份.zip > 合同\\附件\\报价单.xlsx"_s});
        QCOMPARE(found(index.search({"read", "ME"}, 50)), (QStringList {u"project-src.7z > README.md"_s, u"2025备份.zip > notes\\Read Me.TXT"_s})); // the shorter name first
        // A hit says what a row shows and what an action needs.
        const Found one = index.search({"密件"}, 50);
        QCOMPARE(one.hits.size(), std::size_t {1});
        QCOMPARE(one.hits[0].archive, std::string("D:\\备份\\2025备份.zip"));
        QCOMPARE(one.hits[0].index, std::uint32_t {4});
        QCOMPARE(one.hits[0].size, std::uint64_t {77});
        QCOMPARE(one.hits[0].modified, std::int64_t {1'650'000'000});
        QVERIFY(one.hits[0].locked && !one.hits[0].folder);
        // A folder inside an archive is found too, and said to be one.
        const Found folders = index.search({"合同"}, 50);
        QVERIFY(folders.hits.size() == 3);
        QVERIFY(std::ranges::count_if(folders.hits, [](const Hit& h) { return h.folder; }) == 1);
        // Nothing: an empty answer, not an error; an archive whose names are locked has none.
        QVERIFY(index.search({"没有这个"}, 50).hits.empty());
        QVERIFY(index.search({}, 50).hits.empty());
        QVERIFY(index.search({"秘密"}, 50).hits.empty());
    }

    void searchRanksOwnNameFirst()
    {
        Index index;
        fill(index);
        // "report": members named so before those only in a folder of that
        // name; of those, the shorter name first.
        const QStringList rows = found(index.search({"report"}, 50));
        QVERIFY2(rows.size() == 4, qPrintable(rows.join(u" | "_s)));
        QCOMPARE(rows.first(), u"report.tar.gz > old\\report-2019.txt"_s);
        QVERIFY(rows.indexOf(u"report.tar.gz > old\\report-2019.txt"_s) < rows.indexOf(u"project-src.7z > src\\report\\builder.cpp"_s));
        // A path typed with either separator.
        QCOMPARE(found(index.search({"src/report"}, 50)), QStringList {u"project-src.7z > src\\report\\builder.cpp"_s});
    }

    void searchWithTheArchiveName()
    {
        Index index;
        fill(index);
        // One word the archive's, one the member's.
        QCOMPARE(found(index.search({"备份", "报价"}, 50)), QStringList {u"2025备份.zip > 合同\\附件\\报价单.xlsx"_s});
        QCOMPARE(found(index.search({"project", "cpp"}, 50)), (QStringList {u"project-src.7z > src\\main.cpp"_s, u"project-src.7z > src\\report\\builder.cpp"_s}));
        // Every word in the archive's name: its members that have one of
        // them, not all it holds (the archive itself is an ordinary result).
        QVERIFY(index.search({"备份"}, 50).hits.empty());
        QCOMPARE(found(index.search({"project", "src"}, 50)), (QStringList {u"project-src.7z > src\\main.cpp"_s, u"project-src.7z > src\\report\\builder.cpp"_s}));
        // The archive's folder is not its name.
        QVERIFY(index.search({"下载", "main"}, 50).hits.empty());
    }

    void searchWithin()
    {
        Index index;
        fill(index);
        Where where;
        where.folders = {"d:\\备份"};
        QCOMPARE(found(index.search({"txt"}, 50, where)), QStringList {u"2025备份.zip > notes\\Read Me.TXT"_s});
        where.folders = {"D:\\备"}; // part of a folder's name is no folder
        QVERIFY(index.search({"txt"}, 50, where).hits.empty());
        where.folders = {"D:\\"};
        QCOMPARE(index.search({"txt"}, 50, where).hits.size(), std::size_t {1});
        // Types: by the member's name, folders have none.
        where = {};
        where.extensions = {"xlsx", "md"};
        QCOMPARE(found(index.search({"报价"}, 50, where)), (QStringList {u"2025备份.zip > 合同\\附件\\报价单.xlsx"_s, u"project-src.7z > docs\\报价说明.md"_s}));
        where.extensions = {"docx"};
        QCOMPARE(found(index.search({"合同"}, 50, where)), QStringList {u"2025备份.zip > 合同\\2026 年度采购合同.docx"_s});
    }

    void searchLimit()
    {
        Index index;
        Entry many;
        many.path = "E:\\photos.zip";
        many.outcome = Outcome::Listed;
        Contents photos;
        for (int i = 0; i < 500; ++i)
            photos.add({"2024\\IMG_" + std::to_string(1000 + i) + ".jpg", 10, 0, false, false});
        index.put(std::move(many), std::move(photos));
        const Found some = index.search({"img_"}, 20);
        QCOMPARE(some.hits.size(), std::size_t {20});
        QVERIFY(some.more);
        QCOMPARE(some.hits[0].member, std::string("2024\\IMG_1000.jpg")); // in the archive's order among equals
        const Found all = index.search({"img_1499"}, 20);
        QCOMPARE(all.hits.size(), std::size_t {1});
        QCOMPARE(all.hits[0].index, std::uint32_t {499});
        QVERIFY(!all.more);
    }

    void keepsAndForgets()
    {
        QTemporaryDir dir;
        const std::wstring saved = (dir.path() + u"/archives.bin"_s).toStdWString();
        Index index;
        QVERIFY(!index.load(saved)); // none yet
        fill(index);
        QVERIFY(index.changed());
        const Totals before = index.totals();
        QCOMPARE(before.archives, std::size_t {4});
        QCOMPARE(before.members, std::size_t {12});
        QCOMPARE(before.byOutcome[static_cast<std::size_t>(Outcome::Locked)], std::size_t {1});
        QVERIFY(index.save(saved));
        QVERIFY(!index.changed());

        Index again;
        QVERIFY(again.load(saved));
        QCOMPARE(again.totals().members, before.members);
        QCOMPARE(again.totals().nameBytes, before.nameBytes);
        QCOMPARE(found(again.search({"报价单"}, 50)), QStringList {u"2025备份.zip > 合同\\附件\\报价单.xlsx"_s});
        const auto kept = again.find("D:\\备份\\2025备份.zip");
        QVERIFY(kept);
        QCOMPARE(kept->size, std::uint64_t {100});
        QCOMPARE(kept->modified, std::int64_t {1'700'000'000});
        QVERIFY(!again.find("d:\\nowhere.zip"));

        // An archive read again replaces what was kept; one gone is forgotten.
        put(again, "D:\\备份\\2025备份.zip", {file("新\\只有这个.txt")});
        QVERIFY(again.search({"报价单"}, 50).hits.empty());
        QCOMPARE(again.search({"只有这个"}, 50).hits.size(), std::size_t {1});
        QCOMPARE(again.keepOnly({"D:\\备份\\2025备份.zip", "C:\\Users\\me\\report.tar.gz"}), std::size_t {2});
        QVERIFY(again.search({"main"}, 50).hits.empty());
        QCOMPARE(again.totals().archives, std::size_t {2});

        // A damaged file (cut short, or not ours): an empty index, built again.
        QFile file(QString::fromStdWString(saved));
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.resize(file.size() - 7));
        file.close();
        Index damaged;
        QVERIFY(!damaged.load(saved));
        QCOMPARE(damaged.totals().archives, std::size_t {0});
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("not an index at all");
        file.close();
        QVERIFY(!damaged.load(saved));
    }

    // Copies of one archive (a delivery kept in several folders): one list
    // of members between them, looked through once, and a copy by its name,
    // size and time is not opened again to be listed.
    void copiesShareTheirContents()
    {
        const std::initializer_list<Member> members {file("qt\\src\\corelib\\global\\qglobal.h", 9000), file("qt\\src\\gui\\kernel\\qwindow.cpp"),
            file("qt\\README.md")};
        Index index;
        const std::size_t first = put(index, "F:\\交付\\A\\qt-src.tar.xz", members, Outcome::Listed, 5'000'000, 1'690'000'000);
        QVERIFY(first > 0);
        QCOMPARE(put(index, "F:\\交付\\B\\qt-src.tar.xz", members, Outcome::Listed, 5'000'000, 1'690'000'000), std::size_t {0});
        QCOMPARE(put(index, "F:\\备份\\别的名字.zip", members, Outcome::Listed, 4'000'000, 1'680'000'000), std::size_t {0}); // the same names: shared too
        QVERIFY(put(index, "F:\\备份\\另一个.zip", {file("qt\\README.md")}) > 0);
        const Totals totals = index.totals();
        QCOMPARE(totals.archives, std::size_t {4});
        QCOMPARE(totals.members, std::size_t {10}); // every archive's, as the user counts them
        QCOMPARE(totals.nameBytes, first + std::string("qt\\README.md\n").size()); // kept once
        QCOMPARE(index.find("F:\\交付\\A\\qt-src.tar.xz")->contents.get(), index.find("F:\\交付\\B\\qt-src.tar.xz")->contents.get());
        // Each copy is a row of its own.
        QCOMPARE(found(index.search({"qglobal"}, 50)), (QStringList {u"qt-src.tar.xz > qt\\src\\corelib\\global\\qglobal.h"_s,
            u"qt-src.tar.xz > qt\\src\\corelib\\global\\qglobal.h"_s, u"别的名字.zip > qt\\src\\corelib\\global\\qglobal.h"_s}));
        // ... but a member comes once before its copies in other archives.
        QCOMPARE(found(index.search({"readme"}, 50)), (QStringList {u"qt-src.tar.xz > qt\\README.md"_s, u"另一个.zip > qt\\README.md"_s,
            u"qt-src.tar.xz > qt\\README.md"_s, u"别的名字.zip > qt\\README.md"_s}));
        QCOMPARE(index.search({"readme"}, 2).hits[1].archive, std::string("F:\\备份\\另一个.zip"));
        // A word of the archive's own name counts for each by its own name.
        QCOMPARE(found(index.search({"别的", "qwindow"}, 50)), QStringList {u"别的名字.zip > qt\\src\\gui\\kernel\\qwindow.cpp"_s});

        // Another copy, not read yet: known by name (any case), size and time.
        const auto same = index.copyOf("G:\\elsewhere\\QT-SRC.tar.xz", 5'000'000, 1'690'000'000);
        QVERIFY(same);
        QCOMPARE(same->count(), std::size_t {3});
        QVERIFY(!index.copyOf("G:\\elsewhere\\qt-src.tar.xz", 5'000'001, 1'690'000'000)); // another size
        QVERIFY(!index.copyOf("G:\\elsewhere\\qt-src.tar.xz", 5'000'000, 1'690'000'001)); // another time
        QVERIFY(!index.copyOf("G:\\elsewhere\\qt-src-2.tar.xz", 5'000'000, 1'690'000'000)); // another name
        QVERIFY(!index.copyOf("G:\\elsewhere\\qt-src.tar.xz", 0, 1'690'000'000)); // a size the index does not know
        QVERIFY(!index.copyOf("G:\\elsewhere\\qt-src.tar.xz", 5'000'000, 0));
        Entry copy;
        copy.path = "G:\\elsewhere\\QT-SRC.tar.xz";
        copy.size = 5'000'000;
        copy.modified = 1'690'000'000;
        copy.outcome = Outcome::Listed;
        index.put(copy, same);
        QCOMPARE(index.totals().nameBytes, totals.nameBytes);
        QCOMPARE(index.search({"qglobal"}, 50).hits.size(), std::size_t {4});
        // One read only in part does not speak for its copies.
        put(index, "F:\\下载\\half.zip", {file("a.txt")}, Outcome::Partial, 777, 1'600'000'000);
        QVERIFY(!index.copyOf("F:\\别处\\half.zip", 777, 1'600'000'000));

        // Kept once in the file too, and shared again when read back.
        QTemporaryDir dir;
        const std::wstring saved = (dir.path() + u"/archives.bin"_s).toStdWString();
        QVERIFY(index.save(saved));
        Index again;
        QVERIFY(again.load(saved));
        QCOMPARE(again.totals().archives, index.totals().archives);
        QCOMPARE(again.totals().members, index.totals().members);
        QCOMPARE(again.totals().nameBytes, index.totals().nameBytes);
        QCOMPARE(again.find("F:\\交付\\A\\qt-src.tar.xz")->contents.get(), again.find("F:\\备份\\别的名字.zip")->contents.get());
        QVERIFY(again.copyOf("H:\\qt-src.tar.xz", 5'000'000, 1'690'000'000));
        QCOMPARE(again.search({"qglobal"}, 50).hits.size(), std::size_t {4});
        // The copies gone, what they held goes with them.
        QCOMPARE(again.keepOnly({"F:\\备份\\另一个.zip"}), std::size_t {5});
        QVERIFY(!again.copyOf("H:\\qt-src.tar.xz", 5'000'000, 1'690'000'000));
        QCOMPARE(again.totals().nameBytes, std::string("qt\\README.md\n").size());
    }

    // ---- reading archives: WinShunArchive.exe --list ----------------------------------

    void lists_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("format");
        QTest::addColumn<bool>("folders"); // the archive has entries for its folders
        QTest::newRow("zip, names flagged UTF-8") << u"names-utf8.zip"_s << u"ZIP"_s << true;
        QTest::newRow("zip, names in GBK") << u"names-gbk.zip"_s << u"ZIP"_s << true;
        QTest::newRow("zip, names in UTF-8 without the flag") << u"names-utf8-unflagged.zip"_s << u"ZIP"_s << true;
        QTest::newRow("7z") << u"files.7z"_s << u"7-Zip"_s << false;
        QTest::newRow("tar") << u"files.tar"_s << u"pax"_s << true;
        QTest::newRow("tar.gz") << u"files.tar.gz"_s << u"pax"_s << true;
        QTest::newRow("tar.xz") << u"files.tar.xz"_s << u"pax"_s << true;
    }
    void lists()
    {
        QFETCH(QString, name);
        QFETCH(QString, format);
        QFETCH(bool, folders);
        const QJsonObject listing = listed(name);
        QCOMPARE(listing.value(u"outcome"_s).toString(), u"listed"_s);
        QVERIFY2(listing.value(u"format"_s).toString().contains(format), qPrintable(listing.value(u"format"_s).toString()));
        QStringList expected {u"合同\\2026 年度采购合同.docx"_s, u"合同\\附件\\报价单.xlsx"_s, u"notes\\Read Me.TXT"_s, u"notes\\会议纪要.md"_s};
        if (folders)
            expected.prepend(u"合同\\"_s);
        QCOMPARE(paths(listing), expected);
        // Sizes and times as the archive has them (2026-03-14 09:26:52 +08:00,
        // a zip's in local time: within a day).
        const QJsonObject last = listing.value(u"members"_s).toArray().last().toObject();
        QCOMPARE(last.value(u"size"_s).toInt(), 35);
        QVERIFY(!last.value(u"locked"_s).toBool());
        QVERIFY2(std::abs(last.value(u"modified"_s).toInteger() - 1'773'480'412) <= 86'400, qPrintable(QString::number(last.value(u"modified"_s).toInteger())));
    }

    void listsRar()
    {
        QJsonObject listing = listed(u"rar5.rar"_s);
        QCOMPARE(listing.value(u"outcome"_s).toString(), u"listed"_s);
        QCOMPARE(listing.value(u"format"_s).toString(), u"RAR5"_s);
        QCOMPARE(paths(listing), QStringList {u"helloworld.txt"_s});
        listing = listed(u"rar4.rar"_s);
        QCOMPARE(listing.value(u"outcome"_s).toString(), u"listed"_s);
        const QStringList members = paths(listing);
        QVERIFY2(members.contains(u"test.txt"_s) && members.contains(u"testdir\\test.txt"_s) && members.contains(u"testdir\\"_s),
            qPrintable(members.join(u" | "_s)));
    }

    void saysWhatItCannotRead()
    {
        // Content behind a password: the names are there, each said locked.
        QJsonObject listing = listed(u"locked-files.7z"_s);
        QCOMPARE(listing.value(u"outcome"_s).toString(), u"listed"_s);
        QCOMPARE(paths(listing).size(), 4);
        for (const QJsonValue& v : listing.value(u"members"_s).toArray())
            QVERIFY(v.toObject().value(u"locked"_s).toBool());
        // Names behind a password, 7z and rar: locked, nothing listed.
        for (const QString& name : {u"locked-names.7z"_s, u"locked-names.rar"_s}) {
            listing = listed(name);
            QVERIFY2(listing.value(u"outcome"_s).toString() == u"locked", qPrintable(name + u": "_s + listing.value(u"outcome"_s).toString()));
            QVERIFY(paths(listing).isEmpty());
        }
        // A text file called .zip; a file that is not there.
        listing = listed(u"not-an-archive.zip"_s);
        QCOMPARE(listing.value(u"outcome"_s).toString(), u"not an archive"_s);
        QVERIFY(paths(listing).isEmpty());
        listing = listed(u"no-such-file.zip"_s);
        QCOMPARE(listing.value(u"outcome"_s).toString(), u"unreadable"_s);
        // A download cut short: what can still be read, and said partial.
        listing = listed(u"cut-short.zip"_s);
        QCOMPARE(listing.value(u"outcome"_s).toString(), u"partial"_s);
        QVERIFY(!paths(listing).isEmpty());
    }

    void takesOneMemberOut()
    {
        QTemporaryDir dir;
        const auto take = [&](const QString& name, const QString& member, int index, const QString& target) {
            return run({u"--take"_s, kFixtures + u'/' + name, member, QString::number(index), QDir::toNativeSeparators(dir.path() + u'/' + target), u"936"_s});
        };
        const auto text = [&](const QString& target) {
            QFile file(dir.path() + u'/' + target);
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : u"(no file)"_s;
        };
        // From each kind, by the name as it was listed (GBK in the archive too).
        QCOMPARE(take(u"names-gbk.zip"_s, u"合同\\附件\\报价单.xlsx"_s, 2, u"a.xlsx"_s).value(u"taken"_s).toString(), u"done"_s);
        QCOMPARE(text(u"a.xlsx"_s), u"报价单\n"_s);
        QCOMPARE(take(u"files.7z"_s, u"notes\\会议纪要.md"_s, 3, u"b.md"_s).value(u"taken"_s).toString(), u"done"_s);
        QVERIFY(text(u"b.md"_s).startsWith(u"# 会议纪要"));
        QCOMPARE(take(u"files.tar.xz"_s, u"notes\\Read Me.TXT"_s, 3, u"c.txt"_s).value(u"taken"_s).toString(), u"done"_s);
        QCOMPARE(text(u"c.txt"_s), u"hello from the archive\n"_s);
        QCOMPARE(take(u"rar5.rar"_s, u"helloworld.txt"_s, 0, u"d.txt"_s).value(u"taken"_s).toString(), u"done"_s);
        QVERIFY(text(u"d.txt"_s).contains(u"hello"));
        // Its place given wrong (the archive changed since): by its path alone.
        QCOMPARE(take(u"files.tar.gz"_s, u"notes\\Read Me.TXT"_s, 0, u"e.txt"_s).value(u"taken"_s).toString(), u"done"_s);
        QCOMPARE(text(u"e.txt"_s), u"hello from the archive\n"_s);
        // What cannot be taken says why, and leaves no file behind.
        QCOMPARE(take(u"locked-files.7z"_s, u"notes\\会议纪要.md"_s, 3, u"f.md"_s).value(u"taken"_s).toString(), u"locked"_s);
        QCOMPARE(take(u"files.7z"_s, u"notes\\没有.md"_s, 1, u"g.md"_s).value(u"taken"_s).toString(), u"not found"_s);
        QCOMPARE(take(u"names-utf8.zip"_s, u"合同"_s, 0, u"h"_s).value(u"taken"_s).toString(), u"not found"_s); // a folder
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files, QDir::Name), (QStringList {u"a.xlsx"_s, u"b.md"_s, u"c.txt"_s, u"d.txt"_s, u"e.txt"_s}));
    }
};

QTEST_GUILESS_MAIN(TestArchive)
#include "tst_archive.moc"
