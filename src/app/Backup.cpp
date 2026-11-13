#include "Backup.h"

#include "BackupFiles.h"
#include "Settings.h"
#include "platform/Shell.h"

#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QFileInfo>
#include <QPointer>
#include <QThreadPool>
#include <QWindow>

#include <chrono>
#include <memory>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws {

namespace {

constexpr auto kFirst = 3min; // after start: login settles first
constexpr auto kAfterEdit = 1min; // several changes in a row make one backup
constexpr auto kEvery = 30min;
QString restoreProblem;

QString stagedFolder()
{
    return QDir(Settings::dataDir()).filePath(u"restore"_s);
}

// `path` is a file inside `folder` (a plugin's data folder).
bool insideFolder(const QString& path, const QString& folder)
{
    if (path.isEmpty() || folder.isEmpty() || !QFileInfo(path).isAbsolute())
        return false;
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    const QString base = QDir::cleanPath(QDir::fromNativeSeparators(folder)) + u'/';
    return clean.startsWith(base, Qt::CaseInsensitive) && !QDir::fromNativeSeparators(path).contains(u"/../"_s);
}

} // namespace

Backup::Backup(QObject* parent)
    : QObject(parent)
    , m_problem(restoreProblem)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        m_timer.start(kEvery);
        if (!m_folder.isEmpty())
            run();
    });
}

void Backup::setFolder(const QString& folder)
{
    if (folder == m_folder)
        return;
    const bool first = m_folder.isEmpty();
    m_folder = folder;
    readLast();
    if (m_folder.isEmpty())
        m_timer.stop();
    else if (first)
        m_timer.start(kFirst);
    emit changed();
}

void Backup::soon()
{
    if (!m_folder.isEmpty())
        m_timer.start(kAfterEdit);
}

void Backup::backUpNow()
{
    run();
}

void Backup::exportBackup(QWindow* window)
{
    if (m_busy)
        return;
    const HWND owner = window ? reinterpret_cast<HWND>(window->winId()) : nullptr;
    const QString path = shell::pickSaveFile(
        owner, tr("导出设置备份"), backupfiles::fileName(QDateTime::currentDateTime()), tr("Win顺 备份"), u"zip"_s);
    if (!path.isEmpty())
        run({}, path);
}

void Backup::openFolder()
{
    if (!m_folder.isEmpty())
        shell::open(m_folder);
}

void Backup::restore(QWindow* window)
{
    if (m_busy)
        return;
    const HWND owner = window ? reinterpret_cast<HWND>(window->winId()) : nullptr;
    const QString file
        = shell::pickFile(owner, tr("选择要恢复的备份"), {{tr("Win顺 备份"), u"*.zip"_s}, {tr("所有文件"), u"*.*"_s}});
    if (!file.isEmpty())
        run(file);
}

void Backup::exportFor(const QString& file, const QString& unlessHash, std::function<void(Exported)> done)
{
    const QString settingsFile = Settings::filePath();
    const QString dataFolder = Settings::dataDir();
    const QString version = QCoreApplication::applicationVersion();
    QThreadPool::globalInstance()->start([file, unlessHash, settingsFile, dataFolder, version, done = std::move(done)]() mutable {
        Exported result;
        result.when = QDateTime::currentDateTime();
        const backupfiles::Packed packed
            = backupfiles::pack(backupfiles::collect(settingsFile, dataFolder), version, result.when, true);
        result.hash = packed.hash;
        if (packed.zip.isEmpty())
            result.problem = QCoreApplication::translate("ws::Backup", "有文件读不出来");
        else if (packed.hash != unlessHash) {
            result.problem = backupfiles::exportTo(file, packed);
            result.written = result.problem.isEmpty();
        }
        QMetaObject::invokeMethod(qApp, [result, done = std::move(done)] { done(result); });
    });
}

bool Backup::restoreFrom(const QString& file, std::function<void(QString)> done)
{
    if (m_busy)
        return false;
    run(file, {}, std::move(done));
    return true;
}

void Backup::exportTool(const QString& dataDir, const QJsonObject& arguments, ToolDone done)
{
    const QString file = arguments.value(u"file"_s).toString();
    if (!insideFolder(file, dataDir)) {
        done({}, u"file must be a file in the plugin's data folder"_s);
        return;
    }
    exportFor(file, arguments.value(u"unless"_s).toString(), [done = std::move(done)](const Exported& e) {
        if (!e.problem.isEmpty())
            done({}, e.problem);
        else
            done({{u"hash"_s, e.hash}, {u"when"_s, e.when.toString(Qt::ISODate)}, {u"written"_s, e.written}}, {});
    });
}

void Backup::restoreTool(const QString& dataDir, const QJsonObject& arguments, ToolDone done)
{
    const QString file = arguments.value(u"file"_s).toString();
    if (!insideFolder(file, dataDir)) {
        done({}, u"file must be a file in the plugin's data folder"_s);
        return;
    }
    // Answered once: by the restore, or here when another one is under way.
    const auto answer = std::make_shared<ToolDone>(std::move(done));
    if (!restoreFrom(file, [answer](const QString& problem) {
            (*answer)(problem.isEmpty() ? QJsonObject {{u"restarting"_s, true}} : QJsonObject(), problem);
        }))
        (*answer)({}, tr("另一个备份正在进行，稍后再试"));
}

void Backup::run(const QString& restoring, const QString& exporting, std::function<void(QString)> done)
{
    if (m_busy)
        return;
    m_busy = true;
    m_problem.clear();
    m_message.clear();
    emit changed();
    // A restore's safety copy is local: an offline sync drive must not
    // prevent migration, and pruning must never delete the selected input.
    const QString folder = m_folder.isEmpty() || !restoring.isEmpty() ? QDir(Settings::dataDir()).filePath(u"backups"_s)
                                                                      : QDir::fromNativeSeparators(m_folder);
    const QString settingsFile = Settings::filePath();
    const QString dataFolder = Settings::dataDir();
    const QString version = QCoreApplication::applicationVersion();
    const QString staged = stagedFolder();
    QThreadPool::globalInstance()->start([self = QPointer(this), folder, settingsFile, dataFolder, version, restoring,
                                             exporting, staged, done = std::move(done)]() mutable {
        const QString part = staged + u".part"_s;
        QString problem;
        QString message;
        // Read and validate the input before writing/pruning any backups.
        if (!restoring.isEmpty())
            problem = backupfiles::unpack(restoring, part);
        // Nothing written when nothing changed since the newest backup.
        const QDateTime now = QDateTime::currentDateTime();
        if (problem.isEmpty()) {
            const backupfiles::Packed packed
                = backupfiles::pack(backupfiles::collect(settingsFile, dataFolder), version, now, !exporting.isEmpty());
            if (packed.zip.isEmpty())
                problem = QCoreApplication::translate("ws::Backup", "有文件读不出来");
            else if (!exporting.isEmpty()) {
                problem = backupfiles::exportTo(exporting, packed);
                message = QCoreApplication::translate("ws::Backup", "备份已导出：%1")
                              .arg(QDir::toNativeSeparators(exporting));
            } else {
                const QList<backupfiles::Found> existing = backupfiles::list(folder);
                if (existing.isEmpty() || backupfiles::hashOf(existing.first().path) != packed.hash)
                    problem = backupfiles::write(folder, packed, now);
                message = QCoreApplication::translate("ws::Backup", "备份已保存到：%1")
                              .arg(QDir::toNativeSeparators(folder));
            }
        }
        // A backup to restore: unpacked beside the data, in place once
        // complete, applied by the next start.
        bool restart = false;
        if (!restoring.isEmpty() && problem.isEmpty()) {
            if ((QFileInfo::exists(staged) && !QDir(staged).removeRecursively()) || !QDir().rename(part, staged))
                problem
                    = QCoreApplication::translate("ws::Backup", "写不进 %1").arg(QDir::toNativeSeparators(dataFolder));
            restart = problem.isEmpty();
        }
        if (!restoring.isEmpty() && !problem.isEmpty())
            QDir(part).removeRecursively();
        const QList<backupfiles::Found> found
            = exporting.isEmpty() ? backupfiles::list(folder) : QList<backupfiles::Found>();
        const QDateTime last = found.isEmpty() ? QDateTime() : found.first().when;
        QMetaObject::invokeMethod(
            qApp, [self, folder, problem, message, restart, last, exported = !exporting.isEmpty(), done = std::move(done)] {
                if (done)
                    done(problem);
                if (!self)
                    return;
                self->m_busy = false;
                self->m_problem = problem;
                self->m_message = problem.isEmpty() ? message : QString();
                if (!exported && QDir::fromNativeSeparators(self->m_folder) == folder)
                    self->m_last = last;
                emit self->changed();
                if (restart)
                    emit self->restartRequested();
            });
    });
}

// Off the GUI thread: the folder may be on a drive that answers slowly.
void Backup::readLast()
{
    m_last = {};
    if (m_folder.isEmpty())
        return;
    QThreadPool::globalInstance()->start([self = QPointer(this), folder = m_folder] {
        const QList<backupfiles::Found> found = backupfiles::list(folder);
        const QDateTime last = found.isEmpty() ? QDateTime() : found.first().when;
        QMetaObject::invokeMethod(qApp, [self, folder, last] {
            if (!self || self->m_folder != folder || self->m_last == last)
                return;
            self->m_last = last;
            emit self->changed();
        });
    });
}

void Backup::applyRestored()
{
    const QString staged = stagedFolder();
    QDir(staged + u".part"_s).removeRecursively(); // one cut short
    if (QFileInfo(staged).isDir()) {
        restoreProblem = backupfiles::apply(staged, Settings::filePath(), Settings::dataDir());
        if (!restoreProblem.isEmpty())
            qWarning().noquote() << restoreProblem;
    }
}

} // namespace ws
