#include "AppCatalog.h"

#include "Query.h"
#include "TextUtil.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <QCollator>
#include <QHash>
#include <QMetaObject>

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
#include <utility>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using Microsoft::WRL::ComPtr;

namespace qf {

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
        if (!app.program.empty()) {
            if (const int s = matcher.matchName(app.program); s >= 0)
                score = std::max(score, s - 6); // "winword" finds Word, a little below a name match
        }
        if (!initials.empty() && app.initials.starts_with(initials))
            score = std::max(score, initials.size() == app.initials.size() ? 45 : 30);
        if (score < 0)
            continue;
        if (app.auxiliary)
            score -= 30;
        if (const auto it = recent.constFind(app.id); it != recent.cend())
            score += 20 - std::min(*it, 10);
        hits.push_back({i, score});
    }
    // Stable: equal scores keep the list's alphabetical order.
    std::stable_sort(hits.begin(), hits.end(), [](const AppHit& a, const AppHit& b) { return a.score > b.score; });
    return hits;
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

} // namespace qf
