#include "OcrWorker.h"

#include <QElapsedTimer>
#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>

#include <windows.h>
#include <algorithm>
#include <stdexcept>

using namespace Qt::StringLiterals;

namespace ws::ocrsearch {
namespace {
constexpr qsizetype kMaxReply = 16 << 20;
[[noreturn]] void fail(const QString& message)
{
    throw std::runtime_error(message.toUtf8().constData());
}
} // namespace

Worker::Worker(QString program, QString appDir, QString dataDir, QString engine, int timeoutMs)
    : m_program(std::move(program))
    , m_appDir(std::move(appDir))
    , m_dataDir(std::move(dataDir))
    , m_engine(std::move(engine))
    , m_timeoutMs(std::clamp(timeoutMs, 50, 120000))
{
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
}
Worker::~Worker()
{
    reset();
}
void Worker::reset()
{
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
    if (m_process.isOpen()) {
        m_process.readAllStandardOutput();
        m_process.readAllStandardError();
        m_process.close();
    }
}
QJsonObject Worker::read(const QString& path, const Cancel& cancel)
{
    return request({{u"path"_s, path}, {u"dataDir"_s, m_dataDir}, {u"engine"_s, m_engine}}, cancel);
}
QImage Worker::preview(const QString& path, const QJsonObject& source, const Cancel& cancel)
{
    const auto reply = request({{u"path"_s, path}, {u"preview"_s, source}}, cancel);
    const QImage image = QImage::fromData(QByteArray::fromBase64(reply.value(u"image"_s).toString().toLatin1()), "PNG");
    if (image.isNull())
        fail(QObject::tr("预览图片无效，请打开原文件查看"));
    return image;
}
QJsonObject Worker::request(const QJsonObject& parameters, const Cancel& cancel)
{
    if (m_process.state() != QProcess::Running) {
        reset();
        m_process.start(m_program, {u"--ocr-worker"_s, u"--app-dir"_s, m_appDir});
        if (!m_process.waitForStarted(5000))
            fail(QObject::tr("无法启动识别工作进程：%1").arg(m_process.errorString()));
    }
    const QByteArray bytes = QJsonDocument(parameters).toJson(QJsonDocument::Compact) + '\n';
    if (m_process.write(bytes) != bytes.size()) {
        reset();
        fail(QObject::tr("无法提交识别任务"));
    }
    QElapsedTimer timer;
    timer.start();
    QByteArray reply, diagnostics;
    while (!reply.contains('\n')) {
        if (cancel && cancel()) {
            reset();
            fail(QObject::tr("已暂停，稍后可继续"));
        }
        if (timer.elapsed() >= m_timeoutMs) {
            reset();
            fail(QObject::tr("文件处理超过 %1 秒，已跳过，稍后可重试").arg(m_timeoutMs / 1000));
        }
        m_process.waitForReadyRead(50);
        reply += m_process.readAllStandardOutput();
        diagnostics = (diagnostics + m_process.readAllStandardError()).right(4096);
        if (reply.size() > kMaxReply) {
            reset();
            fail(QObject::tr("识别结果超过 16 MB，已停止该文件"));
        }
        if (m_process.state() == QProcess::NotRunning && !reply.contains('\n')) {
            reset();
            fail(QObject::tr("识别工作进程异常结束，此文件未完成：%1").arg(QString::fromUtf8(diagnostics)));
        }
    }
    QJsonParseError error {};
    const auto document = QJsonDocument::fromJson(reply.left(reply.indexOf('\n')), &error);
    if (!document.isObject()) {
        reset();
        fail(QObject::tr("识别工作进程返回了无效数据"));
    }
    const QJsonObject object = document.object();
    if (!object.value(u"error"_s).toString().isEmpty())
        fail(object.value(u"error"_s).toString());
    return object;
}

int workerMain(const QString& appDir)
{
    std::unique_ptr<PaddleOcr> reader;
    QByteArray pending;
    const HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE), output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    char buffer[65536];
    for (;;) {
        while (!pending.contains('\n')) {
            DWORD got = 0;
            if (!::ReadFile(input, buffer, sizeof buffer, &got, nullptr) || !got)
                return 0;
            pending.append(buffer, got);
            if (pending.size() > 256 * 1024)
                return 2;
        }
        const qsizetype newline = pending.indexOf('\n');
        const auto request = QJsonDocument::fromJson(pending.left(newline)).object();
        pending.remove(0, newline + 1);
        QJsonObject response;
        try {
            if (request.contains(u"preview"_s)) {
                const QImage image
                    = previewImage(request.value(u"path"_s).toString(), request.value(u"preview"_s).toObject());
                QByteArray png;
                QBuffer imageBuffer(&png);
                imageBuffer.open(QIODevice::WriteOnly);
                if (!image.save(&imageBuffer, "PNG") || png.size() > (10 << 20))
                    fail(QObject::tr("预览图片过大，请打开原文件查看"));
                response = {{u"image"_s, QString::fromLatin1(png.toBase64())}};
            } else {
                Store store(request.value(u"dataDir"_s).toString(), 0);
                QJsonArray pages;
                QHash<QByteArray, QJsonObject> recognized;
                const QString engine = request.value(u"engine"_s).toString();
                qsizetype bytes = 0;
                const auto result = extractImages(request.value(u"path"_s).toString(),
                    [&](const QImage& image, const QString& label, const QJsonObject& source) {
                        const QByteArray hash = imageHash(image, engine);
                        QJsonObject data = recognized.value(hash);
                        if (data.isEmpty()) {
                            const qint64 id = store.asset(hash);
                            data = {{u"hash"_s, QString::fromLatin1(hash.toHex())}, {u"asset"_s, id},
                                {u"width"_s, image.width()}, {u"height"_s, image.height()}};
                            if (!id) {
                                QString error;
                                if (!reader)
                                    reader = PaddleOcr::load(appDir, 2, &error);
                                if (!reader)
                                    fail(error);
                                QList<OcrPiece> pieces;
                                if (!reader->readPieces(image, &pieces, &error))
                                    fail(error);
                                data.insert(u"pieces"_s, piecesToJson(pieces));
                            }
                            recognized.insert(hash, data);
                        }
                        data.insert(u"label"_s, label);
                        data.insert(u"source"_s, source);
                        bytes += QJsonDocument(data).toJson(QJsonDocument::Compact).size();
                        if (bytes > kMaxReply - 4096)
                            fail(QObject::tr("识别结果超过 16 MB，已停止该文件"));
                        pages.append(data);
                    });
                response = {{u"pages"_s, pages}, {u"partial"_s, result.partial}, {u"message"_s, result.message},
                    {u"images"_s, result.images}};
            }
        } catch (const std::exception& e) {
            response = {{u"error"_s, QString::fromUtf8(e.what())}};
        }
        const QByteArray line = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
        qsizetype sent = 0;
        while (sent < line.size()) {
            DWORD written = 0;
            if (!::WriteFile(output, line.constData() + sent, DWORD(line.size() - sent), &written, nullptr) || !written)
                return 0;
            sent += written;
        }
    }
}

} // namespace ws::ocrsearch
