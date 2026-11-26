#include "Engine.h"

namespace lm {

namespace {

constexpr int kLoadTimeoutMs = 180'000; // a big model from a slow disk

} // namespace

void Engine::configure(EngineConfig config, bool chinese)
{
    bool stopNow = false;
    {
        const std::lock_guard lock(m_mutex);
        stopNow = !config.sameModel(m_config) || !config.unusable.empty();
        m_config = std::move(config);
        m_chinese = chinese;
        if (m_phase == Phase::Failed && stopNow) {
            m_phase = Phase::Unloaded; // a new choice: the old failure is no longer the story
            m_error.clear();
        }
    }
    if (stopNow)
        tick(); // stops it unless it is busy or loading; tick() tries again later
    if (m_changed)
        m_changed();
}

void Engine::setPhase(Phase phase, std::string error, bool cpuFallback)
{
    {
        const std::lock_guard lock(m_mutex);
        m_phase = phase;
        m_error = std::move(error);
        m_stopped = false;
        m_cpuFallback = cpuFallback;
        if (phase == Phase::Loaded)
            m_lastUse = std::chrono::steady_clock::now();
    }
    if (m_changed)
        m_changed();
}

Target Engine::acquire()
{
    // One load at a time; the requests that come meanwhile wait for it.
    std::unique_lock serverLock(m_serverMutex);
    EngineConfig config;
    bool chinese = true;
    {
        const std::lock_guard lock(m_mutex);
        config = m_config;
        chinese = m_chinese;
    }
    if (!config.unusable.empty())
        return {0, 503, config.unusableCode, config.unusable};
    if (m_canceled)
        return {0, 503, "stopping", chinese ? "本地模型正在关闭" : "The local models are shutting down"};
    {
        const std::lock_guard lock(m_mutex);
        if (m_server.running() && m_loaded.sameModel(config)) {
            m_lastUse = std::chrono::steady_clock::now();
            return {m_server.launch().port};
        }
    }
    m_server.stop();
    setPhase(Phase::Loading);
    Launch launch {config.program, config.model, config.alias, config.gpu, config.context, freePort()};
    launch.embedding = config.embedding;
    std::string error;
    bool ok = m_server.start(launch, config.log, m_canceled, kLoadTimeoutMs, error, chinese);
    if (!ok && !m_canceled && m_server.lostPort()) { // taken between freePort() and its bind
        launch.port = freePort();
        ok = m_server.start(launch, config.log, m_canceled, kLoadTimeoutMs, error, chinese);
    }
    bool cpuFallback = false;
    if (!ok && !m_canceled && launch.gpu && m_server.gpuTrouble()) {
        // The GPU could not (too little video memory, a driver fault): the
        // CPU instead, and the card says so.
        launch.gpu = false;
        launch.port = freePort();
        ok = m_server.start(launch, config.log, m_canceled, kLoadTimeoutMs, error, chinese);
        cpuFallback = ok;
    }
    if (!ok) {
        setPhase(Phase::Failed, error);
        return {0, 503, "load_failed", error};
    }
    {
        const std::lock_guard lock(m_mutex);
        m_loaded = config;
    }
    setPhase(Phase::Loaded, {}, cpuFallback);
    return {launch.port};
}

void Engine::busy(int delta)
{
    const std::lock_guard lock(m_mutex);
    m_active = std::max(0, m_active + delta);
    m_lastUse = std::chrono::steady_clock::now();
}

void Engine::tick()
{
    std::unique_lock serverLock(m_serverMutex, std::try_to_lock);
    if (!serverLock)
        return; // loading: tried again next time
    bool stopped = false;
    {
        const std::lock_guard lock(m_mutex);
        if (!m_server.running()) {
            if (m_phase == Phase::Loaded) {
                // It ended by itself (memory, a crash, a lost device): the
                // card says why; the next request loads it again.
                m_phase = Phase::Failed;
                m_error = m_server.reap(m_chinese);
                m_stopped = true;
                stopped = true;
            }
        } else if (m_active == 0) {
            const bool other = !m_loaded.sameModel(m_config) || !m_config.unusable.empty();
            const bool idle = std::chrono::steady_clock::now() - m_lastUse >= std::chrono::seconds(m_config.idleSeconds);
            if (other || idle) {
                m_server.stop();
                m_phase = Phase::Unloaded;
                stopped = true;
            }
        }
    }
    if (stopped && m_changed)
        m_changed();
}

bool Engine::unload()
{
    std::unique_lock serverLock(m_serverMutex, std::try_to_lock);
    if (!serverLock)
        return false;
    {
        const std::lock_guard lock(m_mutex);
        if (m_active > 0)
            return false;
        m_server.stop();
        m_phase = Phase::Unloaded;
    }
    if (m_changed)
        m_changed();
    return true;
}

void Engine::shutdown()
{
    m_canceled = true;
    const std::lock_guard serverLock(m_serverMutex); // a load in progress gives up first
    m_server.stop();
}

Engine::State Engine::state() const
{
    State s;
    {
        const std::lock_guard lock(m_mutex);
        s.phase = m_phase;
        s.error = m_error;
        s.stopped = m_stopped;
        s.cpuFallback = m_cpuFallback;
    }
    if (s.phase == Phase::Loaded) {
        std::unique_lock serverLock(m_serverMutex, std::try_to_lock);
        if (serverLock) {
            s.memory = m_server.memory();
            const LogFacts facts = m_server.facts();
            if (facts.offloaded > 0 && m_server.launch().gpu)
                s.device = facts.device;
        }
    }
    return s;
}

std::string Engine::alias() const
{
    const std::lock_guard lock(m_mutex);
    return m_config.alias;
}

bool Engine::usable() const
{
    const std::lock_guard lock(m_mutex);
    return m_config.unusable.empty() && !m_config.model.empty();
}

} // namespace lm
