#pragma once

#include "FileIndex.h"
#include "WorkerPool.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Everything's own way of being asked (its IPC: a window that takes
// WM_COPYDATA queries and answers with lists), spoken by Win顺 for the
// programs that know no other: Directory Opus, Total Commander, es.exe and
// whatever uses the Everything SDK (GitHub #40). What is here is the
// protocol and the search, with no window: what a query's bytes say, the
// search it stands for in Win顺's own words, the rows found in the index,
// and the bytes of the answer. The window is the app's
// (platform/EverythingServer).
//
// Not all of Everything: its query is read as far as Win顺's own search goes
// (words, "quoted words", !not, a | b, wildcards, a folder's path to look
// in, the start of a full path, "D:\images\*.jpg", ext:, size:, dm:,
// regex:, file: / folder:, parent:, and root: alone, which lists the
// drives, as Directory Opus asks first). A query that asks
// for more (attrib:, content:, dupe:, <groups> of words…) finds nothing
// rather than something else. Match case and whole word are not honoured (more is found,
// never less); what cannot be known from the index (date accessed, run
// counts) is answered as unknown. Sizes past 16 MB are within a
// hundred-thousandth (FileSize).
namespace ws::everything {

inline constexpr wchar_t kWindowClass[] = L"EVERYTHING_TASKBAR_NOTIFICATION";
// The version answered: the last Everything 1.4, whose IPC this is.
inline constexpr std::uint32_t kVersion[] = {1, 4, 1, 1026};

// WM_COPYDATA's dwData.
namespace copydata {
inline constexpr std::uint32_t QueryA = 1;
inline constexpr std::uint32_t QueryW = 2;
inline constexpr std::uint32_t Query2A = 17;
inline constexpr std::uint32_t Query2W = 18;
} // namespace copydata

// EVERYTHING_WM_IPC (WM_USER) commands, in wParam.
namespace command {
inline constexpr std::uint32_t MajorVersion = 0;
inline constexpr std::uint32_t MinorVersion = 1;
inline constexpr std::uint32_t Revision = 2;
inline constexpr std::uint32_t BuildNumber = 3;
inline constexpr std::uint32_t TargetMachine = 5;
inline constexpr std::uint32_t IsNtfsDriveIndexed = 400;
inline constexpr std::uint32_t IsDbLoaded = 401;
inline constexpr std::uint32_t IsFastSort = 410;
inline constexpr std::uint32_t IsFileInfoIndexed = 411;
} // namespace command

namespace searchflag {
inline constexpr std::uint32_t Regex = 0x8;
} // namespace searchflag

namespace request {
inline constexpr std::uint32_t Name = 0x1;
inline constexpr std::uint32_t Path = 0x2;
inline constexpr std::uint32_t FullPath = 0x4;
inline constexpr std::uint32_t Extension = 0x8;
inline constexpr std::uint32_t Size = 0x10;
inline constexpr std::uint32_t Created = 0x20;
inline constexpr std::uint32_t Modified = 0x40;
inline constexpr std::uint32_t Accessed = 0x80;
inline constexpr std::uint32_t Attributes = 0x100;
inline constexpr std::uint32_t FileListName = 0x200;
inline constexpr std::uint32_t RunCount = 0x400;
inline constexpr std::uint32_t DateRun = 0x800;
inline constexpr std::uint32_t RecentlyChanged = 0x1000;
inline constexpr std::uint32_t HighlightedName = 0x2000;
inline constexpr std::uint32_t HighlightedPath = 0x4000;
inline constexpr std::uint32_t HighlightedFullPath = 0x8000;
} // namespace request

// EVERYTHING_IPC_SORT_*: ascending is odd, descending the next.
namespace sort {
inline constexpr std::uint32_t Name = 1;
inline constexpr std::uint32_t Path = 3;
inline constexpr std::uint32_t Size = 5;
inline constexpr std::uint32_t Extension = 7;
inline constexpr std::uint32_t TypeName = 9;
inline constexpr std::uint32_t Created = 11;
inline constexpr std::uint32_t Modified = 13;
} // namespace sort

// EVERYTHING_IPC_FILE_INFO_*: what IsFileInfoIndexed asks about.
namespace fileinfo {
inline constexpr std::uint32_t FileSize = 1;
inline constexpr std::uint32_t FolderSize = 2;
inline constexpr std::uint32_t Created = 3;
inline constexpr std::uint32_t Modified = 4;
inline constexpr std::uint32_t Attributes = 6;
} // namespace fileinfo

// A value the index does not know (a date never read, a folder too large to
// add up in time), as Everything writes it.
inline constexpr std::uint64_t kUnknown = 0xFFFF'FFFF'FFFF'FFFFull;
// The most rows one query is answered with, whatever it asks for: a list of
// every file on the disks is gigabytes (the totals still say how many).
inline constexpr std::size_t kMostRows = 200'000;

struct Query {
    int version = 1; // 1: EVERYTHING_IPC_QUERY, 2: EVERYTHING_IPC_QUERY2
    bool wide = true; // UTF-16; else the system's ANSI code page, both ways
    std::uint32_t replyWindow = 0;
    std::uint32_t replyMessage = 0;
    std::uint32_t searchFlags = 0;
    std::uint32_t offset = 0;
    std::uint32_t maxResults = 0xFFFF'FFFFu;
    std::uint32_t requestFlags = request::Name | request::Path;
    std::uint32_t sortType = sort::Name;
    QString search;
};
// A query as WM_COPYDATA brought it; none for another kind of message or
// bytes that are no query.
std::optional<Query> readQuery(std::uint32_t kind, std::span<const std::byte> data);

enum class Kind : std::uint8_t { Any, Files, Folders };
// An Everything search in Win顺's words.
struct Search {
    QString query; // for parseQuery
    bool regex = false; // ... with every term a regular expression
    Kind kind = Kind::Any;
    QStringList folders; // only under one of these ("C:\Work", "C:"); none: anywhere
    // Only what a full path starts with (any case): a path from a drive or a
    // share that does not end in \ ("D:\images": D:\images and all in it,
    // and D:\images2 too, as Everything compares it, GitHub #399).
    QStringList pathStarts;
    QString directIn; // only what lies right in this folder (parent:)
    bool roots = false; // root: alone: the drives and shares themselves
    std::optional<std::uint32_t> count; // count:
    QString unsupported; // what of it Win顺 cannot do; the search then finds nothing
    friend bool operator==(const Search&, const Search&) = default;
};
Search translate(const QString& text, std::uint32_t searchFlags = 0);

struct Row {
    std::wstring name;
    std::wstring path; // the folder it is in, no backslash at the end; empty for a drive
    bool folder = false;
    bool root = false;
    bool hidden = false;
    std::uint64_t size = kUnknown;
    std::uint64_t created = kUnknown; // FILETIME ticks
    std::uint64_t modified = kUnknown;
};
struct Rows {
    std::vector<Row> rows; // the window asked for (offset, maxResults), in order
    std::uint32_t totalItems = 0; // all that match
    std::uint32_t totalFolders = 0;
    std::uint32_t totalFiles = 0;
};
// The query's answer from the index (takes its read lock). Empty when
// cancelled.
Rows find(const FileIndex& index, WorkerPool& pool, const Query& query, const std::function<bool()>& cancelled = {});
// The bytes of the answer: EVERYTHING_IPC_LIST or LIST2, as the query asked.
QByteArray encode(const Query& query, const Rows& rows);

// What the window answers to an EVERYTHING_WM_IPC command; 0 for all it does
// not do (exit, rebuild, the options of Everything's own window).
std::uint32_t answerCommand(std::uint32_t command, std::uint32_t argument, bool indexReady) noexcept;

} // namespace ws::everything
