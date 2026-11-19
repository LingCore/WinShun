#include "EverythingIpc.h"

#include "FolderSize.h"
#include "NameSearch.h"
#include "Query.h"
#include "Wtf8.h"

#include <QSet>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>

using namespace Qt::StringLiterals;

namespace ws::everything {

namespace {

constexpr std::size_t kQueryHeader = 5 * sizeof(std::uint32_t);
constexpr std::size_t kQuery2Header = 7 * sizeof(std::uint32_t);
constexpr std::uint32_t kItemFolder = 0x1;
constexpr std::uint32_t kItemRoot = 0x2;
// Adding up the folders of one answer may take this long; the rest are unknown.
constexpr auto kFolderSizeBudget = std::chrono::milliseconds(1500);

std::uint32_t u32At(std::span<const std::byte> data, std::size_t offset) noexcept
{
    std::uint32_t value = 0;
    std::memcpy(&value, data.data() + offset, sizeof value);
    return value;
}

QString textAt(std::span<const std::byte> data, std::size_t offset, bool wide)
{
    const std::span<const std::byte> rest = data.subspan(offset);
    if (wide) {
        const auto* chars = reinterpret_cast<const char16_t*>(rest.data());
        const std::size_t most = rest.size() / sizeof(char16_t);
        std::size_t length = 0;
        while (length < most && chars[length] != u'\0')
            ++length;
        return QString::fromUtf16(chars, static_cast<qsizetype>(length));
    }
    const auto* chars = reinterpret_cast<const char*>(rest.data());
    const std::size_t length = ::strnlen(chars, rest.size());
    if (length == 0)
        return {};
    const int needed = ::MultiByteToWideChar(CP_ACP, 0, chars, static_cast<int>(length), nullptr, 0);
    std::wstring text(static_cast<std::size_t>(std::max(needed, 0)), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, chars, static_cast<int>(length), text.data(), needed);
    return QString::fromStdWString(text);
}

// ---- the query's words -----------------------------------------------------------------

// Split at white space outside quotes; the quotes stay.
QStringList tokensOf(const QString& text)
{
    QStringList tokens;
    QString token;
    bool quoted = false;
    for (const QChar c : text) {
        if (c == u'"')
            quoted = !quoted;
        if (c.isSpace() && !quoted) {
            if (!token.isEmpty())
                tokens.append(std::exchange(token, {}));
            continue;
        }
        token.append(c);
    }
    if (!token.isEmpty())
        tokens.append(token);
    return tokens;
}

// "a|b" is a | b in Everything; Win顺 wants the bar apart.
QString barsApart(const QString& text)
{
    QString out;
    out.reserve(text.size() + 8);
    bool quoted = false;
    for (const QChar c : text) {
        if (c == u'"')
            quoted = !quoted;
        if (c == u'|' && !quoted)
            out.append(u" | ");
        else
            out.append(c);
    }
    return out;
}

QString unquoted(QString text)
{
    text.remove(u'"');
    return text;
}

bool isFolderPath(const QString& path)
{
    if (!path.endsWith(u'\\') || path.contains(u'*') || path.contains(u'?'))
        return false;
    const bool drive = path.size() >= 3 && path[0].isLetter() && path[1] == u':' && path[2] == u'\\';
    return drive || (path.startsWith(u"\\\\") && path.size() > 3);
}

QString withoutEndSlash(QString path)
{
    while (path.endsWith(u'\\'))
        path.chop(1);
    return path;
}

bool hasWildcards(QStringView text)
{
    return text.contains(u'*') || text.contains(u'?');
}

// From a drive ("D:\images") or a share ("\\host\share\docs"), something
// after the root: a full path's start, as Everything reads a word with a \.
bool fromRoot(const QString& path)
{
    if (path.size() > 3 && path[0].isLetter() && path[1] == u':' && path[2] == u'\\')
        return true;
    return path.startsWith(u"\\\\") && path.indexOf(u'\\', 2) > 2;
}

// What a full path's start (Search::pathStarts) is in the index: the entries
// in its folder whose names begin with its last part, which a search takes
// with all under them (NameSearchOptions::within). Sorted; none when its
// folder is not in the index. A share's root ("\\host\share") is itself.
std::vector<EntryId> pathStartEntries(const FileIndex& index, const QString& start)
{
    std::vector<EntryId> within;
    const qsizetype slash = start.lastIndexOf(u'\\');
    const QStringView stem = QStringView(start).mid(slash + 1);
    const EntryId folder = index.findPath(start.left(slash).toStdWString());
    if (folder == kNoEntry || !index.scan(folder).isDir()) {
        if (const EntryId self = index.findPath(start.toStdWString()); self != kNoEntry)
            within.push_back(self);
        return within;
    }
    for (EntryId child = index.entry(folder).firstChild(); child != kNoEntry; child = index.entry(child).nextSibling) {
        if (!index.scan(child).isDeleted() && wtf8::toQString(index.name(child)).startsWith(stem, Qt::CaseInsensitive))
            within.push_back(child);
    }
    std::ranges::sort(within);
    return within;
}

// The folders of a group, <"C:\a\" | "D:\b\">: how Directory Opus names
// where to look. None when it holds anything else (words: not Win顺's to do).
std::optional<QStringList> groupFolders(const QString& inner)
{
    QStringList folders;
    for (const QString& token : tokensOf(barsApart(inner))) {
        if (token == u"|")
            continue;
        const QString path = unquoted(token);
        if (!isFolderPath(path))
            return std::nullopt;
        folders.append(withoutEndSlash(path));
    }
    return folders;
}

// The query with its groups taken out: those of folders into `search`, any
// other marked as not done.
QString withoutGroups(const QString& text, Search& search)
{
    QString out;
    QString group;
    bool quoted = false;
    bool inGroup = false;
    QChar before = u' ';
    for (const QChar c : text) {
        if (c == u'"')
            quoted = !quoted;
        if (!quoted && !inGroup && c == u'<' && before.isSpace()) {
            inGroup = true; // "size:<1kb" has its < inside a word
            group.clear();
        } else if (!quoted && inGroup && c == u'>') {
            inGroup = false;
            if (const auto folders = groupFolders(group)) {
                for (const QString& folder : *folders) {
                    if (!search.folders.contains(folder, Qt::CaseInsensitive))
                        search.folders.append(folder);
                }
            } else {
                search.unsupported = u"<>"_s;
            }
            out.append(u' ');
        } else if (inGroup) {
            group.append(c);
        } else {
            out.append(c);
        }
        before = c;
    }
    if (inGroup)
        search.unsupported = u"<>"_s;
    return out;
}

// Modifiers that change how a word is compared, not what is looked for: the
// word stays, compared Win顺's way.
const QSet<QString>& plainModifiers()
{
    static const QSet<QString> names {u"case"_s, u"nocase"_s, u"ww"_s, u"wholeword"_s, u"noww"_s, u"nowholeword"_s, u"wfn"_s,
        u"wholefilename"_s, u"nowfn"_s, u"nowholefilename"_s, u"exact"_s, u"path"_s, u"nopath"_s, u"wildcards"_s,
        u"nowildcards"_s, u"diacritics"_s, u"nodiacritics"_s, u"noregex"_s, u"ascii"_s, u"utf8"_s, u"utf16"_s, u"name"_s,
        u"basename"_s};
    return names;
}

// Everything's functions Win顺's search has nothing for.
const QSet<QString>& unsupportedFunctions()
{
    static const QSet<QString> names {u"attrib"_s, u"attributes"_s, u"dc"_s, u"datecreated"_s, u"da"_s, u"dateaccessed"_s,
        u"dr"_s, u"daterun"_s, u"rc"_s, u"recentchange"_s, u"runcount"_s, u"content"_s, u"ansicontent"_s, u"utf8content"_s,
        u"utf16content"_s, u"utf16becontent"_s, u"dupe"_s, u"namepartdupe"_s, u"sizedupe"_s, u"dmdupe"_s, u"dcdupe"_s,
        u"dadupe"_s, u"attribdupe"_s, u"empty"_s, u"child"_s, u"childcount"_s, u"childfilecount"_s, u"childfoldercount"_s,
        u"len"_s, u"startwith"_s, u"endwith"_s, u"type"_s, u"depth"_s, u"parents"_s, u"fsi"_s, u"filelist"_s,
        u"filelistfilename"_s, u"frn"_s, u"shell"_s, u"width"_s, u"height"_s, u"dimensions"_s, u"orientation"_s, u"bitdepth"_s,
        u"album"_s, u"artist"_s, u"genre"_s, u"title"_s, u"track"_s, u"comment"_s, u"year"_s};
    return names;
}

// "name:" at the start of a word, the name in lower case; empty for a
// drive's "C:", a word with no colon, one in quotes.
QString functionOf(const QString& word)
{
    const qsizetype colon = word.indexOf(u':');
    if (colon < 2)
        return {};
    for (qsizetype i = 0; i < colon; ++i) {
        if (!word[i].isLetter() && word[i] != u'-')
            return {};
    }
    return word.left(colon).toLower();
}

SortOrder orderOf(std::uint32_t sortType) noexcept
{
    const bool descending = sortType != 0 && sortType % 2 == 0;
    switch (descending ? sortType - 1 : sortType) {
    case sort::Path:
        return {SortKey::Path, descending};
    case sort::Size:
        return {SortKey::Size, descending};
    case sort::Extension:
    case sort::TypeName:
        return {SortKey::Type, descending};
    case sort::Created:
        return {SortKey::Created, descending};
    case sort::Modified:
        return {SortKey::Modified, descending};
    case sort::Name:
        return {SortKey::Name, descending};
    default: // by what the index does not have: by name
        return {SortKey::Name, false};
    }
}

std::uint64_t ticksOf(FileTime time) noexcept
{
    constexpr std::uint64_t kTicksPerSecond = 10'000'000;
    constexpr std::uint64_t kSeconds1601To1970 = 11'644'473'600;
    return knownTime(time) == 0 ? kUnknown : (time + kSeconds1601To1970) * kTicksPerSecond;
}

// ---- the answer's bytes ----------------------------------------------------------------

class Writer {
public:
    explicit Writer(bool wide)
        : m_wide(wide)
    {
    }
    qsizetype size() const noexcept { return m_bytes.size(); }
    void u32(std::uint32_t value) { m_bytes.append(reinterpret_cast<const char*>(&value), sizeof value); }
    void u64(std::uint64_t value) { m_bytes.append(reinterpret_cast<const char*>(&value), sizeof value); }
    void setU32(qsizetype at, std::uint32_t value) { std::memcpy(m_bytes.data() + at, &value, sizeof value); }
    // The characters and their zero; how many characters (not the zero).
    std::uint32_t chars(std::wstring_view text)
    {
        if (m_wide) {
            m_bytes.append(reinterpret_cast<const char*>(text.data()), static_cast<qsizetype>(text.size() * sizeof(wchar_t)));
            m_bytes.append(2, '\0');
            return static_cast<std::uint32_t>(text.size());
        }
        int length = 0;
        if (!text.empty()) {
            length = ::WideCharToMultiByte(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            const qsizetype at = m_bytes.size();
            m_bytes.resize(at + length);
            ::WideCharToMultiByte(CP_ACP, 0, text.data(), static_cast<int>(text.size()), m_bytes.data() + at, length, nullptr,
                nullptr);
        }
        m_bytes.append('\0');
        return static_cast<std::uint32_t>(length);
    }
    // EVERYTHING_IPC_QUERY2's strings: the length first.
    void counted(std::wstring_view text)
    {
        const qsizetype at = m_bytes.size();
        u32(0);
        setU32(at, chars(text));
    }
    QByteArray take() { return std::move(m_bytes); }

private:
    bool m_wide;
    QByteArray m_bytes;
};

std::uint32_t flagsOf(const Row& row) noexcept
{
    return (row.folder ? kItemFolder : 0) | (row.root ? kItemRoot : 0);
}

// What the index knows of them: a folder or not, hidden or not.
std::uint32_t attributesOf(const Row& row) noexcept
{
    const std::uint32_t attributes = (row.folder ? FILE_ATTRIBUTE_DIRECTORY : 0) | (row.hidden ? FILE_ATTRIBUTE_HIDDEN : 0);
    return attributes ? attributes : FILE_ATTRIBUTE_NORMAL;
}

std::wstring fullPathOf(const Row& row)
{
    return row.path.empty() ? row.name : row.path + L'\\' + row.name;
}

std::wstring_view extensionOf(const Row& row) noexcept
{
    if (row.folder)
        return {};
    const std::size_t dot = row.name.rfind(L'.');
    return dot == std::wstring::npos || dot == 0 ? std::wstring_view() : std::wstring_view(row.name).substr(dot + 1);
}

QByteArray encodeList(const Query& query, const Rows& rows)
{
    Writer out(query.wide);
    const auto count = static_cast<std::uint32_t>(rows.rows.size());
    const auto folders = static_cast<std::uint32_t>(std::ranges::count_if(rows.rows, &Row::folder));
    for (const std::uint32_t value :
        {rows.totalFolders, rows.totalFiles, rows.totalItems, folders, count - folders, count, query.offset})
        out.u32(value);
    const qsizetype items = out.size();
    for (std::uint32_t i = 0; i < count * 3; ++i)
        out.u32(0);
    for (std::uint32_t i = 0; i < count; ++i) {
        const Row& row = rows.rows[i];
        const qsizetype item = items + static_cast<qsizetype>(i) * 12;
        out.setU32(item, flagsOf(row));
        out.setU32(item + 4, static_cast<std::uint32_t>(out.size()));
        out.chars(row.name);
        out.setU32(item + 8, static_cast<std::uint32_t>(out.size()));
        out.chars(row.path);
    }
    return out.take();
}

QByteArray encodeList2(const Query& query, const Rows& rows)
{
    Writer out(query.wide);
    const auto count = static_cast<std::uint32_t>(rows.rows.size());
    const std::uint32_t asked = query.requestFlags;
    for (const std::uint32_t value : {rows.totalItems, count, query.offset, asked, query.sortType})
        out.u32(value);
    const qsizetype items = out.size();
    for (std::uint32_t i = 0; i < count * 2; ++i)
        out.u32(0);
    for (std::uint32_t i = 0; i < count; ++i) {
        const Row& row = rows.rows[i];
        const qsizetype item = items + static_cast<qsizetype>(i) * 8;
        out.setU32(item, flagsOf(row));
        out.setU32(item + 4, static_cast<std::uint32_t>(out.size()));
        // In the order of the flags' bits, as the SDK reads them back.
        if (asked & request::Name)
            out.counted(row.name);
        if (asked & request::Path)
            out.counted(row.path);
        if (asked & request::FullPath)
            out.counted(fullPathOf(row));
        if (asked & request::Extension)
            out.counted(extensionOf(row));
        if (asked & request::Size)
            out.u64(row.size);
        if (asked & request::Created)
            out.u64(row.created);
        if (asked & request::Modified)
            out.u64(row.modified);
        if (asked & request::Accessed)
            out.u64(kUnknown);
        if (asked & request::Attributes)
            out.u32(attributesOf(row));
        if (asked & request::FileListName)
            out.counted({});
        if (asked & request::RunCount)
            out.u32(0);
        if (asked & request::DateRun)
            out.u64(kUnknown);
        if (asked & request::RecentlyChanged)
            out.u64(kUnknown);
        if (asked & request::HighlightedName)
            out.counted(row.name);
        if (asked & request::HighlightedPath)
            out.counted(row.path);
        if (asked & request::HighlightedFullPath)
            out.counted(fullPathOf(row));
    }
    return out.take();
}

} // namespace

std::optional<Query> readQuery(std::uint32_t kind, std::span<const std::byte> data)
{
    Query query;
    switch (kind) {
    case copydata::QueryA:
    case copydata::QueryW:
        query.version = 1;
        query.wide = kind == copydata::QueryW;
        break;
    case copydata::Query2A:
    case copydata::Query2W:
        query.version = 2;
        query.wide = kind == copydata::Query2W;
        break;
    default:
        return std::nullopt;
    }
    const std::size_t header = query.version == 1 ? kQueryHeader : kQuery2Header;
    if (data.size() < header)
        return std::nullopt;
    query.replyWindow = u32At(data, 0);
    query.replyMessage = u32At(data, 4);
    query.searchFlags = u32At(data, 8);
    query.offset = u32At(data, 12);
    query.maxResults = u32At(data, 16);
    if (query.version == 2) {
        query.requestFlags = u32At(data, 20);
        query.sortType = u32At(data, 24);
    }
    query.search = textAt(data, header, query.wide);
    return query;
}

Search translate(const QString& text, std::uint32_t searchFlags)
{
    Search search;
    if (searchFlags & searchflag::Regex) {
        // The whole of it is the expression; Win顺 reads each word as one.
        search.query = text.trimmed();
        search.regex = !search.query.isEmpty();
        return search;
    }
    QStringList words;
    for (const QString& token : tokensOf(barsApart(withoutGroups(text, search)))) {
        const bool negated = token.startsWith(u'!') && token.size() > 1;
        QString word = negated ? token.mid(1) : token;
        bool consumed = false;
        // The functions in front of it, one after another: "file:path:abc".
        for (QString name = functionOf(word); !name.isEmpty() && !consumed; name = functionOf(word)) {
            const QString rest = word.mid(name.size() + 1);
            if (plainModifiers().contains(name)) {
                word = rest;
            } else if (name == u"file" || name == u"files") {
                search.kind = Kind::Files;
                word = rest;
            } else if (name == u"folder" || name == u"folders") {
                search.kind = Kind::Folders;
                word = rest;
            } else if (name == u"parent" || name == u"infolder" || name == u"nosubfolders") {
                if (negated)
                    search.unsupported = u'!' + name + u':';
                search.directIn = withoutEndSlash(unquoted(rest));
                consumed = true;
            } else if ((name == u"attrib" || name == u"attributes") && rest.compare(u"d", Qt::CaseInsensitive) == 0) {
                search.kind = negated ? Kind::Files : Kind::Folders; // es.exe's /ad, /a-d
                consumed = true;
            } else if ((name == u"attrib" || name == u"attributes") && rest.compare(u"!d", Qt::CaseInsensitive) == 0) {
                search.kind = Kind::Files;
                consumed = true;
            } else if (name == u"root") {
                if (negated || !rest.isEmpty())
                    search.unsupported = u"root:"_s; // "root:" alone lists the drives; a word after it is another search
                search.roots = true;
                consumed = true;
            } else if (name == u"count") {
                bool ok = false;
                if (const uint count = rest.toUInt(&ok); ok)
                    search.count = count;
                consumed = true;
            } else if (name == u"ext") {
                word = u"ext:"_s + unquoted(rest).replace(u';', u',');
                break;
            } else if (name == u"datemodified") {
                word = u"dm:"_s + rest;
                break;
            } else if (unsupportedFunctions().contains(name)) {
                search.unsupported = name + u':';
                consumed = true;
            } else {
                break; // size:, dm:, regex: as they are; anything else is a word with a colon
            }
        }
        if (consumed || word.isEmpty())
            continue;
        // A name of one character and a colon: only a drive has one ("C:").
        // Total Commander asks "?:" for the drives Everything has (GitHub #364).
        if (!negated && (word == u"?:" || word == u"*:")) {
            search.roots = true;
            continue;
        }
        if (const QString path = unquoted(word); !negated && isFolderPath(path)) {
            const QString folder = withoutEndSlash(path);
            if (!search.folders.contains(folder, Qt::CaseInsensitive))
                search.folders.append(folder);
            continue;
        }
        // Without the \ at the end (GitHub #399: "D:\images" from the
        // Everything SDK found every "images" anywhere): Everything compares
        // a word with a \ in it with each whole full path, which one from a
        // root can only start. Wildcards in its last part alone: the names in
        // that folder, at any depth ("D:\images\*.jpg").
        if (const QString path = unquoted(word); !negated && fromRoot(path)) {
            const qsizetype slash = path.lastIndexOf(u'\\');
            const QString folder = path.left(slash);
            const QString last = path.mid(slash + 1);
            if (!hasWildcards(folder) && !hasWildcards(last)) {
                if (!search.pathStarts.contains(path, Qt::CaseInsensitive))
                    search.pathStarts.append(path);
                continue;
            }
            if (!hasWildcards(folder) && !last.isEmpty()) {
                if (!search.folders.contains(folder, Qt::CaseInsensitive))
                    search.folders.append(folder);
                words.append(last.contains(u' ') ? u'"' + last + u'"' : last);
                continue;
            }
        }
        words.append(negated ? u'!' + word : word);
    }
    search.query = words.join(u' ');
    if (search.roots
        && (!search.query.isEmpty() || !search.folders.isEmpty() || !search.pathStarts.isEmpty() || !search.directIn.isEmpty()))
        search.unsupported = u"root:"_s;
    return search;
}

Rows find(const FileIndex& index, WorkerPool& pool, const Query& query, const std::function<bool()>& cancelled)
{
    Rows out;
    const Search search = translate(query.search, query.searchFlags);
    if (!search.unsupported.isEmpty())
        return out;
    if (search.roots) {
        const auto lock = index.readLock();
        for (const EntryId id : index.roots()) {
            if (index.scan(id).isDeleted())
                continue;
            Row row;
            row.name = index.wpath(id); // "C:" as Everything names a drive
            while (row.name.size() > 2 && row.name.back() == L'\\')
                row.name.pop_back();
            row.folder = row.root = true;
            out.rows.push_back(std::move(row));
        }
        std::ranges::sort(out.rows, {}, &Row::name);
        out.totalItems = out.totalFolders = static_cast<std::uint32_t>(out.rows.size());
        const std::size_t first = std::min<std::size_t>(query.offset, out.rows.size());
        out.rows.erase(out.rows.begin(), out.rows.begin() + static_cast<std::ptrdiff_t>(first));
        if (out.rows.size() > query.maxResults)
            out.rows.resize(query.maxResults);
        return out;
    }
    // Nothing asked for is everything (in the folders named).
    const ParsedQuery parsed = parseQuery(search.query.isEmpty() ? u"*"_s : search.query, search.regex);
    if (!parsed.error.isEmpty())
        return out;

    const auto lock = index.readLock();
    NameSearchOptions options;
    for (const QString& folder : search.folders) {
        const EntryId id = index.findPath(folder.toStdWString());
        if (id == kNoEntry || !index.scan(id).isDir())
            return out; // not in the index: nothing is known to be in it
        options.filter.folders.push_back(id);
    }
    std::ranges::sort(options.filter.folders);
    if (!search.pathStarts.isEmpty()) {
        // Each must hold (Everything's words all do): the longest, when the
        // others start it; else nothing can.
        const QString& longest
            = *std::ranges::max_element(search.pathStarts, {}, [](const QString& start) { return start.size(); });
        if (!std::ranges::all_of(search.pathStarts,
                [&longest](const QString& start) { return longest.startsWith(start, Qt::CaseInsensitive); }))
            return out;
        options.within = pathStartEntries(index, longest);
        if (options.within.empty())
            return out;
    }
    if (!search.directIn.isEmpty()) {
        const EntryId id = index.findPath(search.directIn.toStdWString());
        options.filter.directIn = id == kNoEntry ? kNoEntry - 1 : id;
    }
    options.foldersOnly = search.kind == Kind::Folders;
    options.order = orderOf(query.sortType);
    const std::size_t most = std::min<std::size_t>(search.count.value_or(0xFFFF'FFFFu), kMostRows);
    const std::size_t wanted = std::min<std::size_t>(most, std::size_t {query.offset} + query.maxResults);
    // Files alone are picked from the hits afterwards: all of them are needed then.
    options.limit = std::max<std::size_t>(search.kind == Kind::Files ? most : wanted, 1);
    const auto stop = [&cancelled] { return cancelled && cancelled(); };
    NameSearchOutput found = searchNames(index, NameMatcher(parsed), options, pool, stop);
    if (found.cancelled || stop())
        return out;
    std::vector<NameHit>& hits = found.hits;
    if (search.kind == Kind::Files)
        std::erase_if(hits, [&index](const NameHit& hit) { return index.scan(hit.id).isDir(); });
    std::size_t total = search.kind == Kind::Files ? hits.size() : std::max(found.totalMatches, hits.size());
    total = std::min(total, std::size_t {search.count.value_or(0xFFFF'FFFFu)});
    if (hits.size() > most)
        hits.resize(most);
    const auto folders
        = static_cast<std::size_t>(std::ranges::count_if(hits, [&index](const NameHit& hit) { return index.scan(hit.id).isDir(); }));
    out.totalItems = static_cast<std::uint32_t>(std::min<std::size_t>(total, 0xFFFF'FFFFu));
    // Of those kept: past kMostRows the two are not counted apart.
    out.totalFolders = static_cast<std::uint32_t>(folders);
    out.totalFiles = static_cast<std::uint32_t>(hits.size() - folders);

    const std::size_t first = std::min<std::size_t>(query.offset, hits.size());
    const std::size_t count = std::min<std::size_t>(query.maxResults, hits.size() - first);
    const bool sizes = query.version == 2 && (query.requestFlags & request::Size);
    const auto deadline = std::chrono::steady_clock::now() + kFolderSizeBudget;
    const auto late = [&] { return stop() || std::chrono::steady_clock::now() > deadline; };
    out.rows.reserve(count);
    for (std::size_t i = first; i < first + count; ++i) {
        const EntryId id = hits[i].id;
        const EntryScan& entry = index.scan(id);
        Row row;
        row.folder = entry.isDir();
        row.root = (entry.flags & EntryFlag::Root) != 0;
        row.hidden = (entry.flags & EntryFlag::Hidden) != 0;
        std::wstring path = index.wpath(id);
        if (const std::size_t slash = path.rfind(L'\\'); row.root || slash == std::wstring::npos) {
            row.name = std::move(path);
        } else {
            row.name = path.substr(slash + 1);
            row.path = path.substr(0, slash);
        }
        if (!row.folder) {
            if (const FileSize size = index.size(id); size != kUnknownSize)
                row.size = unpackSize(size);
        } else if (sizes && !late()) {
            row.size = indexedFolderSize(index, id, late).value_or(kUnknown);
        }
        row.created = ticksOf(index.created(id));
        row.modified = ticksOf(index.modified(id));
        out.rows.push_back(std::move(row));
    }
    if (stop())
        return {};
    return out;
}

QByteArray encode(const Query& query, const Rows& rows)
{
    return query.version == 2 ? encodeList2(query, rows) : encodeList(query, rows);
}

std::uint32_t answerCommand(std::uint32_t command, std::uint32_t argument, bool indexReady) noexcept
{
    switch (command) {
    case command::MajorVersion:
        return kVersion[0];
    case command::MinorVersion:
        return kVersion[1];
    case command::Revision:
        return kVersion[2];
    case command::BuildNumber:
        return kVersion[3];
    case command::TargetMachine:
#if defined(_M_ARM64)
        return 4;
#else
        return 2; // x64
#endif
    case command::IsDbLoaded:
    case command::IsNtfsDriveIndexed:
        return indexReady ? 1 : 0;
    case command::IsFastSort: {
        const std::uint32_t ascending = argument != 0 && argument % 2 == 0 ? argument - 1 : argument;
        return ascending == sort::Name || ascending == sort::Path || ascending == sort::Size || ascending == sort::Extension
                || ascending == sort::Created || ascending == sort::Modified
            ? 1
            : 0;
    }
    case command::IsFileInfoIndexed:
        return argument == fileinfo::FileSize || argument == fileinfo::FolderSize || argument == fileinfo::Created
                || argument == fileinfo::Modified || argument == fileinfo::Attributes
            ? 1
            : 0;
    default:
        return 0;
    }
}

} // namespace ws::everything
