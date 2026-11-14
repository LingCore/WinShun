#pragma once

#include "SearchTypes.h"
#include "History.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ws {

class AppAliases;
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
    QString arguments; // desktop: what the shortcut passes its program ("/k vcvars64.bat"); mostly empty
    AppKind kind = AppKind::Desktop;
    bool elevatable = false; // can run as administrator (desktop programs, full-trust packages)

    // Filled by prepare() from the fields above.
    // From Win顺's list of apps (AppAliases), found by the app's id, package,
    // program or name: its names in other languages, matched and shown when
    // found by one ("计算器" where Windows shows "Calculator", and the other
    // way round);
    QStringList otherNames;
    std::vector<std::string> otherNamesUtf8;
    // what else people call it, UTF-8, matched as its name is but never shown
    // ("微信" for WeChat.exe, whatever its shortcut is named);
    std::vector<std::string> aliases;
    // what Windows lists the software it came with under (Settings > 应用:
    // "Microsoft Office LTSC 2024" for Word), UTF-8; matched below its own
    // names, never shown, never the exact match (addInstalledNames; prepare()
    // leaves it alone).
    std::vector<std::string> installedNames;
    // its abbreviations, folded ASCII, matched only when typed in full ("ps").
    std::vector<std::string> abbreviations;
    bool auxiliary = false; // an uninstaller or a document (manual, readme): ranked lower
    // Runs something else through a host program (cmd /k vcvars64.bat, mmc
    // virtmgmt.msc, a web app through chrome_proxy): `program` stays empty, as
    // it is not that program.
    bool hosted = false;
    std::string nameUtf8;
    std::string program; // UTF-8 file name of the desktop program without extension ("WINWORD"), matched too
    std::string initials; // folded first letters of the name's words ("vsc" for Visual Studio Code)
    std::vector<std::string> words; // folded ASCII words of the name (PowerShell: power, shell), for the word-prefix span match

    void prepare(); // with AppAliases::builtIn()
    void prepare(const AppAliases& aliases);
    QString launchPath() const;
};

using AppList = std::vector<AppInfo>;

// Win顺's own list of what apps are also called (src/core/apps.txt): each
// block names an app by its AppUserModelID, its package, its program or,
// failing those, its name in the Start menu, and gives its names in other
// languages, its other names and its abbreviations.
class AppAliases {
public:
    struct Entry {
        QString label; // the block's [header], for people (wsbench --apps)
        QStringList names; // the app's own names in other languages: shown when found by one
        QStringList aliases; // what else people call it: matched, never shown
        std::vector<std::string> abbreviations; // folded ASCII letters and digits, two or more
    };

    // The list's format (see apps.txt); blocks without a way to find their
    // app are left out.
    static AppAliases parse(QStringView text);
    // The list Win顺 ships, read once; empty (with a warning) if it is missing.
    static const AppAliases& builtIn();

    // The block for an app, or null: by `id` (an AppUserModelID, or the
    // package family before its '!'), then by `program` (the file name
    // without extension, or the end of `target`'s path), then by `name`.
    // Empty `program` and `target` for an app that is no program.
    const Entry* find(QStringView id, QStringView program, QStringView target, QStringView name) const;
    const std::vector<Entry>& entries() const noexcept { return m_entries; }

private:
    std::vector<Entry> m_entries;
    // Case-folded keys: AppUserModelIDs, package family names, program
    // names, names; and "\google\chrome\application\chrome.exe" path endings.
    QHash<QString, std::size_t> m_byId;
    QHash<QString, std::size_t> m_byPackage;
    QHash<QString, std::size_t> m_byProgram;
    QHash<QString, std::size_t> m_byName;
    std::vector<std::pair<QString, std::size_t>> m_byPath;
};

// "shell:AppsFolder\<id>": opens the app, and stands for it in History.
QString appLaunchPath(const QString& id);
bool isAppLaunchPath(QStringView path) noexcept;
QString appIdOf(QStringView launchPath); // empty for any other path

struct AppHit {
    std::size_t index = 0; // into the AppList
    int score = 0; // higher is better; may be negative for weak matches
    int otherName = -1; // found by otherNames[otherName], not by its name: shown by that one
    bool exact = false; // its name, another name of it, or an abbreviation of it is what was typed
};

// The apps matching `query`, best first: by name (as NameMatcher matches
// names, pinyin and all), other names and aliases alike (115 for one typed
// in full); an abbreviation typed in full (112); the program's file name
// ("winword", 6 below a name); the initials of the name's words ("vsc": 45
// all of them, 30 the first few); the word-prefix span ("vscode": 30-42).
// Uninstallers and documents rank lower; recently opened apps (their launch
// paths in `history`, newest first) higher.
std::vector<AppHit> searchApps(const AppList& apps, const ParsedQuery& query, const NameMatcher& matcher,
    const QStringList& history, const History::Affinities& affinities = {});

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

// Whether a desktop app's program (or document) is known to be gone: its
// path is on a local fixed disk, and Windows says there is no such file or
// folder. A drive that is not there, a network or removable one, a path of
// another form or any other answer (no access): not gone (#207).
bool appTargetGone(const QString& target);
// `apps` without the desktop apps whose target `gone` says is gone: the
// shell lists a Start-menu shortcut whose program was deleted (by hand, or
// an uninstaller that left the shortcut) as long as the shortcut is there.
AppList presentApps(const AppList& apps, const std::function<bool(const QString&)>& gone = appTargetGone);

// A program Windows lists as installed (Settings > 应用), under the name its
// installer gave it, and the folder it is installed in: "7-Zip 25.01 (x64)" in
// C:\Program Files\7-Zip. The Start menu names its apps by their shortcuts
// ("7-Zip File Manager"), which hold none of the words such a name is typed by.
struct InstalledName {
    QString name; // DisplayName
    QString folder; // InstallLocation: %VARS% expanded, native separators, no closing '\'
};

// The Uninstall keys' installed programs that name their folder (HKLM, its
// 32-bit view, and HKCU), without components and updates (SystemComponent,
// ParentKeyName) and without those that name a folder many programs share (a
// drive, Program Files, the Windows folder, the user's own: sharedInstallFolder).
// Empty on failure. Sorted, so that a second read keeps each app's names in
// the same order.
std::vector<InstalledName> installedNames();

// Whether `folder` is one of `shared` (compared without case): an installer
// that wrote such a place as its own would name every program in it.
bool sharedInstallFolder(const QString& folder, const QStringList& shared);

// Gives every desktop app whose program is in a name's folder that name
// (AppInfo::installedNames): "Microsoft Office" finds Word and Excel, "7-Zip
// 25.01 (x64)" finds 7-Zip File Manager. Only the names of the deepest such
// folder: a suite's name is not given to the program installed in a folder of
// its own inside it. Not a name the app has already.
void addInstalledNames(AppList& apps, const std::vector<InstalledName>& names);

// How often at most AppCatalog asks the disk whether the programs are
// there, unless the list was just read or an app did not open: a hard disk
// asked each time the search box opens would hardly ever sleep.
inline constexpr std::chrono::steady_clock::duration kProgramsCheckEvery = std::chrono::minutes(5);
// Whether to ask now: `sinceLast` the last time (none: never), `reloaded`
// the list just read from the shell, `asked` by checkPrograms().
bool programsCheckDue(std::optional<std::chrono::steady_clock::duration> sinceLast, bool reloaded, bool asked);

// The list of installed apps, read from the shell in the background and
// re-read when the Start menu or the installed packages change; those
// whose program is gone left out (presentApps), looked at after every read
// and otherwise as programsCheckDue says.
class AppCatalog : public QObject {
    Q_OBJECT

public:
    explicit AppCatalog(QObject* parent = nullptr);
    ~AppCatalog() override;

    // Sorted by name; empty until the first load finishes. Thread-safe.
    std::shared_ptr<const AppList> apps() const;
    bool isLoaded() const;
    // Puts this list in as if it had been read (tests). Thread-safe.
    void setApps(std::shared_ptr<const AppList> apps);

    // Re-reads the list in the background if the Start-menu folders or the
    // installed packages changed since the last read (a check of a few
    // milliseconds), or regardless with `force`; asks the disk again whether
    // the programs are there when programsCheckDue (a few ms more). Call
    // from the owner thread.
    void refresh(bool force = false);
    // Asks the disk whether the programs are there now, whatever the time
    // since the last look (an app did not open). Call from the owner thread.
    void checkPrograms();

signals:
    void changed(); // the list differs from before

private:
    struct Outcome {
        quint64 fingerprint = 0;
        bool reloaded = false; // the list was read again
        std::shared_ptr<const AppList> all; // as the shell read it; null: not read, or the read failed
        std::shared_ptr<const AppList> apps; // presentApps of the newest list; null: not looked at
        bool checked = false; // the programs were looked at
    };

    void start(bool force, bool check);
    void publish(const Outcome& outcome);

    mutable std::mutex m_mutex;
    std::shared_ptr<const AppList> m_apps; // guarded by m_mutex

    // Owner thread only.
    std::shared_ptr<const AppList> m_all; // the last list the shell read, gone programs and all
    quint64 m_fingerprint = 0;
    std::chrono::steady_clock::time_point m_loadedAt;
    bool m_busy = false;
    std::optional<std::chrono::steady_clock::time_point> m_checkedAt; // the programs last looked at
    bool m_again = false; // refresh(true) arrived while busy
    bool m_checkAgain = false; // checkPrograms() arrived while busy
    std::jthread m_worker; // last: stopped and joined before the members above go
};

// Reads shell:AppsFolder (COM; some 0.4 s for 350 apps). Empty on failure.
AppList loadInstalledApps(const std::stop_token& stop);

// Hash of the Start-menu folders' write times and the per-user package
// repository's: changes whenever an app is installed or removed.
quint64 installedAppsFingerprint();

} // namespace ws
