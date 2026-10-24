#include "Output.h"

#include "Win.h"

#include <algorithm>
#include <atomic>

namespace ws::cli {

namespace {

constexpr std::size_t kFlushAt = 64 * 1024;

std::wstring_view sequence(Style style) noexcept
{
    switch (style) {
    case Style::Plain:
        return {};
    case Style::Bold:
    case Style::Name:
        return L"\x1b[1m";
    case Style::Dim:
        return L"\x1b[2m";
    case Style::FolderName:
        return L"\x1b[1;34m";
    case Style::Match:
        return L"\x1b[1;31m";
    case Style::GrepPath:
        return L"\x1b[35m";
    case Style::GrepPlace:
        return L"\x1b[32m";
    case Style::Error:
        return L"\x1b[1;31m";
    }
    return {};
}

constexpr std::wstring_view kReset = L"\x1b[0m";

// Saved by ConsoleModes, for restoreNow() from Ctrl+C's thread.
struct SavedMode {
    std::atomic<HANDLE> handle {nullptr};
    std::atomic<DWORD> mode {0};
    std::atomic<bool> changed {false};
};
SavedMode g_saved[2];

} // namespace

Stream::Stream(HANDLE handle, UINT codePage)
    : m_handle(handle == INVALID_HANDLE_VALUE ? nullptr : handle)
    , m_encoder(codePage)
{
    DWORD mode = 0;
    m_console = m_handle && ::GetConsoleMode(m_handle, &mode);
}

Stream::~Stream()
{
    flush();
}

void Stream::pending()
{
    if (m_partner)
        m_partner->flush();
}

void Stream::put(std::wstring_view text)
{
    if (m_broken || !m_handle || text.empty())
        return;
    if (m_console) {
        m_wide.append(text);
        if (m_wide.size() >= kFlushAt / 2)
            flush();
        return;
    }
    if (m_encoder.append(m_bytes, text) > 0)
        m_lineLossy = true;
    if (m_bytes.size() >= kFlushAt)
        flush();
}

void Stream::write(std::wstring_view text, Style style)
{
    pending();
    if (m_colored && style != Style::Plain && !text.empty()) {
        put(sequence(style));
        put(text);
        put(kReset);
        return;
    }
    put(text);
}

void Stream::writeUtf8(std::string_view text)
{
    pending();
    if (!m_console && m_encoder.codePage() == CP_UTF8) {
        if (m_broken || !m_handle)
            return;
        m_bytes.append(text);
        if (m_bytes.size() >= kFlushAt)
            flush();
        return;
    }
    put(wide(text));
}

void Stream::newline()
{
    pending();
    put(L"\n");
    if (m_lineLossy) {
        ++m_lossyLines;
        m_lineLossy = false;
    }
}

void Stream::nul()
{
    pending();
    if (m_lineLossy) {
        ++m_lossyLines;
        m_lineLossy = false;
    }
    if (m_broken || !m_handle)
        return;
    if (m_console)
        m_wide.push_back(L'\0');
    else
        m_encoder.append(m_bytes, std::wstring_view(L"\0", 1));
}

void Stream::writeOut(const void* data, std::size_t bytes)
{
    const auto* p = static_cast<const char*>(data);
    while (bytes > 0 && !m_broken) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes, 1u << 20));
        if (!::WriteFile(m_handle, p, chunk, &written, nullptr) || written == 0) {
            m_broken = true; // the pipe's reader went (ERROR_NO_DATA, ERROR_BROKEN_PIPE), the disk is full…
            return;
        }
        p += written;
        bytes -= written;
    }
}

void Stream::flush()
{
    if (m_broken || !m_handle) {
        m_wide.clear();
        m_bytes.clear();
        return;
    }
    if (m_console) {
        std::wstring_view rest = m_wide;
        while (!rest.empty()) {
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(rest.size(), 8192));
            if (!::WriteConsoleW(m_handle, rest.data(), chunk, &written, nullptr) || written == 0) {
                m_broken = true;
                break;
            }
            rest.remove_prefix(written);
        }
        m_wide.clear();
        return;
    }
    if (!m_bytes.empty()) {
        writeOut(m_bytes.data(), m_bytes.size());
        m_bytes.clear();
    }
}

ConsoleModes::ConsoleModes()
{
    const HANDLE handles[2] {::GetStdHandle(STD_OUTPUT_HANDLE), ::GetStdHandle(STD_ERROR_HANDLE)};
    for (int i = 0; i < 2; ++i) {
        DWORD mode = 0;
        if (!handles[i] || handles[i] == INVALID_HANDLE_VALUE || !::GetConsoleMode(handles[i], &mode))
            continue;
        g_saved[i].handle = handles[i];
        g_saved[i].mode = mode;
        if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)
            continue;
        if (::SetConsoleMode(handles[i], mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
            g_saved[i].changed = true;
    }
}

ConsoleModes::~ConsoleModes()
{
    restoreNow();
}

bool ConsoleModes::vt(HANDLE handle) const noexcept
{
    DWORD mode = 0;
    return handle && ::GetConsoleMode(handle, &mode) && (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

void ConsoleModes::restoreNow() noexcept
{
    // stderr first: when both are the same console, stdout's saved mode is
    // the one from before either was changed.
    for (int i = 1; i >= 0; --i) {
        if (g_saved[i].changed.exchange(false))
            ::SetConsoleMode(g_saved[i].handle, g_saved[i].mode);
    }
}

bool wantColor(ColorMode mode, bool console, bool vt)
{
    switch (mode) {
    case ColorMode::Always:
        return true;
    case ColorMode::Never:
        return false;
    case ColorMode::Auto:
        break;
    }
    // https://no-color.org: set and not empty.
    return console && vt && environment(L"NO_COLOR").empty();
}

} // namespace ws::cli
