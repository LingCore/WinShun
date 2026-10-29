#include "Args.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cwctype>

namespace ws::cli {

namespace {

// The options and what the help says of them. The help of each command
// lists the ones it takes (Commands.cpp); tst_cli checks that every option the
// help texts and SKILL.md mention is here.
constexpr OptionSpec kOptions[] = {
    {Opt::Limit, L"limit", L'n', true, false, false, {L"数量", L"N"},
        {L"最多几个（默认 100；0：能给多少给多少）", L"At most N results (default 100; 0: as many as allowed)"}},
    {Opt::Files, L"files", 0, false, false, false, {}, {L"只要文件", L"Files only"}},
    {Opt::Folders, L"folders", 0, false, false, false, {}, {L"只要文件夹", L"Folders only"}},
    {Opt::Ext, L"ext", 0, true, true, false, {L"扩展名", L"EXT"},
        {L"只要这些扩展名，逗号分开：--ext pdf,docx", L"Only these extensions, comma-separated: --ext pdf,docx"}},
    {Opt::In, L"in", 0, true, true, false, {L"文件夹", L"FOLDER"},
        {L"只在这个文件夹里找（可写多次；. 是当前文件夹）", L"Only in this folder (repeatable; . is the current folder)"}},
    {Opt::Sort, L"sort", 0, true, false, false, {L"顺序", L"ORDER"},
        {L"match（默认，最相关的在前）、newest、oldest、name、folder、type、largest、smallest",
            L"match (default, best first), newest, oldest, name, folder, type, largest, smallest"}},
    {Opt::Long, L"long", L'l', false, false, false, {}, {L"加上大小和修改时间", L"Add the size and the modified time"}},
    {Opt::Json, L"json", 0, false, false, false, {}, {L"输出一个 JSON 数组（一行）", L"Print one JSON array (one line)"}},
    {Opt::Null, L"null", L'0', false, false, false, {},
        {L"路径用 NUL 分隔（给 xargs -0）", L"Separate the paths with NUL (for xargs -0)"}},
    {Opt::Days, L"days", 0, true, false, false, {L"天数", L"N"},
        {L"只要最近几天改过的", L"Only what changed in the last N days"}},
    {Opt::From, L"from", 0, true, false, false, {L"位置", L"OFFSET"},
        {L"从这个位置接着读（上次提示的那个数）", L"Go on from this offset (the one ws said last time)"}},
    {Opt::Max, L"max", 0, true, false, false, {L"字数", L"N"},
        {L"最多读这么多字（默认 20000；0：全部）", L"At most N characters (default 20000; 0: all)"}},
    {Opt::Timeout, L"timeout", 0, true, false, false, {L"时间", L"TIME"},
        {L"最多搜这么久（默认 5s，最多 30s）", L"Search at most this long (default 5s, at most 30s)"}},
    {Opt::Reveal, L"reveal", 0, false, false, false, {},
        {L"在文件夹里显示它，不打开", L"Show it in its folder instead of opening it"}},
    {Opt::Kind, L"kind", 0, true, false, false, {L"种类", L"KIND"},
        {L"只要这一种：text、link、path、files、image", L"Only this kind: text, link, path, files, image"}},
    {Opt::Install, L"install", 0, true, false, false, {L"客户端", L"CLIENT"},
        {L"装进 AI 客户端：claude（Claude Code）", L"Install it for a client: claude (Claude Code)"}},
    {Opt::Color, L"color", 0, true, false, false, {L"何时", L"WHEN"},
        {L"颜色：auto（默认，只在控制台上）、always、never", L"Colours: auto (default, on a console only), always, never"}},
    {Opt::Encoding, L"encoding", 0, true, false, false, {L"编码", L"ENC"},
        {L"不是控制台时的编码：utf8、utf16、ansi、oem、console", L"Encoding when not on a console: utf8, utf16, ansi, oem, console"}},
    {Opt::Wait, L"wait", 0, true, false, false, {L"时间", L"TIME"},
        {L"索引没准备好时最多等多久（默认 10s；0：不等）", L"How long to wait for the index to be ready (default 10s; 0: not at all)"}},
    {Opt::NoStart, L"no-start", 0, false, false, false, {},
        {L"Win顺 没在运行时不启动它", L"Do not start WinShun when it is not running"}},
    {Opt::Help, L"help", L'h', false, false, false, {}, {L"帮助", L"Help"}},
    {Opt::Version, L"version", 0, false, false, false, {}, {L"ws 和 Win顺 的版本", L"The versions of ws and WinShun"}},
    {Opt::Pipe, L"pipe", 0, true, false, true, {}, {}},
    {Opt::Instance, L"instance", 0, true, false, true, {}, {}},
};
static_assert(std::size(kOptions) == kOptionCount);
constexpr bool inEnumOrder()
{
    for (std::size_t i = 0; i < std::size(kOptions); ++i) {
        if (static_cast<std::size_t>(kOptions[i].id) != i)
            return false;
    }
    return true;
}
static_assert(inEnumOrder(), "optionSpec() looks options up by their number");

constexpr Text kNoSuchOption {L"没有 {} 这个选项", L"There is no option {}"};
constexpr Text kDidYouMean {L"没有 {} 这个选项，是不是 {}？", L"There is no option {}. Did you mean {}?"};
constexpr Text kNotForCommand {L"ws {} 不认 {}（ws help {} 列出它认的）", L"ws {} does not take {} (ws help {} lists what it takes)"};
constexpr Text kNeedsValue {L"{} 后面要跟一个值", L"{} needs a value"};
constexpr Text kNoValue {L"{} 不带值", L"{} takes no value"};
constexpr Text kNotInteger {L"{} 要一个整数，不是“{}”", L"{} needs a whole number, not \"{}\""};
constexpr Text kOutOfRange {L"{} 要在 {} 到 {} 之间", L"{} must be between {} and {}"};
constexpr Text kNotDuration {L"{} 要一个时间，比如 10s、500ms、2m，不是“{}”", L"{} needs a time such as 10s, 500ms or 2m, not \"{}\""};
constexpr Text kNotChoice {L"{} 只能是 {}，不是“{}”", L"{} must be one of {}, not \"{}\""};

bool isAllowed(Opt id, std::span<const Opt> allowed, std::span<const Opt> shared) noexcept
{
    return std::ranges::find(allowed, id) != allowed.end() || std::ranges::find(shared, id) != shared.end();
}

const OptionSpec* byName(std::wstring_view name) noexcept
{
    for (const OptionSpec& spec : kOptions) {
        if (spec.name == name)
            return &spec;
    }
    return nullptr;
}

const OptionSpec* byLetter(wchar_t letter) noexcept
{
    for (const OptionSpec& spec : kOptions) {
        if (spec.letter != 0 && spec.letter == letter)
            return &spec;
    }
    return nullptr;
}

bool isHelpWord(std::wstring_view arg) noexcept
{
    return arg == L"/?" || arg == L"-?";
}

// An argument that is an option (or the end of them), not a word: "-" alone
// is a word (stdin, by convention), as is a negative number.
bool looksLikeOption(std::wstring_view arg) noexcept
{
    if (isHelpWord(arg))
        return true;
    return arg.size() >= 2 && arg[0] == L'-' && !(arg.size() >= 2 && std::iswdigit(arg[1]) && arg[1] != L'0');
}

Failure usage(std::wstring message)
{
    return {Exit::Usage, std::move(message)};
}

std::wstring dashed(const OptionSpec& spec)
{
    return L"--" + std::wstring(spec.name);
}

std::wstring choicesText(std::span<const std::wstring_view> choices)
{
    std::wstring text;
    for (const std::wstring_view choice : choices) {
        if (!text.empty())
            text += L", ";
        text += choice;
    }
    return text;
}

} // namespace

std::span<const OptionSpec> optionTable()
{
    return kOptions;
}

const OptionSpec& optionSpec(Opt id)
{
    return kOptions[static_cast<std::size_t>(id)];
}

std::optional<std::size_t> firstWord(std::span<const std::wstring> args)
{
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::wstring_view arg = args[i];
        if (arg == L"--")
            return std::nullopt;
        if (!looksLikeOption(arg))
            return i;
        if (isHelpWord(arg))
            continue;
        // Skip the value of an option that takes one and has none attached.
        if (arg.starts_with(L"--")) {
            if (arg.find(L'=') == std::wstring_view::npos) {
                if (const OptionSpec* spec = byName(arg.substr(2)); spec && spec->takesValue)
                    ++i;
            }
            continue;
        }
        for (std::size_t c = 1; c < arg.size(); ++c) {
            const OptionSpec* spec = byLetter(arg[c]);
            if (!spec)
                break;
            if (spec->takesValue) {
                if (c + 1 == arg.size())
                    ++i;
                break;
            }
        }
    }
    return std::nullopt;
}

Result<Parsed> parseArgs(std::span<const std::wstring> args, std::optional<std::size_t> skip, std::span<const Opt> allowed,
    std::wstring_view command, std::span<const Opt> shared)
{
    Parsed parsed;
    const auto take = [&](const OptionSpec& spec, std::wstring spelling, std::wstring value) {
        auto& values = parsed.values[static_cast<std::size_t>(spec.id)];
        if (!spec.repeatable)
            values.clear();
        values.push_back(std::move(value));
        parsed.spelled[static_cast<std::size_t>(spec.id)] = std::move(spelling);
    };
    const auto unknown = [&](std::wstring_view written, std::wstring_view name) -> Failure {
        if (const OptionSpec* spec = name.size() > 1 ? byName(name) : byLetter(name.empty() ? 0 : name.front()); spec && !spec->hidden)
            return usage(fmt(kNotForCommand, command, written, command));
        if (name.size() > 1) {
            std::vector<Opt> mine;
            for (const OptionSpec& spec : kOptions) {
                if (!spec.hidden && isAllowed(spec.id, allowed, shared))
                    mine.push_back(spec.id);
            }
            if (const std::optional<Opt> best = likeliest(name, mine))
                return usage(fmt(kDidYouMean, written, dashed(optionSpec(*best))));
        }
        return usage(fmt(kNoSuchOption, written));
    };

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (skip && i == *skip)
            continue;
        const std::wstring& arg = args[i];
        if (parsed.afterDashes || !looksLikeOption(arg)) {
            parsed.words.push_back(arg);
            continue;
        }
        if (arg == L"--") {
            parsed.afterDashes = true;
            continue;
        }
        if (isHelpWord(arg)) {
            take(optionSpec(Opt::Help), arg, {});
            continue;
        }
        if (arg.starts_with(L"--")) {
            const std::size_t equals = arg.find(L'=');
            const std::wstring_view name = std::wstring_view(arg).substr(2, equals == std::wstring::npos ? std::wstring::npos : equals - 2);
            const std::wstring written = L"--" + std::wstring(name);
            const OptionSpec* spec = byName(name);
            if (!spec || !isAllowed(spec->id, allowed, shared))
                return unknown(written, name);
            if (!spec->takesValue) {
                if (equals != std::wstring::npos)
                    return usage(fmt(kNoValue, written));
                take(*spec, written, {});
                continue;
            }
            if (equals != std::wstring::npos) {
                take(*spec, written, arg.substr(equals + 1));
                continue;
            }
            if (i + 1 >= args.size() || (skip && i + 1 == *skip))
                return usage(fmt(kNeedsValue, written));
            take(*spec, written, args[++i]);
            continue;
        }
        // -abc: flags, the last (or the first that takes a value) with its value.
        for (std::size_t c = 1; c < arg.size(); ++c) {
            const std::wstring written {L'-', arg[c]};
            const OptionSpec* spec = byLetter(arg[c]);
            if (!spec || !isAllowed(spec->id, allowed, shared))
                return unknown(written, std::wstring_view(&arg[c], 1));
            if (!spec->takesValue) {
                take(*spec, written, {});
                continue;
            }
            if (c + 1 < arg.size()) {
                std::wstring_view rest = std::wstring_view(arg).substr(c + 1);
                if (rest.starts_with(L'='))
                    rest.remove_prefix(1);
                take(*spec, written, std::wstring(rest));
            } else {
                if (i + 1 >= args.size() || (skip && i + 1 == *skip))
                    return usage(fmt(kNeedsValue, written));
                take(*spec, written, args[++i]);
            }
            break;
        }
    }
    return parsed;
}

std::size_t editDistance(std::wstring_view a, std::wstring_view b)
{
    const auto lower = [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); };
    const std::size_t n = a.size();
    const std::size_t m = b.size();
    std::vector<std::size_t> d((n + 1) * (m + 1));
    const auto at = [&](std::size_t i, std::size_t j) -> std::size_t& { return d[i * (m + 1) + j]; };
    for (std::size_t i = 0; i <= n; ++i)
        at(i, 0) = i;
    for (std::size_t j = 0; j <= m; ++j)
        at(0, j) = j;
    for (std::size_t i = 1; i <= n; ++i) {
        for (std::size_t j = 1; j <= m; ++j) {
            const std::size_t cost = lower(a[i - 1]) == lower(b[j - 1]) ? 0 : 1;
            at(i, j) = std::min({at(i - 1, j) + 1, at(i, j - 1) + 1, at(i - 1, j - 1) + cost});
            if (i > 1 && j > 1 && lower(a[i - 1]) == lower(b[j - 2]) && lower(a[i - 2]) == lower(b[j - 1]))
                at(i, j) = std::min(at(i, j), at(i - 2, j - 2) + 1);
        }
    }
    return at(n, m);
}

std::optional<Opt> likeliest(std::wstring_view name, std::span<const Opt> among)
{
    std::optional<Opt> best;
    std::size_t bestDistance = 0;
    for (const Opt id : among) {
        const OptionSpec& spec = optionSpec(id);
        if (spec.hidden)
            continue;
        // A beginning of exactly one name ("--fold"): that one.
        std::size_t distance = editDistance(name, spec.name);
        if (name.size() >= 2 && spec.name.starts_with(name))
            distance = std::min<std::size_t>(distance, 1);
        if (!best || distance < bestDistance) {
            best = id;
            bestDistance = distance;
        }
    }
    // Close enough: one edit for short names, two for longer ones.
    const std::size_t allowedDistance = name.size() <= 4 ? 1 : 2;
    if (!best || bestDistance > allowedDistance)
        return std::nullopt;
    return best;
}

Result<long long> integerOption(const Parsed& args, Opt id, long long fallback, long long minimum, long long maximum)
{
    if (!args.has(id))
        return fallback;
    const std::wstring_view text = args.value(id);
    std::string ascii;
    bool plain = !text.empty() && text.size() <= 20;
    for (const wchar_t c : text) {
        plain = plain && c < 0x80;
        ascii += static_cast<char>(c & 0x7F);
    }
    long long value = 0;
    if (plain) {
        const auto [end, error] = std::from_chars(ascii.data(), ascii.data() + ascii.size(), value);
        plain = error == std::errc() && end == ascii.data() + ascii.size();
    }
    if (!plain)
        return usage(fmt(kNotInteger, args.spelling(id), text));
    if (value < minimum || value > maximum)
        return usage(fmt(kOutOfRange, args.spelling(id), minimum, maximum));
    return value;
}

std::optional<long long> parseDuration(std::wstring_view text)
{
    std::wstring_view number = text;
    double unitMs = 1000;
    if (text.ends_with(L"ms")) {
        number = text.substr(0, text.size() - 2);
        unitMs = 1;
    } else if (text.ends_with(L's') || text.ends_with(L'S')) {
        number = text.substr(0, text.size() - 1);
    } else if (text.ends_with(L'm') || text.ends_with(L'M')) {
        number = text.substr(0, text.size() - 1);
        unitMs = 60'000;
    }
    if (number.empty() || number.size() > 20)
        return std::nullopt;
    std::string ascii;
    for (const wchar_t c : number) {
        if (!(std::iswdigit(c) && c < 0x80) && c != L'.')
            return std::nullopt;
        ascii += static_cast<char>(c);
    }
    double value = 0;
    const auto [end, error] = std::from_chars(ascii.data(), ascii.data() + ascii.size(), value);
    if (error != std::errc() || end != ascii.data() + ascii.size() || !std::isfinite(value) || value < 0)
        return std::nullopt;
    const double ms = value * unitMs;
    if (ms > 86'400'000.0) // a day: enough for any wait
        return std::nullopt;
    return static_cast<long long>(std::llround(ms));
}

Result<long long> durationOption(const Parsed& args, Opt id, long long fallbackMs)
{
    if (!args.has(id))
        return fallbackMs;
    const std::optional<long long> ms = parseDuration(args.value(id));
    if (!ms)
        return usage(fmt(kNotDuration, args.spelling(id), args.value(id)));
    return *ms;
}

Result<std::wstring_view> choiceOption(const Parsed& args, Opt id, std::wstring_view fallback, std::span<const std::wstring_view> choices)
{
    if (!args.has(id))
        return fallback;
    const std::wstring_view given = args.value(id);
    for (const std::wstring_view choice : choices) {
        if (::CompareStringOrdinal(given.data(), static_cast<int>(given.size()), choice.data(), static_cast<int>(choice.size()), TRUE)
            == CSTR_EQUAL)
            return choice;
    }
    return usage(fmt(kNotChoice, args.spelling(id), choicesText(choices), given));
}

} // namespace ws::cli
