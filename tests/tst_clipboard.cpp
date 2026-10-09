#include "ClipStore.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace ws;
using namespace Qt::StringLiterals;

namespace {

constexpr qint64 kDay = 24ll * 60 * 60 * 1000;
constexpr qint64 kStart = 1'790'000'000'000; // some time in 2026

ClipCapture text(const QString& s, const QString& source = u"记事本"_s)
{
    ClipCapture c;
    c.kind = kindOfText(s);
    c.text = s;
    c.source = source;
    return c;
}

QStringList texts(const ClipStore& store, const std::vector<int>& rows)
{
    QStringList list;
    for (const int row : rows)
        list.append(store.clips()[static_cast<std::size_t>(row)].text);
    return list;
}

QStringList texts(const ClipStore& store)
{
    QStringList list;
    for (const Clip& c : store.clips())
        list.append(c.text);
    return list;
}

} // namespace

class ClipboardTest : public QObject {
    Q_OBJECT

private slots:
    void kinds()
    {
        QCOMPARE(kindOfText(u"会议改到周三"_s), ClipKind::Text);
        QCOMPARE(kindOfText(u"https://github.com/LingCore/WinShun"_s), ClipKind::Link);
        QCOMPARE(kindOfText(u"  www.example.com/a?b=1 \n"_s), ClipKind::Link);
        QCOMPARE(kindOfText(u"see https://example.com"_s), ClipKind::Text);
        QCOMPARE(kindOfText(u"C:\\Users\\young\\年度报告.docx"_s), ClipKind::Path);
        QCOMPARE(kindOfText(u"\"C:\\Program Files\\WinShun\\WinShun.exe\""_s), ClipKind::Path); // "Copy as path"
        QCOMPARE(kindOfText(u"\\\\nas\\share\\a.txt\r\nD:/b.txt"_s), ClipKind::Path);
        QCOMPARE(kindOfText(u"C:\\a\nnot a path"_s), ClipKind::Text);
        QCOMPARE(kindOfText(u"C:"_s), ClipKind::Text);

        Clip path;
        path.kind = ClipKind::Path;
        path.text = u"\"C:/a b/c.txt\"\r\nD:\\d.txt"_s;
        QCOMPARE(path.files(), (QStringList {u"C:\\a b\\c.txt"_s, u"D:\\d.txt"_s}));
    }

    void addMovesCopiesToTheTop()
    {
        QTemporaryDir dir;
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart));
        const qint64 a = store.add(text(u"甲"_s), kStart);
        const qint64 b = store.add(text(u"乙"_s), kStart + 1);
        QVERIFY(a > 0 && b > a);
        QCOMPARE(texts(store), (QStringList {u"乙"_s, u"甲"_s}));
        // The same text again: no new entry, moved up, from where it came this time.
        QCOMPARE(store.add(text(u"甲"_s, u"微信"_s), kStart + 2), a);
        QCOMPARE(texts(store), (QStringList {u"甲"_s, u"乙"_s}));
        QCOMPARE(store.clips().front().source, u"微信"_s);
        QCOMPARE(store.clips().front().created, kStart);
        store.touch(b, kStart + 3);
        QCOMPARE(texts(store), (QStringList {u"乙"_s, u"甲"_s}));
        // A path as text and the same file copied in Explorer are two things.
        ClipCapture file;
        file.kind = ClipKind::Files;
        file.text = u"C:\\x.txt"_s;
        QVERIFY(store.add(text(u"C:\\x.txt"_s), kStart + 4) != store.add(file, kStart + 5));
        QCOMPARE(store.clips().size(), std::size_t(4));

        // On the clipboard when the history started: a listed entry stays
        // where it is, with its times; one not listed yet is added.
        ClipCapture already = text(u"甲"_s, u"微信"_s);
        already.initial = true;
        QCOMPARE(store.add(already, kStart + 6), a);
        QCOMPARE(store.clips().back().text, u"甲"_s);
        QCOMPARE(store.clips().back().used, kStart + 2);
        ClipCapture fresh = text(u"丙"_s);
        fresh.initial = true;
        QVERIFY(store.add(fresh, kStart + 7) > 0);
        QCOMPARE(store.clips().front().text, u"丙"_s);
    }

    void limits()
    {
        QTemporaryDir dir;
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart));
        store.setLimits({3, 30}, kStart);
        const qint64 pinned = store.add(text(u"1"_s), kStart);
        const qint64 one = pinned;
        store.setGroup(std::span(&one, 1), ClipStore::kPinned);
        for (int i = 2; i <= 6; ++i)
            store.add(text(QString::number(i)), kStart + i);
        // Three in no group, and the pinned one however old.
        QCOMPARE(texts(store), (QStringList {u"6"_s, u"5"_s, u"4"_s, u"1"_s}));
        store.setLimits({100, 30}, kStart + 40 * kDay);
        QCOMPARE(texts(store), (QStringList {u"1"_s}));
        store.setLimits({100, 0}, kStart + 400 * kDay); // no time limit
        store.add(text(u"old"_s), kStart);
        store.setLimits({100, 0}, kStart + 4000 * kDay);
        QCOMPARE(store.clips().size(), std::size_t(2));
    }

    void find()
    {
        QTemporaryDir dir;
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart));
        store.add(text(u"下周三的会议改到十点"_s, u"微信"_s), kStart);
        store.add(text(u"https://github.com/LingCore/WinShun"_s, u"Edge"_s), kStart + 1);
        store.add(text(u"C:\\报告\\年度报告.docx"_s), kStart + 2);
        ClipCapture image;
        image.kind = ClipKind::Image;
        image.png = QByteArray("\x89PNG", 4);
        image.hash = "pixels";
        image.width = 10;
        image.height = 20;
        image.source = u"PixPin"_s;
        const qint64 imageId = store.add(image, kStart + 3);
        QVERIFY(imageId > 0);
        store.flush(); // written in the background
        QVERIFY(QFile::exists(store.imagePath(imageId)));
        QVERIFY(!store.imagePending(imageId));

        using Category = ClipFilter::Category;
        QCOMPARE(store.find({}).size(), std::size_t(4));
        QCOMPARE(texts(store, store.find({Category::Links, 0, {}})), (QStringList {u"https://github.com/LingCore/WinShun"_s}));
        QCOMPARE(texts(store, store.find({Category::Files, 0, {}})), (QStringList {u"C:\\报告\\年度报告.docx"_s}));
        QCOMPARE(store.find({Category::Images, 0, {}}).size(), std::size_t(1));
        QCOMPARE(store.find({Category::Text, 0, {}}).size(), std::size_t(1));
        // Words, all of them, any case; pinyin; the program after @.
        QCOMPARE(texts(store, store.find({Category::All, 0, u"WINSHUN github"_s})).size(), qsizetype(1));
        QCOMPARE(texts(store, store.find({Category::All, 0, u"hy"_s})), (QStringList {u"下周三的会议改到十点"_s}));
        QCOMPARE(texts(store, store.find({Category::All, 0, u"ndbg"_s})), (QStringList {u"C:\\报告\\年度报告.docx"_s}));
        QCOMPARE(texts(store, store.find({Category::All, 0, u"会议　十点"_s})).size(), qsizetype(1)); // full-width space
        QCOMPARE(texts(store, store.find({Category::All, 0, u"会议 github"_s})).size(), qsizetype(0));
        QCOMPARE(store.find({Category::All, 0, u"@wx"_s}).size(), std::size_t(1));
        QCOMPARE(store.find({Category::All, 0, u"@pixpin"_s}).size(), std::size_t(1));

        const ClipMatcher matcher(u"hy 十点"_s);
        const QString line = u"下周三的会议改到十点"_s;
        QCOMPARE(matcher.spans(line), (QList<std::pair<qsizetype, qsizetype>> {{4, 2}, {8, 2}}));
    }

    void persistsAndRestores()
    {
        QTemporaryDir dir;
        qint64 kept = 0;
        qint64 group = 0;
        QString longText(ClipStore::kTextInMemory + 10, u'字');
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart));
            ClipCapture formatted = text(u"粗体"_s);
            formatted.html = "Version:0.9\r\n<b>粗体</b>";
            formatted.rtf = "{\\rtf1 \\b 粗体}";
            kept = store.add(formatted, kStart);
            store.add(text(longText), kStart + 1);
            group = store.addGroup(u"常用回复"_s);
            QVERIFY(group > ClipStore::kPinned);
            store.setGroup(std::span(&kept, 1), group);
            store.setValue(u"separator"_s, u"comma"_s);
        }
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart + 2));
        QCOMPARE(store.groups().size(), std::size_t(2));
        QCOMPARE(store.groups().front().id, ClipStore::kPinned);
        QCOMPARE(store.groups().back().name, u"常用回复"_s);
        QCOMPARE(store.value(u"separator"_s), u"comma"_s);
        const Clip* c = store.clip(kept);
        QVERIFY(c);
        QCOMPARE(c->group, group);
        QVERIFY(c->html && c->rtf);
        const ClipPayload payload = store.payload(kept);
        QCOMPARE(payload.html, QByteArray("Version:0.9\r\n<b>粗体</b>"));
        QCOMPARE(payload.rtf, QByteArray("{\\rtf1 \\b 粗体}"));
        // Only the start of a long text is kept in memory; pasting reads all of it.
        const Clip& big = store.clips().front();
        QVERIFY(big.truncated());
        QCOMPARE(big.text.size(), ClipStore::kTextInMemory);
        QCOMPARE(store.payload(big.id).text, longText);

        // Removed and put back, as it was.
        const qint64 ids[] {kept, big.id};
        std::vector<ClipRecord> removed = store.remove(ids);
        QCOMPARE(removed.size(), std::size_t(2));
        QVERIFY(store.clips().empty());
        store.restore(std::move(removed));
        QCOMPARE(store.clips().size(), std::size_t(2));
        QCOMPARE(store.clips().front().textLength, longText.size());
        QCOMPARE(store.clip(kept)->group, group);
        QCOMPARE(store.payload(kept).rtf, QByteArray("{\\rtf1 \\b 粗体}"));

        // Removing a group removes what is in it.
        store.removeGroup(group);
        QCOMPARE(store.groups().size(), std::size_t(1));
        QVERIFY(!store.clip(kept));
        store.removeGroup(ClipStore::kPinned); // the built-in one stays
        QCOMPARE(store.groups().size(), std::size_t(1));
        QCOMPARE(store.clearHistory(), 1);
        QVERIFY(store.clips().empty());
    }

    void picturesOfRemovedEntries()
    {
        QTemporaryDir dir;
        qint64 id = 0;
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart));
            ClipCapture image;
            image.kind = ClipKind::Image;
            image.png = "png";
            image.hash = "a";
            id = store.add(image, kStart);
            const std::vector<ClipRecord> removed = store.remove(std::span(&id, 1));
            QVERIFY(QFile::exists(store.imagePath(id))); // for undo
        }
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart));
        store.flush(); // cleared away in the background
        QVERIFY(!QFile::exists(store.imagePath(id))); // nothing refers to it any more
    }

    void removedGroupsComeBack()
    {
        QTemporaryDir dir;
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart));
            const qint64 work = store.addGroup(u"工作"_s);
            const qint64 home = store.addGroup(u"家"_s);
            const qint64 a = store.add(text(u"甲"_s), kStart);
            ClipCapture image;
            image.kind = ClipKind::Image;
            image.png = "png";
            image.hash = "b";
            const qint64 picture = store.add(image, kStart + 1);
            const qint64 members[] {a, picture};
            store.setGroup(members, work);

            ClipGroupRecord removed = store.removeGroup(work);
            QCOMPARE(removed.group.name, u"工作"_s);
            QCOMPARE(removed.clips.size(), std::size_t(2));
            QVERIFY(store.clips().empty());
            QCOMPARE(store.groups().size(), std::size_t(2));
            QVERIFY(QFile::exists(store.imagePath(picture))); // for undo
            QCOMPARE(store.removeGroup(ClipStore::kPinned).group.id, 0); // the built-in one stays

            store.restoreGroup(std::move(removed));
            QCOMPARE(store.groups().size(), std::size_t(3));
            QCOMPARE(store.groups()[1].id, work); // back in its place, before 家
            QCOMPARE(store.groups()[2].id, home);
            QCOMPARE(store.groupSize(work), 2);
        }
        ClipStore store(dir.path()); // and so it was saved
        QVERIFY(store.open(kStart));
        QCOMPARE(store.groups().size(), std::size_t(3));
        QCOMPARE(store.groups()[1].name, u"工作"_s);
        QCOMPARE(store.groupSize(store.groups()[1].id), 2);
    }

    void writtenInTheBackground()
    {
        QTemporaryDir dir;
        const QString big(300'000, u'长');
        qint64 picture = 0;
        qint64 formatted = 0;
        qint64 last = 0;
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart));
            store.setLimits({2, 0}, kStart);
            ClipCapture image;
            image.kind = ClipKind::Image;
            image.png = "png";
            image.hash = "c";
            picture = store.add(image, kStart);
            ClipCapture capture = text(big);
            capture.html = "Version:0.9\r\n<b>长</b>";
            formatted = store.add(capture, kStart + 1);
            // Read back at once, all of it, written yet or not.
            const ClipPayload payload = store.payload(formatted);
            QCOMPARE(payload.text, big);
            QCOMPARE(payload.html, capture.html);
            // Over the limit: the picture goes, maybe before it was even written.
            const qint64 removed = store.add(text(u"甲"_s), kStart + 2);
            QVERIFY(!store.clip(picture) && !store.imagePending(picture));
            QCOMPARE(store.remove(std::span(&removed, 1)).size(), std::size_t(1));
            last = store.add(text(u"乙"_s), kStart + 3);
            QVERIFY(last > removed); // an id is never given twice
        } // what is still queued is written before the store goes
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart + 4));
            QCOMPARE(texts(store), (QStringList {u"乙"_s, big.left(ClipStore::kTextInMemory)}));
            QCOMPARE(store.payload(formatted).text, big);
            QVERIFY(!QFile::exists(store.imagePath(picture)));
            store.remove(std::span(&last, 1)); // the highest id there was
        }
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart + 5));
        QVERIFY(store.add(text(u"丙"_s), kStart + 5) > last);
    }

    void entriesThatCannotBeSaved()
    {
        QTemporaryDir dir;
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart));
        int lost = 0;
        QList<qint64> saved;
        store.setListener({.lost = [&] { ++lost; }, .pictureSaved = [&](qint64 id) { saved.append(id); }});
        store.add(text(u"甲"_s), kStart);
        ClipCapture image;
        image.kind = ClipKind::Image;
        image.png = "png";
        image.hash = "d";
        const qint64 good = store.add(image, kStart + 1);
        // A folder where the next picture's file would go: it cannot be written.
        QVERIFY(QDir().mkpath(store.imagePath(good + 1)));
        image.hash = "e";
        const qint64 bad = store.add(image, kStart + 2);
        QCOMPARE(bad, good + 1);
        QVERIFY(store.clip(bad)); // listed at once
        store.flush();
        QCOMPARE(saved, QList<qint64> {good});
        QCOMPARE(lost, 1);
        QVERIFY(!store.clip(bad));
        QVERIFY(!store.imagePending(bad));
        QCOMPARE(store.clips().size(), std::size_t(2));
    }

    void bundles()
    {
        Clip a;
        a.kind = ClipKind::Text;
        a.text = u"甲"_s;
        Clip link;
        link.kind = ClipKind::Link;
        link.text = u"https://a.com"_s;
        Clip files;
        files.kind = ClipKind::Files;
        files.text = u"C:\\a.txt\nC:\\b.txt"_s;
        Clip moreFiles;
        moreFiles.kind = ClipKind::Files;
        moreFiles.text = u"c:\\A.txt\nC:\\c.txt"_s;
        Clip image;
        image.kind = ClipKind::Image;
        const auto whole = [](const Clip& c) { return c.text + u"!"_s; };

        const Clip* mixed[] {&link, &a, &files, &image};
        ClipBundle b = bundle(mixed, whole, u", ");
        QCOMPARE(b.text, u"https://a.com!, 甲!, C:\\a.txt\r\nC:\\b.txt"_s);
        QVERIFY(b.files.isEmpty());
        QCOMPARE(b.skippedImages, 1);

        const Clip* onlyFiles[] {&files, &moreFiles};
        b = bundle(onlyFiles, whole, u"\n");
        QCOMPARE(b.files, (QStringList {u"C:\\a.txt"_s, u"C:\\b.txt"_s, u"C:\\c.txt"_s}));
        QVERIFY(b.text.isEmpty());

        const Clip* onlyImage[] {&image};
        b = bundle(onlyImage, whole, u"\n");
        QVERIFY(b.text.isEmpty() && b.files.isEmpty());
        QCOMPARE(b.skippedImages, 1);
    }
};

QTEST_GUILESS_MAIN(ClipboardTest)
#include "tst_clipboard.moc"
