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

// A line of Japanese read by the installed component: 0 with the larger
// reading model and the kana read, 20 with the user's own model and the kana
// read, 10 with the tiny one and none read (it has no kana); anything else
// is wrong.
int readJapanese(const QString& root)
{
    ws::ocr::setComponentsRoot(root);
    QImage image(700, 100, QImage::Format_RGB32);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        QFont font(u"Yu Gothic UI"_s);
        font.setPixelSize(28);
        painter.setFont(font);
        painter.setPen(Qt::black);
        painter.drawText(12, 55, u"設定を保存しますか 2026年"_s);
    }
    QString error;
    const std::unique_ptr<ws::PaddleOcr> reader = ws::PaddleOcr::load(root + u"/no-bundled-copy"_s, 2, &error);
    QStringList lines;
    if (!reader || !reader->read(image, &lines, &error)) {
        qInfo().noquote() << "Japanese reading failed:" << error;
        return 2;
    }
    const QString text = lines.join(u' ');
    qInfo().noquote() << "Japanese read by the" << (reader->own() ? "user's own" : reader->precise() ? "larger" : "tiny") << "model:" << text;
    const bool kana = text.contains(u"を保存しますか");
    if (reader->own())
        return kana && text.contains(u"2026") ? 20 : 26;
    if (reader->precise())
        return kana && text.contains(u"2026") ? 0 : 6;
    return kana ? 7 : 10;
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

    // The larger reading model: one file, taken only with its size and hash,
    // found by its receipt, and gone again when removed.
    void preciseModelIsInstalledWholeAndRemoved()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString beforeRoot = ws::ocr::componentsRoot();
        const auto restore = qScopeGuard([&] { ws::ocr::setComponentsRoot(beforeRoot); });
        ws::ocr::setComponentsRoot(temporary.filePath(u"用户 组件"_s));
        const QByteArray model("a reading model");
        const ws::ocr::Model spec {u"unused"_s, QCryptographicHash::hash(model, QCryptographicHash::Sha256).toHex(), model.size()};
        std::atomic_bool cancelled {false};
        QVERIFY(ws::ocr::preciseModel().isEmpty());
        QVERIFY(!ws::ocr::installPrecise("a reading mode1", spec, cancelled).isEmpty()); // same size, another file
        QVERIFY(!ws::ocr::installPrecise(model + model, spec, cancelled).isEmpty());
        QVERIFY(ws::ocr::preciseModel().isEmpty());
        QVERIFY(!QFileInfo::exists(ws::ocr::preciseFolder()));
        cancelled = true;
        QVERIFY(!ws::ocr::installPrecise(model, spec, cancelled).isEmpty());
        QVERIFY(ws::ocr::preciseModel().isEmpty());
        cancelled = false;
        QCOMPARE(ws::ocr::installPrecise(model, spec, cancelled), QString());
        const QString installed = ws::ocr::preciseModel();
        QCOMPARE(QFileInfo(installed).fileName(), QString(ws::ocr::kPreciseModel));
        QFile file(installed);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), model);
        file.close();
        QCOMPARE(ws::ocr::installPrecise(model, spec, cancelled), QString()); // over itself
        QVERIFY(!ws::ocr::preciseModel().isEmpty());
        // Nothing staged is left beside it.
        QCOMPARE(QDir(ws::ocr::componentsRoot()).entryList(QDir::Dirs | QDir::NoDotAndDotDot), QStringList {QString(ws::ocr::kPreciseVersion)});
        // A model without its receipt (a copy cut short) is not used.
        QVERIFY(QFile::remove(ws::ocr::preciseFolder() + u"/.ready"_s));
        QVERIFY(ws::ocr::preciseModel().isEmpty());
        QCOMPARE(ws::ocr::installPrecise(model, spec, cancelled), QString());
        QCOMPARE(ws::ocr::removePrecise(), QString());
        QVERIFY(ws::ocr::preciseModel().isEmpty());
        QVERIFY(!QFileInfo::exists(ws::ocr::preciseFolder()));
        QCOMPARE(ws::ocr::removePrecise(), QString()); // nothing there: nothing to say
    }

    // The settings' row: a download that is not the model is refused and
    // said so, the model is installed, another object sees it, removal too.
    void preciseModelDownloadsAreCheckedAndCanBeRemoved()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString beforeRoot = ws::ocr::componentsRoot();
        const auto restore = qScopeGuard([&] { ws::ocr::setComponentsRoot(beforeRoot); });
        ws::ocr::setComponentsRoot(temporary.filePath(u"组件"_s));
        const QByteArray model(4096, 'm');
        const ws::ocr::Model spec {u"https://models.example/rec.onnx"_s, QCryptographicHash::hash(model, QCryptographicHash::Sha256).toHex(), model.size()};
        QByteArray served(4096, 'x');
        QStringList asked;
        QMutex mutex;
        const ws::download::Fetcher web = [&](const QString& url, qint64, const ws::download::Progress& progress) {
            const QMutexLocker lock(&mutex);
            asked.append(url);
            if (progress)
                progress(served.size());
            return ws::download::Fetched {200, served, 0};
        };
        ws::OcrModel row(nullptr, web, spec);
        ws::OcrModel other(nullptr, web, spec); // the settings window's, beside the program's
        QVERIFY(!row.installed());
        QVERIFY(row.status().isEmpty());
        row.download();
        QVERIFY(row.busy());
        QTRY_VERIFY_WITH_TIMEOUT(!row.busy(), 15000);
        QVERIFY(!row.installed());
        QVERIFY(!row.status().isEmpty());
        QVERIFY(ws::ocr::preciseModel().isEmpty());
        {
            const QMutexLocker lock(&mutex);
            QCOMPARE(asked, QStringList {spec.url}); // a model's address has no mirrors
            served = model;
        }
        QSignalSpy seen(&other, &ws::OcrModel::changed);
        row.download();
        QTRY_VERIFY_WITH_TIMEOUT(!row.busy(), 15000);
        QVERIFY2(row.installed(), qUtf8Printable(row.status()));
        QVERIFY(row.status().isEmpty());
        QCOMPARE(row.progress(), 1.0);
        QTRY_VERIFY_WITH_TIMEOUT(other.installed(), 5000); // by the folder's change
        QVERIFY(seen.count() > 0);
        row.download(); // installed: nothing to do
        QVERIFY(!row.busy());
        row.remove();
        QVERIFY(!row.installed());
        QVERIFY2(row.status().isEmpty(), qUtf8Printable(row.status()));
        QVERIFY(ws::ocr::preciseModel().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!other.installed(), 5000);
        // A file picked by hand that is not the model.
        const QString wrong = temporary.filePath(u"别的.onnx"_s);
        QFile file(wrong);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(4096, 'x'));
        file.close();
        row.installFile(wrong);
        QTRY_VERIFY_WITH_TIMEOUT(!row.busy(), 15000);
        QVERIFY(!row.installed());
        QVERIFY(row.status().contains(QString(ws::ocr::kPreciseModel)));
    }

    // The real one: without it the tiny model reads no kana, with it Japanese
    // is read, and after its removal the tiny model reads again.
    void realPreciseModelReadsJapanese()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString beforeRoot = ws::ocr::componentsRoot();
        const auto restore = qScopeGuard([&] { ws::ocr::setComponentsRoot(beforeRoot); });
        const QString root = temporary.filePath(u"用户 组件"_s);
        ws::ocr::setComponentsRoot(root);
        std::atomic_bool cancelled {false};
        QCOMPARE(ws::ocr::install(archive(realFiles()), root, ws::ocr::package(), cancelled), QString());
        const auto read = [&] {
            QProcess child;
            child.start(QCoreApplication::applicationFilePath(), {u"--read-japanese"_s, root});
            if (!child.waitForFinished(30000) || child.exitStatus() != QProcess::NormalExit)
                return -1;
            qInfo().noquote() << child.readAllStandardError().trimmed();
            return child.exitCode();
        };
        QCOMPARE(read(), 10);
        QFile model(QCoreApplication::applicationDirPath() + u"/ocr-precise/"_s + QString(ws::ocr::kPreciseModel));
        QVERIFY2(model.open(QIODevice::ReadOnly), "the build folder has no ocr-precise model (tests/CMakeLists.txt)");
        QCOMPARE(ws::ocr::installPrecise(model.readAll(), ws::ocr::preciseSpec(), cancelled), QString());
        QCOMPARE(read(), 0);
        QCOMPARE(ws::ocr::removePrecise(), QString());
        QCOMPARE(read(), 10);
    }

    // A model of the user's own: a file that is no reading model is refused
    // with nothing installed; a real one is tried, copied in under its own
    // name, read with before the others, and gone again when removed.
    void ownModelIsTriedUsedAndRemoved()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString beforeRoot = ws::ocr::componentsRoot();
        const auto restore = qScopeGuard([&] { ws::ocr::setComponentsRoot(beforeRoot); });
        const QString root = temporary.filePath(u"用户 组件"_s);
        ws::ocr::setComponentsRoot(root);
        std::atomic_bool cancelled {false};
        // Tried in this process with the build folder's own copy of the
        // runtime: one loaded from the temporary folder would keep it there.
        const QString build = QCoreApplication::applicationDirPath();
        const QString program = build;
        // Not a model at all; and a model that finds lines, not one that reads them.
        const QString junk = temporary.filePath(u"不是模型.onnx"_s);
        QFile file(junk);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(4096, 'x'));
        file.close();
        QVERIFY(!ws::ocr::installOwn(junk, program, cancelled).isEmpty());
        QVERIFY(!ws::ocr::installOwn(build + u"/ocr/PP-OCRv6_det_tiny.onnx"_s, program, cancelled).isEmpty());
        QVERIFY(!ws::ocr::installOwn(temporary.filePath(u"没有.onnx"_s), program, cancelled).isEmpty());
        QVERIFY(ws::ocr::ownModel().file.isEmpty());
        QVERIFY(!QFileInfo::exists(ws::ocr::ownFolder()));
        // The larger model under a name of the user's, as their own.
        const QString mine = temporary.filePath(u"我下载的 模型.onnx"_s);
        QVERIFY(QFile::copy(build + u"/ocr-precise/"_s + QString(ws::ocr::kPreciseModel), mine));
        ws::OcrModel row(nullptr, {}, ws::ocr::preciseSpec(), program);
        QVERIFY(row.ownName().isEmpty());
        row.useOwnFile(mine);
        QVERIFY(row.busy());
        QTRY_VERIFY_WITH_TIMEOUT(!row.busy(), 30000);
        QVERIFY2(row.status().isEmpty(), qUtf8Printable(row.status()));
        QCOMPARE(row.ownName(), u"我下载的 模型.onnx"_s);
        const ws::ocr::OwnModel own = ws::ocr::ownModel();
        QVERIFY(QFileInfo(own.file).isFile());
        QVERIFY(own.dictionary.isEmpty()); // its characters are in it
        QVERIFY(own.id.contains(own.name));
        const auto read = [&] {
            QProcess child;
            child.start(QCoreApplication::applicationFilePath(), {u"--read-japanese"_s, root});
            if (!child.waitForFinished(30000) || child.exitStatus() != QProcess::NormalExit)
                return -1;
            qInfo().noquote() << child.readAllStandardError().trimmed();
            return child.exitCode();
        };
        QCOMPARE(ws::ocr::install(archive(realFiles()), root, ws::ocr::package(), cancelled), QString()); // for the child
        QCOMPARE(read(), 20);
        row.useOwnFile(junk); // a wrong pick leaves the one in use alone
        QTRY_VERIFY_WITH_TIMEOUT(!row.busy(), 30000);
        QVERIFY(!row.status().isEmpty());
        QCOMPARE(row.ownName(), u"我下载的 模型.onnx"_s);
        row.removeOwn();
        QVERIFY(row.ownName().isEmpty());
        QVERIFY(ws::ocr::ownModel().file.isEmpty());
        QCOMPARE(read(), 10);
    }

    // A model without its characters (PaddleOCR's own exports): refused with
    // the dictionary's wanted name while there is none beside it, and with a
    // dictionary of another length; read with once the right one is there.
    // The pair is made by tools/ocr_strip_dictionary.py (it needs Python's
    // onnx), so only where the build folder has it.
    void ownModelTakesTheDictionaryBesideIt()
    {
        const QString build = QCoreApplication::applicationDirPath();
        const QString model = build + u"/ocr-precise/nodict.onnx"_s;
        if (!QFileInfo::exists(model) || !QFileInfo::exists(build + u"/ocr-precise/nodict.txt"_s))
            QSKIP("no ocr-precise/nodict.onnx in the build folder (tools/ocr_strip_dictionary.py)");
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString beforeRoot = ws::ocr::componentsRoot();
        const auto restore = qScopeGuard([&] { ws::ocr::setComponentsRoot(beforeRoot); });
        const QString root = temporary.filePath(u"组件"_s);
        ws::ocr::setComponentsRoot(root);
        std::atomic_bool cancelled {false};
        const QString mine = temporary.filePath(u"官方 导出.onnx"_s);
        const QString words = temporary.filePath(u"官方 导出.txt"_s);
        QVERIFY(QFile::copy(model, mine));
        QVERIFY(ws::ocr::installOwn(mine, build, cancelled).contains(u"官方 导出.txt"));
        QFile shorter(words);
        QVERIFY(shorter.open(QIODevice::WriteOnly));
        shorter.write("a\nb\nc\n");
        shorter.close();
        QVERIFY(!ws::ocr::installOwn(mine, build, cancelled).isEmpty());
        QVERIFY(ws::ocr::ownModel().file.isEmpty());
        QVERIFY(QFile::remove(words));
        QVERIFY(QFile::copy(build + u"/ocr-precise/nodict.txt"_s, words));
        QCOMPARE(ws::ocr::installOwn(mine, build, cancelled), QString());
        QVERIFY(QFileInfo(ws::ocr::ownModel().dictionary).isFile());
        QCOMPARE(ws::ocr::install(archive(realFiles()), root, ws::ocr::package(), cancelled), QString()); // for the child
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {u"--read-japanese"_s, root});
        QVERIFY(child.waitForFinished(30000));
        qInfo().noquote() << child.readAllStandardError().trimmed();
        QCOMPARE(child.exitCode(), 20);
        QCOMPARE(ws::ocr::removeOwn(), QString());
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (const int index = args.indexOf(u"--read-component"_s); index >= 0)
        return readComponent(args.value(index + 1));
    if (const int index = args.indexOf(u"--read-japanese"_s); index >= 0)
        return readJapanese(args.value(index + 1));
    TestOcrPackage test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ocrpackage.moc"
