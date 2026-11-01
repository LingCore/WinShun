#include "platform/OcrFiles.h"
#include "platform/OcrPackage.h"
#include "platform/PaddleOcr.h"
#include "platform/Http.h"
#include "CaptureOcr.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QTemporaryDir>
#include <QtTest>

#include <miniz.h>

using namespace Qt::StringLiterals;

namespace {
QByteArray archive(const QList<QPair<QString, QByteArray>>& files)
{
    mz_zip_archive zip {};
    if (!mz_zip_writer_init_heap(&zip, 0, 0))
        return {};
    const auto close = qScopeGuard([&] { mz_zip_writer_end(&zip); });
    for (const auto& [name, data] : files) {
        if (!mz_zip_writer_add_mem(&zip, name.toUtf8().constData(), data.constData(), static_cast<size_t>(data.size()), MZ_BEST_COMPRESSION))
            return {};
    }
    void* buffer = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size))
        return {};
    const QByteArray result(static_cast<const char*>(buffer), static_cast<qsizetype>(size));
    mz_free(buffer);
    return result;
}

QList<QPair<QString, QByteArray>> smallFiles()
{
    return {{u"onnxruntime.dll"_s, "runtime"}, {u"ocr/PP-OCRv6_det_tiny.onnx"_s, "detector"},
        {u"ocr/PP-OCRv6_rec_tiny.onnx"_s, "recognizer"}};
}

ws::ocr::Package smallPackage()
{
    ws::ocr::Package spec {QString(ws::ocr::kVersion), u"test.zip"_s, 300, {}};
    for (const auto& [path, bytes] : smallFiles())
        spec.files.append({path, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(), bytes.size()});
    return spec;
}

int readComponent(const QString& root)
{
    ws::ocr::setComponentsRoot(root);
    const QString noLegacy = root + u"/no-bundled-copy"_s;
    if (ws::ocr::folder(noLegacy) != QDir(root).filePath(QString(ws::ocr::kVersion)))
        return 2;
    QImage image(700, 100, QImage::Format_RGB32);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        QFont font(u"Microsoft YaHei UI"_s);
        font.setPixelSize(28);
        painter.setFont(font);
        painter.setPen(Qt::black);
        painter.drawText(12, 55, u"Invoice 123.45 测试成功"_s);
    }
    ws::ocr::setComponentsRoot(root + u"/not-installed"_s);
    ws::capture::CaptureOcr::setProgramFolder(noLegacy);
    ws::capture::CaptureOcr reader;
    reader.read(image, 1.0);
    if (!reader.needsPackage() || reader.busy() || !reader.text().isEmpty())
        return 3;
    QSignalSpy setup(&reader, &ws::capture::CaptureOcr::setupRequested);
    reader.setupRecognition();
    if (setup.count() != 1)
        return 4;
    reader.discardPending();
    reader.retry();
    if (reader.needsPackage() || reader.busy())
        return 5;
    reader.read(image, 1.0);
    ws::ocr::setComponentsRoot(root);
    reader.retry(); // uses the original picture, with no network access or second capture
    QElapsedTimer waiting;
    waiting.start();
    while (reader.busy() && waiting.elapsed() < 15000) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    if (reader.busy() || reader.needsPackage())
        return 5;
    const QString text = reader.text();
    qInfo().noquote() << "Installed component recognition:" << text;
    return text.contains(u"123.45") && text.contains(u"测试成功") ? 0 : 6;
}
}

class TestOcrPackage : public QObject {
    Q_OBJECT
    static QList<QPair<QString, QByteArray>> realFiles()
    {
        QList<QPair<QString, QByteArray>> files;
        const QString build = QCoreApplication::applicationDirPath();
        for (const auto& item : ws::ocr::package().files) {
            QString source = QDir(build).filePath(item.path);
            if (item.path == u"licenses/onnxruntime/LICENSE.txt")
                source = build + u"/_deps/onnxruntime-win-x64-1.30.0/LICENSE"_s;
            else if (item.path == u"licenses/onnxruntime/ThirdPartyNotices.txt")
                source = build + u"/_deps/onnxruntime-win-x64-1.30.0/ThirdPartyNotices.txt"_s;
            else if (item.path == u"licenses/paddleocr.txt")
                source = u":/components/paddleocr-license.txt"_s;
            QFile file(source);
            if (!file.open(QIODevice::ReadOnly))
                return {};
            files.append({item.path, file.readAll()});
        }
        return files;
    }

private slots:
    void noPartialInstallation()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto spec = smallPackage();
        std::atomic_bool cancelled {false};
        auto files = smallFiles();
        files.removeLast();
        QVERIFY(!ws::ocr::install(archive(files), root.path(), spec, cancelled).isEmpty());
        QVERIFY(!QFileInfo::exists(root.filePath(spec.version)));
        files = smallFiles();
        files[0].second[0] = 'X';
        QVERIFY(!ws::ocr::install(archive(files), root.path(), spec, cancelled).isEmpty());
        QVERIFY(!QFileInfo::exists(root.filePath(spec.version)));
        cancelled = true;
        QVERIFY(!ws::ocr::install(archive(smallFiles()), root.path(), spec, cancelled).isEmpty());
        QVERIFY(!QFileInfo::exists(root.filePath(spec.version)));
        cancelled = false;
        QCOMPARE(ws::ocr::install(archive(smallFiles()), root.path(), spec, cancelled), QString());
        QVERIFY(QFileInfo::exists(root.filePath(spec.version + u"/.ready"_s)));
    }

    void unexpectedAndDuplicateEntriesAreRejected()
    {
        QTemporaryDir root;
        const auto spec = smallPackage();
        std::atomic_bool cancelled {false};
        for (const QString& name : {u"../outside.txt"_s, u"ocr/../../outside.txt"_s, u"extra.exe"_s, u"onnxruntime.dll"_s}) {
            auto files = smallFiles();
            files.append({name, "unexpected"});
            QVERIFY(!ws::ocr::install(archive(files), root.path(), spec, cancelled).isEmpty());
            QVERIFY(!QFileInfo::exists(root.filePath(spec.version)));
        }
    }

    void failedReplacementKeepsInstalledFiles()
    {
        QTemporaryDir root;
        const auto spec = smallPackage();
        std::atomic_bool cancelled {false};
        QCOMPARE(ws::ocr::install(archive(smallFiles()), root.path(), spec, cancelled), QString());
        auto files = smallFiles();
        files[1].second[0] = 'X';
        QVERIFY(!ws::ocr::install(archive(files), root.path(), spec, cancelled).isEmpty());
        QFile kept(root.filePath(spec.version + u"/ocr/PP-OCRv6_det_tiny.onnx"_s));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("detector"));
    }

    void legacyAndSharedCopiesAreLocatedWithoutMovingData()
    {
        QTemporaryDir temporary;
        const QString before = ws::ocr::componentsRoot();
        const auto restore = qScopeGuard([&] { ws::ocr::setComponentsRoot(before); });
        ws::ocr::setComponentsRoot(temporary.filePath(u"用户组件"_s));
        const QString legacy = temporary.filePath(u"旧版程序"_s);
        for (const auto& [name, bytes] : smallFiles()) {
            const QString target = QDir(legacy).filePath(name);
            QVERIFY(QDir().mkpath(QFileInfo(target).absolutePath()));
            QFile file(target);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(bytes), bytes.size());
        }
        QCOMPARE(ws::ocr::folder(legacy), legacy);
        QCOMPARE(ws::ocr::folder(temporary.filePath(u"empty"_s)), QString());
        std::atomic_bool cancelled {false};
        QCOMPARE(ws::ocr::install(archive(smallFiles()), ws::ocr::componentsRoot(), smallPackage(), cancelled), QString());
        QCOMPARE(ws::ocr::folder(legacy), QDir(ws::ocr::componentsRoot()).filePath(QString(ws::ocr::kVersion)));
        QVERIFY(QFileInfo::exists(QDir(legacy).filePath(u"onnxruntime.dll"_s)));
    }

    void realPinnedPackageInstallsAndReadsOffline()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto files = realFiles();
        const auto spec = ws::ocr::package();
        QCOMPARE(spec.files.size(), 6);
        QCOMPARE(files.size(), spec.files.size());
        std::atomic_bool cancelled {false};
        const QString root = temporary.filePath(u"用户 组件"_s);
        QCOMPARE(ws::ocr::install(archive(files), root, spec, cancelled), QString());
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {u"--read-component"_s, root});
        QVERIFY(child.waitForFinished(30000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, child.readAll().constData());
        // The child has exited, so its loaded DLL no longer holds the test folder.
    }

    void downloadCancellationAndRetryUseTheRealHttpPath()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString beforeRoot = ws::ocr::componentsRoot();
        const QByteArray beforeSource = qgetenv("WINSHUN_OCR_PACKAGE");
        const bool hadSource = qEnvironmentVariableIsSet("WINSHUN_OCR_PACKAGE");
        const auto restore = qScopeGuard([&] {
            ws::ocr::setComponentsRoot(beforeRoot);
            if (hadSource) qputenv("WINSHUN_OCR_PACKAGE", beforeSource);
            else qunsetenv("WINSHUN_OCR_PACKAGE");
        });
        ws::ocr::setComponentsRoot(temporary.filePath(u"组件"_s));
        ws::http::setProxy(ws::http::Proxy::None);
        const auto files = realFiles();
        QCOMPARE(files.size(), 6);
        const QByteArray payload = archive(files);
        QVERIFY(!payload.isEmpty());
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        bool slow = true;
        bool corrupt = false;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto* socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, sent = false, request = QByteArray()]() mutable {
                request += socket->readAll();
                if (!request.contains("\r\n\r\n")) return;
                if (sent) return;
                sent = true;
                if (request.startsWith("GET /mirror-unavailable ")) {
                    socket->write("HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                    return;
                }
                const QByteArray body = corrupt || request.startsWith("GET /mirror-corrupt ") ? QByteArray("not an OCR archive") : payload;
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n");
                if (!slow) {
                    socket->write(body);
                    socket->disconnectFromHost();
                    return;
                }
                auto* timer = new QTimer(socket);
                connect(timer, &QTimer::timeout, socket, [socket, timer, body, offset = qsizetype(0)]() mutable {
                    const QByteArray part = body.mid(offset, 128 * 1024);
                    offset += part.size();
                    socket->write(part);
                    if (offset == body.size()) {
                        timer->stop();
                        socket->disconnectFromHost();
                    }
                });
                timer->start(20);
            });
        });
        qputenv("WINSHUN_OCR_PACKAGE", (u"http://127.0.0.1:%1/ocr.zip"_s.arg(server.serverPort())).toUtf8());
        ws::OcrPackage model(nullptr, temporary.filePath(u"no-bundled-copy"_s));
        ws::OcrPackage observer(nullptr, temporary.filePath(u"another-no-bundled-copy"_s));
        QVERIFY(!model.available());
        model.download();
        QTRY_VERIFY_WITH_TIMEOUT(model.progress() > 0.03, 10000);
        model.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 10000);
        QVERIFY(!model.available());
        QVERIFY(ws::ocr::installedFolder().isEmpty());
        slow = false;
        corrupt = true;
        model.download();
        QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 10000);
        QVERIFY(!model.available());
        QVERIFY(!model.status().isEmpty());
        corrupt = false;
        model.download();
        QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 15000);
        QVERIFY2(model.available(), qUtf8Printable(model.status()));
        QCOMPARE(model.progress(), 1.0);
        QTRY_VERIFY(observer.available());

        // Exercise the production mirror order and real WinHTTP path. Only the
        // transport destination is mapped to our server; validation and retry
        // selection still use the app's real GitHub/component URLs.
        qunsetenv("WINSHUN_OCR_PACKAGE");
        ws::ocr::setComponentsRoot(temporary.filePath(u"镜像组件"_s));
        QMutex triedMutex;
        QStringList tried;
        const ws::download::Fetcher mirroredWeb = [&](const QString& url, qint64 limit, const ws::download::Progress& progress) {
            {
                const QMutexLocker lock(&triedMutex);
                tried.append(url);
            }
            const QString route = url.startsWith(u"https://gh-proxy.com/") ? u"/mirror-unavailable"_s
                : url.startsWith(u"https://ghfast.top/") ? u"/mirror-corrupt"_s : u"/valid"_s;
            auto response = ws::http::get(u"http://127.0.0.1:%1"_s.arg(server.serverPort()) + route, {}, 5000, limit,
                ws::http::Session::Own, progress);
            return ws::download::Fetched {response.status, std::move(response.body), response.error};
        };
        ws::OcrPackage mirrored(nullptr, temporary.filePath(u"no-legacy"_s), mirroredWeb);
        mirrored.download();
        QTRY_VERIFY_WITH_TIMEOUT(!mirrored.busy(), 15000);
        QVERIFY2(mirrored.available(), qUtf8Printable(mirrored.status()));
        const QMutexLocker lock(&triedMutex);
        const QString origin = u"https://github.com/LingCore/WinShun/releases/download/v"_s
            + QString::fromLatin1(WINSHUN_VERSION) + u'/' + ws::ocr::package().name;
        QCOMPARE(tried, ws::download::sources(origin));
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (const int index = args.indexOf(u"--read-component"_s); index >= 0)
        return readComponent(args.value(index + 1));
    TestOcrPackage test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ocrpackage.moc"
