#pragma once

#include "ContentIndex.h"
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
    bool doubleCtrlPauseInGames = true; // not while a game has the mouse or exclusive full screen (GameGuard)
    bool doubleCtrlPauseInFullScreen = false; // not over any full-screen window
    QStringList doubleCtrlExcludedApps; // program files ("TheFinals.exe") in front of which double Ctrl does nothing
    QString hotkey; // e.g. "Alt+Space"; empty = none
    QString renderer; // software | d3d11; defaults() picks by memory (defaultRenderer)
    bool recordHistory = true; // remember what was opened: listed with nothing typed, first among matches
    bool dialogJump = true; // Ctrl+G in an Open or Save dialog goes to the folder open in File Explorer
    bool dialogBar = true; // a search bar under Open and Save dialogs (DialogBar)
    bool dialogAutoJump = false; // file dialogs go to the folder open in File Explorer by themselves
    QStringList dialogBarExcludedApps; // program files ("notepad.exe") whose file dialogs go without the bar

    // [Appearance]
    QString theme = QStringLiteral("system"); // system | light | dark
    QString language = QStringLiteral("system"); // system | zh | en
    QString transparency; // off | on: Mica behind the windows, where Windows 11 has it; defaults() picks by memory

    // [Update]
    bool autoUpdate = true; // look for new versions on GitHub (see Updater)

    // [Index]
    QStringList excludedPaths;
    QStringList excludedNames;
    bool includeRemovableDrives = false;
    bool rescanOnStartup = true;

    // [Content]
    QStringList contentExtensions;
    // Larger files are not searched, by kind (ContentSizeLimits::Kind): plain
    // text and logs, source code, data and web pages.
    std::array<int, ContentSizeLimits::kKinds> contentMaxSizeMB {64, 8, 16, 512};
    bool contentIndex = true; // which files have which Chinese, Japanese and Korean characters
    bool contentInLowPriority = false; // 内容 also looks in system, program and tool folders
    bool contentDocuments = true; // and in Word, Excel, PowerPoint and PDF files (documentExtensions())

    // [Clipboard]
    bool clipboard = true; // keep a history of what is copied (Win+V page); at first as Windows' own clipboard history is
    bool clipboardWinV = false; // Win+V opens it, instead of Windows' clipboard history (see winv::)
    QString clipboardHotkey; // another shortcut for it; empty = none
    int clipboardMaxItems = 200; // entries kept outside groups; defaults() keeps 100 with 16 GB of RAM or less
    int clipboardMaxDays = 30; // since last copied or pasted; 0 = no limit
    bool clipboardImages = true;
    QStringList clipboardExcludedApps; // program files whose copies are not kept, "KeePass.exe"

    static Settings defaults();
    static bool exists(); // false on the very first run

    void load();
    void save() const;

    bool operator==(const Settings&) const = default;

    CrawlRules crawlRules() const;

    static QString filePath();
    static QString storedRenderer(); // just [Launcher] Renderer; works before QGuiApplication exists
    static QString resolveRenderer(const QString& renderer); // anything but software or d3d11 (an old "auto") -> defaultRenderer()
    static QString defaultRenderer(); // d3d11 with 8 GB of RAM or more, else software
    static bool lowMemory(); // 16 GB of RAM or less: transparency starts off, the clipboard keeps 100
    static QString dataDir(); // %LOCALAPPDATA%\WinShun
    static bool windowsClipboardHistory(); // Windows' own clipboard history is on (Settings > System > Clipboard)
    static bool hasClipboardSettings(); // the file has a [Clipboard] section: written by a version with the clipboard history
    static QString resolveLanguage(const QString& language); // "system" -> zh with a Chinese Windows, else en
};

} // namespace ws
