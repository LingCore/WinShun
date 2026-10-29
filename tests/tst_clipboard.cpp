#include "ClipStore.h"

#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QFileInfo>
#include <QThread>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
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

// A database file's size and its pages, for messages.
QString fileState(const QString& file)
{
    QString state = QString::number(QFileInfo(file).size()) + u" bytes"_s;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"probe"_s);
        db.setDatabaseName(file);
        if (db.open()) {
            QSqlQuery query(db);
            for (const QString pragma : {u"auto_vacuum"_s, u"freelist_count"_s, u"page_count"_s}) {
                if (query.exec(u"PRAGMA "_s + pragma) && query.next())
                    state += u", "_s + pragma + u" "_s + query.value(0).toString();
            }
        }
    }
    QSqlDatabase::removeDatabase(u"probe"_s);
    return state;
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
    void closedHistoryReleasesMemoryAndKeepsPendingWrites()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString folder = dir.filePath(u"history"_s);
        ClipStore store(folder);
        QVERIFY(!store.isOpen());
        store.flush();
        store.close();
        QVERIFY(!QFileInfo::exists(folder)); // disabled startup creates no database or folders
        QVERIFY(store.open(kStart));
        const qint64 entry = store.add(text(u"关闭再开启，内容还在"_s), kStart);
        const qint64 group = store.addGroup(u"保留的分组"_s);
        store.setGroup(std::span(&entry, 1), group);
        store.setValue(u"preview"_s, u"open"_s);
        ClipCapture picture;
        picture.kind = ClipKind::Image;
        picture.imageData = QByteArray("\x89PNG-pending", 12);
        picture.hash = "screenshot";
        picture.width = 12;
        picture.height = 8;
        picture.screenshot = true;
        picture.project = "editable-project";
        const qint64 image = store.add(picture, kStart + 1);
        QVERIFY(store.imagePending(image));
        store.close(); // drains the queued PNG and project before the connection is released
        QVERIFY(!store.isOpen());
        QVERIFY(store.clips().empty());
        QVERIFY(store.groups().empty());
        QVERIFY(!store.imagePending(image));
        QVERIFY(QFileInfo::exists(store.imagePath(image)));
        QVERIFY(store.open(kStart + 2));
        QCOMPARE(store.clips().size(), std::size_t(2));
        QVERIFY(store.clip(entry));
        QCOMPARE(store.clip(entry)->group, group);
        QCOMPARE(store.group(group)->name, u"保留的分组"_s);
        QCOMPARE(store.value(u"preview"_s), u"open"_s);
        QCOMPARE(store.project(image), picture.project);
        QVERIFY(store.clip(image)->screenshot);
        QFile file(store.imagePath(image));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), picture.imageData);
        QVERIFY(store.add(text(u"新记录"_s), kStart + 3) > image);
        store.close();
        store.close(); // retiring an already closed history is harmless
    }

    void globalQuickPasteEntries()
    {
        QTemporaryDir dir;
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart));
        QCOMPARE(store.quickPasteEntry(1, false), 0);
        std::vector<qint64> ids;
        for (int i = 0; i < 12; ++i)
            ids.push_back(store.add(text(QString::number(i)), kStart + i));
        QCOMPARE(store.quickPasteEntry(0, false), 0);
        QCOMPARE(store.quickPasteEntry(10, false), 0);
        QCOMPARE(store.quickPasteEntry(1, false), ids[11]);
        QCOMPARE(store.quickPasteEntry(9, false), ids[3]);
        store.setGroup(std::span(&ids[0], 1), ClipStore::kPinned);
        QCOMPARE(store.quickPasteEntry(1, true), ids[0]);
        QCOMPARE(store.quickPasteEntry(2, true), ids[11]);
        QCOMPARE(store.quickPasteEntry(1, false), ids[11]);
        store.setHidden({{ids[11], store.clip(ids[11])->used}});
        QCOMPARE(store.quickPasteEntry(2, true), ids[10]);
        store.remove(std::span(&ids[10], 1));
        QCOMPARE(store.quickPasteEntry(2, true), ids[9]);
        store.touch(ids[1], kStart + 50);
        QCOMPARE(store.quickPasteEntry(1, false), ids[1]);
        QCOMPARE(store.quickPasteEntry(1, true), ids[0]);
        QCOMPARE(store.quickPasteEntry(2, true), ids[1]);
    }

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

    void pasteKeepsPlaceWhenAsked() // GitHub #122
    {
        QTemporaryDir dir;
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart));
        const qint64 a = store.add(text(u"甲"_s), kStart);
        const qint64 b = store.add(text(u"乙"_s), kStart + 1);
        const qint64 c = store.add(text(u"丙"_s), kStart + 2);
        QVERIFY(!store.keepsPlaceOnPaste()); // off: today's behaviour
        store.touch(a, kStart + 3);
        QCOMPARE(texts(store), (QStringList {u"甲"_s, u"丙"_s, u"乙"_s}));

        store.setKeepsPlaceOnPaste(true);
        store.touch(b, kStart + 4); // pasted from the middle: stays
        store.touch(c, kStart + 5);
        QCOMPARE(texts(store), (QStringList {u"甲"_s, u"丙"_s, u"乙"_s}));
        QCOMPARE(store.clip(b)->used, kStart + 1);
        // Pinned or grouped entries too; copying the content again is a new copy.
        store.setGroup(std::span(&b, 1), ClipStore::kPinned);
        store.touch(b, kStart + 6);
        QCOMPARE(texts(store), (QStringList {u"甲"_s, u"丙"_s, u"乙"_s}));
        QCOMPARE(store.add(text(u"乙"_s), kStart + 7), b);
        QCOMPARE(texts(store), (QStringList {u"乙"_s, u"甲"_s, u"丙"_s}));

        // Pasted is set either way; `used` only when the entry moves.
        QCOMPARE(store.clip(b)->pasted, kStart + 6);
        QCOMPARE(store.clip(c)->pasted, kStart + 5);
        QCOMPARE(store.clip(a)->pasted, kStart + 3);
        QCOMPARE(store.clip(c)->used, kStart + 2);

        // The same order after a restart (the list is sorted by `used`).
        store.touch(a, kStart + 8);
        store.close();
        QVERIFY(store.open(kStart + 9));
        QCOMPARE(texts(store), (QStringList {u"乙"_s, u"甲"_s, u"丙"_s}));
        QCOMPARE(store.clip(a)->pasted, kStart + 8);
        QCOMPARE(store.clip(c)->pasted, kStart + 5);
        QCOMPARE(store.clip(c)->used, kStart + 2);
        store.setKeepsPlaceOnPaste(false);
        store.touch(c, kStart + 10);
        QCOMPARE(texts(store), (QStringList {u"丙"_s, u"乙"_s, u"甲"_s}));
        QCOMPARE(store.clip(c)->pasted, kStart + 10);
        QCOMPARE(store.clip(c)->used, kStart + 10);
    }

    // Expiry goes by the later of the last copy and the last paste; the count
    // limit still keeps the first ones listed.
    void pastedEntriesDoNotExpire()
    {
        QTemporaryDir dir;
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart));
        store.setKeepsPlaceOnPaste(true);
        const qint64 old = store.add(text(u"旧"_s), kStart);
        const qint64 stale = store.add(text(u"更旧"_s), kStart);
        store.setLimits({100, 30}, kStart + 20 * kDay); // both are 20 days old
        QVERIFY(store.clip(old) && store.clip(stale));
        store.touch(old, kStart + 35 * kDay); // pasted every few weeks, never copied again
        const qint64 fresh = store.add(text(u"新"_s), kStart + 39 * kDay);
        store.setLimits({100, 30}, kStart + 40 * kDay);
        QVERIFY(store.clip(old));
        QVERIFY(!store.clip(stale));
        QVERIFY(store.clip(fresh));
        QCOMPARE(store.clip(old)->used, kStart); // still where it was, as old as ever
        QCOMPARE(texts(store).first(), u"新"_s);
        // Past 30 days since the paste too: gone, kept or not.
        store.setLimits({100, 30}, kStart + 66 * kDay);
        QVERIFY(!store.clip(old));
        // The count limit counts in list order, whatever was pasted.
        store.setLimits({2, 0}, kStart + 67 * kDay);
        const qint64 other = store.add(text(u"又一条"_s), kStart + 66 * kDay);
        store.touch(fresh, kStart + 67 * kDay); // pasted last, but listed second
        store.setLimits({1, 0}, kStart + 67 * kDay);
        QVERIFY(store.clip(other));
        QVERIFY(!store.clip(fresh));
    }

    // A history from before `pasted`: the column is added, nothing is lost;
    // an older Win顺 opening the upgraded file still reads it.
    void pastedColumnAddedToAnOldHistory()
    {
        QTemporaryDir dir;
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart));
            QVERIFY(store.add(text(u"旧记录"_s), kStart) > 0);
        }
        const auto sql = [&](const QString& statement) {
            bool ok = false;
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"old3"_s);
                db.setDatabaseName(dir.filePath(u"clipboard.db"_s));
                ok = db.open() && QSqlQuery(db).exec(statement);
                db.close();
            }
            QSqlDatabase::removeDatabase(u"old3"_s);
            return ok;
        };
        QVERIFY(sql(u"ALTER TABLE clips DROP COLUMN pasted"_s)); // as a file from before
        qint64 id = 0;
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart + 1));
            QCOMPARE(store.clips().size(), std::size_t(1));
            id = store.clips().front().id;
            QCOMPARE(store.clip(id)->pasted, qint64(0));
            store.touch(id, kStart + 2);
        }
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart + 3));
            QCOMPARE(store.clip(id)->pasted, kStart + 2);
        }
        // What an older version does with the file: its own columns, no `pasted`.
        QVERIFY(sql(u"INSERT INTO clips (id, kind, text, hash, created, used) VALUES (99, 0, 'x', 'h', %1, %1)"_s.arg(kStart + 3)));
        QVERIFY(sql(u"UPDATE clips SET used = %1 WHERE id = 99"_s.arg(kStart + 3)));
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart + 4));
        QVERIFY(store.clip(99));
        QCOMPARE(store.clip(99)->pasted, qint64(0));
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
        // No limit on the number (GitHub #32): only the days count.
        store.setLimits({0, 30}, kStart + 4000 * kDay);
        for (int i = 0; i < 400; ++i)
            store.add(text(u"n%1"_s.arg(i)), kStart + 4000 * kDay + i);
        QCOMPARE(store.clips().size(), std::size_t(401));
        store.setLimits({0, 30}, kStart + 4100 * kDay);
        QCOMPARE(texts(store), (QStringList {u"1"_s}));
    }

    // The limits must be set before open(): open() prunes by what is set, and
    // the defaults (300 entries, 30 days) would drop the history of anyone who
    // chose "no limit" at every start.
    void limitsBeforeOpenKeepTheHistory()
    {
        QTemporaryDir dir;
        const qint64 now = kStart + 200 * kDay;
        {
            ClipStore store(dir.path());
            store.setLimits({0, 0}, now);
            QVERIFY(store.open(now));
            for (int i = 0; i < 3; ++i) // older than 30 days
                store.add(text(u"old%1"_s.arg(i)), kStart + i);
            for (int i = 0; i < 400; ++i)
                store.add(text(u"n%1"_s.arg(i)), now - 1000 + i);
            QCOMPARE(store.clips().size(), std::size_t(403));
            store.flush();
        }
        { // no limit at all: not one is lost
            ClipStore store(dir.path());
            store.setLimits({0, 0}, now);
            QVERIFY(store.open(now));
            QCOMPARE(store.clips().size(), std::size_t(403));
        }
        { // no limit on the number, 30 days: only the old three go
            ClipStore store(dir.path());
            store.setLimits({0, 30}, now);
            QVERIFY(store.open(now));
            QCOMPARE(store.clips().size(), std::size_t(400));
        }
        { // the defaults still prune: 300 entries
            ClipStore store(dir.path());
            QVERIFY(store.open(now));
            QCOMPARE(store.clips().size(), std::size_t(300));
        }
        { // and the old ones are gone for good
            ClipStore store(dir.path());
            store.setLimits({0, 0}, now);
            QVERIFY(store.open(now));
            QCOMPARE(store.clips().size(), std::size_t(300));
        }
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
        image.imageData = QByteArray("\x89PNG", 4);
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

    // "固定" listed first in its own order (pinnedFirst), a group in its own
    // order, moved by the user; pasting or copying again does not move them.
    void pinnedFirstAndOwnOrder()
    {
        QTemporaryDir dir;
        using Category = ClipFilter::Category;
        qint64 a = 0, b = 0, c = 0;
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart));
            a = store.add(text(u"甲"_s), kStart);
            b = store.add(text(u"乙"_s), kStart + 1);
            c = store.add(text(u"丙"_s), kStart + 2);
            store.add(text(u"丁"_s), kStart + 3);
            store.add(text(u"https://example.com"_s), kStart + 4);
            // Pinned one by one: each at the top of "固定".
            store.setGroup(std::span(&a, 1), ClipStore::kPinned);
            store.setGroup(std::span(&c, 1), ClipStore::kPinned);
            QCOMPARE(texts(store, store.find({})), (QStringList {u"https://example.com"_s, u"丁"_s, u"丙"_s, u"乙"_s, u"甲"_s}));
            ClipFilter first;
            first.pinnedFirst = true;
            QCOMPARE(texts(store, store.find(first)), (QStringList {u"丙"_s, u"甲"_s, u"https://example.com"_s, u"丁"_s, u"乙"_s}));
            // Pasting one keeps it in its place among the pinned.
            store.touch(a, kStart + 5);
            QCOMPARE(texts(store, store.find(first)), (QStringList {u"丙"_s, u"甲"_s, u"https://example.com"_s, u"丁"_s, u"乙"_s}));
            QCOMPARE(texts(store, store.find({Category::Group, ClipStore::kPinned, {}})), (QStringList {u"丙"_s, u"甲"_s}));
            // Moved down below 甲, then a third one pinned on top.
            store.moveInGroup(c, a);
            QCOMPARE(texts(store, store.find({Category::Group, ClipStore::kPinned, {}})), (QStringList {u"甲"_s, u"丙"_s}));
            store.setGroup(std::span(&b, 1), ClipStore::kPinned);
            QCOMPARE(store.groupOrder(ClipStore::kPinned), (std::vector<qint64> {b, a, c}));
            store.moveInGroup(c, b); // up, to the top
            QCOMPARE(store.groupOrder(ClipStore::kPinned), (std::vector<qint64> {c, b, a}));
            store.moveInGroup(c, a); // down, to the end
            QCOMPARE(store.groupOrder(ClipStore::kPinned), (std::vector<qint64> {b, a, c}));
            // A category: its pinned ones first; a search keeps them first too.
            first.category = Category::Links;
            QCOMPARE(texts(store, store.find(first)), (QStringList {u"https://example.com"_s}));
            first.category = Category::All;
            first.query = u"丙"_s;
            QCOMPARE(texts(store, store.find(first)), (QStringList {u"丙"_s}));
            // Out of the group and back: at its top again.
            store.setGroup(std::span(&c, 1), 0);
            store.setGroup(std::span(&c, 1), ClipStore::kPinned);
            QCOMPARE(store.groupOrder(ClipStore::kPinned), (std::vector<qint64> {c, b, a}));
        }
        {
            ClipStore store(dir.path()); // the order is kept
            QVERIFY(store.open(kStart + 6));
            QCOMPARE(store.groupOrder(ClipStore::kPinned), (std::vector<qint64> {c, b, a}));
        }
        // A history from before the order was kept: its groups' entries keep
        // the order they were listed in, newest first.
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"old"_s);
            db.setDatabaseName(dir.path() + u"/clipboard.db"_s);
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec(u"ALTER TABLE clips DROP COLUMN rank"_s));
            db.close();
        }
        QSqlDatabase::removeDatabase(u"old"_s);
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart + 7));
        QCOMPARE(store.groupOrder(ClipStore::kPinned), (std::vector<qint64> {a, c, b})); // a pasted last
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

    void removedEntriesFreeTheDisk()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(u"clipboard.db"_s);
        const auto fill = [](ClipStore& store) {
            std::vector<qint64> ids;
            for (int i = 0; i < 20; ++i)
                ids.push_back(store.add(text(QString(100'000, QChar(u'a' + i))), kStart + i));
            return ids;
        };
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart));
            store.remove(fill(store));
            store.flush();
        }
        QVERIFY2(QFileInfo(file).size() < 256 * 1024, qPrintable(fileState(file)));

        // A file made before it gave back its free pages: turned into one
        // that does when opened.
        QFile::remove(file);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"old"_s);
            db.setDatabaseName(file);
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec(u"CREATE TABLE junk (data BLOB)"_s));
            QVERIFY(query.exec(u"INSERT INTO junk VALUES (zeroblob(4000000))"_s));
            QVERIFY(query.exec(u"DROP TABLE junk"_s));
            db.close();
        }
        QSqlDatabase::removeDatabase(u"old"_s);
        QVERIFY(QFileInfo(file).size() > 3'000'000);
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart));
            store.flush();
        }
        QVERIFY2(QFileInfo(file).size() < 256 * 1024, qPrintable(fileState(file)));
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart));
            store.remove(fill(store));
            store.flush();
        }
        QVERIFY2(QFileInfo(file).size() < 256 * 1024, qPrintable(fileState(file)));
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
            image.imageData = "png";
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

    // The words in a picture (GitHub #9): found by a search, read once.
    void picturesWords()
    {
        QTemporaryDir dir;
        // A history from before: no column for them yet.
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"old"_s);
            db.setDatabaseName(dir.filePath(u"clipboard.db"_s));
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec(u"CREATE TABLE clips (id INTEGER PRIMARY KEY AUTOINCREMENT, kind INTEGER NOT NULL, "
                               u"text TEXT NOT NULL DEFAULT '', length INTEGER NOT NULL DEFAULT 0, html BLOB, rtf BLOB, "
                               u"hash BLOB NOT NULL, created INTEGER NOT NULL, used INTEGER NOT NULL, "
                               u"source TEXT NOT NULL DEFAULT '', sourcePath TEXT NOT NULL DEFAULT '', "
                               u"groupId INTEGER NOT NULL DEFAULT 0, width INTEGER NOT NULL DEFAULT 0, "
                               u"height INTEGER NOT NULL DEFAULT 0, bytes INTEGER NOT NULL DEFAULT 0)"_s));
            QVERIFY(query.exec(u"INSERT INTO clips (id, kind, hash, created, used) VALUES (5, 4, 'old', %1, %1)"_s.arg(kStart - 1)));
        }
        QSqlDatabase::removeDatabase(u"old"_s);
        QVERIFY(QDir(dir.path()).mkpath(u"images"_s));
        QFile file(dir.filePath(u"images/5.png"_s)); // else cleared away as no one's
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();

        qint64 id = 0;
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart));
            QCOMPARE(store.unreadPictures(), std::vector<qint64> {5});
            ClipCapture image;
            image.kind = ClipKind::Image;
            image.imageData = "png";
            image.hash = "a";
            id = store.add(image, kStart);
            QCOMPARE(store.unreadPictures(), std::vector<qint64> {5}); // its file is still being written
            store.flush();
            QCOMPARE(store.unreadPictures(), (std::vector<qint64> {id, 5}));
            store.setPictureText(id, u"第三季度会议纪要 Q3"_s);
            QCOMPARE(store.unreadPictures(true), (std::vector<qint64> {5, id})); // unread first, then the read ones
            store.setPictureText(5, QString()); // read, nothing in it
            QVERIFY(store.unreadPictures().empty());
            QCOMPARE(store.unreadPictures(true), (std::vector<qint64> {id, 5})); // all, to read again
            ClipFilter filter;
            filter.query = u"会议"_s;
            QCOMPARE(store.find(filter).size(), std::size_t(1));
            filter.query = u"hyjy"_s; // by pinyin too
            QCOMPARE(store.find(filter).size(), std::size_t(1));
        }
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart));
        QVERIFY(store.unreadPictures().empty()); // not read again
        QCOMPARE(store.clip(id)->ocr, u"第三季度会议纪要 Q3"_s);
        // Removed and put back: with its words.
        std::vector<ClipRecord> removed = store.remove(std::span(&id, 1));
        store.restore(std::move(removed));
        QCOMPARE(store.clip(id)->ocr, u"第三季度会议纪要 Q3"_s);
        QVERIFY(store.clip(id)->read);
        ClipFilter filter;
        filter.query = u"Q3"_s;
        QCOMPARE(store.find(filter).size(), std::size_t(1));
    }

    // The screenshot history (jt): pictures Win顺's capture took, marked so;
    // a history from before the mark gets it from the source they were saved
    // with (in either language), and only pictures from WinShun.exe do.
    void screenshotHistory()
    {
        QTemporaryDir dir;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"old"_s);
            db.setDatabaseName(dir.filePath(u"clipboard.db"_s));
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec(u"CREATE TABLE clips (id INTEGER PRIMARY KEY AUTOINCREMENT, kind INTEGER NOT NULL, "
                               u"text TEXT NOT NULL DEFAULT '', length INTEGER NOT NULL DEFAULT 0, html BLOB, rtf BLOB, "
                               u"hash BLOB NOT NULL, created INTEGER NOT NULL, used INTEGER NOT NULL, "
                               u"source TEXT NOT NULL DEFAULT '', sourcePath TEXT NOT NULL DEFAULT '', "
                               u"groupId INTEGER NOT NULL DEFAULT 0, width INTEGER NOT NULL DEFAULT 0, "
                               u"height INTEGER NOT NULL DEFAULT 0, bytes INTEGER NOT NULL DEFAULT 0)"_s));
            const auto insert = [&](int id, int kind, const QString& hash, qint64 created, qint64 used, const QString& source,
                                    const QString& path) {
                query.prepare(u"INSERT INTO clips (id, kind, text, hash, created, used, source, sourcePath) "
                              u"VALUES (?, ?, ?, ?, ?, ?, ?, ?)"_s);
                query.addBindValue(id);
                query.addBindValue(kind);
                query.addBindValue(kind == 0 ? source : u""_s);
                query.addBindValue(hash.toUtf8());
                query.addBindValue(created);
                query.addBindValue(used);
                query.addBindValue(source);
                query.addBindValue(path);
                const bool ok = query.exec();
                if (!ok)
                    qWarning().noquote() << query.lastError().text();
                return ok;
            };
            QVERIFY(insert(5, 4, u"p5"_s, kStart - 10, kStart - 10, u"Win顺 截图"_s, u"C:\\Program Files\\WinShun\\WinShun.exe"_s));
            // Taken earlier, pasted later: still listed by when it was taken.
            QVERIFY(insert(6, 4, u"p6"_s, kStart - 20, kStart - 1, u"WinShun screenshot"_s, u"D:\\apps\\winshun.exe"_s));
            QVERIFY(insert(7, 4, u"p7"_s, kStart - 5, kStart - 5, u"微信"_s, u"C:\\WeChat\\WeChat.exe"_s));
            QVERIFY(insert(8, 4, u"p8"_s, kStart - 4, kStart - 4, u"Win顺 截图"_s, u"C:\\Other\\Snipper.exe"_s)); // not ours
            QVERIFY(insert(9, 0, u"p9"_s, kStart - 3, kStart - 3, u"Win顺 截图"_s, u"C:\\WinShun\\WinShun.exe"_s)); // text
        }
        QSqlDatabase::removeDatabase(u"old"_s);
        QVERIFY(QDir(dir.path()).mkpath(u"images"_s));
        for (const int id : {5, 6, 7, 8}) { // else cleared away as no one's
            QFile file(dir.filePath(u"images/%1.png"_s.arg(id)));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        ClipFilter shots;
        shots.screenshots = true;
        const auto ids = [](const ClipStore& store, const std::vector<int>& rows) {
            std::vector<qint64> list;
            for (const int row : rows)
                list.push_back(store.clips()[static_cast<std::size_t>(row)].id);
            return list;
        };
        qint64 taken = 0;
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart));
            QVERIFY(store.clip(5)->screenshot);
            QVERIFY(store.clip(6)->screenshot);
            QVERIFY(!store.clip(7)->screenshot);
            QVERIFY(!store.clip(8)->screenshot);
            QVERIFY(!store.clip(9)->screenshot);
            QCOMPARE(ids(store, store.find(shots)), (std::vector<qint64> {5, 6}));

            ClipCapture image;
            image.kind = ClipKind::Image;
            image.imageData = "png";
            image.hash = "new";
            image.screenshot = true;
            taken = store.add(image, kStart + 1);
            ClipCapture other = image; // a picture copied in a program
            other.hash = "other";
            other.screenshot = false;
            store.add(other, kStart + 2);
            // The same pixels copied again from a program: still a screenshot,
            // at the top of the clipboard history, in its place in this list.
            image.screenshot = false;
            QCOMPARE(store.add(image, kStart + 3), taken);
            QVERIFY(store.clip(taken)->screenshot);
            store.touch(6, kStart + 4);
            QCOMPARE(ids(store, store.find(shots)), (std::vector<qint64> {taken, 5, 6}));
            // Words: in what was read in them, by pinyin too; nothing else counts.
            store.flush();
            store.setPictureText(5, u"季度报表 Q3"_s);
            store.setPictureText(6, u"会议纪要"_s);
            shots.query = u"jdbb"_s;
            QCOMPARE(ids(store, store.find(shots)), (std::vector<qint64> {5}));
            shots.query = u"会议"_s;
            QCOMPARE(ids(store, store.find(shots)), (std::vector<qint64> {6}));
            shots.query = u"微信"_s; // a source is no word in the picture
            QVERIFY(store.find(shots).empty());
            shots.query.clear();
        }
        ClipStore store(dir.path()); // kept, and not migrated again
        QVERIFY(store.open(kStart + 5));
        QCOMPARE(ids(store, store.find(shots)), (std::vector<qint64> {taken, 5, 6}));
        // Removed and put back: still one.
        std::vector<ClipRecord> removed = store.remove(std::span(&taken, 1));
        QCOMPARE(ids(store, store.find(shots)), (std::vector<qint64> {5, 6}));
        store.restore(std::move(removed));
        QVERIFY(store.clip(taken)->screenshot);
        QCOMPARE(ids(store, store.find(shots)), (std::vector<qint64> {taken, 5, 6}));
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
            image.imageData = "png";
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
            image.imageData = "png";
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
        image.imageData = "png";
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

    // A picture's project (annotation, stage 4a): a file beside the PNG that
    // lives and dies with the entry; ClipStore keeps its bytes as they are.
    void projects()
    {
        const auto picture = [](const QByteArray& hash, const QByteArray& project) {
            ClipCapture image;
            image.kind = ClipKind::Image;
            image.imageData = "png";
            image.hash = hash;
            image.screenshot = true;
            image.project = project;
            return image;
        };
        const auto sqlCount = [](const QString& file, const QString& where) {
            int n = -1;
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"count"_s);
                db.setDatabaseName(file);
                if (db.open()) {
                    QSqlQuery query(db);
                    if (query.exec(u"SELECT COUNT(*) FROM clips WHERE "_s + where) && query.next())
                        n = query.value(0).toInt();
                }
            }
            QSqlDatabase::removeDatabase(u"count"_s);
            return n;
        };
        const QByteArray bytes("PK-project-bytes\0with a zero", 28);
        QTemporaryDir dir;
        const QString dbFile = dir.filePath(u"clipboard.db"_s);
        qint64 a = 0;
        qint64 plain = 0;
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart));
            store.setLimits({3, 0}, kStart);

            // Added with a project: the file is there, the flag set, the bytes read back.
            a = store.add(picture("a", bytes), kStart);
            QVERIFY(store.clip(a)->project);
            QCOMPARE(store.project(a), bytes);
            QVERIFY(QFile::exists(store.projectPath(a)));
            QVERIFY(store.projectPath(a).endsWith(u"projects\\%1.wsedit"_s.arg(a)));
            QVERIFY(QFile::exists(store.imagePath(a)));
            // Without: no flag, no file, nothing to read.
            plain = store.add(picture("plain", {}), kStart + 1);
            QVERIFY(!store.clip(plain)->project);
            QVERIFY(store.project(plain).isEmpty());
            QVERIFY(!QFile::exists(store.projectPath(plain)));
            // A text has none either.
            ClipCapture words = text(u"甲"_s);
            words.project = bytes;
            QVERIFY(!store.clip(store.add(words, kStart + 2))->project);

            // The same pixels again: a new project replaces the old one ...
            QCOMPARE(store.add(picture("a", "second"), kStart + 3), a);
            QCOMPARE(store.project(a), QByteArray("second"));
            QCOMPARE(store.clips().front().id, a);
            // ... none keeps it ...
            QCOMPARE(store.add(picture("a", {}), kStart + 4), a);
            QCOMPARE(store.project(a), QByteArray("second"));
            QVERIFY(store.clip(a)->project);
            // ... and an entry without one gets one.
            QCOMPARE(store.add(picture("plain", "late"), kStart + 5), plain);
            QVERIFY(store.clip(plain)->project);
            QCOMPARE(store.project(plain), QByteArray("late"));
            store.flush();
            QCOMPARE(sqlCount(dbFile, u"project = 1"_s), 2);

            // Removed: the file stays for undo, comes back with the column.
            std::vector<ClipRecord> removed = store.remove(std::span(&a, 1));
            QVERIFY(removed.front().clip.project);
            QVERIFY(!store.clip(a));
            QVERIFY(QFile::exists(store.projectPath(a)));
            store.restore(std::move(removed));
            QVERIFY(store.clip(a)->project);
            QCOMPARE(store.project(a), QByteArray("second"));
            store.flush();
            QCOMPARE(sqlCount(dbFile, u"project = 1"_s), 2);

            // Removed and discarded: gone.
            removed = store.remove(std::span(&plain, 1));
            QVERIFY(QFile::exists(store.projectPath(plain)));
            store.discardImages(std::span(&plain, 1));
            store.flush();
            QVERIFY(!QFile::exists(store.projectPath(plain)));
            QVERIFY(!QFile::exists(store.imagePath(plain)));
        }

        // Orphans go at open(): a name that is no id, an id nobody has.
        const QString folder = dir.filePath(u"projects"_s);
        QVERIFY(QDir().mkpath(folder));
        const auto touch = [&](const QString& name) {
            QFile file(QDir(folder).filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
        };
        touch(u"notanumber.wsedit"_s);
        touch(u"99999.wsedit"_s);
        touch(u"readme.txt"_s); // not ours: stays
        qint64 kept = 0;
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart + 10));
            store.flush();
            QVERIFY(!QFile::exists(QDir(folder).filePath(u"notanumber.wsedit"_s)));
            QVERIFY(!QFile::exists(QDir(folder).filePath(u"99999.wsedit"_s)));
            QVERIFY(QFile::exists(QDir(folder).filePath(u"readme.txt"_s)));
            kept = a;
            QVERIFY(QFile::exists(store.projectPath(kept))); // listed, so it stays
            QVERIFY(store.clip(kept)->project);

            // The file is lost (a crash between the two writes): no project, the
            // column goes back to 0, also for the next time.
            QVERIFY(QFile::remove(store.projectPath(kept)));
            QVERIFY(store.project(kept).isEmpty());
            store.flush();
            QCOMPARE(sqlCount(dbFile, u"project = 1"_s), 0);
        }
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart + 11));
            QVERIFY(!store.clip(kept)->project);
            // Dropped by the limits: the project goes with the row.
            store.setLimits({3, 0}, kStart + 11);
            const qint64 b = store.add(picture("b", "bee"), kStart + 12);
            QVERIFY(QFile::exists(store.projectPath(b)) || store.project(b) == QByteArray("bee"));
            store.setLimits({1, 0}, kStart + 13);
            store.flush();
            QCOMPARE(store.clips().size(), std::size_t(1));
            QVERIFY(QFile::exists(store.projectPath(b)));
            store.add(picture("c", "sea"), kStart + 14);
            store.flush();
            QVERIFY(!store.clip(b));
            QVERIFY(!QFile::exists(store.projectPath(b))); // pruned
            // Clearing the history is the same.
            const qint64 c = store.clips().front().id;
            QVERIFY(QFile::exists(store.projectPath(c)));
            QCOMPARE(store.clearHistory(), 1);
            store.flush();
            QVERIFY(!QFile::exists(store.projectPath(c)));
            QVERIFY(!QFile::exists(store.imagePath(c)));
        }
    }

    // 编辑 then Enter without a change copies the original entry again; the
    // watcher sees the picture come back and adds it, a duplicate of the same
    // pixels: the entry moves to the top and its PNG file is not written
    // again (the bytes stay the ones that were stored), its project not touched.
    void copiedAgainKeepsThePicture()
    {
        QTemporaryDir dir;
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart));
        ClipCapture first;
        first.kind = ClipKind::Image;
        first.imageData = "first-png-bytes";
        first.hash = "same";
        first.screenshot = true;
        first.project = "the-project";
        const qint64 id = store.add(first, kStart);
        store.flush();
        const QString path = store.imagePath(id);
        QVERIFY(QFile::exists(path));
        const QDateTime written = QFileInfo(path).lastModified();
        QThread::msleep(30);
        ClipCapture other = text(u"between"_s);
        store.add(other, kStart + 1);
        ClipCapture again;
        again.kind = ClipKind::Image;
        again.imageData = "re-encoded-differently";
        again.hash = "same";
        const qint64 back = store.add(again, kStart + 2);
        store.flush();
        QCOMPARE(back, id);
        QCOMPARE(store.clips().front().id, id); // moved to the top
        QCOMPARE(store.clips().size(), std::size_t(2));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("first-png-bytes"));
        file.close();
        QCOMPARE(QFileInfo(path).lastModified(), written);
        QVERIFY(store.clip(id)->screenshot);
        QCOMPARE(store.project(id), QByteArray("the-project"));
    }

    // An equation or an embedded object (GitHub #58): a picture, with the
    // formats it was copied in to paste it back from, kept through undo, the
    // same picture copied again and a restart; found by its own text (Word's
    // linear form), which reading the picture does not replace.
    void objectFormats()
    {
        const QString linear = u"x=(-b±√(b^2-4ac))/2a"_s;
        const QList<ClipFormat> formats {
            {u"MathML"_s, QByteArray("<mml:math/>")},
            {u"Rich Text Format"_s, QByteArray("{\\rtf1{\\mmath{\\*\\moMathPara x}}}")},
            {u"CF_UNICODETEXT"_s, QByteArray(reinterpret_cast<const char*>(linear.utf16()), (linear.size() + 1) * 2)},
            {u"CF_ENHMETAFILE"_s, QByteArray(100, '\x01')},
            {u"Embed Source"_s, QByteArray(4096, '\0')},
        };
        const QByteArray packed = packClipFormats(formats);
        const QList<ClipFormat> unpacked = unpackClipFormats(packed);
        QCOMPARE(unpacked.size(), formats.size());
        for (qsizetype i = 0; i < formats.size(); ++i) {
            QCOMPARE(unpacked[i].name, formats[i].name);
            QCOMPARE(unpacked[i].data, formats[i].data);
        }
        QVERIFY(packClipFormats({}).isEmpty());
        QVERIFY(unpackClipFormats({}).isEmpty());
        QVERIFY(unpackClipFormats("not packed formats").isEmpty());
        QVERIFY(unpackClipFormats(packed.chopped(3)).isEmpty()); // cut short

        const auto equation = [&linear](const QByteArray& kept) {
            ClipCapture c;
            c.kind = ClipKind::Image;
            c.imageData = "png";
            c.hash = "equation";
            c.formats = kept;
            if (!kept.isEmpty())
                c.text = linear; // the watcher keeps it, as the program gave it
            return c;
        };
        const auto found = [](const ClipStore& store, const QString& query) {
            ClipFilter filter;
            filter.query = query;
            return store.find(filter).size();
        };
        QTemporaryDir dir;
        qint64 id = 0;
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart));
            id = store.add(equation(packed), kStart);
            QVERIFY(store.clip(id)->formats);
            QCOMPARE(store.payload(id).formats, packed);
            // Found by its text, also once its picture was read: the words
            // read are not what it is found by.
            QCOMPARE(found(store, u"b^2"_s), std::size_t(1));
            store.flush();
            store.setPictureText(id, u"unrelated words"_s);
            QCOMPARE(store.clip(id)->text, linear);
            QCOMPARE(found(store, u"b^2"_s), std::size_t(1));
            QCOMPARE(found(store, u"unrelated"_s), std::size_t(0));
            QCOMPARE(store.payload(id).text, linear);
            // The same picture from somewhere else, without them: kept.
            QCOMPARE(store.add(equation({}), kStart + 1), id);
            QVERIFY(store.clip(id)->formats);
            QCOMPARE(store.payload(id).formats, packed);
            // Removed and put back.
            const qint64 ids[] {id};
            std::vector<ClipRecord> removed = store.remove(ids);
            QCOMPARE(removed.size(), std::size_t(1));
            QCOMPARE(removed.front().formats, packed);
            store.restore(std::move(removed));
            QVERIFY(store.clip(id)->formats);
            QCOMPARE(store.payload(id).formats, packed);
            QCOMPARE(found(store, u"b^2"_s), std::size_t(1));
            // Text has none.
            const qint64 plain = store.add(text(u"x=1"_s), kStart + 2);
            QVERIFY(!store.clip(plain)->formats);
            QVERIFY(store.payload(plain).formats.isEmpty());
        }
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart + 3));
        QVERIFY(store.clip(id)->formats);
        QCOMPARE(store.payload(id).formats, packed);
        QCOMPARE(store.payload(id).imagePath, store.imagePath(id));
        QCOMPARE(found(store, u"b^2"_s), std::size_t(1)); // its text, read back from the disk
        QCOMPARE(found(store, u"unrelated"_s), std::size_t(0));
        // Copied again with formats: the new ones.
        const QByteArray newer = packClipFormats({{u"MathML"_s, QByteArray("<mml:math>2</mml:math>")}});
        QCOMPARE(store.add(equation(newer), kStart + 4), id);
        QCOMPARE(store.payload(id).formats, newer);
    }

    // A history from before projects (and kept formats): the columns are
    // added, nothing is lost.
    void projectColumnAddedToAnOldHistory()
    {
        QTemporaryDir dir;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"old2"_s);
            db.setDatabaseName(dir.filePath(u"clipboard.db"_s));
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec(u"CREATE TABLE clips (id INTEGER PRIMARY KEY AUTOINCREMENT, kind INTEGER NOT NULL, "
                               u"text TEXT NOT NULL DEFAULT '', length INTEGER NOT NULL DEFAULT 0, html BLOB, rtf BLOB, "
                               u"hash BLOB NOT NULL, created INTEGER NOT NULL, used INTEGER NOT NULL, "
                               u"source TEXT NOT NULL DEFAULT '', sourcePath TEXT NOT NULL DEFAULT '', "
                               u"groupId INTEGER NOT NULL DEFAULT 0, width INTEGER NOT NULL DEFAULT 0, "
                               u"height INTEGER NOT NULL DEFAULT 0, bytes INTEGER NOT NULL DEFAULT 0, ocr TEXT, "
                               u"rank INTEGER NOT NULL DEFAULT 0, screenshot INTEGER NOT NULL DEFAULT 0)"_s));
            QVERIFY(query.exec(u"INSERT INTO clips (id, kind, text, hash, created, used) "
                               u"VALUES (3, 0, 'old', 'h', %1, %1)"_s.arg(kStart)));
        }
        QSqlDatabase::removeDatabase(u"old2"_s);
        qint64 picture = 0;
        {
            ClipStore store(dir.path());
            QVERIFY(store.open(kStart));
            QCOMPARE(store.clips().size(), std::size_t(1));
            QVERIFY(!store.clip(3)->project);
            QVERIFY(!store.clip(3)->formats);
            ClipCapture image;
            image.kind = ClipKind::Image;
            image.imageData = "png";
            image.hash = "p";
            image.project = "proj";
            image.formats = packClipFormats({{u"Embedded Object"_s, QByteArray("storage")}});
            picture = store.add(image, kStart + 1);
            QCOMPARE(store.project(picture), QByteArray("proj"));
        }
        ClipStore store(dir.path()); // and the columns are there the second time
        QVERIFY(store.open(kStart + 2));
        QVERIFY(store.clip(picture)->project);
        QCOMPARE(store.project(picture), QByteArray("proj"));
        QVERIFY(store.clip(picture)->formats);
        QCOMPARE(unpackClipFormats(store.payload(picture).formats).front().data, QByteArray("storage"));
        QCOMPARE(store.payload(3).text, u"old"_s);
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
