#include "SwitchProtocol.h"
#include "SwitchService.h"
#include "platform/SystemSwitches.h"

#include <QCoreApplication>
#include <QTranslator>

#include <windows.h>

#include <atomic>

using namespace Qt::StringLiterals;

namespace {

std::atomic<HANDLE> g_err {nullptr};

int serve(HANDLE in, HANDLE out, HANDLE err)
{
    g_err = err;
    QByteArray pending;
    const auto first = ws::switchplugin::Protocol::first(in, pending);
    if (!first || first->value(u"method"_s) != u"initialize"_s)
        return 2;
    // Inside the plugin host this thread is the host's only Qt thread, and
    // the host keeps it for the next start (src/plugin/runner/InProcess.cpp).
    int argc = 1;
    char name[] = "WinShunSwitches";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& text) {
        const QByteArray line = text.toUtf8() + '\n';
        DWORD put = 0;
        if (const HANDLE handle = g_err.load())
            ::WriteFile(handle, line.constData(), static_cast<DWORD>(line.size()), &put, nullptr);
    });
    QTranslator english;
    if (first->value(u"params"_s).toObject().value(u"locale"_s).toString().startsWith(u"en")
        && english.load(u":/i18n/winshun_en.qm"_s))
        app.installTranslator(&english);
    ws::switchplugin::Service* service = nullptr;
    ws::switchplugin::Protocol protocol(in, out, std::move(pending),
        [&](const QJsonValue& id, const QString& method, const QJsonObject& params) {
            if (method == u"appearance") {
                app.removeTranslator(&english);
                if (params.value(u"locale"_s).toString().startsWith(u"en") && english.load(u":/i18n/winshun_en.qm"_s))
                    app.installTranslator(&english);
            } else if (service)
                service->request(id, method, params);
        }, [&] { app.quit(); });
    ws::switchplugin::Service provider(ws::systemSwitchBackends(), protocol);
    service = &provider;
    protocol.respond(first->value(u"id"_s), QJsonObject {{u"protocolVersion"_s, u"1"_s}, {u"name"_s, u"winshun.switches"_s}});
    const int code = app.exec();
    qInstallMessageHandler(nullptr);
    return code;
}

} // namespace

// WinShunSwitches.exe (stub.cpp): alone, on its stdin and stdout.
extern "C" __declspec(dllexport) int __cdecl winshunSwitchesMain(const wchar_t*)
{
    return serve(::GetStdHandle(STD_INPUT_HANDLE), ::GetStdHandle(STD_OUTPUT_HANDLE), ::GetStdHandle(STD_ERROR_HANDLE));
}

// The plugin host: on its Qt thread, over its pipes; returns when `in` closes
// or shutdown comes.
extern "C" __declspec(dllexport) int __cdecl winshunSwitchesServe(HANDLE in, HANDLE out, HANDLE err)
{
    return serve(in, out, err);
}
