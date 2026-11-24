#include "PictureFile.h"
#include "PictureWrite.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <windows.h>
#include <objbase.h>

using namespace Qt::StringLiterals;
using namespace ws::capture;

namespace {

QImage decodeRoundTrip(const QString& path)
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    const auto uninit = qScopeGuard([com] {
        if (SUCCEEDED(com))
            ::CoUninitialize();
    });
    return ws::decodePictureFile(path, 0);
}

QImage testPattern()
{
    QImage img(32, 24, QImage::Format_ARGB32);
    img.fill(QColor(220, 40, 20, 255)); // bright red-orange on left
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 16; x < img.width(); ++x)
            img.setPixelColor(x, y, QColor(20, 80, 210, 255)); // distinct blue on right
    }
    return img;
}

} // namespace

class PictureWriteTest : public QObject {
    Q_OBJECT

private slots:
    void writableExtensionsAndSourceFilter()
    {
        for (const QString& ok : {u"C:\\a.png"_s, u"C:\\a.PNG"_s, u"C:\\a.jpg"_s, u"C:\\a.JPEG"_s,
                 u"C:\\a.jpe"_s, u"C:\\a.jfif"_s, u"C:\\a.bmp"_s, u"C:\\a.dib"_s, u"C:\\a.tif"_s, u"C:\\a.tiff"_s})
            QVERIFY2(isWritableImageExtension(ok), qPrintable(ok));

        for (const QString& no : {u"C:\\a.gif"_s, u"C:\\a.ico"_s, u"C:\\a.webp"_s, u"C:\\a.heic"_s,
                 u"C:\\a.avif"_s, u"C:\\a.jxr"_s, u"C:\\a.txt"_s, u"C:\\noext"_s})
            QVERIFY2(!isWritableImageExtension(no), qPrintable(no));

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString userFile = dir.filePath(u"user/photo.jpg"_s);
        const QString profile = dir.filePath(u"profile"_s);
        const QString pluginData = profile + u"/plugins/winshun.capture.data"_s;
        QVERIFY(QDir().mkpath(dir.filePath(u"user"_s)));
        QVERIFY(QDir().mkpath(pluginData + u"/out"_s));
        QVERIFY(QDir().mkpath(profile + u"/images"_s));

        QVERIFY(isWritableSourceFile(userFile, pluginData));
        QVERIFY(!isWritableSourceFile(dir.filePath(u"user"_s), pluginData)); // directory rejected
        QVERIFY(!isWritableSourceFile(pluginData + u"/out/tmp.png"_s, pluginData)); // plugin data rejected
        QVERIFY(!isWritableSourceFile(profile + u"/images/clip.png"_s, pluginData)); // host clipboard cache rejected
        QVERIFY(!isWritableSourceFile(dir.filePath(u"user/anim.gif"_s), pluginData)); // animated format rejected
    }

    void shortFileNameTruncation()
    {
        QCOMPARE(shortFileName(u"C:\\dir\\a.png"_s, 24), u"a.png"_s);
        const QString shortened = shortFileName(u"C:\\dir\\a_very_very_very_long_screenshot_name_2026.png"_s, 24);
        QVERIFY(shortened.size() <= 24);
        QVERIFY(shortened.endsWith(u".png"_s));
        QVERIFY(shortened.contains(u'…'));
    }

    void encodesPngJpegBmpTiffAccurately()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QImage pattern = testPattern();

        // PNG preserves alpha too.
        QImage alphaImg(16, 16, QImage::Format_ARGB32);
        alphaImg.fill(QColor(10, 200, 30, 128));
        const QString pngPath = dir.filePath(u"out.png"_s);
        QCOMPARE(writeImageFile(alphaImg, pngPath), QString());
        const QImage pngRead(pngPath);
        QCOMPARE(pngRead.size(), alphaImg.size());
        QCOMPARE(pngRead.pixelColor(4, 4), QColor(10, 200, 30, 128));

        // JPEG: verify real JPEG header (FF D8) and non-swapped R/B channels.
        const QString jpgPath = dir.filePath(u"out.jpg"_s);
        QCOMPARE(writeImageFile(pattern, jpgPath), QString());
        {
            QFile f(jpgPath);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QByteArray head = f.read(2);
            QCOMPARE(head, QByteArray("\xFF\xD8", 2));
        }
        const QImage jpgRead = decodeRoundTrip(jpgPath);
        QCOMPARE(jpgRead.size(), pattern.size());
        const QColor leftJpg = jpgRead.pixelColor(4, 12);
        const QColor rightJpg = jpgRead.pixelColor(24, 12);
        QVERIFY2(leftJpg.red() > 190 && leftJpg.blue() < 50, qPrintable(leftJpg.name()));
        QVERIFY2(rightJpg.blue() > 180 && rightJpg.red() < 50, qPrintable(rightJpg.name()));

        // BMP: exact pixel match (24-bit BGR) and real BM header.
        const QString bmpPath = dir.filePath(u"out.bmp"_s);
        QCOMPARE(writeImageFile(pattern, bmpPath), QString());
        {
            QFile f(bmpPath);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.read(2), QByteArray("BM", 2));
        }
        const QImage bmpRead = decodeRoundTrip(bmpPath);
        QCOMPARE(bmpRead.size(), pattern.size());
        QCOMPARE(bmpRead.pixelColor(4, 12), QColor(220, 40, 20));
        QCOMPARE(bmpRead.pixelColor(24, 12), QColor(20, 80, 210));

        // TIFF: exact pixel match.
        const QString tifPath = dir.filePath(u"out.tiff"_s);
        QCOMPARE(writeImageFile(pattern, tifPath), QString());
        const QImage tifRead = decodeRoundTrip(tifPath);
        QCOMPARE(tifRead.size(), pattern.size());
        QCOMPARE(tifRead.pixelColor(4, 12), QColor(220, 40, 20));
        QCOMPARE(tifRead.pixelColor(24, 12), QColor(20, 80, 210));
    }

    void atomicOverwriteAndReadOnlyProtection()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = dir.filePath(u"photo.png"_s);

        QImage v1(20, 20, QImage::Format_RGB32);
        v1.fill(QColor(10, 20, 30));
        QCOMPARE(writeImageFile(v1, target), QString());

        // Overwrite with v2: updated atomically, no leftover temp files.
        QImage v2(24, 24, QImage::Format_RGB32);
        v2.fill(QColor(200, 150, 100));
        QCOMPARE(writeImageFile(v2, target), QString());
        const QImage after(target);
        QCOMPARE(after.size(), QSize(24, 24));
        QCOMPARE(after.pixelColor(0, 0), QColor(200, 150, 100));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), QStringList {u"photo.png"_s});

        // Read-only target: write fails cleanly and original bytes remain untouched.
        QFile original(target);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const QByteArray beforeBytes = original.readAll();
        original.close();
        const std::wstring wTarget = QDir::toNativeSeparators(target).toStdWString();
        QVERIFY(::SetFileAttributesW(wTarget.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE);
        const auto restoreWritable = qScopeGuard([&] { ::SetFileAttributesW(wTarget.c_str(), FILE_ATTRIBUTE_NORMAL); });

        QImage v3(12, 12, QImage::Format_RGB32);
        v3.fill(Qt::yellow);
        const QString err = writeImageFile(v3, target);
        QVERIFY(!err.isEmpty());
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), beforeBytes);
        original.close();
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), QStringList {u"photo.png"_s});

        // Non-existent directory fails cleanly.
        const QString missingErr = writeImageFile(v3, dir.filePath(u"missing/a.png"_s));
        QVERIFY(!missingErr.isEmpty());
    }
};

QTEST_MAIN(PictureWriteTest)
#include "tst_picturewrite.moc"
