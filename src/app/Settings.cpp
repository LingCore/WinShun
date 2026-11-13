#include "Settings.h"
#include "DoubleTapAction.h"
#include "WebSearchConfig.h"
#include "PreviewShortcut.h"
#include "platform/HotkeyText.h"
#include "PathText.h"
#include "Query.h"
#include "SwitchWire.h"

#include "ServiceVolumeAccess.h"
#include "Unc.h"
#include "WebDav.h"
#include "Win32Util.h"
#include "ExplorerLanguage.h" // windowsIsChinese

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QOperatingSystemVersion>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>

#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <type_traits>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

QString g_profile; // see Settings::useProfile()

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

// 优先显示的文件类型: programs and scripts, as forum #104 asked (Listary's
// list), so that a program's name finds the program before its folder and
// its header ("curl": curl.exe, then the folder curl and curl.h). Not lnk:
// the Start menu's shortcuts are apps in 全部 already, and Windows keeps one
// in Recent for every file opened ("报告.docx.lnk"), which would go before
// the file itself when its whole name is typed; nor appref-ms, whose apps
// are in the Start menu too.
const QStringList kDefaultPreferredTypes {u"exe"_s, u"bat"_s, u"cmd"_s, u"ps1"_s, u"msi"_s};
// Unlike most keys, written only when not the default: a later version's
// better list reaches everyone who never changed it. Not done for all keys:
// the uninstaller reads [Clipboard] WinV and [Taskbar] WinS from the file, and
// some defaults are the machine's at the first start (clipboard history,
// renderer, low memory), which must stay as they were.
const QString kPreferredTypesKey = u"Results/PreferredTypes"_s;

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

// Folders on network shares that NAS boxes (Synology, QNAP) and Macs keep for
// themselves: recycle bins, thumbnails, snapshots, Spotlight. Shares only:
// [Index] ExcludedNames of earlier versions is in the file already, so its
// defaults growing would reach nobody.
const QStringList kDefaultNetworkExcludedNames {
    u"#recycle"_s,
    u"@eaDir"_s,
    u"@Recycle"_s,
    u".@__thumb"_s,
    u"@Recently-Snapshot"_s,
    u"#snapshot"_s,
    u".snapshot"_s,
    u".recycle"_s,
    u"$RECYCLE.BIN"_s,
    u"System Volume Information"_s,
    u".TemporaryItems"_s,
    u".Trashes"_s,
    u".Spotlight-V100"_s,
    u".fseventsd"_s,
};

// What a Mac leaves next to the files it writes to a share.
const QStringList kDefaultNetworkExcludedFiles {u"._*"_s, u".DS_Store"_s};

// Bumped when kDefaultContentExtensions grows, so lists that still hold an
// older default pick up the new one; 3 and 4: smaller default size limits.
constexpr int kContentDefaultsVersion = 4;
// The default size limits before version 3 and in version 3, by
// ContentSizeLimits::Kind: one still at the default of its file's version
// takes the new one; one the user set stays.
constexpr std::array<double, ContentSizeLimits::kKinds> kContentMaxSizeMBBefore3 {64, 8, 16, 512};
constexpr std::array<double, ContentSizeLimits::kKinds> kContentMaxSizeMB3 {16, 2, 4, 100};

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
    // A number written by hand that is none ("abc", "", "1h"): the default,
    // not the 0 a failed conversion gives (每 0 小时). A switch reads as before.
    if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
        QVariant value = s.value(key);
        return value.convert(QMetaType::fromType<T>()) ? value.value<T>() : fallback;
    } else {
        return s.value(key).value<T>();
    }
}

void writeWebShortcuts(QSettings& s, const WebShortcuts& shortcuts, bool suggestions)
{
    s.remove(u"WebSearch"_s); // no entries left over from a longer list
    s.beginWriteArray(u"WebSearch"_s, static_cast<int>(shortcuts.size()));
    for (int i = 0; i < shortcuts.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue(u"Keyword"_s, shortcuts[i].keyword);
        s.setValue(u"Name"_s, shortcuts[i].name);
        s.setValue(u"Url"_s, shortcuts[i].url);
        s.setValue(u"Home"_s, shortcuts[i].home);
        if (!shortcuts[i].icon.isEmpty())
            s.setValue(u"Icon"_s, shortcuts[i].icon);
        s.setValue(u"Suggest"_s, shortcuts[i].suggest); // even empty: none, as its user left it
    }
    s.endArray();
    s.setValue(u"WebSearch/Defaults"_s, kWebDefaultsVersion); // the list as its user left it, new defaults seen
    s.setValue(u"WebSearch/Suggestions"_s, suggestions); // within the group: written again with it
}

// The file may have been edited by hand: addresses are tidied as the settings
// window would ("github.com/..." gets https://), an entry without a name takes
// its keyword, and one that cannot be used (no keyword to type, an address
// Win顺 does not open) is left out.
WebShortcuts readWebShortcuts(QSettings& s, const WebShortcuts& fallback, bool suggestions)
{
    if (!s.contains(u"WebSearch/size"_s)) { // never written: the defaults, and into the file with them
        writeWebShortcuts(s, fallback, suggestions);
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
        if (const QString icon = s.value(u"Icon"_s).toString(); isValidCustomIconName(icon))
            shortcut.icon = icon; // a name of ours, never a path
        if (!isValidWebKeyword(shortcut.keyword) || shortcut.url.isEmpty())
            continue; // nothing to type, or nothing to open
        // No Suggest= at all: written before there were suggestions.
        shortcut.suggest = s.contains(u"Suggest"_s) ? normalizeSuggestUrl(s.value(u"Suggest"_s).toString())
                                                    : knownSuggestUrl(shortcut.url);
        if (shortcut.name.isEmpty())
            shortcut.name = shortcut.keyword;
        shortcuts.append(shortcut);
    }
    s.endArray();
    // Written by an older Win顺: the defaults added since, once.
    if (s.value(u"WebSearch/Defaults"_s, 1).toInt() < kWebDefaultsVersion) {
        shortcuts = withNewDefaults(shortcuts, fallback);
        writeWebShortcuts(s, shortcuts, suggestions);
    }
    return shortcuts;
}

// The built-in file types: id, name (to translate), extensions. As
// Everything groups them, less what few people look for; 代码 last, the
// least asked for.
struct BuiltInFileType {
    const char* id;
    const char* name;
    const char* extensions;
};
constexpr BuiltInFileType kBuiltInFileTypes[] {
    {"documents", QT_TRANSLATE_NOOP("ws::Settings", "文档"),
        "doc docx docm dot dotx rtf odt wps wpd txt md pdf xls xlsx xlsm xlsb csv ods et ppt pptx pptm pps ppsx odp dps "
        "vsd vsdx one pages numbers key"},
    {"pictures", QT_TRANSLATE_NOOP("ws::Settings", "图片"),
        "jpg jpeg jfif png gif bmp webp heic heif avif tif tiff ico svg psd ai raw cr2 cr3 nef arw dng orf rw2"},
    {"videos", QT_TRANSLATE_NOOP("ws::Settings", "视频"),
        "mp4 mkv avi mov wmv flv webm m4v mpg mpeg ts m2ts 3gp rm rmvb vob"},
    {"audio", QT_TRANSLATE_NOOP("ws::Settings", "音频"),
        "mp3 wav flac aac m4a ogg opus wma ape aiff alac mid midi amr"},
    {"archives", QT_TRANSLATE_NOOP("ws::Settings", "压缩包"), "zip rar 7z tar gz tgz bz2 xz zst iso cab lzh arj"},
    {"programs", QT_TRANSLATE_NOOP("ws::Settings", "程序"), "exe msi msix appx bat cmd ps1 vbs com lnk"},
    {"ebooks", QT_TRANSLATE_NOOP("ws::Settings", "电子书"), "epub mobi azw azw3 fb2 djvu chm cbz cbr"},
    {"code", QT_TRANSLATE_NOOP("ws::Settings", "代码"),
        "c h cpp hpp cc cxx cs java kt py js mjs ts jsx tsx go rs swift rb php lua html htm css scss json xml yaml "
        "yml toml ini sql sh ps1 qml vue"},
};

void writeFileTypes(QSettings& s, const FileTypes& types)
{
    s.remove(u"FileTypes"_s); // no entries left over from a longer list
    s.beginWriteArray(u"FileTypes"_s, static_cast<int>(types.size()));
    for (int i = 0; i < types.size(); ++i) {
        s.setArrayIndex(i);
        if (!types[i].id.isEmpty())
            s.setValue(u"Id"_s, types[i].id);
        if (!types[i].name.isEmpty())
            s.setValue(u"Name"_s, types[i].name);
        s.setValue(u"Extensions"_s, types[i].extensions.join(u' '));
        s.setValue(u"Keyword"_s, types[i].keyword);
        if (!types[i].folder.isEmpty())
            s.setValue(u"Folder"_s, types[i].folder);
    }
    s.endArray();
}

void writeQuickFilters(QSettings& s, const QuickFilters& filters)
{
    s.remove(u"QuickFilters"_s); // no entries left over from a longer list
    if (filters.isEmpty())
        return;
    s.beginWriteArray(u"QuickFilters"_s, static_cast<int>(filters.size()));
    for (int i = 0; i < filters.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue(u"Name"_s, filters[i].name);
        s.setValue(u"Query"_s, filters[i].query);
        if (!filters[i].keyword.isEmpty())
            s.setValue(u"Keyword"_s, filters[i].keyword);
    }
    s.endArray();
}

// Edited by hand perhaps: one with no name or no query left out, and a name
// a second time (any case). A query that cannot be read stays, to be put
// right in the settings; the search passes it over. A keyword that cannot
// be one, or is taken (`keywords`: the kinds of file's and the folders',
// and each filter's as it is read), is none.
QuickFilters readQuickFilters(QSettings& s, QSet<QString> keywords)
{
    QuickFilters filters;
    const int size = s.beginReadArray(u"QuickFilters"_s);
    for (int i = 0; i < size; ++i) {
        s.setArrayIndex(i);
        QuickFilter filter {s.value(u"Name"_s).toString().simplified(), s.value(u"Query"_s).toString().trimmed(),
            normalizedTypeKeyword(s.value(u"Keyword"_s).toString()).value_or(QString())};
        if (keywords.contains(filter.keyword))
            filter.keyword.clear();
        const bool named = std::ranges::any_of(filters, [&](const QuickFilter& f) { return f.name.compare(filter.name, Qt::CaseInsensitive) == 0; });
        if (!filter.name.isEmpty() && !filter.query.isEmpty() && !named) {
            filters.append(filter);
            if (!filter.keyword.isEmpty())
                keywords.insert(filter.keyword);
        }
    }
    s.endArray();
    return filters;
}

// Edited by hand perhaps: extensions tidied (".JPG," is jpg), one with no
// name to show or no extension left out.
FileTypes readFileTypes(QSettings& s, const FileTypes& fallback)
{
    if (!s.contains(u"FileTypes/size"_s)) { // never written: the defaults, and into the file with them
        writeFileTypes(s, fallback);
        return fallback;
    }
    FileTypes types;
    const int size = s.beginReadArray(u"FileTypes"_s);
    for (int i = 0; i < size; ++i) {
        s.setArrayIndex(i);
        FileType type;
        type.id = s.value(u"Id"_s).toString().trimmed();
        type.name = s.value(u"Name"_s).toString().trimmed();
        type.extensions = Settings::fileExtensions(s.value(u"Extensions"_s).toString());
        const bool builtIn = std::ranges::any_of(kBuiltInFileTypes, [&](const BuiltInFileType& b) { return type.id == QLatin1StringView(b.id); });
        if (!builtIn)
            type.id.clear();
        type.keyword = normalizedTypeKeyword(s.value(u"Keyword"_s, type.id).toString()).value_or(QString());
        type.folder = pathtext::pathFromText(s.value(u"Folder"_s).toString());
        if ((type.name.isEmpty() && !builtIn) || type.extensions.isEmpty())
            continue;
        types.append(type);
    }
    s.endArray();
    return types;
}

void writeShares(QSettings& s, const QList<NetworkShare>& shares)
{
    s.remove(u"Shares"_s);
    s.beginWriteArray(u"Shares"_s, static_cast<int>(shares.size()));
    for (int i = 0; i < shares.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue(u"Path"_s, shares[i].path);
        s.setValue(u"Name"_s, shares[i].name);
        if (shares[i].content)
            s.setValue(u"Content"_s, true);
        const ShareScan& scan = shares[i].scan;
        if (scan.scheduled)
            s.setValue(u"ScanScheduled"_s, true);
        s.setValue(u"ScanFrom"_s, scan.fromMinute);
        s.setValue(u"ScanUntil"_s, scan.untilMinute);
        if (scan.maxDepth > 0)
            s.setValue(u"ScanDepth"_s, scan.maxDepth);
    }
    s.endArray();
}

// Paths written by hand are tidied (unc::canonical; a mapped drive becomes
// the share it maps); what is no share at all, or twice, is left out.
QList<NetworkShare> readShares(QSettings& s)
{
    if (!s.contains(u"Shares/size"_s)) {
        writeShares(s, {});
        return {};
    }
    QList<NetworkShare> shares;
    const int size = s.beginReadArray(u"Shares"_s);
    for (int i = 0; i < size; ++i) {
        s.setArrayIndex(i);
        const QString path = Settings::sharePath(s.value(u"Path"_s).toString());
        if (path.isEmpty()
            || std::ranges::any_of(shares, [&](const NetworkShare& n) { return unc::equal(n.path.toStdWString(), path.toStdWString()); }))
            continue;
        ShareScan scan;
        scan.scheduled = s.value(u"ScanScheduled"_s, false).toBool();
        scan.fromMinute = std::clamp(s.value(u"ScanFrom"_s, scan.fromMinute).toInt(), 0, 1439);
        scan.untilMinute = std::clamp(s.value(u"ScanUntil"_s, scan.untilMinute).toInt(), 0, 1439);
        scan.maxDepth = std::clamp(s.value(u"ScanDepth"_s, 0).toInt(), 0, 1024);
        shares.append({path, s.value(u"Name"_s).toString().trimmed(), s.value(u"Content"_s, false).toBool(), scan});
    }
    s.endArray();
    return shares;
}

constexpr QStringView kPluginGroup = u"Plugin."; // [Plugin.<id>]

// [Plugin.<id>] sections, as they are: nothing is written back that was
// not there (no defaults), and nothing dropped that is.
QList<PluginConfig> readPlugins(QSettings& s)
{
    QList<PluginConfig> plugins;
    for (const QString& group : s.childGroups()) {
        if (!group.startsWith(kPluginGroup) || group.size() <= kPluginGroup.size())
            continue;
        PluginConfig config;
        config.id = group.mid(kPluginGroup.size());
        s.beginGroup(group);
        for (const QString& key : s.childKeys()) {
            const QString value = s.value(key).toString();
            if (key == u"Enabled")
                config.enabled = s.value(key).toBool();
            else if (key == u"Confirmed")
                config.confirmed = s.value(key).toStringList();
            else if (key == u"Ai")
                config.ai = s.value(key).toBool();
            else if (key.startsWith(u"Keyword."))
                config.keywords.append({key.mid(8), value.trimmed()});
            else if (key.startsWith(u"Setting."))
                config.settings.append({key.mid(8), value});
        }
        s.endGroup();
        plugins.append(std::move(config));
    }
    return plugins;
}

void writePlugins(QSettings& s, const QList<PluginConfig>& plugins)
{
    for (const QString& group : s.childGroups()) {
        if (group.startsWith(kPluginGroup))
            s.remove(group); // sections of plugins the list no longer has
    }
    for (const PluginConfig& config : plugins) {
        s.beginGroup(kPluginGroup + config.id);
        s.setValue(u"Enabled"_s, config.enabled);
        if (!config.confirmed.isEmpty())
            s.setValue(u"Confirmed"_s, config.confirmed);
        if (!config.ai)
            s.setValue(u"Ai"_s, false);
        for (const auto& [command, keyword] : config.keywords)
            s.setValue(u"Keyword."_s + command, keyword);
        for (const auto& [key, value] : config.settings)
            s.setValue(u"Setting."_s + key, value);
        s.endGroup();
    }
}

} // namespace

const PluginConfig* Settings::plugin(QStringView id) const
{
    const auto it = std::find_if(plugins.cbegin(), plugins.cend(), [&](const PluginConfig& p) { return p.id == id; });
    return it == plugins.cend() ? nullptr : &*it;
}

const WebShortcuts& Settings::webShortcutsInUse() const
{
    static const WebShortcuts empty;
    const auto* config = plugin(webplugin::kId);
    return config && config->enabled ? webShortcuts : empty;
}

PluginConfig& Settings::pluginConfig(const QString& id)
{
    const auto it = std::find_if(plugins.begin(), plugins.end(), [&](const PluginConfig& p) { return p.id == id; });
    if (it != plugins.end())
        return *it;
    PluginConfig config;
    config.id = id;
    plugins.append(std::move(config));
    return plugins.last();
}

QString Settings::sharePath(const QString& text)
{
    // A WebDAV folder's web address too ("https://nas:5006/dav/"): as WebClient opens it.
    if (const std::wstring dav = webdav::uncFromUrl(text.trimmed().toStdWString()); !dav.empty())
        return QString::fromStdWString(dav);
    const QString parsed = pathtext::pathFromText(text);
    std::wstring path = (parsed.isEmpty() ? QDir::toNativeSeparators(text.trimmed()) : parsed).toStdWString();
    if (path.size() >= 2 && path[1] == L':')
        path = unc::fromMappedDrive(path);
    // GitHub #114: a WebDAV folder in one form, however it was written, so
    // that one added already is seen to be ("\\nas@SSL\DavWWWRoot\dav" is "\\nas@SSL\dav").
    return QString::fromStdWString(unc::canonical(path));
}

bool Settings::isNetworkPath(const QString& path)
{
    const std::wstring p = win32::expandEnvironment(QDir::toNativeSeparators(path.trimmed()).toStdWString());
    if (unc::isUnc(p))
        return true;
    if (p.size() < 2 || p[1] != L':' || !std::iswalpha(p[0]))
        return false;
    // The letter's type as the process's drive map has it: nothing is read.
    const wchar_t root[] {p[0], L':', L'\\', L'\0'};
    const UINT type = ::GetDriveTypeW(root);
    // An elevated Win顺 does not see the user's mapped letters: as Windows remembers them.
    return type == DRIVE_REMOTE || (type == DRIVE_NO_ROOT_DIR && !unc::fromMappedDrive(std::wstring(root, 2)).empty());
}

std::vector<Settings::KnownFolder> Settings::knownFolders()
{
    struct Known {
        const KNOWNFOLDERID* id;
        const char* name;
    };
    static const Known kKnown[] {
        {&FOLDERID_Desktop, QT_TRANSLATE_NOOP("ws::Settings", "桌面")},
        {&FOLDERID_Documents, QT_TRANSLATE_NOOP("ws::Settings", "文档")},
        {&FOLDERID_Downloads, QT_TRANSLATE_NOOP("ws::Settings", "下载")},
        {&FOLDERID_Pictures, QT_TRANSLATE_NOOP("ws::Settings", "图片")},
        {&FOLDERID_Videos, QT_TRANSLATE_NOOP("ws::Settings", "视频")},
        {&FOLDERID_Music, QT_TRANSLATE_NOOP("ws::Settings", "音乐")},
    };
    std::vector<KnownFolder> known;
    const auto add = [&known](const QString& name, QString path) {
        path = QDir::toNativeSeparators(path.trimmed());
        while (path.size() > 3 && path.endsWith(u'\\'))
            path.chop(1);
        if (path.isEmpty()
            || std::ranges::any_of(known, [&](const KnownFolder& k) { return k.path.compare(path, Qt::CaseInsensitive) == 0; }))
            return;
        const bool network = isNetworkPath(path);
        if (!network) { // one on a server is not looked for (that can take long)
            const DWORD attributes = ::GetFileAttributesW(reinterpret_cast<const wchar_t*>(path.utf16()));
            if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
                return;
        }
        known.push_back({name.isEmpty() ? path : name, path, network});
    };
    for (const Known& k : kKnown) {
        PWSTR folder = nullptr;
        // Not checked by Windows: that looks on the server for a folder redirected there.
        if (SUCCEEDED(::SHGetKnownFolderPath(*k.id, KF_FLAG_DONT_VERIFY, nullptr, &folder)))
            add(QCoreApplication::translate("ws::Settings", k.name), QString::fromWCharArray(folder));
        ::CoTaskMemFree(folder);
    }
    // OneDrive's roots, personal and work, named as they are ("OneDrive - Contoso").
    for (const wchar_t* variable : {L"OneDrive", L"OneDriveCommercial"}) {
        wchar_t value[MAX_PATH] {};
        if (const DWORD n = ::GetEnvironmentVariableW(variable, value, MAX_PATH); n > 0 && n < MAX_PATH) {
            const QString path = QString::fromWCharArray(value, static_cast<qsizetype>(n));
            add(QDir(QDir::fromNativeSeparators(path)).dirName(), path);
        }
    }
    return known;
}

QStringList Settings::defaultIndexFolders()
{
    std::vector<KnownFolder> known = knownFolders();
    std::erase_if(known, [](const KnownFolder& k) { return k.network; });
    const auto inside = [](const QString& path, const QString& folder) {
        const QString prefix = folder.endsWith(u'\\') ? folder : folder + u'\\';
        return path.size() > folder.size() && path.startsWith(prefix, Qt::CaseInsensitive);
    };
    QStringList folders;
    for (const KnownFolder& k : known) {
        // In another one: OneDrive's root holds the desktop and documents it backs up.
        if (std::ranges::none_of(known, [&](const KnownFolder& o) { return inside(k.path, o.path); }))
            folders.push_back(k.path);
    }
    return folders;
}

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

void Settings::useProfile(const QString& folder)
{
    g_profile = QDir::toNativeSeparators(QDir(folder).absolutePath());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, g_profile); // <folder>\WinShun\WinShun.ini
}

QString Settings::profile()
{
    return g_profile;
}

QString Settings::portableFolder()
{
    wchar_t exe[MAX_PATH * 2] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (length == 0 || length >= std::size(exe))
        return {};
    const QString folder = QFileInfo(QString::fromWCharArray(exe, static_cast<qsizetype>(length))).absolutePath()
        + u"/data"_s;
    return QFileInfo(folder).isDir() ? QDir::toNativeSeparators(folder) : QString();
}

QString Settings::dataDir()
{
    if (!g_profile.isEmpty())
        return g_profile;
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
    // The language Windows itself is shown in (core/ExplorerLanguage.h).
    return explorerlang::windowsIsChinese() ? u"zh"_s : u"en"_s;
}

QString Settings::quickFilterProblem(const QString& query)
{
    const ParsedQuery parsed = parseQuery(query);
    if (!parsed.error.isEmpty())
        return parsed.error;
    if (parsed.isEmpty())
        return QCoreApplication::translate("ws::Settings", "请填写筛选条件，例如 !$RECYCLE.BIN\\ 或 ext:pdf size:>1mb");
    return {};
}

FileTypes Settings::defaultFileTypes()
{
    FileTypes types;
    for (const BuiltInFileType& b : kBuiltInFileTypes)
        types.append({{}, QString::fromLatin1(b.extensions).split(u' ', Qt::SkipEmptyParts), QString::fromLatin1(b.id), QString::fromLatin1(b.id)});
    return types;
}

QStringList Settings::fileExtensions(const QString& text)
{
    static const QRegularExpression separators(u"[\\s,;，；、]+"_s);
    QStringList extensions;
    for (QString ext : text.split(separators, Qt::SkipEmptyParts)) {
        while (ext.startsWith(u'*') || ext.startsWith(u'.'))
            ext.remove(0, 1);
        ext = ext.sliced(ext.lastIndexOf(u'.') + 1).toLower(); // "tar.gz": what comes after the last dot
        // What no name ends in (a path, a pattern) is no extension.
        if (ext.isEmpty() || ext.contains(u'\\') || ext.contains(u'/') || ext.contains(u':') || ext.contains(u'*')
            || ext.contains(u'?') || extensions.contains(ext))
            continue;
        extensions.append(ext);
    }
    return extensions;
}

QStringList Settings::fileNamePatterns(const QString& text)
{
    static const QRegularExpression separators(u"[,;，；、\\r\\n]+"_s);
    QStringList patterns;
    for (QString pattern : text.split(separators, Qt::SkipEmptyParts)) {
        pattern = pattern.trimmed();
        // Wildcards alone ("*", "*.*") would be every file.
        const bool everyFile = std::ranges::all_of(pattern, [](QChar c) { return c == u'*' || c == u'?' || c == u'.'; });
        if (everyFile || pattern.contains(u'\\') || pattern.contains(u'/') || pattern.contains(u':')
            || patterns.contains(pattern, Qt::CaseInsensitive))
            continue;
        patterns.append(pattern);
    }
    return patterns;
}

QStringList Settings::contentExtensionList(const QString& text)
{
    static const QRegularExpression separators(u"[\\s,;，；、]+"_s);
    QStringList extensions;
    for (QString ext : text.split(separators, Qt::SkipEmptyParts)) {
        while (ext.startsWith(u'*') || ext.startsWith(u'.'))
            ext.remove(0, 1);
        ext = ext.toLower();
        if (!ext.isEmpty() && !extensions.contains(ext))
            extensions.append(ext);
    }
    return extensions;
}

QString Settings::programFileName(const QString& text)
{
    QString app = QFileInfo(text.trimmed()).fileName();
    if (!app.isEmpty() && !app.endsWith(u".exe", Qt::CaseInsensitive))
        app += u".exe"_s;
    return app;
}

QString Settings::excludedFolderName(const QString& text)
{
    QString name = QDir::fromNativeSeparators(text.trimmed());
    while (name.endsWith(u'/'))
        name.chop(1);
    return name;
}

QString Settings::normalizedHotkey(const QString& shortcut)
{
    return hotkeys::normalized(shortcut);
}

bool Settings::hotkeyAllowed(const QString& shortcut)
{
    return hotkeys::allowed(shortcut);
}

FileTypes Settings::namedFileTypes() const
{
    FileTypes types = fileTypes;
    for (FileType& type : types) {
        if (!type.name.isEmpty())
            continue;
        for (const BuiltInFileType& b : kBuiltInFileTypes) {
            if (type.id == QLatin1StringView(b.id))
                type.name = QCoreApplication::translate("ws::Settings", b.name);
        }
    }
    return types;
}

QStringList Settings::defaultPreferredTypes()
{
    return kDefaultPreferredTypes;
}

Settings Settings::defaults()
{
    Settings d;
    d.excludedPaths = kDefaultExcludedPaths;
    d.excludedNames = kDefaultExcludedNames;
    d.excludedFileNames = {u"~$*"_s}; // Office's and WPS's lock files
    d.indexFolders = defaultIndexFolders();
    d.contentExtensions = kDefaultContentExtensions;
    d.preferredTypes = defaultPreferredTypes();
    d.clipboard = windowsClipboardHistory(); // whoever kept Windows' clipboard history keeps ours
    d.clipboardExcludedApps = kDefaultClipboardExcludedApps;
    d.webShortcuts = defaultWebShortcuts();
    d.fileTypes = defaultFileTypes();
    d.networkExcludedNames = kDefaultNetworkExcludedNames;
    d.networkExcludedFiles = kDefaultNetworkExcludedFiles;
    d.renderer = defaultRenderer();
    d.transparency = lowMemory() ? u"off"_s : u"on"_s;
    d.corners = QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11 ? u"round"_s : u"square"_s;
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
    // Before anything below writes to it: nothing in it, a new install.
    const bool firstRun = s.allKeys().isEmpty();
    onboardingComplete = readOrDefault(s, u"Setup/Completed"_s, !firstRun);
    onboardingCapturePending = readOrDefault(s, u"Setup/CapturePending"_s, false);

    doubleCtrl = readOrDefault(s, u"Launcher/DoubleCtrl"_s, d.doubleCtrl);
    doubleTapAction = readOrDefault(s, u"Launcher/DoubleTapAction"_s, d.doubleTapAction);
    if (!doubletap::valid(doubleTapAction))
        doubleTapAction = d.doubleTapAction;
    doubleTapKey = readOrDefault(s, u"Launcher/DoubleTapKey"_s, d.doubleTapKey).trimmed().toLower();
    if (doubleTapKey != u"alt" && doubleTapKey != u"shift")
        doubleTapKey = d.doubleTapKey;
    doubleCtrlKey = readOrDefault(s, u"Launcher/DoubleCtrlKey"_s, d.doubleCtrlKey).trimmed().toLower();
    if (doubleCtrlKey != u"left" && doubleCtrlKey != u"right")
        doubleCtrlKey = d.doubleCtrlKey;
    doubleCtrlPauseInGames = readOrDefault(s, u"Launcher/DoubleCtrlPauseInGames"_s, d.doubleCtrlPauseInGames);
    doubleCtrlPauseInFullScreen
        = readOrDefault(s, u"Launcher/DoubleCtrlPauseInFullScreen"_s, d.doubleCtrlPauseInFullScreen);
    doubleCtrlExcludedApps = readOrDefault(s, u"Launcher/DoubleCtrlExcludedApps"_s, d.doubleCtrlExcludedApps);
    hotkey = readOrDefault(s, u"Launcher/Hotkey"_s, d.hotkey);
    panelHotkey = readOrDefault(s, u"Launcher/PanelHotkey"_s, d.panelHotkey).trimmed();
    adminTapHinted = readOrDefault(s, u"Launcher/AdminTapHinted"_s, d.adminTapHinted);
    adminDialogHinted = readOrDefault(s, u"Launcher/AdminDialogHinted"_s, d.adminDialogHinted);
    serviceUpdateHinted = readOrDefault(s, u"Launcher/ServiceUpdateHinted"_s, d.serviceUpdateHinted).trimmed();
    shortcutAgain = readOrDefault(s, u"Launcher/ShortcutAgain"_s, d.shortcutAgain).trimmed().toLower();
    if (shortcutAgain != u"panel")
        shortcutAgain = d.shortcutAgain;
    renderer = resolveRenderer(readOrDefault(s, u"Launcher/Renderer"_s, d.renderer).trimmed().toLower());
    proxyMode = readOrDefault(s, u"Proxy/Mode"_s, d.proxyMode).trimmed().toLower();
    if (proxyMode != u"none" && proxyMode != u"manual")
        proxyMode = d.proxyMode;
    proxyAddress = readOrDefault(s, u"Proxy/Address"_s, d.proxyAddress).trimmed();
    recordHistory = readOrDefault(s, u"Launcher/History"_s, d.recordHistory);
    rememberAdmin = readOrDefault(s, u"Launcher/RememberAdmin"_s, d.rememberAdmin);
    mergeFileScope = readOrDefault(s, u"Results/MergeFileScope"_s, d.mergeFileScope);
    appsTab = readOrDefault(s, u"Results/AppsTab"_s, d.appsTab);
    queryHistoryKeys = readOrDefault(s, u"Launcher/QueryHistoryKeys"_s, d.queryHistoryKeys);
    if (queryHistoryKeys != u"ctrl" && queryHistoryKeys != u"alt" && queryHistoryKeys != u"off")
        queryHistoryKeys = d.queryHistoryKeys;
    logoRightClick = readOrDefault(s, u"Launcher/LogoRightClick"_s, d.logoRightClick);
    if (logoRightClick != u"menu" && logoRightClick != u"top" && logoRightClick != u"center" && logoRightClick != u"restore")
        logoRightClick = d.logoRightClick;
    dialogJump = readOrDefault(s, u"Launcher/DialogJump"_s, d.dialogJump);
    // Off for new installs; an ini from before without the key had it on.
    dialogBar = readOrDefault(s, u"Launcher/DialogBar"_s, firstRun ? d.dialogBar : true);
    dialogBarPlace = readOrDefault(s, u"Launcher/DialogBarPlace"_s, d.dialogBarPlace).trimmed().toLower();
    if (dialogBarPlace != u"below" && dialogBarPlace != u"left" && dialogBarPlace != u"right")
        dialogBarPlace = d.dialogBarPlace;
    dialogBarFolderOnly = readOrDefault(s, u"Launcher/DialogBarFolderOnly"_s, d.dialogBarFolderOnly);
    dialogAutoJump = readOrDefault(s, u"Launcher/DialogAutoJump"_s, d.dialogAutoJump);
    dialogAutoJumpWhen = readOrDefault(s, u"Launcher/DialogAutoJumpWhen"_s, d.dialogAutoJumpWhen).trimmed().toLower();
    if (dialogAutoJumpWhen != u"back")
        dialogAutoJumpWhen = d.dialogAutoJumpWhen;
    // Turned on before there was the question: no need to ask.
    dialogAutoJumpAsked = readOrDefault(s, u"Launcher/DialogAutoJumpAsked"_s, dialogAutoJump);
    dialogBarExcludedApps = readOrDefault(s, u"Launcher/DialogBarExcludedApps"_s, d.dialogBarExcludedApps);
    fileManager = readOrDefault(s, u"Launcher/FileManager"_s, d.fileManager).trimmed().toLower();
    if (fileManager != u"totalcmd" && fileManager != u"dopus" && fileManager != u"xyplorer" && fileManager != u"pulse"
        && fileManager != u"onecommander" && fileManager != u"custom")
        fileManager = d.fileManager;
    fileManagerOpenCommand = readOrDefault(s, u"Launcher/FileManagerOpenCommand"_s, d.fileManagerOpenCommand).trimmed();
    fileManagerRevealCommand = readOrDefault(s, u"Launcher/FileManagerRevealCommand"_s, d.fileManagerRevealCommand).trimmed();
    explorerMenu = readOrDefault(s, u"Launcher/ExplorerMenu"_s, d.explorerMenu);
    explorerTyping = readOrDefault(s, u"Launcher/ExplorerTyping"_s, d.explorerTyping);
    desktopTyping = readOrDefault(s, u"Launcher/DesktopTyping"_s, d.desktopTyping);
    opusTyping = readOrDefault(s, u"Launcher/OpusTyping"_s, d.opusTyping);
    opusTypingScope = readOrDefault(s, u"Launcher/OpusTypingScope"_s, d.opusTypingScope).trimmed().toLower();
    if (opusTypingScope != u"all")
        opusTypingScope = d.opusTypingScope;
    folderTyping = readOrDefault(s, u"Launcher/FolderTyping"_s, d.folderTyping).trimmed().toLower();
    if (folderTyping != u"only" && folderTyping != u"level")
        folderTyping = d.folderTyping; // the folder first (GitHub #110), for those who had nothing to set before too
    // Off for new installs, auto before (as dialogBar).
    const QString folderMenuBefore = firstRun ? d.folderMenu : u"auto"_s;
    folderMenu = readOrDefault(s, u"Launcher/FolderMenu"_s, folderMenuBefore).trimmed().toLower();
    if (folderMenu != u"auto" && folderMenu != u"on" && folderMenu != u"off")
        folderMenu = folderMenuBefore;
    folderMenuExplorer = readOrDefault(s, u"Launcher/FolderMenuExplorer"_s, d.folderMenuExplorer);
    folderMenuDesktop = readOrDefault(s, u"Launcher/FolderMenuDesktop"_s, d.folderMenuDesktop);
    folderMenuDouble = readOrDefault(s, u"Launcher/FolderMenuDouble"_s, d.folderMenuDouble);
    folderMenuMiddle = readOrDefault(s, u"Launcher/FolderMenuMiddle"_s, d.folderMenuMiddle);
    if (!folderMenuDouble && !folderMenuMiddle) // no way left to open it: the double click back (#196)
        folderMenuDouble = true;
    if (!folderMenuExplorer && !folderMenuDesktop) // nowhere left: both back
        folderMenuExplorer = folderMenuDesktop = true;
    folderMenuFavorites = readOrDefault(s, u"Launcher/FolderMenuFavorites"_s, d.folderMenuFavorites);
    folderMenuRecent = readOrDefault(s, u"Launcher/FolderMenuRecent"_s, d.folderMenuRecent);
    folderMenuWindows = readOrDefault(s, u"Launcher/FolderMenuWindows"_s, d.folderMenuWindows);
    folderMenuRecentInline = std::clamp(readOrDefault(s, u"Launcher/FolderMenuRecentInline"_s, d.folderMenuRecentInline), 0, 10);
    folderMenuWindowsInline = std::clamp(readOrDefault(s, u"Launcher/FolderMenuWindowsInline"_s, d.folderMenuWindowsInline), 0, 10);
    if (!folderMenuFavorites && !folderMenuRecent && !folderMenuWindows) // a menu of nothing: back to all (#51)
        folderMenuFavorites = folderMenuRecent = folderMenuWindows = true;
    folderMenuItemSearch = readOrDefault(s, u"Launcher/FolderMenuItemSearch"_s, d.folderMenuItemSearch);
    folderMenuItemCopyPath = readOrDefault(s, u"Launcher/FolderMenuItemCopyPath"_s, d.folderMenuItemCopyPath);
    folderMenuItemCmd = readOrDefault(s, u"Launcher/FolderMenuItemCmd"_s, d.folderMenuItemCmd);
    folderMenuItemPowerShell = readOrDefault(s, u"Launcher/FolderMenuItemPowerShell"_s, d.folderMenuItemPowerShell);
    folderMenuItemFavorite = readOrDefault(s, u"Launcher/FolderMenuItemFavorite"_s, d.folderMenuItemFavorite);
    folderMenuItemSettings = readOrDefault(s, u"Launcher/FolderMenuItemSettings"_s, d.folderMenuItemSettings);
    folderMenuHotkey = readOrDefault(s, u"Launcher/FolderMenuHotkey"_s, d.folderMenuHotkey).trimmed();
    folderMenuOpusBefore = readOrDefault(s, u"Launcher/FolderMenuOpusBefore"_s, d.folderMenuOpusBefore);
    dropPie = readOrDefault(s, u"Favorites/DropPie"_s, d.dropPie);
    dropPieKeyTakenTold = readOrDefault(s, u"Favorites/DropPieKeyTakenTold"_s, d.dropPieKeyTakenTold);

    foldersFirst = readOrDefault(s, u"Results/FoldersFirst"_s, d.foldersFirst);
    // In the file only when changed (save): one never changed follows the
    // default as later versions set it. As written by hand too: ".EXE; bat"
    // is exe and bat; empty, none.
    preferredTypes = s.contains(kPreferredTypesKey)
        ? fileExtensions(s.value(kPreferredTypesKey).toStringList().join(u' '))
        : d.preferredTypes;
    if (preferredTypes == d.preferredTypes)
        s.remove(kPreferredTypesKey); // as the first build with it wrote it
    showModified = readOrDefault(s, u"Results/ShowModified"_s, d.showModified);
    showModifiedTime = readOrDefault(s, u"Results/ShowModifiedTime"_s, d.showModifiedTime);
    showFileSize = readOrDefault(s, u"Results/ShowFileSize"_s, d.showFileSize);
    folderKeyword = normalizedTypeKeyword(readOrDefault(s, u"Results/FolderKeyword"_s, d.folderKeyword)).value_or(d.folderKeyword);
    doubleClickToOpen = readOrDefault(s, u"Results/DoubleClickToOpen"_s, d.doubleClickToOpen);
    tabEntersFavoriteFolder = readOrDefault(s, u"Results/TabEntersFavoriteFolder"_s, d.tabEntersFavoriteFolder);
    thumbnails = readOrDefault(s, u"Results/Thumbnails"_s, d.thumbnails);
    rowDeleteButton = readOrDefault(s, u"Results/DeleteButton"_s, d.rowDeleteButton);
    searchApps = readOrDefault(s, u"Results/SearchApps"_s, d.searchApps);
    searchSystem = readOrDefault(s, u"Results/SearchSystem"_s, d.searchSystem);
    regexMode = readOrDefault(s, u"Results/RegexMode"_s, d.regexMode);
    if (!s.contains(u"Results/Sort"_s) && s.value(u"Results/SortByModified"_s).toBool())
        s.setValue(u"Results/Sort"_s, u"newest"_s); // 0.6.0's switch: newest first, or best match
    s.remove(u"Results/SortByModified"_s);
    sort = readOrDefault(s, u"Results/Sort"_s, d.sort).trimmed().toLower();
    if (!isSortName(sort))
        sort = d.sort;
    startList = readOrDefault(s, u"Results/StartList"_s, d.startList).trimmed().toLower();
    recentItems = std::clamp(readOrDefault(s, u"Results/RecentItems"_s, d.recentItems), 1, 200);
    favoritesOnStart = readOrDefault(s, u"Results/FavoritesOnStart"_s, d.favoritesOnStart);
    if (startList != u"modified" && startList != u"none")
        startList = d.startList;
    clearOnOpen = readOrDefault(s, u"Launcher/ClearOnOpen"_s, d.clearOnOpen);
    englishOnOpen = readOrDefault(s, u"Launcher/EnglishOnOpen"_s, d.englishOnOpen);
    numberKeys = readOrDefault(s, u"Results/NumberKeys"_s, d.numberKeys).trimmed().toLower();
    if (numberKeys != u"alt" && numberKeys != u"off")
        numberKeys = d.numberKeys;
    numberHints = readOrDefault(s, u"Results/NumberHints"_s, d.numberHints).trimmed().toLower();
    if (numberHints != u"held" && numberHints != u"always" && numberHints != u"off")
        numberHints = d.numberHints;
    shortcutHelp = readOrDefault(s, u"Results/ShortcutHelp"_s, d.shortcutHelp);
    footerHints = readOrDefault(s, u"Results/FooterHints"_s, d.footerHints);
    pureSearch = readOrDefault(s, u"Results/PureSearch"_s, d.pureSearch);
    panelListsAll = readOrDefault(s, u"Panel/ListAll"_s, d.panelListsAll);
    scopeButton = readOrDefault(s, u"Appearance/ScopeButton"_s, d.scopeButton);
    previewWith = readOrDefault(s, u"Results/PreviewWith"_s, d.previewWith).trimmed().toLower();
    if (previewWith != u"builtin")
        previewWith = d.previewWith;
    previewFollowsPointer = readOrDefault(s, u"Results/PreviewFollowsPointer"_s, d.previewFollowsPointer);
    previewRestore = readOrDefault(s, u"Results/PreviewRestore"_s, d.previewRestore);
    previewHotkey = readOrDefault(s, u"Results/PreviewHotkey"_s, d.previewHotkey).trimmed();
    if (!previewHotkey.isEmpty()) {
        previewHotkey = previewkeys::normalized(previewHotkey);
        if (previewHotkey.isEmpty())
            previewHotkey = d.previewHotkey;
    }
    openHotkey = previewkeys::normalizedOpen(readOrDefault(s, u"Results/OpenHotkey"_s, d.openHotkey).trimmed());
    if (openHotkey == previewHotkey)
        openHotkey.clear();

    theme = readOrDefault(s, u"Appearance/Theme"_s, d.theme).trimmed().toLower();
    if (theme != u"light" && theme != u"dark")
        theme = d.theme;
    language = readOrDefault(s, u"Appearance/Language"_s, d.language).trimmed().toLower();
    if (language != u"zh" && language != u"en")
        language = d.language;
    transparency = readOrDefault(s, u"Appearance/Transparency"_s, d.transparency).trimmed().toLower();
    const auto glassPercent = [&s](const QString& key, int fallback) {
        bool valid = false;
        const int value = s.value(key, fallback).toInt(&valid);
        return valid ? std::clamp(value, 0, 100) : fallback;
    };
    glassTransparency = glassPercent(u"Appearance/GlassTransparency"_s, d.glassTransparency);
    glassClarity = glassPercent(u"Appearance/GlassClarity"_s, d.glassClarity);
    // It used to be a switch: "false" was turned off by hand, "true" was mostly
    // the default written back.
    taskbarIcon = readOrDefault(s, u"Appearance/TaskbarIcon"_s, d.taskbarIcon).trimmed().toLower();
    if (taskbarIcon != u"mono")
        taskbarIcon = d.taskbarIcon;
    uiSize = readOrDefault(s, u"Appearance/UiSize"_s, d.uiSize).trimmed().toLower();
    uiFont = readOrDefault(s, u"Appearance/UiFont"_s, d.uiFont).trimmed();
    if (uiSize != u"compact" && uiSize != u"roomy")
        uiSize = d.uiSize;
    maxRows = std::clamp(readOrDefault(s, u"Appearance/MaxRows"_s, d.maxRows), 4, 20);
    corners = readOrDefault(s, u"Appearance/Corners"_s, d.corners).trimmed().toLower();
    if (corners != u"round" && corners != u"square")
        corners = d.corners;
    uiStyle = readOrDefault(s, u"Appearance/UiStyle"_s, d.uiStyle).trimmed().toLower();
    if (uiStyle != u"brand" && uiStyle != u"tactile" && uiStyle != u"liquid")
        uiStyle = d.uiStyle;
    if (transparency == u"false" || transparency == u"true") {
        transparency = transparency == u"false" ? u"off"_s : d.transparency;
        s.setValue(u"Appearance/Transparency"_s, transparency);
    } else if (transparency != u"on" && transparency != u"off") { // also an old "auto"
        transparency = d.transparency;
    }

    autoUpdate = readOrDefault(s, u"Update/Automatic"_s, d.autoUpdate);
    backupFolder = readOrDefault(s, u"Backup/Folder"_s, d.backupFolder);

    excludedPaths = readOrDefault(s, u"Index/ExcludedPaths"_s, d.excludedPaths);
    excludedNames = readOrDefault(s, u"Index/ExcludedNames"_s, d.excludedNames);
    // As written by hand too: ".WBK; bak" is wbk and bak.
    excludedTypes = fileExtensions(readOrDefault(s, u"Index/ExcludedTypes"_s, d.excludedTypes).join(u' '));
    excludedFileNames = fileNamePatterns(readOrDefault(s, u"Index/ExcludedFileNames"_s, d.excludedFileNames).join(u';'));
    includeRemovableDrives = readOrDefault(s, u"Index/IncludeRemovableDrives"_s, d.includeRemovableDrives);
    selectedDrives.reset();
    if (s.contains(u"Index/Drives"_s)) {
        selectedDrives = QStringList{};
        for (QString root : s.value(u"Index/Drives"_s).toStringList()) {
            root = root.trimmed().toUpper();
            if (root.size() == 3 && root.endsWith(u'\\'))
                root.chop(1);
            if (root.size() == 2 && root[0] >= u'A' && root[0] <= u'Z' && root[1] == u':' && !selectedDrives->contains(root))
                selectedDrives->append(root);
        }
    }
    rescanOnStartup = readOrDefault(s, u"Index/RescanOnStartup"_s, d.rescanOnStartup);
    indexAccess = readOrDefault(s, u"Index/Access"_s, d.indexAccess).trimmed().toLower();
    if (indexAccess != u"service" && indexAccess != u"direct" && indexAccess != u"walk")
        indexAccess = d.indexAccess;
    indexFolder = readOrDefault(s, u"Index/Folder"_s, d.indexFolder).trimmed();
    // GitHub #52. Whole disks were all there was: an older file without the
    // key goes on so (written, so that it stays so), a new install starts
    // with auto.
    if (!s.contains(u"Index/Scope"_s)) {
        indexScope = firstRun ? d.indexScope : u"drives"_s;
        s.setValue(u"Index/Scope"_s, indexScope);
    } else {
        indexScope = s.value(u"Index/Scope"_s).toString().trimmed().toLower();
        if (indexScope != u"drives" && indexScope != u"folders")
            indexScope = d.indexScope;
    }
    // As written, nested ones too: an empty list stays empty (no defaults put
    // back). Never written: the account's known folders, as they are now,
    // and not written on reading (GitHub #52 review: frozen there, a known
    // folder moved later was looked for where it was), only once only the
    // chosen folders are indexed (chooseDefaultIndexFolders) or the user
    // edits them.
    indexFoldersChosen = s.contains(u"Index/Folders"_s);
    indexFolders = indexFoldersChosen ? s.value(u"Index/Folders"_s).value<QStringList>() : d.indexFolders;
    folderVerifyHours = std::clamp(readOrDefault(s, u"Index/FolderVerifyHours"_s, d.folderVerifyHours), 0, 24 * 7);
    folderHintDismissed = readOrDefault(s, u"Index/FolderHintDismissed"_s, d.folderHintDismissed);

    contentSearch = readOrDefault(s, u"Content/Enabled"_s, d.contentSearch);
    contentExtensions = readOrDefault(s, u"Content/Extensions"_s, d.contentExtensions);
    const int contentDefaults = s.value(u"Content/DefaultsVersion"_s, 1).toInt();
    if (contentDefaults < kContentDefaultsVersion) {
        // Version 1 defaulted to just "txt". A list the user edited is left
        // alone, and so is "txt" set by hand since.
        if (contentDefaults < 2 && contentExtensions == QStringList {u"txt"_s}) {
            contentExtensions = d.contentExtensions;
            s.setValue(u"Content/Extensions"_s, contentExtensions);
        }
        s.setValue(u"Content/DefaultsVersion"_s, kContentDefaultsVersion);
    }
    // MaxFileSizeMB once held for every kind of text file: a size set there
    // holds for code and data too, until they have their own.
    const auto* oldDefaults = contentDefaults < 3 ? &kContentMaxSizeMBBefore3
        : contentDefaults < 4                     ? &kContentMaxSizeMB3
                                                  : nullptr;
    for (std::size_t k = 0; k < ContentSizeLimits::kKinds; ++k) {
        double fallback = d.contentMaxSizeMB[k];
        if ((k == ContentSizeLimits::Code || k == ContentSizeLimits::Data) && !s.contains(kContentSizeKeys[k])
            && contentMaxSizeMB[0] != d.contentMaxSizeMB[0])
            fallback = contentMaxSizeMB[0];
        double mb = readOrDefault(s, kContentSizeKeys[k], fallback);
        if (oldDefaults && mb == (*oldDefaults)[k]) { // never changed: the new default
            mb = d.contentMaxSizeMB[k];
            s.setValue(kContentSizeKeys[k], mb);
        }
        contentMaxSizeMB[k] = roundedSizeMB(mb);
    }
    // The index used to be on by default, and save() runs only on a change:
    // an older ini without the key had it on, and keeps it (off would clear
    // it). Written either way, so a new install stays new on its next start.
    if (!s.contains(u"Content/Index"_s)) {
        contentIndex = firstRun ? d.contentIndex : true;
        s.setValue(u"Content/Index"_s, contentIndex);
    } else {
        contentIndex = s.value(u"Content/Index"_s).toBool();
    }
    contentInLowPriority = readOrDefault(s, u"Content/IncludeSystemFolders"_s, d.contentInLowPriority);
    contentDocuments = readOrDefault(s, u"Content/Documents"_s, d.contentDocuments);

    clipboard = readOrDefault(s, u"Clipboard/Enabled"_s, d.clipboard);
    clipboardWinV = readOrDefault(s, u"Clipboard/WinV"_s, d.clipboardWinV);
    clipboardHotkey = readOrDefault(s, u"Clipboard/Hotkey"_s, d.clipboardHotkey);
    clipboardMaxItems = readOrDefault(s, u"Clipboard/MaxItems"_s, d.clipboardMaxItems);
    if (clipboardMaxItems != 0) // 0 = no limit (GitHub #32)
        clipboardMaxItems = std::clamp(clipboardMaxItems, 10, 100000);
    clipboardMaxDays = std::clamp(readOrDefault(s, u"Clipboard/MaxDays"_s, d.clipboardMaxDays), 0, 3650);
    clipboardMissingFiles = readOrDefault(s, u"Clipboard/MissingFiles"_s, d.clipboardMissingFiles).trimmed().toLower();
    if (clipboardMissingFiles != u"hide" && clipboardMissingFiles != u"clean")
        clipboardMissingFiles = d.clipboardMissingFiles;
    clipboardImages = readOrDefault(s, u"Clipboard/Images"_s, d.clipboardImages);
    clipboardClickToPaste = readOrDefault(s, u"Clipboard/ClickToPaste"_s, d.clipboardClickToPaste);
    clipboardUiSize = readOrDefault(s, u"Clipboard/UiSize"_s, d.clipboardUiSize).trimmed().toLower();
    if (clipboardUiSize != u"dense" && clipboardUiSize != u"compact" && clipboardUiSize != u"standard" && clipboardUiSize != u"roomy")
        clipboardUiSize = d.clipboardUiSize;
    clipboardLargePictures = readOrDefault(s, u"Clipboard/LargePictures"_s, d.clipboardLargePictures);
    clipboardKeepPlace = readOrDefault(s, u"Clipboard/KeepPlace"_s, d.clipboardKeepPlace);
    clipboardAtPointer = readOrDefault(s, u"Clipboard/AtPointer"_s, d.clipboardAtPointer);
    clipboardPauseInFullScreen = readOrDefault(s, u"Clipboard/PauseInFullScreen"_s, d.clipboardPauseInFullScreen);
    clipboardFlipAbove = readOrDefault(s, u"Clipboard/FlipAbove"_s, d.clipboardFlipAbove);
    clipboardHoldToStep = readOrDefault(s, u"Clipboard/HoldToStep"_s, d.clipboardHoldToStep);
    clipboardPasteElevated = readOrDefault(s, u"Clipboard/PasteElevated"_s, d.clipboardPasteElevated);
    clipboardSearchBoxSide = readOrDefault(s, u"Clipboard/SearchBoxSide"_s, d.clipboardSearchBoxSide).trimmed().toLower();
    if (clipboardSearchBoxSide != u"far")
        clipboardSearchBoxSide = d.clipboardSearchBoxSide;
    clipboardExcludedApps = readOrDefault(s, u"Clipboard/ExcludedApps"_s, d.clipboardExcludedApps);
    clipboardNumberKeys = readOrDefault(s, u"Clipboard/NumberKeys"_s, d.clipboardNumberKeys).trimmed().toLower();
    if (clipboardNumberKeys != u"ctrl" && clipboardNumberKeys != u"off")
        clipboardNumberKeys = d.clipboardNumberKeys;
    clipboardQuickPaste = readOrDefault(s, u"Clipboard/QuickPaste"_s, d.clipboardQuickPaste).trimmed().toLower();
    if (clipboardQuickPaste != u"ctrl" && clipboardQuickPaste != u"alt")
        clipboardQuickPaste = d.clipboardQuickPaste;

    taskbarWinS = readOrDefault(s, u"Taskbar/WinS"_s, d.taskbarWinS);
    taskbarStartTyping = readOrDefault(s, u"Taskbar/StartMenuTyping"_s, d.taskbarStartTyping);
    taskbarSearchBox = readOrDefault(s, u"Taskbar/SearchBox"_s, d.taskbarSearchBox);
    taskbarSearchBoxStyle = readOrDefault(s, u"Taskbar/SearchBoxStyle"_s, d.taskbarSearchBoxStyle).trimmed().toLower();
    if (taskbarSearchBoxStyle != u"label" && taskbarSearchBoxStyle != u"icon")
        taskbarSearchBoxStyle = d.taskbarSearchBoxStyle;
    taskbarSearchBoxSide = readOrDefault(s, u"Taskbar/SearchBoxSide"_s, d.taskbarSearchBoxSide).trimmed().toLower();
    if (taskbarSearchBoxSide != u"tray")
        taskbarSearchBoxSide = d.taskbarSearchBoxSide;
    // 0.7.1 to 0.11: the box right of Start (a DLL in Explorer made room
    // there). The box has one place now (taskbar::SearchBox).
    s.remove(u"Taskbar/StartGap"_s);

    // The rest of [Capture] is the screenshot plugin's (MovedPlugins).
    captureEnabled = readOrDefault(s, u"Capture/Enabled"_s, d.captureEnabled);
    capturePrintScreenBefore = readOrDefault(s, u"Capture/PrintScreenBefore"_s, d.capturePrintScreenBefore).trimmed();
    captureKeyword = readOrDefault(s, u"Capture/Keyword"_s, d.captureKeyword).trimmed();

    pasteFileEnabled = readOrDefault(s, u"PasteFile/Enabled"_s, d.pasteFileEnabled);
    pasteFileCtrlV = readOrDefault(s, u"PasteFile/CtrlV"_s, d.pasteFileCtrlV);
    pasteFileMenu = readOrDefault(s, u"PasteFile/Menu"_s, d.pasteFileMenu);
    if (!pasteFileCtrlV && !pasteFileMenu) // on, and no way to it: back to both
        pasteFileCtrlV = pasteFileMenu = true;

    // Before the list: writing it (defaults new to the file) rewrites the whole group.
    fileTypes = readFileTypes(s, d.fileTypes);
    QSet<QString> typeKeywords{folderKeyword};
    for (FileType& type : fileTypes) {
        if (typeKeywords.contains(type.keyword))
            type.keyword.clear();
        else if (!type.keyword.isEmpty())
            typeKeywords.insert(type.keyword);
    }

    quickFilters = readQuickFilters(s, typeKeywords);
    shares = readShares(s);
    networkVerifyHours = std::clamp(readOrDefault(s, u"Network/VerifyHours"_s, d.networkVerifyHours), 0, 24 * 7);
    networkVerifyHoursUnwatched
        = std::clamp(readOrDefault(s, u"Network/VerifyHoursUnwatched"_s, d.networkVerifyHoursUnwatched), 0, 24 * 7);
    networkThreads = std::clamp(readOrDefault(s, u"Network/Threads"_s, d.networkThreads), 1, 64);
    networkExcludedNames = readOrDefault(s, u"Network/ExcludedNames"_s, d.networkExcludedNames);
    networkExcludedFiles = readOrDefault(s, u"Network/ExcludedFiles"_s, d.networkExcludedFiles);

    mcp = readOrDefault(s, u"Mcp/Enabled"_s, d.mcp);
    mcpAllowOpen = readOrDefault(s, u"Mcp/AllowOpen"_s, d.mcpAllowOpen);
    mcpClipboard = readOrDefault(s, u"Mcp/Clipboard"_s, d.mcpClipboard);
    mcpScreenshot = readOrDefault(s, u"Mcp/Screenshot"_s, d.mcpScreenshot);
    mcpExcludedPaths = readOrDefault(s, u"Mcp/ExcludedPaths"_s, d.mcpExcludedPaths);
    mcpPlugins = readOrDefault(s, u"Mcp/Plugins"_s, d.mcpPlugins);
    mcpPluginActions = readOrDefault(s, u"Mcp/PluginActions"_s, d.mcpPluginActions);
    mcpSettings = readOrDefault(s, u"Mcp/Settings"_s, d.mcpSettings).trimmed().toLower();
    if (mcpSettings != u"auto" && mcpSettings != u"off")
        mcpSettings = d.mcpSettings;

    pluginDeveloperMode = readOrDefault(s, u"Plugins/DeveloperMode"_s, d.pluginDeveloperMode);
    pluginBuiltinsExpanded = readOrDefault(s, u"Plugins/BuiltinsExpanded"_s, d.pluginBuiltinsExpanded);
    pluginMaxResident = std::clamp(readOrDefault(s, u"Plugins/MaxResident"_s, d.pluginMaxResident), 0, 64);
    plugins = readPlugins(s);
    // The former built-in and the old URL sample become one optional plugin.
    // Keep the old section for rollback, but read it only during this migration.
    if (!s.value(u"Plugins/WebSearchMigrated"_s, false).toBool()) {
        PluginConfig& config = pluginConfig(webplugin::kId.toString());
        if (!webplugin::value(config, u"shortcuts")) {
            const bool legacy = s.contains(u"WebSearch/size"_s);
            webSuggestions = legacy ? s.value(u"WebSearch/Suggestions"_s, true).toBool() : true;
            auto defaults = d.webShortcuts;
            if (!firstRun) {
                // These three belonged only to the old sample. Its overrides
                // (including a removed keyword) must decide their migration.
                defaults.removeIf([](const WebShortcut& shortcut) {
                    return shortcut.keyword == u"gh" || shortcut.keyword == u"yt" || shortcut.keyword == u"wiki";
                });
            }
            webShortcuts = legacy ? readWebShortcuts(s, defaults, webSuggestions) : defaults;
            QJsonArray legacyCommands;
            QFile userManifest(QDir(dataDir()).filePath(u"plugins/winshun.web/plugin.json"_s));
            if (userManifest.open(QIODevice::ReadOnly) && userManifest.size() <= (1 << 20)) {
                const auto object = QJsonDocument::fromJson(userManifest.readAll()).object();
                if (!object.contains(u"runtime"_s) && object.value(u"id"_s).toString() == webplugin::kId)
                    legacyCommands = object.value(u"commands"_s).toArray();
            }
            webplugin::mergeLegacyCommands(webShortcuts, config, legacyCommands);
            webplugin::store(config, webShortcuts, webSuggestions);
            config.enabled = !firstRun || legacy || config.enabled;
            if (config.enabled && config.confirmed.isEmpty())
                config.confirmed = {u"none"_s}; // URL-only; the migration adds no permission.
        }
        writePlugins(s, plugins);
        s.setValue(u"Plugins/WebSearchMigrated"_s, true);
        s.sync();
    }
    webShortcuts = webplugin::shortcuts(plugin(webplugin::kId));
    webSuggestions = webplugin::suggestions(plugin(webplugin::kId));
    // System switches used to be unconditional native backends. Preserve
    // existing users' system-search choice once; a fresh profile starts off.
    // Persist the marker even when off, so the second load is not an upgrade.
    if (!s.value(u"Plugins/SwitchesMigrated"_s, false).toBool()) {
        if (!plugin(switchwire::kPlugin) && !firstRun) {
            PluginConfig config;
            config.id = switchwire::kPlugin.toString();
            config.enabled = searchSystem;
            config.confirmed = {u"ui"_s, u"system:settings"_s};
            plugins.append(config);
            const QString group = u"Plugin."_s + config.id + u'/';
            s.setValue(group + u"Enabled"_s, config.enabled);
            s.setValue(group + u"Confirmed"_s, config.confirmed);
        }
        s.setValue(u"Plugins/SwitchesMigrated"_s, true);
        s.sync();
    }
}

void Settings::save() const
{
    saveChecked();
}

bool Settings::saveChecked() const
{
    QSettings s(QSettings::IniFormat, QSettings::UserScope, u"WinShun"_s, u"WinShun"_s);
    s.setValue(u"Setup/Completed"_s, onboardingComplete);
    s.setValue(u"Setup/CapturePending"_s, onboardingCapturePending);
    s.setValue(u"Launcher/DoubleCtrl"_s, doubleCtrl);
    s.setValue(u"Launcher/DoubleTapAction"_s, doubleTapAction);
    s.setValue(u"Launcher/DoubleTapKey"_s, doubleTapKey);
    s.setValue(u"Launcher/DoubleCtrlKey"_s, doubleCtrlKey);
    s.setValue(u"Launcher/DoubleCtrlPauseInGames"_s, doubleCtrlPauseInGames);
    s.setValue(u"Launcher/DoubleCtrlPauseInFullScreen"_s, doubleCtrlPauseInFullScreen);
    s.setValue(u"Launcher/DoubleCtrlExcludedApps"_s, doubleCtrlExcludedApps);
    s.setValue(u"Launcher/Hotkey"_s, hotkey);
    s.setValue(u"Launcher/PanelHotkey"_s, panelHotkey);
    s.setValue(u"Launcher/AdminTapHinted"_s, adminTapHinted);
    s.setValue(u"Launcher/AdminDialogHinted"_s, adminDialogHinted);
    s.setValue(u"Launcher/ServiceUpdateHinted"_s, serviceUpdateHinted);
    s.setValue(u"Launcher/ShortcutAgain"_s, shortcutAgain);
    s.setValue(u"Launcher/Renderer"_s, renderer);
    s.setValue(u"Proxy/Mode"_s, proxyMode);
    s.setValue(u"Proxy/Address"_s, proxyAddress);
    s.setValue(u"Launcher/History"_s, recordHistory);
    s.setValue(u"Launcher/RememberAdmin"_s, rememberAdmin);
    s.setValue(u"Launcher/DialogJump"_s, dialogJump);
    s.setValue(u"Launcher/DialogBar"_s, dialogBar);
    s.setValue(u"Launcher/DialogBarPlace"_s, dialogBarPlace);
    s.setValue(u"Launcher/DialogBarFolderOnly"_s, dialogBarFolderOnly);
    s.setValue(u"Launcher/DialogAutoJump"_s, dialogAutoJump);
    s.setValue(u"Launcher/DialogAutoJumpWhen"_s, dialogAutoJumpWhen);
    s.setValue(u"Launcher/DialogAutoJumpAsked"_s, dialogAutoJumpAsked);
    s.setValue(u"Launcher/DialogBarExcludedApps"_s, dialogBarExcludedApps);
    s.setValue(u"Launcher/FileManager"_s, fileManager);
    s.setValue(u"Launcher/FileManagerOpenCommand"_s, fileManagerOpenCommand);
    s.setValue(u"Launcher/FileManagerRevealCommand"_s, fileManagerRevealCommand);
    s.setValue(u"Launcher/ExplorerMenu"_s, explorerMenu);
    s.setValue(u"Launcher/ExplorerTyping"_s, explorerTyping);
    s.setValue(u"Launcher/DesktopTyping"_s, desktopTyping);
    s.setValue(u"Launcher/OpusTyping"_s, opusTyping);
    s.setValue(u"Launcher/OpusTypingScope"_s, opusTypingScope);
    s.setValue(u"Launcher/FolderTyping"_s, folderTyping);
    s.setValue(u"Launcher/FolderMenu"_s, folderMenu);
    s.setValue(u"Launcher/FolderMenuExplorer"_s, folderMenuExplorer);
    s.setValue(u"Launcher/FolderMenuDesktop"_s, folderMenuDesktop);
    s.setValue(u"Launcher/FolderMenuDouble"_s, folderMenuDouble);
    s.setValue(u"Launcher/FolderMenuMiddle"_s, folderMenuMiddle);
    s.setValue(u"Launcher/FolderMenuFavorites"_s, folderMenuFavorites);
    s.setValue(u"Launcher/FolderMenuRecent"_s, folderMenuRecent);
    s.setValue(u"Launcher/FolderMenuWindows"_s, folderMenuWindows);
    s.setValue(u"Launcher/FolderMenuRecentInline"_s, folderMenuRecentInline);
    s.setValue(u"Launcher/FolderMenuWindowsInline"_s, folderMenuWindowsInline);
    s.setValue(u"Launcher/FolderMenuItemSearch"_s, folderMenuItemSearch);
    s.setValue(u"Launcher/FolderMenuItemCopyPath"_s, folderMenuItemCopyPath);
    s.setValue(u"Launcher/FolderMenuItemCmd"_s, folderMenuItemCmd);
    s.setValue(u"Launcher/FolderMenuItemPowerShell"_s, folderMenuItemPowerShell);
    s.setValue(u"Launcher/FolderMenuItemFavorite"_s, folderMenuItemFavorite);
    s.setValue(u"Launcher/FolderMenuItemSettings"_s, folderMenuItemSettings);
    s.setValue(u"Launcher/FolderMenuHotkey"_s, folderMenuHotkey);
    s.setValue(u"Launcher/FolderMenuOpusBefore"_s, folderMenuOpusBefore);
    s.setValue(u"Favorites/DropPie"_s, dropPie);
    s.setValue(u"Favorites/DropPieKeyTakenTold"_s, dropPieKeyTakenTold);
    s.setValue(u"Results/FoldersFirst"_s, foldersFirst);
    if (preferredTypes == defaultPreferredTypes())
        s.remove(kPreferredTypesKey);
    else
        s.setValue(kPreferredTypesKey, preferredTypes);
    s.setValue(u"Results/ShowModified"_s, showModified);
    s.setValue(u"Results/ShowModifiedTime"_s, showModifiedTime);
    s.setValue(u"Results/ShowFileSize"_s, showFileSize);
    s.setValue(u"Results/FolderKeyword"_s, folderKeyword);
    s.setValue(u"Results/DoubleClickToOpen"_s, doubleClickToOpen);
    s.setValue(u"Results/TabEntersFavoriteFolder"_s, tabEntersFavoriteFolder);
    s.setValue(u"Results/Thumbnails"_s, thumbnails);
    s.setValue(u"Results/DeleteButton"_s, rowDeleteButton);
    s.setValue(u"Results/SearchApps"_s, searchApps);
    s.setValue(u"Results/SearchSystem"_s, searchSystem);
    s.setValue(u"Results/RegexMode"_s, regexMode);
    s.setValue(u"Results/Sort"_s, sort);
    s.setValue(u"Results/StartList"_s, startList);
    s.setValue(u"Results/RecentItems"_s, recentItems);
    s.setValue(u"Results/MergeFileScope"_s, mergeFileScope);
    s.setValue(u"Results/AppsTab"_s, appsTab);
    s.setValue(u"Launcher/QueryHistoryKeys"_s, queryHistoryKeys);
    s.setValue(u"Launcher/LogoRightClick"_s, logoRightClick);
    s.setValue(u"Results/FavoritesOnStart"_s, favoritesOnStart);
    s.setValue(u"Launcher/ClearOnOpen"_s, clearOnOpen);
    s.setValue(u"Launcher/EnglishOnOpen"_s, englishOnOpen);
    s.setValue(u"Results/NumberKeys"_s, numberKeys);
    s.setValue(u"Results/NumberHints"_s, numberHints);
    s.setValue(u"Results/ShortcutHelp"_s, shortcutHelp);
    s.setValue(u"Results/FooterHints"_s, footerHints);
    s.setValue(u"Results/PureSearch"_s, pureSearch);
    s.setValue(u"Panel/ListAll"_s, panelListsAll);
    s.setValue(u"Appearance/ScopeButton"_s, scopeButton);
    s.setValue(u"Results/PreviewWith"_s, previewWith);
    s.setValue(u"Results/PreviewHotkey"_s, previewHotkey);
    s.setValue(u"Results/OpenHotkey"_s, openHotkey);
    s.setValue(u"Results/PreviewFollowsPointer"_s, previewFollowsPointer);
    s.setValue(u"Results/PreviewRestore"_s, previewRestore);
    s.setValue(u"Appearance/Theme"_s, theme);
    s.setValue(u"Appearance/Language"_s, language);
    s.setValue(u"Appearance/Transparency"_s, transparency);
    s.setValue(u"Appearance/GlassTransparency"_s, glassTransparency);
    s.setValue(u"Appearance/GlassClarity"_s, glassClarity);
    s.setValue(u"Appearance/TaskbarIcon"_s, taskbarIcon);
    s.remove(u"Appearance/SearchLogo"_s); // retired exterior decoration; inline logos are always present
    s.setValue(u"Appearance/UiSize"_s, uiSize);
    s.setValue(u"Appearance/MaxRows"_s, maxRows);
    s.setValue(u"Appearance/UiFont"_s, uiFont);
    s.setValue(u"Appearance/Corners"_s, corners);
    s.setValue(u"Appearance/UiStyle"_s, uiStyle);
    s.setValue(u"Update/Automatic"_s, autoUpdate);
    s.setValue(u"Backup/Folder"_s, backupFolder);
    s.setValue(u"Index/ExcludedPaths"_s, excludedPaths);
    s.setValue(u"Index/ExcludedNames"_s, excludedNames);
    s.setValue(u"Index/ExcludedTypes"_s, excludedTypes);
    s.setValue(u"Index/ExcludedFileNames"_s, excludedFileNames);
    s.setValue(u"Index/IncludeRemovableDrives"_s, includeRemovableDrives);
    if (selectedDrives)
        s.setValue(u"Index/Drives"_s, *selectedDrives);
    else
        s.remove(u"Index/Drives"_s);
    s.setValue(u"Index/RescanOnStartup"_s, rescanOnStartup);
    s.setValue(u"Index/Access"_s, indexAccess);
    s.setValue(u"Index/Scope"_s, indexScope);
    // Not while it is the known folders, never taken (see load).
    if (indexFoldersChosen || indexFolders != defaultIndexFolders())
        s.setValue(u"Index/Folders"_s, indexFolders);
    else
        s.remove(u"Index/Folders"_s);
    s.setValue(u"Index/FolderVerifyHours"_s, folderVerifyHours);
    s.setValue(u"Index/FolderHintDismissed"_s, folderHintDismissed);
    s.setValue(u"Index/Folder"_s, indexFolder);
    s.setValue(u"Content/Enabled"_s, contentSearch);
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
    s.setValue(u"Clipboard/MissingFiles"_s, clipboardMissingFiles);
    s.setValue(u"Clipboard/Images"_s, clipboardImages);
    s.setValue(u"Clipboard/ClickToPaste"_s, clipboardClickToPaste);
    s.setValue(u"Clipboard/UiSize"_s, clipboardUiSize);
    s.setValue(u"Clipboard/LargePictures"_s, clipboardLargePictures);
    s.setValue(u"Clipboard/KeepPlace"_s, clipboardKeepPlace);
    s.setValue(u"Clipboard/AtPointer"_s, clipboardAtPointer);
    s.setValue(u"Clipboard/PauseInFullScreen"_s, clipboardPauseInFullScreen);
    s.setValue(u"Clipboard/FlipAbove"_s, clipboardFlipAbove);
    s.setValue(u"Clipboard/HoldToStep"_s, clipboardHoldToStep);
    s.setValue(u"Clipboard/PasteElevated"_s, clipboardPasteElevated);
    s.setValue(u"Clipboard/SearchBoxSide"_s, clipboardSearchBoxSide);
    s.setValue(u"Clipboard/ExcludedApps"_s, clipboardExcludedApps);
    s.setValue(u"Clipboard/NumberKeys"_s, clipboardNumberKeys);
    s.setValue(u"Clipboard/QuickPaste"_s, clipboardQuickPaste);
    s.setValue(u"Taskbar/WinS"_s, taskbarWinS);
    s.setValue(u"Taskbar/StartMenuTyping"_s, taskbarStartTyping);
    s.setValue(u"Taskbar/SearchBox"_s, taskbarSearchBox);
    s.setValue(u"Taskbar/SearchBoxStyle"_s, taskbarSearchBoxStyle);
    s.setValue(u"Taskbar/SearchBoxSide"_s, taskbarSearchBoxSide);
    if (capturePrintScreenBefore.isEmpty())
        s.remove(u"Capture/PrintScreenBefore"_s);
    else
        s.setValue(u"Capture/PrintScreenBefore"_s, capturePrintScreenBefore);
    s.setValue(u"Capture/Keyword"_s, captureKeyword);
    s.setValue(u"PasteFile/Enabled"_s, pasteFileEnabled);
    s.setValue(u"PasteFile/CtrlV"_s, pasteFileCtrlV);
    s.setValue(u"PasteFile/Menu"_s, pasteFileMenu);
    writeFileTypes(s, fileTypes);
    writeQuickFilters(s, quickFilters);
    writeShares(s, shares);
    s.setValue(u"Network/VerifyHours"_s, networkVerifyHours);
    s.setValue(u"Network/VerifyHoursUnwatched"_s, networkVerifyHoursUnwatched);
    s.setValue(u"Network/Threads"_s, networkThreads);
    s.setValue(u"Network/ExcludedNames"_s, networkExcludedNames);
    s.setValue(u"Network/ExcludedFiles"_s, networkExcludedFiles);
    s.setValue(u"Mcp/Enabled"_s, mcp);
    s.setValue(u"Mcp/AllowOpen"_s, mcpAllowOpen);
    s.setValue(u"Mcp/Clipboard"_s, mcpClipboard);
    s.setValue(u"Mcp/Screenshot"_s, mcpScreenshot);
    s.setValue(u"Mcp/ExcludedPaths"_s, mcpExcludedPaths);
    s.setValue(u"Mcp/Plugins"_s, mcpPlugins);
    s.setValue(u"Mcp/PluginActions"_s, mcpPluginActions);
    s.setValue(u"Mcp/Settings"_s, mcpSettings);
    s.setValue(u"Plugins/DeveloperMode"_s, pluginDeveloperMode);
    s.setValue(u"Plugins/BuiltinsExpanded"_s, pluginBuiltinsExpanded);
    s.setValue(u"Plugins/MaxResident"_s, pluginMaxResident);
    auto pluginConfigs = plugins;
    auto web = std::find_if(pluginConfigs.begin(), pluginConfigs.end(), [](const PluginConfig& c) { return c.id == webplugin::kId; });
    if (web == pluginConfigs.end()) {
        PluginConfig config;
        config.id = webplugin::kId.toString();
        pluginConfigs.append(config);
        web = std::prev(pluginConfigs.end());
    }
    webplugin::store(*web, webShortcuts, webSuggestions);
    writePlugins(s, pluginConfigs);
    s.setValue(u"Plugins/WebSearchMigrated"_s, true);
    s.sync();
    return s.status() == QSettings::NoError;
}

ContentSizeLimits Settings::contentSizeLimits() const
{
    ContentSizeLimits limits;
    for (std::size_t k = 0; k < ContentSizeLimits::kKinds; ++k)
        limits.bytes[k] = std::llround(contentMaxSizeMB[k] * (1 << 20));
    return limits;
}

CrawlRules Settings::crawlRules() const
{
    CrawlRules rules;
    for (const QString& p : excludedPaths) {
        if (!p.trimmed().isEmpty())
            rules.excludedPaths.push_back(normalizePath(p));
    }
    // Our own files churn constantly. Not when they are a drive's root (an
    // empty drive chosen for the index): that would leave the whole drive out.
    const auto ownFolder = [&](const QString& folder) {
        std::wstring path = normalizePath(folder);
        if (!(path.size() == 2 && path[1] == L':'))
            rules.excludedPaths.push_back(std::move(path));
    };
    ownFolder(dataDir());
    if (!indexFolder.isEmpty())
        ownFolder(indexDir());
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

std::wstring Settings::indexFolderPath(const QString& folder)
{
    if (folder.trimmed().isEmpty() || isNetworkPath(folder))
        return {}; // a share's place is 网络共享 (ShareIndexer), and its folders are not looked up here
    std::wstring path = win32::expandEnvironment(QDir::toNativeSeparators(folder.trimmed()).toStdWString());
    if (path.size() < 2 || path[1] != L':' || !std::iswalpha(path[0]) || (path.size() > 2 && path[2] != L'\\'))
        return {}; // no full path
    path = path.find_first_not_of(L'\\', 2) == std::wstring::npos ? path.substr(0, 2) : normalizePath(folder); // "D:\" -> "D:"
    path[0] = static_cast<wchar_t>(std::towupper(path[0]));
    return path;
}

std::vector<std::wstring> Settings::indexFolderPaths() const
{
    std::vector<std::wstring> paths;
    for (const QString& folder : indexFolders) {
        if (std::wstring path = indexFolderPath(folder); !path.empty())
            paths.push_back(std::move(path));
    }
    return paths;
}

bool Settings::chooseDefaultIndexFolders()
{
    // Never set, the list is the known folders as load() found them, or as
    // changed since by an edit that knows nothing of the flag: taken as it
    // is. Empty (the known folders all on a server, say): not yet, the next
    // start looks for them again.
    if (indexFoldersChosen || indexFolders.isEmpty())
        return false;
    indexFoldersChosen = true;
    return true;
}

bool Settings::foldersInEffect(bool walked, bool elevated, bool serviceWanted, ntfs::ServiceState service) const
{
    // Read whole by the service or as administrator: the chosen folders
    // come in there with step 6 of GitHub #52, not yet.
    if (!walked || indexScope == u"drives")
        return false;
    // The index service installed and not switched off is on its way (not
    // started yet at login, being updated, stopped for a moment): the disks
    // stay whole meanwhile, or every login before it would cost them their
    // entries, all read again once it is there (GitHub #52 review).
    const bool longAbsent = service == ntfs::ServiceState::NotInstalled || service == ntfs::ServiceState::Disabled;
    if (serviceWanted && !longAbsent)
        return false;
    if (indexScope == u"folders")
        return true;
    // auto: walked for good, not just while the service is away for a moment.
    return indexAccess == u"walk" || (!elevated && serviceWanted && longAbsent);
}

ShareIndexer::Options Settings::shareOptions() const
{
    ShareIndexer::Options o;
    for (const NetworkShare& n : shares)
        o.shares.push_back({n.path.toStdWString(), n.name.toStdWString(), n.content, n.scan});
    o.rules = crawlRules();
    for (const QString& n : networkExcludedNames) {
        if (!n.trimmed().isEmpty())
            o.rules.excludedNames.push_back(normalizeName(n));
    }
    for (const QString& n : networkExcludedFiles) {
        if (!n.trimmed().isEmpty())
            o.excludedFiles.push_back(n.trimmed().toStdWString());
    }
    o.verifyEvery = std::chrono::hours(networkVerifyHours);
    o.verifyEveryUnwatched = std::chrono::hours(networkVerifyHoursUnwatched);
    o.threads = networkThreads;
    return o;
}

} // namespace ws
