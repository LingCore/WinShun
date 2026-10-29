#include "Util.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <vector>

namespace localsend {

namespace {

std::mutex g_logMutex;
std::function<void(const std::string&)> g_logSink;

void emit(const char* level, const std::string& line)
{
    std::function<void(const std::string&)> sink;
    {
        const std::lock_guard lock(g_logMutex);
        sink = g_logSink;
    }
    if (sink) {
        sink(std::string(level) + ": " + line);
        return;
    }
    const std::string text = std::string("localsend ") + level + ": " + line + "\n";
    DWORD written = 0;
    ::WriteFile(::GetStdHandle(STD_ERROR_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

} // namespace

std::string narrow(std::wstring_view text)
{
    if (text.empty())
        return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(size, 0)), '\0');
    if (size > 0)
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring wide(std::string_view text)
{
    if (text.empty())
        return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    if (size > 0)
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string validUtf8(std::string_view text)
{
    // Windows turns bad sequences into U+FFFD both ways (no MB_ERR_INVALID_CHARS).
    return narrow(wide(text));
}

std::string cutUtf8(std::string_view text, std::size_t bytes)
{
    if (text.size() <= bytes)
        return std::string(text);
    std::size_t end = bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
        --end; // back to the first byte of the character it would cut
    return std::string(text.substr(0, end));
}

std::string hex(const unsigned char* bytes, std::size_t size)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out += kDigits[bytes[i] >> 4];
        out += kDigits[bytes[i] & 15];
    }
    return out;
}

std::string lower(std::string text)
{
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

bool equalsNoCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z')
            x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = static_cast<char>(y - 'A' + 'a');
        if (x != y)
            return false;
    }
    return true;
}

std::string randomToken(int bytes)
{
    std::vector<unsigned char> buffer(static_cast<std::size_t>(bytes));
    ::BCryptGenRandom(nullptr, buffer.data(), static_cast<ULONG>(buffer.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return hex(buffer.data(), buffer.size());
}

std::string trimmed(std::string_view text)
{
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && space(text.back()))
        text.remove_suffix(1);
    return std::string(text);
}

Sha256::Sha256()
{
    ::BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &m_hash, nullptr, 0, nullptr, 0, 0);
}

Sha256::~Sha256()
{
    if (m_hash)
        ::BCryptDestroyHash(m_hash);
}

void Sha256::add(const void* data, std::size_t size)
{
    // BCryptHashData takes a ULONG: big blocks in pieces.
    const auto* p = static_cast<const unsigned char*>(data);
    while (m_hash && size > 0) {
        const ULONG n = static_cast<ULONG>(std::min<std::size_t>(size, 1u << 30));
        ::BCryptHashData(m_hash, const_cast<PUCHAR>(p), n, 0);
        p += n;
        size -= n;
    }
}

std::string Sha256::finish()
{
    unsigned char digest[32] {};
    if (m_hash) {
        ::BCryptFinishHash(m_hash, digest, sizeof digest, 0);
        ::BCryptDestroyHash(m_hash);
        m_hash = nullptr;
    }
    return hex(digest, sizeof digest);
}

std::string sha256Hex(const void* data, std::size_t size)
{
    Sha256 hash;
    hash.add(data, size);
    return hash.finish();
}

void setLogSink(std::function<void(const std::string&)> sink)
{
    const std::lock_guard lock(g_logMutex);
    g_logSink = std::move(sink);
}

void logInfo(const std::string& line)
{
    emit("info", line);
}

void logWarning(const std::string& line)
{
    emit("warning", line);
}

void logStopped(const char* what, const char* why) noexcept
{
    try {
        logWarning(std::string(what) + " stopped: " + (why ? why : "an unknown error"));
    } catch (...) {
        // Not even that could be had: the work ends quietly.
    }
}

std::string sizeText(std::uint64_t bytes)
{
    char text[32];
    if (bytes < 1024)
        std::snprintf(text, sizeof text, "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024 * 1024)
        std::snprintf(text, sizeof text, "%.0f KB", bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024)
        std::snprintf(text, sizeof text, bytes < 100ull * 1024 * 1024 ? "%.1f MB" : "%.0f MB", bytes / 1048576.0);
    else
        std::snprintf(text, sizeof text, "%.2f GB", bytes / 1073741824.0);
    return text;
}

} // namespace localsend
