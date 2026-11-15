#include "Codec.h"

#include <windows.h>
#include <bcrypt.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace tools::codec {

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64Value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}

std::string toHex(const unsigned char* bytes, std::size_t n)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 15];
    }
    return out;
}

const wchar_t* algorithmId(const std::string& algorithm)
{
    if (algorithm == "md5")
        return BCRYPT_MD5_ALGORITHM;
    if (algorithm == "sha1")
        return BCRYPT_SHA1_ALGORITHM;
    if (algorithm == "sha256")
        return BCRYPT_SHA256_ALGORITHM;
    if (algorithm == "sha384")
        return BCRYPT_SHA384_ALGORITHM;
    if (algorithm == "sha512")
        return BCRYPT_SHA512_ALGORITHM;
    return nullptr;
}

// One digest over whatever `feed` hands over in pieces.
class Hasher {
public:
    explicit Hasher(const std::string& algorithm)
    {
        const wchar_t* id = algorithmId(algorithm);
        if (!id)
            return;
        if (::BCryptOpenAlgorithmProvider(&m_algorithm, id, nullptr, 0) != 0)
            return;
        DWORD length = 0;
        DWORD got = 0;
        if (::BCryptGetProperty(m_algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof length, &got, 0) != 0)
            return;
        m_digest.resize(length);
        if (::BCryptCreateHash(m_algorithm, &m_hash, nullptr, 0, nullptr, 0, 0) != 0)
            m_hash = nullptr;
    }
    ~Hasher()
    {
        if (m_hash)
            ::BCryptDestroyHash(m_hash);
        if (m_algorithm)
            ::BCryptCloseAlgorithmProvider(m_algorithm, 0);
    }
    bool valid() const { return m_hash != nullptr; }
    bool feed(const void* data, std::size_t n)
    {
        return m_hash && ::BCryptHashData(m_hash, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(n), 0) == 0;
    }
    std::string finish()
    {
        if (!m_hash || ::BCryptFinishHash(m_hash, m_digest.data(), static_cast<ULONG>(m_digest.size()), 0) != 0)
            return {};
        return toHex(m_digest.data(), m_digest.size());
    }

private:
    BCRYPT_ALG_HANDLE m_algorithm = nullptr;
    BCRYPT_HASH_HANDLE m_hash = nullptr;
    std::vector<unsigned char> m_digest;
};

constexpr std::int64_t kTicksPerSecond = 10000000; // FILETIME: 100 ns
constexpr std::int64_t kEpochTicks = 116444736000000000; // 1970-01-01 in FILETIME

std::int64_t ticksOf(const FILETIME& ft)
{
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

FILETIME fileTimeOf(const Moment& m)
{
    const std::int64_t ticks = kEpochTicks + m.seconds * kTicksPerSecond + static_cast<std::int64_t>(m.milliseconds) * 10000;
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFF);
    ft.dwHighDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(ticks) >> 32);
    return ft;
}

Moment momentOf(const FILETIME& ft)
{
    const std::int64_t ticks = ticksOf(ft) - kEpochTicks;
    Moment m;
    m.seconds = ticks / kTicksPerSecond;
    m.milliseconds = static_cast<int>((ticks % kTicksPerSecond) / 10000);
    if (m.milliseconds < 0) {
        m.milliseconds += 1000;
        m.seconds -= 1;
    }
    return m;
}

bool systemTimeOf(const Moment& m, bool local, SYSTEMTIME* out)
{
    const FILETIME utc = fileTimeOf(m);
    SYSTEMTIME st;
    if (!::FileTimeToSystemTime(&utc, &st))
        return false;
    if (!local) {
        *out = st;
        return true;
    }
    return ::SystemTimeToTzSpecificLocalTime(nullptr, &st, out) != 0;
}

std::string formatSystemTime(const SYSTEMTIME& st, bool withMs, char separator)
{
    char buffer[48];
    if (withMs)
        std::snprintf(buffer, sizeof buffer, "%04u-%02u-%02u%c%02u:%02u:%02u.%03u", st.wYear, st.wMonth, st.wDay, separator, st.wHour, st.wMinute,
            st.wSecond, st.wMilliseconds);
    else
        std::snprintf(buffer, sizeof buffer, "%04u-%02u-%02u%c%02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, separator, st.wHour, st.wMinute,
            st.wSecond);
    return buffer;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Reads up to `max` digits at `i`; false when there are none.
bool readNumber(const std::string& s, std::size_t& i, int max, int& out)
{
    int n = 0;
    out = 0;
    while (i < s.size() && n < max && isDigit(s[i])) {
        out = out * 10 + (s[i] - '0');
        ++i;
        ++n;
    }
    return n > 0;
}

} // namespace

std::string base64Encode(const std::string& bytes)
{
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < bytes.size()) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8)
            | static_cast<unsigned char>(bytes[i + 2]);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
        i += 3;
    }
    if (i + 1 == bytes.size()) {
        const unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::optional<std::string> base64Decode(const std::string& text)
{
    std::string clean;
    for (const char c : text) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            continue;
        clean += c;
    }
    while (!clean.empty() && clean.back() == '=')
        clean.pop_back();
    if (clean.empty() || clean.size() % 4 == 1)
        return std::nullopt;
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    for (const char c : clean) {
        const int v = base64Value(c);
        if (v < 0)
            return std::nullopt;
        buffer = (buffer << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xFF);
        }
    }
    return out;
}

bool validUtf8(const std::string& bytes)
{
    std::size_t i = 0;
    while (i < bytes.size()) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        std::size_t n = 0;
        if (c < 0x80) {
            if (c < 0x20 && c != '\n' && c != '\r' && c != '\t')
                return false; // control characters: not text
            n = 0;
        } else if ((c & 0xE0) == 0xC0 && c >= 0xC2)
            n = 1;
        else if ((c & 0xF0) == 0xE0)
            n = 2;
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4)
            n = 3;
        else
            return false;
        if (i + n >= bytes.size() + (n == 0 ? 1 : 0))
            return false;
        for (std::size_t k = 1; k <= n; ++k) {
            if ((static_cast<unsigned char>(bytes[i + k]) & 0xC0) != 0x80)
                return false;
        }
        i += n + 1;
    }
    return true;
}

std::string hashHex(const std::string& algorithm, const std::string& bytes)
{
    Hasher hasher(algorithm);
    if (!hasher.valid() || !hasher.feed(bytes.data(), bytes.size()))
        return {};
    return hasher.finish();
}

std::string hashFileHex(const std::string& algorithm, const std::wstring& path, std::uint64_t maxBytes, bool* tooLarge)
{
    *tooLarge = false;
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER size;
    if (!::GetFileSizeEx(file, &size) || static_cast<std::uint64_t>(size.QuadPart) > maxBytes) {
        ::CloseHandle(file);
        *tooLarge = true;
        return {};
    }
    Hasher hasher(algorithm);
    std::string out;
    if (hasher.valid()) {
        std::vector<char> chunk(1 << 20);
        for (;;) {
            DWORD got = 0;
            if (!::ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) || got == 0)
                break;
            if (!hasher.feed(chunk.data(), got)) {
                ::CloseHandle(file);
                return {};
            }
        }
        out = hasher.finish();
    }
    ::CloseHandle(file);
    return out;
}

std::string uuid4()
{
    unsigned char bytes[16];
    if (::BCryptGenRandom(nullptr, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        // The system RNG is always there; should it not be, the time and
        // the tick count are better than nothing.
        const unsigned long long t = ::GetTickCount64() ^ (static_cast<unsigned long long>(::GetCurrentProcessId()) << 32);
        std::memcpy(bytes, &t, 8);
        std::memcpy(bytes + 8, &t, 8);
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40); // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80); // variant 1
    const std::string hex = toHex(bytes, 16);
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

Moment now()
{
    FILETIME ft;
    ::GetSystemTimePreciseAsFileTime(&ft);
    return momentOf(ft);
}

std::optional<Moment> parseTimestamp(const std::string& given)
{
    std::string text;
    for (const char c : given) {
        if (c != ' ' && c != ',' && c != '_')
            text += c;
    }
    if (text.empty())
        return std::nullopt;
    std::size_t i = 0;
    bool negative = false;
    if (text[0] == '-') {
        negative = true;
        ++i;
    }
    std::string digits;
    std::string fraction;
    bool dot = false;
    for (; i < text.size(); ++i) {
        if (isDigit(text[i]))
            (dot ? fraction : digits) += text[i];
        else if (text[i] == '.' && !dot)
            dot = true;
        else
            return std::nullopt;
    }
    if (digits.empty() || digits.size() > 19)
        return std::nullopt;
    Moment m;
    const long long whole = std::strtoll(digits.c_str(), nullptr, 10);
    // Seconds are 10 digits today (11 to 2059); longer counts are a smaller
    // unit: milliseconds (12–13), microseconds (14–16, as Java and Go log),
    // nanoseconds (17–19). A fraction after a dot stays milliseconds; finer
    // than milliseconds rounds away.
    if (fraction.empty() && digits.size() >= 12) {
        long long subSecond = whole; // in the unit the digits say
        if (digits.size() >= 17) // nanoseconds to microseconds
            subSecond /= 1000;
        if (digits.size() >= 14) // microseconds to milliseconds
            subSecond /= 1000;
        m.seconds = subSecond / 1000;
        m.milliseconds = static_cast<int>(subSecond % 1000);
    } else {
        m.seconds = whole;
        if (!fraction.empty()) {
            fraction.resize(3, '0');
            m.milliseconds = std::atoi(fraction.c_str());
        }
    }
    if (negative) {
        m.seconds = -m.seconds;
        if (m.milliseconds) {
            m.seconds -= 1;
            m.milliseconds = 1000 - m.milliseconds;
        }
    }
    // Keep to what FILETIME can show: years 1601–30827.
    if (m.seconds < -11644473600LL || m.seconds > 910692730085LL)
        return std::nullopt;
    return m;
}

std::optional<Moment> parseDate(const std::string& s)
{
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    const auto dateSeparator = [&] { return i < s.size() && (s[i] == '-' || s[i] == '/' || s[i] == '.'); };
    int year, month, day;
    if (!readNumber(s, i, 4, year) || year < 1601 || !dateSeparator())
        return std::nullopt;
    ++i;
    if (!readNumber(s, i, 2, month) || month < 1 || month > 12 || !dateSeparator())
        return std::nullopt;
    ++i;
    if (!readNumber(s, i, 2, day) || day < 1 || day > 31)
        return std::nullopt;
    int hour = 0, minute = 0, second = 0, ms = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    if (i < s.size() && (s[i] == 'T' || s[i] == 't'))
        ++i;
    while (i < s.size() && s[i] == ' ')
        ++i;
    if (i < s.size() && isDigit(s[i])) {
        if (!readNumber(s, i, 2, hour) || hour > 23)
            return std::nullopt;
        if (i < s.size() && s[i] == ':') {
            ++i;
            if (!readNumber(s, i, 2, minute) || minute > 59)
                return std::nullopt;
            if (i < s.size() && s[i] == ':') {
                ++i;
                if (!readNumber(s, i, 2, second) || second > 60)
                    return std::nullopt;
                if (i < s.size() && (s[i] == '.' || s[i] == ',')) { // 15:04:05.123
                    ++i;
                    std::string frac;
                    while (i < s.size() && isDigit(s[i]))
                        frac += s[i++];
                    frac.resize(3, '0');
                    ms = std::atoi(frac.c_str());
                }
            }
        }
    }
    while (i < s.size() && s[i] == ' ')
        ++i;
    // The zone: Z, or +08:00 / -0500 / +8; none means local time.
    bool utc = false;
    int offsetMinutes = 0;
    bool zoned = false;
    if (i < s.size() && (s[i] == 'Z' || s[i] == 'z')) {
        utc = true;
        zoned = true;
        ++i;
    } else if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        const int sign = s[i] == '+' ? 1 : -1;
        ++i;
        int oh = 0, om = 0;
        if (!readNumber(s, i, 2, oh))
            return std::nullopt;
        if (i < s.size() && s[i] == ':')
            ++i;
        readNumber(s, i, 2, om);
        offsetMinutes = sign * (oh * 60 + om);
        zoned = true;
        utc = true;
    } else if (s.compare(i, 3, "UTC") == 0 || s.compare(i, 3, "GMT") == 0) {
        utc = true;
        zoned = true;
        i += 3;
    }
    while (i < s.size() && s[i] == ' ')
        ++i;
    if (i != s.size())
        return std::nullopt;
    SYSTEMTIME st {};
    st.wYear = static_cast<WORD>(year);
    st.wMonth = static_cast<WORD>(month);
    st.wDay = static_cast<WORD>(day);
    st.wHour = static_cast<WORD>(hour);
    st.wMinute = static_cast<WORD>(minute);
    st.wSecond = static_cast<WORD>(second == 60 ? 59 : second);
    st.wMilliseconds = static_cast<WORD>(ms);
    SYSTEMTIME asUtc = st;
    if (!zoned || !utc) {
        if (!::TzSpecificLocalTimeToSystemTime(nullptr, &st, &asUtc))
            return std::nullopt;
    }
    FILETIME ft;
    if (!::SystemTimeToFileTime(&asUtc, &ft))
        return std::nullopt;
    Moment m = momentOf(ft);
    m.seconds -= static_cast<std::int64_t>(offsetMinutes) * 60;
    return m;
}

std::string formatLocal(const Moment& m, bool withMs)
{
    SYSTEMTIME st;
    return systemTimeOf(m, true, &st) ? formatSystemTime(st, withMs, ' ') : std::string();
}

std::string formatUtc(const Moment& m, bool withMs)
{
    SYSTEMTIME st;
    return systemTimeOf(m, false, &st) ? formatSystemTime(st, withMs, ' ') : std::string();
}

int localOffsetMinutes(const Moment& m)
{
    SYSTEMTIME utc, local;
    if (!systemTimeOf(m, false, &utc) || !::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        return 0;
    FILETIME a, b;
    ::SystemTimeToFileTime(&utc, &a);
    ::SystemTimeToFileTime(&local, &b);
    return static_cast<int>((ticksOf(b) - ticksOf(a)) / (60 * kTicksPerSecond));
}

std::string formatIsoLocal(const Moment& m)
{
    SYSTEMTIME st;
    if (!systemTimeOf(m, true, &st))
        return {};
    const int offset = localOffsetMinutes(m);
    char zone[16];
    std::snprintf(zone, sizeof zone, "%c%02d:%02d", offset < 0 ? '-' : '+', std::abs(offset) / 60, std::abs(offset) % 60);
    return formatSystemTime(st, false, 'T') + zone;
}

std::string formatIsoUtc(const Moment& m)
{
    SYSTEMTIME st;
    return systemTimeOf(m, false, &st) ? formatSystemTime(st, false, 'T') + "Z" : std::string();
}

std::string localZoneName()
{
    const int offset = localOffsetMinutes(now());
    char zone[16];
    std::snprintf(zone, sizeof zone, "UTC%c%02d:%02d", offset < 0 ? '-' : '+', std::abs(offset) / 60, std::abs(offset) % 60);
    return zone;
}

} // namespace tools::codec
