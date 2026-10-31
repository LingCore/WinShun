#pragma once

#include "SwitchIntent.h"
#include "SwitchState.h"

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <vector>

namespace ws {

// The system's side of one or more switches (platform/SwitchTheme,
// SwitchExplorer, SwitchRadios, SwitchAudio), as Switches drives it; tests put in a fake. Made and
// destroyed on the GUI thread. read() and write() run on the switch thread
// only (ComWorker, multithreaded apartment), one call at a time; close()
// runs there last. watch() and written() run on the GUI thread.
class SwitchBackend {
public:
    using Notify = std::function<void(const QString& id)>;

    virtual ~SwitchBackend() = default;

    // The switch ids it answers for (places.txt "switch =", PlaceInfo::switchId).
    [[nodiscard]] virtual QStringList ids() const = 0;

    // GUI thread, once, before watch(): requested as the switch thread ends
    // (Win顺 exits). A read or write that waits on the system (a radio being
    // switched) looks at it now and then and gives up, so that the exit does
    // not wait for it.
    virtual void setStopToken(std::stop_token stop) { (void)stop; }

    // GUI thread, once, before the first read: from now on `changed` is told
    // when a switch may have been changed elsewhere (another program, the
    // Settings app, Win顺's own write), and Switches reads it again. It may
    // be called on any thread, until close() (sources on other threads stop
    // there) or the backend is destroyed (those on the GUI thread).
    virtual void watch(Notify changed) { (void)changed; }

    // Switch thread: the state now (all but busy and error). Quick: a few
    // registry values, an object asked; never waits on another program.
    [[nodiscard]] virtual SwitchState read(const QString& id) = 0;

    // Switch thread: sets the switch as `intent` says, always explicit (On,
    // Off; SetLevel, LevelOnly and the keep-awake kinds for their backends),
    // never Toggle or OpenSettings: Switches turns a toggle into On or Off on
    // the GUI thread, from the state shown, and a step into the level it
    // reaches (a StepLevel comes only while the level is not known yet).
    // Reads first and writes nothing when it already is so (ok, not wrote);
    // reads back afterwards.
    [[nodiscard]] virtual SwitchWrite write(const QString& id, const SwitchIntent& intent) = 0;

    // GUI thread, after a write that changed the system (SwitchWrite::wrote):
    // what has to happen there. Dark mode: Win顺's own windows told at once,
    // then every other program from a thread of its own (SettingBroadcast).
    virtual void written(const QString& id) { (void)id; }

    // Switch thread, as it ends: let go of what was made there (COM and WinRT
    // objects, their callbacks). No read or write comes after it.
    virtual void close() { }
};

using SwitchBackends = std::vector<std::unique_ptr<SwitchBackend>>;

// For a backend whose sources call on threads of their own, at any time (a
// radio's StateChanged, Core Audio's callbacks): passes their calls on to
// SwitchBackend::watch's `changed` until close(), and drops those that come
// later. The callbacks share it, as they may outlive the backend. Without
// watch() (wsbench), nobody is told.
class SwitchRelay {
public:
    void watch(SwitchBackend::Notify changed)
    {
        std::lock_guard lock(m_mutex);
        m_changed = std::move(changed);
    }
    // Before the sources are let go: a call under way has told Switches, or
    // tells nobody. Never from within one of the calls.
    void close()
    {
        std::lock_guard lock(m_mutex);
        m_changed = nullptr;
    }
    void operator()(const QString& id)
    {
        std::lock_guard lock(m_mutex); // a moment: `changed` only posts to the GUI thread
        if (m_changed)
            m_changed(id);
    }

private:
    std::mutex m_mutex;
    SwitchBackend::Notify m_changed;
};

} // namespace ws
