// A real statically-importing Qt executable with no runtime beside it. The
// production runner must supply DLL, platform-plugin and QML discovery.
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <cstdio>
#include <memory>
#include <windows.h>

using namespace Qt::StringLiterals;

QString loadedFrom(const wchar_t* name)
{
    wchar_t path[32768] {};
    const HMODULE module = ::GetModuleHandleW(name);
    if (!module)
        return {};
    const DWORD length = ::GetModuleFileNameW(module, path, DWORD(std::size(path)));
    return QString::fromWCharArray(path, static_cast<int>(length));
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData("import QtQuick\nimport QtQuick.Controls.Basic\nimport QtQuick.Layouts\n"
                      "Item { width: 300; height: 100; RowLayout { Button { text: 'Qt runtime' } } }", QUrl());
    std::unique_ptr<QObject> object(component.create());
    if (!qobject_cast<QQuickItem*>(object.get())) {
        qWarning() << component.errors();
        return 1;
    }
    const QJsonObject row {{u"title"_s, u"shared Qt ready"_s}, {u"data"_s, QJsonObject {
        {u"qt"_s, QString::fromLatin1(qVersion())}, {u"dir"_s, qEnvironmentVariable("WS_QT_DIR")},
        {u"qml"_s, qEnvironmentVariable("WS_QT_QML_DIR")}, {u"path"_s, qEnvironmentVariable("PATH")},
        {u"coreFile"_s, loadedFrom(L"Qt6Core.dll")}, {u"platformFile"_s, loadedFrom(L"qoffscreen.dll")},
        {u"basicFile"_s, loadedFrom(L"qtquickcontrols2basicstyleplugin.dll")},
        {u"layoutsFile"_s, loadedFrom(L"qquicklayoutsplugin.dll")}}}};
    const QByteArray answer = QJsonDocument(QJsonObject {{u"items"_s, QJsonArray {row}}}).toJson(QJsonDocument::Compact);
    std::fwrite(answer.constData(), 1, static_cast<size_t>(answer.size()), stdout);
    std::fputc('\n', stdout);
    return 0;
}
