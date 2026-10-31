#include "RecordingWorker.h"

#include "KeyStream.h"
#include "RecordingPointer.h"

#include <QElapsedTimer>
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QLocale>
#include <QTemporaryDir>
#include <QDir>
#include <QScopeGuard>
#include <QUuid>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace Qt::StringLiterals;

namespace ws::capture::recording {
namespace {

constexpr qsizetype kMaxMessage = 64 * 1024;

QJsonObject progressJson(const Progress& p)
{
    return {{u"elapsed"_s, p.elapsedMs}, {u"bytes"_s, p.bytes}, {u"frames"_s, qint64(p.frames)},
        {u"encoder"_s, p.encoder}, {u"hardware"_s, p.hardware}, {u"percent"_s, p.percent}, {u"stage"_s, p.stage}};
}

Progress readProgress(const QJsonObject& json)
{
    Progress p;
    p.elapsedMs = json.value(u"elapsed"_s).toInteger();
    p.bytes = json.value(u"bytes"_s).toInteger();
    p.frames = quint64(std::max<qint64>(0, json.value(u"frames"_s).toInteger()));
    p.encoder = json.value(u"encoder"_s).toString();
    p.hardware = json.value(u"hardware"_s).toBool();
    p.percent = json.value(u"percent"_s).toInt(-1);
    p.stage = json.value(u"stage"_s).toString();
    return p;
}

QByteArray line(const QJsonObject& json)
{
    return QJsonDocument(json).toJson(QJsonDocument::Compact) + '\n';
}

// Only the supervisor writes to stdin; only the media thread writes to
// stdout. Input uses PeekNamedPipe so shutdown never joins a blocked read.
class Input {
public:
    bool next(QJsonObject& json)
    {
        const qsizetype end = m_bytes.indexOf('\n');
        if (end >= 0) {
            json = QJsonDocument::fromJson(m_bytes.first(end)).object();
            m_bytes.remove(0, end + 1);
            return true;
        }
        DWORD available = 0;
        const HANDLE pipe = ::GetStdHandle(STD_INPUT_HANDLE);
        if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) { closed = true; return false; }
        if (available) {
            char buffer[4096];
            DWORD count = 0;
            if (!::ReadFile(pipe, buffer, std::min<DWORD>(sizeof buffer, available), &count, nullptr) || !count) {
                closed = true;
                return false;
            }
            m_bytes.append(buffer, qsizetype(count));
            if (m_bytes.size() > kMaxMessage) { closed = true; return false; }
            return next(json);
        }
        return false;
    }
    bool closed = false;
private:
    QByteArray m_bytes;
};

void send(const QJsonObject& json)
{
    const QByteArray bytes = line(json);
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
}

Result supervise(const WorkerProgram& program, QJsonObject request, const Options* inputs,
    Control& control, const std::function<void(const Progress&)>& progress)
{
    Result result;
    const bool capturing = request.value(u"operation"_s).toString() == u"record";
    if (capturing && control.stop) return result;
    QProcess process;
    process.setProgram(program.executable);
    process.setArguments(program.arguments + QStringList {u"--recording-worker"_s});
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(u"WINSHUN_RECORDING_LOCALE"_s, QLocale().name());
    process.setProcessEnvironment(environment);
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    // Closing this job also removes a driver-stalled child if its parent
    // exits unexpectedly. It contains only the process started below.
    const HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    const auto closeJob = qScopeGuard([&] { if (job) ::CloseHandle(job); });
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    }
    process.start();
    const auto cleanup = qScopeGuard([&] {
        if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(3000); }
    });
    if (!process.waitForStarted(3000)) {
        result.error = QCoreApplication::translate("Recording", "无法启动录屏工作进程：%1").arg(process.errorString());
        return result;
    }
    const HANDLE child = ::OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process.processId()));
    if (child) {
        if (job && !::AssignProcessToJobObject(job, child))
            qWarning("Recording: could not attach worker to lifetime job (error %lu)", ::GetLastError());
        ::CloseHandle(child);
    }
    process.write(line(request));
    QElapsedTimer lastProgress, stopping;
    lastProgress.start();
    QByteArray received;
    bool complete = false;
    bool wasPaused = false;
    QString reason;
    while (!complete) {
        QJsonObject command {{u"stop"_s, control.stop.load()}, {u"discard"_s, control.discard.load()},
            {u"paused"_s, control.paused.load()}};
        if (capturing && control.stop && !stopping.isValid()) stopping.start();
        if (inputs && inputs->clickInput) {
            QJsonArray clicks;
            const auto batch = inputs->clickInput->take();
            for (const Click& click : batch.view())
                clicks.append(QJsonArray {click.point.x(), click.point.y(), click.time, click.secondary});
            command.insert(u"clicks"_s, clicks);
        }
        if (inputs && inputs->keyInput) {
            QJsonArray keys;
            const auto batch = inputs->keyInput->take();
            for (const keycaps::Key& key : batch.view())
                keys.append(QJsonArray {key.time, key.vk, key.mods, key.down, key.repeat, key.inside,
                    int(key.legend), int(key.text[0]), int(key.text[1])});
            command.insert(u"keys"_s, keys);
        }
        if (process.bytesToWrite() > kMaxMessage || process.write(line(command)) < 0) {
            reason = u"control pipe"_s;
            break;
        }
        process.waitForReadyRead(16);
        received += process.readAllStandardOutput();
        const QByteArray errors = process.readAllStandardError();
        if (!errors.isEmpty()) qWarning().noquote() << "Recording worker:" << QString::fromUtf8(errors).trimmed();
        if (received.size() > kMaxMessage) { reason = u"worker protocol"_s; break; }
        qsizetype end;
        while ((end = received.indexOf('\n')) >= 0) {
            const auto message = QJsonDocument::fromJson(received.first(end)).object();
            received.remove(0, end + 1);
            if (message.isEmpty()) { reason = u"worker protocol"_s; break; }
            static_cast<Progress&>(result) = readProgress(message);
            lastProgress.restart();
            if (message.value(u"done"_s).toBool()) {
                result.path = message.value(u"path"_s).toString();
                result.error = message.value(u"error"_s).toString();
                complete = true;
                break;
            }
            if (progress) progress(result);
        }
        if (complete || !reason.isEmpty()) break;
        if (process.state() == QProcess::NotRunning) { reason = u"worker exited (%1)"_s.arg(process.exitCode()); break; }
        const bool paused = capturing && control.paused && !control.stop;
        if (paused || wasPaused) lastProgress.restart();
        wasPaused = paused;
        if ((stopping.isValid() && stopping.elapsed() > program.timeoutMs)
            || (!paused && lastProgress.elapsed() > program.timeoutMs)) {
            reason = u"timeout at %1"_s.arg(result.stage);
            break;
        }
    }
    if (complete) {
        process.waitForFinished(100);
    } else {
        process.kill();
        process.waitForFinished(3000);
        qWarning().noquote() << "Recording:" << reason << "encoder:" << result.encoder << "frames:" << result.frames;
        const QString temporary = request.value(u"temporary"_s).toString();
        if (!temporary.isEmpty()) {
            if (result.frames && QFileInfo::exists(temporary)) result.path = temporary;
            else QFile::remove(temporary); // this attempt's UUID-named file only
        }
        result.error = QCoreApplication::translate("Recording", "录屏工作进程未响应或意外退出（%1）。请重试；已恢复操作。").arg(reason);
        if (!result.path.isEmpty())
            result.error += QCoreApplication::translate("Recording", "\n未完成的录制文件保留在：%1").arg(QDir::toNativeSeparators(result.path));
        if (control.stop && !result.frames) result.error.clear();
    }
    return result;
}

} // namespace

Result recordIsolated(const WorkerProgram& program, const Options& options, Control& control,
    const std::function<void(const Progress&)>& progress, int syntheticMs)
{
    const auto& w = options.keys.words;
    QJsonArray hotkeys;
    for (const auto& key : options.keys.own) hotkeys.append(QJsonArray {key.mods, key.vk});
    QJsonObject request {{u"operation"_s, u"record"_s}, {u"path"_s, options.path},
        {u"region"_s, QJsonArray {options.region.x(), options.region.y(), options.region.width(), options.region.height()}},
        {u"fps"_s, options.fps}, {u"quality"_s, options.quality}, {u"cursor"_s, options.cursor},
        {u"systemAudio"_s, options.systemAudio}, {u"microphone"_s, options.microphone},
        {u"clickHighlights"_s, options.clickHighlights}, {u"pointerScale"_s, options.pointerScale},
        {u"outputScale"_s, options.outputScale}, {u"font"_s, options.keyFont.toString()},
        {u"keys"_s, int(options.keys.mode)}, {u"mouse"_s, options.keys.mouse},
        {u"doubleTap"_s, options.keys.doubleTap}, {u"hotkeys"_s, hotkeys},
        {u"words"_s, QJsonArray {w.space, w.left, w.right, w.middle, w.back, w.forward}}, {u"syntheticMs"_s, syntheticMs}};
    Result result;
    for (int attempt = 0; attempt < (options.software ? 1 : 2); ++attempt) {
        request.insert(u"software"_s, options.software || attempt > 0);
        request.insert(u"temporary"_s, options.path + u'.' + QUuid::createUuid().toString(QUuid::Id128) + u".partial.mp4"_s);
        result = supervise(program, request, &options, control, progress);
        const QString temporary = request.value(u"temporary"_s).toString();
        if (result.error.isEmpty() || result.elapsedMs >= 1000 || (!result.path.isEmpty() && result.path != temporary)
            || control.stop || QFileInfo::exists(options.path)) break;
        if (!options.software && attempt == 0) {
            qWarning().noquote() << "Recording: retrying with software encoding after" << result.error;
            QFile::remove(temporary); // only this failed startup attempt's staging file
            Progress retry;
            retry.stage = u"software fallback"_s;
            if (progress) progress(retry);
        }
    }
    qInfo().noquote() << "Recording finished: encoder" << result.encoder << "hardware" << result.hardware
                     << "frames" << result.frames << "elapsed ms" << result.elapsedMs << "stage" << result.stage;
    if (!result.error.isEmpty()) qWarning().noquote() << result.error;
    return result;
}

Result exportGifIsolated(const WorkerProgram& program, const QString& mp4, const GifOptions& options,
    Control& control, const std::function<void(const Progress&)>& progress)
{
    QTemporaryDir staging(QDir(QFileInfo(options.path).absolutePath()).filePath(u".winshun-gif-XXXXXX"_s));
    if (!staging.isValid()) {
        Result result;
        result.error = QCoreApplication::translate("Recording", "无法创建 GIF 临时文件夹，请检查保存位置和剩余空间。");
        return result;
    }
    return supervise(program, {{u"operation"_s, u"gif"_s}, {u"input"_s, mp4}, {u"path"_s, options.path},
        {u"fps"_s, options.fps}, {u"longest"_s, options.maximumEdge}, {u"scratch"_s, staging.path()}}, nullptr, control, progress);
}

static int runWorker()
{
    Input input;
    QJsonObject request;
    QElapsedTimer waiting;
    waiting.start();
    while (!input.next(request)) {
        if (input.closed || waiting.elapsed() > 5000) return 2;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto r = request.value(u"region"_s).toArray();
    Options options;
    if (r.size() == 4) options.region = QRect(r[0].toInt(), r[1].toInt(), r[2].toInt(), r[3].toInt());
    options.path = request.value(u"path"_s).toString();
    options.temporary = request.value(u"temporary"_s).toString();
    options.fps = request.value(u"fps"_s).toInt(30);
    options.quality = request.value(u"quality"_s).toInt(75);
    options.cursor = request.value(u"cursor"_s).toBool();
    options.systemAudio = request.value(u"systemAudio"_s).toBool();
    options.microphone = request.value(u"microphone"_s).toBool();
    options.software = request.value(u"software"_s).toBool();
    options.clickHighlights = request.value(u"clickHighlights"_s).toBool();
    options.pointerScale = request.value(u"pointerScale"_s).toDouble(1);
    options.outputScale = request.value(u"outputScale"_s).toDouble(1);
    options.keyFont.fromString(request.value(u"font"_s).toString());
    options.keys.mode = keycaps::Mode(request.value(u"keys"_s).toInt());
    options.keys.mouse = request.value(u"mouse"_s).toBool();
    options.keys.doubleTap = request.value(u"doubleTap"_s).toInteger(5'000'000);
    const auto words = request.value(u"words"_s).toArray();
    if (words.size() == 6) options.keys.words = {words[0].toString(), words[1].toString(), words[2].toString(),
        words[3].toString(), words[4].toString(), words[5].toString()};
    for (const auto& value : request.value(u"hotkeys"_s).toArray()) {
        const auto key = value.toArray();
        if (key.size() == 2) options.keys.own.push_back({std::uint8_t(key[0].toInt()), std::uint16_t(key[1].toInt())});
    }
    options.clickInput = std::make_shared<ClickStream>(options.region);
    options.keyInput = std::make_shared<KeyStream>();
    Control control;
    std::jthread commands([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            QJsonObject command;
            while (input.next(command)) {
                control.paused = command.value(u"paused"_s).toBool();
                control.discard = command.value(u"discard"_s).toBool();
                if (command.value(u"stop"_s).toBool()) control.stop = true;
                for (const auto& value : command.value(u"clicks"_s).toArray()) {
                    const auto click = value.toArray();
                    if (click.size() == 4) options.clickInput->push(options.region.topLeft() + QPoint(click[0].toInt(), click[1].toInt()),
                        click[3].toBool(), click[2].toInteger());
                }
                for (const auto& value : command.value(u"keys"_s).toArray()) {
                    const auto key = value.toArray();
                    if (key.size() != 9) continue;
                    options.keyInput->push({key[0].toInteger(), std::uint16_t(key[1].toInt()), std::uint8_t(key[2].toInt()),
                        key[3].toBool(), key[4].toBool(), key[5].toBool(), char16_t(key[6].toInt()),
                        {char16_t(key[7].toInt()), char16_t(key[8].toInt())}});
                }
            }
            if (input.closed) { control.stop = true; control.discard = true; return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    const auto update = [](const Progress& p) { send(progressJson(p)); };
    Result result;
    if (request.value(u"operation"_s).toString() == u"gif") {
        result = exportGif(request.value(u"input"_s).toString(), {options.path, options.fps,
            request.value(u"longest"_s).toInt(), request.value(u"scratch"_s).toString()}, control, update);
    } else if (request.value(u"operation"_s).toString() == u"record") {
        const int syntheticMs = request.value(u"syntheticMs"_s).toInt(-1);
        result = syntheticMs < 0 ? record(options, control, update) : sample(options, syntheticMs, control, update);
    } else return 2;
    QJsonObject reply = progressJson(result);
    reply.insert(u"done"_s, true);
    reply.insert(u"path"_s, result.path);
    reply.insert(u"error"_s, result.error);
    send(reply);
    return 0;
}

int recordingWorkerMain()
{
    int code = 2;
    std::jthread media([&] { code = runWorker(); });
    media.join(); // this process owns no UI; Qt's main thread stays out of COM's MTA
    return code;
}

} // namespace ws::capture::recording
