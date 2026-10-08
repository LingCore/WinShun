#include "SystemCatalog.h"

#include "Pinyin.h"
#include "Query.h"
#include "TextUtil.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>

#include <windows.h>
// After windows.h; ole2.h first, as WIN32_LEAN_AND_MEAN keeps it out of windows.h.
#include <ole2.h>

#include <shlwapi.h>
#include <versionhelpers.h>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr QStringView kPathPrefix = u"winshun-place:";

// HostID of the entries Windows Security shows. Their names resolve only in
// English, so WinShun's own list names the main pages instead.
constexpr QStringView kSecurityHost = u"{6E6DDBCB-9C89-434B-A994-D5F22239523B}";

// The Settings app, whose logo the pages of Settings show.
constexpr QStringView kSettingsApp = u"app:windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel";
constexpr QStringView kControlPanel = u"shell:::{26EE0668-A00A-44D7-9371-BEB064C98683}";

struct ComApartment {
    ComApartment()
        : hr(::CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE))
    {
    }
    ~ComApartment()
    {
        if (SUCCEEDED(hr))
            ::CoUninitialize();
    }
    HRESULT hr;
};

const wchar_t* wide(const QString& s)
{
    return reinterpret_cast<const wchar_t*>(s.utf16());
}

bool fileExists(const QString& path)
{
    return ::GetFileAttributesW(wide(path)) != INVALID_FILE_ATTRIBUTES;
}

// "ms-settings:display", "shell:::{...}", "windowsdefender://threat": a URI
// or shell location, not a path ("C:\..." has its colon second).
bool isUri(QStringView s)
{
    const qsizetype colon = s.indexOf(u':');
    if (colon < 2)
        return false;
    return std::all_of(s.begin(), s.begin() + colon, [](QChar c) {
        return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || c == u'-'
            || c == u'.' || c == u'+';
    });
}

// "a; b；c": split at ASCII and full-width semicolons.
QStringList splitKeywords(QStringView list)
{
    QStringList out;
    qsizetype start = 0;
    for (qsizetype i = 0; i <= list.size(); ++i) {
        if (i < list.size() && list[i] != u';' && list[i] != u'；')
            continue;
        const QStringView word = list.mid(start, i - start).trimmed();
        if (!word.isEmpty())
            out.append(word.toString());
        start = i + 1;
    }
    return out;
}

// Indirect strings ("@shell32.dll,-24389", "@{windows?ms-resource://...}")
// in the display language, each looked up once.
class StringResolver {
public:
    QString operator()(const QString& s)
    {
        if (!s.startsWith(u'@'))
            return s;
        const auto it = m_cache.constFind(s);
        if (it != m_cache.cend())
            return *it;
        QString text = load(s);
        // "@%SystemRoot%\System32\SyncCenter.dll,-3000#immutable1"
        if (text.isEmpty() && s.contains(u'#'))
            text = load(s.left(s.indexOf(u'#')));
        m_cache.insert(s, text);
        return text;
    }

private:
    static QString load(const QString& s)
    {
        wchar_t buffer[4096];
        if (FAILED(::SHLoadIndirectString(wide(s), buffer, static_cast<UINT>(std::size(buffer)), nullptr)))
            return {};
        return QString::fromWCharArray(buffer);
    }

    QHash<QString, QString> m_cache;
};

// "HKEY_LOCAL_MACHINE\SOFTWARE\..." opened for reading.
bool openKey(const QString& path, win32::UniqueKey& key)
{
    const qsizetype slash = path.indexOf(u'\\');
    const QString root = path.left(slash);
    HKEY hive = nullptr;
    if (root == u"HKEY_LOCAL_MACHINE")
        hive = HKEY_LOCAL_MACHINE;
    else if (root == u"HKEY_CURRENT_USER")
        hive = HKEY_CURRENT_USER;
    else if (root == u"HKEY_CLASSES_ROOT")
        hive = HKEY_CLASSES_ROOT;
    else
        return false;
    const QString subkey = slash < 0 ? QString() : path.mid(slash + 1);
    return ::RegOpenKeyExW(hive, wide(subkey), 0, KEY_READ | KEY_WOW64_64KEY, key.out()) == ERROR_SUCCESS;
}

// The value as text (numbers in decimal); null if it is missing.
QString registryValue(const QString& keyPath, const QString& name)
{
    win32::UniqueKey key;
    if (!openKey(keyPath, key))
        return {};
    DWORD type = 0;
    wchar_t buffer[1024] {};
    DWORD size = sizeof buffer - sizeof(wchar_t);
    if (::RegQueryValueExW(key.get(), wide(name), nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size) != ERROR_SUCCESS)
        return {};
    if (type == REG_DWORD && size >= sizeof(DWORD))
        return QString::number(*reinterpret_cast<const DWORD*>(buffer));
    if (type == REG_SZ || type == REG_EXPAND_SZ)
        return QString::fromWCharArray(buffer); // terminated: one character was kept spare
    return u""_s; // there, of a type nothing compares with
}

bool isHomeEdition()
{
    DWORD product = 0;
    if (!::GetProductInfo(10, 0, 0, 0, &product))
        return false;
    switch (product) {
    case PRODUCT_CORE:
    case PRODUCT_CORE_N:
    case PRODUCT_CORE_COUNTRYSPECIFIC:
    case PRODUCT_CORE_SINGLELANGUAGE:
    case PRODUCT_HOME_BASIC:
    case PRODUCT_HOME_PREMIUM:
        return true;
    default:
        return false;
    }
}

// An index entry's <Condition>, "shcond://v1#<test>;<negate>;<name>;<value>...":
// whether the entry applies to this PC. Tests WinShun cannot make leave it out.
bool conditionHolds(const QString& condition)
{
    const qsizetype hash = condition.indexOf(u'#');
    if (hash < 0)
        return true;
    const QStringList parts = condition.mid(hash + 1).split(u';');
    const QString test = parts.value(0);
    const auto arg = [&](QStringView name) {
        for (qsizetype i = 2; i + 1 < parts.size(); i += 2) {
            if (parts[i] == name)
                return parts[i + 1];
        }
        return QString();
    };

    bool holds = false;
    if (test == u"IsServer") {
        holds = ::IsWindowsServer();
    } else if (test == u"RegkeyExists") {
        win32::UniqueKey key;
        holds = openKey(arg(u"Regkey"), key);
    } else if (test == u"RegvalExists") {
        holds = !registryValue(arg(u"Regkey"), arg(u"Regval")).isNull();
    } else if (test == u"RegvalEquals") {
        holds = registryValue(arg(u"Regkey"), arg(u"Regval")).compare(arg(u"RegvalExpected"), Qt::CaseInsensitive) == 0;
    } else if (test == u"COMCondition") {
        win32::UniqueKey key;
        holds = openKey(u"HKEY_CLASSES_ROOT\\CLSID\\"_s + arg(u"CLSID"), key);
    } else if (test == u"IsMachineOnDomain") {
        wchar_t domain[256] {};
        DWORD size = static_cast<DWORD>(std::size(domain));
        holds = ::GetComputerNameExW(ComputerNameDnsDomain, domain, &size) && size > 0;
    } else if (test == u"SkuEquals") {
        holds = arg(u"Sku") == u"anyhome" && isHomeEdition(); // "starter" and the like: not Windows 10 or 11
    } else if (test == u"IsPenAvailable") {
        holds = ::GetSystemMetrics(SM_DIGITIZER) & (NID_INTEGRATED_PEN | NID_EXTERNAL_PEN);
    } else if (test == u"IsTouchAvailable") {
        holds = ::GetSystemMetrics(SM_DIGITIZER) & (NID_INTEGRATED_TOUCH | NID_EXTERNAL_TOUCH);
    } else if (test == u"IsTabletPC") {
        holds = ::GetSystemMetrics(SM_TABLETPC) != 0;
    } else if (test == u"IsMobilePC") {
        SYSTEM_POWER_STATUS power {};
        holds = ::GetSystemPowerStatus(&power) && power.BatteryFlag != 128 && power.BatteryFlag != 255; // has a battery
    } else if (test == u"IsUserAdmin") {
        holds = true; // WinShun runs elevated: the user is an administrator
    } else {
        return false; // offline files, IrDA, Mobility Center ...: mostly gone
    }
    return holds != (parts.value(1) == u"1");
}

// The program a command starts, and the console it opens if that is MMC,
// are there (gpedit.msc is not, on Home).
bool commandRunsHere(const QString& command)
{
    const auto [program, args] = splitCommand(command);
    if (isUri(program))
        return true;
    if (program.contains(u'\\') && !fileExists(program))
        return false;
    return !(args.endsWith(u".msc", Qt::CaseInsensitive) && args.contains(u'\\') && !fileExists(args));
}

// What a command opens if its program only hosts it: the console of
// "mmc.exe ...\diskmgmt.msc", the applet of "control.exe mmsys.cpl".
// Empty for anything else.
QString hostedFile(const QString& program, const QString& args)
{
    const QString file = program.mid(program.lastIndexOf(u'\\') + 1);
    if (args.isEmpty() || args.contains(u' ')
        || (file.compare(u"mmc.exe", Qt::CaseInsensitive) != 0 && file.compare(u"control.exe", Qt::CaseInsensitive) != 0))
        return {};
    return args;
}

// The icon of what a command opens: the console or applet it names, else
// its program, found where Windows would find it.
QString iconForCommand(const QString& command)
{
    auto [program, args] = splitCommand(command);
    if (isUri(program))
        return program;
    if (const QString hosted = hostedFile(program, args); !hosted.isEmpty())
        program = hosted;
    if (!program.contains(u'\\')) {
        wchar_t found[MAX_PATH] {};
        if (::SearchPathW(nullptr, wide(program), nullptr, MAX_PATH, found, nullptr))
            return QString::fromWCharArray(found);
    }
    return program;
}

std::pair<QString, QString> splitKeyValue(QStringView line)
{
    const qsizetype eq = line.indexOf(u'=');
    if (eq < 0)
        return {};
    return {line.left(eq).trimmed().toString().toLower(), line.mid(eq + 1).trimmed().toString()};
}

} // namespace

void PlaceInfo::prepare()
{
    nameUtf8 = wtf8::fromUtf16(wtf8::view(name));
    const auto fold = [](const QStringList& words, std::vector<std::string>& out) {
        out.clear();
        for (const QString& word : words) {
            std::string f = text::foldAscii(wtf8::fromUtf16(wtf8::view(word)));
            if (!f.empty() && std::find(out.begin(), out.end(), f) == out.end())
                out.push_back(std::move(f));
        }
    };
    fold(keywords, folded);
    fold(ownKeywords, ownFolded);
}

QString PlaceInfo::path() const
{
    return placePath(key);
}

QString placePath(const QString& key)
{
    return kPathPrefix + key;
}

bool isPlacePath(QStringView path) noexcept
{
    return path.startsWith(kPathPrefix);
}

QString placeKeyOf(QStringView path)
{
    return isPlacePath(path) ? path.mid(kPathPrefix.size()).toString() : QString();
}

namespace {

// One word of the query, to look for in a place's keywords.
struct Word {
    std::string text; // folded
    pinyin::Matcher pinyin; // by each character's most common reading
    bool han = false;
    std::size_t chars = 0; // code points
};

// How well a word matches one keyword (folded): -1 if it does not. A whole
// keyword beats a name the word merely begins (30 + coverage), and pinyin
// initials that happen to fit a name ("dns": 电脑设置); an own keyword
// (places.txt) beats nearly every name.
int keywordScore(std::string_view keyword, const Word& word, bool own)
{
    if (keyword == word.text)
        return own ? 50 : 30;
    int score = -1;
    if (const std::size_t pos = keyword.find(word.text); pos != std::string_view::npos) {
        if (pos == 0)
            score = 14;
        else if (text::isWordStart(keyword, pos))
            score = 10; // "saver" in "battery saver settings"
        else if (word.han)
            score = 8; // "网络连接" in "以太网网络连接"
    }
    if (score < 0 && word.pinyin.valid() && pinyin::hasHan(keyword)) {
        if (const auto span = word.pinyin.findUtf8(keyword); span && text::isWordStart(keyword, span->start))
            score = span->start == 0 && span->length == keyword.size() ? 14 : 10; // "spq" for 适配器
    }
    return score < 0 || !own ? score : score + 6;
}

} // namespace

std::vector<PlaceHit> searchPlaces(
    const PlaceList& places, const ParsedQuery& query, const NameMatcher& matcher, const QStringList& history)
{
    std::vector<PlaceHit> hits;
    if (query.isEmpty())
        return hits;

    QHash<QString, int> recent; // key -> position in the history, 0 = newest
    for (int i = 0; i < history.size(); ++i) {
        if (isPlacePath(history[i]) && !recent.contains(placeKeyOf(history[i])))
            recent.insert(placeKeyOf(history[i]), i);
    }

    // Keywords answer plain words only: file syntax (ext:, folders,
    // wildcards) is for files. A single letter or character is too little
    // to go by: the name must have it.
    bool byKeyword = query.extensions.empty();
    std::vector<Word> words;
    std::vector<std::string> excluded;
    for (const QueryTerm& t : query.terms) {
        if (t.wildcard || !t.ancestors.empty()) {
            byKeyword = false;
            break;
        }
        if (t.negated) {
            excluded.push_back(t.text);
            continue;
        }
        Word w {t.text, pinyin::Matcher(t.text, true), pinyin::hasHan(t.text), 0};
        w.chars = static_cast<std::size_t>(std::count_if(
            t.text.begin(), t.text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
        words.push_back(std::move(w));
    }
    byKeyword = byKeyword && !words.empty();

    for (std::size_t i = 0; i < places.size(); ++i) {
        const PlaceInfo& place = places[i];
        int score = matcher.matchName(place.nameUtf8);
        const bool excludedByName = std::any_of(excluded.begin(), excluded.end(),
            [&](const std::string& x) { return text::findFolded(place.nameUtf8, x) != text::npos; });
        if (byKeyword && !excludedByName) {
            // Each word in the name or in a keyword.
            int byWords = 0;
            for (const Word& w : words) {
                int best = text::findFolded(place.nameUtf8, w.text) != text::npos
                        || (w.pinyin.valid() && w.pinyin.findUtf8(place.nameUtf8))
                    ? 8
                    : -1;
                if (w.chars >= 2) {
                    for (const std::string& keyword : place.folded)
                        best = std::max(best, keywordScore(keyword, w, false));
                    for (const std::string& keyword : place.ownFolded)
                        best = std::max(best, keywordScore(keyword, w, true));
                }
                if (best < 0) {
                    byWords = -1;
                    break;
                }
                byWords += best;
            }
            score = std::max(score, byWords);
        }
        if (score < 0)
            continue;
        if (const auto it = recent.constFind(place.key); it != recent.cend())
            score += 20 - std::min(*it, 10);
        hits.push_back({i, score});
    }
    // Equal scores: the shorter name, which is more often the page itself
    // than one of the tasks on it.
    std::stable_sort(hits.begin(), hits.end(), [&](const PlaceHit& a, const PlaceHit& b) {
        if (a.score != b.score)
            return a.score > b.score;
        return places[a.index].name.size() < places[b.index].name.size();
    });

    // One row per command: the tasks of a page all open the page.
    QSet<QString> commands;
    std::erase_if(hits, [&](const PlaceHit& hit) {
        const QString command = places[hit.index].command.toLower();
        if (commands.contains(command))
            return true;
        commands.insert(command);
        return false;
    });
    return hits;
}

std::pair<QString, QString> splitCommand(const QString& command)
{
    const QString expanded = QString::fromStdWString(win32::expandEnvironment(command.trimmed().toStdWString()));
    if (expanded.startsWith(u'"')) {
        const qsizetype end = expanded.indexOf(u'"', 1);
        if (end > 0)
            return {expanded.mid(1, end - 1), expanded.mid(end + 1).trimmed()};
    }
    if (isUri(expanded))
        return {expanded, {}};
    const qsizetype space = expanded.indexOf(u' ');
    if (space < 0)
        return {expanded, {}};
    // A path with spaces in it is either quoted or, like "%TEMP%" can be,
    // the whole command.
    if (fileExists(expanded))
        return {expanded, {}};
    return {expanded.left(space), expanded.mid(space + 1).trimmed()};
}

QString describeCommand(const QString& command)
{
    const auto [program, args] = splitCommand(command);
    if (isUri(program))
        return program;
    if (const QString hosted = hostedFile(program, args); !hosted.isEmpty())
        return hosted.mid(hosted.lastIndexOf(u'\\') + 1);
    const QString file = program.mid(program.lastIndexOf(u'\\') + 1);
    if (!args.isEmpty())
        return file + u' ' + args;
    const DWORD attributes = ::GetFileAttributesW(wide(program));
    const bool folder = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    return folder ? program : file;
}

std::vector<SettingsIndexEntry> parseSettingsIndex(QByteArrayView xml)
{
    std::vector<SettingsIndexEntry> entries;
    QXmlStreamReader reader(QByteArray::fromRawData(xml.data(), xml.size()));
    SettingsIndexEntry e;
    bool inside = false;
    int paths = 0; // <Path> elements so far: the page is taken from the first
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::EndElement && reader.name() == u"SearchableContent") {
            if (inside && !e.key.isEmpty())
                entries.push_back(std::move(e));
            e = {};
            inside = false;
            continue;
        }
        if (token != QXmlStreamReader::StartElement)
            continue;
        const QStringView name = reader.name();
        if (name == u"SearchableContent") {
            e = {};
            inside = true;
            paths = 0;
            continue;
        }
        if (!inside)
            continue;
        if (name == u"Path") {
            ++paths;
            continue;
        }
        const bool firstPath = paths <= 1;
        if (name == u"Node") { // newer files: <Node Type="Page">SettingsPageX</Node>
            const bool page = reader.attributes().value(u"Type") == u"Page";
            const QString text = reader.readElementText().trimmed();
            if (page && firstPath && e.page.isEmpty())
                e.page = text;
            continue;
        }
        static constexpr QStringView kLeaves[] {u"Filename", u"DeepLink", u"Icon", u"HostID", u"Condition",
            u"Description", u"HighKeywords", u"Keywords", u"PageID", u"PolicyIds"};
        if (std::find(std::begin(kLeaves), std::end(kLeaves), name) == std::end(kLeaves))
            continue; // a container (ApplicationInformation, SettingPaths ...) or something unused
        const QString text = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
        if (name == u"Filename") {
            e.key = text;
        } else if (name == u"DeepLink") {
            e.deepLink = text;
        } else if (name == u"Icon") {
            e.icon = text;
        } else if (name == u"HostID") {
            e.host = text;
        } else if (name == u"Condition") {
            e.condition = text;
        } else if (name == u"Description") {
            e.description = text;
        } else if (name == u"HighKeywords" || name == u"Keywords") {
            // "@@a.dll,-1@b.dll,-2": several lists
            if (text.startsWith(u"@@")) {
                for (const QString& part : text.mid(2).split(u'@', Qt::SkipEmptyParts))
                    e.keywords.append(u'@' + part);
            } else if (!text.isEmpty()) {
                e.keywords.append(text);
            }
        } else if (name == u"PageID") {
            if (firstPath && e.page.isEmpty())
                e.page = text;
        } else if (name == u"PolicyIds") {
            if (firstPath && e.policyIds.isEmpty())
                e.policyIds = text;
        }
    }
    return entries;
}

QString choosePageUri(const QString& page, const QString& policyIds)
{
    QStringList ids;
    for (const QString& id : policyIds.split(u';', Qt::SkipEmptyParts)) {
        if (!id.trimmed().isEmpty())
            ids.append(id.trimmed());
    }
    if (ids.isEmpty())
        return {};
    const QString pageName = page.toLower();
    QString named;
    for (const QString& id : ids) {
        if (id.size() > named.size() && pageName.contains(QString(id).remove(u'-').toLower()))
            named = id;
    }
    if (!named.isEmpty())
        return named;
    QString best = ids.first();
    qsizetype bestCount = 0;
    for (const QString& id : ids) {
        const qsizetype count = std::count_if(
            ids.cbegin(), ids.cend(), [&](const QString& other) { return other.startsWith(id + u'-'); });
        if (count > bestCount) {
            best = id;
            bestCount = count;
        }
    }
    return best;
}

QString commandForDeepLink(const QString& deepLink)
{
    if (deepLink.isEmpty())
        return {};
    if (deepLink.startsWith(u'%') || deepLink.contains(u":\\") || isUri(deepLink))
        return deepLink;
    // A Control Panel item by its canonical name, maybe with a page:
    // "Microsoft.WindowsFirewall\PageConfigureApps".
    static const QRegularExpression canonical(u"^(Microsoft\\.[A-Za-z0-9]+)(?:\\\\(.+))?$"_s);
    const QRegularExpressionMatch m = canonical.match(deepLink);
    if (!m.hasMatch())
        return {};
    QString command = u"%windir%\\system32\\control.exe /name "_s + m.captured(1);
    if (m.hasCaptured(2))
        command += u" /page "_s + m.captured(2);
    return command;
}

QStringList applyPlaceExtras(PlaceList& places, QStringView text, bool chinese)
{
    struct Block {
        QString header;
        QHash<QString, QString> fields;
    };
    std::vector<Block> blocks;
    for (QStringView line : text.tokenize(u'\n')) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(u'#'))
            continue;
        if (line.startsWith(u'[') && line.endsWith(u']')) {
            blocks.push_back({line.mid(1, line.size() - 2).trimmed().toString(), {}});
            continue;
        }
        const auto [key, value] = splitKeyValue(line);
        if (!blocks.empty() && !key.isEmpty())
            blocks.back().fields.insert(key, value);
    }

    QStringList unmatched;
    for (const Block& b : blocks) {
        const QStringList keywords = splitKeywords(b.fields.value(u"keywords"_s));
        const QString open = b.fields.value(u"open"_s);
        if (open.isEmpty()) { // more keywords for what this command opens
            const auto opens = [&](const PlaceInfo& p) { return p.command.compare(b.header, Qt::CaseInsensitive) == 0; };
            // The page itself, rather than one of its tasks: "壁纸" should
            // show 背景图像设置, not 视差背景.
            const bool page = std::any_of(places.begin(), places.end(), [&](const PlaceInfo& p) { return p.page && opens(p); });
            bool found = false;
            for (PlaceInfo& p : places) {
                if (opens(p) && (p.page || !page)) {
                    p.ownKeywords += keywords;
                    found = true;
                }
            }
            if (!found)
                unmatched.append(b.header);
            continue;
        }
        const QStringList names = b.fields.value(u"name"_s).split(u'|');
        PlaceInfo p;
        p.key = u"winshun/"_s + b.header;
        p.name = (chinese || names.size() < 2 ? names.first() : names[1]).trimmed();
        p.command = open;
        p.icon = b.fields.value(u"icon"_s);
        const QString kind = b.fields.value(u"kind"_s);
        p.kind = kind == u"settings" ? AppKind::Setting
            : kind == u"control"     ? AppKind::ControlPanel
            : kind == u"security"    ? AppKind::Security
                                     : AppKind::Tool;
        p.ownKeywords = keywords;
        if (!p.name.isEmpty())
            places.push_back(std::move(p));
    }
    return unmatched;
}

SystemCatalog::SystemCatalog(QObject* parent)
    : QObject(parent)
{
}

SystemCatalog::~SystemCatalog()
{
    m_worker.request_stop();
    if (m_worker.joinable())
        m_worker.join(); // an outcome it already posted is dropped along with this object
}

std::shared_ptr<const PlaceList> SystemCatalog::places() const
{
    static const auto kEmpty = std::make_shared<const PlaceList>();
    std::lock_guard lock(m_mutex);
    return m_places ? m_places : kEmpty;
}

void SystemCatalog::refresh(bool force)
{
    if (m_busy) {
        m_again = m_again || force;
        return;
    }
    m_busy = true;
    {
        std::lock_guard lock(m_mutex);
        force = force || !m_places;
    }
    if (m_worker.joinable())
        m_worker.join(); // the previous read; it has posted its outcome, so it is done
    m_worker = std::jthread([this, force, known = m_fingerprint](std::stop_token stop) {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        Outcome outcome;
        outcome.fingerprint = placesFingerprint();
        if (force || outcome.fingerprint != known) {
            PlaceList places = loadPlaces(stop);
            if (!stop.stop_requested())
                outcome.places = std::make_shared<const PlaceList>(std::move(places));
        }
        if (!stop.stop_requested())
            QMetaObject::invokeMethod(this, [this, outcome] { publish(outcome); }, Qt::QueuedConnection);
    });
}

void SystemCatalog::publish(const Outcome& outcome)
{
    m_busy = false;
    if (outcome.places) {
        m_fingerprint = outcome.fingerprint;
        {
            std::lock_guard lock(m_mutex);
            m_places = outcome.places;
        }
        emit changed();
    }
    if (std::exchange(m_again, false))
        refresh(true);
}

namespace {

QStringList indexFiles()
{
    const QDir dir(qEnvironmentVariable("WINDIR") + u"\\ImmersiveControlPanel\\Settings"_s);
    QStringList files;
    for (const QString& name : dir.entryList({u"AllSystemSettings_*.xml"_s}, QDir::Files, QDir::Name))
        files.append(dir.filePath(name));
    return files;
}

bool displayLanguageIsChinese()
{
    return PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_CHINESE;
}

} // namespace

PlaceList loadPlaces(const std::stop_token& stop)
{
    const ComApartment com; // ms-resource strings go through COM

    // Windows ships one index per edition of Settings, newer ones larger and
    // with more in them; only older ones tell each page's ms-settings: name.
    std::vector<std::vector<SettingsIndexEntry>> indexes;
    for (const QString& path : indexFiles()) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            indexes.push_back(parseSettingsIndex(file.readAll()));
    }
    std::stable_sort(indexes.begin(), indexes.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
    QHash<QString, QString> pageUris;
    for (const auto& index : indexes) {
        for (const SettingsIndexEntry& e : index) {
            if (!e.page.isEmpty() && !e.policyIds.isEmpty() && !pageUris.contains(e.page))
                pageUris.insert(e.page, choosePageUri(e.page, e.policyIds));
        }
    }

    StringResolver resolve;
    PlaceList places;
    QSet<QString> keys;
    QHash<QString, std::size_t> byNameAndCommand; // the same task twice (two versions of its keywords): one place
    for (const auto& index : indexes) {
        for (const SettingsIndexEntry& e : index) {
            if (stop.stop_requested())
                return {};
            if (keys.contains(e.key))
                continue;
            keys.insert(e.key);
            if (e.host == kSecurityHost || (!e.condition.isEmpty() && !conditionHolds(e.condition)))
                continue;

            PlaceInfo p;
            p.key = e.key;
            if (!e.deepLink.isEmpty()) {
                p.command = commandForDeepLink(e.deepLink);
                p.kind = AppKind::ControlPanel;
                p.icon = e.icon.isEmpty() ? kControlPanel.toString() : e.icon;
            } else if (const QString uri = pageUris.value(e.page); !uri.isEmpty()) {
                p.command = u"ms-settings:"_s + uri;
                p.kind = AppKind::Setting;
                p.icon = kSettingsApp.toString();
                p.page = e.key == u"AAA_" + e.page; // "AAA_SettingsPageNetworkEthernet": 以太网设置
            }
            if (p.command.isEmpty() || !commandRunsHere(p.command))
                continue;
            p.name = resolve(e.description).trimmed();
            if (p.name.isEmpty())
                continue;
            for (const QString& list : e.keywords)
                p.keywords += splitKeywords(resolve(list));

            const QString both = p.name + u'\n' + p.command.toLower();
            if (const auto it = byNameAndCommand.constFind(both); it != byNameAndCommand.cend()) {
                places[*it].keywords += p.keywords;
                places[*it].page = places[*it].page || p.page;
                continue;
            }
            byNameAndCommand.insert(both, places.size());
            places.push_back(std::move(p));
        }
    }

    QFile extras(u":/winshun/places.txt"_s);
    if (extras.open(QIODevice::ReadOnly | QIODevice::Text))
        applyPlaceExtras(places, QString::fromUtf8(extras.readAll()), displayLanguageIsChinese());
    std::erase_if(places, [](const PlaceInfo& p) { return p.key.startsWith(u"winshun/") && !commandRunsHere(p.command); });
    for (PlaceInfo& p : places) {
        if (p.icon.isEmpty())
            p.icon = iconForCommand(p.command);
        p.prepare();
    }
    return places;
}

quint64 placesFingerprint()
{
    QString state = QString::number(::GetUserDefaultUILanguage());
    for (const QString& path : indexFiles()) {
        WIN32_FILE_ATTRIBUTE_DATA info {};
        if (!::GetFileAttributesExW(wide(QDir::toNativeSeparators(path)), GetFileExInfoStandard, &info))
            continue;
        const quint64 written = (quint64(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
        state += u'|' + path + u'|' + QString::number(info.nFileSizeLow) + u'|' + QString::number(written);
    }
    return qHash(state, 0);
}

} // namespace ws
