#pragma once

#include "SearchTypes.h"

#include <QByteArrayView>
#include <QObject>
#include <QString>
#include <QStringList>

#include <chrono>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ws {

class NameMatcher;
struct ParsedQuery;

// A place in Windows to open by name or by what it is about: a page of
// Settings, a Control Panel item or task ("查看网络连接", which 适配器 finds
// too), or a tool or folder from WinShun's own list (places.txt).
struct PlaceInfo {
    QString name; // in the Windows display language
    QString key; // the same in every language: the Settings index's Filename, or "winshun/<id>"
    // What opens it: "ms-settings:display", "shell:::{...}",
    // "%windir%\system32\control.exe /name Microsoft.DeviceManager", "diskmgmt.msc".
    QString command;
    // "app:<AppUserModelID>" for that app's logo, an icon in a module
    // ("%SystemRoot%\System32\netcenter.dll,-1"), or a file or shell path
    // whose icon it takes.
    QString icon;
    AppKind kind = AppKind::ControlPanel; // Setting, ControlPanel, Security or Tool
    QStringList keywords; // Windows': other words people look for it by
    QStringList ownKeywords; // places.txt's: what people call it ("网卡"), worth more
    bool page = false; // the entry of a page of Settings itself, not of a task on it

    // Filled by prepare() from the fields above: UTF-8, ASCII folded, no duplicates.
    std::string nameUtf8;
    std::vector<std::string> folded; // keywords
    std::vector<std::string> ownFolded; // ownKeywords

    void prepare();
    QString path() const; // placePath(key)
};

using PlaceList = std::vector<PlaceInfo>;

// "winshun-place:<key>": stands for a place in History.
QString placePath(const QString& key);
bool isPlacePath(QStringView path) noexcept;
QString placeKeyOf(QStringView path); // empty for any other path

struct PlaceHit {
    std::size_t index = 0; // into the PlaceList
    int score = 0; // higher is better
};

// The places matching `query`, best first, one per command (several tasks
// can open the same page). Each word of the query must be in the name, or
// begin one of the keywords: a word of two or more Chinese characters may
// be anywhere in one, and pinyin works for both. A keyword spelled out in
// full ranks above a name that merely starts with the word, an own keyword
// (places.txt) above most names. Recently opened places (their paths in
// `history`, newest first) rank higher.
std::vector<PlaceHit> searchPlaces(const PlaceList& places, const ParsedQuery& query, const NameMatcher& matcher,
    const QStringList& history);

// A command split into what to start and its arguments, environment
// variables expanded: "%windir%\system32\control.exe /name X" ->
// {"C:\Windows\system32\control.exe", "/name X"}. A shell location or URI
// ("shell:::{...}", "ms-settings:display") has no arguments.
std::pair<QString, QString> splitCommand(const QString& command);

// A command as a row shows it: the console or applet it opens
// ("diskmgmt.msc", "mmsys.cpl"), the folder ("C:\Users\me\AppData\Local\Temp"),
// the shell location ("shell:startup"), else the program and its arguments.
QString describeCommand(const QString& command);

// One entry of the Settings search index that Windows ships
// (%WINDIR%\ImmersiveControlPanel\Settings\AllSystemSettings_*.xml), as written.
struct SettingsIndexEntry {
    QString key; // <Filename>
    QString deepLink; // Control Panel and the like; empty for a page of Settings
    QString page; // the page of Settings it is on
    QString policyIds; // older files only: the page's ms-settings: names, "sound;sound-devices;..."
    QString host; // <HostID>: the program that shows it
    QString icon;
    QString condition; // "shcond://v1#IsServer;1": shown only where this holds
    QString description; // its name, as an indirect string ("@shell32.dll,-24389")
    QStringList keywords; // indirect strings, each resolving to a ';'-separated list ("@shell32.dll,-25174")
};

std::vector<SettingsIndexEntry> parseSettingsIndex(QByteArrayView xml);

// Which of a page's ms-settings: names opens the page itself: the one its id
// names ("installed-apps" for SettingsPageInstalledApps), else the one the
// others extend ("sound" for "sound-devices" ...), else the first.
QString choosePageUri(const QString& page, const QString& policyIds);

// What opens a deep link of the index ("Microsoft.DeviceManager" ->
// "%windir%\system32\control.exe /name Microsoft.DeviceManager"); empty if
// it is not one WinShun can open.
QString commandForDeepLink(const QString& deepLink);

// Applies WinShun's own list (places.txt): keywords for places already in
// `places` (a block headed by their command: the page's own entry if one of
// them is, else each of them) and places of its own (a block with "open =").
// Names are written "中文 | English"; `chinese` picks the first. Returns the
// headers of keyword blocks that matched no place.
QStringList applyPlaceExtras(PlaceList& places, QStringView text, bool chinese);

// The list of places, read in the background (some 0.3 s) and again when
// Windows updates its index or changes its display language.
class SystemCatalog : public QObject {
    Q_OBJECT

public:
    explicit SystemCatalog(QObject* parent = nullptr);
    ~SystemCatalog() override;

    // Empty until the first load finishes. Thread-safe.
    std::shared_ptr<const PlaceList> places() const;

    // Reads the list again in the background if the index files or the
    // display language changed since the last read (a check of well under a
    // millisecond), or regardless with `force`. Call from the owner thread.
    void refresh(bool force = false);

signals:
    void changed(); // a new list is in

private:
    struct Outcome {
        quint64 fingerprint = 0;
        std::shared_ptr<const PlaceList> places; // null: not read
    };

    void publish(const Outcome& outcome);

    mutable std::mutex m_mutex;
    std::shared_ptr<const PlaceList> m_places; // guarded by m_mutex

    // Owner thread only.
    quint64 m_fingerprint = 0;
    bool m_busy = false;
    bool m_again = false; // refresh(true) arrived while busy
    std::jthread m_worker; // last: stopped and joined before the members above go
};

// Reads the Settings index in the Windows display language and adds
// WinShun's own list. Empty only when stopped.
PlaceList loadPlaces(const std::stop_token& stop);

// Hash of the index files' names, sizes and write times and of the display language.
quint64 placesFingerprint();

} // namespace ws
