#include "Switches.h"

#include <QDebug>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace ws {

namespace {

using Kind = SwitchIntent::Kind;
using Value = SwitchState::Value;

constexpr int kLevelStep = 5; // Alt+← →: the volume in steps of 5, as the volume keys

// Whether `state` already is what `intent` asks. On, Off and a level are
// known here; a step whose level was not known and a keep-awake time are
// left to the backend, which reads before it writes.
bool already(const SwitchState& state, const SwitchIntent& intent)
{
    switch (intent.kind) {
    case Kind::On:
        return state.on == Value::On;
    case Kind::Off:
        return state.on == Value::Off;
    case Kind::SetLevel: // and audible
        return state.level == intent.value && state.on == Value::On;
    case Kind::LevelOnly:
        return state.level == intent.value;
    default:
        return false;
    }
}

} // namespace

Switches::Switches(SwitchBackends backends, QObject* parent)
    : Switches(std::move(backends), Options {}, parent)
{
}

Switches::Switches(SwitchBackends backends, Options options, QObject* parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_backends(std::move(backends))
    , m_worker(std::make_unique<ComWorker>(ComWorker::Apartment::MultiThreaded, [this] {
        for (const std::unique_ptr<SwitchBackend>& backend : m_backends)
            backend->close();
    }))
{
    for (const std::unique_ptr<SwitchBackend>& backend : m_backends) {
        for (const QString& id : backend->ids()) {
            if (find(id)) {
                qWarning() << "Switches: two backends for" << id;
                continue;
            }
            m_entries.push_back(Entry {.id = id, .backend = backend.get()});
        }
    }
    // From any thread (a radio's callback, a registry watch here): read on
    // the GUI thread's turn, so that every change of the cache is made here.
    const SwitchBackend::Notify changed = [this](const QString& id) {
        QMetaObject::invokeMethod(this, [this, id] {
            if (Entry* e = find(id))
                requestRead(*e);
        }, Qt::QueuedConnection);
    };
    for (const std::unique_ptr<SwitchBackend>& backend : m_backends) {
        backend->setStopToken(m_worker->stopToken());
        backend->watch(changed);
    }
    for (Entry& e : m_entries)
        requestRead(e);
}

Switches::Switches(Remote remote, QObject* parent)
    : Switches(std::move(remote), Options {}, parent)
{
}

Switches::Switches(Remote remote, Options options, QObject* parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_remote(std::move(remote))
{
    for (const QString& id : m_remote->ids) {
        if (!find(id))
            m_entries.push_back(Entry {.id = id});
    }
    refresh();
}

Switches::~Switches() = default; // m_worker first: no task runs, nothing comes back, when the backends go

SwitchState Switches::state(QStringView id) const
{
    if (const Entry* e = find(id))
        return e->shown;
    return SwitchState {.availability = SwitchState::Availability::Unsupported};
}

SwitchIntent Switches::resolve(QStringView id, SwitchIntent intent) const
{
    const Entry* e = find(id);
    switch (intent.kind) {
    case Kind::Toggle:
        break;
    case Kind::SetLevel:
    case Kind::LevelOnly:
        intent.value = std::clamp(intent.value, 0, 100);
        return intent;
    case Kind::StepLevel:
        if (e && e->shown.level >= 0) {
            const int to = std::clamp(e->shown.level + intent.value, 0, 100);
            return SwitchIntent {.kind = intent.value > 0 ? Kind::SetLevel : Kind::LevelOnly, .value = to};
        }
        return intent; // the backend works it out from the level it reads
    default:
        return intent;
    }
    if (!e)
        return intent;
    switch (e->shown.on) {
    case Value::On:
        return SwitchIntent {.kind = Kind::Off};
    case Value::Off:
        return SwitchIntent {.kind = Kind::On};
    case Value::Mixed:
        return SwitchIntent {.kind = e->shown.mixedToggleOn ? Kind::On : Kind::Off};
    case Value::Unknown:
        break;
    }
    return intent;
}

Switches::Outcome Switches::apply(QStringView id, SwitchIntent intent)
{
    Entry* e = find(id);
    if (!e || intent.kind == Kind::OpenSettings)
        return Outcome::OpenPage;
    const SwitchState& shown = e->shown;
    const bool known = shown.availability != SwitchState::Availability::Unknown;
    if (known && !shown.ready())
        return Outcome::OpenPage;

    if (intent.kind == Kind::Toggle) {
        if (e->writing || e->waitToken != 0)
            return Outcome::Busy;
        intent = resolve(id, intent);
        if (intent.kind == Kind::Toggle) { // the state is not known yet: wait for it
            e->waitToken = ++m_serial;
            if (!e->reading)
                requestRead(*e);
            QTimer::singleShot(m_options.stateWait, this, [this, id = e->id, token = e->waitToken] {
                Entry* waiting = find(id);
                if (waiting && waiting->waitToken == token)
                    endWait(*waiting, Outcome::NotRead);
            });
            return Outcome::Waiting;
        }
    } else {
        if (e->waitToken != 0)
            endWait(*e, Outcome::Busy); // an explicit On, Off or level goes ahead of the toggle waiting
        intent = resolve(id, intent); // a level within 0–100, a step to the level it reaches
    }

    if (e->writing) {
        // One is being written: this one goes next, in place of any asked
        // before it; asking for what is being written takes that back.
        if (intent == e->queued.value_or(*e->writing))
            return Outcome::Busy;
        if (intent == *e->writing)
            e->queued.reset();
        else
            e->queued = intent;
        showTarget(*e, intent);
        return Outcome::Started;
    }
    if (known && already(shown, intent))
        return Outcome::Already;
    startWrite(*e, intent);
    return Outcome::Started;
}

Switches::Outcome Switches::step(QStringView id, int steps)
{
    const Entry* e = find(id);
    if (!e)
        return Outcome::OpenPage;
    if (e->shown.availability == SwitchState::Availability::Unknown)
        return Outcome::NotRead;
    if (!e->shown.ready() || e->shown.level < 0)
        return Outcome::OpenPage;
    // From the level shown: while one is written, its target, so that steps
    // taken quickly add up and the newest is written next.
    return apply(id, stepped(e->shown.level, steps));
}

SwitchIntent Switches::stepped(int level, int steps)
{
    level = std::clamp(level, 0, 100);
    steps = std::clamp(steps, -100 / kLevelStep, 100 / kLevelStep);
    int to = level;
    if (steps > 0)
        to = level / kLevelStep * kLevelStep + steps * kLevelStep;
    else if (steps < 0)
        to = (level + kLevelStep - 1) / kLevelStep * kLevelStep + steps * kLevelStep;
    return SwitchIntent {.kind = steps > 0 ? Kind::SetLevel : Kind::LevelOnly, .value = std::clamp(to, 0, 100)};
}

void Switches::refresh()
{
    for (const Entry& e : m_entries)
        refresh(e.id);
}

void Switches::refresh(QStringView id)
{
    if (Entry* e = find(id)) {
        if (!e->shown.error.isEmpty() && !e->writing && m_options.now() - e->errorAt >= m_options.errorShown) {
            SwitchState state = e->shown;
            state.error.clear();
            show(*e, state);
        }
        requestRead(*e);
    }
}

Switches::Entry* Switches::find(QStringView id)
{
    for (Entry& e : m_entries) {
        if (e.id == id)
            return &e;
    }
    return nullptr;
}

const Switches::Entry* Switches::find(QStringView id) const
{
    return const_cast<Switches*>(this)->find(id);
}

void Switches::requestRead(Entry& e)
{
    if (e.reading) {
        e.readAgain = true; // the one on its way may have read before this change
        return;
    }
    e.reading = true;
    if (m_remote) {
        const QPointer self(this);
        m_remote->read(e.id, [self, id = e.id](SwitchState now) {
            if (self)
                self->readDone(id, now);
        });
        return;
    }
    m_worker->post([this, backend = e.backend, id = e.id] {
        const SwitchState now = backend->read(id);
        QMetaObject::invokeMethod(this, [this, id, now] { readDone(id, now); }, Qt::QueuedConnection);
    });
}

void Switches::readDone(const QString& id, const SwitchState& now)
{
    Entry* e = find(id);
    if (!e)
        return;
    e->reading = false;
    // A change made elsewhere since the last read: a failure's words no
    // longer say how things are.
    const bool moved = now.on != e->confirmed.on || now.availability != e->confirmed.availability;
    e->confirmed = now;
    // While a write is under way the row stays at its target: a read from
    // before the write, or from halfway through it (dark mode's two values,
    // one written), only says where a failure would go back to.
    if (!e->writing) {
        SwitchState state = now;
        if (!moved)
            state.error = e->shown.error;
        show(*e, state);
    }
    if (std::exchange(e->readAgain, false))
        requestRead(*e);
    if (e->waitToken != 0 && now.availability != SwitchState::Availability::Unknown
        && (!now.ready() || now.on != Value::Unknown)) {
        e->waitToken = 0;
        const Outcome outcome = apply(id, SwitchIntent {.kind = Kind::Toggle});
        emit waitEnded(id, outcome);
    }
}

void Switches::startWrite(Entry& e, SwitchIntent intent)
{
    e.writing = intent;
    e.writingSerial = ++m_serial;
    showTarget(e, intent);
    if (m_remote) {
        const QPointer self(this);
        m_remote->write(e.id, intent, [self, id = e.id, serial = e.writingSerial](SwitchWrite result) {
            if (self)
                self->writeDone(id, serial, result);
        });
        return;
    }
    m_worker->post([this, backend = e.backend, id = e.id, serial = e.writingSerial, intent] {
        const SwitchWrite result = backend->write(id, intent);
        QMetaObject::invokeMethod(this, [this, id, serial, result] { writeDone(id, serial, result); },
            Qt::QueuedConnection);
    });
}

void Switches::writeDone(const QString& id, std::uint64_t serial, const SwitchWrite& result)
{
    Entry* e = find(id);
    if (!e || !e->writing || e->writingSerial != serial)
        return;
    e->writing.reset();
    if (result.after.availability != SwitchState::Availability::Unknown)
        e->confirmed = result.after;
    if (!result.ok)
        qInfo() << "Switches:" << id << "not set:" << result.error;

    const bool known = e->confirmed.availability != SwitchState::Availability::Unknown;
    if (e->queued) {
        // Overtaken while it was written: the newest goes next and the row
        // stays at its target; this result only says where things are. Not
        // when that is where it is now, or the switch is not ready any more.
        const SwitchIntent next = *std::exchange(e->queued, std::nullopt);
        if (!known || (e->confirmed.ready() && !already(e->confirmed, next))) {
            startWrite(*e, next);
            if (result.wrote)
                written(*e);
            return;
        }
    }
    SwitchState state = e->confirmed; // back where it is, if it failed
    // Not when it turned out not ready: its note says why.
    if (!result.ok && !result.error.isEmpty() && (!known || e->confirmed.ready())) {
        state.error = result.error;
        e->errorAt = m_options.now();
    }
    show(*e, state);
    if (result.wrote)
        written(*e); // after the row: dark mode's windows change with it here
}

void Switches::written(Entry& e)
{
    if (m_remote) {
        if (m_remote->written)
            m_remote->written(e.id);
    } else
        e.backend->written(e.id);
}

void Switches::show(Entry& e, const SwitchState& state)
{
    if (e.shown == state)
        return;
    e.shown = state;
    emit changed(e.id);
}

void Switches::showTarget(Entry& e, const SwitchIntent& intent)
{
    SwitchState state = e.shown;
    state.busy = true;
    state.error.clear();
    switch (intent.kind) {
    case Kind::On:
    case Kind::AwakeFor: // the end time comes with the read back
    case Kind::AwakeUntil:
        state.on = Value::On;
        break;
    case Kind::Off:
        state.on = Value::Off;
        break;
    case Kind::SetLevel: // and audible
        state.level = static_cast<qint8>(intent.value);
        state.on = Value::On;
        break;
    case Kind::LevelOnly:
        state.level = static_cast<qint8>(intent.value);
        break;
    default: // a step from a level not known yet: as read back
        break;
    }
    show(e, state);
}

void Switches::endWait(Entry& e, Outcome outcome)
{
    e.waitToken = 0;
    emit waitEnded(e.id, outcome);
}

} // namespace ws
