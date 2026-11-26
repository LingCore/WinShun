#include "Favorites.h"
#include "FileIconProvider.h"
#include "SiteIconImage.h"
#include "ThumbnailProvider.h"

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>
#include <psapi.h>

#include <cstdlib>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;
using namespace ws;

namespace {

QImage load(FileIconProvider& provider, const QString& url, int px = 32)
{
    QSize size;
    return provider.requestImage(url.sliced(QStringView(u"image://fileicon/").size()), &size, QSize(px, px));
}

QString embedded(const QImage& image)
{
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return QString::fromLatin1("data:image/png;base64," + png.toBase64());
}

} // namespace

class TestCommandIcons : public QObject {
    Q_OBJECT

private slots:
    // A type outside the built-in list gets its thumbnail when a program on
    // this PC makes thumbnails of it (CorelDRAW's .cdr: asked for, never
    // shown, the list not having it). Three made-up types under the user's
    // own classes, removed after: one with a thumbnail handler (Windows' own
    // for pictures, so that a real thumbnail can be asked for), one with it
    // but called a document, one with none.
    void thumbnailsOfTypesWithAHandler()
    {
        const wchar_t* const kHandler = L"{C7657C4A-9F68-40fa-A4DF-96BC08EB3551}"; // Windows' photo thumbnail provider
        const std::wstring classes = L"Software\\Classes\\";
        std::vector<std::wstring> made;
        const auto cleanup = qScopeGuard([&] {
            for (const std::wstring& key : made)
                ::RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        });
        const auto set = [](const std::wstring& key, const wchar_t* name, const wchar_t* value) {
            return ::RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value,
                       static_cast<DWORD>((std::wcslen(value) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        };
        const auto addType = [&](const wchar_t* extension, bool handler, const wchar_t* perceived) {
            const std::wstring key = classes + extension;
            HKEY there = nullptr;
            if (::RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &there) == ERROR_SUCCESS) {
                ::RegCloseKey(there);
                return false; // someone's real type: not touched
            }
            made.push_back(key);
            bool ok = set(key, nullptr, L"WinShun test type");
            if (handler)
                ok = ok && set(key + L"\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}", nullptr, kHandler);
            if (perceived)
                ok = ok && set(key, L"PerceivedType", perceived);
            return ok;
        };
        QVERIFY(addType(L".wsthumbdraw", true, nullptr));
        QVERIFY(addType(L".wsthumbdoc", true, L"document"));
        QVERIFY(addType(L".wsthumbnone", false, nullptr));

        QVERIFY(ThumbnailProvider::hasThumbnailType(u"D:\\设计\\海报.wsthumbdraw"_s));
        QVERIFY(ThumbnailProvider::hasThumbnailType(u"D:\\设计\\海报.WSTHUMBDRAW"_s));
        QVERIFY(!ThumbnailProvider::hasThumbnailType(u"D:\\设计\\合同.wsthumbdoc"_s)); // a document keeps its icon
        QVERIFY(!ThumbnailProvider::hasThumbnailType(u"D:\\设计\\海报.wsthumbnone"_s));
        // As before: the built-in types wherever they are, no shortcut, no name without a type.
        QVERIFY(ThumbnailProvider::hasThumbnailType(u"D:\\a\\photo.JPG"_s));
        QVERIFY(ThumbnailProvider::hasThumbnailType(u"D:\\a\\clip.mkv"_s));
        QVERIFY(!ThumbnailProvider::hasThumbnailType(u"D:\\a\\readme.txt"_s));
        QVERIFY(!ThumbnailProvider::hasThumbnailType(u"D:\\a\\Photoshop.lnk"_s));
        QVERIFY(!ThumbnailProvider::hasThumbnailType(u"D:\\a\\notes v1.2 final"_s));
        QVERIFY(!ThumbnailProvider::hasThumbnailType(u"D:\\a.b\\noextension"_s));
        QVERIFY(!ThumbnailProvider::hasThumbnailType(u"D:\\a\\trailing."_s));
        QVERIFY(!ThumbnailProvider::url(u"D:\\设计\\海报.wsthumbdraw"_s, 7).isEmpty());
        QVERIFY(ThumbnailProvider::url(u"\\\\nas\\设计\\海报.wsthumbdraw"_s, 7).isEmpty()); // a share: not read for a thumbnail

        // The thumbnail itself, by the handler registered: a picture saved under the made-up type.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QImage picture(96, 64, QImage::Format_RGB32);
        picture.fill(QColor(220, 30, 30));
        const QString path = QDir::toNativeSeparators(dir.filePath(u"海报.wsthumbdraw"_s));
        QVERIFY(picture.save(path, "PNG"));
        const QImage thumbnail = FileIconProvider::thumbnail(path, 48, false);
        QVERIFY2(!thumbnail.isNull(), "the registered handler made no thumbnail");
        const QColor middle = thumbnail.pixelColor(thumbnail.width() / 2, thumbnail.height() / 2);
        QVERIFY2(middle.red() > 180 && middle.green() < 80 && middle.blue() < 80, qPrintable(middle.name()));
    }

    void largeIconCacheHasBoundedMemory()
    {
        FileIconProvider provider;
        const auto url = [](int i) {
            return FileIconProvider::pluginIconUrl({}, u"color:#%1"_s.arg(i, 6, 16, QLatin1Char('0')), false);
        };
        // Real provider requests, at the largest supported icon size. Warm
        // Qt and the shell integration before measuring retained memory.
        const QImage first = load(provider, url(0), 256);
        QCOMPARE(first.size(), QSize(256, 256));
        PROCESS_MEMORY_COUNTERS_EX before {};
        QVERIFY(::GetProcessMemoryInfo(
            ::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof before));
        QImage last;
        for (int i = 1; i <= 512; ++i) {
            last = load(provider, url(i), 256);
            QCOMPARE(last.size(), QSize(256, 256));
        }
        PROCESS_MEMORY_COUNTERS_EX after {};
        QVERIFY(::GetProcessMemoryInfo(
            ::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after), sizeof after));
        const qint64 growth = static_cast<qint64>(after.PrivateUsage) - static_cast<qint64>(before.PrivateUsage);
        qInfo("512 large icon requests retained %.2f MiB of additional private memory",
            static_cast<double>(growth) / (1 << 20));
        QVERIFY2(growth < 16 * (1 << 20), "Large icons must not retain tens of MiB in the provider cache");
        QCOMPARE(load(provider, url(512), 256).cacheKey(), last.cacheKey()); // recent: no decoding or redraw
        QCOMPARE(load(provider, url(0), 256), first); // evicted: the same full-resolution pixels
    }

    void smallIconsStayCached()
    {
        FileIconProvider provider;
        std::vector<std::pair<QString, qint64>> keys;
        for (int i = 0; i < 256; ++i) {
            const QString url
                = FileIconProvider::pluginIconUrl({}, u"color:#%1"_s.arg(i, 6, 16, QLatin1Char('0')), false);
            const QImage image = load(provider, url);
            QCOMPARE(image.size(), QSize(32, 32));
            keys.emplace_back(url, image.cacheKey());
        }
        // The ordinary launcher's working set still fits, preserving its
        // cache hits while large gallery icons have a separate byte cost.
        for (const auto& [url, key] : keys)
            QCOMPARE(load(provider, url).cacheKey(), key);
    }

    void resolvesProgramsWithoutRunningThem()
    {
        const QString cmd = QStandardPaths::findExecutable(u"cmd.exe"_s);
        QVERIFY(!cmd.isEmpty());
        const QImage expected = FileIconProvider::fileIcon(cmd, 32);
        QVERIFY(!expected.isNull());
        FileIconProvider provider;
        for (const QString& command : {u'"' + cmd + u"\" /c echo test"_s,
                 u"\"%SystemRoot%\\System32\\cmd.exe\" /c echo test"_s, u"cmd /c echo test"_s}) {
            QCOMPARE(load(provider, FileIconProvider::commandIconUrl(command, {}, {}, false)), expected);
        }

        QTemporaryDir temporary(QDir::tempPath() + u"/命令图标 with spaces-XXXXXX"_s);
        QVERIFY(temporary.isValid());
        const QString executable = temporary.filePath(u"图标程序.exe"_s);
        QVERIFY(QFile::copy(cmd, executable));
        QCOMPARE(
            load(provider, FileIconProvider::commandIconUrl(u"图标程序.exe --argument"_s, temporary.path(), {}, false)),
            FileIconProvider::fileIcon(executable, 32));
        const QString oldPath = qEnvironmentVariable("PATH");
        const auto restore = qScopeGuard([oldPath] { ::_wputenv_s(L"PATH", oldPath.toStdWString().c_str()); });
        QCOMPARE(::_wputenv_s(L"PATH", temporary.path().toStdWString().c_str()), 0);
        QCOMPARE(load(provider, FileIconProvider::commandIconUrl(u"图标程序 --argument"_s, {}, {}, false)),
            FileIconProvider::fileIcon(executable, 32));

        QFile script(temporary.filePath(u"never-run.cmd"_s));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("@echo off\r\necho executed > \"%~dp0executed.txt\"\r\n");
        script.close();
        QVERIFY(!load(provider, FileIconProvider::commandIconUrl(script.fileName(), {}, {}, false)).isNull());
        QVERIFY(!QFile::exists(temporary.filePath(u"executed.txt"_s)));
    }

    void customPictureAndTheme()
    {
        FileIconProvider provider;
        QImage picture(32, 32, QImage::Format_ARGB32_Premultiplied);
        picture.fill(Qt::transparent);
        {
            QPainter painter(&picture);
            painter.fillRect(QRect(6, 6, 20, 20), QColor(10, 10, 10));
        }
        const QString icon = embedded(picture);
        QVERIFY(!favoriteIconPng(icon).isEmpty());
        const QString light = FileIconProvider::commandIconUrl(u"nonexistent-program.exe"_s, {}, icon, false);
        const QString dark = FileIconProvider::commandIconUrl(u"nonexistent-program.exe"_s, {}, icon, true);
        QCOMPARE(load(provider, light), siteicon::drawn(picture, 32, false));
        QCOMPARE(load(provider, dark), siteicon::drawn(picture, 32, true));
        QVERIFY(load(provider, light) != load(provider, dark));
        picture.fill(QColor(210, 60, 80));
        const QString replaced = FileIconProvider::commandIconUrl(u"nonexistent-program.exe"_s, {}, embedded(picture), false);
        QVERIFY(replaced != light);
        QVERIFY(load(provider, replaced) != load(provider, light));
    }

    void missingAndOfflineProgramsKeepAnIcon()
    {
        FileIconProvider provider;
        const QImage fallback = load(provider, FileIconProvider::commandIconUrl({}, {}, {}, false));
        QVERIFY(!fallback.isNull());
        QCOMPARE(load(provider, FileIconProvider::commandIconUrl(u"winshun-nonexistent-92.exe"_s, {}, {}, false)), fallback);
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(load(provider, FileIconProvider::commandIconUrl(u"\"\\\\winshun-offline-92\\share\\app.exe\" --arg"_s,
                     {}, {}, false)), fallback);
        QVERIFY2(timer.elapsed() < 2000, "An offline icon must not wait for the server");
        const QString cmd = QStandardPaths::findExecutable(u"cmd.exe"_s);
        QCOMPARE(load(provider, FileIconProvider::commandIconUrl(u'"' + cmd + u'"', {}, u"broken"_s, false)),
            FileIconProvider::fileIcon(cmd, 32));
    }
};

QTEST_MAIN(TestCommandIcons)
#include "tst_commandicons.moc"
