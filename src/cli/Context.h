#pragma once

// What a command runs with: its arguments, the two streams, Win顺's settings,
// and the way to Win顺 (connecting, starting it, waiting for its index).

#include "Args.h"
#include "Config.h"
#include "Connect.h"
#include "Output.h"
#include "Session.h"

#include <memory>
#include <string>
#include <string_view>

namespace ws::cli {

struct Command;

struct Context {
    Context(const Command& command, const Parsed& args, Stream& out, Stream& err, const Config& config)
        : command(command)
        , args(args)
        , out(out)
        , err(err)
        , config(config)
    {
    }

    const Command& command;
    const Parsed& args;
    Stream& out;
    Stream& err;
    const Config& config;
    std::wstring pipe; // --pipe, else the user's
    std::wstring instanceMutex; // --instance, else Win顺's
    Launcher launcher;
    long long waitMs = 10'000; // --wait: for the index
    bool mayStart = true; // not --no-start
    bool errVt = false; // stderr is a console that takes VT sequences

    // A line on stderr: "ws: …" (dim on a console).
    void note(std::wstring_view text);
    // The same for an error ("ws:" in red on a console).
    void error(std::wstring_view text);
    // A line on a console's stderr that the next one replaces; nothing elsewhere.
    void progress(std::wstring_view text);
    void endProgress();

private:
    std::size_t m_progressLength = 0;
};

// The pipe and the session on it.
struct Connection {
    pipe::LineClient client;
    std::unique_ptr<Session> session; // made once connected; goes before the client

    ~Connection() { session.reset(); }
};

// The percentage the "loading the index" line shows: never lower than `shown`,
// the highest said so far (-1: none, or unknown), whatever Win顺 now reports.
int stablePercent(int shown, int reported);

// How to reach Win顺 for this run: its pipe, the settings file's switch, --no-start.
ConnectOptions connectOptions(Context& context);
// Connects, starting Win顺 when it should be (docs/design-cli.md §7).
Result<std::unique_ptr<Connection>> connect(Context& context);

// For an argument only a Win顺 made for ws understands (--in's `folders`): a
// Win顺 before that ignores it and answers for everything, so ws asks first
// (initialize says so) and refuses with exit 7, naming `option`, when it is an
// older one. Costs the wait for initialize's answer: ask only for those.
Result<void> requireCliContract(Connection& connection, std::wstring_view option);

// Calls a tool. `needsIndex`: the index must be ready; its state is asked in
// the same breath, and when it is still loading the call is cancelled, ws
// waits as --wait says (a progress line on a console), then asks again.
// A refusal comes back as a Failure with its exit code (toolFailure):
// `subject` is what it was about (a path), for the message; `errorCode`, if
// given, gets Win顺's kind of refusal (CallResult::errorCode).
Result<json::Value> callTool(Context& context, Connection& connection, std::string_view tool, const json::Value& arguments, bool needsIndex,
    std::wstring_view subject = {}, std::string* errorCode = nullptr);

} // namespace ws::cli
