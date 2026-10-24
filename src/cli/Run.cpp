#include "Run.h"

#include "Commands.h"
#include "Encoding.h"
#include "Interrupt.h"
#include "Launch.h"
#include "Messages.h"
#include "Win.h"

#include <algorithm>

namespace ws::cli {

namespace {

constexpr std::wstring_view kColorModes[] = {L"auto", L"always", L"never"};

ColorMode colorMode(std::wstring_view name)
{
    if (name == L"always")
        return ColorMode::Always;
    if (name == L"never")
        return ColorMode::Never;
    return ColorMode::Auto;
}

constexpr Text kBadEncoding {L"{} 只能是 utf8、utf16、ansi、oem 或 console，不是“{}”", L"{} must be utf8, utf16, ansi, oem or console, not \"{}\""};
constexpr Text kJsonOrNul {L"--json 和 -0 只能选一个", L"Use --json or -0, not both"};
constexpr Text kLongOrNul {L"-l 和 -0 只能选一个（-0 只输出路径，没有大小和时间）",
    L"Use -l or -0, not both (-0 prints the paths alone, with no size and time)"};
constexpr Text kWriteFailed {L"写出结果时出错，输出可能不完整：{}", L"Writing the output failed, so it may be incomplete: {}"};

// --encoding's value (or WS_ENCODING), looked for before the arguments are
// parsed: the language of the parser's messages depends on it. The last one
// counts, as the parser takes it. Whether it is a valid one, the parser
// (and, for WS_ENCODING, run()) say.
std::optional<EncodingChoice> encodingAsked(std::span<const std::wstring> args)
{
    constexpr std::wstring_view kWithValue = L"--encoding=";
    bool given = false;
    std::optional<EncodingChoice> last;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::wstring_view arg = args[i];
        if (arg == L"--")
            break;
        if (arg == L"--encoding") {
            given = true;
            last = i + 1 < args.size() ? encodingNamed(args[++i]) : std::nullopt;
        } else if (arg.starts_with(kWithValue)) {
            given = true;
            last = encodingNamed(arg.substr(kWithValue.size()));
        }
    }
    if (given)
        return last;
    if (const std::wstring fromEnvironment = environment(L"WS_ENCODING"); !fromEnvironment.empty())
        return encodingNamed(fromEnvironment);
    return std::nullopt;
}

} // namespace

// Tested with characters that only simplified Chinese has: Big5 (950) and
// Shift-JIS (932) hold the traditional ones the Taiwanese and Japanese code
// pages share, and Chinese in those would still be mostly '?'.
bool holdsChinese(UINT codePage)
{
    std::string bytes;
    return Encoder(codePage).append(bytes, L"这说简顺") == 0;
}

std::wstring lossyNote(std::size_t lines, UINT codePage, bool powerShell, bool hasJson)
{
    std::wstring note = fmt(msg::kLossy, lines, codePage);
    note += tr(powerShell ? msg::kLossyHintPowerShell : msg::kLossyHintEncoding);
    if (hasJson)
        note += tr(msg::kLossyHintJson);
    return note;
}

int run(std::span<const std::wstring> args)
{
    interrupt::install();
    const Config config = readConfig();
    const std::wstring wsLang = environment(L"WS_LANG");
    setLanguage(languageNamed(wsLang.empty() ? config.language : wsLang));

    // Where the output goes, and how (before anything is written, errors too).
    const std::optional<EncodingChoice> encoding = encodingAsked(args);
    const HANDLE stdoutHandle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE stderrHandle = ::GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    const bool redirected = !::GetConsoleMode(stdoutHandle, &mode) || !::GetConsoleMode(stderrHandle, &mode);
    // Who started ws decides how it is read, only when that matters.
    const bool powerShell = redirected && !encoding && parentIsPowerShell();
    const UINT codePage = redirectedCodePage(encoding, powerShell, ::GetConsoleOutputCP());
    if (redirected && language() == Lang::Zh && !holdsChinese(codePage))
        setLanguage(Lang::En); // words that can be read, rather than ? for each

    // The command: the first word when it names one, else find.
    const std::optional<std::size_t> first = firstWord(args);
    const Command* command = first ? commandNamed(args[*first]) : nullptr;
    const std::optional<std::size_t> skip = command ? first : std::nullopt;
    if (!command)
        command = &defaultCommand();
    Result<Parsed> parsed = parseArgs(args, skip, command->options, command->name, command->shared);
    static const Parsed kNothing;
    const Parsed& given = parsed ? *parsed : kNothing;

    std::optional<Failure> problem;
    if (!parsed)
        problem = parsed.failure();
    if (given.has(Opt::Encoding) && !encodingNamed(given.value(Opt::Encoding)) && !problem)
        problem = Failure {Exit::Usage, fmt(kBadEncoding, given.spelling(Opt::Encoding), given.value(Opt::Encoding))};
    // WS_ENCODING counts when --encoding is not given: a value ws does not know is said, not ignored.
    if (const std::wstring fromEnvironment = environment(L"WS_ENCODING");
        !given.has(Opt::Encoding) && !fromEnvironment.empty() && !encodingNamed(fromEnvironment) && !problem)
        problem = Failure {Exit::Usage, fmt(kBadEncoding, L"WS_ENCODING", fromEnvironment)};
    // Two output formats at once: neither could be the one asked for.
    if (given.has(Opt::Json) && given.has(Opt::Null) && !problem)
        problem = Failure {Exit::Usage, std::wstring(tr(kJsonOrNul))};
    if (given.has(Opt::Long) && given.has(Opt::Null) && !problem)
        problem = Failure {Exit::Usage, std::wstring(tr(kLongOrNul))};
    Result<std::wstring_view> color = choiceOption(given, Opt::Color, L"auto", kColorModes);
    if (!color && !problem)
        problem = color.failure();

    const ConsoleModes modes;
    Stream out(stdoutHandle, codePage);
    Stream err(stderrHandle, codePage);
    out.setPartner(&err);
    err.setPartner(&out);
    const ColorMode colors = colorMode(color ? *color : L"auto");
    out.setColored(wantColor(colors, out.console(), modes.vt(stdoutHandle)));
    err.setColored(wantColor(colors, err.console(), modes.vt(stderrHandle)));

    Context context {*command, given, out, err, config};
    context.pipe = given.has(Opt::Pipe) ? std::wstring(given.value(Opt::Pipe)) : pipe::defaultName();
    context.instanceMutex = given.has(Opt::Instance) ? std::wstring(given.value(Opt::Instance)) : std::wstring(launch::kInstanceMutex);
    context.launcher = systemLauncher(context.instanceMutex);
    context.mayStart = !given.has(Opt::NoStart);
    context.errVt = err.console() && modes.vt(stderrHandle);
    if (!problem) {
        if (Result<long long> wait = durationOption(given, Opt::Wait, 10'000); wait)
            context.waitMs = *wait;
        else
            problem = wait.failure();
    }

    Result<Exit> result = Exit::Found;
    if (problem)
        result = *problem;
    else if (given.has(Opt::Help))
        out.write(helpText(skip ? command : nullptr));
    else if (given.has(Opt::Version))
        result = runVersion(context);
    else if (args.empty())
        result = out.console() ? runBare(context) : Result<Exit>(Failure {Exit::Usage, std::wstring(tr(msg::kMissingQuery))});
    else
        result = command->run(context);

    Exit code = Exit::Found;
    if (result) {
        code = *result;
    } else {
        code = result.failure().code;
        if (!result.failure().message.empty())
            context.error(result.failure().message);
    }
    if (out.lossyLines() > 0) {
        const bool hasJson = std::ranges::find(command->options, Opt::Json) != command->options.end();
        context.note(lossyNote(out.lossyLines(), out.codePage(), powerShell, hasJson));
    }
    out.flush();
    // A write that failed for another reason than a reader that went away (a full
    // disk behind `> out.txt`): said once, and the run is not a success.
    if (out.writeError() != 0 && !interrupt::requested()) {
        context.error(fmt(kWriteFailed, systemMessage(out.writeError())));
        code = Exit::Failed;
    }
    err.flush();
    interrupt::finished();
    return static_cast<int>(interrupt::requested() ? Exit::Interrupted : code);
}

} // namespace ws::cli
