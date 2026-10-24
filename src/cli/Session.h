#pragma once

// One MCP session on Win顺's pipe (docs/design-cli.md §2): initialize (the
// client named "ws", which Win顺 knows for the command line), initialized,
// then tool calls. Lines are queued and written together: the first call goes
// out with the handshake, nothing waits for its answer. Answers are matched
// by id; notifications and answers to cancelled calls are passed over.
// While a session is open, Ctrl+C cancels its calls in Win顺 (MCP's
// notifications/cancelled) before ws ends.

#include "Json.h"
#include "PipeClient.h"
#include "Result.h"

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace ws::cli {

namespace json = ws::plugin::json;

struct CallResult {
    bool isError = false; // Win顺 said no (the tool, or the request itself): errorText says why
    // What kind of no, for the exit code: "not_found", "off" or "too_large"
    // (structuredContent.error of a refusal); empty for the rest, and from a
    // Win顺 older than ws.
    std::string errorCode;
    std::wstring errorText;
    json::Value structured; // structuredContent (Win顺 always sends it)
};

class Session {
public:
    explicit Session(pipe::LineClient& client);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // A tools/call, queued; its id. Written by flush() (or wait()).
    long long queue(std::string_view tool, json::Value arguments);
    bool flush();
    // The answer to `id`; a refusal (the tool's, or a protocol error such as
    // an unknown tool) as isError with Win顺's text. A Failure only when there
    // was no answer (the pipe broke, Ctrl+C).
    Result<CallResult> wait(long long id);
    Result<CallResult> call(std::string_view tool, json::Value arguments);
    // Tells Win顺 to drop a call; its answer, should it come, is passed over.
    void cancel(long long id);

    // initialize's answer: Win顺's version. Waits for it.
    Result<std::wstring> serverVersion();

private:
    Result<json::Value> answer(long long id); // the raw response
    void onInterrupt();

    pipe::LineClient& m_client;
    std::string m_out; // queued lines
    long long m_nextId = 1;
    long long m_initializeId = 0; // until its answer is read
    std::wstring m_version;
    std::map<long long, json::Value> m_early; // answers read while waiting for another
    std::set<long long> m_dropped; // cancelled: their answers are passed over

    std::mutex m_mutex; // m_open, written from Ctrl+C's thread too
    std::set<long long> m_open; // calls sent and not answered
    bool m_cancelSent = false;
};

// Win顺's refusal mapped to an exit code and the user's language
// (docs/design-cli.md §6): by its code (`errorCode`) when it has one, else by
// the English words of a Win顺 before ws. `subject`: the path it is about;
// empty: the path Win顺's words end with, if any.
Failure toolFailure(std::string_view tool, std::string_view errorCode, const std::wstring& text, std::wstring_view subject);

} // namespace ws::cli
