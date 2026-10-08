// Loads the saved index and reports memory use and search timings.
//
//   wsbench [snapshot] [query ...]
//   wsbench --mft      (as administrator) reads every NTFS volume's MFT,
//                      compares the result with a directory walk, and tests
//                      following the change journal on the build folder's volume
//   wsbench [snapshot] --content [--sample N] [--needle text] [--no-throughput]
//                      what a content search costs: its candidates, and for a
//                      random sample of them the time to open, read, close and
//                      scan each file (first touch, then again from the cache),
//                      reading past the cache, and throughput by thread count.
//                      The sample is the same on every run (fixed seed), so a
//                      later run shows what the caches kept in the meantime
//   wsbench [snapshot] --content-index [--all] [--threads N] [--sample N] [--dir D] [--keep] [--verify]
//                      builds the content index of those files (without system
//                      and program folders unless --all) and reports its size,
//                      how fast it was read, and lookup times; --verify reads
//                      every file a lookup rules out, to show none is missed
//   wsbench --places [query ...]
//                      reads the places (Settings, Control Panel, places.txt)
//                      as the app does and lists them, or the best places for
//                      each query; reports keyword blocks of places.txt that
//                      match no place here
//
// Default snapshot: %LOCALAPPDATA%\WinShun\index.bin
#include "ContentIndex.h"
#include "ContentIndexer.h"
#include "ContentScanner.h"
#include "Crawler.h"
#include "NameSearch.h"
#include "Ntfs.h"
#include "NtfsIndexer.h"
#include "Query.h"
#include "Settings.h"
#include "Snapshot.h"
#include "SystemCatalog.h"
#include "TextUtil.h"
#include "Win32Util.h"
#include "Wtf8.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <random>
#include <thread>
#include <unordered_map>

using namespace Qt::StringLiterals;

namespace {

double privateMB()
{
    PROCESS_MEMORY_COUNTERS_EX pmc {};
    ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
    return static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
}

double peakCommitMB()
{
    PROCESS_MEMORY_COUNTERS_EX pmc {};
    ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
    return static_cast<double>(pmc.PeakPagefileUsage) / (1024.0 * 1024.0);
}

double msSince(const QElapsedTimer& t)
{
    return static_cast<double>(t.nsecsElapsed()) / 1e6;
}

std::string narrow(std::u16string_view s)
{
    return ws::wtf8::fromUtf16(s);
}

int crawlThreads()
{
    return std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 2, 8);
}

// Builds one volume's index from its MFT, as the app does.
bool buildFromMft(ws::FileIndex& index, const ws::Crawler& crawler, const std::wstring& root, double* readMs, double* syncMs)
{
    const ws::EntryId r = index.addRoot(narrow(ws::wtf8::view(root)));
    index.setFolderRecord(r, ws::ntfs::kRootRecord, r);
    index.setInterning(true);
    QElapsedTimer t;
    t.start();
    ws::MftTree tree;
    std::wstring error;
    if (!tree.read(root, index, {}, &error)) {
        std::printf("  MFT read failed: %s\n", narrow(ws::wtf8::view(error)).c_str());
        return false;
    }
    *readMs = msSince(t);
    t.restart();
    crawler.sync(index, {{r, root, 0, r, ws::ntfs::kRootRecord}}, 1, false, {},
        [&](const ws::Crawler::Root& dir, ws::DirListing& out) { return tree.list(index, crawler, dir, out); });
    index.setInterning(false);
    index.compact(true);
    *syncMs = msSince(t);
    return true;
}

// Every live entry by path, with the flags that matter for search.
std::unordered_map<std::u16string, std::uint8_t> pathsOf(const ws::FileIndex& index)
{
    constexpr std::uint8_t kCompared = ws::EntryFlag::Directory | ws::EntryFlag::Hidden | ws::EntryFlag::LowPriority
        | ws::EntryFlag::Offline;
    std::unordered_map<std::u16string, std::uint8_t> out;
    out.reserve(index.liveCount());
    for (std::size_t i = 0; i < index.slotCount(); ++i) {
        const auto id = static_cast<ws::EntryId>(i);
        if (!index.entry(id).isDeleted())
            out.emplace(index.path16(id), static_cast<std::uint8_t>(index.entry(id).flags & kCompared));
    }
    return out;
}

// The first `depth` components of a path ("C:\Windows\System32").
std::u16string prefixOf(const std::u16string& path, int depth)
{
    std::size_t pos = 0;
    for (int i = 0; i < depth; ++i) {
        pos = path.find(u'\\', pos + 1);
        if (pos == std::u16string::npos)
            return path;
    }
    return path.substr(0, pos);
}

void reportOnly(const char* label, const std::vector<std::u16string>& paths)
{
    std::printf("  only %-5s %zu\n", label, paths.size());
    std::map<std::u16string, std::size_t> groups;
    for (const auto& p : paths)
        ++groups[prefixOf(p, 3)];
    std::vector<std::pair<std::size_t, std::u16string>> top;
    for (const auto& [g, n] : groups)
        top.emplace_back(n, g);
    std::sort(top.rbegin(), top.rend());
    for (std::size_t i = 0; i < top.size() && i < 12; ++i)
        std::printf("      %7zu  %s\\...\n", top[i].first, narrow(top[i].second).c_str());
    for (std::size_t i = 0; i < paths.size() && i < 8; ++i)
        std::printf("      e.g. %s\n", narrow(paths[i]).c_str());
}

// Compares an index built from the MFT with one built by walking the disk.
void compareIndexes(const ws::FileIndex& mft, const ws::FileIndex& walked)
{
    const auto a = pathsOf(mft);
    const auto b = pathsOf(walked);
    std::vector<std::u16string> onlyMft, onlyWalk, flagged;
    std::map<int, std::size_t> flagKinds;
    for (const auto& [path, flags] : a) {
        const auto it = b.find(path);
        if (it == b.end()) {
            onlyMft.push_back(path);
        } else if (it->second != flags) {
            flagged.push_back(path);
            ++flagKinds[(flags ^ it->second)];
        }
    }
    for (const auto& [path, flags] : b) {
        if (!a.contains(path))
            onlyWalk.push_back(path);
    }
    std::sort(onlyMft.begin(), onlyMft.end());
    std::sort(onlyWalk.begin(), onlyWalk.end());
    std::sort(flagged.begin(), flagged.end());
    std::printf("  same %zu of %zu (MFT) / %zu (walk)\n", a.size() - onlyMft.size(), a.size(), b.size());
    reportOnly("MFT", onlyMft);
    reportOnly("walk", onlyWalk);
    std::printf("  flags differ %zu  (xor:count", flagged.size());
    for (const auto& [kind, n] : flagKinds)
        std::printf(" 0x%02X:%zu", kind, n);
    std::printf(")\n");
    for (std::size_t i = 0; i < flagged.size() && i < 8; ++i)
        std::printf("      e.g. %s  mft 0x%02X walk 0x%02X\n", narrow(flagged[i]).c_str(), a.at(flagged[i]),
            b.at(flagged[i]));
}

struct OwnedRecord {
    ws::ntfs::UsnRecord record;
    std::u16string name;
};

// Reads every journal record from `usn` up to now.
std::vector<OwnedRecord> readJournal(const std::wstring& root, const ws::ntfs::JournalInfo& journal, std::int64_t usn)
{
    std::vector<OwnedRecord> out;
    ws::ntfs::JournalReader reader(root, journal.id);
    std::vector<ws::ntfs::UsnRecord> records;
    for (;;) {
        reader.start(usn, false);
        std::int64_t next = 0;
        if (!reader.finish(records, next)) {
            std::printf("  journal read failed (error %lu)\n", ::GetLastError());
            break;
        }
        for (const auto& r : records)
            out.push_back({r, std::u16string(r.name)});
        if (next == usn)
            break;
        usn = next;
    }
    for (auto& o : out)
        o.record.name = o.name;
    return out;
}

void applyAll(ws::UsnApplier& applier, const std::vector<OwnedRecord>& owned)
{
    std::vector<ws::ntfs::UsnRecord> records;
    for (const auto& o : owned)
        records.push_back(o.record);
    applier.apply(records, {});
}

// Paths below `dir` in `index`, relative to it, with flags.
std::map<std::u16string, std::uint8_t> subtree(const ws::FileIndex& index, ws::EntryId dir)
{
    std::map<std::u16string, std::uint8_t> out;
    std::u16string base = index.path16(dir);
    if (!base.ends_with(u'\\'))
        base.push_back(u'\\'); // a root's path already ends in one
    std::vector<ws::EntryId> stack {dir};
    while (!stack.empty()) {
        const ws::EntryId id = stack.back();
        stack.pop_back();
        for (ws::EntryId c = index.entry(id).firstChild; c != ws::kNoEntry; c = index.entry(c).nextSibling) {
            out.emplace(index.path16(c).substr(base.size()),
                static_cast<std::uint8_t>(index.entry(c).flags & (ws::EntryFlag::Directory | ws::EntryFlag::Hidden)));
            stack.push_back(c);
        }
    }
    return out;
}

bool sameTree(const char* label, const std::map<std::u16string, std::uint8_t>& got,
    const std::map<std::u16string, std::uint8_t>& want)
{
    if (got == want) {
        std::printf("  %-28s OK (%zu items)\n", label, want.size());
        return true;
    }
    std::printf("  %-28s DIFFERS\n", label);
    for (const auto& [p, f] : want) {
        const auto it = got.find(p);
        if (it == got.end())
            std::printf("      missing  %s\n", narrow(p).c_str());
        else if (it->second != f)
            std::printf("      flags    %s  got 0x%02X want 0x%02X\n", narrow(p).c_str(), it->second, f);
    }
    for (const auto& [p, f] : got) {
        if (!want.contains(p))
            std::printf("      extra    %s\n", narrow(p).c_str());
    }
    return false;
}

// Note: the folder of the test is still there while this runs.
std::uint32_t recordOfPath(const std::filesystem::path& p)
{
    const HANDLE h = ::CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    BY_HANDLE_FILE_INFORMATION info {};
    const bool ok = h != INVALID_HANDLE_VALUE && ::GetFileInformationByHandle(h, &info);
    if (h != INVALID_HANDLE_VALUE)
        ::CloseHandle(h);
    return ok ? ws::ntfs::recordOf((std::uint64_t {info.nFileIndexHigh} << 32) | info.nFileIndexLow) : 0;
}

void touch(const std::filesystem::path& p)
{
    const HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE)
        ::CloseHandle(h);
}

// Makes real changes in a scratch folder, then checks that applying the
// journal (and replaying it) gives the same tree as walking the folder.
void journalSelfTest(const std::wstring& root, const std::filesystem::path& base, ws::CrawlRules rules)
{
    namespace fs = std::filesystem;
    std::printf("\nchange journal self-test in %s\n", narrow(ws::wtf8::view(base.wstring())).c_str());
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base / L"keep", ec);
    fs::create_directories(base / L"old", ec);
    fs::create_directories(base / L"ws-excluded" / L"incoming" / L"deep", ec);
    touch(base / L"keep" / L"k.txt");
    touch(base / L"old" / L"o.txt");
    touch(base / L"ws-excluded" / L"incoming" / L"deep" / L"x.txt");
    rules.excludedNames.push_back(L"ws-excluded");
    const ws::Crawler crawler(rules);

    ws::FileIndex index;
    double readMs = 0, syncMs = 0;
    if (!buildFromMft(index, crawler, root, &readMs, &syncMs))
        return;
    const auto journal = ws::ntfs::queryJournal(root);
    if (!journal) {
        std::printf("  no change journal\n");
        return;
    }
    const std::int64_t start = journal->nextUsn;

    // The changes.
    fs::create_directories(base / L"new", ec);
    touch(base / L"new" / L"a.txt");
    touch(base / L"new" / L"b.txt");
    fs::rename(base / L"new" / L"a.txt", base / L"new" / L"A2.txt", ec); // rename
    fs::rename(base / L"new" / L"b.txt", base / L"keep" / L"b.txt", ec); // move
    fs::rename(base / L"old", base / L"Old2", ec); // folder rename, with its file
    fs::rename(base / L"keep" / L"k.txt", base / L"keep" / L"K.TXT", ec); // case only
    ::CreateHardLinkW((base / L"keep" / L"hl.txt").c_str(), (base / L"new" / L"A2.txt").c_str(), nullptr);
    ::CreateHardLinkW((base / L"new" / L"hl2.txt").c_str(), (base / L"new" / L"A2.txt").c_str(), nullptr);
    fs::remove(base / L"new" / L"hl2.txt", ec); // one link of several
    fs::remove(base / L"new" / L"A2.txt", ec); // hl.txt keeps the file
    ::SetFileAttributesW((base / L"keep" / L"b.txt").c_str(), FILE_ATTRIBUTE_HIDDEN);
    fs::rename(base / L"Old2", base / L"ws-excluded" / L"Old2", ec); // moved out of the index
    fs::rename(base / L"ws-excluded" / L"incoming", base / L"incoming", ec); // moved in: walked
    touch(base / L"incoming" / L"deep" / L"y.txt"); // inside a folder only the walk registered
    fs::create_directories(base / L"gone" / L"sub", ec);
    touch(base / L"gone" / L"sub" / L"z.txt");
    fs::remove_all(base / L"gone", ec); // a whole tree

    QElapsedTimer t;
    t.start();
    const auto records = readJournal(root, *journal, start);
    const double readJournalMs = msSince(t);
    t.restart();
    ws::UsnApplier applier(index, crawler, narrow(ws::wtf8::view(root)));
    applyAll(applier, records);
    std::printf("  %zu records read in %.1f ms, applied in %.1f ms\n", records.size(), readJournalMs, msSince(t));

    // What the folder really holds now.
    ws::FileIndex walked;
    const ws::EntryId wroot = walked.addRoot(narrow(ws::wtf8::view(base.wstring())));
    crawler.sync(walked, {{wroot, base.wstring(), 0}}, 1, false, {});
    const auto want = subtree(walked, wroot);
    const ws::EntryId dir = index.findPath(base.wstring());
    if (dir == ws::kNoEntry) {
        std::printf("  test folder missing from the index\n");
    } else {
        sameTree("journal applied", subtree(index, dir), want);
        ws::UsnApplier replay(index, crawler, narrow(ws::wtf8::view(root)));
        applyAll(replay, records); // as after a restart from an older position
        sameTree("replayed again", subtree(index, dir), want);
        // y.txt was only found because the walk registered "deep"; check it directly too.
        const ws::EntryId deep = index.findPath((base / L"incoming" / L"deep").wstring());
        const std::uint32_t deepRecord = recordOfPath(base / L"incoming" / L"deep");
        std::printf("  walked-in folder registered   %s\n",
            deep != ws::kNoEntry && index.folderByRecord(index.roots().front(), deepRecord) == deep ? "yes" : "NO");
    }
    fs::remove_all(base, ec);
}

int runMftCheck(bool compare)
{
    ws::Settings settings;
    settings.load();
    const ws::CrawlRules rules = settings.crawlRules();
    const ws::Crawler crawler(rules);
    for (const auto& v : ws::listLocalVolumes(false)) {
        std::printf("\n%s  %s\n", narrow(ws::wtf8::view(v.root)).c_str(), v.ntfs ? "NTFS" : "not NTFS");
        if (!v.ntfs)
            continue;
        const auto journal = ws::ntfs::queryJournal(v.root);
        if (journal)
            std::printf("  journal id %llx  first %lld  next %lld  (%.1f MB kept)\n",
                static_cast<unsigned long long>(journal->id), static_cast<long long>(journal->firstUsn),
                static_cast<long long>(journal->nextUsn),
                static_cast<double>(journal->nextUsn - journal->firstUsn) / (1024.0 * 1024.0));
        else
            std::printf("  no change journal (error %lu)\n", ::GetLastError());
        {
            ws::ntfs::MftReader reader(v.root);
            if (!reader.valid()) {
                std::printf("  MFT: %s\n", narrow(ws::wtf8::view(reader.error())).c_str());
                continue;
            }
            QElapsedTimer t;
            t.start();
            double parseMs = 0;
            std::size_t inUse = 0;
            std::size_t freeBlocks = 0; // 4 MB blocks without a record in use
            ws::ntfs::FileRecord record;
            QElapsedTimer p;
            while (reader.next()) {
                p.restart();
                std::size_t used = 0;
                for (std::size_t i = 0; i < reader.blockRecords(); ++i)
                    used += reader.parse(i, record) && record.inUse ? 1 : 0;
                inUse += used;
                freeBlocks += used == 0 ? 1 : 0;
                parseMs += msSince(p);
            }
            std::printf("  MFT %llu records (%zu in use, %zu of the 4 MB blocks unused), extents from %s,\n"
                        "      read %.0f ms (of which parsing %.0f ms)%s\n",
                static_cast<unsigned long long>(reader.recordCount()), inUse, freeBlocks,
                reader.extentsFromRecordZero() ? "record 0" : "FSCTL_GET_RETRIEVAL_POINTERS", msSince(t), parseMs,
                reader.valid() ? "" : " (failed)");
        }
        const double before = privateMB();
        ws::FileIndex mft;
        double readMs = 0, syncMs = 0;
        if (!buildFromMft(mft, crawler, v.root, &readMs, &syncMs))
            continue;
        ::HeapCompact(::GetProcessHeap(), 0);
        std::printf("  index from MFT: %zu items, read %.0f ms + build %.0f ms, %.1f MB (peak commit %.0f MB)\n",
            mft.liveCount(), readMs, syncMs, privateMB() - before, peakCommitMB());
        const ws::RecordTable* records = mft.folderRecords(mft.roots().front());
        std::printf("  folder records %zu\n", records ? records->size() : 0);
        if (!compare)
            continue;
        ws::FileIndex walked;
        const ws::EntryId r = walked.addRoot(narrow(ws::wtf8::view(v.root)));
        QElapsedTimer t;
        t.start();
        walked.setInterning(true);
        crawler.sync(walked, {{r, v.root, 0}}, crawlThreads(), false, {});
        walked.setInterning(false);
        std::printf("  index from walk: %zu items in %.0f ms\n", walked.liveCount(), msSince(t));
        compareIndexes(mft, walked);
    }

    // The journal test runs on the volume of the build folder.
    const std::filesystem::path base
        = std::filesystem::path(QDir::toNativeSeparators(QCoreApplication::applicationDirPath()).toStdWString())
        / L"usn-selftest";
    journalSelfTest(base.root_name().wstring(), base, rules);
    return 0;
}

// ---- content search costs (--content) ---------------------------------------

using Clock = std::chrono::steady_clock;

double usSince(Clock::time_point t)
{
    return std::chrono::duration<double, std::micro>(Clock::now() - t).count();
}

constexpr std::size_t kReadChunk = 512 * 1024; // as ContentScanner::scanFile reads
constexpr DWORD kSkipAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN
    | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;

// SearchEngine ranks candidates like this: your files first, system / program
// / tool folders (EntryFlag::LowPriority) last; 全部 leaves those out.
const char* const kClassNames[] = {"user profile", "other", "hidden", "low priority"};

struct ContentCandidate {
    ws::EntryId id;
    int cls; // index into kClassNames
};

struct FileCost {
    enum Outcome { OpenFailed, Skipped, Read } outcome = OpenFailed; // Skipped: cloud placeholder, empty, too big
    double openUs = 0;
    double readUs = 0; // with the attribute and size queries before it
    double closeUs = 0;
    double scanUs = 0; // CPU only: the bytes are in memory by then
    std::size_t size = 0;
    ws::TextEncoding encoding = ws::TextEncoding::Utf8;
    bool nonAscii = false;
    bool han = false; // has Chinese characters (for ANSI files: any double-byte character)
};

// Whether the text has any non-ASCII character, and any CJK ideograph (U+4E00..U+9FFF).
void classifyText(const char* d, std::size_t n, ws::TextEncoding encoding, FileCost& cost)
{
    const auto* b = reinterpret_cast<const unsigned char*>(d);
    switch (encoding) {
    case ws::TextEncoding::Utf16LE:
    case ws::TextEncoding::Utf16BE:
        for (std::size_t i = 0; i + 1 < n; i += 2) {
            const unsigned u = encoding == ws::TextEncoding::Utf16LE ? b[i] | (b[i + 1] << 8) : (b[i] << 8) | b[i + 1];
            cost.nonAscii |= u >= 0x80 && u != 0xFEFF;
            if (u >= 0x4E00 && u <= 0x9FFF) {
                cost.han = true;
                return;
            }
        }
        return;
    case ws::TextEncoding::Ansi:
        for (std::size_t i = 0; i + 1 < n; ++i) {
            if (b[i] >= 0x81 && b[i + 1] >= 0x40) {
                cost.nonAscii = cost.han = true;
                return;
            }
        }
        return;
    case ws::TextEncoding::Utf8:
        for (std::size_t i = 0; i + 2 < n; ++i) {
            if (b[i] < 0x80)
                continue;
            if (!(i == 0 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF))
                cost.nonAscii = true;
            if (b[i] >= 0xE4 && b[i] <= 0xE9 && (b[i + 1] & 0xC0) == 0x80 && (b[i + 2] & 0xC0) == 0x80) {
                cost.han = true;
                return;
            }
        }
        return;
    }
}

// Opens and reads one file as ContentScanner::scanFile does, timing each step,
// then scans the bytes from memory. `buffer` is page aligned and holds
// `capacity` bytes (the size limit plus one chunk). Unbuffered reads skip the
// file cache, so they show what the disk itself takes.
FileCost measureFile(const std::wstring& path, const ws::ContentScanner& scanner, char* buffer, std::size_t capacity,
    bool unbuffered)
{
    FileCost cost;
    auto t = Clock::now();
    const HANDLE h = ::CreateFileW(ws::win32::longPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        unbuffered ? FILE_FLAG_NO_BUFFERING : FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    cost.openUs = usSince(t);
    if (h == INVALID_HANDLE_VALUE)
        return cost;

    t = Clock::now();
    FILE_BASIC_INFO basic {};
    LARGE_INTEGER size {};
    const bool skip = (::GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof basic)
                          && (basic.FileAttributes & kSkipAttributes))
        || !::GetFileSizeEx(h, &size) || size.QuadPart <= 0
        || static_cast<std::uint64_t>(size.QuadPart) + kReadChunk > capacity;
    std::size_t total = 0;
    if (!skip) {
        FILE_IO_PRIORITY_HINT_INFO hint {};
        hint.PriorityHint = IoPriorityHintLow;
        ::SetFileInformationByHandle(h, FileIoPriorityHintInfo, &hint, sizeof hint);
        while (total < static_cast<std::size_t>(size.QuadPart)) {
            DWORD got = 0;
            if (!::ReadFile(h, buffer + total, static_cast<DWORD>(kReadChunk), &got, nullptr) || got == 0)
                break;
            total += got;
        }
    }
    cost.readUs = usSince(t);
    t = Clock::now();
    ::CloseHandle(h);
    cost.closeUs = usSince(t);
    if (skip) {
        cost.outcome = FileCost::Skipped;
        return cost;
    }
    cost.outcome = FileCost::Read;
    cost.size = total;
    std::size_t bom = 0;
    cost.encoding = ws::ContentScanner::detect({buffer, std::min(total, kReadChunk)}, &bom);
    classifyText(buffer, total, cost.encoding, cost);
    std::size_t pos = 0;
    const ws::ContentScanner::ReadFn read = [&](char* out, std::size_t room) {
        const std::size_t n = std::min(room, total - pos);
        std::memcpy(out, buffer + pos, n);
        pos += n;
        return n;
    };
    t = Clock::now();
    (void)scanner.scan(read, kReadChunk, {});
    cost.scanUs = usSince(t);
    return cost;
}

// ContentScanner::scanFile, with the low I/O priority hint optional. Returns the bytes read.
std::size_t scanStreaming(const std::wstring& path, const ws::ContentScanner& scanner, std::int64_t maxBytes,
    bool lowPriority)
{
    ws::win32::UniqueHandle file(::CreateFileW(ws::win32::longPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!file.valid())
        return 0;
    FILE_BASIC_INFO basic {};
    if (::GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic, sizeof basic)
        && (basic.FileAttributes & kSkipAttributes))
        return 0;
    LARGE_INTEGER size {};
    if (!::GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 || size.QuadPart > maxBytes)
        return 0;
    if (lowPriority) {
        FILE_IO_PRIORITY_HINT_INFO hint {};
        hint.PriorityHint = IoPriorityHintLow;
        ::SetFileInformationByHandle(file.get(), FileIoPriorityHintInfo, &hint, sizeof hint);
    }
    std::size_t total = 0;
    const HANDLE h = file.get();
    const ws::ContentScanner::ReadFn read = [&](char* buffer, std::size_t capacity) -> std::size_t {
        DWORD got = 0;
        if (!::ReadFile(h, buffer, static_cast<DWORD>(std::min<std::size_t>(capacity, 1u << 30)), &got, nullptr))
            return 0;
        total += got;
        return got;
    };
    (void)scanner.scan(read, kReadChunk, {});
    return total;
}

void printSpread(const char* label, std::vector<double> values, double scale, const char* unit)
{
    if (values.empty()) {
        std::printf("    %-8s -\n", label);
        return;
    }
    std::sort(values.begin(), values.end());
    const auto at = [&](double q) {
        return values[std::min(values.size() - 1, static_cast<std::size_t>(q * static_cast<double>(values.size())))]
            / scale;
    };
    const double mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size()) / scale;
    std::printf("    %-8s mean %9.1f   p50 %9.1f   p90 %9.1f   p99 %9.1f   max %10.1f  %s\n", label, mean, at(0.5),
        at(0.9), at(0.99), values.back() / scale, unit);
}

// Times every file of `paths` one after another; returns the mean cost per file in microseconds.
double reportPass(const char* title, const std::vector<std::pair<std::wstring, int>>& paths,
    const ws::ContentScanner& scanner, char* buffer, std::size_t capacity, bool unbuffered,
    std::array<double, 4>* meanSizeByClass)
{
    std::vector<double> open, read, close, scan, total, sizes;
    std::size_t outcomes[3] = {};
    std::size_t encodings[4] = {};
    std::size_t tiny = 0; // could sit in the MFT record itself
    std::array<double, 4> bytesByClass {};
    std::array<std::size_t, 4> filesByClass {};
    std::array<std::size_t, 4> readByClass {};
    std::array<std::size_t, 4> nonAsciiByClass {};
    std::array<std::size_t, 4> hanByClass {};
    double hanBytes = 0;
    std::vector<double> sizesAbove1MB;
    const auto start = Clock::now();
    for (const auto& [path, cls] : paths) {
        const FileCost c = measureFile(path, scanner, buffer, capacity, unbuffered);
        ++outcomes[c.outcome];
        open.push_back(c.openUs);
        total.push_back(c.openUs + c.readUs + c.closeUs + c.scanUs);
        ++filesByClass[static_cast<std::size_t>(cls)];
        if (c.outcome != FileCost::Read)
            continue;
        read.push_back(c.readUs);
        close.push_back(c.closeUs);
        scan.push_back(c.scanUs);
        sizes.push_back(static_cast<double>(c.size));
        const auto k = static_cast<std::size_t>(cls);
        bytesByClass[k] += static_cast<double>(c.size);
        ++readByClass[k];
        nonAsciiByClass[k] += c.nonAscii ? 1 : 0;
        hanByClass[k] += c.han ? 1 : 0;
        hanBytes += c.han ? static_cast<double>(c.size) : 0.0;
        if (c.size > (1u << 20))
            sizesAbove1MB.push_back(static_cast<double>(c.size));
        ++encodings[static_cast<int>(c.encoding)];
        tiny += c.size <= 600 ? 1 : 0;
    }
    const double seconds = usSince(start) / 1e6;
    std::printf("\n  %s: %zu files in %.1f s, one thread (%zu read, %zu skipped, %zu could not be opened)\n", title,
        paths.size(), seconds, outcomes[FileCost::Read], outcomes[FileCost::Skipped], outcomes[FileCost::OpenFailed]);
    printSpread("open", open, 1.0, "us");
    printSpread("read", read, 1.0, "us");
    printSpread("close", close, 1.0, "us");
    printSpread("scan", scan, 1.0, "us (CPU)");
    printSpread("total", total, 1.0, "us per file");
    printSpread("size", sizes, 1024.0, "KB");
    const double readFiles = std::max<double>(1.0, static_cast<double>(sizes.size()));
    std::printf("    encodings: UTF-8 %zu, UTF-16LE %zu, UTF-16BE %zu, ANSI %zu;  <= 600 bytes: %.1f%%\n", encodings[0],
        encodings[1], encodings[2], encodings[3], 100.0 * static_cast<double>(tiny) / readFiles);
    const double allBytes = std::accumulate(sizes.begin(), sizes.end(), 0.0);
    std::printf("    over 1 MB: %zu files (%.1f%%) holding %.1f%% of the bytes;  files with Chinese hold %.1f%% of the bytes\n",
        sizesAbove1MB.size(), 100.0 * static_cast<double>(sizesAbove1MB.size()) / readFiles,
        100.0 * std::accumulate(sizesAbove1MB.begin(), sizesAbove1MB.end(), 0.0) / std::max(1.0, allBytes),
        100.0 * hanBytes / std::max(1.0, allBytes));
    for (std::size_t k = 0; k < 4; ++k) {
        if (readByClass[k] == 0)
            continue;
        const auto r = static_cast<double>(readByClass[k]);
        std::printf("    %-13s %5zu read: non-ASCII %5.1f%%, Chinese %5.1f%%\n", kClassNames[k], readByClass[k],
            100.0 * static_cast<double>(nonAsciiByClass[k]) / r, 100.0 * static_cast<double>(hanByClass[k]) / r);
    }
    if (meanSizeByClass) {
        for (std::size_t k = 0; k < 4; ++k)
            (*meanSizeByClass)[k] = filesByClass[k] ? bytesByClass[k] / static_cast<double>(filesByClass[k]) : 0.0;
    }
    return std::accumulate(total.begin(), total.end(), 0.0) / std::max<double>(1.0, static_cast<double>(total.size()));
}

int runContentBench(const ws::FileIndex& index, QStringList args)
{
    ws::Settings settings;
    settings.load();
    std::size_t sample = 4000;
    QString needle = u"量子纠缠qzxv"_s; // matches nothing, so every file is read to the end
    if (const qsizetype at = args.indexOf(u"--sample"_s); at >= 0 && at + 1 < args.size())
        sample = std::max(100, args[at + 1].toInt());
    if (const qsizetype at = args.indexOf(u"--needle"_s); at >= 0 && at + 1 < args.size())
        needle = args[at + 1];
    const std::int64_t maxBytes = std::int64_t {settings.maxContentFileSizeMB} << 20;

    std::vector<std::string> extensions;
    for (QString ext : settings.contentExtensions) {
        while (ext.startsWith(u'.') || ext.startsWith(u'*'))
            ext.remove(0, 1);
        if (!ext.isEmpty())
            extensions.push_back(ws::text::foldAscii(ws::wtf8::fromUtf16(ws::wtf8::view(ext))));
    }

    // 1. The candidates, collected as SearchEngine::runContentSearch does,
    //    including the buffer it keeps every candidate's full path in.
    const std::wstring profile = ws::win32::expandEnvironment(L"%USERPROFILE%");
    std::vector<ContentCandidate> candidates;
    std::u16string paths;
    std::map<std::string, std::array<std::size_t, 2>> byExtension; // all, in low priority folders
    std::array<std::size_t, 4> byClass {};
    auto t = Clock::now();
    for (std::size_t c = 0; c < index.chunkCount(); ++c) {
        const auto entries = index.chunk(c);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const ws::Entry& e = entries[i];
            if ((e.flags & (ws::EntryFlag::Deleted | ws::EntryFlag::Directory | ws::EntryFlag::Offline))
                || e.extLength == 0)
                continue;
            const std::string_view name = index.name(e);
            const std::string_view ext = name.substr(name.size() - e.extLength);
            const auto x = std::find_if(extensions.begin(), extensions.end(),
                [&](const std::string& s) { return ws::text::equalsFolded(ext, s); });
            if (x == extensions.end())
                continue;
            const auto id = static_cast<ws::EntryId>((c << ws::FileIndex::kChunkBits) + i);
            const std::size_t offset = paths.size();
            index.appendPath16(id, paths);
            const std::wstring_view path = ws::wtf8::wview(std::u16string_view(paths).substr(offset));
            int cls = 1;
            if (e.flags & ws::EntryFlag::LowPriority)
                cls = 3;
            else if (e.flags & ws::EntryFlag::Hidden)
                cls = 2;
            else if (path.size() > profile.size() && path[profile.size()] == L'\\'
                && ws::win32::equalsIgnoreCase(path.substr(0, profile.size()), profile))
                cls = 0;
            candidates.push_back({id, cls});
            ++byClass[static_cast<std::size_t>(cls)];
            auto& counts = byExtension[*x];
            ++counts[0];
            counts[1] += cls == 3 ? 1 : 0;
        }
    }
    const double collectMs = usSince(t) / 1000.0;
    const double pathMB = static_cast<double>(paths.capacity() * sizeof(char16_t)) / (1024.0 * 1024.0);
    // The app's Candidate is 12 bytes: offset, length, priority.
    const double listMB = static_cast<double>(candidates.size() * 12) / (1024.0 * 1024.0);
    std::printf("\ncontent candidates  %zu, collected in %.0f ms (%zu extensions, files up to %d MB)\n",
        candidates.size(), collectMs, extensions.size(), settings.maxContentFileSizeMB);
    std::printf("  path buffer %.1f MB allocated (%.1f characters per path) + candidate list %.1f MB\n", pathMB,
        static_cast<double>(paths.size()) / static_cast<double>(std::max<std::size_t>(1, candidates.size())), listMB);
    for (std::size_t k = 0; k < 4; ++k)
        std::printf("  %-13s %9zu  (%4.1f%%)\n", kClassNames[k], byClass[k],
            100.0 * static_cast<double>(byClass[k]) / static_cast<double>(std::max<std::size_t>(1, candidates.size())));
    std::vector<std::pair<std::size_t, std::string>> topExt;
    for (const auto& [ext, counts] : byExtension)
        topExt.emplace_back(counts[0], ext);
    std::sort(topExt.rbegin(), topExt.rend());
    std::printf("  by extension (files, of which in low priority folders):\n");
    for (std::size_t k = 0; k < topExt.size() && k < 16; ++k) {
        const auto& counts = byExtension[topExt[k].second];
        std::printf("    %-6s %9zu  %5.1f%%\n", topExt[k].second.c_str(), counts[0],
            100.0 * static_cast<double>(counts[1]) / static_cast<double>(counts[0]));
    }
    paths = std::u16string();
    if (candidates.empty())
        return 0;

    // 2. Random samples, disjoint, so each one starts as cold as the cache allows.
    std::vector<std::size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), std::size_t {0});
    std::shuffle(order.begin(), order.end(), std::mt19937_64(20261007));
    std::size_t cursor = 0;
    const auto take = [&](std::size_t n) {
        std::vector<std::pair<std::wstring, int>> out;
        for (; n > 0 && cursor < order.size(); --n, ++cursor) {
            const ContentCandidate& c = candidates[order[cursor]];
            out.emplace_back(index.wpath(c.id), c.cls);
        }
        return out;
    };

    const ws::ContentScanner scanner(needle);
    const std::size_t capacity = static_cast<std::size_t>(maxBytes) + kReadChunk;
    auto* buffer = static_cast<char*>(::VirtualAlloc(nullptr, capacity, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buffer)
        return 1;
    std::printf("\nsample of %zu files (needle \"%s\", matches nothing: every file is read to the end)", sample,
        needle.toUtf8().constData());
    const auto first = take(sample);
    std::array<double, 4> meanSize {};
    const double coldUs = reportPass("first touch", first, scanner, buffer, capacity, false, &meanSize);
    const double warmUs = reportPass("same files again (cache warm)", first, scanner, buffer, capacity, false, nullptr);
    reportPass("other files, unbuffered (past the file cache)", take(sample / 2), scanner, buffer, capacity, true,
        nullptr);
    ::VirtualFree(buffer, 0, MEM_RELEASE);

    double totalBytes = 0;
    std::printf("\n  estimated bytes of all candidates (mean size in the sample x count):\n");
    for (std::size_t k = 0; k < 4; ++k) {
        const double bytes = meanSize[k] * static_cast<double>(byClass[k]);
        totalBytes += bytes;
        std::printf("    %-13s %8.2f GB\n", kClassNames[k], bytes / (1024.0 * 1024.0 * 1024.0));
    }
    std::printf("    %-13s %8.2f GB\n", "all", totalBytes / (1024.0 * 1024.0 * 1024.0));
    const double n = static_cast<double>(candidates.size());
    std::printf("  one thread over all candidates: %.0f s first touch, %.0f s warm\n", coldUs * n / 1e6,
        warmUs * n / 1e6);
    if (args.contains(u"--gram-test"_s)) {
        // How big a file-level index of the text would be: per file, its
        // distinct Chinese characters, pairs of adjacent ones, and trigrams
        // (ASCII folded), counted on the first sample (warm by now).
        const auto isHan = [](char16_t c) { return (c >= 0x3400 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF); };
        std::array<std::array<double, 3>, 4> pairsByClass {}; // [class][uni, bi, tri]
        std::array<std::size_t, 4> filesByClass {};
        std::array<double, 3> pairsBig {}; // files over 1 MB
        std::vector<std::uint64_t> uni, bi, tri;
        auto* text = static_cast<char*>(::VirtualAlloc(nullptr, capacity, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        cursor = 0;
        for (const auto& [path, cls] : take(sample)) {
            ws::win32::UniqueHandle file(::CreateFileW(ws::win32::longPath(path).c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
            LARGE_INTEGER size {};
            if (!file.valid() || !::GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0
                || static_cast<std::uint64_t>(size.QuadPart) + kReadChunk > capacity)
                continue;
            std::size_t total = 0;
            DWORD got = 0;
            while (total < static_cast<std::size_t>(size.QuadPart)
                && ::ReadFile(file.get(), text + total, static_cast<DWORD>(kReadChunk), &got, nullptr) && got > 0)
                total += got;
            std::size_t bom = 0;
            const ws::TextEncoding encoding = ws::ContentScanner::detect({text, std::min(total, kReadChunk)}, &bom);
            QString s;
            switch (encoding) {
            case ws::TextEncoding::Utf8:
                s = QString::fromUtf8(text + bom, static_cast<qsizetype>(total - bom));
                break;
            case ws::TextEncoding::Utf16LE:
            case ws::TextEncoding::Utf16BE:
                s.resize(static_cast<qsizetype>((total - bom) / 2));
                for (qsizetype i = 0; i < s.size(); ++i) {
                    const auto* b = reinterpret_cast<const unsigned char*>(text + bom + 2 * i);
                    s[i] = QChar(encoding == ws::TextEncoding::Utf16LE ? b[0] | (b[1] << 8) : (b[0] << 8) | b[1]);
                }
                break;
            case ws::TextEncoding::Ansi: {
                const int wide = ::MultiByteToWideChar(scanner.ansiCodePage(), 0, text, static_cast<int>(total), nullptr, 0);
                s.resize(std::max(wide, 0));
                ::MultiByteToWideChar(scanner.ansiCodePage(), 0, text, static_cast<int>(total),
                    reinterpret_cast<wchar_t*>(s.data()), wide);
                break;
            }
            }
            uni.clear();
            bi.clear();
            tri.clear();
            const char16_t* c = reinterpret_cast<const char16_t*>(s.utf16());
            const auto fold = [](char16_t u) -> std::uint64_t { return u >= 'A' && u <= 'Z' ? u + 32 : u; };
            for (qsizetype i = 0; i < s.size(); ++i) {
                if (isHan(c[i])) {
                    uni.push_back(c[i]);
                    if (i + 1 < s.size() && isHan(c[i + 1]))
                        bi.push_back((std::uint64_t {c[i]} << 16) | c[i + 1]);
                }
                if (i + 2 < s.size())
                    tri.push_back((fold(c[i]) << 32) | (fold(c[i + 1]) << 16) | fold(c[i + 2]));
            }
            const auto distinct = [](std::vector<std::uint64_t>& v) {
                std::sort(v.begin(), v.end());
                return static_cast<double>(std::unique(v.begin(), v.end()) - v.begin());
            };
            const std::array<double, 3> counts {distinct(uni), distinct(bi), distinct(tri)};
            const auto k = static_cast<std::size_t>(cls);
            ++filesByClass[k];
            for (std::size_t g = 0; g < 3; ++g) {
                pairsByClass[k][g] += counts[g];
                if (total > (1u << 20))
                    pairsBig[g] += counts[g];
            }
        }
        ::VirtualFree(text, 0, MEM_RELEASE);
        // Posting lists of file numbers, delta + varint coded: ~1.5 bytes per (gram, file) pair.
        constexpr double kBytesPerPair = 1.5;
        std::printf("\nfile-level index of the text (distinct grams per file; ~%.1f bytes per gram and file):\n",
            kBytesPerPair);
        std::array<double, 3> all {}, mostly {};
        for (std::size_t k = 0; k < 4; ++k) {
            if (filesByClass[k] == 0)
                continue;
            const double f = static_cast<double>(filesByClass[k]);
            std::printf("  %-13s %5zu files: per file %7.0f chars, %7.0f char pairs, %8.0f trigrams\n", kClassNames[k],
                filesByClass[k], pairsByClass[k][0] / f, pairsByClass[k][1] / f, pairsByClass[k][2] / f);
            for (std::size_t g = 0; g < 3; ++g) {
                const double estimate = pairsByClass[k][g] / f * static_cast<double>(byClass[k]) * kBytesPerPair;
                all[g] += estimate;
                if (k != 3)
                    mostly[g] += estimate;
            }
        }
        const double gb = 1024.0 * 1024.0 * 1024.0;
        std::printf("  estimated index, without low priority folders: Chinese chars + pairs %.2f GB, trigrams %.2f GB\n",
            (mostly[0] + mostly[1]) / gb, mostly[2] / gb);
        std::printf("  estimated index, all candidates:               Chinese chars + pairs %.2f GB, trigrams %.2f GB\n",
            (all[0] + all[1]) / gb, all[2] / gb);
        const double sampleTri = pairsByClass[0][2] + pairsByClass[1][2] + pairsByClass[2][2] + pairsByClass[3][2];
        std::printf("  files over 1 MB account for %.1f%% of the trigram pairs, %.1f%% of the Chinese pairs\n",
            100.0 * pairsBig[2] / std::max(1.0, sampleTri),
            100.0 * pairsBig[1]
                / std::max(1.0, pairsByClass[0][1] + pairsByClass[1][1] + pairsByClass[2][1] + pairsByClass[3][1]));
    }
    if (args.contains(u"--open-test"_s)) {
        // Who makes opening slow: the file system, or the scan that real-time
        // protection runs when a file is opened to read its data? Opening for
        // attributes only reads nothing, so it is not scanned.
        cursor = std::max<std::size_t>(cursor, 30000); // past every file earlier runs touched
        std::vector<double> attributesOnly, data, dataAgain;
        for (const auto& [path, cls] : take(1000)) {
            const std::wstring p = ws::win32::longPath(path);
            const auto timeOpen = [&](DWORD access, DWORD flags) {
                const auto start = Clock::now();
                const HANDLE h = ::CreateFileW(p.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, flags, nullptr);
                const double us = usSince(start);
                if (h != INVALID_HANDLE_VALUE)
                    ::CloseHandle(h);
                return us;
            };
            attributesOnly.push_back(timeOpen(FILE_READ_ATTRIBUTES, 0));
            data.push_back(timeOpen(GENERIC_READ, FILE_FLAG_SEQUENTIAL_SCAN));
            dataAgain.push_back(timeOpen(GENERIC_READ, FILE_FLAG_SEQUENTIAL_SCAN));
        }
        std::printf("\nopening 1000 untouched files, each three times in a row:\n");
        printSpread("attrs", attributesOnly, 1.0, "us  (FILE_READ_ATTRIBUTES, first open)");
        printSpread("data", data, 1.0, "us  (GENERIC_READ, second open)");
        printSpread("again", dataAgain, 1.0, "us  (GENERIC_READ, third open)");
    }
    if (args.contains(u"--no-throughput"_s))
        return 0;

    // 3. Throughput by thread count, each run on files not touched before.
    const std::size_t runFiles = std::max<std::size_t>(500, sample * 3 / 4);
    const int appThreads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 2, 6);
    struct Run {
        int threads;
        bool lowPriority;
    };
    const Run runs[] = {{appThreads, true}, {appThreads, false}, {16, true}, {32, true}, {64, true}};
    std::printf("\nthroughput (%zu new files per run; the app uses %d threads, low I/O priority):\n", runFiles,
        appThreads);
    for (const Run& run : runs) {
        const auto files = take(runFiles);
        if (files.size() < runFiles)
            break;
        std::atomic<std::size_t> next {0};
        std::atomic<std::size_t> bytes {0};
        t = Clock::now();
        {
            std::vector<std::jthread> workers;
            for (int w = 0; w < run.threads; ++w) {
                workers.emplace_back([&] {
                    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                    for (std::size_t k; (k = next.fetch_add(1)) < files.size();)
                        bytes += scanStreaming(files[k].first, scanner, maxBytes, run.lowPriority);
                });
            }
        }
        const double seconds = usSince(t) / 1e6;
        const double perSecond = static_cast<double>(files.size()) / seconds;
        std::printf("  %2d threads, %-6s I/O priority: %7.0f files/s, %6.1f MB/s  -> all candidates in ~%.0f s, "
                    "without low priority folders ~%.0f s\n",
            run.threads, run.lowPriority ? "low" : "normal", perSecond,
            static_cast<double>(bytes.load()) / seconds / (1024.0 * 1024.0), n / perSecond,
            (n - static_cast<double>(byClass[3])) / perSecond);
    }
    return 0;
}

// Builds a content index of the files a content search looks in (as the app's
// indexer does, with more threads), then times lookups and the candidate walk
// a content search does with it.
int runContentIndexBench(const ws::FileIndex& index, QStringList args)
{
    ws::Settings settings;
    settings.load();
    const bool all = args.contains(u"--all"_s); // system and program folders too
    const ws::ContentFilter filter {ws::ExtensionFilter(settings.contentExtensions), all};
    int threads = 32;
    if (const qsizetype at = args.indexOf(u"--threads"_s); at >= 0 && at + 1 < args.size())
        threads = std::max(1, args[at + 1].toInt());
    std::size_t limit = std::numeric_limits<std::size_t>::max();
    if (const qsizetype at = args.indexOf(u"--sample"_s); at >= 0 && at + 1 < args.size())
        limit = args[at + 1].toULongLong();
    QString directory = QDir::tempPath() + u"/wsbench-content"_s;
    if (const qsizetype at = args.indexOf(u"--dir"_s); at >= 0 && at + 1 < args.size())
        directory = args[at + 1];
    QDir(directory).removeRecursively();

    std::vector<ws::EntryId> files;
    for (std::size_t c = 0; c < index.chunkCount(); ++c) {
        const auto entries = index.chunk(c);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (filter.accepts(index, entries[i]))
                files.push_back(static_cast<ws::EntryId>((c << ws::FileIndex::kChunkBits) + i));
        }
    }
    if (files.size() > limit) {
        std::mt19937_64 rng(20261007);
        std::shuffle(files.begin(), files.end(), rng);
        files.resize(limit);
        std::sort(files.begin(), files.end());
    }
    std::printf("content index: %zu files (%s), %d threads, into %s\n", files.size(),
        all ? "all folders" : "without system and program folders", threads, qPrintable(directory));

    const double privateBefore = privateMB();
    ws::ContentIndex content(directory);
    const std::int64_t maxBytes = static_cast<std::int64_t>(settings.maxContentFileSizeMB) << 20;
    std::atomic<std::size_t> next {0};
    std::atomic<std::size_t> indexed {0};
    std::atomic<std::size_t> empty {0};
    std::atomic<std::size_t> skipped {0};
    std::atomic<std::uint64_t> pairs {0};
    std::atomic<std::size_t> withGrams {0};
    QElapsedTimer timer;
    timer.start();
    {
        std::vector<std::jthread> workers;
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&] {
                for (;;) {
                    const std::size_t i = next.fetch_add(1);
                    if (i >= files.size())
                        break;
                    const std::wstring path = index.wpath(files[i]);
                    const auto text = ws::ContentIndexer::readFile(path, maxBytes, {});
                    switch (text.outcome) {
                    case ws::ContentIndexer::Outcome::Indexed:
                        ++indexed;
                        pairs += text.keys.size();
                        withGrams += text.keys.empty() ? 0 : 1;
                        break;
                    case ws::ContentIndexer::Outcome::Empty:
                        ++empty;
                        break;
                    case ws::ContentIndexer::Outcome::Skipped:
                        ++skipped;
                        continue;
                    }
                    content.add(files[i], text.keys, text.outcome == ws::ContentIndexer::Outcome::Empty, text.stamp, 0);
                    if ((i + 1) % 20000 == 0)
                        std::printf("  %zu files, %.0f s\n", i + 1, msSince(timer) / 1000.0);
                }
            });
        }
    }
    const double buildMs = msSince(timer);
    timer.restart();
    content.merge();
    const double mergeMs = msSince(timer);
    ::HeapCompact(::GetProcessHeap(), 0);
    const auto stats = content.stats();
    std::printf("read         %.0f s (%.0f files/s): %zu read, %zu with grams, %zu empty/too large, %zu skipped\n",
        buildMs / 1000.0, static_cast<double>(files.size()) * 1000.0 / buildMs, indexed.load(), withGrams.load(),
        empty.load(), skipped.load());
    std::printf("index        %.1f MB on disk (%zu documents, %.1f M gram-document pairs, %.2f bytes each), merge %.0f ms\n",
        static_cast<double>(stats.segmentBytes) / (1024.0 * 1024.0), stats.documents,
        static_cast<double>(pairs.load()) / 1e6, static_cast<double>(stats.segmentBytes) / static_cast<double>(std::max<std::uint64_t>(pairs, 1)),
        mergeMs);
    std::printf("             %.2f M distinct grams (tables %.1f MB), postings %.1f MB\n",
        static_cast<double>(stats.grams) / 1e6,
        static_cast<double>(stats.segmentBytes - stats.postingBytes) / (1024.0 * 1024.0),
        static_cast<double>(stats.postingBytes) / (1024.0 * 1024.0));
    std::printf("memory       %.1f MB private since the start (documents table, read buffers, heap slack), peak +%.1f MB\n",
        privateMB() - privateBefore, peakCommitMB() - privateBefore);
    std::vector<std::uint64_t> segments;
    std::vector<ws::EntryId> identity(index.slotCount());
    std::iota(identity.begin(), identity.end(), ws::EntryId {0});
    std::printf("state        %.1f MB in the snapshot\n",
        static_cast<double>(content.serialize(identity, segments).size()) / (1024.0 * 1024.0));

    // Lookups, and the candidate walk a content search does with them.
    const QStringList needles = {u"的"_s, u"中文"_s, u"合同"_s, u"配置"_s, u"季度报告"_s, u"快速搜索"_s, u"日本語"_s,
        u"测试用例"_s, u"错误"_s, u"注意事项"_s, u"the"_s, u"error"_s, u"function"_s, u"WinShun"_s,
        u"localhost:8080"_s, u"2026-10"_s, u"std::vector"_s, u"getElementById"_s, u"ab"_s};
    const auto lock = index.readLock();
    for (const QString& needle : needles) {
        double lookupMs = 1e9;
        double walkMs = 1e9;
        std::size_t matched = 0;
        std::size_t unknown = 0;
        std::size_t ruledOut = 0;
        for (int run = 0; run < 3; ++run) {
            timer.restart();
            const auto lookup = content.lookup(needle);
            lookupMs = std::min(lookupMs, msSince(timer));
            timer.restart();
            matched = unknown = ruledOut = 0;
            std::size_t k = 0;
            constexpr std::uint32_t kMatch = ws::ContentIndex::Lookup::kMatch;
            for (std::size_t c = 0; c < index.chunkCount(); ++c) {
                const auto entries = index.chunk(c);
                for (std::size_t i = 0; i < entries.size(); ++i) {
                    if (!filter.accepts(index, entries[i]))
                        continue;
                    const auto id = static_cast<ws::EntryId>((c << ws::FileIndex::kChunkBits) + i);
                    while (k < lookup.known.size() && (lookup.known[k] & ~kMatch) < id)
                        ++k;
                    if (k < lookup.known.size() && (lookup.known[k] & ~kMatch) == id)
                        ++((lookup.known[k] & kMatch) ? matched : ruledOut);
                    else
                        ++unknown;
                }
            }
            walkMs = std::min(walkMs, msSince(timer));
        }
        std::printf("%s%*s lookup %6.1f ms, walk %5.0f ms: %7zu files may contain it, %7zu ruled out, %6zu unknown\n",
            needle.toUtf8().constData(), std::max(0, static_cast<int>(16 - needle.size() * (needle[0].unicode() < 0x80 ? 1 : 2))), "", lookupMs, walkMs, matched,
            ruledOut, unknown);
    }
    // --verify: read every file the index rules out; any that contains the
    // phrase is a miss (a bug, or a file written to since it was indexed).
    if (args.contains(u"--verify"_s)) {
        for (const QString& needle : {u"WinShun"_s, u"std::vector"_s, u"localhost:8080"_s, u"getElementById"_s,
                 u"error"_s, u"季度报告"_s, u"配置"_s}) {
            const auto lookup = content.lookup(needle);
            std::vector<ws::EntryId> ruledOut;
            for (const std::uint32_t k : lookup.known) {
                if (!(k & ws::ContentIndex::Lookup::kMatch))
                    ruledOut.push_back(k);
            }
            const ws::ContentScanner scanner(needle);
            std::atomic<std::size_t> at {0};
            std::atomic<std::size_t> misses {0};
            std::mutex printMutex;
            timer.restart();
            {
                std::vector<std::jthread> workers;
                for (int t = 0; t < threads; ++t) {
                    workers.emplace_back([&] {
                        for (std::size_t i = at.fetch_add(1); i < ruledOut.size(); i = at.fetch_add(1)) {
                            const std::wstring path = index.wpath(ruledOut[i]);
                            if (!scanner.scanFile(path, maxBytes, {}))
                                continue;
                            if (misses.fetch_add(1) < 5) {
                                std::lock_guard printLock(printMutex);
                                std::printf("    miss: %s\n", QString::fromStdWString(path).toUtf8().constData());
                            }
                        }
                    });
                }
            }
            std::printf("verify %s: %zu ruled out, %zu of them contain it (%.0f s)\n", needle.toUtf8().constData(),
                ruledOut.size(), misses.load(), msSince(timer) / 1000.0);
        }
    }
    if (!args.contains(u"--keep"_s))
        QDir(directory).removeRecursively();
    return 0;
}

// Process heap: bytes handed out vs committed (the gap is free memory the
// heap keeps instead of returning it to the OS).
void printHeap(const char* label)
{
    HEAP_SUMMARY hs {};
    hs.cb = sizeof hs;
    if (::HeapSummary(::GetProcessHeap(), 0, &hs))
        std::printf("%-12s heap allocated %.1f MB, committed %.1f MB\n", label,
            static_cast<double>(hs.cbAllocated) / (1024.0 * 1024.0), static_cast<double>(hs.cbCommitted) / (1024.0 * 1024.0));
}

// ---- places (--places) -----------------------------------------------------

int runPlaces(const QStringList& queries)
{
    QElapsedTimer timer;
    timer.start();
    const ws::PlaceList places = ws::loadPlaces({});
    std::printf("places       %zu, read in %.0f ms\n", places.size(), static_cast<double>(timer.nsecsElapsed()) / 1e6);

    QFile extras(u":/winshun/places.txt"_s);
    if (extras.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ws::PlaceList copy = places;
        for (const QString& header : ws::applyPlaceExtras(copy, QString::fromUtf8(extras.readAll()), true))
            std::printf("places.txt: no place here opens [%s]\n", qUtf8Printable(header));
    }

    if (queries.isEmpty()) {
        for (const ws::PlaceInfo& p : places) {
            std::printf("%s | %s | %zu + %zu keywords\n", qUtf8Printable(p.name), qUtf8Printable(p.command),
                p.folded.size(), p.ownFolded.size());
        }
        return 0;
    }
    for (const QString& text : queries) {
        const ws::ParsedQuery query = ws::parseQuery(text);
        const ws::NameMatcher matcher(query);
        timer.restart();
        const std::vector<ws::PlaceHit> hits = ws::searchPlaces(places, query, matcher, {});
        const double us = static_cast<double>(timer.nsecsElapsed()) / 1e3;
        std::printf("\n%s  (%zu, %.0f us)\n", qUtf8Printable(text), hits.size(), us);
        for (std::size_t i = 0; i < std::min<std::size_t>(hits.size(), 8); ++i) {
            const ws::PlaceInfo& p = places[hits[i].index];
            std::printf("  %4d  %s | %s\n", hits[i].score, qUtf8Printable(p.name), qUtf8Printable(p.command));
        }
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments().mid(1);
    if (const qsizetype at = args.indexOf(u"--out"_s); at >= 0 && at + 1 < args.size()) {
        // For runs started elevated, whose console cannot be redirected.
        FILE* reopened = nullptr;
        _wfreopen_s(&reopened, reinterpret_cast<const wchar_t*>(args[at + 1].utf16()), L"w", stdout);
        std::setvbuf(stdout, nullptr, _IONBF, 0); // keep what was written if it crashes
        args.remove(at, 2);
    }
    if (args.contains(u"--mft"_s))
        return runMftCheck(!args.contains(u"--no-compare"_s));
    if (args.contains(u"--places"_s)) {
        args.removeAll(u"--places"_s);
        return runPlaces(args);
    }
    QString path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/WinShun/index.bin"_s;
    if (!args.isEmpty() && args.first().endsWith(u".bin"))
        path = args.takeFirst();
    if (args.isEmpty())
        args = {u"report"_s, u"a"_s, u"readme.md"_s, u"*.pdf"_s, u"ext:docx 合同"_s, u"src\\main"_s, u"zzzzqx"_s};

    const double before = privateMB();
    QElapsedTimer timer;
    timer.start();
    auto contents = ws::snapshot::load(path);
    ws::FileIndex* index = contents ? contents->index.get() : nullptr;
    if (!index) {
        std::printf("could not load %s\n", qPrintable(path));
        return 1;
    }
    const double loadMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    const double after = privateMB();
    const std::size_t chunks = index->chunkCount();
    std::printf("entries      %zu (slots %zu)\n", index->liveCount(), index->slotCount());
    std::printf("load         %.0f ms\n", loadMs);
    std::printf("memory       %.1f MB private (entries %.1f MB in %zu chunks, %.1f bytes/item overall)\n",
        after - before, static_cast<double>(chunks * ws::FileIndex::kChunkSize * sizeof(ws::Entry)) / (1024.0 * 1024.0),
        chunks, (after - before) * 1024.0 * 1024.0 / static_cast<double>(index->liveCount()));
    printHeap("after load");

    if (args.contains(u"--content"_s))
        return runContentBench(*index, args);
    if (args.contains(u"--content-index"_s))
        return runContentIndexBench(*index, args);

    if (args.contains(u"--sync"_s)) {
        // Re-walk all drives in place, as the app does after startup.
        args.removeAll(u"--sync"_s);
        // The app's rules (from the user's settings), so the walk matches the app's startup rescan.
        ws::Settings settings;
        settings.load();
        const ws::CrawlRules rules = settings.crawlRules();
        std::vector<ws::Crawler::Root> roots;
        for (const ws::EntryId r : index->roots()) {
            const std::string_view name = index->name(r);
            roots.push_back({r, std::wstring(name.begin(), name.end()), 0});
        }
        // Run twice: the first pass picks up changes since the snapshot was
        // saved; the second shows the cost of a no-change sync.
        for (int pass = 1; pass <= 2; ++pass) {
            const std::size_t slotsBefore = index->slotCount();
            timer.restart();
            index->setInterning(true);
            ws::Crawler(rules).sync(*index, roots, ws::WorkerPool::defaultThreadCount(), true, {});
            index->setInterning(false);
            const double syncMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
            ::HeapCompact(::GetProcessHeap(), 0);
            std::printf("sync pass %d  %.0f ms, slots %zu -> %zu, live %zu, private %.1f MB\n", pass, syncMs,
                slotsBefore, index->slotCount(), index->liveCount(), privateMB() - before);
            printHeap("after sync");
        }
    }

    if (args.contains(u"--compact"_s)) {
        // Remove every 8th file (as if deleted from disk), then compact.
        args.removeAll(u"--compact"_s);
        std::size_t removed = 0;
        for (std::size_t i = 0; i < index->slotCount(); i += 8) {
            const auto id = static_cast<ws::EntryId>(i);
            if (!index->entry(id).isDir() && !index->entry(id).isDeleted())
                removed += index->remove(id);
        }
        ::HeapCompact(::GetProcessHeap(), 0);
        const double withGarbage = privateMB() - before;
        timer.restart();
        const std::size_t freed = index->compact();
        const double compactMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        ::HeapCompact(::GetProcessHeap(), 0);
        std::printf("compact      removed %zu files, freed %zu slots in %.0f ms, private %.1f -> %.1f MB\n", removed,
            freed, compactMs, withGarbage, privateMB() - before);
        printHeap("after compact");
    }

    const bool showTop = args.removeAll(u"--top"_s) > 0; // list the first hits of each query
    ws::WorkerPool pool(ws::WorkerPool::defaultThreadCount());
    const auto lock = index->readLock();
    for (const QString& q : args) {
        const ws::NameMatcher matcher(ws::parseQuery(q));
        double best = 1e9;
        ws::NameSearchOutput out;
        for (int run = 0; run < 5; ++run) {
            timer.restart();
            out = ws::searchNames(*index, matcher, 100, pool, {});
            best = std::min(best, static_cast<double>(timer.nsecsElapsed()) / 1e6);
        }
        std::printf("%-16s %9zu matches  %6.1f ms   top: %s\n", qPrintable(q), out.totalMatches, best,
            out.hits.empty() ? "-" : qPrintable(index->path(out.hits.front().id)));
        for (std::size_t i = 0; showTop && i < out.hits.size() && i < 10; ++i)
            std::printf("      %4d  %s\n", out.hits[i].score, index->path(out.hits[i].id).toUtf8().constData());
    }
    return 0;
}
