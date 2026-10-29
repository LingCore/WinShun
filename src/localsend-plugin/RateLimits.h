#pragma once

// How much a peer may make this PC do, apart from the server so that
// tests/tst_localsend.cpp can look at it: prepare-upload bodies read at a
// time, requests per address in a second, and notifications in a while.
// None of these lock: their owners do.

#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <string>

namespace localsend {

// Prepare-upload bodies being read and parsed (4 MB of JSON is tens of
// megabytes parsed): at most one per address and two in all. A request
// that finds none free is refused before its body is read.
class ReadSlots {
public:
    static constexpr std::size_t kPerAddress = 1;
    static constexpr std::size_t kInAll = 2;

    bool claim(const std::string& address); // false: none free
    void release(const std::string& address); // one claimed before
    std::size_t taken() const { return m_taken; }

private:
    std::map<std::string, std::size_t> m_byAddress;
    std::size_t m_taken = 0;
};

// At most `limit` a second for each key (a peer's address).
class PerSecond {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kPruneAt = 256; // this many keys: the ones whose second is over go

    explicit PerSecond(int limit) : m_limit(limit) {}
    bool allow(const std::string& key, Clock::time_point now); // counted when allowed and when not
    std::size_t keys() const { return m_counts.size(); }

private:
    struct Count {
        Clock::time_point second; // when this second began
        int count = 0;
    };
    int m_limit;
    std::map<std::string, Count> m_counts;
};

// At most `count` in any `span`, all together.
class Window {
public:
    using Clock = std::chrono::steady_clock;

    Window(std::size_t count, Clock::duration span) : m_count(count), m_span(span) {}
    bool allow(Clock::time_point now); // counted only when allowed

private:
    std::size_t m_count;
    Clock::duration m_span;
    std::deque<Clock::time_point> m_times;
};

} // namespace localsend
