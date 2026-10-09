#include "AppCatalog.h"

#include "History.h"
#include "Query.h"
#include "TextUtil.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <QCollator>
#include <QHash>
#include <QMetaObject>
#include <QStringTokenizer>

#include <windows.h>
// After windows.h; ole2.h first, as WIN32_LEAN_AND_MEAN keeps it out of windows.h.
#include <ole2.h>

#include <appmodel.h>
#include <shlobj.h>
#include <shobjidl.h>
// After shobjidl.h (PROPERTYKEY):
#include <propkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using Microsoft::WRL::ComPtr;

namespace ws {

namespace {

constexpr QStringView kLaunchPrefix = u"shell:AppsFolder\\";

// Re-read even without a detected change after this long: covers what the
// fingerprint cannot see, such as a new display language.
constexpr auto kMaxAge = 10min;

// System.AppUserModel.* properties that propkey.h has no constants for.
constexpr GUID kAppUserModel {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}};
constexpr PROPERTYKEY kHostEnvironment {kAppUserModel, 14}; // 0 desktop, 1 UWP, 2 full-trust package
constexpr PROPERTYKEY kPackageInstallPath {kAppUserModel, 15};
constexpr PROPERTYKEY kPackageFullName {kAppUserModel, 21};

// What a Start-menu shortcut may point to and still be a program.
const QStringList& programSuffixes()
{
    static const QStringList kSuffixes {u"exe"_s, u"com"_s, u"bat"_s, u"cmd"_s, u"msc"_s, u"cpl"_s, u"ps1"_s,
        u"vbs"_s, u"scr"_s, u"appref-ms"_s};
    return kSuffixes;
}

// The ones with a "run as administrator" verb.
const QStringList& elevatableSuffixes()
{
    static const QStringList kSuffixes {u"exe"_s, u"com"_s, u"bat"_s, u"cmd"_s, u"msc"_s};
    return kSuffixes;
}

bool isAsciiAlnum(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool isUrl(const QString& s)
{
    return s.contains(u"://"_s) || s.startsWith(u"mailto:"_s, Qt::CaseInsensitive);
}

// Windows' own apps by their AppUserModelID, named in English and in Chinese.
// A packaged app takes its name in the first language of the user's list
// (Settings > Time & language > Language & region), whatever language
// Windows itself shows: with English first on a Chinese Windows, Calculator
// is "Calculator" and 计算器 found nothing; the apps in Windows' folder
// (Settings) go by Windows' language all the same. A package's other
// languages are out of reach short of parsing its resources.pri:
// SHLoadIndirectString resolves in the user's list alone, thread and process
// languages notwithstanding, and ResourceManager wants an app identity.
// The Chinese names are those in the apps' zh-Hans resources (checked
// 2026-10-09); Mail, Calendar, Maps, Movies & TV and Paint 3D, gone from
// Windows 11, as Windows 10 named them.
struct KnownApp {
    QStringView id;
    QStringView names; // separated by '|'
};
constexpr KnownApp kKnownApps[] = {
    {u"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App", u"Calculator|计算器"},
    {u"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App", u"Notepad|记事本"},
    {u"Microsoft.Paint_8wekyb3d8bbwe!App", u"Paint|画图"},
    {u"Microsoft.ScreenSketch_8wekyb3d8bbwe!App", u"Snipping Tool|截图工具"},
    {u"Microsoft.WindowsAlarms_8wekyb3d8bbwe!App", u"Clock|时钟"},
    {u"Microsoft.WindowsCamera_8wekyb3d8bbwe!App", u"Camera|相机"},
    {u"Microsoft.Windows.Photos_8wekyb3d8bbwe!App", u"Photos|照片"},
    {u"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App", u"Terminal|终端"},
    {u"Microsoft.MicrosoftStickyNotes_8wekyb3d8bbwe!App", u"Sticky Notes|便笺"},
    {u"Microsoft.WindowsSoundRecorder_8wekyb3d8bbwe!App", u"Sound Recorder|录音机"},
    {u"Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic", u"Media Player|媒体播放器"},
    {u"Microsoft.BingWeather_8wekyb3d8bbwe!App", u"Weather|天气"},
    {u"Microsoft.BingNews_8wekyb3d8bbwe!AppexNews", u"News|资讯"},
    {u"Microsoft.YourPhone_8wekyb3d8bbwe!App", u"Phone Link|手机连接"},
    {u"Microsoft.GetHelp_8wekyb3d8bbwe!App", u"Get Help|获取帮助"},
    {u"Microsoft.WindowsFeedbackHub_8wekyb3d8bbwe!App", u"Feedback Hub|反馈中心"},
    {u"Microsoft.SecHealthUI_8wekyb3d8bbwe!SecHealthUI", u"Windows Security|Windows 安全中心"},
    {u"Microsoft.XboxGamingOverlay_8wekyb3d8bbwe!App", u"Game Bar|游戏栏"},
    {u"Microsoft.CommandPalette_8wekyb3d8bbwe!App", u"Command Palette|命令面板"},
    {u"MicrosoftCorporationII.QuickAssist_8wekyb3d8bbwe!App", u"Quick Assist|快速助手"},
    {u"windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel", u"Settings|设置"},
    {u"microsoft.windowscommunicationsapps_8wekyb3d8bbwe!microsoft.windowslive.mail", u"Mail|邮件"},
    {u"microsoft.windowscommunicationsapps_8wekyb3d8bbwe!microsoft.windowslive.calendar", u"Calendar|日历"},
    {u"Microsoft.WindowsMaps_8wekyb3d8bbwe!App", u"Maps|地图"},
    {u"Microsoft.ZuneVideo_8wekyb3d8bbwe!Microsoft.ZuneVideo", u"Movies & TV|电影和电视"},
    {u"Microsoft.MSPaint_8wekyb3d8bbwe!Microsoft.MSPaint", u"Paint 3D|画图 3D"},
};

// The names of a Windows app other than the one it shows.
QStringList otherNamesOf(const QString& id, const QString& name)
{
    const auto known = std::find_if(std::begin(kKnownApps), std::end(kKnownApps),
        [&](const KnownApp& k) { return id.compare(k.id, Qt::CaseInsensitive) == 0; });
    QStringList names;
    if (known == std::end(kKnownApps))
        return names;
    for (QStringView other : known->names.tokenize(u'|')) {
        if (other.compare(name, Qt::CaseInsensitive) != 0)
            names.append(other.toString());
    }
    return names;
}

struct ComApartment {
    ComApartment()
        : hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))
    {
    }
    ~ComApartment()
    {
        if (SUCCEEDED(hr))
            ::CoUninitialize();
    }
    HRESULT hr;
};

QString takeString(PWSTR s)
{
    QString out = QString::fromWCharArray(s);
    ::CoTaskMemFree(s);
    return out;
}

QString displayName(IShellItem* item, SIGDN form)
{
    PWSTR s = nullptr;
    return SUCCEEDED(item->GetDisplayName(form, &s)) && s ? takeString(s) : QString();
}

QString stringProperty(IShellItem2* item, const PROPERTYKEY& key)
{
    PWSTR s = nullptr;
    return SUCCEEDED(item->GetString(key, &s)) && s ? takeString(s) : QString();
}

AppKind packageKind(const QString& fullName, const QString& installPath, const QString& windowsDir)
{
    // Exported by kernelbase.dll; the SDK's kernel32.lib has no import for it.
    using GetOrigin = LONG(WINAPI*)(PCWSTR, PackageOrigin*);
    static const auto getOrigin = reinterpret_cast<GetOrigin>(
        reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"kernelbase.dll"), "GetStagedPackageOrigin")));
    PackageOrigin origin = PackageOrigin_Unknown;
    if (getOrigin)
        getOrigin(reinterpret_cast<PCWSTR>(fullName.utf16()), &origin);
    switch (origin) {
    case PackageOrigin_Store:
        return AppKind::Store;
    case PackageOrigin_Inbox:
        return AppKind::System;
    case PackageOrigin_Unknown:
        if (!windowsDir.isEmpty() && installPath.startsWith(windowsDir + u'\\', Qt::CaseInsensitive))
            return AppKind::System;
        return AppKind::Package;
    default:
        return AppKind::Package;
    }
}

bool sameApps(const AppList& a, const AppList& b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const AppInfo& x, const AppInfo& y) {
        return x.name == y.name && x.id == y.id && x.target == y.target && x.kind == y.kind
            && x.elevatable == y.elevatable;
    });
}

class Fnv1a {
public:
    void add(const void* data, std::size_t size) noexcept
    {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i)
            m_hash = (m_hash ^ p[i]) * 1099511628211ull;
    }
    quint64 value() const noexcept { return m_hash; }

private:
    quint64 m_hash = 14695981039346656037ull;
};

// A folder's write time changes when an entry in it is added, removed or
// renamed, so the write times of a tree's folders reveal any such change.
void addFolderTree(Fnv1a& hash, const std::wstring& dir, int depth)
{
    WIN32_FIND_DATAW data;
    const win32::UniqueFind find(::FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &data,
        FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH));
    if (!find.valid())
        return;
    do {
        const std::wstring_view name(data.cFileName);
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            || name == L"." || name == L"..")
            continue;
        hash.add(name.data(), name.size() * sizeof(wchar_t));
        hash.add(&data.ftLastWriteTime, sizeof data.ftLastWriteTime);
        if (depth < 8)
            addFolderTree(hash, dir + L'\\' + std::wstring(name), depth + 1);
    } while (::FindNextFileW(find.get(), &data));
}

} // namespace

void AppInfo::prepare()
{
    nameUtf8 = wtf8::fromUtf16(wtf8::view(name));
    otherNames = otherNamesOf(id, name);
    otherNamesUtf8.clear();
    for (const QString& other : otherNames)
        otherNamesUtf8.push_back(wtf8::fromUtf16(wtf8::view(other)));
    const QString file = target.mid(target.lastIndexOf(u'\\') + 1);
    const qsizetype dot = file.lastIndexOf(u'.');
    const QString suffix = dot > 0 ? file.mid(dot + 1).toLower() : QString();

    program.clear();
    if (kind == AppKind::Desktop && suffix == u"exe")
        program = wtf8::fromUtf16(wtf8::view(file.left(dot)));

    initials.clear();
    for (std::size_t i = 0; i < nameUtf8.size(); ++i) {
        if (isAsciiAlnum(nameUtf8[i]) && text::isWordStart(nameUtf8, i))
            initials += static_cast<char>(text::fold(nameUtf8[i]));
    }

    const bool uninstaller = name.contains(u"uninstall"_s, Qt::CaseInsensitive) || name.contains(u"卸载"_s)
        || file.startsWith(u"unins"_s, Qt::CaseInsensitive);
    const bool document = !target.isEmpty() && !programSuffixes().contains(suffix);
    auxiliary = kind == AppKind::Desktop && (uninstaller || document);
}

QString AppInfo::launchPath() const
{
    return appLaunchPath(id);
}

QString appLaunchPath(const QString& id)
{
    return kLaunchPrefix + id;
}

bool isAppLaunchPath(QStringView path) noexcept
{
    return path.startsWith(kLaunchPrefix, Qt::CaseInsensitive);
}

QString appIdOf(QStringView launchPath)
{
    return isAppLaunchPath(launchPath) ? launchPath.mid(kLaunchPrefix.size()).toString() : QString();
}

std::vector<AppHit> searchApps(
    const AppList& apps, const ParsedQuery& query, const NameMatcher& matcher, const QStringList& history)
{
    std::vector<AppHit> hits;
    if (query.isEmpty())
        return hits;

    QHash<QString, int> recent; // id -> position in the history, 0 = newest
    for (int i = 0; i < history.size(); ++i) {
        if (isAppLaunchPath(history[i]))
            recent.insert(appIdOf(history[i]), std::min(recent.value(appIdOf(history[i]), i), i));
    }

    // A lone plain term may be the initials of the name: "vsc" for Visual Studio Code.
    std::string_view initials;
    if (query.terms.size() == 1 && query.extensions.empty()) {
        const QueryTerm& t = query.terms.front();
        if (!t.negated && !t.wildcard && t.ancestors.empty() && t.text.size() >= 2
            && std::all_of(t.text.begin(), t.text.end(), isAsciiAlnum))
            initials = t.text;
    }

    for (std::size_t i = 0; i < apps.size(); ++i) {
        const AppInfo& app = apps[i];
        int score = matcher.matchName(app.nameUtf8);
        int otherName = -1;
        const auto better = [&](int s, int other) {
            if (s > score) {
                score = s;
                otherName = other;
            }
        };
        // "计算器" for Calculator, as much as its own name.
        for (std::size_t k = 0; k < app.otherNamesUtf8.size(); ++k)
            better(matcher.matchName(app.otherNamesUtf8[k]), static_cast<int>(k));
        if (!app.program.empty()) {
            if (const int s = matcher.matchName(app.program); s >= 0)
                better(s - 6, -1); // "winword" finds Word, a little below a name match
        }
        if (!initials.empty() && app.initials.starts_with(initials))
            better(initials.size() == app.initials.size() ? 45 : 30, -1);
        if (score < 0)
            continue;
        if (app.auxiliary)
            score -= 30;
        if (const auto it = recent.constFind(app.id); it != recent.cend())
            score += 20 - std::min(*it, 10);
        hits.push_back({i, score, otherName});
    }
    // Stable: equal scores keep the list's alphabetical order.
    std::stable_sort(hits.begin(), hits.end(), [](const AppHit& a, const AppHit& b) { return a.score > b.score; });
    return hits;
}

namespace {

QString rot13(QString text)
{
    for (QChar& c : text) {
        const char16_t u = c.unicode();
        if (u >= u'a' && u <= u'z')
            c = QChar(u'a' + (u - u'a' + 13) % 26);
        else if (u >= u'A' && u <= u'Z')
            c = QChar(u'A' + (u - u'A' + 13) % 26);
    }
    return text;
}

// "{6D809377-…}\Listary\Listary.exe" -> "C:\Program Files\Listary\Listary.exe";
// anything else as it is.
QString expandKnownFolder(const QString& name)
{
    if (!name.startsWith(u'{'))
        return name;
    const qsizetype close = name.indexOf(u'}');
    if (close < 0)
        return name;
    GUID id {};
    PWSTR folder = nullptr;
    QString path = name;
    if (SUCCEEDED(::CLSIDFromString(reinterpret_cast<LPCOLESTR>(name.left(close + 1).utf16()), &id))
        && SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &folder)))
        path = QString::fromWCharArray(folder) + name.mid(close + 1);
    ::CoTaskMemFree(folder);
    return path;
}

// Opened this week counts in full, then less as it gets older.
double recency(qint64 last, qint64 now)
{
    if (last <= 0)
        return 0.5; // recorded before times were
    const double days = double(now - last) / 86'400'000.0;
    return days <= 7 ? 1.0 : days <= 30 ? 0.7 : days <= 90 ? 0.4 : 0.2;
}

bool isWinShun(const AppInfo& app)
{
    const QString file = app.target.mid(app.target.lastIndexOf(u'\\') + 1);
    return file.compare(u"WinShun.exe", Qt::CaseInsensitive) == 0
        || file.compare(u"WinShunSearch.exe", Qt::CaseInsensitive) == 0;
}

} // namespace

std::vector<AppUse> windowsAppUses()
{
    // The programs and apps started; the other key of note ({F4E57C4B-…})
    // has the shortcuts started, whose targets would have to be read.
    win32::UniqueKey owned;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\UserAssist\\"
            L"{CEBFF5CD-ACE2-4F4F-9178-9926F41749EA}\\Count",
            0, KEY_READ, owned.out())
        != ERROR_SUCCESS)
        return {};
    const HKEY key = owned.get();
    std::vector<AppUse> uses;
    std::vector<wchar_t> name(1024);
    BYTE data[128];
    for (DWORD i = 0;; ++i) {
        DWORD nameLength = static_cast<DWORD>(name.size());
        DWORD size = sizeof data;
        DWORD type = 0;
        const LSTATUS status = ::RegEnumValueW(key, i, name.data(), &nameLength, nullptr, &type, data, &size);
        if (status == ERROR_NO_MORE_ITEMS)
            break;
        // Version 5 records (Windows 7 on): runs at 4, focus count at 8,
        // last started (FILETIME) at 60.
        if (status != ERROR_SUCCESS || type != REG_BINARY || size < 68)
            continue;
        const QString decoded = rot13(QString::fromWCharArray(name.data(), nameLength));
        if (decoded.startsWith(u"UEME_"))
            continue; // the record's own bookkeeping
        DWORD runs = 0;
        DWORD focus = 0;
        qint64 fileTime = 0;
        std::memcpy(&runs, data + 4, sizeof runs);
        std::memcpy(&focus, data + 8, sizeof focus);
        std::memcpy(&fileTime, data + 60, sizeof fileTime);
        constexpr qint64 kEpoch = 116'444'736'000'000'000; // 1970 in FILETIME units
        uses.push_back({decoded, runs + focus / 10.0, fileTime > kEpoch ? (fileTime - kEpoch) / 10'000 : 0});
    }
    return uses;
}

std::vector<std::size_t> frequentApps(
    const AppList& apps, const History& history, const std::vector<AppUse>& windows, qint64 now, std::size_t limit)
{
    // By id, and by program for desktop apps whose id is an AppUserModelID
    // where Windows recorded the program's path.
    QHash<QString, std::size_t> byId;
    QHash<QString, std::size_t> byProgram;
    for (std::size_t i = 0; i < apps.size(); ++i) {
        const AppInfo& app = apps[i];
        if (app.auxiliary || isWinShun(app))
            continue;
        byId.insert(app.id.toCaseFolded(), i);
        if (app.kind == AppKind::Desktop && !app.target.isEmpty())
            byProgram.insert(app.target.toCaseFolded(), i);
    }

    struct Ranked {
        double score;
        qint64 last;
        std::size_t index;
    };
    const auto byScore = [](const Ranked& a, const Ranked& b) {
        return a.score != b.score ? a.score > b.score : a.last > b.last;
    };
    std::vector<Ranked> own;
    for (const QString& path : history.items()) {
        const auto it = byId.constFind(appIdOf(path).toCaseFolded());
        if (it == byId.cend())
            continue;
        const History::Use use = history.use(path);
        own.push_back({use.count * recency(use.last, now), use.last, *it});
    }
    std::stable_sort(own.begin(), own.end(), byScore); // ties: the history's order, newest first

    constexpr qint64 kWindowsSpan = 60LL * 86'400'000; // what Windows saw lately
    std::vector<Ranked> seen;
    for (const AppUse& use : windows) {
        if (use.last <= 0 || now - use.last > kWindowsSpan)
            continue;
        std::optional<std::size_t> index;
        if (const auto id = byId.constFind(use.name.toCaseFolded()); id != byId.cend())
            index = *id;
        else if (const auto program = byProgram.constFind(expandKnownFolder(use.name).toCaseFolded());
                 program != byProgram.cend())
            index = *program;
        if (index)
            seen.push_back({std::max(1.0, use.uses) * recency(use.last, now), use.last, *index});
    }
    std::sort(seen.begin(), seen.end(), byScore);

    std::vector<std::size_t> picked;
    for (const std::vector<Ranked>* list : {&own, &seen}) {
        for (const Ranked& r : *list) {
            if (picked.size() >= limit)
                return picked;
            if (std::ranges::find(picked, r.index) == picked.end())
                picked.push_back(r.index);
        }
    }
    return picked;
}

AppCatalog::AppCatalog(QObject* parent)
    : QObject(parent)
{
}

AppCatalog::~AppCatalog()
{
    m_worker.request_stop();
    if (m_worker.joinable())
        m_worker.join(); // an outcome it already posted is dropped along with this object
}

std::shared_ptr<const AppList> AppCatalog::apps() const
{
    static const auto kEmpty = std::make_shared<const AppList>();
    std::lock_guard lock(m_mutex);
    return m_apps ? m_apps : kEmpty;
}

bool AppCatalog::isLoaded() const
{
    std::lock_guard lock(m_mutex);
    return m_apps != nullptr;
}

void AppCatalog::refresh(bool force)
{
    if (m_busy) {
        m_again = m_again || force;
        return;
    }
    m_busy = true;
    force = force || !isLoaded() || std::chrono::steady_clock::now() - m_loadedAt > kMaxAge;
    if (m_worker.joinable())
        m_worker.join(); // the previous read; it has posted its outcome, so it is done
    m_worker = std::jthread([this, force, known = m_fingerprint](std::stop_token stop) {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        Outcome outcome;
        outcome.fingerprint = installedAppsFingerprint();
        if (force || outcome.fingerprint != known) {
            outcome.reloaded = true;
            AppList apps = loadInstalledApps(stop);
            if (!apps.empty())
                outcome.apps = std::make_shared<const AppList>(std::move(apps));
        }
        if (!stop.stop_requested())
            QMetaObject::invokeMethod(this, [this, outcome] { publish(outcome); }, Qt::QueuedConnection);
    });
}

void AppCatalog::publish(const Outcome& outcome)
{
    m_busy = false;
    if (outcome.apps) {
        m_fingerprint = outcome.fingerprint;
        m_loadedAt = std::chrono::steady_clock::now();
        bool same = false;
        {
            std::lock_guard lock(m_mutex);
            same = m_apps && sameApps(*m_apps, *outcome.apps);
            m_apps = outcome.apps;
        }
        if (!same)
            emit changed();
    }
    // A failed read keeps the old fingerprint, so the next refresh tries again.
    if (std::exchange(m_again, false))
        refresh(true);
}

AppList loadInstalledApps(const std::stop_token& stop)
{
    const ComApartment com;
    ComPtr<IShellItem> folder;
    ComPtr<IEnumShellItems> items;
    if (FAILED(::SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder)))
        || FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items))))
        return {};

    const QString windowsDir = qEnvironmentVariable("WINDIR");
    AppList apps;
    ComPtr<IShellItem> item;
    while (!stop.stop_requested() && items->Next(1, item.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
        ComPtr<IShellItem2> properties;
        AppInfo app;
        app.name = displayName(item.Get(), SIGDN_NORMALDISPLAY).trimmed();
        app.id = displayName(item.Get(), SIGDN_PARENTRELATIVEPARSING);
        if (app.name.isEmpty() || app.id.isEmpty() || FAILED(item.As(&properties)))
            continue;

        const QString packageName = stringProperty(properties.Get(), kPackageFullName);
        if (!packageName.isEmpty()) {
            app.target = stringProperty(properties.Get(), kPackageInstallPath);
            app.kind = packageKind(packageName, app.target, windowsDir);
            ULONG host = 0;
            app.elevatable = SUCCEEDED(properties->GetUInt32(kHostEnvironment, &host)) && host == 2;
        } else {
            QString target = stringProperty(properties.Get(), PKEY_Link_TargetParsingPath);
            if (isUrl(app.id) || isUrl(target))
                continue; // a link to a web site, not an app
            if (target.startsWith(u"::"_s) || target.startsWith(u"shell:"_s, Qt::CaseInsensitive))
                target.clear(); // a shell object such as Control Panel: no file behind it
            if (!target.isEmpty()) {
                const DWORD attributes = ::GetFileAttributesW(reinterpret_cast<LPCWSTR>(target.utf16()));
                if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
                    continue; // a shortcut to a folder, not an app
            }
            app.target = target;
            app.kind = AppKind::Desktop;
            const qsizetype dot = target.lastIndexOf(u'.');
            app.elevatable = dot > 0 && elevatableSuffixes().contains(target.mid(dot + 1), Qt::CaseInsensitive);
        }
        app.prepare();
        apps.push_back(std::move(app));
    }
    if (stop.stop_requested())
        return {};

    QCollator collator; // the user's locale: Chinese names in pinyin order on a Chinese system
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(apps.begin(), apps.end(), [&](const AppInfo& a, const AppInfo& b) {
        return collator.compare(a.name, b.name) < 0;
    });
    return apps;
}

quint64 installedAppsFingerprint()
{
    Fnv1a hash;
    for (const KNOWNFOLDERID& folder : {FOLDERID_Programs, FOLDERID_CommonPrograms}) { // the Start menu
        PWSTR path = nullptr;
        if (SUCCEEDED(::SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &path))) {
            const std::wstring dir(path);
            WIN32_FILE_ATTRIBUTE_DATA info {};
            if (::GetFileAttributesExW(dir.c_str(), GetFileExInfoStandard, &info))
                hash.add(&info.ftLastWriteTime, sizeof info.ftLastWriteTime);
            addFolderTree(hash, dir, 0);
        }
        ::CoTaskMemFree(path);
    }
    // One subkey per package installed for this user: adding or removing one
    // updates the key's write time.
    win32::UniqueKey key;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository"
            L"\\Packages",
            0, KEY_READ, key.out())
        == ERROR_SUCCESS) {
        DWORD subkeys = 0;
        FILETIME written {};
        if (::RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, &written)
            == ERROR_SUCCESS) {
            hash.add(&subkeys, sizeof subkeys);
            hash.add(&written, sizeof written);
        }
    }
    return hash.value();
}

} // namespace ws
