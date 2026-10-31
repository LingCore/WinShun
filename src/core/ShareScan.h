#pragma once

#include <algorithm>
#include <string_view>

namespace ws {

// Per remote root, in local wall-clock time. Equal endpoints mean all day;
// 0 depth means unlimited, 1 lists just the chosen folder's children.
struct ShareScan {
    bool scheduled = false;
    int fromMinute = 22 * 60;
    int untilMinute = 7 * 60;
    int maxDepth = 0;

    bool operator==(const ShareScan&) const = default;

    bool allows(int minute) const noexcept
    {
        if (!scheduled || fromMinute == untilMinute)
            return true;
        return fromMinute < untilMinute ? minute >= fromMinute && minute < untilMinute
                                       : minute >= fromMinute || minute < untilMinute;
    }

    // Call with a path known to be under root (or root itself).
    static int depth(std::wstring_view root, std::wstring_view path) noexcept
    {
        if (path.size() <= root.size())
            return 0;
        const auto tail = path.substr(root.size() + 1);
        return 1 + static_cast<int>(std::ranges::count(tail, L'\\'));
    }
};

} // namespace ws
