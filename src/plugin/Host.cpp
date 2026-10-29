#include "Host.h"

#include "OpenPolicy.h"
#include "Programs.h"
#include "Session.h"
#include "ToastPolicy.h"
#include "ZeroCode.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::plugin {

namespace {

constexpr qint64 kCrashWindowMs = 5 * 60 * 1000; // three crashes in it: off
constexpr int kCrashesToDisable = 3;
constexpr int kTimeoutsToRestart = 3; // a resident plugin that misses three in a row is hung
constexpr int kShutdownGraceMs = 2000;
constexpr qint64 kLogRotateBytes = 1 << 20;

const char* kindName(Match::Kind kind)
{
    switch (kind) {
    case Match::Kind::Keyword: return "keyword";
    case Match::Kind::Regex: return "regex";
    case Match::Kind::Push: return "push";
    }
    return "keyword";
}

// `text` cut to `most` characters with "…", never inside a surrogate pair.
QString capped(QString text, qsizetype most)
{
    text = text.trimmed();
    if (text.size() <= most)
        return text;
    qsizetype cut = most - 1;
    if (cut > 0 && text.at(cut - 1).isHighSurrogate())
        --cut;
    return text.left(cut) + u'…';
}

// Which permission a tool of Win顺's needs (the plugin design, §6.3).
std::optional<Permission> permissionFor(const QString& tool)
{
    if (tool == u"search_files" || tool == u"search_content" || tool == u"recent_files" || tool == u"list_folder"
        || tool == u"get_file_info")
        return Permission::WinshunSearch;
    if (tool == u"open_item" || tool == u"show_in_folder")
        return Permission::WinshunOpen;
    if (tool == u"clipboard_history" || tool == u"clipboard_get")
        return Permission::ClipboardRead;
    if (tool == u"read_file_text" || tool == u"list_documents")
        return Permission::WinshunContent;
    if (tool == u"index_status")
        return std::nullopt;
    return Permission::WinshunHistory; // unknown names: the strictest, and the bridge says "unknown" anyway
}

// Why a plugin that is not enabled() does nothing: off, or a newer version
// waiting for the user's OK on more permissions (Catalog::setConfirmed).
QString offText(const Catalog& catalog, const QString& id)
{
    return catalog.awaitingConsent(id).isEmpty() ? u"插件已停用"_s : u"插件的新版本要更多权限，要先在设置里同意"_s;
}

} // namespace

bool isLabelledLow(const QString& path)
{
    std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL sacl = nullptr;
    if (::GetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr,
                                &sacl, &descriptor)
        != ERROR_SUCCESS)
        return false;
    bool low = false;
    for (DWORD i = 0; sacl && i < sacl->AceCount && !low; ++i) {
        void* ace = nullptr;
        if (!::GetAce(sacl, i, &ace) || static_cast<ACE_HEADER*>(ace)->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE)
            continue;
        const auto* label = static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(ace);
        const PSID sid = const_cast<DWORD*>(&label->SidStart);
        const DWORD rid = *::GetSidSubAuthority(sid, *::GetSidSubAuthorityCount(sid) - 1);
        constexpr BYTE kInherits = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
        low = rid <= SECURITY_MANDATORY_LOW_RID && (label->Header.AceFlags & kInherits) == kInherits;
    }
    ::LocalFree(descriptor);
    return low;
}

namespace {

// Full control on `path` (and what is in it) for the user this runs as. A
// folder made by an unelevated process on a drive with Windows' default
// root ACL (Authenticated Users: Modify, as on a second disk) is the user's
// own, but the user lacks WRITE_OWNER on it, which a label needs; as its
// owner the user may still change its DACL (implicit WRITE_DAC).
bool grantSelfFullControl(const std::wstring& path)
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    const bool haveUser = size && ::GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    ::CloseHandle(token);
    if (!haveUser)
        return false;
    const PSID user = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;

    std::wstring native = path;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    if (::GetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr,
                                &descriptor)
        != ERROR_SUCCESS)
        return false;
    EXPLICIT_ACCESS_W access {};
    access.grfAccessPermissions = FILE_ALL_ACCESS;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(user);
    PACL updated = nullptr;
    bool ok = ::SetEntriesInAclW(1, &access, dacl, &updated) == ERROR_SUCCESS;
    if (ok)
        ok = ::SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr)
            == ERROR_SUCCESS;
    ::LocalFree(updated);
    ::LocalFree(descriptor);
    return ok;
}

} // namespace

bool labelLow(const QString& path)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;OICI;NW;;;LW)", SDDL_REVISION_1, &descriptor, nullptr))
        return false;
    PACL sacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    bool ok = ::GetSecurityDescriptorSacl(descriptor, &present, &sacl, &defaulted) && present;
    if (ok) {
        std::wstring native = QDir::toNativeSeparators(path).toStdWString();
        const auto label = [&] {
            return ::SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr,
                                           sacl);
        };
        DWORD result = label();
        if (result == ERROR_ACCESS_DENIED && grantSelfFullControl(native))
            result = label();
        ok = result == ERROR_SUCCESS;
    }
    ::LocalFree(descriptor);
    return ok;
}

ToolBridge toolBridge(mcp::ToolHost* host)
{
    ToolBridge bridge;
    if (!host)
        return bridge;
    bridge.call = [host](std::uint64_t callId, const QString& client, const QString& tool, const QJsonObject& arguments,
                      std::function<void(mcp::ToolResult)> done) { host->call(callId, client, tool, arguments, std::move(done)); };
    bridge.cancel = [host](std::uint64_t callId) { host->cancel(callId); };
    bridge.known = [host](const QString& tool) { return host->known(tool); };
    return bridge;
}

// A query a plugin is to answer: what Catalog::match found, kept by value
// (the catalog may rescan meanwhile).
struct Host::Pending {
    std::uint64_t queryId = 0;
    QString commandId;
    Match::Kind kind = Match::Kind::Keyword;
    QString keyword;
    QString text;
    QString raw;
    QJsonObject filters;
    QStringList captures;
    int limit = kMaxItems;
    // Actions (once plugins start a process for them too).
    std::uint64_t requestId = 0;
    QString actionId;
    QJsonObject item;
    QStringList files; // a `files` trigger's action: the paths chosen
    std::uint64_t commandCall = 0; // a callCommand() query: its answer goes to the call, not the signals
};

struct Host::OnceRun {
    RunKind kind = RunKind::Query;
    std::uint64_t queryId = 0; // Query
    std::uint64_t requestId = 0; // Action
    std::uint64_t commandCall = 0; // Query for callCommand(): the call's id
    QString commandId;
    QByteArray output; // its stdout so far
    QTimer timer; // the query's timeout
    bool discarded = false; // superseded: whatever it says is dropped
    quint32 pid = 0;
    Pending query; // Query: what it was started for (a `rerun` starts it again)
};

// A command asked from outside the launcher (callCommand): one query with
// a timer of its own, waiting for the plugin to start if it must.
struct Host::CommandCall {
    std::uint64_t id = 0;
    QString pluginId;
    QString commandId;
    QString text;
    int limit = kMaxItems;
    CommandReply reply;
    QTimer timer;
    std::uint64_t rpcId = 0; // resident: the query request, once sent
    bool waitingForStart = false; // resident: sent when initialize is answered
};

struct Host::Plugin {
    QString id;
    enum class Phase { Stopped, Starting, Ready, ShuttingDown, Backoff };
    Phase phase = Phase::Stopped;
    std::uint64_t instance = 0; // the resident process
    quint32 pid = 0;
    std::unique_ptr<Session> session;
    bool restartAfterStop = false; // a shutdown we asked for, to start again (hung)
    QString error;
    // The resident query in flight, and the one waiting for the process.
    struct Inflight {
        std::uint64_t queryId = 0;
        QString commandId;
        std::uint64_t requestId = 0;
    };
    std::optional<Inflight> inflight;
    QTimer queryTimer;
    std::optional<Pending> pending;
    int timeouts = 0;
    std::map<std::uint64_t, std::uint64_t> actions; // our request id -> JSON-RPC id (resident)
    std::map<std::uint64_t, std::uint64_t> toolCalls; // the bridge's call id -> nothing (to cancel)
    // Once plugins.
    QTimer debounce;
    std::optional<Pending> waiting;
    std::map<std::uint64_t, std::unique_ptr<OnceRun>> runs; // instance -> run
    // Crashes and rest.
    QList<qint64> crashTimes;
    int backoffStep = 0;
    QTimer backoff;
    bool disabledByCrashes = false;
    QTimer idle;
    qint64 lastUsed = 0;
    // Developer mode's trace: when each request of ours went out, by its
    // JSON-RPC id, for the time to its response.
    std::map<std::uint64_t, qint64> sentAt;
    bool restartWhenGone = false; // reload(): start again once the process has left
    // A final answer's `rerun`: the query, sent again when `rerun` fires
    // while it is still the launcher's (cancel, cancelAll and the process
    // leaving forget it).
    std::optional<Pending> lastQuery;
    QTimer rerun;
    std::uint64_t rerunQuery = 0; // the query whose answer asked for `rerun`, every rerunMs
    int rerunMs = 0;
    // winshun/refresh while that query was in flight: its answer may predate
    // the change, so it is asked again right after that answer (onRefresh).
    bool refreshAfterReply = false;
    QElapsedTimer lastQuerySent; // for kRefreshGapMs
    // Toasts: one shown every kToastGapMs at most; the newest of those that
    // came meanwhile waits for `toastGap`.
    struct Toast {
        QString title;
        QString text;
        QJsonArray click;
    };
    std::optional<Toast> toastWaiting;
    QTimer toastGap;
    QElapsedTimer lastToast; // monotonic: a clock set back must not hold toasts back
    bool toastRefusedLogged = false;
};

Host::Host(Options options, QObject* parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_catalog(m_options.userPluginDir, m_options.bundledPluginDir)
{
    qRegisterMetaType<Items>();
    qRegisterMetaType<Effects>();
    if (m_options.dataRoot.isEmpty())
        m_options.dataRoot = m_options.userPluginDir;
    m_runnerBackoff.setSingleShot(true);
    connect(&m_runnerBackoff, &QTimer::timeout, this, [this] {
        // Start it again only for someone who needs it: the next query does.
        for (auto& [id, p] : m_plugins) {
            if (p->pending || p->waiting)
                ensureRunner();
        }
    });
    connect(&m_catalog, &Catalog::changed, this, [this] {
        // A plugin that changed or went: its process is stale.
        for (auto& [id, p] : m_plugins) {
            const Entry* e = m_catalog.find(id);
            if (!e || !e->manifest || !m_catalog.enabled(id)) {
                if (p->phase != Plugin::Phase::Stopped && p->phase != Plugin::Phase::Backoff)
                    stopResident(*p, false);
            }
            emit stateChanged(id);
        }
    });
    connect(&m_catalog, &Catalog::scriptsChanged, this, [this](const QStringList& ids) {
        if (!m_developerMode)
            return;
        for (const QString& id : ids) {
            const Entry* e = entry(id);
            if (!e || !m_catalog.enabled(id) || !e->manifest->resident())
                continue;
            Plugin& p = plugin(id);
            const bool running = p.phase == Plugin::Phase::Starting || p.phase == Plugin::Phase::Ready;
            log(u"%1: 脚本改了，重新加载"_s.arg(id));
            if (running) {
                p.restartWhenGone = true;
                stopResident(p, false);
            }
            emit reloaded(id);
        }
    });
    m_catalog.scan();
}

void Host::setDeveloperMode(bool on)
{
    if (m_developerMode == on)
        return;
    m_developerMode = on;
    log(on ? u"开发者模式：开"_s : u"开发者模式：关"_s);
}

void Host::stop(const QString& id)
{
    const auto it = m_plugins.find(id);
    if (it == m_plugins.end())
        return;
    Plugin& p = *it->second;
    p.restartWhenGone = false;
    p.pending.reset();
    p.waiting.reset();
    p.debounce.stop();
    p.backoff.stop();
    if (p.phase == Plugin::Phase::Backoff) {
        p.phase = Plugin::Phase::Stopped;
        emit stateChanged(id);
    }
    for (auto& [instance, run] : p.runs) {
        if (!run->discarded) {
            run->discarded = true;
            run->timer.stop();
            if (run->commandCall)
                finishCommand(run->commandCall, false, {}, u"插件已停止"_s);
            else if (run->kind == RunKind::Query)
                emit queryFailed(run->queryId, id, u"插件已停止"_s);
            else
                emit actionDone(run->requestId, id, {}, false, u"插件已停止"_s);
            if (m_runner)
                m_runner->stop(instance);
        }
    }
    stopResident(p, false);
    failCommandCalls(id, u"插件已停止"_s);
}

void Host::reload(const QString& id)
{
    if (const auto it = m_plugins.find(id); it != m_plugins.end()) {
        Plugin& p = *it->second;
        const bool running = p.phase == Plugin::Phase::Starting || p.phase == Plugin::Phase::Ready;
        p.disabledByCrashes = false;
        p.crashTimes.clear();
        p.backoffStep = 0;
        p.error.clear();
        stop(id);
        p.restartWhenGone = running;
    }
    log(u"%1: 重新加载"_s.arg(id));
    m_catalog.scan();
    emit stateChanged(id);
}

// Passwords among a plugin's settings, wherever they appear in a line
// (initialize, settingsChanged, WS_SETTINGS): never in a log or a trace.
QString Host::redact(const QString& id, QString text) const
{
    const Entry* e = entry(id);
    if (!e)
        return text;
    const QJsonObject settings = pluginSettings(id);
    for (const Setting& s : e->manifest->settings) {
        if (s.type != u"password")
            continue;
        const QString value = settings.value(s.key).toString();
        if (value.size() >= 1)
            text.replace(value, u"••••••"_s);
    }
    return text;
}

void Host::trace(Plugin& p, const QString& direction, const QByteArray& line)
{
    if (!m_developerMode)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString summary;
    int elapsed = -1;
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (document.isObject()) {
        const QJsonObject o = document.object();
        const QString method = o.value(u"method"_s).toString();
        const QJsonValue id = o.value(u"id"_s);
        if (!method.isEmpty())
            summary = method + (id.isUndefined() ? QString() : u" #%1"_s.arg(id.toVariant().toString()));
        else if (o.contains(u"error"_s))
            summary = u"error #%1"_s.arg(id.toVariant().toString());
        else
            summary = u"result #%1"_s.arg(id.toVariant().toString());
        if (id.isDouble() && direction == u"→" && !method.isEmpty())
            p.sentAt[static_cast<std::uint64_t>(id.toDouble())] = now;
        else if (id.isDouble() && direction == u"←" && method.isEmpty()) {
            if (const auto it = p.sentAt.find(static_cast<std::uint64_t>(id.toDouble())); it != p.sentAt.end()) {
                elapsed = static_cast<int>(now - it->second);
                p.sentAt.erase(it);
            }
        }
    } else
        summary = QString::fromUtf8(line.left(60));
    emit traced(p.id, direction, redact(p.id, summary), redact(p.id, QString::fromUtf8(line)), elapsed);
}

void Host::traceEvent(const QString& id, const QString& text)
{
    if (m_developerMode)
        emit traced(id, u"·"_s, text, text, -1);
}

Host::~Host()
{
    shutdown();
}

int Host::backoffDelayMs(int step)
{
    static constexpr int kDelays[] = {1000, 5000, 30000};
    return kDelays[std::clamp(step, 0, 2)];
}

Host::Plugin& Host::plugin(const QString& id)
{
    auto it = m_plugins.find(id);
    if (it == m_plugins.end()) {
        auto p = std::make_unique<Plugin>();
        p->id = id;
        p->queryTimer.setSingleShot(true);
        p->debounce.setSingleShot(true);
        p->backoff.setSingleShot(true);
        p->idle.setSingleShot(true);
        p->rerun.setSingleShot(true);
        p->toastGap.setSingleShot(true);
        Plugin* raw = p.get();
        connect(&raw->queryTimer, &QTimer::timeout, this, [this, id] { onQueryTimeout(id); });
        connect(&raw->rerun, &QTimer::timeout, this, [this, raw] {
            // Still the launcher's (cancel() and cancelAll() forget it), and
            // nothing else asked meanwhile: the same query again.
            if (raw->lastQuery && raw->phase == Plugin::Phase::Ready && raw->session && !raw->inflight) {
                const Pending q = *raw->lastQuery;
                sendQuery(*raw, q);
            }
        });
        connect(&raw->toastGap, &QTimer::timeout, this, [this, raw] { showToast(*raw); });
        connect(&raw->debounce, &QTimer::timeout, this, [this, raw] {
            if (raw->waiting) {
                const Pending q = *raw->waiting;
                raw->waiting.reset();
                launchOnce(*raw, q);
            }
        });
        connect(&raw->backoff, &QTimer::timeout, this, [this, raw] {
            if (raw->phase != Plugin::Phase::Backoff)
                return;
            raw->phase = Plugin::Phase::Stopped;
            const Entry* e = entry(raw->id);
            const bool eager = e && e->manifest && e->manifest->runtime && e->manifest->runtime->start == StartWhen::Eager;
            const bool called = std::any_of(m_commandCalls.begin(), m_commandCalls.end(),
                [raw](const auto& call) { return call.second->pluginId == raw->id && call.second->waitingForStart; });
            if (raw->pending || eager || called)
                startResident(*raw);
            emit stateChanged(raw->id);
        });
        connect(&raw->idle, &QTimer::timeout, this, [this, raw] {
            if (raw->phase == Plugin::Phase::Ready && !raw->inflight && raw->actions.empty()) {
                log(u"%1: 空闲，退出"_s.arg(raw->id));
                stopResident(*raw, false);
            }
        });
        it = m_plugins.emplace(id, std::move(p)).first;
    }
    return *it->second;
}

const Entry* Host::entry(const QString& id) const
{
    const Entry* e = m_catalog.find(id);
    return e && e->manifest ? e : nullptr;
}

void Host::log(const QString& line)
{
    const QString stamped = QDateTime::currentDateTime().toString(u"yyyy-MM-dd HH:mm:ss.zzz "_s) + line;
    emit logged(stamped);
    if (m_options.logPath.isEmpty())
        return;
    if (QFileInfo(m_options.logPath).size() > kLogRotateBytes) {
        QFile::remove(m_options.logPath + u".1"_s);
        QFile::rename(m_options.logPath, m_options.logPath + u".1"_s);
    }
    QFile file(m_options.logPath);
    if (file.open(QIODevice::Append | QIODevice::Text))
        file.write((stamped + u'\n').toUtf8());
}

void Host::setEnabled(const QString& id, bool on)
{
    if (on) {
        Plugin& p = plugin(id);
        p.disabledByCrashes = false;
        p.crashTimes.clear();
        p.backoffStep = 0;
    }
    m_catalog.setEnabled(id, on);
    emit stateChanged(id);
}

QJsonObject Host::pluginSettings(const QString& id) const
{
    QJsonObject settings;
    if (const Entry* e = entry(id)) {
        for (const Setting& s : e->manifest->settings) {
            if (!s.defaultValue.isUndefined())
                settings.insert(s.key, s.defaultValue);
        }
    }
    const QJsonObject user = m_settings.value(id);
    for (auto it = user.begin(); it != user.end(); ++it)
        settings.insert(it.key(), it.value());
    return settings;
}

void Host::setAppearance(const QString& locale, const QString& theme)
{
    m_options.locale = locale;
    m_options.theme = theme;
}

void Host::setPluginSettings(const QString& id, const QJsonObject& settings)
{
    if (const auto have = m_settings.constFind(id); have != m_settings.constEnd() && *have == settings)
        return; // nothing new to tell
    m_settings[id] = settings;
    const auto it = m_plugins.find(id);
    if (it != m_plugins.end() && it->second->phase == Plugin::Phase::Ready && it->second->session)
        it->second->session->notify(u"settingsChanged"_s, {{u"settings"_s, pluginSettings(id)}});
}

QString Host::dataDirFor(const Entry& e)
{
    const QString dir = QDir::toNativeSeparators(QDir(m_options.dataRoot).filePath(e.id + u".data"_s));
    const bool made = !QFileInfo::exists(dir) && QDir().mkpath(dir);
    // An existing folder too: one a failed label left behind (before labelLow
    // could grant itself the right) would stay unwritable for good.
    if (e.manifest->runtime && e.manifest->runtime->sandbox == SandboxLevel::Strict && (made || !isLabelledLow(dir))
        && !labelLow(dir))
        log(u"%1: 无法给 %2 打低完整性标签"_s.arg(e.id, dir));
    return dir;
}

void Host::touch(Plugin& p)
{
    p.lastUsed = QDateTime::currentMSecsSinceEpoch();
    const Entry* e = entry(p.id);
    if (e && e->manifest->runtime && e->manifest->runtime->idleExitSec > 0 && p.phase == Plugin::Phase::Ready)
        p.idle.start(e->manifest->runtime->idleExitSec * 1000);
}

// ---- the runner ---------------------------------------------------------------

bool Host::ensureRunner()
{
    if (m_runner && m_runner->running())
        return true;
    if (m_runnerBackoff.isActive())
        return false;
    if (!m_runner) {
        m_runner = std::make_unique<Runner>(m_options.runnerExe, this);
        connect(m_runner.get(), &Runner::ready, this, [this] {
            m_runnerRestarts = 0;
            log(u"runner 就绪（pid %1，令牌来源 %2）"_s.arg(m_runner->pid()).arg(static_cast<int>(m_runner->tokenSource())));
            // Back after a crash: whatever waited for it goes on now.
            for (auto& [id, p] : m_plugins) {
                const Entry* e = entry(id);
                if (!e || !m_catalog.enabled(id) || p->disabledByCrashes)
                    continue;
                if (p->waiting && !p->debounce.isActive())
                    p->debounce.start(0);
                const bool eager = e->manifest->resident() && e->manifest->runtime->start == StartWhen::Eager;
                if (p->phase == Plugin::Phase::Stopped && (p->pending || eager))
                    startResident(*p);
            }
        });
        connect(m_runner.get(), &Runner::exited, this, &Host::onRunnerExited);
        connect(m_runner.get(), &Runner::started, this, &Host::onStarted);
        connect(m_runner.get(), &Runner::failed, this, &Host::onFailed);
        connect(m_runner.get(), &Runner::line, this, &Host::onLine);
        connect(m_runner.get(), &Runner::stderrLine, this, [this](std::uint64_t instance, const QString& text) {
            const auto it = m_owners.find(instance);
            const QString id = it == m_owners.end() ? u"?"_s : it->second;
            log(u"%1 stderr: %2"_s.arg(id, text));
            if (m_developerMode && it != m_owners.end())
                emit traced(id, u"stderr"_s, redact(id, text.left(60)), redact(id, text), -1);
        });
        connect(m_runner.get(), &Runner::instanceExited, this, &Host::onInstanceExited);
        connect(m_runner.get(), &Runner::logged, this, [this](const QString& text) { log(u"runner: "_s + text); });
    }
    QString error;
    if (!m_runner->start(&error)) {
        log(u"runner 启动失败：%1"_s.arg(error));
        const int delay = backoffDelayMs(m_runnerRestarts++);
        m_runnerBackoff.start(static_cast<int>(delay * m_backoffScale));
        return false;
    }
    return true;
}

void Host::onRunnerExited()
{
    if (m_shuttingDown)
        return;
    log(u"runner 退出了"_s);
    for (auto& [id, p] : m_plugins) {
        if (p->phase == Plugin::Phase::Starting || p->phase == Plugin::Phase::Ready || p->phase == Plugin::Phase::ShuttingDown) {
            const bool asked = p->phase == Plugin::Phase::ShuttingDown && !p->restartAfterStop;
            p->phase = Plugin::Phase::Stopped;
            p->session.reset();
            p->instance = 0;
            p->pid = 0;
            if (p->inflight) {
                emit queryFailed(p->inflight->queryId, id, u"插件进程退出了"_s);
                p->inflight.reset();
                p->queryTimer.stop();
            }
            for (const auto& [requestId, rpcId] : p->actions)
                emit actionDone(requestId, id, {}, false, u"插件进程退出了"_s);
            p->actions.clear();
            if (!asked)
                crashed(*p, u"runner 退出"_s);
            else
                emit stateChanged(id);
        }
        for (auto& [instance, run] : p->runs) {
            if (run->discarded)
                continue;
            if (run->commandCall)
                finishCommand(run->commandCall, false, {}, u"插件进程退出了"_s);
            else if (run->kind == RunKind::Query)
                emit queryFailed(run->queryId, id, u"插件进程退出了"_s);
            else
                emit actionDone(run->requestId, id, {}, false, u"插件进程退出了"_s);
        }
        p->runs.clear();
    }
    m_owners.clear();
    failCommandCalls({}, u"插件进程退出了"_s);
    const int delay = backoffDelayMs(m_runnerRestarts++);
    m_runnerBackoff.start(static_cast<int>(delay * m_backoffScale));
}

// ---- queries ------------------------------------------------------------------

bool Host::query(const QString& text, Scope scope, std::uint64_t queryId, const QJsonObject& filters)
{
    cancelAll();
    if (scope != Scope::All)
        return false;
    const std::vector<Match> matches = m_catalog.match(text);
    bool any = false;
    for (const Match& m : matches) {
        if (m.command->zeroCode()) {
            emit queryTaken(queryId, m.entry->id);
            const Items items = runZeroCode(*m.command, m.text, m_options.locale);
            emit results(queryId, m.entry->id, m.command->id, items, false, true);
            emit queryFinished(queryId, m.entry->id);
            any = true;
            continue;
        }
        Plugin& p = plugin(m.entry->id);
        if (p.disabledByCrashes)
            continue;
        emit queryTaken(queryId, m.entry->id);
        Pending q;
        q.queryId = queryId;
        q.commandId = m.command->id;
        q.kind = m.kind;
        q.keyword = m.keyword;
        q.text = m.text;
        q.raw = text;
        q.filters = filters;
        q.captures = m.captures;
        q.limit = m.kind == Match::Kind::Push ? m.maxItems : kMaxItems;
        any = true;
        if (m.entry->manifest->resident()) {
            if (p.phase == Plugin::Phase::Ready) {
                sendQuery(p, q);
            } else {
                p.pending = q; // the newest waits for the process
                if (p.phase == Plugin::Phase::Stopped)
                    startResident(p);
            }
        } else {
            const int debounce = m.entry->manifest->runtime->debounceMs;
            p.waiting = q;
            p.debounce.start(debounce);
        }
    }
    return any;
}

void Host::dropInflight(Plugin& p, bool tellPlugin)
{
    if (!p.inflight)
        return;
    if (tellPlugin && p.session)
        p.session->notify(u"cancel"_s, {{u"queryId"_s, static_cast<double>(p.inflight->queryId)}});
    if (p.session)
        p.session->forget(p.inflight->requestId);
    p.inflight.reset();
    p.queryTimer.stop();
    p.refreshAfterReply = false;
}

void Host::stopRerun(Plugin& p)
{
    p.rerun.stop();
    p.lastQuery.reset();
    p.refreshAfterReply = false;
}

// winshun/refresh: the rows the launcher shows changed by themselves (传: a
// request came). Only a list still shown whose last answer asked for
// `rerun`, as the rerun timer would send it; else nothing.
void Host::onRefresh(const QString& id)
{
    Plugin& p = plugin(id);
    if (p.phase != Plugin::Phase::Ready || !p.session || !p.lastQuery)
        return;
    if (p.inflight) {
        // Its answer may be from before the change (a key typed just then,
        // or the first query): onQueryReply asks again if it wants `rerun`.
        if (p.inflight->queryId == p.lastQuery->queryId)
            p.refreshAfterReply = true;
        return;
    }
    if (p.rerunMs <= 0 || p.rerunQuery != p.lastQuery->queryId || !p.rerun.isActive())
        return;
    const qint64 since = p.lastQuerySent.isValid() ? p.lastQuerySent.elapsed() : kRefreshGapMs;
    const int wait = static_cast<int>(std::max<qint64>(0, kRefreshGapMs - since));
    if (p.rerun.remainingTime() > wait) // a burst of them: the first one decides
        p.rerun.start(wait);
}

void Host::cancel(std::uint64_t queryId)
{
    for (auto& [id, p] : m_plugins) {
        if (p->lastQuery && p->lastQuery->queryId == queryId)
            stopRerun(*p);
        if (p->inflight && p->inflight->queryId == queryId)
            dropInflight(*p, true);
        if (p->pending && p->pending->queryId == queryId)
            p->pending.reset();
        if (p->waiting && p->waiting->queryId == queryId) {
            p->waiting.reset();
            p->debounce.stop();
        }
        for (auto& [instance, run] : p->runs) {
            if (run->kind == RunKind::Query && run->queryId == queryId && !run->discarded && !run->commandCall) {
                run->discarded = true;
                run->timer.stop();
                if (m_runner)
                    m_runner->stop(instance);
            }
        }
    }
}

void Host::cancelAll()
{
    for (auto& [id, p] : m_plugins) {
        stopRerun(*p);
        dropInflight(*p, true);
        p->pending.reset();
        if (p->waiting) {
            p->waiting.reset();
            p->debounce.stop();
        }
        for (auto& [instance, run] : p->runs) {
            if (run->kind == RunKind::Query && !run->discarded && !run->commandCall) {
                run->discarded = true;
                run->timer.stop();
                if (m_runner)
                    m_runner->stop(instance);
            }
        }
    }
}

// ---- resident plugins -------------------------------------------------------

Runner::Spec Host::specFor(Plugin& p, const Entry& e, RunKind kind, const Pending* q, QString* error)
{
    Runner::Spec spec;
    const Runtime& runtime = *e.manifest->runtime;
    const std::optional<programs::Launch> launch = programs::launchFor(runtime, e.dir, error);
    if (!launch)
        return spec;
    spec.exe = launch->exe;
    spec.args = launch->args;
    spec.cwd = launch->cwd;
    spec.env = launch->env;
    spec.env += runtime.env;
    spec.env.push_back({u"WS_PLUGIN_DIR"_s, e.dir});
    spec.env.push_back({u"WS_DATA_DIR"_s, dataDirFor(e)});
    spec.env.push_back({u"WS_LOCALE"_s, m_options.locale});
    spec.env.push_back({u"WS_THEME"_s, m_options.theme});
    spec.env.push_back({u"WS_VERSION"_s, m_options.version});
    spec.env.push_back({u"WS_SETTINGS"_s, QString::fromUtf8(QJsonDocument(pluginSettings(e.id)).toJson(QJsonDocument::Compact))});
    if (q) {
        spec.env.push_back({u"WS_COMMAND"_s, q->commandId});
        spec.env.push_back({u"WS_TEXT"_s, q->raw});
        spec.env.push_back({u"WS_QUERY"_s, q->text});
        spec.env.push_back({u"WS_FILTERS"_s, QString::fromUtf8(QJsonDocument(q->filters).toJson(QJsonDocument::Compact))});
        spec.env.push_back({u"WS_TRIGGER"_s, q->files.isEmpty() ? QString::fromLatin1(kindName(q->kind)) : u"files"_s});
        if (!q->keyword.isEmpty())
            spec.env.push_back({u"WS_KEYWORD"_s, q->keyword});
        spec.env.push_back({u"WS_MATCH"_s, QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(q->captures)).toJson(QJsonDocument::Compact))});
        if (kind == RunKind::Action) {
            spec.env.push_back({u"WS_ACTION"_s, q->actionId});
            spec.env.push_back({u"WS_ITEM"_s, QString::fromUtf8(QJsonDocument(q->item).toJson(QJsonDocument::Compact))});
            if (!q->files.isEmpty())
                spec.env.push_back({u"WS_FILES"_s, QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(q->files)).toJson(QJsonDocument::Compact))});
        }
    }
    const Permissions& permissions = e.manifest->permissions;
    spec.memoryMB = runtime.memoryMB;
    spec.processes = permissions.has(Permission::Processes);
    spec.clipboardRead = permissions.has(Permission::ClipboardRead);
    spec.clipboardWrite = permissions.has(Permission::ClipboardWrite);
    spec.ui = permissions.has(Permission::Ui);
    spec.strict = runtime.sandbox == SandboxLevel::Strict;
    spec.oem = runtime.encoding == Encoding::Oem;
    spec.stdinOpen = runtime.mode == RunMode::Resident;
    (void)p;
    return spec;
}

void Host::evictIfCrowded(const QString& starting)
{
    // Resident processes beyond the cap: the one unused longest goes,
    // unless it is eager or busy.
    for (;;) {
        int running = 0;
        Plugin* oldest = nullptr;
        for (auto& [id, p] : m_plugins) {
            if (p->phase != Plugin::Phase::Starting && p->phase != Plugin::Phase::Ready)
                continue;
            ++running;
            if (id == starting || p->inflight || !p->actions.empty())
                continue;
            const Entry* e = entry(id);
            if (e && e->manifest->runtime && e->manifest->runtime->start == StartWhen::Eager)
                continue;
            if (!oldest || p->lastUsed < oldest->lastUsed)
                oldest = p.get();
        }
        if (running < m_options.maxResident || !oldest)
            return;
        log(u"%1: 常驻的太多，退出"_s.arg(oldest->id));
        stopResident(*oldest, false);
    }
}

void Host::startResident(Plugin& p)
{
    const Entry* e = entry(p.id);
    if (!e || !m_catalog.enabled(p.id) || p.disabledByCrashes || p.phase != Plugin::Phase::Stopped)
        return;
    if (!ensureRunner())
        return; // the pending query waits for the runner's return
    QString error;
    Runner::Spec spec = specFor(p, *e, RunKind::Query, nullptr, &error);
    if (spec.exe.isEmpty()) {
        p.error = error;
        log(u"%1: 启动不了：%2"_s.arg(p.id, error));
        if (p.pending) {
            emit queryFailed(p.pending->queryId, p.id, error);
            p.pending.reset();
        }
        emit stateChanged(p.id);
        return;
    }
    evictIfCrowded(p.id);
    p.instance = m_runner->launch(spec);
    p.phase = Plugin::Phase::Starting;
    p.restartAfterStop = false;
    p.timeouts = 0;
    m_owners[p.instance] = p.id;
    log(u"%1: 启动 %2"_s.arg(p.id, spec.exe));
    emit stateChanged(p.id);
}

void Host::onStarted(std::uint64_t instance, quint32 pid)
{
    const auto owner = m_owners.find(instance);
    if (owner == m_owners.end())
        return;
    Plugin& p = plugin(owner->second);
    if (const auto run = p.runs.find(instance); run != p.runs.end()) {
        run->second->pid = pid;
        emit processStarted(p.id, pid);
        traceEvent(p.id, u"进程 %1 启动（一次性）"_s.arg(pid));
        return;
    }
    if (p.instance != instance || p.phase != Plugin::Phase::Starting)
        return;
    p.pid = pid;
    emit processStarted(p.id, pid);
    traceEvent(p.id, u"进程 %1 启动"_s.arg(pid));
    const Entry* e = entry(p.id);
    if (!e) {
        stopResident(p, false);
        return;
    }
    const QString pluginId = p.id;
    p.session = std::make_unique<Session>([this, instance, pluginId](const QByteArray& line) {
        if (m_developerMode)
            trace(plugin(pluginId), u"→"_s, line);
        if (m_runner)
            m_runner->send(instance, line);
    });
    Session::Handlers handlers;
    const QString id = p.id;
    handlers.partial = [this, id](const QJsonObject& params) {
        Plugin& q = plugin(id);
        const auto queryId = static_cast<std::uint64_t>(params.value(u"queryId"_s).toDouble());
        if (!q.inflight || q.inflight->queryId != queryId)
            return;
        const QueryResult result = parseResult(params.value(u"items"_s));
        emit results(queryId, id, q.inflight->commandId, result.items, true, params.value(u"replace"_s).toBool());
    };
    handlers.log = [this, id](const QString& level, const QString& text) { log(u"%1 [%2] %3"_s.arg(id, level, text)); };
    handlers.notify = [this, id](const QString& text) { emit notice(id, text); };
    handlers.toast = [this, id](const QString& title, const QString& text, const QJsonArray& click) { onToast(id, title, text, click); };
    handlers.progress = [this, id](std::uint64_t queryId, const QString& text) {
        Plugin& q = plugin(id);
        if (q.inflight && q.inflight->queryId == queryId)
            emit progress(queryId, id, text);
    };
    handlers.refresh = [this, id] { onRefresh(id); };
    handlers.toolCall = [this, id](const QJsonValue& requestId, const QString& tool, const QJsonObject& arguments) {
        onToolCall(id, requestId, tool, arguments);
    };
    handlers.protocolError = [this, id](const QString& problem, const QByteArray& line) {
        log(u"%1: 协议错误：%2：%3"_s.arg(id, problem, QString::fromUtf8(line.left(200))));
    };
    p.session->setHandlers(std::move(handlers));

    const QJsonObject params {{u"protocolVersion"_s, QString::fromLatin1(kProtocolVersion)}, {u"winshun"_s, m_options.version},
        {u"pluginDir"_s, e->dir}, {u"dataDir"_s, dataDirFor(*e)}, {u"locale"_s, m_options.locale}, {u"theme"_s, m_options.theme},
        {u"settings"_s, pluginSettings(p.id)}};
    p.session->request(u"initialize"_s, params, [this, id, instance](const jsonrpc::Message& response) {
        Plugin& q = plugin(id);
        if (q.instance != instance || q.phase != Plugin::Phase::Starting)
            return;
        if (response.failed) {
            q.error = u"initialize 失败：%1"_s.arg(response.error.value(u"message"_s).toString());
            log(u"%1: %2"_s.arg(id, q.error));
            stopResident(q, false);
            return;
        }
        const QString version = response.result.toObject().value(u"protocolVersion"_s).toString();
        if (!version.isEmpty() && version != QLatin1StringView(kProtocolVersion))
            log(u"%1: 插件说的协议版本是 %2，Win顺 的是 %3"_s.arg(id, version, QLatin1StringView(kProtocolVersion)));
        q.phase = Plugin::Phase::Ready;
        q.error.clear();
        touch(q);
        emit stateChanged(id);
        if (q.pending) {
            const Pending pending = *q.pending;
            q.pending.reset();
            sendQuery(q, pending);
        }
        flushCommandCalls(q);
    });
    p.queryTimer.start(e->manifest->runtime->timeoutMs * 2); // initialize: twice a query's time
}

void Host::sendQuery(Plugin& p, const Pending& q)
{
    if (!p.session)
        return;
    dropInflight(p, true);
    touch(p);
    p.rerun.stop();
    p.refreshAfterReply = false; // this query is the asking again (also after onRunnerExited, which skips dropInflight)
    p.lastQuery = q;
    QJsonObject params {{u"queryId"_s, static_cast<double>(q.queryId)}, {u"command"_s, q.commandId},
        {u"trigger"_s, QString::fromLatin1(kindName(q.kind))}, {u"text"_s, q.text}, {u"raw"_s, q.raw}, {u"limit"_s, q.limit}};
    if (!q.keyword.isEmpty())
        params.insert(u"keyword"_s, q.keyword);
    if (!q.captures.isEmpty())
        params.insert(u"match"_s, QJsonArray::fromStringList(q.captures));
    params.insert(u"filters"_s, q.filters);
    const QString id = p.id;
    const std::uint64_t queryId = q.queryId;
    const std::uint64_t requestId = p.session->request(u"query"_s, params, [this, id, queryId](const jsonrpc::Message& response) {
        onQueryReply(id, queryId, response);
    });
    p.inflight = Plugin::Inflight {q.queryId, q.commandId, requestId};
    p.lastQuerySent.start();
    const Entry* e = entry(p.id);
    p.queryTimer.start(e ? e->manifest->runtime->timeoutMs : 1500);
}

void Host::onQueryReply(const QString& id, std::uint64_t queryId, const jsonrpc::Message& response)
{
    Plugin& p = plugin(id);
    if (!p.inflight || p.inflight->queryId != queryId)
        return; // a reply to a query since cancelled
    const QString commandId = p.inflight->commandId;
    p.inflight.reset();
    p.queryTimer.stop();
    touch(p);
    if (response.failed) {
        stopRerun(p);
        emit queryFailed(queryId, id, response.error.value(u"message"_s).toString());
        return;
    }
    p.timeouts = 0;
    p.backoffStep = 0;
    const QueryResult result = parseResult(response.result);
    emit results(queryId, id, commandId, result.items, false, true);
    emit queryFinished(queryId, id);
    // Asked again in a while (a list that changes by itself: progress, devices
    // coming and going), unless the launcher moved on meanwhile.
    Plugin& again = plugin(id);
    const bool refreshed = std::exchange(again.refreshAfterReply, false);
    if (result.rerun > 0 && again.lastQuery && again.lastQuery->queryId == queryId && !again.inflight) {
        again.rerunQuery = queryId;
        again.rerunMs = static_cast<int>(std::clamp(result.rerun, kRerunMin, kRerunMax) * 1000);
        // A refresh came while this was being answered: the answer may be
        // from before the change, so it is asked again now (onRefresh).
        again.rerun.start(refreshed ? static_cast<int>(std::max<qint64>(0, kRefreshGapMs - again.lastQuerySent.elapsed())) : again.rerunMs);
    } else if (again.lastQuery && again.lastQuery->queryId == queryId) {
        again.rerunQuery = 0; // this answer did not ask for `rerun`: a refresh does nothing
        again.rerunMs = 0;
    }
}

void Host::onQueryTimeout(const QString& id)
{
    Plugin& p = plugin(id);
    if (p.phase == Plugin::Phase::Starting) {
        p.error = u"initialize 没有回应"_s;
        log(u"%1: %2"_s.arg(id, p.error));
        if (p.pending) {
            emit queryFailed(p.pending->queryId, id, p.error);
            p.pending.reset();
        }
        failCommandCalls(id, p.error);
        stopResident(p, true);
        return;
    }
    if (!p.inflight)
        return;
    const std::uint64_t queryId = p.inflight->queryId;
    dropInflight(p, true);
    log(u"%1: 查询 %2 超时"_s.arg(id).arg(queryId));
    traceEvent(id, u"查询 %1 超时"_s.arg(queryId));
    emit queryFailed(queryId, id, u"没有响应"_s);
    if (++p.timeouts >= kTimeoutsToRestart) {
        log(u"%1: 连续 %2 次没有响应，重启"_s.arg(id).arg(p.timeouts));
        p.timeouts = 0;
        stopResident(p, true);
    } else if (p.lastQuery && p.lastQuery->queryId == queryId && p.rerunQuery == queryId && p.rerunMs > 0) {
        // A list that changes by itself answered late once: asked again as
        // before, so it does not freeze until the next key.
        p.rerun.start(p.rerunMs);
    }
}

// A shutdown request, then the job is ended; `restartAfter`: the process
// comes back after a backoff (it hung), else it stays down until needed.
void Host::stopResident(Plugin& p, bool restartAfter)
{
    if (p.phase == Plugin::Phase::Stopped || p.phase == Plugin::Phase::Backoff)
        return;
    p.idle.stop();
    dropInflight(p, false);
    stopRerun(p);
    p.restartAfterStop = restartAfter;
    if (p.phase == Plugin::Phase::ShuttingDown)
        return;
    const std::uint64_t instance = p.instance;
    p.phase = Plugin::Phase::ShuttingDown;
    emit stateChanged(p.id);
    if (p.session && !restartAfter) {
        p.session->request(u"shutdown"_s, {}, [this, instance](const jsonrpc::Message&) {
            if (m_runner)
                m_runner->stop(instance);
        });
        QTimer::singleShot(kShutdownGraceMs, this, [this, instance] {
            if (m_runner && m_owners.count(instance))
                m_runner->stop(instance);
        });
    } else if (m_runner)
        m_runner->stop(instance);
}

void Host::crashed(Plugin& p, const QString& why)
{
    p.error = why;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    p.crashTimes.push_back(now);
    p.crashTimes.removeIf([now](qint64 t) { return now - t > kCrashWindowMs; });
    if (p.crashTimes.size() >= kCrashesToDisable) {
        p.disabledByCrashes = true;
        p.phase = Plugin::Phase::Stopped;
        log(u"%1: %2 分钟内崩溃 %3 次，已停用"_s.arg(p.id).arg(kCrashWindowMs / 60000).arg(p.crashTimes.size()));
        if (p.pending) {
            emit queryFailed(p.pending->queryId, p.id, u"插件连续崩溃，已停用"_s);
            p.pending.reset();
        }
        failCommandCalls(p.id, u"插件连续崩溃，已停用"_s);
        emit disabledAfterCrashes(p.id);
        emit stateChanged(p.id);
        return;
    }
    failCommandCalls(p.id, why);
    const int delay = backoffDelayMs(p.backoffStep++);
    p.phase = Plugin::Phase::Backoff;
    p.backoff.start(static_cast<int>(delay * m_backoffScale));
    log(u"%1: %2，%3 秒后重启"_s.arg(p.id, why).arg(delay / 1000));
    emit restartScheduled(p.id, delay);
    emit stateChanged(p.id);
}

void Host::onFailed(std::uint64_t instance, const QString& stage, quint32 error)
{
    const auto owner = m_owners.find(instance);
    if (owner == m_owners.end())
        return;
    Plugin& p = plugin(owner->second);
    m_owners.erase(owner);
    const QString why = u"启动失败：%1（错误 %2）"_s.arg(stage).arg(error);
    log(u"%1: %2"_s.arg(p.id, why));
    if (const auto run = p.runs.find(instance); run != p.runs.end()) {
        if (!run->second->discarded) {
            if (run->second->commandCall)
                finishCommand(run->second->commandCall, false, {}, why);
            else if (run->second->kind == RunKind::Query)
                emit queryFailed(run->second->queryId, p.id, why);
            else
                emit actionDone(run->second->requestId, p.id, {}, false, why);
        }
        p.runs.erase(run);
        return;
    }
    if (p.instance != instance)
        return;
    p.instance = 0;
    p.session.reset();
    p.phase = Plugin::Phase::Stopped;
    crashed(p, why);
}

void Host::onLine(std::uint64_t instance, const QByteArray& line)
{
    const auto owner = m_owners.find(instance);
    if (owner == m_owners.end())
        return;
    Plugin& p = plugin(owner->second);
    if (const auto run = p.runs.find(instance); run != p.runs.end()) {
        if (m_developerMode)
            trace(p, u"←"_s, line);
        run->second->output += line;
        run->second->output += '\n';
        return;
    }
    if (p.instance == instance && p.session) {
        if (m_developerMode)
            trace(p, u"←"_s, line);
        p.session->receive(line);
    }
}

void Host::onInstanceExited(std::uint64_t instance, quint32 code, const QStringList& stderrTail, bool overlong)
{
    const auto owner = m_owners.find(instance);
    if (owner == m_owners.end())
        return;
    Plugin& p = plugin(owner->second);
    m_owners.erase(owner);
    if (p.runs.count(instance)) {
        finishOnce(p, instance, code, stderrTail, overlong);
        return;
    }
    if (p.instance != instance)
        return;
    emit processExited(p.id, code);
    traceEvent(p.id, u"进程退出（代码 %1）"_s.arg(code));
    const Plugin::Phase was = p.phase;
    p.instance = 0;
    p.pid = 0;
    p.phase = Plugin::Phase::Stopped;
    p.idle.stop();
    p.queryTimer.stop();
    stopRerun(p);
    p.sentAt.clear();
    if (p.session)
        p.session->failAll(jsonrpc::kUnavailable, u"插件进程退出了"_s); // the query in flight fails through its reply
    p.session.reset();
    for (const auto& [callId, unused] : p.toolCalls) {
        if (m_bridge.cancel)
            m_bridge.cancel(callId);
    }
    p.toolCalls.clear();
    if (p.inflight) {
        emit queryFailed(p.inflight->queryId, p.id, u"插件进程退出了"_s);
        p.inflight.reset();
    }
    for (const auto& [requestId, rpcId] : p.actions)
        emit actionDone(requestId, p.id, {}, false, u"插件进程退出了"_s);
    p.actions.clear();
    failCommandCalls(p.id, u"插件进程退出了"_s); // the ones sent failed through their replies already; the waiting ones here
    if (was == Plugin::Phase::ShuttingDown && !p.restartAfterStop) {
        log(u"%1: 退出（代码 %2）"_s.arg(p.id).arg(code));
        emit stateChanged(p.id);
        if (p.pending || std::exchange(p.restartWhenGone, false))
            startResident(p); // asked for again while it was leaving, or reloaded
        return;
    }
    QString why = was == Plugin::Phase::ShuttingDown ? u"挂死后重启"_s : u"崩溃（退出码 %1）"_s.arg(code);
    if (overlong)
        why = u"一行超过 16 MB"_s;
    if (!stderrTail.isEmpty())
        log(u"%1: stderr 末尾：%2"_s.arg(p.id, stderrTail.join(u" | "_s)));
    crashed(p, why);
}

void Host::onToolCall(const QString& id, const QJsonValue& requestId, const QString& tool, const QJsonObject& arguments)
{
    Plugin& p = plugin(id);
    if (!p.session)
        return;
    // Turned off, or a newer manifest waiting for the user's OK, while the
    // process is on its way out: its permissions are not the user's word.
    if (!m_catalog.enabled(id)) {
        p.session->respondError(requestId, jsonrpc::kUnavailable, u"The plugin is off in Win顺's settings, or waits for the user to allow new permissions"_s);
        return;
    }
    const Entry* e = entry(id);
    const std::optional<Permission> needed = permissionFor(tool);
    if (!m_bridge.call || !m_bridge.known) {
        p.session->respondError(requestId, jsonrpc::kUnavailable, u"Win顺's tools are not available here"_s);
        return;
    }
    switch (m_bridge.known(tool)) {
    case mcp::ToolHost::Known::No:
        p.session->respondError(requestId, jsonrpc::kInvalidParams, u"Unknown tool: "_s + tool);
        return;
    case mcp::ToolHost::Known::Off:
        p.session->respondError(requestId, jsonrpc::kUnavailable, u"The tool '%1' is turned off in Win顺's settings"_s.arg(tool));
        return;
    case mcp::ToolHost::Known::Yes:
        break;
    }
    if (needed && (!e || !e->manifest->permissions.has(*needed))) {
        p.session->respondError(requestId, kPermissionDenied,
            u"The plugin's manifest does not ask for the '%1' permission, which '%2' needs"_s.arg(permissionName(*needed), tool));
        return;
    }
    // open_item opens any path the plugin names, with the user's rights, outside
    // the sandbox -- the same reach as a declarative `open`. Without `processes`
    // a plugin may use it for folders and ordinary files, never to start a
    // program (the same rule as OpenPolicy; show_in_folder only selects, so it
    // is left alone).
    if (tool == u"open_item" && e && !e->manifest->permissions.has(Permission::Processes)) {
        Open open;
        open.kind = Open::Kind::Path;
        open.target = arguments.value(u"path"_s).toString();
        if (!ruleOpen(open, false, systemProbe()).allowed()) {
            p.session->respondError(requestId, kPermissionDenied,
                u"The plugin lacks the 'processes' permission, so Win顺 will not open a program for it: "_s + open.target);
            return;
        }
    }
    const std::uint64_t callId = m_nextToolCall++;
    p.toolCalls[callId] = 0;
    const std::uint64_t instance = p.instance;
    m_bridge.call(callId, u"plugin:"_s + id, tool, arguments, [this, id, instance, requestId, callId](mcp::ToolResult result) {
        Plugin& q = plugin(id);
        q.toolCalls.erase(callId);
        if (q.instance != instance || !q.session)
            return;
        if (result.isError)
            q.session->respondError(requestId, kToolFailed, result.errorText);
        else
            q.session->respond(requestId, result.structured);
    });
}

// ---- once plugins -------------------------------------------------------------

void Host::launchOnce(Plugin& p, const Pending& q)
{
    const Entry* e = entry(p.id);
    if (!e || !m_catalog.enabled(p.id))
        return;
    const RunKind kind = q.actionId.isEmpty() ? RunKind::Query : RunKind::Action;
    const auto fail = [&](const QString& why) {
        log(u"%1: %2"_s.arg(p.id, why));
        if (q.commandCall)
            finishCommand(q.commandCall, false, {}, why);
        else if (kind == RunKind::Query)
            emit queryFailed(q.queryId, p.id, why);
        else
            emit actionDone(q.requestId, p.id, {}, false, why);
    };
    if (!ensureRunner()) {
        if (kind == RunKind::Query)
            p.waiting = q; // until the runner is back
        else
            fail(u"runner 没有运行"_s);
        return;
    }
    QString error;
    const Runner::Spec spec = specFor(p, *e, kind, &q, &error);
    if (spec.exe.isEmpty()) {
        p.error = error;
        fail(error);
        emit stateChanged(p.id);
        return;
    }
    touch(p);
    const std::uint64_t instance = m_runner->launch(spec);
    traceEvent(p.id, u"起进程：%1 %2"_s.arg(spec.exe, spec.args.join(u' ')));
    auto run = std::make_unique<OnceRun>();
    run->kind = kind;
    run->queryId = q.queryId;
    run->requestId = q.requestId;
    run->commandCall = q.commandCall;
    run->commandId = q.commandId;
    if (kind == RunKind::Query && !q.commandCall)
        run->query = q;
    run->timer.setSingleShot(true);
    const QString id = p.id;
    connect(&run->timer, &QTimer::timeout, this, [this, id, instance] {
        Plugin& q = plugin(id);
        const auto it = q.runs.find(instance);
        if (it == q.runs.end() || it->second->discarded)
            return;
        it->second->discarded = true;
        log(u"%1: 进程超时，结束"_s.arg(id));
        if (it->second->commandCall)
            finishCommand(it->second->commandCall, false, {}, u"没有响应"_s);
        else if (it->second->kind == RunKind::Query)
            emit queryFailed(it->second->queryId, id, u"没有响应"_s);
        else
            emit actionDone(it->second->requestId, id, {}, false, u"没有响应"_s);
        if (m_runner)
            m_runner->stop(instance);
    });
    run->timer.start(e->manifest->runtime->timeoutMs);
    p.runs[instance] = std::move(run);
    m_owners[instance] = p.id;
}

void Host::finishOnce(Plugin& p, std::uint64_t instance, quint32 code, const QStringList& stderrTail, bool overlong)
{
    const auto it = p.runs.find(instance);
    if (it == p.runs.end())
        return;
    std::unique_ptr<OnceRun> run = std::move(it->second);
    p.runs.erase(it);
    run->timer.stop();
    emit processExited(p.id, code);
    if (run->discarded)
        return;
    const auto fail = [&](const QString& why) {
        if (run->commandCall)
            finishCommand(run->commandCall, false, {}, why);
        else if (run->kind == RunKind::Query)
            emit queryFailed(run->queryId, p.id, why);
        else
            emit actionDone(run->requestId, p.id, {}, false, why);
    };
    if (overlong) {
        log(u"%1: 一行超过 16 MB"_s.arg(p.id));
        fail(u"输出的一行超过 16 MB"_s);
        return;
    }
    if (code != 0) {
        const QString why = u"退出码 %1%2"_s.arg(code).arg(stderrTail.isEmpty() ? QString() : u"：%1"_s.arg(stderrTail.last()));
        log(u"%1: %2"_s.arg(p.id, why));
        fail(why);
        return;
    }
    if (run->commandCall) {
        finishCommand(run->commandCall, true, parseOutput(run->output).items, {});
    } else if (run->kind == RunKind::Query) {
        const QueryResult result = parseOutput(run->output);
        emit results(run->queryId, p.id, run->commandId, result.items, false, true);
        emit queryFinished(run->queryId, p.id);
        // `rerun`: started again then, as a query waiting for its debounce
        // (cancel() and cancelAll() forget it), at most once a second.
        Plugin& again = plugin(p.id);
        if (result.rerun > 0 && !again.waiting && run->query.queryId == run->queryId) {
            again.waiting = run->query;
            again.debounce.start(static_cast<int>(std::clamp(result.rerun, 1.0, kRerunMax) * 1000));
        }
    } else {
        const QByteArray trimmed = run->output.trimmed();
        Effects effects;
        if (trimmed.isEmpty())
            effects.push_back(Effect {});
        else {
            const QJsonDocument document = QJsonDocument::fromJson(trimmed);
            effects = parseEffects(document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object()));
        }
        emit actionDone(run->requestId, p.id, effects, true, {});
    }
}

// ---- toasts (winshun/notify with "toast") ----------------------------------------

void Host::onToast(const QString& id, const QString& title, const QString& text, const QJsonArray& click)
{
    Plugin& p = plugin(id);
    const Entry* e = entry(id);
    if (!e || !e->manifest || !e->manifest->permissions.has(Permission::Notifications)) {
        // Not declared: the launcher's footer, as a plain notify would be.
        if (!p.toastRefusedLogged) {
            p.toastRefusedLogged = true;
            log(u"%1: 没有 notifications 权限，系统通知改成了启动器里的提示"_s.arg(id));
        }
        const QString shown = text.trimmed().isEmpty() ? title : text;
        if (!shown.trimmed().isEmpty())
            emit notice(id, shown);
        return;
    }
    if (title.trimmed().isEmpty() && text.trimmed().isEmpty())
        return;
    // The newest waits: what came before it is out of date (a progress, a
    // count), and a plugin cannot flood the Action Center.
    // Of its click the first few only: a click does one of each kind at most (toastClick).
    QJsonArray clickRead;
    for (qsizetype i = 0; i < click.size() && i < kToastClickMost; ++i)
        clickRead.append(click.at(i));
    p.toastWaiting = Plugin::Toast {capped(title, 64), capped(text, 200), clickRead};
    const qint64 since = p.lastToast.isValid() ? p.lastToast.elapsed() : kToastGapMs;
    if (since >= kToastGapMs) {
        p.toastGap.stop();
        showToast(p);
    } else if (!p.toastGap.isActive())
        p.toastGap.start(static_cast<int>(kToastGapMs - since));
}

void Host::showToast(Plugin& p)
{
    if (!p.toastWaiting)
        return;
    const Plugin::Toast toast = *std::exchange(p.toastWaiting, std::nullopt);
    const Entry* e = entry(p.id);
    if (!e || !e->manifest || !e->manifest->permissions.has(Permission::Notifications) || !m_catalog.enabled(p.id))
        return; // turned off or gone meanwhile
    p.lastToast.start();
    // No click effects: nothing to do on a click (parseEffects would make
    // that a Hide). Of the rest only what cannot surprise (ToastPolicy): its
    // own list, a local file or folder, a copy with clipboard:write.
    const Effects asked = toast.click.isEmpty() ? Effects {} : parseEffects(QJsonValue(toast.click));
    const QString id = p.id;
    const Effects click = toastClick(asked, [this, &id](const QString& query) { return opensOwnList(id, query); },
        e->manifest->permissions.has(Permission::ClipboardWrite));
    if (click.size() < asked.size() && !(asked.size() == 1 && asked.first().kind == Effect::Kind::Hide))
        log(u"%1: 系统通知的点击只留下打开自己的列表、本机文件或文件夹、复制（要 clipboard:write），每样一个"_s.arg(p.id));
    constexpr bool oneLine = false; // the text keeps its line breaks
    const QString heading = toastWords(toast.title);
    const QString words = toastWords(toast.text, oneLine);
    if (heading.isEmpty() && words.isEmpty()) {
        log(u"%1: 系统通知里没有能显示的字，没有弹出"_s.arg(p.id));
        return;
    }
    log(u"%1: 系统通知"_s.arg(p.id)); // never its words: they may be private
    // Its name first, which it cannot leave out: never taken for Win顺's own
    // (toastName: one the user put in shows in 插件「…」, its id when its
    // name shows nothing). No text: the title is the text too (a tray
    // notification without one is taken away instead of shown).
    const ToastFrom from = e->source == Entry::Source::Bundled ? ToastFrom::Bundled : ToastFrom::User;
    emit toastRequested(p.id, toastTitle(from, p.id, e->manifest->name.pick(m_options.locale), toast.title, m_options.locale),
        words.isEmpty() ? heading : words, click);
}

// `query` opens one of this plugin's commands: its keyword (with a space for
// a list), as the launcher would read it (Catalog::match, hint).
bool Host::opensOwnList(const QString& id, const QString& query) const
{
    for (const Match& m : m_catalog.match(query)) {
        if (m.kind == Match::Kind::Keyword)
            return m.entry && m.entry->id == id;
    }
    const std::optional<Match> alone = m_catalog.hint(query);
    return alone && alone->entry && alone->entry->id == id;
}

// ---- actions ------------------------------------------------------------------

void Host::action(std::uint64_t requestId, const QString& pluginId, const QString& commandId, const QString& actionId,
    const Item& item, const QString& text)
{
    action(requestId, pluginId, commandId, actionId, itemToJson(item), text);
}

void Host::action(std::uint64_t requestId, const QString& pluginId, const QString& commandId, const QString& actionId,
    const QJsonObject& item, const QString& text, const QStringList& files)
{
    const Entry* e = entry(pluginId);
    const Command* command = e ? e->manifest->command(commandId) : nullptr;
    if (!command) {
        emit actionDone(requestId, pluginId, {}, false, u"没有这个命令"_s);
        return;
    }
    // Off, or waiting for the user's OK on a newer version's permissions:
    // its rows still on show, a `files` command, an AI's _run do nothing
    // (a once plugin's launchOnce would just return, with no answer).
    if (!m_catalog.enabled(pluginId)) {
        emit actionDone(requestId, pluginId, {}, false, offText(m_catalog, pluginId));
        return;
    }
    if (command->zeroCode()) {
        emit actionDone(requestId, pluginId, {Effect {}}, true, {});
        return;
    }
    Plugin& p = plugin(pluginId);
    if (p.disabledByCrashes) {
        emit actionDone(requestId, pluginId, {}, false, u"插件已停用"_s);
        return;
    }
    Pending q;
    q.requestId = requestId;
    q.commandId = commandId;
    q.actionId = actionId.isEmpty() ? u"default"_s : actionId;
    q.item = item;
    q.text = text;
    q.raw = text;
    q.files = files;
    if (!e->manifest->resident()) {
        // WS_FILES is one environment variable: 32,767 characters at most.
        if (!files.isEmpty() && QJsonDocument(QJsonArray::fromStringList(files)).toJson(QJsonDocument::Compact).size() > 32000) {
            emit actionDone(requestId, pluginId, {}, false, u"选的文件太多了，少选一些再试"_s);
            return;
        }
        launchOnce(p, q);
        return;
    }
    if (p.phase != Plugin::Phase::Ready || !p.session) {
        // Not running: start it, and act once it is. One action waits at a time.
        if (p.phase == Plugin::Phase::Stopped)
            startResident(p);
        QTimer::singleShot(0, this, [this, requestId, pluginId, commandId, actionId, item, text, files, tries = 0]() mutable {
            Plugin& q = plugin(pluginId);
            if (q.phase == Plugin::Phase::Ready && q.session) {
                action(requestId, pluginId, commandId, actionId, item, text, files);
                return;
            }
            if (q.phase == Plugin::Phase::Stopped || q.disabledByCrashes || ++tries > 100) {
                emit actionDone(requestId, pluginId, {}, false, q.error.isEmpty() ? u"插件没有启动"_s : q.error);
                return;
            }
            // Still starting: look again shortly.
            QTimer::singleShot(50, this, [this, requestId, pluginId, commandId, actionId, item, text, files] {
                action(requestId, pluginId, commandId, actionId, item, text, files);
            });
        });
        return;
    }
    touch(p);
    QJsonObject params {{u"command"_s, commandId}, {u"action"_s, q.actionId}, {u"item"_s, q.item}, {u"text"_s, text}};
    if (!files.isEmpty()) {
        params.insert(u"trigger"_s, u"files"_s);
        params.insert(u"files"_s, QJsonArray::fromStringList(files));
    }
    const std::uint64_t rpcId = p.session->request(u"action"_s, params, [this, pluginId, requestId](const jsonrpc::Message& response) {
        Plugin& q = plugin(pluginId);
        q.actions.erase(requestId);
        touch(q);
        if (response.failed) {
            emit actionDone(requestId, pluginId, {}, false, response.error.value(u"message"_s).toString());
            return;
        }
        emit actionDone(requestId, pluginId, parseEffects(response.result), true, {});
    });
    p.actions[requestId] = rpcId;
}

// ---- command calls (the AI's) ---------------------------------------------------

std::uint64_t Host::callCommand(const QString& pluginId, const QString& commandId, const QString& text, int limit, CommandReply reply)
{
    const std::uint64_t id = m_nextCommandCall++;
    const Entry* e = entry(pluginId);
    const Command* command = e ? e->manifest->command(commandId) : nullptr;
    if (!command || !m_catalog.enabled(pluginId)) {
        reply(false, {}, !command ? u"没有这个命令"_s : offText(m_catalog, pluginId));
        return id;
    }
    if (command->zeroCode()) {
        Items items = runZeroCode(*command, text, m_options.locale);
        if (items.size() > limit)
            items.resize(limit);
        reply(true, items, {});
        return id;
    }
    Plugin& p = plugin(pluginId);
    if (p.disabledByCrashes) {
        reply(false, {}, u"插件连续崩溃，已停用"_s);
        return id;
    }
    auto call = std::make_unique<CommandCall>();
    call->id = id;
    call->pluginId = pluginId;
    call->commandId = commandId;
    call->text = text;
    call->limit = std::clamp(limit, 1, kMaxItems);
    call->reply = std::move(reply);
    call->timer.setSingleShot(true);
    connect(&call->timer, &QTimer::timeout, this, [this, id] {
        const auto it = m_commandCalls.find(id);
        if (it == m_commandCalls.end())
            return;
        CommandCall& c = *it->second;
        log(u"%1: 工具调用 %2 超时"_s.arg(c.pluginId).arg(id));
        Plugin& q = plugin(c.pluginId);
        if (c.rpcId && q.session) {
            q.session->forget(c.rpcId);
            q.session->notify(u"cancel"_s, {{u"queryId"_s, static_cast<double>(id)}});
        }
        for (auto& [instance, run] : q.runs) {
            if (run->commandCall == id && !run->discarded) {
                run->discarded = true;
                run->timer.stop();
                if (m_runner)
                    m_runner->stop(instance);
            }
        }
        finishCommand(id, false, {}, u"没有响应"_s);
    });
    // The AI is not typing: a start and a query may both take their time.
    call->timer.start(e->manifest->runtime->timeoutMs * 3);
    CommandCall& c = *m_commandCalls.emplace(id, std::move(call)).first->second;
    if (e->manifest->resident()) {
        if (p.phase == Plugin::Phase::Ready && p.session) {
            sendCommand(p, c);
            return id;
        }
        c.waitingForStart = true;
        if (p.phase == Plugin::Phase::Stopped)
            startResident(p);
        if (p.phase == Plugin::Phase::Stopped) // it would not start: the error says why
            finishCommand(id, false, {}, p.error.isEmpty() ? u"插件没有启动"_s : p.error);
        return id;
    }
    Pending q;
    q.queryId = id;
    q.commandCall = id;
    q.commandId = commandId;
    q.kind = Match::Kind::Keyword;
    q.keyword = command->keywords.isEmpty() ? QString() : command->keywords.first();
    q.text = text;
    q.raw = q.keyword.isEmpty() ? text : q.keyword + u' ' + text;
    q.limit = c.limit;
    launchOnce(p, q);
    return id;
}

void Host::sendCommand(Plugin& p, CommandCall& call)
{
    if (!p.session)
        return;
    call.waitingForStart = false;
    touch(p);
    const Entry* e = entry(p.id);
    const Command* command = e ? e->manifest->command(call.commandId) : nullptr;
    const QString keyword = command && !command->keywords.isEmpty() ? command->keywords.first() : QString();
    QJsonObject params {{u"queryId"_s, static_cast<double>(call.id)}, {u"command"_s, call.commandId}, {u"trigger"_s, u"keyword"_s},
        {u"text"_s, call.text}, {u"raw"_s, keyword.isEmpty() ? call.text : keyword + u' ' + call.text}, {u"limit"_s, call.limit}};
    if (!keyword.isEmpty())
        params.insert(u"keyword"_s, keyword);
    const std::uint64_t id = call.id;
    call.rpcId = p.session->request(u"query"_s, params, [this, id](const jsonrpc::Message& response) {
        const auto it = m_commandCalls.find(id);
        if (it == m_commandCalls.end())
            return;
        if (response.failed) {
            finishCommand(id, false, {}, response.error.value(u"message"_s).toString());
            return;
        }
        QueryResult result = parseResult(response.result);
        if (result.items.size() > it->second->limit)
            result.items.resize(it->second->limit);
        finishCommand(id, true, result.items, {});
    });
}

void Host::finishCommand(std::uint64_t callId, bool ok, const Items& items, const QString& error)
{
    const auto it = m_commandCalls.find(callId);
    if (it == m_commandCalls.end())
        return;
    std::unique_ptr<CommandCall> call = std::move(it->second);
    m_commandCalls.erase(it);
    call->timer.stop();
    if (call->reply)
        call->reply(ok, items, error);
}

void Host::flushCommandCalls(Plugin& p)
{
    std::vector<std::uint64_t> ids;
    for (const auto& [id, call] : m_commandCalls) {
        if (call->pluginId == p.id && call->waitingForStart)
            ids.push_back(id);
    }
    for (const std::uint64_t id : ids) {
        if (const auto it = m_commandCalls.find(id); it != m_commandCalls.end())
            sendCommand(p, *it->second);
    }
}

void Host::failCommandCalls(const QString& pluginId, const QString& why)
{
    std::vector<std::uint64_t> ids;
    for (const auto& [id, call] : m_commandCalls) {
        if (pluginId.isEmpty() || call->pluginId == pluginId)
            ids.push_back(id);
    }
    for (const std::uint64_t id : ids)
        finishCommand(id, false, {}, why);
}

void Host::cancelCommand(std::uint64_t callId)
{
    const auto it = m_commandCalls.find(callId);
    if (it == m_commandCalls.end())
        return;
    std::unique_ptr<CommandCall> call = std::move(it->second);
    m_commandCalls.erase(it);
    call->timer.stop();
    Plugin& p = plugin(call->pluginId);
    if (call->rpcId && p.session) {
        p.session->forget(call->rpcId);
        p.session->notify(u"cancel"_s, {{u"queryId"_s, static_cast<double>(callId)}});
    }
    for (auto& [instance, run] : p.runs) {
        if (run->commandCall == callId && !run->discarded) {
            run->discarded = true;
            run->timer.stop();
            if (m_runner)
                m_runner->stop(instance);
        }
    }
}

// ---- lifecycle ----------------------------------------------------------------

void Host::preload()
{
    for (const Entry& e : m_catalog.entries()) {
        if (!e.manifest || !m_catalog.enabled(e.id) || !e.manifest->resident() || e.manifest->runtime->start != StartWhen::Eager)
            continue;
        Plugin& p = plugin(e.id);
        if (p.phase == Plugin::Phase::Stopped && !p.disabledByCrashes)
            startResident(p);
    }
}

void Host::shutdown(bool grace)
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    m_runnerBackoff.stop();
    if (m_runner && m_runner->running()) {
        // Resident plugins get their shutdown request and a moment to go.
        int waiting = 0;
        for (auto& [id, p] : m_plugins) {
            p->idle.stop();
            p->backoff.stop();
            p->debounce.stop();
            dropInflight(*p, false);
            if (grace && p->phase == Plugin::Phase::Ready && p->session) {
                ++waiting;
                p->phase = Plugin::Phase::ShuttingDown;
                p->session->request(u"shutdown"_s, {}, [&waiting](const jsonrpc::Message&) { --waiting; });
            }
        }
        if (waiting > 0) {
            QDeadlineTimer deadline(kShutdownGraceMs);
            while (waiting > 0 && !deadline.hasExpired())
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        m_runner->shutdown(); // ends the runner's job: every plugin with it
    }
    m_runner.reset();
    for (auto& [id, p] : m_plugins) {
        p->phase = Plugin::Phase::Stopped;
        p->session.reset();
        p->runs.clear();
    }
    m_owners.clear();
    failCommandCalls({}, u"Win顺 正在退出"_s);
}

Host::State Host::state(const QString& id) const
{
    State s;
    const Entry* e = m_catalog.find(id);
    if (!e)
        return s;
    if (!e->manifest) {
        s.status = State::Status::Unusable;
        s.error = e->error;
        return s;
    }
    if (!m_catalog.enabled(id))
        return s;
    s.status = State::Status::Ready;
    const auto it = m_plugins.find(id);
    if (it == m_plugins.end())
        return s;
    const Plugin& p = *it->second;
    s.pid = p.pid;
    s.error = p.error;
    s.timeouts = p.timeouts;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    s.crashes = static_cast<int>(std::count_if(p.crashTimes.begin(), p.crashTimes.end(), [now](qint64 t) { return now - t <= kCrashWindowMs; }));
    if (p.disabledByCrashes)
        s.status = State::Status::Crashed;
    else if (p.phase == Plugin::Phase::Backoff)
        s.status = State::Status::Backoff;
    else if (p.phase == Plugin::Phase::Starting)
        s.status = State::Status::Starting;
    else if (p.phase == Plugin::Phase::Ready || p.phase == Plugin::Phase::ShuttingDown)
        s.status = State::Status::Running;
    else if (!p.runs.empty())
        s.status = State::Status::Running;
    return s;
}

} // namespace ws::plugin
