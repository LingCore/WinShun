// The winshun.tools plugin that comes with Win顺 (the plugin design, §5.6):
// a resident C++ plugin on the single-header SDK (src/plugins-sdk/winshun.h),
// no Qt, the C runtime linked in. Win顺 runs it inside its plugin host
// (WinShunPluginHost.exe, on a thread of its own); WinShunTools.exe runs the
// same alone, in the strict sandbox, when plugins are kept apart
// (WINSHUN_PLUGINS_APART=1). No clipboard either way: Enter copies through
// Win顺. Its commands:
//
//   =      calculator and unit conversion ("= 1+2*3", "12 inch to cm"; both
//          also without the keyword, by regex)
//   color  colour notations ("#ff8800", "rgb(255, 136, 0)"; by regex too)
//   ts     timestamps to dates and back ("ts", "ts 1700000000", "ts 2024-01-02 15:04")
//   uuid   a new UUID
//   b64    Base64 encode / decode
//   md5, sha1, sha256   hashes of text, or of a file by its path

#include "Tools.h"

#include "winshun.h"

#include "Calc.h"
#include "Codec.h"
#include "Color.h"
#include "Units.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using winshun::Item;
using winshun::Items;
using winshun::Query;

namespace {

constexpr const char* kCalcIcon = "glyph:U+E8EF"; // Calculator
constexpr const char* kTimeIcon = "glyph:U+E823"; // Recent
constexpr const char* kIdIcon = "glyph:U+E8D7"; // Tag
constexpr const char* kCodeIcon = "glyph:U+E943"; // Code
constexpr const char* kHashIcon = "glyph:U+E928"; // Fingerprint
constexpr std::uint64_t kMaxHashFile = 64ull << 20;

class Tools : public winshun::Plugin {
protected:
    std::string name() const override { return "winshun.tools"; }

    Items query(const Query& q) override
    {
        if (q.command == "calc")
            return calc(q);
        if (q.command == "color")
            return color(q);
        if (q.command == "time")
            return time(q);
        if (q.command == "uuid")
            return uuid(q);
        if (q.command == "b64")
            return base64(q);
        if (q.command == "hash")
            return hash(q);
        return {};
    }

private:
    const char* zh(const char* chinese, const char* english) const { return this->chinese() ? chinese : english; }
    bool pushed(const Query& q) const { return q.trigger != "keyword"; }

    Item help(const std::string& id, const std::string& title, const std::string& subtitle, const char* icon) const
    {
        return Item(id, title, subtitle).withIcon(icon).invalid();
    }

    // ---- = -----------------------------------------------------------------------

    Items calc(const Query& q)
    {
        std::string text = winshun::trim(q.text);
        // "2024-1987=", as typed on a calculator: asked for, as after the keyword (GitHub #281).
        const bool asked = tools::calc::takeTrailingEquals(text);
        Items items;
        if (text.empty()) {
            if (pushed(q))
                return items;
            items.push_back(help("h1", zh("输入算式，Enter 复制结果", "Type an expression; Enter copies the result"),
                zh("(1+2)*3、2^10、sqrt(2)、15% 、5!、0xff、pi、sin(rad(30))", "(1+2)*3, 2^10, sqrt(2), 15%, 5!, 0xff, pi, sin(rad(30))"), kCalcIcon));
            items.push_back(help("h2", zh("或者换算单位", "Or convert units"),
                zh("12 inch to cm、100 f to c、1.5 GB in MB、3 斤 换成 kg、2 小时 to 分钟", "12 inch to cm, 100 f to c, 1.5 GB in MB, 2 h to min"), kCalcIcon));
            return items;
        }
        // A conversion first: "12 inch to cm" is no arithmetic.
        if (const auto request = tools::units::parse(text)) {
            const std::vector<tools::units::Row> rows = tools::units::convert(*request);
            int n = 0;
            for (const tools::units::Row& row : rows) {
                const std::string value = row.value + " " + row.unit;
                std::string subtitle = tools::units::formatValue(request->value) + " " + request->from->id + " = " + value;
                if (!row.note.empty())
                    subtitle += " · " + row.note;
                items.push_back(Item("u" + std::to_string(n), value, subtitle).withIcon(kCalcIcon).withCopy(row.value).withScore(row.alternate ? 60 : 90 - n));
                ++n;
                if (pushed(q) && n >= 2)
                    break;
            }
            return items;
        }
        // Typed without "=", 1998-2003 is a span of years to search for; the
        // manifest's trigger cannot tell it from 2024-1987 (GitHub #281).
        if (pushed(q) && !asked && tools::calc::looksLikeYearRange(text))
            return items;
        const tools::calc::Result result = tools::calc::evaluate(text);
        if (!result.ok()) {
            if (pushed(q))
                return items; // a sum that does not add up is not worth a row among the files
            items.push_back(help("err", errorText(result), text, kCalcIcon));
            return items;
        }
        const std::string shown = tools::calc::format(result.value);
        const std::string plain = tools::calc::format(result.value, true);
        items.push_back(Item("r", shown, text + " = " + plain).withIcon(kCalcIcon).withCopy(plain).withScore(95));
        if (!pushed(q)) {
            const std::string hex = tools::calc::hex(result.value);
            if (!hex.empty() && result.value != 0) {
                items.push_back(Item("hex", hex, zh("十六进制", "Hexadecimal")).withIcon(kCalcIcon).withCopy(hex));
                items.push_back(Item("bin", tools::calc::binary(result.value), zh("二进制", "Binary")).withIcon(kCalcIcon).withCopy(tools::calc::binary(result.value)));
                items.push_back(Item("oct", tools::calc::octal(result.value), zh("八进制", "Octal")).withIcon(kCalcIcon).withCopy(tools::calc::octal(result.value)));
            }
        }
        return items;
    }

    std::string errorText(const tools::calc::Result& r) const
    {
        using tools::calc::Error;
        switch (r.error) {
        case Error::Empty: return zh("没有算式", "Nothing to compute");
        case Error::Syntax: return zh("算式不完整或有错", "The expression is incomplete or wrong");
        case Error::UnknownName: return std::string(zh("不认识 ", "Unknown name ")) + r.detail;
        case Error::DivisionByZero: return zh("不能除以零", "Division by zero");
        case Error::OutOfRange: return zh("结果超出范围，或没有实数结果", "Out of range, or no real result");
        case Error::WrongArguments: return zh("函数的参数个数不对", "Wrong number of arguments");
        case Error::None: break;
        }
        return {};
    }

    // ---- color -------------------------------------------------------------------

    Items color(const Query& q)
    {
        const std::string text = winshun::trim(q.text);
        Items items;
        const auto parsed = tools::color::parse(text);
        if (!parsed) {
            if (pushed(q))
                return items;
            items.push_back(help("h1", zh("输入一个颜色值，列出它的各种写法", "Type a colour to see it in every notation"),
                "#ff8800、#f80、rgb(255, 136, 0)、hsl(32, 100%, 50%)、rgba(255 136 0 / 50%)", "glyph:U+E790"));
            return items;
        }
        const std::string swatch = "color:" + tools::color::hex(*parsed, false);
        int n = 0;
        for (const tools::color::Notation& notation : tools::color::notations(*parsed)) {
            std::string subtitle = notation.label;
            if (parsed->a < 1)
                subtitle += std::string(" · ") + zh("不透明度 ", "opacity ") + std::to_string(static_cast<int>(std::lround(parsed->a * 100))) + "%";
            items.push_back(Item("c" + std::to_string(n), notation.value, subtitle).withIcon(swatch).withCopy(notation.value).withScore(90 - n));
            ++n;
            if (pushed(q) && n >= 1)
                break; // one swatch among the files; the keyword lists them all
        }
        if (pushed(q) && !items.empty())
            items[0].subtitle += std::string(" · ") + zh("color 查看全部写法", "color: every notation");
        return items;
    }

    // ---- ts ----------------------------------------------------------------------

    Items time(const Query& q)
    {
        using namespace tools::codec;
        const std::string text = winshun::trim(q.text);
        Items items;
        const std::string zone = localZoneName();
        const auto moment = [&](const Moment& m, const std::string& heading) {
            const std::string local = formatLocal(m);
            items.push_back(Item("local", local, heading + " · " + zh("本地时间 ", "local time ") + zone).withIcon(kTimeIcon).withCopy(local).withScore(90));
            const std::string utc = formatUtc(m);
            items.push_back(Item("utc", utc, "UTC").withIcon(kTimeIcon).withCopy(utc));
            const std::string isoLocal = formatIsoLocal(m);
            items.push_back(Item("isoLocal", isoLocal, "ISO 8601").withIcon(kTimeIcon).withCopy(isoLocal));
            const std::string isoUtc = formatIsoUtc(m);
            items.push_back(Item("isoUtc", isoUtc, "ISO 8601 · UTC").withIcon(kTimeIcon).withCopy(isoUtc));
        };
        const auto stamps = [&](const Moment& m, const std::string& heading) {
            const std::string seconds = std::to_string(m.seconds);
            const std::string ms = std::to_string(m.seconds * 1000 + m.milliseconds);
            items.push_back(Item("s", seconds, heading + " · " + zh("Unix 时间戳（秒）", "Unix timestamp (seconds)")).withIcon(kTimeIcon).withCopy(seconds).withScore(90));
            items.push_back(Item("ms", ms, zh("毫秒", "milliseconds")).withIcon(kTimeIcon).withCopy(ms));
        };
        if (text.empty()) {
            if (pushed(q))
                return items;
            const Moment m = now();
            stamps(m, zh("现在", "now"));
            moment(m, zh("现在", "now"));
            return items;
        }
        if (const auto m = parseTimestamp(text)) {
            moment(*m, text);
            if (pushed(q)) {
                items.resize(1);
                items[0].subtitle += std::string(" · ") + zh("ts 查看更多", "ts: more");
            }
            return items;
        }
        if (const auto m = parseDate(text)) {
            stamps(*m, text);
            items.push_back(Item("iso", formatIsoUtc(*m), "ISO 8601 · UTC").withIcon(kTimeIcon).withCopy(formatIsoUtc(*m)));
            return items;
        }
        if (!pushed(q))
            items.push_back(help("h1", zh("输入时间戳或日期", "Type a timestamp or a date"),
                zh("1700000000、1700000000000（毫秒）、2024-01-02 15:04:05、2024-01-02T15:04:05Z；留空看现在", "1700000000, 1700000000000 (ms), 2024-01-02 15:04:05, 2024-01-02T15:04:05Z; empty for now"), kTimeIcon));
        return items;
    }

    // ---- uuid --------------------------------------------------------------------

    Items uuid(const Query& q)
    {
        (void)q;
        const std::string id = tools::codec::uuid4();
        std::string upper = id;
        for (char& c : upper)
            c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
        std::string bare;
        for (const char c : id) {
            if (c != '-')
                bare += c;
        }
        Items items;
        items.push_back(Item("lower", id, zh("UUID v4 · Enter 复制", "UUID v4 · Enter copies")).withIcon(kIdIcon).withCopy(id).withScore(90));
        items.push_back(Item("upper", upper, zh("大写", "upper case")).withIcon(kIdIcon).withCopy(upper));
        items.push_back(Item("braces", "{" + id + "}", zh("带花括号（注册表、COM）", "with braces (registry, COM)")).withIcon(kIdIcon).withCopy("{" + id + "}"));
        items.push_back(Item("bare", bare, zh("无连字符", "no hyphens")).withIcon(kIdIcon).withCopy(bare));
        return items;
    }

    // ---- b64 ---------------------------------------------------------------------

    Items base64(const Query& q)
    {
        using namespace tools::codec;
        const std::string text = winshun::trim(q.text);
        Items items;
        if (text.empty()) {
            items.push_back(help("h1", zh("输入文字：编码成 Base64；输入 Base64：解码", "Type text to encode, or Base64 to decode"), "", kCodeIcon));
            return items;
        }
        const std::string encoded = base64Encode(text);
        items.push_back(Item("enc", encoded, zh("Base64 编码", "Base64 encoded")).withIcon(kCodeIcon).withCopy(encoded).withScore(80));
        if (const auto decoded = base64Decode(text)) {
            if (validUtf8(*decoded) && !decoded->empty())
                items.insert(items.begin(), Item("dec", *decoded, zh("Base64 解码", "Base64 decoded")).withIcon(kCodeIcon).withCopy(*decoded).withScore(90));
            else if (!decoded->empty())
                items.push_back(help("bin", std::string(zh("解码后是 ", "Decodes to ")) + std::to_string(decoded->size()) + zh(" 字节的二进制数据", " bytes of binary data"), "", kCodeIcon));
        }
        return items;
    }

    // ---- md5, sha1, sha256 -----------------------------------------------------------

    Items hash(const Query& q)
    {
        using namespace tools::codec;
        std::string text = winshun::trim(q.text);
        Items items;
        // The algorithm: the keyword typed, or named first in the text
        // ("sha256 abc", as an AI would ask), else all three.
        std::vector<std::string> algorithms;
        std::string keyword = q.keyword;
        for (const char* name : {"sha256", "sha1", "md5", "sha512", "sha384"}) {
            const std::size_t n = std::strlen(name);
            if (text.size() > n && text.compare(0, n, name) == 0 && text[n] == ' ') {
                keyword = name;
                text = winshun::trim(text.substr(n));
                break;
            }
        }
        if (!keyword.empty())
            algorithms.push_back(keyword);
        else
            algorithms = {"md5", "sha1", "sha256"};
        if (text.empty()) {
            items.push_back(help("h1", zh("输入文字或文件路径", "Type text or a file path"),
                zh("文字按 UTF-8 计算；文件最大 64 MB", "Text is hashed as UTF-8; files up to 64 MB"), kHashIcon));
            return items;
        }
        // A file path: hash the file.
        std::string path = text;
        if (path.size() > 2 && path.front() == '"' && path.back() == '"')
            path = path.substr(1, path.size() - 2);
        const DWORD attributes = path.size() > 3 && path[1] == ':' ? ::GetFileAttributesW(winshun::wide(path).c_str()) : INVALID_FILE_ATTRIBUTES;
        const bool file = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
        int n = 0;
        for (const std::string& algorithm : algorithms) {
            std::string upper = algorithm;
            for (char& c : upper)
                c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
            std::string digest;
            if (file) {
                bool tooLarge = false;
                digest = hashFileHex(algorithm, winshun::wide(path), kMaxHashFile, &tooLarge);
                if (tooLarge) {
                    items.push_back(help("big", zh("文件超过 64 MB，不算了", "The file is over 64 MB"), path, kHashIcon));
                    return items;
                }
            } else
                digest = hashHex(algorithm, text);
            if (digest.empty()) {
                items.push_back(help("fail" + algorithm, std::string(zh("算不出 ", "Cannot compute ")) + upper, text, kHashIcon));
                continue;
            }
            const std::string subtitle = upper + (file ? std::string(" · ") + zh("文件 ", "file ") + path : std::string(" · ") + zh("文字（UTF-8）", "text (UTF-8)"));
            items.push_back(Item(algorithm, digest, subtitle).withIcon(kHashIcon).withCopy(digest).withScore(90 - n++));
        }
        return items;
    }
};

} // namespace

int tools::serve(HANDLE in, HANDLE out)
{
    return Tools().serve(in, out);
}
