#include "OcrSource.h"
#include "OcrStore.h"
#include "OcrWorker.h"
#include "PictureFile.h"
#include "Host.h"
#include "Install.h"
#include "ContentIndex.h"
#include "platform/OcrFiles.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QSettings>
#include <QSqlQuery>
#include <QSqlError>
#include <QTest>

#include <miniz.h>
#include <objbase.h>

using namespace Qt::StringLiterals;
using namespace ws::ocrsearch;

namespace {
// The Win顺 folder the worker stub takes Qt and the OCR files from (deployed by
// the build: tests/CMakeLists.txt); the plugin's own files stay in the build folder.
QString workerAppDir()
{
    return QString::fromUtf8(WINSHUN_OCR_APP_DIR);
}
void write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot write test fixture");
}
QByteArray imageBytes(const QString& text = u"INVOICE 731905"_s)
{
    QImage image(1000, 300, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(Qt::black);
    QFont font(u"Microsoft YaHei"_s);
    font.setPixelSize(60);
    painter.setFont(font);
    painter.drawText(QRect(40, 30, 920, 100), Qt::AlignLeft | Qt::AlignVCenter, text);
    painter.drawText(QRect(40, 150, 920, 100), Qt::AlignLeft | Qt::AlignVCenter, u"报销规定 ABC 123.45"_s);
    painter.end();
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}
void zip(const QString& path, const QList<QPair<QByteArray, QByteArray>>& parts)
{
    mz_zip_archive archive {};
    if (!mz_zip_writer_init_heap(&archive, 0, 0))
        throw std::runtime_error("zip init");
    for (const auto& [name, data] : parts)
        if (!mz_zip_writer_add_mem(&archive, name.constData(), data.constData(), data.size(), MZ_DEFAULT_LEVEL))
            throw std::runtime_error("zip entry");
    void* bytes = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&archive, &bytes, &size))
        throw std::runtime_error("zip finalize");
    write(path, QByteArray(static_cast<const char*>(bytes), qsizetype(size)));
    mz_free(bytes);
    mz_zip_writer_end(&archive);
}
void office(const QString& path, const QByteArray& png)
{
    const QByteArray imageRel = "<Relationships><Relationship Id='rId1' "
                                "Type='http://schemas.openxmlformats.org/officeDocument/2006/relationships/image' "
                                "Target='../media/image1.png'/></Relationships>";
    const QByteArray drawing
        = "<root xmlns:r='http://schemas.openxmlformats.org/officeDocument/2006/relationships'><p><blip "
          "r:embed='rId1'/></p></root>";
    if (path.endsWith(u".docx")) {
        zip(path,
            {{"word/document.xml", drawing},
                {"word/_rels/document.xml.rels",
                    "<Relationships><Relationship Id='rId1' Type='a/image' "
                    "Target='media/image1.png'/></Relationships>"},
                {"word/media/image1.png", png}});
    } else if (path.endsWith(u".pptx")) {
        zip(path,
            {{"ppt/presentation.xml", "<presentation xmlns:r='r'><sldId r:id='first'/></presentation>"},
                {"ppt/_rels/presentation.xml.rels",
                    "<Relationships><Relationship Id='first' Type='a/slide' "
                    "Target='slides/slide12.xml'/></Relationships>"},
                {"ppt/slides/slide12.xml", drawing}, {"ppt/slides/_rels/slide12.xml.rels", imageRel},
                {"ppt/media/image1.png", png}});
    } else {
        zip(path,
            {{"xl/workbook.xml", "<workbook xmlns:r='r'><sheet name='Travel' r:id='sheet1'/></workbook>"},
                {"xl/_rels/workbook.xml.rels",
                    "<Relationships><Relationship Id='sheet1' Type='a/worksheet' "
                    "Target='worksheets/sheet1.xml'/></Relationships>"},
                {"xl/worksheets/sheet1.xml", "<worksheet/>"},
                {"xl/worksheets/_rels/sheet1.xml.rels",
                    "<Relationships><Relationship Id='d' Type='a/drawing' "
                    "Target='../drawings/drawing1.xml'/></Relationships>"},
                {"xl/drawings/drawing1.xml", drawing}, {"xl/drawings/_rels/drawing1.xml.rels", imageRel},
                {"xl/media/image1.png", png}});
    }
}
void pdf(const QString& path, const QByteArray& png)
{
    QPdfWriter writer(path);
    writer.setResolution(100);
    QPainter painter(&writer);
    painter.drawImage(QRect(30, 100, 700, 210), QImage::fromData(png));
    writer.newPage();
    painter.setFont(QFont(u"Arial"_s, 24));
    painter.drawText(40, 80, u"This page has a native text layer"_s);
    painter.drawImage(QRect(30, 150, 700, 210), QImage::fromData(png));
    writer.newPage();
    painter.drawText(40, 80, u"Text only: nothing to recognise"_s);
    painter.end();
}
qint64 add(Store& store, const QString& path, const QString& text, qint64 generation = 1)
{
    const QFileInfo info(path);
    store.beginFile(path, info.size(), info.lastModified().toMSecsSinceEpoch(), "file-hash", u"test"_s, generation);
    ws::OcrPiece piece;
    piece.text = text;
    piece.score = 0.99f;
    piece.quad = QPolygonF(QRectF(10, 20, 300, 40));
    const qint64 id = store.putAsset(text.toUtf8(), QSize(1000, 300), {piece});
    store.addPage(id, u"test image"_s, {{u"kind"_s, u"image"_s}, {u"frame"_s, 0}});
    store.finishFile();
    return id;
}
QByteArray bigEndian(quint32 value)
{
    const char bytes[] {char(value >> 24), char(value >> 16), char(value >> 8), char(value)};
    return QByteArray(bytes, 4);
}
QByteArray pngChunk(const char* type, const QByteArray& data)
{
    const QByteArray body = QByteArray(type, 4) + data;
    return bigEndian(quint32(data.size())) + body
        + bigEndian(quint32(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(body.constData()),
            size_t(body.size()))));
}
// A small PNG file (a few kB) whose header says width x height: one bit per
// pixel, all zero, compressed.
QByteArray pngBomb(quint32 width, quint32 height)
{
    const QByteArray header = bigEndian(width) + bigEndian(height) + QByteArray("\x01\x00\x00\x00\x00", 5);
    const QByteArray rows(qsizetype(1 + (width + 7) / 8) * height, '\0');
    mz_ulong size = mz_compressBound(mz_ulong(rows.size()));
    QByteArray packed(qsizetype(size), '\0');
    if (mz_compress(reinterpret_cast<unsigned char*>(packed.data()), &size,
            reinterpret_cast<const unsigned char*>(rows.constData()), mz_ulong(rows.size()))
        != MZ_OK)
        throw std::runtime_error("png compress");
    packed.resize(qsizetype(size));
    return QByteArray("\x89PNG\r\n\x1a\n", 8) + pngChunk("IHDR", header) + pngChunk("IDAT", packed)
        + pngChunk("IEND", {});
}
int icacls(const QStringList& arguments)
{
    QProcess process;
    process.setStandardOutputFile(QProcess::nullDevice());
    process.start(u"icacls"_s, arguments);
    return process.waitForFinished(30000) ? process.exitCode() : -1;
}
// Indexes `folders` in this process (no worker, no OCR needed for files that
// fail to decode); returns the last progress message.
QString scanHere(const QString& index, const QStringList& folders, bool retry = false)
{
    ScanOptions options;
    options.folders = folders;
    options.retry = retry;
    QString message;
    scan(QCoreApplication::applicationDirPath(), index, options, [] { return false; },
        [&](const ScanProgress& p) {
            if (!p.message.isEmpty())
                message = p.message;
        });
    return message;
}
} // namespace

class TestOcrSearch : public QObject {
    Q_OBJECT
private slots:
    void normalizationAndPathBoundaries()
    {
        // Spaces next to Chinese or Japanese characters go, put there by OCR or
        // typed; spaces between other words stay, and Korean keeps its own.
        QCOMPARE(normalized(u"报 销 规定 ＡＢＣ １２３.４５"_s), u"报销规定abc 123.45"_s);
        QCOMPARE(normalized(u"第 3 頁 A4 紙"_s), u"第3页a4纸"_s);
        QCOMPARE(normalized(u"Win顺  插件\n说明"_s), u"win顺插件说明"_s);
        QCOMPARE(normalized(u"カタカナ テスト"_s), u"カタカナテスト"_s);
        QCOMPARE(normalized(u"Invoice  No. 12"_s), u"invoice no. 12"_s);
        QCOMPARE(normalized(u"한국어 문서 abc"_s), u"한국어문서 abc"_s);
        QCOMPARE(normalized(u"𠀀 abc"_s), u"𠀀abc"_s); // beyond the BMP
        // Traditional characters fold to simplified ones, through a chain too.
        QCOMPARE(normalized(u"報銷發票"_s), u"报销发票"_s);
        QCOMPARE(normalized(u"薴"_s), u"苎"_s);
        QCOMPARE(normalized(u"苧"_s), u"苎"_s);
        // One or two letters or digits are whole words; longer terms match anywhere.
        QVERIFY(containsTerm(u"a4纸第3页"_s, u"a4"_s));
        QVERIFY(containsTerm(u"a4纸第3页"_s, u"3"_s));
        QVERIFY(containsTerm(u"单价12.5元"_s, u"12"_s));
        QVERIFY(!containsTerm(u"ba4 123"_s, u"a4"_s));
        QVERIFY(!containsTerm(u"ba4 123"_s, u"12"_s));
        QVERIFY(containsTerm(u"ba4 123"_s, u"a4 1"_s));
        QVERIFY(containsTerm(u"invoice"_s, u"voi"_s));
        QVERIFY(inside(u"C:/Work/a.pdf"_s, u"c:/work"_s));
        QVERIFY(!inside(u"C:/Work-other/a.pdf"_s, u"C:/Work"_s));
    }
    void shortWordsAndTraditionalCharacters()
    {
        QTemporaryDir temp;
        const QString a = temp.filePath(u"a.png"_s), b = temp.filePath(u"b.png"_s);
        write(a, "a");
        write(b, "b");
        Store store(temp.filePath(u"index"_s));
        add(store, a, u"A4 紙 第 3 頁 Q3 報銷"_s);
        add(store, b, u"BA4 123 Q35 报销"_s);
        const auto found = [&](const QString& words) {
            QStringList names;
            for (const Hit& hit : store.search(words, 20))
                names.append(QFileInfo(hit.path).fileName());
            names.sort();
            return names.join(u',');
        };
        QCOMPARE(found(u"A4"_s), u"a.png"_s);
        QCOMPARE(found(u"a4纸"_s), u"a.png"_s);
        QCOMPARE(found(u"3"_s), u"a.png"_s);
        QCOMPARE(found(u"q3"_s), u"a.png"_s);
        QCOMPARE(found(u"12"_s), QString());
        QCOMPARE(found(u"123"_s), u"b.png"_s);
        QCOMPARE(found(u"报销"_s), u"a.png,b.png"_s);
        QCOMPARE(found(u"報銷"_s), u"a.png,b.png"_s);
        QCOMPARE(found(u"第3頁"_s), u"a.png"_s);
        QCOMPARE(found(u"\"第 3 页\""_s), u"a.png"_s);
        QCOMPARE(found(u"q3 报销"_s), u"a.png"_s);
        QCOMPARE(store.search(u"q3"_s, 20).first().snippet, u"A4 紙 第 3 頁 Q3 報銷"_s);
        Store reader(temp.filePath(u"index"_s), 0);
        QCOMPARE(reader.search(u"A4"_s, 20).size(), 1);
    }
    void shortChineseAndExactVerification()
    {
        QTemporaryDir temp;
        const QString file = temp.filePath(u"a.png"_s);
        write(file, "fixture");
        Store store(temp.filePath(u"index"_s));
        add(store, file, u"去年报销的规定 ABC 123.45"_s);
        for (const QString& word : {u"报"_s, u"报销"_s, u"报销的"_s, u"规定"_s, u"ABC"_s, u"123.45"_s, u"报销 abc"_s})
            QCOMPARE(store.search(word, 20).size(), 1);
        QVERIFY(store.search(u"报销abc"_s, 20).isEmpty());
        QVERIFY(store.search(u"报销 missing"_s, 20).isEmpty());
        QVERIFY(store.search(u"销报"_s, 20).isEmpty());
        QCOMPARE(store.search(u"\"报销的规定\""_s, 20).size(), 1);
        Store reader(temp.filePath(u"index"_s), 0);
        QCOMPARE(reader.search(u"规定"_s, 20).size(), 1);
    }
    void dedupCopyMoveAndPrune()
    {
        QTemporaryDir temp;
        const QString index = temp.filePath(u"index"_s);
        const QString a = temp.filePath(u"a.png"_s), b = temp.filePath(u"b.png"_s);
        write(a, "fixture");
        write(b, "fixture");
        Store store(index);
        const qint64 asset = add(store, a, u"报销规定"_s);
        const QFileInfo info(b);
        QVERIFY(store.reuse(b, info.size(), info.lastModified().toMSecsSinceEpoch(), "file-hash", u"test"_s, 2));
        QCOMPARE(store.search(u"报销"_s, 20).size(), 2);
        QCOMPARE(store.statistics().value(u"assets"_s).toInt(), 1);
        QCOMPARE(store.pieces(asset).first().text, u"报销规定"_s);
        store.prune(temp.path(), {pathKey(b)});
        QCOMPARE(store.search(u"报销"_s, 20).size(), 1);
        QCOMPARE(store.search(u"报销"_s, 20).first().path, QDir::toNativeSeparators(b));
        store.retainFolders({});
        store.compact();
        QCOMPARE(store.statistics().value(u"assets"_s).toInt(), 0);
        QVERIFY(QFileInfo::exists(a));
        QVERIFY(QFileInfo::exists(b));
    }
    void interruptedFileRollsBackAndChangedFilesDisappear()
    {
        QTemporaryDir temp;
        const QString file = temp.filePath(u"a.png"_s);
        write(file, "fixture");
        const QString index = temp.filePath(u"index"_s);
        {
            Store store(index);
            add(store, file, u"original"_s);
            store.beginFile(file, 7, 0, "new", u"test"_s, 2);
        }
        Store store(index);
        QCOMPARE(store.search(u"original"_s, 10).size(), 1);
        write(file, "changed fixture");
        QVERIFY(store.search(u"original"_s, 10).isEmpty());
        QVERIFY(!store.current(
            file, QFileInfo(file).size(), QFileInfo(file).lastModified().toMSecsSinceEpoch(), u"test"_s));
    }

    void termsMayMatchDifferentPagesOfOneFile()
    {
        QTemporaryDir temp;
        const QString file = temp.filePath(u"pages.pdf"_s);
        write(file, "fixture");
        const QFileInfo info(file);
        Store store(temp.filePath(u"index"_s));
        store.beginFile(file, info.size(), info.lastModified().toMSecsSinceEpoch(), "pages", u"test"_s, 1);
        ws::OcrPiece first;
        first.text = u"报销规定"_s;
        ws::OcrPiece second;
        second.text = u"731905 invoice"_s;
        store.addPage(
            store.putAsset("page1", QSize(100, 100), {first}), u"page 1"_s, {{u"kind"_s, u"pdf"_s}, {u"page"_s, 1}});
        store.addPage(
            store.putAsset("page2", QSize(100, 100), {second}), u"page 2"_s, {{u"kind"_s, u"pdf"_s}, {u"page"_s, 2}});
        store.finishFile();
        QCOMPARE(store.search(u"报销 731905"_s, 20).size(), 1);
        QCOMPARE(store.search(u"invoice"_s, 20).first().source.value(u"page"_s).toInt(), 2);
        QVERIFY(store.search(u"报销 missing"_s, 20).isEmpty());
        store.beginFile(file, info.size(), info.lastModified().toMSecsSinceEpoch(), "pages", u"test"_s, 2);
        {
            Store reader(temp.filePath(u"index"_s), 0);
            QCOMPARE(reader.search(u"报销 731905"_s, 20).size(), 1);
        }
        store.rollback();
    }

    void warmQueriesOnTenThousandSyntheticEntries()
    {
        QTemporaryDir temp;
        const QString folder = temp.filePath(u"files"_s);
        QDir().mkpath(folder);
        Store store(temp.filePath(u"index"_s));
        for (int i = 0; i < 10000; ++i) {
            const QString path = QDir(folder).filePath(u"document-%1.png"_s.arg(i, 5, 10, QChar(u'0')));
            write(path, "fixture");
            add(store, path, u"年度报销规定 INVOICE %1 批次"_s.arg(i, 5, 10, QChar(u'0')));
        }
        QList<qint64> timings;
        for (int i = 0; i < 20; ++i) {
            QElapsedTimer timer;
            timer.start();
            QCOMPARE(store.search(i % 2 ? u"报销"_s : u"INVOICE 02731"_s, 100).size(), i % 2 ? 100 : 1);
            timings.append(timer.elapsed());
        }
        std::sort(timings.begin(), timings.end());
        qInfo() << "Synthetic 10000-entry index (not OCR throughput), bytes:"
                << store.statistics().value(u"bytes"_s).toInteger() << "warm query P95 ms:" << timings[18]
                << "max ms:" << timings.last();
        QVERIFY2(store.statistics().value(u"bytes"_s).toInteger() < 11 * 1024 * 1024,
            "Synthetic index exceeded the storage regression budget");
        QVERIFY2(timings[18] < 250, "Warm query P95 exceeds the desktop interaction budget");
    }
    void storageWithDensePages()
    {
        QTemporaryDir temp;
        Store store(temp.filePath(u"index"_s));
        quint32 random = 42;
        for (int i = 0; i < 500; ++i) {
            const QString path = temp.filePath(u"scan-%1.png"_s.arg(i));
            write(path, "fixture");
            const QFileInfo info(path);
            store.beginFile(path, info.size(), info.lastModified().toMSecsSinceEpoch(), QByteArray::number(i), u"test"_s, 1);
            QList<ws::OcrPiece> pieces;
            for (int line = 0; line < 40; ++line) {
                ws::OcrPiece piece;
                piece.text = u"报销规定 INVOICE 申请编号 %1 %2 "_s.arg(i).arg(line);
                for (int c = 0; c < 16; ++c) {
                    random = random * 1664525u + 1013904223u;
                    piece.text += QChar(0x4e00 + (random >> 16) % 400);
                }
                piece.score = 0.95f;
                piece.quad = {QPointF(10, line * 20), QPointF(700, line * 20),
                    QPointF(700, line * 20 + 18), QPointF(10, line * 20 + 18)};
                pieces.append(piece);
            }
            store.addPage(store.putAsset(QByteArray::number(i), QSize(800, 1000), pieces), u"图片"_s, {{u"kind"_s, u"image"_s}});
            store.finishFile();
        }
        store.compact();
        const qint64 bytes = store.statistics().value(u"bytes"_s).toInteger();
        qInfo() << "Synthetic dense 500-page index bytes:" << bytes;
        QVERIFY2(bytes < 8 * 1024 * 1024, "Dense index exceeded the storage regression budget");
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(store.search(u"INVOICE 申请编号 271"_s, 100).size(), 1);
        qInfo() << "Dense index selective query ms:" << timer.elapsed();
        QCOMPARE(store.pieces(store.search(u"INVOICE 申请编号 271"_s, 100).first().asset).size(), 40);
        QSet<QString> seen;
        for (int i = 250; i < 500; ++i)
            seen.insert(pathKey(temp.filePath(u"scan-%1.png"_s.arg(i))));
        store.prune(temp.path(), seen);
        store.compact();
        QCOMPARE(store.statistics().value(u"assets"_s).toInt(), 250);
        QVERIFY(store.statistics().value(u"bytes"_s).toInteger() < bytes);
        QCOMPARE(store.search(u"INVOICE 申请编号 271"_s, 100).size(), 1);
        QVERIFY(store.search(u"INVOICE 申请编号 171"_s, 100).isEmpty());
        store.retainFolders({});
        store.compact();
        QCOMPARE(store.statistics().value(u"assets"_s).toInt(), 0);
        qInfo() << "After clearing, bytes:" << store.statistics().value(u"bytes"_s).toInteger();
        QVERIFY(store.statistics().value(u"bytes"_s).toInteger() < 256 * 1024);
    }

    void legacyIndexMigratesWithoutReadingSources()
    {
        QTemporaryDir temp;
        const QString index = temp.filePath(u"index"_s), path = temp.filePath(u"scan.png"_s);
        write(path, "fixture");
        qint64 asset = 0;
        {
            Store store(index);
            asset = add(store, path, u"报销规定 INVOICE 731905"_s);
        }
        // Restore the actual version-1 gram table and payload format.
        {
            auto db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"legacy-fixture"_s);
            db.setDatabaseName(QDir(index).filePath(u"ocr-index.sqlite"_s));
            QVERIFY(db.open());
            QSqlQuery q(db), assets(db);
            QVERIFY(q.exec(u"DROP TABLE asset_terms"_s));
            QVERIFY(q.exec(u"CREATE TABLE grams(gram INTEGER,asset INTEGER REFERENCES assets(id) ON DELETE CASCADE,PRIMARY KEY(gram,asset)) WITHOUT ROWID"_s));
            QVERIFY(assets.exec(u"SELECT id,text FROM assets"_s));
            q.prepare(u"INSERT INTO grams VALUES(?,?)"_s);
            while (assets.next()) {
                const QString text = QString::fromUtf8(qUncompress(assets.value(1).toByteArray()));
                auto keys = ws::grams::ofPhrase(text);
                for (auto c : text.toUcs4())
                    if (ws::grams::isIndexed(c))
                        keys.push_back(ws::grams::Key(c) << ws::grams::kCharBits);
                std::sort(keys.begin(), keys.end());
                keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
                for (auto key : keys) {
                    q.bindValue(0, qlonglong(key));
                    q.bindValue(1, assets.value(0));
                    QVERIFY(q.exec());
                }
            }
            assets.finish();
            QVERIFY(q.exec(u"PRAGMA user_version=1"_s));
        }
        QSqlDatabase::removeDatabase(u"legacy-fixture"_s);
        const QString away = temp.filePath(u"offline.png"_s);
        QVERIFY(QFile::rename(path, away));
        {
            Store upgraded(index); // Original file is unavailable throughout migration.
            QCOMPARE(upgraded.pieces(asset).first().text, u"报销规定 INVOICE 731905"_s);
        }
        QVERIFY(QFile::rename(away, path));
        {
            Store reader(index, 0);
            QCOMPARE(reader.search(u"报"_s, 10).size(), 1);
            QCOMPARE(reader.search(u"报销 invoice 731905"_s, 10).size(), 1);
            QVERIFY(reader.search(u"missing"_s, 10).isEmpty());
        }
        {
            Store reopened(index);
            QCOMPARE(reopened.search(u"报销"_s, 10).size(), 1);
            reopened.retainFolders({});
            reopened.compact();
            add(reopened, path, u"更新 864201"_s); // SQLite can reuse a deleted asset ID.
            QVERIFY(reopened.search(u"731905"_s, 10).isEmpty());
            QCOMPARE(reopened.search(u"864201"_s, 10).size(), 1);
        }
    }

    void capacityUsesActualPageSizeAndNewerSchemasAreUntouched()
    {
        QTemporaryDir temp;
        const QString index = temp.filePath(u"index"_s);
        QDir().mkpath(index);
        {
            auto db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"page-fixture"_s);
            db.setDatabaseName(QDir(index).filePath(u"ocr-index.sqlite"_s));
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(u"PRAGMA page_size=8192"_s));
            QVERIFY(q.exec(u"VACUUM"_s));
        }
        QSqlDatabase::removeDatabase(u"page-fixture"_s);
        {
            Store store(index, 16);
            // max_page_count is connection-local: the write boundary must use
            // Store's real connection, whose path can be identified here.
            for (const QString& name : QSqlDatabase::connectionNames()) {
                auto db = QSqlDatabase::database(name);
                if (db.databaseName() != QDir(index).filePath(u"ocr-index.sqlite"_s))
                    continue;
                QSqlQuery q(db);
                QVERIFY(q.exec(u"PRAGMA max_page_count"_s));
                QVERIFY(q.next());
                QCOMPARE(q.value(0).toInt(), 2048);
                q.finish();
                QVERIFY(!q.exec(u"INSERT INTO assets(hash,text,pieces) VALUES(x'01',zeroblob(20000000),x'00')"_s));
                QCOMPARE(store.statistics().value(u"assets"_s).toInt(), 0);
                QVERIFY(q.exec(u"PRAGMA user_version=99"_s));
            }
        }
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Store {index});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (Store {index, 0}));
        auto db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"future-fixture"_s);
        db.setDatabaseName(QDir(index).filePath(u"ocr-index.sqlite"_s));
        QVERIFY(db.open());
        {
            QSqlQuery q(db);
            QVERIFY(q.exec(u"PRAGMA user_version"_s));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 99);
        }
        db.close();
        db = {};
        QSqlDatabase::removeDatabase(u"future-fixture"_s);
    }

    void changedRulesRebuildFromStoredLines()
    {
        QTemporaryDir temp;
        const QString index = temp.filePath(u"index"_s), path = temp.filePath(u"scan.png"_s);
        const QString line = u"報銷規定 INVOICE 731905 A4 紙"_s;
        write(path, "fixture");
        qint64 asset = 0;
        {
            Store store(index);
            asset = add(store, path, line);
        }
        // An index written under other rules: its postings are useless, and its
        // text normalized the old way (traditional characters, spaces kept).
        const auto storedText = [&](const QString& connection) {
            auto db = QSqlDatabase::addDatabase(u"QSQLITE"_s, connection);
            db.setDatabaseName(QDir(index).filePath(u"ocr-index.sqlite"_s));
            if (!db.open())
                return QString();
            QSqlQuery q(db);
            q.prepare(u"SELECT text FROM assets WHERE id=?"_s);
            q.addBindValue(asset);
            return q.exec() && q.next() ? QString::fromUtf8(qUncompress(q.value(0).toByteArray())) : QString();
        };
        {
            auto db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"rules-fixture"_s);
            db.setDatabaseName(QDir(index).filePath(u"ocr-index.sqlite"_s));
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(u"INSERT INTO asset_terms(asset_terms) VALUES('delete-all')"_s));
            QVERIFY(q.exec(u"UPDATE meta SET value='1:old' WHERE key='terms'"_s));
            QCOMPARE(q.numRowsAffected(), 1);
            q.prepare(u"UPDATE assets SET text=? WHERE id=?"_s);
            q.addBindValue(qCompress(u"報銷規定 invoice 731905 a4 紙"_s.toUtf8(), 6));
            q.addBindValue(asset);
            QVERIFY(q.exec());
        }
        QSqlDatabase::removeDatabase(u"rules-fixture"_s);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (Store {index, 0}));
        const QString away = temp.filePath(u"offline.png"_s);
        QVERIFY(QFile::rename(path, away));
        {
            Store store(index); // rebuilds without the original file or OCR
            QCOMPARE(store.pieces(asset).first().text, line);
        }
        QCOMPARE(storedText(u"rules-text"_s), normalized(line));
        QSqlDatabase::removeDatabase(u"rules-text"_s);
        QVERIFY(QFile::rename(away, path)); // a hit is shown only for the file still there
        Store reader(index, 0);
        QCOMPARE(reader.search(u"报销 invoice"_s, 10).size(), 1);
        QCOMPARE(reader.search(u"731905"_s, 10).size(), 1);
        QCOMPARE(reader.search(u"A4"_s, 10).size(), 1);
        QCOMPARE(reader.search(u"规定invoice"_s, 10).size(), 1);
    }

    void migrationFailureRollsBackLegacySchema()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath(u"ocr-index.sqlite"_s);
        {
            auto db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"full-legacy"_s);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(u"CREATE TABLE grams(gram INTEGER,asset INTEGER,PRIMARY KEY(gram,asset)) WITHOUT ROWID"_s));
            QVERIFY(q.exec(u"INSERT INTO grams VALUES(123,1)"_s));
            QVERIFY(q.exec(u"CREATE TABLE filler(data BLOB)"_s));
            QVERIFY(q.exec(u"INSERT INTO filler VALUES(zeroblob(18000000))"_s));
            QVERIFY(q.exec(u"PRAGMA user_version=1"_s));
        }
        QSqlDatabase::removeDatabase(u"full-legacy"_s);
        // Lowering the budget below this existing database leaves no room for
        // migration. The original recognition/index must remain recoverable.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (Store {temp.path(), 16}));
        {
            auto db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"check-legacy"_s);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(u"PRAGMA user_version"_s));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 1);
            QVERIFY(q.exec(u"SELECT gram,asset FROM grams"_s));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 123);
            QCOMPARE(q.value(1).toInt(), 1);
            QVERIFY(q.exec(u"SELECT count(*) FROM sqlite_master WHERE name='asset_terms'"_s));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 0);
        }
        QSqlDatabase::removeDatabase(u"check-legacy"_s);
    }

    // Opening an index of the pipeline before (ocr-search-2) marks to be read
    // again only the files where what it left out can matter; the rest keeps
    // its rows and is not read again. Nothing is decoded or read here.
    void upgradeMarksOnlyWhatTheOldPipelineLeftOut()
    {
        QTemporaryDir temp;
        const QString index = temp.filePath(u"index"_s);
        const QString older = QString(ws::ocr::kVersion) + u"/ocr-search-2"_s;
        struct Row {
            QString name, status, error, kind;
            QSize size;
            QList<float> scores;
            bool again;
        };
        const QList<Row> rows {
            // Made smaller to be read: a photo stays as read; a long picture
            // and a page rendered at the old size are read again.
            {u"photo.jpg"_s, u"ready"_s, {}, u"image"_s, {4096, 3072}, {0.98f, 0.97f}, false},
            {u"chat.png"_s, u"ready"_s, {}, u"image"_s, {1100, 4096}, {0.96f, 0.95f}, true},
            {u"slides.pptx"_s, u"ready"_s, {}, u"office"_s, {4096, 1200}, {0.97f, 0.98f}, true},
            {u"poster.pdf"_s, u"ready"_s, {}, u"pdf"_s, {1839, 2600}, {0.99f, 0.98f}, true},
            {u"letter.pdf"_s, u"ready"_s, {}, u"pdf"_s, {1240, 1754}, {0.99f, 0.99f}, false},
            {u"plain.png"_s, u"ready"_s, {}, u"image"_s, {1200, 800}, {0.99f, 0.99f}, false},
            // A PDF without pictures may draw its words; an Office file
            // without pictures has nothing to read.
            {u"drawn.pdf"_s, u"empty"_s, {}, {}, {}, {}, true},
            {u"memo.docx"_s, u"empty"_s, {}, {}, {}, {}, false},
            // Given up for want of progress, in either language; a damaged file is not tried again.
            {u"dense.png"_s, u"failed"_s, u"文件处理 60 秒没有进展，已跳过；可用“重新检查所有文件”重试"_s, {}, {}, {},
                true},
            {u"dense-en.png"_s, u"failed"_s,
                u"No progress on the file for 60 seconds; skipped. Use “Recheck all files” to try again"_s, {}, {}, {},
                true},
            {u"damaged.png"_s, u"failed"_s, u"文件读取不完整"_s, {}, {}, {}, false},
            // Read poorly, maybe upside down; one poor line is not enough to tell.
            {u"turned.png"_s, u"ready"_s, {}, u"image"_s, {1000, 700}, {0.52f, 0.61f, 0.58f}, true},
            {u"one-line.png"_s, u"ready"_s, {}, u"image"_s, {1000, 700}, {0.41f}, false},
        };
        const auto hashOf
            = [](const QString& text) { return QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256); };
        // Real files: a search only answers for a file as it was read.
        const auto sizeOf = [&](const QString& name) { return QFileInfo(temp.filePath(name)).size(); };
        const auto modifiedOf
            = [&](const QString& name) { return QFileInfo(temp.filePath(name)).lastModified().toMSecsSinceEpoch(); };
        int again = 0;
        {
            Store store(index);
            QCOMPARE(store.recheckRemaining(), 0);
            for (const Row& row : rows) {
                const QString path = temp.filePath(row.name);
                write(path, row.name.toUtf8());
                again += row.again;
                if (row.status == u"failed") {
                    store.status(path, sizeOf(row.name), modifiedOf(row.name), older, 1, row.status, row.error);
                    continue;
                }
                store.beginFile(path, sizeOf(row.name), modifiedOf(row.name), hashOf(path), older, 1);
                if (!row.kind.isEmpty()) {
                    QList<ws::OcrPiece> pieces;
                    for (const float score : row.scores) {
                        ws::OcrPiece piece;
                        piece.text = QFileInfo(row.name).completeBaseName() + u" 731905"_s;
                        piece.score = score;
                        piece.quad = QPolygonF(QRectF(10, 20 + 40 * pieces.size(), 300, 30));
                        pieces.append(piece);
                    }
                    store.addPage(
                        store.putAsset(hashOf(row.name), row.size, pieces), u"page"_s, {{u"kind"_s, row.kind}});
                }
                store.finishFile(row.status);
            }
        }
        {
            auto db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"pipeline-2"_s);
            db.setDatabaseName(QDir(index).filePath(u"ocr-index.sqlite"_s));
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(u"PRAGMA user_version=2"_s));
        }
        QSqlDatabase::removeDatabase(u"pipeline-2"_s);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (Store {index, 0})); // searching waits for the upgrade
        {
            Store upgraded(index);
            for (const Row& row : rows) {
                const QString state
                    = upgraded.state(temp.filePath(row.name), sizeOf(row.name), modifiedOf(row.name), pipelineEngine());
                QVERIFY2(state == (row.again ? QString() : row.status), qPrintable(row.name + u": "_s + state));
                if (!row.kind.isEmpty())
                    QVERIFY2(
                        (upgraded.asset(hashOf(row.name)) == 0) == (row.name == u"turned.png"), qPrintable(row.name));
            }
            QCOMPARE(upgraded.recheckRemaining(), again);
            QCOMPARE(upgraded.statistics().value(u"recheck"_s).toInt(), again);
            // Until it is read again, the poorly read picture still answers searches.
            QCOMPARE(upgraded.search(u"turned"_s, 10).size(), 1);
            // Nor is a file to be read again called one without words: the
            // drawn PDF may well have them then; the memo stays listed.
            const QString textless = upgraded.textlessText();
            QVERIFY2(!textless.contains(u"drawn.pdf"_s), qPrintable(textless));
            QVERIFY2(textless.contains(u"memo.docx"_s), qPrintable(textless));
            QCOMPARE(upgraded.statistics().value(u"textless"_s).toInt(), 1);
        }
        {
            Store reopened(index); // upgraded once: nothing more is marked
            QCOMPARE(reopened.recheckRemaining(), again);
            QVERIFY(reopened.asset(hashOf(u"one-line.png"_s)));
            reopened.recheckDone();
            QCOMPARE(reopened.statistics().value(u"recheck"_s).toInt(), 0);
        }
        Store reader(index, 0);
        QCOMPARE(reader.search(u"731905"_s, 20).size(), 8);
    }

    // The files where nothing was found to read, each with why: documents by
    // name, pictures without words only counted.
    void filesWithoutWordsAreListedWithWhy()
    {
        QTemporaryDir temp;
        Store store(temp.filePath(u"index"_s));
        const auto file = [&](const QString& name, const QString& status, const QString& kind, const QString& text) {
            const QString path = temp.filePath(name);
            store.beginFile(
                path, 100, 1, QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256), pipelineEngine(), 1);
            if (!kind.isEmpty()) {
                QList<ws::OcrPiece> pieces;
                if (!text.isEmpty()) {
                    ws::OcrPiece piece;
                    piece.text = text;
                    piece.score = 0.98f;
                    piece.quad = QPolygonF(QRectF(10, 20, 300, 40));
                    pieces.append(piece);
                }
                const QByteArray hash = QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256);
                store.addPage(store.putAsset(hash, QSize(800, 600), pieces), u"page"_s, {{u"kind"_s, kind}});
            }
            store.finishFile(status);
        };
        file(u"drawn.pdf"_s, u"empty"_s, {}, {});
        file(u"memo.docx"_s, u"empty"_s, {}, {});
        file(u"blank-scan.pdf"_s, u"ready"_s, u"pdf"_s, {});
        file(u"holiday.jpg"_s, u"ready"_s, u"image"_s, {});
        file(u"invoice.png"_s, u"ready"_s, u"image"_s, u"INVOICE 731905"_s);
        store.status(temp.filePath(u"broken.png"_s), 10, 1, pipelineEngine(), 1, u"failed"_s, u"文件读取不完整"_s);
        const QJsonObject statistics = store.statistics();
        QCOMPARE(statistics.value(u"textless"_s).toInt(), 4);
        QCOMPARE(statistics.value(u"unfinished"_s).toInt(), 1);
        const QString text = store.textlessText();
        QVERIFY2(text.contains(u"drawn.pdf\n没有要识别的页面：字都在 PDF 的文字层里"_s), qPrintable(text));
        QVERIFY2(text.contains(u"memo.docx\n文档里没有图片"_s), qPrintable(text));
        QVERIFY2(text.contains(u"blank-scan.pdf\n识别过了，没有认出文字"_s), qPrintable(text));
        QVERIFY2(text.endsWith(u"另有 1 张图片识别过了，没有认出文字"_s), qPrintable(text));
        for (const QString& name : {u"holiday.jpg"_s, u"invoice.png"_s, u"broken.png"_s})
            QVERIFY2(!text.contains(name), qPrintable(name));
    }

    void filtersAndCancellation()
    {
        QTemporaryDir temp;
        const QString a = temp.filePath(u"a.png"_s), b = temp.filePath(u"b.pdf"_s);
        write(a, "a");
        write(b, "b");
        Store store(temp.filePath(u"index"_s));
        add(store, a, u"报销"_s);
        add(store, b, u"报销"_s);
        QCOMPARE(store.search(u"报销"_s, 20, {{u"extensions"_s, QJsonArray {u"pdf"_s}}}).size(), 1);
        QVERIFY(store.search(u"报销"_s, 20, {{u"folder"_s, temp.filePath(u"missing"_s)}}).isEmpty());
        QVERIFY(store.search(u"报销"_s, 20, {}, [] { return true; }).isEmpty());
        QCOMPARE(store.search(u"报销"_s, 1).size(), 1);
        QCOMPARE(store.search(u"报销"_s, 20, {{u"excludedExtensions"_s, QJsonArray {u"png"_s}}}).size(), 1);
        QVERIFY(store.search(u"报销"_s, 20, {{u"excludedFileNames"_s, QJsonArray {u"a.*"_s, u"b.*"_s}}}).isEmpty());
        QVERIFY(store.search(u"报销"_s, 20, {{u"maxSize"_s, 0}}).isEmpty());
        const QString modified = QFileInfo(a).lastModified().toUTC().toString(Qt::ISODateWithMs);
        QVERIFY(store.search(u"报销"_s, 20, {{u"extensions"_s, QJsonArray {u"png"_s}}, {u"modifiedTo"_s, modified}})
                .isEmpty());
        QCOMPARE(store.search(u"报销"_s, 20, {{u"extensions"_s, QJsonArray {u"png"_s}}, {u"modifiedFrom"_s, modified}})
                     .size(),
            1);
    }
    void sourceImagesAndOfficeLocations()
    {
        QTemporaryDir temp;
        const QByteArray png = imageBytes();
        const QString image = temp.filePath(u"source.png"_s);
        write(image, png);
        int count = 0;
        const auto direct = extractImages(image, [&](const QImage& decoded, const QString&, const QJsonObject& source) {
            ++count;
            QCOMPARE(decoded.size(), QSize(1000, 300));
            QCOMPARE(previewImage(image, source).size(), decoded.size());
        });
        QCOMPARE(count, 1);
        QCOMPARE(direct.images, 1);
        for (const QString& extension : {u"docx"_s, u"pptx"_s, u"xlsx"_s}) {
            const QString path = temp.filePath(u"embedded."_s + extension);
            office(path, png);
            const auto result
                = extractImages(path, [&](const QImage& decoded, const QString& label, const QJsonObject& source) {
                      QCOMPARE(decoded.size(), QSize(1000, 300));
                      QCOMPARE(previewImage(path, source).size(), decoded.size());
                      if (extension == u"pptx")
                          QCOMPARE(source.value(u"slide"_s).toInt(), 1);
                      if (extension == u"xlsx")
                          QVERIFY(label.contains(u"Travel"));
                      QVERIFY(source.value(u"part"_s).toString().endsWith(u"image1.png"));
                  });
            QCOMPARE(result.images, 1);
            QVERIFY(!result.partial);
        }
    }
    void scannedAndMixedPdfBothRender()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath(u"mixed.pdf"_s);
        pdf(path, imageBytes());
        QList<int> pages;
        const auto result = extractImages(path, [&](const QImage& image, const QString&, const QJsonObject& source) {
            QVERIFY(!image.isNull());
            QVERIFY(image.width() <= 2600);
            QVERIFY(image.height() <= 2600);
            const int page = source.value(u"page"_s).toInt();
            pages.append(page);
            QCOMPARE(previewImage(path, source).size(), image.size());
        });
        QCOMPARE(result.images, 2); // page 3 has no picture: not rendered
        QCOMPARE(pages, QList<int>({1, 2}));
    }
    // Without pictures, a page is read only when it shows words its text layer
    // does not hold; an ordinary text page is left to the content index.
    void pagesWithWordsBeyondTheirTextLayer()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath(u"rule.pdf"_s);
        {
            QPdfWriter writer(path);
            writer.setResolution(100);
            QPainter painter(&writer);
            const QFont font(u"Arial"_s, 24);
            painter.setFont(font);
            // 1: an ordinary text page, a rule under its heading.
            painter.drawText(40, 80, u"An ordinary page keeps its words as text"_s);
            painter.drawLine(40, 100, 700, 100);
            writer.newPage();
            // 2: lettering turned into outlines, as design and CAD exports do.
            QPainterPath outlined;
            outlined.addText(40, 80, font, u"OUTLINED 731905"_s);
            painter.fillPath(outlined, Qt::black);
            writer.newPage();
            // 3: a drawing with a short caption as real text.
            painter.drawText(40, 80, u"Fig 3"_s);
            painter.drawRect(40, 120, 400, 200);
            writer.newPage();
            // 4: a short line of text and nothing else.
            painter.drawText(40, 80, u"Notes"_s);
            writer.newPage();
            // 5: blank.
            painter.end();
        }
        QList<int> pages;
        const auto result = extractImages(path, [&](const QImage& image, const QString&, const QJsonObject& source) {
            QVERIFY(std::max(image.width(), image.height()) <= 2600);
            pages.append(source.value(u"page"_s).toInt());
        });
        QCOMPARE(pages, QList<int>({2, 3}));
        QCOMPARE(result.images, 2);
        QVERIFY(!result.partial);
    }
    // A long picture keeps its width, up to the pixels the reader takes; a
    // photo is read at 4096 on its longer side as before. The size as stored
    // goes with it, to tell later which pictures were made smaller.
    void longPicturesKeepTheirWidth()
    {
        QTemporaryDir temp;
        const auto read = [&](const QString& name, QSize size, QSize* stored) {
            QImage picture(size, QImage::Format_RGB32);
            picture.fill(Qt::white);
            QPainter painter(&picture);
            painter.drawText(20, 40, u"LONG 731905"_s);
            painter.end();
            const QString path = temp.filePath(name);
            if (!picture.save(path))
                return QSize();
            QSize read;
            extractImages(path, [&](const QImage& image, const QString&, const QJsonObject& source) {
                read = image.size();
                const QJsonArray size = source.value(u"size"_s).toArray();
                *stored = QSize(size.at(0).toInt(), size.at(1).toInt());
            });
            return read;
        };
        QSize stored;
        QCOMPARE(read(u"chat.png"_s, QSize(800, 12000), &stored), QSize(800, 12000));
        QCOMPARE(stored, QSize(800, 12000));
        QCOMPARE(read(u"photo.png"_s, QSize(6000, 4000), &stored), QSize(4096, 2731));
        QCOMPARE(stored, QSize(6000, 4000));
        const QSize huge = read(u"scroll.png"_s, QSize(3000, 15000), &stored); // 45 million pixels
        QCOMPARE(stored, QSize(3000, 15000));
        const qint64 pixels = qint64(huge.width()) * huge.height();
        QVERIFY2(pixels <= ws::PaddleOcr::kMaxPixels && pixels > ws::PaddleOcr::kMaxPixels * 99 / 100,
            qPrintable(u"%1x%2"_s.arg(huge.width()).arg(huge.height())));
        QVERIFY(std::abs(huge.height() - 5 * huge.width()) <= 5);
    }
    // A big page renders as finely as the picture placed on it, up to 200 dpi
    // and the pixels the reader takes; other pages render as before.
    void bigPdfPagesKeepTheirPicturesDetail()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath(u"big.pdf"_s);
        const auto scan = [](QSize size) {
            QImage picture(size, QImage::Format_RGB32);
            picture.fill(Qt::white);
            QPainter painter(&picture);
            painter.drawRect(10, 10, size.width() / 2, size.height() / 2);
            return picture;
        };
        {
            QPdfWriter writer(path);
            writer.setResolution(72); // in points
            writer.setPageMargins({});
            writer.setPageSize(QPageSize(QPageSize::A3));
            QPainter painter(&writer);
            painter.drawImage(QRectF(0, 0, 842, 1191), scan({2339, 3308})); // 1: scanned at 200 dpi
            writer.newPage();
            painter.drawImage(QRectF(0, 0, 842, 1191), scan({600, 848})); // 2: at 51 dpi
            writer.setPageSize(QPageSize(QPageSize::A0));
            writer.newPage();
            painter.drawImage(QRectF(100, 100, 100, 100), scan({278, 278})); // 3: a fine picture on A0
            writer.setPageSize(QPageSize(QPageSize::A4));
            writer.newPage();
            painter.drawImage(QRectF(0, 0, 595, 842), scan({2480, 3508})); // 4: A4 scanned at 300 dpi
            painter.end();
        }
        QList<QSize> sizes;
        QList<QJsonObject> sources;
        const auto result = extractImages(path, [&](const QImage& image, const QString&, const QJsonObject& source) {
            sizes.append(image.size());
            sources.append(source);
        });
        QCOMPARE(result.images, 4);
        // A page's size in points need not be whole: a pixel either way.
        const auto roughly = [](QSize size, QSize expected) {
            return std::abs(size.width() - expected.width()) <= 1 && std::abs(size.height() - expected.height()) <= 1;
        };
        const auto text = [](QSize size) { return u"%1x%2"_s.arg(size.width()).arg(size.height()).toUtf8(); };
        QVERIFY2(roughly(sizes[0], {2339, 3308}), text(sizes[0]).constData());
        QVERIFY2(roughly(sizes[1], {1839, 2600}), text(sizes[1]).constData()); // as before
        const qint64 pixels = qint64(sizes[2].width()) * sizes[2].height();
        QVERIFY2(pixels <= ws::PaddleOcr::kMaxPixels && pixels > ws::PaddleOcr::kMaxPixels * 99 / 100,
            text(sizes[2]).constData());
        QVERIFY2(roughly(sizes[3], {1653, 2339}), text(sizes[3]).constData()); // 200 dpi, as before
        const QJsonArray points = sources[0].value(u"points"_s).toArray();
        QCOMPARE(QSize(points.at(0).toInt(), points.at(1).toInt()), QSize(842, 1191));
        QCOMPARE(sources[0].value(u"pictureDpi"_s).toInt(), 200);
        const QImage preview = previewImage(path, sources[0]);
        QVERIFY2(roughly(preview.size(), sizes[1]), text(preview.size()).constData()); // the preview as before
    }
    void externalImageIsNotRead()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath(u"external.docx"_s);
        zip(path,
            {{"word/document.xml", "<root xmlns:r='r'><blip r:embed='external'/></root>"},
                {"word/_rels/document.xml.rels",
                    "<Relationships><Relationship Id='external' Type='a/image' "
                    "Target='https://example.invalid/private.png' TargetMode='External'/></Relationships>"}});
        const auto result = extractImages(path,
            [](const QImage&, const QString&, const QJsonObject&) { QFAIL("External image must not be loaded"); });
        QCOMPARE(result.images, 0);
    }
    void damagedSourcesAndCancellation()
    {
        QTemporaryDir temp;
        for (const QString& extension : {u"png"_s, u"pdf"_s, u"docx"_s}) {
            const QString path = temp.filePath(u"broken."_s + extension);
            write(path, "not a document");
            bool failed = false;
            try {
                extractImages(path, [](const QImage&, const QString&, const QJsonObject&) { });
            } catch (const std::exception&) {
                failed = true;
            }
            QVERIFY(failed);
        }
        const QString path = temp.filePath(u"image.png"_s);
        write(path, imageBytes());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
            extractImages(path, [](const QImage&, const QString&, const QJsonObject&) {}, [] { return true; }));
    }
    void unreadableFolderOnlyKeepsItsOwnEntries()
    {
        QTemporaryDir temp;
        const QString root = temp.filePath(u"root"_s), locked = temp.filePath(u"root/locked"_s);
        const QString index = temp.filePath(u"index"_s);
        QVERIFY(QDir().mkpath(locked));
        QVERIFY(QDir().mkpath(temp.filePath(u"root/open"_s)));
        // Undecodable pictures: recorded as failed without any OCR.
        write(QDir(locked).filePath(u"x.png"_s), "broken");
        write(temp.filePath(u"root/open/y.png"_s), "broken");
        write(temp.filePath(u"root/m.png"_s), "broken");
        scanHere(index, {root});
        {
            Store store(index, 0);
            QCOMPARE(store.statistics().value(u"failed"_s).toInt(), 3);
        }
        // Listing `locked` is now refused (like System Volume Information).
        const QString everyone = u"*S-1-1-0"_s;
        QCOMPARE(icacls({QDir::toNativeSeparators(locked), u"/deny"_s, everyone + u":(RX)"_s}), 0);
        const auto unlock = qScopeGuard([&] { icacls({QDir::toNativeSeparators(locked), u"/remove:d"_s, everyone}); });
        QVERIFY(QDir(locked).entryList(QDir::Files).isEmpty());
        write(temp.filePath(u"root/open/new.png"_s), "broken");
        write(temp.filePath(u"root/z.png"_s), "broken");
        QVERIFY(QFile::remove(temp.filePath(u"root/m.png"_s)));
        const QString message = scanHere(index, {root});
        QVERIFY2(message.contains(u"部分目录无法读取"), qPrintable(message));
        Store store(index, 0);
        const QString errors = store.errorText();
        // The rest of the root was walked: new files indexed, a deleted one pruned,
        // and the unreadable folder's entry kept.
        QCOMPARE(store.statistics().value(u"failed"_s).toInt(), 4);
        QVERIFY2(errors.contains(u"new.png"), qPrintable(errors));
        QVERIFY2(errors.contains(u"z.png"), qPrintable(errors));
        QVERIFY2(errors.contains(u"x.png"), qPrintable(errors));
        QVERIFY2(!errors.contains(u"m.png"), qPrintable(errors));
    }

    void failedUnchangedFileIsNotRedone()
    {
        QTemporaryDir temp;
        const QString root = temp.filePath(u"root"_s), index = temp.filePath(u"index"_s);
        QVERIFY(QDir().mkpath(root));
        const QString bad = QDir(root).filePath(u"bad.png"_s);
        write(bad, "broken");
        scanHere(index, {root});
        QString first;
        {
            Store store(index, 0);
            QCOMPARE(store.statistics().value(u"failed"_s).toInt(), 1);
            first = store.errorText();
        }
        // A read now fails differently (sharing violation): a redo would show.
        const HANDLE exclusive = ::CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(bad).utf16()),
            GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(exclusive != INVALID_HANDLE_VALUE);
        const auto close = qScopeGuard([exclusive] { ::CloseHandle(exclusive); });
        scanHere(index, {root});
        {
            Store store(index, 0);
            QCOMPARE(store.errorText(), first);
        }
        scanHere(index, {root}, true); // "recheck all files"
        Store store(index, 0);
        QVERIFY2(store.errorText() != first, qPrintable(store.errorText()));
        QCOMPARE(store.statistics().value(u"failed"_s).toInt(), 1);
    }

    void workerTroubleIsNotBlamedOnTheFile()
    {
        QTemporaryDir temp;
        const QString root = temp.filePath(u"root"_s), index = temp.filePath(u"index"_s);
        QVERIFY(QDir().mkpath(root));
        write(QDir(root).filePath(u"fine.png"_s), pngBomb(64, 32));
        ScanOptions options;
        options.folders = {root};
        options.workerProgram = temp.filePath(u"missing-worker.exe"_s);
        QString error;
        try {
            scan(QCoreApplication::applicationDirPath(), index, options, [] { return false; },
                [](const ScanProgress&) {});
        } catch (const ScanStopped& e) {
            error = QString::fromUtf8(e.what());
        }
        QVERIFY2(error.contains(u"无法启动识别工作进程"), qPrintable(error));
        // Nothing is kept as failed: the next round reads the file again.
        Store store(index, 0);
        QCOMPARE(store.statistics().value(u"failed"_s).toInt(), 0);
        QCOMPARE(store.statistics().value(u"files"_s).toInt(), 0);
    }

    // Installed, the plugin lives in the user's plugins folder, not beside
    // pdfium.dll in Win顺's: its worker must still read PDFs (the other tests
    // run it from the build folder, where pdfium.dll is next to it).
    void installedWorkerReadsPdfPages()
    {
        const QString appDir = workerAppDir();
        if (!ws::PaddleOcr::installed(appDir))
            QSKIP("Enhanced OCR component is not installed in this test runtime");
        QVERIFY2(QFileInfo::exists(QDir(appDir).filePath(u"pdfium.dll"_s)), qPrintable(appDir));
        QTemporaryDir temp;
        const QDir installed(temp.filePath(u"plugins/winshun.ocr-search"_s));
        QVERIFY(installed.mkpath(u"."_s));
        const QDir build(QCoreApplication::applicationDirPath());
        for (const QString& name : {u"WinShunOcrSearch.exe"_s, u"WinShunOcrSearchApp.dll"_s})
            QVERIFY(QFile::copy(build.filePath(name), installed.filePath(name)));
        QVERIFY(!installed.exists(u"pdfium.dll"_s));
        const QString path = temp.filePath(u"mixed.pdf"_s), data = temp.filePath(u"data"_s);
        pdf(path, imageBytes());
        {
            Store store(data);
        }
        Worker worker(installed.filePath(u"WinShunOcrSearch.exe"_s), appDir, data, u"test"_s);
        QJsonObject result;
        try {
            result = worker.read(path, [] { return false; });
        } catch (const std::exception& e) {
            QFAIL(e.what());
        }
        QCOMPARE(result.value(u"images"_s).toInt(), 2); // the third page is text only
        QVERIFY(!result.value(u"pages"_s).toArray().first().toObject().value(u"pieces"_s).toArray().isEmpty());
    }

    void fullIndexCanStillBeEmptied()
    {
        QTemporaryDir temp;
        const QString index = temp.filePath(u"index"_s);
        Store store(index, 16);
        quint32 random = 7;
        const auto addDense = [&](const QString& folder, int i) {
            const QString path = QDir(temp.path()).filePath(folder + u"/scan-%1.png"_s.arg(i));
            store.beginFile(path, 1, 1, QByteArray::number(i), u"test"_s, 1);
            QList<ws::OcrPiece> pieces;
            for (int line = 0; line < 40; ++line) {
                ws::OcrPiece piece;
                for (int c = 0; c < 24; ++c) {
                    random = random * 1664525u + 1013904223u;
                    piece.text += QChar(0x4e00 + (random >> 16) % 6000);
                }
                piece.quad = QPolygonF(QRectF(10, line * 20, 600, 18));
                pieces.append(piece);
            }
            store.addPage(store.putAsset(folder.toUtf8() + QByteArray::number(i), QSize(800, 1000), pieces),
                u"图片"_s, {{u"kind"_s, u"image"_s}});
            store.finishFile();
        };
        bool full = false;
        for (int i = 0; i < 20000 && !full; ++i) {
            try {
                addDense(i % 2 ? u"b"_s : u"a"_s, i);
            } catch (const std::exception& e) {
                store.rollback();
                full = QString::fromUtf8(e.what()).contains(u"full", Qt::CaseInsensitive);
                QVERIFY2(full, e.what());
            }
        }
        QVERIFY(full);
        const qint64 before = store.statistics().value(u"bytes"_s).toInteger();
        const int assets = store.statistics().value(u"assets"_s).toInt();
        // Removing a folder frees room although deleting needs new pages first.
        store.prune(temp.filePath(u"a"_s), {});
        store.compact();
        QVERIFY(store.statistics().value(u"assets"_s).toInt() < assets);
        QVERIFY(store.statistics().value(u"bytes"_s).toInteger() < before);
        addDense(u"c"_s, 0); // room again under the cap
        store.retainFolders({});
        store.compact();
        QCOMPARE(store.statistics().value(u"assets"_s).toInt(), 0);
        QVERIFY(store.search(u"图片"_s, 10).isEmpty());
        // The user's cap applies again after the maintenance that ran above it.
        bool found = false;
        for (const QString& name : QSqlDatabase::connectionNames()) {
            auto db = QSqlDatabase::database(name);
            if (db.databaseName() != QDir(index).filePath(u"ocr-index.sqlite"_s))
                continue;
            QSqlQuery q(db);
            QVERIFY(q.exec(u"PRAGMA page_size"_s) && q.next());
            const qint64 pageSize = q.value(0).toLongLong();
            q.finish();
            QVERIFY(q.exec(u"PRAGMA max_page_count"_s) && q.next());
            QCOMPARE(q.value(0).toLongLong(), 16 * 1024 * 1024 / pageSize);
            found = true;
        }
        QVERIFY(found);
    }

    // One or two letters or digits are looked up as whole words; a word with
    // nothing to look up (Cyrillic, ...) must not read every file.
    void shortWordsAndWordsWithoutGrams()
    {
        QTemporaryDir temp;
        Store store(temp.filePath(u"index"_s));
        for (int i = 0; i < 2000; ++i) {
            const QString path = temp.filePath(u"doc-%1.png"_s.arg(i));
            write(path, "fixture");
            add(store, path, u"报销 ab документ %1"_s.arg(i));
        }
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(store.search(u"ab"_s, 100).size(), 100);
        QCOMPARE(store.search(u"12"_s, 100).size(), 1); // not 120 or 1200
        for (const QString& word : {u"a"_s, u"b"_s, u"док"_s, u"документ"_s})
            QVERIFY2(store.search(word, 100).isEmpty(), qPrintable(word));
        QVERIFY2(timer.elapsed() < 200, "A short word must be looked up, not read from every file");
        // Short words narrow a word that has grams, as whole words too.
        QCOMPARE(store.search(u"报销 ab"_s, 100).size(), 100);
        QVERIFY(store.search(u"报销 xy"_s, 100).isEmpty());
        QCOMPARE(store.search(u"报销 7"_s, 100).size(), 1);
    }

    void pictureBombIsRefusedFromItsHeader()
    {
        const QByteArray bomb = pngBomb(12000, 10000); // 120 megapixels
        QVERIFY(bomb.size() < 1024 * 1024);
        QTemporaryDir temp;
        const QString path = temp.filePath(u"bomb.png"_s);
        write(path, bomb);
        {
            const bool com = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED));
            const auto uninitialize = qScopeGuard([com] {
                if (com)
                    ::CoUninitialize();
            });
            QSize original;
            QVERIFY(ws::decodePictureData(bomb, 4096, &original, 0, nullptr, 100'000'000).isNull());
            QCOMPARE(original, QSize(12000, 10000));
            // Other callers (no limit) still get the picture.
            QVERIFY(!ws::decodePictureData(pngBomb(64, 32), 0, &original).isNull());
            QCOMPARE(original, QSize(64, 32));
        }
        QString error;
        try {
            extractImages(path, [](const QImage&, const QString&, const QJsonObject&) { QFAIL("Must not decode"); });
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        }
        QVERIFY2(error.contains(u"1 亿像素"), qPrintable(error));
    }

    // JFIF/JPE, GIF, HEIC/HEIF and AVIF are read too; an animation by its
    // first frame. A picture neither Windows nor Qt can read says which
    // Store extension it may need: a damaged file looks the same to both.
    void morePictureTypesAndWhatTheyMayNeed()
    {
        for (const QString& name : {u"a.jpe"_s, u"a.JFIF"_s, u"a.gif"_s, u"a.heic"_s, u"a.heif"_s, u"a.avif"_s})
            QVERIFY2(supported(name), qPrintable(name));
        QVERIFY(!supported(u"a.svg"_s));
        QTemporaryDir temp;
        const QString gif = temp.filePath(u"moving.gif"_s);
        QVERIFY(QFile::copy(QStringLiteral(WINSHUN_TEST_DATA "/clipboard-animated.gif"), gif));
        QList<QJsonObject> sources;
        const auto result = extractImages(gif, [&](const QImage& image, const QString&, const QJsonObject& source) {
            QVERIFY(!image.isNull());
            sources.append(source);
        });
        QCOMPARE(result.images, 1);
        QCOMPARE(sources.size(), 1);
        QCOMPARE(sources.first().value(u"frames"_s).toInt(), 1);
        const auto errorOf = [&](const QString& name) {
            const QString path = temp.filePath(name);
            write(path, QByteArray(4096, 'x'));
            try {
                extractImages(path, [](const QImage&, const QString&, const QJsonObject&) { });
            } catch (const std::exception& e) {
                return QString::fromUtf8(e.what());
            }
            return QString();
        };
        const QString heic = errorOf(u"photo.heic"_s);
        QVERIFY2(heic.contains(u"“HEIF 图像扩展”和“HEVC 视频扩展”"_s), qPrintable(heic));
        QVERIFY(errorOf(u"photo.heif"_s).contains(u"HEIF 图像扩展"_s));
        QVERIFY(errorOf(u"photo.avif"_s).contains(u"“AV1 视频扩展”"_s));
        QVERIFY(errorOf(u"photo.webp"_s).contains(u"“WebP 图像扩展”"_s));
        const QString png = errorOf(u"photo.png"_s);
        QVERIFY2(!png.isEmpty() && !png.contains(u"Microsoft Store"_s), qPrintable(png));
    }

    void realOcrIndexSearchUpdateAndResume()
    {
        const QString appDir = QCoreApplication::applicationDirPath();
        if (!ws::PaddleOcr::installed(appDir))
            QSKIP("Enhanced OCR component is not installed in this test runtime");
        QTemporaryDir temp;
        const QString source = temp.filePath(u"sources"_s);
        QDir().mkpath(source);
        const QString index = temp.filePath(u"index"_s);
        const QByteArray png = imageBytes();
        write(QDir(source).filePath(u"image.png"_s), png);
        for (const QString& ext : {u"docx"_s, u"pptx"_s, u"xlsx"_s})
            office(QDir(source).filePath(u"embedded."_s + ext), png);
        pdf(QDir(source).filePath(u"mixed.pdf"_s), png);
        ScanOptions options;
        options.folders = {source};
        int recognized = 0;
        QElapsedTimer timer;
        timer.start();
        scan(appDir, index, options, [] { return false; }, [&](const ScanProgress& p) { recognized = p.recognized; });
        qInfo() << "Real OCR indexing, ms:" << timer.elapsed() << "new images:" << recognized;
        {
            Store store(index, 0);
            const auto hits = store.search(u"731905"_s, 20);
            QCOMPARE(hits.size(), 5);
            QCOMPARE(store.search(u"报销"_s, 20).size(), 5);
            QVERIFY(store.statistics().value(u"assets"_s).toInt() < store.statistics().value(u"pages"_s).toInt());
        }
        recognized = -1;
        scan(appDir, index, options, [] { return false; }, [&](const ScanProgress& p) { recognized = p.recognized; });
        QCOMPARE(recognized, 0);
        const QString original = QDir(source).filePath(u"image.png"_s), moved = QDir(source).filePath(u"moved.png"_s);
        QVERIFY(QFile::rename(original, moved));
        scan(appDir, index, options, [] { return false; }, [](const ScanProgress&) {});
        {
            Store store(index, 0);
            QCOMPARE(store.search(u"731905"_s, 20).size(), 5);
        }
        write(moved, imageBytes(u"UPDATED 864201"_s));
        scan(appDir, index, options, [] { return false; }, [](const ScanProgress&) {});
        {
            Store store(index, 0);
            QCOMPARE(store.search(u"731905"_s, 20).size(), 4);
            QCOMPARE(store.search(u"864201"_s, 20).size(), 1);
        }
        QVERIFY(QFile::remove(moved));
        scan(appDir, index, options, [] { return false; }, [](const ScanProgress&) {});
        {
            Store store(index, 0);
            QVERIFY(store.search(u"864201"_s, 20).isEmpty());
        }
    }

    // An index of the pipeline before reads again what that one left out
    // (drawn words in a PDF, a picture read upside down; a text-only PDF is
    // looked at once more) and nothing else: the other pictures keep their
    // stored recognition and are not read with OCR again.
    void upgradeReadsAgainOnlyWhatWasLeftOut()
    {
        const QString appDir = QCoreApplication::applicationDirPath();
        if (!ws::PaddleOcr::installed(appDir))
            QSKIP("Enhanced OCR component is not installed in this test runtime");
        QTemporaryDir temp;
        const QString source = temp.filePath(u"sources"_s), index = temp.filePath(u"index"_s);
        QDir().mkpath(source);
        write(QDir(source).filePath(u"upright.png"_s), imageBytes());
        QVERIFY(QImage::fromData(imageBytes(u"TURNED 864201"_s))
                .transformed(QTransform().rotate(180))
                .save(QDir(source).filePath(u"turned.png"_s)));
        {
            QPdfWriter writer(QDir(source).filePath(u"drawn.pdf"_s));
            writer.setResolution(100);
            QPainter painter(&writer);
            QPainterPath outlined;
            outlined.addText(40, 80, QFont(u"Arial"_s, 24), u"OUTLINED 552210"_s);
            painter.fillPath(outlined, Qt::black);
        }
        {
            QPdfWriter writer(QDir(source).filePath(u"text.pdf"_s));
            writer.setResolution(100);
            QPainter painter(&writer);
            painter.setFont(QFont(u"Arial"_s, 24));
            painter.drawText(40, 80, u"Words in the text layer"_s);
        }
        ScanOptions options;
        options.folders = {source};
        int recognized = 0;
        scan(appDir, index, options, [] { return false; }, [&](const ScanProgress& p) { recognized = p.recognized; });
        QCOMPARE(recognized, 3);
        // What ocr-search-2 stored: no page of the drawn PDF, the turned
        // picture read as it came (poorly), every row by that pipeline.
        {
            auto db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"left-out"_s);
            db.setDatabaseName(QDir(index).filePath(u"ocr-index.sqlite"_s));
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(u"UPDATE files SET engine='%1'"_s.arg(QString(ws::ocr::kVersion) + u"/ocr-search-2"_s)));
            QVERIFY(q.exec(u"UPDATE assets SET hash=hash||x'00' WHERE id IN "
                           u"(SELECT asset FROM pages WHERE file LIKE '%drawn.pdf')"_s));
            QVERIFY(q.exec(u"DELETE FROM pages WHERE file LIKE '%drawn.pdf'"_s));
            QVERIFY(q.exec(u"UPDATE files SET status='empty' WHERE path LIKE '%drawn.pdf'"_s));
            QList<ws::OcrPiece> poor;
            for (int i = 0; i < 3; ++i) {
                ws::OcrPiece piece;
                piece.text = u"10Z98t"_s;
                piece.score = 0.55f;
                piece.quad = QPolygonF(QRectF(10, 20 + 40 * i, 300, 30));
                poor.append(piece);
            }
            q.prepare(u"UPDATE assets SET pieces=? WHERE id=(SELECT asset FROM pages WHERE file LIKE '%turned.png')"_s);
            q.addBindValue(qCompress(QJsonDocument(piecesToJson(poor)).toJson(QJsonDocument::Compact), 6));
            QVERIFY(q.exec());
            QCOMPARE(q.numRowsAffected(), 1);
            QVERIFY(q.exec(u"PRAGMA user_version=2"_s));
        }
        QSqlDatabase::removeDatabase(u"left-out"_s);
        recognized = -1;
        int recheck = 0;
        scan(
            appDir, index, options, [] { return false; },
            [&](const ScanProgress& p) {
                recognized = p.recognized;
                recheck = std::max(recheck, p.recheck);
            });
        QCOMPARE(recheck, 3); // the drawn and the text-only PDF, the turned picture
        QCOMPARE(recognized, 2); // the drawn page and the turned picture, not the upright one
        Store store(index, 0);
        const QJsonObject statistics = store.statistics();
        QCOMPARE(statistics.value(u"recheck"_s).toInt(), 0);
        QCOMPARE(statistics.value(u"empty"_s).toInt(), 1); // the text-only PDF
        QCOMPARE(store.search(u"552210"_s, 10).size(), 1);
        const auto turned = store.search(u"864201"_s, 10);
        QCOMPARE(turned.size(), 1);
        double scores = 0;
        const auto pieces = store.pieces(turned.first().asset);
        for (const auto& piece : pieces)
            scores += piece.score;
        QVERIFY2(!pieces.isEmpty() && scores / pieces.size() > 0.75, qPrintable(QString::number(scores)));
    }

    void workerDeadlineCancellationAndRecovery()
    {
        const QString appDir = workerAppDir();
        if (!ws::PaddleOcr::installed(appDir))
            QSKIP("Enhanced OCR component is not installed in this test runtime");
        QVERIFY2(QFileInfo::exists(QDir(appDir).filePath(u"Qt6Core.dll"_s)),
            qPrintable(u"No deployed Win顺 folder for the worker: "_s + appDir));
        QTemporaryDir temp;
        const QString image = temp.filePath(u"invoice.png"_s), broken = temp.filePath(u"broken.pdf"_s);
        write(image, imageBytes());
        write(broken, "broken");
        const QString data = temp.filePath(u"data"_s);
        {
            Store store(data);
        }
        const QString program = QDir(QCoreApplication::applicationDirPath()).filePath(u"WinShunOcrSearch.exe"_s);
        Worker worker(program, appDir, data, u"test"_s);
        const auto read = [&](const Cancel& cancel, QString* error) {
            try {
                return worker.read(image, cancel);
            } catch (const std::exception& e) {
                *error = QString::fromUtf8(e.what());
                return QJsonObject {};
            }
        };
        // The worker's own answer about the file, not a worker that never started.
        QString brokenError;
        try {
            worker.read(broken, [] { return false; });
        } catch (const std::exception& e) {
            brokenError = QString::fromUtf8(e.what());
        }
        QVERIFY(!brokenError.isEmpty());
        QVERIFY2(!brokenError.contains(u"工作进程"), qPrintable(brokenError));
        QString error;
        auto result = read([] { return false; }, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(result.value(u"images"_s).toInt(), 1);
        QVERIFY(!result.value(u"pages"_s).toArray().first().toObject().value(u"pieces"_s).toArray().isEmpty());
        read([] { return true; }, &error);
        QVERIFY2(error.contains(u"暂停"), qPrintable(error));
        // Cancelling killed the process; the next file starts a new one.
        error.clear();
        result = read([] { return false; }, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(result.value(u"images"_s).toInt(), 1);
        Worker timed(program, appDir, data, u"test"_s, 50);
        error.clear();
        try {
            timed.read(image, [] { return false; });
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        }
        QVERIFY2(error.contains(u"没有进展"), qPrintable(error));
    }

    // Pictures Windows reads with the Microsoft Store's extensions, read by
    // the worker as indexing runs it (background mode: those decoders fail
    // under its working-set cap). Skipped where this PC has no extension.
    void workerReadsPicturesOfStoreExtensions()
    {
        const QString appDir = workerAppDir();
        if (!ws::PaddleOcr::installed(appDir))
            QSKIP("Enhanced OCR component is not installed in this test runtime");
        QTemporaryDir temp;
        const QString data = temp.filePath(u"data"_s);
        {
            Store store(data);
        }
        const QString program = QDir(QCoreApplication::applicationDirPath()).filePath(u"WinShunOcrSearch.exe"_s);
        Worker worker(program, appDir, data, u"test"_s);
        int read = 0;
        for (const QString& name : {u"ocr-sample.heic"_s, u"ocr-sample.avif"_s}) {
            const QString path = QStringLiteral(WINSHUN_TEST_DATA "/") + name;
            QFile file(path);
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
            if (ws::decodePictureData(file.readAll(), 0).isNull()) {
                qInfo() << "Windows cannot decode" << name << "on this PC";
                continue;
            }
            QJsonObject result;
            try {
                result = worker.read(path, [] { return false; });
            } catch (const std::exception& e) {
                QFAIL(qPrintable(name + u": "_s + QString::fromUtf8(e.what())));
            }
            QCOMPARE(result.value(u"images"_s).toInt(), 1);
            QVERIFY(!result.value(u"pages"_s).toArray().first().toObject().value(u"pieces"_s).toArray().isEmpty());
            ++read;
        }
        if (!read)
            QSKIP("Windows has neither the HEIF nor the AV1 extension on this PC");
    }

    void installedPluginRunsInRealHostAndOpensItsWindow()
    {
        const QString appDir = workerAppDir(), buildDir = QCoreApplication::applicationDirPath();
        if (!ws::PaddleOcr::installed(appDir))
            QSKIP("Enhanced OCR component is not installed in this test runtime");
        QVERIFY2(QFileInfo::exists(QDir(appDir).filePath(u"Qt6Core.dll"_s)),
            qPrintable(u"No deployed Win顺 folder for the plugin: "_s + appDir));
        QTemporaryDir temp;
        const QString sources = temp.filePath(u"sources"_s);
        QDir().mkpath(sources);
        write(QDir(sources).filePath(u"invoice.png"_s), imageBytes());
        const QString id = u"winshun.ocr-search"_s;
        const QString dataRoot = temp.filePath(u"data"_s);
        const QString data = QDir(dataRoot).filePath(id + u".data"_s);
        QDir().mkpath(data);
        {
            QSettings settings(QDir(data).filePath(u"ocr-search.ini"_s), QSettings::IniFormat);
            settings.setValue(u"folders"_s, QStringList {sources});
            settings.setValue(u"autoUpdate"_s, true);
            settings.sync();
        }
        QFile manifest(QString::fromUtf8(WINSHUN_OCR_MANIFEST));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        const QByteArray manifestBytes = manifest.readAll();
        const auto actions = QJsonDocument::fromJson(manifestBytes)
                                 .object()
                                 .value(u"commands"_s)
                                 .toArray()
                                 .first()
                                 .toObject()
                                 .value(u"actions"_s)
                                 .toArray();
        QCOMPARE(actions.size(), 3);
        QCOMPARE(actions[0].toObject().value(u"id"_s).toString(), u"open"_s);
        QCOMPARE(actions[1].toObject().value(u"id"_s).toString(), u"preview"_s);
        QCOMPARE(actions[2].toObject().value(u"id"_s).toString(), u"folder"_s);
        QFile exe(QDir(buildDir).filePath(u"WinShunOcrSearch.exe"_s));
        QVERIFY(exe.open(QIODevice::ReadOnly));
        QFile dll(QDir(buildDir).filePath(u"WinShunOcrSearchApp.dll"_s));
        QVERIFY(dll.open(QIODevice::ReadOnly));
        const QString package = temp.filePath(u"ocr.wsplugin"_s);
        zip(package,
            {{"plugin.json", manifestBytes}, {"WinShunOcrSearch.exe", exe.readAll()},
                {"WinShunOcrSearchApp.dll", dll.readAll()}});
        auto candidate = ws::plugin::install::inspect(package);
        QVERIFY(candidate.has_value());
        QVERIFY2(candidate->error.isEmpty(), qPrintable(candidate->error));
        const QString plugins = temp.filePath(u"plugins"_s);
        QString error;
        QVERIFY2(!ws::plugin::install::place(*candidate, plugins, &error).isEmpty(), qPrintable(error));
        ws::plugin::install::discard(*candidate);
        ws::plugin::Host::Options options;
        options.userPluginDir = plugins;
        options.dataRoot = dataRoot;
        options.appDir = appDir;
        options.runnerExe = QDir(buildDir).filePath(u"WinShunPluginHost.exe"_s);
        options.version = QStringLiteral(WINSHUN_VERSION);
        ws::plugin::Host host(options);
        host.setEnabled(id, true);
        ws::plugin::Items answer;
        quint64 finished = 0;
        QString failure;
        QStringList logs;
        connect(&host, &ws::plugin::Host::logged, this, [&](const QString& line) { logs.append(line); });
        connect(&host, &ws::plugin::Host::results, this,
            [&](quint64 query, const QString&, const QString&, const ws::plugin::Items& items, bool partial, bool) {
                if (!partial) {
                    finished = query;
                    answer = items;
                }
            });
        connect(&host, &ws::plugin::Host::queryFailed, this,
            [&](quint64, const QString&, const QString& why) { failure = why; });
        QVERIFY(host.query(u"ocr 731905"_s, ws::Scope::All, 51));
        QVERIFY2(
            QTest::qWaitFor(
                [&] {
                    return (finished == 51 && !answer.isEmpty() && answer.first().data.toObject().contains(u"hit"_s))
                        || !failure.isEmpty();
                },
                15000),
            qPrintable(logs.join(u'\n')));
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(answer.size(), 1);
        QVERIFY(answer.first().subtitle.contains(u"731905"));
        QVERIFY(!answer.first().previewText.isEmpty());
        QVERIFY(answer.first().data.toObject().contains(u"hit"_s));
        QVERIFY(host.query(u"731905"_s, ws::Scope::Content, 52));
        QVERIFY2(
            QTest::qWaitFor([&] { return finished == 52 || !failure.isEmpty(); }, 10000), qPrintable(logs.join(u'\n')));
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(answer.size(), 1);
        bool window = false;
        QVERIFY(host.request(id, u"showSettings"_s, {}, [&](bool ok, const QJsonValue&) { window = ok; }));
        QVERIFY2(QTest::qWaitFor([&] { return window; }, 8000), qPrintable(logs.join(u'\n')));
        QVERIFY2(std::none_of(logs.begin(), logs.end(),
                     [](const QString& line) { return line.contains(u"qrc:/") || line.contains(u"ReferenceError"); }),
            qPrintable(logs.join(u'\n')));
        const HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, host.state(id).pid);
        host.shutdown(false);
        if (process) {
            QCOMPARE(::WaitForSingleObject(process, 5000), DWORD(WAIT_OBJECT_0));
            ::CloseHandle(process);
        }
    }
};

QTEST_MAIN(TestOcrSearch)
#include "tst_ocrsearch.moc"
