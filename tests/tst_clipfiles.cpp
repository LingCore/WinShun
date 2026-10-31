#include "ClipFiles.h"
#include "platform/DragOut.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

#include <shellapi.h>

using namespace Qt::StringLiterals;
using namespace ws;

namespace {

qint64 addImage(ClipStore& store, QRgb color)
{
    QImage image(12, 10, QImage::Format_ARGB32);
    image.fill(color);
    ClipCapture capture;
    capture.kind = ClipKind::Image;
    capture.width = image.width();
    capture.height = image.height();
    QBuffer buffer(&capture.imageData);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    capture.hash = QByteArray::number(color);
    return store.add(capture, 1'790'000'000'000); // both copied in the same second
}

qint64 addText(ClipStore& store, ClipKind kind, const QString& text)
{
    ClipCapture capture;
    capture.kind = kind;
    capture.text = text;
    capture.hash = clipHash(kind, text);
    return store.add(capture, 1'790'000'000'000);
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class ClipFilesTest : public QObject {
    Q_OBJECT

private slots:
    void allSelectedImagesReachTheDropTarget()
    {
        QTemporaryDir dir;
        ClipStore store(dir.filePath(u"history"_s));
        QVERIFY(store.open(1'790'000'000'000));
        const auto red = addImage(store, qRgb(255, 0, 0));
        const auto blue = addImage(store, qRgb(0, 0, 255));
        QVERIFY(red && blue && red != blue);
        const std::vector<const Clip*> selected {store.clip(blue), store.clip(red)};
        const auto exported = clipfiles::images(store, selected, dir.filePath(u"exports"_s), u"剪贴板图片"_s);
        QCOMPARE(exported.error, clipfiles::Error::None);
        QCOMPARE(exported.paths.size(), 2);
        QVERIFY(QFileInfo(exported.paths[0]).fileName() != QFileInfo(exported.paths[1]).fileName());
        QCOMPARE(QImage(exported.paths[0]).pixel(0, 0), qRgb(0, 0, 255));
        QCOMPARE(QImage(exported.paths[1]).pixel(0, 0), qRgb(255, 0, 0));
        QCOMPARE(clipfiles::images(store, selected, dir.filePath(u"exports"_s), u"剪贴板图片"_s).paths, exported.paths);

        QVERIFY(SUCCEEDED(::OleInitialize(nullptr)));
        {
            const auto data = dragout::dataObject(exported.paths);
            QVERIFY(data);
            FORMATETC format {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM medium {};
            QVERIFY(SUCCEEDED(data->GetData(&format, &medium)));
            const auto drop = static_cast<HDROP>(medium.hGlobal);
            QCOMPARE(::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0), 2u);
            for (UINT i = 0; i < 2; ++i) {
                wchar_t path[32768] {};
                ::DragQueryFileW(drop, i, path, 32768);
                QCOMPARE(QString::fromWCharArray(path), exported.paths[i]);
            }
            ::ReleaseStgMedium(&medium);
        }
        ::OleUninitialize();
        // Removing an export never changes the history's original.
        QVERIFY(QFile::remove(exported.paths[0]));
        QVERIFY(QFile::exists(store.payload(blue).imagePath));
        QCOMPARE(QImage(store.payload(blue).imagePath).pixel(0, 0), qRgb(0, 0, 255));
    }

    void failedBatchDoesNotOfferPartialSelection()
    {
        QTemporaryDir dir;
        ClipStore store(dir.filePath(u"history"_s));
        QVERIFY(store.open(1'790'000'000'000));
        const auto first = addImage(store, qRgb(255, 0, 0));
        const auto missing = addImage(store, qRgb(0, 0, 255));
        QVERIFY(QFile::remove(store.payload(missing).imagePath));
        const auto result = clipfiles::images(
            store, {store.clip(first), store.clip(missing)}, dir.filePath(u"exports"_s), u"Image"_s);
        QCOMPARE(result.error, clipfiles::Error::WriteFailed);
        QVERIFY(result.paths.isEmpty());
        Clip text;
        text.kind = ClipKind::Text;
        QCOMPARE(clipfiles::images(store, {store.clip(first), &text}, dir.filePath(u"mixed"_s), u"Image"_s).error,
            clipfiles::Error::NotImages);
        QVERIFY(!QFile::exists(dir.filePath(u"mixed"_s)));
    }

    // 保存为文件: each entry a file of the kind its words make, under a free name.
    void entriesSavedAsFiles()
    {
        QTemporaryDir dir;
        ClipStore store(dir.filePath(u"history"_s));
        QVERIFY(store.open(1'790'000'000'000));
        const auto notes = addText(store, ClipKind::Text, u"# 会议记录\n- 第一条"_s);
        const auto again = addText(store, ClipKind::Text, u"会议记录\n另一份"_s);
        const auto json = addText(store, ClipKind::Text, u"{\"a\": [1, 2]}"_s);
        const auto link = addText(store, ClipKind::Link, u"https://www.example.com/docs/页面"_s);
        const auto picture = addImage(store, qRgb(0, 128, 0));
        const QString kept = dir.filePath(u"source/报告.final.pdf"_s);
        QVERIFY(QDir().mkpath(dir.filePath(u"source/资料/子"_s)));
        QFile pdf(kept);
        QVERIFY(pdf.open(QIODevice::WriteOnly) && pdf.write("%PDF") == 4);
        pdf.close();
        QFile inner(dir.filePath(u"source/资料/子/a.txt"_s));
        QVERIFY(inner.open(QIODevice::WriteOnly) && inner.write("abc") == 3);
        inner.close();
        const auto files = addText(store, ClipKind::Files,
            QDir::toNativeSeparators(kept) + u"\r\n"_s + QDir::toNativeSeparators(dir.filePath(u"source/资料"_s))
                + u"\r\n"_s + QDir::toNativeSeparators(dir.filePath(u"source/gone.txt"_s)));
        QVERIFY(notes && again && json && link && picture && files);

        const std::vector<const Clip*> selected {store.clip(notes), store.clip(again), store.clip(json),
            store.clip(link), store.clip(picture), store.clip(files)};
        const clipfiles::Plan plan = clipfiles::plan(store, selected, false);
        QCOMPARE(plan.missing, 0);
        const QString out = dir.filePath(u"out"_s);
        QVERIFY(QDir().mkpath(out));
        const clipfiles::Saved saved = clipfiles::save(plan, out);
        QCOMPARE(saved.failed, 0);
        QCOMPARE(saved.missing, 1); // gone.txt
        QStringList names;
        for (const QString& path : saved.paths)
            names.append(QFileInfo(path).fileName());
        QCOMPARE(names.size(), 7);
        QCOMPARE(names[0], u"会议记录.txt"_s);
        QCOMPARE(names[1], u"会议记录 (2).txt"_s);
        QVERIFY(names[2].endsWith(u".json"_s));
        QCOMPARE(names[3], u"example.com docs 页面.url"_s);
        QVERIFY(names[4].startsWith(u"图片 "_s) && names[4].endsWith(u".png"_s));
        QCOMPARE(names[5], u"报告.final.pdf"_s);
        QCOMPARE(names[6], u"资料"_s);
        QCOMPARE(readAll(saved.paths[0]), QByteArray("# 会议记录\n- 第一条"));
        QVERIFY(readAll(saved.paths[3]).contains("URL=https://www.example.com/docs/%E9%A1%B5%E9%9D%A2"));
        QCOMPARE(QImage(saved.paths[4]).pixel(0, 0), qRgb(0, 128, 0));
        QCOMPARE(readAll(saved.paths[5]), QByteArray("%PDF"));
        QCOMPARE(readAll(out + u"/资料/子/a.txt"_s), QByteArray("abc"));

        // Again: nothing replaced, the next free names.
        const clipfiles::Saved twice = clipfiles::save(clipfiles::plan(store, {store.clip(files)}, false), out);
        QCOMPARE(twice.paths.size(), 2);
        QCOMPARE(QFileInfo(twice.paths[0]).fileName(), u"报告.final (2).pdf"_s);
        QCOMPARE(QFileInfo(twice.paths[1]).fileName(), u"资料 (2)"_s);
        QCOMPARE(readAll(saved.paths[5]), QByteArray("%PDF"));

        // A folder never into itself.
        const clipfiles::Saved inside
            = clipfiles::save(clipfiles::plan(store, {store.clip(files)}, false), dir.filePath(u"source/资料/子"_s));
        QCOMPARE(inside.failed, 1);
        QVERIFY(!QFileInfo::exists(dir.filePath(u"source/资料/子/资料"_s)));
    }

    void mergedIntoOneFile()
    {
        QTemporaryDir dir;
        ClipStore store(dir.filePath(u"history"_s));
        QVERIFY(store.open(1'790'000'000'000));
        const auto first = addText(store, ClipKind::Text, u"第一段"_s);
        const auto second = addText(store, ClipKind::Link, u"https://example.com"_s);
        const clipfiles::Plan plan
            = clipfiles::plan(store, {store.clip(first), store.clip(second)}, true, true, u"\r\n"_s);
        QCOMPARE(plan.files.size(), std::size_t {1});
        const clipfiles::Saved saved = clipfiles::save(plan, dir.filePath(u"drag 1"_s)); // not there yet
        QCOMPARE(saved.paths.size(), 1);
        QCOMPARE(QFileInfo(saved.paths[0]).fileName(), u"第一段.txt"_s);
        QCOMPARE(readAll(saved.paths[0]), QByteArray("第一段\r\nhttps://example.com"));
    }

    void uploadFileNames()
    {
        QCOMPARE(clipfiles::fileNames({u"C:/报告 甲.txt"_s}), u"\"C:\\报告 甲.txt\""_s);
        QCOMPARE(clipfiles::fileNames({u"C:/甲/a.txt"_s, u"\\\\nas\\共享\\b 乙.txt"_s}),
            u"\"C:\\甲\\a.txt\" \"\\\\nas\\共享\\b 乙.txt\""_s);
        QVERIFY(clipfiles::fileNames({u"C:/ok.txt"_s, u"bad\"name"_s}).isEmpty());
        QVERIFY(clipfiles::fileNames({u"C:/ok.txt\nD:/other.txt"_s}).isEmpty());
        QVERIFY(clipfiles::fileNames({}).isEmpty());
    }

    void sendHistorySelection()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ClipStore store(dir.filePath(u"history"_s));
        QVERIFY(store.open(1'790'000'000'000));
        const auto first = addText(store, ClipKind::Text, u"  第一段\n"_s);
        const auto second = addText(store, ClipKind::Link, u"https://example.com"_s);
        const auto words = clipfiles::planSend(store, {store.clip(first), store.clip(second)}, false, u"\r\n"_s);
        QCOMPARE(words.text, u"  第一段\n\r\nhttps://example.com"_s);
        QVERIFY(words.attachments.files.empty());

        const auto large = addText(store, ClipKind::Text, QString(40'000, QChar(0x4E2D)));
        const auto longText = clipfiles::planSend(store, {store.clip(large)}, true, {});
        QVERIFY(longText.text.isEmpty());
        const auto largeFile = clipfiles::prepareSend(longText.attachments, dir.filePath(u"large"_s));
        QCOMPARE(largeFile.error, clipfiles::Error::None);
        QCOMPARE(largeFile.paths.size(), 1);
        QCOMPARE(readAll(largeFile.paths.front()), store.payload(large).text.toUtf8());

        const auto image = addImage(store, qRgb(10, 20, 30));
        const QByteArray png = readAll(store.payload(image).imagePath);
        const auto mixed = clipfiles::planSend(store, {store.clip(first), store.clip(image)}, true, u"\n"_s);
        QVERIFY(mixed.text.isEmpty());
        const auto exported = clipfiles::prepareSend(mixed.attachments, dir.filePath(u"mixed"_s));
        QCOMPARE(exported.error, clipfiles::Error::None);
        QCOMPARE(exported.paths.size(), 2);
        QCOMPARE(readAll(exported.paths[0]), store.payload(first).text.toUtf8());
        QCOMPARE(readAll(exported.paths[1]), png);
        const qint64 removed[] {image};
        store.remove(removed); // prepared pictures survive changes to history
        QCOMPARE(readAll(exported.paths[1]), png);

        const QString original = dir.filePath(u"original.txt"_s);
        QFile file(original);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("original");
        file.close();
        const auto files = addText(store, ClipKind::Files, QDir::toNativeSeparators(original));
        const auto originalPlan = clipfiles::planSend(store, {store.clip(files)}, true, {});
        const auto ready = clipfiles::prepareSend(originalPlan.attachments, dir.filePath(u"unused"_s));
        QCOMPARE(ready.error, clipfiles::Error::None);
        QCOMPARE(ready.paths, QStringList {QDir::toNativeSeparators(original)});
        QVERIFY(!QFileInfo::exists(dir.filePath(u"unused"_s))); // source files are never duplicated
        QVERIFY(QFile::remove(original));
        const auto missing = clipfiles::prepareSend(originalPlan.attachments, dir.filePath(u"missing"_s));
        QCOMPARE(missing.error, clipfiles::Error::Missing);
        QVERIFY(missing.paths.isEmpty());
        auto partial = mixed.attachments;
        partial.missing = 1;
        QVERIFY(clipfiles::prepareSend(partial, dir.filePath(u"partial"_s)).paths.isEmpty());
        clipfiles::Plan tooMany;
        tooMany.files.resize(1001);
        QCOMPARE(clipfiles::prepareSend(tooMany, dir.filePath(u"too-many"_s)).error, clipfiles::Error::TooMany);
    }
};

QTEST_GUILESS_MAIN(ClipFilesTest)
#include "tst_clipfiles.moc"
