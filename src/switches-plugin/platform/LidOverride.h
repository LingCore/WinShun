#pragma once

#include <QString>
#include <QUuid>
#include <QtGlobal>

#include <memory>
#include <optional>

namespace ws {

// What closing a laptop's lid does in one power scheme (a Windows power plan),
// plugged in and on battery: 0 nothing, 1 sleep, 2 hibernate, 3 shut down.
struct LidActions {
    quint32 ac = 0;
    quint32 dc = 0;
    friend bool operator==(const LidActions&, const LidActions&) = default;
};

// The power plans as LidOverride uses them; tests put in a fake.
class PowerPlans {
public:
    virtual ~PowerPlans() = default;
    // Whether this computer has a lid (a laptop).
    [[nodiscard]] virtual bool lidPresent() = 0;
    // Whether a group policy sets the lid's action over every scheme's own.
    [[nodiscard]] virtual bool lidByPolicy() = 0;
    // The active scheme; null when Windows does not say.
    [[nodiscard]] virtual QUuid active() = 0;
    // Null when it cannot be read (the scheme is gone).
    [[nodiscard]] virtual std::optional<LidActions> lid(const QUuid& scheme) = 0;
    // Writes both; when it is the active scheme, applies it again so that
    // they count at once.
    [[nodiscard]] virtual bool setLid(const QUuid& scheme, const LidActions& actions) = 0;

    // Windows' own (powrprof).
    [[nodiscard]] static std::unique_ptr<PowerPlans> system();
};

// Keep awake with the lid closed (GitHub #503): while held, closing the lid
// does nothing in the active scheme; let go, each action Win顺 changed comes
// back where it still does nothing (one the user changed meanwhile stays).
// What is changed is written to `record` first, so that a Win顺 that ended
// without letting go (a crash, the power cut) puts it back at its next start:
// the constructor does. Not thread-safe: SwitchKeepAwake calls it under its
// mutex.
class LidOverride {
public:
    enum class Outcome { Held, NoLid, ByPolicy, Failed };

    // `record` empty: nothing is kept for a next start (wsbench).
    LidOverride(std::unique_ptr<PowerPlans> plans, QString record);
    ~LidOverride(); // lets go
    LidOverride(const LidOverride&) = delete;
    LidOverride& operator=(const LidOverride&) = delete;

    [[nodiscard]] bool lidPresent() const { return m_lidPresent; }
    [[nodiscard]] bool held() const { return m_held; }

    // Closing the lid does nothing from now on, until release().
    Outcome hold();
    // The lid does what it did again.
    void release();
    // While held: another scheme was made active, so the old one gets its
    // actions back and the new one is held. Held when nothing changed.
    Outcome follow();

private:
    // What a scheme's actions were, where Win顺 set them to nothing.
    struct Change {
        QUuid scheme;
        std::optional<quint32> ac;
        std::optional<quint32> dc;
    };
    void recover();
    [[nodiscard]] bool putBack(const Change& change);
    [[nodiscard]] bool save(const Change& change) const;
    [[nodiscard]] std::optional<Change> load() const;

    std::unique_ptr<PowerPlans> m_plans;
    QString m_record;
    bool m_lidPresent = false;
    bool m_held = false;
    QUuid m_scheme; // held: the scheme it holds
    std::optional<Change> m_change; // held: null when nothing needed changing
};

} // namespace ws
