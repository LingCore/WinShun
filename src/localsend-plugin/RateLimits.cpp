#include "RateLimits.h"

#include <iterator>

namespace localsend {

bool ReadSlots::claim(const std::string& address)
{
    if (m_taken >= kInAll)
        return false;
    std::size_t& mine = m_byAddress[address];
    if (mine >= kPerAddress)
        return false;
    ++mine;
    ++m_taken;
    return true;
}

void ReadSlots::release(const std::string& address)
{
    const auto it = m_byAddress.find(address);
    if (it == m_byAddress.end())
        return;
    if (it->second > 0 && m_taken > 0) {
        --it->second;
        --m_taken;
    }
    if (it->second == 0)
        m_byAddress.erase(it);
}

bool PerSecond::allow(const std::string& key, Clock::time_point now)
{
    if (m_counts.size() >= kPruneAt) {
        // A flood of addresses: the ones that no longer count go first.
        for (auto it = m_counts.begin(); it != m_counts.end();)
            it = now - it->second.second >= std::chrono::seconds(1) ? m_counts.erase(it) : std::next(it);
    }
    Count& c = m_counts[key];
    if (c.count == 0 || now - c.second >= std::chrono::seconds(1)) {
        c.second = now;
        c.count = 0;
    }
    return ++c.count <= m_limit;
}

bool Window::allow(Clock::time_point now)
{
    while (!m_times.empty() && now - m_times.front() >= m_span)
        m_times.pop_front();
    if (m_times.size() >= m_count)
        return false;
    m_times.push_back(now);
    return true;
}

} // namespace localsend
