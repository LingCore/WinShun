#pragma once

#include "SearchTypes.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <chrono>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace ws {

class History;
class NameMatcher;
struct ParsedQuery;

// An installed app as the Start menu's "All apps" lists it: classic desktop
// programs (through their Start-menu shortcuts) and packaged apps (Microsoft
// Store, MSIX, the ones built into Windows) alike.
struct AppInfo {
    QString name; // display name, localized ("计算器")
    QString id; // its name in shell:AppsFolder: an AppUserModelID, or a path
    QString target; // desktop: the program or document it starts; packaged: the install folder; may be empty
    AppKind kind = AppKind::Desktop;
    bool elevatable = false; // can run as administrator (desktop programs, full-trust packages)

    // Filled by prepare() from the fields above.
    // A Windows app's names in the other language, matched as well: "计算器"
    // where Windows shows "Calculator", and the other way round.
    QStringList otherNames;
    std::vector<std::string> otherNamesUtf8;
    bool auxiliary = false; // an uninstaller or a document (manual, readme): ranked lower
    std::string nameUtf8;
    std::string program; // UTF-8 file name of the desktop program without extension ("WINWORD"), matched too
    std::string initials; // folded first letters of the name's words ("vsc" for Visual Studio Code)

    void prepare();
    QString launchPath() const;
};

using AppList = std::vector<AppInfo>;

// "shell:AppsFolder\<id>": opens the app, and stands for it in History.
QString appLaunchPath(const QString& id);
bool isAppLaunchPath(QStringView path) noexcept;
QString appIdOf(QStringView launchPath); // empty for any other path

struct AppHit {
    std::size_t index = 0; // into the AppList
    int score = 0; // higher is better; may be negative for weak matches
    int otherName = -1; // found by otherNames[otherName], not by its name: shown by that one
};

// The apps matching `query`, best first. Recently opened apps (their launch
// paths in `history`, newest first) rank higher.
std::vector<AppHit> searchApps(const AppList& apps, const ParsedQuery& query, const NameMatcher& matcher,
    const QStringList& history);

// Something Windows saw started from the shell (Start menu, taskbar,
// Explorer): its UserAssist record.
struct AppUse {
    QString name; // a program, "{known folder id}\relative path" (as AppsFolder ids are) or a full path; or an AppUserModelID
    double uses = 0; // times started, plus a tenth of the times it had the focus
    qint64 last = 0; // ms since the epoch, last started
};
// Windows' record of the programs and apps started (UserAssist, under
// HKCU). On Windows 11 the run counts are mostly 0 and the times kept.
std::vector<AppUse> windowsAppUses();

// Up to `limit` apps the user opens most, best first (indexes into `apps`):
// those opened from Win顺 (`history`, by how often and how lately), then
// those Windows saw started in the last 60 days (`windows`). Uninstallers
// and documents, and Win顺 itself, are left out.
std::vector<std::size_t> frequentApps(const AppList& apps, const History& history, const std::vector<AppUse>& windows,
    qint64 now, std::size_t limit);

// The list of installed apps, read from the shell in the background and
// re-read when the Start menu or the installed packages change.
class AppCatalog : public QObject {
    Q_OBJECT

public:
    explicit AppCatalog(QObject* parent = nullptr);
    ~AppCatalog() override;

    // Sorted by name; empty until the first load finishes. Thread-safe.
    std::shared_ptr<const AppList> apps() const;
    bool isLoaded() const;

    // Re-reads the list in the background if the Start-menu folders or the
    // installed packages changed since the last read (a check of a few
    // milliseconds), or regardless with `force`. Call from the owner thread.
    void refresh(bool force = false);

signals:
    void changed(); // the list differs from before

private:
    struct Outcome {
        quint64 fingerprint = 0;
        bool reloaded = false; // the list was read again
        std::shared_ptr<const AppList> apps; // null: not read, or the read failed
    };

    void publish(const Outcome& outcome);

    mutable std::mutex m_mutex;
    std::shared_ptr<const AppList> m_apps; // guarded by m_mutex

    // Owner thread only.
    quint64 m_fingerprint = 0;
    std::chrono::steady_clock::time_point m_loadedAt;
    bool m_busy = false;
    bool m_again = false; // refresh(true) arrived while busy
    std::jthread m_worker; // last: stopped and joined before the members above go
};

// Reads shell:AppsFolder (COM; some 0.4 s for 350 apps). Empty on failure.
AppList loadInstalledApps(const std::stop_token& stop);

// Hash of the Start-menu folders' write times and the per-user package
// repository's: changes whenever an app is installed or removed.
quint64 installedAppsFingerprint();

} // namespace ws
