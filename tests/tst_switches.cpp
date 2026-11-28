// The system switches' cache and its writes (src/app/Switches), through a
// fake backend on the real switch thread. Nothing of the system is read or
// set: the platform backends are not built into this program, only their
// pure parts inline in their headers (SwitchTheme::stateOf,
// SwitchExplorer::onFor, SwitchRadios::stateOf, SwitchAudio's targetOf,
// levelOf and stateOf). Keep awake is the exception: SwitchKeepAwake is built
// in, over a clock the test moves; it holds a real power request for the
// moment a test needs it (made, looked at, cleared) and nothing else. What it
// does to the lid goes to power plans in memory; Windows' own are written
// only on a copy of the active plan made for the test and deleted after it.

#include "Switches.h"
#include "platform/SwitchAudio.h"
#include "platform/SwitchExplorer.h"
#include "platform/SwitchKeepAwake.h"
#include "platform/SwitchRadios.h"
#include "platform/SwitchTheme.h"

#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>
#include <objbase.h>
#include <powrprof.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws {

// QCOMPARE's words for an intent: its kind and value.
inline char* toString(const SwitchIntent& intent)
{
    return QTest::toString(u"Kind %1, %2"_s.arg(static_cast<int>(intent.kind)).arg(intent.value));
}

inline char* toString(const LidActions& lid)
{
    return QTest::toString(u"ac %1, dc %2"_s.arg(lid.ac).arg(lid.dc));
}

namespace {

using Kind = SwitchIntent::Kind;
using Value = SwitchState::Value;
using Availability = SwitchState::Availability;
using Outcome = Switches::Outcome;

const QString kTheme = u"theme"_s;
const QString kHidden = u"hidden-files"_s;
const QString kVolume = SwitchAudio::kId.toString();

SwitchState ready(Value on)
{
    SwitchState state;
    state.availability = Availability::Ready;
    state.on = on;
    return state;
}

SwitchState volume(int level, bool muted)
{
    return SwitchAudio::stateOf(level, muted, u"扬声器 (测试)"_s);
}

SwitchIntent intent(Kind kind, int value = 0)
{
    return SwitchIntent {.kind = kind, .value = value};
}

// Where the switch thread waits (in a read or a write) while the test holds
// it shut.
class Gate {
public:
    void shut()
    {
        std::lock_guard lock(m_mutex);
        m_open = false;
    }
    void open()
    {
        {
            std::lock_guard lock(m_mutex);
            m_open = true;
        }
        m_changed.notify_all();
    }
    bool held() // the switch thread is waiting at it
    {
        std::lock_guard lock(m_mutex);
        return m_waiting > 0;
    }
    void pass()
    {
        std::unique_lock lock(m_mutex);
        ++m_waiting;
        m_changed.wait(lock, [this] { return m_open; });
        --m_waiting;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_open = true;
    int m_waiting = 0;
};

bool inMultithreadedApartment()
{
    APTTYPE type {};
    APTTYPEQUALIFIER qualifier {};
    return SUCCEEDED(::CoGetApartmentType(&type, &qualifier)) && type == APTTYPE_MTA;
}

// The system as the fake backend has it, shared with the test: what reads
// give, how writes go, and what was asked on which thread.
struct FakeSystem {
    std::mutex mutex;
    std::map<QString, SwitchState> states;
    std::map<QString, QString> failWith; // write(): fails with these words, changing nothing
    std::vector<std::pair<QString, SwitchIntent>> writes; // as asked, in order
    int reads = 0;
    bool offGuiThread = true; // every read and write ran on another thread than the test's
    bool multithreaded = true; // ... in the multithreaded apartment
    std::vector<QString> written; // written(id)
    bool writtenOnGuiThread = true;
    bool closed = false;
    bool closedOffGuiThread = false;
    std::optional<std::stop_token> stop; // setStopToken()
    bool stopBeforeWatch = false; // ... it came before watch()
    SwitchBackend::Notify notify;
    Gate readGate;
    Gate writeGate;
    const DWORD guiThread = ::GetCurrentThreadId();

    void set(const QString& id, const SwitchState& state)
    {
        std::lock_guard lock(mutex);
        states[id] = state;
    }
    void fail(const QString& id, const QString& words)
    {
        std::lock_guard lock(mutex);
        if (words.isEmpty())
            failWith.erase(id);
        else
            failWith[id] = words;
    }
    QList<Kind> writesTo(const QString& id) // the kinds asked of `id`, in order
    {
        std::lock_guard lock(mutex);
        QList<Kind> kinds;
        for (const auto& [to, what] : writes) {
            if (to == id)
                kinds.append(what.kind);
        }
        return kinds;
    }
    std::vector<SwitchIntent> writesOf(const QString& id) // the intents asked of `id`, in order
    {
        std::lock_guard lock(mutex);
        std::vector<SwitchIntent> intents;
        for (const auto& [to, what] : writes) {
            if (to == id)
                intents.push_back(what);
        }
        return intents;
    }
    std::size_t writeCount()
    {
        std::lock_guard lock(mutex);
        return writes.size();
    }
    int readCount()
    {
        std::lock_guard lock(mutex);
        return reads;
    }
    void noteThread() // under the lock
    {
        offGuiThread = offGuiThread && ::GetCurrentThreadId() != guiThread;
        multithreaded = multithreaded && inMultithreadedApartment();
    }
};

class FakeBackend final : public SwitchBackend {
public:
    FakeBackend(std::shared_ptr<FakeSystem> system, QStringList ids)
        : m_system(std::move(system))
        , m_ids(std::move(ids))
    {
    }

    QStringList ids() const override { return m_ids; }

    void setStopToken(std::stop_token stop) override
    {
        std::lock_guard lock(m_system->mutex);
        m_system->stop = std::move(stop);
        m_system->stopBeforeWatch = !m_system->notify;
    }

    void watch(Notify changed) override
    {
        std::lock_guard lock(m_system->mutex);
        m_system->notify = std::move(changed);
    }

    SwitchState read(const QString& id) override
    {
        m_system->readGate.pass();
        std::lock_guard lock(m_system->mutex);
        m_system->noteThread();
        ++m_system->reads;
        const auto it = m_system->states.find(id);
        return it != m_system->states.end() ? it->second : SwitchState {};
    }

    SwitchWrite write(const QString& id, const SwitchIntent& intent) override
    {
        m_system->writeGate.pass();
        std::lock_guard lock(m_system->mutex);
        m_system->noteThread();
        m_system->writes.emplace_back(id, intent);
        SwitchState& state = m_system->states[id];
        SwitchWrite result;
        result.after = state;
        if (!state.ready())
            return result;
        if (const auto failure = m_system->failWith.find(id); failure != m_system->failWith.end()) {
            result.error = failure->second;
            return result;
        }
        if (id == kVolume) { // its level and its mute, where SwitchAudio takes them
            const bool muted = state.on == Value::Off;
            const std::optional<SwitchAudio::Target> target = SwitchAudio::targetOf(state.level, muted, intent);
            result.ok = target.has_value();
            if (target && (target->level != state.level || target->muted != muted)) {
                state.level = static_cast<qint8>(target->level);
                state.on = target->muted ? Value::Off : Value::On;
                result.wrote = true;
            }
            result.after = state;
            return result;
        }
        const Value target = intent.kind == Kind::On ? Value::On : Value::Off;
        result.ok = true;
        if (state.on != target) {
            state.on = target;
            result.wrote = true;
        }
        result.after = state;
        return result;
    }

    void written(const QString& id) override
    {
        std::lock_guard lock(m_system->mutex);
        m_system->written.push_back(id);
        m_system->writtenOnGuiThread = m_system->writtenOnGuiThread && ::GetCurrentThreadId() == m_system->guiThread;
    }

    void close() override
    {
        std::lock_guard lock(m_system->mutex);
        m_system->closed = true;
        m_system->closedOffGuiThread = ::GetCurrentThreadId() != m_system->guiThread;
    }

private:
    std::shared_ptr<FakeSystem> m_system;
    QStringList m_ids;
};

// A Switches over the fake theme and hidden-files switches ("volume" has
// no backend), or the ones `ids` says. Its gates are opened before it goes,
// so that the switch thread can end.
struct Rig {
    std::shared_ptr<FakeSystem> system = std::make_shared<FakeSystem>();
    std::unique_ptr<Switches> switches;

    explicit Rig(std::map<QString, SwitchState> states, const Switches::Options& options = {}, bool holdReads = false,
        const QStringList& ids = {kTheme, kHidden})
    {
        system->states = std::move(states);
        if (holdReads)
            system->readGate.shut();
        SwitchBackends backends;
        backends.push_back(std::make_unique<FakeBackend>(system, ids));
        switches = std::make_unique<Switches>(std::move(backends), options);
    }
    ~Rig()
    {
        system->readGate.open();
        system->writeGate.open();
        switches.reset();
    }

    SwitchState state(const QString& id = kTheme) const { return switches->state(id); }
    void notify(const QString& id = kTheme) // as a backend's watch would, from another thread
    {
        SwitchBackend::Notify notify;
        {
            std::lock_guard lock(system->mutex);
            notify = system->notify;
        }
        std::thread([notify, id] { notify(id); }).join();
    }
    // Lets the reads asked so far come back: one more than `before` was made.
    void settleReads(int before)
    {
        QTRY_VERIFY(system->readCount() > before);
        QTest::qWait(20); // its result, queued to this thread
    }
};

const QString kAwake = SwitchKeepAwake::kId.toString();

// Keep awake's wall clock, which the test moves (local time, to the second).
struct FakeClock {
    std::shared_ptr<std::atomic<qint64>> ms = std::make_shared<std::atomic<qint64>>(
        QDateTime(QDate(2026, 10, 19), QTime(15, 0)).toMSecsSinceEpoch());

    SwitchKeepAwake::Clock clock() const
    {
        return [ms = ms] { return QDateTime::fromMSecsSinceEpoch(ms->load()); };
    }
    QDateTime now() const { return QDateTime::fromMSecsSinceEpoch(ms->load()); }
    void advance(int minutes) { *ms += minutes * 60'000LL; }
};

QDateTime at(int day, int hour, int minute, int second = 0, int msec = 0)
{
    return QDateTime(QDate(2026, 10, day), QTime(hour, minute, second, msec));
}

// Power plans in memory: the schemes' lid actions, the active one, and what
// a write does. Shared, as LidOverride owns its PowerPlans.
struct FakePlans final : PowerPlans {
    struct Data {
        std::map<QUuid, LidActions> schemes;
        QUuid active;
        bool lid = true;
        bool policy = false;
        bool refuse = false; // every write fails
        bool halfOnce = false; // the next write sets ac, then fails
        int writes = 0;
        int applied = 0; // the active scheme made active again
    };
    explicit FakePlans(std::shared_ptr<Data> data) : d(std::move(data)) { }
    bool lidPresent() override { return d->lid; }
    bool lidByPolicy() override { return d->policy; }
    QUuid active() override { return d->active; }
    std::optional<LidActions> lid(const QUuid& scheme) override
    {
        const auto it = d->schemes.find(scheme);
        return it == d->schemes.end() ? std::nullopt : std::optional(it->second);
    }
    bool setLid(const QUuid& scheme, const LidActions& actions) override
    {
        if (d->refuse || !d->schemes.contains(scheme))
            return false;
        if (std::exchange(d->halfOnce, false)) {
            d->schemes[scheme].ac = actions.ac;
            return false;
        }
        ++d->writes;
        d->schemes[scheme] = actions;
        if (scheme == d->active)
            ++d->applied;
        return true;
    }
    std::shared_ptr<Data> d;
};

std::unique_ptr<LidOverride> lidOver(const std::shared_ptr<FakePlans::Data>& data, const QString& record)
{
    return std::make_unique<LidOverride>(std::make_unique<FakePlans>(data), record);
}

// What Windows says is keeping the computer awake: ES_SYSTEM_REQUIRED (1) and
// ES_DISPLAY_REQUIRED (2), whoever holds them.
ULONG executionState()
{
    ULONG state = 0;
    ::CallNtPowerInformation(SystemExecutionState, nullptr, 0, &state, sizeof state);
    return state;
}

} // namespace

class SwitchesTest : public QObject {
    Q_OBJECT

private slots:
    void readsAtStart();
    void toggleBecomesExplicit();
    void alreadyWritesNothing();
    void mixedGoesWhereBackendSays();
    void notReadyOpensPage();
    void failureGoesBack();
    void errorFadesOnRefresh();
    void changedElsewhere();
    void readDuringWriteKeepsTarget();
    void toggleWaitsForState();
    void toggleWaitTimesOut();
    void toggleWaitEndsNotReady();
    void explicitWhileUnknown();
    void pressesWhileBusy();
    void backendFindsItAlready();
    void closedOnSwitchThread();
    void themeStates();
    void explorerValues();
    void levelResolution();
    void steppedSnaps();
    void setLevelUnmutes();
    void stepDownKeepsMute();
    void stepsCoalesce();
    void stepAtEnds();
    void stepBeforeRead();
    void stepNotReady();
    void audioTargets();
    void radioStates();
    void relayStopsAtClose();
    void stopTokenGiven();
    void keepAwakeEndTimes();
    void keepAwakeNotes();
    void keepAwakeHolds();
    void keepAwakeEndsByTheClock();
    void keepAwakeTimer();
    void keepAwakeThroughSwitches();
    void lidHeldAndPutBack();
    void lidAfterACrash();
    void keepAwakeHoldsTheLid();
    void lidOnWindowsPlans();
};

void SwitchesTest::readsAtStart()
{
    Rig rig({{kTheme, ready(Value::Off)}, {kHidden, ready(Value::On)}}, {}, true);
    QSignalSpy changed(rig.switches.get(), &Switches::changed);
    QCOMPARE(rig.state().availability, Availability::Unknown);
    QCOMPARE(rig.state().on, Value::Unknown);
    rig.system->readGate.open();
    QTRY_VERIFY(rig.state().ready() && rig.state(kHidden).ready());
    QCOMPARE(rig.state().on, Value::Off);
    QCOMPARE(rig.state(kHidden).on, Value::On);
    QCOMPARE(changed.count(), 2); // once each
    QVERIFY(rig.system->offGuiThread);
    QVERIFY(rig.system->multithreaded);

    // No backend: an ordinary place, Enter opens its page.
    QCOMPARE(rig.state(u"volume"_s).availability, Availability::Unsupported);
    QCOMPARE(rig.switches->apply(u"volume"_s, intent(Kind::Toggle)), Outcome::OpenPage);
    QCOMPARE(rig.switches->apply(u""_s, intent(Kind::On)), Outcome::OpenPage);
    QCOMPARE(rig.switches->resolve(u"volume"_s, intent(Kind::Toggle)).kind, Kind::Toggle);
}

void SwitchesTest::toggleBecomesExplicit()
{
    Rig rig({{kTheme, ready(Value::Off)}});
    QTRY_VERIFY(rig.state().ready());
    QCOMPARE(rig.switches->resolve(kTheme, intent(Kind::Toggle)).kind, Kind::On);
    QCOMPARE(rig.switches->resolve(kTheme, intent(Kind::Off)).kind, Kind::Off); // explicit: as it is

    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Toggle)), Outcome::Started);
    QVERIFY(rig.state().busy); // at the target at once
    QCOMPARE(rig.state().on, Value::On);
    QTRY_VERIFY(!rig.state().busy);
    QCOMPARE(rig.state().on, Value::On);
    QCOMPARE(rig.system->writesTo(kTheme), QList<Kind> {Kind::On}); // never Toggle

    QCOMPARE(rig.switches->resolve(kTheme, intent(Kind::Toggle)).kind, Kind::Off);
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Toggle)), Outcome::Started);
    QTRY_VERIFY(!rig.state().busy);
    QCOMPARE(rig.state().on, Value::Off);
    QCOMPARE(rig.system->writesTo(kTheme), (QList<Kind> {Kind::On, Kind::Off}));
    QVERIFY(rig.system->offGuiThread);
    QVERIFY(rig.system->multithreaded);
    // Told on the GUI thread after each write that changed something.
    QCOMPARE(rig.system->written, (std::vector<QString> {kTheme, kTheme}));
    QVERIFY(rig.system->writtenOnGuiThread);
}

void SwitchesTest::alreadyWritesNothing()
{
    Rig rig({{kTheme, ready(Value::On)}, {kHidden, ready(Value::Off)}});
    QTRY_VERIFY(rig.state().ready() && rig.state(kHidden).ready());
    QSignalSpy changed(rig.switches.get(), &Switches::changed);
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::On)), Outcome::Already);
    QCOMPARE(rig.switches->apply(kHidden, intent(Kind::Off)), Outcome::Already);
    QTest::qWait(50);
    QVERIFY(rig.system->writesTo(kTheme).isEmpty());
    QVERIFY(rig.system->writesTo(kHidden).isEmpty());
    QCOMPARE(changed.count(), 0);
    QVERIFY(!rig.state().busy);
}

void SwitchesTest::mixedGoesWhereBackendSays()
{
    SwitchState mixed = ready(Value::Mixed);
    mixed.mixedToggleOn = true; // dark mode: the apps light, the system dark
    mixed.note = u"自定义：应用浅色，系统深色"_s;
    Rig rig({{kTheme, mixed}});
    QTRY_VERIFY(rig.state().ready());
    QCOMPARE(rig.state().on, Value::Mixed);
    QCOMPARE(rig.switches->resolve(kTheme, intent(Kind::Toggle)).kind, Kind::On);
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Toggle)), Outcome::Started);
    QTRY_VERIFY(!rig.state().busy);
    QCOMPARE(rig.state().on, Value::On);
    QCOMPARE(rig.system->writesTo(kTheme), QList<Kind> {Kind::On});

    // The other way round, told by the backend; an explicit Off is written
    // from Mixed too (half of it is off, not all).
    mixed.mixedToggleOn = false;
    rig.system->set(kTheme, mixed);
    rig.notify();
    QTRY_COMPARE(rig.state().on, Value::Mixed);
    QCOMPARE(rig.switches->resolve(kTheme, intent(Kind::Toggle)).kind, Kind::Off);
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Off)), Outcome::Started);
    QTRY_VERIFY(!rig.state().busy);
    QCOMPARE(rig.state().on, Value::Off);
    QCOMPARE(rig.system->writesTo(kTheme), (QList<Kind> {Kind::On, Kind::Off}));
}

void SwitchesTest::notReadyOpensPage()
{
    SwitchState blocked;
    blocked.availability = Availability::Blocked;
    blocked.on = Value::Off;
    blocked.note = u"对比度主题开着时，深浅色不起作用"_s;
    blocked.openInstead = u"ms-settings:easeofaccess-highcontrast"_s;
    SwitchState none;
    none.availability = Availability::NoDevice;
    none.note = u"这台电脑没有蓝牙"_s;
    Rig rig({{kTheme, blocked}, {kHidden, none}});
    QTRY_COMPARE(rig.state().availability, Availability::Blocked);
    QTRY_COMPARE(rig.state(kHidden).availability, Availability::NoDevice);
    QCOMPARE(rig.state().openInstead, blocked.openInstead);
    QCOMPARE(rig.state().note, blocked.note);
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Toggle)), Outcome::OpenPage);
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::On)), Outcome::OpenPage);
    QCOMPARE(rig.switches->apply(kHidden, intent(Kind::Toggle)), Outcome::OpenPage);
    // "蓝牙设置": the page, whatever the state.
    QCOMPARE(rig.switches->apply(kHidden, intent(Kind::OpenSettings)), Outcome::OpenPage);
    QTest::qWait(50);
    QCOMPARE(rig.system->writeCount(), std::size_t {0});
}

void SwitchesTest::failureGoesBack()
{
    Rig rig({{kTheme, ready(Value::Off)}});
    QTRY_VERIFY(rig.state().ready());
    rig.system->fail(kTheme, u"没能切到深色：测试"_s);
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Toggle)), Outcome::Started);
    QVERIFY(rig.state().busy);
    QCOMPARE(rig.state().on, Value::On);
    QTRY_VERIFY(!rig.state().busy);
    QCOMPARE(rig.state().on, Value::Off); // back where it is
    QCOMPARE(rig.state().error, u"没能切到深色：测试"_s);
    QVERIFY(rig.system->written.empty()); // nothing changed: no one told

    // Enter again is a retry: the words go at once.
    rig.system->fail(kTheme, {});
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Toggle)), Outcome::Started);
    QVERIFY(rig.state().error.isEmpty());
    QTRY_VERIFY(!rig.state().busy);
    QCOMPARE(rig.state().on, Value::On);
    QVERIFY(rig.state().error.isEmpty());
}

void SwitchesTest::errorFadesOnRefresh()
{
    auto clock = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::time_point {} + 1h);
    Switches::Options options;
    options.now = [clock] { return *clock; };
    Rig rig({{kTheme, ready(Value::Off)}}, options);
    QTRY_VERIFY(rig.state().ready());
    rig.system->fail(kTheme, u"没能切到深色：测试"_s);
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::On)), Outcome::Started);
    QTRY_VERIFY(!rig.state().busy);
    QVERIFY(!rig.state().error.isEmpty());

    // Opened again within 30 s: the words are still there, the same state read again.
    *clock += 29s;
    int reads = rig.system->readCount();
    rig.switches->refresh();
    rig.settleReads(reads);
    QVERIFY(!rig.state().error.isEmpty());

    // Later than that: gone as the window opens.
    *clock += 2s;
    QSignalSpy changed(rig.switches.get(), &Switches::changed);
    rig.switches->refresh();
    QVERIFY(rig.state().error.isEmpty());
    QCOMPARE(changed.count(), 1);
}

void SwitchesTest::changedElsewhere()
{
    Rig rig({{kTheme, ready(Value::Off)}});
    QTRY_VERIFY(rig.state().ready());
    QSignalSpy changed(rig.switches.get(), &Switches::changed);
    rig.system->set(kTheme, ready(Value::On));
    rig.notify(); // from another thread, as a radio's callback comes
    QTRY_COMPARE(rig.state().on, Value::On);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.at(0).at(0).toString(), kTheme);

    // The same state read again: nothing to tell.
    int reads = rig.system->readCount();
    rig.notify();
    rig.settleReads(reads);
    QCOMPARE(changed.count(), 1);

    // A failure's words go once the switch moves elsewhere: they no longer
    // say how things are.
    rig.system->fail(kTheme, u"没能切到浅色：测试"_s);
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Off)), Outcome::Started);
    QTRY_VERIFY(!rig.state().busy);
    QCOMPARE(rig.state().on, Value::On);
    QVERIFY(!rig.state().error.isEmpty());
    reads = rig.system->readCount();
    rig.notify(); // read again, unmoved: the words stay
    rig.settleReads(reads);
    QVERIFY(!rig.state().error.isEmpty());
    rig.system->set(kTheme, ready(Value::Off));
    rig.notify();
    QTRY_COMPARE(rig.state().on, Value::Off);
    QVERIFY(rig.state().error.isEmpty());
}

void SwitchesTest::readDuringWriteKeepsTarget()
{
    Rig rig({{kTheme, ready(Value::Off)}});
    QTRY_VERIFY(rig.state().ready());
    QList<SwitchState> seen;
    connect(rig.switches.get(), &Switches::changed, this, [&](const QString& id) { seen.append(rig.state(id)); });

    // A read on its way when Enter is pressed, coming back from halfway
    // through the write (dark mode's apps value written, the system's not).
    rig.system->readGate.shut();
    rig.notify();
    QTRY_VERIFY(rig.system->readGate.held());
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::On)), Outcome::Started);
    rig.system->set(kTheme, ready(Value::Mixed));
    rig.system->readGate.open();
    QTRY_VERIFY(!rig.state().busy);
    QCOMPARE(rig.state().on, Value::On);
    for (const SwitchState& state : seen)
        QVERIFY(state.on != Value::Mixed); // never "自定义" on the way
}

void SwitchesTest::toggleWaitsForState()
{
    Rig rig({{kTheme, ready(Value::Off)}}, {}, true);
    QSignalSpy ended(rig.switches.get(), &Switches::waitEnded);
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Toggle)), Outcome::Waiting);
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Toggle)), Outcome::Busy); // one waits already
    QCOMPARE(rig.switches->resolve(kTheme, intent(Kind::Toggle)).kind, Kind::Toggle); // the footer can't say yet
    rig.system->readGate.open();
    QTRY_COMPARE(ended.count(), 1);
    QCOMPARE(ended.at(0).at(0).toString(), kTheme);
    QCOMPARE(ended.at(0).at(1).value<Outcome>(), Outcome::Started);
    QTRY_VERIFY(!rig.state().busy);
    QCOMPARE(rig.state().on, Value::On);
    QCOMPARE(rig.system->writesTo(kTheme), QList<Kind> {Kind::On});
    QTest::qWait(600); // past the wait: it does not end twice
    QCOMPARE(ended.count(), 1);
}

void SwitchesTest::toggleWaitTimesOut()
{
    Switches::Options options;
    options.stateWait = 50ms;
    Rig rig({{kTheme, ready(Value::Off)}}, options, true);
    QSignalSpy ended(rig.switches.get(), &Switches::waitEnded);
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Toggle)), Outcome::Waiting);
    QTRY_COMPARE(ended.count(), 1);
    QCOMPARE(ended.at(0).at(1).value<Outcome>(), Outcome::NotRead);
    rig.system->readGate.open();
    QTRY_VERIFY(rig.state().ready());
    QTest::qWait(50);
    QCOMPARE(rig.system->writeCount(), std::size_t {0}); // the state came too late: nothing done
    QCOMPARE(rig.state().on, Value::Off);
    QCOMPARE(ended.count(), 1);
}

void SwitchesTest::toggleWaitEndsNotReady()
{
    SwitchState none;
    none.availability = Availability::NoDevice;
    none.note = u"这台电脑没有蓝牙"_s;
    Rig rig({{kTheme, none}}, {}, true);
    QSignalSpy ended(rig.switches.get(), &Switches::waitEnded);
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Toggle)), Outcome::Waiting);
    rig.system->readGate.open();
    QTRY_COMPARE(ended.count(), 1);
    QCOMPARE(ended.at(0).at(1).value<Outcome>(), Outcome::OpenPage);
    QCOMPARE(rig.system->writeCount(), std::size_t {0});
}

void SwitchesTest::explicitWhileUnknown()
{
    // On and Off mean the same whatever the state: written at once.
    {
        Rig rig({{kTheme, ready(Value::Off)}}, {}, true);
        QCOMPARE(rig.switches->apply(kTheme, intent(Kind::On)), Outcome::Started);
        QVERIFY(rig.state().busy);
        QCOMPARE(rig.state().on, Value::On);
        rig.system->readGate.open();
        QTRY_VERIFY(!rig.state().busy);
        QCOMPARE(rig.state().on, Value::On);
        QCOMPARE(rig.system->writesTo(kTheme), QList<Kind> {Kind::On});
    }
    // ... and ahead of a toggle that waits.
    {
        Rig rig({{kTheme, ready(Value::Off)}}, {}, true);
        QSignalSpy ended(rig.switches.get(), &Switches::waitEnded);
        QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Toggle)), Outcome::Waiting);
        QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Off)), Outcome::Started);
        QCOMPARE(ended.count(), 1);
        QCOMPARE(ended.at(0).at(1).value<Outcome>(), Outcome::Busy);
        rig.system->readGate.open();
        QTRY_VERIFY(!rig.state().busy);
        QCOMPARE(rig.state().on, Value::Off);
        QCOMPARE(rig.system->writesTo(kTheme), QList<Kind> {Kind::Off}); // the toggle never went
        QVERIFY(rig.system->written.empty()); // it was off already
    }
}

void SwitchesTest::pressesWhileBusy()
{
    Rig rig({{kTheme, ready(Value::Off)}});
    QTRY_VERIFY(rig.state().ready());
    QList<SwitchState> seen;
    connect(rig.switches.get(), &Switches::changed, this, [&](const QString& id) { seen.append(rig.state(id)); });
    rig.system->writeGate.shut();
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Toggle)), Outcome::Started); // On, held
    QTRY_VERIFY(rig.system->writeGate.held());
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Toggle)), Outcome::Busy); // a toggle: ignored
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::On)), Outcome::Busy); // being written
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Off)), Outcome::Started); // next, after it
    QVERIFY(rig.state().busy);
    QCOMPARE(rig.state().on, Value::Off);
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Off)), Outcome::Busy); // asked already
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::On)), Outcome::Started); // takes the Off back
    QCOMPARE(rig.state().on, Value::On);
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::Off)), Outcome::Started); // and Off again
    rig.system->writeGate.open();
    QTRY_VERIFY(!rig.state().busy);
    QCOMPARE(rig.state().on, Value::Off);
    QCOMPARE(rig.system->writesTo(kTheme), (QList<Kind> {Kind::On, Kind::Off})); // two writes for six presses
    // The overtaken result (On, done) was never shown.
    for (const SwitchState& state : seen)
        QVERIFY(state.busy || state.on == Value::Off);
    QCOMPARE(rig.system->written, (std::vector<QString> {kTheme, kTheme})); // both changed the system
}

void SwitchesTest::backendFindsItAlready()
{
    Rig rig({{kTheme, ready(Value::Off)}});
    QTRY_VERIFY(rig.state().ready());
    rig.system->set(kTheme, ready(Value::On)); // changed elsewhere, not told yet
    QCOMPARE(rig.switches->apply(kTheme, intent(Kind::On)), Outcome::Started);
    QTRY_VERIFY(!rig.state().busy);
    QCOMPARE(rig.state().on, Value::On);
    QVERIFY(rig.state().error.isEmpty());
    QVERIFY(rig.system->written.empty()); // nothing changed: no one told
}

void SwitchesTest::closedOnSwitchThread()
{
    std::shared_ptr<FakeSystem> system;
    {
        Rig rig({{kTheme, ready(Value::Off)}});
        system = rig.system;
        QTRY_VERIFY(rig.state().ready());
        QVERIFY(!system->closed);
    }
    QVERIFY(system->closed);
    QVERIFY(system->closedOffGuiThread);
}

void SwitchesTest::themeStates()
{
    const SwitchState dark = SwitchTheme::stateOf(true, true, false);
    QCOMPARE(dark.availability, Availability::Ready);
    QCOMPARE(dark.on, Value::On);
    QVERIFY(dark.note.isEmpty());
    const SwitchState light = SwitchTheme::stateOf(false, false, false);
    QCOMPARE(light.on, Value::Off);
    QVERIFY(light.note.isEmpty());

    // Mixed: a toggle goes to the opposite of the apps' value.
    const SwitchState appsLight = SwitchTheme::stateOf(false, true, false);
    QCOMPARE(appsLight.on, Value::Mixed);
    QVERIFY(appsLight.mixedToggleOn); // to dark
    QCOMPARE(appsLight.note, u"自定义：应用浅色，系统深色"_s);
    const SwitchState appsDark = SwitchTheme::stateOf(true, false, false);
    QCOMPARE(appsDark.on, Value::Mixed);
    QVERIFY(!appsDark.mixedToggleOn); // to light
    QCOMPARE(appsDark.note, u"自定义：应用深色，系统浅色"_s);

    // High contrast: not here; Enter opens its page.
    const SwitchState contrast = SwitchTheme::stateOf(true, true, true);
    QCOMPARE(contrast.availability, Availability::Blocked);
    QCOMPARE(contrast.openInstead, u"ms-settings:easeofaccess-highcontrast"_s);
    QCOMPARE(contrast.note, u"对比度主题开着时，深浅色不起作用"_s);

    // Through Switches: where Enter goes from each.
    Rig rig({{kTheme, appsLight}, {kHidden, appsDark}});
    QTRY_VERIFY(rig.state().ready() && rig.state(kHidden).ready());
    QCOMPARE(rig.switches->resolve(kTheme, intent(Kind::Toggle)).kind, Kind::On);
    QCOMPARE(rig.switches->resolve(kHidden, intent(Kind::Toggle)).kind, Kind::Off);
}

void SwitchesTest::explorerValues()
{
    QVERIFY(SwitchExplorer::onFor(SwitchExplorer::kHiddenFiles, 1)); // Hidden 1: shown
    QVERIFY(!SwitchExplorer::onFor(SwitchExplorer::kHiddenFiles, 2)); // 2, or missing
    QVERIFY(SwitchExplorer::onFor(SwitchExplorer::kFileExtensions, 0)); // HideFileExt 0: shown
    QVERIFY(!SwitchExplorer::onFor(SwitchExplorer::kFileExtensions, 1)); // 1, or missing
}

void SwitchesTest::levelResolution()
{
    // Before the level is read: a step stays relative, for the backend; a
    // level asked is kept within 0–100 all the same.
    Rig rig({{kVolume, volume(24, false)}}, {}, true, {kVolume});
    QCOMPARE(rig.switches->resolve(kVolume, intent(Kind::StepLevel, 10)), intent(Kind::StepLevel, 10));
    QCOMPARE(rig.switches->resolve(kVolume, intent(Kind::SetLevel, 150)), intent(Kind::SetLevel, 100));
    rig.system->readGate.open();
    QTRY_VERIFY(rig.state(kVolume).ready());
    QCOMPARE(rig.state(kVolume).level, qint8 {24});

    // Read: "音量 150" sets 100, a step goes to the level it reaches (just
    // that: typed steps are not put on the multiples of 5), up a SetLevel
    // (which unmutes), down LevelOnly (the mute as it is).
    QCOMPARE(rig.switches->resolve(kVolume, intent(Kind::SetLevel, 150)), intent(Kind::SetLevel, 100));
    QCOMPARE(rig.switches->resolve(kVolume, intent(Kind::SetLevel, -5)), intent(Kind::SetLevel, 0));
    QCOMPARE(rig.switches->resolve(kVolume, intent(Kind::SetLevel, 30)), intent(Kind::SetLevel, 30));
    QCOMPARE(rig.switches->resolve(kVolume, intent(Kind::StepLevel, 10)), intent(Kind::SetLevel, 34));
    QCOMPARE(rig.switches->resolve(kVolume, intent(Kind::StepLevel, -5)), intent(Kind::LevelOnly, 19));
    QCOMPARE(rig.switches->resolve(kVolume, intent(Kind::StepLevel, -50)), intent(Kind::LevelOnly, 0));
    QCOMPARE(rig.switches->resolve(kVolume, intent(Kind::StepLevel, 200)), intent(Kind::SetLevel, 100));
    // The plain row's Enter is the sound: audible now, so it mutes.
    QCOMPARE(rig.switches->resolve(kVolume, intent(Kind::Toggle)), intent(Kind::Off));
    QCOMPARE(rig.switches->resolve(kVolume, intent(Kind::Off)), intent(Kind::Off));

    // A typed step is written as resolved: absolute.
    QCOMPARE(rig.switches->apply(kVolume, intent(Kind::StepLevel, 10)), Outcome::Started);
    QCOMPARE(rig.state(kVolume).level, qint8 {34}); // at the target at once
    QTRY_VERIFY(!rig.state(kVolume).busy);
    QCOMPARE(rig.state(kVolume).level, qint8 {34});
    QCOMPARE(rig.system->writesOf(kVolume), std::vector<SwitchIntent> {intent(Kind::SetLevel, 34)});
}

void SwitchesTest::steppedSnaps()
{
    // Onto the multiples of 5 first, then on in fives, within 0–100.
    QCOMPARE(Switches::stepped(24, 1), intent(Kind::SetLevel, 25));
    QCOMPARE(Switches::stepped(24, -1), intent(Kind::LevelOnly, 20));
    QCOMPARE(Switches::stepped(25, 1), intent(Kind::SetLevel, 30));
    QCOMPARE(Switches::stepped(25, -1), intent(Kind::LevelOnly, 20));
    QCOMPARE(Switches::stepped(21, -1), intent(Kind::LevelOnly, 20));
    QCOMPARE(Switches::stepped(24, 2), intent(Kind::SetLevel, 30));
    QCOMPARE(Switches::stepped(24, -2), intent(Kind::LevelOnly, 15));
    QCOMPARE(Switches::stepped(0, 1), intent(Kind::SetLevel, 5));
    QCOMPARE(Switches::stepped(0, -1), intent(Kind::LevelOnly, 0));
    QCOMPARE(Switches::stepped(3, -1), intent(Kind::LevelOnly, 0));
    QCOMPARE(Switches::stepped(98, 1), intent(Kind::SetLevel, 100));
    QCOMPARE(Switches::stepped(100, 1), intent(Kind::SetLevel, 100)); // still up: unmutes
    QCOMPARE(Switches::stepped(100, -1), intent(Kind::LevelOnly, 95));
    QCOMPARE(Switches::stepped(50, 1000), intent(Kind::SetLevel, 100));
    QCOMPARE(Switches::stepped(50, -1000), intent(Kind::LevelOnly, 0));
}

void SwitchesTest::setLevelUnmutes()
{
    Rig rig({{kVolume, volume(24, true)}}, {}, false, {kVolume});
    QTRY_VERIFY(rig.state(kVolume).ready());
    QCOMPARE(rig.state(kVolume).on, Value::Off); // muted

    // "音量 30" while muted at 24: heard at 30.
    QCOMPARE(rig.switches->apply(kVolume, intent(Kind::SetLevel, 30)), Outcome::Started);
    QVERIFY(rig.state(kVolume).busy);
    QCOMPARE(rig.state(kVolume).level, qint8 {30});
    QCOMPARE(rig.state(kVolume).on, Value::On);
    QTRY_VERIFY(!rig.state(kVolume).busy);
    QCOMPARE(rig.state(kVolume).level, qint8 {30});
    QCOMPARE(rig.state(kVolume).on, Value::On);
    QCOMPARE(rig.state(kVolume).note, u"扬声器 (测试)"_s);
    QCOMPARE(rig.switches->apply(kVolume, intent(Kind::SetLevel, 30)), Outcome::Already);

    // Muted again by hand: the same level asked is not "already" (it unmutes).
    QCOMPARE(rig.switches->apply(kVolume, intent(Kind::Off)), Outcome::Started);
    QTRY_VERIFY(!rig.state(kVolume).busy);
    QCOMPARE(rig.state(kVolume).on, Value::Off);
    QCOMPARE(rig.state(kVolume).level, qint8 {30}); // the mute alone
    QCOMPARE(rig.switches->apply(kVolume, intent(Kind::SetLevel, 30)), Outcome::Started);
    QTRY_VERIFY(!rig.state(kVolume).busy);
    QCOMPARE(rig.state(kVolume).on, Value::On);

    // "音量 150": 100.
    QCOMPARE(rig.switches->apply(kVolume, intent(Kind::SetLevel, 150)), Outcome::Started);
    QTRY_VERIFY(!rig.state(kVolume).busy);
    QCOMPARE(rig.state(kVolume).level, qint8 {100});
    QCOMPARE(rig.system->writesOf(kVolume),
        (std::vector<SwitchIntent> {intent(Kind::SetLevel, 30), intent(Kind::Off), intent(Kind::SetLevel, 30),
            intent(Kind::SetLevel, 100)}));
}

void SwitchesTest::stepDownKeepsMute()
{
    Rig rig({{kVolume, volume(24, true)}}, {}, false, {kVolume});
    QTRY_VERIFY(rig.state(kVolume).ready());
    QCOMPARE(rig.switches->step(kVolume, -1), Outcome::Started);
    QCOMPARE(rig.state(kVolume).level, qint8 {20});
    QCOMPARE(rig.state(kVolume).on, Value::Off); // still muted, at once and after
    QTRY_VERIFY(!rig.state(kVolume).busy);
    QCOMPARE(rig.state(kVolume).level, qint8 {20});
    QCOMPARE(rig.state(kVolume).on, Value::Off);
    // A step up unmutes, as the volume keys do.
    QCOMPARE(rig.switches->step(kVolume, 1), Outcome::Started);
    QCOMPARE(rig.state(kVolume).on, Value::On);
    QTRY_VERIFY(!rig.state(kVolume).busy);
    QCOMPARE(rig.state(kVolume).level, qint8 {25});
    QCOMPARE(rig.state(kVolume).on, Value::On);
    QCOMPARE(rig.system->writesOf(kVolume),
        (std::vector<SwitchIntent> {intent(Kind::LevelOnly, 20), intent(Kind::SetLevel, 25)}));
}

void SwitchesTest::stepsCoalesce()
{
    Rig rig({{kVolume, volume(20, false)}}, {}, false, {kVolume});
    QTRY_VERIFY(rig.state(kVolume).ready());
    QList<SwitchState> seen;
    connect(rig.switches.get(), &Switches::changed, this, [&](const QString& id) { seen.append(rig.state(id)); });
    rig.system->writeGate.shut();
    // Alt+→ three times, the first still being written: each counts from the
    // level shown, the newest goes next in place of the one before.
    QCOMPARE(rig.switches->step(kVolume, 1), Outcome::Started); // 25, held
    QTRY_VERIFY(rig.system->writeGate.held());
    QCOMPARE(rig.switches->step(kVolume, 1), Outcome::Started); // 30, queued
    QCOMPARE(rig.state(kVolume).level, qint8 {30});
    QCOMPARE(rig.switches->step(kVolume, 1), Outcome::Started); // 35 in its place
    QCOMPARE(rig.state(kVolume).level, qint8 {35});
    QVERIFY(rig.state(kVolume).busy);
    rig.system->writeGate.open();
    QTRY_VERIFY(!rig.state(kVolume).busy);
    QCOMPARE(rig.state(kVolume).level, qint8 {35});
    QCOMPARE(rig.system->writesOf(kVolume),
        (std::vector<SwitchIntent> {intent(Kind::SetLevel, 25), intent(Kind::SetLevel, 35)})); // two writes for three steps
    // Never back on the way: the overtaken 25 was not shown once it was done.
    int highest = 0;
    for (const SwitchState& state : seen) {
        QVERIFY(state.level >= highest);
        highest = state.level;
    }

    // And back down while one goes up: from the level being written.
    rig.system->writeGate.shut();
    QCOMPARE(rig.switches->step(kVolume, 1), Outcome::Started); // 40, held
    QTRY_VERIFY(rig.system->writeGate.held());
    QCOMPARE(rig.switches->step(kVolume, -1), Outcome::Started); // 35: queued
    QCOMPARE(rig.state(kVolume).level, qint8 {35});
    QCOMPARE(rig.switches->step(kVolume, 1), Outcome::Started); // 40 again: being written, the 35 taken back
    rig.system->writeGate.open();
    QTRY_VERIFY(!rig.state(kVolume).busy);
    QCOMPARE(rig.state(kVolume).level, qint8 {40});
    QCOMPARE(rig.system->writesOf(kVolume).size(), std::size_t {3});
}

void SwitchesTest::stepAtEnds()
{
    {
        Rig rig({{kVolume, volume(100, false)}}, {}, false, {kVolume});
        QTRY_VERIFY(rig.state(kVolume).ready());
        QCOMPARE(rig.switches->step(kVolume, 1), Outcome::Already);
        QCOMPARE(rig.switches->step(kVolume, -1), Outcome::Started);
        QTRY_VERIFY(!rig.state(kVolume).busy);
        QCOMPARE(rig.state(kVolume).level, qint8 {95});
    }
    {
        Rig rig({{kVolume, volume(0, false)}}, {}, false, {kVolume});
        QTRY_VERIFY(rig.state(kVolume).ready());
        QCOMPARE(rig.switches->step(kVolume, -1), Outcome::Already);
        QTest::qWait(50);
        QVERIFY(rig.system->writesOf(kVolume).empty());
    }
    {
        // At 100 and muted, up is still something: the sound comes back.
        Rig rig({{kVolume, volume(100, true)}}, {}, false, {kVolume});
        QTRY_VERIFY(rig.state(kVolume).ready());
        QCOMPARE(rig.switches->step(kVolume, 1), Outcome::Started);
        QTRY_VERIFY(!rig.state(kVolume).busy);
        QCOMPARE(rig.state(kVolume).on, Value::On);
        QCOMPARE(rig.state(kVolume).level, qint8 {100});
    }
}

void SwitchesTest::stepBeforeRead()
{
    Rig rig({{kVolume, volume(24, false)}}, {}, true, {kVolume});
    // Alt+→ before the level is known: nothing to step from.
    QCOMPARE(rig.switches->step(kVolume, 1), Outcome::NotRead);
    // A typed step goes to the backend as it is, which reads before it writes.
    QCOMPARE(rig.switches->apply(kVolume, intent(Kind::StepLevel, 10)), Outcome::Started);
    rig.system->readGate.open();
    QTRY_VERIFY(rig.state(kVolume).ready() && !rig.state(kVolume).busy);
    QCOMPARE(rig.state(kVolume).level, qint8 {34});
    QCOMPARE(rig.system->writesOf(kVolume), std::vector<SwitchIntent> {intent(Kind::StepLevel, 10)});
}

void SwitchesTest::stepNotReady()
{
    // No output device; and a switch without a level.
    Rig rig({{kVolume, SwitchAudio::noDevice()}, {kTheme, ready(Value::Off)}}, {}, false, {kVolume, kTheme});
    QTRY_COMPARE(rig.state(kVolume).availability, Availability::NoDevice);
    QTRY_VERIFY(rig.state(kTheme).ready());
    QCOMPARE(rig.state(kVolume).note, u"没有声音输出设备"_s);
    QCOMPARE(rig.switches->step(kVolume, 1), Outcome::OpenPage);
    QCOMPARE(rig.switches->apply(kVolume, intent(Kind::SetLevel, 30)), Outcome::OpenPage);
    QCOMPARE(rig.switches->step(kTheme, 1), Outcome::OpenPage);
    QCOMPARE(rig.switches->step(u"bluetooth"_s, 1), Outcome::OpenPage); // no backend here
    QTest::qWait(50);
    QCOMPARE(rig.system->writeCount(), std::size_t {0});
}

void SwitchesTest::audioTargets()
{
    using Target = SwitchAudio::Target;
    // The flyout's number.
    QCOMPARE(SwitchAudio::levelOf(0.0f), 0);
    QCOMPARE(SwitchAudio::levelOf(0.24f), 24);
    QCOMPARE(SwitchAudio::levelOf(0.2449f), 24);
    QCOMPARE(SwitchAudio::levelOf(0.2451f), 25);
    QCOMPARE(SwitchAudio::levelOf(1.0f), 100);
    QCOMPARE(SwitchAudio::levelOf(1.5f), 100);
    QCOMPARE(SwitchAudio::levelOf(-0.1f), 0);

    // From 24, muted.
    QCOMPARE(SwitchAudio::targetOf(24, true, intent(Kind::On)), (Target {.level = 24, .muted = false}));
    QCOMPARE(SwitchAudio::targetOf(24, false, intent(Kind::Off)), (Target {.level = 24, .muted = true}));
    QCOMPARE(SwitchAudio::targetOf(24, true, intent(Kind::SetLevel, 20)), (Target {.level = 20, .muted = false}));
    QCOMPARE(SwitchAudio::targetOf(24, true, intent(Kind::SetLevel, 150)), (Target {.level = 100, .muted = false}));
    QCOMPARE(SwitchAudio::targetOf(24, true, intent(Kind::LevelOnly, 20)), (Target {.level = 20, .muted = true}));
    QCOMPARE(SwitchAudio::targetOf(24, false, intent(Kind::LevelOnly, 120)), (Target {.level = 100, .muted = false}));
    QCOMPARE(SwitchAudio::targetOf(24, true, intent(Kind::StepLevel, 10)), (Target {.level = 34, .muted = false}));
    QCOMPARE(SwitchAudio::targetOf(24, true, intent(Kind::StepLevel, -5)), (Target {.level = 19, .muted = true}));
    QCOMPARE(SwitchAudio::targetOf(24, false, intent(Kind::StepLevel, -50)), (Target {.level = 0, .muted = false}));
    QVERIFY(!SwitchAudio::targetOf(24, false, intent(Kind::Toggle))); // Switches resolves it first
    QVERIFY(!SwitchAudio::targetOf(24, false, intent(Kind::OpenSettings)));

    const SwitchState muted = SwitchAudio::stateOf(24, true, u"扬声器 (3- Realtek(R) Audio)"_s);
    QCOMPARE(muted.availability, Availability::Ready);
    QCOMPARE(muted.on, Value::Off);
    QCOMPARE(muted.level, qint8 {24});
    QCOMPARE(muted.note, u"扬声器 (3- Realtek(R) Audio)"_s);
    QCOMPARE(SwitchAudio::stateOf(24, false, {}).on, Value::On);

    const SwitchState none = SwitchAudio::noDevice();
    QCOMPARE(none.availability, Availability::NoDevice);
    QCOMPARE(none.note, u"没有声音输出设备"_s);
    QVERIFY(none.openInstead.isEmpty()); // the row's own page: ms-settings:sound
}

void SwitchesTest::radioStates()
{
    using Access = SwitchRadios::Access;
    using Power = SwitchRadios::Power;
    const QString bluetooth = SwitchRadios::kBluetooth.toString();
    const QString wlan = SwitchRadios::kWlan.toString();
    const auto of = [](const QString& id, Access access, std::vector<Power> radios, bool connected = false,
                        bool internet = false) {
        return SwitchRadios::stateOf(id, access, radios, connected, internet);
    };

    const SwitchState off = of(bluetooth, Access::Allowed, {Power::Off});
    QCOMPARE(off.availability, Availability::Ready);
    QCOMPARE(off.on, Value::Off);
    QVERIFY(off.note.isEmpty());
    QCOMPARE(off.level, qint8 {-1});
    // Several of a kind: on when any is on; one held by a hardware switch
    // counts for nothing while another is there.
    QCOMPARE(of(bluetooth, Access::Allowed, {Power::Off, Power::On}).on, Value::On);
    QCOMPARE(of(bluetooth, Access::Allowed, {Power::Disabled, Power::Off}).on, Value::Off);
    QCOMPARE(of(bluetooth, Access::Allowed, {Power::Unknown}).on, Value::Unknown);
    // Access not known (the request failed): tried, not refused.
    QCOMPARE(of(bluetooth, Access::Unspecified, {Power::On}).availability, Availability::Ready);

    const SwitchState hardware = of(bluetooth, Access::Allowed, {Power::Disabled});
    QCOMPARE(hardware.availability, Availability::Blocked);
    QCOMPARE(hardware.note, u"被硬件开关关闭"_s);
    const SwitchState none = of(bluetooth, Access::Allowed, {});
    QCOMPARE(none.availability, Availability::NoDevice);
    QCOMPARE(none.note, u"这台电脑没有蓝牙"_s);
    QCOMPARE(of(wlan, Access::Allowed, {}).note, u"这台电脑没有 WLAN"_s);

    // "让应用控制设备无线电" off: Enter opens that page; ahead of everything else.
    const SwitchState denied = of(bluetooth, Access::DeniedByUser, {Power::On});
    QCOMPARE(denied.availability, Availability::Denied);
    QCOMPARE(denied.openInstead, u"ms-settings:privacy-radios"_s);
    QCOMPARE(denied.note, u"“让应用控制设备无线电”已关闭"_s);
    QCOMPARE(of(wlan, Access::DeniedByUser, {}).availability, Availability::Denied);
    const SwitchState policy = of(wlan, Access::DeniedBySystem, {Power::On});
    QCOMPARE(policy.availability, Availability::Blocked);
    QVERIFY(policy.openInstead.isEmpty());
    QVERIFY(!policy.note.isEmpty());

    // WLAN on: whether it is on a network, and whether the internet goes
    // through it (the footer's "会断网"). Off: neither.
    const SwitchState online = of(wlan, Access::Allowed, {Power::On}, true, true);
    QCOMPARE(online.on, Value::On);
    QCOMPARE(online.note, u"已连接"_s);
    QVERIFY(online.carriesInternet);
    const SwitchState idle = of(wlan, Access::Allowed, {Power::On}, false, false);
    QCOMPARE(idle.note, u"未连接"_s);
    QVERIFY(!idle.carriesInternet);
    const SwitchState wlanOff = of(wlan, Access::Allowed, {Power::Off}, true, true);
    QVERIFY(wlanOff.note.isEmpty());
    QVERIFY(!wlanOff.carriesInternet);
    const SwitchState bluetoothOn = of(bluetooth, Access::Allowed, {Power::On}, true, true);
    QVERIFY(bluetoothOn.note.isEmpty()); // no such line for Bluetooth
    QVERIFY(!bluetoothOn.carriesInternet);

    // Through Switches: a radio turned off by its hardware switch opens the page.
    Rig rig({{bluetooth, hardware}, {wlan, online}}, {}, false, {bluetooth, wlan});
    QTRY_COMPARE(rig.state(bluetooth).availability, Availability::Blocked);
    QTRY_VERIFY(rig.state(wlan).ready());
    QCOMPARE(rig.switches->apply(bluetooth, intent(Kind::Toggle)), Outcome::OpenPage);
    QVERIFY(rig.state(wlan).carriesInternet);
    QCOMPARE(rig.switches->resolve(wlan, intent(Kind::Toggle)), intent(Kind::Off));
}

void SwitchesTest::relayStopsAtClose()
{
    const auto relay = std::make_shared<SwitchRelay>();
    std::atomic<int> told = 0;
    (*relay)(kVolume); // before watch() (wsbench): nobody to tell
    relay->watch([&told](const QString& id) {
        if (id == kVolume)
            ++told;
    });
    std::thread([relay] { (*relay)(kVolume); }).join(); // a callback, on a thread of its own
    QCOMPARE(told.load(), 1);
    relay->close();
    std::thread([relay] { (*relay)(kVolume); }).join(); // one that comes later: dropped
    QCOMPARE(told.load(), 1);
}

void SwitchesTest::stopTokenGiven()
{
    std::shared_ptr<FakeSystem> system;
    {
        Rig rig({{kTheme, ready(Value::Off)}});
        system = rig.system;
        QTRY_VERIFY(rig.state().ready());
        std::lock_guard lock(system->mutex);
        QVERIFY(system->stop.has_value());
        QVERIFY(system->stopBeforeWatch);
        QVERIFY(system->stop->stop_possible());
        QVERIFY(!system->stop->stop_requested());
    }
    // As the switch thread ends: a write waiting on Windows gives up.
    std::lock_guard lock(system->mutex);
    QVERIFY(system->stop->stop_requested());
}

void SwitchesTest::keepAwakeEndTimes()
{
    const QDateTime now = at(19, 15, 0, 20, 500);
    // A duration: counted from now, to the second; within a minute and 99 hours.
    QCOMPARE(SwitchKeepAwake::endOf(intent(Kind::AwakeFor, 90), now), at(19, 16, 30, 20));
    QCOMPARE(SwitchKeepAwake::endOf(intent(Kind::AwakeFor, 0), now), at(19, 15, 1, 20));
    QCOMPARE(SwitchKeepAwake::endOf(intent(Kind::AwakeFor, 100'000), now), now.addSecs(99 * 3600).addMSecs(-500));
    QCOMPARE(SwitchKeepAwake::endOf(intent(Kind::AwakeFor, 24 * 60), now), at(20, 15, 0, 20));
    // A time of day: today's if it is ahead, else tomorrow's (past midnight too).
    QCOMPARE(SwitchKeepAwake::endOf(intent(Kind::AwakeUntil, 18 * 60), now), at(19, 18, 0));
    QCOMPARE(SwitchKeepAwake::endOf(intent(Kind::AwakeUntil, 15 * 60), now), at(20, 15, 0)); // 15:00 has gone
    QCOMPARE(SwitchKeepAwake::endOf(intent(Kind::AwakeUntil, 15 * 60 + 1), now), at(19, 15, 1));
    QCOMPARE(SwitchKeepAwake::endOf(intent(Kind::AwakeUntil, 30), at(19, 23, 30)), at(20, 0, 30)); // after midnight
    QCOMPARE(SwitchKeepAwake::endOf(intent(Kind::AwakeUntil, 18 * 60), at(19, 18, 0)), at(20, 18, 0)); // now is not ahead
    QCOMPARE(SwitchKeepAwake::endOf(intent(Kind::AwakeUntil, 0), at(19, 23, 59, 59)), at(20, 0, 0));
    for (const Kind other : {Kind::Toggle, Kind::On, Kind::Off, Kind::SetLevel, Kind::OpenSettings})
        QVERIFY(!SwitchKeepAwake::endOf(intent(other, 5), now).isValid());
    // The timer looks at the clock at the end, no sooner than a millisecond, and within what a QTimer takes.
    QCOMPARE(SwitchKeepAwake::msUntil(at(19, 15, 1), at(19, 15, 0)), 60'000);
    QCOMPARE(SwitchKeepAwake::msUntil(at(19, 15, 0), at(19, 15, 5)), 1);
    QCOMPARE(SwitchKeepAwake::msUntil(at(19, 15, 0).addDays(400), at(19, 15, 0)), 2'000'000'000);
}

void SwitchesTest::keepAwakeNotes()
{
    // Without a translator the words are the source's.
    QCOMPARE(SwitchKeepAwake::noteOf({}, at(19, 16, 0)), u"直到手动结束或退出 Win顺"_s);
    QCOMPARE(SwitchKeepAwake::noteOf(at(19, 16, 42), at(19, 16, 0, 0)), u"到 16:42 · 还剩 42 分钟"_s);
    QCOMPARE(SwitchKeepAwake::noteOf(at(19, 16, 42), at(19, 16, 0, 30)), u"到 16:42 · 还剩 42 分钟"_s); // 41.5, rounded up
    QCOMPARE(SwitchKeepAwake::noteOf(at(19, 16, 42), at(19, 16, 41, 59)), u"到 16:42 · 还剩 1 分钟"_s);
    QCOMPARE(SwitchKeepAwake::noteOf(at(19, 17, 30), at(19, 16, 0)), u"到 17:30 · 还剩 1 小时 30 分钟"_s);
    QCOMPARE(SwitchKeepAwake::noteOf(at(19, 18, 0), at(19, 16, 0)), u"到 18:00 · 还剩 2 小时"_s);
    QCOMPARE(SwitchKeepAwake::noteOf(at(19, 16, 42), at(19, 16, 42)), u"到 16:42"_s); // at the end
}

void SwitchesTest::keepAwakeHolds()
{
    FakeClock clock;
    SwitchKeepAwake awake(clock.clock());
    QCOMPARE(awake.ids(), QStringList {kAwake});

    SwitchState state = awake.read(kAwake);
    QVERIFY(state.ready());
    QCOMPARE(state.on, Value::Off);
    QVERIFY(!state.until.isValid());
    QVERIFY(!state.note.isEmpty()); // that it keeps the computer awake, and the lid
    QVERIFY(!awake.held());

    // A duration: the request is held, with the system and the display required.
    SwitchWrite w = awake.write(kAwake, intent(Kind::AwakeFor, 5));
    QVERIFY(w.ok && w.wrote);
    QCOMPARE(w.after.on, Value::On);
    QCOMPARE(w.after.until, clock.now().addSecs(300));
    QCOMPARE(w.after.note, SwitchKeepAwake::noteOf(w.after.until, clock.now()));
    QVERIFY(awake.held());
    QCOMPARE(executionState() & 3, 3UL);

    // The same again: nothing is written; another end time moves it.
    w = awake.write(kAwake, intent(Kind::AwakeFor, 5));
    QVERIFY(w.ok && !w.wrote);
    QCOMPARE(w.after.until, clock.now().addSecs(300));
    w = awake.write(kAwake, intent(Kind::AwakeFor, 10));
    QVERIFY(w.ok && w.wrote);
    QCOMPARE(w.after.until, clock.now().addSecs(600));
    w = awake.write(kAwake, intent(Kind::AwakeUntil, 18 * 60));
    QVERIFY(w.ok && w.wrote);
    QCOMPARE(w.after.until, at(19, 18, 0));
    w = awake.write(kAwake, intent(Kind::AwakeUntil, 18 * 60));
    QVERIFY(w.ok && !w.wrote);
    // On keeps the end time it finds.
    w = awake.write(kAwake, intent(Kind::On));
    QVERIFY(w.ok && !w.wrote);
    QCOMPARE(w.after.until, at(19, 18, 0));
    QCOMPARE(awake.read(kAwake).on, Value::On);

    // Off lets go; again, nothing to do.
    w = awake.write(kAwake, intent(Kind::Off));
    QVERIFY(w.ok && w.wrote);
    QCOMPARE(w.after.on, Value::Off);
    QVERIFY(!w.after.until.isValid());
    QVERIFY(!awake.held());
    w = awake.write(kAwake, intent(Kind::Off));
    QVERIFY(w.ok && !w.wrote);

    // On, with no end time: until it is ended or Win顺 exits.
    w = awake.write(kAwake, intent(Kind::On));
    QVERIFY(w.ok && w.wrote);
    QCOMPARE(w.after.on, Value::On);
    QVERIFY(!w.after.until.isValid());
    QCOMPARE(w.after.note, SwitchKeepAwake::noteOf({}, clock.now()));
    QVERIFY(awake.held());
    clock.advance(30 * 24 * 60); // a month: nothing ends it
    QCOMPARE(awake.read(kAwake).on, Value::On);

    // Not for keep awake: refused, nothing changed.
    w = awake.write(kAwake, intent(Kind::SetLevel, 30));
    QVERIFY(!w.ok && !w.wrote);
    QVERIFY(awake.held());

    // Win顺 exits: the request goes (the switch thread's close()).
    awake.close();
    QVERIFY(!awake.held());
    QCOMPARE(awake.read(kAwake).on, Value::Off);
}

void SwitchesTest::keepAwakeEndsByTheClock()
{
    FakeClock clock;
    SwitchKeepAwake awake(clock.clock());
    QVERIFY(awake.write(kAwake, intent(Kind::AwakeFor, 5)).wrote);
    clock.advance(4);
    SwitchState state = awake.read(kAwake);
    QCOMPARE(state.on, Value::On);
    QCOMPARE(state.note, SwitchKeepAwake::noteOf(state.until, clock.now())); // 1 minute left
    clock.advance(1); // exactly the end
    state = awake.read(kAwake);
    QCOMPARE(state.on, Value::Off);
    QVERIFY(!state.until.isValid());
    QVERIFY(!awake.held());

    // The computer slept through the end time (the clock jumps): the next read ends it.
    QVERIFY(awake.write(kAwake, intent(Kind::AwakeFor, 5)).wrote);
    clock.advance(600);
    QVERIFY(awake.held()); // nobody has looked yet
    QCOMPARE(awake.read(kAwake).on, Value::Off);
    QVERIFY(!awake.held());

    // A write after the end finds it ended: Off has nothing to do; a new time starts again.
    QVERIFY(awake.write(kAwake, intent(Kind::AwakeFor, 60)).wrote);
    clock.advance(120);
    SwitchWrite w = awake.write(kAwake, intent(Kind::Off));
    QVERIFY(w.ok && !w.wrote);
    w = awake.write(kAwake, intent(Kind::AwakeFor, 5));
    QVERIFY(w.ok && w.wrote);
    QVERIFY(awake.held());
    // A time of day: the next one.
    w = awake.write(kAwake, intent(Kind::AwakeUntil, 16 * 60));
    QVERIFY(w.ok);
    QVERIFY(w.after.until > clock.now());
    QVERIFY(w.after.until <= clock.now().addDays(1));
    QCOMPARE(w.after.until.time(), QTime(16, 0));
}

void SwitchesTest::keepAwakeTimer()
{
    FakeClock clock;
    SwitchKeepAwake awake(clock.clock());
    std::vector<QString> told;
    awake.watch([&told](const QString& id) { told.push_back(id); });
    QVERIFY(!awake.timerActive());

    // Armed once written; not for a request with no end time; off again after Off.
    QVERIFY(awake.write(kAwake, intent(Kind::AwakeFor, 5)).wrote);
    awake.written(kAwake);
    QVERIFY(awake.timerActive());
    awake.checkEnd(); // not yet: the timer goes on
    QVERIFY(told.empty());
    QVERIFY(awake.held());
    QVERIFY(awake.timerActive());

    clock.advance(5);
    awake.checkEnd(); // the end: let go, and Switches is told
    QCOMPARE(told, (std::vector<QString> {kAwake}));
    QVERIFY(!awake.held());
    QVERIFY(!awake.timerActive());
    awake.checkEnd(); // nothing more to end
    QCOMPARE(told.size(), std::size_t {1});

    QVERIFY(awake.write(kAwake, intent(Kind::On)).wrote);
    awake.written(kAwake);
    QVERIFY(!awake.timerActive());
    QVERIFY(awake.write(kAwake, intent(Kind::AwakeFor, 5)).wrote);
    awake.written(kAwake);
    QVERIFY(awake.timerActive());
    QVERIFY(awake.write(kAwake, intent(Kind::Off)).wrote);
    awake.written(kAwake);
    QVERIFY(!awake.timerActive());
}

void SwitchesTest::keepAwakeThroughSwitches()
{
    FakeClock clock;
    auto backend = std::make_unique<SwitchKeepAwake>(clock.clock());
    SwitchKeepAwake* awake = backend.get();
    SwitchBackends backends;
    backends.push_back(std::move(backend));
    Switches switches(std::move(backends));
    QTRY_VERIFY(switches.state(kAwake).ready());
    QCOMPARE(switches.state(kAwake).on, Value::Off);

    // A typed duration: the row is on at once, then read back with its end time.
    QCOMPARE(switches.apply(kAwake, intent(Kind::AwakeFor, 30)), Outcome::Started);
    QVERIFY(switches.state(kAwake).busy);
    QCOMPARE(switches.state(kAwake).on, Value::On);
    QTRY_VERIFY(!switches.state(kAwake).busy);
    QCOMPARE(switches.state(kAwake).until, clock.now().addSecs(30 * 60));
    QVERIFY(awake->held());
    QVERIFY(awake->timerActive()); // written() ran on this thread

    // Enter on the row while it is on: the other state, Off.
    QCOMPARE(switches.resolve(kAwake, intent(Kind::Toggle)), intent(Kind::Off));
    QCOMPARE(switches.apply(kAwake, intent(Kind::Toggle)), Outcome::Started);
    QTRY_COMPARE(switches.state(kAwake).on, Value::Off);
    QTRY_VERIFY(!switches.state(kAwake).busy);
    QVERIFY(!awake->held());
    QVERIFY(!awake->timerActive());

    QCOMPARE(switches.apply(kAwake, intent(Kind::On)), Outcome::Started);
    QTRY_VERIFY(!switches.state(kAwake).busy);
    QCOMPARE(switches.state(kAwake).on, Value::On);
    QVERIFY(!switches.state(kAwake).until.isValid());
    QCOMPARE(switches.apply(kAwake, intent(Kind::On)), Outcome::Already);
    // A time asked while it is on is not "already": it sets the end time.
    QCOMPARE(switches.apply(kAwake, intent(Kind::AwakeUntil, 18 * 60)), Outcome::Started);
    QTRY_COMPARE(switches.state(kAwake).until, at(19, 18, 0));

    // The computer resumed past the end time: refresh() reads again and it has ended.
    QCOMPARE(switches.apply(kAwake, intent(Kind::AwakeFor, 5)), Outcome::Started);
    QTRY_COMPARE(switches.state(kAwake).until, clock.now().addSecs(300));
    clock.advance(600);
    QCOMPARE(switches.state(kAwake).on, Value::On); // the cache has not been asked
    switches.refresh();
    QTRY_COMPARE(switches.state(kAwake).on, Value::Off);
    QVERIFY(!awake->held());

    // The end time comes while nobody looks: the timer's check tells Switches, which reads it again.
    QCOMPARE(switches.apply(kAwake, intent(Kind::AwakeFor, 5)), Outcome::Started);
    QTRY_VERIFY(!switches.state(kAwake).busy);
    QCOMPARE(switches.state(kAwake).on, Value::On);
    QSignalSpy changed(&switches, &Switches::changed);
    clock.advance(5);
    awake->checkEnd();
    QTRY_COMPARE(switches.state(kAwake).on, Value::Off);
    QVERIFY(!changed.isEmpty());
}

// #503: closing the lid does nothing while held; what it did comes back,
// where it still does nothing.
void SwitchesTest::lidHeldAndPutBack()
{
    using Lid = LidOverride::Outcome;
    QTemporaryDir dir;
    const QString record = dir.filePath(u"lid-close.json"_s);
    const QUuid balanced = QUuid::createUuid();
    const QUuid saver = QUuid::createUuid();
    auto plans = std::make_shared<FakePlans::Data>();
    plans->schemes = {{balanced, {1, 1}}, {saver, {2, 1}}};
    plans->active = balanced;
    {
        const std::unique_ptr<LidOverride> lid = lidOver(plans, record);
        QVERIFY(lid->lidPresent());
        QVERIFY(!lid->held());
        QCOMPARE(lid->hold(), Lid::Held);
        QVERIFY(lid->held());
        QCOMPARE(plans->schemes[balanced], (LidActions {0, 0}));
        QCOMPARE(plans->applied, 1); // counts at once
        QVERIFY(QFileInfo::exists(record)); // written before it was changed
        QCOMPARE(lid->hold(), Lid::Held); // already: nothing written
        QCOMPARE(plans->writes, 1);
        lid->release();
        QVERIFY(!lid->held());
        QCOMPARE(plans->schemes[balanced], (LidActions {1, 1}));
        QVERIFY(!QFileInfo::exists(record));
        lid->release(); // nothing more
        QCOMPARE(plans->writes, 2);

        // Changed by the user meanwhile: theirs stays.
        QCOMPARE(lid->hold(), Lid::Held);
        plans->schemes[balanced].dc = 2;
        lid->release();
        QCOMPARE(plans->schemes[balanced], (LidActions {1, 2}));

        // Another plan made active: the old one gets its actions back, the
        // new one is held; again, nothing happens.
        QCOMPARE(lid->hold(), Lid::Held);
        plans->active = saver;
        QCOMPARE(lid->follow(), Lid::Held);
        QCOMPARE(plans->schemes[balanced], (LidActions {1, 2}));
        QCOMPARE(plans->schemes[saver], (LidActions {0, 0}));
        const int writes = plans->writes;
        QCOMPARE(lid->follow(), Lid::Held);
        QCOMPARE(plans->writes, writes);
        lid->release();
        QCOMPARE(plans->schemes[saver], (LidActions {2, 1}));

        // Doing nothing already when plugged in: only the battery's changes.
        plans->schemes[saver] = {0, 1};
        QCOMPARE(lid->hold(), Lid::Held);
        QCOMPARE(plans->schemes[saver], (LidActions {0, 0}));
        lid->release();
        QCOMPARE(plans->schemes[saver], (LidActions {0, 1}));
        // Nothing to change: held all the same, nothing written or recorded.
        plans->schemes[saver] = {0, 0};
        const int before = plans->writes;
        QCOMPARE(lid->hold(), Lid::Held);
        QVERIFY(lid->held());
        QVERIFY(!QFileInfo::exists(record));
        lid->release();
        QCOMPARE(plans->writes, before);

        // Refused, or half written: nothing is left changed.
        plans->schemes[saver] = {1, 1};
        plans->refuse = true;
        QCOMPARE(lid->hold(), Lid::Failed);
        QVERIFY(!lid->held());
        plans->refuse = false;
        plans->halfOnce = true;
        QCOMPARE(lid->hold(), Lid::Failed);
        QVERIFY(!lid->held());
        QCOMPARE(plans->schemes[saver], (LidActions {1, 1}));
        QVERIFY(!QFileInfo::exists(record));
        // Set by a group policy: left alone.
        plans->policy = true;
        QCOMPARE(lid->hold(), Lid::ByPolicy);
        QVERIFY(!lid->held());
        plans->policy = false;
        // Held as it goes: let go.
        QCOMPARE(lid->hold(), Lid::Held);
        QCOMPARE(plans->schemes[saver], (LidActions {0, 0}));
    }
    QCOMPARE(plans->schemes[saver], (LidActions {1, 1}));
    QVERIFY(!QFileInfo::exists(record));

    // No lid (a desktop): nothing to hold.
    plans->lid = false;
    const std::unique_ptr<LidOverride> lid = lidOver(plans, record);
    QVERIFY(!lid->lidPresent());
    QCOMPARE(lid->hold(), Lid::NoLid);
    QVERIFY(!lid->held());
    QCOMPARE(plans->schemes[saver], (LidActions {1, 1}));
}

// Win顺 ended while it held the lid (a crash, the power cut): the next start
// puts it back from the record.
void SwitchesTest::lidAfterACrash()
{
    QTemporaryDir dir;
    const QString record = dir.filePath(u"plugin-data/lid-close.json"_s); // made when first needed
    const QString left = dir.filePath(u"left.json"_s);
    const QUuid scheme = QUuid::createUuid();
    auto plans = std::make_shared<FakePlans::Data>();
    plans->schemes = {{scheme, {1, 3}}};
    plans->active = scheme;
    // What a crash leaves: the actions set to nothing, and the record.
    const auto crash = [&] {
        const std::unique_ptr<LidOverride> lid = lidOver(plans, record);
        QCOMPARE(lid->hold(), LidOverride::Outcome::Held);
        QVERIFY(QFile::copy(record, left));
        lid->release();
        plans->schemes[scheme] = {0, 0};
        QVERIFY(QFile::rename(left, record));
    };

    crash();
    plans->active = QUuid::createUuid(); // another plan chosen since: put back all the same
    lidOver(plans, record);
    QCOMPARE(plans->schemes[scheme], (LidActions {1, 3}));
    QVERIFY(!QFileInfo::exists(record));

    // Changed by the user before the next start: theirs stays.
    plans->active = scheme;
    crash();
    plans->schemes[scheme].ac = 2;
    lidOver(plans, record);
    QCOMPARE(plans->schemes[scheme], (LidActions {2, 3}));
    QVERIFY(!QFileInfo::exists(record));

    // Not written yet: the record stays, for the start after.
    plans->schemes[scheme] = {1, 3};
    crash();
    plans->refuse = true;
    lidOver(plans, record);
    QVERIFY(QFileInfo::exists(record));
    plans->refuse = false;
    lidOver(plans, record);
    QCOMPARE(plans->schemes[scheme], (LidActions {1, 3}));
    QVERIFY(!QFileInfo::exists(record));

    // A record Win顺 did not write (or not whole) is dropped; nothing changes.
    const QString id = scheme.toString(QUuid::WithoutBraces);
    for (const QString& bad : {u"{\"scheme\": \"x\", \"ac\": 1}"_s, u"not json"_s,
             u"{\"scheme\": \"%1\", \"ac\": 7}"_s.arg(id), u"{\"scheme\": \"%1\"}"_s.arg(id)}) {
        plans->schemes[scheme] = {0, 0};
        QFile file(record);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(bad.toUtf8());
        file.close();
        lidOver(plans, record);
        QCOMPARE(plans->schemes[scheme], (LidActions {0, 0}));
        QVERIFY(!QFileInfo::exists(record));
    }
}

// Keep awake with the plugin's setting: the lid held with it, let go with it.
void SwitchesTest::keepAwakeHoldsTheLid()
{
    const QJsonObject lidOn {{SwitchKeepAwake::kLidSetting.toString(), true}};
    const QJsonObject lidOff {{SwitchKeepAwake::kLidSetting.toString(), false}};
    FakeClock clock;
    QTemporaryDir dir;
    const QUuid balanced = QUuid::createUuid();
    const QUuid saver = QUuid::createUuid();
    auto plans = std::make_shared<FakePlans::Data>();
    plans->schemes = {{balanced, {1, 1}}, {saver, {2, 2}}};
    plans->active = balanced;
    SwitchKeepAwake awake(clock.clock(), lidOver(plans, dir.filePath(u"lid-close.json"_s)));
    std::vector<QString> told;
    awake.watch([&told](const QString& id) { told.push_back(id); });
    QCOMPARE(awake.read(kAwake).note, u"电脑不睡眠、屏幕不关；合盖仍会睡眠"_s);

    // Setting off: the lid is left alone.
    QVERIFY(awake.write(kAwake, intent(Kind::On)).wrote);
    QVERIFY(!awake.lidHeld());
    QCOMPARE(plans->schemes[balanced], (LidActions {1, 1}));
    // Turned on while kept awake: held at once, and Switches is told.
    awake.configure(lidOn);
    QVERIFY(awake.lidHeld());
    QCOMPARE(plans->schemes[balanced], (LidActions {0, 0}));
    QCOMPARE(told, (std::vector<QString> {kAwake}));
    awake.configure(lidOn); // the same again: nothing
    QCOMPARE(told.size(), std::size_t {1});
    // Ended: both let go.
    QVERIFY(awake.write(kAwake, intent(Kind::Off)).wrote);
    QVERIFY(!awake.lidHeld());
    QCOMPARE(plans->schemes[balanced], (LidActions {1, 1}));
    QCOMPARE(awake.read(kAwake).note, u"电脑不睡眠、屏幕不关，合盖也不睡眠"_s);

    // Kept awake for a while: held with it, let go when its time comes.
    SwitchWrite w = awake.write(kAwake, intent(Kind::AwakeFor, 5));
    QVERIFY(w.ok);
    QVERIFY(w.after.error.isEmpty());
    QVERIFY(awake.lidHeld());
    clock.advance(5);
    QCOMPARE(awake.read(kAwake).on, Value::Off);
    QVERIFY(!awake.lidHeld());
    QCOMPARE(plans->schemes[balanced], (LidActions {1, 1}));

    // Another plan made active while held: the next read follows it.
    QVERIFY(awake.write(kAwake, intent(Kind::On)).wrote);
    plans->active = saver;
    QCOMPARE(awake.read(kAwake).on, Value::On);
    QCOMPARE(plans->schemes[balanced], (LidActions {1, 1}));
    QCOMPARE(plans->schemes[saver], (LidActions {0, 0}));
    // Turned off while kept awake: let go at once; kept awake all the same.
    awake.configure(lidOff);
    QVERIFY(!awake.lidHeld());
    QVERIFY(awake.held());
    QCOMPARE(plans->schemes[saver], (LidActions {2, 2}));
    QCOMPARE(told.size(), std::size_t {2});

    // Set by a group policy, or refused: kept awake, and the row says why
    // the lid still sleeps (in red); asked again once it may, held.
    plans->policy = true;
    awake.configure(lidOn);
    SwitchState state = awake.read(kAwake);
    QCOMPARE(state.on, Value::On);
    QCOMPARE(state.error, u"合盖仍会睡眠：合盖的动作由管理员统一设定"_s);
    plans->policy = false;
    plans->refuse = true;
    w = awake.write(kAwake, intent(Kind::AwakeFor, 30));
    QVERIFY(w.ok);
    QCOMPARE(w.after.error, u"合盖仍会睡眠：Windows 没让改合盖的动作"_s);
    plans->refuse = false;
    w = awake.write(kAwake, intent(Kind::On));
    QVERIFY(w.after.error.isEmpty());
    QVERIFY(awake.lidHeld());
    QCOMPARE(plans->schemes[saver], (LidActions {0, 0}));

    // Win顺 exits: both let go.
    awake.close();
    QVERIFY(!awake.held());
    QVERIFY(!awake.lidHeld());
    QCOMPARE(plans->schemes[saver], (LidActions {2, 2}));

    // No lid: the note does not speak of one, and nothing is held.
    plans->lid = false;
    SwitchKeepAwake desktop(clock.clock(), lidOver(plans, {}));
    desktop.configure(lidOn);
    QCOMPARE(desktop.read(kAwake).note, u"电脑不睡眠、屏幕不关"_s);
    w = desktop.write(kAwake, intent(Kind::On));
    QVERIFY(w.ok);
    QVERIFY(w.after.error.isEmpty());
    QVERIFY(!desktop.lidHeld());
    desktop.close();
}

// Windows' own power plans: read and written on a copy of the active plan,
// made for the test and deleted after it; the active plan is only read.
void SwitchesTest::lidOnWindowsPlans()
{
    const std::unique_ptr<PowerPlans> plans = PowerPlans::system();
    const QUuid active = plans->active();
    QVERIFY(!active.isNull());
    const std::optional<LidActions> was = plans->lid(active);
    QVERIFY(was);
    const GUID source = active;
    GUID* made = nullptr;
    QCOMPARE(::PowerDuplicateScheme(nullptr, &source, &made), DWORD(ERROR_SUCCESS));
    const QUuid copy(*made);
    ::LocalFree(made);
    const auto remove = qScopeGuard([&copy] {
        const GUID id = copy;
        ::PowerDeleteScheme(nullptr, &id);
    });
    QCOMPARE(*plans->lid(copy), *was);
    const LidActions other {was->ac == 2 ? 3u : 2u, was->dc == 3 ? 2u : 3u};
    QVERIFY(plans->setLid(copy, other));
    QCOMPARE(*plans->lid(copy), other);
    QCOMPARE(plans->active(), active); // not made active
    QCOMPARE(*plans->lid(active), *was);
    QVERIFY(!plans->lid(QUuid::createUuid())); // no such plan
}

} // namespace ws

QTEST_GUILESS_MAIN(ws::SwitchesTest)
#include "tst_switches.moc"
