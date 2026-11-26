// Runs the model when a request needs it, and only then: the first request
// starts llama-server (the others wait for that load), a model left unused
// for a while is let go of (memory back), and a change of model or device
// stops the running one. When the GPU cannot hold the model it is loaded on
// the CPU instead, and the state says so.
#pragma once

#include "LlamaServer.h"
#include "Relay.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

namespace lm {

struct EngineConfig {
    std::filesystem::path program; // llama-server.exe
    std::filesystem::path log; // its output
    std::filesystem::path model; // the model to load; empty when none can be
    std::string alias; // its name in the API
    std::string unusable; // why no model can be used now, for the requests' answer
    std::string unusableCode; // "no_model", "downloading", "bad_model"
    bool gpu = true;
    int idleSeconds = 300;
    int context = 8192;
    bool embedding = false; // an embedding model (Launch::embedding)

    bool sameModel(const EngineConfig& other) const
    {
        return program == other.program && model == other.model && alias == other.alias && gpu == other.gpu && context == other.context && embedding == other.embedding;
    }
};

class Engine {
public:
    enum class Phase { Unloaded, Loading, Loaded, Failed };
    struct State {
        Phase phase = Phase::Unloaded;
        std::string error; // Failed: why
        bool stopped = false; // Failed after it had loaded (it ended by itself), not while loading
        std::string device; // Loaded on a GPU: its name; empty on the CPU
        bool cpuFallback = false; // the GPU was tried and could not: on the CPU instead
        std::uint64_t memory = 0; // Loaded: the server's private bytes
    };
    using Changed = std::function<void()>;

    explicit Engine(Changed changed) : m_changed(std::move(changed)) {}
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    ~Engine() { shutdown(); }

    // What to run. Another model (or device) than the one running: that one
    // is stopped now if it is idle, or as soon as it is.
    void configure(EngineConfig config, bool chinese);
    // From the relay's threads: the port of a server that is up (started now
    // if need be), or why there is none.
    Target acquire();
    void busy(int delta);
    // Every few seconds: lets an idle model go, notices one that crashed.
    void tick();
    // Lets the model go now (false: it is loading or answering).
    bool unload();
    // The plugin ends: a load in progress gives up, the server stops.
    void shutdown();

    State state() const;
    std::string alias() const;
    bool usable() const; // a model is configured that can be loaded

private:
    void setPhase(Phase phase, std::string error = {}, bool cpuFallback = false);

    Changed m_changed;
    mutable std::mutex m_mutex; // the fields below but m_server
    EngineConfig m_config;
    EngineConfig m_loaded; // what the running server was started for
    bool m_chinese = true;
    Phase m_phase = Phase::Unloaded;
    std::string m_error;
    bool m_stopped = false; // see State::stopped
    bool m_cpuFallback = false;
    int m_active = 0;
    std::chrono::steady_clock::time_point m_lastUse = std::chrono::steady_clock::now();

    mutable std::mutex m_serverMutex; // starting, stopping and asking m_server
    LlamaServer m_server;
    std::atomic_bool m_canceled = false;
};

} // namespace lm
