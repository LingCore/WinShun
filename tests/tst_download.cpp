#include "Download.h"
#include "Release.h"
#include "Gallery.h"

#include <QFile>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include <windows.h>

using namespace Qt::StringLiterals;
using namespace ws;

class TestDownload : public QObject {
    Q_OBJECT

private slots:
    void publicFilesUseMirrors_data()
    {
        QTest::addColumn<QString>("url");
        QTest::newRow("app") << u"https://github.com/LingCore/WinShun/releases/download/v0.17.0/WinShun-setup.exe"_s;
        QTest::newRow("plugin-index") << u"https://github.com/LingCore/WinShun-plugins/releases/download/index/index.json"_s;
        QTest::newRow("plugin") << u"https://github.com/LingCore/WinShun-plugins/releases/download/v1/capture.wsplugin"_s;
        QTest::newRow("ocr") << u"https://github.com/LingCore/WinShun/releases/download/v1/ocr.zip"_s;
        QTest::newRow("third-party") << u"https://github.com/microsoft/onnxruntime/releases/download/v1.30.0/onnxruntime.zip"_s;
        QTest::newRow("raw") << u"https://raw.githubusercontent.com/LingCore/WinShun/main/LICENSE"_s;
    }

    void publicFilesUseMirrors()
    {
        QFETCH(QString, url);
        QCOMPARE(download::sources(url), (QStringList {u"https://gh-proxy.com/"_s + url, u"https://ghfast.top/"_s + url, url}));
        release::Asset asset;
        asset.url = url;
        QCOMPARE(release::downloadUrls(asset), download::sources(url));
    }

    void otherRequestsStayAtTheirSource_data()
    {
        QTest::addColumn<QString>("url");
        QTest::newRow("api") << u"https://api.github.com/repos/LingCore/WinShun/releases/latest"_s;
        QTest::newRow("github-page") << u"https://github.com/LingCore/WinShun"_s;
        QTest::newRow("query-token") << u"https://github.com/private/repo/releases/download/v1/a.zip?token=secret"_s;
        QTest::newRow("user-info") << u"https://user:secret@github.com/private/repo/releases/download/v1/a.zip"_s;
        QTest::newRow("raw-token") << u"https://raw.githubusercontent.com/private/repo/main/a?token=secret"_s;
        QTest::newRow("fragment") << u"https://github.com/a/b/releases/download/v1/a.zip#secret"_s;
        QTest::newRow("port") << u"https://github.com:8443/a/b/releases/download/v1/a.zip"_s;
        QTest::newRow("lookalike") << u"https://github.com.evil.example/a/b/releases/download/v1/a.zip"_s;
        QTest::newRow("already-mirrored") << u"https://gh-proxy.com/https://github.com/a/b/releases/download/v1/a.zip"_s;
        QTest::newRow("http") << u"http://github.com/a/b/releases/download/v1/a.zip"_s;
        QTest::newRow("local-test") << u"http://127.0.0.1:18500/ocr.zip"_s;
        QTest::newRow("domestic-model") << u"https://www.modelscope.cn/models/a/resolve/main/model.onnx"_s;
        QTest::newRow("ai") << u"https://api.siliconflow.cn/v1/embeddings"_s;
        QTest::newRow("file") << u"C:/local files/plugin.wsplugin"_s;
    }

    void otherRequestsStayAtTheirSource()
    {
        QFETCH(QString, url);
        QCOMPARE(download::sources(url), QStringList {url});
    }

    void serviceOrderIsPreservedAndDeduplicated()
    {
        const QString url = u"https://github.com/a/b/releases/download/v1/a.zip"_s;
        const QString first = u"https://service.example/a.zip"_s;
        const QString second = u"https://second.example/a.zip"_s;
        QCOMPARE(download::sources(url, {first, first, second, url}), (QStringList {first, second, url}));
        QCOMPARE(download::sources(url, {url, first, first, second}), (QStringList {first, second, url}));
    }

    void failedAndCorruptSourcesFallBack()
    {
        const QString url = u"https://github.com/a/b/releases/download/v1/a.zip"_s;
        QStringList tried;
        const auto fetched = download::fetch(url, 1024, [&](const QString& at, qint64, const download::Progress&) {
            tried.append(at);
            if (tried.size() == 1) return download::Fetched {503, {}, 0};
            return download::Fetched {200, tried.size() == 2 ? QByteArray("wrong package") : QByteArray("verified package"), 0};
        }, {}, [](const QByteArray& bytes) { return bytes == "verified package"; });
        QCOMPARE(tried, download::sources(url));
        QCOMPARE(fetched.error, 0ul);
        QCOMPARE(fetched.body, QByteArray("verified package"));
    }

    void invalidContentNeverBecomesAValidResult()
    {
        const auto result = download::fetch(u"https://github.com/a/b/releases/download/v1/a.zip"_s, 1024,
            [](const QString&, qint64, const download::Progress&) { return download::Fetched {200, "bad", 0}; }, {},
            [](const QByteArray&) { return false; });
        QCOMPARE(result.error, static_cast<unsigned long>(ERROR_CRC));
    }

    void pluginIndexesRejectInvalidMirrorContent()
    {
        namespace gallery = plugin::gallery;
        const QString source(gallery::kIndexUrl);
        QStringList tried;
        const QByteArray valid("{\"schema\":1,\"plugins\":[]}");
        const auto result = gallery::fetch(source, gallery::kMaxIndexBytes,
            [&](const QString& url, qint64, const gallery::Progress&) {
                tried.append(url);
                return gallery::Fetched {200, tried.size() < 3 ? QByteArray("<html>proxy error</html>") : valid, 0};
            }, {}, [&](const QByteArray& bytes) { return gallery::parse(bytes, source, nullptr).has_value(); });
        QCOMPARE(tried, download::sources(source));
        QCOMPARE(result.error, 0ul);
        QCOMPARE(result.body, valid);
    }

    void pluginPackagesRejectWrongSizeAndHash()
    {
        namespace gallery = plugin::gallery;
        const QByteArray good("expected package bytes");
        gallery::Item item;
        item.url = u"https://github.com/a/b/releases/download/v1/plugin.wsplugin"_s;
        item.size = good.size();
        item.sha256 = QString::fromLatin1(QCryptographicHash::hash(good, QCryptographicHash::Sha256).toHex());
        QStringList tried;
        const auto result = gallery::fetch(item.url, item.size + 1,
            [&](const QString& url, qint64, const gallery::Progress&) {
                tried.append(url);
                QByteArray bytes = good;
                if (tried.size() == 1) bytes.chop(1);
                else if (tried.size() == 2) bytes[0] = '!';
                return gallery::Fetched {200, bytes, 0};
            }, {}, [&](const QByteArray& bytes) { return gallery::check(item, bytes) == gallery::Problem::None; });
        QCOMPARE(tried, download::sources(item.url));
        QCOMPARE(gallery::check(item, result.body), gallery::Problem::None);
    }

    void cancellationNeverStartsAnotherSource()
    {
        const QString url = u"https://github.com/a/b/releases/download/v1/a.zip"_s;
        int calls = 0;
        bool cancel = false;
        const auto web = [&](const QString&, qint64, const download::Progress& progress) {
            ++calls;
            cancel = true;
            progress(1);
            return download::Fetched {0, {}, ERROR_CANCELLED};
        };
        const auto result = download::fetch(url, 1024, web, [&](qint64) { return !cancel; });
        QCOMPARE(calls, 1);
        QCOMPARE(result.error, static_cast<unsigned long>(ERROR_CANCELLED));
        calls = 0;
        download::fetch(url, 1024, web, [](qint64) { return false; });
        QCOMPARE(calls, 0);
    }

    void localFileIsReadOnceAndStillValidated()
    {
        QTemporaryDir temporary;
        const QString path = temporary.filePath(u"本地 包.zip"_s);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("local package"), 13);
        file.close();
        int calls = 0;
        const auto result = download::fetch(QUrl::fromLocalFile(path).toString(), 100,
            [&](const QString&, qint64, const download::Progress&) { ++calls; return download::Fetched {}; }, {},
            [](const QByteArray& bytes) { return bytes == "something else"; });
        QCOMPARE(calls, 0);
        QCOMPARE(result.body, QByteArray("local package"));
        QCOMPARE(result.error, static_cast<unsigned long>(ERROR_CRC));
    }

    void slowSourcesHaveAGracePeriod()
    {
        QVERIFY(!download::tooSlow(0, 30000));
        QVERIFY(download::tooSlow(0, 30001));
        QVERIFY(download::tooSlow(16383 * 31, 31000));
        QVERIFY(!download::tooSlow(16384 * 31, 31000));
        QVERIFY(!download::tooSlow(17000 * 31, 31000));
    }
};

QTEST_GUILESS_MAIN(TestDownload)
#include "tst_download.moc"
