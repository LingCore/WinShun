#include "Hider.h"

#include <cstdio>
#include <cstdlib>

namespace tabs {

std::string journalText(const std::vector<JournalLine>& lines)
{
    std::string text;
    char buffer[64];
    for (const JournalLine& l : lines) {
        std::snprintf(buffer, sizeof buffer, "%llx %d\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(l.window)),
            l.addedLayered ? 1 : 0);
        text += buffer;
    }
    return text;
}

std::vector<JournalLine> parseJournal(std::string_view text)
{
    std::vector<JournalLine> lines;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string line(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        unsigned long long value = 0;
        int added = 0;
        char rest = 0;
        if (sscanf_s(line.c_str(), "%llx %d %c", &value, &added, &rest, 1u) != 2 || value == 0 || (added != 0 && added != 1))
            continue;
        lines.push_back({reinterpret_cast<HWND>(static_cast<std::uintptr_t>(value)), added == 1});
    }
    return lines;
}

namespace {

std::string readAll(const std::wstring& path)
{
    std::string text;
    FILE* f = nullptr;
    if (path.empty() || _wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
        return text;
    char buffer[4096];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0 && text.size() < (1u << 20))
        text.append(buffer, n);
    std::fclose(f);
    return text;
}

void writeAll(const std::wstring& path, const std::vector<JournalLine>& lines)
{
    if (path.empty())
        return;
    if (lines.empty()) {
        ::DeleteFileW(path.c_str());
        return;
    }
    // A whole new file, then swapped in: a process killed while writing
    // leaves the old journal, not half a one.
    const std::wstring temp = path + L".new";
    FILE* f = nullptr;
    if (_wfopen_s(&f, temp.c_str(), L"wb") != 0 || !f)
        return;
    const std::string text = journalText(lines);
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (!ok || !::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        ::DeleteFileW(temp.c_str());
}

} // namespace

Hider::Hider(std::wstring journal, WindowOps ops) : m_journal(std::move(journal)), m_ops(std::move(ops)) {}

bool Hider::hidden(HWND w) const
{
    const std::lock_guard lock(m_mutex);
    return m_windows.contains(w);
}

std::size_t Hider::count() const
{
    const std::lock_guard lock(m_mutex);
    return m_windows.size();
}

bool Hider::hide(HWND w)
{
    const std::lock_guard serial(m_serial); // a show of the same window waits for this
    const bool added = !m_ops.layered(w);
    {
        const std::lock_guard lock(m_mutex);
        if (m_closed || !m_windows.emplace(w, added).second)
            return false;
        writeLocked();
    }
    m_ops.conceal(w, added); // a message to Explorer's thread: may wait for it
    return true;
}

void Hider::show(HWND w)
{
    const std::lock_guard serial(m_serial);
    bool added = false;
    {
        const std::lock_guard lock(m_mutex);
        const auto it = m_windows.find(w);
        if (it == m_windows.end())
            return;
        added = it->second;
    }
    m_ops.reveal(w, added);
    const std::lock_guard lock(m_mutex);
    if (m_windows.erase(w))
        writeLocked();
}

void Hider::forget(HWND w)
{
    const std::lock_guard lock(m_mutex);
    if (m_windows.erase(w))
        writeLocked();
}

void Hider::showAll()
{
    const std::lock_guard serial(m_serial);
    std::map<HWND, bool> windows;
    {
        const std::lock_guard lock(m_mutex);
        windows = m_windows;
    }
    for (const auto& [w, added] : windows) {
        m_ops.reveal(w, added);
        const std::lock_guard lock(m_mutex);
        if (m_windows.erase(w))
            writeLocked();
    }
}

void Hider::close()
{
    const std::lock_guard lock(m_mutex);
    m_closed = true;
}

void Hider::revealSnapshot()
{
    std::map<HWND, bool> windows;
    {
        const std::lock_guard lock(m_mutex);
        windows = m_windows;
    }
    for (const auto& [w, added] : windows)
        m_ops.reveal(w, added);
}

void Hider::writeLocked() const
{
    std::vector<JournalLine> lines;
    lines.reserve(m_windows.size());
    for (const auto& [w, added] : m_windows)
        lines.push_back({w, added});
    writeAll(m_journal, lines);
}

int restoreJournal(const std::wstring& journal, const std::function<bool(HWND)>& stillHidden,
    const std::function<bool(HWND, bool)>& reveal)
{
    const std::string text = readAll(journal);
    if (text.empty()) {
        if (!journal.empty())
            ::DeleteFileW(journal.c_str());
        return 0;
    }
    int shown = 0;
    std::vector<JournalLine> left;
    for (const JournalLine& line : parseJournal(text)) {
        // Only what is still an invisible Explorer window: the handle may
        // belong to something else by now.
        if (!stillHidden(line.window))
            continue;
        if (reveal(line.window, line.addedLayered) && !stillHidden(line.window))
            ++shown;
        else
            left.push_back(line); // tried again next time
    }
    writeAll(journal, left);
    return shown;
}

} // namespace tabs
