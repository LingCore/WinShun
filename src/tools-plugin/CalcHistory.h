#pragma once

// The sums the calculator of winshun.tools was asked for and whose result
// was taken (Enter): kept, newest first, to look back over and check
// against one another, as the tape of a desk calculator. A text file in the
// plugin's own folder, one sum a line; no Qt.

#include <cstddef>
#include <string>
#include <vector>

namespace tools::calc {

struct Record {
    long long when = 0; // Unix seconds
    std::string expression; // as typed, on one line
    std::string shown; // the result as the row shows it ("21,110")
    std::string plain; // ... and as it is copied ("21110")
    friend bool operator==(const Record&, const Record&) = default;
};

class History {
public:
    static constexpr std::size_t kMost = 200; // the oldest go first
    static constexpr long long kKeepSeconds = 30ll * 24 * 60 * 60; // and those older than a month

    // Reads the file (UTF-8 path); none, or lines that are no records: nothing.
    // An empty path keeps the sums in memory only.
    void open(const std::string& file, long long now);
    // The newest first. The same sum again right after itself is one record, at its new time.
    void add(Record record);
    // The record of that time and expression; false if there is none.
    bool remove(long long when, const std::string& expression);
    void clear();
    const std::vector<Record>& records() const noexcept { return m_records; }

private:
    void save() const;

    std::string m_file;
    std::vector<Record> m_records; // newest first
};

// When a record was made, said briefly beside it: "10:36" today, "昨天 10:36"
// (or "yesterday 10:36"), "11-13 10:36" before that; local time.
std::string whenText(long long when, long long now, bool chinese);

} // namespace tools::calc
