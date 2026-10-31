// The real provider service and the real host/runner, with fake hardware for
// writes. The production plugin smoke test changes only its own power request.
#include "Host.h"
#include "SwitchPlugin.h"
#include "SwitchWire.h"
#include "SwitchService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;
using namespace ws;
using namespace ws::plugin;
using Kind = SwitchIntent::Kind;
using Value = SwitchState::Value;
using Availability = SwitchState::Availability;

namespace {
const QString kId = switchwire::kPlugin.toString();

class FakeBackend final : public SwitchBackend {
public:
    FakeBackend()
    {
        for (const QString& id : switchwire::ids()) {
            SwitchState s;
            s.availability = Availability::Ready;
            s.on = Value::Off;
            if (id == u"volume") {
                s.level = 40;
                s.on = Value::On;
            }
            m_states.insert(id, s);
        }
    }
    QStringList ids() const override { return switchwire::ids(); }
    void watch(Notify notify) override { m_notify = std::move(notify); }
    SwitchState read(const QString& id) override
    {
        std::lock_guard lock(m_mutex);
        return m_states.value(id);
    }
    SwitchWrite write(const QString& id, const SwitchIntent& intent) override
    {
        // Keep one write in flight so the real UI cache must coalesce keys.
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        std::lock_guard lock(m_mutex);
        ++m_writes;
        SwitchState& state = m_states[id];
        if (intent.kind == Kind::SetLevel || intent.kind == Kind::LevelOnly) {
            state.level = static_cast<qint8>(std::clamp(intent.value, 0, 100));
            if (intent.kind == Kind::SetLevel)
                state.on = Value::On;
        } else
            state.on = intent.kind == Kind::Off ? Value::Off : Value::On;
        return SwitchWrite {true, true, {}, state};
    }
    void external(const QString& id, int on)
    {
        {
            std::lock_guard lock(m_mutex);
            m_states[id].on = static_cast<Value>(on);
        }
        if (m_notify)
            m_notify(id);
    }
    int writes()
    {
        std::lock_guard lock(m_mutex);
        return m_writes;
    }
private:
    QHash<QString, SwitchState> m_states;
    Notify m_notify;
    std::mutex m_mutex;
    int m_writes = 0;
};

int serve(QCoreApplication& app)
{
    QByteArray pending;
    const auto first = switchplugin::Protocol::first(pending);
    if (!first || first->value(u"method"_s) != u"initialize"_s)
        return 2;
    auto fake = std::make_unique<FakeBackend>();
    FakeBackend* hardware = fake.get();
    switchplugin::Service* service = nullptr;
    bool badRead = false;
    switchplugin::Protocol* channel = nullptr;
    switchplugin::Protocol protocol(std::move(pending),
        [&](const QJsonValue& id, const QString& method, const QJsonObject& params) {
            if (method == u"test/change") {
                hardware->external(params.value(u"id"_s).toString(), params.value(u"on"_s).toInt());
                channel->respond(id, QJsonObject {});
            } else if (method == u"test/writes")
                channel->respond(id, hardware->writes());
            else if (method == u"test/badRead") {
                badRead = params.value(u"enabled"_s).toBool();
                channel->respond(id, QJsonObject {});
            } else if (badRead && method == u"systemSwitches/read")
                channel->respond(id, QJsonObject {{u"availability"_s, 999}});
            else
                service->request(id, method, params);
        }, [&] { app.quit(); });
    channel = &protocol;
    SwitchBackends backends;
    backends.push_back(std::move(fake));
    switchplugin::Service provider(std::move(backends), protocol);
    service = &provider;
    protocol.respond(first->value(u"id"_s), QJsonObject {{u"protocolVersion"_s, u"1"_s}});
    return app.exec();
}

struct Fixture {
    QTemporaryDir dir;
    std::unique_ptr<Host> host;
    std::vector<HANDLE> processes;
    Fixture()
    {
        Host::Options options;
        options.userPluginDir = dir.path() + u"/plugins"_s;
        options.dataRoot = dir.path() + u"/data"_s;
        options.runnerExe = QString::fromUtf8(WINSHUN_PLUGIN_HOST);
        options.appDir = QString::fromUtf8(WINSHUN_QT_BIN);
        options.version = u"0.16.0"_s;
        host = std::make_unique<Host>(options);
        host->setBackoffScale(0.01);
        QObject::connect(host.get(), &Host::processStarted, [this](const QString&, quint32 pid) {
            if (HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, pid))
                processes.push_back(process);
        });
    }
    ~Fixture()
    {
        host->shutdown();
        for (const HANDLE process : processes) {
            ::WaitForSingleObject(process, 3000);
            ::CloseHandle(process);
        }
        host.reset(); // no watcher can retain the temporary plugin folder
    }
    bool install(bool native = false, const QString& id = kId)
    {
        const QString folder = dir.path() + u"/plugins/"_s + id;
        if (!QDir().mkpath(folder))
            return false;
        QFile original(QString::fromUtf8(WINSHUN_SWITCH_MANIFEST));
        if (!original.open(QIODevice::ReadOnly))
            return false;
        QJsonObject manifest = QJsonDocument::fromJson(original.readAll()).object();
        manifest.insert(u"id"_s, id);
        QJsonObject runtime = manifest.value(u"runtime"_s).toObject();
        if (native) {
            if (!QFile::copy(QString::fromUtf8(WINSHUN_SWITCH_EXE), folder + u"/WinShunSwitches.exe"_s)
                || !QFile::copy(QString::fromUtf8(WINSHUN_SWITCH_DLL), folder + u"/WinShunSwitchesApp.dll"_s))
                return false;
        } else {
            if (!QFile::copy(QCoreApplication::applicationFilePath(), folder + u"/provider.exe"_s))
                return false;
            runtime.insert(u"program"_s, u"provider.exe"_s);
            runtime.insert(u"args"_s, QJsonArray {u"--provider"_s});
            // An elevated host starts its runner with Explorer's environment,
            // so a Qt test double needs an explicit DLL search path. The real
            // plugin uses its native stub and WS_APP_DIR instead.
            runtime.insert(u"env"_s, QJsonObject {{u"PATH"_s,
                QString::fromUtf8(WINSHUN_QT_BIN) + u';' + qEnvironmentVariable("PATH")}});
        }
        manifest.insert(u"runtime"_s, runtime);
        QFile file(folder + u"/plugin.json"_s);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.write(QJsonDocument(manifest).toJson(QJsonDocument::Compact));
        file.close();
        host->catalog().scan();
        host->setEnabled(id, true);
        host->preload();
        return true;
    }
    bool running(const QString& id = kId)
    {
        const bool ready = QTest::qWaitFor([&] { return host->state(id).status == Host::State::Status::Running; }, 15000);
        if (!ready)
            qWarning() << "provider did not start" << id << host->state(id).error;
        return ready;
    }
    std::optional<QJsonValue> ask(const QString& method, const QJsonObject& params = {})
    {
        struct Reply { bool done = false; bool ok = false; QJsonValue value; };
        const auto reply = std::make_shared<Reply>();
        host->request(kId, method, params, [reply](bool ok, const QJsonValue& value) {
            reply->done = true;
            reply->ok = ok;
            reply->value = value;
        });
        if (!QTest::qWaitFor([&] { return reply->done; }, 20000) || !reply->ok)
            return std::nullopt;
        return reply->value;
    }
};

void refreshOnChange(SwitchPlugin& bridge, Switches& cache)
{
    QObject::connect(&bridge, &SwitchPlugin::changed, &cache, [&cache](const QString& id) {
        if (id.isEmpty())
            cache.refresh();
        else
            cache.refresh(id);
    });
}
} // namespace

class SwitchPluginTest : public QObject {
    Q_OBJECT
private slots:
    void wireValidation()
    {
        SwitchState state;
        state.availability = Availability::Ready;
        state.on = Value::Mixed;
        state.level = 42;
        state.note = u"扬声器"_s;
        state.until = QDateTime::currentDateTime().addSecs(600);
        state.openInstead = u"ms-settings:sound"_s;
        state.mixedToggleOn = true;
        state.carriesInternet = true;
        const auto restored = switchwire::state(switchwire::state(state));
        QVERIFY(restored);
        QCOMPARE(*restored, state);
        for (int kind = 0; kind <= int(Kind::LevelOnly); ++kind) {
            SwitchIntent intent {static_cast<Kind>(kind), 25};
            const auto round = switchwire::intent(switchwire::intent(intent));
            QVERIFY(round);
            QCOMPARE(*round, intent);
        }
        QJsonObject json = switchwire::state(state);
        for (const QJsonValue invalid : {QJsonValue(101), QJsonValue(u"50"_s), QJsonValue(1.5)}) {
            json.insert(u"level"_s, invalid);
            QVERIFY(!switchwire::state(json));
        }
        json = switchwire::state(state);
        json.insert(u"openInstead"_s, u"C:\\arbitrary.exe"_s);
        QVERIFY(!switchwire::state(json));
        json = switchwire::state(state);
        json.insert(u"mixedToggleOn"_s, 1);
        QVERIFY(!switchwire::state(json));
        QVERIFY(!switchwire::intent(QJsonObject {{u"kind"_s, u"invalid"_s}, {u"value"_s, 0}}));
        QVERIFY(!switchwire::write(QJsonObject {}));
    }

    void capabilityRequiresPermission()
    {
        QFile file(QString::fromUtf8(WINSHUN_SWITCH_MANIFEST));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject manifest = QJsonDocument::fromJson(file.readAll()).object();
        QString error;
        QVERIFY(Manifest::parse(QJsonDocument(manifest).toJson(), &error));
        manifest.insert(u"permissions"_s, QJsonArray {u"ui"_s});
        QVERIFY(!Manifest::parse(QJsonDocument(manifest).toJson(), &error));
        manifest.insert(u"permissions"_s, QJsonArray {u"ui"_s, u"system:settings"_s});
        QJsonObject runtime = manifest.value(u"runtime"_s).toObject();
        runtime.remove(u"start"_s); // a lazy provider cannot receive read requests
        manifest.insert(u"runtime"_s, runtime);
        QVERIFY(!Manifest::parse(QJsonDocument(manifest).toJson(), &error));
        runtime.insert(u"start"_s, u"eager"_s);
        runtime.insert(u"idleExitSec"_s, 1); // its power request must not expire on idle
        manifest.insert(u"runtime"_s, runtime);
        QVERIFY(!Manifest::parse(QJsonDocument(manifest).toJson(), &error));
        runtime.insert(u"idleExitSec"_s, 0);
        runtime.insert(u"mode"_s, u"once"_s);
        manifest.insert(u"runtime"_s, runtime);
        QVERIFY(!Manifest::parse(QJsonDocument(manifest).toJson(), &error));
    }

    void coalescedWritesAndExternalChanges()
    {
        Fixture f;
        QVERIFY(f.install());
        QVERIFY(f.running());
        SwitchPlugin bridge(f.host.get(), kId);
        Switches cache(bridge.remote());
        refreshOnChange(bridge, cache);
        QTRY_COMPARE(cache.state(u"volume").level, 40);
        QCOMPARE(cache.apply(u"volume", {Kind::SetLevel, 50}), Switches::Outcome::Started);
        QVERIFY(cache.state(u"volume").busy);
        QCOMPARE(cache.apply(u"volume", {Kind::SetLevel, 60}), Switches::Outcome::Started);
        QCOMPARE(cache.apply(u"volume", {Kind::SetLevel, 70}), Switches::Outcome::Started);
        QCOMPARE(cache.state(u"volume").level, 70);
        QTRY_VERIFY(!cache.state(u"volume").busy);
        QCOMPARE(cache.state(u"volume").level, 70);
        const auto writes = f.ask(u"test/writes"_s);
        QVERIFY(writes);
        QCOMPARE(writes->toInt(), 2);
        QVERIFY(f.ask(u"test/change"_s, {{u"id"_s, u"bluetooth"_s}, {u"on"_s, 1}}));
        QTRY_COMPARE(cache.state(u"bluetooth").on, Value::On);
        QCOMPARE(cache.apply(u"bluetooth", {}), Switches::Outcome::Started);
        QTRY_VERIFY(!cache.state(u"bluetooth").busy);
        QCOMPARE(cache.state(u"bluetooth").on, Value::Off);
        // A malformed provider answer becomes a usable Settings fallback.
        QVERIFY(f.ask(u"test/badRead"_s, {{u"enabled"_s, true}}));
        cache.refresh(u"bluetooth");
        QTRY_COMPARE(cache.state(u"bluetooth").availability, Availability::Blocked);
        QCOMPARE(cache.apply(u"bluetooth", {}), Switches::Outcome::OpenPage);
        QVERIFY(f.ask(u"test/badRead"_s, {{u"enabled"_s, false}}));
        cache.refresh(u"bluetooth");
        QTRY_COMPARE(cache.state(u"bluetooth").availability, Availability::Ready);
        // The provider rejects ambiguous writes; only the host resolves Toggle.
        QVERIFY(!f.ask(u"systemSwitches/write"_s,
            {{u"id"_s, u"bluetooth"_s}, {u"intent"_s, switchwire::intent({Kind::Toggle, 0})}}));
    }

    void crashDoesNotReplayWrite()
    {
        Fixture f;
        QVERIFY(f.install());
        QVERIFY(f.running());
        SwitchPlugin bridge(f.host.get(), kId);
        Switches cache(bridge.remote());
        refreshOnChange(bridge, cache);
        QTRY_COMPARE(cache.state(u"volume").level, 40);
        const quint32 pid = f.host->state(kId).pid;
        QCOMPARE(cache.apply(u"volume", {Kind::SetLevel, 90}), Switches::Outcome::Started);
        HANDLE process = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        QVERIFY(process);
        const BOOL ended = ::TerminateProcess(process, 99);
        ::CloseHandle(process);
        QVERIFY(ended);
        QTRY_VERIFY_WITH_TIMEOUT(f.host->state(kId).status == Host::State::Status::Running
            && f.host->state(kId).pid != pid, 15000);
        QTRY_VERIFY(!cache.state(u"volume").busy);
        QTRY_COMPARE(cache.state(u"volume").level, 40);
        const auto writes = f.ask(u"test/writes"_s);
        QVERIFY(writes);
        QCOMPARE(writes->toInt(), 0);
    }

    void disableAndProviderPriority()
    {
        Fixture f;
        QVERIFY(f.install(false, u"test.switches"_s));
        QCOMPARE(SwitchPlugin::provider(f.host->catalog()), u"test.switches"_s);
        QVERIFY(f.install());
        QVERIFY(f.running());
        QCOMPARE(SwitchPlugin::provider(f.host->catalog()), kId);
        f.host->setEnabled(kId, false);
        QCOMPARE(SwitchPlugin::provider(f.host->catalog()), u"test.switches"_s);
        QTRY_VERIFY(f.host->state(kId).pid == 0);
        f.host->setEnabled(u"test.switches"_s, false);
        QVERIFY(SwitchPlugin::provider(f.host->catalog()).isEmpty());
        QTRY_VERIFY(f.host->state(u"test.switches"_s).pid == 0);
    }

    void lateReplyAfterCacheDestroyed()
    {
        std::function<void(SwitchState)> reply;
        Switches::Remote remote;
        remote.ids = {u"volume"_s};
        remote.read = [&](const QString&, std::function<void(SwitchState)> done) { reply = std::move(done); };
        remote.write = [](const QString&, SwitchIntent, std::function<void(SwitchWrite)>) {};
        auto cache = std::make_unique<Switches>(std::move(remote));
        QVERIFY(reply);
        cache.reset();
        reply(SwitchState {}); // callback must not access the freed cache
    }

    void nativeProviderReadAndPowerCleanup()
    {
        Fixture f;
        QVERIFY(f.install(true));
        QVERIFY(f.running());
        SwitchPlugin bridge(f.host.get(), kId);
        Switches cache(bridge.remote());
        refreshOnChange(bridge, cache);
        QTRY_VERIFY_WITH_TIMEOUT(cache.state(u"volume").availability != Availability::Unknown, 20000);
        QTRY_COMPARE_WITH_TIMEOUT(cache.state(u"keep-awake").on, Value::Off, 20000);
        for (const QString& id : switchwire::ids())
            QTRY_VERIFY_WITH_TIMEOUT(cache.state(id).availability != Availability::Unknown, 20000);
        HANDLE measured = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, f.host->state(kId).pid);
        QVERIFY(measured);
        PROCESS_MEMORY_COUNTERS_EX memory {};
        memory.cb = sizeof memory;
        QVERIFY(::GetProcessMemoryInfo(measured, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof memory));
        ::CloseHandle(measured);
        qInfo() << "native provider private KiB" << memory.PrivateUsage / 1024
                << "working set KiB" << memory.WorkingSetSize / 1024;
        QCOMPARE(cache.apply(u"keep-awake", {Kind::On, 0}), Switches::Outcome::Started);
        QTRY_VERIFY_WITH_TIMEOUT(!cache.state(u"keep-awake").busy, 20000);
        QCOMPARE(cache.state(u"keep-awake").on, Value::On);
        QCOMPARE(cache.apply(u"keep-awake", {Kind::Off, 0}), Switches::Outcome::Started);
        QTRY_VERIFY_WITH_TIMEOUT(!cache.state(u"keep-awake").busy, 20000);
        QCOMPARE(cache.state(u"keep-awake").on, Value::Off);
        QCOMPARE(cache.apply(u"keep-awake", {Kind::On, 0}), Switches::Outcome::Started);
        QTRY_VERIFY_WITH_TIMEOUT(!cache.state(u"keep-awake").busy, 20000);
        const quint32 pid = f.host->state(kId).pid;
        HANDLE process = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        QVERIFY(process);
        const BOOL ended = ::TerminateProcess(process, 99);
        ::CloseHandle(process);
        QVERIFY(ended);
        QTRY_VERIFY_WITH_TIMEOUT(f.host->state(kId).status == Host::State::Status::Running
            && f.host->state(kId).pid != pid, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(cache.state(u"keep-awake").on, Value::Off, 20000);
        f.host->setEnabled(kId, false);
        QTRY_VERIFY(f.host->state(kId).pid == 0);
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(u"--provider"_s))
        return serve(app);
    SwitchPluginTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_switchplugin.moc"
