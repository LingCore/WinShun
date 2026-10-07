#include "Wtf8.h"

namespace qf::wtf8 {

void append(std::string& out, std::u16string_view s)
{
    const std::size_t n = s.size();
    for (std::size_t i = 0; i < n; ++i) {
        char32_t c = s[i];
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            continue;
        }
        if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            continue;
        }
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            continue;
        }
        // BMP code point, or an unpaired surrogate (the "W" in WTF-8).
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

std::string fromUtf16(std::u16string_view utf16)
{
    std::string out;
    out.reserve(utf16.size() + utf16.size() / 2);
    append(out, utf16);
    return out;
}

void decodeAppend(std::u16string& out, std::string_view in)
{
    const std::size_t n = in.size();
    std::size_t i = 0;
    while (i < n) {
        const auto c = static_cast<unsigned char>(in[i]);
        if (c < 0x80) {
            out.push_back(c);
            ++i;
            continue;
        }
        int len = 0;
        char32_t cp = 0;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + len > n) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            const auto cc = static_cast<unsigned char>(in[i + k]);
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
        i += len;
    }
}

QString toQString(std::string_view bytes)
{
    std::u16string tmp;
    tmp.reserve(bytes.size());
    decodeAppend(tmp, bytes);
    return QString(reinterpret_cast<const QChar*>(tmp.data()), static_cast<qsizetype>(tmp.size()));
}

} // namespace qf::wtf8
