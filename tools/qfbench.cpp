// Loads the saved index and reports memory use and search timings.
//
//   qfbench [snapshot] [query ...]
//   qfbench --mft      (as administrator) reads every NTFS volume's MFT,
//                      compares the result with a directory walk, and tests
//                      following the change journal on the build folder's volume
//
// Default snapshot: %LOCALAPPDATA%\QuickFind\index.bin
#include "Crawler.h"
#include "NameSearch.h"
#include "Ntfs.h"
#include "NtfsIndexer.h"
#include "Query.h"
#include "Settings.h"
#include "Snapshot.h"
#include "Wtf8.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QStandardPaths>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
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
    return qf::wtf8::fromUtf16(s);
}

int crawlThreads()
{
    return std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 2, 8);
}

// Builds one volume's index from its MFT, as the app does.
bool buildFromMft(qf::FileIndex& index, const qf::Crawler& crawler, const std::wstring& root, double* readMs, double* syncMs)
{
    const qf::EntryId r = index.addRoot(narrow(qf::wtf8::view(root)));
    index.setFolderRecord(r, qf::ntfs::kRootRecord, r);
    index.setInterning(true);
    QElapsedTimer t;
    t.start();
    qf::MftTree tree;
    std::wstring error;
    if (!tree.read(root, index, {}, &error)) {
        std::printf("  MFT read failed: %s\n", narrow(qf::wtf8::view(error)).c_str());
        return false;
    }
    *readMs = msSince(t);
    t.restart();
    crawler.sync(index, {{r, root, 0, r, qf::ntfs::kRootRecord}}, 1, false, {},
        [&](const qf::Crawler::Root& dir, qf::DirListing& out) { return tree.list(index, crawler, dir, out); });
    index.setInterning(false);
    index.compact(true);
    *syncMs = msSince(t);
    return true;
}

// Every live entry by path, with the flags that matter for search.
std::unordered_map<std::u16string, std::uint8_t> pathsOf(const qf::FileIndex& index)
{
    constexpr std::uint8_t kCompared = qf::EntryFlag::Directory | qf::EntryFlag::Hidden | qf::EntryFlag::LowPriority
        | qf::EntryFlag::Offline;
    std::unordered_map<std::u16string, std::uint8_t> out;
    out.reserve(index.liveCount());
    for (std::size_t i = 0; i < index.slotCount(); ++i) {
        const auto id = static_cast<qf::EntryId>(i);
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
void compareIndexes(const qf::FileIndex& mft, const qf::FileIndex& walked)
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
    qf::ntfs::UsnRecord record;
    std::u16string name;
};

// Reads every journal record from `usn` up to now.
std::vector<OwnedRecord> readJournal(const std::wstring& root, const qf::ntfs::JournalInfo& journal, std::int64_t usn)
{
    std::vector<OwnedRecord> out;
    qf::ntfs::JournalReader reader(root, journal.id);
    std::vector<qf::ntfs::UsnRecord> records;
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

void applyAll(qf::UsnApplier& applier, const std::vector<OwnedRecord>& owned)
{
    std::vector<qf::ntfs::UsnRecord> records;
    for (const auto& o : owned)
        records.push_back(o.record);
    applier.apply(records, {});
}

// Paths below `dir` in `index`, relative to it, with flags.
std::map<std::u16string, std::uint8_t> subtree(const qf::FileIndex& index, qf::EntryId dir)
{
    std::map<std::u16string, std::uint8_t> out;
    std::u16string base = index.path16(dir);
    if (!base.ends_with(u'\\'))
        base.push_back(u'\\'); // a root's path already ends in one
    std::vector<qf::EntryId> stack {dir};
    while (!stack.empty()) {
        const qf::EntryId id = stack.back();
        stack.pop_back();
        for (qf::EntryId c = index.entry(id).firstChild; c != qf::kNoEntry; c = index.entry(c).nextSibling) {
            out.emplace(index.path16(c).substr(base.size()),
                static_cast<std::uint8_t>(index.entry(c).flags & (qf::EntryFlag::Directory | qf::EntryFlag::Hidden)));
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
    return ok ? qf::ntfs::recordOf((std::uint64_t {info.nFileIndexHigh} << 32) | info.nFileIndexLow) : 0;
}

void touch(const std::filesystem::path& p)
{
    const HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE)
        ::CloseHandle(h);
}

// Makes real changes in a scratch folder, then checks that applying the
// journal (and replaying it) gives the same tree as walking the folder.
void journalSelfTest(const std::wstring& root, const std::filesystem::path& base, qf::CrawlRules rules)
{
    namespace fs = std::filesystem;
    std::printf("\nchange journal self-test in %s\n", narrow(qf::wtf8::view(base.wstring())).c_str());
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base / L"keep", ec);
    fs::create_directories(base / L"old", ec);
    fs::create_directories(base / L"qf-excluded" / L"incoming" / L"deep", ec);
    touch(base / L"keep" / L"k.txt");
    touch(base / L"old" / L"o.txt");
    touch(base / L"qf-excluded" / L"incoming" / L"deep" / L"x.txt");
    rules.excludedNames.push_back(L"qf-excluded");
    const qf::Crawler crawler(rules);

    qf::FileIndex index;
    double readMs = 0, syncMs = 0;
    if (!buildFromMft(index, crawler, root, &readMs, &syncMs))
        return;
    const auto journal = qf::ntfs::queryJournal(root);
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
    fs::rename(base / L"Old2", base / L"qf-excluded" / L"Old2", ec); // moved out of the index
    fs::rename(base / L"qf-excluded" / L"incoming", base / L"incoming", ec); // moved in: walked
    touch(base / L"incoming" / L"deep" / L"y.txt"); // inside a folder only the walk registered
    fs::create_directories(base / L"gone" / L"sub", ec);
    touch(base / L"gone" / L"sub" / L"z.txt");
    fs::remove_all(base / L"gone", ec); // a whole tree

    QElapsedTimer t;
    t.start();
    const auto records = readJournal(root, *journal, start);
    const double readJournalMs = msSince(t);
    t.restart();
    qf::UsnApplier applier(index, crawler, narrow(qf::wtf8::view(root)));
    applyAll(applier, records);
    std::printf("  %zu records read in %.1f ms, applied in %.1f ms\n", records.size(), readJournalMs, msSince(t));

    // What the folder really holds now.
    qf::FileIndex walked;
    const qf::EntryId wroot = walked.addRoot(narrow(qf::wtf8::view(base.wstring())));
    crawler.sync(walked, {{wroot, base.wstring(), 0}}, 1, false, {});
    const auto want = subtree(walked, wroot);
    const qf::EntryId dir = index.findPath(base.wstring());
    if (dir == qf::kNoEntry) {
        std::printf("  test folder missing from the index\n");
    } else {
        sameTree("journal applied", subtree(index, dir), want);
        qf::UsnApplier replay(index, crawler, narrow(qf::wtf8::view(root)));
        applyAll(replay, records); // as after a restart from an older position
        sameTree("replayed again", subtree(index, dir), want);
        // y.txt was only found because the walk registered "deep"; check it directly too.
        const qf::EntryId deep = index.findPath((base / L"incoming" / L"deep").wstring());
        const std::uint32_t deepRecord = recordOfPath(base / L"incoming" / L"deep");
        std::printf("  walked-in folder registered   %s\n",
            deep != qf::kNoEntry && index.folderByRecord(index.roots().front(), deepRecord) == deep ? "yes" : "NO");
    }
    fs::remove_all(base, ec);
}

int runMftCheck(bool compare)
{
    qf::Settings settings;
    settings.load();
    const qf::CrawlRules rules = settings.crawlRules();
    const qf::Crawler crawler(rules);
    for (const auto& v : qf::listLocalVolumes(false)) {
        std::printf("\n%s  %s\n", narrow(qf::wtf8::view(v.root)).c_str(), v.ntfs ? "NTFS" : "not NTFS");
        if (!v.ntfs)
            continue;
        const auto journal = qf::ntfs::queryJournal(v.root);
        if (journal)
            std::printf("  journal id %llx  first %lld  next %lld  (%.1f MB kept)\n",
                static_cast<unsigned long long>(journal->id), static_cast<long long>(journal->firstUsn),
                static_cast<long long>(journal->nextUsn),
                static_cast<double>(journal->nextUsn - journal->firstUsn) / (1024.0 * 1024.0));
        else
            std::printf("  no change journal (error %lu)\n", ::GetLastError());
        {
            qf::ntfs::MftReader reader(v.root);
            if (!reader.valid()) {
                std::printf("  MFT: %s\n", narrow(qf::wtf8::view(reader.error())).c_str());
                continue;
            }
            QElapsedTimer t;
            t.start();
            double parseMs = 0;
            std::size_t inUse = 0;
            std::size_t freeBlocks = 0; // 4 MB blocks without a record in use
            qf::ntfs::FileRecord record;
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
        qf::FileIndex mft;
        double readMs = 0, syncMs = 0;
        if (!buildFromMft(mft, crawler, v.root, &readMs, &syncMs))
            continue;
        ::HeapCompact(::GetProcessHeap(), 0);
        std::printf("  index from MFT: %zu items, read %.0f ms + build %.0f ms, %.1f MB (peak commit %.0f MB)\n",
            mft.liveCount(), readMs, syncMs, privateMB() - before, peakCommitMB());
        const qf::RecordTable* records = mft.folderRecords(mft.roots().front());
        std::printf("  folder records %zu\n", records ? records->size() : 0);
        if (!compare)
            continue;
        qf::FileIndex walked;
        const qf::EntryId r = walked.addRoot(narrow(qf::wtf8::view(v.root)));
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
    QString path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/QuickFind/index.bin"_s;
    if (!args.isEmpty() && args.first().endsWith(u".bin"))
        path = args.takeFirst();
    if (args.isEmpty())
        args = {u"report"_s, u"a"_s, u"readme.md"_s, u"*.pdf"_s, u"ext:docx 合同"_s, u"src\\main"_s, u"zzzzqx"_s};

    const double before = privateMB();
    QElapsedTimer timer;
    timer.start();
    auto contents = qf::snapshot::load(path);
    qf::FileIndex* index = contents ? contents->index.get() : nullptr;
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
        after - before, static_cast<double>(chunks * qf::FileIndex::kChunkSize * sizeof(qf::Entry)) / (1024.0 * 1024.0),
        chunks, (after - before) * 1024.0 * 1024.0 / static_cast<double>(index->liveCount()));
    printHeap("after load");

    if (args.contains(u"--sync"_s)) {
        // Re-walk all drives in place, as the app does after startup.
        args.removeAll(u"--sync"_s);
        // The app's rules (from the user's settings), so the walk matches the app's startup rescan.
        qf::Settings settings;
        settings.load();
        const qf::CrawlRules rules = settings.crawlRules();
        std::vector<qf::Crawler::Root> roots;
        for (const qf::EntryId r : index->roots()) {
            const std::string_view name = index->name(r);
            roots.push_back({r, std::wstring(name.begin(), name.end()), 0});
        }
        // Run twice: the first pass picks up changes since the snapshot was
        // saved; the second shows the cost of a no-change sync.
        for (int pass = 1; pass <= 2; ++pass) {
            const std::size_t slotsBefore = index->slotCount();
            timer.restart();
            index->setInterning(true);
            qf::Crawler(rules).sync(*index, roots, qf::WorkerPool::defaultThreadCount(), true, {});
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
            const auto id = static_cast<qf::EntryId>(i);
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
    qf::WorkerPool pool(qf::WorkerPool::defaultThreadCount());
    const auto lock = index->readLock();
    for (const QString& q : args) {
        const qf::NameMatcher matcher(qf::parseQuery(q));
        double best = 1e9;
        qf::NameSearchOutput out;
        for (int run = 0; run < 5; ++run) {
            timer.restart();
            out = qf::searchNames(*index, matcher, qf::Scope::All, 100, pool, {});
            best = std::min(best, static_cast<double>(timer.nsecsElapsed()) / 1e6);
        }
        std::printf("%-16s %9zu matches  %6.1f ms   top: %s\n", qPrintable(q), out.totalMatches, best,
            out.hits.empty() ? "-" : qPrintable(index->path(out.hits.front().id)));
        for (std::size_t i = 0; showTop && i < out.hits.size() && i < 10; ++i)
            std::printf("      %4d  %s\n", out.hits[i].score, index->path(out.hits[i].id).toUtf8().constData());
    }
    return 0;
}
