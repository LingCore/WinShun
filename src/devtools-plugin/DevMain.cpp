// winshun.devtools: the JSON tool. Resident and started when first wanted;
// the search box asks it about `json …` and about JSON pasted straight in,
// Enter copies a form or opens the window (a tree beside the text), a
// .json file's 用 JSON 工具查看 opens it there.
#include "JsonTool.h"
#include "Launcher.h"
#include "../capture-plugin/Protocol.h"
#include "FontFile.h"
#include "Phosphor.h"
#include "SystemTheme.h"
#include "WindowFrame.h"
#include "platform/IdleTrim.h"
#include "platform/WindowEffects.h"

#include <QClipboard>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeData>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QSettings>
#include <QStyleHints>
#include <QTimer>
#include <QTranslator>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

using namespace Qt::StringLiterals;
using namespace ws::devtools;

namespace {

struct Text {
    Q_DECLARE_TR_FUNCTIONS(DevTools)
};

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

// Qt's own stderr logging writes the ANSI code page; the host reads UTF-8.
void logToStderrAsUtf8(QtMsgType type, const QMessageLogContext& context, const QString& text)
{
    const HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
    if (!err || err == INVALID_HANDLE_VALUE)
        return;
    const QByteArray line = qFormatLogMessage(type, context, text).toUtf8() + '\n';
    DWORD put = 0;
    ::WriteFile(err, line.constData(), static_cast<DWORD>(line.size()), &put, nullptr);
}

QString indentOf(const QJsonObject& settings)
{
    const QString chosen = settings.value(u"indent"_s).toString();
    return chosen == u"4" ? u"    "_s : chosen == u"tab" ? u"\t"_s : u"  "_s;
}

QString clipboardText()
{
    const QMimeData* data = QGuiApplication::clipboard()->mimeData();
    return data && data->hasText() ? data->text() : QString();
}

// What a query or an action is about: the words after the keyword, else
// the clipboard (never for the AI or `ws`: the clipboard is the user's
// alone); the whole input for JSON pasted into the box.
struct Asked {
    QString text;
    From from = From::Typed;
    bool full = true;
};

Asked asked(const QJsonObject& params)
{
    const QString text = params.value(u"text"_s).toString();
    if (params.value(u"trigger"_s).toString() == u"regex")
        return {text, From::Pasted, false};
    if (!text.trimmed().isEmpty())
        return {text, From::Typed, true};
    if (params.value(u"source"_s).toString() == u"command")
        return {QString(), From::Typed, true};
    return {clipboardText(), From::Clipboard, true};
}

// The last text asked about, read once for its rows, its Enter and its
// preview: typing after `json` asks again at every key, and a big
// clipboard is not read and written out again for each.
class Answers {
public:
    const Reading& reading(const QString& text)
    {
        if (!m_reading || text != m_text) {
            m_text = text;
            m_reading = read(text);
            m_rows = {};
        }
        return *m_reading;
    }
    QJsonArray rows(const Asked& a, const QString& indent)
    {
        const Reading& r = reading(a.text);
        const QString key = u"%1/%2/%3"_s.arg(int(a.from)).arg(a.full).arg(indent);
        if (m_rowsKey != key || m_rows.isEmpty()) {
            m_rowsKey = key;
            m_rows = ws::devtools::rows(a.text, a.from, r, a.full, indent);
        }
        return m_rows;
    }
    void forget()
    {
        m_reading.reset();
        m_text.clear();
        m_rows = {};
    }

private:
    QString m_text;
    std::optional<Reading> m_reading;
    QString m_rowsKey;
    QJsonArray m_rows;
};

QJsonObject effects(std::initializer_list<QJsonObject> list)
{
    QJsonArray out;
    for (const QJsonObject& e : list)
        out.append(e);
    return {{u"effects"_s, out}};
}

} // namespace

extern "C" __declspec(dllexport) int winshunDevToolsMain(const wchar_t* host, int mode)
{
    const bool preview = mode == 1;
    const QString appDir = QString::fromWCharArray(host);
    qInstallMessageHandler(logToStderrAsUtf8);
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
    if (!preview) {
        first = ws::captureplugin::Protocol::readFirst(&pending);
        if (!first || first->value(u"method"_s).toString() != u"initialize")
            return 2;
    }
    const QJsonObject params = first ? first->value(u"params"_s).toObject() : QJsonObject {};
    QJsonObject appearance = params.value(u"appearance"_s).toObject();
    if (preview)
        appearance = {{u"themeSetting"_s, option(u"--theme"_s)}, {u"locale"_s, option(u"--locale"_s)},
            {u"uiStyle"_s, option(u"--style"_s)}};
    const bool software
        = appearance.value(u"renderer"_s).toString() == u"software" || arguments.contains(u"--software"_s);
    qputenv("QT_QPA_PLATFORM", software ? "windows:fontengine=gdi" : "windows:fontengine=freetype");
    QCoreApplication::setLibraryPaths({appDir});
    QCoreApplication::setApplicationName(u"WinShunDevTools"_s);
    int argc = 1;
    char name[] = "WinShunDevTools";
    char* argv[] = {name, nullptr};
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationDisplayName(u"Win顺"_s);
    QGuiApplication::setQuitOnLastWindowClosed(false);
    QQuickWindow::setGraphicsApi(software ? QSGRendererInterface::Software : QSGRendererInterface::Direct3D11);
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    QImageReader::setAllocationLimit(64);
    ws::SystemTheme::setBackdropAvailable(false);
    ws::phosphor::loadFonts();
    const auto families = QFontDatabase::applicationFontFamilies(
        ws::addFontFromFile(QDir(appDir).filePath(u"fonts/AlibabaPuHuiTi-3-55-Regular.ttf"_s)));
    if (!families.isEmpty()) {
        QFont f(families.first());
        f.setHintingPreference(QFont::PreferVerticalHinting);
        QGuiApplication::setFont(f);
    }
    // Where the window's size is kept; a preview without --data-dir keeps none.
    const QString dataDir
        = preview ? option(u"--data-dir"_s) : params.value(u"dataDir"_s).toString(qEnvironmentVariable("WS_DATA_DIR"));
    QTranslator translator;
    JsonTool tool;
    QString indent = indentOf(params.value(u"settings"_s).toObject());
    if (preview && !option(u"--indent"_s).isEmpty())
        indent = indentOf({{u"indent"_s, option(u"--indent"_s)}});
    tool.setIndent(indent);
    Answers answers;

    QQmlApplicationEngine engine;
    engine.addImportPath(appDir + u"/qml"_s);
    engine.addImageProvider(u"devappicon"_s, new AppIcon(QDir(appDir).filePath(u"WinShun.exe"_s)));
    ws::WindowFrame::setIcon(u"image://devappicon/app"_s);
    QQuickWindow* window = nullptr;
    const auto applyAppearance = [&](const QJsonObject& look) {
        qputenv("WS_LOCALE", look.value(u"locale"_s).toString(u"zh"_s).toUtf8());
        const QString theme = look.value(u"themeSetting"_s).toString();
        QGuiApplication::styleHints()->setColorScheme(theme == u"dark" ? Qt::ColorScheme::Dark
                : theme == u"light"                                    ? Qt::ColorScheme::Light
                                                                       : Qt::ColorScheme::Unknown);
        ws::SystemTheme::setRoundedCorners(look.value(u"corners"_s).toString(u"round"_s) == u"round");
        ws::SystemTheme::setUiSize(look.value(u"uiSize"_s).toString(u"standard"_s));
        ws::SystemTheme::setUiStyle(look.value(u"uiStyle"_s).toString());
        ws::SystemTheme::setGlassAppearance(look.value(u"glassTransparency"_s).toInt(50), look.value(u"glassClarity"_s).toInt(50));
        ws::SystemTheme::setHoverTips(look.value(u"hoverTips"_s).toBool(true));
        QCoreApplication::removeTranslator(&translator);
        if (look.value(u"locale"_s).toString() == u"en" && translator.load(u":/i18n/devtools_en.qm"_s))
            QCoreApplication::installTranslator(&translator);
        engine.retranslate();
        if (window)
            ws::win::setDarkFrame(window, QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
    };
    applyAppearance(appearance);

    ws::WindowFrame frame(true);
    const QString geometryFile = dataDir.isEmpty() ? QString() : QDir(dataDir).filePath(u"window.ini"_s);
    // The text and the tree of a closed window, and the window's graphics,
    // given back to Windows while it stays closed (not in the preview).
    std::unique_ptr<ws::IdleTrim> idleTrim;
    if (!preview)
        idleTrim = std::make_unique<ws::IdleTrim>([]() -> const char* { return nullptr; },
            [&window] { return window && window->isVisible(); });

    const auto saveGeometry = [&] {
        if (!window || geometryFile.isEmpty())
            return;
        QSettings kept(geometryFile, QSettings::IniFormat);
        const bool maximized = window->windowStates().testFlag(Qt::WindowMaximized);
        kept.setValue(u"maximized"_s, maximized);
        if (!maximized)
            kept.setValue(u"size"_s, window->size());
        kept.setValue(u"split"_s, window->property("split"));
        kept.setValue(u"textShown"_s, window->property("textShown"));
    };
    // On the screen with the pointer, in its middle, at the size it had.
    const auto place = [&] {
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        QSize size = window->size();
        bool maximized = false;
        if (!geometryFile.isEmpty()) {
            QSettings kept(geometryFile, QSettings::IniFormat);
            size = kept.value(u"size"_s, size).toSize();
            maximized = kept.value(u"maximized"_s, false).toBool();
        }
        const QRect area = screen->availableGeometry();
        size = size.boundedTo(area.size() - QSize(32, 32)).expandedTo(window->minimumSize());
        window->setScreen(screen);
        window->setGeometry(QRect(area.center() - QPoint(size.width() / 2, size.height() / 2), size));
        if (maximized)
            window->setWindowStates(Qt::WindowMaximized);
    };
    const auto showWindow = [&] {
        if (idleTrim)
            idleTrim->used();
        if (!window) {
            QVariantMap initial {{u"tool"_s, QVariant::fromValue(&tool)}, {u"frame"_s, QVariant::fromValue(&frame)}};
            if (!geometryFile.isEmpty()) {
                QSettings kept(geometryFile, QSettings::IniFormat);
                if (kept.contains(u"split"_s))
                    initial.insert(u"split"_s, kept.value(u"split"_s).toReal());
                if (kept.contains(u"textShown"_s))
                    initial.insert(u"textShown"_s, kept.value(u"textShown"_s).toBool());
            }
            QQmlComponent component(&engine, QUrl(u"qrc:/qt/qml/WinShun/JsonWindow.qml"_s));
            QObject* object = component.createWithInitialProperties(initial);
            window = qobject_cast<QQuickWindow*>(object);
            if (!window) {
                qWarning().noquote() << component.errorString();
                delete object;
                return false;
            }
            window->create();
            frame.setWindow(window);
            ws::win::styleFramelessWindow(window, false);
            ws::win::setDarkFrame(window, QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
            QObject::connect(window, &QQuickWindow::visibleChanged, &app, [&] {
                if (window->isVisible())
                    return;
                if (preview) {
                    app.quit();
                    return;
                }
                saveGeometry();
                tool.release();
                if (idleTrim)
                    idleTrim->poke();
            });
        }
        if (!window->isVisible())
            place();
        window->show();
        window->raise();
        window->requestActivate();
        QMetaObject::invokeMethod(window, "focusTree");
        return true;
    };

    std::unique_ptr<ws::captureplugin::Protocol> protocol;
    if (!preview) {
        protocol = std::make_unique<ws::captureplugin::Protocol>(pending);
        QObject::connect(protocol.get(), &ws::captureplugin::Protocol::closed, &app, &QCoreApplication::quit);
        QObject::connect(protocol.get(), &ws::captureplugin::Protocol::notified, &app,
            [&](const QString& method, const QJsonObject& p) {
                if (method == u"appearance")
                    applyAppearance(p);
                else if (method == u"settingsChanged") {
                    indent = indentOf(p.value(u"settings"_s).toObject());
                    tool.setIndent(indent);
                    answers.forget();
                }
            });
        QObject::connect(protocol.get(), &ws::captureplugin::Protocol::requested, &app,
            [&](const QJsonValue& id, const QString& method, const QJsonObject& p) {
                if (method == u"query") {
                    const Asked a = asked(p);
                    protocol->respond(id, QJsonObject {{u"items"_s, answers.rows(a, indent)}, {u"cache"_s, 0}});
                } else if (method == u"details") {
                    const Asked a = asked(p);
                    const Reading& r = answers.reading(a.text);
                    QString text;
                    if (!r.document.isEmpty())
                        text = write(r.document, 0, Layout::Pretty, indent);
                    else if (r.error)
                        text = Text::tr("JSON 有错误：%1（第 %2 行第 %3 列）").arg(r.error->message).arg(r.error->line).arg(r.error->column)
                            + u"\n\n"_s + a.text;
                    protocol->respond(id, QJsonObject {{u"text"_s, text}});
                } else if (method == u"action") {
                    if (p.value(u"trigger"_s).toString() == u"files") {
                        const QString path = p.value(u"files"_s).toArray().at(0).toString();
                        QString why;
                        const std::optional<QString> text = readTextFile(path, &why);
                        if (!text) {
                            protocol->respond(id, effects({{{u"type"_s, u"notify"_s}, {u"text"_s, why}}, {{u"type"_s, u"keep"_s}}}));
                            return;
                        }
                        tool.open(*text, QFileInfo(path).fileName());
                        if (!showWindow()) {
                            protocol->respondError(id, -32002, u"Cannot open the plugin window"_s);
                            return;
                        }
                        protocol->respond(id, effects({{{u"type"_s, u"hide"_s}}}));
                        return;
                    }
                    const QString form = p.value(u"item"_s).toObject().value(u"data"_s).toObject().value(u"do"_s).toString();
                    const Asked a = asked(p);
                    if (form == u"view" || form.isEmpty()) {
                        // Nothing to show: the window as it was, its text kept.
                        if (!a.text.trimmed().isEmpty())
                            tool.open(a.text);
                        if (!showWindow()) {
                            protocol->respondError(id, -32002, u"Cannot open the plugin window"_s);
                            return;
                        }
                        protocol->respond(id, effects({{{u"type"_s, u"hide"_s}}}));
                        return;
                    }
                    const QString text = copied(answers.reading(a.text), form, indent);
                    if (text.isEmpty())
                        protocol->respond(id, effects({{{u"type"_s, u"notify"_s}, {u"text"_s, Text::tr("这段文字不是有效的 JSON")}},
                            {{u"type"_s, u"keep"_s}}}));
                    else
                        protocol->respond(id, effects({{{u"type"_s, u"copy"_s}, {u"text"_s, text}}, {{u"type"_s, u"hide"_s}}}));
                } else if (method == u"showSettings") {
                    if (showWindow())
                        protocol->respond(id, QJsonObject {});
                    else
                        protocol->respondError(id, -32002, u"Cannot open the plugin window"_s);
                } else if (method == u"shutdown") {
                    saveGeometry();
                    protocol->respond(id, QJsonObject {});
                    app.quit();
                } else
                    protocol->respondError(id, -32601, u"Unknown method"_s);
            });
        protocol->respond(
            first->value(u"id"_s), QJsonObject {{u"protocolVersion"_s, u"1"_s}, {u"name"_s, u"winshun.devtools"_s}});
    } else {
        // --input <file> | --text <json>, --search <words>, --output <folder>:
        // window.png and report.json, then quit (--keep-open stays).
        if (!option(u"--input"_s).isEmpty()) {
            QString why;
            const std::optional<QString> text = readTextFile(option(u"--input"_s), &why);
            if (!text) {
                qWarning().noquote() << why;
                return 3;
            }
            tool.open(*text, QFileInfo(option(u"--input"_s)).fileName());
        } else if (arguments.contains(u"--text"_s))
            tool.open(option(u"--text"_s));
        if (!showWindow())
            return 4;
        if (option(u"--width"_s).toInt() > 0 || option(u"--height"_s).toInt() > 0) {
            window->setWindowStates(Qt::WindowNoState);
            window->resize(qMax(window->minimumWidth(), option(u"--width"_s).toInt() > 0 ? option(u"--width"_s).toInt() : window->width()),
                qMax(window->minimumHeight(), option(u"--height"_s).toInt() > 0 ? option(u"--height"_s).toInt() : window->height()));
        }
        if (arguments.contains(u"--text-hidden"_s))
            window->setProperty("textShown", false);
        // Once the size above has reached the window: a row shown for the
        // old height could be below the new one.
        QTimer::singleShot(300, &app, [&] {
            if (!option(u"--search"_s).isEmpty())
                QMetaObject::invokeMethod(window, "find", Q_ARG(QVariant, option(u"--search"_s)));
            if (!option(u"--select"_s).isEmpty())
                QMetaObject::invokeMethod(window, "selectRow", Q_ARG(QVariant, option(u"--select"_s).toInt()));
        });
        const QString output = option(u"--output"_s);
        if (!output.isEmpty()) {
            QDir().mkpath(output);
            QTimer::singleShot(900, &app, [&, output] {
                const bool saved = window->grabWindow().save(QDir(output).filePath(u"window.png"_s));
                QJsonObject controls;
                for (const auto& name : {u"jsonText"_s, u"jsonTree"_s, u"jsonSearch"_s, u"jsonToolbar"_s, u"jsonFooter"_s}) {
                    if (auto* item = window->findChild<QQuickItem*>(name)) {
                        const QRectF bounds = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
                        controls.insert(name, QJsonObject {{u"x"_s, bounds.x()}, {u"y"_s, bounds.y()},
                                                  {u"width"_s, bounds.width()}, {u"height"_s, bounds.height()}});
                    }
                }
                QFile report(QDir(output).filePath(u"report.json"_s));
                if (report.open(QIODevice::WriteOnly))
                    report.write(QJsonDocument(QJsonObject {{u"screenshot"_s, saved},
                                                   {u"devicePixelRatio"_s, window->devicePixelRatio()},
                                                   {u"width"_s, window->width()}, {u"height"_s, window->height()},
                                                   {u"valid"_s, tool.valid()}, {u"empty"_s, tool.empty()},
                                                   {u"summary"_s, tool.summary()}, {u"notes"_s, tool.notes()},
                                                   {u"error"_s, tool.error()}, {u"errorLine"_s, tool.errorLine()},
                                                   {u"rows"_s, tool.tree()->rowCount()},
                                                   {u"matches"_s, tool.tree()->matchCount()},
                                                   {u"controls"_s, controls}})
                                     .toJson());
                if (!arguments.contains(u"--keep-open"_s))
                    app.exit(saved ? 0 : 5);
            });
            QTimer::singleShot(60000, &app, [&] { app.exit(9); });
        }
    }
    const int result = app.exec();
    delete window;
    return result;
}
