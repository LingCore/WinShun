#include "Host.h"
#include "Install.h"
#include "Programs.h"
#include "Updates.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSettings>
#include <QtTest/QTest>
#include <windows.h>
#include <stdexcept>
using namespace Qt::StringLiterals;
using namespace ws::plugin;

void require(bool value, const QString& message)
{
    if (!value)
        throw std::runtime_error(message.toUtf8().constData());
}
QByteArray read(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), u"Read failed "_s + path);
    return file.readAll();
}
void write(const QString& path, const QByteArray& bytes)
{
    require(QDir().mkpath(QFileInfo(path).path()), u"mkdir failed"_s);
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), u"Write failed "_s + path);
    require(file.write(bytes) == bytes.size(), u"Short write"_s);
}
Manifest place(const QString& package, const QString& plugins, const QString& staging, Host* host = nullptr)
{
    auto candidate = install::inspect(package, staging);
    require(candidate && candidate->manifest, candidate ? candidate->error : u"Inspect failed"_s);
    const auto discard = qScopeGuard([&] { install::discard(*candidate); });
    QString error;
    const Manifest manifest = *candidate->manifest;
    if (host)
        require(updates::replace(*host, *candidate, plugins).isEmpty(), u"Production replacement failed"_s);
    else
        require(!install::place(*candidate, plugins, &error).isEmpty(), error);
    return manifest;
}
struct WindowCheck {
    DWORD pid;
    bool visible = false;
    int width = 0;
    int height = 0;
};
BOOL CALLBACK findWindow(HWND window, LPARAM parameter)
{
    auto& check = *reinterpret_cast<WindowCheck*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == check.pid && IsWindowVisible(window)) {
        RECT rect {};
        GetWindowRect(window, &rect);
        if (rect.right - rect.left > 600 && rect.bottom - rect.top > 400) {
            check.visible = true;
            check.width = rect.right - rect.left;
            check.height = rect.bottom - rect.top;
            return FALSE;
        }
    }
    return TRUE;
}
QJsonObject launch(Host& host, const QString& id, const QString& text, const QJsonObject& settings, int queryId)
{
    bool answered = false;
    bool forwarded = false;
    QString error;
    QElapsedTimer timer;
    timer.start();
    const auto result = QObject::connect(&host, &Host::results,
        [&](std::uint64_t requestId, const QString& plugin, const QString&, const Items& items, bool partial, bool) {
            if (requestId == queryId && plugin == id && !partial)
                answered = !items.isEmpty() && items.first().id == u"open"_s;
        });
    const auto failed = QObject::connect(&host, &Host::queryFailed,
        [&](std::uint64_t, const QString&, const QString& reason) { error = reason; });
    const auto trace = QObject::connect(&host, &Host::traced,
        [&](const QString& plugin, const QString&, const QString&, const QString& detail, int) {
            const QJsonObject message = QJsonDocument::fromJson(detail.toUtf8()).object();
            if (plugin == id && message.value(u"method"_s) == u"initialize"_s) {
                const auto delivered = message.value(u"params"_s).toObject().value(u"settings"_s).toObject();
                forwarded = delivered.value(u"model"_s).toString() == settings.value(u"model"_s).toString()
                    && delivered.value(u"endpoint"_s).toString() == settings.value(u"endpoint"_s).toString();
            }
        });
    const auto disconnect = qScopeGuard([&] {
        QObject::disconnect(result);
        QObject::disconnect(failed);
        QObject::disconnect(trace);
    });
    const bool taken = host.query(text, ws::Scope::All, queryId);
    const bool responded = QTest::qWaitFor([&] { return answered || !error.isEmpty(); }, 10000) && answered;
    WindowCheck window {host.state(id).pid};
    const bool visible = responded && QTest::qWaitFor([&] {
        EnumWindows(findWindow, reinterpret_cast<LPARAM>(&window));
        return window.visible;
    }, 3000);
    const auto state = host.state(id);
    const bool running = state.status == Host::State::Status::Running && state.crashes == 0;
    QJsonObject report {{u"id"_s, id}, {u"queryTaken"_s, taken}, {u"queryAnswered"_s, responded},
        {u"settingsForwarded"_s, forwarded}, {u"windowVisible"_s, visible}, {u"running"_s, running},
        {u"width"_s, window.width}, {u"height"_s, window.height}, {u"elapsedMs"_s, timer.elapsed()}, {u"error"_s, error}};
    require(taken && responded && forwarded && visible && running, QString::fromUtf8(QJsonDocument(report).toJson()));
    return report;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString root = QDir::toNativeSeparators(app.arguments().at(1));
    const QString packages = QDir::toNativeSeparators(app.arguments().at(2));
    QJsonObject report;
    QJsonArray scenarios;
    const QString runName = QDateTime::currentDateTimeUtc().toString(u"hhmmsszzz"_s);
    bool ok = true;
    try {
        ::_wputenv_s(L"PATH", reinterpret_cast<const wchar_t*>((qEnvironmentVariable("SystemRoot") + u"\\System32"_s).utf16()));
        for (const bool upgrade : {false, true}) {
            const QString profile = root + (upgrade ? u"\\脚本版升级-"_s : u"\\首次使用-"_s) + runName;
            const QString plugins = profile + u"\\plugins"_s;
            const QString dataRoot = profile + u"\\data"_s;
            const QString staging = profile + u"\\staging"_s;
            const QString ini = profile + u"\\WinShun.ini"_s;
            QDir().mkpath(profile);
            QHash<QString, Manifest> old;
            QHash<QString, Manifest> current;
            for (const QString& id : {u"day.ocr"_s, u"day.rename"_s}) {
                const QString command = id == u"day.ocr"_s ? u"ocr"_s : u"rename"_s;
                if (upgrade)
                    old.insert(id, place(root + u'\\' + id + u"-old.wsplugin"_s, plugins, staging));
                else
                    current.insert(id, place(packages + u'\\' + id + u"-1.1.0.wsplugin"_s, plugins, staging));
                QSettings settings(ini, QSettings::IniFormat);
                settings.beginGroup(u"Plugin."_s + id);
                settings.setValue(u"Enabled"_s, true);
                settings.setValue(u"Confirmed"_s, (upgrade ? old[id] : current[id]).permissions.names());
                if (upgrade) {
                    settings.setValue(u"Setting.model"_s, u"保留的模型设置"_s);
                    settings.setValue(u"Setting.endpoint"_s, u"https://example.invalid/v1"_s);
                    settings.setValue(u"Keyword."_s + command, u"saved"_s + command);
                    write(dataRoot + u'\\' + id + u".data\\保留的数据.txt"_s, "preserve-user-data\n");
                }
                settings.endGroup();
                settings.sync();
            }
            const QByteArray before = read(ini);
            Host::Options options;
            options.userPluginDir = plugins;
            options.dataRoot = dataRoot;
            options.logPath = profile + u"\\plugins.log"_s;
            options.runnerExe = root + u"\\host\\WinShunPluginHost.exe"_s;
            options.version = u"0.22.0"_s;
            Host host(options);
            host.setDeveloperMode(true);
            QHash<QString, QStringList> confirmed;
            QHash<QString, QString> keywords;
            QJsonArray runs;
            int queryId = 0;
            for (const QString& id : {u"day.ocr"_s, u"day.rename"_s}) {
                const QString command = id == u"day.ocr"_s ? u"ocr"_s : u"rename"_s;
                QSettings saved(ini, QSettings::IniFormat);
                saved.beginGroup(u"Plugin."_s + id);
                QJsonObject values {{u"model"_s, saved.value(u"Setting.model"_s).toString()},
                    {u"endpoint"_s, saved.value(u"Setting.endpoint"_s).toString()}, {u"apiKey"_s, u"isolated-test-key"_s}};
                QString keyword = saved.value(u"Keyword."_s + command).toString();
                if (keyword.isEmpty())
                    keyword = id == u"day.ocr"_s ? u"ocr"_s : u"pl"_s;
                keywords.insert(id + u'/' + command, keyword);
                host.setKeywordOverrides(keywords);
                host.setPluginSettings(id, values);
                confirmed.insert(id, saved.value(u"Confirmed"_s).toStringList());
                host.catalog().setConfirmed(confirmed);
                host.setEnabled(id, true);
                bool asks = false;
                if (upgrade) {
                    programs::setOverride(Program::Python, root + u"\\python\\python.exe"_s);
                    launch(host, id, keyword, values, ++queryId);
                    current.insert(id, place(packages + u'\\' + id + u"-1.1.0.wsplugin"_s, plugins, staging, &host));
                    gallery::Item item;
                    item.id = id;
                    item.version = current[id].version;
                    item.minWinShun = u"0.22.0"_s;
                    item.review = gallery::Review::UnreviewedNative;
                    item.permissions = current[id].permissions.names();
                    updates::Installed installed;
                    installed.local.userCopy = true;
                    installed.local.userVersion = old[id].version;
                    installed.manifest = &old[id];
                    installed.enabled = true;
                    installed.confirmed = old[id].permissions.names();
                    updates::Policy policy {true, false, true, u"0.22.0"_s};
                    asks = updates::decide(item, installed, policy, {}, QDateTime::currentDateTime()).action == updates::Action::Ask;
                    require(asks, u"Native migration must require confirmation"_s);
                    require(install::unconfirmed(current[id], old[id].permissions.names()).isEmpty(), u"Unexpected permissions"_s);
                    host.catalog().scan();
                }
                programs::setOverride(Program::Python, root + u"\\missing-python.exe"_s);
                require(programs::find(Program::Python).isEmpty(), u"Python must be unavailable"_s);
                auto run = launch(host, id, keyword, values, ++queryId);
                run.insert(u"upgrade"_s, upgrade);
                run.insert(u"confirmationRequired"_s, asks);
                run.insert(u"pythonUnavailable"_s, true);
                run.insert(u"program"_s, current[id].runtime->programPath);
                runs.append(run);
                host.stop(id);
                if (upgrade)
                    require(read(dataRoot + u'\\' + id + u".data\\保留的数据.txt"_s) == "preserve-user-data\n", u"User data changed"_s);
            }
            host.shutdown();
            require(read(ini) == before, u"Settings changed during upgrade"_s);
            scenarios.append(QJsonObject {{u"upgrade"_s, upgrade}, {u"settingsPreserved"_s, true}, {u"runs"_s, runs}});
        }
    } catch (const std::exception& error) {
        ok = false;
        report.insert(u"error"_s, QString::fromUtf8(error.what()));
    }
    report.insert(u"scenarios"_s, scenarios);
    report.insert(u"ok"_s, ok);
    write(root + u"\\host-validation.json"_s, QJsonDocument(report).toJson());
    return ok ? 0 : 1;
}
