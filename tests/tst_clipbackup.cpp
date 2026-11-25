// Backing the clipboard history up and restoring it (src/app/ClipBackupFiles):
// a real store packed into a zip, unpacked and put in another's place; and
// what is no backup of it, or a damaged one, turned away whole.
#include "BackupFiles.h"
#include "ClipBackupFiles.h"
#include "ClipStore.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <miniz.h>

using namespace ws;
using namespace Qt::StringLiterals;

namespace {

constexpr qint64 kNow = 1'790'000'000'000; // some time in 2026

QByteArray pngOf(const QColor& color)
{
    QImage image(8, 6, QImage::Format_ARGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(bytes);
}

ClipCapture text(const QString& words)
{
    ClipCapture c;
    c.kind = ClipKind::Text;
    c.text = words;
    c.hash = clipHash(ClipKind::Text, words);
    c.source = u"记事本"_s;
    return c;
}

ClipCapture picture(const QColor& color)
{
    ClipCapture c;
    c.kind = ClipKind::Image;
    c.imageData = pngOf(color);
    c.hash = QByteArray::number(color.rgba());
    c.width = 8;
    c.height = 6;
    return c;
}

// A history with two texts (one formatted, one in a group with a note) and a
// picture; the ids of the grouped text and the picture.
struct Made {
    qint64 kept = 0;
    qint64 image = 0;
    qint64 group = 0;
};
Made fill(const QString& folder)
{
    Made made;
    ClipStore store(folder);
    if (!store.open(kNow))
        return made;
    ClipCapture formatted = text(u"会议纪要 2026"_s);
    formatted.html = "<b>会议纪要</b>";
    store.add(formatted, kNow);
    made.kept = store.add(text(u"常用回复：收到，谢谢"_s), kNow + 1);
    made.image = store.add(picture(Qt::red), kNow + 2);
    made.group = store.addGroup(u"工作"_s);
    const qint64 ids[] {made.kept};
    store.setGroup(ids, made.group);
    store.setNote(made.kept, u"客服用"_s, 2);
    store.flush();
    return made;
}

// A zip of these entries, as given.
void writeZip(const QString& path, const QList<std::pair<QByteArray, QByteArray>>& entries) // Entries
{
    mz_zip_archive zip {};
    QVERIFY(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto& [name, bytes] : entries)
        QVERIFY(mz_zip_writer_add_mem(&zip, name.constData(), bytes.constData(), static_cast<size_t>(bytes.size()), 0));
    void* buffer = nullptr;
    size_t size = 0;
    QVERIFY(mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size));
    mz_zip_writer_end(&zip);
    writeFile(path, QByteArray(static_cast<const char*>(buffer), static_cast<qsizetype>(size)));
    mz_free(buffer);
}

using Entries = QList<std::pair<QByteArray, QByteArray>>;

QByteArray manifest(const QString& kind = u"clipboard"_s, int format = 1)
{
    QJsonObject o {{u"app"_s, u"WinShun"_s}, {u"format"_s, format}};
    if (!kind.isEmpty())
        o.insert(u"kind"_s, kind);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

} // namespace

class TestClipBackup : public QObject {
    Q_OBJECT

private slots:
    void name()
    {
        QCOMPARE(clipbackup::fileName(QDateTime(QDate(2026, 11, 25), QTime(19, 47, 0))),
            u"WinShun-clipboard-2026-11-25-194700.zip"_s);
    }

    // The whole way: a history packed while its store is open, unpacked, put
    // in the place of another history, and read by a store again.
    void roundTrip()
    {
        QTemporaryDir dir;
        const QString source = dir.filePath(u"旧电脑/clipboard"_s); // a path with Chinese in it
        const QString zip = dir.filePath(u"备份/剪贴板.zip"_s);
        QDir().mkpath(QFileInfo(zip).path());
        Made made;
        QByteArray pictureBytes;
        {
            made = fill(source);
            QVERIFY(made.kept && made.image && made.group);
            // Open, as when the user asks for it: more is copied meanwhile.
            ClipStore store(source);
            QVERIFY(store.open(kNow + 10));
            pictureBytes = readFile(store.imagePath(made.image));
            QVERIFY(!pictureBytes.isEmpty());
            const clipbackup::Summary packed = clipbackup::pack(source, zip, u"0.47.0"_s, QDateTime::currentDateTime());
            QVERIFY2(packed.problem.isEmpty(), qPrintable(packed.problem));
            QCOMPARE(packed.entries, 3);
            QCOMPARE(packed.pictures, 1);
        }
        // Nothing left beside the history, and none of it changed.
        QCOMPARE(QDir(dir.filePath(u"旧电脑"_s)).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList {u"clipboard"_s});

        // Another computer, with a history of its own.
        const QString data = dir.filePath(u"新电脑"_s);
        const QString target = clipbackup::historyFolder(data);
        {
            ClipStore other(target);
            QVERIFY(other.open(kNow));
            other.add(text(u"这台电脑上原来的"_s), kNow);
            other.add(picture(Qt::blue), kNow + 1);
            other.flush();
        }
        const QString staged = clipbackup::stagedFolder(data);
        const clipbackup::Summary unpacked = clipbackup::unpack(zip, staged);
        QVERIFY2(unpacked.problem.isEmpty(), qPrintable(unpacked.problem));
        QCOMPARE(unpacked.entries, 3);
        QCOMPARE(unpacked.pictures, 1);
        writeFile(staged + u".part/half"_s, "cut short"); // an earlier one, interrupted

        const clipbackup::Started started = clipbackup::applyStaged(data);
        QCOMPARE(started.what, clipbackup::Start::Restored);
        QCOMPARE(clipbackup::started().what, clipbackup::Start::Restored);
        QCOMPARE(QDir(data).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList {u"clipboard"_s});

        ClipStore store(target);
        QVERIFY(store.open(kNow + 20));
        QCOMPARE(store.clips().size(), 3u);
        const Clip* kept = store.clip(made.kept);
        QVERIFY(kept);
        QCOMPARE(kept->text, u"常用回复：收到，谢谢"_s);
        QCOMPARE(kept->note, u"客服用"_s);
        QCOMPARE(kept->noteColor, 2);
        QCOMPARE(kept->source, u"记事本"_s);
        const ClipGroup* group = store.group(kept->group);
        QVERIFY(group);
        QCOMPARE(group->name, u"工作"_s);
        const Clip* image = store.clip(made.image);
        QVERIFY(image);
        QCOMPARE(image->kind, ClipKind::Image);
        QCOMPARE(readFile(store.imagePath(made.image)), pictureBytes);
        bool formatted = false;
        for (const Clip& clip : store.clips()) {
            if (clip.html) {
                formatted = true;
                QCOMPARE(store.payload(clip.id).html, QByteArray("<b>会议纪要</b>"));
            }
            QVERIFY(clip.text != u"这台电脑上原来的"_s);
        }
        QVERIFY(formatted);
        // The other history's picture went with it.
        QCOMPARE(QDir(target + u"/images"_s).entryList(QDir::Files).size(), 1);
    }

    void nothingToBackUp()
    {
        QTemporaryDir dir;
        const clipbackup::Summary packed
            = clipbackup::pack(dir.filePath(u"clipboard"_s), dir.filePath(u"a.zip"_s), u"1"_s, QDateTime::currentDateTime());
        QVERIFY(!packed.problem.isEmpty());
        QVERIFY(!QFile::exists(dir.filePath(u"a.zip"_s)));
    }

    // A backup that cannot be written leaves the file that was there.
    void keepsTheOldFileOnFailure()
    {
        QTemporaryDir dir;
        const QString zip = dir.filePath(u"a.zip"_s);
        writeFile(zip, "earlier backup");
        QVERIFY(!clipbackup::pack(dir.filePath(u"none"_s), zip, u"1"_s, QDateTime::currentDateTime()).problem.isEmpty());
        QCOMPARE(readFile(zip), QByteArray("earlier backup"));
    }

    void turnsAway_data()
    {
        QTest::addColumn<Entries>("entries");
        QTemporaryDir dir;
        fill(dir.filePath(u"clipboard"_s));
        // A real backup's database, for the entries that are fine but for one thing.
        const QString zip = dir.filePath(u"real.zip"_s);
        QVERIFY(clipbackup::pack(dir.filePath(u"clipboard"_s), zip, u"1"_s, QDateTime::currentDateTime()).problem.isEmpty());
        mz_zip_archive reader {};
        const QByteArray zipBytes = readFile(zip);
        QVERIFY(mz_zip_reader_init_mem(&reader, zipBytes.constData(), static_cast<size_t>(zipBytes.size()), 0));
        size_t size = 0;
        void* data = mz_zip_reader_extract_file_to_heap(&reader, "clipboard.db", &size, 0);
        QVERIFY(data);
        const QByteArray good(static_cast<const char*>(data), static_cast<qsizetype>(size));
        mz_free(data);
        mz_zip_reader_end(&reader);

        QTest::newRow("no manifest") << Entries {{"clipboard.db", good}};
        QTest::newRow("another program's") << Entries {{"backup.json", "{\"app\":\"Other\"}"}, {"clipboard.db", good}};
        QTest::newRow("a settings backup") << Entries {{"backup.json", manifest({})}, {"WinShun.ini", "[General]\n"}};
        QTest::newRow("a newer format") << Entries {{"backup.json", manifest(u"clipboard"_s, 2)}, {"clipboard.db", good}};
        QTest::newRow("no database") << Entries {{"backup.json", manifest()}, {"images/1.png", "x"}};
        QTest::newRow("not a database") << Entries {{"backup.json", manifest()}, {"clipboard.db", "not sqlite at all"}};
        QTest::newRow("leaves the folder")
            << Entries {{"backup.json", manifest()}, {"clipboard.db", good}, {"images/../../evil.png", "x"}};
        QTest::newRow("a folder in images")
            << Entries {{"backup.json", manifest()}, {"clipboard.db", good}, {"images/sub/1.png", "x"}};
        QTest::newRow("something else") << Entries {{"backup.json", manifest()}, {"clipboard.db", good}, {"run.exe", "x"}};
        QTest::newRow("the same name twice")
            << Entries {{"backup.json", manifest()}, {"clipboard.db", good}, {"images/1.png", "x"}, {"images/1.PNG", "y"}};
    }
    void turnsAway()
    {
        QFETCH(const Entries, entries);
        QTemporaryDir dir;
        const QString zip = dir.filePath(u"bad.zip"_s);
        writeZip(zip, entries);
        const QString folder = dir.filePath(u"staged"_s);
        const clipbackup::Summary unpacked = clipbackup::unpack(zip, folder);
        QVERIFY(!unpacked.problem.isEmpty());
        QVERIFY(!QFileInfo::exists(folder)); // nothing half unpacked left to be applied
        QVERIFY(!QFileInfo::exists(dir.filePath(u"evil.png"_s)));
    }

    void notAZip()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath(u"a.zip"_s), "plain text, no zip");
        QVERIFY(!clipbackup::unpack(dir.filePath(u"a.zip"_s), dir.filePath(u"staged"_s)).problem.isEmpty());
        QVERIFY(!clipbackup::unpack(dir.filePath(u"none.zip"_s), dir.filePath(u"staged"_s)).problem.isEmpty());
    }

    // A byte of a picture changed in the zip: its checksum no longer fits.
    void damagedEntry()
    {
        QTemporaryDir dir;
        fill(dir.filePath(u"clipboard"_s));
        const QString zip = dir.filePath(u"a.zip"_s);
        QVERIFY(clipbackup::pack(dir.filePath(u"clipboard"_s), zip, u"1"_s, QDateTime::currentDateTime()).problem.isEmpty());
        QByteArray bytes = readFile(zip);
        const qsizetype at = bytes.indexOf("IDAT"); // stored as it is: the PNG's own chunk name
        QVERIFY(at > 0);
        bytes[at + 6] = static_cast<char>(bytes[at + 6] ^ 0x55);
        writeFile(zip, bytes);
        QVERIFY(!clipbackup::unpack(zip, dir.filePath(u"staged"_s)).problem.isEmpty());
        QVERIFY(!QFileInfo::exists(dir.filePath(u"staged"_s)));
    }

    // Each kind of backup says so when given to the other's restore.
    void settingsRestoreNamesAClipboardBackup()
    {
        QTemporaryDir dir;
        fill(dir.filePath(u"clipboard"_s));
        const QString zip = dir.filePath(u"a.zip"_s);
        QVERIFY(clipbackup::pack(dir.filePath(u"clipboard"_s), zip, u"1"_s, QDateTime::currentDateTime()).problem.isEmpty());
        const QString problem = backupfiles::unpack(zip, dir.filePath(u"restore"_s));
        QVERIFY(problem.contains(u"剪贴板"_s));
    }

    void nothingStaged()
    {
        QTemporaryDir dir;
        fill(clipbackup::historyFolder(dir.path()));
        // (Start::Restored of roundTrip stays as this process's; the return tells.)
        QCOMPARE(clipbackup::applyStaged(dir.path()).problem, QString());
        ClipStore store(clipbackup::historyFolder(dir.path()));
        QVERIFY(store.open(kNow + 5));
        QCOMPARE(store.clips().size(), 3u);
    }

    // With no history yet (a new computer): the backup becomes it.
    void restoresWithoutAHistory()
    {
        QTemporaryDir dir;
        fill(dir.filePath(u"old/clipboard"_s));
        const QString zip = dir.filePath(u"a.zip"_s);
        QVERIFY(clipbackup::pack(dir.filePath(u"old/clipboard"_s), zip, u"1"_s, QDateTime::currentDateTime()).problem.isEmpty());
        const QString data = dir.filePath(u"new"_s);
        QDir().mkpath(data);
        QVERIFY(clipbackup::unpack(zip, clipbackup::stagedFolder(data)).problem.isEmpty());
        QCOMPARE(clipbackup::applyStaged(data).what, clipbackup::Start::Restored);
        ClipStore store(clipbackup::historyFolder(data));
        QVERIFY(store.open(kNow + 5));
        QCOMPARE(store.clips().size(), 3u);
    }
};

QTEST_GUILESS_MAIN(TestClipBackup)
#include "tst_clipbackup.moc"
