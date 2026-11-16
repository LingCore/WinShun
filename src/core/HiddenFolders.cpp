#include "HiddenFolders.h"

#include "Win32Util.h"
#include "Wtf8.h"

#include <algorithm>

namespace ws {

namespace {

std::wstring_view lastPart(std::wstring_view rule)
{
    const std::size_t slash = rule.find_last_of(L'\\');
    return slash == std::wstring_view::npos ? rule : rule.substr(slash + 1);
}

} // namespace

HiddenFolders::HiddenFolders(CrawlRules rules)
    : m_rules([&] {
        CrawlRules excluded;
        excluded.excludedPaths = std::move(rules.excludedPaths);
        excluded.excludedNames = std::move(rules.excludedNames);
        return excluded;
    }())
{
    for (const auto* list : {&m_rules.rules().excludedPaths, &m_rules.rules().excludedNames}) {
        for (const std::wstring& rule : *list) {
            const std::wstring_view last = lastPart(rule);
            if (!last.empty()
                && std::ranges::none_of(m_lastNames, [&](const std::wstring& n) { return win32::equalsIgnoreCase(n, last); }))
                m_lastNames.emplace_back(last);
        }
    }
}

bool HiddenFolders::mayBeNamed(std::string_view name) const
{
    std::u16string wide;
    wtf8::decodeAppend(wide, name);
    return std::ranges::any_of(m_lastNames, [&](const std::wstring& n) { return win32::equalsIgnoreCase(n, wtf8::wview(wide)); });
}

bool HiddenFolders::ruleFor(const FileIndex& index, EntryId folder) const
{
    // A root is named by its path ("C:", "\\nas\docs").
    const bool root = index.scan(folder).parent == kNoEntry;
    if (!root && !mayBeNamed(index.name(folder)))
        return false;
    std::wstring path = index.wpath(folder);
    if (path.size() > 2 && path.back() == L'\\')
        path.pop_back(); // a root alone: "C:\"
    return m_rules.isExcludedDir(path, root ? std::wstring_view(path) : lastPart(path));
}

bool HiddenFolders::folderHidden(const FileIndex& index, EntryId folder, Memo& memo) const
{
    if (memo.m_slots.empty())
        memo.m_slots.resize(Memo::kSlots);
    const auto slotOf = [&](EntryId id) -> Memo::Slot& { return memo.m_slots[id % Memo::kSlots]; };
    // Up to a folder known already (or the top), then down again, noting each.
    std::array<EntryId, 64> chain;
    std::size_t length = 0;
    bool hidden = false;
    EntryId cur = folder;
    for (; cur != kNoEntry; cur = index.scan(cur).parent) {
        if (const Memo::Slot& slot = slotOf(cur); slot.folder == cur) {
            hidden = slot.hidden;
            break;
        }
        if (length == chain.size()) { // that deep: the rest on its own
            hidden = folderHidden(index, cur, memo);
            break;
        }
        chain[length++] = cur;
    }
    while (length > 0) {
        const EntryId id = chain[--length];
        hidden = hidden || ruleFor(index, id);
        slotOf(id) = {id, hidden};
    }
    return hidden;
}

bool HiddenFolders::hides(const FileIndex& index, const EntryScan& entry, Memo& memo) const
{
    if (entry.isDir()) {
        const std::string_view name = index.name(entry);
        if (entry.parent == kNoEntry) {
            std::u16string wide;
            wtf8::decodeAppend(wide, name);
            return m_rules.isExcludedDir(wtf8::wview(wide), wtf8::wview(wide));
        }
        if (mayBeNamed(name)) {
            std::wstring path = index.wpath(entry.parent);
            if (!path.empty() && path.back() != L'\\')
                path.push_back(L'\\');
            const std::size_t start = path.size();
            std::u16string wide;
            wtf8::decodeAppend(wide, name);
            path.append(wtf8::wview(wide));
            if (m_rules.isExcludedDir(path, std::wstring_view(path).substr(start)))
                return true;
        }
    }
    return entry.parent != kNoEntry && folderHidden(index, entry.parent, memo);
}

} // namespace ws
