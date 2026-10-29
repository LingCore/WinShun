#pragma once

// How a run of ws ends: its exit code (docs/design-cli.md §6; the same in
// every language, for scripts and agents) and, when it failed, one line for
// the user. Failures travel up as values; nothing deep inside exits.

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace ws::cli {

enum class Exit : int {
    Found = 0, // done, with results
    NothingFound = 1, // done, no results (as grep and ripgrep)
    Usage = 2, // wrong arguments, a missing query
    NotRunning = 3, // Win顺 is not running and was not started
    Off = 4, // Win顺 runs, but the AI and command-line connection is off
    NotReady = 5, // the index was not ready within --wait
    NotVisible = 6, // the path: not there, no access, or kept from AI (one wording for all three)
    Failed = 7, // anything else: the pipe broke, Win顺 answered an error
    Interrupted = 130, // Ctrl+C (the call was cancelled in Win顺)
};

struct Failure {
    Exit code = Exit::Failed;
    std::wstring message; // for the user, in their language; empty: nothing more to say
};

// A value, or why there is none.
template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value)
        : m_state(std::move(value))
    {
    }
    Result(Failure failure)
        : m_state(std::move(failure))
    {
    }

    bool ok() const noexcept { return m_state.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T& operator*() & { return std::get<0>(m_state); }
    const T& operator*() const& { return std::get<0>(m_state); }
    T&& operator*() && { return std::get<0>(std::move(m_state)); }
    T* operator->() { return &std::get<0>(m_state); }
    const T* operator->() const { return &std::get<0>(m_state); }

    const Failure& failure() const& { return std::get<1>(m_state); }
    Failure&& failure() && { return std::get<1>(std::move(m_state)); }

private:
    std::variant<T, Failure> m_state;
};

// Done, or why not.
template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Failure failure)
        : m_failure(std::move(failure))
    {
    }

    bool ok() const noexcept { return !m_failure; }
    explicit operator bool() const noexcept { return ok(); }

    const Failure& failure() const& { return *m_failure; }
    Failure&& failure() && { return std::move(*m_failure); }

private:
    std::optional<Failure> m_failure;
};

} // namespace ws::cli
