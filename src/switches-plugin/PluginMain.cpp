#include "SwitchProtocol.h"
#include "SwitchService.h"
#include "platform/SystemSwitches.h"

#include <QCoreApplication>
#include <QTranslator>

#include <cstdio>

using namespace Qt::StringLiterals;

extern "C" __declspec(dllexport) int __cdecl winshunSwitchesMain(const wchar_t*)
{
    QByteArray pending;
    const auto first = ws::switchplugin::Protocol::first(pending);
    if (!first || first->value(u"method"_s) != u"initialize"_s)
        return 2;
    int argc = 1;
    char name[] = "WinShunSwitches";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& text) {
        std::fprintf(stderr, "%s\n", text.toUtf8().constData());
    });
    QTranslator english;
    if (first->value(u"params"_s).toObject().value(u"locale"_s).toString().startsWith(u"en")
        && english.load(u":/i18n/winshun_en.qm"_s))
        app.installTranslator(&english);
    ws::switchplugin::Service* service = nullptr;
    ws::switchplugin::Protocol protocol(std::move(pending),
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
    return app.exec();
}
