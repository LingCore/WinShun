#include "Updates.h"

#include "Host.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QUrl>

#include <windows.h>

using namespace Qt::StringLiterals;

namespace ws::plugin::updates {

namespace {

bool isLink(const QString& path)
{
    const QFileInfo info(path);
    return info.isJunction() || info.isSymLink();
}

bool containsNoCase(const QStringList& list, const QString& item) { return list.contains(item, Qt::CaseInsensitive); }

// The first of `have` that `allowed` does not have, or empty.
QString firstNotIn(const QStringList& have, const QStringList& allowed)
{
    for (const QString& item : have) {
        if (!containsNoCase(allowed, item))
            return item;
    }
    return {};
}

QStringList moreThan(const QStringList& names, const QStringList& known)
{
    QStringList out;
    for (const QString& name : names) {
        if (!containsNoCase(known, name) && !containsNoCase(out, name))
            out.push_back(name);
    }
    return out;
}

bool isWebSource(const QString& source)
{
    const QString scheme = QUrl(source).scheme();
    return scheme == u"https" || scheme == u"http";
}

bool fetchedOk(const gallery::Fetched& f, qint64 maxBytes) { return f.status == 200 && f.error == 0 && f.body.size() <= maxBytes; }

QString key(const char* what, const QString& id) { return u"PluginUpdates/"_s + QLatin1StringView(what) + u'/' + id; }

void removeStage(QSettings& state, const QString& id)
{
    for (const char* what : {"Staged", "StagedFrom", "StagedDir", "StagedTries"})
        state.remove(key(what, id));
}

// A folder of ours to remove. Never a link: that would reach what it points to.
void removeFolder(const QString& dir)
{
    if (!dir.isEmpty() && QFileInfo::exists(dir) && !isLink(dir))
        QDir(dir).removeRecursively();
}

// What an interrupted install::place() left: <userDir>\.winshun-update-XXXXXX\plugin
// is the old version put aside. If the plugin's own folder is missing (or
// half there), the old one goes back; else it is the leftover of a swap that
// finished, and goes.
void repairInterruptedSwaps(const QString& userDir)
{
    const QDir user(userDir);
    for (const QString& name : user.entryList({u".winshun-update-*"_s}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) {
        const QString holder = user.filePath(name);
        const QString aside = holder + u"/plugin"_s;
        if (!QFileInfo::exists(aside)) {
            removeFolder(holder); // nothing aside: an empty holder
            continue;
        }
        if (isLink(aside)) {
            qWarning().noquote() << "Plugin updates: leaving" << aside << "alone, it is a link";
            continue;
        }
        QString error;
        const std::optional<Manifest> old = Manifest::load(aside + u"/plugin.json"_s, &error);
        if (!old) {
            removeFolder(holder);
            continue;
        }
        const QString target = user.filePath(old->id);
        if (QFileInfo::exists(target + u"/plugin.json"_s)) {
            removeFolder(holder); // the new one is whole: the old is the leftover
            continue;
        }
        if (isLink(target)) {
            qWarning().noquote() << "Plugin updates: leaving" << target << "alone, it is a link";
            continue;
        }
        removeFolder(target); // half of the new one
        if (QDir().rename(aside, target)) {
            qInfo().noquote() << "Plugin updates: put back" << old->id << old->version << "after an interrupted update";
            removeFolder(holder);
        } else
            qWarning().noquote() << "Plugin updates: could not put back" << aside;
    }
}

} // namespace

Decision decide(const gallery::Item& item, const Installed& installed, const Policy& policy, const History& history, const QDateTime& now)
{
    Decision d;
    if (!installed.local.userCopy) {
        d.why = u"not installed by the user"_s;
        return d;
    }
    if (item.bundled || installed.local.bundled) {
        d.why = u"comes with WinShun"_s;
        return d;
    }
    if (!installed.manifest) {
        d.action = Action::Ask;
        d.why = u"the installed manifest cannot be read"_s;
        return d;
    }
    if (compareVersions(installed.manifest->version, item.version) >= 0) {
        d.why = u"up to date"_s;
        return d;
    }
    if (!item.minWinShun.isEmpty() && compareVersions(item.minWinShun, policy.appVersion) > 0) {
        d.action = Action::NeedsNewerApp;
        d.why = u"needs WinShun %1"_s.arg(item.minWinShun);
        return d;
    }
    d.more = moreThan(item.permissions, installed.manifest->permissions.names());
    if (item.needsReviewConsent()) {
        d.action = Action::Ask;
        d.needsConsent = installed.enabled && !moreThan(item.permissions, installed.confirmed).isEmpty();
        d.why = u"unreviewed native package requires confirmation for each version"_s;
        return d;
    }
    if (!d.more.isEmpty()) {
        d.action = Action::Ask;
        d.needsConsent = installed.enabled && !moreThan(item.permissions, installed.confirmed).isEmpty();
        d.why = u"asks for more: %1"_s.arg(d.more.join(u", "_s));
        return d;
    }
    if (!policy.automatic || policy.developerMode || installed.developerCopy) {
        d.action = Action::Ask;
        d.why = !policy.automatic ? u"automatic updates are off"_s : policy.developerMode ? u"developer mode"_s : u"a copy in development"_s;
        return d;
    }
    if (!policy.trusted) {
        d.action = Action::Ask;
        d.why = u"the list is not signed"_s;
        return d;
    }
    if (history.widened == item.version) {
        d.action = Action::Ask;
        d.why = u"its package asked for more than the list said"_s;
        return d;
    }
    if (history.failedVersion == item.version && history.failedAt.isValid() && history.failedAt.secsTo(now) < 24 * 3600) {
        d.action = Action::Ask;
        d.why = u"failed within 24 hours"_s;
        return d;
    }
    d.action = Action::Install;
    d.why = u"%1 -> %2"_s.arg(installed.manifest->version, item.version);
    return d;
}

QStringList permissionsToAllow(const Manifest& candidate, bool enabled, const QStringList& confirmed)
{
    if (!enabled || confirmed.isEmpty())
        return {};
    return install::unconfirmed(candidate, confirmed);
}

QStringList confirmedFor(const Manifest& manifest)
{
    QStringList names = manifest.permissions.names();
    if (names.isEmpty())
        names = {QStringLiteral("none")};
    return names;
}

QString widening(const Manifest& installed, const Manifest& candidate, const QStringList& confirmed, bool enabled)
{
    const Permissions& was = installed.permissions;
    const Permissions& now = candidate.permissions;
    if (const QStringList more = moreThan(now.names(), was.names()); !more.isEmpty() || (now.flags & ~was.flags) != 0)
        return u"要更多权限：%1"_s.arg(more.join(u"、"_s));
    if (const QString path = firstNotIn(now.readPaths, was.readPaths); !path.isEmpty())
        return u"要读更多的路径：%1"_s.arg(path);
    if (const QString port = firstNotIn(now.listens, was.listens); !port.isEmpty())
        return u"要开更多的端口：%1"_s.arg(port);
    if (candidate.runtime && !installed.runtime)
        return u"多了要运行的程序"_s;
    if (candidate.runtime && installed.runtime && installed.runtime->sandbox == SandboxLevel::Strict
        && candidate.runtime->sandbox == SandboxLevel::Normal)
        return u"沙箱放宽了"_s;
    if (enabled) {
        const QStringList before = install::unconfirmed(installed, confirmed);
        if (const QString name = firstNotIn(install::unconfirmed(candidate, confirmed), before); !name.isEmpty())
            return u"要你还没同意过的权限：%1"_s.arg(name);
    }
    return {};
}

QString review(const Manifest& installed, const Manifest& candidate, QStringView id, QStringView version, QStringView appVersion,
    const QStringList& confirmed, bool enabled)
{
    if (candidate.id != id)
        return u"包里的插件是 %1，不是 %2"_s.arg(candidate.id, id);
    if (candidate.version != version)
        return u"包里的版本是 %1，列表说是 %2"_s.arg(candidate.version, version);
    if (compareVersions(candidate.version, installed.version) <= 0)
        return u"包的版本 %1 不比已装的 %2 新"_s.arg(candidate.version, installed.version);
    if (!candidate.minWinShun.isEmpty() && compareVersions(candidate.minWinShun, appVersion) > 0)
        return u"要 Win顺 %1 或更新"_s.arg(candidate.minWinShun);
    return widening(installed, candidate, confirmed, enabled);
}

bool developerCopy(const QString& pluginDir)
{
    return isLink(pluginDir) || QFileInfo::exists(QDir(pluginDir).filePath(u".git"_s));
}

SignedList fetchSigned(const QString& source, const gallery::Fetcher& web, const QList<signing::PublicKey>& keys)
{
    SignedList out;
    const QString sigSource = source + u".sig"_s;
    const auto isList = [&](const QByteArray& b) { return b.size() <= gallery::kMaxIndexBytes && gallery::parse(b, source, nullptr).has_value(); };
    const auto isSignature = [](const QByteArray& b) { return signing::parseFile(b, nullptr).has_value(); };

    out.list = gallery::fetch(source, gallery::kMaxIndexBytes + 1, web, {}, isList);
    if (!fetchedOk(out.list, gallery::kMaxIndexBytes)) {
        out.list.body.clear();
        out.why = u"the list could not be fetched"_s;
        return out;
    }
    const gallery::Fetched sig = gallery::fetch(sigSource, signing::kMaxSignatureBytes + 1, web, {}, isSignature);

    if (!isWebSource(source)) {
        if (sig.status != 200) {
            if (sig.error == ERROR_FILE_NOT_FOUND) {
                out.trusted = true;
                out.key = u"local"_s;
                out.why = u"unsigned local index trusted (WINSHUN_PLUGIN_INDEX)"_s;
            } else
                out.why = u"the signature file could not be read"_s;
            return out;
        }
        const signing::Verdict verdict = signing::verify(out.list.body, sig.body, keys);
        out.trusted = verdict.ok;
        out.key = verdict.key;
        out.why = verdict.why;
        return out;
    }

    signing::Verdict verdict {false, {}, u"no signature file"_s};
    if (fetchedOk(sig, signing::kMaxSignatureBytes))
        verdict = signing::verify(out.list.body, sig.body, keys);
    if (verdict.ok) {
        out.trusted = true;
        out.key = verdict.key;
        return out;
    }
    // The pair may come from two moments (a mirror's cache, an upload in
    // between): both once more, from the origin itself.
    if (web) {
        const gallery::Fetched list = web(source, gallery::kMaxIndexBytes + 1, {});
        const gallery::Fetched signature = web(sigSource, signing::kMaxSignatureBytes + 1, {});
        if (fetchedOk(list, gallery::kMaxIndexBytes) && isList(list.body) && fetchedOk(signature, signing::kMaxSignatureBytes)) {
            const signing::Verdict again = signing::verify(list.body, signature.body, keys);
            if (again.ok) {
                out.list = list;
                out.trusted = true;
                out.key = again.key;
                return out;
            }
            verdict = again;
        }
    }
    out.why = verdict.why;
    return out;
}

QString replace(Host& host, install::Candidate& candidate, const QString& userDir, int waitMs)
{
    if (!candidate.manifest)
        return candidate.error.isEmpty() ? u"不是可用的插件"_s : candidate.error;
    const QString id = candidate.manifest->id;
    host.stop(id); // its program, if running, lets go of its folder
    // A moment for the process to leave (shutdown is asked for, then the job ends it).
    const QDeadlineTimer deadline(waitMs);
    while (!deadline.hasExpired()) {
        const Host::State::Status s = host.state(id).status;
        if (s != Host::State::Status::Running && s != Host::State::Status::Starting)
            break;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    QString error;
    const QString placed = install::place(candidate, userDir, &error);
    install::discard(candidate);
    return placed.isEmpty() ? error : QString();
}

QString stage(install::Candidate& candidate, const QString& fromVersion, QSettings& state)
{
    if (!candidate.manifest || !candidate.archive || candidate.dir.isEmpty())
        return u"没有可暂存的插件"_s;
    const QString id = candidate.manifest->id;
    const QString before = state.value(key("StagedDir", id)).toString();
    if (!before.isEmpty() && QDir::cleanPath(before).compare(QDir::cleanPath(candidate.dir), Qt::CaseInsensitive) != 0)
        removeFolder(before);
    state.setValue(key("Staged", id), candidate.manifest->version);
    state.setValue(key("StagedFrom", id), fromVersion);
    state.setValue(key("StagedDir", id), QDir::toNativeSeparators(candidate.dir));
    state.remove(key("StagedTries", id));
    state.sync();
    candidate.dir.clear(); // the staging record owns the folder now
    return {};
}

QList<Applied> prepare(const QString& stagingRoot, const QString& userDir, QSettings& state, QStringView appVersion,
    const std::function<std::pair<bool, QStringList>(const QString& id)>& switchOf)
{
    QList<Applied> applied;
    repairInterruptedSwaps(userDir);

    state.beginGroup(u"PluginUpdates/Staged"_s);
    const QStringList ids = state.childKeys();
    state.endGroup();
    for (const QString& id : ids) {
        Applied a;
        a.id = id;
        a.version = state.value(key("Staged", id)).toString();
        a.from = state.value(key("StagedFrom", id)).toString();
        const QString dir = state.value(key("StagedDir", id)).toString();
        const auto drop = [&](const QString& why) {
            qInfo().noquote() << "Plugin updates: dropped the staged" << id << a.version << "-" << why;
            removeFolder(dir);
            removeStage(state, id);
            a.error = why;
            a.dropped = true;
        };
        QString error;
        const std::optional<Manifest> installed = Manifest::load(QDir(userDir).filePath(id + u"/plugin.json"_s), &error);
        std::optional<install::Candidate> candidate;
        if (!validPluginId(id) || dir.isEmpty())
            drop(u"the record is no good"_s);
        else if (!installed)
            drop(u"the plugin is no longer there"_s);
        else if (isLink(QDir(userDir).filePath(id)))
            drop(u"the plugin is a link"_s);
        else if (compareVersions(installed->version, a.from) != 0)
            drop(u"the installed version is %1, not %2"_s.arg(installed->version, a.from));
        else if (!(candidate = install::adopt(dir)) || !candidate->manifest)
            drop(candidate ? candidate->error : u"the staged folder is gone"_s);
        else {
            const auto [enabled, confirmed] = switchOf(id);
            if (const QString why = review(*installed, *candidate->manifest, id, a.version, appVersion, confirmed, enabled); !why.isEmpty()) {
                drop(why);
            } else if (QString placeError; install::place(*candidate, userDir, &placeError).isEmpty()) {
                // Locked (its program still running?): once more at the next start.
                const int tries = state.value(key("StagedTries", id), 0).toInt() + 1;
                if (tries >= 2)
                    drop(placeError);
                else {
                    state.setValue(key("StagedTries", id), tries);
                    a.error = placeError;
                    qWarning().noquote() << "Plugin updates: could not apply the staged" << id << a.version << "-" << placeError;
                }
            } else {
                install::discard(*candidate);
                removeStage(state, id);
                state.setValue(key("Updated", id), a.version);
                state.setValue(key("UpdatedFrom", id), a.from);
                state.setValue(key("UpdatedAt", id), QDateTime::currentDateTime());
                qInfo().noquote() << "Plugin updates: applied the staged" << id << a.from << "->" << a.version;
            }
        }
        applied.push_back(a);
    }
    state.sync();

    // Whatever the records do not name goes.
    QSet<QString> kept;
    state.beginGroup(u"PluginUpdates/StagedDir"_s);
    for (const QString& id : state.childKeys())
        kept.insert(QDir::cleanPath(state.value(id).toString()).toLower());
    state.endGroup();
    const QDir root(stagingRoot);
    for (const QString& idDir : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QDir inner(root.filePath(idDir));
        for (const QString& name : inner.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (!kept.contains(QDir::cleanPath(inner.filePath(name)).toLower()))
                removeFolder(inner.filePath(name));
        }
        for (const QString& file : inner.entryList(QDir::Files))
            QFile::remove(inner.filePath(file));
        if (inner.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot))
            QDir().rmdir(inner.path());
    }
    return applied;
}

} // namespace ws::plugin::updates
