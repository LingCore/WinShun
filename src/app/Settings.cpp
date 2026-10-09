#include "Settings.h"

#include "Win32Util.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <windows.h>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

// Never indexed: system stores and machine-generated caches nobody searches.
// Forward slashes keep the INI file readable; %VARS% are expanded.
const QStringList kDefaultExcludedPaths {
    u"%WINDIR%/WinSxS"_s,
    u"%WINDIR%/Installer"_s,
    u"%WINDIR%/servicing"_s,
    u"%WINDIR%/SoftwareDistribution"_s,
    u"%WINDIR%/assembly"_s,
    u"%WINDIR%/System32/DriverStore"_s,
    u"%WINDIR%/Prefetch"_s,
    u"%SystemDrive%/$WinREAgent"_s,
    u"%TEMP%"_s,
};

// A folder name at any depth, or a tail such as ".svn/pristine".
const QStringList kDefaultExcludedNames {
    u"$Recycle.Bin"_s,
    u"System Volume Information"_s,
    u"$SysReset"_s,
    u"Config.Msi"_s,
    u".git/objects"_s,
    u".svn/pristine"_s,
    // Browser / Electron caches
    u"Code Cache"_s,
    u"GPUCache"_s,
    u"CacheStorage"_s,
    u"ScriptCache"_s,
    u"DawnCache"_s,
    u"DawnGraphiteCache"_s,
    u"DawnWebGPUCache"_s,
    u"GrShaderCache"_s,
    u"ShaderCache"_s,
    u"Crashpad"_s,
    u"blob_storage"_s,
};

// Indexed, but ranked below your own files. Dot-folders (.git, .vscode, ...)
// are always ranked lower as well.
const QStringList kLowPriorityPaths {
    u"%WINDIR%"_s,
    u"%ProgramFiles%"_s,
    u"%ProgramFiles(x86)%"_s,
    u"%ProgramData%"_s,
    u"%SystemDrive%/Recovery"_s,
    u"%SystemDrive%/PerfLogs"_s,
};

const QStringList kLowPriorityNames {
    u"AppData"_s,
    u"node_modules"_s,
    u"__pycache__"_s,
    u"site-packages"_s,
    u"Cache"_s,
    u"Caches"_s,
    u"Temp"_s,
    u"Logs"_s,
    u"vendor"_s,
    u"bower_components"_s,
};

// Plain-text formats: the scanner reads their raw bytes. Documents (docx,
// xlsx, pdf...) are read by WinShunExtract.exe instead, under their own
// setting (Content/Documents). The first three name the scope in the
// launcher ("搜索 .txt / .md / .log 等文件中的文字").
const QStringList kDefaultContentExtensions {
    // Documents and logs
    u"txt"_s, u"md"_s, u"log"_s, u"csv"_s, u"tsv"_s, u"ini"_s, u"cfg"_s, u"conf"_s,
    // Source code and scripts
    u"c"_s, u"cpp"_s, u"h"_s, u"hpp"_s, u"py"_s, u"js"_s, u"ts"_s, u"java"_s, u"cs"_s, u"go"_s, u"rs"_s,
    u"qml"_s, u"cmake"_s, u"bat"_s, u"cmd"_s, u"ps1"_s, u"sh"_s, u"sql"_s,
    // Data and markup
    u"json"_s, u"xml"_s, u"yaml"_s, u"yml"_s, u"toml"_s, u"html"_s, u"htm"_s, u"css"_s, u"svg"_s, u"srt"_s,
};

// Password managers: most mark what they copy as not for clipboard history
// (see ClipboardWatcher), these are skipped in any case.
const QStringList kDefaultClipboardExcludedApps {
    u"KeePass.exe"_s,
    u"KeePassXC.exe"_s,
    u"1Password.exe"_s,
    u"Bitwarden.exe"_s,
};

// Bumped when kDefaultContentExtensions grows, so lists that still hold an
// older default pick up the new one.
constexpr int kContentDefaultsVersion = 2;

// The size limits of the kinds of files, by ContentSizeLimits::Kind.
const QString kContentSizeKeys[ContentSizeLimits::kKinds] {u"Content/MaxFileSizeMB"_s, u"Content/MaxCodeFileSizeMB"_s,
    u"Content/MaxDataFileSizeMB"_s, u"Content/MaxDocumentFileSizeMB"_s};

std::wstring normalizeName(const QString& name)
{
    return QDir::toNativeSeparators(name.trimmed()).toStdWString();
}

std::wstring normalizePath(const QString& path)
{
    std::wstring p = win32::expandEnvironment(QDir::toNativeSeparators(path.trimmed()).toStdWString());
    while (p.size() > 3 && p.back() == L'\\')
        p.pop_back();
    // %TEMP% can be an 8.3 short path (C:\Users\JOHNDO~1\...); the crawler sees long names.
    if (const DWORD n = ::GetLongPathNameW(p.c_str(), nullptr, 0); n > 0) {
        std::wstring full(n, L'\0');
        if (const DWORD written = ::GetLongPathNameW(p.c_str(), full.data(), n); written > 0 && written < n) {
            full.resize(written);
            p = std::move(full);
        }
    }
    if (p.size() == 3 && p[1] == L':' && p[2] == L'\\')
        p.pop_back(); // "C:\" -> "C:"
    return p;
}

template <typename T> T readOrDefault(QSettings& s, const QString& key, const T& fallback)
{
    if (!s.contains(key)) {
        s.setValue(key, fallback);
        return fallback;
    }
    return s.value(key).value<T>();
}

void writeWebShortcuts(QSettings& s, const WebShortcuts& shortcuts)
{
    s.remove(u"WebSearch"_s); // no entries left over from a longer list
    s.beginWriteArray(u"WebSearch"_s, static_cast<int>(shortcuts.size()));
    for (int i = 0; i < shortcuts.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue(u"Keyword"_s, shortcuts[i].keyword);
        s.setValue(u"Name"_s, shortcuts[i].name);
        s.setValue(u"Url"_s, shortcuts[i].url);
        s.setValue(u"Home"_s, shortcuts[i].home);
    }
    s.endArray();
}

// The file may have been edited by hand: addresses are tidied as the settings
// window would ("github.com/..." gets https://), an entry without a name takes
// its keyword, and one that cannot be used (no keyword to type, an address
// Win顺 does not open) is left out.
WebShortcuts readWebShortcuts(QSettings& s, const WebShortcuts& fallback)
{
    if (!s.contains(u"WebSearch/size"_s)) { // never written: the defaults, and into the file with them
        writeWebShortcuts(s, fallback);
        return fallback;
    }
    WebShortcuts shortcuts;
    const int size = s.beginReadArray(u"WebSearch"_s);
    for (int i = 0; i < size; ++i) {
        s.setArrayIndex(i);
        WebShortcut shortcut;
        shortcut.keyword = s.value(u"Keyword"_s).toString().trimmed();
        shortcut.name = s.value(u"Name"_s).toString().trimmed();
        shortcut.url = normalizeWebUrl(s.value(u"Url"_s).toString());
        shortcut.home = normalizeWebUrl(s.value(u"Home"_s).toString());
        if (!isValidWebKeyword(shortcut.keyword) || shortcut.url.isEmpty())
            continue; // nothing to type, or nothing to open
        if (shortcut.name.isEmpty())
            shortcut.name = shortcut.keyword;
        shortcuts.append(shortcut);
    }
    s.endArray();
    return shortcuts;
}

} // namespace

QString Settings::filePath()
{
    return QSettings(QSettings::IniFormat, QSettings::UserScope, u"WinShun"_s, u"WinShun"_s).fileName();
}

QString Settings::storedRenderer()
{
    QSettings s(QSettings::IniFormat, QSettings::UserScope, u"WinShun"_s, u"WinShun"_s);
    return resolveRenderer(s.value(u"Launcher/Renderer"_s).toString().trimmed().toLower());
}

QString Settings::resolveRenderer(const QString& renderer)
{
    if (renderer == u"software" || renderer == u"d3d11")
        return renderer;
    return defaultRenderer();
}

QString Settings::defaultRenderer()
{
    // Only machines with little memory save the ~50 MB that D3D11 costs. The
    // RAM fitted, not what Windows has left of it: an 8 GB PC reports ~7.8 GB.
    ULONGLONG installedKB = 0;
    if (!::GetPhysicallyInstalledSystemMemory(&installedKB)) {
        MEMORYSTATUSEX status {sizeof(status)};
        if (::GlobalMemoryStatusEx(&status))
            installedKB = (status.ullTotalPhys >> 10) + (512ull << 10); // round up what the firmware keeps
    }
    return installedKB >= 8ull << 20 ? u"d3d11"_s : u"software"_s;
}

bool Settings::lowMemory()
{
    MEMORYSTATUSEX status {sizeof(status)};
    return ::GlobalMemoryStatusEx(&status) && status.ullTotalPhys <= 16ull << 30;
}

QString Settings::dataDir()
{
    return QDir::toNativeSeparators(
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/WinShun"_s);
}

QString Settings::indexDir() const
{
    if (indexFolder.isEmpty())
        return dataDir();
    const std::wstring expanded = win32::expandEnvironment(indexFolder.toStdWString());
    return QDir::toNativeSeparators(QDir::cleanPath(QString::fromStdWString(expanded)));
}

QString Settings::resolveLanguage(const QString& language)
{
    if (language == u"zh" || language == u"en")
        return language;
    // The language Windows itself is shown in. Not QLocale::uiLanguages(): that
    // follows the preferred-languages list, which can put English first on a
    // Chinese Windows.
    return PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_CHINESE ? u"zh"_s : u"en"_s;
}

Settings Settings::defaults()
{
    Settings d;
    d.excludedPaths = kDefaultExcludedPaths;
    d.excludedNames = kDefaultExcludedNames;
    d.contentExtensions = kDefaultContentExtensions;
    d.clipboard = windowsClipboardHistory(); // whoever kept Windows' clipboard history keeps ours
    d.clipboardExcludedApps = kDefaultClipboardExcludedApps;
    d.webShortcuts = defaultWebShortcuts();
    d.renderer = defaultRenderer();
    d.transparency = lowMemory() ? u"off"_s : u"on"_s;
    d.clipboardMaxItems = lowMemory() ? 100 : 200;
    return d;
}

bool Settings::hasClipboardSettings()
{
    QSettings s(QSettings::IniFormat, QSettings::UserScope, u"WinShun"_s, u"WinShun"_s);
    return s.contains(u"Clipboard/Enabled"_s);
}

bool Settings::windowsClipboardHistory()
{
    DWORD on = 0;
    DWORD size = sizeof on;
    return ::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Clipboard", L"EnableClipboardHistory",
               RRF_RT_REG_DWORD, nullptr, &on, &size)
            == ERROR_SUCCESS
        && on != 0;
}

bool Settings::exists()
{
    return QFileInfo::exists(filePath());
}

void Settings::load()
{
    QSettings s(QSettings::IniFormat, QSettings::UserScope, u"WinShun"_s, u"WinShun"_s);
    const Settings d = defaults();

    doubleCtrl = readOrDefault(s, u"Launcher/DoubleCtrl"_s, d.doubleCtrl);
    doubleCtrlPauseInGames = readOrDefault(s, u"Launcher/DoubleCtrlPauseInGames"_s, d.doubleCtrlPauseInGames);
    doubleCtrlPauseInFullScreen
        = readOrDefault(s, u"Launcher/DoubleCtrlPauseInFullScreen"_s, d.doubleCtrlPauseInFullScreen);
    doubleCtrlExcludedApps = readOrDefault(s, u"Launcher/DoubleCtrlExcludedApps"_s, d.doubleCtrlExcludedApps);
    hotkey =readOrDefault(s, u"Launcher/Hotkey"_s, d.hotkey);
    renderer = resolveRenderer(readOrDefault(s, u"Launcher/Renderer"_s, d.renderer).trimmed().toLower());
    recordHistory = readOrDefault(s, u"Launcher/History"_s, d.recordHistory);
    dialogJump = readOrDefault(s, u"Launcher/DialogJump"_s, d.dialogJump);
    dialogBar = readOrDefault(s, u"Launcher/DialogBar"_s, d.dialogBar);
    dialogBarPlace = readOrDefault(s, u"Launcher/DialogBarPlace"_s, d.dialogBarPlace).trimmed().toLower();
    if (dialogBarPlace != u"below" && dialogBarPlace != u"left" && dialogBarPlace != u"right")
        dialogBarPlace = d.dialogBarPlace;
    dialogAutoJump = readOrDefault(s, u"Launcher/DialogAutoJump"_s, d.dialogAutoJump);
    dialogBarExcludedApps = readOrDefault(s, u"Launcher/DialogBarExcludedApps"_s, d.dialogBarExcludedApps);
    fileManager = readOrDefault(s, u"Launcher/FileManager"_s, d.fileManager).trimmed().toLower();
    if (fileManager != u"totalcmd" && fileManager != u"dopus")
        fileManager = d.fileManager;

    theme = readOrDefault(s, u"Appearance/Theme"_s, d.theme).trimmed().toLower();
    if (theme != u"light" && theme != u"dark")
        theme = d.theme;
    language = readOrDefault(s, u"Appearance/Language"_s, d.language).trimmed().toLower();
    if (language != u"zh" && language != u"en")
        language = d.language;
    transparency = readOrDefault(s, u"Appearance/Transparency"_s, d.transparency).trimmed().toLower();
    // It used to be a switch: "false" was turned off by hand, "true" was mostly
    // the default written back.
    if (transparency == u"false" || transparency == u"true") {
        transparency = transparency == u"false" ? u"off"_s : d.transparency;
        s.setValue(u"Appearance/Transparency"_s, transparency);
    } else if (transparency != u"on" && transparency != u"off") { // also an old "auto"
        transparency = d.transparency;
    }

    autoUpdate = readOrDefault(s, u"Update/Automatic"_s, d.autoUpdate);

    excludedPaths = readOrDefault(s, u"Index/ExcludedPaths"_s, d.excludedPaths);
    excludedNames = readOrDefault(s, u"Index/ExcludedNames"_s, d.excludedNames);
    includeRemovableDrives = readOrDefault(s, u"Index/IncludeRemovableDrives"_s, d.includeRemovableDrives);
    rescanOnStartup = readOrDefault(s, u"Index/RescanOnStartup"_s, d.rescanOnStartup);
    indexFolder = readOrDefault(s, u"Index/Folder"_s, d.indexFolder).trimmed();

    contentExtensions = readOrDefault(s, u"Content/Extensions"_s, d.contentExtensions);
    if (s.value(u"Content/DefaultsVersion"_s, 1).toInt() < kContentDefaultsVersion) {
        // Version 1 defaulted to just "txt". A list the user edited is left alone.
        if (contentExtensions == QStringList {u"txt"_s}) {
            contentExtensions = d.contentExtensions;
            s.setValue(u"Content/Extensions"_s, contentExtensions);
        }
        s.setValue(u"Content/DefaultsVersion"_s, kContentDefaultsVersion);
    }
    // MaxFileSizeMB once held for every kind of text file: a size set there
    // holds for code and data too, until they have their own.
    for (std::size_t k = 0; k < ContentSizeLimits::kKinds; ++k) {
        int fallback = d.contentMaxSizeMB[k];
        if ((k == ContentSizeLimits::Code || k == ContentSizeLimits::Data) && !s.contains(kContentSizeKeys[k])
            && contentMaxSizeMB[0] != d.contentMaxSizeMB[0])
            fallback = contentMaxSizeMB[0];
        contentMaxSizeMB[k] = std::clamp(readOrDefault(s, kContentSizeKeys[k], fallback), 1, 4096);
    }
    contentIndex = readOrDefault(s, u"Content/Index"_s, d.contentIndex);
    contentInLowPriority = readOrDefault(s, u"Content/IncludeSystemFolders"_s, d.contentInLowPriority);
    contentDocuments = readOrDefault(s, u"Content/Documents"_s, d.contentDocuments);

    clipboard = readOrDefault(s, u"Clipboard/Enabled"_s, d.clipboard);
    clipboardWinV = readOrDefault(s, u"Clipboard/WinV"_s, d.clipboardWinV);
    clipboardHotkey = readOrDefault(s, u"Clipboard/Hotkey"_s, d.clipboardHotkey);
    clipboardMaxItems = std::clamp(readOrDefault(s, u"Clipboard/MaxItems"_s, d.clipboardMaxItems), 10, 300);
    clipboardMaxDays = std::clamp(readOrDefault(s, u"Clipboard/MaxDays"_s, d.clipboardMaxDays), 0, 3650);
    clipboardImages = readOrDefault(s, u"Clipboard/Images"_s, d.clipboardImages);
    clipboardExcludedApps = readOrDefault(s, u"Clipboard/ExcludedApps"_s, d.clipboardExcludedApps);

    webShortcuts = readWebShortcuts(s, d.webShortcuts);
}

void Settings::save() const
{
    QSettings s(QSettings::IniFormat, QSettings::UserScope, u"WinShun"_s, u"WinShun"_s);
    s.setValue(u"Launcher/DoubleCtrl"_s, doubleCtrl);
    s.setValue(u"Launcher/DoubleCtrlPauseInGames"_s, doubleCtrlPauseInGames);
    s.setValue(u"Launcher/DoubleCtrlPauseInFullScreen"_s, doubleCtrlPauseInFullScreen);
    s.setValue(u"Launcher/DoubleCtrlExcludedApps"_s, doubleCtrlExcludedApps);
    s.setValue(u"Launcher/Hotkey"_s, hotkey);
    s.setValue(u"Launcher/Renderer"_s, renderer);
    s.setValue(u"Launcher/History"_s, recordHistory);
    s.setValue(u"Launcher/DialogJump"_s, dialogJump);
    s.setValue(u"Launcher/DialogBar"_s, dialogBar);
    s.setValue(u"Launcher/DialogBarPlace"_s, dialogBarPlace);
    s.setValue(u"Launcher/DialogAutoJump"_s, dialogAutoJump);
    s.setValue(u"Launcher/DialogBarExcludedApps"_s, dialogBarExcludedApps);
    s.setValue(u"Launcher/FileManager"_s, fileManager);
    s.setValue(u"Appearance/Theme"_s, theme);
    s.setValue(u"Appearance/Language"_s, language);
    s.setValue(u"Appearance/Transparency"_s, transparency);
    s.setValue(u"Update/Automatic"_s, autoUpdate);
    s.setValue(u"Index/ExcludedPaths"_s, excludedPaths);
    s.setValue(u"Index/ExcludedNames"_s, excludedNames);
    s.setValue(u"Index/IncludeRemovableDrives"_s, includeRemovableDrives);
    s.setValue(u"Index/RescanOnStartup"_s, rescanOnStartup);
    s.setValue(u"Index/Folder"_s, indexFolder);
    s.setValue(u"Content/Extensions"_s, contentExtensions);
    for (std::size_t k = 0; k < ContentSizeLimits::kKinds; ++k)
        s.setValue(kContentSizeKeys[k], contentMaxSizeMB[k]);
    s.setValue(u"Content/Index"_s, contentIndex);
    s.setValue(u"Content/IncludeSystemFolders"_s, contentInLowPriority);
    s.setValue(u"Content/Documents"_s, contentDocuments);
    s.setValue(u"Clipboard/Enabled"_s, clipboard);
    s.setValue(u"Clipboard/WinV"_s, clipboardWinV);
    s.setValue(u"Clipboard/Hotkey"_s, clipboardHotkey);
    s.setValue(u"Clipboard/MaxItems"_s, clipboardMaxItems);
    s.setValue(u"Clipboard/MaxDays"_s, clipboardMaxDays);
    s.setValue(u"Clipboard/Images"_s, clipboardImages);
    s.setValue(u"Clipboard/ExcludedApps"_s, clipboardExcludedApps);
    writeWebShortcuts(s, webShortcuts);
}

ContentSizeLimits Settings::contentSizeLimits() const
{
    ContentSizeLimits limits;
    for (std::size_t k = 0; k < ContentSizeLimits::kKinds; ++k)
        limits.bytes[k] = static_cast<std::int64_t>(contentMaxSizeMB[k]) << 20;
    return limits;
}

CrawlRules Settings::crawlRules() const
{
    CrawlRules rules;
    for (const QString& p : excludedPaths) {
        if (!p.trimmed().isEmpty())
            rules.excludedPaths.push_back(normalizePath(p));
    }
    // Our own files churn constantly.
    rules.excludedPaths.push_back(normalizePath(dataDir()));
    if (!indexFolder.isEmpty())
        rules.excludedPaths.push_back(normalizePath(indexDir()));
    for (const QString& n : excludedNames) {
        if (!n.trimmed().isEmpty())
            rules.excludedNames.push_back(normalizeName(n));
    }
    for (const QString& p : kLowPriorityPaths) {
        const std::wstring path = normalizePath(p);
        if (path.find(L'%') == std::wstring::npos) // skip variables that do not exist here
            rules.lowPriorityPaths.push_back(path);
    }
    for (const QString& n : kLowPriorityNames)
        rules.lowPriorityNames.push_back(normalizeName(n));
    return rules;
}

} // namespace ws
