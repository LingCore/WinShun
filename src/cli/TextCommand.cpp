// text: a file's text as Win顺 reads it (documents too), in parts.

#include "Commands.h"
#include "Encoding.h"
#include "Interrupt.h"
#include "Messages.h"
#include "Win.h"

#include <algorithm>
#include <limits>

namespace ws::cli {

namespace {

// What one read_file_text call may give: a newer Win顺 gives the command line
// this much at once (older ones 100 000, and at least 1 000).
constexpr long long kMostPerCall = 4'000'000;
constexpr long long kDefaultMax = 20'000;

} // namespace

Result<Exit> runText(Context& context)
{
    const Parsed& args = context.args;
    if (args.words.empty())
        return Failure {Exit::Usage, std::wstring(tr(msg::kNeedsFile))};
    // Several words: a path with spaces, written without quotes.
    std::wstring given;
    for (const std::wstring& word : args.words) {
        if (!given.empty())
            given += L' ';
        given += word;
    }
    if (Result<void> clean = noQuoteIn(given); !clean)
        return std::move(clean).failure();
    const std::wstring path = absolutePath(given);
    const PathKind kindHere = pathKind(path);
    if (args.words.size() > 1 && kindHere == PathKind::None)
        return Failure {Exit::Usage, std::wstring(tr(msg::kOneFile))};
    if (kindHere == PathKind::Folder) // ws sees it is one: no need to ask
        return Failure {Exit::Usage, fmt(msg::kIsFolder, path)};
    Result<long long> from = integerOption(args, Opt::From, 0, 0, std::numeric_limits<int>::max());
    if (!from)
        return std::move(from).failure();
    Result<long long> max = integerOption(args, Opt::Max, kDefaultMax, 0, std::numeric_limits<long long>::max());
    if (!max)
        return std::move(max).failure();
    const long long wanted = *max == 0 ? std::numeric_limits<long long>::max() : *max;

    Result<std::unique_ptr<Connection>> connection = connect(context);
    if (!connection)
        return std::move(connection).failure();

    std::wstring text; // with --json, all of it; else what is still to be written
    long long offset = *from;
    long long collected = 0;
    long long total = 0;
    long long next = -1; // where the rest begins; -1: nothing is left
    bool cut = false;
    std::string kind;
    const bool json = args.has(Opt::Json);
    Stream& out = context.out;
    bool endsWithNewline = true;
    long long perCall = kMostPerCall;
    for (;;) {
        const long long ask = std::min(wanted - collected, perCall);
        json::Value::Object arguments {{"path", utf8(path)}, {"offset", offset}, {"max_chars", ask}};
        std::string refusal;
        Result<json::Value> result = callTool(context, **connection, "read_file_text", json::Value(std::move(arguments)), false, path, &refusal);
        if (!result && refusal == "too_large" && ask > 1'000) {
            // Text written longer as JSON (control characters, \uXXXX)
            // than one answer may be: the same in smaller parts.
            perCall = std::max<long long>(1'000, ask / 4);
            continue;
        }
        if (!result)
            return std::move(result).failure();
        const json::Value& part = *result;
        kind = part["kind"].toString();
        total = part["chars"].toInt();
        cut = part["cut"].toBool();
        std::wstring piece = wide(part["text"].toString());
        // An older Win顺 gives at least 1 000 characters: cut here, never
        // through a character made of two.
        if (static_cast<long long>(piece.size()) > wanted - collected) {
            std::size_t keep = static_cast<std::size_t>(wanted - collected);
            if (keep > 0 && IS_HIGH_SURROGATE(piece[keep - 1]))
                --keep;
            piece.resize(keep);
        }
        const long long start = part["offset"].isNumber() ? part["offset"].toInt() : offset;
        collected += static_cast<long long>(piece.size());
        next = start + static_cast<long long>(piece.size());
        if (next >= total)
            next = -1;
        if (json) {
            text += piece;
        } else if (!piece.empty()) {
            out.write(piece);
            endsWithNewline = piece.back() == L'\n';
            if (out.broken() || interrupt::requested())
                break;
        }
        if (next < 0 || collected >= wanted || piece.empty())
            break;
        offset = next;
    }
    if (interrupt::requested())
        return Failure {Exit::Interrupted, {}};

    if (json) {
        json::Value::Object object {{"path", utf8(path)}, {"kind", kind}, {"text", utf8(text)}, {"chars", total}, {"offset", *from},
            {"cut", cut}};
        if (next >= 0)
            object.emplace("next_offset", next);
        out.setJsonEscapes(true);
        out.writeUtf8(json::write(json::Value(std::move(object))));
        out.newline();
        out.setJsonEscapes(false);
    } else if (collected > 0 && !endsWithNewline && out.console()) {
        out.newline(); // the prompt on a line of its own; redirected, the text stays as it is
    }
    if (total == 0) {
        context.note(tr(kind == "document" ? msg::kNoTextDocument : msg::kEmptyFile));
        return Exit::NothingFound;
    }
    // --from beyond the end: no text, and exit 1 would say nothing of why.
    if (collected == 0 && *from >= total)
        context.note(fmt(msg::kReadPastEnd, total, *from));
    if (next >= 0)
        context.note(fmt(msg::kMoreText, total - next, next));
    if (cut)
        context.note(tr(msg::kTextCut));
    return collected > 0 || json ? Exit::Found : Exit::NothingFound;
}

} // namespace ws::cli
