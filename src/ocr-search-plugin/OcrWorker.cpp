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
constexpr int kPreviewSide = 2048; // the preview pane is never larger
[[noreturn]] void fail(const QString& message)
{
    throw std::runtime_error(message.toUtf8().constData());
}
[[noreturn]] void stop(const QString& message)
{
    throw ScanStopped(message.toUtf8().constData());
}
// The process ended because a DLL or an export it needs is missing or wrong
// (as after a broken update), not because of the file it was reading.
bool loaderFailure(int exitCode)
{
    switch (static_cast<quint32>(exitCode)) {
    case 0xC0000135: // STATUS_DLL_NOT_FOUND
    case 0xC0000139: // STATUS_ENTRYPOINT_NOT_FOUND
    case 0xC000007B: // STATUS_INVALID_IMAGE_FORMAT
    case 0xC0000142: // STATUS_DLL_INIT_FAILED
    case 0xC06D007E: // delay-load: module not found
    case 0xC06D007F: // delay-load: procedure not found
        return true;
    default:
        return false;
    }
}
bool writeLine(HANDLE output, const QByteArray& line)
{
    qsizetype sent = 0;
    while (sent < line.size()) {
        DWORD written = 0;
        if (!::WriteFile(output, line.constData() + sent, DWORD(line.size() - sent), &written, nullptr) || !written)
            return false;
        sent += written;
    }
    return true;
}
QByteArray encoded(const QImage& image, const char* format, int quality)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    return image.save(&buffer, format, quality) ? bytes : QByteArray();
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
    const QImage image = QImage::fromData(QByteArray::fromBase64(reply.value(u"image"_s).toString().toLatin1()));
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
            stop(QObject::tr("无法启动识别工作进程：%1").arg(m_process.errorString()));
    }
    const QByteArray bytes = QJsonDocument(parameters).toJson(QJsonDocument::Compact) + '\n';
    if (m_process.write(bytes) != bytes.size()) {
        reset();
        stop(QObject::tr("无法提交识别任务"));
    }
    // The deadline is for silence: the worker reports each page or picture it
    // is done with, so a long document that keeps going is never cut short.
    QElapsedTimer silence;
    silence.start();
    QByteArray buffer, diagnostics;
    for (;;) {
        for (qsizetype newline = buffer.indexOf('\n'); newline >= 0; newline = buffer.indexOf('\n')) {
            const auto document = QJsonDocument::fromJson(buffer.left(newline));
            buffer.remove(0, newline + 1);
            if (!document.isObject()) {
                reset();
                fail(QObject::tr("识别工作进程返回了无效数据"));
            }
            const QJsonObject object = document.object();
            if (object.contains(u"progress"_s)) {
                silence.restart();
                continue;
            }
            if (!object.value(u"error"_s).toString().isEmpty()) {
                if (object.value(u"stop"_s).toBool())
                    stop(object.value(u"error"_s).toString());
                fail(object.value(u"error"_s).toString());
            }
            return object;
        }
        if (cancel && cancel()) {
            reset();
            fail(QObject::tr("已暂停，稍后可继续"));
        }
        if (silence.elapsed() >= m_timeoutMs) {
            reset();
            fail(QObject::tr("文件处理 %1 秒没有进展，已跳过；可用“重新检查所有文件”重试").arg(m_timeoutMs / 1000));
        }
        m_process.waitForReadyRead(50);
        buffer += m_process.readAllStandardOutput();
        diagnostics = (diagnostics + m_process.readAllStandardError()).right(4096);
        if (buffer.size() > kMaxReply) {
            reset();
            fail(QObject::tr("识别结果超过 16 MB，已停止该文件"));
        }
        if (m_process.state() == QProcess::NotRunning && !buffer.contains('\n')) {
            const bool loader = m_process.exitStatus() == QProcess::CrashExit && loaderFailure(m_process.exitCode());
            reset();
            if (loader)
                stop(QObject::tr("识别工作进程无法加载所需组件，请重新安装 Win顺 或增强识别组件"));
            fail(QObject::tr("识别工作进程异常结束，此文件未完成：%1").arg(QString::fromUtf8(diagnostics)));
        }
    }
}

int workerMain(const QString& appDir)
{
    std::unique_ptr<PaddleOcr> reader;
    bool background = false;
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
                QImage image
                    = previewImage(request.value(u"path"_s).toString(), request.value(u"preview"_s).toObject());
                // Highlights are placed by the recognised image's size, so the
                // preview may be smaller; JPEG keeps big photos within the limit.
                if (std::max(image.width(), image.height()) > kPreviewSide)
                    image = image.scaled(kPreviewSide, kPreviewSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                QByteArray bytes = encoded(image, "JPG", 90);
                if (bytes.isEmpty())
                    bytes = encoded(image, "PNG", -1);
                if (bytes.isEmpty() || bytes.size() > (10 << 20))
                    fail(QObject::tr("预览图片过大，请打开原文件查看"));
                response = {{u"image"_s, QString::fromLatin1(bytes.toBase64())}};
            } else {
                // Indexing comes after the user's work: background CPU, I/O
                // and memory priority (a preview worker is its own process).
                if (!background)
                    background = ::SetPriorityClass(::GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN);
                Store store(request.value(u"dataDir"_s).toString(), 0);
                QJsonArray pages;
                QHash<QByteArray, QJsonObject> recognized;
                const QString engine = request.value(u"engine"_s).toString();
                qsizetype bytes = 0;
                int done = 0;
                const auto result = extractImages(
                    request.value(u"path"_s).toString(),
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
                                    throw ScanStopped(error.toStdString());
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
                    },
                    {},
                    [&] {
                        if (!writeLine(output, "{\"progress\":" + QByteArray::number(++done) + "}\n"))
                            fail(u"The plugin stopped listening"_s);
                    });
                response = {{u"pages"_s, pages}, {u"partial"_s, result.partial}, {u"message"_s, result.message},
                    {u"images"_s, result.images}};
            }
        } catch (const ScanStopped& e) {
            response = {{u"error"_s, QString::fromUtf8(e.what())}, {u"stop"_s, true}};
        } catch (const std::exception& e) {
            response = {{u"error"_s, QString::fromUtf8(e.what())}};
        }
        if (!writeLine(output, QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n'))
            return 0;
    }
}

} // namespace ws::ocrsearch
