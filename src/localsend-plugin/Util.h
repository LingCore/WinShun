#pragma once

// Small pieces the LAN transfer plugin's parts share: UTF-8 and UTF-16,
// hex, SHA-256, random tokens, the log. No Qt (the plugin links the C
// runtime in and starts in milliseconds, like WinShunTools.exe).

#include <windows.h>

#include <bcrypt.h>

#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace localsend {

std::string narrow(std::wstring_view text); // UTF-16 to UTF-8
std::wstring wide(std::string_view text); // UTF-8 to UTF-16 (bad bytes become U+FFFD)
// UTF-8 for sure: a peer's bytes that are not (a host drops a line that is
// not UTF-8 whole) become U+FFFD.
std::string validUtf8(std::string_view text);
// At most `bytes` of UTF-8, never a character cut in two (four-byte ones,
// a surrogate pair in UTF-16, included).
std::string cutUtf8(std::string_view text, std::size_t bytes);
std::string hex(const unsigned char* bytes, std::size_t size); // upper case
std::string lower(std::string text); // ASCII only
bool equalsNoCase(std::string_view a, std::string_view b); // ASCII only
std::string randomToken(int bytes = 16); // hex, from the system's generator
std::string trimmed(std::string_view text);

class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void add(const void* data, std::size_t size);
    std::string finish(); // upper-case hex; once
private:
    BCRYPT_HASH_HANDLE m_hash = nullptr;
};
std::string sha256Hex(const void* data, std::size_t size);

// A closed handle at the end of its scope.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : m_h(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : m_h(std::exchange(other.m_h, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_h = std::exchange(other.m_h, nullptr);
        }
        return *this;
    }
    HANDLE get() const noexcept { return m_h; }
    bool valid() const noexcept { return m_h && m_h != INVALID_HANDLE_VALUE; }
    void reset() noexcept
    {
        if (valid())
            ::CloseHandle(m_h);
        m_h = nullptr;
    }
private:
    HANDLE m_h = nullptr;
};

// Lines for plugins.log: stderr until main hands them to winshun/log too.
// Never file names or message text above "debug": the log is kept.
void setLogSink(std::function<void(const std::string& line)> sink);
void logInfo(const std::string& line);
void logWarning(const std::string& line);
void logStopped(const char* what, const char* why) noexcept; // for guarded()

// A thread's work, or one turn of a thread's loop: an exception out of it
// (memory running out on what a peer sent) is logged and ends that work,
// never the process (an exception out of a thread ends it at once). True
// when the work ran through.
template <typename Work>
bool guarded(const char* what, Work&& work) noexcept
{
    try {
        std::forward<Work>(work)();
        return true;
    } catch (const std::exception& e) {
        logStopped(what, e.what());
    } catch (...) {
        logStopped(what, nullptr);
    }
    return false;
}

// "12.6 MB", "980 KB", "15 B": sizes as the rows and toasts show them.
std::string sizeText(std::uint64_t bytes);

// Marks that turn the text after them around: "报告<U+202E>fdp.exe" shows
// as "报告exe.pdf". Joiners (emoji sequences) are not among them.
constexpr bool directionMark(wchar_t c) noexcept
{
    return c == 0x061C || c == 0x200E || c == 0x200F || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
}

} // namespace localsend
