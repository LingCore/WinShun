#pragma once

#include "Crawler.h"

#include <QString>
#include <QStringList>

namespace qf {

// User settings, edited in the settings window and stored as an INI file
// (%APPDATA%\QuickFind\QuickFind.ini). Missing keys are written back with
// their defaults so every option is discoverable in the file.
struct Settings {
    // [Launcher]
    bool doubleCtrl = true;
    QString hotkey; // e.g. "Alt+Space"; empty = none
    QString renderer = QStringLiteral("auto"); // software | d3d11 | auto

    // [Index]
    QStringList excludedPaths;
    QStringList excludedNames;
    bool includeRemovableDrives = false;
    bool rescanOnStartup = true;

    // [Content]
    QStringList contentExtensions;
    int maxContentFileSizeMB = 64;

    static Settings defaults();

    void load();
    void save() const;
    bool isFirstRun() const noexcept { return m_firstRun; }

    bool operator==(const Settings& other) const; // the options only, not isFirstRun()

    CrawlRules crawlRules() const;

    static QString filePath();
    static QString storedRenderer(); // just [Launcher] Renderer; works before QGuiApplication exists
    static QString resolveRenderer(const QString& renderer); // "auto" -> software with <= 16 GB of RAM, else d3d11
    static QString dataDir(); // %LOCALAPPDATA%\QuickFind

private:
    bool m_firstRun = false;
};

} // namespace qf
