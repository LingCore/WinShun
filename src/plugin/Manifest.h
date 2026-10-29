#pragma once

// A plugin's plugin.json (the plugin design, §1.3 and appendix B): read,
// checked, and kept as plain values. Errors are sentences for the settings
// page ("commands[0].keyword 含空格"). Unknown fields are ignored; a
// `manifest` newer than kManifestVersion is refused.

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <cstdint>
#include <optional>
#include <utility>

namespace ws::plugin {

inline constexpr int kManifestVersion = 1;
inline constexpr const char* kProtocolVersion = "1";

// A string, or {zh, en}: shown in the interface's language.
struct Text {
    QString zh;
    QString en;
    bool isEmpty() const noexcept { return zh.isEmpty() && en.isEmpty(); }
    QString pick(const QString& locale) const; // the locale's, else the other one
    friend bool operator==(const Text&, const Text&) = default;
};

enum class Permission : std::uint32_t {
    FilesWrite = 1u << 0,
    Processes = 1u << 1,
    ClipboardRead = 1u << 2,
    ClipboardWrite = 1u << 3,
    Ui = 1u << 4,
    Network = 1u << 5,
    WinshunSearch = 1u << 6,
    WinshunOpen = 1u << 7,
    WinshunHistory = 1u << 8,
    FilesRead = 1u << 9,
    WinshunContent = 1u << 10,
    Notifications = 1u << 11, // winshun/notify with "toast": a system notification (Host gates it)
    NetworkListen = 1u << 12, // it opens ports for devices nearby (a declaration)
};

struct Permissions {
    std::uint32_t flags = 0;
    QStringList readPaths; // files:read: what it says it reads (a declaration)
    QStringList listens; // network:listen: the ports it says it opens ("TCP 53317-53320")
    bool has(Permission p) const noexcept { return (flags & static_cast<std::uint32_t>(p)) != 0; }
    QStringList names() const; // "processes", "clipboard:read" …
    friend bool operator==(const Permissions&, const Permissions&) = default;
};
std::optional<Permission> permissionFromName(QStringView name);
QString permissionName(Permission p);

enum class RunMode { Once, Resident };
enum class Program { Python, Pwsh, PowerShell, Node, Exe };
enum class Encoding { Utf8, Oem };
enum class StartWhen { Lazy, Eager };
enum class SandboxLevel { Normal, Strict };

struct Runtime {
    RunMode mode = RunMode::Once;
    Program program = Program::Python;
    QString programPath; // Program::Exe: relative to the plugin folder
    QStringList args;
    QString script; // relative to the plugin folder; empty for Exe
    QString cwd; // relative to the plugin folder; empty: the folder
    QList<std::pair<QString, QString>> env;
    Encoding encoding = Encoding::Utf8;
    int timeoutMs = 0; // one query; filled in with the mode's default
    int debounceMs = 0;
    int idleExitSec = 600; // resident; 0: never
    StartWhen start = StartWhen::Lazy;
    int memoryMB = 512;
    SandboxLevel sandbox = SandboxLevel::Normal;
    friend bool operator==(const Runtime&, const Runtime&) = default;
};

struct Trigger {
    enum class Kind { Regex, Push, Files, Clipboard };
    Kind kind = Kind::Regex;
    QString regex; // Regex, Clipboard
    int minLength = 1;
    int maxLength = 64;
    int maxItems = 3; // Push: how many rows it may push (3 at most)
    QString fileKind; // Files: file | folder | any
    QStringList extensions;
    int maxFiles = 20;
    Text title; // Files: the result menu's entry ("用局域网发送…"); empty: the command's title
    friend bool operator==(const Trigger&, const Trigger&) = default;
};

struct Action {
    QString id;
    Text title;
    QString key; // "Enter", "Ctrl+Enter", "Shift+Enter"
    friend bool operator==(const Action&, const Action&) = default;
};

// A zero-code command's `run`.
struct Run {
    enum class Kind { None, Url, Program, Folder, Search, Items };
    Kind kind = Kind::None;
    QString url; // Url: with %s for the words
    QString home; // Url: what the keyword alone opens
    QString program; // Program: what to start (%VARS% allowed)
    QString args; // Program: with %s for the words
    QString cwd;
    QString folder; // Folder
    QString query; // Search: with %s
    QString scope; // Search: all | files | folders | content
    QJsonArray items; // Items: item objects (Items.h)
    friend bool operator==(const Run&, const Run&) = default;
};

struct Command {
    QString id;
    Text title;
    Text description;
    Text hint;
    QString icon;
    QStringList keywords;
    QList<Trigger> triggers;
    enum class Mode { List, Instant };
    Mode mode = Mode::List;
    QList<Action> actions;
    Run run; // Kind::None: the plugin's process answers
    int cacheSeconds = 0;
    QJsonObject ai;
    bool zeroCode() const noexcept { return run.kind != Run::Kind::None; }
    friend bool operator==(const Command&, const Command&) = default;
};

struct Setting {
    QString key;
    QString type; // text | password | bool | number | choice | folder | file | hotkey
    Text title;
    Text hint;
    QJsonValue defaultValue;
    QJsonArray options; // choice
    friend bool operator==(const Setting&, const Setting&) = default;
};

struct Manifest {
    int manifestVersion = 0;
    QString id;
    Text name;
    QString version;
    Text description;
    QString author;
    QString homepage;
    QString license;
    QString icon; // relative path; may be empty
    QString minWinShun;
    std::optional<Runtime> runtime; // none: every command is zero-code
    Permissions permissions;
    QList<Command> commands;
    QList<Setting> settings;
    bool aiExpose = false;

    const Command* command(QStringView commandId) const;
    bool resident() const noexcept { return runtime && runtime->mode == RunMode::Resident; }
    friend bool operator==(const Manifest&, const Manifest&) = default;

    // The text of a plugin.json. Nothing, with `error` a sentence, when it
    // is not a manifest Win顺 can use.
    static std::optional<Manifest> parse(const QByteArray& json, QString* error);
    static std::optional<Manifest> load(const QString& path, QString* error);
};

// "1.2.0" against "1.10": numeric by segment, missing segments 0, a
// pre-release suffix (-beta) ranks below the plain version. <0, 0, >0.
int compareVersions(QStringView a, QStringView b);
bool validPluginId(QStringView id) noexcept; // [a-z0-9._-], 1–64, no leading/trailing dot
// A relative path that stays inside a folder: not absolute, no "..".
bool insideFolder(QStringView relative) noexcept;

} // namespace ws::plugin
