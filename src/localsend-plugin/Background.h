#pragma once

// Work the thread that answers Win顺 must not wait on: files looked at on a
// share that sleeps (seconds), the clipboard of a program that renders what
// it copied only when asked (as long, or for ever when it hangs). A thread
// of its own makes one value at a time, the newest asked for next, and the
// asker waits a moment at most (Win顺 ends a plugin that keeps it waiting).
// A value made after its asker stopped waiting, or made anew and changed,
// is told with `late` (传 asks again), never after stop(). The thread is
// never waited for: one stuck for ever ends with the process, and what it
// works on lives as long as it does.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace localsend {

template <typename Value> // compared with ==: a value made anew and the same is not told
class Background {
public:
    using Clock = std::chrono::steady_clock;
    using Make = std::function<std::optional<Value>()>; // nothing: not made (asked again next time)
    static constexpr std::size_t kKept = 8; // values kept, by key

    explicit Background(std::function<void()> late) : m_state(std::make_shared<State>()) { m_state->late = std::move(late); }
    ~Background() { stop(); }
    Background(const Background&) = delete;
    Background& operator=(const Background&) = delete;

    // The value for `key`. One made within `fresh` comes at once; an older
    // one too, made anew meanwhile. With none, it is made now and waited for
    // up to `wait` (not at all behind another being made: that one may be
    // stuck); nothing when it is not done by then. `make` must not use its
    // asker: it may run after the asker is gone.
    std::optional<Value> get(const std::string& key, Make make, Clock::duration fresh, std::chrono::milliseconds wait)
    {
        State& s = *m_state;
        std::unique_lock lock(s.mutex);
        std::optional<Value> kept;
        if (const auto it = s.made.find(key); it != s.made.end()) {
            if (Clock::now() - it->second.first < fresh)
                return it->second.second;
            kept = it->second.second;
        }
        if (s.stopped)
            return kept;
        if (s.running != key && !(s.next && s.next->first == key)) {
            s.next = std::make_pair(key, std::move(make)); // the newest wins
            if (!s.started) {
                try {
                    std::thread(&Background::loop, m_state).detach();
                    s.started = true;
                } catch (const std::system_error&) {
                    s.next.reset(); // no thread: nothing made
                    return kept;
                }
            }
            s.wake.notify_all();
        }
        if (kept)
            return kept;
        if (!s.running.empty() && s.running != key)
            return std::nullopt;
        s.waitingFor = key;
        s.done.wait_for(lock, wait, [&] { return s.stopped || s.made.count(key) > 0 || (s.running.empty() && !s.next); });
        s.waitingFor.clear();
        if (const auto it = s.made.find(key); it != s.made.end())
            return it->second.second;
        return std::nullopt;
    }

    // Nothing more made or told; the thread ends once it is free.
    void stop()
    {
        const std::lock_guard lock(m_state->mutex);
        m_state->stopped = true;
        m_state->next.reset();
        m_state->wake.notify_all();
        m_state->done.notify_all();
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable wake; // something to make, or stopped
        std::condition_variable done; // a value made
        std::map<std::string, std::pair<Clock::time_point, Value>> made;
        std::optional<std::pair<std::string, Make>> next;
        std::string running; // the key being made now
        std::string waitingFor; // the key its asker waits for now
        std::function<void()> late;
        bool started = false;
        bool stopped = false;
    };

    static void loop(std::shared_ptr<State> state)
    {
        State& s = *state;
        std::unique_lock lock(s.mutex);
        for (;;) {
            s.wake.wait(lock, [&] { return s.stopped || s.next.has_value(); });
            if (s.stopped)
                return;
            std::pair<std::string, Make> job = std::move(*s.next);
            s.next.reset();
            s.running = job.first;
            lock.unlock();
            std::optional<Value> value;
            try {
                value = job.second();
            } catch (...) {
                // Nothing made: asked again next time.
            }
            lock.lock();
            s.running.clear();
            if (!value) {
                s.done.notify_all(); // its asker waits no longer
                continue;
            }
            const auto it = s.made.find(job.first);
            const bool changed = it == s.made.end() || !(it->second.second == *value);
            if (it == s.made.end() && s.made.size() >= kKept)
                s.made.clear();
            s.made[job.first] = {Clock::now(), std::move(*value)};
            s.done.notify_all();
            // Under the lock: never after stop().
            if (changed && !s.stopped && s.waitingFor != job.first && s.late)
                s.late();
        }
    }

    std::shared_ptr<State> m_state;
};

} // namespace localsend
