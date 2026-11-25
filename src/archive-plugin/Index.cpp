#include "Index.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

#include <windows.h>

namespace wsarchive {

namespace {

constexpr std::uint32_t kMagic = 0x52415357; // "WSAR"
constexpr std::uint32_t kVersion = 2; // 2: copies of an archive share their contents
constexpr std::uint32_t kNoContents = 0xFFFF'FFFF;
constexpr std::size_t kMostHits = 20'000; // looked at for the best of them
constexpr std::size_t kMostWords = 16;
constexpr std::size_t kMaxPath = 32 * 1024;

constexpr std::array<unsigned char, 256> kFold = [] {
    std::array<unsigned char, 256> table {};
    for (std::size_t i = 0; i < table.size(); ++i)
        table[i] = static_cast<unsigned char>(i >= 'A' && i <= 'Z' ? i + ('a' - 'A') : i);
    return table;
}();

std::string_view fileNameOf(std::string_view path)
{
    const std::size_t slash = path.find_last_of('\\');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// The type of a member by its name, lower case; empty for none.
std::string extensionOf(std::string_view name)
{
    const std::size_t dot = name.find_last_of('.');
    return dot == std::string_view::npos || dot == 0 || dot + 1 == name.size() ? std::string() : folded(name.substr(dot + 1));
}

bool under(std::string_view path, const std::vector<std::string>& folders)
{
    return std::ranges::any_of(folders, [&](const std::string& folder) {
        if (folder.empty() || path.size() <= folder.size())
            return false;
        const bool separated = folder.back() == '\\' || path[folder.size()] == '\\';
        return separated && findFolded(path.substr(0, folder.size()), folded(folder)) == 0;
    });
}

// FNV-1a over the names (and how many): which contents may be the same.
std::uint64_t hashOf(const Contents& contents)
{
    std::uint64_t h = 1469598103934665603ull ^ contents.count();
    for (const char c : contents.names) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h;
}

// What tells a copy of an archive: its name, its size and its time.
std::string copyKey(std::string_view path, std::uint64_t size, std::int64_t modified)
{
    return folded(fileNameOf(path)) + '|' + std::to_string(size) + '|' + std::to_string(modified);
}

using Shared = std::shared_ptr<const Contents>;
using Pool = std::unordered_multimap<std::uint64_t, std::weak_ptr<const Contents>>;
using Copies = std::unordered_map<std::string, std::weak_ptr<const Contents>>;
using Entries = std::unordered_map<std::string, std::shared_ptr<const Entry>>;

// `contents` as the pool holds it: the one already there when it is the same.
Shared pooledIn(Pool& pool, Contents contents, std::size_t* added)
{
    const std::uint64_t hash = hashOf(contents);
    const auto [first, last] = pool.equal_range(hash);
    for (auto it = first; it != last;) {
        const Shared held = it->second.lock();
        if (!held) {
            it = pool.erase(it); // its archives are gone
            continue;
        }
        if (*held == contents) {
            if (added)
                *added = 0;
            return held;
        }
        ++it;
    }
    if (added)
        *added = contents.names.size();
    contents.names.shrink_to_fit();
    contents.sizes.shrink_to_fit();
    contents.times.shrink_to_fit();
    Shared fresh = std::make_shared<const Contents>(std::move(contents));
    pool.emplace(hash, fresh);
    return fresh;
}

void insertIn(Entries& entries, Copies& copies, Entry entry, Shared contents)
{
    entry.contents = std::move(contents);
    // Only what was read whole speaks for its copies, and only by a size and time that are known.
    if (entry.outcome == Outcome::Listed && entry.size > 0 && entry.modified > 0)
        copies.insert_or_assign(copyKey(entry.path, entry.size, entry.modified), entry.contents);
    std::string path = entry.path;
    entries.insert_or_assign(std::move(path), std::make_shared<const Entry>(std::move(entry)));
}

// ---- the file ---------------------------------------------------------------------
//
//   u32 magic, u32 version, u32 contents, u32 entries
//   contents × (u32 members, u32 nameBytes, names, members × u64, members × u32)
//   entries × (u32 pathBytes, path, u64 size, i64 modified, u8 outcome, u32 contents: which, or kNoContents)

template <typename T> void append(std::string& out, T value)
{
    char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    out.append(bytes, sizeof(T));
}

// The file, read as it goes: nothing more than is left in it is ever asked
// for, whatever a length in it says.
class Bytes {
public:
    Bytes(std::ifstream& in, std::uint64_t size)
        : m_in(in)
        , m_left(size)
    {
    }
    template <typename T> bool get(T& value)
    {
        return read(reinterpret_cast<char*>(&value), sizeof(T));
    }
    template <typename C> bool take(std::size_t count, C& out)
    {
        const std::uint64_t bytes = std::uint64_t {count} * sizeof(typename C::value_type);
        if (bytes > m_left)
            return false;
        out.resize(count);
        return count == 0 || read(reinterpret_cast<char*>(out.data()), static_cast<std::size_t>(bytes));
    }
    bool atEnd() const noexcept { return m_left == 0; }

private:
    bool read(char* to, std::size_t n)
    {
        if (n > m_left || !m_in.read(to, static_cast<std::streamsize>(n)))
            return false;
        m_left -= n;
        return true;
    }
    std::ifstream& m_in;
    std::uint64_t m_left;
};

bool read(Bytes& in, Contents& contents)
{
    std::uint32_t members = 0, nameBytes = 0;
    if (!in.get(members) || !in.get(nameBytes) || !in.take(nameBytes, contents.names) || !in.take(members, contents.sizes)
        || !in.take(members, contents.times))
        return false;
    return static_cast<std::size_t>(std::ranges::count(contents.names, '\n')) == members
        && (contents.names.empty() || contents.names.back() == '\n');
}

bool read(Bytes& in, Entry& entry, std::uint32_t& contents)
{
    std::uint32_t pathBytes = 0;
    std::uint8_t outcome = 0;
    if (!in.get(pathBytes) || pathBytes == 0 || pathBytes > kMaxPath || !in.take(pathBytes, entry.path) || !in.get(entry.size)
        || !in.get(entry.modified) || !in.get(outcome) || outcome >= kOutcomes || !in.get(contents))
        return false;
    entry.outcome = static_cast<Outcome>(outcome);
    return true;
}

// ---- searching --------------------------------------------------------------------

// A member that matched: where its line is in the names.
struct Line {
    std::uint32_t index; // among the members
    std::uint32_t start;
    std::uint32_t length;
    std::uint32_t nameLength; // of its own name, the last part
    int rank;
};

// The members of `contents` whose path holds every needle of `must` (bit i:
// needles[i]); with no bit set, those that hold any one needle.
std::vector<Line> matching(const Contents& contents, const std::vector<std::string>& needles, std::uint64_t must, const Where& where,
    std::size_t most)
{
    std::vector<Line> lines;
    const std::string_view names = contents.names;
    const bool anyOne = must == 0;
    std::unordered_set<std::uint32_t> seen; // anyOne: a member two of the words are in
    std::size_t counted = 0; // lines before `countedAt`
    std::size_t countedAt = 0;
    const auto look = [&](std::size_t anchor) {
        std::size_t at = 0;
        while (lines.size() < most && (at = findFolded(names, needles[anchor], at)) != std::string_view::npos) {
            const std::size_t before = names.rfind('\n', at);
            const std::size_t start = before == std::string_view::npos ? 0 : before + 1;
            const std::size_t end = names.find('\n', at);
            if (end == std::string_view::npos)
                break;
            const std::string_view line = names.substr(start, end - start);
            at = end + 1;
            if (!anyOne) {
                bool all = true;
                for (std::size_t i = 0; i < needles.size() && all; ++i)
                    all = i == anchor || !(must >> i & 1) || findFolded(line, needles[i]) != std::string_view::npos;
                if (!all)
                    continue;
            }
            if (start < countedAt) { // an earlier anchor went past it (anyOne): counted from the top again
                counted = 0;
                countedAt = 0;
            }
            counted += static_cast<std::size_t>(std::ranges::count(names.substr(countedAt, start - countedAt), '\n'));
            countedAt = start;
            const auto index = static_cast<std::uint32_t>(counted);
            if (anyOne && !seen.insert(index).second)
                continue;
            const std::string_view own = fileNameOf(line);
            if (!where.extensions.empty()) {
                const std::string type = (contents.sizes[index] & Contents::kFolder) ? std::string() : extensionOf(own);
                if (std::ranges::find(where.extensions, type) == where.extensions.end())
                    continue;
            }
            const bool inOwnName = std::ranges::all_of(needles, [&](const std::string& w) {
                return findFolded(own, w) != std::string_view::npos;
            });
            lines.push_back({index, static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(line.size()),
                static_cast<std::uint32_t>(own.size()), inOwnName ? 0 : 1});
        }
    };
    if (anyOne) {
        for (std::size_t i = 0; i < needles.size(); ++i)
            look(i);
    } else {
        // The first of them: the longest (search() sorts them so), the rarest by and large.
        std::size_t anchor = 0;
        while (!(must >> anchor & 1))
            ++anchor;
        look(anchor);
    }
    return lines;
}

} // namespace

std::string folded(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(kFold[static_cast<unsigned char>(c)]);
    return out;
}

std::size_t findFolded(std::string_view hay, std::string_view needle, std::size_t from) noexcept
{
    if (needle.empty())
        return from <= hay.size() ? from : std::string_view::npos;
    if (hay.size() < needle.size())
        return std::string_view::npos;
    const auto* h = reinterpret_cast<const unsigned char*>(hay.data());
    const auto* n = reinterpret_cast<const unsigned char*>(needle.data());
    const unsigned char first = n[0];
    const std::size_t last = hay.size() - needle.size();
    for (std::size_t i = from; i <= last; ++i) {
        if (kFold[h[i]] != first)
            continue;
        std::size_t k = 1;
        while (k < needle.size() && kFold[h[i + k]] == n[k])
            ++k;
        if (k == needle.size())
            return i;
    }
    return std::string_view::npos;
}

std::vector<std::string> wordsOf(std::string_view text)
{
    std::vector<std::string> words;
    std::string word;
    bool quoted = false;
    const auto end = [&] {
        if (!word.empty())
            words.push_back(std::move(word));
        word.clear();
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') {
            quoted = !quoted;
            if (!quoted)
                end();
        } else if (!quoted && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
            end();
        } else if (!quoted && text.compare(i, 3, "\xE3\x80\x80") == 0) { // the full-width space a Chinese keyboard types
            end();
            i += 2;
        } else {
            word += c;
        }
    }
    end();
    return words;
}

void Contents::add(const Member& member)
{
    names += member.path;
    names += '\n';
    sizes.push_back((member.size & kSize) | (member.folder ? kFolder : 0) | (member.locked ? kLocked : 0));
    times.push_back(member.modified > 0 && member.modified <= 0xFFFF'FFFFll ? static_cast<std::uint32_t>(member.modified) : 0);
}

bool Index::load(const std::wstring& file)
{
    Entries entries;
    Pool pool;
    Copies copies;
    bool ok = false;
    if (std::ifstream stream(std::filesystem::path(file), std::ios::binary | std::ios::ate); stream) {
        const std::streamoff size = stream.tellg();
        stream.seekg(0);
        Bytes in(stream, size > 0 ? static_cast<std::uint64_t>(size) : 0);
        std::uint32_t magic = 0, version = 0, contentsCount = 0, entryCount = 0;
        if (in.get(magic) && magic == kMagic && in.get(version) && version == kVersion && in.get(contentsCount) && in.get(entryCount)) {
            ok = true;
            std::vector<Shared> held;
            for (std::uint32_t i = 0; i < contentsCount && ok; ++i) {
                Contents contents;
                ok = read(in, contents);
                if (ok)
                    held.push_back(pooledIn(pool, std::move(contents), nullptr));
            }
            const Shared none = pooledIn(pool, {}, nullptr);
            for (std::uint32_t i = 0; i < entryCount && ok; ++i) {
                Entry entry;
                std::uint32_t which = kNoContents;
                ok = read(in, entry, which) && (which == kNoContents || which < held.size());
                if (ok)
                    insertIn(entries, copies, std::move(entry), which == kNoContents ? none : held[which]);
            }
            ok = ok && in.atEnd();
        }
    }
    if (!ok) {
        entries.clear();
        pool.clear();
        copies.clear();
    }
    const std::unique_lock lock(m_mutex);
    m_entries = std::move(entries);
    m_pool = std::move(pool);
    m_copies = std::move(copies);
    m_changed = false;
    return ok;
}

bool Index::save(const std::wstring& file) const
{
    std::vector<std::shared_ptr<const Entry>> entries;
    {
        const std::shared_lock lock(m_mutex);
        entries.reserve(m_entries.size());
        for (const auto& [path, entry] : m_entries)
            entries.push_back(entry);
        m_changed = false; // what is put from now on is not in this file
    }
    // Each contents once, in the order its first archive comes.
    std::unordered_map<const Contents*, std::uint32_t> numbers;
    std::vector<const Contents*> contents;
    for (const auto& entry : entries) {
        if (entry->count() > 0 && numbers.try_emplace(entry->contents.get(), static_cast<std::uint32_t>(contents.size())).second)
            contents.push_back(entry->contents.get());
    }
    const std::wstring partial = file + L".tmp";
    bool ok = false;
    {
        std::ofstream out(std::filesystem::path(partial), std::ios::binary | std::ios::trunc);
        std::string head;
        append(head, kMagic);
        append(head, kVersion);
        append(head, static_cast<std::uint32_t>(contents.size()));
        append(head, static_cast<std::uint32_t>(entries.size()));
        out.write(head.data(), static_cast<std::streamsize>(head.size()));
        for (const Contents* c : contents) {
            std::string record;
            append(record, static_cast<std::uint32_t>(c->count()));
            append(record, static_cast<std::uint32_t>(c->names.size()));
            out.write(record.data(), static_cast<std::streamsize>(record.size()));
            out.write(c->names.data(), static_cast<std::streamsize>(c->names.size()));
            out.write(reinterpret_cast<const char*>(c->sizes.data()), static_cast<std::streamsize>(c->sizes.size() * sizeof(std::uint64_t)));
            out.write(reinterpret_cast<const char*>(c->times.data()), static_cast<std::streamsize>(c->times.size() * sizeof(std::uint32_t)));
        }
        for (const auto& entry : entries) {
            std::string record;
            record.reserve(entry->path.size() + 32);
            append(record, static_cast<std::uint32_t>(entry->path.size()));
            record += entry->path;
            append(record, entry->size);
            append(record, entry->modified);
            append(record, static_cast<std::uint8_t>(entry->outcome));
            const auto number = numbers.find(entry->contents.get());
            append(record, number == numbers.end() ? kNoContents : number->second);
            out.write(record.data(), static_cast<std::streamsize>(record.size()));
        }
        out.flush();
        ok = static_cast<bool>(out);
    }
    if (ok)
        ok = ::MoveFileExW(partial.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
        ::DeleteFileW(partial.c_str());
        m_changed = true;
    }
    return ok;
}

std::shared_ptr<const Entry> Index::find(const std::string& path) const
{
    const std::shared_lock lock(m_mutex);
    const auto it = m_entries.find(path);
    return it == m_entries.end() ? nullptr : it->second;
}

Index::Shared Index::pooled(Contents contents, std::size_t* added)
{
    return pooledIn(m_pool, std::move(contents), added);
}

void Index::insert(Entry entry, Shared contents)
{
    insertIn(m_entries, m_copies, std::move(entry), std::move(contents));
    m_changed = true;
}

std::size_t Index::put(Entry entry, Contents contents)
{
    std::size_t added = 0;
    const std::unique_lock lock(m_mutex);
    insert(std::move(entry), pooled(std::move(contents), &added));
    return added;
}

void Index::put(Entry entry, std::shared_ptr<const Contents> contents)
{
    const std::unique_lock lock(m_mutex);
    insert(std::move(entry), contents ? std::move(contents) : pooled({}, nullptr));
}

std::shared_ptr<const Contents> Index::copyOf(std::string_view path, std::uint64_t size, std::int64_t modified) const
{
    if (size == 0 || modified <= 0)
        return nullptr;
    const std::shared_lock lock(m_mutex);
    const auto it = m_copies.find(copyKey(path, size, modified));
    return it == m_copies.end() ? nullptr : it->second.lock();
}

std::size_t Index::keepOnly(const std::unordered_set<std::string>& paths)
{
    const std::unique_lock lock(m_mutex);
    const std::size_t gone = std::erase_if(m_entries, [&](const auto& item) { return !paths.contains(item.first); });
    if (gone) {
        // What only they held is gone with them.
        std::erase_if(m_pool, [](const auto& item) { return item.second.expired(); });
        std::erase_if(m_copies, [](const auto& item) { return item.second.expired(); });
        m_changed = true;
    }
    return gone;
}

void Index::clear()
{
    const std::unique_lock lock(m_mutex);
    if (!m_entries.empty())
        m_changed = true;
    m_entries.clear();
    m_pool.clear();
    m_copies.clear();
}

Totals Index::totals() const
{
    Totals totals;
    std::unordered_set<const Contents*> counted;
    const std::shared_lock lock(m_mutex);
    totals.archives = m_entries.size();
    for (const auto& [path, entry] : m_entries) {
        totals.members += entry->count();
        if (counted.insert(entry->contents.get()).second)
            totals.nameBytes += entry->contents->names.size();
        ++totals.byOutcome[static_cast<std::size_t>(entry->outcome)];
    }
    return totals;
}

Found Index::search(const std::vector<std::string>& words, std::size_t limit, const Where& where) const
{
    Found found;
    std::vector<std::string> needles;
    for (const std::string& word : words) {
        std::string needle = folded(word);
        std::ranges::replace(needle, '/', '\\'); // paths are kept with '\'
        if (!needle.empty() && needles.size() < kMostWords && std::ranges::find(needles, needle) == needles.end())
            needles.push_back(std::move(needle));
    }
    if (needles.empty() || limit == 0)
        return found;
    // The longest first: the rarest, by and large, and so the fewest lines to look at.
    std::ranges::stable_sort(needles, [](const std::string& a, const std::string& b) { return a.size() > b.size(); });

    std::vector<std::shared_ptr<const Entry>> entries;
    {
        const std::shared_lock lock(m_mutex);
        entries.reserve(m_entries.size());
        for (const auto& [path, entry] : m_entries) {
            if (entry->count() > 0 && (where.folders.empty() || under(entry->path, where.folders)))
                entries.push_back(entry);
        }
    }

    // In the order of their paths: the same answer every time, and which of
    // several copies counts as the first.
    std::ranges::sort(entries, {}, [](const std::shared_ptr<const Entry>& entry) -> const std::string& { return entry->path; });

    struct Scored {
        const Entry* entry;
        const Line* line;
        std::uint32_t copy; // how many archives before it hold the same contents
    };
    std::vector<Scored> scored;
    std::unordered_map<const Contents*, std::uint32_t> copies;
    // The contents of copies are looked through once: by the contents and
    // by which of the words they have to hold (the others are in the
    // archive's own name, which copies share as a rule).
    std::map<std::pair<const Contents*, std::uint64_t>, std::vector<Line>> looked;
    for (const auto& entry : entries) {
        if (scored.size() >= kMostHits)
            break;
        const std::string_view archiveName = fileNameOf(entry->path);
        // Every word in the archive's own name leaves none (0): then one of
        // them in the member's path, or the whole archive would be listed.
        std::uint64_t must = 0;
        for (std::size_t i = 0; i < needles.size(); ++i) {
            if (findFolded(archiveName, needles[i]) == std::string_view::npos)
                must |= std::uint64_t {1} << i;
        }
        const auto [at, fresh] = looked.try_emplace({entry->contents.get(), must});
        if (fresh)
            at->second = matching(*entry->contents, needles, must, where, kMostHits);
        const std::uint32_t copy = copies[entry->contents.get()]++;
        for (const Line& line : at->second) {
            if (scored.size() >= kMostHits)
                break;
            scored.push_back({entry.get(), &line, copy});
        }
    }

    // A member once before its copies in other archives: the first rows
    // (the three that 全部 shows) are not one file three times over.
    const auto better = [](const Scored& a, const Scored& b) {
        if (a.line->rank != b.line->rank)
            return a.line->rank < b.line->rank;
        if (a.copy != b.copy)
            return a.copy < b.copy;
        if (a.line->nameLength != b.line->nameLength)
            return a.line->nameLength < b.line->nameLength;
        if (const int c = a.entry->path.compare(b.entry->path))
            return c < 0;
        return a.line->index < b.line->index;
    };
    found.more = scored.size() > limit;
    const std::size_t take = std::min(limit, scored.size());
    std::partial_sort(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(take), scored.end(), better);
    scored.resize(take);
    found.hits.reserve(take);
    for (const Scored& s : scored) {
        const Contents& contents = *s.entry->contents;
        Hit hit;
        hit.archive = s.entry->path;
        hit.member.assign(contents.names, s.line->start, s.line->length);
        hit.index = s.line->index;
        const std::uint64_t packed = contents.sizes[s.line->index];
        hit.size = packed & Contents::kSize;
        hit.folder = (packed & Contents::kFolder) != 0;
        hit.locked = (packed & Contents::kLocked) != 0;
        hit.modified = contents.times[s.line->index];
        hit.rank = s.line->rank;
        found.hits.push_back(std::move(hit));
    }
    return found;
}

} // namespace wsarchive
