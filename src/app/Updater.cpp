#include "Updater.h"

#include "Settings.h"
#include "platform/Http.h"
#include "platform/Shell.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QOperatingSystemVersion>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QThreadPool>
#include <QUuid>
#include <QVariantMap>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <thread>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws {

namespace {

// Checked at every start, then every 12 hours (looked at hourly, so a
// sleeping PC catches up when it wakes).
constexpr auto kInterval = std::chrono::hours(12);
// An automatic reminder for the same version, at most this often.
constexpr auto kRemindInterval = std::chrono::days(3);
// The packages are 40-60 MB; a hundred times more is no update of ours.
constexpr qint64 kMaxPackageBytes = 400LL << 20;
// Where the downloads page is, for a copy that cannot update itself.
constexpr auto kDownloadsPage = "https://lcstp.com/projects/winshun#downloads";
// The portable copy's new files and its old ones while they are swapped:
// inside the program folder, so on its volume (moves are renames) and
// known to be writable. WinShunUpdate.exe removes both when it is done.
constexpr auto kStageFolder = ".winshun-update-new";
constexpr auto kBackupFolder = ".winshun-update-old";

// Where the checks are remembered: next to the index, not in the settings
// file the user edits.
QSettings state()
{
    return QSettings(Settings::dataDir() + u"\\update.ini"_s, QSettings::IniFormat);
}

QString appDir()
{
    return QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
}

// The downloads: %LOCALAPPDATA%\WinShun\Updates, the user's own, also for a
// portable copy (not its "data", which the update leaves as it is); a
// --profile's own folder for tests.
QString updatesDir()
{
    const QString profile = Settings::profile();
    if (!profile.isEmpty() && profile != Settings::portableFolder())
        return profile + u"\\Updates"_s;
    return QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + u"/WinShun/Updates"_s);
}

// The update service: the latest version for this Windows and package.
// Tests point it at a fake one (WINSHUN_UPDATE_FEED, an http: or https:
// URL; a portable copy adds package=portable when it is not there).
QString feedUrl(bool portable)
{
    QString url = qEnvironmentVariable("WINSHUN_UPDATE_FEED");
    if (url.isEmpty()) {
        const QOperatingSystemVersion os = QOperatingSystemVersion::current();
        url = u"https://lcstp.com/updates/winshun/stable.json?platform=windows&arch=x64&osVersion=%1.%2.%3"_s
                  .arg(os.majorVersion())
                  .arg(os.minorVersion())
                  .arg(os.microVersion());
    }
    if (portable && !url.contains(u"package="_s))
        url += (url.contains(u'?') ? u"&"_s : u"?"_s) + u"package=portable"_s;
    return url;
}

// A fake service on this computer (tests) may send its packages over plain
// http from there too; the real one never does.
bool loopbackHttpAllowed()
{
    const QString custom = qEnvironmentVariable("WINSHUN_UPDATE_FEED");
    return !custom.isEmpty() && release::loopbackHttpUrl(custom);
}

// One argument on a command line: quoted, with the backslashes before the
// closing quote doubled ("C:\" would otherwise escape it).
QString quoted(const QString& path)
{
    QString text = QDir::toNativeSeparators(path);
    qsizetype trailing = 0;
    while (trailing < text.size() && text[text.size() - 1 - trailing] == u'\\')
        ++trailing;
    return u'"' + text + QString(trailing, u'\\') + u'"';
}

bool helperRunning()
{
    const HANDLE mutex = ::OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\WinShun.Updating");
    if (mutex)
        ::CloseHandle(mutex);
    return mutex || ::GetLastError() == ERROR_ACCESS_DENIED;
}

} // namespace

struct Updater::Job {
    release::Info release;
    release::Asset asset;
    update::Kind kind = update::Kind::None;
    QString file; // the package, in updatesDir()
    QString stage; // portable: the new files, unpacked
    QString backup;
    std::atomic<bool> cancel {false};
    std::atomic<qint64> received {0};
    // Held from its check until Setup has ended (or the zip is unpacked):
    // nobody can change it in between.
    update::HeldFile held;
};

Updater::Updater(QObject* parent)
    : QObject(parent)
{
    m_lastChecked = state().value(u"LastChecked"_s).toDateTime();
    m_kind = update::installKind(appDir(), !Settings::portableFolder().isEmpty());
    m_timer.setInterval(1h);
    connect(&m_timer, &QTimer::timeout, this, &Updater::checkIfDue);
    m_progressTimer.setInterval(200);
    connect(&m_progressTimer, &QTimer::timeout, this, [this] {
        if (m_job && m_job->received != m_received) {
            m_received = m_job->received;
            emit installChanged();
        }
    });
}

void Updater::afterStart()
{
    // After the tray icon is up, which shows the notice.
    QTimer::singleShot(3s, this, [this] {
        reportLastUpdate();
        cleanUp();
    });
}

Updater::~Updater()
{
    if (m_job)
        m_job->cancel = true;
}

QString Updater::currentVersion() const
{
    return QCoreApplication::applicationVersion();
}

bool Updater::skipped() const
{
    return m_release && state().value(u"SkippedVersion"_s).toString() == m_release->version;
}

QString Updater::summary() const
{
    return m_release ? release::summary(m_release->notes, m_chinese) : QString();
}

QVariantList Updater::highlights() const
{
    QVariantList list;
    if (!m_release)
        return list;
    for (const release::Highlight& h : release::highlights(m_release->notes, m_chinese))
        list.append(QVariantMap {{u"symbol"_s, h.symbol}, {u"text"_s, h.text}});
    return list;
}

QString Updater::installStage() const
{
    switch (m_stage) {
    case Stage::Idle:
        return {};
    case Stage::Downloading:
        return u"downloading"_s;
    case Stage::Verifying:
        return u"verifying"_s;
    case Stage::Installing:
        return u"installing"_s;
    }
    return {};
}

double Updater::installProgress() const
{
    return m_total > 0 ? std::clamp(static_cast<double>(m_received) / static_cast<double>(m_total), 0.0, 1.0) : 0.0;
}

void Updater::setAutomatic(bool on)
{
    if (!on) {
        m_timer.stop();
        return;
    }
    if (m_timer.isActive())
        return;
    m_timer.start();
    // Right after logging in the network may not be up yet.
    QTimer::singleShot(20s, this, &Updater::checkIfDue);
}

void Updater::setChinese(bool chinese)
{
    if (m_chinese == chinese)
        return;
    m_chinese = chinese;
    emit changed();
}

void Updater::checkIfDue()
{
    if (!m_timer.isActive())
        return; // turned off meanwhile
    const bool due = !m_checkedThisRun || !m_lastChecked.isValid()
        || m_lastChecked.secsTo(QDateTime::currentDateTime()) >= std::chrono::seconds(kInterval).count();
    if (due)
        check(false);
}

void Updater::check(bool manual)
{
    if (m_checking)
        return;
    m_checking = true;
    if (manual)
        m_problem.clear();
    emit changed();

    const QString url = feedUrl(m_kind == update::Kind::Portable);
    QThreadPool::globalInstance()->start([self = QPointer(this), url, manual] {
        const http::Response response = http::get(url, {}, 20000, 1 << 20);
        QMetaObject::invokeMethod(qApp, [self, response, manual] {
            if (self)
                self->finishCheck(response.status, response.body, response.error, manual);
        }, Qt::QueuedConnection);
    });
}

void Updater::finishCheck(int status, const QByteArray& body, unsigned long error, bool manual)
{
    m_checking = false;
    std::optional<release::Info> latest;
    const release::Answer answer = release::judge(status, body, currentVersion(), &latest);
    if (answer == release::Answer::Invalid) {
        QString problem;
        if (status == 403 || status == 429)
            problem = tr("更新服务器暂时不让查（访问太频繁），过一会儿再试。");
        else if (status != 0 && status != 200)
            problem = tr("更新服务器返回了错误（%1）。").arg(status);
        else if (status == 200)
            problem = tr("没看懂更新服务器的回答，过一会儿再试。");
        else if (error == ERROR_WINHTTP_TIMEOUT)
            problem = tr("连接更新服务器超时，请检查网络（或代理）后重试。");
        else
            problem = tr("连不上更新服务器，请检查网络（或代理）后重试。");
        qWarning().noquote() << "Update check failed:" << status << error;
        if (manual)
            m_problem = problem;
        emit changed();
        return;
    }

    m_checkedThisRun = true;
    m_lastChecked = QDateTime::currentDateTime();
    state().setValue(u"LastChecked"_s, m_lastChecked);
    m_problem.clear();
    if (m_stage != Stage::Idle) { // the version being installed stays the one shown
        emit changed();
        return;
    }
    if (answer == release::Answer::UpToDate) {
        m_release.reset();
        emit changed();
        return;
    }
    m_release = latest;
    emit changed();
    if (manual || shouldRemind(latest->version)) {
        QSettings s = state();
        s.setValue(u"PromptedVersion"_s, latest->version);
        s.setValue(u"PromptedAt"_s, QDateTime::currentDateTime());
        emit found(manual);
    }
}

// Skipped versions are not announced again; others at most every few days.
bool Updater::shouldRemind(const QString& version) const
{
    const QSettings s = state();
    if (s.value(u"SkippedVersion"_s).toString() == version)
        return false;
    const QDateTime promptedAt = s.value(u"PromptedAt"_s).toDateTime();
    return s.value(u"PromptedVersion"_s).toString() != version || !promptedAt.isValid()
        || promptedAt.secsTo(QDateTime::currentDateTime()) >= std::chrono::seconds(kRemindInterval).count();
}

void Updater::openDownloadPage()
{
    shell::openUrl(release::releasePage(m_release ? m_release->pageUrl : QString()));
}

void Updater::skip()
{
    if (!m_release)
        return;
    state().setValue(u"SkippedVersion"_s, m_release->version);
    emit changed();
}

void Updater::setStage(Stage stage)
{
    m_stage = stage;
    if (stage == Stage::Downloading)
        m_progressTimer.start();
    else
        m_progressTimer.stop();
    if (stage == Stage::Idle)
        m_job.reset();
    emit installChanged();
}

void Updater::fail(const QString& problem)
{
    m_installProblem = problem;
    setStage(Stage::Idle);
}

void Updater::setPending(const QString& version)
{
    QSettings s = state();
    if (version.isEmpty()) {
        s.remove(u"PendingVersion"_s);
        s.remove(u"PendingAt"_s);
    } else {
        s.setValue(u"PendingVersion"_s, version);
        s.setValue(u"PendingAt"_s, QDateTime::currentDateTime());
    }
    s.sync();
}

void Updater::install()
{
    if (!m_release || m_stage != Stage::Idle)
        return;
    m_installProblem.clear();
    // A copy that cannot put an update in place itself: the downloads page,
    // never administrator rights for a portable copy.
    if (m_kind == update::Kind::None) {
        m_installProblem = tr("这份 Win顺 既不是安装版，也不是便携版，没法在这里更新。已打开下载页。");
        emit installChanged();
        shell::openUrl(QString::fromLatin1(kDownloadsPage));
        return;
    }
    if (m_kind == update::Kind::Portable && !update::folderWritable(appDir())) {
        m_installProblem = tr("便携版所在的文件夹（%1）不能写入，没法在这里更新。已打开下载页：请把新的便携包解压到自己能写入的文件夹。")
                               .arg(appDir());
        emit installChanged();
        shell::openUrl(QString::fromLatin1(kDownloadsPage));
        return;
    }
    const release::Package package
        = m_kind == update::Kind::Portable ? release::Package::Portable : release::Package::Setup;
    const std::optional<release::Asset> asset = release::packageAsset(*m_release, package);
    if (!asset || asset->size > kMaxPackageBytes) {
        qWarning() << "Update: no usable asset for" << m_release->version;
        fail(tr("更新服务器给出的安装包信息不完整（没有校验值），为了安全没有下载。请稍后再试。"));
        return;
    }
    const QStringList urls = release::downloadUrls(*asset, loopbackHttpAllowed());
    if (urls.isEmpty()) {
        fail(tr("更新服务器没有给出可用的下载地址，请稍后再试。"));
        return;
    }
    if (!QDir().mkpath(updatesDir())) {
        fail(tr("没法创建下载文件夹 %1。").arg(updatesDir()));
        return;
    }

    auto job = std::make_shared<Job>();
    job->release = *m_release;
    job->asset = *asset;
    job->kind = m_kind;
    // Named here, not after the service's "name": nothing it sends becomes a path.
    static const QRegularExpression unsafe(u"[^0-9A-Za-z.\\-]"_s);
    const QString version = QString(job->release.version).remove(unsafe);
    job->file = updatesDir() + u"\\WinShun-%1-x64-%2"_s.arg(version,
                    package == release::Package::Setup ? u"setup.exe"_s : u"portable.zip"_s);
    job->stage = appDir() + u'\\' + QLatin1StringView(kStageFolder);
    job->backup = appDir() + u'\\' + QLatin1StringView(kBackupFolder);
    m_job = job;
    m_received = 0;
    m_total = asset->size;
    setStage(Stage::Downloading);

    QThreadPool::globalInstance()->start([self = QPointer(this), job, urls] {
        const update::DownloadResult result = update::download(job->asset, urls, job->file,
            [job](qint64 received, qint64) {
                job->received = received;
                return !job->cancel;
            });
        QMetaObject::invokeMethod(qApp, [self, job, result] {
            if (self)
                self->downloaded(job, result.ok, result.cancelled, result.failures.join(u'\n'));
        }, Qt::QueuedConnection);
    });
}

void Updater::cancelInstall()
{
    if (!m_job || m_stage != Stage::Downloading)
        return;
    m_job->cancel = true;
    m_installProblem.clear();
    setStage(Stage::Idle); // the worker ends by itself and cleans up after it
}

void Updater::downloaded(const std::shared_ptr<Job>& job, bool ok, bool cancelled, const QString& why)
{
    if (job != m_job || job->cancel || cancelled) {
        QFile::remove(job->file);
        return;
    }
    if (!ok) {
        qWarning().noquote() << u"Update download failed:\n"_s + why;
        fail(tr("下载没有成功：几个下载地址都连不上，或下载到的文件没有通过校验（已删除）。请检查网络后重试。"));
        return;
    }
    m_received = m_total;
    setStage(Stage::Verifying);
    QThreadPool::globalInstance()->start([self = QPointer(this), job] {
        // Checked again on the file as it is now, held so that it stays so.
        QString why;
        QByteArray bytes;
        const bool portable = job->kind == update::Kind::Portable;
        if (!job->held.open(job->file))
            why = u"cannot hold the file"_s;
        else if (job->held.sha256(portable ? &bytes : nullptr, kMaxPackageBytes) != job->asset.sha256.toLatin1())
            why = u"SHA-256 differs"_s;
        // Where a publisher check belongs once the packages are signed: the
        // certificate (its thumbprint) fixed in this program, and
        // WinVerifyTrust on the held file compared with it, never with
        // anything the service's answer names (whoever could change that
        // could name their own). They are not signed yet, so nothing is
        // checked here: what makes them trustworthy is https to lcstp.com,
        // the SHA-256 it gave and the version below.
        if (why.isEmpty() && !portable) {
            const QString version = update::fileVersion(job->file);
            if (!release::sameVersion(version, job->release.version))
                why = u"Setup is version %1, not %2"_s.arg(version, job->release.version);
        }
        if (why.isEmpty() && portable) {
            if (QFileInfo::exists(job->stage) && !helperRunning())
                QDir(job->stage).removeRecursively(); // left by an update that did not finish
            why = update::unpackPortable(bytes, job->stage);
            bytes.clear();
            job->held.close();
            QFile::remove(job->file); // unpacked: no longer needed
            if (why.isEmpty()) {
                const QString version = update::fileVersion(job->stage + u"\\WinShun.exe"_s);
                if (!release::sameVersion(version, job->release.version))
                    why = u"WinShun.exe is version %1, not %2"_s.arg(version, job->release.version);
            }
        }
        if (job->cancel && why.isEmpty())
            why = u"cancelled"_s;
        QMetaObject::invokeMethod(qApp, [self, job, why] {
            if (self)
                self->verified(job, why);
        }, Qt::QueuedConnection);
    });
}

void Updater::verified(const std::shared_ptr<Job>& job, const QString& why)
{
    const auto discard = [job] {
        job->held.close();
        QFile::remove(job->file);
        if (job->kind == update::Kind::Portable)
            QDir(job->stage).removeRecursively();
    };
    if (job != m_job || job->cancel) {
        discard();
        return;
    }
    if (!why.isEmpty()) {
        qWarning().noquote() << "Update check of the package failed:" << why;
        discard();
        fail(tr("下载的安装包没有通过检查（已删除），没有安装。请重试。"));
        return;
    }
    setStage(Stage::Installing);
    if (job->kind == update::Kind::Portable)
        runPortableSwap(job);
    else
        runSetup(job);
}

void Updater::runSetup(const std::shared_ptr<Job>& job)
{
    // The way this copy was installed. Not elevated here, also for all
    // users: Setup asks for administrator rights itself, and keeps the
    // user's own for its [Run] entries (runasoriginaluser: Win顺, its
    // autostart task). Started elevated, it would have no other rights to
    // give them, and Win顺 would come back as administrator (GitHub #21).
    // It closes Win顺 itself (--quit: the usual way out, which saves the
    // index, settings and clipboard), installs, re-registers the index
    // service and starts the new version in the tray ([Run] --background,
    // which a silent Setup runs too).
    const QString log = updatesDir() + u"\\setup.log"_s;
    const QString args = u"/SILENT /SUPPRESSMSGBOXES /NORESTART /SP- %1 /LOG=%2"_s.arg(
        job->kind == update::Kind::AllUsers ? u"/ALLUSERS"_s : u"/CURRENTUSER"_s, quoted(log));
    setPending(job->release.version);
    auto setup = std::make_shared<win32::UniqueHandle>(shell::startUnelevated(QDir::toNativeSeparators(job->file).toStdWString(), args.toStdWString()));
    if (!setup->valid()) {
        qWarning() << "Update: Setup did not start" << ::GetLastError();
        setPending({});
        job->held.close();
        QFile::remove(job->file);
        fail(tr("安装程序没能启动，旧版本照常可用。请重试。"));
        return;
    }
    // A Setup that fails after it has closed Win顺 starts nothing: the
    // helper, outside the program folder, starts the installed copy then.
    const QString helper = updatesDir() + u"\\WinShunUpdate.exe"_s;
    QFile::remove(helper);
    if (QFile::copy(appDir() + u"\\WinShunUpdate.exe"_s, helper)) {
        const QString helperArgs = u"--watch-setup --pid %1 --start %2 --log %3"_s.arg(::GetProcessId(setup->get()))
                                       .arg(quoted(appDir() + u"\\WinShun.exe"_s), quoted(updatesDir() + u"\\update.log"_s));
        if (!shell::startUnelevated(helper.toStdWString(), helperArgs.toStdWString()).valid())
            qWarning() << "Update: the helper did not start" << ::GetLastError();
    }
    // Setup ending while Win顺 still runs did not get far: the prompt
    // declined, refused (InitializeSetup), or failed before closing it.
    // A thread of its own, not the pool's: Setup normally ends this process
    // while it waits, and the pool would hold up the exit until Setup ends.
    std::thread([self = QPointer(this), setup] {
        ::WaitForSingleObject(setup->get(), INFINITE);
        DWORD code = 0;
        ::GetExitCodeProcess(setup->get(), &code);
        if (QCoreApplication* app = QCoreApplication::instance())
            QMetaObject::invokeMethod(app, [self, code] {
                if (self)
                    self->setupEnded(code);
            }, Qt::QueuedConnection);
    }).detach();
}

void Updater::setupEnded(unsigned long exitCode)
{
    if (!m_job)
        return;
    qWarning() << "Update: Setup ended with" << exitCode << "while this version still runs";
    setPending({});
    m_job->held.close();
    QFile::remove(m_job->file);
    if (exitCode == 0) {
        setStage(Stage::Idle);
        return;
    }
    // Inno Setup: 1 it could not start, 2 cancelled before installing (also
    // the administrator prompt declined), 5 PrepareToInstall stopped.
    fail(tr("没有安装（安装程序退出代码 %1，可能是在系统提示里选了“否”）。旧版本照常可用，可以重试。").arg(exitCode));
}

void Updater::runPortableSwap(const std::shared_ptr<Job>& job)
{
    const auto give_up = [this, job](const QString& problem) {
        setPending({});
        QDir(job->stage).removeRecursively();
        fail(problem);
    };
    // A copy of the one helper file, run from outside the folder it replaces.
    const QString helper = updatesDir() + u"\\WinShunUpdate.exe"_s;
    QFile::remove(helper);
    if (!QFile::copy(appDir() + u"\\WinShunUpdate.exe"_s, helper)) {
        give_up(tr("便携版里缺少 WinShunUpdate.exe，没法在这里更新。请从下载页下载新的便携包。"));
        return;
    }
    const QString ready = u"Local\\WinShun.UpdateReady.%1.%2"_s.arg(::GetCurrentProcessId())
                              .arg(QUuid::createUuid().toString(QUuid::Id128));
    const HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, reinterpret_cast<const wchar_t*>(ready.utf16()));
    if (!event) {
        give_up(tr("更新助手没能启动，旧版本照常可用。请重试。"));
        return;
    }
    setPending(job->release.version);
    const QString args = u"--portable --pid %1 --ready %2 --app %3 --stage %4 --backup %5 --log %6"_s
                             .arg(::GetCurrentProcessId())
                             .arg(ready, quoted(appDir()), quoted(job->stage), quoted(job->backup),
                                 quoted(updatesDir() + u"\\update.log"_s));
    auto process = std::make_shared<win32::UniqueHandle>(shell::startUnelevated(helper.toStdWString(), args.toStdWString()));
    if (!process->valid()) {
        ::CloseHandle(event);
        give_up(tr("更新助手没能启动，旧版本照常可用。请重试。"));
        return;
    }
    // It holds this process first; then Win顺 quits the usual way (the
    // index, settings and clipboard saved) and it does the rest.
    QThreadPool::globalInstance()->start([self = QPointer(this), event, process, give_up] {
        const HANDLE both[] = {event, process->get()};
        const DWORD woke = ::WaitForMultipleObjects(2, both, FALSE, 15000);
        const bool held = woke == WAIT_OBJECT_0;
        if (!held)
            ::TerminateProcess(process->get(), 1);
        ::CloseHandle(event);
        QMetaObject::invokeMethod(qApp, [self, held, give_up] {
            if (!self)
                return;
            if (held)
                QCoreApplication::exit(0); // as App::handleCommand("quit")
            else
                give_up(tr("更新助手没有响应，旧版本照常可用。请重试。"));
        }, Qt::QueuedConnection);
    });
}

// How the update asked for last time went: this version is the one it
// asked for (or newer), or it is not, and the old one was kept or put back.
void Updater::reportLastUpdate()
{
    QSettings s = state();
    const QString pending = s.value(u"PendingVersion"_s).toString();
    if (pending.isEmpty() || m_stage != Stage::Idle)
        return;
    setPending({});
    const QString current = currentVersion();
    if (release::sameVersion(current, pending) || release::isNewer(current, pending))
        emit notice(tr("Win顺 已更新到 %1").arg(current), tr("设置、索引和剪贴板记录都还在。"));
    else
        emit notice(tr("Win顺 没有更新到 %1").arg(pending), tr("现在仍是 %1，照常可用。可以稍后再试一次。").arg(current));
}

// What earlier updates left: downloads (their Setup may still be ending
// when the new version starts: tried again later), a portable copy's stage
// and backup. Not while the helper is at work. The logs stay.
void Updater::cleanUp()
{
    if (m_stage != Stage::Idle)
        return;
    if (helperRunning()) {
        QTimer::singleShot(1min, this, &Updater::cleanUp);
        return;
    }
    bool all = true;
    QDir updates(updatesDir());
    for (const QFileInfo& entry : updates.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
        if (entry.suffix().compare(u"log"_s, Qt::CaseInsensitive) == 0) {
            if (entry.size() > (1 << 20))
                QFile::remove(entry.absoluteFilePath());
            continue;
        }
        all = QFile::remove(entry.absoluteFilePath()) && all;
    }
    if (m_kind == update::Kind::Portable) {
        for (const auto folder : {kStageFolder, kBackupFolder}) {
            QDir dir(appDir() + u'\\' + QLatin1StringView(folder));
            if (dir.exists())
                all = dir.removeRecursively() && all;
        }
    }
    static int retries = 0;
    if (!all && retries++ < 3)
        QTimer::singleShot(2min, this, &Updater::cleanUp);
}

} // namespace ws
