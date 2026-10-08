#pragma once

#include "Crawler.h"

#include <QString>
#include <QStringList>

namespace ws {

// User settings, edited in the settings window and stored as an INI file
// (%APPDATA%\WinShun\WinShun.ini). Missing keys are written back with
// their defaults so every option is discoverable in the file.
struct Settings {
    // [Launcher]
    bool doubleCtrl = true;
    QString hotkey; // e.g. "Alt+Space"; empty = none
    QString renderer = QStringLiteral("auto"); // software | d3d11 | auto
    bool recordHistory = true; // remember what was opened: listed with nothing typed, first among matches

    // [Appearance]
    QString theme = QStringLiteral("system"); // system | light | dark
    QString language = QStringLiteral("system"); // system | zh | en
    QString transparency = QStringLiteral("auto"); // off | on | auto: Mica behind the windows, where Windows 11 has it

    // [Update]
    bool autoUpdate = true; // look for new versions on GitHub (see Updater)

    // [Index]
    QStringList excludedPaths;
    QStringList excludedNames;
    bool includeRemovableDrives = false;
    bool rescanOnStartup = true;

    // [Content]
    QStringList contentExtensions;
    int maxContentFileSizeMB = 64;
    bool contentIndex = true; // which files have which Chinese, Japanese and Korean characters
    bool contentInLowPriority = false; // 内容 also looks in system, program and tool folders

    static Settings defaults();
    static bool exists(); // false on the very first run

    void load();
    void save() const;

    bool operator==(const Settings&) const = default;

    CrawlRules crawlRules() const;

    static QString filePath();
    static QString storedRenderer(); // just [Launcher] Renderer; works before QGuiApplication exists
    static QString resolveRenderer(const QString& renderer); // "auto" -> software with <= 16 GB of RAM, else d3d11
    static bool resolveTransparency(const QString& transparency); // "auto" -> off with <= 16 GB of RAM, else on
    static bool lowMemory(); // 16 GB of RAM or less: the "auto" choices save memory
    static QString dataDir(); // %LOCALAPPDATA%\WinShun
    static QString resolveLanguage(const QString& language); // "system" -> zh with a Chinese Windows, else en
};

} // namespace ws
