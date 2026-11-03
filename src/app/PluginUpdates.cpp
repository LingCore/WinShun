#include "PluginUpdates.h"

#include "Catalog.h"
#include "Host.h"
#include "Settings.h"
#include "platform/Http.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QScopeGuard>
#include <QThreadPool>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws {

namespace gallery = plugin::gallery;
namespace updates = plugin::updates;

namespace {

constexpr int kHourMs = 60 * 60 * 1000;
constexpr int kFirstCheckMs = 90 * 1000;
constexpr int kDaySecs = 24 * 3600;
constexpr int kTimeoutMs = 30000;

gallery::Fetcher webFetcher(int timeoutMs)
{
    return [timeoutMs](const QString& url, qint64 maxBytes, const gallery::Progress& progress) {
        http::Response r = http::get(url, {}, timeoutMs, maxBytes, http::Session::Own, progress);
        return gallery::Fetched {r.status, std::move(r.body), r.error};
    };
}

QString key(const char* what, const QString& id = {})
{
    QString k = u"PluginUpdates/"_s + QLatin1StringView(what);
    return id.isEmpty() ? k : k + u'/' + id;
}

} // namespace

// What the worker brings back for the GUI thread.
struct PluginUpdates::Downloaded {
    gallery::Item item;
    std::optional<plugin::install::Candidate> candidate; // unpacked under the staging root
    QString error;
};

PluginUpdates::PluginUpdates(plugin::Host* host, Options options, QObject* parent)
    : QObject(parent)
    , m_host(host)
    , m_options(std::move(options))
{
    if (!m_options.web)
        m_options.web = webFetcher(kTimeoutMs);
    if (m_options.appVersion.isEmpty())
        m_options.appVersion = QStringLiteral(WINSHUN_VERSION);
    if (!m_host) {
        m_ownCatalog = std::make_unique<plugin::Catalog>(m_options.userDir, m_options.bundledDir);
        m_ownCatalog->scan();
    }
    m_hourly.setInterval(kHourMs);
    connect(&m_hourly, &QTimer::timeout, this, &PluginUpdates::tick);
    m_first.setSingleShot(true);
    connect(&m_first, &QTimer::timeout, this, &PluginUpdates::tick);
    connect(&catalog(), &plugin::Catalog::changed, this, [this] { evaluate(false); });
    // The list kept from the last time, for hints until a signed one comes.
    if (m_options.source == gallery::kIndexUrl) {
        QFile file(m_options.dataDir + u"/plugin-index.json"_s);
        if (file.open(QIODevice::ReadOnly)) {
            if (std::optional<gallery::Index> index = gallery::parse(file.read(gallery::kMaxIndexBytes), m_options.source, nullptr))
                setIndex(std::move(*index), false);
        }
    }
}

PluginUpdates::~PluginUpdates()
{
    m_cancel->store(true); // a worker's result, should it still come, is let go of
}

QString PluginUpdates::defaultSource()
{
    const QString given = qEnvironmentVariable("WINSHUN_PLUGIN_INDEX");
    return given.isEmpty() ? QString(gallery::kIndexUrl) : given;
}

QList<updates::Applied> PluginUpdates::prepare(const QString& dataDir, const QString& userDir, QStringView appVersion,
    const std::function<std::pair<bool, QStringList>(const QString&)>& switchOf)
{
    QSettings state(dataDir + u"/update.ini"_s, QSettings::IniFormat);
    return updates::prepare(dataDir + u"/plugin-updates"_s, userDir, state, appVersion, switchOf);
}

plugin::Catalog& PluginUpdates::catalog() const
{
    return m_host ? m_host->catalog() : *m_ownCatalog;
}

QSettings PluginUpdates::state() const
{
    return QSettings(m_options.dataDir + u"/update.ini"_s, QSettings::IniFormat);
}

QString PluginUpdates::stagingRoot() const
{
    return m_options.dataDir + u"/plugin-updates"_s;
}

bool PluginUpdates::isRunning(const QString& id) const
{
    if (m_running)
        return m_running(id);
    if (!m_host)
        return false;
    const plugin::Host::State::Status s = m_host->state(id).status;
    return s == plugin::Host::State::Status::Running || s == plugin::Host::State::Status::Starting;
}

bool PluginUpdates::hasUserPlugins() const
{
    return std::ranges::any_of(catalog().entries(), [](const plugin::Entry& e) { return e.source == plugin::Entry::Source::User; });
}

updates::Policy PluginUpdates::policy() const
{
    return {m_automatic, m_developerMode, m_trusted, m_options.appVersion};
}

updates::Installed PluginUpdates::installedOf(const plugin::Entry& entry) const
{
    updates::Installed in;
    in.local = gallery::localOf(catalog(), m_options.bundledDir, entry.id);
    in.manifest = entry.manifest ? &*entry.manifest : nullptr;
    in.developerCopy = updates::developerCopy(entry.dir);
    const auto [enabled, confirmed] = m_switches.value(entry.id);
    in.enabled = enabled;
    in.confirmed = confirmed;
    return in;
}

updates::History PluginUpdates::historyOf(const QString& id) const
{
    const QSettings s = state();
    updates::History h;
    h.widened = s.value(key("Widened", id)).toString();
    h.dismissed = s.value(key("Dismissed", id)).toString();
    h.failedVersion = s.value(key("Failed", id)).toString();
    h.failedAt = s.value(key("FailedAt", id)).toDateTime();
    return h;
}

void PluginUpdates::setAutomatic(bool on)
{
    if (m_automatic == on && (m_hourly.isActive() == on))
        return;
    m_automatic = on;
    if (on) {
        m_hourly.start();
        m_first.start(kFirstCheckMs);
    } else {
        m_hourly.stop();
        m_first.stop();
    }
    evaluate(false);
}

void PluginUpdates::setPolicy(bool developerMode, const QList<PluginConfig>& plugins)
{
    m_developerMode = developerMode;
    m_switches.clear();
    for (const PluginConfig& c : plugins)
        m_switches.insert(c.id, {c.enabled, c.confirmed});
    evaluate(false);
}

void PluginUpdates::setBusy(std::function<bool()> busy)
{
    m_busy = std::move(busy);
}

void PluginUpdates::setRunning(std::function<bool(const QString&)> running)
{
    m_running = std::move(running);
}

void PluginUpdates::setList(const QByteArray& bytes)
{
    if (m_trusted)
        return; // a checked list is better than a hint
    if (std::optional<gallery::Index> index = gallery::parse(bytes, m_options.source, nullptr))
        setIndex(std::move(*index), false);
}

int PluginUpdates::count() const
{
    return m_count;
}

QString PluginUpdates::stagedVersion(const QString& id) const
{
    return state().value(key("Staged", id)).toString();
}

QString PluginUpdates::problem(const QString& id) const
{
    return m_problems.value(id);
}

QStringList PluginUpdates::more(const QString& id) const
{
    return m_verdicts.value(id).decision.more;
}

bool PluginUpdates::claim(const QString& id)
{
    if (m_claims.contains(id))
        return false;
    m_claims.insert(id);
    return true;
}

void PluginUpdates::release(const QString& id)
{
    m_claims.remove(id);
}

bool PluginUpdates::claimed(const QString& id) const
{
    return m_claims.contains(id);
}

void PluginUpdates::clearStage(const QString& id)
{
    QSettings s = state();
    const QString dir = s.value(key("StagedDir", id)).toString();
    if (!dir.isEmpty() && QFileInfo::exists(dir) && !QFileInfo(dir).isJunction() && !QFileInfo(dir).isSymLink())
        QDir(dir).removeRecursively();
    if (!dir.isEmpty())
        QDir().rmdir(QFileInfo(dir).dir().path()); // the plugin's staging folder, if empty now
    for (const char* what : {"Staged", "StagedFrom", "StagedDir", "StagedTries"})
        s.remove(key(what, id));
    s.sync();
}

void PluginUpdates::forget(const QString& id)
{
    clearStage(id);
    QSettings s = state();
    for (const char* what : {"Failed", "FailedAt", "Widened", "Dismissed", "Notified"})
        s.remove(key(what, id));
    s.sync();
    m_problems.remove(id);
    evaluate(false);
}

void PluginUpdates::dismiss(const QString& id, const QString& version)
{
    QSettings s = state();
    s.setValue(key("Dismissed", id), version);
    s.sync();
    evaluate(false);
}

void PluginUpdates::recordFailure(const QString& id, const QString& version, const QString& why)
{
    QSettings s = state();
    s.setValue(key("Failed", id), version);
    s.setValue(key("FailedAt", id), m_options.now());
    s.sync();
    m_problems.insert(id, why);
    qWarning().noquote() << "Plugin updates:" << id << version << "not updated -" << why;
    evaluate(false);
}

void PluginUpdates::recordUpdated(const QString& id, const QString& version, const QString& from)
{
    QSettings s = state();
    s.setValue(key("Updated", id), version);
    s.setValue(key("UpdatedFrom", id), from);
    s.setValue(key("UpdatedAt", id), m_options.now());
    for (const char* what : {"Failed", "FailedAt", "Widened"})
        s.remove(key(what, id));
    s.sync();
    m_problems.remove(id);
    qInfo().noquote() << "Plugin updates: updated" << id << from << "->" << version;
    evaluate(false);
}

void PluginUpdates::rescan()
{
    catalog().scan();
}

void PluginUpdates::tick()
{
    applyStaged();
    if (!m_automatic || m_checking || m_working)
        return;
    if (m_busy && m_busy())
        return;
    if (!hasUserPlugins())
        return; // nothing of the user's to update: no network
    const QDateTime checked = state().value(key("Checked")).toDateTime();
    if (checked.isValid() && checked.secsTo(m_options.now()) < kDaySecs)
        return;
    startCheck();
}

void PluginUpdates::checkNow()
{
    startCheck();
}

void PluginUpdates::startCheck()
{
    if (m_checking)
        return;
    m_checking = true;
    QThreadPool::globalInstance()->start([self = QPointer(this), cancel = m_cancel, source = m_options.source, web = m_options.web,
                                             keys = m_options.keys] {
        auto list = std::make_shared<updates::SignedList>(updates::fetchSigned(source, web, keys));
        if (cancel->load())
            return;
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, list] {
            if (self)
                self->finishCheck(*list);
        }, Qt::QueuedConnection);
    });
}

void PluginUpdates::finishCheck(const updates::SignedList& fetched)
{
    m_checking = false;
    if (fetched.list.body.isEmpty()) {
        // Not a check: tried again at the next hourly beat.
        qInfo().noquote() << "Plugin updates: no list from" << m_options.source << "-" << fetched.why;
        return;
    }
    std::optional<gallery::Index> index = gallery::parse(fetched.list.body, m_options.source, nullptr);
    if (!index) {
        qWarning().noquote() << "Plugin updates: the list from" << m_options.source << "makes no sense";
        return;
    }
    {
        QSettings s = state();
        s.setValue(key("Checked"), m_options.now());
        s.sync();
    }
    qInfo().noquote() << "Plugin updates: list of" << index->items.size() << "from" << m_options.source
                      << (fetched.trusted ? u"signed by "_s + fetched.key : u"not trusted: "_s + fetched.why);
    if (m_options.source == gallery::kIndexUrl) { // kept for the hints, as the 插件库 tab keeps it
        QDir().mkpath(m_options.dataDir);
        QSaveFile file(m_options.dataDir + u"/plugin-index.json"_s);
        if (!file.open(QIODevice::WriteOnly) || file.write(fetched.list.body) != fetched.list.body.size() || !file.commit())
            qWarning().noquote() << "Plugin updates: the list could not be kept:" << file.errorString();
    }
    m_trusted = false; // setIndex() says
    setIndex(std::move(*index), fetched.trusted);
    startNext();
}

void PluginUpdates::setIndex(gallery::Index index, bool trusted)
{
    m_index = std::move(index);
    m_hasIndex = true;
    m_trusted = trusted;
    evaluate(trusted);
}

// Every plugin of the user's against the list: the counts, the queue of what
// may be installed (`installs`: only right after a checked list came), and the
// one notification a version that wants a yes is owed.
void PluginUpdates::evaluate(bool installs)
{
    m_verdicts.clear();
    if (m_hasIndex) {
        const QSettings s = state();
        const QDateTime now = m_options.now();
        for (const plugin::Entry& e : catalog().entries()) {
            if (e.source != plugin::Entry::Source::User)
                continue;
            const gallery::Item* item = m_index.find(e.id);
            if (!item)
                continue;
            const updates::Installed in = installedOf(e);
            Verdict v;
            v.decision = updates::decide(*item, in, policy(), historyOf(e.id), now);
            v.version = item->version;
            v.developerCopy = in.developerCopy;
            m_verdicts.insert(e.id, v);
            if (installs && v.decision.action == updates::Action::Install && s.value(key("Staged", e.id)).toString() != item->version
                && !m_queue.contains(e.id)) {
                m_queue.push_back(e.id);
                qInfo().noquote() << "Plugin updates:" << e.id << v.decision.why;
            }
        }
    }
    int count = 0;
    QStringList consent;
    QStringList signature;
    QSettings s = state();
    for (auto it = m_verdicts.cbegin(); it != m_verdicts.cend(); ++it) {
        const QString& id = it.key();
        const Verdict& v = it.value();
        const bool dismissed = s.value(key("Dismissed", id)).toString() == v.version;
        if (v.decision.action == updates::Action::Ask && !dismissed && s.value(key("Staged", id)).toString() != v.version) {
            ++count;
            signature.push_back(id + u'=' + v.version);
        }
        if (m_trusted && v.decision.action == updates::Action::Ask && v.decision.needsConsent && !dismissed && !v.developerCopy
            && s.value(key("Notified", id)).toString() != v.version)
            consent.push_back(id);
    }
    signature.sort();
    const QString joined = signature.join(u',');
    const bool differs = joined != m_signature || count != m_count;
    m_signature = joined;
    m_count = count;
    if (!consent.isEmpty()) {
        consent.sort();
        for (const QString& id : std::as_const(consent))
            s.setValue(key("Notified", id), m_verdicts.value(id).version);
        s.sync();
        qInfo().noquote() << "Plugin updates: new permissions to agree to:" << consent.join(u", "_s);
        emit consentNeeded(consent);
    }
    if (differs)
        emit changed();
}

void PluginUpdates::startNext()
{
    if (m_working)
        return;
    while (!m_queue.isEmpty()) {
        const QString id = m_queue.takeFirst();
        const gallery::Item* found = m_index.find(id);
        if (!found || m_verdicts.value(id).decision.action != updates::Action::Install)
            continue;
        if (claimed(id)) {
            qInfo().noquote() << "Plugin updates:" << id << "is being changed, later";
            continue;
        }
        m_working = true;
        const QString root = stagingRoot();
        // An elevated Win顺 updating a plugin it runs as administrator: the
        // checked bytes become its confirmed copy too (plugin::elevation).
        const bool elevated = m_host && m_host->hostElevated();
        const plugin::elevation::Store store = m_host ? m_host->elevationStore() : plugin::elevation::Store {};
        const QString pluginDir = QDir::toNativeSeparators(QDir(m_options.userDir).filePath(found->id));
        QThreadPool::globalInstance()->start([self = QPointer(this), cancel = m_cancel, item = *found, web = m_options.web, root, elevated,
                                                 store, pluginDir] {
            auto d = std::make_shared<Downloaded>();
            d->item = item;
            const gallery::Fetched f = gallery::fetch(
                item.url, item.size + 1, web, [cancel](qint64) { return !cancel->load(); },
                [&](const QByteArray& b) { return gallery::check(item, b) == gallery::Problem::None; });
            if (cancel->load())
                return;
            if (f.status != 200 || f.error != 0)
                d->error = u"download failed (%1, %2)"_s.arg(f.status).arg(f.error);
            else if (gallery::check(item, f.body) != gallery::Problem::None)
                d->error = u"the package does not match the list (size or SHA-256)"_s;
            else {
                if (QString why; elevated && !plugin::elevation::keepConfirmed(store, item.id, pluginDir, f.body, &why))
                    qWarning().noquote() << "Plugin updates:" << item.id << "confirmed copy not kept:" << why;
                QString saveError;
                const QString path = gallery::saveTemporary(item, f.body, &saveError);
                if (path.isEmpty())
                    d->error = saveError;
                else {
                    const QString parent = root + u'/' + item.id;
                    QDir().mkpath(parent);
                    d->candidate = plugin::install::inspect(path, parent);
                    QFile::remove(path);
                    if (!d->candidate)
                        d->error = u"the package cannot be read"_s;
                    else if (!d->candidate->manifest) {
                        d->error = d->candidate->error;
                        plugin::install::discard(*d->candidate);
                    }
                }
            }
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, d] {
                if (self)
                    self->finishDownload(*d);
                else if (d->candidate)
                    plugin::install::discard(*d->candidate);
            }, Qt::QueuedConnection);
        });
        return;
    }
}

void PluginUpdates::finishDownload(const Downloaded& downloaded)
{
    m_working = false;
    const gallery::Item& item = downloaded.item;
    const QString& id = item.id;
    std::optional<plugin::install::Candidate> candidate = downloaded.candidate;
    const auto drop = [&] {
        if (candidate)
            plugin::install::discard(*candidate);
    };
    const auto done = qScopeGuard([&] { startNext(); });

    if (!downloaded.error.isEmpty()) {
        drop();
        recordFailure(id, item.version, downloaded.error);
        return;
    }
    const plugin::Entry* e = catalog().find(id);
    if (!e || e->source != plugin::Entry::Source::User || !e->manifest || !candidate || !candidate->manifest || !m_automatic
        || m_developerMode || updates::developerCopy(e->dir)) {
        drop(); // it changed while the package came: not now
        qInfo().noquote() << "Plugin updates:" << id << "changed meanwhile, not updated now";
        return;
    }
    if (!claim(id)) {
        drop();
        qInfo().noquote() << "Plugin updates:" << id << "is being changed, later";
        return;
    }
    const auto releaseIt = qScopeGuard([&] { release(id); });
    const QString from = e->manifest->version;
    const auto [enabled, confirmed] = m_switches.value(id);
    if (const QString why = updates::review(*e->manifest, *candidate->manifest, id, item.version, m_options.appVersion, confirmed, enabled);
        !why.isEmpty()) {
        const bool wider = candidate->manifest->id == id && candidate->manifest->version == item.version
            && !updates::widening(*e->manifest, *candidate->manifest, confirmed, enabled).isEmpty();
        drop();
        if (wider) { // asks for more than the list said: not again until a newer one
            QSettings s = state();
            s.setValue(key("Widened", id), item.version);
            s.sync();
            m_problems.insert(id, why);
            qWarning().noquote() << "Plugin updates:" << id << item.version << "package" << why;
            evaluate(false);
        } else
            recordFailure(id, item.version, why);
        return;
    }
    if (isRunning(id)) { // for the next start
        QSettings s = state();
        if (const QString error = updates::stage(*candidate, from, s); !error.isEmpty()) {
            drop();
            recordFailure(id, item.version, error);
            return;
        }
        qInfo().noquote() << "Plugin updates:" << id << item.version << "is running, kept for the next start";
        evaluate(false);
        emit changed();
        return;
    }
    QString placeError;
    if (plugin::install::place(*candidate, m_options.userDir, &placeError).isEmpty()) {
        drop();
        recordFailure(id, item.version, placeError);
        return;
    }
    drop(); // the folder moved in: nothing left to remove
    clearStage(id);
    catalog().scan();
    recordUpdated(id, item.version, from);
}

// The versions kept for the next start, of plugins that are not running now.
void PluginUpdates::applyStaged()
{
    QStringList ids;
    {
        QSettings s = state();
        s.beginGroup(key("Staged"));
        ids = s.childKeys();
    }
    for (const QString& id : ids) {
        if (claimed(id) || isRunning(id))
            continue;
        const plugin::Entry* e = catalog().find(id);
        const QSettings s = state();
        const QString version = s.value(key("Staged", id)).toString();
        const QString from = s.value(key("StagedFrom", id)).toString();
        const QString dir = s.value(key("StagedDir", id)).toString();
        if (!e || e->source != plugin::Entry::Source::User || !e->manifest || plugin::compareVersions(e->manifest->version, from) != 0) {
            qInfo().noquote() << "Plugin updates: dropped the staged" << id << version << "- the plugin is not as it was";
            clearStage(id);
            continue;
        }
        if (m_developerMode || updates::developerCopy(e->dir))
            continue;
        if (!claim(id))
            continue;
        const auto releaseIt = qScopeGuard([&] { release(id); });
        std::optional<plugin::install::Candidate> candidate = plugin::install::adopt(dir);
        const auto [enabled, confirmed] = m_switches.value(id);
        QString why;
        if (!candidate || !candidate->manifest)
            why = u"the staged folder is gone"_s;
        else
            why = updates::review(*e->manifest, *candidate->manifest, id, version, m_options.appVersion, confirmed, enabled);
        if (!why.isEmpty()) {
            qInfo().noquote() << "Plugin updates: dropped the staged" << id << version << "-" << why;
            clearStage(id);
            continue;
        }
        QString placeError;
        if (plugin::install::place(*candidate, m_options.userDir, &placeError).isEmpty()) {
            qWarning().noquote() << "Plugin updates: could not apply the staged" << id << version << "-" << placeError;
            continue; // the next beat, or the next start
        }
        clearStage(id);
        catalog().scan();
        recordUpdated(id, version, from);
    }
}

QString PluginUpdates::applyNow(const QString& id)
{
    const QSettings s = state();
    const QString version = s.value(key("Staged", id)).toString();
    const QString dir = s.value(key("StagedDir", id)).toString();
    if (version.isEmpty())
        return tr("没有等着更新的版本");
    const plugin::Entry* e = catalog().find(id);
    if (!e || e->source != plugin::Entry::Source::User || !e->manifest) {
        clearStage(id);
        return tr("这个插件已经不在了");
    }
    if (!claim(id))
        return tr("正在更新这个插件，稍后再试");
    const auto releaseIt = qScopeGuard([&] { release(id); });
    const QString from = e->manifest->version;
    std::optional<plugin::install::Candidate> candidate = plugin::install::adopt(dir);
    const auto [enabled, confirmed] = m_switches.value(id);
    QString why;
    if (!candidate || !candidate->manifest)
        why = u"the staged folder is gone"_s;
    else
        why = updates::review(*e->manifest, *candidate->manifest, id, version, m_options.appVersion, confirmed, enabled);
    if (why.isEmpty()) {
        if (m_host) {
            why = updates::replace(*m_host, *candidate, m_options.userDir); // the program is asked to leave first
        } else {
            plugin::install::place(*candidate, m_options.userDir, &why);
            plugin::install::discard(*candidate);
        }
    }
    clearStage(id);
    catalog().scan();
    if (!why.isEmpty()) {
        recordFailure(id, version, why);
        return why;
    }
    recordUpdated(id, version, from);
    return {};
}

} // namespace ws
