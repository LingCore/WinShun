#include "McpTools.h"

#include "Aside.h"
#include "DocExtractor.h"
#include "Documents.h"
#include "FileIndex.h"
#include "FileText.h"
#include "IndexService.h"
#include "IsoTime.h"
#include "PathText.h"
#include "Pipe.h"
#include "Query.h"
#include "SearchEngine.h"
#include "ShareIndexer.h"
#include "ShareLister.h"
#include "TextUtil.h"
#include "Unc.h"
#include "Visibility.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <QDateTime>
#include <QDir>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QJsonDocument>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws::mcp {

namespace {

constexpr int kDefaultFiles = 30;
constexpr int kMaxFiles = 200;
constexpr int kDefaultContent = 20;
constexpr int kMaxContent = 100;
constexpr int kDefaultFolder = 100;
constexpr int kMaxFolder = 500;
constexpr int kDefaultClips = 20;
constexpr int kMaxClips = 100;
constexpr int kDefaultShots = 10;
constexpr int kMaxShots = 50;
constexpr int kDefaultClipChars = 500;
constexpr int kMaxClipChars = 20000;
constexpr int kDefaultTimeoutMs = 5000;
constexpr int kMinTimeoutMs = 500;
constexpr int kMaxTimeoutMs = 30000;
constexpr int kSnippetChars = 300;
constexpr int kDefaultTextChars = 20000;
constexpr int kMinTextChars = 1000;
constexpr int kMaxTextChars = 100000;
constexpr int kDefaultDocuments = 1000;
constexpr int kMaxDocuments = 5000;
constexpr int kContentThreads = 8;
constexpr int kMaxSettingChanges = 20;
constexpr int kMaxReasonChars = 200;
constexpr int kMaxClientChars = 60;
// The tools' work off the GUI thread (AsideWorkers): access checks, sizes,
// reads. A few at once, so that one run of ws asking for 50 000 sizes from
// a hard disk does not take the disk from everything else.
constexpr int kAsideThreads = 3;

// What one answer may hold, in bytes of JSON. A message holds 16 MB
// (pipe::kMaxLineBytes); an answer that would not fit is the too_large
// error instead, which gives nothing of what was found. So a list stops
// short of this (truncated: true), and a text sooner (next_offset). An AI
// client's answer goes out twice (the structured result, and the same again
// as escaped text): a third of it there (Budget).
constexpr qint64 kAnswerBudget = qint64 {15} << 20;
// Around an answer made aside: the JSON-RPC envelope (jsonrpc::responseWithResult).
constexpr qint64 kEnvelopeBytes = 4096;

// The most a call may ask for. An AI client's answers go into the model's
// context, where a few hundred items is already a lot (and the schemas say
// so); ws's go to a program or a terminal: `ws find -n 0` asks for all there
// is. Whatever is asked, an answer stays within kAnswerBudget.
struct Limits {
    int files; // search_files, recent_files
    int content; // search_content
    int folder; // list_folder
    int textChars; // read_file_text
};
constexpr Limits kModelLimits {kMaxFiles, kMaxContent, kMaxFolder, kMaxTextChars};
constexpr Limits kCliLimits {50'000, 2'000, 50'000, 4'000'000};

// ---- JSON Schema ------------------------------------------------------------

QJsonObject typed(const char* type, const QString& description)
{
    return {{u"type"_s, QString::fromLatin1(type)}, {u"description"_s, description}};
}

QJsonObject integer(const QString& description, int minimum, int maximum, int fallback)
{
    QJsonObject schema = typed("integer", description);
    schema.insert(u"minimum"_s, minimum);
    schema.insert(u"maximum"_s, maximum);
    schema.insert(u"default"_s, fallback);
    return schema;
}

QJsonObject boolean(const QString& description, bool fallback)
{
    QJsonObject schema = typed("boolean", description);
    schema.insert(u"default"_s, fallback);
    return schema;
}

QJsonObject choice(const QString& description, const QStringList& values, const QString& fallback)
{
    QJsonObject schema = typed("string", description);
    schema.insert(u"enum"_s, QJsonArray::fromStringList(values));
    schema.insert(u"default"_s, fallback);
    return schema;
}

QJsonObject array(const QJsonObject& items, const QString& description)
{
    QJsonObject schema = typed("array", description);
    schema.insert(u"items"_s, items);
    return schema;
}

QJsonObject object(const QJsonObject& properties, const QStringList& required = {})
{
    QJsonObject schema {{u"type"_s, u"object"_s}, {u"properties"_s, properties}};
    if (!required.isEmpty())
        schema.insert(u"required"_s, QJsonArray::fromStringList(required));
    schema.insert(u"additionalProperties"_s, false);
    return schema;
}

QJsonObject annotations(bool readOnly, bool idempotent)
{
    return {{u"readOnlyHint"_s, readOnly}, {u"destructiveHint"_s, false}, {u"idempotentHint"_s, idempotent},
        {u"openWorldHint"_s, false}};
}

// The `size` argument of the tools that list files.
QJsonObject sizeArgument()
{
    return boolean(u"Each file's size in bytes; false skips reading sizes from disk; faster for long lists"_s, true);
}

// The schema of one file or folder in a result.
QJsonObject itemSchema()
{
    return object({{u"path"_s, typed("string", u"Full Windows path"_s)}, {u"name"_s, typed("string", u"File or folder name"_s)},
        {u"type"_s, choice(u"file or folder"_s, {u"file"_s, u"folder"_s}, u"file"_s)},
        {u"modified"_s, typed("string", u"When last written, ISO 8601 with offset; null if unknown"_s)},
        {u"size"_s, typed("integer", u"Bytes; files only"_s)}});
}

// ---- arguments --------------------------------------------------------------

// Reads the arguments of a call, noting the first thing wrong with them.
class Args {
public:
    explicit Args(const QJsonObject& arguments)
        : m_args(arguments)
    {
    }

    QString string(const QString& key, bool required = false)
    {
        const QJsonValue v = m_args.value(key);
        if (v.isUndefined() || v.isNull()) {
            if (required)
                fail(u"Missing required argument '%1'"_s.arg(key));
            return {};
        }
        if (!v.isString()) {
            fail(u"Argument '%1' must be a string"_s.arg(key));
            return {};
        }
        return v.toString();
    }

    int integer(const QString& key, int fallback, int minimum, int maximum)
    {
        const QJsonValue v = m_args.value(key);
        if (v.isUndefined() || v.isNull())
            return fallback;
        if (!v.isDouble()) {
            fail(u"Argument '%1' must be an integer"_s.arg(key));
            return fallback;
        }
        return std::clamp(static_cast<int>(v.toDouble()), minimum, maximum);
    }

    bool boolean(const QString& key, bool fallback)
    {
        const QJsonValue v = m_args.value(key);
        if (v.isUndefined() || v.isNull())
            return fallback;
        if (!v.isBool()) {
            fail(u"Argument '%1' must be true or false"_s.arg(key));
            return fallback;
        }
        return v.toBool();
    }

    QString choice(const QString& key, const QString& fallback, const QStringList& values)
    {
        const QString value = string(key);
        if (value.isEmpty())
            return fallback;
        if (!values.contains(value, Qt::CaseInsensitive)) {
            fail(u"Argument '%1' must be one of: %2"_s.arg(key, values.join(u", "_s)));
            return fallback;
        }
        return value.toLower();
    }

    QStringList strings(const QString& key)
    {
        const QJsonValue v = m_args.value(key);
        if (v.isUndefined() || v.isNull())
            return {};
        QStringList out;
        if (v.isString()) { // one, or comma-separated
            for (const QString& s : v.toString().split(u',', Qt::SkipEmptyParts))
                out.push_back(s.trimmed());
            return out;
        }
        if (!v.isArray()) {
            fail(u"Argument '%1' must be an array of strings"_s.arg(key));
            return {};
        }
        for (const QJsonValue& item : v.toArray()) {
            if (!item.isString()) {
                fail(u"Argument '%1' must be an array of strings"_s.arg(key));
                return {};
            }
            out.push_back(item.toString().trimmed());
        }
        return out;
    }

    const QString& error() const noexcept { return m_error; }

private:
    void fail(const QString& why)
    {
        if (m_error.isEmpty())
            m_error = why;
    }

    const QJsonObject& m_args;
    QString m_error;
};

// ---- values -----------------------------------------------------------------

QJsonValue isoTimeMs(qint64 ms)
{
    if (ms <= 0)
        return QJsonValue::Null;
    const QDateTime time = QDateTime::fromMSecsSinceEpoch(ms);
    return time.toOffsetFromUtc(time.offsetFromUtc()).toString(Qt::ISODate);
}

QJsonValue isoFileTime(const FILETIME& time)
{
    return isoTime(static_cast<qint64>(fileTimeOf(win32::ticks(time))));
}

// A file's size from the disk (folders and failures: nothing).
std::optional<qint64> sizeOf(const QString& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data {};
    if (!::GetFileAttributesExW(win32::longPath(path.toStdWString()).c_str(), GetFileExInfoStandard, &data)
        || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return std::nullopt;
    return static_cast<qint64>((static_cast<quint64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow);
}

// What an item of a list holds besides its path, type and time.
struct ItemShape {
    bool name = true; // ws's leave it out: the path ends with it
    bool size = true; // asked not to (size: false), no read of the disk for each
};

// Made aside: the size is read from the disk, at the disk's pace (QuietIo).
// `times`: the list's, one for all its items. `known`: the size the index
// has (-1: none), the one given off this PC's drives: a server is not asked
// for each file (one gone away would hold up every one for seconds; GitHub
// #114).
QJsonObject fileItem(AsideContext& aside, LocalTimes& times, const QString& path, const QString& name, bool isDir,
    std::uint32_t modified, std::int64_t known, ItemShape shape)
{
    QJsonObject item {
        {u"path"_s, path}, {u"type"_s, isDir ? u"folder"_s : u"file"_s}, {u"modified"_s, times.iso(modified)}};
    if (shape.name)
        item.insert(u"name"_s, name);
    if (shape.size && !isDir) {
        if (Visibility::placeOf(path, {}) != Visibility::Place::Local) {
            if (known >= 0)
                item.insert(u"size"_s, static_cast<qint64>(known));
        } else {
            aside.quiet.at(path);
            if (const auto size = sizeOf(path))
                item.insert(u"size"_s, *size);
        }
    }
    return item;
}

// The size the index has of an entry; -1 for a folder, or none known.
std::int64_t indexedSize(const FileIndex& index, EntryId id)
{
    const FileSize size = index.entry(id).isDir() ? kUnknownSize : index.size(id);
    return size == kUnknownSize ? -1 : static_cast<std::int64_t>(unpackSize(size));
}

// The rows of a list that the caller can see (AsideContext::sees), in their
// order, each to `take` until it returns false; `wanted`: a row to look at
// at all (one of the kind asked for, not excluded, not on a share out of
// reach). Their folders are checked ahead a batch of rows at a time
// (AsideContext::seeAhead): ws asks for tens of thousands, in thousands of
// folders, and one check after another was most of such an answer's time,
// while an answer its budget cuts short has checked at most a batch more
// than it gives. False when stopped.
template <typename Rows, typename Wanted, typename Take>
bool forEachVisible(AsideContext& aside, const Rows& rows, Wanted&& wanted, Take&& take)
{
    constexpr std::ptrdiff_t kBatch = 4096;
    const std::ptrdiff_t count = std::ranges::ssize(rows);
    std::vector<char> looked;
    QStringList paths;
    for (std::ptrdiff_t from = 0; from < count; from += kBatch) {
        const std::ptrdiff_t to = std::min(count, from + kBatch);
        looked.assign(static_cast<std::size_t>(to - from), 0);
        paths.clear();
        for (std::ptrdiff_t i = from; i < to; ++i) {
            const auto& row = std::ranges::begin(rows)[i];
            if (wanted(row)) {
                looked[static_cast<std::size_t>(i - from)] = 1;
                paths.push_back(row.path);
            }
        }
        aside.seeAhead(paths);
        for (std::ptrdiff_t i = from; i < to; ++i) {
            if (aside.stopped())
                return false;
            const auto& row = std::ranges::begin(rows)[i];
            if (looked[static_cast<std::size_t>(i - from)] && aside.sees(row.path) && !take(row))
                return true;
        }
    }
    return !aside.stopped();
}

ToolResult failure(const QString& why)
{
    return {{}, true, why};
}

// No such path, or one the caller may not see: the same to it (kErrorNotFound).
ToolResult notFound(const QString& why)
{
    return {{}, true, why, QString::fromLatin1(kErrorNotFound)};
}

// A full path: "D:\..." or "\\server\share...". Not "D:x" (relative to the
// drive's current folder) or "\x" (to the current drive): those depend on
// where the caller stands, which the host does not know; ws makes its
// relative paths full itself. "D:" alone is the drive.
bool isFullPath(const QString& path)
{
    const bool drive = path.size() >= 2 && path[0].isLetter() && path[0].unicode() < 128 && path[1] == u':';
    if (drive)
        return path.size() == 2 || path[2] == u'\\' || path[2] == u'/';
    return path.size() > 2 && (path[0] == u'\\' || path[0] == u'/') && (path[1] == u'\\' || path[1] == u'/');
}

// Whether a path lies in one of the folders AI never sees (ToolHost::excluded's
// rule, for a path as the index gives it: native and clean already, so it
// is only folded). Any thread.
bool excludedIndexPath(const std::vector<QString>& excluded, const QString& path)
{
    if (excluded.empty())
        return false;
    QString folded = path.toCaseFolded();
    if (!folded.endsWith(u'\\'))
        folded.append(u'\\');
    return std::ranges::any_of(excluded, [&](const QString& ex) { return folded.startsWith(ex); });
}

QString nativePath(const QString& path)
{
    return QDir::toNativeSeparators(QDir::cleanPath(QString::fromStdWString(win32::expandEnvironment(path.trimmed().toStdWString()))));
}

// A path as the index knows it: native, without a trailing separator
// (a root keeps "C:\" as "C:").
std::wstring indexPath(const QString& path)
{
    QString p = nativePath(path);
    while (p.size() > 2 && p.endsWith(u'\\'))
        p.chop(1);
    if (p.size() == 3 && p[1] == u':' && p[2] == u'\\')
        p.chop(1);
    return p.toStdWString();
}

QString contentWhere(SearchResult::Where where)
{
    switch (where) {
    case SearchResult::Where::Page:
        return u"page"_s;
    case SearchResult::Where::Slide:
        return u"slide"_s;
    case SearchResult::Where::Row:
        return u"row"_s;
    case SearchResult::Where::Document:
        return u"document"_s;
    case SearchResult::Where::Line:
        break;
    }
    return u"line"_s;
}

QJsonObject contentItem(const SearchResult& r, LocalTimes& times, ItemShape shape)
{
    QJsonObject item {{u"path"_s, r.path}, {u"modified"_s, times.iso(r.modified)}};
    if (shape.name)
        item.insert(u"name"_s, r.name);
    QJsonObject location {{u"kind"_s, contentWhere(r.where)}};
    if (r.where == SearchResult::Where::Line)
        location.insert(u"line"_s, r.line);
    else if (r.where != SearchResult::Where::Document)
        location.insert(u"number"_s, r.placeNumber);
    if (!r.sheet.isEmpty())
        location.insert(u"sheet"_s, r.sheet);
    item.insert(u"location"_s, location);
    // The snippet is the line (or the passage) with the terms; a long one
    // (minified code, a data file) is cut around the first term.
    QString snippet = r.snippet;
    if (snippet.size() > kSnippetChars) {
        qsizetype from = 0;
        if (!r.snippetMatches.isEmpty())
            from = std::max<qsizetype>(0, r.snippetMatches.first().first - kSnippetChars / 3);
        snippet = (from > 0 ? u"…"_s : QString()) + snippet.mid(from, kSnippetChars) + u"…"_s;
    }
    item.insert(u"snippet"_s, snippet.simplified());
    item.insert(u"score"_s, r.relevance);
    return item;
}

QString stateName(IndexService::State state)
{
    switch (state) {
    case IndexService::State::Idle:
        return u"idle"_s;
    case IndexService::State::Loading:
        return u"loading"_s;
    case IndexService::State::Building:
        return u"building"_s;
    case IndexService::State::Ready:
        break;
    }
    return u"ready"_s;
}

// ---- the size of an answer ------------------------------------------------------

// The bytes of JSON the character at `i` takes, at least as many as
// QJsonDocument writes (compact UTF-8, a control character as an escape of
// up to six), and how many UTF-16 units it is.
qint64 charJsonBytes(QStringView text, qsizetype i, qsizetype& units)
{
    units = 1;
    const char16_t c = text[i].unicode();
    if (c < 0x20 || c == 0x7F)
        return 6;
    if (c == u'"' || c == u'\\')
        return 2;
    if (c < 0x80)
        return 1;
    if (c < 0x800)
        return 2;
    if (QChar::isHighSurrogate(c) && i + 1 < text.size() && QChar::isLowSurrogate(text[i + 1].unicode())) {
        units = 2;
        return 4;
    }
    return QChar::isSurrogate(c) ? 6 : 3; // half a character alone: an escape, or a replacement
}

qint64 stringJsonBytes(QStringView text)
{
    qint64 bytes = 2;
    qsizetype units = 1;
    for (qsizetype i = 0; i < text.size(); i += units)
        bytes += charJsonBytes(text, i, units);
    return bytes;
}

// At least the bytes of `value` as compact JSON.
qint64 jsonBytes(const QJsonValue& value)
{
    switch (value.type()) {
    case QJsonValue::Null:
        return 4;
    case QJsonValue::Bool:
        return 5;
    case QJsonValue::Double:
        return 24; // "-1.2345678901234567e-308"
    case QJsonValue::String:
        return stringJsonBytes(value.toString());
    case QJsonValue::Array: {
        qint64 bytes = 2;
        for (const QJsonValue& v : value.toArray())
            bytes += jsonBytes(v) + 1;
        return bytes;
    }
    case QJsonValue::Object: {
        const QJsonObject object = value.toObject();
        qint64 bytes = 2;
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
            bytes += stringJsonBytes(it.key()) + 1 + jsonBytes(it.value()) + 1;
        return bytes;
    }
    case QJsonValue::Undefined:
        break;
    }
    return 0;
}

// Where a text from `from` ends, at `to` at most, within `bytes` of JSON:
// not between the halves of a character.
qsizetype fitText(QStringView text, qsizetype from, qsizetype to, qint64 bytes)
{
    qint64 used = 2;
    qsizetype at = from;
    qsizetype units = 1;
    while (at < to) {
        const qint64 cost = charJsonBytes(text, at, units);
        if (at + units > to || used + cost > bytes)
            break;
        used += cost;
        at += units;
    }
    return at;
}

// What is left of an answer's bytes (kAnswerBudget) as its items go in. Once
// one does not fit, the list ends there: truncated.
class Budget {
public:
    explicit Budget(bool cli)
        : m_left(cli ? kAnswerBudget : kAnswerBudget / 3)
    {
    }

    bool take(const QJsonValue& item) { return takeBytes(jsonBytes(item) + 1); }
    bool takeBytes(qint64 bytes)
    {
        if (m_cut || bytes > m_left) {
            m_cut = true;
            return false;
        }
        m_left -= bytes;
        return true;
    }
    bool cut() const noexcept { return m_cut; }
    qint64 left() const noexcept { return m_left; }

private:
    qint64 m_left;
    bool m_cut = false;
};

// The answer as it goes out, made where the work was done: the JSON of
// 50 000 items is megabytes, too much to make on the GUI thread. One larger
// than a message holds is the too_large error instead. `structured` keeps
// only what the call's log line reads (logCall).
void encodeAnswer(ToolResult& result, bool cli)
{
    result.encoded = QJsonDocument(callToolResult(result, cli)).toJson(QJsonDocument::Compact);
    if (result.encoded.size() + kEnvelopeBytes > static_cast<qint64>(pipe::kMaxLineBytes)) {
        result = tooLargeResult(result.encoded.size());
        result.encoded = QJsonDocument(callToolResult(result, cli)).toJson(QJsonDocument::Compact);
        return;
    }
    QJsonObject kept;
    for (const QString& key : {u"returned"_s, u"opened"_s}) {
        if (const QJsonValue v = result.structured.value(key); !v.isUndefined())
            kept.insert(key, v);
    }
    result.structured = kept;
}

// ---- what a caller may know of a path it names ---------------------------------------

QString notInIndex(const QString& folder)
{
    return u"The folder is not in the index: it may not exist, be excluded from indexing, or be on a drive that is not indexed. Check it with get_file_info: "_s
        + folder;
}

// A folder a call names (search_files, search_content, recent_files,
// list_folder), as the index has it.
struct FolderArg {
    QString given; // as asked, for the answers
    QString native; // the same, clean (nativePath)
    QString path; // as the index has it; empty: not in the index
    bool isDir = false;
};

// Looks `folders` up in the index: a moment under its lock, no disk.
std::vector<FolderArg> lookUp(const FileIndex& index, const QStringList& folders)
{
    std::vector<FolderArg> out;
    const auto lock = index.readLock();
    for (const QString& given : folders) {
        FolderArg folder {given, nativePath(given)};
        if (const EntryId id = index.findPath(indexPath(given)); id != kNoEntry) {
            const Entry& e = index.entry(id);
            folder.path = QString::fromStdWString(index.wpath(id));
            folder.isDir = e.isDir();
        }
        out.push_back(std::move(folder));
    }
    return out;
}

// Whether any of `folders` needs no search for its answer (it is not one to
// search in), or else checkFolders after the search says what of them the
// caller may know.
bool answeredWithoutSearch(const std::vector<FolderArg>& folders, const std::function<bool(const QString&)>& offline)
{
    return std::ranges::any_of(folders, [&](const FolderArg& f) { return f.path.isEmpty() || !f.isDir || offline(f.native); });
}

// What the caller may know of the folders a call names, in an order that
// tells it no more than that (aside: the checks go to the disk). One on a
// share that cannot be reached now is not there: no check waits for the
// network. Nor is one the index does not have, or one the caller cannot
// see; one it can see may then be called a file. A folder must be one it can
// list: its names are what the call is about. One on a share or in WSL is
// searched for its contents too: what the content index has of it (GitHub
// #114).
std::optional<ToolResult> checkFolders(
    AsideContext& aside, const std::vector<FolderArg>& folders, const std::function<bool(const QString&)>& offline)
{
    for (const FolderArg& f : folders) {
        if (offline(f.native))
            return notFound(u"The folder is on a network share that cannot be reached now: "_s + f.given);
        if (f.path.isEmpty())
            return notFound(notInIndex(f.given));
        if (!f.isDir) {
            if (!aside.sees(f.path))
                return notFound(notInIndex(f.given));
            return failure(u"The path is a file, not a folder: "_s + f.given);
        }
        if (!aside.canList(f.path))
            return notFound(notInIndex(f.given));
    }
    return std::nullopt;
}

// "\\?\C:\x", "\\.\C:", "\??\C:\x": paths Windows takes as they are written,
// past the cleaning (and the checks) of the others.
bool isDevicePath(QString path)
{
    path.replace(u'/', u'\\');
    return path.startsWith(u"\\\\?\\") || path.startsWith(u"\\\\.\\") || path.startsWith(u"\\??\\") || path == u"\\\\?"
        || path == u"\\\\.";
}

// Whether a network path's host is this computer: its files then, by
// another path.
bool isThisComputer(QString host)
{
    host = host.toCaseFolded();
    if (host.endsWith(u'.'))
        host.chop(1);
    const bool ipv4 = !host.isEmpty() && std::ranges::all_of(host, [](QChar c) { return c.isDigit() || c == u'.'; });
    if (host.isEmpty() || host == u"localhost" || (ipv4 && host.startsWith(u"127.")) || host == u"::1" || host == u"[::1]"
        || host == u"0--1.ipv6-literal.net")
        return true;
    for (const COMPUTER_NAME_FORMAT format : {ComputerNameNetBIOS, ComputerNameDnsHostname, ComputerNameDnsFullyQualified}) {
        wchar_t name[256] {};
        DWORD size = static_cast<DWORD>(std::size(name));
        if (::GetComputerNameExW(format, name, &size) && size > 0 && host == QString::fromWCharArray(name, size).toCaseFolded())
            return true;
    }
    return false;
}

// For a path that is on none of this computer's drives nor under one of the
// index's roots on a server or in WSL (Visibility::placeOf): refused before
// anything touches it. Opening a path on a server signs in to it: a server
// named in what the AI read (a web page, a file) would get the user's NTLM
// answer.
QString notOurs(const QString& path)
{
    return u"Only paths on this PC's drives and in the network locations Win顺 indexes can be used: "_s + path;
}

// Why a path a call names is not looked at, from how it is written alone (no
// disk). `excluded`: the folders AI never sees; `remoteRoots`: the index's
// roots on servers and in WSL (notOurs).
std::optional<ToolResult> pathProblem(
    const std::vector<QString>& excluded, const std::vector<std::wstring>& remoteRoots, const QString& given, const QString& native)
{
    if (isDevicePath(given) || isDevicePath(native))
        return failure(u"Device paths (\\\\?\\…, \\\\.\\…) are not taken: give the path as File Explorer shows it, like D:\\Projects\\file.txt."_s);
    if (excludedIndexPath(excluded, native))
        return notFound(u"This path is not visible to AI (excluded in Win顺's settings)."_s);
    const std::wstring wide = native.toStdWString();
    if (!excluded.empty() && unc::isUnc(wide) && isThisComputer(QString::fromStdWString(std::wstring(unc::host(wide))))) {
        return notFound(u"This network path leads back to this computer. While folders are hidden from AI (Win顺's settings), Win顺 takes this computer's files only by their own paths (C:\\…)."_s);
    }
    if (Visibility::placeOf(native, remoteRoots) == Visibility::Place::Elsewhere)
        return failure(notOurs(native));
    return std::nullopt;
}

// Where an open file is, by its own name ("C:\dir\file", "\\server\share\
// file"): what the path given led to, through links, junctions and short
// names. Empty when Windows cannot say; `exact`: nor in the long names
// (FILE_NAME_NORMALIZED), which are what the folders AI never sees are
// compared with.
QString finalPathOf(HANDLE file, bool exact)
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (const DWORD flags : {DWORD {FILE_NAME_NORMALIZED}, DWORD {FILE_NAME_OPENED}}) {
        if (flags == FILE_NAME_OPENED && exact)
            break;
        DWORD length = ::GetFinalPathNameByHandleW(file, buffer.data(), static_cast<DWORD>(buffer.size()), flags | VOLUME_NAME_DOS);
        if (length >= buffer.size()) {
            buffer.resize(length + 1);
            length = ::GetFinalPathNameByHandleW(file, buffer.data(), static_cast<DWORD>(buffer.size()), flags | VOLUME_NAME_DOS);
        }
        if (length == 0 || length >= buffer.size())
            continue;
        std::wstring_view path(buffer.data(), length);
        if (path.starts_with(L"\\\\?\\UNC\\"))
            return u"\\\\"_s + QString::fromWCharArray(path.data() + 8, static_cast<qsizetype>(path.size() - 8));
        if (path.starts_with(L"\\\\?\\"))
            path.remove_prefix(4);
        if (path.size() >= 2 && path[1] == L':')
            return QString::fromWCharArray(path.data(), static_cast<qsizetype>(path.size()));
    }
    return {};
}

// Whether a file opened through a network path is on a volume of this
// computer that holds a folder AI never sees: a share of this computer
// reached by an address of its own (its LAN address, a name in the hosts
// file) is its own files by another path. The server gives its volume's
// serial number; the same as such a volume's is taken to be that volume.
bool onExcludedVolume(const BY_HANDLE_FILE_INFORMATION& info, const std::vector<QString>& excluded)
{
    for (const QString& folder : excluded) {
        if (folder.size() < 3 || folder[1] != u':' || folder[2] != u'\\')
            continue;
        DWORD serial = 0;
        if (::GetVolumeInformationW(folder.left(3).toStdWString().c_str(), nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)
            && serial == info.dwVolumeSerialNumber)
            return true;
    }
    return false;
}

// What opening a path a call names came to.
enum class Reach {
    Ok,
    Missing, // not there, or not for the caller: the same to it
    Unreachable, // on a network share that cannot be reached now
    Error, // there, the caller may see it, and it cannot be opened (Opened::error)
};

struct Opened {
    Reach reach = Reach::Missing;
    DWORD error = 0;
    win32::UniqueHandle handle; // no read access: attributes, and ReOpenFile to read it
    QString path; // the final path (finalPathOf)
    // What the caller's rights are checked on: the final path, but the path
    // given on a mapped drive. That leads to its server ("\\nas\docs\..."),
    // which Visibility asks nothing of without a root of the index there; the
    // drive is checked as any drive is (Visibility::placeOf).
    QString seen;
    BY_HANDLE_FILE_INFORMATION info {};

    bool isDir() const noexcept { return (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0; }
};

// Opens a path a call names (get_file_info, open_item, read_file_text), no
// data read: what it leads to, whatever it is written as, is what is checked
// against the folders AI never sees and then what the caller may see (by its
// final path), and what a read reads (through this handle). A network path
// is asked of its server first, briefly: one that is off would hold the
// thread for the half minute Windows waits.
Opened openGiven(AsideContext& aside, const QString& native, const std::vector<QString>& excluded,
    const std::function<bool(const QString&)>& offline)
{
    Opened opened;
    const std::wstring wide = native.toStdWString();
    const std::wstring network = unc::isUnc(wide) ? wide : unc::fromMappedDrive(wide);
    if (!network.empty() && unc::isUnc(network)
        && (offline(QString::fromStdWString(network)) || !share::reachablePath(network))) {
        opened.reach = Reach::Unreachable;
        return opened;
    }
    aside.quiet.at(native);
    opened.handle.reset(::CreateFileW(win32::longPath(wide).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_NO_RECALL, nullptr));
    if (!opened.handle.valid() || !::GetFileInformationByHandle(opened.handle.get(), &opened.info)) {
        const DWORD error = ::GetLastError();
        opened.handle.reset();
        // Not there, or kept from Win顺 (and so from its callers): the same
        // to them. Any other failure is told only of a path the caller sees,
        // and never while folders are hidden from AI: the path may be one of
        // theirs by another name, unknown without the file.
        const bool missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_INVALID_NAME
            || error == ERROR_BAD_NETPATH || error == ERROR_BAD_NET_NAME || error == ERROR_BAD_PATHNAME
            || error == ERROR_DIRECTORY || error == ERROR_ACCESS_DENIED;
        if (!missing && excluded.empty() && aside.sees(native)) {
            opened.reach = Reach::Error;
            opened.error = error;
        }
        return opened;
    }
    // Where it led: that is what may or may not be seen. While folders are
    // hidden from AI, a path whose end cannot be told is not followed.
    opened.path = finalPathOf(opened.handle.get(), !excluded.empty());
    if (opened.path.isEmpty()) {
        if (!excluded.empty()) {
            opened.handle.reset();
            return opened;
        }
        opened.path = native;
    }
    if (!excluded.empty()) {
        const std::wstring final = opened.path.toStdWString();
        const bool hidden = excludedIndexPath(excluded, opened.path)
            || (unc::isUnc(final)
                && (isThisComputer(QString::fromStdWString(std::wstring(unc::host(final)))) || onExcludedVolume(opened.info, excluded)));
        if (hidden) {
            opened.handle.reset();
            return opened;
        }
    }
    opened.seen = !unc::isUnc(wide) && !network.empty() ? native : opened.path;
    opened.reach = Reach::Ok;
    return opened;
}

// search_content's timeout_ms, as the call has it (the handler says what is
// wrong with it).
int contentTimeout(const QJsonObject& arguments)
{
    Args args(arguments);
    return args.integer(u"timeout_ms"_s, kDefaultTimeoutMs, kMinTimeoutMs, kMaxTimeoutMs);
}

// search_content's answer when its timeout ran out before the engine was
// free for it: nothing was searched. `queued` says so to a program (ws), for
// which complete: false alone reads as a search that stopped at its limit.
ToolResult waitedTooLong(const QElapsedTimer& timer)
{
    return {{{u"items"_s, QJsonArray()}, {u"returned"_s, 0}, {u"complete"_s, false}, {u"queued"_s, true}, {u"files_checked"_s, 0},
        {u"files_candidates"_s, 0}, {u"elapsed_ms"_s, static_cast<double>(timer.elapsed())},
        {u"note"_s, u"Another content search was running the whole time, so this one did not start before its timeout_ms ran out. Call it again once the other one has answered, or with a larger timeout_ms."_s}}};
}

} // namespace

// ---- results as they go out ------------------------------------------------------

QJsonObject callToolResult(const ToolResult& result, bool cli)
{
    if (result.isError) {
        QJsonObject out {{u"content"_s, QJsonArray {QJsonObject {{u"type"_s, u"text"_s}, {u"text"_s, result.errorText}}}}, {u"isError"_s, true}};
        // ws acts on the kind (its exit codes); an AI client reads the words,
        // and might hold a structured error against the tool's outputSchema.
        if (cli && !result.errorCode.isEmpty()) {
            QJsonObject kind {{u"error"_s, result.errorCode}};
            if (!result.errorSetting.isEmpty())
                kind.insert(u"setting"_s, result.errorSetting);
            out.insert(u"structuredContent"_s, kind);
        }
        return out;
    }
    // ws reads the structured result alone: the same again as text would
    // double the answer (and its 50 000 items).
    if (cli)
        return {{u"content"_s, QJsonArray()}, {u"structuredContent"_s, result.structured}, {u"isError"_s, false}};
    const QString text = QString::fromUtf8(QJsonDocument(result.structured).toJson(QJsonDocument::Compact));
    return {{u"content"_s, QJsonArray {QJsonObject {{u"type"_s, u"text"_s}, {u"text"_s, text}}}},
        {u"structuredContent"_s, result.structured}, {u"isError"_s, false}};
}

ToolResult tooLargeResult(qint64 bytes)
{
    const QString size = QString::number(static_cast<double>(bytes) / (1 << 20), 'f', 1);
    ToolResult result {{}, true,
        u"The answer is too large to send: %1 MB, where one message holds at most %2 MB. Ask for less: a smaller limit (or max_chars), fewer folders, a more specific query, or size: false."_s
            .arg(size)
            .arg(pipe::kMaxLineBytes >> 20),
        QString::fromLatin1(kErrorTooLarge)};
    result.tooLargeBytes = bytes;
    return result;
}

// ---- the host ---------------------------------------------------------------

struct ToolHost::Tool {
    enum class Gate { None, Open, Clipboard, Documents, Screenshot, Settings };
    // Which search engine it runs on, one call at a time (its Lane); None: it
    // needs none and starts at once.
    enum class Engine { None, Names, Content };
    QString name;
    QString title;
    QString description;
    QJsonObject inputSchema;
    QJsonObject outputSchema;
    QJsonObject annotations;
    Gate gate = Gate::None;
    Engine engine = Engine::None;
    bool offered = true; // the services it needs are there; external: what its owner says
    // ws's alone (kPluginTools): not in the list, and no tool at all to a
    // caller that is not ws. What it shows, not what it guards: any program
    // may call itself ws, so such a tool does nothing the user's own
    // programs could not, or asks the user on the screen.
    bool cliOnly = false;
    std::function<void(const std::shared_ptr<Call>&)> run;
    // External tools (ExternalTool): their own handlers and words.
    bool external = false;
    QString offReason;
    std::function<void(std::uint64_t, const QJsonObject&, std::function<void(ToolResult)>)> runExternal;
    std::function<void(std::uint64_t)> cancelExternal;

    bool allowed(const Policy& policy) const
    {
        if (external)
            return offered;
        return offered
            && (gate == Gate::None || (gate == Gate::Open && policy.allowOpen)
                || (gate == Gate::Clipboard && policy.allowClipboard && policy.clipboardHistory)
                || (gate == Gate::Documents && policy.allowDocuments) || (gate == Gate::Screenshot && policy.allowScreenshot)
                || (gate == Gate::Settings && policy.settingsMode != SettingsMode::Off));
    }

    QJsonObject json() const
    {
        QJsonObject tool {{u"name"_s, name}, {u"title"_s, title}, {u"description"_s, description},
            {u"inputSchema"_s, inputSchema}, {u"annotations"_s, annotations}};
        if (!outputSchema.isEmpty())
            tool.insert(u"outputSchema"_s, outputSchema);
        return tool;
    }
};

struct ToolHost::Call {
    std::uint64_t id = 0;
    QString client;
    const Tool* tool = nullptr;
    std::shared_ptr<Tool> keep; // an external tool, held while the list may be replaced
    QJsonObject arguments;
    Done done;
    QElapsedTimer timer;
    bool cancelled = false;
    bool settingsCard = false; // change_settings: the card asking the user is up for this call
    HANDLE token = nullptr; // the caller's, for Visibility; null: the default
    bool cli = false; // CallOptions::cli
    bool encode = false; // CallOptions::encode
    std::stop_source stop {std::nostopstate}; // its work aside (aside()), stopped when the call is cancelled
    int checks = 0; // access checks made, and their time, for the log
    qint64 checkUs = 0;

    const Limits& limits() const noexcept { return cli ? kCliLimits : kModelLimits; }
    ItemShape shape(bool size = true) const noexcept { return {!cli, size}; }
};

// The search running on a lane's engine for a call, and what it gathered.
struct ToolHost::Active {
    std::shared_ptr<Call> call;
    // The engine's id for it while it searches; 0 before it was submitted:
    // a handler that answers without a search (the arguments are wrong, or
    // a folder named is not one to search in) submits none.
    quint64 requestId = 0;
    bool content = false;
    int limit = 0;
    int requestLimit = 0; // search_files: what the engine was asked for (Request::limit)
    QString kind; // search_files: all | files | folders
    bool sizes = true; // search_files: read each file's size
    std::vector<FolderArg> folders; // the folders asked for, checked once the engine answered (checkFolders)
    SearchResults found; // content matches so far
    int scanned = 0;
    int total = 0;
};

// A search engine and the calls waiting for it. An engine runs only its
// newest request, so a lane runs one search at a time; the next starts as
// soon as the engine answered the one before, which is finished aside
// meanwhile (finishSearch): calls in a row may answer in another order.
struct ToolHost::Lane {
    SearchEngine* engine = nullptr;
    std::deque<std::shared_ptr<Call>> queue;
    std::unique_ptr<Active> active;
    QTimer timeout; // search_content's timeout_ms
};

ToolHost::ToolHost(Services services, QObject* parent)
    : QObject(parent)
    , m_services(std::move(services))
{
    if (!m_services.offline) {
        IndexService* service = m_services.index;
        m_services.offline = [service](const QString& path) {
            return service && path.startsWith(u"\\\\") && service->shares().offline(path.toStdWString());
        };
    }
    addTools();
    m_names = makeLane(m_services.engine);
    // Two lanes on one engine would cancel each other's searches: content
    // searches get a lane of their own only with an engine of their own.
    if (m_services.contentEngine && m_services.contentEngine != m_services.engine)
        m_contents = makeLane(m_services.contentEngine);
    m_workers = std::make_unique<AsideWorkers>(kAsideThreads);
}

// m_workers goes first (the last member): every job told to stop, then
// waited for, while all they use is still there.
ToolHost::~ToolHost() = default;

// Each engine's answers go to its own lane: the request ids are the
// engine's own (two engines both count from 1), and a content search's
// opening resultsReady must not finish a name search.
std::unique_ptr<ToolHost::Lane> ToolHost::makeLane(SearchEngine* engine)
{
    auto lane = std::make_unique<Lane>();
    Lane* l = lane.get();
    l->engine = engine;
    if (engine) {
        connect(engine, &SearchEngine::resultsReady, this,
            [this, l](quint64 id, const SearchResults& results, qint64 total, qint64 us) { onResults(*l, id, results, total, us); });
        connect(engine, &SearchEngine::contentResults, this,
            [this, l](quint64 id, const SearchResults& results) { onContentResults(*l, id, results); });
        connect(engine, &SearchEngine::contentProgress, this,
            [this, l](quint64 id, int scanned, int total, bool finished) { onContentProgress(*l, id, scanned, total, finished); });
    }
    l->timeout.setSingleShot(true);
    connect(&l->timeout, &QTimer::timeout, this, [this, l] { finishContent(*l, false); });
    return lane;
}

ToolHost::Lane& ToolHost::laneOf(const Tool& tool)
{
    return tool.engine == Tool::Engine::Content && m_contents ? *m_contents : *m_names;
}

void ToolHost::setPolicy(Policy policy)
{
    const QStringList before = toolNames();
    m_policy = std::move(policy);
    m_excluded.clear();
    for (const QString& path : m_policy.excludedPaths) {
        QString folder = nativePath(path);
        if (folder.isEmpty() || folder == u".")
            continue;
        if (!folder.endsWith(u'\\'))
            folder.append(u'\\');
        m_excluded.push_back(folder.toCaseFolded());
    }
    if (toolNames() != before)
        emit toolsChanged();
}

bool ToolHost::excluded(const QString& path) const
{
    if (m_excluded.empty())
        return false;
    QString folded = nativePath(path).toCaseFolded();
    if (!folded.endsWith(u'\\'))
        folded.append(u'\\');
    return std::any_of(m_excluded.cbegin(), m_excluded.cend(), [&](const QString& ex) { return folded.startsWith(ex); });
}

std::vector<std::wstring> ToolHost::remoteRoots() const
{
    std::vector<std::wstring> roots;
    if (!m_services.index)
        return roots;
    for (const ShareIndexer::Status& s : m_services.index->shares().statuses()) {
        std::wstring root = unc::canonical(s.path);
        roots.push_back(root.empty() ? s.path : std::move(root));
    }
    return roots;
}

QJsonArray ToolHost::tools() const
{
    QJsonArray out;
    for (const Tool& tool : m_tools) {
        if (tool.allowed(m_policy) && !tool.cliOnly)
            out.push_back(tool.json());
    }
    for (const auto& tool : m_external) {
        if (tool->allowed(m_policy))
            out.push_back(tool->json());
    }
    return out;
}

QStringList ToolHost::toolNames() const
{
    QStringList out;
    for (const Tool& tool : m_tools) {
        if (tool.allowed(m_policy) && !tool.cliOnly)
            out.push_back(tool.name);
    }
    for (const auto& tool : m_external) {
        if (tool->allowed(m_policy))
            out.push_back(tool->name);
    }
    return out;
}

const ToolHost::Tool* ToolHost::find(const QString& name) const
{
    for (const Tool& tool : m_tools) {
        if (tool.name == name)
            return &tool;
    }
    for (const auto& tool : m_external) {
        if (tool->name == name)
            return tool.get();
    }
    return nullptr;
}

ToolHost::Known ToolHost::known(const QString& name, bool cli) const
{
    const Tool* tool = find(name);
    if (!tool || (tool->cliOnly && (!cli || !tool->offered)))
        return Known::No;
    return tool->allowed(m_policy) ? Known::Yes : Known::Off;
}

QString ToolHost::offReason(const QString& name) const
{
    const Tool* tool = find(name);
    if (tool && tool->gate == Tool::Gate::Settings) {
        return u"The tool '%1' is off: the user has not allowed AI to change Win顺's settings. Ask them to turn it on in "
               "设置 → AI 助手 → 权限 → “允许 AI 修改设置” (every time ask me, or change directly), then call it again. "
               "If they would rather do it themselves, open_settings shows them where a setting is."_s.arg(name);
    }
    // Allowed to AI, but there is no history to read: the switch to name is
    // the history's own, not the AI one (the default words).
    if (tool && tool->gate == Tool::Gate::Clipboard && m_policy.allowClipboard && !m_policy.clipboardHistory) {
        return u"The tool '%1' is off: Win顺 is not keeping a clipboard history, so there is nothing to read. Ask the user to turn on "
               "设置 → 剪贴板 → “记录剪贴板历史”, then call it again (what was copied while it was off is not there)."_s.arg(name);
    }
    return tool && tool->external ? tool->offReason : QString();
}

QString ToolHost::offSetting(const QString& name) const
{
    const Tool* tool = find(name);
    // As Tool::allowed asks: AI's switch first, then the history's.
    if (tool && tool->gate == Tool::Gate::Clipboard)
        return !m_policy.allowClipboard ? QString::fromLatin1(kSettingAiClipboard)
             : !m_policy.clipboardHistory ? QString::fromLatin1(kSettingClipboardHistory)
                                          : QString();
    return {};
}

void ToolHost::setExternalTools(std::vector<ExternalTool> tools)
{
    const QStringList before = toolNames();
    m_external.clear();
    for (ExternalTool& t : tools) {
        auto tool = std::make_shared<Tool>();
        tool->external = true;
        tool->name = t.name;
        tool->title = t.title;
        tool->description = t.description;
        tool->inputSchema = t.inputSchema;
        tool->outputSchema = t.outputSchema;
        tool->annotations = {{u"readOnlyHint"_s, t.readOnly}, {u"destructiveHint"_s, !t.readOnly}, {u"idempotentHint"_s, false},
            {u"openWorldHint"_s, t.openWorld}};
        tool->offered = t.offered;
        tool->offReason = t.offReason;
        tool->runExternal = std::move(t.run);
        tool->cancelExternal = std::move(t.cancel);
        m_external.push_back(std::move(tool));
    }
    if (toolNames() != before)
        emit toolsChanged();
}

int ToolHost::pendingCalls() const
{
    int count = static_cast<int>(m_others.size());
    for (const Lane* lane : {m_names.get(), m_contents.get()}) {
        if (lane)
            count += static_cast<int>(lane->queue.size()) + (lane->active ? 1 : 0);
    }
    return count;
}

void ToolHost::log(const QString& line) const
{
    if (m_services.log)
        m_services.log(line);
}

void ToolHost::call(std::uint64_t id, const QString& client, const QString& tool, const QJsonObject& arguments, Done done,
    CallOptions options)
{
    auto call = std::make_shared<Call>();
    call->id = id;
    call->client = client;
    call->arguments = arguments;
    call->done = std::move(done);
    call->token = options.token;
    call->cli = options.cli;
    call->encode = options.encode;
    call->timer.start();
    for (const Tool& t : m_tools) {
        if (t.name == tool)
            call->tool = &t;
    }
    for (const auto& t : m_external) {
        if (t->name == tool) {
            call->tool = t.get();
            call->keep = t;
        }
    }
    if (!call->tool || !call->tool->allowed(m_policy) || (call->tool->cliOnly && !call->cli)) {
        finish(call, {{}, true, u"Unknown tool: "_s + tool});
        return;
    }
    if (call->tool->external) {
        // Its owner runs it and answers when it will; the call waits among
        // the others so that cancel() finds it.
        m_others.push_back(call);
        std::weak_ptr<Call> weak = call;
        call->tool->runExternal(call->id, call->arguments, [this, weak](ToolResult result) {
            const std::shared_ptr<Call> c = weak.lock();
            if (!c)
                return;
            std::erase(m_others, c);
            finish(c, std::move(result));
        });
        return;
    }
    if (call->tool->engine != Tool::Engine::None) {
        Lane& lane = laneOf(*call->tool);
        // search_content's timeout counts from now: one still waiting for
        // the engine when it runs out answers then, without having searched.
        if (call->tool->engine == Tool::Engine::Content && (lane.active || !lane.queue.empty())) {
            QTimer::singleShot(std::chrono::milliseconds(contentTimeout(call->arguments)), this,
                [this, &lane, weak = std::weak_ptr<Call>(call)] { expireQueued(lane, weak); });
        }
        lane.queue.push_back(std::move(call));
        next(lane);
    } else {
        start(std::move(call));
    }
}

void ToolHost::expireQueued(Lane& lane, const std::weak_ptr<Call>& weak)
{
    const std::shared_ptr<Call> call = weak.lock();
    if (!call)
        return;
    const auto it = std::ranges::find(lane.queue, call);
    if (it == lane.queue.end())
        return; // it started (or was cancelled)
    lane.queue.erase(it);
    finish(call, waitedTooLong(call->timer));
}

void ToolHost::cancel(std::uint64_t id)
{
    for (Lane* lane : {m_names.get(), m_contents.get()}) {
        if (!lane)
            continue;
        for (auto it = lane->queue.begin(); it != lane->queue.end(); ++it) {
            if ((*it)->id == id) {
                (*it)->cancelled = true;
                lane->queue.erase(it);
                return;
            }
        }
        if (lane->active && lane->active->call->id == id) {
            const std::shared_ptr<Call> call = lane->active->call;
            call->cancelled = true;
            call->stop.request_stop(); // its work aside, if it got that far
            if (lane->active->requestId != 0)
                lane->engine->cancel(); // still searching
            lane->timeout.stop();
            lane->active.reset();
            next(*lane);
            return;
        }
    }
    for (auto it = m_others.begin(); it != m_others.end(); ++it) {
        if ((*it)->id == id) {
            const std::shared_ptr<Call> call = *it;
            call->cancelled = true;
            call->stop.request_stop();
            m_others.erase(it);
            if (call->tool && call->tool->external && call->tool->cancelExternal)
                call->tool->cancelExternal(id);
            if (call->settingsCard && m_services.cancelSettings)
                m_services.cancelSettings(); // the card goes with the call
            return;
        }
    }
}

void ToolHost::next(Lane& lane)
{
    while (!lane.active && !lane.queue.empty()) {
        std::shared_ptr<Call> call = std::move(lane.queue.front());
        lane.queue.pop_front();
        if (call->cancelled)
            continue;
        lane.active = std::make_unique<Active>();
        lane.active->call = call;
        start(call);
        // A handler that found the arguments wrong finished the call without
        // a search: on to the next.
        if (lane.active && lane.active->requestId == 0)
            lane.active.reset();
    }
}

// The caller's token belongs to its session, which closes it when it ends
// (and Windows may hand the same value to another handle): the thread gets
// a handle of its own to the same token. One that cannot be had is an error,
// never the default token, which may see more than the caller.
//
// The work runs on m_workers, a few at a time, the rest waiting their turn;
// cancelled while it waits, it never runs. What it held goes on its thread
// (a search's results), as does the making of the answer's JSON
// (encodeAnswer), unless `answer` is false: then `back` goes on with what
// came of it. Stopped (cancelled, or the host going), nothing comes back.
void ToolHost::aside(const std::shared_ptr<Call>& call, AsideWork work, std::function<void(ToolResult)> back, bool answer)
{
    std::shared_ptr<void> token;
    if (call->token) {
        HANDLE copy = nullptr;
        if (!::DuplicateHandle(::GetCurrentProcess(), call->token, ::GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            back(failure(u"Win顺 could not check what this client may see (error %1). Try again."_s.arg(::GetLastError())));
            return;
        }
        token.reset(copy, ::CloseHandle);
    }
    const bool encode = answer && call->encode;
    call->stop = m_workers->post([this, weak = std::weak_ptr<Call>(call), work = std::move(work), back = std::move(back), token,
                                     roots = remoteRoots(), encode, cli = call->cli,
                                     tool = call->tool ? call->tool->name : QString(),
                                     starting = m_services.asideStarting](const std::stop_token& stop) mutable {
        if (starting)
            starting(tool, stop);
        QuietIo quiet;
        Visibility view(static_cast<HANDLE>(token.get()), std::move(roots)); // null: the default token
        AsideContext context {stop, view, quiet, &m_checks};
        ToolResult result = work(context);
        work = nullptr; // what it held goes here, not on the GUI thread
        if (stop.stop_requested())
            return;
        if (encode)
            encodeAnswer(result, cli);
        QMetaObject::invokeMethod(this,
            [weak, back = std::move(back), result = std::move(result), checks = view.checks(), us = view.microseconds()]() mutable {
                if (const std::shared_ptr<Call> call = weak.lock()) {
                    call->checks += checks;
                    call->checkUs += us;
                }
                back(std::move(result));
            },
            Qt::QueuedConnection);
    });
}

// A call answered by `work` aside, among the others meanwhile (where
// cancel() finds it).
void ToolHost::runAside(const std::shared_ptr<Call>& call, AsideWork work)
{
    m_others.push_back(call);
    aside(call, std::move(work), [this, call](ToolResult result) {
        if (std::ranges::find(m_others, call) == m_others.end())
            return; // cancelled
        std::erase(m_others, call);
        finish(call, std::move(result));
    });
}

// A lane's search that its engine is done with: the lane goes on to the next
// one at once, and this call is finished by `work` aside meanwhile (its
// checks of the disk may take a while), so answers may come in another order
// than the calls were made.
void ToolHost::finishSearch(Lane& lane, AsideWork work)
{
    const std::shared_ptr<Call> call = std::move(lane.active->call);
    lane.active.reset();
    runAside(call, std::move(work));
    next(lane);
}

void ToolHost::start(std::shared_ptr<Call> call)
{
    call->tool->run(call);
}

void ToolHost::finish(const std::shared_ptr<Call>& call, ToolResult result)
{
    if (call->cancelled)
        return;
    logCall(*call, result);
    if (call->done)
        call->done(std::move(result));
}

void ToolHost::logCall(const Call& call, const ToolResult& result) const
{
    if (!m_services.log)
        return;
    // What was asked and what came of it; never the clipboard's texts.
    QJsonObject arguments = call.arguments;
    QString outcome;
    if (result.isError) {
        outcome = u"error: "_s + result.errorText.left(200).simplified();
    } else {
        const QJsonValue returned = result.structured.value(u"returned"_s);
        if (returned.isDouble())
            outcome = u"%1 items"_s.arg(static_cast<int>(returned.toDouble()));
        else if (result.structured.contains(u"opened"_s))
            outcome = result.structured.value(u"opened"_s).toBool() ? u"opened"_s : u"not opened"_s;
        else
            outcome = u"ok"_s;
    }
    QString line = u"%1 %2 %3 %4 → %5, %6 ms"_s.arg(QDateTime::currentDateTime().toString(Qt::ISODate),
        call.client.isEmpty() ? u"?"_s : call.client, call.tool ? call.tool->name : u"?"_s,
        QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact)), outcome)
                       .arg(call.timer.elapsed());
    if (call.checks > 0)
        line += u" (%1 access checks, %2 ms)"_s.arg(call.checks).arg(static_cast<double>(call.checkUs) / 1000.0, 0, 'f', 1);
    m_services.log(line);
}

QString ToolHost::instructions()
{
    return u"Win顺 (WinShun) indexes every file and folder name on this PC's local drives and searches the text "
           "inside text files and documents (Word, Excel, PowerPoint, PDF). search_files finds by name in "
           "milliseconds; search_content looks inside files (slower, returns partial results with complete=false when "
           "its timeout runs out); recent_files lists what changed lately; list_folder and get_file_info look around; "
           "read_file_text reads one file's whole text; index_status says whether the index is ready.\n"
           "search_files query syntax: words must all match (in the name, or in a parent folder's name); Chinese "
           "names also match pinyin initials or full pinyin (bg, baogao or ndbg for 年度报告); \"quoted phrase\"; "
           "!word excludes; *.pdf and ? wildcards; ext:pdf,docx keeps only those extensions; folder\\name needs a "
           "parent folder containing 'folder'; a full path such as D:\\Projects\\report lists that folder's matches "
           "first. Results are ranked: exact names, the user's own files and recently opened items first, system "
           "and program folders last.\n"
           "Users often describe a file by what it is about rather than by its words. When a search finds nothing or "
           "not the right file, search again with synonyms, related or broader terms, shorter word stems, and both "
           "Chinese and English (报销 → 费用, 差旅, reimbursement); look at likely candidates with read_file_text "
           "before telling the user which file it is.\n"
           "Paths are Windows paths; show them to the user as they are. A network path (\\\\server\\share) can be "
           "used only in a network location Win顺 indexes. Keep limits small (20–50) unless more is "
           "needed. Tools that open files or read the clipboard history are listed only when the user allowed them "
           "in Win顺's settings. screenshot_history lists the screenshots the user took with Win顺, with the text read "
           "in them and a PNG file to look at; take_screenshot asks the user on the screen every time and waits for "
           "the answer: call it only when a picture of the screen is really needed.\n"
           "Settings: get_settings lists Win顺's settings (use its sections, query or keys to look only at what is "
           "relevant; facts=true adds what Win顺 knows of this PC), open_settings shows the user where a setting is, "
           "and change_settings (offered only when the user allowed it) changes them. Read get_settings before you "
           "change anything; change only what the user asked for or agreed to; put every related change in ONE "
           "change_settings call, with a one-sentence reason the user will read next to your name. The user may be "
           "asked to allow it, and may undo it afterwards. Some settings are never changed by AI (the AI switches, "
           "plugins, commands, folders); when the user wants to do it themselves, call open_settings instead."_s;
}

QString ToolHost::setupPrompt(const QString& focus)
{
    QString text = u"Help me set up Win顺 (WinShun), the file search tool on this PC, to suit how I use my computer. "
                   "Answer in my language.\n"
                   "1. Call get_settings with facts=true (and, to keep it short, sections or a query for the area "
                   "I care about) to see the settings, their defaults and what this PC is like.\n"
                   "2. Ask me 3 to 5 short questions about how I use my computer: what I search for most (file names, "
                   "text inside files, the clipboard, web pages), which file manager and browser I use, whether I play "
                   "games, whether the computer is a laptop with little memory, and what annoys me in Win顺 now. "
                   "Ask them together in one message, in plain words, not in technical terms.\n"
                   "3. From my answers propose a small set of changes (at most about ten): for each say what it does "
                   "and why, in plain words. Do not change anything I did not ask for. Do not touch settings you could "
                   "not explain.\n"
                   "4. When I agree, apply them all with ONE change_settings call and a one-sentence reason. If it is "
                   "not offered, tell me to turn on 允许 AI 修改设置 in Win顺's settings (AI 助手 → 权限), or offer "
                   "open_settings so I can do it myself. I may be asked on the screen to allow the change, and I can "
                   "undo it afterwards.\n"
                   "5. Tell me what changed, and anything that needs me to do something (for example a restart of File "
                   "Explorer)."_s;
    if (!focus.trimmed().isEmpty())
        text += u"\nFocus on: "_s + focus.trimmed();
    return text;
}

// ---- the engine's answers ----------------------------------------------------

void ToolHost::onResults(Lane& lane, quint64 requestId, const SearchResults& results, qint64 totalMatches, qint64 elapsedUs)
{
    // A content search starts with an empty resultsReady: not this.
    const Active* active = lane.active.get();
    if (!active || active->requestId != requestId || active->content)
        return;
    const Call& call = *active->call;
    // Only what the caller may see: no count of the rest is given either
    // (the engine's total would count files the caller cannot list). The
    // access checks and the sizes go to the disk (a share: the network),
    // thousands of them for ws: aside.
    AsideWork work = [results, kind = active->kind, limit = active->limit, requestLimit = active->requestLimit,
                         folders = active->folders, shape = call.shape(active->sizes), cli = call.cli, excluded = m_excluded,
                         offline = m_services.offline, index = m_services.index, totalMatches, elapsedUs](AsideContext& aside) -> ToolResult {
        // A folder asked for that the caller cannot list is, to it, not
        // there: the same answer as for one the index does not have.
        if (std::optional<ToolResult> problem = checkFolders(aside, folders, offline))
            return *std::move(problem);
        const auto ofKind = [&](const SearchResult& r) {
            return !r.isApp() && !((kind == u"files" && r.isDir) || (kind == u"folders" && !r.isDir));
        };
        Budget budget(cli);
        LocalTimes times;
        QJsonArray items;
        bool beyond = false; // one more to show than the limit takes
        // On a share that cannot be reached now: nothing of it can be
        // checked (nor its size read) without waiting for the network.
        const bool done = forEachVisible(
            aside, results,
            [&](const SearchResult& r) {
                return ofKind(r) && !r.offline && !offline(r.path) && !excludedIndexPath(excluded, r.path);
            },
            [&](const SearchResult& r) {
                if (items.size() >= limit) {
                    beyond = true;
                    return false;
                }
                const QJsonObject item = fileItem(aside, times, r.path, r.name, r.isDir, r.modified, r.size, shape);
                if (!budget.take(item))
                    return false;
                items.push_back(item);
                return true;
            });
        if (!done)
            return {};
        // The engine found more than it gave. Asked for files, it gives the
        // folders that match too (kept apart, each kind to its limit): only
        // with all the files it could give are there more of them.
        const bool more = totalMatches > results.size()
            && (kind != u"files" || std::ranges::count_if(results, ofKind) >= requestLimit);
        QJsonObject out {{u"items"_s, items}, {u"returned"_s, items.size()}, {u"truncated"_s, beyond || budget.cut() || more},
            {u"elapsed_ms"_s, static_cast<double>(elapsedUs) / 1000.0}};
        if (index && index->state() != IndexService::State::Ready)
            out.insert(u"index_state"_s, stateName(index->state()));
        return {out};
    };
    finishSearch(lane, std::move(work));
}

void ToolHost::onContentResults(Lane& lane, quint64 requestId, const SearchResults& results)
{
    Active* active = lane.active.get();
    if (!active || active->requestId != requestId || !active->content)
        return;
    active->found += results;
}

void ToolHost::onContentProgress(Lane& lane, quint64 requestId, int scanned, int total, bool finished)
{
    Active* active = lane.active.get();
    if (!active || active->requestId != requestId || !active->content)
        return;
    active->scanned = scanned;
    active->total = total;
    if (finished)
        finishContent(lane, true);
}

void ToolHost::finishContent(Lane& lane, bool complete)
{
    Active* active = lane.active.get();
    if (!active || !active->content || active->requestId == 0)
        return;
    lane.timeout.stop();
    if (!complete)
        lane.engine->cancel();
    const Call& call = *active->call;
    // The caller must be able to read the file itself (not only to see it):
    // the snippet is its contents. Checks of the disk: aside.
    AsideWork work = [found = std::move(active->found), limit = active->limit, scanned = active->scanned, total = active->total,
                         complete, folders = active->folders, shape = call.shape(), cli = call.cli, excluded = m_excluded,
                         offline = m_services.offline, timer = call.timer](AsideContext& aside) mutable -> ToolResult {
        if (std::optional<ToolResult> problem = checkFolders(aside, folders, offline))
            return *std::move(problem);
        std::ranges::stable_sort(found, std::ranges::greater {}, &SearchResult::relevance);
        Budget budget(cli);
        LocalTimes times;
        QJsonArray items;
        bool beyond = false; // one more the caller may read, past the limit
        const bool done = forEachVisible(
            aside, found,
            [&](const SearchResult& r) {
                return !r.offline && !offline(r.path) && !excludedIndexPath(excluded, r.path);
            },
            [&](const SearchResult& r) {
                if (!aside.canRead(r.path))
                    return true;
                if (items.size() >= limit) {
                    beyond = true;
                    return false;
                }
                const QJsonObject item = contentItem(r, times, shape);
                if (!budget.take(item))
                    return false;
                items.push_back(item);
                return true;
            });
        if (!done)
            return {};
        QJsonObject out {{u"items"_s, items}, {u"returned"_s, items.size()}, {u"truncated"_s, beyond || budget.cut()}, {u"complete"_s, complete},
            {u"files_checked"_s, scanned}, {u"files_candidates"_s, total}, {u"elapsed_ms"_s, static_cast<double>(timer.elapsed())}};
        if (budget.cut())
            out.insert(u"note"_s, u"More matched than one answer holds: these are the best. Ask for fewer (limit), or narrow the query or the folders."_s);
        else if (!complete)
            out.insert(u"note"_s, u"The timeout ran out: these are the matches found so far, best first. Call again with a larger timeout_ms or a more specific query for the rest."_s);
        return {out};
    };
    finishSearch(lane, std::move(work));
}

// ---- the tools ----------------------------------------------------------------

void ToolHost::addTools()
{
    Tool search;
    search.name = u"search_files"_s;
    search.title = u"Search file and folder names"_s;
    search.description = u"Find files and folders on this PC by name, instantly, from Win顺's index of all local drives. "
                         "Supports pinyin for Chinese names (bg or baogao for 报告), \"quoted phrases\", !exclusions, "
                         "wildcards (*.pdf), ext:pdf,docx, folder\\name, and full paths (D:\\Projects\\report). "
                         "Ranked by relevance (exact names and the user's own, recently opened files first) or by "
                         "modification time."_s;
    search.inputSchema = object(
        {{u"query"_s, typed("string", u"What to find; see the server instructions for the syntax"_s)},
            {u"kind"_s, choice(u"Both files and folders, files only, or folders only"_s, {u"all"_s, u"files"_s, u"folders"_s}, u"all"_s)},
            {u"sort"_s, choice(u"Best matches first, the most recently modified first, the oldest first, or by name, containing folder, file type (extension), largest first, smallest first"_s,
                                         {u"relevance"_s, u"modified"_s, u"oldest"_s, u"name"_s, u"folder"_s, u"type"_s, u"largest"_s, u"smallest"_s}, u"relevance"_s)},
            {u"limit"_s, integer(u"How many results at most"_s, 1, kMaxFiles, kDefaultFiles)},
            {u"folders"_s, array(typed("string", u"Full path"_s), u"Only in these folders and the folders in them; default: everywhere"_s)},
            {u"size"_s, sizeArgument()}},
        {u"query"_s});
    search.outputSchema = object({{u"items"_s, array(itemSchema(), u"The matches, best first"_s)},
        {u"returned"_s, typed("integer", u"How many items"_s)},
        {u"truncated"_s, typed("boolean", u"More matched than returned: narrow the query, or raise the limit"_s)},
        {u"elapsed_ms"_s, typed("number", u"Search time"_s)},
        {u"index_state"_s, typed("string", u"Present while the index is still being built: results may be incomplete"_s)}});
    search.annotations = annotations(true, true);
    search.engine = Tool::Engine::Names;
    search.run = [this](const std::shared_ptr<Call>& call) { searchFiles(call); };
    m_tools.push_back(std::move(search));

    Tool content;
    content.name = u"search_content"_s;
    content.title = u"Search text inside files"_s;
    content.description = u"Find files whose text contains the given words: text files (txt, md, log, csv, json, code…) "
                          "and documents (Word, Excel, PowerPoint, PDF, WPS). Several words must all occur in the file; "
                          "a \"quoted phrase\" is found as written; a line of code is found as it is; a|b finds files "
                          "with either word; regex:EXPR is a regular expression (PCRE, any case, matched within a "
                          "line; regex=true makes every word one). An expression with some fixed text in it (three "
                          "letters or more, or Chinese) is fast; one without (\\d+) has to read every file. Each match comes "
                          "with the matching line (or page, slide, sheet row) and a snippet. Slower than search_files: "
                          "returns what it found when timeout_ms runs out (complete=false). Files on network shares and "
                          "in WSL are found only where the user turned on their content search in Win顺's settings."_s;
    content.inputSchema = object(
        {{u"query"_s, typed("string", u"Words that must all occur, a \"quoted phrase\", a|b for either, regex:EXPR, or a line of code"_s)},
            {u"regex"_s, boolean(u"Every word of the query is a regular expression (a space still parts them; quote one that has spaces)"_s, false)},
            {u"limit"_s, integer(u"How many matching files at most"_s, 1, kMaxContent, kDefaultContent)},
            {u"timeout_ms"_s, integer(u"How long to search before returning what was found"_s, kMinTimeoutMs, kMaxTimeoutMs, kDefaultTimeoutMs)},
            {u"include_system"_s, boolean(u"Also look in system, program and tool folders (node_modules, .git…)"_s, false)},
            {u"folders"_s, array(typed("string", u"Full path"_s), u"Only in these folders and the folders in them; default: everywhere. A folder named is searched even if it is a system or program folder."_s)}},
        {u"query"_s});
    content.outputSchema = object({{u"items"_s, array(object({{u"path"_s, typed("string", u"Full Windows path"_s)},
                                                           {u"name"_s, typed("string", u"File name"_s)},
                                                           {u"modified"_s, typed("string", u"When last written, ISO 8601"_s)},
                                                           {u"location"_s, typed("object", u"kind (line, page, slide, row, document) with line or number, and sheet"_s)},
                                                           {u"snippet"_s, typed("string", u"The matching line or passage"_s)},
                                                           {u"score"_s, typed("integer", u"Relevance, higher is better"_s)}}),
                                       u"The matches, best first"_s)},
        {u"returned"_s, typed("integer", u"How many items"_s)},
        {u"truncated"_s, typed("boolean", u"More matched than returned: narrow the query or the folders, or raise the limit"_s)},
        {u"complete"_s, typed("boolean", u"False when the timeout ran out first"_s)},
        {u"queued"_s, typed("boolean", u"True (only then present) when another content search kept the engine for the whole timeout: nothing was searched"_s)},
        {u"files_checked"_s, typed("integer", u"Files looked at"_s)}, {u"files_candidates"_s, typed("integer", u"Files that could contain the words"_s)},
        {u"elapsed_ms"_s, typed("number", u"Search time"_s)}, {u"note"_s, typed("string", u"Advice when the result is partial"_s)}});
    content.annotations = annotations(true, true);
    content.engine = Tool::Engine::Content;
    content.run = [this](const std::shared_ptr<Call>& call) { searchContent(call); };
    m_tools.push_back(std::move(content));

    Tool recent;
    recent.name = u"recent_files"_s;
    recent.title = u"Recently modified files"_s;
    recent.description = u"The files (or folders) most recently written, newest first, optionally within a folder, of "
                         "certain extensions, or within the last days. No query needed; for names use search_files "
                         "with sort=modified."_s;
    recent.inputSchema = object({{u"folder"_s, typed("string", u"Only under this folder (full path); default: all drives"_s)},
        {u"extensions"_s, array(typed("string", u"Extension without the dot, e.g. docx"_s), u"Only these extensions"_s)},
        {u"days"_s, integer(u"Only files modified within this many days; 0 = any time"_s, 0, 36500, 0)},
        {u"kind"_s, choice(u"Files, folders, or both"_s, {u"files"_s, u"folders"_s, u"all"_s}, u"files"_s)},
        {u"include_system"_s, boolean(u"Also system, program and tool folders"_s, false)},
        {u"include_hidden"_s, boolean(u"Also hidden files and folders"_s, false)},
        {u"limit"_s, integer(u"How many at most"_s, 1, kMaxFiles, kDefaultFiles)},
        {u"size"_s, sizeArgument()}});
    recent.outputSchema = object({{u"items"_s, array(itemSchema(), u"Newest first"_s)}, {u"returned"_s, typed("integer", u"How many items"_s)},
        {u"truncated"_s, typed("boolean", u"More matched than returned: narrow it (folder, extensions, days), or raise the limit"_s)},
        {u"elapsed_ms"_s, typed("number", u"Scan time"_s)},
        {u"index_state"_s, typed("string", u"Present while the index is still being built: results may be incomplete"_s)}});
    recent.annotations = annotations(true, true);
    recent.run = [this](const std::shared_ptr<Call>& call) { recentFiles(call); };
    m_tools.push_back(std::move(recent));

    Tool list;
    list.name = u"list_folder"_s;
    list.title = u"List a folder"_s;
    list.description = u"What a folder holds, from the index (instant, no disk access): folders first, then files, by name. "
                       "Only folders on indexed drives that are not excluded from indexing."_s;
    list.inputSchema = object({{u"path"_s, typed("string", u"Full path of the folder, e.g. D:\\Projects"_s)},
                                  {u"kind"_s, choice(u"Both, files only, or folders only"_s, {u"all"_s, u"files"_s, u"folders"_s}, u"all"_s)},
                                  {u"limit"_s, integer(u"How many entries at most"_s, 1, kMaxFolder, kDefaultFolder)},
                                  {u"size"_s, sizeArgument()}},
        {u"path"_s});
    list.outputSchema = object({{u"path"_s, typed("string", u"The folder"_s)}, {u"items"_s, array(itemSchema(), u"Its entries"_s)},
        {u"returned"_s, typed("integer", u"How many items"_s)},
        {u"total"_s, typed("integer", u"How many entries the folder has (not counting any in folders hidden from AI)"_s)},
        {u"truncated"_s, typed("boolean", u"More than returned"_s)}});
    list.annotations = annotations(true, true);
    list.run = [this](const std::shared_ptr<Call>& call) { listFolder(call); };
    m_tools.push_back(std::move(list));

    Tool info;
    info.name = u"get_file_info"_s;
    info.title = u"File or folder details"_s;
    info.description = u"Size, dates and attributes of one file or folder, read from the disk, and whether Win顺 indexes it."_s;
    info.inputSchema = object({{u"path"_s, typed("string", u"Full path"_s)}}, {u"path"_s});
    info.outputSchema = object({{u"exists"_s, typed("boolean", u"Whether it is there"_s)}, {u"path"_s, typed("string", u"The path, normalized"_s)},
        {u"name"_s, typed("string", u"Name"_s)}, {u"type"_s, typed("string", u"file or folder"_s)}, {u"size"_s, typed("integer", u"Bytes (files)"_s)},
        {u"created"_s, typed("string", u"ISO 8601"_s)}, {u"modified"_s, typed("string", u"ISO 8601"_s)}, {u"accessed"_s, typed("string", u"ISO 8601"_s)},
        {u"extension"_s, typed("string", u"Without the dot"_s)}, {u"hidden"_s, typed("boolean", u"Hidden attribute"_s)},
        {u"read_only"_s, typed("boolean", u"Read-only attribute"_s)}, {u"system"_s, typed("boolean", u"System attribute"_s)},
        {u"cloud_placeholder"_s, typed("boolean", u"A cloud file not on this disk: reading it would download it"_s)},
        {u"indexed"_s, typed("boolean", u"Whether it is in Win顺's index"_s)}});
    info.annotations = annotations(true, true);
    info.run = [this](const std::shared_ptr<Call>& call) { fileInfo(call); };
    m_tools.push_back(std::move(info));

    Tool open;
    open.name = u"open_item"_s;
    open.title = u"Open a file or folder"_s;
    open.description = u"Opens a file with its default program, or a folder in the file manager, with the user's normal "
                       "rights (never elevated), as a double-click would. Only for items that exist on this PC."_s;
    open.inputSchema = object({{u"path"_s, typed("string", u"Full path of the file or folder"_s)}}, {u"path"_s});
    open.outputSchema = object({{u"opened"_s, typed("boolean", u"Whether it was started"_s)}, {u"path"_s, typed("string", u"What was opened"_s)}});
    open.annotations = annotations(false, false);
    open.gate = Tool::Gate::Open;
    open.offered = static_cast<bool>(m_services.open);
    open.run = [this](const std::shared_ptr<Call>& call) { openItem(call, false); };
    m_tools.push_back(std::move(open));

    Tool reveal;
    reveal.name = u"show_in_folder"_s;
    reveal.title = u"Show in its folder"_s;
    reveal.description = u"Opens the folder that holds the file (or folder) in the file manager, with the item selected."_s;
    reveal.inputSchema = object({{u"path"_s, typed("string", u"Full path of the item to select"_s)}}, {u"path"_s});
    reveal.outputSchema = object({{u"opened"_s, typed("boolean", u"Whether the folder was opened"_s)}, {u"path"_s, typed("string", u"The item"_s)}});
    reveal.annotations = annotations(false, false);
    reveal.gate = Tool::Gate::Open;
    reveal.offered = static_cast<bool>(m_services.reveal);
    reveal.run = [this](const std::shared_ptr<Call>& call) { openItem(call, true); };
    m_tools.push_back(std::move(reveal));

    Tool clips;
    clips.name = u"clipboard_history"_s;
    clips.title = u"Clipboard history"_s;
    clips.description = u"What the user copied lately (Win顺's clipboard history), newest first: text, links, file paths, "
                        "copied files and pictures (pictures as their size only). Texts are cut to max_chars; "
                        "clipboard_get reads one whole. Searchable by words (pinyin too) and @program."_s;
    clips.inputSchema = object({{u"query"_s, typed("string", u"Words that must all occur; @name keeps to what one program copied"_s)},
        {u"kind"_s, choice(u"Which entries"_s, {u"all"_s, u"text"_s, u"link"_s, u"path"_s, u"files"_s, u"image"_s}, u"all"_s)},
        {u"limit"_s, integer(u"How many entries at most"_s, 1, kMaxClips, kDefaultClips)},
        {u"max_chars"_s, integer(u"Characters of each text to include"_s, 50, kMaxClipChars, kDefaultClipChars)}});
    clips.outputSchema = object({{u"items"_s, array(object({{u"id"_s, typed("integer", u"For clipboard_get"_s)},
                                                         {u"kind"_s, typed("string", u"text, link, path, files or image"_s)},
                                                         {u"text"_s, typed("string", u"The text, cut to max_chars; files as one path per line"_s)},
                                                         {u"chars"_s, typed("integer", u"Length of the whole text"_s)},
                                                         {u"truncated"_s, typed("boolean", u"The text was cut"_s)},
                                                         {u"copied"_s, typed("string", u"First copied, ISO 8601"_s)},
                                                         {u"used"_s, typed("string", u"Last copied or pasted, ISO 8601"_s)},
                                                         {u"source"_s, typed("string", u"The program it was copied from"_s)},
                                                         {u"group"_s, typed("string", u"The group it is kept in, if any"_s)},
                                                         {u"pinned"_s, typed("boolean", u"Kept for good"_s)},
                                                         {u"width"_s, typed("integer", u"Pictures: pixels"_s)},
                                                         {u"height"_s, typed("integer", u"Pictures: pixels"_s)}}),
                                     u"Newest first"_s)},
        {u"returned"_s, typed("integer", u"How many items"_s)}});
    clips.annotations = annotations(true, true);
    clips.gate = Tool::Gate::Clipboard;
    clips.offered = static_cast<bool>(m_services.clipboard);
    clips.run = [this](const std::shared_ptr<Call>& call) { clipboardHistory(call); };
    m_tools.push_back(std::move(clips));

    Tool clip;
    clip.name = u"clipboard_get"_s;
    clip.title = u"One clipboard entry"_s;
    clip.description = u"The whole text of one entry of the clipboard history, by the id clipboard_history gave."_s;
    clip.inputSchema = object({{u"id"_s, typed("integer", u"The entry's id"_s)},
                                  {u"max_chars"_s, integer(u"Characters at most"_s, 100, 200000, 20000)}},
        {u"id"_s});
    clip.outputSchema = object({{u"id"_s, typed("integer", u"The entry"_s)}, {u"text"_s, typed("string", u"Its text"_s)},
        {u"chars"_s, typed("integer", u"Length of the whole text"_s)}, {u"truncated"_s, typed("boolean", u"Cut to max_chars"_s)}});
    clip.annotations = annotations(true, true);
    clip.gate = Tool::Gate::Clipboard;
    clip.offered = static_cast<bool>(m_services.clipboardText);
    clip.run = [this](const std::shared_ptr<Call>& call) { clipboardGet(call); };
    m_tools.push_back(std::move(clip));

    Tool shots;
    shots.name = u"screenshot_history"_s;
    shots.title = u"Screenshot history"_s;
    shots.description = u"Screenshots the user took with Win顺's screen capture, newest first, with the text read in them "
                        "(OCR) and the path of a PNG copy to look at. query keeps those whose text has all the words "
                        "(pinyin too). Empty, with a reason, while Win顺's screen capture is turned off."_s;
    shots.inputSchema = object({{u"query"_s, typed("string", u"Words that must all occur in the text read in the picture"_s)},
        {u"limit"_s, integer(u"How many screenshots at most"_s, 1, kMaxShots, kDefaultShots)}});
    shots.outputSchema = object({{u"items"_s, array(object({{u"id"_s, typed("integer", u"The clipboard history entry"_s)},
                                                         {u"taken"_s, typed("string", u"When it was taken, ISO 8601"_s)},
                                                         {u"width"_s, typed("integer", u"Pixels"_s)},
                                                         {u"height"_s, typed("integer", u"Pixels"_s)},
                                                         {u"text"_s, typed("string", u"The text read in it; empty for none"_s)},
                                                         {u"image_path"_s, typed("string", u"A PNG copy in the temporary folder"_s)}}),
                                     u"Newest first"_s)},
        {u"returned"_s, typed("integer", u"How many items"_s)},
        {u"reason"_s, typed("string", u"Why there are none, when the screen capture is off"_s)}});
    shots.annotations = annotations(true, true);
    shots.gate = Tool::Gate::Clipboard;
    shots.offered = static_cast<bool>(m_services.screenshots);
    shots.run = [this](const std::shared_ptr<Call>& call) { screenshotHistory(call); };
    m_tools.push_back(std::move(shots));

    Tool take;
    take.name = u"take_screenshot"_s;
    take.title = u"Take a screenshot"_s;
    take.description = u"A picture of one monitor, as a PNG file. Every call shows the user a confirmation on the screen "
                       "naming the client and the monitor; the picture is taken only when the user clicks Allow. Refused, "
                       "or no answer within 30 seconds, it returns an error: do not ask again unless the user wants it. "
                       "The picture is not put on the clipboard or into its history."_s;
    take.inputSchema = object({{u"monitor"_s, integer(u"Which monitor: 1 is the primary one, then the others; 0: the one with the mouse pointer"_s, 0, 16, 0)}});
    take.outputSchema = object({{u"path"_s, typed("string", u"The PNG file"_s)}, {u"width"_s, typed("integer", u"Pixels"_s)},
        {u"height"_s, typed("integer", u"Pixels"_s)}, {u"monitor"_s, typed("string", u"Which monitor was taken"_s)}});
    take.annotations = annotations(true, false);
    take.gate = Tool::Gate::Screenshot;
    take.offered = static_cast<bool>(m_services.takeScreenshot);
    take.run = [this](const std::shared_ptr<Call>& call) { takeScreenshot(call); };
    m_tools.push_back(std::move(take));

    Tool settings;
    settings.name = u"get_settings"_s;
    settings.title = u"Win顺's settings"_s;
    settings.description = u"The settings of Win顺 that AI may know, each with its key, current value, default, what it does and "
                           "what values it takes, grouped in sections (shortcuts, taskbar, files_and_dialogs, startup, appearance, "
                           "scope, results, content, web, clipboard, capture, advanced). Narrow it with sections, keys or query "
                           "(words in Chinese or English), or changed_only for what differs from the defaults. facts=true adds "
                           "what Win顺 knows of this PC that bears on the settings. Settings that are not listed are not AI's to "
                           "know or change."_s;
    settings.inputSchema = object(
        {{u"sections"_s, array(typed("string", u"A section id"_s), u"Only these sections"_s)},
            {u"keys"_s, array(typed("string", u"A setting's key, like Launcher/DoubleCtrl"_s), u"Only these settings"_s)},
            {u"query"_s, typed("string", u"Words that must all occur in a setting's key, title, description or choices"_s)},
            {u"changed_only"_s, boolean(u"Only the settings that differ from their defaults"_s, false)},
            {u"facts"_s, boolean(u"Also say what Win顺 knows of this PC (Windows version, memory, file managers installed, administrator rights …)"_s, false)}});
    settings.outputSchema = object({{u"settings"_s, array(typed("object", u"key, title, section, type, value, default, choices, min, max, writable, ask_always, effect, about"_s), u"The settings that fit"_s)},
        {u"sections"_s, array(typed("object", u"id and title"_s), u"The sections they are in"_s)},
        {u"ai"_s, typed("object", u"settings_mode: off, ask or auto: whether change_settings is offered and how"_s)},
        {u"facts"_s, typed("object", u"Only with facts=true"_s)}});
    settings.annotations = annotations(true, true);
    settings.offered = static_cast<bool>(m_services.getSettings);
    settings.run = [this](const std::shared_ptr<Call>& call) { getSettings(call); };
    m_tools.push_back(std::move(settings));

    Tool change;
    change.name = u"change_settings"_s;
    change.title = u"Change Win顺's settings"_s;
    change.description = u"Changes settings of Win顺 for the user, all of them or none: every change is checked first and one "
                         "that is wrong stops the whole call with every problem listed. Take the keys and the values they accept "
                         "from get_settings. A change is {key, value} (a list: the whole new list), or {key, add: [...]} or "
                         "{key, remove: [...]} for a list. Put all related changes in one call, with a one-sentence reason in the "
                         "user's language: the user reads it, as yours, on a card that asks them to allow the change (always "
                         "for some settings; for the others unless they said to change directly). Declined or unanswered "
                         "(60 seconds): an error, nothing changed, do not ask again unless the user wants it. The result says "
                         "what each setting was and is, whether it took effect, and what the user still has to do (File "
                         "Explorer or Win顺 restarting). To undo, call again with the old values."_s;
    {
        QJsonObject item = object({{u"key"_s, typed("string", u"The setting's key from get_settings"_s)},
                                      {u"value"_s, QJsonObject {{u"description"_s, u"The new value (a boolean, a string, a number, or for a list the whole new list)"_s}}},
                                      {u"add"_s, QJsonObject {{u"description"_s, u"A list: the items to add (an array; for web_shortcuts objects {keyword, name?, url, home?})"_s}}},
                                      {u"remove"_s, QJsonObject {{u"description"_s, u"A list: the items to take out (an array; for web_shortcuts the keywords)"_s}}}},
            {u"key"_s});
        QJsonObject changes = array(item, u"The changes to make together"_s);
        changes.insert(u"minItems"_s, 1);
        changes.insert(u"maxItems"_s, kMaxSettingChanges);
        QJsonObject reason = typed("string", u"One sentence for the user: why these changes (shown next to your name)"_s);
        reason.insert(u"maxLength"_s, kMaxReasonChars);
        change.inputSchema = object({{u"changes"_s, changes}, {u"reason"_s, reason}}, {u"changes"_s, u"reason"_s});
    }
    change.outputSchema = object({{u"changed"_s, typed("integer", u"How many settings changed"_s)},
        {u"unchanged"_s, array(typed("string", u"A key"_s), u"Settings that were set so already"_s)},
        {u"settings"_s, array(typed("object", u"key, title, old, new, effect, applied, note"_s), u"What each change came to"_s)},
        {u"explorer_restart_needed"_s, typed("boolean", u"File Explorer has to restart for some of it to show: the user can do it from the notice"_s)},
        {u"undo_hint"_s, typed("string", u"How to undo"_s)}});
    change.annotations = annotations(false, true);
    change.gate = Tool::Gate::Settings;
    change.offered = m_services.planSettings && m_services.confirmSettings && m_services.applySettings;
    change.run = [this](const std::shared_ptr<Call>& call) { changeSettings(call); };
    m_tools.push_back(std::move(change));

    Tool openSetting;
    openSetting.name = u"open_settings"_s;
    openSetting.title = u"Show a setting to the user"_s;
    openSetting.description = u"Opens Win顺's settings window at a setting (key from get_settings) or a section, scrolled to it, "
                              "in front of the user. Use it when the user wants to look at or change something themselves, or "
                              "for what AI may not change."_s;
    openSetting.inputSchema = object({{u"key"_s, typed("string", u"A setting's key from get_settings"_s)},
        {u"section"_s, typed("string", u"A section id from get_settings"_s)}});
    openSetting.outputSchema = object({{u"opened"_s, typed("boolean", u"The window was opened"_s)},
        {u"where"_s, typed("string", u"The page it was opened at"_s)}});
    openSetting.annotations = annotations(true, true);
    openSetting.offered = static_cast<bool>(m_services.openSettings);
    openSetting.run = [this](const std::shared_ptr<Call>& call) { openSettings(call); };
    m_tools.push_back(std::move(openSetting));

    Tool status;
    status.name = u"index_status"_s;
    status.title = u"Win顺 status"_s;
    status.description = u"Whether Win顺 is running and its index is ready: how many items it holds, which drives are "
                         "still being read, the state of the content index, and which optional tools the user allowed."_s;
    status.inputSchema = {{u"type"_s, u"object"_s}, {u"additionalProperties"_s, false}};
    status.outputSchema = object({{u"running"_s, typed("boolean", u"Win顺 is running"_s)},
        {u"connected"_s, typed("boolean", u"The AI connection to Win顺 is up; false when it is off or Win顺 is not running, and the relay answers alone"_s)},
        {u"version"_s, typed("string", u"Its version"_s)},
        {u"index"_s, typed("object", u"state (loading, building, ready), items, volumes"_s)},
        {u"content_index"_s, typed("object", u"enabled, files, pending, reading"_s)},
        {u"ai"_s, typed("object", u"open_allowed, clipboard_allowed (and readable), clipboard_history (Win顺 keeps one), screenshot_allowed, capture_enabled, settings_mode (off, ask, auto), excluded_folders"_s)}, {u"hint"_s, typed("string", u"What to tell the user when Win顺 is not running"_s)}});
    status.annotations = annotations(true, true);
    status.run = [this](const std::shared_ptr<Call>& call) { indexStatus(call); };
    m_tools.push_back(std::move(status));

    Tool text;
    text.name = u"read_file_text"_s;
    text.title = u"Read the text of a file"_s;
    text.description = u"The text of one file: a document's (Word, Excel, PowerPoint, PDF, WPS) as Win顺 read it out, "
                       "with [page N], [slide N] and [sheet Name] lines where those begin, or a text file's (txt, md, "
                       "csv, code…). Use it to see what a file found by search_files or search_content says. A long "
                       "text comes in parts: pass next_offset as offset for the next one."_s;
    text.inputSchema = object({{u"path"_s, typed("string", u"Full path of the file"_s)},
                                  {u"offset"_s, integer(u"Where to start, in characters"_s, 0, std::numeric_limits<int>::max(), 0)},
                                  {u"max_chars"_s, integer(u"Characters at most"_s, kMinTextChars, kMaxTextChars, kDefaultTextChars)}},
        {u"path"_s});
    text.outputSchema = object({{u"path"_s, typed("string", u"The file"_s)},
        {u"kind"_s, choice(u"A document or a text file"_s, {u"document"_s, u"text"_s}, u"text"_s)},
        {u"text"_s, typed("string", u"The text from offset"_s)}, {u"chars"_s, typed("integer", u"Length of the whole text"_s)},
        {u"offset"_s, typed("integer", u"Where this part starts"_s)},
        {u"next_offset"_s, typed("integer", u"Where the rest starts; absent at the end"_s)},
        {u"cut"_s, typed("boolean", u"The file holds more than Win顺 reads (16 MB of text)"_s)},
        {u"note"_s, typed("string", u"Why the text is empty, when it is"_s)}});
    text.annotations = annotations(true, true);
    text.run = [this](const std::shared_ptr<Call>& call) { readFileText(call); };
    m_tools.push_back(std::move(text));

    Tool docs;
    docs.name = u"list_documents"_s;
    docs.title = u"Files with text in the content index"_s;
    docs.description = u"Every file whose text the content index holds (documents and text files), in a stable order, a "
                       "page at a time: pass next_cursor as cursor for the next page. Each comes with a stamp that "
                       "changes when the file does, so a reader can tell what to read again with read_file_text."_s;
    docs.inputSchema = object({{u"folders"_s, array(typed("string", u"Full path"_s), u"Only under these folders; default: all"_s)},
        {u"extensions"_s, array(typed("string", u"Extension without the dot, e.g. docx"_s), u"Only these extensions"_s)},
        {u"include_system"_s, boolean(u"Also system, program and tool folders"_s, false)},
        {u"cursor"_s, integer(u"Where to go on from: next_cursor of the page before"_s, 0, std::numeric_limits<int>::max(), 0)},
        {u"limit"_s, integer(u"How many at most"_s, 1, kMaxDocuments, kDefaultDocuments)}});
    docs.outputSchema = object({{u"items"_s, array(object({{u"path"_s, typed("string", u"Full Windows path"_s)},
                                                        {u"kind"_s, typed("string", u"document or text"_s)},
                                                        {u"stamp"_s, typed("integer", u"Changes when the file changes"_s)}}),
                                    u"The files"_s)},
        {u"returned"_s, typed("integer", u"How many items"_s)},
        {u"next_cursor"_s, typed("integer", u"For the next page; absent after the last"_s)},
        {u"reading"_s, typed("boolean", u"The content index is still reading files: more may come"_s)}});
    docs.annotations = annotations(true, true);
    docs.gate = Tool::Gate::Documents;
    docs.run = [this](const std::shared_ptr<Call>& call) { listDocuments(call); };
    m_tools.push_back(std::move(docs));

    // ws plugin: one handler in the app for them all (Services::plugins),
    // which reads the arguments itself. No schema: they are never listed.
    for (const char* name : kPluginTools) {
        Tool plugins;
        plugins.name = QString::fromLatin1(name);
        plugins.cliOnly = true;
        plugins.offered = static_cast<bool>(m_services.plugins);
        plugins.run = [this](const std::shared_ptr<Call>& call) { pluginCommand(call); };
        m_tools.push_back(std::move(plugins));
    }
}

void ToolHost::searchFiles(const std::shared_ptr<Call>& call)
{
    Args args(call->arguments);
    const QString query = args.string(u"query"_s, true).trimmed();
    const QString kind = args.choice(u"kind"_s, u"all"_s, {u"all"_s, u"files"_s, u"folders"_s});
    const QString sort = args.choice(u"sort"_s, u"relevance"_s,
        {u"relevance"_s, u"modified"_s, u"oldest"_s, u"name"_s, u"folder"_s, u"type"_s, u"largest"_s, u"smallest"_s});
    const int limit = args.integer(u"limit"_s, kDefaultFiles, 1, call->limits().files);
    // Sorted by size, the sizes are what the order is about: read them
    // whatever was asked. (The order itself is the index's: it holds sizes.)
    const bool sizes = args.boolean(u"size"_s, true) || sort == u"largest" || sort == u"smallest";
    const QStringList folders = args.strings(u"folders"_s);
    if (!args.error().isEmpty())
        return finish(call, failure(args.error()));
    if (query.isEmpty())
        return finish(call, failure(u"query is empty. For the newest files without a query, use recent_files."_s));
    // The folder of a full path in the query is listed from the disk too
    // (SearchEngine): not one on a server the index has no root on, which
    // would be signed in to.
    if (const QString typed = pathtext::splitTyped(query).folder;
        !typed.isEmpty() && Visibility::placeOf(typed, remoteRoots()) == Visibility::Place::Elsewhere)
        return finish(call, failure(notOurs(typed)));
    QStringList named;
    for (const QString& folder : folders) {
        if (folder.isEmpty())
            continue;
        if (!isFullPath(folder))
            return finish(call, failure(u"folders must be full paths, like D:\\Projects or \\\\server\\share\\folder: "_s + folder));
        if (excluded(folder))
            return finish(call, notFound(u"This folder is not visible to AI (excluded in Win顺's settings): "_s + folder));
        named.push_back(folder);
    }
    // The folders to look in, as the index has them: the engine keeps to
    // them as it goes through the names (Request::folders), so the limit
    // counts only what lies in them. What the caller may know of them is
    // checked aside (checkFolders): once the engine answered, or, for one
    // that is not a folder to search in, without a search.
    std::vector<FolderArg> under;
    if (!named.isEmpty())
        under = lookUp(*m_services.index->index(), named);
    if (answeredWithoutSearch(under, m_services.offline)) {
        return runAside(call, [under, offline = m_services.offline](AsideContext& aside) {
            return checkFolders(aside, under, offline).value_or(notFound(notInIndex(under.front().given)));
        });
    }

    SearchEngine::Request request;
    request.text = query;
    request.scope = kind == u"folders" ? Scope::Folders : Scope::Files;
    // The engine returns the folders that match along with the files, kept
    // apart: asked for files, they go first, or a page of folders would take
    // the places of the files (the folders are dropped afterwards).
    request.kindOrder = kind == u"files" ? KindOrder::FilesFirst : m_policy.kindOrder;
    for (const FolderArg& folder : under)
        request.folders.push_back(folder.path);
    request.rankBy = sort == u"modified" ? RankBy::Newest : sort == u"oldest" ? RankBy::Oldest : RankBy::Match;
    // The launcher's other sorts (#44), over every match, not the first page.
    if (sort == u"name")
        request.order = {SortKey::Name, false};
    else if (sort == u"folder")
        request.order = {SortKey::Path, false};
    else if (sort == u"type")
        request.order = {SortKey::Type, false};
    else if (sort == u"largest")
        request.order = {SortKey::Size, true};
    else if (sort == u"smallest")
        request.order = {SortKey::Size, false};
    request.excludedTypes = m_policy.excludedTypes; // unless the query says ext:
    request.excludedFileNames = m_policy.excludedFileNames;
    // Files and folders are each kept to the limit (kindsApart); excluded
    // ones and those the caller may not see are taken out afterwards, so a
    // few more are asked for.
    request.limit = limit + std::max(10, limit / 10);
    if (m_services.history)
        request.history = m_services.history();
    Lane& lane = laneOf(*call->tool);
    Active& active = *lane.active;
    active.content = false;
    active.limit = limit;
    active.requestLimit = request.limit;
    active.kind = kind;
    active.sizes = sizes;
    active.folders = std::move(under);
    active.requestId = lane.engine->submit(std::move(request));
}

void ToolHost::searchContent(const std::shared_ptr<Call>& call)
{
    Args args(call->arguments);
    const QString query = args.string(u"query"_s, true).trimmed();
    const int limit = args.integer(u"limit"_s, kDefaultContent, 1, call->limits().content);
    const int timeout = args.integer(u"timeout_ms"_s, kDefaultTimeoutMs, kMinTimeoutMs, kMaxTimeoutMs);
    const bool system = args.boolean(u"include_system"_s, false);
    const bool regex = args.boolean(u"regex"_s, false);
    QStringList folders = args.strings(u"folders"_s);
    if (!args.error().isEmpty())
        return finish(call, {{}, true, args.error()});
    if (query.isEmpty())
        return finish(call, {{}, true, u"query is empty"_s});
    // An expression that cannot be read finds nothing: said, so that the
    // caller writes it again rather than taking it that no file has it.
    if (const ContentPattern pattern = contentPattern(splitContentQuery(query, regex).words, regex); !pattern.error.isEmpty())
        return finish(call, {{}, true, u"The regular expression in the query cannot be read: "_s + pattern.errorReason});
    if (!m_policy.content)
        return finish(call, {{}, true, u"File content search is turned off in WinShun's settings"_s});
    // The timeout counts from the call's arrival: what it waited for the
    // engine is gone (call()).
    const qint64 left = timeout - call->timer.elapsed();
    if (left <= 0)
        return finish(call, waitedTooLong(call->timer));
    folders.removeIf([](const QString& folder) { return folder.trimmed().isEmpty(); });
    for (QString& folder : folders) {
        folder = folder.trimmed();
        if (excluded(folder))
            return finish(call, notFound(u"This folder is not visible to AI (excluded in Win顺's settings): "_s + folder));
    }
    // As search_files' (checkFolders): checked once the engine answered, or
    // without a search for one that is not a folder to read in.
    std::vector<FolderArg> under;
    if (!folders.isEmpty())
        under = lookUp(*m_services.index->index(), folders);
    if (answeredWithoutSearch(under, m_services.offline)) {
        return runAside(call, [under, offline = m_services.offline](AsideContext& aside) {
            return checkFolders(aside, under, offline).value_or(notFound(notInIndex(under.front().given)));
        });
    }
    folders.clear();
    for (const FolderArg& folder : under)
        folders.push_back(folder.path);

    SearchEngine::Request request;
    request.text = query;
    request.scope = Scope::Content;
    request.regex = regex;
    request.contentExtensions = m_policy.contentExtensions;
    request.contentSizeLimits = m_policy.contentSizeLimits;
    request.contentDocuments = m_policy.contentDocuments;
    request.contentInLowPriority = system || m_policy.contentInLowPriority;
    request.excludedTypes = m_policy.excludedTypes;
    request.excludedFileNames = m_policy.excludedFileNames;
    request.maxContentResults = limit + 10;
    request.contentThreads = kContentThreads;
    request.contentFolders = std::move(folders);
    Lane& lane = laneOf(*call->tool);
    Active& active = *lane.active;
    active.content = true;
    active.limit = limit;
    active.folders = std::move(under);
    active.requestId = lane.engine->submit(std::move(request));
    lane.timeout.start(std::chrono::milliseconds(left));
}

void ToolHost::recentFiles(const std::shared_ptr<Call>& call)
{
    Args args(call->arguments);
    const QString folder = args.string(u"folder"_s).trimmed();
    QStringList extensions = args.strings(u"extensions"_s);
    const int days = args.integer(u"days"_s, 0, 0, 36500);
    const QString kind = args.choice(u"kind"_s, u"files"_s, {u"files"_s, u"folders"_s, u"all"_s});
    const bool system = args.boolean(u"include_system"_s, false);
    const bool hidden = args.boolean(u"include_hidden"_s, false);
    const int limit = args.integer(u"limit"_s, kDefaultFiles, 1, call->limits().files);
    const bool sizes = args.boolean(u"size"_s, true);
    if (!args.error().isEmpty())
        return finish(call, {{}, true, args.error()});
    if (!folder.isEmpty() && excluded(folder))
        return finish(call, notFound(u"This folder is not visible to AI (excluded in Win顺's settings)."_s));
    std::vector<std::string> wanted; // extensions, lower case, without the dot
    for (QString ext : extensions) {
        while (ext.startsWith(u'.'))
            ext.remove(0, 1);
        if (!ext.isEmpty())
            wanted.push_back(ext.toLower().toStdString());
    }
    // The skipped file types, less those asked for by extension.
    std::vector<std::string> skipped;
    for (QString ext : m_policy.excludedTypes) {
        ext = ext.toLower();
        while (ext.startsWith(u'.'))
            ext.remove(0, 1);
        const std::string folded = ext.toStdString();
        if (!ext.isEmpty() && std::ranges::find(wanted, folded) == wanted.end())
            skipped.push_back(folded);
    }
    std::vector<std::string> skippedNames; // and file names, folded wildcards
    for (const QString& pattern : m_policy.excludedFileNames)
        skippedNames.push_back(text::foldAscii(pattern.toStdString()));

    // The scan of the index (millions of entries), and the checks and sizes
    // from the disk: aside.
    AsideWork work = [this, folder, wanted = std::move(wanted), skipped = std::move(skipped),
                         skippedNames = std::move(skippedNames), days, kind, system, hidden, limit,
                         shape = call->shape(sizes), cli = call->cli, excluded = m_excluded, offline = m_services.offline,
                         service = m_services.index](AsideContext& aside) -> ToolResult {
        QElapsedTimer timer;
        timer.start();
        struct Hit {
            EntryId id;
            FileTime modified;
        };
        struct Row {
            QString path;
            QString name;
            bool isDir;
            FileTime modified;
            std::int64_t size; // the index's (fileItem)
        };
        const auto index = service->index();
        if (!folder.isEmpty()) {
            if (std::optional<ToolResult> problem = checkFolders(aside, lookUp(*index, {folder}), offline))
                return *std::move(problem);
        }
        std::vector<Hit> hits;
        std::vector<Row> rows;
        bool cut = false; // more matched than were kept: some older than the last row
        {
            // One scan at a time, the pool taken before the index (it waits
            // for no other scan holding a lock); the index read for long
            // without holding up its searches (longReadLock).
            const std::lock_guard scan(m_scanMutex);
            const auto lock = index->longReadLock();
            EntryId under = kNoEntry;
            if (!folder.isEmpty()) {
                // Again: it may have gone since it was checked.
                under = index->findPath(indexPath(folder));
                if (under == kNoEntry || !index->entry(under).isDir())
                    return notFound(notInIndex(folder));
            }
            const FileTime since = days > 0
                ? static_cast<FileTime>(std::max<qint64>(1, QDateTime::currentSecsSinceEpoch() - static_cast<qint64>(days) * 86400))
                : 1;
            const std::size_t keep = static_cast<std::size_t>(limit) * 2; // some may be excluded afterwards
            std::vector<std::vector<Hit>> found(index->chunkCount());
            std::vector<char> overflowed(index->chunkCount(), 0); // a chunk had more than `keep`
            m_pool.parallelFor(index->chunkCount(), [&](std::size_t c) {
                if (aside.stopped())
                    return;
                std::unordered_map<EntryId, bool> below; // folder -> under `under`
                const auto isUnder = [&](EntryId id) {
                    if (under == kNoEntry)
                        return true;
                    std::vector<EntryId> walked;
                    EntryId at = index->entry(id).parent;
                    bool result = false;
                    while (at != kNoEntry) {
                        if (at == under) {
                            result = true;
                            break;
                        }
                        if (const auto it = below.find(at); it != below.end()) {
                            result = it->second;
                            break;
                        }
                        walked.push_back(at);
                        at = index->entry(at).parent;
                    }
                    for (const EntryId w : walked)
                        below.emplace(w, result);
                    return result;
                };
                std::vector<Hit>& mine = found[c];
                const auto byTime = [](const Hit& a, const Hit& b) { return a.modified > b.modified; }; // a min-heap on time
                const auto entries = index->chunk(c);
                const auto first = static_cast<EntryId>(c << FileIndex::kChunkBits);
                for (std::size_t i = 0; i < entries.size(); ++i) {
                    const Entry& e = entries[i];
                    if (e.isDeleted() || (e.flags & EntryFlag::Root))
                        continue;
                    if ((kind == u"files" && e.isDir()) || (kind == u"folders" && !e.isDir()))
                        continue;
                    if (!system && (e.flags & EntryFlag::LowPriority))
                        continue;
                    if (!hidden && (e.flags & EntryFlag::Hidden))
                        continue;
                    const auto id = static_cast<EntryId>(first + i);
                    const FileTime modified = index->modified(id);
                    if (modified < since)
                        continue;
                    // Older than the oldest of a full heap: not kept. Once
                    // one such was found to match, the rest need not be.
                    const bool older = mine.size() >= keep && modified <= mine.front().modified;
                    if (older && overflowed[c])
                        continue;
                    if (!wanted.empty()) {
                        if (e.isDir())
                            continue;
                        std::size_t extLength = 0;
                        const std::string_view name = index->fileName(e, extLength);
                        if (extLength == 0)
                            continue;
                        std::string ext(name.substr(name.size() - extLength));
                        std::ranges::transform(ext, ext.begin(), [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
                        if (std::ranges::find(wanted, ext) == wanted.end())
                            continue;
                    }
                    if (!skipped.empty() && !e.isDir()) { // the user's skipped file types
                        std::size_t extLength = 0;
                        const std::string_view name = index->fileName(e, extLength);
                        if (extLength > 0) {
                            std::string ext(name.substr(name.size() - extLength));
                            std::ranges::transform(ext, ext.begin(), [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
                            if (std::ranges::find(skipped, ext) != skipped.end())
                                continue;
                        }
                    }
                    if (!skippedNames.empty() && !e.isDir() && nameMatchesAny(index->name(e), skippedNames))
                        continue;
                    if (!isUnder(id))
                        continue;
                    if (older) {
                        overflowed[c] = 1;
                        continue;
                    }
                    mine.push_back({id, modified});
                    std::ranges::push_heap(mine, byTime);
                    if (mine.size() > keep) {
                        std::ranges::pop_heap(mine, byTime);
                        mine.pop_back();
                        overflowed[c] = 1;
                    }
                }
            });
            if (aside.stopped())
                return {};
            for (auto& f : found)
                hits.insert(hits.end(), f.begin(), f.end());
            cut = std::ranges::any_of(overflowed, [](char o) { return o != 0; });
            // The newest `keep` of them, newest first: with ws's limits each
            // chunk keeps nearly all it has, millions together, of which a
            // selection is linear where a sort of them all was not.
            const auto newer = [](const Hit& a, const Hit& b) { return a.modified > b.modified; };
            if (hits.size() > keep) {
                std::ranges::nth_element(hits, hits.begin() + static_cast<std::ptrdiff_t>(keep), newer);
                hits.resize(keep);
                cut = true;
            }
            std::ranges::sort(hits, newer);
            rows.reserve(hits.size());
            for (const Hit& hit : hits) {
                const Entry& e = index->entry(hit.id);
                rows.push_back({index->path(hit.id), wtf8::toQString(index->name(e)), e.isDir(), hit.modified, indexedSize(*index, hit.id)});
            }
        }
        // The access checks and the sizes go to the disk: not under the index's lock.
        Budget budget(cli);
        LocalTimes times;
        QJsonArray items;
        bool beyond = false; // one more to show than the limit takes
        const bool done = forEachVisible(
            aside, rows, [&](const Row& row) { return !offline(row.path) && !excludedIndexPath(excluded, row.path); },
            [&](const Row& row) {
                if (items.size() >= limit) {
                    beyond = true;
                    return false;
                }
                const QJsonObject item = fileItem(aside, times, row.path, row.name, row.isDir, row.modified, row.size, shape);
                if (!budget.take(item))
                    return false;
                items.push_back(item);
                return true;
            });
        if (!done)
            return {};
        QJsonObject out {{u"items"_s, items}, {u"returned"_s, items.size()}, {u"truncated"_s, beyond || budget.cut() || cut},
            {u"elapsed_ms"_s, static_cast<double>(timer.elapsed())}};
        if (service->state() != IndexService::State::Ready)
            out.insert(u"index_state"_s, stateName(service->state()));
        return {out};
    };
    runAside(call, std::move(work));
}

void ToolHost::listFolder(const std::shared_ptr<Call>& call)
{
    Args args(call->arguments);
    const QString path = args.string(u"path"_s, true).trimmed();
    const QString kind = args.choice(u"kind"_s, u"all"_s, {u"all"_s, u"files"_s, u"folders"_s});
    const int limit = args.integer(u"limit"_s, kDefaultFolder, 1, call->limits().folder);
    const bool sizes = args.boolean(u"size"_s, true);
    if (!args.error().isEmpty())
        return finish(call, {{}, true, args.error()});
    if (path.isEmpty())
        return finish(call, {{}, true, u"path is empty"_s});
    if (excluded(path))
        return finish(call, notFound(u"This folder is not visible to AI (excluded in Win顺's settings)."_s));

    // A large folder holds tens of thousands, each with its size from the
    // disk unless asked not to: aside.
    AsideWork work = [path, kind, limit, shape = call->shape(sizes), cli = call->cli, excluded = m_excluded,
                         offline = m_services.offline, service = m_services.index](AsideContext& aside) -> ToolResult {
        const auto index = service->index();
        // A folder the caller cannot list is, to it, not there: the same
        // answer as for one that is not in the index; nor is a file it
        // cannot see a file. Its entries' names are all visible once it can
        // be listed.
        if (std::optional<ToolResult> problem = checkFolders(aside, lookUp(*index, {path}), offline))
            return *std::move(problem);
        struct Child {
            QString name;
            bool isDir;
            FileTime modified;
            std::int64_t size; // the index's (fileItem)
        };
        std::vector<Child> children;
        QString folderPath;
        {
            // Hundreds of thousands, it may be: read for long without
            // holding up the index's searches.
            const auto lock = index->longReadLock();
            const EntryId folder = index->findPath(indexPath(path));
            if (folder == kNoEntry || !index->entry(folder).isDir())
                return notFound(notInIndex(path)); // gone since it was checked
            folderPath = index->path(folder);
            for (EntryId child = index->entry(folder).firstChild(); child != kNoEntry; child = index->entry(child).nextSibling) {
                const Entry& e = index->entry(child);
                if (e.isDeleted())
                    continue;
                if ((kind == u"files" && e.isDir()) || (kind == u"folders" && !e.isDir()))
                    continue;
                children.push_back({wtf8::toQString(index->name(e)), e.isDir(), index->modified(child), indexedSize(*index, child)});
            }
        }
        if (aside.stopped())
            return {};
        std::ranges::sort(children, [](const Child& a, const Child& b) {
            if (a.isDir != b.isDir)
                return a.isDir;
            return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
        });
        const QString prefix = folderPath.endsWith(u'\\') ? folderPath : folderPath + u'\\';
        Budget budget(cli);
        LocalTimes times;
        QJsonArray items;
        qint64 total = 0; // the entries the caller may see: not those in folders AI never sees
        for (const Child& child : children) {
            if (aside.stopped())
                return {};
            const QString childPath = prefix + child.name;
            if (excludedIndexPath(excluded, childPath))
                continue;
            ++total;
            if (items.size() >= limit || budget.cut())
                continue;
            const QJsonObject item = fileItem(aside, times, childPath, child.name, child.isDir, child.modified, child.size, shape);
            if (budget.take(item))
                items.push_back(item);
        }
        return {{{u"path"_s, folderPath}, {u"items"_s, items}, {u"returned"_s, items.size()}, {u"total"_s, total},
            {u"truncated"_s, total > items.size()}}};
    };
    runAside(call, std::move(work));
}

void ToolHost::fileInfo(const std::shared_ptr<Call>& call)
{
    Args args(call->arguments);
    const QString given = args.string(u"path"_s, true).trimmed();
    if (!args.error().isEmpty())
        return finish(call, {{}, true, args.error()});
    if (given.isEmpty())
        return finish(call, {{}, true, u"path is empty"_s});
    const QString path = nativePath(given);
    if (std::optional<ToolResult> problem = pathProblem(m_excluded, remoteRoots(), given, path))
        return finish(call, *std::move(problem));
    // The disk (a share: the network): aside.
    AsideWork work = [path, excluded = m_excluded, offline = m_services.offline, service = m_services.index](AsideContext& aside) -> ToolResult {
        const Opened opened = openGiven(aside, path, excluded, offline);
        QJsonObject out {{u"exists"_s, false}, {u"path"_s, path}};
        if (opened.reach == Reach::Unreachable)
            out.insert(u"error"_s, u"Its network share cannot be reached now"_s);
        else if (opened.reach == Reach::Error)
            out.insert(u"error"_s, u"Cannot be read (Win32 error %1)"_s.arg(opened.error));
        // What the caller could not see or read the attributes of is, to it,
        // not there: where the path led (a link's target) is what counts.
        if (opened.reach != Reach::Ok || !aside.sees(opened.seen) || !aside.canReadAttributes(opened.seen))
            return {out};
        const BY_HANDLE_FILE_INFORMATION& data = opened.info;
        const QString name = path.section(u'\\', -1);
        out = {{u"exists"_s, true}, {u"path"_s, path}, {u"name"_s, name}, {u"type"_s, opened.isDir() ? u"folder"_s : u"file"_s},
            {u"created"_s, isoFileTime(data.ftCreationTime)}, {u"modified"_s, isoFileTime(data.ftLastWriteTime)},
            {u"accessed"_s, isoFileTime(data.ftLastAccessTime)}, {u"hidden"_s, (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0},
            {u"read_only"_s, (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0},
            {u"system"_s, (data.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0},
            {u"cloud_placeholder"_s,
                (data.dwFileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) != 0}};
        if (!opened.isDir()) {
            out.insert(u"size"_s, static_cast<qint64>((static_cast<quint64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow));
            if (const qsizetype dot = name.lastIndexOf(u'.'); dot > 0)
                out.insert(u"extension"_s, name.mid(dot + 1).toLower());
        }
        {
            const auto index = service->index();
            const auto lock = index->readLock();
            out.insert(u"indexed"_s, index->findPath(indexPath(opened.path)) != kNoEntry);
        }
        return {out};
    };
    runAside(call, std::move(work));
}

void ToolHost::openItem(const std::shared_ptr<Call>& call, bool reveal)
{
    Args args(call->arguments);
    const QString given = args.string(u"path"_s, true).trimmed();
    if (!args.error().isEmpty())
        return finish(call, {{}, true, args.error()});
    if (given.isEmpty())
        return finish(call, {{}, true, u"path is empty"_s});
    const QString path = nativePath(given);
    if (std::optional<ToolResult> problem = pathProblem(m_excluded, remoteRoots(), given, path))
        return finish(call, *std::move(problem));
    // The checks go to the disk: aside. Only what the caller could open
    // itself; the rest is, to it, not there.
    AsideWork work = [path, excluded = m_excluded, offline = m_services.offline](AsideContext& aside) -> ToolResult {
        const Opened opened = openGiven(aside, path, excluded, offline);
        if (opened.reach != Reach::Ok || !aside.sees(opened.seen)
            || !(opened.isDir() ? aside.canList(opened.seen) : aside.canRead(opened.seen)))
            return notFound(u"There is no such file or folder: "_s + path);
        return {{{u"path"_s, opened.path}}};
    };
    m_others.push_back(call);
    aside(call, std::move(work), [this, call, reveal](ToolResult checked) {
        if (std::ranges::find(m_others, call) == m_others.end())
            return; // cancelled
        if (checked.isError) {
            std::erase(m_others, call);
            return finish(call, std::move(checked));
        }
        // What the path led to, as it was checked (a link's target, the long
        // names), is what opens.
        const QString target = checked.structured.value(u"path"_s).toString();
        const auto& action = reveal ? m_services.reveal : m_services.open;
        action(target, [this, call, target](bool ok) {
            // Any thread: back to ours.
            QMetaObject::invokeMethod(this, [this, call, target, ok] {
                std::erase(m_others, call);
                if (ok)
                    finish(call, {{{u"opened"_s, true}, {u"path"_s, target}}});
                else
                    finish(call, {{}, true, u"Could not open it: no Explorer to open it with the user's normal rights. The user can try again in a moment."_s});
            }, Qt::QueuedConnection);
        });
    }, false);
}

void ToolHost::clipboardHistory(const std::shared_ptr<Call>& call)
{
    Args args(call->arguments);
    ClipQuery query;
    query.query = args.string(u"query"_s).trimmed();
    query.kind = args.choice(u"kind"_s, u"all"_s, {u"all"_s, u"text"_s, u"link"_s, u"path"_s, u"files"_s, u"image"_s});
    query.limit = args.integer(u"limit"_s, kDefaultClips, 1, kMaxClips);
    query.maxChars = args.integer(u"max_chars"_s, kDefaultClipChars, 50, kMaxClipChars);
    if (!args.error().isEmpty())
        return finish(call, {{}, true, args.error()});
    const std::vector<ClipSummary> clips = m_services.clipboard(query);
    QJsonArray items;
    for (const ClipSummary& c : clips) {
        QJsonObject item {{u"id"_s, c.id}, {u"kind"_s, c.kind}, {u"copied"_s, isoTimeMs(c.created)}, {u"used"_s, isoTimeMs(c.used)},
            {u"pinned"_s, c.pinned}};
        if (c.kind != u"image") {
            item.insert(u"text"_s, c.text);
            item.insert(u"chars"_s, c.chars);
            item.insert(u"truncated"_s, c.chars > c.text.size());
        } else {
            item.insert(u"width"_s, c.width);
            item.insert(u"height"_s, c.height);
        }
        if (!c.source.isEmpty())
            item.insert(u"source"_s, c.source);
        if (!c.group.isEmpty())
            item.insert(u"group"_s, c.group);
        items.push_back(item);
    }
    finish(call, {{{u"items"_s, items}, {u"returned"_s, items.size()}}});
}

void ToolHost::clipboardGet(const std::shared_ptr<Call>& call)
{
    Args args(call->arguments);
    const QJsonValue id = call->arguments.value(u"id"_s);
    const int maxChars = args.integer(u"max_chars"_s, 20000, 100, 200000);
    if (!args.error().isEmpty())
        return finish(call, {{}, true, args.error()});
    if (!id.isDouble())
        return finish(call, {{}, true, u"id must be the integer id of an entry from clipboard_history"_s});
    const std::optional<QString> text = m_services.clipboardText(static_cast<qint64>(id.toDouble()));
    if (!text)
        return finish(call, {{}, true, u"No such clipboard entry (it may have expired)"_s});
    finish(call, {{{u"id"_s, id}, {u"text"_s, text->left(maxChars)}, {u"chars"_s, text->size()}, {u"truncated"_s, text->size() > maxChars}}});
}

QString ToolHost::tempFolder() const
{
    return !m_services.tempFolder.isEmpty() ? m_services.tempFolder : QDir::tempPath() + u"/WinShun/mcp"_s;
}

// The pictures are copied out of the history's folder, which a client has
// no business in: a PNG per entry in the temporary folder, by its id (the
// same picture asked for again is the same file).
void ToolHost::screenshotHistory(const std::shared_ptr<Call>& call)
{
    Args args(call->arguments);
    const QString query = args.string(u"query"_s).trimmed();
    const int limit = args.integer(u"limit"_s, kDefaultShots, 1, kMaxShots);
    if (!args.error().isEmpty())
        return finish(call, {{}, true, args.error()});
    if (!m_policy.captureEnabled) {
        return finish(call, {{{u"items"_s, QJsonArray()}, {u"returned"_s, 0},
            {u"reason"_s, u"Win顺's screen capture is turned off (Settings → 插件 → 截图), so there is no screenshot history to list."_s}}});
    }
    const QString folder = tempFolder();
    QDir().mkpath(folder);
    QJsonArray items;
    for (const ScreenshotSummary& shot : m_services.screenshots(query, limit)) {
        if (items.size() >= limit)
            break;
        QJsonObject item {{u"id"_s, shot.id}, {u"taken"_s, isoTimeMs(shot.created)}, {u"width"_s, shot.width},
            {u"height"_s, shot.height}, {u"text"_s, shot.ocr}};
        if (!shot.imagePath.isEmpty()) {
            const QFileInfo source(shot.imagePath);
            const QString copy = folder + u'/' + QString::number(shot.id) + u'.' + source.suffix();
            const QFileInfo there(copy);
            if (there.exists() && there.size() != source.size())
                QFile::remove(copy);
            if (QFileInfo::exists(copy) || QFile::copy(shot.imagePath, copy))
                item.insert(u"image_path"_s, QDir::toNativeSeparators(copy));
        }
        items.push_back(item);
    }
    finish(call, {{{u"items"_s, items}, {u"returned"_s, items.size()}}});
}

// Waits on the user (30 s at most, App::confirmScreenshot), among the calls
// that need no engine: cancelled meanwhile, the answer goes nowhere.
void ToolHost::takeScreenshot(const std::shared_ptr<Call>& call)
{
    Args args(call->arguments);
    const int monitor = args.integer(u"monitor"_s, 0, 0, 16);
    if (!args.error().isEmpty())
        return finish(call, {{}, true, args.error()});
    m_others.push_back(call);
    std::weak_ptr<Call> weak = call;
    QPointer<ToolHost> self(this);
    m_services.takeScreenshot({call->client, monitor}, [self, weak](ScreenshotTaken taken) {
        QMetaObject::invokeMethod(qApp, [self, weak, taken = std::move(taken)] {
            const std::shared_ptr<Call> c = weak.lock();
            if (!self || !c || std::ranges::find(self->m_others, c) == self->m_others.end())
                return; // cancelled
            std::erase(self->m_others, c);
            if (!taken.error.isEmpty())
                return self->finish(c, {{}, true, taken.error});
            self->finish(c, {{{u"path"_s, QDir::toNativeSeparators(taken.path)}, {u"width"_s, taken.width},
                {u"height"_s, taken.height}, {u"monitor"_s, taken.monitor}}});
        }, Qt::QueuedConnection);
    });
}

void ToolHost::readFileText(const std::shared_ptr<Call>& call)
{
    Args args(call->arguments);
    const QString given = args.string(u"path"_s, true).trimmed();
    const int offset = args.integer(u"offset"_s, 0, 0, std::numeric_limits<int>::max());
    const int maxChars = args.integer(u"max_chars"_s, kDefaultTextChars, kMinTextChars, call->limits().textChars);
    if (!args.error().isEmpty())
        return finish(call, {{}, true, args.error()});
    if (given.isEmpty())
        return finish(call, {{}, true, u"path is empty"_s});
    const QString path = nativePath(given);
    if (std::optional<ToolResult> problem = pathProblem(m_excluded, remoteRoots(), given, path))
        return finish(call, *std::move(problem));

    if (!m_extractor)
        m_extractor = std::make_unique<DocExtractor>(1, DocExtractor::Priority::Normal);
    filetext::Sources sources {m_services.index->index(), m_services.index->contentIndex(), m_extractor.get()};
    // The access checks, the disk and the extractor: aside.
    AsideWork work = [path, sources, sizeLimits = m_policy.contentSizeLimits, offset, maxChars, cli = call->cli, excluded = m_excluded,
                         offline = m_services.offline](AsideContext& aside) -> ToolResult {
        // Only what the caller could read itself; the rest is, to it, not
        // there. What the path led to is what is checked, and read: through
        // the handle that was checked.
        const Opened opened = openGiven(aside, path, excluded, offline);
        if (opened.reach != Reach::Ok || !aside.sees(opened.seen) || !aside.canRead(opened.seen))
            return notFound(u"There is no such file: "_s + path);
        const std::wstring final = opened.path.toStdWString();
        aside.quiet.at(opened.path);
        const filetext::Result read = filetext::read(opened.handle.get(), final, sources, sizeLimits.of(final), [&aside] { return aside.stopped(); });
        if (aside.stopped())
            return {};
        using filetext::Status;
        switch (read.status) {
        case Status::Ok:
            break;
        case Status::Missing:
            return notFound(u"There is no such file: "_s + path);
        case Status::Folder:
            return failure(u"This is a folder; list it with list_folder."_s);
        case Status::NotText:
            return failure(u"This is not a text file or a document Win顺 can read."_s);
        case Status::TooLarge:
            return failure(u"The file is larger than Win顺's size limit for its kind (Settings → content search)."_s);
        case Status::CloudOnly:
            return failure(u"This is a cloud file that is not on this PC; reading it would download it. The user can open it to download it first."_s);
        case Status::Unreadable:
            return failure(u"The file could not be read (in use, no access, or damaged)."_s);
        }
        const qsizetype chars = read.text.size();
        const qsizetype from = std::min<qsizetype>(offset, chars);
        qsizetype to = std::min<qsizetype>(from + maxChars, chars);
        if (to < chars && to > from && read.text.at(to - 1).isHighSurrogate())
            --to; // not half a character
        // No more than one answer holds (kAnswerBudget): the rest from next_offset.
        Budget budget(cli);
        budget.takeBytes(stringJsonBytes(path) + 1024); // the rest of the answer
        to = fitText(read.text, from, to, budget.left());
        QJsonObject out {{u"path"_s, path}, {u"kind"_s, read.document ? u"document"_s : u"text"_s},
            {u"text"_s, read.text.mid(from, to - from)}, {u"chars"_s, chars}, {u"offset"_s, from}, {u"cut"_s, read.cut}};
        if (to < chars)
            out.insert(u"next_offset"_s, to);
        if (chars == 0)
            out.insert(u"note"_s, read.document ? u"No text in it: a scanned document (pictures of pages), or only pictures."_s
                                                : u"The file is empty."_s);
        return {out};
    };
    runAside(call, std::move(work));
}

void ToolHost::listDocuments(const std::shared_ptr<Call>& call)
{
    Args args(call->arguments);
    const QStringList folderArgs = args.strings(u"folders"_s);
    const QStringList extensions = args.strings(u"extensions"_s);
    const bool system = args.boolean(u"include_system"_s, false);
    const int cursor = args.integer(u"cursor"_s, 0, 0, std::numeric_limits<int>::max());
    const int limit = args.integer(u"limit"_s, kDefaultDocuments, 1, kMaxDocuments);
    if (!args.error().isEmpty())
        return finish(call, {{}, true, args.error()});
    const auto content = m_services.index->contentIndex();
    if (!content || !m_services.index->contentIndexer()->enabled())
        return finish(call, {{}, true, u"The content index is off: the user can turn it on in Win顺's settings (文件内容)."_s});
    std::vector<QString> folders; // case-folded, with a trailing separator
    for (const QString& folder : folderArgs) {
        QString f = nativePath(folder).toCaseFolded();
        if (f.isEmpty())
            continue;
        if (!f.endsWith(u'\\'))
            f.append(u'\\');
        folders.push_back(f);
    }
    std::vector<std::string> wanted; // extensions, lower case, without the dot
    for (QString ext : extensions) {
        while (ext.startsWith(u'.'))
            ext.remove(0, 1);
        if (!ext.isEmpty())
            wanted.push_back(ext.toLower().toStdString());
    }

    // The index's lock and the access checks: aside.
    AsideWork work = [content, service = m_services.index, folders = std::move(folders), wanted = std::move(wanted), system, cursor,
                         limit, cli = call->cli, reading = m_services.index->readingContent(),
                         excluded = m_excluded](AsideContext& aside) -> ToolResult {
        struct Row {
            QString path;
            EntryId entry;
            std::uint32_t stamp;
            bool document;
        };
        std::vector<Row> rows;
        std::optional<EntryId> next;
        const std::vector<ContentIndex::DocInfo> documents = content->documents(); // ascending by entry
        {
            // All the documents, it may be, to find a page in some folders:
            // read for long without holding up the index's searches.
            const auto index = service->index();
            const auto lock = index->longReadLock();
            auto it = std::ranges::lower_bound(documents, static_cast<EntryId>(cursor), {}, &ContentIndex::DocInfo::entry);
            for (; it != documents.end(); ++it) {
                if (std::ssize(rows) >= limit) {
                    next = it->entry;
                    break;
                }
                // One being read again has no current text yet: it comes in a later listing.
                if (it->need != ContentIndex::Need::None || it->entry >= index->slotCount())
                    continue;
                const Entry& e = index->entry(it->entry);
                if (e.isDeleted() || e.isDir() || (!system && (e.flags & EntryFlag::LowPriority)))
                    continue;
                if (!wanted.empty()) {
                    std::size_t extLength = 0;
                    const std::string_view name = index->fileName(e, extLength);
                    if (extLength == 0)
                        continue;
                    std::string ext(name.substr(name.size() - extLength));
                    std::ranges::transform(ext, ext.begin(), [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
                    if (std::ranges::find(wanted, ext) == wanted.end())
                        continue;
                }
                QString path = index->path(it->entry);
                if (!folders.empty()) {
                    const QString folded = path.toCaseFolded();
                    if (std::ranges::none_of(folders, [&](const QString& f) { return folded.startsWith(f); }))
                        continue;
                }
                const bool document = isDocumentPath(path.toStdWString());
                rows.push_back({std::move(path), it->entry, it->stamp, document});
            }
        }
        // What the caller could not read is, to it, not there.
        Budget budget(cli);
        QJsonArray items;
        for (const Row& row : rows) {
            if (aside.stopped())
                return {};
            if (excludedIndexPath(excluded, row.path) || !aside.sees(row.path) || !aside.canRead(row.path))
                continue;
            const QJsonObject item {{u"path"_s, row.path}, {u"kind"_s, row.document ? u"document"_s : u"text"_s},
                {u"stamp"_s, static_cast<qint64>(row.stamp)}};
            if (!budget.take(item)) {
                next = row.entry; // the next page goes on from here
                break;
            }
            items.push_back(item);
        }
        QJsonObject out {{u"items"_s, items}, {u"returned"_s, items.size()}, {u"reading"_s, reading}};
        if (next)
            out.insert(u"next_cursor"_s, static_cast<qint64>(*next));
        return {out};
    };
    runAside(call, std::move(work));
}

void ToolHost::indexStatus(const std::shared_ptr<Call>& call)
{
    IndexService* service = m_services.index;
    QJsonObject index {{u"state"_s, stateName(service->state())}, {u"items"_s, static_cast<qint64>(service->itemCount())},
        {u"refreshing"_s, service->isRefreshing()}};
    QJsonArray volumes;
    const std::vector<IndexService::Reading> reading = service->readingVolumes();
    for (const std::wstring& root : service->volumeRoots()) {
        QJsonObject volume {{u"root"_s, QString::fromStdWString(root)}};
        const auto it = std::ranges::find_if(reading, [&](const IndexService::Reading& r) { return r.root == root; });
        volume.insert(u"reading"_s, it != reading.end());
        if (it != reading.end() && it->percent >= 0)
            volume.insert(u"percent"_s, it->percent);
        volumes.push_back(volume);
    }
    index.insert(u"volumes"_s, volumes);
    QJsonObject content;
    if (const auto contentIndex = service->contentIndex()) {
        const ContentIndex::Stats stats = contentIndex->stats();
        content.insert(u"enabled"_s, service->contentIndexer()->enabled());
        content.insert(u"files"_s, static_cast<qint64>(stats.documents));
        content.insert(u"pending"_s, static_cast<qint64>(stats.pending));
        content.insert(u"reading"_s, service->readingContent());
    } else {
        content.insert(u"enabled"_s, false);
    }
    const QJsonObject ai {{u"open_allowed"_s, m_policy.allowOpen && m_services.open != nullptr},
        // Allowed and there to read: with Win顺's clipboard history off there is none.
        {u"clipboard_allowed"_s, m_policy.allowClipboard && m_policy.clipboardHistory && m_services.clipboard != nullptr},
        {u"clipboard_history"_s, m_policy.clipboardHistory},
        {u"screenshot_allowed"_s, m_policy.allowScreenshot && m_services.takeScreenshot != nullptr},
        {u"capture_enabled"_s, m_policy.captureEnabled},
        {u"settings_mode"_s, QString::fromLatin1(settingsModeName(m_policy.settingsMode))},
        {u"excluded_folders"_s, static_cast<qint64>(m_excluded.size())}};
    QJsonObject out {{u"running"_s, true}, {u"connected"_s, true}, {u"version"_s, m_services.version}, {u"index"_s, index},
        {u"content_index"_s, content}, {u"ai"_s, ai}};
    if (m_policy.allowDocuments) {
        // For a plugin that works in the background (winshun.semantic with a
        // model on this computer): whether the user is at the computer.
        LASTINPUTINFO input {sizeof input, 0};
        if (::GetLastInputInfo(&input))
            out.insert(u"user_idle_seconds"_s, static_cast<qint64>((::GetTickCount() - input.dwTime) / 1000));
    }
    finish(call, {out});
}

// ---- the settings tools ------------------------------------------------------------------

QString cleanShownText(const QString& text, qsizetype maxChars)
{
    QString plain;
    plain.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        char32_t c = text[i].unicode();
        const bool pair = text[i].isHighSurrogate() && i + 1 < text.size() && text[i + 1].isLowSurrogate();
        if (pair)
            c = QChar::surrogateToUcs4(text[i], text[i + 1]);
        const QChar::Category category = QChar::category(c);
        if (QChar::isSpace(c)) { // line breaks, tabs, U+2028 and U+2029 too: one space
            plain.push_back(u' ');
        } else if (category != QChar::Other_Control && category != QChar::Other_Format && category != QChar::Other_Surrogate) {
            plain.push_back(text[i]);
            if (pair)
                plain.push_back(text[i + 1]);
        }
        if (pair)
            ++i;
    }
    plain = plain.simplified();
    if (plain.size() > maxChars) {
        qsizetype cut = maxChars - 1;
        if (cut > 0 && plain[cut - 1].isHighSurrogate())
            --cut; // not between the halves of one character
        plain = plain.left(cut) + u"…"_s;
    }
    return plain;
}

void ToolHost::getSettings(const std::shared_ptr<Call>& call)
{
    Args args(call->arguments);
    SettingsQuery query;
    query.sections = args.strings(u"sections"_s);
    query.keys = args.strings(u"keys"_s);
    query.words = args.string(u"query"_s).trimmed();
    query.changedOnly = args.boolean(u"changed_only"_s, false);
    query.facts = args.boolean(u"facts"_s, false);
    if (!args.error().isEmpty())
        return finish(call, {{}, true, args.error()});
    ToolResult result = m_services.getSettings(query);
    if (!result.isError) {
        result.structured.insert(u"ai"_s, QJsonObject {{u"settings_mode"_s, QString::fromLatin1(settingsModeName(m_policy.settingsMode))},
                                              {u"change_settings_offered"_s, known(u"change_settings"_s) == Known::Yes}});
    }
    finish(call, std::move(result));
}

// Plans the changes (all of them or none), asks the user unless the mode
// is auto and nothing asks always, then applies them from a queued
// invocation, never inside this call. A call is waiting among the others
// while the card is up: cancelled meanwhile, the card goes and the answer
// goes nowhere.
void ToolHost::changeSettings(const std::shared_ptr<Call>& call)
{
    Args args(call->arguments);
    const QString reason = cleanShownText(args.string(u"reason"_s, true), kMaxReasonChars);
    if (!args.error().isEmpty())
        return finish(call, {{}, true, args.error()});
    if (reason.isEmpty())
        return finish(call, {{}, true, u"reason must say in a sentence why these changes are made: the user reads it."_s});
    const QJsonValue changes = call->arguments.value(u"changes"_s);
    if (!changes.isArray() || changes.toArray().isEmpty() || changes.toArray().size() > kMaxSettingChanges)
        return finish(call, {{}, true, u"changes must be an array of 1 to %1 objects {key, value | add | remove}."_s.arg(kMaxSettingChanges)});

    const SettingsPlan plan = m_services.planSettings(changes.toArray());
    if (!plan.problems.isEmpty()) {
        return finish(call, {{}, true,
            u"Nothing was changed. These problems must be fixed first:\n- "_s + plan.problems.join(u"\n- "_s)});
    }
    QJsonArray shown;
    QJsonArray unchanged;
    SettingsApply apply;
    bool askAlways = false;
    for (const SettingsPlanItem& item : plan.items) {
        if (item.changed) {
            shown.push_back(item.shown);
            apply.approved.push_back({item.key, item.newValue});
            askAlways = askAlways || item.askAlways;
        } else {
            unchanged.push_back(item.key);
        }
    }
    if (shown.isEmpty()) {
        return finish(call, {{{u"changed"_s, 0}, {u"unchanged"_s, unchanged}, {u"settings"_s, QJsonArray()},
            {u"explorer_restart_needed"_s, false}, {u"note"_s, u"Every setting was set so already: nothing changed."_s}}});
    }

    const QString client = cleanShownText(call->client, kMaxClientChars);
    apply.client = client;
    apply.reason = reason;
    apply.changes = changes.toArray();
    QPointer<ToolHost> self(this);
    std::weak_ptr<Call> weak = call;
    if (m_policy.settingsMode == SettingsMode::Auto && !askAlways) {
        m_others.push_back(call);
        QMetaObject::invokeMethod(this, [self, weak, apply = std::move(apply)]() mutable {
            const std::shared_ptr<Call> c = weak.lock();
            if (self && c && std::ranges::find(self->m_others, c) != self->m_others.end())
                self->applySettings(c, std::move(apply));
        }, Qt::QueuedConnection);
        return;
    }
    if (std::ranges::any_of(m_others, [](const std::shared_ptr<Call>& c) { return c->settingsCard; }))
        return finish(call, {{}, true, u"Another settings change is waiting for the user's answer; try again later."_s});
    m_others.push_back(call);
    call->settingsCard = true;
    m_services.confirmSettings({client, reason, shown, askAlways},
        [self, weak, apply = std::move(apply), askAlways](SettingsAnswer answer, bool always) mutable {
            QMetaObject::invokeMethod(qApp, [self, weak, apply = std::move(apply), askAlways, answer, always]() mutable {
                const std::shared_ptr<Call> c = weak.lock();
                if (!self || !c || std::ranges::find(self->m_others, c) == self->m_others.end())
                    return; // cancelled
                c->settingsCard = false;
                if (answer == SettingsAnswer::Allowed) {
                    // Only a card with nothing that asks always has the box.
                    apply.asked = true;
                    apply.alwaysAllow = always && !askAlways;
                    self->applySettings(c, std::move(apply));
                    return;
                }
                std::erase(self->m_others, c);
                QString why;
                switch (answer) {
                case SettingsAnswer::Refused:
                    why = u"The user declined the change. Nothing was changed; do not ask again unless the user wants it."_s;
                    break;
                case SettingsAnswer::Timeout:
                    why = u"The user did not answer within 60 seconds, so nothing was changed."_s;
                    break;
                case SettingsAnswer::Busy:
                    why = u"Another settings request is waiting for the user's answer; try again once it is answered."_s;
                    break;
                case SettingsAnswer::Unavailable:
                case SettingsAnswer::Allowed:
                    why = u"Win顺 could not ask the user, so nothing was changed."_s;
                    break;
                }
                self->finish(c, {{}, true, why});
            }, Qt::QueuedConnection);
        });
}

// Runs in a queued invocation: the app applies and answers; the answer comes
// back through another, so that the reply is sent after the apply returned.
void ToolHost::applySettings(const std::shared_ptr<Call>& call, SettingsApply apply)
{
    QPointer<ToolHost> self(this);
    std::weak_ptr<Call> weak = call;
    m_services.applySettings(apply, [self, weak](ToolResult result) {
        QMetaObject::invokeMethod(qApp, [self, weak, result = std::move(result)]() mutable {
            const std::shared_ptr<Call> c = weak.lock();
            if (!self || !c || std::ranges::find(self->m_others, c) == self->m_others.end())
                return; // cancelled
            std::erase(self->m_others, c);
            self->finish(c, std::move(result));
        }, Qt::QueuedConnection);
    });
}

// The app answers when it will (a package of the library is fetched first);
// the call waits among the others so that cancel() finds it.
void ToolHost::pluginCommand(const std::shared_ptr<Call>& call)
{
    m_others.push_back(call);
    QPointer<ToolHost> self(this);
    std::weak_ptr<Call> weak = call;
    m_services.plugins(call->tool->name, call->arguments, [self, weak](ToolResult result) {
        QMetaObject::invokeMethod(qApp, [self, weak, result = std::move(result)]() mutable {
            const std::shared_ptr<Call> c = weak.lock();
            if (!self || !c || std::ranges::find(self->m_others, c) == self->m_others.end())
                return; // cancelled
            std::erase(self->m_others, c);
            self->finish(c, std::move(result));
        }, Qt::QueuedConnection);
    });
}

void ToolHost::openSettings(const std::shared_ptr<Call>& call)
{
    Args args(call->arguments);
    const QString key = args.string(u"key"_s).trimmed();
    const QString section = args.string(u"section"_s).trimmed();
    if (!args.error().isEmpty())
        return finish(call, {{}, true, args.error()});
    if (key.isEmpty() && section.isEmpty())
        return finish(call, {{}, true, u"Give a key (from get_settings) or a section id."_s});
    finish(call, m_services.openSettings(key, section));
}

} // namespace ws::mcp
