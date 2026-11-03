// The plugin updates' background service (src/app/PluginUpdates): a signed
// list (made with a key generated here and handed over through Options::keys),
// the installed plugins, a package beside the list, the clock and "running"
// injected.

#include "PluginUpdates.h"

#include "Install.h"
#include "Manifest.h"
#include "Settings.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <windows.h>
#include <bcrypt.h>
#include <miniz.h>

#include <memory>
#include <set>

using namespace Qt::StringLiterals;
using namespace ws;

namespace {

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

bool writeZip(const QString& path, const QList<std::pair<QString, QByteArray>>& entries)
{
    mz_zip_archive zip {};
    if (!mz_zip_writer_init_heap(&zip, 0, 64 * 1024))
        return false;
    bool ok = true;
    for (const auto& [name, bytes] : entries)
        ok = ok && mz_zip_writer_add_mem(&zip, name.toUtf8().constData(), bytes.constData(), static_cast<size_t>(bytes.size()), MZ_BEST_SPEED);
    void* buffer = nullptr;
    size_t size = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size);
    if (ok)
        ok = writeFile(path, QByteArray(static_cast<const char*>(buffer), static_cast<qsizetype>(size)));
    mz_free(buffer);
    mz_zip_writer_end(&zip);
    return ok;
}

QByteArray manifestOf(const QString& id, const QString& version, const QString& extra = {})
{
    return (u"{\"manifest\":1,\"id\":\"%1\",\"name\":\"Zero\",\"version\":\"%2\",%3\"commands\":[{\"id\":\"go\",\"keyword\":\"go\",\"run\":{\"folder\":\"C:\\\\\"}}]}"_s
        .arg(id, version, extra))
        .toUtf8();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A key made here with CNG; signs like the plugin repository does (r||s).
struct TestKey {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    QByteArray xy;
    TestKey()
    {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0
            || BCryptGenerateKeyPair(alg, &key, 256, 0) != 0 || BCryptFinalizeKeyPair(key, 0) != 0) {
            key = nullptr;
            return;
        }
        ULONG size = 0;
        BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &size, 0);
        QByteArray blob(static_cast<qsizetype>(size), 0);
        if (BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, reinterpret_cast<PUCHAR>(blob.data()), size, &size, 0) == 0)
            xy = blob.mid(sizeof(BCRYPT_ECCKEY_BLOB), 64);
    }
    ~TestKey()
    {
        if (key)
            BCryptDestroyKey(key);
        if (alg)
            BCryptCloseAlgorithmProvider(alg, 0);
    }
    TestKey(const TestKey&) = delete;
    TestKey& operator=(const TestKey&) = delete;
    bool ok() const { return key && xy.size() == 64; }
    QByteArray signFile(const QByteArray& data) const
    {
        QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
        QByteArray rs(64, 0);
        ULONG written = 0;
        if (BCryptSignHash(key, nullptr, reinterpret_cast<PUCHAR>(hash.data()), 32, reinterpret_cast<PUCHAR>(rs.data()), 64, &written, 0) != 0)
            return {};
        const QJsonArray signatures {QJsonObject {{u"key"_s, u"test"_s}, {u"algorithm"_s, u"ecdsa-p256-sha256"_s},
            {u"signature"_s, QString::fromLatin1(rs.toBase64())}}};
        return QJsonDocument(QJsonObject {{u"schema"_s, 1}, {u"file"_s, u"index.json"_s}, {u"signatures"_s, signatures}}).toJson();
    }
};

struct Offer {
    QString id, version, extra; // extra: what the package's manifest has besides (e.g. permissions)
    QStringList listed; // the permissions the list names
    bool badHash = false;
    bool bundled = false;
    QString minWinShun;
};

class Tst : public QObject {
    Q_OBJECT

    TestKey m_key;
    TestKey m_other;

    struct Fixture {
        QTemporaryDir dir;
        QString data, user, list;
        QDateTime now = QDateTime(QDate(2026, 11, 2), QTime(12, 0));
        std::set<QString> running;
        std::unique_ptr<PluginUpdates> svc;
        QList<PluginConfig> configs;
        Fixture()
        {
            data = dir.path() + u"/data"_s;
            user = data + u"/plugins"_s;
            list = dir.path() + u"/list"_s;
            QDir().mkpath(user);
            QDir().mkpath(list);
        }
        QString source() const { return list + u"/index.json"_s; }
        void install(const QString& id, const QString& version, bool enabled = false, const QStringList& confirmed = {})
        {
            QVERIFY(writeFile(user + u'/' + id + u"/plugin.json"_s, manifestOf(id, version)));
            QVERIFY(writeFile(user + u'/' + id + u"/old.txt"_s, "o"));
            PluginConfig c;
            c.id = id;
            c.enabled = enabled;
            c.confirmed = confirmed;
            configs.push_back(c);
        }
        // The list (and its signature by `signer`, or none) with these offers; the packages beside it.
        void publish(const QList<Offer>& offers, const TestKey* signer, bool unsigned_ = false)
        {
            QJsonArray plugins;
            for (const Offer& o : offers) {
                const QString file = u"%1-%2.wsplugin"_s.arg(o.id, o.version);
                QVERIFY(writeZip(list + u'/' + file, {{u"plugin.json"_s, manifestOf(o.id, o.version, o.extra)}, {u"new.txt"_s, "n"}}));
                const QByteArray bytes = readFile(list + u'/' + file);
                QString sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
                if (o.badHash)
                    sha = QString(64, u'0');
                plugins.push_back(QJsonObject {{u"id"_s, o.id}, {u"name"_s, o.id}, {u"version"_s, o.version},
                    {u"permissions"_s, QJsonArray::fromStringList(o.listed)}, {u"minWinShun"_s, o.minWinShun}, {u"bundled"_s, o.bundled},
                    {u"url"_s, file}, {u"sha256"_s, sha}, {u"size"_s, static_cast<double>(bytes.size())}});
            }
            const QByteArray json = QJsonDocument(QJsonObject {{u"schema"_s, 1}, {u"generated"_s, u"2026-11-01T00:00:00Z"_s}, {u"plugins"_s, plugins}}).toJson();
            QVERIFY(writeFile(source(), json));
            QFile::remove(source() + u".sig"_s);
            if (!unsigned_ && signer)
                QVERIFY(writeFile(source() + u".sig"_s, signer->signFile(json)));
        }
        void startSigned(const TestKey& key, const QString& sourceOverride = {}, plugin::gallery::Fetcher web = {})
        {
            PluginUpdates::Options o;
            o.dataDir = data;
            o.userDir = user;
            o.source = sourceOverride.isEmpty() ? source() : sourceOverride;
            o.web = web;
            o.keys = {{u"test"_s, key.xy}};
            o.appVersion = u"0.12.0"_s;
            o.now = [this] { return now; };
            svc = std::make_unique<PluginUpdates>(nullptr, o);
            svc->setRunning([this](const QString& id) { return running.count(id) > 0; });
            svc->setPolicy(false, configs);
            svc->setAutomatic(true);
        }
        QString versionOf(const QString& id) const
        {
            QString error;
            const auto m = plugin::Manifest::load(user + u'/' + id + u"/plugin.json"_s, &error);
            return m ? m->version : QString();
        }
        QVariant stateValue(const QString& key) const { return QSettings(data + u"/update.ini"_s, QSettings::IniFormat).value(key); }
        bool settle() { return QTest::qWaitFor([this] { return svc->idle(); }, 10000); }
    };

    struct CaptureLike {
        Fixture f;
        PluginConfig config;
        const QString id = u"test.cap"_s;
        const QString before = u"\"permissions\":[\"clipboard:write\",\"ui\"],"_s;
        const QString after = u"\"permissions\":[\"clipboard:write\",\"ui\",\"processes\"],"_s;
        bool setUp(const TestKey& key)
        {
            if (!writeFile(f.user + u"/test.cap/plugin.json"_s, manifestOf(id, u"1.0.0"_s, before)) || !writeFile(f.user + u"/test.cap/old.txt"_s, "o"))
                return false;
            config.id = id;
            config.enabled = true;
            config.confirmed = {u"clipboard:write"_s, u"ui"_s};
            f.configs.push_back(config);
            f.publish({{id, u"1.0.1"_s, after, {u"clipboard:write"_s, u"ui"_s, u"processes"_s}}}, &key);
            f.startSigned(key);
            return true;
        }
    };

private slots:
    void initTestCase() { QVERIFY(m_key.ok() && m_other.ok()); }

    void installsWhenNotRunningAndSigned()
    {
        Fixture f;
        f.install(u"test.a"_s, u"1.0.0"_s);
        f.publish({{u"test.a"_s, u"1.1.0"_s, {}, {}}}, &m_key);
        f.startSigned(m_key);
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(f.versionOf(u"test.a"_s), u"1.1.0"_s);
        QVERIFY(QFileInfo::exists(f.user + u"/test.a/new.txt"_s));
        QVERIFY(!QFileInfo::exists(f.user + u"/test.a/old.txt"_s));
        QCOMPARE(f.svc->count(), 0);
        QCOMPARE(f.stateValue(u"PluginUpdates/Updated/test.a"_s).toString(), u"1.1.0"_s);
        QCOMPARE(f.stateValue(u"PluginUpdates/UpdatedFrom/test.a"_s).toString(), u"1.0.0"_s);
    }

    void stagedWhileRunningThenAppliedAtStart()
    {
        Fixture f;
        f.install(u"test.a"_s, u"1.0.0"_s, true);
        f.publish({{u"test.a"_s, u"1.1.0"_s, {}, {}}}, &m_key);
        f.running.insert(u"test.a"_s);
        f.startSigned(m_key);
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(f.versionOf(u"test.a"_s), u"1.0.0"_s); // not touched
        QCOMPARE(f.svc->stagedVersion(u"test.a"_s), u"1.1.0"_s);
        QCOMPARE(f.svc->count(), 0); // it will take effect by itself
        f.svc.reset();
        const auto applied = PluginUpdates::prepare(f.data, f.user, u"0.12.0", [](const QString&) { return std::pair<bool, QStringList>(true, {}); });
        QCOMPARE(applied.size(), 1);
        QVERIFY2(!applied[0].dropped && applied[0].error.isEmpty(), qPrintable(applied[0].error));
        QCOMPARE(f.versionOf(u"test.a"_s), u"1.1.0"_s);
    }

    void stagedAppliedAtTheHourOnceItStopped()
    {
        Fixture f;
        f.install(u"test.a"_s, u"1.0.0"_s, true);
        f.publish({{u"test.a"_s, u"1.1.0"_s, {}, {}}}, &m_key);
        f.running.insert(u"test.a"_s);
        f.startSigned(m_key);
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(f.svc->stagedVersion(u"test.a"_s), u"1.1.0"_s);
        f.svc->tick(); // still running: left alone
        QCOMPARE(f.versionOf(u"test.a"_s), u"1.0.0"_s);
        f.running.clear();
        f.svc->tick();
        QCOMPARE(f.versionOf(u"test.a"_s), u"1.1.0"_s);
        QVERIFY(f.svc->stagedVersion(u"test.a"_s).isEmpty());
    }

    void badSignatureDoesNotInstall()
    {
        Fixture f;
        f.install(u"test.a"_s, u"1.0.0"_s);
        f.publish({{u"test.a"_s, u"1.1.0"_s, {}, {}}}, &m_other); // signed by another key
        f.startSigned(m_key);
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(f.versionOf(u"test.a"_s), u"1.0.0"_s);
        QCOMPARE(f.svc->count(), 1); // told, not installed
    }

    void unsignedLocalListIsTrusted()
    {
        Fixture f;
        f.install(u"test.a"_s, u"1.0.0"_s);
        f.publish({{u"test.a"_s, u"1.1.0"_s, {}, {}}}, nullptr);
        f.startSigned(m_key);
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(f.versionOf(u"test.a"_s), u"1.1.0"_s); // the local-file exception (WINSHUN_PLUGIN_INDEX)
    }

    void failedPackageWaitsADay()
    {
        Fixture f;
        f.install(u"test.a"_s, u"1.0.0"_s);
        Offer o {u"test.a"_s, u"1.1.0"_s, {}, {}};
        o.badHash = true;
        f.publish({o}, &m_key);
        f.startSigned(m_key);
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(f.versionOf(u"test.a"_s), u"1.0.0"_s);
        QCOMPARE(f.stateValue(u"PluginUpdates/Failed/test.a"_s).toString(), u"1.1.0"_s);
        QCOMPARE(f.svc->count(), 1);
        QVERIFY(!f.svc->problem(u"test.a"_s).isEmpty());
        const QVariant first = f.stateValue(u"PluginUpdates/FailedAt/test.a"_s);
        f.now = f.now.addSecs(3600); // within the day: no new try
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(f.stateValue(u"PluginUpdates/FailedAt/test.a"_s), first);
        f.now = f.now.addSecs(25 * 3600); // a day on: again
        f.svc->checkNow();
        QVERIFY(f.settle());
        QVERIFY(f.stateValue(u"PluginUpdates/FailedAt/test.a"_s) != first);
        // The fixed package comes: installed.
        f.publish({{u"test.a"_s, u"1.1.0"_s, {}, {}}}, &m_key);
        f.now = f.now.addSecs(25 * 3600);
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(f.versionOf(u"test.a"_s), u"1.1.0"_s);
        QCOMPARE(f.svc->count(), 0);
    }

    void widenedPackageIsNotInstalled()
    {
        Fixture f;
        f.install(u"test.a"_s, u"1.0.0"_s);
        // The list says nothing new; the package asks for a program.
        f.publish({{u"test.a"_s, u"1.1.0"_s, u"\"permissions\":[\"processes\"],"_s, {}}}, &m_key);
        f.startSigned(m_key);
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(f.versionOf(u"test.a"_s), u"1.0.0"_s);
        QCOMPARE(f.stateValue(u"PluginUpdates/Widened/test.a"_s).toString(), u"1.1.0"_s);
        QCOMPARE(f.svc->count(), 1);
        QVERIFY(!QFileInfo::exists(f.data + u"/plugin-updates/test.a"_s) || QDir(f.data + u"/plugin-updates/test.a"_s).isEmpty());
        f.svc->checkNow(); // again: asked of the user, not downloaded again
        QVERIFY(f.settle());
        QCOMPARE(f.versionOf(u"test.a"_s), u"1.0.0"_s);
        QCOMPARE(f.svc->count(), 1);
    }

    void switchOffMeansNoFetch()
    {
        Fixture f;
        f.install(u"test.a"_s, u"1.0.0"_s);
        int fetches = 0;
        const QString source = u"https://example.test/list/index.json"_s;
        const plugin::gallery::Fetcher web = [&](const QString& url, qint64, const plugin::gallery::Progress&) {
            ++fetches;
            Q_UNUSED(url);
            return plugin::gallery::Fetched {404, {}, 0};
        };
        f.startSigned(m_key, source, web);
        f.svc->setAutomatic(false);
        f.svc->tick();
        QVERIFY(f.settle());
        QCOMPARE(fetches, 0);
        f.svc->setAutomatic(true);
        f.svc->tick();
        QVERIFY(f.settle());
        QVERIFY(fetches > 0); // asked now
        const int once = fetches;
        f.svc->tick(); // a failed fetch is tried again at the next beat
        QVERIFY(f.settle());
        QVERIFY(fetches > once);
    }

    void dailyAfterASuccessfulCheck()
    {
        Fixture f;
        f.install(u"test.a"_s, u"1.0.0"_s);
        f.publish({{u"test.a"_s, u"1.0.0"_s, {}, {}}}, &m_key);
        int fetches = 0;
        const QString source = u"https://example.test/list/index.json"_s;
        const plugin::gallery::Fetcher web = [&](const QString& url, qint64, const plugin::gallery::Progress&) {
            ++fetches;
            const QString name = url.mid(url.lastIndexOf(u'/') + 1);
            const QByteArray bytes = readFile(f.list + u'/' + name);
            return bytes.isEmpty() ? plugin::gallery::Fetched {404, {}, 0} : plugin::gallery::Fetched {200, bytes, 0};
        };
        f.startSigned(m_key, source, web);
        f.svc->setAutomatic(true);
        f.svc->tick();
        QVERIFY(f.settle());
        const int once = fetches;
        QVERIFY(once >= 2); // the list and its signature
        f.now = f.now.addSecs(3600);
        f.svc->tick();
        QVERIFY(f.settle());
        QCOMPARE(fetches, once);
        f.now = f.now.addSecs(24 * 3600);
        f.svc->tick();
        QVERIFY(f.settle());
        QVERIFY(fetches > once);
    }

    void noUserPluginsNoNetwork()
    {
        Fixture f;
        int fetches = 0;
        const plugin::gallery::Fetcher web = [&](const QString&, qint64, const plugin::gallery::Progress&) {
            ++fetches;
            return plugin::gallery::Fetched {404, {}, 0};
        };
        f.startSigned(m_key, u"https://example.test/list/index.json"_s, web);
        f.svc->setAutomatic(true);
        f.svc->tick();
        QVERIFY(f.settle());
        QCOMPARE(fetches, 0);
    }

    void countsLeaveOutBundledNewerAppAndDismissed()
    {
        Fixture f;
        f.install(u"test.bundled"_s, u"1.0.0"_s);
        f.install(u"test.needsapp"_s, u"1.0.0"_s);
        f.install(u"test.asks"_s, u"1.0.0"_s);
        Offer bundled {u"test.bundled"_s, u"1.1.0"_s, {}, {}};
        bundled.bundled = true;
        Offer needs {u"test.needsapp"_s, u"1.1.0"_s, {}, {}};
        needs.minWinShun = u"99.0"_s;
        Offer asks {u"test.asks"_s, u"1.1.0"_s, u"\"permissions\":[\"processes\"],"_s, {u"processes"_s}};
        f.publish({bundled, needs, asks}, &m_key);
        f.startSigned(m_key);
        f.svc->setAutomatic(true);
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(f.svc->count(), 1); // only the one asking for more
        QVERIFY(f.svc->waiting(u"test.asks"_s) && !f.svc->waiting(u"test.needsapp"_s) && !f.svc->waiting(u"test.bundled"_s));
        QCOMPARE(f.svc->more(u"test.asks"_s), QStringList {u"processes"_s});
        QCOMPARE(f.versionOf(u"test.asks"_s), u"1.0.0"_s);
        f.svc->dismiss(u"test.asks"_s, u"1.1.0"_s);
        QCOMPARE(f.svc->count(), 0);
        QVERIFY(!f.svc->waiting(u"test.asks"_s));
        f.svc->forget(u"test.asks"_s);
        QCOMPARE(f.svc->count(), 1);
    }

    void countsDevelopmentCopiesNotAndAutomaticOffYes()
    {
        Fixture f;
        f.install(u"test.dev"_s, u"1.0.0"_s);
        f.install(u"test.plain"_s, u"1.0.0"_s);
        QVERIFY(writeFile(f.user + u"/test.dev/.git/HEAD"_s, "ref"));
        f.publish({{u"test.dev"_s, u"1.1.0"_s, {}, {}}, {u"test.plain"_s, u"1.1.0"_s, {}, {}}}, &m_key);
        f.startSigned(m_key);
        f.svc->setAutomatic(false);
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(f.svc->count(), 1); // the plain one waits for the user; the copy in development does not count
        QVERIFY(f.svc->waiting(u"test.plain"_s) && !f.svc->waiting(u"test.dev"_s)); // the ids 只看可更新 shows are the counted ones (disabled plugins too)
        QCOMPARE(f.versionOf(u"test.plain"_s), u"1.0.0"_s);
        f.svc->setAutomatic(true); // now the plain one is put in place by itself
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(f.versionOf(u"test.plain"_s), u"1.1.0"_s);
        QCOMPARE(f.versionOf(u"test.dev"_s), u"1.0.0"_s);
        QCOMPARE(f.svc->count(), 0);
        QVERIFY(!f.svc->waiting(u"test.plain"_s));
    }

    void automaticSwitchIsReadAndAnnounced()
    {
        Fixture f;
        f.install(u"test.a"_s, u"1.0.0"_s);
        f.publish({{u"test.a"_s, u"1.1.0"_s, {}, {}}}, &m_key);
        f.startSigned(m_key);
        QVERIFY(f.svc->automatic());
        QSignalSpy spy(f.svc.get(), &PluginUpdates::automaticChanged);
        f.svc->setAutomatic(true); // no change: no signal
        QCOMPARE(spy.count(), 0);
        f.svc->setAutomatic(false);
        QVERIFY(!f.svc->automatic());
        QCOMPARE(spy.count(), 1);
    }

    void consentIsToldOncePerVersion()
    {
        Fixture f;
        f.install(u"test.a"_s, u"1.0.0"_s, true, {}); // on
        f.publish({{u"test.a"_s, u"1.1.0"_s, u"\"permissions\":[\"processes\"],"_s, {u"processes"_s}}}, &m_key);
        f.startSigned(m_key);
        f.svc->setAutomatic(true);
        QSignalSpy spy(f.svc.get(), &PluginUpdates::consentNeeded);
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList {u"test.a"_s});
        QCOMPARE(f.versionOf(u"test.a"_s), u"1.0.0"_s); // never put in place without the yes
        f.svc->checkNow(); // same version again
        QVERIFY(f.settle());
        f.svc->setPolicy(false, f.configs);
        QCOMPARE(spy.size(), 1);
        // A newer one: told again.
        f.publish({{u"test.a"_s, u"1.2.0"_s, u"\"permissions\":[\"processes\"],"_s, {u"processes"_s}}}, &m_key);
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(spy.size(), 2);
    }

    void consentNotToldForAnUnsignedListOrAnOffPlugin()
    {
        Fixture f;
        f.install(u"test.on"_s, u"1.0.0"_s, true, {});
        f.install(u"test.off"_s, u"1.0.0"_s, false, {});
        const QString extra = u"\"permissions\":[\"processes\"],"_s;
        f.publish({{u"test.on"_s, u"1.1.0"_s, extra, {u"processes"_s}}, {u"test.off"_s, u"1.1.0"_s, extra, {u"processes"_s}}}, &m_other);
        f.startSigned(m_key); // the list is signed by a key not known
        f.svc->setAutomatic(true);
        QSignalSpy spy(f.svc.get(), &PluginUpdates::consentNeeded);
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(spy.size(), 0);
        QCOMPARE(f.svc->count(), 2);
        // Signed: only the one that is on.
        f.publish({{u"test.on"_s, u"1.1.0"_s, extra, {u"processes"_s}}, {u"test.off"_s, u"1.1.0"_s, extra, {u"processes"_s}}}, &m_key);
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.at(0).at(0).toStringList(), QStringList {u"test.on"_s});
    }

    void claimKeepsTheServiceOff()
    {
        Fixture f;
        f.install(u"test.a"_s, u"1.0.0"_s);
        f.publish({{u"test.a"_s, u"1.1.0"_s, {}, {}}}, &m_key);
        f.startSigned(m_key);
        f.svc->setAutomatic(true);
        QVERIFY(f.svc->claim(u"test.a"_s));
        QVERIFY(!f.svc->claim(u"test.a"_s));
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(f.versionOf(u"test.a"_s), u"1.0.0"_s);
        f.svc->release(u"test.a"_s);
        QVERIFY(!f.svc->claimed(u"test.a"_s));
        f.svc->checkNow();
        QVERIFY(f.settle());
        QCOMPARE(f.versionOf(u"test.a"_s), u"1.1.0"_s);
    }

    // The reason the feature exists: a plugin that is on (capture), a newer version that names one permission more
    // (processes). It is never put in place by itself; the user is told once; the one question (更新并允许) is
    // answered through the same steps PluginSettings takes (installFrom -> permissionsToAllow, finishInstall -> replace,
    // scan, confirmEnable -> confirmedFor).
    void captureUpdateAddingProcessesIsAllowedInOneQuestion()
    {
        CaptureLike c;
        QVERIFY(c.setUp(m_key));
        PluginUpdates& svc = *c.f.svc;
        QSignalSpy told(&svc, &PluginUpdates::consentNeeded);
        svc.checkNow();
        QVERIFY(c.f.settle());
        QCOMPARE(c.f.versionOf(c.id), u"1.0.0"_s); // not installed by itself
        QCOMPARE(svc.count(), 1);
        QCOMPARE(svc.more(c.id), QStringList {u"processes"_s});
        QCOMPARE(told.size(), 1);
        svc.checkNow(); // a second check says nothing
        QVERIFY(c.f.settle());
        QCOMPARE(told.size(), 1);

        // The 插件库 gives the package to the page's install: the question is the combined one.
        std::optional<plugin::install::Candidate> candidate = plugin::install::inspect(c.f.list + u"/test.cap-1.0.1.wsplugin"_s);
        QVERIFY(candidate && candidate->manifest);
        QCOMPARE(plugin::updates::permissionsToAllow(*candidate->manifest, c.config.enabled, c.config.confirmed), QStringList {u"processes"_s});
        // ... a plugin that is off, or a never-confirmed one, is not asked here (turning it on asks)
        QVERIFY(plugin::updates::permissionsToAllow(*candidate->manifest, false, c.config.confirmed).isEmpty());
        QVERIFY(plugin::updates::permissionsToAllow(*candidate->manifest, true, {}).isEmpty());

        // The yes: claim, replace, forget, scan; then confirm what the new manifest names, still on.
        QVERIFY(svc.claim(c.id));
        QString error;
        QVERIFY(!plugin::install::place(*candidate, c.f.user, &error).isEmpty());
        plugin::install::discard(*candidate);
        PluginConfig confirmed = c.config;
        confirmed.confirmed = plugin::updates::confirmedFor(*candidate->manifest);
        c.f.configs = {confirmed};
        svc.rescan(); // as finishInstall: the scan first, then what was kept for the old version goes
        svc.forget(c.id);
        svc.release(c.id);
        svc.setPolicy(false, c.f.configs);
        QCOMPARE(c.f.versionOf(c.id), u"1.0.1"_s);
        QVERIFY(QFileInfo::exists(c.f.user + u"/test.cap/new.txt"_s));
        QVERIFY(c.f.configs.at(0).enabled);
        QVERIFY(c.f.configs.at(0).confirmed.contains(u"processes"_s));
        QCOMPARE(svc.count(), 0);
        QCOMPARE(told.size(), 1);
    }

    void captureUpdateDeclinedKeepsTheOldVersionAndIsNotCountedAgain()
    {
        CaptureLike c;
        QVERIFY(c.setUp(m_key));
        PluginUpdates& svc = *c.f.svc;
        QSignalSpy told(&svc, &PluginUpdates::consentNeeded);
        svc.checkNow();
        QVERIFY(c.f.settle());
        QCOMPARE(svc.count(), 1);
        QCOMPARE(told.size(), 1);

        std::optional<plugin::install::Candidate> candidate = plugin::install::inspect(c.f.list + u"/test.cap-1.0.1.wsplugin"_s);
        QVERIFY(candidate && candidate->manifest);
        QVERIFY(!plugin::updates::permissionsToAllow(*candidate->manifest, c.config.enabled, c.config.confirmed).isEmpty());
        // The no (PluginSettings::declineInstall): this version stops being counted; nothing was touched.
        svc.dismiss(c.id, candidate->manifest->version);
        plugin::install::discard(*candidate);
        QCOMPARE(c.f.versionOf(c.id), u"1.0.0"_s);
        QVERIFY(QFileInfo::exists(c.f.user + u"/test.cap/old.txt"_s));
        QVERIFY(!QFileInfo::exists(c.f.user + u"/test.cap/new.txt"_s));
        QVERIFY(c.f.configs.at(0).enabled);
        QCOMPARE(c.f.configs.at(0).confirmed, (QStringList {u"clipboard:write"_s, u"ui"_s}));
        QCOMPARE(svc.count(), 0);
        svc.checkNow(); // checked again: still counted out, still not told
        QVERIFY(c.f.settle());
        QCOMPARE(svc.count(), 0);
        QCOMPARE(told.size(), 1);
        QCOMPARE(c.f.versionOf(c.id), u"1.0.0"_s);
        // A newer version is a new question.
        c.f.publish({{c.id, u"1.0.2"_s, c.after, {u"clipboard:write"_s, u"ui"_s, u"processes"_s}}}, &m_key);
        svc.checkNow();
        QVERIFY(c.f.settle());
        QCOMPARE(svc.count(), 1);
        QCOMPARE(told.size(), 2);
    }
};

} // namespace

QTEST_MAIN(Tst)
#include "tst_pluginupdates.moc"
