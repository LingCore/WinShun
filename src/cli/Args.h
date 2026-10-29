#pragma once

// The command line (docs/design-cli.md §3): GNU style. --long and --long=value,
// -s, -s value and -svalue, flags bundled (-l0), "--" ends the options, /? and
// -? ask for help. Options may come before or after the words. An unknown
// option is an error with the likeliest one named, never taken as a query
// word.

#include "Result.h"
#include "Text.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ws::cli {

enum class Opt : std::uint8_t {
    Limit,
    Files,
    Folders,
    Ext,
    In,
    Sort,
    Long,
    Json,
    Null,
    Days,
    From,
    Max,
    Timeout,
    Reveal,
    Kind,
    Install,
    // Every command's:
    Color,
    Encoding,
    Wait,
    NoStart,
    Help,
    Version,
    // Hidden, for the tests: another pipe, another single-instance mutex.
    Pipe,
    Instance,
    Count_
};

inline constexpr std::size_t kOptionCount = static_cast<std::size_t>(Opt::Count_);

struct OptionSpec {
    Opt id;
    std::wstring_view name; // without "--"
    wchar_t letter = 0; // short form, or none
    bool takesValue = false;
    bool repeatable = false; // every value kept (else the last counts)
    bool hidden = false; // not in the help, never suggested
    Text valueName {}; // "<数量>" in the help
    Text help {};
};

std::span<const OptionSpec> optionTable();
const OptionSpec& optionSpec(Opt id);

// The options every command takes.
inline constexpr std::array kGlobalOptions {Opt::Color, Opt::Encoding, Opt::Wait, Opt::NoStart, Opt::Help, Opt::Version, Opt::Pipe,
    Opt::Instance};

struct Parsed {
    std::vector<std::wstring> words; // the rest, in order (without the command's name)
    bool afterDashes = false; // a "--" came

    bool has(Opt id) const noexcept { return !values[index(id)].empty(); }
    // The value given last; empty when none.
    std::wstring_view value(Opt id) const noexcept
    {
        const auto& v = values[index(id)];
        return v.empty() ? std::wstring_view() : std::wstring_view(v.back());
    }
    const std::vector<std::wstring>& all(Opt id) const noexcept { return values[index(id)]; }
    // As the user wrote it last ("-n" or "--limit"), for messages.
    std::wstring_view spelling(Opt id) const noexcept { return spelled[index(id)]; }

    std::array<std::vector<std::wstring>, kOptionCount> values; // a flag: one empty string each time
    std::array<std::wstring, kOptionCount> spelled;

private:
    static constexpr std::size_t index(Opt id) noexcept { return static_cast<std::size_t>(id); }
};

// Where the first word that is no option (nor an option's value) is, before
// any "--": the command's name when it is one.
std::optional<std::size_t> firstWord(std::span<const std::wstring> args);

// Reads `args` for command `command` (its name, for messages), which takes
// `allowed` (and the global options in `shared`); the word at `skip` (its name)
// is left out. A wrong option: Exit::Usage with what to write instead.
Result<Parsed> parseArgs(std::span<const std::wstring> args, std::optional<std::size_t> skip, std::span<const Opt> allowed,
    std::wstring_view command, std::span<const Opt> shared = kGlobalOptions);

// The option most like `name` (a long name, without "--") among `among`;
// nothing when none is close enough.
std::optional<Opt> likeliest(std::wstring_view name, std::span<const Opt> among);

// Edit distance with neighbours swapped counting as one (Damerau, optimal
// string alignment), ignoring case.
std::size_t editDistance(std::wstring_view a, std::wstring_view b);

// Values, checked; an error names the option as it was written.
Result<long long> integerOption(const Parsed& args, Opt id, long long fallback, long long minimum, long long maximum);
// A time: 10s, 500ms, 2m, 1.5s; a plain number is seconds. In milliseconds.
Result<long long> durationOption(const Parsed& args, Opt id, long long fallbackMs);
std::optional<long long> parseDuration(std::wstring_view text); // milliseconds; nullopt when it is none
// One of `choices`, any case; the result as listed.
Result<std::wstring_view> choiceOption(const Parsed& args, Opt id, std::wstring_view fallback, std::span<const std::wstring_view> choices);

} // namespace ws::cli
