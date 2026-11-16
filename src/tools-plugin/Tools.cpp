// The winshun.tools plugin that comes with Win顺 (the plugin design, §5.6):
// a resident C++ plugin on the single-header SDK (src/plugins-sdk/winshun.h),
// no Qt, the C runtime linked in. Win顺 runs it inside its plugin host
// (WinShunPluginHost.exe, on a thread of its own); WinShunTools.exe runs the
// same alone, in the strict sandbox, when plugins are kept apart
// (WINSHUN_PLUGINS_APART=1). No clipboard either way: Enter copies through
// Win顺. Its commands:
//
//   =      calculator and unit conversion ("= 1+2*3", "12 inch to cm"; both
//          also without the keyword, by regex); the sums whose result was
//          taken are kept and listed under it, a row each (CalcHistory)
//   color  colour notations ("#ff8800", "rgb(255, 136, 0)"; by regex too)
//   ts     timestamps to dates and back ("ts", "ts 1700000000", "ts 2024-01-02 15:04")
//   uuid   a new UUID
//   b64    Base64 encode / decode
//   md5, sha1, sha256   hashes of text, or of a file by its path; a file's
//          right-click menu has 复制 MD5 / 复制 SHA-1 / 复制 SHA-256, the
//          digest straight to the clipboard

#include "Tools.h"

#include "winshun.h"

#include "Calc.h"
#include "CalcHistory.h"
#include "Codec.h"
#include "Color.h"
#include "Units.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using winshun::ActionRequest;
using winshun::Effects;
using winshun::Item;
using winshun::Items;
using winshun::Json;
using winshun::Query;

namespace {

constexpr const char* kCalcIcon = "ph:calculator";
constexpr const char* kTimeIcon = "ph:clock";
constexpr const char* kIdIcon = "ph:tag";
constexpr const char* kCodeIcon = "ph:code";
constexpr const char* kHashIcon = "ph:fingerprint";
constexpr const char* kClearIcon = "ph:trash";
constexpr std::uint64_t kMaxHashFile = 64ull << 20;
constexpr std::size_t kSumsListed = 50; // of the sums kept, under "="
constexpr std::size_t kSumsBelow = 20; // ... and under a sum being typed

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
        if (q.command == "hash" || q.command.compare(0, 5, "hash-") == 0)
            return hash(q);
        return {};
    }

    void initialized() override
    {
        m_sums.open(dataDir().empty() ? std::string() : dataDir() + "\\calc-history.txt", std::time(nullptr));
    }

    // Enter on a sum's result keeps the sum (CalcHistory); on a sum kept, its
    // result is copied again, or the whole line, or it is taken off the list.
    Effects action(const ActionRequest& a) override
    {
        if (a.trigger == "files")
            return a.files.size() == 1 ? copyFileDigest(a.command, a.files.front()) : Effects {};
        if (a.command != "calc")
            return Plugin::action(a);
        const std::string id = a.item["id"].toString();
        const Json& data = a.item["data"];
        if (id == "r" && data["sum"].isString()) {
            m_sums.add({std::time(nullptr), data["sum"].toString(), a.item["title"].toString(), a.item["copy"].toString()});
            return Plugin::action(a);
        }
        if (id == "clear") { // asks first: the sums cannot come back
            Item confirm("clear-confirm", zh("再按一次 Enter 清空", "Press Enter again to clear"),
                std::to_string(m_sums.records().size()) + zh(" 条，不能恢复", " sums; this cannot be undone"));
            confirm.withIcon(kClearIcon).withActions({{"confirm", zh("清空", "Clear"), "Enter"},
                {"keep", zh("留着，不清", "Keep them"), "Shift+Enter"}});
            return {winshun::effect::replace(Items {confirm}), winshun::effect::keep()};
        }
        if (id == "clear-confirm") {
            if (a.action == "keep") {
                Items left = sums(std::string(), kSumsListed, true);
                return {winshun::effect::replace(left.empty() ? calcHelp() : left), winshun::effect::keep()};
            }
            m_sums.clear();
            return {winshun::effect::replace(calcHelp()), winshun::effect::notify(zh("计算记录已清空", "The sums were cleared")),
                winshun::effect::keep()};
        }
        if (!data["when"].isNumber())
            return Plugin::action(a);
        const std::string sum = data["sum"].toString();
        if (a.action == "remove") {
            m_sums.remove(data["when"].toInt(), sum);
            Items left = sums(data["keyword"].toString(), kSumsListed, true);
            if (left.empty())
                left = calcHelp();
            return {winshun::effect::replace(left), winshun::effect::keep()};
        }
        if (a.action == "line") {
            const std::string line = sum + " = " + a.item["copy"].toString();
            return {winshun::effect::copy(line), winshun::effect::notify(zh("已复制算式和结果", "Copied the sum and its result")),
                winshun::effect::hide()};
        }
        return Plugin::action(a);
    }

private:
    // The file's right-click menu (the hash-md5 / -sha1 / -sha256 commands,
    // one file): that digest straight into the clipboard — Win顺 copies it,
    // the strict sandbox has no clipboard of its own — with a line of status.
    // An action's `replace` rows are listed by the launcher only under an
    // active keyword, so the menu copies instead of listing.
    Effects copyFileDigest(const std::string& command, const std::string& path)
    {
        using tools::codec::hashFileHex;
        const char* algorithm = command == "hash-md5" ? "md5" : command == "hash-sha1" ? "sha1" : "sha256";
        const char* shown = command == "hash-md5" ? "MD5" : command == "hash-sha1" ? "SHA-1" : "SHA-256";
        const DWORD attributes = ::GetFileAttributesW(winshun::wide(path).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
            return {winshun::effect::notify(zh("没有这个文件，查一下路径", "No such file; check the path"))};
        if (attributes & FILE_ATTRIBUTE_DIRECTORY)
            return {winshun::effect::notify(zh("这是一个文件夹，给它里面的文件", "That is a folder; name a file in it"))};
        bool tooLarge = false;
        const std::string digest = hashFileHex(algorithm, winshun::wide(path), kMaxHashFile, &tooLarge);
        if (tooLarge)
            return {winshun::effect::notify(zh("文件超过 64 MB，不算了", "The file is over 64 MB"))};
        if (digest.empty())
            return {winshun::effect::notify(zh("算不出摘要", "Cannot compute the digest"))};
        return {winshun::effect::copy(digest),
            winshun::effect::notify(std::string(zh("已复制 ", "Copied the ")) + shown)};
    }

    const char* zh(const char* chinese, const char* english) const { return this->chinese() ? chinese : english; }
    bool pushed(const Query& q) const { return q.trigger != "keyword"; }
    // Asked by an AI assistant or the command line: the sums kept are the
    // user's to see, and such a sum is not kept.
    bool commanded(const Query& q) const { return q.source == "command"; }

    Item help(const std::string& id, const std::string& title, const std::string& subtitle, const char* icon) const
    {
        return Item(id, title, subtitle).withIcon(icon).invalid();
    }

    // ---- = -----------------------------------------------------------------------

    Items calcHelp() const
    {
        Items items;
        items.push_back(help("h1", zh("输入算式，Enter 复制结果", "Type an expression; Enter copies the result"),
            zh("(1+2)*3、2^10、sqrt(2)、15% 、5!、0xff、pi、sin(rad(30))", "(1+2)*3, 2^10, sqrt(2), 15%, 5!, 0xff, pi, sin(rad(30))"), kCalcIcon));
        items.push_back(help("h2", zh("或者换算单位", "Or convert units"),
            zh("12 inch to cm、100 f to c、1.5 GB in MB、3 斤 换成 kg、2 小时 to 分钟", "12 inch to cm, 100 f to c, 1.5 GB in MB, 2 h to min"), kCalcIcon));
        items.push_back(help("h3", zh("算过的会记在这里", "Sums are kept here"),
            zh("按 Enter 复制过结果的算式，一行一条列在“=”下面，方便回头核对", "A sum whose result you took with Enter is listed under “=”, a row each, to check later"), kTimeIcon));
        return items;
    }

    // The sums kept, newest first, a row each: the result, the sum and when
    // under it. Tab puts the sum back in the box, to change and reckon again.
    Items sums(const std::string& keyword, std::size_t most, bool withClear) const
    {
        Items items;
        const long long now = std::time(nullptr);
        const std::vector<tools::calc::Record>& records = m_sums.records();
        const std::size_t count = std::min(most, records.size());
        for (std::size_t i = 0; i < count; ++i) {
            const tools::calc::Record& r = records[i];
            Item item("s" + std::to_string(r.when) + "-" + std::to_string(i), r.shown,
                r.expression + " · " + tools::calc::whenText(r.when, now, chinese()));
            item.withIcon(kTimeIcon)
                .withCopy(r.plain)
                .withPreview(r.expression + " = " + r.shown)
                .withAutocomplete((keyword.empty() ? std::string("=") : keyword) + " " + r.expression)
                .withScore(60 - static_cast<int>(std::min<std::size_t>(i, 55)))
                .withData(Json::Object {{"when", static_cast<double>(r.when)}, {"sum", r.expression}, {"keyword", keyword}})
                .withActions({{"copy", zh("复制结果", "Copy the result"), "Enter"},
                    {"line", zh("复制算式和结果", "Copy the sum and its result"), "Ctrl+Enter"},
                    {"remove", zh("删除这条记录", "Remove this sum"), "Shift+Enter"}});
            items.push_back(std::move(item));
        }
        if (withClear && count > 0)
            items.push_back(Item("clear", zh("清空计算记录", "Clear the sums"),
                std::to_string(records.size()) + zh(" 条；保留最近一个月、最多 200 条", " kept; a month's, 200 at most"))
                                .withIcon(kClearIcon)
                                .withScore(1));
        return items;
    }

    Items calc(const Query& q)
    {
        std::string text = winshun::trim(q.text);
        // "2024-1987=", as typed on a calculator: asked for, as after the keyword (GitHub #281).
        const bool asked = tools::calc::takeTrailingEquals(text);
        Items items;
        if (text.empty()) {
            if (pushed(q))
                return items;
            if (!commanded(q))
                items = sums(q.keyword, kSumsListed, true);
            return items.empty() ? calcHelp() : items;
        }
        // A conversion first: "12 inch to cm" is no arithmetic.
        if (const auto request = tools::units::parse(text)) {
            const std::vector<tools::units::Row> rows = tools::units::convert(*request, chinese());
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
        if (!commanded(q))
            items.back().withData(Json::Object {{"sum", text}}); // Enter keeps it: action()
        if (!pushed(q)) {
            const std::string hex = tools::calc::hex(result.value);
            if (!hex.empty() && result.value != 0) {
                items.push_back(Item("hex", hex, zh("十六进制", "Hexadecimal")).withIcon(kCalcIcon).withCopy(hex).withScore(70));
                items.push_back(Item("bin", tools::calc::binary(result.value), zh("二进制", "Binary")).withIcon(kCalcIcon).withCopy(tools::calc::binary(result.value)).withScore(69));
                items.push_back(Item("oct", tools::calc::octal(result.value), zh("八进制", "Octal")).withIcon(kCalcIcon).withCopy(tools::calc::octal(result.value)).withScore(68));
            }
            // The sums before this one under it, to check against.
            if (!commanded(q)) {
                for (Item& kept : sums(q.keyword, kSumsBelow, false))
                    items.push_back(std::move(kept));
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
                "#ff8800、#f80、rgb(255, 136, 0)、hsl(32, 100%, 50%)、rgba(255 136 0 / 50%)", "ph:palette"));
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
            // Milliseconds shown when the moment has them (a fractional
            // timestamp); whole seconds keep the plain form.
            const bool withMs = m.milliseconds != 0;
            const std::string local = formatLocal(m, withMs);
            items.push_back(Item("local", local, heading + " · " + zh("本地时间 ", "local time ") + zone).withIcon(kTimeIcon).withCopy(local).withScore(90));
            const std::string utc = formatUtc(m, withMs);
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
                zh("1700000000、1700000000000（毫秒）、1700000000000000（微秒）、2024-01-02 15:04:05、2024-01-02T15:04:05Z；留空看现在", "1700000000, 1700000000000 (ms), 1700000000000000 (µs), 2024-01-02 15:04:05, 2024-01-02T15:04:05Z; empty for now"), kTimeIcon));
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
        // A file-menu command (hash-md5 …) pins its one algorithm: the AI
        // calls it by name, the menu by its click.
        if (q.command.size() > 5 && q.command.compare(0, 5, "hash-") == 0)
            algorithms.assign(1, q.command.substr(5));
        if (text.empty()) {
            items.push_back(help("h1", zh("输入文字或文件路径", "Type text or a file path"),
                zh("文字按 UTF-8 计算；文件最大 64 MB", "Text is hashed as UTF-8; files up to 64 MB"), kHashIcon));
            return items;
        }
        // A file path: hash the file. A full path (C:\…, C:/…, \\server\share\…)
        // is a file; anything else is text, dots and slashes included
        // ("hello.world", "a/b"). A full path that is not there says so
        // instead of hashing the path itself, which would be silently wrong.
        std::string path = text;
        if (path.size() > 2 && path.front() == '"' && path.back() == '"')
            path = path.substr(1, path.size() - 2);
        const bool drivePath = path.size() > 3 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':'
            && (path[2] == '\\' || path[2] == '/');
        const bool pathLike = drivePath || (path.size() > 2 && path.compare(0, 2, "\\\\") == 0);
        const DWORD attributes = pathLike ? ::GetFileAttributesW(winshun::wide(path).c_str()) : INVALID_FILE_ATTRIBUTES;
        const bool exists = attributes != INVALID_FILE_ATTRIBUTES;
        if (pathLike && !exists) {
            items.push_back(help("missing", zh("没有这个文件，查一下路径", "No such file; check the path"), path, kHashIcon));
            return items;
        }
        const bool file = exists && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
        if (exists && !file) {
            items.push_back(help("dir", zh("这是一个文件夹，给它里面的文件", "That is a folder; name a file in it"), path, kHashIcon));
            return items;
        }
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

    tools::calc::History m_sums;
};

} // namespace

int tools::serve(HANDLE in, HANDLE out)
{
    return Tools().serve(in, out);
}
