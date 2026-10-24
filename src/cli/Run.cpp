#include "Run.h"

#include "Commands.h"
#include "Encoding.h"
#include "Interrupt.h"
#include "Launch.h"
#include "Messages.h"
#include "Win.h"

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

// --encoding's value (or WS_ENCODING), looked for before the arguments are
// parsed: the language of the parser's messages depends on it. The parser
// still says whether it is a valid one.
std::optional<EncodingChoice> encodingAsked(std::span<const std::wstring> args)
{
    constexpr std::wstring_view kWithValue = L"--encoding=";
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::wstring_view arg = args[i];
        if (arg == L"--")
            break;
        if (arg == L"--encoding")
            return i + 1 < args.size() ? encodingNamed(args[i + 1]) : std::nullopt;
        if (arg.starts_with(kWithValue))
            return encodingNamed(arg.substr(kWithValue.size()));
    }
    if (const std::wstring fromEnvironment = environment(L"WS_ENCODING"); !fromEnvironment.empty())
        return encodingNamed(fromEnvironment);
    return std::nullopt;
}

// Whether `codePage` can write Chinese: if not, Chinese messages would come
// out as rows of '?' (an English system locale's 437 or 1252 under
// PowerShell, or --encoding ansi there).
bool holdsChinese(UINT codePage)
{
    std::string bytes;
    return Encoder(codePage).append(bytes, L"中文提示") == 0;
}

} // namespace

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
    Result<Parsed> parsed = parseArgs(args, skip, command->options, command->name);
    static const Parsed kNothing;
    const Parsed& given = parsed ? *parsed : kNothing;

    std::optional<Failure> problem;
    if (!parsed)
        problem = parsed.failure();
    if (given.has(Opt::Encoding) && !encodingNamed(given.value(Opt::Encoding)) && !problem)
        problem = Failure {Exit::Usage, fmt(kBadEncoding, given.spelling(Opt::Encoding), given.value(Opt::Encoding))};
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
    if (out.lossyLines() > 0)
        context.note(fmt(msg::kLossy, out.lossyLines(), out.codePage()));
    out.flush();
    err.flush();
    interrupt::finished();
    return static_cast<int>(interrupt::requested() ? Exit::Interrupted : code);
}

} // namespace ws::cli
