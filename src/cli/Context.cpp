#include "Context.h"

#include "Encoding.h"
#include "Interrupt.h"
#include "Messages.h"

#include <algorithm>
#include <cmath>

namespace ws::cli {

namespace {

struct IndexState {
    bool ready = false;
    bool refreshing = false;
    int percent = -1; // of the volumes being read, on average; -1: unknown
};

IndexState indexState(const json::Value& status)
{
    IndexState state;
    const json::Value& index = status["index"];
    state.ready = index["state"].toString() == "ready";
    state.refreshing = index["refreshing"].toBool();
    double sum = 0;
    int counted = 0;
    for (const json::Value& volume : index["volumes"].toArray()) {
        if (volume["reading"].toBool() && volume["percent"].isNumber()) {
            sum += volume["percent"].toNumber();
            ++counted;
        }
    }
    if (counted > 0)
        state.percent = static_cast<int>(std::lround(sum / counted));
    return state;
}

// Waits for the index; `first` is the state already known.
Result<void> waitForIndex(Context& context, Session& session, IndexState state)
{
    if (context.waitMs <= 0)
        return Failure {Exit::NotReady, std::wstring(tr(msg::kNotReadyNoWait))};
    const ULONGLONG start = ::GetTickCount64();
    const ULONGLONG deadline = start + static_cast<ULONGLONG>(context.waitMs);
    for (;;) {
        context.progress(state.percent >= 0 ? fmt(msg::kLoadingPercent, state.percent) : std::wstring(tr(msg::kLoading)));
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline) {
            context.endProgress();
            const long long seconds = (context.waitMs + 999) / 1000;
            return Failure {Exit::NotReady, fmt(msg::kNotReady, seconds)};
        }
        if (!interrupt::sleep(static_cast<DWORD>(std::min<ULONGLONG>(250, deadline - now)))) {
            context.endProgress();
            return Failure {Exit::Interrupted, {}};
        }
        Result<CallResult> status = session.call("index_status", json::Value::Object {});
        if (!status) {
            context.endProgress();
            return std::move(status).failure();
        }
        state = indexState(status->structured);
        if (state.ready) {
            context.endProgress();
            return {};
        }
    }
}

} // namespace

void Context::note(std::wstring_view text)
{
    endProgress();
    err.write(L"ws: ", Style::Dim);
    err.write(text, Style::Dim);
    err.newline();
    err.flush();
}

void Context::error(std::wstring_view text)
{
    endProgress();
    err.write(L"ws:", Style::Error);
    err.write(L" ");
    err.write(text);
    err.newline();
    err.flush();
}

void Context::progress(std::wstring_view text)
{
    if (!err.console())
        return;
    std::wstring line = L"\r" + std::wstring(text);
    if (errVt) {
        line += L"\x1b[K";
    } else if (text.size() < m_progressLength) {
        line.append(m_progressLength - text.size(), L' ');
    }
    err.write(line);
    err.flush();
    m_progressLength = std::max(m_progressLength, text.size());
}

void Context::endProgress()
{
    if (m_progressLength == 0)
        return;
    std::wstring line = L"\r";
    if (errVt)
        line += L"\x1b[K";
    else
        line += std::wstring(m_progressLength, L' ') + L"\r";
    err.write(line);
    err.flush();
    m_progressLength = 0;
}

Result<std::unique_ptr<Connection>> connect(Context& context)
{
    auto connection = std::make_unique<Connection>();
    ConnectOptions options;
    options.pipe = context.pipe;
    options.switchOn = context.config.connectionOn;
    options.mayStart = context.mayStart;
    options.startingMutex = context.instanceMutex + L".Starting";
    options.note = [&context](std::wstring_view text) { context.note(text); };
    Result<void> connected = connectToWinShun(connection->client, options, context.launcher);
    if (!connected)
        return std::move(connected).failure();
    connection->session = std::make_unique<Session>(connection->client);
    return connection;
}

Result<json::Value> callTool(Context& context, Connection& connection, std::string_view tool, const json::Value& arguments, bool needsIndex,
    std::wstring_view subject, std::string* errorCode)
{
    Session& session = *connection.session;
    long long id = 0;
    if (needsIndex) {
        // Both at once: no round trip more than the call itself.
        const long long statusId = session.queue("index_status", json::Value::Object {});
        id = session.queue(tool, arguments);
        Result<CallResult> status = session.wait(statusId);
        if (!status)
            return std::move(status).failure();
        if (!status->isError) {
            const IndexState state = indexState(status->structured);
            if (!state.ready) {
                session.cancel(id);
                Result<void> ready = waitForIndex(context, session, state);
                if (!ready)
                    return std::move(ready).failure();
                id = session.queue(tool, arguments);
            } else if (state.refreshing) {
                context.note(tr(msg::kRefreshing));
            }
        }
    } else {
        id = session.queue(tool, arguments);
    }
    Result<CallResult> result = session.wait(id);
    if (!result)
        return std::move(result).failure();
    if (result->isError) {
        if (errorCode)
            *errorCode = result->errorCode;
        return toolFailure(tool, result->errorCode, result->errorText, subject);
    }
    return std::move(result->structured);
}

} // namespace ws::cli
