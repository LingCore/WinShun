#pragma once

#include "SwitchBackend.h"
#include "platform/ComWorker.h"

#include <QObject>
#include <QString>
#include <QStringView>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace ws {

// The system switches as the launcher's rows show them: dark mode, hidden
// files, ... (docs/design-system-switches.md 4.2–4.5). Lives on the GUI
// thread and answers from its cache there at once: the search never waits
// for the system, nor does a row being drawn. The backends read and write on
// a thread of their own (the switch thread: a ComWorker in the multithreaded
// apartment), one task at a time; their results come back here. Made by App
// (systemSwitchBackends()), before the launcher, which asks it.
//
// For a row with SearchResult::switchId:
// - state(id) draws it. Unsupported: no switch here yet, an ordinary place.
// - resolve(id, row's intent) says what Enter will do, for the footer
//   ("Enter 关闭蓝牙"); apply(id, row's intent) does it, and its Outcome says
//   what the footer flashes. The two agree: a toggle becomes On or Off from
//   the state shown, here on the GUI thread, and goes to the backend as such.
// - changed(id) comes whenever state(id) is different, whoever changed it:
//   this app, another program, the Settings app.
// - refresh() when the launcher is shown: everything read again (cheap).
class Switches : public QObject {
    Q_OBJECT

public:
    // What Enter on a switch's row did (apply), or a toggle that had to wait
    // for its state did in the end (waitEnded).
    enum class Outcome : quint8 {
        // Being written: state(id) has the target and busy at once, until
        // the write is confirmed. The window stays; changed(id) follows.
        Started,
        Already, // it already is so: nothing written (the footer: "蓝牙已经是关的")
        // Nothing done: a toggle while this switch is being written (or one
        // already waiting for its state), or the target being written already.
        Busy,
        // A toggle before the state is known (just started): done once it
        // is read, within Options::stateWait; waitEnded(id, …) follows, once.
        Waiting,
        // Nothing to switch here: no backend (Unsupported), not ready (no
        // device, denied, blocked), or OpenSettings asked. Enter opens
        // state(id).openInstead, else the row's own page, as before.
        OpenPage,
        // waitEnded: the state did not come in time; step(): the level is not
        // known yet. Nothing written (the footer: "还没读到蓝牙的状态").
        NotRead,
    };
    Q_ENUM(Outcome)

    struct Options {
        // A toggle asked before the state is known waits this long for it.
        std::chrono::milliseconds stateWait {500};
        // A failure's words stay that long; refresh() clears older ones (a
        // failure while the window was closed shows if it opens within it).
        std::chrono::milliseconds errorShown {30'000};
        std::function<std::chrono::steady_clock::time_point()> now = &std::chrono::steady_clock::now; // tests
    };

    // An asynchronous provider on this object's thread. The plugin adapter
    // uses this path: no COM worker or native watcher exists in the host.
    struct Remote {
        QStringList ids;
        std::function<void(const QString&, std::function<void(SwitchState)>)> read;
        std::function<void(const QString&, SwitchIntent, std::function<void(SwitchWrite)>)> write;
        std::function<void(const QString&)> written;
    };

    // Every backend's setStopToken() and watch() are called here and its
    // switches read at once.
    explicit Switches(SwitchBackends backends, QObject* parent = nullptr);
    Switches(SwitchBackends backends, Options options, QObject* parent = nullptr);
    explicit Switches(Remote remote, QObject* parent = nullptr);
    Switches(Remote remote, Options options, QObject* parent = nullptr);
    // Waits for the switch thread: the task under way (a few ms at most:
    // tasks give up on its stop token) and the backends' close(). Pending
    // results and waits are dropped; nothing more is signalled.
    ~Switches() override;

    Switches(const Switches&) = delete;
    Switches& operator=(const Switches&) = delete;

    // The switch as the row shows it now. An id no backend answers for
    // (none yet in this build, or not a switch at all): Unsupported.
    [[nodiscard]] SwitchState state(QStringView id) const;
    // What apply(id, intent) would ask of the system: a Toggle becomes On or
    // Off from the state shown (from Mixed: the backend's choice,
    // SwitchState::mixedToggleOn); stays Toggle while the state is unknown.
    // A SetLevel is kept within 0–100 ("音量 150" sets 100); a StepLevel
    // ("音量 +10") becomes the level it reaches from the level shown, just
    // that (24 + 10: 34): a SetLevel up, LevelOnly down; it stays a StepLevel
    // while the level is unknown. Any other intent comes back as it is.
    [[nodiscard]] SwitchIntent resolve(QStringView id, SwitchIntent intent) const;
    // Enter on the row (or a click on its switch): see Outcome. `intent` is
    // the row's (SearchResult::intent): Toggle, On, Off, SetLevel, StepLevel
    // (the keep-awake kinds for the backend that comes with them; until then
    // its switch is Unsupported), as resolve() makes it. On, Off and a level
    // are written even while the state is unknown (they mean the same either
    // way); a toggle waits. Several asked while one is written: only the
    // newest is written next, and the row never shows a result already
    // overtaken. Levels go absolute, so a step taken meanwhile counts from
    // the level being written.
    [[nodiscard]] Outcome apply(QStringView id, SwitchIntent intent);
    // Alt+← →: `steps` steps of 5 down (< 0) or up from the level shown
    // (stepped()), as apply(). NotRead while the level is not known yet,
    // OpenPage for a switch that is not ready or has no level.
    [[nodiscard]] Outcome step(QStringView id, int steps);
    // Where `steps` steps of 5 take the volume from `level` (0–100): onto the
    // multiples of 5 first (24 goes to 25 or to 20, 25 to 30 or 20), within
    // 0–100. Up a SetLevel, which unmutes; down LevelOnly, the mute as it is.
    [[nodiscard]] static SwitchIntent stepped(int level, int steps);
    // The launcher is shown: every switch read again (a few registry values,
    // on the switch thread); failures older than Options::errorShown cleared.
    void refresh();
    void refresh(QStringView id); // a provider notification: read just this switch

signals:
    void changed(const QString& id); // state(id) is different now
    // A toggle that got Outcome::Waiting is over: Started, OpenPage (the
    // state came in, not ready), Busy (an explicit On or Off came first) or
    // NotRead (none came in time). Exactly once per Waiting.
    void waitEnded(const QString& id, ws::Switches::Outcome outcome);

private:
    struct Entry {
        QString id;
        SwitchBackend* backend = nullptr;
        SwitchState shown; // what the row shows: the target while busy
        SwitchState confirmed; // as last read or read back: where a failure goes back to
        std::optional<SwitchIntent> writing; // on the switch thread now
        std::uint64_t writingSerial = 0; // ... as which write
        std::optional<SwitchIntent> queued; // asked meanwhile, the newest only: written next
        bool reading = false; // a read is on its way
        bool readAgain = false; // asked for again meanwhile: one more once it is back
        std::uint64_t waitToken = 0; // a toggle waiting for the state (Outcome::Waiting); 0: none
        std::chrono::steady_clock::time_point errorAt; // when `shown.error` was set
    };

    Entry* find(QStringView id);
    const Entry* find(QStringView id) const;
    void requestRead(Entry& e);
    void readDone(const QString& id, const SwitchState& now);
    void startWrite(Entry& e, SwitchIntent intent);
    void writeDone(const QString& id, std::uint64_t serial, const SwitchWrite& result);
    void show(Entry& e, const SwitchState& state); // changed(id) if it differs
    void showTarget(Entry& e, const SwitchIntent& intent); // busy, at the target
    void endWait(Entry& e, Outcome outcome);
    void written(Entry& e);

    Options m_options;
    SwitchBackends m_backends;
    std::optional<Remote> m_remote;
    std::vector<Entry> m_entries; // one per id, made once: pointers into it stay valid
    std::uint64_t m_serial = 0; // of the writes and the waits
    std::unique_ptr<ComWorker> m_worker; // local backends only; joined before they go
};

} // namespace ws
