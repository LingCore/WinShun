#include "OcrController.h"
#include "OcrWorker.h"
#include "../capture-plugin/Protocol.h"
#include "Phosphor.h"
#include "SystemTheme.h"
#include "WindowFrame.h"
#include "platform/IdleTrim.h"
#include "platform/OcrFiles.h"
#include "platform/WindowEffects.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQuickImageProvider>
#include <QScreen>
#include <QStyleHints>
#include <QTranslator>

#include <shellapi.h>
#include <shlobj.h>

using namespace Qt::StringLiterals;
using namespace ws::ocrsearch;

namespace {
class AppIcon : public QQuickImageProvider {
public:
    explicit AppIcon(QString path)
        : QQuickImageProvider(Image)
        , m_path(std::move(path))
    {
    }
    QImage requestImage(const QString&, QSize* size, const QSize& requested) override
    {
        const int side = requested.isValid() ? std::max(requested.width(), requested.height()) : 32;
        HICON icon = nullptr;
        ::SHDefExtractIconW(reinterpret_cast<LPCWSTR>(m_path.utf16()), 0, 0, &icon, nullptr, MAKELONG(side, 0));
        QImage image;
        if (icon) {
            image = QImage::fromHICON(icon);
            ::DestroyIcon(icon);
        }
        if (size)
            *size = image.size();
        return image;
    }

private:
    QString m_path;
};
} // namespace

extern "C" __declspec(dllexport) int winshunOcrSearchMain(const wchar_t* host, int mode)
{
    const bool preview = mode == 1;
    const bool worker = mode == 2;
    const QString appDir = QString::fromWCharArray(host);
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    QStringList arguments;
    int count = 0;
    if (LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i)
            arguments.append(QString::fromWCharArray(wide[i]));
        ::LocalFree(wide);
    }
    const auto option = [&](const QString& name) {
        const int index = arguments.indexOf(name);
        return index >= 0 && index + 1 < arguments.size() ? arguments[index + 1] : QString();
    };
    QByteArray pending;
    std::optional<QJsonObject> first;
    if (!preview && !worker) {
        first = ws::captureplugin::Protocol::readFirst(&pending);
        if (!first || first->value(u"method"_s).toString() != u"initialize")
            return 2;
    }
    QJsonObject params = first ? first->value(u"params"_s).toObject() : QJsonObject {};
    QJsonObject appearance = params.value(u"appearance"_s).toObject();
    if (preview)
        appearance = {{u"themeSetting"_s, option(u"--theme"_s)}, {u"locale"_s, option(u"--locale"_s)},
                      {u"uiStyle"_s, option(u"--style"_s)}};
    const bool software
        = appearance.value(u"renderer"_s).toString() == u"software" || arguments.contains(u"--software"_s);
    qputenv("QT_QPA_PLATFORM", software ? "windows:fontengine=gdi" : "windows:fontengine=freetype");
    QCoreApplication::setLibraryPaths({appDir});
    QCoreApplication::setApplicationName(u"WinShunOcrSearch"_s);
    int argc = 1;
    char name[] = "WinShunOcrSearch";
    char* argv[] = {name, nullptr};
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationDisplayName(u"Win顺"_s);
    QGuiApplication::setQuitOnLastWindowClosed(false);
    QQuickWindow::setGraphicsApi(software ? QSGRendererInterface::Software : QSGRendererInterface::Direct3D11);
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    QImageReader::setAllocationLimit(128);
    ws::SystemTheme::setBackdropAvailable(false);
    if (!qEnvironmentVariable("WS_COMPONENTS_DIR").isEmpty())
        ws::ocr::setComponentsRoot(qEnvironmentVariable("WS_COMPONENTS_DIR"));
    if (worker) {
        QTranslator workerTranslator;
        if (qEnvironmentVariable("WS_LOCALE") == u"en" && workerTranslator.load(u":/i18n/ocr-search_en.qm"_s))
            QCoreApplication::installTranslator(&workerTranslator);
        ::SetPriorityClass(::GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
        return workerMain(appDir);
    }
    // Fonts only for the window (some 8 MB): the worker draws no text.
    ws::phosphor::loadFonts();
    QFile font(QDir(appDir).filePath(u"fonts/AlibabaPuHuiTi-3-55-Regular.ttf"_s));
    if (font.open(QIODevice::ReadOnly)) {
        const int id = QFontDatabase::addApplicationFontFromData(font.readAll());
        const auto families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty()) {
            QFont f(families.first());
            f.setHintingPreference(QFont::PreferVerticalHinting);
            QGuiApplication::setFont(f);
        }
    }
    QString dataDir
        = preview ? option(u"--data-dir"_s) : params.value(u"dataDir"_s).toString(qEnvironmentVariable("WS_DATA_DIR"));
    // A standalone preview must name its own isolated profile explicitly.
    if (dataDir.isEmpty())
        return 3;
    // SQLite spills statement journals and big sorts to %TEMP%, which the
    // low integrity sandbox cannot write (the 1.0.0 index upgrade failed
    // with "unable to open database file"). The data folder's low label is
    // inherited; the worker started later inherits these variables.
    const QString temp = QDir(dataDir).filePath(u"tmp"_s);
    if (QDir().mkpath(temp)) {
        // Wide: SQLite reads it with GetTempPathW; the path may not fit the code page.
        const std::wstring native = QDir::toNativeSeparators(temp).toStdWString();
        ::SetEnvironmentVariableW(L"TMP", native.c_str());
        ::SetEnvironmentVariableW(L"TEMP", native.c_str());
    }
    QTranslator translator;
    if (appearance.value(u"locale"_s).toString() == u"en" && translator.load(u":/i18n/ocr-search_en.qm"_s))
        QCoreApplication::installTranslator(&translator);
    Controller editor(appDir, dataDir);
    QQmlApplicationEngine engine;
    engine.addImportPath(appDir + u"/qml"_s);
    engine.addImageProvider(u"ocrpreview"_s, editor.imageProvider());
    engine.addImageProvider(u"ocrappicon"_s, new AppIcon(QDir(appDir).filePath(u"WinShun.exe"_s)));
    ws::WindowFrame::setIcon(u"image://ocrappicon/app"_s);
    const auto applyAppearance = [&](const QJsonObject& look) {
        qputenv("WS_LOCALE", look.value(u"locale"_s).toString(u"zh"_s).toUtf8());
        const QString theme = look.value(u"themeSetting"_s).toString();
        QGuiApplication::styleHints()->setColorScheme(theme == u"dark" ? Qt::ColorScheme::Dark
                : theme == u"light"                                    ? Qt::ColorScheme::Light
                                                                       : Qt::ColorScheme::Unknown);
        ws::SystemTheme::setRoundedCorners(look.value(u"corners"_s).toString(u"round"_s) == u"round");
        ws::SystemTheme::setUiSize(look.value(u"uiSize"_s).toString(u"standard"_s));
        ws::SystemTheme::setUiStyle(look.value(u"uiStyle"_s).toString());
        QCoreApplication::removeTranslator(&translator);
        if (look.value(u"locale"_s).toString() == u"en" && translator.load(u":/i18n/ocr-search_en.qm"_s))
            QCoreApplication::installTranslator(&translator);
        engine.retranslate();
    };
    applyAppearance(appearance);
    ws::WindowFrame frame(true);
    QQuickWindow* window = nullptr;
    // The scan's walk, recognition results and the search's pages stay in the
    // working set long after the work; given back to Windows while idle (the
    // recognition itself runs in a worker process of its own per pass, gone
    // when the pass is over). Not in the preview or the worker.
    std::unique_ptr<ws::IdleTrim> idleTrim;
    if (!preview) {
        idleTrim = std::make_unique<ws::IdleTrim>(
            [&editor]() -> const char* { return editor.working(); },
            [&window] { return window && window->isVisible(); });
        QObject::connect(&editor, &Controller::refreshRequested, &app, [&idleTrim] { idleTrim->poke(); });
    }
    const auto showWindow = [&] {
        if (idleTrim)
            idleTrim->used();
        if (!window) {
            QQmlComponent component(&engine, QUrl(u"qrc:/qt/qml/WinShun/OcrSearchWindow.qml"_s));
            QObject* object = component.createWithInitialProperties(
                {{u"editor"_s, QVariant::fromValue(&editor)}, {u"frame"_s, QVariant::fromValue(&frame)}});
            window = qobject_cast<QQuickWindow*>(object);
            if (!window) {
                qWarning().noquote() << component.errorString();
                delete object;
                return false;
            }
            frame.setWindow(window);
            QObject::connect(window, &QQuickWindow::visibleChanged, &app, [&] {
                if (window->isVisible())
                    return;
                if (preview)
                    app.quit();
                else {
                    editor.releasePreview(); // up to 64 MB of decoded picture
                    if (idleTrim)
                        idleTrim->poke();
                }
            });
        }
        editor.wake();
        window->show();
        window->raise();
        window->requestActivate();
        return true;
    };
    std::unique_ptr<ws::captureplugin::Protocol> protocol;
    if (!preview) {
        protocol = std::make_unique<ws::captureplugin::Protocol>(pending);
        QObject::connect(protocol.get(), &ws::captureplugin::Protocol::closed, &app, &QCoreApplication::quit);
        QObject::connect(&editor, &Controller::setupRequested, &app, [&] { protocol->tool(u"show_ocr_setup"_s, {}); });
        QObject::connect(&editor, &Controller::refreshRequested, &app, [&] { protocol->notify(u"winshun/refresh"_s); });
        QObject::connect(protocol.get(), &ws::captureplugin::Protocol::notified, &app,
            [&](const QString& method, const QJsonObject& p) {
                if (method == u"appearance")
                    applyAppearance(p);
                else if (method == u"cancel")
                    editor.cancelQuery();
            });
        QObject::connect(protocol.get(), &ws::captureplugin::Protocol::requested, &app,
            [&](const QJsonValue& id, const QString& method, const QJsonObject& p) {
                if (method == u"query") {
                    if (idleTrim)
                        idleTrim->used(); // the pages of the search: kept while queries come
                    editor.externalQuery(p, [&, id](const QJsonObject& answer) { protocol->respond(id, answer); });
                } else if (method == u"showSettings") {
                    if (showWindow())
                        protocol->respond(id, QJsonObject {});
                    else
                        protocol->respondError(id, -32002, u"Cannot open the plugin window"_s);
                } else if (method == u"action") {
                    const auto item = p.value(u"item"_s).toObject();
                    const auto data = item.value(u"data"_s).toObject();
                    if (item.value(u"id"_s).toString() == u"manage") {
                        if (!showWindow()) {
                            protocol->respondError(id, -32002, u"Cannot open the plugin window"_s);
                            return;
                        }
                    } else {
                        const Hit hit = hitFromJson(data.value(u"hit"_s).toObject());
                        const QString action = p.value(u"action"_s).toString();
                        if (action == u"folder")
                            editor.showFolder(hit.path);
                        else {
                            if (!showWindow()) {
                                protocol->respondError(id, -32002, u"Cannot open the plugin window"_s);
                                return;
                            }
                            editor.showHit(hit, data.value(u"words"_s).toString());
                        }
                    }
                    protocol->respond(
                        id, QJsonObject {{u"effects"_s, QJsonArray {QJsonObject {{u"type"_s, u"hide"_s}}}}});
                } else if (method == u"shutdown") {
                    protocol->respond(id, QJsonObject {});
                    app.quit();
                } else
                    protocol->respondError(id, -32601, u"Unknown method"_s);
            });
        protocol->respond(
            first->value(u"id"_s), QJsonObject {{u"protocolVersion"_s, u"1"_s}, {u"name"_s, u"winshun.ocr-search"_s}});
    } else {
        if (!showWindow())
            return 4;
        if (option(u"--width"_s).toInt() > 0)
            window->setWidth(qMax(window->minimumWidth(), option(u"--width"_s).toInt()));
        if (option(u"--height"_s).toInt() > 0)
            window->setHeight(qMax(window->minimumHeight(), option(u"--height"_s).toInt()));
        if (!option(u"--folder"_s).isEmpty())
            editor.addPath(option(u"--folder"_s));
        if (!option(u"--exclude-folder"_s).isEmpty())
            editor.addPath(option(u"--exclude-folder"_s), true);
        const QString output = option(u"--output"_s);
        if (!output.isEmpty()) {
            QDir().mkpath(output);
            auto captured = std::make_shared<bool>(false);
            auto searched = std::make_shared<bool>(false);
            const QString words = option(u"--query"_s);
            if (auto* input = window->findChild<QObject*>(u"ocrSearchInput"_s))
                input->setProperty("text", words);
            QObject::connect(&editor, &Controller::changed, &app, [&, searched, words] {
                if (!editor.busy() && !*searched && !words.isEmpty()) {
                    *searched = true;
                    editor.search(words);
                }
            });
            QTimer* check = new QTimer(&app);
            check->setInterval(150);
            QObject::connect(check, &QTimer::timeout, &app, [&, output, captured, searched, words] {
                if (*captured || editor.busy()
                    || (!words.isEmpty()
                        && (!*searched || editor.searching() || editor.preview().value(u"loading"_s).toBool())))
                    return;
                auto* previewImage = window->findChild<QQuickItem*>(u"ocrPreviewImage"_s);
                if (previewImage && !editor.preview().value(u"url"_s).toString().isEmpty()
                    && previewImage->property("status").toInt() != 1)
                    return;
                QJsonObject controls;
                for (const auto& name : {u"ocrSearchInput"_s, u"startOcrIndex"_s, u"ocrPreviewImage"_s}) {
                    if (auto* item = window->findChild<QQuickItem*>(name)) {
                        const QRectF bounds = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
                        controls.insert(name,
                            QJsonObject {{u"x"_s, bounds.x()}, {u"y"_s, bounds.y()}, {u"width"_s, bounds.width()},
                                {u"height"_s, bounds.height()}});
                    }
                }
                *captured = true;
                const bool saved = window->grabWindow().save(QDir(output).filePath(u"window.png"_s));
                QFile report(QDir(output).filePath(u"report.json"_s));
                if (report.open(QIODevice::WriteOnly))
                    report.write(QJsonDocument(
                        QJsonObject {{u"screenshot"_s, saved}, {u"status"_s, editor.statusText()},
                            {u"devicePixelRatio"_s, window->devicePixelRatio()}, {u"controls"_s, controls},
                            {u"width"_s, window->width()}, {u"height"_s, window->height()},
                            {u"statistics"_s, QJsonObject::fromVariantMap(editor.statistics())},
                            {u"results"_s, QJsonArray::fromVariantList(editor.results())},
                            {u"preview"_s, QJsonObject::fromVariantMap(editor.preview())},
                            {u"errors"_s, editor.errors()}})
                            .toJson());
                if (!arguments.contains(u"--keep-open"_s))
                    app.exit(saved ? 0 : 5);
            });
            check->start();
            if (arguments.contains(u"--index"_s))
                QTimer::singleShot(100, &editor, [&] { editor.start(); });
            else {
                *searched = true;
                editor.search(words);
            }
            QTimer::singleShot(120000, &app, [&] { app.exit(9); });
        }
    }
    const int result = app.exec();
    delete window;
    return result;
}
