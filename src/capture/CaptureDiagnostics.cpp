#include "CaptureDiagnostics.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <string>

using namespace Qt::StringLiterals;

namespace ws::capture::diag {

namespace {

// HKCU\<path>\<name> as text (REG_SZ or REG_EXPAND_SZ, not expanded); null when absent.
QString registryText(const wchar_t* path, const wchar_t* name)
{
    constexpr DWORD kFlags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
    DWORD bytes = 0;
    if (::RegGetValueW(HKEY_CURRENT_USER, path, name, kFlags, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < 2)
        return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(HKEY_CURRENT_USER, path, name, kFlags, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
        return {};
    return QString::fromWCharArray(value.c_str());
}

std::optional<DWORD> registryNumber(const wchar_t* path, const wchar_t* name)
{
    DWORD value = 0;
    DWORD bytes = sizeof value;
    if (::RegGetValueW(HKEY_CURRENT_USER, path, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) != ERROR_SUCCESS)
        return std::nullopt;
    return value;
}

bool systemFlag(UINT action)
{
    BOOL on = FALSE;
    return ::SystemParametersInfoW(action, 0, &on, 0) && on;
}

struct Bitmaps {
    bool mono = false;
    int bpp = 0;
    int width = 0;
    int height = 0;
    POINT hotspot {};
};

std::optional<Bitmaps> readBitmaps(HCURSOR cursor)
{
    ICONINFO info {};
    if (!cursor || !::GetIconInfo(cursor, &info))
        return std::nullopt;
    Bitmaps out;
    out.hotspot = {static_cast<LONG>(info.xHotspot), static_cast<LONG>(info.yHotspot)};
    BITMAP bm {};
    bool ok = false;
    if (!info.hbmColor && info.hbmMask) {
        // A monochrome cursor: the mask holds the AND plane over the XOR plane.
        ok = ::GetObjectW(info.hbmMask, sizeof bm, &bm) != 0;
        out.mono = true;
        out.width = bm.bmWidth;
        out.height = bm.bmHeight / 2;
    } else if (info.hbmColor) {
        ok = ::GetObjectW(info.hbmColor, sizeof bm, &bm) != 0;
        out.bpp = bm.bmBitsPixel;
        out.width = bm.bmWidth;
        out.height = bm.bmHeight;
    }
    if (info.hbmMask)
        ::DeleteObject(info.hbmMask);
    if (info.hbmColor)
        ::DeleteObject(info.hbmColor);
    if (!ok)
        return std::nullopt;
    return out;
}

QString kindOf(const Bitmaps& b)
{
    return b.mono ? u"monochrome"_s : u"colour %1bpp"_s.arg(b.bpp);
}

} // namespace

std::optional<StallWatch::Stall> StallWatch::note(qint64 intervalMs, qint64 atMs, const QString& phase)
{
    if (intervalMs <= kStallAfterMs)
        return std::nullopt;
    const qint64 ms = intervalMs - kTickMs;
    ++m_count;
    if (ms > m_longestMs) {
        m_longestMs = ms;
        m_longestAtMs = atMs;
        m_longestPhase = phase;
    }
    return Stall {ms, m_count <= kMaxStallEvents};
}

bool StallWatch::noteHidden()
{
    return ++m_hiddenTicks == 1;
}

QJsonObject StallWatch::summary() const
{
    return {{u"count"_s, m_count}, {u"longest_ms"_s, m_longestMs}, {u"longest_at_ms"_s, m_longestAtMs},
        {u"longest_phase"_s, m_longestPhase}, {u"hidden_ticks"_s, m_hiddenTicks}};
}

bool cursorHidden(DWORD flags)
{
    return !(flags & CURSOR_SHOWING) || (flags & CURSOR_SUPPRESSED);
}

QJsonObject frameStats(std::vector<qint64> nanoseconds)
{
    if (nanoseconds.empty())
        return {};
    std::ranges::sort(nanoseconds);
    const auto at = [&nanoseconds](double q) {
        return static_cast<double>(nanoseconds[static_cast<std::size_t>(q * (nanoseconds.size() - 1))]) / 1e6;
    };
    const auto tenth = [](double ms) { return std::round(ms * 10) / 10; };
    return {{u"count"_s, static_cast<qint64>(nanoseconds.size())}, {u"p95_ms"_s, tenth(at(0.95))},
        {u"max_ms"_s, tenth(at(1.0))}};
}

QString fileNameOf(const QString& path)
{
    const qsizetype at = std::max(path.lastIndexOf(u'\\'), path.lastIndexOf(u'/'));
    return path.mid(at + 1).trimmed();
}

QString describeBitmaps(HCURSOR cursor)
{
    const std::optional<Bitmaps> b = readBitmaps(cursor);
    return b ? u"%1 %2x%3"_s.arg(kindOf(*b)).arg(b->width).arg(b->height) : QString();
}

QJsonObject cursorFacts(HCURSOR cursor, DWORD flags)
{
    QJsonObject out {{u"flags"_s, static_cast<qint64>(flags)}};
    const std::optional<Bitmaps> b = readBitmaps(cursor);
    if (!b) {
        out[u"kind"_s] = cursor ? u"unreadable"_s : u"none"_s;
        return out;
    }
    out[u"kind"_s] = kindOf(*b);
    out[u"size"_s] = u"%1x%2"_s.arg(b->width).arg(b->height);
    out[u"hotspot_x"_s] = static_cast<int>(b->hotspot.x);
    out[u"hotspot_y"_s] = static_cast<int>(b->hotspot.y);
    return out;
}

QJsonObject pointerFacts()
{
    constexpr const wchar_t* kCursors = L"Control Panel\\Cursors";
    QJsonObject out;
    out[u"scheme"_s] = registryText(kCursors, nullptr);
    out[u"cursor_base_size"_s] = static_cast<qint64>(registryNumber(kCursors, L"CursorBaseSize").value_or(32));
    out[u"crosshair_file"_s] = fileNameOf(registryText(kCursors, L"Crosshair"));
    out[u"ibeam_file"_s] = fileNameOf(registryText(kCursors, L"IBeam"));
    out[u"mouse_trails"_s] = registryText(L"Control Panel\\Mouse", L"MouseTrails");
    constexpr const wchar_t* kAccessibility = L"Software\\Microsoft\\Accessibility";
    if (const auto type = registryNumber(kAccessibility, L"CursorType"))
        out[u"accessibility_cursor_type"_s] = static_cast<qint64>(*type);
    if (const auto color = registryNumber(kAccessibility, L"CursorColor"))
        out[u"accessibility_cursor_color"_s] = static_cast<qint64>(*color);
    out[u"cursor_shadow"_s] = systemFlag(SPI_GETCURSORSHADOW);
    out[u"mouse_sonar"_s] = systemFlag(SPI_GETMOUSESONAR);
    out[u"system_cross"_s] = describeBitmaps(::LoadCursorW(nullptr, IDC_CROSS));
    out[u"system_ibeam"_s] = describeBitmaps(::LoadCursorW(nullptr, IDC_IBEAM));
    return out;
}

qint64 secondsSinceProcessStart()
{
    FILETIME created {}, exited {}, kernel {}, user {}, now {};
    if (!::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user))
        return -1;
    ::GetSystemTimeAsFileTime(&now);
    const auto ticks = [](const FILETIME& t) { return (quint64(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return static_cast<qint64>((ticks(now) - ticks(created)) / 10'000'000ull);
}

} // namespace ws::capture::diag
