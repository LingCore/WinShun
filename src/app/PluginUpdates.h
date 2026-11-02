#pragma once

// The plugins the user has installed, kept up to date in the background (the
// plugin updates design, §4.2): the plugin library's list is fetched with its
// signature now and then (once a day, when the 自动更新 switch is on), each
// installed plugin judged against it (plugin::updates::decide), and what may
// be put in place without asking is downloaded, checked again and swapped in
// -- at once when the plugin is not running, else kept aside for the next start
// (plugin::updates::stage, prepare). It never stops a running plugin.
// What needs the user (a newer version that asks for more, an unsigned list,
// a failure) is counted for the badge and the tray line, and a version that
// needs a yes to new permissions is told once by a signal.

#include "Download.h"
#include "Gallery.h"
#include "IndexSignature.h"
#include "Updates.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace ws::plugin {
class Catalog;
class Host;
struct Entry;
}

namespace ws {

struct PluginConfig;

class PluginUpdates : public QObject {
    Q_OBJECT

public:
    struct Options {
        QString dataDir, userDir, bundledDir;
        QString source; // WINSHUN_PLUGIN_INDEX or gallery::kIndexUrl
        plugin::gallery::Fetcher web; // empty: WinHTTP
        QList<plugin::signing::PublicKey> keys = plugin::signing::indexKeys();
        std::function<QDateTime()> now = [] { return QDateTime::currentDateTime(); };
        QString appVersion; // empty: this build's
    };
    // `host` may be null (tests): the catalog is then read here, and nothing is stopped.
    PluginUpdates(plugin::Host* host, Options options, QObject* parent = nullptr);
    ~PluginUpdates() override;

    static QString defaultSource(); // WINSHUN_PLUGIN_INDEX, or the published list
    // At start, before the Host is made (GUI thread): see plugin::updates::prepare.
    static QList<plugin::updates::Applied> prepare(const QString& dataDir, const QString& userDir, QStringView appVersion,
        const std::function<std::pair<bool, QStringList>(const QString&)>& switchOf);

    void setAutomatic(bool on); // the 自动更新 switch: the timer, and whether installs are allowed
    void setPolicy(bool developerMode, const QList<PluginConfig>& plugins); // which are on, and what they were allowed
    void setBusy(std::function<bool()> busy); // true: not now (recovery running, Win顺 updating itself)
    void setRunning(std::function<bool(const QString&)> running); // tests; default: the host's state
    void setList(const QByteArray& bytes); // the 插件库 tab's list: hints only, never installed from

    int count() const; // badge and tray line
    bool idle() const { return !m_checking && !m_working && m_queue.isEmpty(); } // no check or download under way
    QString stagedVersion(const QString& id) const;
    QString problem(const QString& id) const;
    QStringList more(const QString& id) const; // permissions the listed version adds
    bool claim(const QString& id); // false: someone else is changing this plugin
    void release(const QString& id);
    bool claimed(const QString& id) const;
    void forget(const QString& id); // after an uninstall or a manual install: what is remembered of the plugin goes
    void dismiss(const QString& id, const QString& version); // the user said no to this version
    QString applyNow(const QString& id); // a staged version, now: its program is stopped. Empty or why not
    void checkNow(); // fetches the list (whatever the clock says)
    void rescan(); // the plugins folder was changed by someone else (tests; the app's Host scans itself)
    void tick(); // the hourly beat: staged versions of plugins that stopped; the daily check when due

signals:
    void changed(); // count() or what the rows show
    void consentNeeded(const QStringList& ids); // enabled plugins whose newer version asks for permissions, once per version

private:
    struct Downloaded;
    struct Verdict {
        plugin::updates::Decision decision;
        QString version; // the listed one
        bool developerCopy = false;
    };
    plugin::Catalog& catalog() const;
    bool isRunning(const QString& id) const;
    plugin::updates::Installed installedOf(const plugin::Entry& entry) const;
    plugin::updates::Policy policy() const;
    plugin::updates::History historyOf(const QString& id) const;
    void startCheck();
    void finishCheck(const plugin::updates::SignedList& fetched);
    void setIndex(plugin::gallery::Index index, bool trusted);
    void evaluate(bool installs);
    void startNext();
    void finishDownload(const Downloaded& d);
    void applyStaged();
    void recordFailure(const QString& id, const QString& version, const QString& why);
    void recordUpdated(const QString& id, const QString& version, const QString& from);
    void clearStage(const QString& id);
    QSettings state() const;
    QString stagingRoot() const;
    bool hasUserPlugins() const;

    plugin::Host* m_host;
    Options m_options;
    std::unique_ptr<plugin::Catalog> m_ownCatalog; // without a host
    QTimer m_hourly;
    QTimer m_first;
    bool m_automatic = false;
    bool m_developerMode = false;
    QHash<QString, std::pair<bool, QStringList>> m_switches; // id -> enabled, confirmed
    std::function<bool()> m_busy;
    std::function<bool(const QString&)> m_running;
    plugin::gallery::Index m_index;
    bool m_hasIndex = false;
    bool m_trusted = false; // the list now held checked against a key
    QHash<QString, Verdict> m_verdicts;
    QHash<QString, QString> m_problems; // id -> why its last try failed (this run)
    QString m_signature; // of the verdicts shown, to tell when they changed
    int m_count = 0;
    QSet<QString> m_claims;
    QStringList m_queue;
    bool m_checking = false;
    bool m_working = false;
    std::shared_ptr<std::atomic_bool> m_cancel = std::make_shared<std::atomic_bool>(false);
};

} // namespace ws
