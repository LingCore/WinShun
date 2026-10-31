// Importing clipboard history: Ditto's database, text and CSV files, and the
// store's batch import under them. Also ClipImage, which they share with the
// watcher: the picture of a metafile.
#include "ClipImage.h"
#include "ClipImport.h"
#include "ClipImportJob.h"
#include "ClipStore.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>

using namespace ws;
using namespace ws::clipimport;
using namespace Qt::StringLiterals;

namespace {

constexpr qint64 kStart = 1'790'000'000'000; // some time in 2026
constexpr qint64 kDay = 24ll * 60 * 60 * 1000;

QByteArray utf16(const QString& text, bool terminator = true)
{
    QByteArray bytes(reinterpret_cast<const char*>(text.utf16()), text.size() * 2);
    if (terminator)
        bytes.append(QByteArray(2, '\0'));
    return bytes;
}

QByteArray dibOf(const QImage& image)
{
    QByteArray bmp;
    QBuffer buffer(&bmp);
    buffer.open(QIODevice::WriteOnly);
    image.convertToFormat(QImage::Format_RGB888).save(&buffer, "BMP");
    return bmp.mid(14); // a DIB is a BMP file without its file header
}

QByteArray dropFiles(const QStringList& files)
{
    QByteArray drop(20, '\0');
    drop[0] = 20; // where the names start
    drop[16] = 1; // wide characters
    for (const QString& file : files)
        drop += utf16(file);
    drop += QByteArray(2, '\0');
    return drop;
}

// A database made by Ditto itself (3.25.113.0, run once on an empty clipboard:
// data/ditto-empty.db, with its tables, indexes and triggers), the rows put in
// the way its code does (src/Clip.cpp AddToMainTable and the Data table).
class DittoDb {
public:
    explicit DittoDb(const QString& file)
        : m_name(u"ditto-fixture-"_s + file)
    {
        QVERIFY2(QFile::copy(QStringLiteral(WINSHUN_TEST_DATA "/ditto-empty.db"), file), "cannot copy the fixture");
        QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner);
        QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, m_name);
        db.setDatabaseName(file);
        QVERIFY2(db.open(), "cannot open the fixture");
    }
    ~DittoDb()
    {
        QSqlDatabase::database(m_name).close();
        QSqlDatabase::removeDatabase(m_name);
    }

    // Returns the row's lID. parent: a group's lID, -1 for none.
    qint64 clip(const QString& description, qint64 date, qint64 parent, double order, double sticky,
        const QList<std::pair<QString, QByteArray>>& formats, bool isGroup = false, bool dontAutoDelete = false)
    {
        QSqlDatabase db = QSqlDatabase::database(m_name);
        QSqlQuery query(db);
        query.prepare(u"INSERT INTO Main (lDate, mText, lShortCut, lDontAutoDelete, CRC, bIsGroup, lParentID, "
                      u"clipOrder, clipGroupOrder, lastPasteDate, stickyClipOrder, stickyClipGroupOrder) "
                      u"VALUES (?, ?, 0, ?, 0, ?, ?, ?, ?, ?, ?, ?)"_s);
        query.addBindValue(date);
        query.addBindValue(description);
        query.addBindValue(dontAutoDelete ? date : 0); // Ditto stores a nonzero protection value
        query.addBindValue(isGroup ? 1 : 0);
        query.addBindValue(parent);
        query.addBindValue(order);
        query.addBindValue(order);
        query.addBindValue(date + 5);
        query.addBindValue(sticky);
        query.addBindValue(sticky);
        if (!query.exec())
            return QTest::qFail(qPrintable(query.lastError().text()), __FILE__, __LINE__), 0;
        const qint64 id = query.lastInsertId().toLongLong();
        QSqlQuery data(db);
        data.prepare(u"INSERT INTO Data (lParentID, strClipBoardFormat, ooData) VALUES (?, ?, ?)"_s);
        for (const auto& [name, bytes] : formats) {
            data.addBindValue(id);
            data.addBindValue(name);
            data.addBindValue(bytes);
            data.exec();
        }
        return id;
    }

    // Ditto open and writing: a transaction not committed yet, with a row of its own.
    void holdOpen()
    {
        QSqlQuery query(QSqlDatabase::database(m_name));
        QVERIFY(query.exec(u"BEGIN IMMEDIATE"_s));
        QVERIFY(query.exec(u"INSERT INTO Main (lDate, mText, bIsGroup, lParentID) VALUES (1, 'not yet', 0, -1)"_s));
    }

    static constexpr double kNotSticky = -2147483647;

private:
    QString m_name;
};

Report runImport(ClipStore& store, const QString& path, Options options = {})
{
    Hooks hooks;
    const QString own = store.uniqueGroupName(groupName(path));
    hooks.sink = [&](std::vector<ClipImportItem>&& items) { return store.importClips(std::move(items), own); };
    options.now = options.now > 0 ? options.now : kStart;
    return importFile(path, options, hooks);
}

QStringList textsIn(const ClipStore& store, qint64 group)
{
    QStringList list;
    for (const qint64 id : store.groupOrder(group))
        list.append(store.clip(id)->text);
    return list;
}

qint64 groupNamed(const ClipStore& store, const QString& name)
{
    for (const ClipGroup& g : store.groups()) {
        if (g.id != ClipStore::kPinned && g.name == name)
            return g.id;
    }
    return 0;
}

QString write(const QTemporaryDir& dir, const QString& name, const QByteArray& bytes)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        QTest::qFail("cannot write a test file", __FILE__, __LINE__);
    return path;
}

} // namespace

class ClipImportTest : public QObject {
    Q_OBJECT

private slots:
    void storeBatch()
    {
        QTemporaryDir dir;
        ClipStore store(dir.path());
        QVERIFY(store.open(kStart));
        ClipCapture own;
        own.kind = ClipKind::Text;
        own.text = u"本来就有"_s;
        const qint64 existing = store.add(own, kStart + 10 * kDay);
        const qint64 mine = store.addGroup(u"我的"_s);
        store.setGroup(std::vector<qint64> {existing}, mine);

        std::vector<ClipImportItem> items;
        for (const QString& s : {u"甲"_s, u"乙"_s, u"本来就有"_s, u"甲"_s, u"丙"_s}) {
            ClipImportItem item;
            item.text = s;
            item.created = item.used = kStart + (s == u"乙"_s ? 3 * kDay : 0);
            item.group = s == u"丙"_s ? u"我的"_s : QString();
            items.push_back(item);
        }
        const ClipImportResult result = store.importClips(items, u"新来的"_s);
        QCOMPARE(result.added, 3);
        QCOMPARE(result.duplicates, 2);
        QCOMPARE(result.failed, 0);
        const qint64 fresh = groupNamed(store, u"新来的"_s);
        QVERIFY(fresh != 0);
        QCOMPARE(textsIn(store, fresh), (QStringList {u"甲"_s, u"乙"_s})); // in the order given
        QCOMPARE(textsIn(store, mine), (QStringList {u"本来就有"_s, u"丙"_s})); // after what it has
        // In the list by when they were used: the oldest last, the existing one on top.
        QCOMPARE(store.clips().front().text, u"本来就有"_s);
        QCOMPARE(store.clips().back().text, u"甲"_s);
        QCOMPARE(store.clips()[1].text, u"乙"_s);
        QCOMPARE(store.uniqueGroupName(u"新来的"_s), u"新来的 2"_s);

        // Nothing new: no group made.
        QCOMPARE(store.importClips({items.front()}, u"空的"_s).duplicates, 1);
        QCOMPARE(groupNamed(store, u"空的"_s), qint64(0));

        // On disk the same.
        store.flush();
        ClipStore again(dir.path());
        QVERIFY(again.open(kStart));
        QCOMPARE(again.clips().size(), store.clips().size());
        QCOMPARE(textsIn(again, groupNamed(again, u"新来的"_s)), (QStringList {u"甲"_s, u"乙"_s}));
        QCOMPARE(again.clip(store.groupOrder(fresh).front())->created, kStart);
    }

    void dittoProtectedEntries()
    {
        QTemporaryDir source;
        QTemporaryDir data;
        QVERIFY(source.isValid() && data.isValid());
        const QString file = source.filePath(u"Ditto.db"_s);
        {
            DittoDb ditto(file);
            const auto add = [&](const QString& text, qint64 parent, bool protectedEntry, double sticky) {
                return ditto.clip(text, 1700000100, parent, 10, sticky,
                    {{u"CF_UNICODETEXT"_s, utf16(text)}}, false, protectedEntry);
            };
            const qint64 group = ditto.clip(u"Work"_s, 1700000000, -1, 0, DittoDb::kNotSticky, {}, true);
            add(u"protected root"_s, -1, true, DittoDb::kNotSticky);
            add(u"protected in group"_s, group, true, DittoDb::kNotSticky);
            add(u"normal"_s, -1, false, DittoDb::kNotSticky);
            add(u"sticky is not protection"_s, -1, false, 5);
        }
        ClipStore store(data.path());
        QVERIFY(store.open(kStart));
        const Report report = runImport(store, file);
        QCOMPARE(report.error, Error::None);
        QCOMPARE(report.added, 4);
        QCOMPARE(textsIn(store, ClipStore::kPinned).size(), 2);
        const auto check = [](const ClipStore& clips) {
            for (const Clip& clip : clips.clips()) {
                if (clip.text.startsWith(u"protected"))
                    QCOMPARE(clip.group, ClipStore::kPinned);
                else
                    QVERIFY(clip.group != ClipStore::kPinned);
            }
        };
        check(store);
        store.setLimits({1, 1}, kStart + 1000 * kDay);
        store.flush();
        ClipStore reopened(data.path());
        QVERIFY(reopened.open(kStart + 1000 * kDay));
        QCOMPARE(textsIn(reopened, ClipStore::kPinned).size(), 2);
        check(reopened);
        QCOMPARE(runImport(reopened, file).duplicates, 4);
    }

    void dittoFixture()
    {
        QTemporaryDir dir;
        QTemporaryDir data;
        const QString file = dir.filePath(u"Ditto.db"_s);
        QImage picture(4, 3, QImage::Format_RGB32);
        picture.fill(QColor(200, 30, 40));
        const QByteArray html = "Version:0.9\r\nStartHTML:0000000105\r\n<html><body><b>粗体</b></body></html>";
        const QByteArray rtf = R"({\rtf1\ansi bold})";
        {
            DittoDb ditto(file);
            const double plain = DittoDb::kNotSticky;
            // Newest first by clipOrder. An older one with ANSI text only.
            ditto.clip(u"hello 世界"_s, 1'700'000'100, -1, 10, plain,
                {{u"CF_UNICODETEXT"_s, utf16(u"hello 世界"_s)}, {u"CF_TEXT"_s, "hello ?"}});
            ditto.clip(u"粗体"_s, 1'700'000'200, -1, 20, plain,
                {{u"CF_UNICODETEXT"_s, utf16(u"粗体"_s)}, {u"HTML Format"_s, html + '\0'}, {u"Rich Text Format"_s, rtf + '\0'}});
            ditto.clip(u"https://example.com/a"_s, 1'700'000'300, -1, 30, plain,
                {{u"CF_UNICODETEXT"_s, utf16(u"https://example.com/a"_s)}});
            ditto.clip(u"[Image]"_s, 1'700'000'400, -1, 40, plain, {{u"CF_DIB"_s, dibOf(picture)}});
            ditto.clip(u"C:\\a.txt"_s, 1'700'000'500, -1, 50, plain,
                {{u"CF_HDROP"_s, dropFiles({u"C:\\a.txt"_s, u"D:\\b c\\d.txt"_s})}});
            ditto.clip(u"hello 世界"_s, 1'700'000'600, -1, 60, plain, // the same again
                {{u"CF_UNICODETEXT"_s, utf16(u"hello 世界"_s)}});
            ditto.clip(u"   "_s, 1'700'000'700, -1, 70, plain, {{u"CF_UNICODETEXT"_s, utf16(u"   "_s)}}); // nothing
            ditto.clip(u"仅 ANSI"_s, 1'700'000'750, -1, 75, plain, {{u"CF_TEXT"_s, "ansi only\0"_ba}});
            const qint64 work = ditto.clip(u"工作"_s, 1'700'000'800, -1, 0, plain, {}, true);
            const qint64 sub = ditto.clip(u"子"_s, 1'700'000'810, work, 0, plain, {}, true);
            ditto.clip(u"组里一"_s, 1'700'000'900, work, 1, plain, {{u"CF_UNICODETEXT"_s, utf16(u"组里一"_s)}});
            ditto.clip(u"组里二（置顶）"_s, 1'700'000'910, work, 2, 5, {{u"CF_UNICODETEXT"_s, utf16(u"组里二（置顶）"_s)}});
            ditto.clip(u"嵌套的"_s, 1'700'000'920, sub, 1, plain, {{u"CF_UNICODETEXT"_s, utf16(u"嵌套的"_s)}});
        }

        ClipStore store(data.path());
        QVERIFY(store.open(kStart));
        store.setLimits({1, 1}, kStart + 1000 * kDay); // everything but groups would go
        const Report report = runImport(store, file);
        QCOMPARE(int(report.error), int(Error::None));
        QCOMPARE(report.added, 9);
        QCOMPARE(report.duplicates, 1);
        QCOMPARE(report.skipped, 1);
        QVERIFY(report.ownGroupUsed);
        QCOMPARE(groupName(file), u"Ditto 导入"_s);

        const qint64 own = groupNamed(store, u"Ditto 导入"_s);
        const qint64 work = groupNamed(store, u"工作"_s);
        const qint64 sub = groupNamed(store, u"工作/子"_s);
        QVERIFY(own && work && sub);
        QCOMPARE(store.clips().size(), std::size_t(9));
        // Ditto's order inside: newest (highest clipOrder) first.
        QCOMPARE(textsIn(store, own).size(), 6);
        QCOMPARE(textsIn(store, own).front(), u"ansi only"_s);
        QCOMPARE(textsIn(store, work), (QStringList {u"组里二（置顶）"_s, u"组里一"_s})); // pinned first
        QCOMPARE(textsIn(store, sub), (QStringList {u"嵌套的"_s}));

        const auto find = [&](const QString& text) -> const Clip* {
            for (const Clip& c : store.clips()) {
                if (c.text == text)
                    return &c;
            }
            return nullptr;
        };
        const Clip* hello = find(u"hello 世界"_s);
        QVERIFY(hello);
        QCOMPARE(hello->kind, ClipKind::Text);
        // The same text twice: the newer one's times (Ditto lists it first).
        QCOMPARE(hello->created, qint64(1'700'000'600) * 1000);
        QCOMPARE(hello->used, qint64(1'700'000'605) * 1000); // when last pasted
        QCOMPARE(hello->group, own);
        const Clip* bold = find(u"粗体"_s);
        QVERIFY(bold && bold->html && bold->rtf);
        QCOMPARE(store.payload(bold->id).html, html);
        QCOMPARE(store.payload(bold->id).rtf, rtf);
        QCOMPARE(find(u"https://example.com/a"_s)->kind, ClipKind::Link);
        const Clip* files = find(u"C:\\a.txt\nD:\\b c\\d.txt"_s);
        QVERIFY(files);
        QCOMPARE(files->kind, ClipKind::Files);

        const Clip* image = nullptr;
        for (const Clip& c : store.clips()) {
            if (c.kind == ClipKind::Image)
                image = &c;
        }
        QVERIFY(image);
        QCOMPARE(image->width, 4);
        QCOMPARE(image->height, 3);
        store.flush();
        const QImage png(store.imagePath(image->id));
        QCOMPARE(png.size(), QSize(4, 3));
        QCOMPARE(png.pixelColor(1, 1), QColor(200, 30, 40));
        QCOMPARE(image->bytes, QFileInfo(store.imagePath(image->id)).size());
        QCOMPARE(image->hash, imagePixelHash(png));

        // Importing again: all already there. The limits took nothing away.
        const Report again = runImport(store, file);
        QCOMPARE(again.added, 0);
        QCOMPARE(again.duplicates, 10);
        QCOMPARE(store.clips().size(), std::size_t(9));
        QCOMPARE(groupNamed(store, u"Ditto 导入 2"_s), qint64(0));
        store.setLimits({1, 1}, kStart + 2000 * kDay);
        QCOMPARE(store.clips().size(), std::size_t(9));

        // After a restart too.
        store.flush();
        ClipStore reopened(data.path());
        QVERIFY(reopened.open(kStart + 3000 * kDay));
        reopened.setLimits({1, 1}, kStart + 3000 * kDay);
        QCOMPARE(reopened.clips().size(), std::size_t(9));
    }

    // Ditto running: its database has a transaction open, and the file is read
    // all the same; what is not committed is not in the history.
    void dittoPreservesAnimations()
    {
        QTemporaryDir dir;
        const auto load = [](const QString& name) {
            QFile file(QStringLiteral(WINSHUN_TEST_DATA "/") + name);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        const QByteArray first = load(u"clipboard-animated.gif"_s);
        const QByteArray other = load(u"clipboard-other-animation.gif"_s);
        QVERIFY(!first.isEmpty() && !other.isEmpty());
        QByteArray flattened;
        QBuffer buffer(&flattened);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(QImage::fromData(first).save(&buffer, "PNG"));
        const QString source = dir.filePath(u"source.db"_s);
        {
            DittoDb ditto(source);
            ditto.clip(u"动图一"_s, kStart / 1000, -1, 10, DittoDb::kNotSticky,
                {{u"GIF"_s, first}, {u"PNG"_s, flattened}});
            ditto.clip(u"动图二"_s, kStart / 1000, -1, 20, DittoDb::kNotSticky,
                {{u"image/gif"_s, other}, {u"PNG"_s, flattened}});
        }
        ClipStore store(dir.filePath(u"history"_s));
        QVERIFY(store.open(kStart));
        const auto report = runImport(store, source);
        QCOMPARE(report.added, 2);
        QCOMPARE(report.duplicates, 0);
        QCOMPARE(report.skipped, 0);
        store.close();
        QVERIFY(store.open(kStart));
        for (const Clip& clip : store.clips()) {
            QCOMPARE(clip.imageFormat, ClipImageFormat::Gif);
            QVERIFY(clip.animated);
            QFile saved(store.payload(clip.id).imagePath);
            QVERIFY(saved.open(QIODevice::ReadOnly));
            const QByteArray bytes = saved.readAll();
            QVERIFY(bytes == first || bytes == other);
        }
    }

    void dittoRunning()
    {
        QTemporaryDir dir;
        QTemporaryDir data;
        const QString file = dir.filePath(u"Ditto.db"_s);
        {
            DittoDb ditto(file);
            ditto.clip(u"已经存好的"_s, 1'700'000'100, -1, 10, DittoDb::kNotSticky,
                {{u"CF_UNICODETEXT"_s, utf16(u"已经存好的"_s)}});
            ditto.holdOpen();
            ClipStore store(data.path());
            QVERIFY(store.open(kStart));
            const Report report = runImport(store, file);
            QCOMPARE(int(report.error), int(Error::None));
            QCOMPARE(report.added, 1);
            QCOMPARE(textsIn(store, groupNamed(store, u"Ditto 导入"_s)), (QStringList {u"已经存好的"_s}));
            QVERIFY(QFileInfo::exists(file)); // the source itself is left as it was
        }
    }

    // Only entries in groups: none in the import's own.
    void dittoOnlyGroups()
    {
        QTemporaryDir dir;
        QTemporaryDir data;
        const QString file = dir.filePath(u"Ditto.db"_s);
        {
            DittoDb ditto(file);
            const qint64 work = ditto.clip(u"工作"_s, 1'700'000'100, -1, 0, DittoDb::kNotSticky, {}, true);
            ditto.clip(u"仅在组里"_s, 1'700'000'200, work, 1, DittoDb::kNotSticky,
                {{u"CF_UNICODETEXT"_s, utf16(u"仅在组里"_s)}});
        }
        ClipStore store(data.path());
        QVERIFY(store.open(kStart));
        const Report report = runImport(store, file);
        QCOMPARE(report.added, 1);
        QVERIFY(!report.ownGroupUsed);
        QCOMPARE(groupNamed(store, u"Ditto 导入"_s), qint64(0)); // none made for nothing
        QVERIFY(groupNamed(store, u"工作"_s) != 0);
    }

    // Another database Ditto made, to look at by hand (set WINSHUN_TEST_DITTO_DB).
    void realDittoDb()
    {
        const QString file = qEnvironmentVariable("WINSHUN_TEST_DITTO_DB");
        if (file.isEmpty())
            QSKIP("WINSHUN_TEST_DITTO_DB is not set");
        QTemporaryDir data;
        ClipStore store(data.path());
        QVERIFY(store.open(kStart));
        const Report report = runImport(store, file);
        QCOMPARE(int(report.error), int(Error::None));
        QCOMPARE(report.added, 2);
        QCOMPARE(textsIn(store, groupNamed(store, u"Ditto 导入"_s)), (QStringList {u"beta"_s, u"alpha 真实库"_s}));
    }

    // Bytes captured by Ditto 3.25.113.0 itself (data/ditto-real.db): five clips put on the
    // clipboard by another program (text; HTML Format + text; Rich Text Format + text; a 37x23
    // CF_DIB; a CF_HDROP of two files), as Ditto stored them.
    void dittoRealBytes()
    {
        QTemporaryDir dir;
        QTemporaryDir data;
        const QString file = dir.filePath(u"Ditto.db"_s);
        QVERIFY(QFile::copy(QStringLiteral(WINSHUN_TEST_DATA "/ditto-real.db"), file));
        QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner);

        // What Ditto stored, read straight from its tables.
        const auto stored = [&](const QString& format) {
            QByteArray bytes;
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"real-bytes"_s);
                db.setDatabaseName(file);
                if (db.open()) {
                    QSqlQuery query(db);
                    query.prepare(u"SELECT ooData FROM Data WHERE strClipBoardFormat = ?"_s);
                    query.addBindValue(format);
                    if (query.exec() && query.next())
                        bytes = query.value(0).toByteArray();
                }
                db.close();
            }
            QSqlDatabase::removeDatabase(u"real-bytes"_s);
            return bytes;
        };
        const QByteArray storedHtml = stored(u"HTML Format"_s);
        const QByteArray storedRtf = stored(u"Rich Text Format"_s);
        QVERIFY(storedHtml.size() > 100);
        QVERIFY(storedRtf.size() > 20);

        ClipStore store(data.path());
        QVERIFY(store.open(kStart));
        const Report report = runImport(store, file);
        QCOMPARE(int(report.error), int(Error::None));
        QCOMPARE(report.added, 5);
        QCOMPARE(report.skipped, 0);
        QCOMPARE(report.failed, 0);
        const qint64 own = groupNamed(store, u"Ditto 导入"_s);
        QCOMPARE(textsIn(store, own).size(), 5);

        const auto byText = [&](const QString& text) -> const Clip* {
            for (const Clip& c : store.clips()) {
                if (c.text == text)
                    return &c;
            }
            return nullptr;
        };

        const QString plain = u"Ditto real text: café 真实 αβγ \U0001F600\r\nline two\ttab"_s;
        const Clip* text = byText(plain);
        QVERIFY(text);
        QCOMPARE(text->kind, ClipKind::Text);
        QVERIFY(!text->html && !text->rtf);

        const Clip* html = byText(u"Ditto html 真实 link"_s);
        QVERIFY(html);
        QCOMPARE(html->kind, ClipKind::Text);
        QVERIFY(html->html);
        const QByteArray gotHtml = store.payload(html->id).html;
        QCOMPARE(gotHtml, storedHtml.chopped(1)); // Ditto stores the trailing NUL; the clip does not need it
        QVERIFY(gotHtml.contains("<!--StartFragment-->"));
        QVERIFY(gotHtml.contains(QString(u"真实").toUtf8()));

        const Clip* rtf = byText(u"Ditto rtf italic end"_s);
        QVERIFY(rtf && rtf->rtf);
        QCOMPARE(rtf->kind, ClipKind::Text);
        QCOMPARE(store.payload(rtf->id).rtf, storedRtf.chopped(1));
        QVERIFY(store.payload(rtf->id).rtf.startsWith("{\\rtf1"));

        const Clip* image = nullptr;
        const Clip* files = nullptr;
        for (const Clip& c : store.clips()) {
            if (c.kind == ClipKind::Image)
                image = &c;
            else if (c.kind == ClipKind::Files)
                files = &c;
        }
        QVERIFY(image);
        QCOMPARE(image->width, 37);
        QCOMPARE(image->height, 23);
        store.flush();
        const QImage png(store.imagePath(image->id));
        QCOMPARE(png.size(), QSize(37, 23));
        for (int y = 0; y < 23; ++y) {
            for (int x = 0; x < 37; ++x) {
                // The pattern the capture helper drew: R = 7x, G = 11y, B = x*y (each mod 256).
                const QColor want((x * 7) % 256, (y * 11) % 256, (x * y) % 256);
                if (png.pixelColor(x, y) != want)
                    QFAIL(qPrintable(u"pixel %1,%2 is %3, wanted %4"_s.arg(x).arg(y).arg(png.pixelColor(x, y).name(), want.name())));
            }
        }
        QCOMPARE(image->hash, imagePixelHash(png));

        QVERIFY(files);
        QStringList names = files->text.split(u'\n');
        QCOMPARE(names.size(), 2);
        QCOMPARE(QFileInfo(names[0]).fileName(), u"ditto-drop-a.txt"_s);
        QCOMPARE(QFileInfo(names[1]).fileName(), u"ditto-drop-b.txt"_s);
        QVERIFY(names[0].contains(u"\\scratchpad\\dropfiles\\"_s));
    }

    void dittoNotDitto()
    {
        QTemporaryDir dir;
        QTemporaryDir data;
        ClipStore store(data.path());
        QVERIFY(store.open(kStart));
        // A SQLite file with other tables.
        const QString other = dir.filePath(u"other.db"_s);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"other"_s);
            db.setDatabaseName(other);
            QVERIFY(db.open());
            QSqlQuery(db).exec(u"CREATE TABLE x (a)"_s);
        }
        QSqlDatabase::removeDatabase(u"other"_s);
        Report report = runImport(store, other);
        QCOMPARE(int(report.error), int(Error::NotDitto));
        QCOMPARE(report.added, 0);
        // Not a database at all, named like one.
        report = runImport(store, write(dir, u"fake.db"_s, "just words"));
        QCOMPARE(report.added, 1); // read as text
        QCOMPARE(int(report.error), int(Error::None));
        QCOMPARE(int(runImport(store, dir.filePath(u"missing.txt"_s)).error), int(Error::Missing));
    }

    void decoding()
    {
        QCOMPARE(decodeText("plain"), u"plain"_s);
        QCOMPARE(decodeText("\xEF\xBB\xBF" "中文"), u"中文"_s);
        QCOMPARE(decodeText(QString(u"中文").toUtf8()), u"中文"_s);
        QCOMPARE(decodeText("\xFF\xFE" + utf16(u"中文"_s, false)), u"中文"_s);
        QByteArray be = "\xFE\xFF";
        be += QByteArray("\x4E\x2D\x65\x87", 4); // 中文
        QCOMPARE(decodeText(be), u"中文"_s);
        QCOMPARE(decodeText("\xD6\xD0\xCE\xC4"), u"中文"_s); // GBK
        QCOMPARE(decodeText("\xB1\xEA\xCC\xE2,\xC4\xDA\xC8\xDD"), u"标题,内容"_s);
    }

    // The picture the history shows of an equation, which Word gives as a
    // metafile only (GitHub #58; ClipImage.cpp, used by the watcher): twice
    // its size at 96 dpi, on white, opaque, the blank around it cut off.
    void metafilePicture()
    {
        const RECT frame {0, 0, 5000, 2000}; // 50 x 20 mm
        HDC dc = ::CreateEnhMetaFileW(nullptr, nullptr, &frame, nullptr);
        QVERIFY(dc);
        // A black box, about 10 x 4 mm, 20 mm from the left and 8 mm from the top.
        const double perMm = static_cast<double>(::GetDeviceCaps(dc, HORZRES)) / ::GetDeviceCaps(dc, HORZSIZE);
        const double perMmY = static_cast<double>(::GetDeviceCaps(dc, VERTRES)) / ::GetDeviceCaps(dc, VERTSIZE);
        const RECT box {static_cast<LONG>(20 * perMm), static_cast<LONG>(8 * perMmY), static_cast<LONG>(30 * perMm),
            static_cast<LONG>(12 * perMmY)};
        ::FillRect(dc, &box, static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
        HENHMETAFILE emf = ::CloseEnhMetaFile(dc);
        QVERIFY(emf);
        QByteArray bits(static_cast<qsizetype>(::GetEnhMetaFileBits(emf, 0, nullptr)), Qt::Uninitialized);
        ::GetEnhMetaFileBits(emf, static_cast<UINT>(bits.size()), reinterpret_cast<BYTE*>(bits.data()));
        // Its size by the reference device the metafile records: a process
        // not aware of the display's scaling gets scaled numbers from
        // GetDeviceCaps, the metafile the real ones.
        ENHMETAHEADER header {};
        ::GetEnhMetaFileHeader(emf, sizeof header, &header);
        ::DeleteEnhMetaFile(emf);
        const int boxWidth = qRound((box.right - box.left) * 192 / 25.4 * header.szlMillimeters.cx / header.szlDevice.cx);
        const int boxHeight = qRound((box.bottom - box.top) * 192 / 25.4 * header.szlMillimeters.cy / header.szlDevice.cy);

        const QImage image = imageFromEmf(bits);
        QVERIFY(!image.isNull());
        // At 192 dpi, and 8 pixels of white on each side.
        QVERIFY2(qAbs(image.width() - (boxWidth + 16)) <= 2 && qAbs(image.height() - (boxHeight + 16)) <= 2,
            qPrintable(u"%1 x %2, the box %3 x %4"_s.arg(image.width()).arg(image.height()).arg(boxWidth).arg(boxHeight)));
        QCOMPARE(image.pixel(0, 0), qRgba(255, 255, 255, 255));
        QCOMPARE(image.pixel(image.width() - 1, image.height() - 1), qRgba(255, 255, 255, 255));
        QCOMPARE(image.pixel(image.width() / 2, image.height() / 2), qRgba(0, 0, 0, 255));

        // Nothing drawn, or not a metafile: no picture.
        dc = ::CreateEnhMetaFileW(nullptr, nullptr, &frame, nullptr);
        emf = ::CloseEnhMetaFile(dc);
        QByteArray blank(static_cast<qsizetype>(::GetEnhMetaFileBits(emf, 0, nullptr)), Qt::Uninitialized);
        ::GetEnhMetaFileBits(emf, static_cast<UINT>(blank.size()), reinterpret_cast<BYTE*>(blank.data()));
        ::DeleteEnhMetaFile(emf);
        QVERIFY(imageFromEmf(blank).isNull());
        QVERIFY(imageFromEmf(QByteArray(200, 'x')).isNull());
        QVERIFY(imageFromEmf({}).isNull());
    }

    void splitting()
    {
        const QString text = u"一\r\n二\r\n\r\n\r\n  三\n四\n\n五  \n"_s;
        QCOMPARE(splitText(text, Split::Lines), (QStringList {u"一"_s, u"二"_s, u"  三"_s, u"四"_s, u"五  "_s}));
        QCOMPARE(splitText(text, Split::Paragraphs), (QStringList {u"一\r\n二"_s, u"  三\n四"_s, u"五"_s}));
        QCOMPARE(splitText(text, Split::Whole), (QStringList {u"一\r\n二\r\n\r\n\r\n  三\n四\n\n五"_s}));
        QVERIFY(splitText(u" \n\n \n"_s, Split::Whole).isEmpty());
        QVERIFY(splitText(QString(), Split::Paragraphs).isEmpty());
    }

    void textFile()
    {
        QTemporaryDir dir;
        QTemporaryDir data;
        ClipStore store(data.path());
        QVERIFY(store.open(kStart));
        const QString path = write(dir, u"笔记.txt"_s, "\xEF\xBB\xBF" "第一段\r\n还是第一段\r\n\r\nhttps://example.com\r\n\r\n\r\nC:\\x\\y.txt\r\n");
        Options options;
        options.split = Split::Paragraphs;
        Report report = runImport(store, path, options);
        QCOMPARE(report.added, 3);
        const qint64 group = groupNamed(store, u"笔记 导入"_s);
        QVERIFY(group);
        QCOMPARE(textsIn(store, group), (QStringList {u"第一段\r\n还是第一段"_s, u"https://example.com"_s, u"C:\\x\\y.txt"_s}));
        QCOMPARE(store.clip(store.groupOrder(group)[1])->kind, ClipKind::Link);
        QCOMPARE(store.clip(store.groupOrder(group)[2])->kind, ClipKind::Path);
        // The first is the newest, a second apart.
        QCOMPARE(store.clip(store.groupOrder(group)[0])->used, kStart);
        QCOMPARE(store.clip(store.groupOrder(group)[2])->used, kStart - 2000);
        // The same file again: all there; with other lines a second group of that name.
        report = runImport(store, path, options);
        QCOMPARE(report.added, 0);
        QCOMPARE(report.duplicates, 3);
        options.split = Split::Lines;
        report = runImport(store, path, options);
        QCOMPARE(report.added, 2); // the url and the path are there
        QCOMPARE(report.duplicates, 2);
        QVERIFY(groupNamed(store, u"笔记 导入 2"_s) != 0);
        options.split = Split::Whole;
        QCOMPARE(runImport(store, path, options).added, 1);
        QCOMPARE(groupName(path), u"笔记 导入"_s);
    }

    void csvParsing()
    {
        auto rows = parseCsv(u"a,b\r\n\"x, y\",\"he said \"\"hi\"\"\"\r\n\"line1\nline2\",z\r\n"_s);
        QCOMPARE(rows.size(), std::size_t(3));
        QCOMPARE(rows[1], (QStringList {u"x, y"_s, u"he said \"hi\""_s}));
        QCOMPARE(rows[2], (QStringList {u"line1\nline2"_s, u"z"_s}));
        rows = parseCsv(u"a;b\n1;2"_s);
        QCOMPARE(rows[1], (QStringList {u"1"_s, u"2"_s}));
        rows = parseCsv(u"a\tb\n1\t2\n\n3\t4"_s);
        QCOMPARE(rows.size(), std::size_t(3)); // the blank line is no row
        QCOMPARE(rows[2], (QStringList {u"3"_s, u"4"_s}));
        QVERIFY(parseCsv(QString()).empty());
        rows = parseCsv(u"\"unclosed,1\n2"_s); // does not hang or lose the text
        QCOMPARE(rows.size(), std::size_t(1));
    }

    void csvFiles()
    {
        QTemporaryDir dir;
        QTemporaryDir data;
        ClipStore store(data.path());
        QVERIFY(store.open(kStart));

        // A header names the text and the time columns.
        const QString withHeader = write(dir, u"a.csv"_s,
            "id,time,\xE5\x86\x85\xE5\xAE\xB9\r\n"
            "1,2026-01-02 03:04:05,\"first, with comma\"\r\n"
            "2,1700000000,\"two\r\nlines\"\r\n"
            "3,,\r\n"
            "4,not a time,last\r\n");
        Report report = runImport(store, withHeader);
        QCOMPARE(report.added, 3);
        QCOMPARE(report.skipped, 1);
        const qint64 group = groupNamed(store, u"a 导入"_s);
        QCOMPARE(textsIn(store, group), (QStringList {u"first, with comma"_s, u"two\r\nlines"_s, u"last"_s}));
        const std::vector<qint64> order = store.groupOrder(group);
        QCOMPARE(store.clip(order[0])->created, QDateTime(QDate(2026, 1, 2), QTime(3, 4, 5)).toMSecsSinceEpoch());
        QCOMPARE(store.clip(order[1])->created, qint64(1'700'000'000'000));
        QCOMPARE(store.clip(order[2])->created, kStart - 3000); // no time: by its place in the file (the 4th row)

        // No header: the first column, the first row is data.
        const QString plain = write(dir, u"b.csv"_s, "alpha,1\nbeta,2\n");
        report = runImport(store, plain);
        QCOMPARE(report.added, 2);
        QCOMPARE(textsIn(store, groupNamed(store, u"b 导入"_s)), (QStringList {u"alpha"_s, u"beta"_s}));

        // GBK, as Chinese Excel saves it; "text" in a later column.
        const QString gbk = write(dir, u"c.csv"_s, "\xB1\xEA\xCC\xE2,Text\n1,\xD6\xD0\xCE\xC4\n");
        report = runImport(store, gbk);
        QCOMPARE(report.added, 1);
        QCOMPARE(textsIn(store, groupNamed(store, u"c 导入"_s)), (QStringList {u"中文"_s}));

        // Only a time column named: the text is the first of the others.
        const QString onlyTime = write(dir, u"d.csv"_s, "Date,words\n2026-03-04,hi there\n");
        QCOMPARE(runImport(store, onlyTime).added, 1);
        QCOMPARE(textsIn(store, groupNamed(store, u"d 导入"_s)), (QStringList {u"hi there"_s}));
    }

    // The way the app does it: read on a thread of its own, each batch into the
    // store on this one.
    void jobOnAThread()
    {
        QTemporaryDir dir;
        QTemporaryDir data;
        ClipStore store(data.path());
        QVERIFY(store.open(kStart));
        QByteArray lines;
        for (int i = 0; i < 700; ++i)
            lines += "entry " + QByteArray::number(i) + "\n";
        const QString path = write(dir, u"job.txt"_s, lines);
        ClipImportJob job(&store);
        QSignalSpy finished(&job, &ClipImportJob::finished);
        job.start(path, Split::Lines);
        QCOMPARE(job.group(), u"job 导入"_s);
        QVERIFY(finished.wait(20000));
        const auto report = finished.first().first().value<clipimport::Report>();
        QCOMPARE(report.added, 700);
        QCOMPARE(int(report.error), int(Error::None));
        QCOMPARE(store.groupSize(groupNamed(store, u"job 导入"_s)), 700);
    }

    // Gone while reading: no hang, nothing left running.
    void jobDestroyedEarly()
    {
        QTemporaryDir dir;
        QTemporaryDir data;
        ClipStore store(data.path());
        QVERIFY(store.open(kStart));
        QByteArray lines;
        for (int i = 0; i < 5000; ++i)
            lines += "entry " + QByteArray::number(i) + "\n";
        const QString path = write(dir, u"early.txt"_s, lines);
        {
            ClipImportJob job(&store);
            job.start(path, Split::Lines);
        } // the batches it asked for are dropped with it
        QVERIFY(store.clips().size() <= 5000);
    }

    void progressAndCancel()
    {
        QTemporaryDir dir;
        QTemporaryDir data;
        ClipStore store(data.path());
        QVERIFY(store.open(kStart));
        QByteArray lines;
        for (int i = 0; i < 450; ++i)
            lines += "line " + QByteArray::number(i) + "\n";
        const QString path = write(dir, u"many.txt"_s, lines);
        Hooks hooks;
        int batches = 0;
        int lastDone = 0;
        hooks.sink = [&](std::vector<ClipImportItem>&& items) {
            ++batches;
            return store.importClips(std::move(items), u"many"_s);
        };
        hooks.progress = [&](int done, int total) {
            QCOMPARE(total, 450);
            lastDone = done;
        };
        Options options;
        options.split = Split::Lines;
        Report report = importFile(path, options, hooks);
        QCOMPARE(report.added, 450);
        QCOMPARE(batches, 3); // 200, 200, 50
        QCOMPARE(lastDone, 450);
        QCOMPARE(store.groupSize(groupNamed(store, u"many"_s)), 450);
        QCOMPARE(store.groupOrder(groupNamed(store, u"many"_s)).size(), std::size_t(450));

        // Cancelled after the first batch.
        int asked = 0;
        hooks.cancelled = [&] { return ++asked > 250; };
        hooks.sink = [&](std::vector<ClipImportItem>&& items) { return store.importClips(std::move(items), u"cut"_s); };
        lines.replace("line", "Line"); // other entries
        report = importFile(write(dir, u"more.txt"_s, lines), options, hooks);
        QVERIFY(report.cancelled);
        QVERIFY(report.added < 450);
        QVERIFY(report.added > 0);
    }
};

QTEST_GUILESS_MAIN(ClipImportTest)
#include "tst_clipimport.moc"
