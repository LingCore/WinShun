#include "CalcHistory.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace tools::calc {

namespace {

std::filesystem::path pathOf(const std::string& utf8)
{
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// A record's line: when, plain, shown and the expression, tabs between.
// None of them holds a tab or a line break (oneLine).
std::string oneLine(std::string text)
{
    for (char& c : text) {
        if (c == '\t' || c == '\r' || c == '\n')
            c = ' ';
    }
    return text;
}

bool readRecord(const std::string& line, Record& record)
{
    std::istringstream in(line);
    std::string when;
    if (!std::getline(in, when, '\t') || !std::getline(in, record.plain, '\t') || !std::getline(in, record.shown, '\t')
        || !std::getline(in, record.expression))
        return false;
    try {
        record.when = std::stoll(when);
    } catch (const std::exception&) {
        return false;
    }
    return record.when > 0 && !record.expression.empty() && !record.shown.empty();
}

std::tm localOf(long long when)
{
    const std::time_t time = static_cast<std::time_t>(when);
    std::tm local {};
    localtime_s(&local, &time);
    return local;
}

} // namespace

void History::open(const std::string& file, long long now)
{
    m_file = file;
    m_records.clear();
    if (m_file.empty())
        return;
    std::ifstream in(pathOf(m_file), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        Record record;
        if (readRecord(line, record) && now - record.when <= kKeepSeconds && m_records.size() < kMost)
            m_records.push_back(std::move(record));
    }
}

void History::add(Record record)
{
    record.expression = oneLine(std::move(record.expression));
    record.shown = oneLine(std::move(record.shown));
    record.plain = oneLine(std::move(record.plain));
    if (record.expression.empty() || record.shown.empty())
        return;
    if (!m_records.empty() && m_records.front().expression == record.expression && m_records.front().plain == record.plain)
        m_records.erase(m_records.begin()); // taken twice in a row: once, at the later time
    m_records.insert(m_records.begin(), std::move(record));
    const long long newest = m_records.front().when;
    std::erase_if(m_records, [newest](const Record& r) { return newest - r.when > kKeepSeconds; });
    if (m_records.size() > kMost)
        m_records.resize(kMost);
    save();
}

bool History::remove(long long when, const std::string& expression)
{
    const auto found
        = std::ranges::find_if(m_records, [&](const Record& r) { return r.when == when && r.expression == expression; });
    if (found == m_records.end())
        return false;
    m_records.erase(found);
    save();
    return true;
}

void History::clear()
{
    m_records.clear();
    save();
}

void History::save() const
{
    if (m_file.empty())
        return;
    // Whole, beside it, then put in its place: a write cut short leaves the old file.
    const std::filesystem::path file = pathOf(m_file);
    std::filesystem::path fresh = file;
    fresh += ".new";
    std::error_code made;
    std::filesystem::create_directories(file.parent_path(), made);
    {
        std::ofstream out(fresh, std::ios::binary | std::ios::trunc);
        for (const Record& r : m_records)
            out << r.when << '\t' << r.plain << '\t' << r.shown << '\t' << r.expression << '\n';
        if (!out)
            return;
    }
    std::error_code error;
    std::filesystem::rename(fresh, file, error);
    if (error)
        std::filesystem::remove(fresh, error);
}

std::string whenText(long long when, long long now, bool chinese)
{
    const std::tm then = localOf(when);
    const std::tm today = localOf(now);
    const std::tm yesterday = localOf(now - 24 * 60 * 60);
    char clock[16] {};
    std::snprintf(clock, sizeof clock, "%02d:%02d", then.tm_hour, then.tm_min);
    const auto sameDay = [&then](const std::tm& day) { return then.tm_year == day.tm_year && then.tm_yday == day.tm_yday; };
    if (sameDay(today))
        return clock;
    if (sameDay(yesterday))
        return std::string(chinese ? "昨天 " : "yesterday ") + clock;
    char date[16] {};
    std::snprintf(date, sizeof date, "%02d-%02d ", then.tm_mon + 1, then.tm_mday);
    return std::string(date) + clock;
}

} // namespace tools::calc
