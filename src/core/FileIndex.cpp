#include "FileIndex.h"

#include "Pinyin.h"
#include "TextUtil.h"
#include "Wtf8.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <cwctype>

namespace qf {

namespace {

std::uint8_t hanFlag(std::string_view name) noexcept
{
    return pinyin::hasHan(name) ? EntryFlag::Han : 0;
}

// Children a writer's lookup walks through before it builds a ChildTable.
constexpr std::size_t kChildTableMin = 256;

// FNV-1a of the ASCII-folded name: spellings that differ in case collide on purpose.
std::size_t foldedHash(std::string_view name) noexcept
{
    std::uint64_t h = 14695981039346656037ull;
    for (const char c : name)
        h = (h ^ text::fold(c)) * 1099511628211ull;
    return static_cast<std::size_t>(h ^ (h >> 29));
}

} // namespace

std::uint8_t extensionLength(std::string_view name, bool isDir) noexcept
{
    if (isDir)
        return 0;
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= name.size())
        return 0;
    const std::size_t len = name.size() - dot - 1;
    return len > 255 ? 0 : static_cast<std::uint8_t>(len);
}

std::size_t RecordTable::home(std::uint32_t record) const noexcept
{
    // Record numbers are dense: scatter them (Fibonacci hashing).
    return static_cast<std::size_t>((std::uint64_t {record} * 0x9E3779B97F4A7C15ull) >> 32) & (m_slots.size() - 1);
}

EntryId RecordTable::find(std::uint32_t record) const noexcept
{
    if (m_slots.empty() || record == 0)
        return kNoEntry;
    const std::size_t mask = m_slots.size() - 1;
    for (std::size_t i = home(record); m_slots[i].record != 0; i = (i + 1) & mask) {
        if (m_slots[i].record == record)
            return m_slots[i].id;
    }
    return kNoEntry;
}

void RecordTable::set(std::uint32_t record, EntryId id)
{
    assert(record != 0);
    if ((m_count + 1) * 4 > m_slots.size() * 3) {
        std::vector<Slot> old(std::max<std::size_t>(m_slots.size() * 2, 1024));
        old.swap(m_slots);
        m_count = 0;
        for (const Slot& s : old) {
            if (s.record != 0)
                set(s.record, s.id);
        }
    }
    const std::size_t mask = m_slots.size() - 1;
    std::size_t i = home(record);
    while (m_slots[i].record != 0 && m_slots[i].record != record)
        i = (i + 1) & mask;
    if (m_slots[i].record == 0)
        ++m_count;
    m_slots[i] = {record, id};
}

void RecordTable::erase(std::uint32_t record) noexcept
{
    if (m_slots.empty() || record == 0)
        return;
    const std::size_t mask = m_slots.size() - 1;
    std::size_t i = home(record);
    while (m_slots[i].record != record) {
        if (m_slots[i].record == 0)
            return;
        i = (i + 1) & mask;
    }
    // Backward-shift deletion: pull later slots of the same run into the gap
    // when their home lies at or before it, so lookups never stop early.
    for (std::size_t j = (i + 1) & mask; m_slots[j].record != 0; j = (j + 1) & mask) {
        const std::size_t h = home(m_slots[j].record);
        const bool movable = i <= j ? (h <= i || h > j) : (h <= i && h > j);
        if (movable) {
            m_slots[i] = m_slots[j];
            i = j;
        }
    }
    m_slots[i] = Slot {};
    --m_count;
}

FileIndex::FileIndex() = default;
FileIndex::~FileIndex() = default;

std::span<const Entry> FileIndex::chunk(std::size_t index) const noexcept
{
    const std::size_t first = index << kChunkBits;
    const std::size_t n = std::min(kChunkSize, m_count - first);
    return {m_chunks[index].get(), n};
}

EntryId FileIndex::allocate()
{
    if ((m_count >> kChunkBits) == m_chunks.size())
        m_chunks.push_back(std::make_unique<Entry[]>(kChunkSize));
    assert(m_count < kNoEntry);
    return static_cast<EntryId>(m_count++);
}

namespace {

constexpr std::uint64_t packSlot(std::uint32_t offset, std::size_t length, std::uint16_t tag) noexcept
{
    return (std::uint64_t {offset} << 32) | (std::uint64_t {static_cast<std::uint16_t>(length)} << 16) | tag;
}
constexpr std::uint32_t slotOffset(std::uint64_t s) noexcept
{
    return static_cast<std::uint32_t>(s >> 32);
}
constexpr std::size_t slotLength(std::uint64_t s) noexcept
{
    return static_cast<std::uint16_t>(s >> 16);
}
constexpr std::uint16_t slotTag(std::uint64_t s) noexcept
{
    return static_cast<std::uint16_t>(s);
}
constexpr std::uint16_t tagOf(std::size_t hash) noexcept
{
    return static_cast<std::uint16_t>(hash >> 48);
}

std::size_t hashName(std::string_view name) noexcept
{
    return std::hash<std::string_view> {}(name);
}

} // namespace

void FileIndex::setInterning(bool enabled)
{
    if (!enabled) {
        m_intern = decltype(m_intern)(); // not `= {}`: that keeps the capacity
        m_internCount = 0;
        return;
    }
    if (!m_intern.empty())
        return;
    // Sized for roughly half the names being unique, at most 75% full.
    std::size_t capacity = 1u << 16;
    while (capacity * 3 / 4 < liveCount() / 2)
        capacity <<= 1;
    m_intern.assign(capacity, 0);
    m_internCount = 0;
    // Seed with existing names so new entries can share them.
    for (std::size_t i = 0; i < m_count; ++i) {
        const Entry& e = entry(static_cast<EntryId>(i));
        if (e.isDeleted())
            continue;
        const std::string_view n = name(e);
        const std::size_t h = hashName(n);
        const std::size_t mask = m_intern.size() - 1;
        bool known = false;
        for (std::size_t s = h & mask; m_intern[s] != 0; s = (s + 1) & mask) {
            const std::uint64_t slot = m_intern[s];
            if (slotLength(slot) == n.size() && slotTag(slot) == tagOf(h) && nameAt(slotOffset(slot), n.size()) == n) {
                known = true;
                break;
            }
        }
        if (!known)
            internInsert(packSlot(e.nameOffset, n.size(), tagOf(h)), h);
    }
}

std::size_t FileIndex::compact(bool always)
{
    assert(m_intern.empty());
    const std::size_t oldCount = m_count;
    const std::size_t live = liveCount();
    if (live == oldCount && !always)
        return 0;

    // New id = live slots before the old one, from a live bitmap plus a
    // running count per 64 slots: ~0.2 bytes per slot instead of a 4-byte map.
    const std::size_t words = (oldCount + 63) / 64;
    std::vector<std::uint64_t> liveBits(words, 0);
    std::vector<EntryId> liveBefore(words, 0);
    for (std::size_t i = 0; i < oldCount; ++i) {
        if (!entry(static_cast<EntryId>(i)).isDeleted())
            liveBits[i >> 6] |= std::uint64_t {1} << (i & 63);
    }
    EntryId running = 0;
    for (std::size_t w = 0; w < words; ++w) {
        liveBefore[w] = running;
        running += static_cast<EntryId>(std::popcount(liveBits[w]));
    }
    const auto renumber = [&](EntryId id) -> EntryId {
        if (id == kNoEntry)
            return kNoEntry;
        const std::uint64_t bits = liveBits[id >> 6];
        const std::uint64_t bit = std::uint64_t {1} << (id & 63);
        if (!(bits & bit))
            return kNoEntry; // a removed item; live entries never link to one
        return liveBefore[id >> 6] + static_cast<EntryId>(std::popcount(bits & (bit - 1)));
    };

    // Each name still in use moves to a fresh pool once, so entries that
    // shared a name keep sharing it. Open addressing: (old + 1) << 32 | new.
    std::vector<std::unique_ptr<char[]>> oldNames;
    oldNames.swap(m_names);
    m_nameUsed = kNameChunkSize; // the next name starts a new chunk
    std::vector<std::uint64_t> moved(std::bit_ceil(std::max<std::size_t>(live / 2, 1024)), 0);
    std::size_t movedCount = 0;
    const auto slotFor = [](const std::vector<std::uint64_t>& table, std::uint64_t key) {
        const std::size_t mask = table.size() - 1;
        std::size_t s = static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> 32) & mask;
        while (table[s] != 0 && (table[s] >> 32) != key)
            s = (s + 1) & mask;
        return s;
    };
    const auto moveName = [&](const Entry& e) -> std::uint32_t {
        const std::uint64_t key = std::uint64_t {e.nameOffset} + 1;
        std::size_t s = slotFor(moved, key);
        if (moved[s] != 0)
            return static_cast<std::uint32_t>(moved[s]);
        if ((movedCount + 1) * 4 > moved.size() * 3) {
            std::vector<std::uint64_t> bigger(moved.size() * 2, 0);
            for (const std::uint64_t m : moved) {
                if (m != 0)
                    bigger[slotFor(bigger, m >> 32)] = m;
            }
            moved.swap(bigger);
            s = slotFor(moved, key);
        }
        const char* old = oldNames[e.nameOffset >> kNameChunkBits].get() + (e.nameOffset & (kNameChunkSize - 1));
        const std::uint32_t offset = appendName({old, e.nameLength});
        moved[s] = (key << 32) | offset;
        ++movedCount;
        return offset;
    };

    // Ascending, every live entry moves down or stays, onto a slot already read.
    for (std::size_t i = 0; i < oldCount; ++i) {
        const auto id = static_cast<EntryId>(i);
        if (entry(id).isDeleted())
            continue;
        Entry e = entry(id);
        e.parent = renumber(e.parent);
        e.firstChild = renumber(e.firstChild);
        e.nextSibling = renumber(e.nextSibling);
        e.prevSibling = renumber(e.prevSibling);
        e.nameOffset = moveName(e);
        mut(renumber(id)) = e;
    }
    for (EntryId& r : m_roots)
        r = renumber(r);
    std::erase_if(m_folderRecords, [&](auto& t) {
        t.first = renumber(t.first);
        t.second.remap(renumber);
        return t.first == kNoEntry;
    });

    m_count = live;
    m_chunks.resize((live + kChunkSize - 1) >> kChunkBits); // frees the emptied chunks
    m_chunks.shrink_to_fit();
    m_childTables = decltype(m_childTables)(); // by old ids; rebuilt on demand
    return oldCount - live;
}

void FileIndex::internInsert(std::uint64_t slot, std::size_t hash) noexcept
{
    if ((m_internCount + 1) * 4 > m_intern.size() * 3) {
        std::vector<std::uint64_t> old(m_intern.size() * 2, 0);
        old.swap(m_intern);
        m_internCount = 0;
        for (const std::uint64_t s : old) {
            if (s != 0)
                internInsert(s, hashName(nameAt(slotOffset(s), slotLength(s))));
        }
    }
    const std::size_t mask = m_intern.size() - 1;
    std::size_t i = hash & mask;
    while (m_intern[i] != 0)
        i = (i + 1) & mask;
    m_intern[i] = slot;
    ++m_internCount;
}

std::uint32_t FileIndex::storeName(std::string_view name)
{
    if (m_intern.empty())
        return appendName(name);
    const std::size_t h = hashName(name);
    const std::size_t mask = m_intern.size() - 1;
    for (std::size_t i = h & mask; m_intern[i] != 0; i = (i + 1) & mask) {
        const std::uint64_t slot = m_intern[i];
        if (slotLength(slot) == name.size() && slotTag(slot) == tagOf(h)
            && nameAt(slotOffset(slot), name.size()) == name)
            return slotOffset(slot);
    }
    const std::uint32_t offset = appendName(name);
    internInsert(packSlot(offset, name.size(), tagOf(h)), h);
    return offset;
}

std::uint32_t FileIndex::appendName(std::string_view name)
{
    if (m_nameUsed + name.size() > kNameChunkSize) {
        // Padding lets SIMD name matching read a little past the last name.
        m_names.push_back(std::make_unique<char[]>(kNameChunkSize + text::kReadPastEnd));
        m_nameUsed = 0;
    }
    const auto offset = static_cast<std::uint32_t>(((m_names.size() - 1) << kNameChunkBits) + m_nameUsed);
    std::memcpy(m_names.back().get() + m_nameUsed, name.data(), name.size());
    m_nameUsed += name.size();
    return offset;
}

void FileIndex::link(EntryId parent, EntryId child)
{
    Entry& c = mut(child);
    Entry& p = mut(parent);
    c.parent = parent;
    c.prevSibling = kNoEntry;
    c.nextSibling = p.firstChild;
    if (p.firstChild != kNoEntry)
        mut(p.firstChild).prevSibling = child;
    p.firstChild = child;
    if (!m_childTables.empty()) {
        if (const auto it = m_childTables.find(parent); it != m_childTables.end())
            tableInsert(it->second, child);
    }
}

// Before the child's name changes: its parent's ChildTable finds it by name.
void FileIndex::unlink(EntryId child) noexcept
{
    Entry& c = mut(child);
    if (!m_childTables.empty() && c.parent != kNoEntry) {
        if (const auto it = m_childTables.find(c.parent); it != m_childTables.end())
            tableErase(it->second, child);
    }
    if (c.prevSibling != kNoEntry)
        mut(c.prevSibling).nextSibling = c.nextSibling;
    else if (c.parent != kNoEntry && entry(c.parent).firstChild == child)
        mut(c.parent).firstChild = c.nextSibling;
    if (c.nextSibling != kNoEntry)
        mut(c.nextSibling).prevSibling = c.prevSibling;
    c.prevSibling = kNoEntry;
    c.nextSibling = kNoEntry;
}

EntryId FileIndex::addRoot(std::string_view name)
{
    const EntryId id = allocate();
    const std::uint32_t offset = storeName(name);
    Entry& e = mut(id);
    e = Entry {};
    e.nameOffset = offset;
    e.nameLength = static_cast<std::uint16_t>(name.size());
    e.flags = static_cast<std::uint8_t>(EntryFlag::Directory | EntryFlag::Root | hanFlag(name));
    m_roots.push_back(id);
    m_live.fetch_add(1, std::memory_order_relaxed);
    return id;
}

EntryId FileIndex::add(EntryId parent, std::string_view name, std::uint8_t flags)
{
    const EntryId id = allocate();
    const std::uint32_t offset = storeName(name);
    Entry& e = mut(id);
    e = Entry {};
    e.nameOffset = offset;
    e.nameLength = static_cast<std::uint16_t>(std::min<std::size_t>(name.size(), 0xFFFF));
    e.flags = static_cast<std::uint8_t>((flags & ~(EntryFlag::Deleted | EntryFlag::Root | EntryFlag::Han)) | hanFlag(name));
    e.extLength = extensionLength(name, e.isDir());
    link(parent, id);
    m_live.fetch_add(1, std::memory_order_relaxed);
    return id;
}

std::size_t FileIndex::remove(EntryId id)
{
    if (id >= m_count || entry(id).isDeleted())
        return 0;
    unlink(id);
    if (entry(id).flags & EntryFlag::Root) {
        std::erase(m_roots, id);
        std::erase_if(m_folderRecords, [id](const auto& t) { return t.first == id; });
    }

    std::size_t removed = 0;
    std::vector<EntryId> stack {id};
    while (!stack.empty()) {
        const EntryId cur = stack.back();
        stack.pop_back();
        Entry& e = mut(cur);
        if (e.isDeleted())
            continue;
        e.flags |= EntryFlag::Deleted;
        ++removed;
        if (e.isDir() && !m_childTables.empty())
            m_childTables.erase(cur);
        for (EntryId c = e.firstChild; c != kNoEntry; c = entry(c).nextSibling)
            stack.push_back(c);
    }
    m_live.fetch_sub(removed, std::memory_order_relaxed);
    return removed;
}

bool FileIndex::move(EntryId id, EntryId newParent, std::string_view newName)
{
    if (id >= m_count || newParent >= m_count)
        return false;
    if (entry(id).isDeleted() || entry(newParent).isDeleted() || !entry(newParent).isDir())
        return false;
    for (EntryId p = newParent; p != kNoEntry; p = entry(p).parent) {
        if (p == id)
            return false; // would move a folder into itself
    }
    unlink(id); // under the old name
    if (name(id) != newName) {
        const std::uint32_t offset = storeName(newName);
        Entry& e = mut(id);
        e.nameOffset = offset;
        e.nameLength = static_cast<std::uint16_t>(std::min<std::size_t>(newName.size(), 0xFFFF));
        e.extLength = extensionLength(newName, e.isDir());
        e.flags = static_cast<std::uint8_t>((e.flags & ~EntryFlag::Han) | hanFlag(newName));
    }
    link(newParent, id); // under the new one
    return true;
}

void FileIndex::setFlags(EntryId id, std::uint8_t flags)
{
    Entry& e = mut(id);
    // What the index itself tracks stays; the rest comes from the caller.
    constexpr std::uint8_t kOwn = EntryFlag::Deleted | EntryFlag::Root | EntryFlag::Directory | EntryFlag::Han;
    e.flags = static_cast<std::uint8_t>((e.flags & kOwn) | (flags & ~kOwn));
}

void FileIndex::setFolderRecord(EntryId root, std::uint32_t record, EntryId folder)
{
    if (record == 0)
        return;
    for (auto& [r, table] : m_folderRecords) {
        if (r == root) {
            table.set(record, folder);
            return;
        }
    }
    m_folderRecords.emplace_back(root, RecordTable {}).second.set(record, folder);
}

EntryId FileIndex::folderByRecord(EntryId root, std::uint32_t record) const noexcept
{
    const RecordTable* table = folderRecords(root);
    const EntryId id = table ? table->find(record) : kNoEntry;
    if (id >= m_count || entry(id).isDeleted() || !entry(id).isDir())
        return kNoEntry; // removed since (deleted, or moved out of the index)
    return id;
}

void FileIndex::forgetFolderRecord(EntryId root, std::uint32_t record) noexcept
{
    for (auto& [r, table] : m_folderRecords) {
        if (r == root)
            table.erase(record);
    }
}

const RecordTable* FileIndex::folderRecords(EntryId root) const noexcept
{
    for (const auto& [r, table] : m_folderRecords) {
        if (r == root)
            return &table;
    }
    return nullptr;
}

EntryId FileIndex::tableFind(const ChildTable& table, std::string_view childName, bool exactCase) const noexcept
{
    const std::size_t mask = table.buckets.size() - 1;
    for (std::size_t i = foldedHash(childName) & mask; table.buckets[i] != kNoEntry; i = (i + 1) & mask) {
        const std::string_view candidate = name(table.buckets[i]);
        if (exactCase ? candidate == childName : text::equalsIgnoreAsciiCase(candidate, childName))
            return table.buckets[i];
    }
    return kNoEntry;
}

void FileIndex::tableInsert(ChildTable& table, EntryId child)
{
    if ((table.count + 1) * 2 > table.buckets.size()) { // at most half full
        std::vector<EntryId> old(std::max<std::size_t>(table.buckets.size() * 2, kChildTableMin * 4), kNoEntry);
        old.swap(table.buckets);
        table.count = 0;
        for (const EntryId id : old) {
            if (id != kNoEntry)
                tableInsert(table, id);
        }
    }
    const std::size_t mask = table.buckets.size() - 1;
    std::size_t i = foldedHash(name(child)) & mask;
    while (table.buckets[i] != kNoEntry)
        i = (i + 1) & mask;
    table.buckets[i] = child;
    ++table.count;
}

void FileIndex::tableErase(ChildTable& table, EntryId child) noexcept
{
    const std::size_t mask = table.buckets.size() - 1;
    std::size_t i = foldedHash(name(child)) & mask;
    while (table.buckets[i] != child) {
        if (table.buckets[i] == kNoEntry)
            return;
        i = (i + 1) & mask;
    }
    // Backward-shift deletion, as in RecordTable::erase.
    for (std::size_t j = (i + 1) & mask; table.buckets[j] != kNoEntry; j = (j + 1) & mask) {
        const std::size_t h = foldedHash(name(table.buckets[j])) & mask;
        const bool movable = i <= j ? (h <= i || h > j) : (h <= i && h > j);
        if (movable) {
            table.buckets[i] = table.buckets[j];
            i = j;
        }
    }
    table.buckets[i] = kNoEntry;
    --table.count;
}

EntryId FileIndex::findChild(EntryId parent, std::string_view childName) const
{
    if (parent >= m_count)
        return kNoEntry;
    if (const auto it = m_childTables.find(parent); it != m_childTables.end())
        return tableFind(it->second, childName, false);
    for (EntryId c = entry(parent).firstChild; c != kNoEntry; c = entry(c).nextSibling) {
        if (text::equalsIgnoreAsciiCase(name(c), childName))
            return c;
    }
    return kNoEntry;
}

EntryId FileIndex::childForUpdate(EntryId parent, std::string_view childName, bool exactCase)
{
    if (parent >= m_count)
        return kNoEntry;
    if (const auto it = m_childTables.find(parent); it != m_childTables.end())
        return tableFind(it->second, childName, exactCase);
    std::size_t walked = 0;
    for (EntryId c = entry(parent).firstChild; c != kNoEntry; c = entry(c).nextSibling) {
        if (exactCase ? name(c) == childName : text::equalsIgnoreAsciiCase(name(c), childName))
            return c;
        if (++walked == kChildTableMin) {
            // A big folder: index its children once, so the next lookups are quick.
            ChildTable& table = m_childTables[parent];
            for (EntryId k = entry(parent).firstChild; k != kNoEntry; k = entry(k).nextSibling)
                tableInsert(table, k);
            return tableFind(table, childName, exactCase);
        }
    }
    return kNoEntry;
}

EntryId FileIndex::findPath(std::wstring_view path) const
{
    return walkPath(path, [this](EntryId parent, std::string_view name) { return findChild(parent, name); });
}

EntryId FileIndex::pathForUpdate(std::wstring_view path)
{
    return walkPath(
        path, [this](EntryId parent, std::string_view name) { return childForUpdate(parent, name, false); });
}

template <typename Lookup> EntryId FileIndex::walkPath(std::wstring_view path, Lookup&& lookup) const
{
    if (path.size() < 2 || path[1] != L':')
        return kNoEntry;
    const auto drive = static_cast<unsigned char>(std::towupper(path[0]) & 0x7F);
    EntryId cur = kNoEntry;
    for (EntryId r : m_roots) {
        const std::string_view rn = name(r);
        if (rn.size() >= 2 && text::fold(rn[0]) == text::fold(static_cast<char>(drive)) && rn[1] == ':') {
            cur = r;
            break;
        }
    }
    if (cur == kNoEntry)
        return kNoEntry;

    std::string component;
    std::size_t i = 2;
    while (i < path.size()) {
        while (i < path.size() && (path[i] == L'\\' || path[i] == L'/'))
            ++i;
        std::size_t j = i;
        while (j < path.size() && path[j] != L'\\' && path[j] != L'/')
            ++j;
        if (j == i)
            break;
        component.clear();
        wtf8::append(component, wtf8::view(path.substr(i, j - i)));
        cur = lookup(cur, component);
        if (cur == kNoEntry)
            return kNoEntry;
        i = j;
    }
    return cur;
}

std::u16string FileIndex::path16(EntryId id) const
{
    std::u16string out;
    appendPath16(id, out);
    return out;
}

void FileIndex::appendPath16(EntryId id, std::u16string& out) const
{
    EntryId chain[64];
    std::vector<EntryId> deep;
    std::size_t n = 0;
    for (EntryId cur = id; cur != kNoEntry; cur = entry(cur).parent) {
        if (n < std::size(chain))
            chain[n++] = cur;
        else
            deep.push_back(cur);
    }
    const std::size_t start = out.size();
    auto appendName = [&](EntryId e) {
        if (out.size() > start)
            out.push_back(u'\\');
        wtf8::decodeAppend(out, name(e));
    };
    for (auto it = deep.rbegin(); it != deep.rend(); ++it)
        appendName(*it);
    for (std::size_t k = n; k-- > 0;)
        appendName(chain[k]);
    if (n == 1 && deep.empty())
        out.push_back(u'\\'); // a volume root: "C:\"
}

QString FileIndex::path(EntryId id) const
{
    const std::u16string p = path16(id);
    return QString(reinterpret_cast<const QChar*>(p.data()), static_cast<qsizetype>(p.size()));
}

std::wstring FileIndex::wpath(EntryId id) const
{
    const std::u16string p = path16(id);
    return std::wstring(p.begin(), p.end());
}

int FileIndex::depth(EntryId id) const noexcept
{
    int d = 0;
    for (EntryId cur = entry(id).parent; cur != kNoEntry; cur = entry(cur).parent)
        ++d;
    return d;
}

} // namespace qf
