#pragma once

// Updating the plugins the user has installed (the plugin updates design):
// what the plugin library's list says to do about each one (decide), whether
// the package that came is not asking for more than the one installed
// (review, widening), the list fetched together with its signature
// (fetchSigned; IndexSignature.h), the swap of a folder for a newer one
// (replace), and the new version kept aside for the next start when the
// plugin is running (stage, prepare). No window and no timer here: the
// service that drives these lives in the app.

#include "Gallery.h"
#include "IndexSignature.h"
#include "Install.h"

#include <QDateTime>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <functional>
#include <utility>

namespace ws::plugin {
class Host;
}

namespace ws::plugin::updates {

enum class Action {
    None, // nothing to do (also: not installed, comes with Win顺, as new as the list's)
    Install, // may be put in place without asking
    Ask, // a newer version the user is to be told about
    NeedsNewerApp, // asks for a newer Win顺 than this one
};

struct Policy {
    bool automatic = false; // the 自动更新 switch
    bool developerMode = false;
    bool trusted = false; // the list's signature checked
    QString appVersion;
};

struct Installed {
    gallery::Local local;
    const Manifest* manifest = nullptr; // the user's copy; nothing when it cannot be read
    bool developerCopy = false; // developerCopy(): a link or a repository
    bool enabled = false; // switched on
    QStringList confirmed; // the permissions the user said yes to
};

// What happened to this plugin before.
struct History {
    QString widened; // the version whose package asked for more than the list said
    QString dismissed; // the version the user said no to (decide() ignores it: it only keeps the version out of the count)
    QString failedVersion; // the version that failed to download, check, unpack or place
    QDateTime failedAt;
};

struct Decision {
    Action action = Action::None;
    QStringList more; // the permissions the list names that the installed one does not
    bool needsConsent = false; // more, and the plugin is on, and the user has not said yes to some of it
    QString why; // for the log
};

// The list's `item` against what is installed (never to a lower version).
Decision decide(const gallery::Item& item, const Installed& installed, const Policy& policy, const History& history, const QDateTime& now);

// Why `candidate` may not replace `installed`, or empty: it asks for more
// (permissions, the paths it reads, the ports it opens, a sandbox loosened, a
// program where there was none), or, when it is on, for permissions the
// installed one did not need the user's yes for.
QString widening(const Manifest& installed, const Manifest& candidate, const QStringList& confirmed, bool enabled);
// The whole check of a package before it replaces the installed copy: it is
// the plugin and version the list named, newer, for this Win顺, and
// widening() finds nothing. Why not, or empty.
QString review(const Manifest& installed, const Manifest& candidate, QStringView id, QStringView version, QStringView appVersion,
    const QStringList& confirmed, bool enabled);

// A newer version of a plugin that is on, naming permissions the user has not
// said yes to: those names (empty: nothing to ask, the plugin is off, or it
// was never confirmed -- turning it on asks then). The 更新并允许 question.
QStringList permissionsToAllow(const Manifest& candidate, bool enabled, const QStringList& confirmed);
// Confirmed= once the user said yes to `manifest`: its permission names ("none" for no permission at all).
QStringList confirmedFor(const Manifest& manifest);

// A copy under development: a link, or a folder with a repository in it.
bool developerCopy(const QString& pluginDir);

// The list fetched with its signature (blocking: off the GUI thread).
struct SignedList {
    gallery::Fetched list; // the list's bytes as fetched; empty body when it could not be had
    bool trusted = false; // the signature checks (or: a local file with no signature)
    QString key; // the key that checked; "local" for a local file with no signature
    QString why; // when not trusted: why (for the log)
};
// `source`: the list's address or file; its signature is `source` + ".sig".
// Mirrors first; when the pair does not check, both come again from the
// origin once (a mirror's cache, or an upload in between). A local file with
// no .sig beside it is trusted (WINSHUN_PLUGIN_INDEX: development).
SignedList fetchSigned(const QString& source, const gallery::Fetcher& web, const QList<signing::PublicKey>& keys);

// Puts `candidate` in the place of the plugin of its id in `userDir`: its
// program is asked to leave (up to `waitMs`, the event loop running), then
// install::place(), then the candidate's folder is discarded. Empty on
// success, else why not (what was there is left as it was). The caller holds
// the plugin's claim: this runs an event loop. `asideForeignTo`: as
// install::place()'s, for a folder there that is no installed plugin.
QString replace(Host& host, install::Candidate& candidate, const QString& userDir, int waitMs = 2500, const QString& asideForeignTo = {});

// The candidate (unpacked into its own folder under the staging root,
// inspect(path, <root>\<id>)) is kept for the next start: `fromVersion` is
// the version it was meant to replace. update.ini's [PluginUpdates] gets
// Staged, StagedFrom and StagedDir of the id; an earlier stage of the id is
// removed. The candidate no longer owns its folder afterwards. Empty or why not.
QString stage(install::Candidate& candidate, const QString& fromVersion, QSettings& state);

struct Applied {
    QString id, version, from, error;
    bool dropped = false; // the stage is gone, not applied
};
// At start, before the Host is made: puts back what an interrupted swap left
// aside, applies what was staged (checking it again against the plugin as it
// is now), and clears what nothing records out of `stagingRoot`.
// `switchOf(id)`: whether the plugin is on, and what the user said yes to.
QList<Applied> prepare(const QString& stagingRoot, const QString& userDir, QSettings& state, QStringView appVersion,
    const std::function<std::pair<bool, QStringList>(const QString& id)>& switchOf);

} // namespace ws::plugin::updates
