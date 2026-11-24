#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <vector>

namespace ws {

// Browser-like Back / Forward navigation stack for the settings window.
// Records logical places (page id, sub-tab, optional installed-plugin detail,
// optional committed search query) together with their remembered scroll offset.
// Navigating to a new place truncates the forward stack; consecutive pushes to
// the same logical place update its scroll offset instead of duplicating.
class SettingsHistory : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool canBack READ canBack NOTIFY changed FINAL)
    Q_PROPERTY(bool canForward READ canForward NOTIFY changed FINAL)
    Q_PROPERTY(int count READ count NOTIFY changed FINAL)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY changed FINAL)
    Q_PROPERTY(QVariantMap currentEntry READ currentEntry NOTIFY changed FINAL)
    Q_PROPERTY(QVariantMap backEntry READ backEntry NOTIFY changed FINAL)
    Q_PROPERTY(QVariantMap forwardEntry READ forwardEntry NOTIFY changed FINAL)
    Q_PROPERTY(QVariantList backEntries READ backEntries NOTIFY changed FINAL)
    Q_PROPERTY(QVariantList forwardEntries READ forwardEntries NOTIFY changed FINAL)
    Q_PROPERTY(bool restoring READ restoring WRITE setRestoring NOTIFY restoringChanged FINAL)

public:
    static constexpr int kMaxEntries = 64;
    static constexpr int kDropdownLimit = 12;

    struct Entry {
        QString pageId;
        int tab = 0;
        QString pluginId;
        QString query;
        qreal scrollY = 0;

        bool samePlace(const Entry& other) const noexcept;
        QVariantMap toVariantMap(int historyIndex = -1) const;
    };

    explicit SettingsHistory(QObject* parent = nullptr);

    bool canBack() const noexcept { return m_index > 0; }
    bool canForward() const noexcept { return m_index >= 0 && m_index + 1 < count(); }
    int count() const noexcept { return static_cast<int>(m_entries.size()); }
    int currentIndex() const noexcept { return m_index; }
    QVariantMap currentEntry() const;
    QVariantMap backEntry() const;
    QVariantMap forwardEntry() const;
    QVariantList backEntries() const;
    QVariantList forwardEntries() const;

    bool restoring() const noexcept { return m_restoring; }
    Q_INVOKABLE void setRestoring(bool on);

    Q_INVOKABLE void clear();
    Q_INVOKABLE void reset(const QString& pageId, int tab = 0, const QString& pluginId = {},
        const QString& query = {}, qreal scrollY = 0);
    Q_INVOKABLE void updateScroll(qreal scrollY);
    Q_INVOKABLE bool push(const QString& pageId, int tab = 0, const QString& pluginId = {},
        const QString& query = {}, qreal scrollY = 0);
    Q_INVOKABLE void replaceCurrent(const QString& pageId, int tab = 0, const QString& pluginId = {},
        const QString& query = {}, qreal scrollY = 0);
    Q_INVOKABLE QVariantMap back();
    Q_INVOKABLE QVariantMap forward();
    Q_INVOKABLE QVariantMap jumpTo(int index);

signals:
    void changed();
    void restoringChanged();

private:
    std::vector<Entry> m_entries;
    int m_index = -1;
    bool m_restoring = false;
};

// Guards boundary-overscroll navigation between adjacent settings views so fast
// scrolling to the top/bottom of a page never flips pages accidentally:
// 1. After arriving at a boundary, wheel ticks within `settleMs` are treated as
//    the tail of that scroll burst and absorbed silently.
// 2. Once settled, deliberate wheel movement past the boundary accumulates
//    toward `threshold` (clamped per event by `maxTickDelta`), exposing
//    `direction`, `progress` and `readyToSwitch` for the docked edge cue and
//    auto-disarming after `disarmMs`.
// 3. A fast burst right at a resting boundary cannot snap across before the
//    cue has been visible for `minArmMs`: progress fills to the edge and waits
//    for one more deliberate push after `minArmMs`.
// 4. Upon switching, a cooldown prevents double-skipping and a brief
//    same-direction lock (`trailingLockMs`) swallows leftover wheel ticks so
//    the newly opened page lands cleanly at its edge.
class EdgeScrollGate : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY configChanged FINAL)
    Q_PROPERTY(QString sensitivity READ sensitivity WRITE setSensitivity NOTIFY configChanged FINAL)
    Q_PROPERTY(bool canPrevious READ canPrevious WRITE setCanPrevious NOTIFY configChanged FINAL)
    Q_PROPERTY(bool canNext READ canNext WRITE setCanNext NOTIFY configChanged FINAL)
    Q_PROPERTY(int threshold READ threshold WRITE setThreshold NOTIFY configChanged FINAL)
    Q_PROPERTY(int settleMs READ settleMs WRITE setSettleMs NOTIFY configChanged FINAL)
    Q_PROPERTY(int minArmMs READ minArmMs WRITE setMinArmMs NOTIFY configChanged FINAL)
    Q_PROPERTY(int maxTickDelta READ maxTickDelta WRITE setMaxTickDelta NOTIFY configChanged FINAL)
    Q_PROPERTY(int switchCooldownMs READ switchCooldownMs WRITE setSwitchCooldownMs NOTIFY configChanged FINAL)
    Q_PROPERTY(int trailingLockMs READ trailingLockMs WRITE setTrailingLockMs NOTIFY configChanged FINAL)
    Q_PROPERTY(int disarmMs READ disarmMs WRITE setDisarmMs NOTIFY configChanged FINAL)
    Q_PROPERTY(int direction READ direction NOTIFY stateChanged FINAL)
    Q_PROPERTY(qreal progress READ progress NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool armed READ armed NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool readyToSwitch READ readyToSwitch NOTIFY stateChanged FINAL)

public:
    using Clock = std::function<qint64()>;

    struct Preset {
        int threshold = 360;
        int settleMs = 340;
        int minArmMs = 280;
        int switchCooldownMs = 440;
        int trailingLockMs = 300;
        int disarmMs = 1600;
        int maxTickDelta = 120;
    };

    static Preset presetFor(QStringView sensitivity) noexcept;

    explicit EdgeScrollGate(QObject* parent = nullptr);

    bool enabled() const noexcept { return m_enabled; }
    void setEnabled(bool on);
    QString sensitivity() const { return m_sensitivity; }
    void setSensitivity(const QString& mode);
    bool canPrevious() const noexcept { return m_canPrevious; }
    void setCanPrevious(bool on);
    bool canNext() const noexcept { return m_canNext; }
    void setCanNext(bool on);
    int threshold() const noexcept { return m_threshold; }
    void setThreshold(int delta);
    int settleMs() const noexcept { return m_settleMs; }
    void setSettleMs(int ms);
    int minArmMs() const noexcept { return m_minArmMs; }
    void setMinArmMs(int ms);
    int maxTickDelta() const noexcept { return m_maxTickDelta; }
    void setMaxTickDelta(int delta);
    int switchCooldownMs() const noexcept { return m_switchCooldownMs; }
    void setSwitchCooldownMs(int ms);
    int trailingLockMs() const noexcept { return m_trailingLockMs; }
    void setTrailingLockMs(int ms);
    int disarmMs() const noexcept { return m_disarmMs; }
    void setDisarmMs(int ms);

    int direction() const noexcept { return m_direction; }
    qreal progress() const noexcept { return m_progress; }
    bool armed() const noexcept { return m_direction != 0 && m_progress > 0; }
    bool readyToSwitch() const noexcept { return armed() && m_accumulated + 1e-6 >= static_cast<qreal>(m_threshold); }

    // For deterministic unit tests: override the monotonic millisecond clock.
    void setClock(Clock clock);

    Q_INVOKABLE void noteBoundaryReached();
    Q_INVOKABLE void noteSwitched(int direction);
    Q_INVOKABLE bool shouldConsumeWheel(int direction);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool overscroll(int direction, qreal delta);

signals:
    void configChanged();
    void stateChanged();
    void armedDirectionChanged(int direction);
    void triggered(int direction);

private:
    qint64 nowMs() const;
    bool active() const noexcept;
    bool canMove(int direction) const noexcept;
    void applyPreset(const Preset& preset);
    void setState(int direction, qreal accumulated);

    bool m_enabled = true;
    QString m_sensitivity = QStringLiteral("standard");
    bool m_canPrevious = false;
    bool m_canNext = false;
    int m_threshold = 360; // three standard mouse-wheel notches (120 each)
    int m_settleMs = 340;
    int m_minArmMs = 280;
    int m_maxTickDelta = 120;
    int m_switchCooldownMs = 440;
    int m_trailingLockMs = 300;
    int m_disarmMs = 1600;

    int m_direction = 0; // -1 previous (top), +1 next (bottom), 0 idle
    qreal m_accumulated = 0;
    qreal m_progress = 0;

    qint64 m_settleUntil = 0;
    qint64 m_trailingLockUntil = 0;
    qint64 m_armedAt = 0;
    qint64 m_lastOverscrollAt = 0;
    int m_trailingLockDir = 0;

    QElapsedTimer m_clock;
    Clock m_clockOverride;
    QTimer m_disarmTimer;
};

} // namespace ws
