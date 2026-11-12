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
// plugins.log holds what plugins say about themselves (a plugin may write a
// path or a typed word into its log lines): only its newest part, enough for
// the last starts, crashes and stderr tails.
constexpr qint64 kMaxPluginLogBytes = 256 << 10;
QString tr(const char* text)
{
    return QCoreApplication::translate("ws::diagnostics", text);
}
} // namespace

QJsonObject environment()
{
    MEMORYSTATUSEX memory {sizeof memory};
    ::GlobalMemoryStatusEx(&memory);
    // "administrator" is the account's; "rights" what this copy runs with
    // (GitHub #203: not elevated, or Explorer's keys would not reach it).
    return {{u"version"_s, QStringLiteral(WINSHUN_VERSION)}, {u"os"_s, QSysInfo::prettyProductName()},
        {u"kernel"_s, QSysInfo::kernelVersion()}, {u"architecture"_s, QSysInfo::currentCpuArchitecture()},
        {u"logical_processors"_s, static_cast<int>(std::thread::hardware_concurrency())},
        {u"memory_mb"_s, static_cast<qint64>(memory.ullTotalPhys >> 20)}, {u"administrator"_s, userIsAdministrator()},
        {u"rights"_s, rightsOf(0)}};
}

QJsonObject rightsOf(unsigned long pid)
{
    QJsonObject rights {{u"integrity"_s, u"unknown"_s}};
    const HANDLE process = pid ? ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : ::GetCurrentProcess();
    if (!process)
        return rights;
    HANDLE token = nullptr;
    const bool opened = ::OpenProcessToken(process, TOKEN_QUERY, &token);
    if (pid)
        ::CloseHandle(process);
    if (!opened)
        return rights;
    alignas(TOKEN_MANDATORY_LABEL) BYTE label[64];
    DWORD size = 0;
    if (::GetTokenInformation(token, TokenIntegrityLevel, label, sizeof label, &size)) {
        const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label)->Label.Sid;
        if (const UCHAR count = *::GetSidSubAuthorityCount(sid)) {
            const DWORD level = *::GetSidSubAuthority(sid, count - 1);
            rights[u"integrity"_s] = level >= SECURITY_MANDATORY_SYSTEM_RID ? u"system"_s
                : level >= SECURITY_MANDATORY_HIGH_RID                      ? u"high"_s
                : level >= SECURITY_MANDATORY_MEDIUM_PLUS_RID               ? u"medium-plus"_s
                : level >= SECURITY_MANDATORY_MEDIUM_RID                    ? u"medium"_s
                : level >= SECURITY_MANDATORY_LOW_RID                       ? u"low"_s
                                                                            : u"untrusted"_s;
        }
    }
    TOKEN_ELEVATION elevation {};
    if (::GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size))
        rights[u"elevated"_s] = elevation.TokenIsElevated != 0;
    DWORD uiAccess = 0;
    if (::GetTokenInformation(token, TokenUIAccess, &uiAccess, sizeof uiAccess, &size))
        rights[u"ui_access"_s] = uiAccess != 0;
    ::CloseHandle(token);
    return rights;
}

Exported exportArchive(Log& log, const QString& dataFolder, const QString& serviceFolder, QJsonObject context)
{
    std::vector<Log::File> files = log.snapshot();
    QJsonArray unavailable;
    // Deliberately whitelist runtime logs. Settings, clipboard, search
    // history, MCP/plugin conversations and document data are not packed
    // (plugins.log is the host's log about plugins, not their conversations).
    const auto add = [&](const QString& folder, const QString& name, const QString& packedName, qint64 limit = kMaxSourceBytes) {
        if (folder.isEmpty())
            return;
        QFile file(QDir(folder).filePath(name));
        if (!file.exists())
            return;
        if (!file.open(QIODevice::ReadOnly)) {
            unavailable.append(packedName);
            return;
        }
        if (file.size() > limit)
            file.seek(file.size() - limit); // recent errors, bounded even for an old giant log
        files.push_back({packedName, file.read(limit)});
    };
    add(dataFolder, u"WinShun.log"_s, u"app/WinShun.log"_s);
    add(dataFolder, u"WinShun.old.log"_s, u"app/WinShun.old.log"_s);
    add(dataFolder, u"plugins.log"_s, u"app/plugins.log"_s, kMaxPluginLogBytes);
    // The screenshot plugin's own journal (its process's: capture, capture-stall,
    // idle-trim, long-capture): times, sizes and pointer facts, nothing captured.
    const QString captureJournal = QDir(dataFolder).filePath(u"plugins/winshun.capture.data/diagnostics"_s);
    add(captureJournal, u"events.jsonl"_s, u"capture/events.jsonl"_s);
    add(captureJournal, u"events.1.jsonl"_s, u"capture/events.1.jsonl"_s);
    add(serviceFolder, u"service.log"_s, u"service/service.log"_s);
    add(serviceFolder, u"service.old.log"_s, u"service/service.old.log"_s);
    context[u"schema_version"_s] = 1;
    context[u"generated_at"_s] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    context[u"environment"_s] = environment();
    context[u"unavailable_logs"_s] = unavailable;
    context[u"privacy"_s]
        = u"Runtime logs (the plugin log is cut to its newest 256 KB) may contain file paths. Clipboard diagnostics contain bounded read metadata only, without payloads or content hashes. No settings file, credentials, clipboard content, search history or document text is included."_s;
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
