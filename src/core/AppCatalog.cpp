#include "AppCatalog.h"

#include "History.h"
#include "Pinyin.h"
#include "Query.h"
#include "TextUtil.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <QCollator>
#include <QDir>
#include <QFile>
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

// An abbreviation typed in full ("ps" for Photoshop): below a name typed in
// full (115), above a program's file name (109).
constexpr int kAbbreviationScore = 112;
// What a match in the name Windows lists the software under is worth less
// than one in the app's own name: the apps named so themselves come first.
constexpr int kInstalledNamePenalty = 8;

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

bool isAsciiLetterOrDigit(QChar c) noexcept
{
    return c.unicode() < 128 && isAsciiAlnum(static_cast<char>(c.unicode()));
}

bool isUrl(const QString& s)
{
    return s.contains(u"://"_s) || s.startsWith(u"mailto:"_s, Qt::CaseInsensitive);
}

bool sameFolded(QStringView a, QStringView b) noexcept
{
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

// Programs that only start what they are given: a shortcut to one with
// arguments opens something else (rundll32 desk.cpl,..., mmc virtmgmt.msc,
// explorer and a folder, a web app through chrome_proxy).
bool isLauncher(QStringView program)
{
    static constexpr QStringView kLaunchers[] {u"appvlp", u"chrome_proxy", u"conhost", u"control", u"cscript",
        u"explorer", u"hh", u"mmc", u"msedge_proxy", u"mshta", u"msiexec", u"rundll32", u"wscript", u"wt"};
    return std::ranges::any_of(kLaunchers, [&](QStringView p) { return sameFolded(program, p); });
}

// Shells and interpreters: started for themselves as well, with options
// (PowerShell 7's shortcut: pwsh -WorkingDirectory ~).
bool isShell(QStringView program)
{
    static constexpr QStringView kShells[] {
        u"cmd", u"java", u"javaw", u"node", u"powershell", u"pwsh", u"py", u"python", u"pythonw", u"pyw"};
    return std::ranges::any_of(kShells, [&](QStringView p) { return sameFolded(program, p); });
}

// "idle.pyw", "vcvars64.bat": a dot with letters or digits after it, one a letter.
bool hasExtension(QStringView word)
{
    const qsizetype dot = word.lastIndexOf(u'.');
    const QStringView ext = dot > 0 ? word.sliced(dot + 1) : QStringView();
    return !ext.isEmpty() && ext.size() <= 6 && std::ranges::all_of(ext, isAsciiLetterOrDigit)
        && std::ranges::any_of(ext, [](QChar c) { return c.isLetter(); });
}

// Whether a shell's arguments give it something to run: a command (/k,
// -Command, -m pydoc), or a script, file, path, address or script block.
bool namesSomethingToRun(const QString& arguments)
{
    static constexpr QStringView kRunSwitches[] {u"/c", u"/k", u"-c", u"-command", u"-e", u"-ec", u"-encodedcommand",
        u"-f", u"-file", u"-m", u"-jar", u"-cp", u"-classpath"};
    for (const QString& word : queryWords(arguments)) {
        if (word.startsWith(u'-') || word.startsWith(u'/')) {
            if (std::ranges::any_of(kRunSwitches, [&](QStringView s) { return sameFolded(word, s); }))
                return true;
            continue; // an option; the word after it may be its value ("~")
        }
        if (word.contains(u'\\') || word.contains(u':') || word.contains(u'{') || word.contains(u'&')
            || hasExtension(word))
            return true;
    }
    return false;
}

// A shortcut that runs something else through a host program: a Visual
// Studio developer prompt is cmd /k vcvars64.bat, not the Command Prompt.
bool runsSomethingElse(QStringView program, const QString& arguments)
{
    return !arguments.isEmpty() && (isLauncher(program) || (isShell(program) && namesSomethingToRun(arguments)));
}

// "a; b；c | d": split at semicolons, ASCII or full-width, and bars.
QStringList splitList(QStringView list)
{
    QStringList out;
    qsizetype start = 0;
    for (qsizetype i = 0; i <= list.size(); ++i) {
        if (i < list.size() && list[i] != u';' && list[i] != u'；' && list[i] != u'|')
            continue;
        const QStringView item = list.sliced(start, i - start).trimmed();
        if (!item.isEmpty())
            out.append(item.toString());
        start = i + 1;
    }
    return out;
}

// Two or more ASCII letters and digits, folded; empty for anything else.
std::string abbreviationOf(QStringView word)
{
    if (word.size() < 2 || !std::ranges::all_of(word, isAsciiLetterOrDigit))
        return {};
    return word.toString().toLower().toStdString();
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
        return x.name == y.name && x.id == y.id && x.target == y.target && x.arguments == y.arguments
            && x.kind == y.kind && x.elevatable == y.elevatable && x.installedNames == y.installedNames;
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

// The ASCII words of a name, folded: spaces, punctuation, non-ASCII, digit/letter
// changes and camelCase all separate them ("PowerShell" -> power, shell;
// "XMLParser" -> xml, parser; "Office365" -> office, 365).
std::vector<std::string> splitWords(std::string_view name)
{
    enum Cls { Other, Lower, Upper, Digit };
    const auto cls = [](char c) {
        if (c >= 'a' && c <= 'z')
            return Lower;
        if (c >= 'A' && c <= 'Z')
            return Upper;
        if (c >= '0' && c <= '9')
            return Digit;
        return Other;
    };
    std::vector<std::string> out;
    std::string cur;
    const auto flush = [&] {
        if (!cur.empty())
            out.push_back(std::move(cur));
        cur.clear();
    };
    for (std::size_t i = 0; i < name.size(); ++i) {
        const Cls c = cls(name[i]);
        if (c == Other) {
            flush();
            continue;
        }
        if (!cur.empty()) {
            const Cls p = cls(name[i - 1]);
            const bool next = i + 1 < name.size() && cls(name[i + 1]) == Lower;
            if ((p == Digit) != (c == Digit) || (p == Lower && c == Upper) || (p == Upper && c == Upper && next))
                flush();
        }
        cur += static_cast<char>(c == Upper ? name[i] - 'A' + 'a' : name[i]);
    }
    flush();
    return out;
}

// Can q be cut into non-empty prefixes of consecutive words, from words[wi]
// on? Best bonus of the ways to do it, or -1.
int spanFrom(const std::vector<std::string>& words, std::size_t wi, std::string_view q)
{
    if (wi >= words.size())
        return -1;
    const std::string& w = words[wi];
    int best = -1;
    for (std::size_t n = 1; n <= std::min(w.size(), q.size()); ++n) {
        if (w[n - 1] != q[n - 1])
            break;
        if (n == q.size()) {
            best = std::max(best, n == w.size() ? 6 : 0);
            continue;
        }
        if (const int rest = spanFrom(words, wi + 1, q.substr(n)); rest >= 0)
            best = std::max(best, rest);
    }
    return best;
}

// "vscode" for Visual Studio Code: v|s|code, each a prefix of the next word,
// no word skipped, at least two words. Score 30, +6 from the first word, +6
// when the last piece is a whole word; -1 when it does not fit.
int wordSpanScore(const AppInfo& app, std::string_view q)
{
    if (q.size() < 3 || q.size() > 32 || app.words.size() < 2)
        return -1;
    int best = -1;
    for (std::size_t start = 0; start + 1 < app.words.size(); ++start) {
        const std::string& w = app.words[start];
        if (w.empty() || w[0] != q[0])
            continue;
        // Pieces must cover two words at least: a single word is the substring path's.
        for (std::size_t n = 1; n <= std::min(w.size(), q.size() - 1); ++n) {
            if (w[n - 1] != q[n - 1])
                break;
            if (const int rest = spanFrom(app.words, start + 1, q.substr(n)); rest >= 0)
                best = std::max(best, 30 + rest + (start == 0 ? 6 : 0));
        }
    }
    return best;
}

} // namespace

AppAliases AppAliases::parse(QStringView text)
{
    struct Block {
        Entry entry;
        QStringList ids, packages, programs, names; // what finds the app
    };
    std::vector<Block> blocks;
    for (QStringView line : text.tokenize(u'\n')) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(u'#'))
            continue;
        if (line.startsWith(u'[') && line.endsWith(u']')) {
            blocks.push_back({});
            blocks.back().entry.label = line.sliced(1, line.size() - 2).trimmed().toString();
            continue;
        }
        const qsizetype eq = line.indexOf(u'=');
        if (blocks.empty() || eq < 0)
            continue;
        const QString field = line.first(eq).trimmed().toString().toLower();
        const QStringList values = splitList(line.sliced(eq + 1));
        Block& b = blocks.back();
        if (field == u"id")
            b.ids += values;
        else if (field == u"package")
            b.packages += values;
        else if (field == u"program")
            b.programs += values;
        else if (field == u"title")
            b.names += values;
        else if (field == u"names")
            b.entry.names += values;
        else if (field == u"aliases")
            b.entry.aliases += values;
        else if (field == u"short") {
            for (const QString& value : values) {
                if (std::string a = abbreviationOf(value); !a.empty())
                    b.entry.abbreviations.push_back(std::move(a));
            }
        }
    }

    AppAliases table;
    for (Block& b : blocks) {
        if (b.ids.isEmpty() && b.packages.isEmpty() && b.programs.isEmpty() && b.names.isEmpty())
            continue;
        const std::size_t at = table.m_entries.size();
        // The first block to claim a key keeps it.
        const auto claim = [at](QHash<QString, std::size_t>& map, const QString& key) {
            if (!key.isEmpty() && !map.contains(key.toCaseFolded()))
                map.insert(key.toCaseFolded(), at);
        };
        for (const QString& id : std::as_const(b.ids))
            claim(table.m_byId, id);
        for (const QString& package : std::as_const(b.packages))
            claim(table.m_byPackage, package);
        for (QString program : std::as_const(b.programs)) {
            program.replace(u'/', u'\\');
            if (program.endsWith(u".exe", Qt::CaseInsensitive))
                program.chop(4);
            if (!program.contains(u'\\')) {
                claim(table.m_byProgram, program);
                continue;
            }
            // Google\Chrome\Application\chrome: the end of the program's path.
            if (!program.startsWith(u'\\'))
                program.prepend(u'\\');
            table.m_byPath.push_back({(program + u".exe").toCaseFolded(), at});
        }
        for (const QString& name : std::as_const(b.names))
            claim(table.m_byName, name);
        table.m_entries.push_back(std::move(b.entry));
    }
    return table;
}

const AppAliases& AppAliases::builtIn()
{
    static const AppAliases table = [] {
        QFile file(u":/winshun/apps.txt"_s);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning("Apps: %s is missing: apps go by their own names only", qUtf8Printable(file.fileName()));
            return AppAliases();
        }
        return parse(QString::fromUtf8(file.readAll()));
    }();
    return table;
}

const AppAliases::Entry* AppAliases::find(
    QStringView id, QStringView program, QStringView target, QStringView name) const
{
    const auto lookUp = [&](const QHash<QString, std::size_t>& map, QStringView key) -> const Entry* {
        if (key.isEmpty())
            return nullptr;
        const auto it = map.constFind(key.toString().toCaseFolded());
        return it == map.cend() ? nullptr : &m_entries[*it];
    };
    if (const Entry* e = lookUp(m_byId, id))
        return e;
    if (const qsizetype bang = id.indexOf(u'!'); bang > 0) {
        if (const Entry* e = lookUp(m_byPackage, id.first(bang)))
            return e;
    }
    if (!target.isEmpty()) {
        for (const auto& [ending, at] : m_byPath) {
            if (target.endsWith(ending, Qt::CaseInsensitive))
                return &m_entries[at];
        }
    }
    if (const Entry* e = lookUp(m_byProgram, program))
        return e;
    return lookUp(m_byName, name);
}

void AppInfo::prepare()
{
    prepare(AppAliases::builtIn());
}

void AppInfo::prepare(const AppAliases& table)
{
    nameUtf8 = wtf8::fromUtf16(wtf8::view(name));
    const QString file = target.mid(target.lastIndexOf(u'\\') + 1);
    const qsizetype dot = file.lastIndexOf(u'.');
    const QString suffix = dot > 0 ? file.mid(dot + 1).toLower() : QString();

    // The program a desktop app is; not cmd.exe for a VS developer prompt.
    const QString stem = kind == AppKind::Desktop && suffix == u"exe" ? file.left(dot) : QString();
    hosted = !stem.isEmpty() && runsSomethingElse(stem, arguments);
    const QString own = hosted ? QString() : stem;
    program = wtf8::fromUtf16(wtf8::view(own));

    otherNames.clear();
    otherNamesUtf8.clear();
    aliases.clear();
    abbreviations.clear();
    if (const AppAliases::Entry* entry = table.find(id, own, own.isEmpty() ? QString() : target, name)) {
        for (const QString& other : entry->names) {
            if (!sameFolded(other, name))
                otherNames.append(other);
        }
        for (const QString& alias : entry->aliases) {
            if (!sameFolded(alias, name) && !otherNames.contains(alias, Qt::CaseInsensitive))
                aliases.push_back(wtf8::fromUtf16(wtf8::view(alias)));
        }
        abbreviations = entry->abbreviations;
    }
    for (const QString& other : std::as_const(otherNames))
        otherNamesUtf8.push_back(wtf8::fromUtf16(wtf8::view(other)));

    initials.clear();
    for (std::size_t i = 0; i < nameUtf8.size(); ++i) {
        if (isAsciiAlnum(nameUtf8[i]) && text::isWordStart(nameUtf8, i))
            initials += static_cast<char>(text::fold(nameUtf8[i]));
    }

    words = splitWords(nameUtf8);

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
    const AppList& apps, const ParsedQuery& query, const NameMatcher& matcher, const QStringList& history,
    const History::Affinities& affinities)
{
    std::vector<AppHit> hits;
    if (query.isEmpty())
        return hits;

    QHash<QString, int> recent; // id -> position in the history, 0 = newest
    for (int i = 0; i < history.size(); ++i) {
        if (isAppLaunchPath(history[i]))
            recent.insert(appIdOf(history[i]), std::min(recent.value(appIdOf(history[i]), i), i));
    }

    // A lone plain term may be the initials of the name ("vsc" for Visual
    // Studio Code), or an abbreviation of it typed in full ("ps").
    std::string_view initials;
    if (query.terms.size() == 1 && query.extensions.empty()) {
        const QueryTerm& t = query.terms.front();
        if (!t.negated && !t.wildcard && t.ancestors.empty() && t.text.size() >= 2
            && std::all_of(t.text.begin(), t.text.end(), isAsciiAlnum))
            initials = t.text;
    }

    // Plain ASCII terms only ("vscode", "vs code"): the word-prefix span match
    // takes them joined, so Visual Studio Code is v|s|code.
    std::string joined;
    if (query.extensions.empty() && !query.filter.any()) {
        for (const QueryTerm& t : query.terms) {
            if (t.negated || t.wildcard || !t.ancestors.empty() || t.text.empty()
                || !std::all_of(t.text.begin(), t.text.end(), isAsciiAlnum)) {
                joined.clear();
                break;
            }
            joined += t.text;
        }
    }

    // One letter alone is not read as pinyin (it is in most Chinese names),
    // but it brings back an app opened before whose name has a character
    // starting with it: "y" after 网易云音乐 was opened from "yy" (GitHub #292).
    char letter = 0;
    if (query.terms.size() == 1 && query.extensions.empty() && !query.filter.any()) {
        const QueryTerm& t = query.terms.front();
        if (!t.negated && !t.wildcard && !t.regex && t.ancestors.empty() && t.text.size() == 1 && isAsciiAlnum(t.text.front()))
            letter = t.text.front();
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
        // "计算器" for Calculator, and "微信" (or "wx") for WeChat.exe whatever
        // its shortcut is named, as much as its own name.
        for (std::size_t k = 0; k < app.otherNamesUtf8.size(); ++k)
            better(matcher.matchName(app.otherNamesUtf8[k]), static_cast<int>(k));
        for (const std::string& alias : app.aliases)
            better(matcher.matchName(alias), -1);
        if (!app.program.empty()) {
            if (const int s = matcher.matchName(app.program); s >= 0)
                better(s - 6, -1); // "winword" finds Word, a little below a name match
        }
        // "office ltsc" finds Word and Excel, below every app named so itself:
        // "visio" is Visio first, then the suite it was installed with.
        for (const std::string& installed : app.installedNames) {
            if (const int s = matcher.matchName(installed); s >= 0)
                better(std::max(s - kInstalledNamePenalty, 1), -1);
        }
        // Whole or not at all: "p" and "s" are in too many names to pull in Photoshop.
        const bool abbreviation
            = !initials.empty() && std::ranges::find(app.abbreviations, initials) != app.abbreviations.end();
        if (abbreviation)
            better(kAbbreviationScore, -1);
        if (!initials.empty() && app.initials.starts_with(initials))
            better(initials.size() == app.initials.size() ? 45 : 30, -1);
        else if (!initials.empty() && text::wordInitialsMatch(app.nameUtf8, initials))
            better(28, -1);
        if (!joined.empty())
            better(wordSpanScore(app, joined), -1);
        if (score < 0 && letter != 0 && recent.contains(app.id) && pinyin::findInitialUtf16(wtf8::view(app.name), letter))
            score = 20; // below what has the letter itself, until its use lifts it
        if (score < 0)
            continue;
        const auto typed = [&](const std::string& n) { return matcher.isExactName(n, false); };
        const bool exact = abbreviation || typed(app.nameUtf8) || std::ranges::any_of(app.otherNamesUtf8, typed)
            || std::ranges::any_of(app.aliases, typed);
        if (app.auxiliary)
            score -= 30;
        if (recent.contains(app.id))
            score += affinities.isEmpty() ? 4 : History::affinity(affinities, app.launchPath()).bonus();
        hits.push_back({i, score, otherName, exact});
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

bool appTargetGone(const QString& target)
{
    // "C:\..." only: a share, "\\?\", a relative name or a shell object is not asked.
    if (target.size() < 4 || !target[0].isLetter() || target[0].unicode() >= 128 || target[1] != u':'
        || target[2] != u'\\')
        return false;
    // A drive not there now (PATH_NOT_FOUND too), a USB stick or a mapped
    // share: the program may well come back with it.
    const wchar_t root[] {static_cast<wchar_t>(target[0].unicode()), L':', L'\\', 0};
    if (::GetDriveTypeW(root) != DRIVE_FIXED)
        return false;
    std::wstring path = target.toStdWString();
    if (path.size() >= MAX_PATH - 12)
        path.insert(0, L"\\\\?\\");
    if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return false;
    const DWORD error = ::GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

AppList presentApps(const AppList& apps, const std::function<bool(const QString&)>& gone)
{
    AppList present;
    present.reserve(apps.size());
    for (const AppInfo& app : apps) {
        if (app.kind != AppKind::Desktop || app.target.isEmpty() || !gone(app.target))
            present.push_back(app);
    }
    return present;
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

void AppCatalog::setApps(std::shared_ptr<const AppList> apps)
{
    std::lock_guard lock(m_mutex);
    m_apps = std::move(apps);
}

bool programsCheckDue(std::optional<std::chrono::steady_clock::duration> sinceLast, bool reloaded, bool asked)
{
    return reloaded || asked || !sinceLast || *sinceLast >= kProgramsCheckEvery;
}

void AppCatalog::refresh(bool force)
{
    start(force, false);
}

void AppCatalog::checkPrograms()
{
    start(false, true);
}

void AppCatalog::start(bool force, bool check)
{
    if (m_busy) {
        m_again = m_again || force;
        m_checkAgain = m_checkAgain || check;
        return;
    }
    m_busy = true;
    const auto now = std::chrono::steady_clock::now();
    force = force || !isLoaded() || now - m_loadedAt > kMaxAge;
    const auto sinceCheck = m_checkedAt ? std::optional(now - *m_checkedAt) : std::nullopt;
    if (m_worker.joinable())
        m_worker.join(); // the previous read; it has posted its outcome, so it is done
    m_worker = std::jthread([this, force, check, sinceCheck, known = m_fingerprint, all = m_all](std::stop_token stop) {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        Outcome outcome;
        outcome.fingerprint = installedAppsFingerprint();
        std::shared_ptr<const AppList> shell = all; // a failed read keeps the last one
        if (force || outcome.fingerprint != known || !shell) {
            outcome.reloaded = true;
            AppList apps = loadInstalledApps(stop);
            if (!apps.empty())
                shell = outcome.all = std::make_shared<const AppList>(std::move(apps));
        }
        // The shell keeps listing a shortcut whose program was deleted, and
        // nothing in the Start menu changes then: asked of the disk, one look
        // per desktop app (#207). Not each time the search box opens, as that
        // would keep waking a hard disk that went to sleep: after a read (so
        // a new list is never shown before it is looked at), when an app did
        // not open, and otherwise every few minutes. Until then the list as
        // last looked at stays.
        if (shell && !stop.stop_requested() && programsCheckDue(sinceCheck, outcome.reloaded, check)) {
            outcome.checked = true;
            AppList present = presentApps(*shell);
            outcome.apps = present.size() == shell->size() ? shell : std::make_shared<const AppList>(std::move(present));
        }
        if (!stop.stop_requested())
            QMetaObject::invokeMethod(this, [this, outcome] { publish(outcome); }, Qt::QueuedConnection);
    });
}

void AppCatalog::publish(const Outcome& outcome)
{
    m_busy = false;
    if (outcome.all) { // read anew: not when only the programs were looked at, or kMaxAge would never come
        m_all = outcome.all;
        m_fingerprint = outcome.fingerprint;
        m_loadedAt = std::chrono::steady_clock::now();
    }
    if (outcome.checked)
        m_checkedAt = std::chrono::steady_clock::now();
    if (outcome.apps) {
        bool same = false;
        {
            std::lock_guard lock(m_mutex);
            same = m_apps && (m_apps == outcome.apps || sameApps(*m_apps, *outcome.apps));
            if (!same)
                m_apps = outcome.apps;
        }
        if (!same)
            emit changed();
    }
    // A failed read keeps the old fingerprint, so the next refresh tries again.
    const bool again = std::exchange(m_again, false);
    const bool checkAgain = std::exchange(m_checkAgain, false);
    if (again || checkAgain)
        start(again, checkAgain);
}

namespace {

// Where installed programs are listed: the same keys Settings > 应用 reads.
const std::pair<HKEY, const wchar_t*> kUninstallKeys[] {
    {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
    {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
    {HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"},
};

std::wstring registryString(HKEY key, const wchar_t* name)
{
    DWORD size = 0; // %VARS% expanded: RRF_NOEXPAND is not asked for
    if (::RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS
        || size < sizeof(wchar_t))
        return {};
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, value.data(), &size)
        != ERROR_SUCCESS)
        return {};
    while (!value.empty() && value.back() == L'\0')
        value.pop_back();
    return value;
}

DWORD registryDword(HKEY key, const wchar_t* name)
{
    DWORD value = 0;
    DWORD size = sizeof value;
    return ::RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS ? value : 0;
}

// An install folder as installers write it: in quotes, with a closing '\'.
QString installFolder(const std::wstring& text)
{
    QString folder = QString::fromStdWString(text).trimmed();
    if (folder.size() >= 2 && folder.startsWith(u'"') && folder.endsWith(u'"'))
        folder = folder.mid(1, folder.size() - 2).trimmed();
    folder = QDir::toNativeSeparators(folder);
    while (folder.size() > 3 && folder.endsWith(u'\\'))
        folder.chop(1);
    return folder;
}

bool inFolder(const QString& path, const QString& folder)
{
    return !folder.isEmpty() && path.size() > folder.size() && path[folder.size()] == u'\\'
        && path.startsWith(folder, Qt::CaseInsensitive);
}

// A subkey added or removed updates the key's write time: how an install,
// an uninstall or an update (a new version's key) shows here.
void addUninstallKeys(Fnv1a& hash)
{
    for (const auto& [root, path] : kUninstallKeys) {
        win32::UniqueKey key;
        if (::RegOpenKeyExW(root, path, 0, KEY_READ, key.out()) != ERROR_SUCCESS)
            continue;
        DWORD subkeys = 0;
        FILETIME written {};
        if (::RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, &written)
            == ERROR_SUCCESS) {
            hash.add(&subkeys, sizeof subkeys);
            hash.add(&written, sizeof written);
        }
    }
}

} // namespace

bool sharedInstallFolder(const QString& folder, const QStringList& shared)
{
    return folder.size() <= 3 || shared.contains(folder, Qt::CaseInsensitive); // "C:\" too
}

std::vector<InstalledName> installedNames()
{
    // Where many programs live side by side: an installer that gave one of
    // these as its own folder would lend its name to all of them.
    QStringList shared;
    for (const char* variable : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432", "CommonProgramFiles",
             "CommonProgramFiles(x86)", "ProgramData", "SystemRoot", "USERPROFILE", "APPDATA", "LOCALAPPDATA", "PUBLIC"}) {
        if (const QString folder = installFolder(qEnvironmentVariable(variable).toStdWString()); !folder.isEmpty())
            shared.append(folder);
    }
    for (const QString& inside : {uR"(\System32)"_s, uR"(\SysWOW64)"_s})
        shared.append(installFolder(qEnvironmentVariable("SystemRoot").toStdWString()) + inside);
    shared.append(installFolder(qEnvironmentVariable("LOCALAPPDATA").toStdWString()) + uR"(\Programs)"_s);

    std::vector<InstalledName> names;
    for (const auto& [root, path] : kUninstallKeys) {
        win32::UniqueKey list;
        if (::RegOpenKeyExW(root, path, 0, KEY_READ, list.out()) != ERROR_SUCCESS)
            continue;
        DWORD subkeys = 0;
        if (::RegQueryInfoKeyW(list.get(), nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr)
            != ERROR_SUCCESS)
            continue;
        for (DWORD i = 0; i < subkeys; ++i) {
            wchar_t child[256];
            DWORD length = static_cast<DWORD>(std::size(child));
            if (::RegEnumKeyExW(list.get(), i, child, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                continue;
            win32::UniqueKey entry;
            if (::RegOpenKeyExW(list.get(), child, 0, KEY_READ, entry.out()) != ERROR_SUCCESS)
                continue;
            // A component, an update or a patch: not software of its own.
            if (registryDword(entry.get(), L"SystemComponent") != 0
                || !registryString(entry.get(), L"ParentKeyName").empty())
                continue;
            const std::wstring name = registryString(entry.get(), L"DisplayName");
            const QString folder = installFolder(registryString(entry.get(), L"InstallLocation"));
            if (name.empty() || folder.isEmpty() || sharedInstallFolder(folder, shared))
                continue;
            names.push_back({QString::fromStdWString(name), folder});
        }
    }
    std::ranges::sort(names, [](const InstalledName& a, const InstalledName& b) {
        return a.folder != b.folder ? a.folder < b.folder : a.name < b.name;
    });
    return names;
}

namespace {

// Whether `name` holds `word` as words of its own: "Microsoft Visio LTSC 2024"
// holds "Visio", not "Vis".
bool holdsWord(const QString& name, const QString& word)
{
    if (word.size() < 2)
        return false;
    for (qsizetype at = name.indexOf(word, 0, Qt::CaseInsensitive); at >= 0;
        at = name.indexOf(word, at + 1, Qt::CaseInsensitive)) {
        const qsizetype end = at + word.size();
        if ((at == 0 || !name[at - 1].isLetterOrNumber()) && (end == name.size() || !name[end].isLetterOrNumber()))
            return true;
    }
    return false;
}

} // namespace

void addInstalledNames(AppList& apps, const std::vector<InstalledName>& names)
{
    const auto installedIn = [](const AppInfo& app, const InstalledName& one) {
        return app.kind == AppKind::Desktop && inFolder(app.target, one.folder);
    };
    // A name that says one of the folder's apps is that app's alone: Visio is
    // installed in Office's folder under "Microsoft Visio LTSC 2024", and
    // "visio" is not to bring Word and Excel along.
    std::vector<bool> ofOneApp(names.size());
    for (std::size_t n = 0; n < names.size(); ++n) {
        ofOneApp[n] = std::ranges::any_of(apps, [&](const AppInfo& app) {
            return installedIn(app, names[n]) && holdsWord(names[n].name, app.name);
        });
    }
    for (AppInfo& app : apps) {
        app.installedNames.clear();
        if (app.kind != AppKind::Desktop || app.target.isEmpty())
            continue;
        qsizetype deepest = 0;
        for (const InstalledName& one : names) {
            if (one.folder.size() > deepest && inFolder(app.target, one.folder))
                deepest = one.folder.size();
        }
        if (deepest == 0)
            continue;
        for (std::size_t n = 0; n < names.size(); ++n) {
            const InstalledName& one = names[n];
            if (one.folder.size() != deepest || !inFolder(app.target, one.folder))
                continue;
            if (ofOneApp[n] && !holdsWord(one.name, app.name))
                continue;
            if (sameFolded(one.name, app.name) || app.otherNames.contains(one.name, Qt::CaseInsensitive))
                continue; // found by that name already, and at its full worth
            std::string name = wtf8::fromUtf16(wtf8::view(one.name));
            if (std::ranges::find(app.aliases, name) == app.aliases.end()
                && std::ranges::find(app.installedNames, name) == app.installedNames.end())
                app.installedNames.push_back(std::move(name));
        }
    }
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
            app.arguments = stringProperty(properties.Get(), PKEY_Link_Arguments).trimmed();
            app.kind = AppKind::Desktop;
            const qsizetype dot = target.lastIndexOf(u'.');
            app.elevatable = dot > 0 && elevatableSuffixes().contains(target.mid(dot + 1), Qt::CaseInsensitive);
        }
        app.prepare();
        apps.push_back(std::move(app));
    }
    if (stop.stop_requested())
        return {};

    // Settings > 应用's names for the software in these folders: what people
    // type for it ("7-Zip 25.01", "Microsoft Office") is often not what its
    // shortcut is called ("7-Zip File Manager", "Word").
    addInstalledNames(apps, installedNames());

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
    // The installed software's own names come from the Uninstall keys too.
    addUninstallKeys(hash);
    return hash.value();
}

} // namespace ws
