#include "Diagnostics.h"

#include "SystemCatalog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSysInfo>

#include <thread>
#include <windows.h>

#include <miniz.h>

using namespace Qt::StringLiterals;

namespace ws::diagnostics {
namespace {
constexpr qint64 kMaxSourceBytes = 1 << 20;
QString tr(const char* text)
{
    return QCoreApplication::translate("ws::diagnostics", text);
}
} // namespace

QJsonObject environment()
{
    MEMORYSTATUSEX memory {sizeof memory};
    ::GlobalMemoryStatusEx(&memory);
    return {{u"version"_s, QStringLiteral(WINSHUN_VERSION)}, {u"os"_s, QSysInfo::prettyProductName()},
        {u"kernel"_s, QSysInfo::kernelVersion()}, {u"architecture"_s, QSysInfo::currentCpuArchitecture()},
        {u"logical_processors"_s, static_cast<int>(std::thread::hardware_concurrency())},
        {u"memory_mb"_s, static_cast<qint64>(memory.ullTotalPhys >> 20)}, {u"administrator"_s, userIsAdministrator()}};
}

Exported exportArchive(Log& log, const QString& dataFolder, const QString& serviceFolder, QJsonObject context)
{
    std::vector<Log::File> files = log.snapshot();
    QJsonArray unavailable;
    // Deliberately whitelist runtime logs. Settings, clipboard, search
    // history, MCP/plugin conversations and document data are not packed.
    const auto add = [&](const QString& folder, const QString& name, const QString& packedName) {
        if (folder.isEmpty())
            return;
        QFile file(QDir(folder).filePath(name));
        if (!file.exists())
            return;
        if (!file.open(QIODevice::ReadOnly)) {
            unavailable.append(packedName);
            return;
        }
        if (file.size() > kMaxSourceBytes)
            file.seek(file.size() - kMaxSourceBytes); // recent errors, bounded even for an old giant log
        files.push_back({packedName, file.read(kMaxSourceBytes)});
    };
    add(dataFolder, u"WinShun.log"_s, u"app/WinShun.log"_s);
    add(dataFolder, u"WinShun.old.log"_s, u"app/WinShun.old.log"_s);
    add(serviceFolder, u"service.log"_s, u"service/service.log"_s);
    add(serviceFolder, u"service.old.log"_s, u"service/service.old.log"_s);
    context[u"schema_version"_s] = 1;
    context[u"generated_at"_s] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    context[u"environment"_s] = environment();
    context[u"unavailable_logs"_s] = unavailable;
    context[u"privacy"_s]
        = u"Runtime logs may contain file paths. Clipboard diagnostics contain bounded read metadata only, without payloads or content hashes. No settings file, credentials, clipboard content, search history or document text is included."_s;
    files.push_back({u"summary.json"_s, QJsonDocument(context).toJson(QJsonDocument::Indented)});

    mz_zip_archive zip {};
    if (!mz_zip_writer_init_heap(&zip, 0, 64 * 1024))
        return {{}, tr("生成诊断包失败")};
    bool ok = true;
    for (const Log::File& file : files) {
        const QByteArray name = file.name.toUtf8();
        if (!mz_zip_writer_add_mem(&zip, name.constData(), file.bytes.constData(),
                static_cast<size_t>(file.bytes.size()), MZ_BEST_SPEED)) {
            ok = false;
            break;
        }
    }
    void* buffer = nullptr;
    size_t bytes = 0;
    if (ok)
        ok = mz_zip_writer_finalize_heap_archive(&zip, &buffer, &bytes) != 0;
    mz_zip_writer_end(&zip);
    const QString path = log.archivePath();
    QSaveFile file(path); // a failed export leaves the last good archive intact
    if (ok)
        ok = file.open(QIODevice::WriteOnly)
            && file.write(static_cast<const char*>(buffer), static_cast<qint64>(bytes)) == static_cast<qint64>(bytes)
            && file.commit();
    mz_free(buffer);
    return ok ? Exported {path, {}} : Exported {{}, tr("写入诊断包失败，请检查文件夹权限和剩余空间")};
}
} // namespace ws::diagnostics
