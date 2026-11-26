#include "Preview.h"
#include "PreviewInteractionProbe.h"
#include "NavigationProbe.h"
#include "ForegroundMemoryProbe.h"
#include "ForegroundUseProbe.h"
#include "IdleOpenProbe.h"
#include "ClipboardAnimationProbe.h"
#include "LauncherSettingsProbe.h"
#include "SearchShadowProbe.h"
#include "ServiceLocation.h"
#include "PastePlainProbe.h"
#include "SettingsMotionProbe.h"
#include "platform/AllocationTrace.h"
#include "InteractionOptimizationProbe.h"
#include "UiFont.h"

#include <QAccessible>

#include "HealthMonitor.h"
#include "ClipScrollBench.h"
#include "ContentPaneProbe.h"

#include "Backup.h"
#include "AiSettings.h"
#include "App.h"
#include "Clipboard.h"
#include "ClipboardDisplay.h"
#include "DiagnosticLog.h"
#include "DialogBar.h"
#include "AppCatalog.h"
#include "History.h"
#include "IndexService.h"
#include "Onboarding.h"
#include "Launcher.h"
#include "Placement.h"
#include "SystemCatalog.h"
#include "CliSetup.h"
#include "CommandLine.h"
#include "FavoritesSettings.h"
#include "FileIconProvider.h"
#include "FilePreview.h"
#include "PluginSettings.h"
#include "ResultModel.h"
#include "ThumbnailProvider.h"
#include "ClipThumbnailProvider.h"
#include "Settings.h"
#include "SettingsCommand.h"
#include "SettingsEditor.h"
#include "SystemTheme.h"
#include "Switches.h"
#include "Updater.h"
#include "WindowAvoidance.h"
#include "WindowFrame.h"
#include "DropPieHelper.h"
#include "DropPie.h"
#include "FolderMenu.h"
#include "Query.h"
#include "Host.h"
#include "Secrets.h"
#include "platform/Integrity.h"
#include "platform/SiteIcons.h"
#include "platform/FileDialog.h"
#include "platform/FileManagers.h"
#include "platform/Shell.h"
#include "platform/FolderViewHit.h"
#include "platform/TypeToSearch.h"
#include "platform/WindowEffects.h"
#include "platform/Foreground.h"
#include "platform/MessageWindow.h"
#include "platform/OcrFiles.h"
#include "platform/OcrPackage.h"
#include "miniz.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QPixmap>
#include <QCryptographicHash>
#include <QProcess>
#include <QRegularExpression>
#include <QJsonArray>
#include <QBuffer>
#include <QThread>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QLibraryInfo>
#include <QLocale>
#include <QMutex>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQmlProperty>
#include <QAbstractEventDispatcher>
#include <QAbstractNativeEventFilter>
#include <QEventLoop>
#include <QQuickItem>
#include <QQuickWindow>
#include <stdexcept>
#include <QScreen>
#include <QScopeGuard>
#include <QSettings>
#include <QStyleHints>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QtMath>
#include <QTimer>
#include <QTime>
#include <QTranslator>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <thread>

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <ncrypt.h>
#include <ole2.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <exdisp.h>
#include <uiautomation.h>
#include <shlobj.h>
#include <wrl/client.h>

using namespace Qt::StringLiterals;

QT_BEGIN_NAMESPACE
// QtGui's own, exported for QTest (qtestmouse.h): mouse input through
// QGuiApplication, as the system's comes.
Q_GUI_EXPORT void qt_handleMouseEvent(QWindow* window, const QPointF& local, const QPointF& global, Qt::MouseButtons state,
    Qt::MouseButton button, QEvent::Type type, Qt::KeyboardModifiers mods, int timestamp);
QT_END_NAMESPACE

namespace ws::preview {

// Clipboard's paste as far as what it would put on the clipboard
// (Clipboard::startPaste), let go before its timer could write it or press
// a key: the native clipboard untouched.
struct PasteWriteProbe {
    static ClipWrite takeWrite(Clipboard& clipboard)
    {
        clipboard.m_keysTimer.stop();
        clipboard.m_pasting = false;
        clipboard.m_pasteGuard.reset();
        return std::exchange(clipboard.m_write, {});
    }
};

namespace {

constexpr auto kFlag = u"--preview-plugins";
constexpr auto kSettingsFlag = u"--preview-settings";
constexpr auto kResultsFlag = u"--preview-results";
constexpr auto kAiCardsFlag = u"--preview-ai-cards";
constexpr auto kRuntimeFlag = u"--probe-runtime";
constexpr int kFolderTypingMany = 40; // --folder-typing's many_NN.txt: more rows than its box shows

int g_warnings = 0;
bool g_edges = false; // --edges (or --tree): every grab also writes <png>.edges.txt
QtMessageHandler g_previous = nullptr;
QString g_logPath; // the run is elevated, its console cannot be read

void say(const QString& line)
{
    std::fprintf(stderr, "%s\n", qUtf8Printable(line));
    if (g_logPath.isEmpty())
        return;
    QFile file(g_logPath);
    if (file.open(QIODevice::Append | QIODevice::Text))
        file.write((line + u'\n').toUtf8());
}

void countWarnings(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    const bool offscreenSizeHint = QGuiApplication::platformName() == u"offscreen"
        && message == u"This plugin does not support propagateSizeHints()";
    if (!offscreenSizeHint && (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg))
        ++g_warnings;
    if (g_previous)
        g_previous(type, context, message);
    say((type == QtDebugMsg || type == QtInfoMsg ? u""_s : u"WARNING: "_s) + message);
}

QString valueAfter(const QStringList& args, QStringView flag, const QString& fallback = {})
{
    const qsizetype at = args.indexOf(flag);
    return at >= 0 && at + 1 < args.size() ? args[at + 1] : fallback;
}

void pump(int ms)
{
    QElapsedTimer timer;
    timer.start();
    do
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    while (timer.elapsed() < ms);
}

// The left button pressed, moved with or let go at `at` (in the window), as
// the system's mouse comes: through QGuiApplication, which gives the point
// its press position and keeps its grabs. A QMouseEvent sent to the window
// has no press position, so a DragHandler takes the press itself for a drag
// already made from (0, 0) and loses it to the MouseArea under it.
void systemMouse(QWindow* window, QEvent::Type type, QPointF at)
{
    static int timestamp = 0;
    timestamp += type == QEvent::MouseButtonRelease ? 1000 : 16; // a press after a release is no double click
    const Qt::MouseButtons buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    qt_handleMouseEvent(window, at, window->mapToGlobal(at), buttons, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type, Qt::NoModifier, timestamp);
}

// A window off the screen gets no resize or expose from Windows, so its
// content item would stay 0x0 (and every `parent.width` with it): sized
// here, more than once, as the height follows the width through the
// wrapping. Items are laid out (polished) only for a frame, and a frame
// comes only with a grab here: a grab per pass, thrown away, until the
// height the page asks for is the window's.
void settle(QQuickWindow* window, int passes = 4)
{
    QPointer<QQuickWindow> liveWindow(window);
    if (!liveWindow)
        return;
    for (int pass = 0; pass < passes; ++pass) {
        window->contentItem()->setSize(QSizeF(window->width(), window->height()));
        pump(200);
        if (!liveWindow)
            return; // a queued close must not leave the probe grabbing a deleted window
        (void)window->grabWindow();
        if (window->contentItem()->height() == window->height() && pass > 0)
            break;
    }
}

// The items with their places and sizes, for finding what is laid out
// wrong: "<png>.tree.txt".
void writeTree(QQuickWindow* window, const QString& path)
{
    QFile tree(path);
    if (!tree.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return;
    std::function<void(QQuickItem*, int)> walk = [&](QQuickItem* item, int depth) {
        if (depth > 14)
            return;
        const QString name = QString::fromLatin1(item->metaObject()->className()).section(u'_', 0, 0);
        tree.write(u"%1%2 x=%3 y=%4 w=%5 h=%6 iw=%7 ih=%8%9\n"_s.arg(QString(depth * 2, u' '), name).arg(item->x()).arg(item->y())
                       .arg(item->width()).arg(item->height()).arg(item->implicitWidth()).arg(item->implicitHeight())
                       .arg(item->isVisible() ? QString() : u" (hidden)"_s)
                       .toUtf8());
        for (QQuickItem* child : item->childItems())
            walk(child, depth + 1);
    };
    walk(window->contentItem(), 0);
}

// Every visible Rectangle with an edge that is not on a whole device pixel
// (what a 1 px line, a border or a child inset by a border looks like at 150 %),
// in device pixels: "<png>.edges.txt". Thin ones (a separator) and ones with a
// border or a radius are the ones that show.
void writeEdges(QQuickWindow* window, const QString& path)
{
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return;
    const qreal dpr = window->effectiveDevicePixelRatio();
    const auto fraction = [](qreal v) { return std::abs(v - std::round(v)); };
    std::function<void(QQuickItem*, int, const QString&)> walk = [&](QQuickItem* item, int depth, const QString& trail) {
        if (!item->isVisible() || item->opacity() <= 0 || depth > 40)
            return;
        const QString name = QString::fromLatin1(item->metaObject()->className()).section(u'_', 0, 0);
        const QString here = trail + u"/"_s + (item->objectName().isEmpty() ? name : item->objectName());
        if (name == u"QQuickRectangle" && item->width() > 0 && item->height() > 0) {
            const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
            const qreal l = r.left() * dpr, t = r.top() * dpr, rt = r.right() * dpr, b = r.bottom() * dpr;
            qreal bw = 0;
            if (QObject* pen = item->property("border").value<QObject*>())
                bw = pen->property("width").toReal();
            const qreal alpha = item->property("color").value<QColor>().alphaF();
            const bool edge = fraction(l) > 0.02 || fraction(t) > 0.02 || fraction(rt) > 0.02 || fraction(b) > 0.02;
            if (edge && (alpha > 0.02 || bw > 0)) {
                out.write(u"l=%1 t=%2 r=%3 b=%4 | %5x%6 dev | border %7 radius %8 alpha %9 | %10 (qml y=%11 h=%12)"_s
                              .arg(l, 0, 'f', 2).arg(t, 0, 'f', 2).arg(rt, 0, 'f', 2).arg(b, 0, 'f', 2)
                              .arg(rt - l, 0, 'f', 2).arg(b - t, 0, 'f', 2).arg(bw, 0, 'f', 2)
                              .arg(item->property("radius").toReal()).arg(alpha, 0, 'f', 2).arg(here)
                              .arg(item->y(), 0, 'f', 3).arg(item->height(), 0, 'f', 3)
                              .toUtf8() + '\n');
            }
        }
        for (QQuickItem* child : item->childItems())
            walk(child, depth + 1, here);
    };
    walk(window->contentItem(), 0, QString());
}

// `crop` (logical pixels, none: all of it): the device pixels it covers,
// outwards. A window bigger than what it draws (the drop pie's covers the work
// area) is cut down to that.
QRect cropPixels(QQuickWindow* window, const QRectF& crop)
{
    const qreal dpr = window->devicePixelRatio();
    return QRectF(crop.topLeft() * dpr, crop.size() * dpr).toAlignedRect();
}

bool saveGrab(QQuickWindow* window, const QString& path, const QRectF& crop = {})
{
    QImage image = window->grabWindow();
    if (!crop.isEmpty() && !image.isNull())
        image = image.copy(cropPixels(window, crop).intersected(image.rect()));
    if (g_edges)
        writeEdges(window, path + u".edges.txt"_s);
    const bool saved = !image.isNull() && image.save(path, "PNG");
    say(u"%1 %2 %3x%4"_s.arg(saved ? u"saved"_s : u"NOT saved"_s, path).arg(image.width()).arg(image.height()));
    return saved;
}

// Should anything hang (a renderer waiting for a frame that never comes),
// the log says so and the process ends.
void armWatchdog(QTimer& watchdog, int seconds)
{
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [seconds] {
        say(u"timeout: gave up after %1 s"_s.arg(seconds));
        std::_Exit(5);
    });
    watchdog.start(seconds * 1000);
}

// Off the screen, never activated: the software renderer draws what is
// shown, and the grab takes the frame.
void showOffScreen(QQuickWindow* window)
{
    window->setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    window->setProperty("_q_showWithoutActivating", true);
    window->setMaximumHeight(32000); // taller than the screen, for a whole page at a time
    // Win32 window coordinates are limited to 32767 physical pixels. Keep
    // the offscreen position below that even at 200% and larger scales.
    const int offscreen = static_cast<int>(std::floor(24000 / window->devicePixelRatio()));
    window->setPosition(offscreen, offscreen);
    window->show();
}

int finish(QQuickWindow* window, bool saved, QObject* controller = nullptr)
{
    window->hide();
    delete window;
    delete controller; // stop profile watchers before removing their directories
    say(u"%1 warning(s)"_s.arg(g_warnings));
    return saved ? std::min(g_warnings, 100) : 4;
}

// ---- --preview-plugins ------------------------------------------------------------------

QQuickItem* visualNamed(QQuickItem* item, const QString& name);

int runPlugins(const QStringList& args)
{
    const QString out = valueAfter(args, kFlag);
    const bool dark = args.contains(u"--dark"_s);
    const bool dialog = args.contains(u"--dialog"_s);
    const bool aiPage = valueAfter(args, u"--page") == u"ai"; // the AI 助手 page instead (its plugin switches)
    const bool allow = args.contains(u"--dialog-allow"_s); // the 更新并允许 question instead
    const bool aside = args.contains(u"--dialog-aside"_s); // the question over a folder that is no plugin (GitHub #253)
    const bool manyLogs = args.contains(u"--log-many"_s); // the developer log with lines from more plugins than fit a row of tabs (GitHub #256)
    const bool updatesOnly = args.contains(u"--updates"_s); // the 插件库 tab as the badge leads to it

    say(u"starting (dark %1, dialog %2)"_s.arg(dark).arg(dialog));
    QQmlApplicationEngine engine;
    engine.addImageProvider(u"fileicon"_s, new FileIconProvider);
    Settings settings = Settings::defaults();
    if (aiPage) { // every row on show: the switches above the plugins' ones on
        settings.mcp = true;
        settings.mcpPlugins = true;
    }
    // --keyword-check (GitHub #282): a real catalog over one plugin that
    // starts nothing (its command opens an address), in the preview's own
    // profile, so that what is typed in a card's keyword box is followed to
    // the INI and to the index; told of the settings as App::applyPlugins does.
    const bool keywordCheck = args.contains(u"--keyword-check"_s);
    const QString wordsPlugin = u"test.words"_s;
    std::unique_ptr<plugin::Host> wordsHost;
    if (keywordCheck) {
        const QString folder = Settings::profile() + u"/plugins/"_s + wordsPlugin;
        QDir().mkpath(folder);
        QFile manifest(folder + u"/plugin.json"_s);
        const QJsonObject command {{u"id"_s, u"main"_s}, {u"title"_s, u"翻译"_s}, {u"keyword"_s, QJsonArray {u"tr"_s, u"fy"_s}},
            {u"run"_s, QJsonObject {{u"url"_s, u"https://example.com/?q=%s"_s}}}};
        const QJsonObject plugin {{u"manifest"_s, 1}, {u"id"_s, wordsPlugin}, {u"version"_s, u"1.0.0"_s},
            {u"name"_s, QJsonObject {{u"zh"_s, u"翻译"_s}, {u"en"_s, u"Translate"_s}}}, {u"commands"_s, QJsonArray {command}}};
        if (!manifest.open(QIODevice::WriteOnly) || manifest.write(QJsonDocument(plugin).toJson()) < 0) {
            say(u"#282 keywords: the plugin could not be written"_s);
            return 3;
        }
        manifest.close();
        settings.pluginConfig(wordsPlugin).enabled = true;
        plugin::Host::Options options;
        options.userPluginDir = Settings::profile() + u"/plugins"_s;
        options.version = QStringLiteral(WINSHUN_VERSION);
        wordsHost = std::make_unique<plugin::Host>(options);
        wordsHost->catalog().scan();
        wordsHost->setEnabled(wordsPlugin, true);
    }
    // --slider-check (GitHub #359): the same over one plugin whose numbers
    // have ranges; its card's sliders moved by keys and by the pointer, its
    // boxes typed into, a value sent by the plugin; the INI, what the plugin
    // is told and the card read each time.
    const bool sliderCheck = args.contains(u"--slider-check"_s) && !keywordCheck;
    const QString rangesPlugin = u"test.ranges"_s;
    const QString hiddenOnlyPlugin = u"test.hiddenonly"_s;
    std::unique_ptr<plugin::Host> rangesHost;
    if (sliderCheck) {
        const QString folder = Settings::profile() + u"/plugins/"_s + rangesPlugin;
        QDir().mkpath(folder);
        QFile manifest(folder + u"/plugin.json"_s);
        const QByteArray json = R"({"manifest":1,"id":"test.ranges","name":{"zh":"范围","en":"Ranges"},"version":"1.0.0",
            "commands":[{"id":"main","title":"范围","keyword":"rg","run":{"folder":"C:\\"}}],
            "settings":[
                {"key":"pause","type":"number","title":"停顿多久再翻译","default":400,"min":100,"max":2000,"step":50,"unit":"ms"},
                {"key":"match","type":"number","title":"术语匹配的相似度","default":0.75,"min":0,"max":1,"step":0.05},
                {"key":"edge","type":"number","title":"最后一步短一些","min":5,"max":99,"step":10,"unit":{"zh":"个","en":"items"}},
                {"key":"limit","type":"number","title":"最多列出几项","min":1,"unit":{"zh":"项","en":"items"}},
                {"key":"lines","type":"text","multiline":true,"title":"缩写","hint":"每行一条：缩写 = 换成的文字","default":"cmd = command"},
                {"key":"layout","type":"text","hidden":true,"title":"手势布局","default":"{}"}]})";
        if (!manifest.open(QIODevice::WriteOnly) || manifest.write(json) < 0) {
            say(u"#359 sliders: the plugin could not be written"_s);
            return 3;
        }
        manifest.close();
        // A plugin whose only setting is hidden (0.41.0): its card is one without settings.
        const QString hiddenOnlyFolder = Settings::profile() + u"/plugins/"_s + hiddenOnlyPlugin;
        QDir().mkpath(hiddenOnlyFolder);
        QFile hiddenOnly(hiddenOnlyFolder + u"/plugin.json"_s);
        const QByteArray hiddenJson = R"({"manifest":1,"id":"test.hiddenonly","name":{"zh":"只有隐藏项","en":"Hidden only"},"version":"1.0.0",
            "commands":[{"id":"main","title":"隐藏","keyword":"hd","run":{"folder":"C:\\"}}],
            "settings":[{"key":"layout","type":"text","hidden":true,"title":"手势布局","default":"{}"}]})";
        if (!hiddenOnly.open(QIODevice::WriteOnly) || hiddenOnly.write(hiddenJson) < 0) {
            say(u"hidden setting: the plugin could not be written"_s);
            return 3;
        }
        hiddenOnly.close();
        settings.pluginConfig(rangesPlugin).enabled = true;
        settings.pluginConfig(hiddenOnlyPlugin).enabled = true;
        plugin::Host::Options options;
        options.userPluginDir = Settings::profile() + u"/plugins"_s;
        options.version = QStringLiteral(WINSHUN_VERSION);
        rangesHost = std::make_unique<plugin::Host>(options);
        rangesHost->catalog().scan();
        rangesHost->setEnabled(rangesPlugin, true);
        rangesHost->setEnabled(hiddenOnlyPlugin, true);
    }
    auto* editor = new SettingsEditor(settings, u"software"_s, &engine);
    editor->plugins()->setLocale(u"zh"_s);
    if (keywordCheck) {
        editor->plugins()->setHost(wordsHost.get());
        QObject::connect(editor, &SettingsEditor::edited, wordsHost.get(), [&wordsHost](const Settings& now) {
            wordsHost->setKeywordOverrides(PluginSettings::keywordOverrides(now.plugins));
        });
    } else if (sliderCheck) {
        // The settings as App::applyPlugins tells the host: the INI's, fitted.
        const auto tell = [&rangesHost, rangesPlugin](const Settings& now) {
            const plugin::Entry* e = rangesHost->catalog().find(rangesPlugin);
            const PluginConfig* c = now.plugin(rangesPlugin);
            if (e && e->manifest)
                rangesHost->setPluginSettings(rangesPlugin,
                    plugin::secrets::settingsOf(rangesPlugin, *e->manifest, c ? c->settings : QList<std::pair<QString, QString>>()));
        };
        tell(settings);
        editor->plugins()->setHost(rangesHost.get());
        QObject::connect(editor, &SettingsEditor::edited, rangesHost.get(), tell);
    } else {
        editor->plugins()->loadPreview(args.contains(u"--elevated"_s), manyLogs ? 8 : 2, args.contains(u"--host-normal"_s)); // the `elevated` shield and 确认 note
    }
    if (updatesOnly)
        editor->plugins()->gallery()->setUpdatesOnly(true);
    if (aiPage)
        editor->setMcpStatus(u"已连接：Claude Desktop（12 次调用）"_s);
    say(u"loading the page"_s);

    QQmlComponent component(&engine, u"WinShun"_s, u"PluginsPreview"_s);
    QObject* object = component.createWithInitialProperties({
        {u"plugins"_s, QVariant::fromValue(editor->plugins())},
        {u"editor"_s, QVariant::fromValue(editor)},
        {u"dialogShown"_s, dialog},
        {u"allowShown"_s, allow},
        {u"asideShown"_s, aside},
        {u"aiPage"_s, aiPage},
        {u"opened"_s, args.contains(u"--open"_s)},
    });
    auto* window = qobject_cast<QQuickWindow*>(object);
    if (!window) {
        say(component.errorString());
        delete object;
        return 3;
    }
    QTimer watchdog;
    armWatchdog(watchdog, 60);
    showOffScreen(window);
    settle(window);
    say(u"showing %1x%2"_s.arg(window->width()).arg(window->height()));
    pump(500); // the bindings settle (sizes, the asynchronous icons)
    // A menu open (DropMenu): 添加插件's ("add"), or the "…" of the first card
    // ("card"), whose entries run over the cards below it. Each lives on the
    // window's content item (the cards' are not QObject children of the
    // window: a Repeater's delegates have no QObject parent).
    if (const QString menu = valueAfter(args, u"--menu"); !menu.isEmpty()) {
        if (menu == u"card") { // the first plugin opened: its card, with the "…", in place of the tiles
            if (QQuickItem* section = visualNamed(window->contentItem(), u"pluginsSection"_s))
                QMetaObject::invokeMethod(section, "expandAt", Q_ARG(QVariant, QVariant(0)));
            pump(300);
            settle(window);
        }
        QObject* chosen = nullptr;
        for (QQuickItem* o : window->contentItem()->childItems()) {
            if (!QString::fromLatin1(o->metaObject()->className()).startsWith(u"DropMenu"))
                continue;
            // 添加插件's drops from a button with that text; a card's from a "…".
            const auto* anchor = o->property("anchor").value<QObject*>();
            const bool add = anchor && !anchor->property("text").toString().isEmpty();
            if (!chosen && (menu == u"add" ? add : !add))
                chosen = o;
        }
        if (!chosen) {
            say(u"no menu %1"_s.arg(menu));
            return finish(window, false);
        }
        // Opened by clicks on its button, as a user does: open, closed again
        // (what a button opened, a second click closes), open for the picture.
        auto* anchor = qobject_cast<QQuickItem*>(chosen->property("anchor").value<QObject*>());
        const QPointF at = anchor->mapToScene(QPointF(anchor->width() / 2, anchor->height() / 2));
        const auto click = [&] {
            QMouseEvent press(QEvent::MouseButtonPress, at, window->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, {});
            QCoreApplication::sendEvent(window, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), Qt::LeftButton, {}, {});
            QCoreApplication::sendEvent(window, &release);
            pump(100);
            return chosen->property("open").toBool();
        };
        const bool first = click();
        const bool second = click();
        const bool third = click();
        say(u"clicks on the button: %1, %2, %3"_s.arg(first ? u"open"_s : u"closed"_s,
                                                       second ? u"open"_s : u"closed"_s,
                                                       third ? u"open"_s : u"closed"_s));
        if (!first || second || !third)
            return finish(window, false);
        if (args.contains(u"--pointer-check"_s)) {
            auto* menuItem = qobject_cast<QQuickItem*>(chosen);
            auto* highlight = chosen->findChild<QQuickItem*>(u"dropMenuHighlight"_s);
            const int count = engine.newQObject(chosen).property(u"entries"_s).property(u"length"_s).toInt();
            say(u"menu pointer probe: %1 entries, highlight %2"_s.arg(count).arg(highlight != nullptr));
            bool aligned = menuItem && highlight && count >= 2;
            for (const int index : {count - 1, 0, 1, count - 1, 0}) {
                auto* entry = visualNamed(menuItem, u"dropMenuEntry-%1"_s.arg(index));
                if (!entry || !highlight) { aligned = false; break; }
                const QPointF entryAt = entry->mapToScene({entry->width() / 2, entry->height() / 2});
                QMouseEvent move(QEvent::MouseMove, entryAt, window->mapToGlobal(entryAt), Qt::NoButton, {}, {});
                QCoreApplication::sendEvent(window, &move);
                pump(350);
                settle(window);
                const bool hit = chosen->property("hovered").toInt() == index;
                const bool painted = std::abs(entry->mapToScene({0, 0}).y() - highlight->mapToScene({0, 0}).y()) < 1;
                say(u"menu pointer row %1: hit=%2 painted=%3"_s.arg(index).arg(hit).arg(painted));
                aligned = aligned && hit && painted;
            }
            if (!aligned) return finish(window, false);
        }
    }
    // GitHub #230 (with --elevated): 确认 on the card not confirmed yet,
    // clicked as a user does; the sample's is refused: why, and 再试一次,
    // right there on that card.
    bool confirmed = true;
    if (args.contains(u"--confirm"_s)) {
        const std::function<QQuickItem*(QQuickItem*, const QString&)> shown = [&](QQuickItem* item, const QString& name) -> QQuickItem* {
            if (item->objectName() == name && item->isVisible())
                return item;
            for (QQuickItem* child : item->childItems())
                if (QQuickItem* found = shown(child, name))
                    return found;
            return nullptr;
        };
        QQuickItem* button = shown(window->contentItem(), u"elevationConfirm"_s);
        const QString before = button ? button->property("text").toString() : QString();
        if (button) {
            const QPointF at = button->mapToScene(QPointF(button->width() / 2, button->height() / 2));
            QMouseEvent press(QEvent::MouseButtonPress, at, window->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, {});
            QCoreApplication::sendEvent(window, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), Qt::LeftButton, {}, {});
            QCoreApplication::sendEvent(window, &release);
            pump(300);
            settle(window);
        }
        QQuickItem* problem = shown(window->contentItem(), u"elevationProblem"_s);
        button = shown(window->contentItem(), u"elevationConfirm"_s);
        const QString why = problem ? problem->property("text").toString() : QString();
        // Both on the one card, in its 管理员 part: the problem above the button.
        const bool together = problem && button && problem->parentItem() == button->parentItem()
            && button->mapToScene({}).y() > problem->mapToScene({}).y();
        confirmed = before == u"确认" && why.startsWith(u"没能确认：插件文件夹是一个链接") && button
            && button->property("text").toString() == u"再试一次" && together;
        say(u"#230 确认: before '%1', problem '%2', button '%3', together %4"_s
                .arg(before, why, button ? button->property("text").toString() : QString()).arg(together));
    }
    // --pointer-at <x>,<y>: the pointer put there (the page's own units) before
    // the picture is taken, never pressed: what the page looks like under it.
    if (const QStringList at = valueAfter(args, u"--pointer-at").split(u','); at.size() == 2) {
        const QPointF where(at[0].toDouble(), at[1].toDouble());
        QMouseEvent move(QEvent::MouseMove, where, window->mapToGlobal(where), Qt::NoButton, {}, {});
        QCoreApplication::sendEvent(window, &move);
        pump(400); // what follows a pointer (a tip's delay) has come
        say(u"pointer at %1,%2"_s.arg(where.x()).arg(where.y()));
    }
    // --homepage-check (GitHub #248): the library's entries offer 主页 where
    // the list gives an address of the web, and not where it gives none; its
    // tip would name where it leads; it is faint until the pointer is on it.
    // Found by name: the tree (--tree) stops a level above it. The pointer is
    // put over one and never pressed: pressed, it opens the browser.
    bool homepages = true;
    if (args.contains(u"--homepage-check"_s)) {
        const QStringList ids {u"lingcore.translate"_s, u"lingcore.kill"_s, u"lingcore.todo"_s, u"lingcore.clock"_s,
            u"winshun.bookmarks"_s, u"lingcore.clips"_s, u"lingcore.weather"_s, u"lingcore.timer"_s, u"lingcore.notes"_s};
        // PluginGallery::loadPreview gives the first none, the second one that is not an address of the web.
        const QStringList without {u"lingcore.clips"_s, u"lingcore.timer"_s};
        QStringList wrong;
        for (const QString& id : ids) {
            QQuickItem* button = visualNamed(window->contentItem(), u"pluginHomepage-"_s + id);
            if (!button)
                wrong.append(id + u": no button"_s);
            else if (button->isVisible() != !without.contains(id))
                wrong.append(id + (button->isVisible() ? u": shown without an address"_s : u": not shown"_s));
        }
        QQuickItem* button = visualNamed(window->contentItem(), u"pluginHomepage-winshun.bookmarks"_s);
        QString host;
        qreal faint = -1;
        qreal lit = -1;
        QString surfaceHovered = u"not found"_s;
        if (button) {
            for (QQuickItem* item = button; item && host.isEmpty(); item = item->parentItem())
                host = item->property("homepageHost").toString();
            faint = button->opacity();
            const QPointF at = button->mapToScene(QPointF(button->width() / 2, button->height() / 2));
            QMouseEvent move(QEvent::MouseMove, at, window->mapToGlobal(at), Qt::NoButton, {}, {});
            QCoreApplication::sendEvent(window, &move);
            pump(100);
            lit = button->opacity();
            // Told, not judged: whether the button's own surface took the
            // pointer too (its fill under the pointer is the style's to draw).
            for (QQuickItem* child : button->childItems()) {
                if (QString::fromLatin1(child->metaObject()->className()).startsWith(u"Hook"))
                    surfaceHovered = child->property("hovered").toBool() ? u"yes"_s : u"no"_s;
            }
        }
        homepages = wrong.isEmpty() && host == u"github.com" && qAbs(faint - 0.55) < 0.01 && qAbs(lit - 1) < 0.01;
        say(u"#248 主页: %1 of %2 entries as they should be%3; the tip's host '%4'; opacity %5, with the pointer on it %6: %7"_s
                .arg(ids.size() - wrong.size()).arg(ids.size())
                .arg(wrong.isEmpty() ? QString() : u" ("_s + wrong.join(u"; "_s) + u')')
                .arg(host).arg(faint).arg(lit).arg(homepages ? u"passed"_s : u"FAILED"_s));
        say(u"#248 主页: the button's own surface hovered under that pointer: %1"_s.arg(surfaceHovered));
        if (!homepages)
            qWarning("plugin homepage check failed");
    }
    // --keyword-check (GitHub #282): typed into the card's own box and left,
    // as a user does; each time the box, 默认, the note under the box, the
    // INI and the catalog's index are read.
    bool keywords = true;
    if (keywordCheck) {
        const QString key = wordsPlugin + u"/main"_s;
        if (QQuickItem* section = visualNamed(window->contentItem(), u"pluginsSection"_s))
            QMetaObject::invokeMethod(section, "expand", Q_ARG(QVariant, wordsPlugin));
        pump(600); // the card opens
        struct Seen {
            QString box;
            bool marked = false; // the box's error mark
            bool reset = false; // 默认 on show
            QString note; // why it was not taken, under the box
            QVariant kept; // the INI's Keyword.main; invalid: none
            QStringList inUse; // the catalog's
        };
        const auto look = [&]() -> std::optional<Seen> {
            QQuickItem* box = visualNamed(window->contentItem(), u"pluginKeyword-"_s + key);
            QQuickItem* reset = visualNamed(window->contentItem(), u"pluginKeywordReset-"_s + key);
            QQuickItem* note = visualNamed(window->contentItem(), u"pluginKeywordRejected-"_s + key);
            if (!box || !reset || !note)
                return std::nullopt;
            const QSettings ini(QSettings::IniFormat, QSettings::UserScope, u"WinShun"_s, u"WinShun"_s);
            return Seen {box->property("text").toString(), box->property("error").toBool(), reset->isVisible(),
                note->isVisible() ? note->property("text").toString() : QString(),
                ini.value(u"Plugin."_s + wordsPlugin + u"/Keyword.main"_s), wordsHost->catalog().keywordsOf(wordsPlugin, u"main")};
        };
        const auto type = [&](const QString& text) {
            if (QQuickItem* box = visualNamed(window->contentItem(), u"pluginKeyword-"_s + key)) {
                box->setProperty("text", text);
                QMetaObject::invokeMethod(box, "editingFinished"); // the focus left it
            }
            pump(200);
            return look();
        };
        const auto expect = [&](const char* what, const std::optional<Seen>& seen, const QString& box, bool marked, bool reset,
                                bool noted, const QVariant& kept, const QStringList& inUse) {
            const bool ok = seen && seen->box == box && seen->marked == marked && seen->reset == reset && seen->note.isEmpty() != noted
                && seen->kept == kept && seen->kept.isValid() == kept.isValid() && seen->inUse == inUse;
            say(u"#282 keywords, %1: box [%2] marked %3, reset %4, note [%5], INI %6, in use [%7]: %8"_s
                    .arg(QString::fromUtf8(what), seen ? seen->box : u"?"_s)
                    .arg(seen && seen->marked)
                    .arg(seen && seen->reset)
                    .arg(seen ? seen->note : QString(), seen && seen->kept.isValid() ? u'[' + seen->kept.toString() + u']' : u"none"_s,
                        seen ? seen->inUse.join(u", "_s) : QString(), ok ? u"passed"_s : u"FAILED"_s));
            keywords = keywords && ok;
        };
        const QStringList own {u"tr"_s, u"fy"_s};
        const QString quoted = u"tr \"x\""_s;
        expect("as installed", look(), u"tr fy"_s, false, false, false, {}, own);
        expect("two words", type(u" try\u3000FY "_s), u"try FY"_s, false, true, false, u"try FY"_s, {u"try"_s, u"FY"_s});
        expect("a quote", type(quoted), quoted, true, true, true, u"try FY"_s, {u"try"_s, u"FY"_s});
        expect("the plugin's own again", type(u"TR  fy"_s), u"tr fy"_s, false, false, false, {}, own);
        expect("none", type(QString()), QString(), false, true, false, QString(), {});
        if (QQuickItem* reset = visualNamed(window->contentItem(), u"pluginKeywordReset-"_s + key))
            QMetaObject::invokeMethod(reset, "clicked");
        pump(200);
        expect("reset", look(), u"tr fy"_s, false, false, false, {}, own);
        // The command's own shortcut (GitHub #466), recorded on the card:
        // the INI's Hotkey.main, the recorder (made again with the rows),
        // where it stands by the keyword's box, and why it does not work
        // (App's to say: setCommandHotkeyErrors) under the row.
        {
            const auto recorder = [&] { return visualNamed(window->contentItem(), u"pluginCommandHotkey-"_s + key); };
            const auto kept = [&] {
                const QSettings ini(QSettings::IniFormat, QSettings::UserScope, u"WinShun"_s, u"WinShun"_s);
                return ini.value(u"Plugin."_s + wordsPlugin + u"/Hotkey.main"_s);
            };
            const auto record = [&](const QString& keys) {
                if (QQuickItem* r = recorder())
                    QMetaObject::invokeMethod(r, "recorded", Q_ARG(QString, keys));
                pump(200);
            };
            const auto check = [&](const char* what, const QString& keys, const QString& why) {
                QQuickItem* r = recorder();
                QQuickItem* box = visualNamed(window->contentItem(), u"pluginKeyword-"_s + key);
                QQuickItem* note = visualNamed(window->contentItem(), u"pluginCommandHotkeyError-"_s + key);
                const QRectF at = r ? r->mapRectToScene(QRectF(0, 0, r->width(), r->height())) : QRectF();
                const QRectF by = box ? box->mapRectToScene(QRectF(0, 0, box->width(), box->height())) : QRectF();
                // Beside the box on its line, clear of it, or under it from the box's left edge.
                const bool beside = at.top() == by.top() && at.height() == by.height() && at.left() >= by.right() + 12;
                const bool under = at.top() >= by.bottom() + 6 && at.left() == by.left();
                const QVariant ini = kept();
                const QString said = note && note->isVisible() ? note->property("text").toString() : QString();
                const bool ok = r && box && note && r->property("hotkey").toString() == keys && ini.isValid() != keys.isEmpty()
                    && ini.toString() == keys && (beside || under) && by.width() >= 120 && said == why;
                say(u"#466 command shortcut, %1: recorder [%2] %3 (%4x%5 at %6,%7; box to %8), INI %9, note [%10]: %11"_s
                        .arg(QString::fromUtf8(what), r ? r->property("hotkey").toString() : u"?"_s, beside ? u"beside"_s : under ? u"under"_s : u"astray"_s)
                        .arg(at.width()).arg(at.height()).arg(at.left()).arg(at.top()).arg(by.right())
                        .arg(ini.isValid() ? u'[' + ini.toString() + u']' : u"none"_s, said, ok ? u"passed"_s : u"FAILED"_s));
                keywords = keywords && ok;
            };
            const QString taken = u"“Ctrl+Alt+B” 已被其他程序或系统占用，请换一个。"_s;
            check("as installed", QString(), QString());
            record(u"Ctrl+Alt+B"_s);
            check("recorded", u"Ctrl+Alt+B"_s, QString());
            editor->plugins()->setCommandHotkeyErrors({{key, taken}});
            pump(200);
            check("refused by Windows", u"Ctrl+Alt+B"_s, taken);
            record(QString()); // Backspace while recording
            check("cleared", QString(), QString());
            editor->plugins()->setCommandHotkeyErrors({});
            record(u"Ctrl+Alt+Shift+F12"_s); // the longest a chord gets, for the picture
            check("the longest chord", u"Ctrl+Alt+Shift+F12"_s, QString());
        }
        expect("for the picture", type(quoted), quoted, true, false, true, {}, own);
        if (!keywords)
            qWarning("plugin keyword check failed");
    }
    // --slider-check (GitHub #359): keys on a slider (its focus kept while
    // the card is rebuilt under it), a held key saved once, the box typed
    // into, the pointer dragging, a value the plugin sends, a plain box with
    // only a minimum; each time the box, the slider, the INI and what the
    // plugin is told.
    bool sliders = true;
    if (sliderCheck) {
        if (QQuickItem* section = visualNamed(window->contentItem(), u"pluginsSection"_s))
            QMetaObject::invokeMethod(section, "expand", Q_ARG(QVariant, rangesPlugin));
        pump(600); // the card opens
        int edits = 0;
        QObject::connect(editor, &SettingsEditor::edited, editor, [&edits] { ++edits; });
        const auto control = [&](const QString& key) { return visualNamed(window->contentItem(), u"pluginSlider-"_s + rangesPlugin + u'/' + key); };
        const auto part = [&](const QString& key, const QString& name) -> QQuickItem* {
            QQuickItem* c = control(key);
            return c ? visualNamed(c, name) : nullptr;
        };
        const auto ini = [&](const QString& key) {
            const QSettings file(QSettings::IniFormat, QSettings::UserScope, u"WinShun"_s, u"WinShun"_s);
            return file.value(u"Plugin."_s + rangesPlugin + u"/Setting."_s + key);
        };
        const auto check = [&](const char* what, const QString& key, const QString& box, double slider, const QVariant& kept, double told,
                               bool focused) {
            QQuickItem* b = part(key, u"settingSliderValue"_s);
            QQuickItem* s = part(key, u"settingSlider"_s);
            const QVariant k = ini(key);
            const QJsonValue t = rangesHost->pluginSettings(rangesPlugin).value(key);
            const bool ok = b && s && b->property("text").toString() == box && qFuzzyCompare(s->property("value").toDouble(), slider)
                && k.isValid() == kept.isValid() && (!kept.isValid() || k.toString() == kept.toString()) && t.isDouble()
                && qFuzzyCompare(t.toDouble(), told) && s->hasActiveFocus() == focused;
            say(u"#359 sliders, %1 (%2): box [%3] slider %4, INI %5, told %6, focus %7: %8"_s
                    .arg(QString::fromUtf8(what), key, b ? b->property("text").toString() : u"?"_s)
                    .arg(s ? s->property("value").toDouble() : -1)
                    .arg(k.isValid() ? u'[' + k.toString() + u']' : u"none"_s)
                    .arg(t.toDouble())
                    .arg(s && s->hasActiveFocus())
                    .arg(ok ? u"passed"_s : u"FAILED"_s));
            sliders = sliders && ok;
        };
        const auto key = [&](int code, int repeats = 0) {
            // Held: Qt's repeats, each after a release that says it is one.
            QKeyEvent press(QEvent::KeyPress, code, {});
            QCoreApplication::sendEvent(window, &press);
            for (int i = 0; i < repeats; ++i) {
                QKeyEvent up(QEvent::KeyRelease, code, {}, QString(), true);
                QKeyEvent down(QEvent::KeyPress, code, {}, QString(), true);
                QCoreApplication::sendEvent(window, &up);
                QCoreApplication::sendEvent(window, &down);
            }
            QKeyEvent release(QEvent::KeyRelease, code, {});
            QCoreApplication::sendEvent(window, &release);
            pump(120);
        };
        const auto focus = [&](const QString& key) {
            if (QQuickItem* s = part(key, u"settingSlider"_s))
                s->forceActiveFocus(Qt::TabFocusReason);
            pump(60);
        };
        const auto unfocus = [&] { // the root has the focus already: the focused one gives it up
            if (QQuickItem* focused = window->activeFocusItem(); focused && focused != window->contentItem())
                focused->setFocus(false);
            pump(60);
        };
        const auto type = [&](const QString& name, const QString& text) {
            if (QQuickItem* b = name.contains(u'/') ? visualNamed(window->contentItem(), name) : part(name, u"settingSliderValue"_s)) {
                b->setProperty("text", text);
                QMetaObject::invokeMethod(b, "editingFinished"); // Enter, or the focus left it
            }
            pump(150);
        };
        QQuickItem* pause = control(u"pause"_s);
        check("as installed", u"pause"_s, u"400"_s, 400, {}, 400, false);
        const bool unitShown = pause && pause->property("unit").toString() == u"ms" && pause->property("hasUnit").toBool();
        say(u"#359 sliders: the unit after the box: %1"_s.arg(unitShown ? u"passed"_s : u"FAILED"_s));
        sliders = sliders && unitShown;
        // Down the card the tracks start and end together, and the boxes
        // (the plain number's too) end together, whatever their units.
        settle(window); // laid out only with a frame
        {
            const auto left = [](QQuickItem* i) { return i ? i->mapToScene({0, 0}).x() : -1.0; };
            const auto right = [](QQuickItem* i) { return i ? i->mapToScene({i->width(), 0}).x() : -1.0; };
            QQuickItem* tracks[] = {part(u"pause"_s, u"settingSlider"_s), part(u"match"_s, u"settingSlider"_s), part(u"edge"_s, u"settingSlider"_s)};
            QQuickItem* boxes[] = {part(u"pause"_s, u"settingSliderValue"_s), part(u"match"_s, u"settingSliderValue"_s),
                                   part(u"edge"_s, u"settingSliderValue"_s), visualNamed(window->contentItem(), u"pluginSetting-"_s + rangesPlugin + u"/limit"_s)};
            bool lined = true;
            for (QQuickItem* t : tracks)
                lined = lined && t && qFuzzyCompare(left(t), left(tracks[0])) && qFuzzyCompare(right(t), right(tracks[0]));
            for (QQuickItem* b : boxes)
                lined = lined && b && qFuzzyCompare(right(b), right(boxes[0]));
            say(u"#359 sliders: tracks from %1 to %2, boxes to %3 and %4: %5"_s.arg(left(tracks[0])).arg(right(tracks[0]))
                    .arg(right(boxes[0])).arg(right(boxes[3])).arg(lined ? u"lined up, passed"_s : u"FAILED"_s));
            sliders = sliders && lined;
        }
        focus(u"pause"_s);
        key(Qt::Key_Right);
        check("→", u"pause"_s, u"450"_s, 450, u"450"_s, 450, true);
        key(Qt::Key_Right);
        check("→ again, the focus kept", u"pause"_s, u"500"_s, 500, u"500"_s, 500, true);
        key(Qt::Key_PageUp);
        check("PageUp: a tenth, in steps", u"pause"_s, u"700"_s, 700, u"700"_s, 700, true);
        key(Qt::Key_End);
        check("End", u"pause"_s, u"2000"_s, 2000, u"2000"_s, 2000, true);
        key(Qt::Key_Home);
        check("Home", u"pause"_s, u"100"_s, 100, u"100"_s, 100, true);
        key(Qt::Key_Up);
        check("↑", u"pause"_s, u"150"_s, 150, u"150"_s, 150, true);
        key(Qt::Key_Down);
        check("↓", u"pause"_s, u"100"_s, 100, u"100"_s, 100, true);
        const int before = edits;
        key(Qt::Key_Right, 3);
        check("→ held, four steps", u"pause"_s, u"300"_s, 300, u"300"_s, 300, true);
        say(u"#359 sliders: a held key saved %1 time(s): %2"_s.arg(edits - before).arg(edits - before == 1 ? u"passed"_s : u"FAILED"_s));
        sliders = sliders && edits - before == 1;
        unfocus();
        type(u"pause"_s, u"1234"_s);
        check("typed 1234: on a step", u"pause"_s, u"1250"_s, 1250, u"1250"_s, 1250, false);
        type(u"pause"_s, u"99999"_s);
        check("typed past the end", u"pause"_s, u"2000"_s, 2000, u"2000"_s, 2000, false);
        type(u"pause"_s, u"99999"_s);
        check("past the end again: the box shows what is kept", u"pause"_s, u"2000"_s, 2000, u"2000"_s, 2000, false);
        type(u"pause"_s, u"-5"_s);
        check("typed under the start", u"pause"_s, u"100"_s, 100, u"100"_s, 100, false);
        type(u"pause"_s, QString());
        check("emptied: nothing saved", u"pause"_s, u"100"_s, 100, u"100"_s, 100, false);
        // The pointer: pressed on the thumb, moved to the track's middle,
        // let go; nothing saved until then.
        if (QQuickItem* s = part(u"pause"_s, u"settingSlider"_s)) {
            settle(window); // laid out (the opened card's rows where they are drawn) only with a frame
            auto* handle = s->property("handle").value<QQuickItem*>();
            const QPointF start = handle ? handle->mapToScene({handle->width() / 2, handle->height() / 2}) : QPointF();
            const QPointF middle = s->mapToScene({s->width() / 2, s->height() / 2});
            const auto mouse = [&](QEvent::Type type, const QPointF& at, Qt::MouseButton button, Qt::MouseButtons buttons) {
                QMouseEvent event(type, at, window->mapToGlobal(at), button, buttons, {});
                QCoreApplication::sendEvent(window, &event);
                settle(window);
            };
            const int dragEdits = edits;
            mouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            mouse(QEvent::MouseMove, (start + middle) / 2, Qt::NoButton, Qt::LeftButton);
            mouse(QEvent::MouseMove, middle, Qt::NoButton, Qt::LeftButton);
            const double during = s->property("value").toDouble();
            const QString shown = part(u"pause"_s, u"settingSliderValue"_s)->property("text").toString();
            const bool onStep = std::fmod(during - 100, 50) == 0 && during > 100 && during < 2000;
            const bool unsaved = edits == dragEdits && ini(u"pause"_s).toString() == u"100";
            mouse(QEvent::MouseButtonRelease, middle, Qt::LeftButton, {});
            const bool saved = ini(u"pause"_s).toString() == QString::number(during) && edits == dragEdits + 1;
            say(u"#359 sliders, dragged: %1 shown [%2] on a step %3, unsaved while held %4, saved once let go %5: %6"_s
                    .arg(during).arg(shown).arg(onStep).arg(unsaved).arg(saved)
                    .arg(onStep && unsaved && saved && shown == QString::number(during) ? u"passed"_s : u"FAILED"_s));
            sliders = sliders && onStep && unsaved && saved && shown == QString::number(during);
        } else
            sliders = false;
        unfocus(); // a click gave the slider the focus
        // The plugin sends a value (winshun/settings/update): kept to the range.
        emit rangesHost->settingsUpdateRequested(rangesPlugin, QJsonObject {{u"pause"_s, 3333}});
        pump(150);
        check("sent 3333 by the plugin", u"pause"_s, u"2000"_s, 2000, u"2000"_s, 2000, false);
        // Typed by keys, as a user does: through the box's validator, which
        // lets Enter (and leaving) say so only for a number. Left empty, the
        // box shows the value again, nothing saved.
        const auto press = [&](int code, Qt::KeyboardModifiers modifiers = {}, const QString& text = {}) {
            QKeyEvent down(QEvent::KeyPress, code, modifiers, text);
            QKeyEvent up(QEvent::KeyRelease, code, modifiers, text);
            QCoreApplication::sendEvent(window, &down);
            QCoreApplication::sendEvent(window, &up);
        };
        const auto keys = [&](QQuickItem* box, const QString& text, bool enter) {
            if (box && !box->property("inputFocused").toBool())
                QMetaObject::invokeMethod(box, "takeFocus");
            press(Qt::Key_A, Qt::ControlModifier); // all of it, typed over
            press(Qt::Key_Backspace);
            for (const QChar c : text)
                press(c == u'-' ? int(Qt::Key_Minus) : c == u'.' ? int(Qt::Key_Period) : Qt::Key_0 + c.digitValue(), {}, QString(c));
            if (enter)
                press(Qt::Key_Return);
            pump(150);
        };
        QQuickItem* pauseBox = part(u"pause"_s, u"settingSliderValue"_s);
        keys(pauseBox, u"1234"_s, true);
        check("keys 1234, Enter", u"pause"_s, u"1250"_s, 1250, u"1250"_s, 1250, false);
        keys(pauseBox, QString(), false);
        const bool pauseEmptied = pauseBox && pauseBox->property("text").toString().isEmpty();
        unfocus(); // the focus leaves it empty
        check(pauseEmptied ? "keys: emptied, then left" : "keys: emptied, then left (it was not emptied)", u"pause"_s, u"1250"_s, 1250,
            u"1250"_s, 1250, false);
        sliders = sliders && pauseEmptied;
        // Steps after the point: shown with as many digits, kept without noise.
        type(u"match"_s, u"0.33"_s);
        check("typed 0.33", u"match"_s, u"0.35"_s, 0.35, u"0.35"_s, 0.35, false);
        focus(u"match"_s);
        key(Qt::Key_Right);
        check("→ from 0.35", u"match"_s, u"0.40"_s, 0.4, u"0.4"_s, 0.4, true);
        unfocus();
        // A last step shorter than the others: max is a value too, both ways.
        check("no default: min", u"edge"_s, u"5"_s, 5, {}, 5, false);
        focus(u"edge"_s);
        key(Qt::Key_End);
        check("End: max off the steps", u"edge"_s, u"99"_s, 99, u"99"_s, 99, true);
        key(Qt::Key_Left);
        check("← from max: the last step", u"edge"_s, u"95"_s, 95, u"95"_s, 95, true);
        key(Qt::Key_Right);
        check("→ to max again", u"edge"_s, u"99"_s, 99, u"99"_s, 99, true);
        unfocus();
        // Only a minimum: the plain box, kept to it (by keys); left empty, it
        // shows the value again, nothing saved.
        QQuickItem* limit = visualNamed(window->contentItem(), u"pluginSetting-"_s + rangesPlugin + u"/limit"_s);
        keys(limit, u"-3"_s, true);
        const bool limited = limit && limit->property("text").toString() == u"1" && ini(u"limit"_s).toString() == u"1"
            && rangesHost->pluginSettings(rangesPlugin).value(u"limit"_s).toDouble() == 1;
        keys(limit, QString(), false);
        const bool limitEmptied = limit && limit->property("text").toString().isEmpty();
        unfocus();
        const bool limitKept = limit && limit->property("text").toString() == u"1" && ini(u"limit"_s).toString() == u"1";
        say(u"#359 sliders, a minimum only: keys -3, Enter: kept as 1 %1; emptied %2, then left: 1 again %3: %4"_s
                .arg(limited).arg(limitEmptied).arg(limitKept).arg(limited && limitEmptied && limitKept ? u"passed"_s : u"FAILED"_s));
        sliders = sliders && limited && limitEmptied && limitKept;
        // A text of several lines (0.41.0, GitHub #403): the row's width
        // under its title; typed into by keys, kept a second after the typing
        // stops with the focus still there, Esc puts back what is kept,
        // leaving keeps it at once, Tab goes on; as tall as its lines between
        // 4 and 12. Each time the box, the INI and what the plugin is told.
        {
            QQuickItem* area = visualNamed(window->contentItem(), u"pluginSetting-"_s + rangesPlugin + u"/lines"_s);
            QQuickItem* field = area ? visualNamed(area, u"settingTextArea"_s) : nullptr;
            QQuickItem* row = area && area->parentItem() && area->parentItem()->parentItem() ? area->parentItem()->parentItem()->parentItem() : nullptr;
            const bool wide = row && qFuzzyCompare(area->width(), row->width()) && area->mapToItem(row, QPointF(0, 0)).y() > 10;
            say(u"multiline text: %1 wide of a row %2 wide, %3 down it: %4"_s.arg(area ? area->width() : -1).arg(row ? row->width() : -1)
                    .arg(area && row ? area->mapToItem(row, QPointF(0, 0)).y() : -1).arg(wide ? u"under the title, passed"_s : u"FAILED"_s));
            sliders = sliders && wide;
            const auto typeKeys = [&](const QString& text) {
                for (const QChar c : text) {
                    const int code = c == u'\n' ? int(Qt::Key_Return) : int(c.toUpper().unicode()); // Qt's key codes are ASCII's
                    const QString typed = c == u'\n' ? u"\r"_s : QString(c);
                    QKeyEvent press(QEvent::KeyPress, code, {}, typed);
                    QCoreApplication::sendEvent(window, &press);
                    QKeyEvent release(QEvent::KeyRelease, code, {}, typed);
                    QCoreApplication::sendEvent(window, &release);
                }
                pump(60);
            };
            const auto lines = [&](const char* what, const QString& shown, const QVariant& kept, const QString& told, bool focused) {
                const QVariant k = ini(u"lines"_s);
                const QString t = rangesHost->pluginSettings(rangesPlugin).value(u"lines"_s).toString();
                const QString s = field ? field->property("text").toString() : u"?"_s;
                const bool ok = field && s == shown && k.isValid() == kept.isValid() && (!kept.isValid() || k.toString() == kept.toString())
                    && t == told && field->hasActiveFocus() == focused;
                const auto oneLine = [](QString v) { return v.replace(u'\n', u'|'); };
                say(u"multiline text, %1: box [%2], INI %3, told [%4], focus %5: %6"_s
                        .arg(QString::fromUtf8(what), oneLine(s), k.isValid() ? u'[' + oneLine(k.toString()) + u']' : u"none"_s, oneLine(t))
                        .arg(field && field->hasActiveFocus())
                        .arg(ok ? u"passed"_s : u"FAILED"_s));
                sliders = sliders && ok;
            };
            const QString first = u"cmd = command"_s;
            const QString two = first + u"\nbtw = by the way"_s;
            const QString three = two + u"\nok = okay"_s;
            lines("as installed", first, {}, first, false);
            const qreal shortHeight = area ? area->height() : -1;
            if (field) {
                field->forceActiveFocus(Qt::TabFocusReason);
                field->setProperty("cursorPosition", first.size());
            }
            typeKeys(u"\nbtw = by the way"_s);
            lines("typed, not kept yet", two, {}, first, true);
            pump(1300);
            lines("a second later, in the box still", two, two, two, true);
            typeKeys(u"xyz"_s);
            key(Qt::Key_Escape);
            pump(1300);
            lines("typed, then Esc: what is kept, and only that", two, two, two, true);
            typeKeys(u"\nok = okay"_s);
            unfocus();
            lines("left: kept at once", three, three, three, false);
            if (field)
                field->forceActiveFocus(Qt::TabFocusReason);
            key(Qt::Key_Tab);
            lines("Tab: on to the next control, no tab typed", three, three, three, false);
            // Its lines: 3 make the 4 at least, 30 the 12 at most, scrolled.
            if (field) {
                field->forceActiveFocus(Qt::TabFocusReason);
                QStringList many;
                for (int i = 1; i <= 30; ++i)
                    many.append(u"a%1 = %1"_s.arg(i));
                field->setProperty("text", many.join(u'\n'));
            }
            pump(100);
            const qreal tallHeight = area ? area->height() : -1;
            const qreal line = area ? (tallHeight - shortHeight) / 8 : 0; // 12 lines less 4
            const bool grows = area && shortHeight > 0 && tallHeight > shortHeight && line > 10 && line < 40
                && field && field->property("implicitHeight").toReal() > tallHeight;
            say(u"multiline text: %1 high with 3 lines, %2 with 30 (a line %3, the text %4): %5"_s.arg(shortHeight).arg(tallHeight).arg(line)
                    .arg(field ? field->property("implicitHeight").toReal() : -1).arg(grows ? u"4 to 12 lines, passed"_s : u"FAILED"_s));
            sliders = sliders && grows;
            key(Qt::Key_Escape);
            unfocus();
            lines("Esc again, for the picture", three, three, three, false);
        }
        // A hidden setting (0.41.0): no row on the card and not found by the
        // settings search, and a card with nothing else is one without
        // settings; kept, told to the plugin and changed by it as any other.
        {
            PluginListModel* list = editor->plugins()->list();
            const auto hasKey = [](const PluginListModel::Card* card, const QString& key) {
                return card && std::ranges::any_of(card->settings, [&](const QVariant& s) { return s.toMap().value(u"key"_s).toString() == key; });
            };
            const PluginListModel::Card* card = list->card(rangesPlugin);
            const PluginListModel::Card* only = list->card(hiddenOnlyPlugin);
            bool searchable = false;
            for (int row = 0; row < list->rowCount(); ++row) {
                const QString id = list->data(list->index(row, 0), PluginListModel::IdRole).toString();
                if ((id == rangesPlugin || id == hiddenOnlyPlugin) && editor->plugins()->searchTerms(row).contains(u"手势布局"_s))
                    searchable = true;
            }
            const bool noRow = hasKey(card, u"lines"_s) && !hasKey(card, u"layout"_s)
                && !visualNamed(window->contentItem(), u"pluginSetting-"_s + rangesPlugin + u"/layout"_s) && only && only->settings.isEmpty();
            const QString first = rangesHost->pluginSettings(rangesPlugin).value(u"layout"_s).toString();
            const QString sent = uR"({"gestures":[{"path":"RDL","action":"power"}],"note":"你好"})"_s;
            emit rangesHost->settingsUpdateRequested(rangesPlugin, QJsonObject {{u"layout"_s, sent}});
            pump(150);
            const bool kept = ini(u"layout"_s).toString() == sent && rangesHost->pluginSettings(rangesPlugin).value(u"layout"_s).toString() == sent
                && !hasKey(list->card(rangesPlugin), u"layout"_s);
            say(u"hidden setting: no row on the card, and a card of hidden settings alone has none %1; found by the settings search %2; "
                u"told %3 before, kept and told %4 after the plugin sent it: %5"_s
                    .arg(noRow).arg(searchable).arg(first).arg(kept)
                    .arg(noRow && !searchable && first == u"{}" && kept ? u"passed"_s : u"FAILED"_s));
            sliders = sliders && noRow && !searchable && first == u"{}" && kept;
        }
        // For the picture: the values a user would leave, nothing focused.
        type(u"pause"_s, u"400"_s);
        type(u"match"_s, u"0.75"_s);
        unfocus();
        if (!sliders)
            qWarning("plugin slider check failed");
    }
    say(u"grabbing"_s);
    if (args.contains(u"--tree"_s))
        writeTree(window, out + u".tree.txt"_s);
    const bool saved = saveGrab(window, out);
    say(u"dpr %1"_s.arg(window->devicePixelRatio()));
    return finish(window, saved && confirmed && homepages && keywords && sliders);
}

// ---- --preview-settings -----------------------------------------------------------------

// A share's status as the app would report it (SettingsEditor::setShareStatuses).
ShareIndexer::Status shareStatus(const QString& path, ShareIndexer::State state, share::Problem problem, std::size_t items)
{
    ShareIndexer::Status status;
    status.path = path.toStdWString();
    status.state = state;
    status.problem = problem;
    status.items = items;
    status.lastVerified = state == ShareIndexer::State::Online ? static_cast<FileTime>(QDateTime::currentSecsSinceEpoch() - 1800) : 0;
    status.watching = state == ShareIndexer::State::Online;
    return status;
}

// An id of SettingsWindow.qml's own (flick, searchBox, settingsSearch …).
QObject* named(QQuickWindow* window, const char* id)
{
    QQmlContext* context = qmlContext(window);
    QObject* object = context ? context->objectForName(QString::fromLatin1(id)) : nullptr;
    if (!object)
        qWarning("preview: no item '%s' in SettingsWindow.qml", id);
    return object;
}

// A row or section of a page of SettingsWindow.qml, by its name in that page's
// parts (SettingsWindow.part): the page is made if it is not yet. `page`: its
// index (window->property("scopePage")) or its id ("scope").
QObject* partOf(QQuickWindow* window, const QVariant& page, const char* name)
{
    QVariant found;
    QMetaObject::invokeMethod(window, "part", Q_RETURN_ARG(QVariant, found), Q_ARG(QVariant, page),
        Q_ARG(QVariant, QVariant(QString::fromLatin1(name))));
    QObject* object = found.metaType() == QMetaType::fromType<QJSValue>() ? found.value<QJSValue>().toQObject() : found.value<QObject*>();
    if (!object)
        qWarning("preview: no part '%s' on settings page %s", name, qUtf8Printable(page.toString()));
    return object;
}

// Repeater delegates belong to their model as QObjects; follow visual parents.
QQuickItem* visualNamed(QQuickItem* root, const QString& name)
{
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = visualNamed(child, name))
            return found;
    }
    return nullptr;
}

// Exercise the real add/edit actions and form on the preview's temporary
// profile. In particular, a whole-page screenshot cannot detect a form
// appended below the viewport, or a Loader whose contents failed to appear.
bool previewWebForms(QQuickWindow* window, SettingsEditor* editor, const QDir& out, bool tree)
{
    QMetaObject::invokeMethod(window, "showWebPage");
    settle(window);
    QObject* add = visualNamed(window->contentItem(), u"webAddButton"_s);
    QObject* loader = named(window, "webDialog");
    QObject* pageFlick = named(window, "flick");
    if (!add || !loader || !pageFlick)
        return false;
    const auto check = [](bool condition, const char* message) {
        if (!condition)
            qWarning("web form: %s", message);
        return condition;
    };
    const auto click = [](QObject* button) {
        return button && QMetaObject::invokeMethod(button, "clicked");
    };
    const auto key = [&](int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QKeyEvent press(QEvent::KeyPress, code, modifiers);
        QCoreApplication::sendEvent(window, &press);
        QKeyEvent release(QEvent::KeyRelease, code, modifiers);
        QCoreApplication::sendEvent(window, &release);
        pump(20);
    };
    const auto form = [&] { return qobject_cast<QQuickItem*>(loader->property("item").value<QObject*>()); };
    const auto child = [&](const char* name) -> QQuickItem* {
        return form() ? form()->findChild<QQuickItem*>(QString::fromLatin1(name)) : nullptr;
    };
    const auto inside = [](QQuickItem* item, QQuickItem* ancestor) {
        for (; item; item = item->parentItem()) {
            if (item == ancestor)
                return true;
        }
        return false;
    };
    bool saved = true;
    const auto grab = [&](const QString& name) {
        settle(window);
        const QString file = out.filePath(name + u".png"_s);
        if (tree)
            writeTree(window, file + u".tree.txt"_s);
        saved = saveGrab(window, file) && saved;
    };
    const auto visibleIn = [](QQuickItem* item, QQuickItem* viewport) {
        if (!item || !viewport || !item->isVisible() || item->width() <= 0 || item->height() <= 0)
            return false;
        const QPointF top = item->mapToItem(viewport, QPointF());
        return QRectF(QPointF(), viewport->size()).contains(QRectF(top, item->size()));
    };
    const qsizetype before = editor->webShortcuts().size();
    click(add);
    settle(window);
    if (!check(form() && child("keywordField") && child("urlField") && child("webFormSave"), "add form did not load"))
        return false;
    check(visibleIn(child("webFormCard"), window->contentItem()), "card extends outside the window");
    check(visibleIn(child("keywordField"), child("webFormViewport")), "first field is outside the viewport");
    check(visibleIn(child("webFormSave"), window->contentItem()), "save button is outside the window");
    check(inside(window->activeFocusItem(), child("keywordField")), "keyword did not receive focus");
    const qreal listHeight = pageFlick->property("contentHeight").toReal();
    grab(u"web-add"_s);

    // Tab through a full cycle in each direction, without entering the
    // settings behind the overlay, and bring lower fields into view.
    for (int direction : {1, -1}) {
        for (int step = 0; step < 12; ++step) {
            key(direction > 0 ? Qt::Key_Tab : Qt::Key_Backtab, direction > 0 ? Qt::NoModifier : Qt::ShiftModifier);
            check(inside(window->activeFocusItem(), form()), "Tab left the modal");
        }
    }
    if (!check(child("webFormSvgInput"), "SVG input did not load"))
        return false;
    QMetaObject::invokeMethod(child("webFormSvgInput"), "takeFocus");
    settle(window);
    check(visibleIn(window->activeFocusItem(), child("webFormViewport")), "focused input was not scrolled into view");
    grab(u"web-scrolled"_s);
    click(child("webFormSave"));
    settle(window);
    check(form() && !form()->property("error").toString().isEmpty(), "empty form did not report validation error");
    check(visibleIn(child("webFormError"), window->contentItem()), "validation error is outside the window");
    check(editor->webShortcuts().size() == before, "invalid form changed settings");
    grab(u"web-error"_s);
    key(Qt::Key_Escape);
    check(!loader->property("active").toBool(), "Escape did not close the form");
    check(window->activeFocusItem() == add, "focus did not return to Add");

    // Reopening must create a fresh draft. Enter saves through the actual
    // TextInput, including a model reset when the shortcut is appended.
    click(add);
    settle(window);
    if (!check(child("keywordField") && child("urlField"), "reopened form is blank"))
        return false;
    check(form()->property("error").toString().isEmpty(), "old error survived reopening");
    child("keywordField")->setProperty("text", u"previewweb"_s);
    child("nameField")->setProperty("text", u"表单验证"_s);
    child("urlField")->setProperty("text", u"https://example.com/search?q=%s"_s);
    QMetaObject::invokeMethod(child("urlField"), "takeFocus");
    key(Qt::Key_Return);
    settle(window);
    check(!loader->property("active").toBool() && editor->webShortcuts().size() == before + 1, "Enter did not add the shortcut");

    QList<QQuickItem*> edits;
    std::function<void(QQuickItem*)> collectEdits = [&](QQuickItem* item) {
        if (item->objectName() == u"webEditButton")
            edits.append(item);
        for (auto* child : item->childItems())
            collectEdits(child);
    };
    collectEdits(window->contentItem()); // Repeater delegates have visual, not QObject, parents
    if (!check(!edits.isEmpty(), "no edit button in the list"))
        return false;
    // The last row is below the initial viewport: its editor must still
    // appear in the center, without changing the page's scroll position.
    pageFlick->setProperty("contentY", std::max(0.0, pageFlick->property("contentHeight").toDouble() - pageFlick->property("height").toDouble()));
    const qreal scrollY = pageFlick->property("contentY").toReal();
    click(edits.constLast());
    settle(window);
    if (!check(child("urlField"), "edit form is blank"))
        return false;
    check(child("keywordField")->property("text").toString() == u"previewweb", "edit did not load the selected shortcut");
    check(inside(window->activeFocusItem(), child("urlField")), "edit URL did not receive focus");
    check(visibleIn(child("webFormCard"), window->contentItem()), "edit card is off screen");
    check(pageFlick->property("contentY").toReal() == scrollY, "opening the edit form moved the list");
    grab(u"web-edit"_s);
    child("nameField")->setProperty("text", u"已修改"_s);
    click(child("webFormSave"));
    settle(window);
    check(!loader->property("active").toBool() && editor->webShortcuts().constLast().toMap().value(u"name"_s).toString() == u"已修改", "edit did not save");
    click(add);
    settle(window);
    click(child("webFormCancel"));
    settle(window);
    check(!loader->property("active").toBool() && editor->webShortcuts().size() == before + 1, "Cancel changed settings or left the form open");
    check(pageFlick->property("contentHeight").toReal() == listHeight + 58, "form changed the list layout");

    // An empty list must not collapse the dialog, and resizing must retain
    // the unsaved draft while keeping both its first field and actions usable.
    while (!editor->webShortcuts().isEmpty())
        editor->removeWebShortcut(0);
    click(add);
    settle(window);
    if (!check(child("keywordField"), "empty-list add form is blank"))
        return false;
    child("keywordField")->setProperty("text", u"draft"_s);
    const QSize initialSize = window->size();
    window->resize(760, 420);
    settle(window);
    check(child("keywordField")->property("text").toString() == u"draft", "resizing lost the draft");
    check(visibleIn(child("webFormCard"), window->contentItem()) && visibleIn(child("webFormSave"), window->contentItem()), "resized form is off screen");
    grab(u"web-add-empty-compact"_s);
    click(child("webFormCancel"));
    window->resize(initialSize);
    check(editor->webShortcuts().isEmpty(), "cancel saved the draft");
    say(u"web forms checked at %1x%2, DPR %3"_s.arg(window->width()).arg(window->height()).arg(window->devicePixelRatio()));
    return saved;
}

bool previewCommandIcons(QQuickWindow* window, SettingsEditor* editor, Favorites& favorites, QObject* section, const QDir& out)
{
    const auto check = [](bool condition, const char* message) {
        if (!condition)
            qWarning("command icons: %s", message);
        return condition;
    };
    const auto form = [&] { return visualNamed(qobject_cast<QQuickItem*>(section), u"favoriteForm"_s); };
    const auto load = [&](const QString& path) {
        QPointer<QObject> current = form();
        if (!current)
            return false;
        current->setProperty("iconRequest", editor->favorites()->loadCommandIcon(path));
        QElapsedTimer wait;
        wait.start();
        while (current && current->property("iconLoading").toBool() && wait.elapsed() < 5000)
            pump(10);
        return check(current && !current->property("iconLoading").toBool(), "icon worker did not finish");
    };
    QTemporaryDir pictures;
    const QString path = pictures.filePath(u"icon.svg"_s);
    QFile svg(path);
    if (!check(svg.open(QIODevice::WriteOnly), "cannot write the SVG fixture"))
        return false;
    svg.write("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'>"
              "<rect x='8' y='8' width='48' height='48' rx='10' fill='#1268b9'/>"
              "<path d='M22 17h20v30H22z' fill='white'/></svg>");
    svg.close();
    if (!load(path) || !check(form()->property("error").toString().isEmpty(), "valid SVG was rejected"))
        return false;
    const QString icon = form()->property("iconData").toString();
    check(!favoriteIconPng(icon).isEmpty(), "SVG was not normalized to a bounded PNG");
    check(favorites.items().size() == 4, "picking an icon saved the draft");
    QFile::remove(path); // the draft must already own the picture
    settle(window, 3);
    if (auto* flick = named(window, "flick"))
        flick->setProperty("contentY", std::max(0.0, flick->property("contentHeight").toDouble() - flick->property("height").toDouble()));
    settle(window, 3);
    auto* preview = form()->findChild<QQuickItem*>(u"favoriteIconPreview"_s);
    check(preview && preview->property("status").toInt() == 1 && preview->property("implicitWidth").toReal() > 0,
        "custom preview did not load after its original file was deleted");
    bool saved = saveGrab(window, out.filePath(u"command-custom-icon.png"_s));
    QMetaObject::invokeMethod(form(), "save");
    if (!check(favorites.items().size() == 5 && favorites.items().constLast().icon == icon, "form did not save the icon"))
        return false;
    const Favorite command = favorites.items().constLast();
    const auto exported = readFavoritesFile(writeFavoritesFile(favorites.items()));
    check(exported && exported->favorites.constLast().icon == icon, "export lost the custom icon");

    SearchResult row;
    row.path = command.path;
    row.app = AppKind::Tool;
    row.target = command.command;
    row.workingDirectory = command.workingDirectory;
    row.icon = command.icon;
    ResultModel results;
    results.assign({row}, {});
    check(results.data(results.index(0), ResultModel::IconRole).toString()
            == editor->favorites()->items().constLast().toMap().value(u"icon"_s).toString(),
        "search results and settings disagree on the command icon");

    const auto reopen = [&] {
        QMetaObject::invokeMethod(window, "showFavoritesPage", Q_ARG(QVariant, QVariant(command.path)));
        settle(window, 3);
        return check(form() && form()->property("iconData").toString() == icon, "editing did not restore the saved icon");
    };
    if (!reopen())
        return false;
    // A failed import preserves the previous custom picture and reports why.
    if (!load(pictures.filePath(u"missing.png"_s)))
        return false;
    check(!form()->property("error").toString().isEmpty() && form()->property("iconData").toString() == icon,
        "failed selection discarded the current icon or hid the error");
    auto* reset = form()->findChild<QObject*>(u"favoriteResetIcon"_s);
    if (!check(reset, "reset button is missing"))
        return false;
    QMetaObject::invokeMethod(reset, "clicked");
    check(form()->property("iconData").toString().isEmpty(), "reset did not select automatic extraction");
    QMetaObject::invokeMethod(form(), "finished");
    check(favorites.items().constLast().icon == icon, "cancelling reset changed the saved icon");
    if (!reopen())
        return false;
    reset = form()->findChild<QObject*>(u"favoriteResetIcon"_s);
    QMetaObject::invokeMethod(reset, "clicked");
    QMetaObject::invokeMethod(form(), "save");
    check(favorites.items().constLast().icon.isEmpty(), "saving reset kept the custom icon");
    favorites.remove(command.path);
    say(u"command icons: real form, asynchronous SVG load, source deletion, search model, export, reopen, cancel and reset checked"_s);
    return saved;
}

// ---- --preview-settings --lazy-pages ------------------------------------------------------
// The settings window makes a page when first needed (SettingsWindow.loadPage,
// PageSlot): with the window only the page on show; a way in from elsewhere only
// its own; a word typed all of them (the search needs every row); the search box
// taking the keyboard the rest one at a time (fillInBackground, fillTimer).
// Nothing more after the first frame (refreshAfterOpen): pages cost memory.

// The ids of SettingsWindow.pageSlots' PageSlots, in the order of the pages (the
// page's index in `pages` is its place here); the last, 运行状况's, is not counted.
constexpr const char* kSlotIds[] = {"generalSlot", "keysSlot", "appearanceSlot", "scopeSlot", "resultsSlot", "integrationSlot",
    "favoritesSlot", "clipboardSlot", "pluginsSlot", "aiSlot", "healthSlot"};
constexpr int kSettingsPages = 10; // the slots counted: all but healthSlot

QList<int> madePages(QQuickWindow* window)
{
    QList<int> pages;
    for (int i = 0; i < kSettingsPages; ++i) {
        QObject* slot = named(window, kSlotIds[i]);
        if (slot && slot->property("item").value<QObject*>())
            pages.append(i);
    }
    return pages;
}

QString pagesText(const QList<int>& pages)
{
    QStringList list;
    for (int page : pages)
        list.append(QString::number(page));
    return u"["_s + list.join(u',') + u"]"_s;
}

// The GUI thread's longest stretch without running its event loop while `until`
// is false (at most `ms`): what a frame, a hover or a key waits for at worst.
qint64 longestBlock(int ms, const std::function<bool()>& until, qint64* took = nullptr)
{
    QElapsedTimer clock;
    clock.start();
    qint64 last = 0;
    qint64 longest = 0;
    QTimer tick;
    tick.setTimerType(Qt::PreciseTimer);
    tick.setInterval(1);
    QObject::connect(&tick, &QTimer::timeout, [&] {
        const qint64 now = clock.elapsed();
        longest = std::max(longest, now - last);
        last = now;
    });
    tick.start();
    while (clock.elapsed() < ms && !until())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    tick.stop();
    if (took)
        *took = clock.elapsed();
    return longest;
}

QVariant evaluateIn(QQuickWindow* window, const QString& expression); // below

bool previewLazyPages(QQuickWindow* window, const std::function<QQuickWindow*()>& make, QQmlEngine* engine,
    const QString& favoritePath, const QDir& out)
{
    Q_UNUSED(out);
    bool ok = true;
    const auto check = [&](bool condition, const QString& message) {
        say((condition ? u"ok: "_s : u"FAILED: "_s) + message);
        ok = condition && ok;
    };
    const auto all = [] {
        QList<int> pages;
        for (int i = 0; i < kSettingsPages; ++i)
            pages.append(i);
        return pages;
    }();
    const auto inputOf = [](QQuickWindow* w) -> QQuickItem* {
        QObject* box = named(w, "searchBox");
        for (QQuickItem* item : box ? box->findChildren<QQuickItem*>() : QList<QQuickItem*>()) {
            if (item->inherits("QQuickTextInput"))
                return item;
        }
        return nullptr;
    };
    const auto close = [](QQuickWindow* w) {
        w->hide();
        pump(50);
        delete w;
    };
    say(u"lazy pages: this engine's incubation controller: %1"_s.arg(engine->incubationController() ? u"set"_s : u"none (incubateObject makes it all at once)"_s));

    // Opened: the page on show, and no other.
    check(madePages(window) == QList<int> {0}, u"opened: page 0 only (made %1)"_s.arg(pagesText(madePages(window))));
    check(qobject_cast<QQuickItem*>(named(window, "flick"))->property("contentHeight").toDouble() > 300, u"opened: the first page is laid out"_s);

    // The ways in from elsewhere, each on a window made anew. Called as App
    // calls them (before the window is shown), they make only their own page.
    const int clipboardPage = window->property("clipboardPage").toInt();
    const int favoritesPage = window->property("favoritesPage").toInt();
    const int pluginsPage = window->property("pluginsPage").toInt();
    const int keysPage = window->property("keysPage").toInt();
    const int keysListTab = window->property("keysListTab").toInt();
    const int scopePage = window->property("scopePage").toInt();
    const int resultsPage = window->property("resultsPage").toInt();
    const QString rowTitle = QCoreApplication::translate("SettingsWindow", "每次打开时清空搜索"); // 结果列表 › 操作习惯
    struct Link {
        QString name;
        std::function<void(QQuickWindow*)> go;
        int page;
        int tab; // -1: either
        QList<int> made;
        bool afterShow = false;
    };
    const auto call = [](const char* method, QVariantList values = {}) {
        return [method, values](QQuickWindow* w) {
            if (values.size() == 3)
                QMetaObject::invokeMethod(w, method, Q_ARG(QVariant, values[0]), Q_ARG(QVariant, values[1]), Q_ARG(QVariant, values[2]));
            else if (values.size() == 1)
                QMetaObject::invokeMethod(w, method, Q_ARG(QVariant, values[0]));
            else
                QMetaObject::invokeMethod(w, method);
        };
    };
    const Link links[] = {
        {u"showClipboardPage"_s, call("showClipboardPage"), clipboardPage, 0, {clipboardPage}},
        {u"showShortcutsPage"_s, call("showShortcutsPage"), keysPage, keysListTab, {keysPage}},
        {u"showWebPage"_s, call("showWebPage"), pluginsPage, -1, {pluginsPage}},
        {u"showFileTypesPage"_s, call("showFileTypesPage"), resultsPage, 3, {resultsPage}},
        {u"showFavoritesPage"_s, call("showFavoritesPage", {favoritePath}), favoritesPage, 0, {favoritesPage}},
        {u"showPluginUpdates"_s, call("showPluginUpdates"), pluginsPage, 1, {pluginsPage}},
        {u"showPlace indexAccess"_s, call("showPlace", {u"indexAccess"_s}), scopePage, -1, {scopePage}},
        {u"showPlace results,2 (--settings-at)"_s, call("showPlace", {u"results,2"_s}), resultsPage, 2, {resultsPage}},
        {u"showSetting (open_settings)"_s, call("showSetting", {u"results"_s, 2, rowTitle}), resultsPage, 2, all},
        {u"showClipboardPage after show"_s, call("showClipboardPage"), clipboardPage, 0, {0, clipboardPage}, true},
    };
    for (const Link& link : links) {
        const int warned = g_warnings;
        QQuickWindow* w = make();
        if (!w) {
            check(false, link.name + u": no window"_s);
            continue;
        }
        if (link.afterShow) {
            showOffScreen(w);
            link.go(w);
        } else {
            link.go(w);
            showOffScreen(w);
        }
        settle(w);
        pump(300); // showSetting finds its row after a turn of the event loop (Qt.callLater)
        settle(w, 2);
        const QList<int> made = madePages(w);
        const int page = w->property("currentPage").toInt();
        const int tab = w->property("currentTab").toInt();
        const double height = named(w, "flick")->property("contentHeight").toDouble();
        check(page == link.page && (link.tab < 0 || tab == link.tab) && !w->property("searching").toBool()
                  && made == link.made && height > 150 && g_warnings == warned,
            u"%1: on %2/%3, made %4, content %5 high, %6 warning(s)"_s.arg(link.name).arg(page).arg(tab)
                .arg(pagesText(made)).arg(height).arg(g_warnings - warned));
        close(w);
    }

    // A word typed: nothing made at once; the page stays as it is (no 没有找到)
    // while the rest is made in the background as fast as it goes, then searched;
    // the results ordered as they stand, page by page, and as many as with all
    // made beforehand. Enter and ↓ pressed meanwhile: no harm, Enter done after.
    {
        const QString word = QCoreApplication::translate("SettingsWindow", "快捷键", "settings tab");
        const int warned = g_warnings;
        QQuickWindow* w = make();
        showOffScreen(w);
        settle(w);
        check(madePages(w) == QList<int> {0}, u"search: page 0 only before typing"_s);
        QQuickItem* input = inputOf(w);
        QObject* noResults = named(w, "noResults");
        const double heightBefore = named(w, "flick")->property("contentHeight").toDouble();
        input->setProperty("text", word);
        QQuickItem* focused = w->activeFocusItem();
        QMetaObject::invokeMethod(w, "moveCurrent", Q_ARG(QVariant, QVariant(1))); // ↓
        check(madePages(w) == QList<int> {0} && !w->property("searchReady").toBool() && !w->property("searching").toBool()
                  && !noResults->property("visible").toBool() && w->property("currentPage").toInt() == 0
                  && named(w, "flick")->property("contentHeight").toDouble() == heightBefore && w->activeFocusItem() == focused,
            u"search: typed, nothing made at once, the page as it was, no 没有找到, ↓ harmless"_s);
        bool nothingFoundMeanwhile = false;
        QElapsedTimer waited;
        waited.start();
        while (!w->property("searchReady").toBool() && waited.elapsed() < 5000) {
            pump(5);
            nothingFoundMeanwhile = nothingFoundMeanwhile || noResults->property("visible").toBool();
        }
        check(madePages(w) == all && w->property("searchReady").toBool() && !nothingFoundMeanwhile && g_warnings == warned,
            u"search: every page made after the letter, a page per turn (%1 ms, made %2), no 没有找到 meanwhile"_s
                .arg(waited.elapsed()).arg(pagesText(madePages(w))));
        settle(w, 3);
        QObject* search = named(w, "settingsSearch");
        const int total = search->property("total").toInt();
        const QVariantList counts = search->property("counts").toList();
        QVariant returned;
        QMetaObject::invokeMethod(w, "results", Q_RETURN_ARG(QVariant, returned));
        const QVariantList results = returned.metaType() == QMetaType::fromType<QJSValue>()
            ? returned.value<QJSValue>().toVariant().toList() : returned.toList();
        bool ordered = !results.isEmpty();
        double lastY = -1;
        int lastPage = -1;
        QSet<int> pagesFound;
        for (const QVariant& entry : results) {
            auto* row = qobject_cast<QQuickItem*>(entry.toMap().value(u"row"_s).value<QObject*>());
            QObject* section = row ? row->property("section").value<QObject*>() : nullptr;
            if (!row || !section) {
                ordered = false;
                break;
            }
            const double y = row->mapToItem(w->contentItem(), QPointF()).y();
            const int page = section->property("page").toInt();
            ordered = ordered && y >= lastY - 0.5 && page >= lastPage;
            lastY = y;
            lastPage = page;
            pagesFound.insert(page);
        }
        check(ordered && pagesFound.size() > 1, u"search '%1': %2 result(s) on %3 page(s), in the order they stand"_s
            .arg(word).arg(results.size()).arg(pagesFound.size()));
        // The same word on a window with every page made before it was typed.
        QMetaObject::invokeMethod(window, "loadAllPages");
        settle(window);
        QQuickItem* mainInput = inputOf(window);
        mainInput->setProperty("text", word);
        settle(window, 3);
        QObject* mainSearch = named(window, "settingsSearch");
        check(total > 0 && total == mainSearch->property("total").toInt() && counts == mainSearch->property("counts").toList(),
            u"search: %1 found, as with every page made beforehand (%2)"_s.arg(total).arg(mainSearch->property("total").toInt()));
        // Where Enter goes there: the best result's page and tab.
        const QString best = evaluateIn(window, u"settingsSearch.current ? settingsSearch.current.section.page + '/' + settingsSearch.current.section.tab : ''"_s).toString();
        mainInput->setProperty("text", QString());
        settle(window, 2);
        close(w);

        // Enter pressed before the search could look: done once it can.
        const int warnedEnter = g_warnings;
        QQuickWindow* e = make();
        showOffScreen(e);
        settle(e);
        inputOf(e)->setProperty("text", word);
        QMetaObject::invokeMethod(named(e, "searchBox"), "activated");
        const bool pending = e->property("activatePending").toBool() && !e->property("searching").toBool();
        QElapsedTimer waitedEnter;
        waitedEnter.start();
        while (!e->property("searchReady").toBool() && waitedEnter.elapsed() < 5000)
            pump(5);
        settle(e, 3);
        const QString landed = u"%1/%2"_s.arg(e->property("currentPage").toInt()).arg(e->property("currentTab").toInt());
        check(pending && !best.isEmpty() && landed == best && !e->property("searching").toBool()
                  && inputOf(e)->property("text").toString().isEmpty() // left the search for the row
                  && !e->property("activatePending").toBool() && g_warnings == warnedEnter,
            u"search: Enter before ready, gone to the best result once searched (%1, expected %2)"_s.arg(landed, best));
        close(e);
    }

    // Closed with some pages made, and while they were being made; opened again.
    {
        const int warned = g_warnings;
        QQuickWindow* w = make();
        showOffScreen(w);
        settle(w);
        QMetaObject::invokeMethod(w, "showPage", Q_ARG(QVariant, QVariant(u"scope"_s)), Q_ARG(QVariant, QVariant(0)));
        settle(w, 2);
        w->hide();
        pump(100);
        const QList<int> twoMade {0, scopePage}; // 常规 (on show) and 搜索范围
        check(madePages(w) == twoMade && w->property("currentPage").toInt() == 0 && g_warnings == warned,
            u"hidden with pages 0 and %1 made: reset without warnings, back on page 0"_s.arg(scopePage));
        showOffScreen(w);
        settle(w);
        check(madePages(w) == twoMade && g_warnings == warned, u"shown again: nothing more made"_s);
        QMetaObject::invokeMethod(w, "refreshAfterOpen");
        pump(400);
        check(madePages(w) == twoMade && g_warnings == warned, u"after the first frame: nothing more made"_s);
        QMetaObject::invokeMethod(w, "fillInBackground");
        pump(250);
        const QList<int> partly = madePages(w);
        w->hide();
        pump(600);
        check(madePages(w) == partly && partly.size() < kSettingsPages && g_warnings == warned,
            u"hidden while filling (made %1): filling stops"_s.arg(pagesText(partly)));
        showOffScreen(w);
        settle(w);
        QMetaObject::invokeMethod(w, "refreshAfterOpen");
        QMetaObject::invokeMethod(w, "fillInBackground");
        qint64 took = 0;
        const qint64 block = longestBlock(4000, [&] { return w->property("searchReady").toBool(); }, &took);
        check(madePages(w) == all && g_warnings == warned, u"shown again, filled: the rest made in the background (%1 ms, longest block %2 ms)"_s
            .arg(took).arg(block));
        close(w);
    }

    // The background filling from scratch: one page per tick, each tick's time,
    // the GUI thread's longest block meanwhile.
    {
        const int warned = g_warnings;
        QQuickWindow* w = make();
        showOffScreen(w);
        settle(w);
        QMetaObject::invokeMethod(w, "refreshAfterOpen");
        QMetaObject::invokeMethod(w, "fillInBackground");
        qint64 took = 0;
        const qint64 block = longestBlock(5000, [&] { return w->property("searchReady").toBool(); }, &took);
        QStringList times;
        double slowest = 0;
        for (int i = 0; i < kSettingsPages; ++i) {
            const double ms = named(w, kSlotIds[i])->property("loadMs").toDouble();
            times.append(u"%1: %2"_s.arg(i).arg(ms));
            if (i > 0)
                slowest = std::max(slowest, ms);
        }
        say(u"lazy pages: made in %1 ms; each page's making (ms) %2; slowest after the first %3 ms; GUI thread's longest block %4 ms"_s
            .arg(took).arg(times.join(u", "_s)).arg(slowest).arg(block));
        check(madePages(w) == all && took < 3000 && g_warnings == warned, u"filling: every page made within 3 s"_s);
        // Typed once filling has made everything (按键一览's sections too), and
        // typed on a window just opened (found once it is all made): as many
        // found as on the window with everything made beforehand.
        const auto found = [&](QQuickWindow* in, const QString& word) {
            inputOf(in)->setProperty("text", word);
            QElapsedTimer waited;
            waited.start();
            while (!in->property("searchReady").toBool() && waited.elapsed() < 5000)
                pump(5);
            settle(in, 3);
            const int total = named(in, "settingsSearch")->property("total").toInt();
            inputOf(in)->setProperty("text", QString());
            settle(in, 2);
            return total;
        };
        // "Ctrl": rows of 按键一览 among them
        for (const QString& word : {QCoreApplication::translate("SettingsWindow", "快捷键", "settings tab"), u"Ctrl"_s}) {
            const int afterFilling = found(w, word);
            QQuickWindow* fresh = make();
            showOffScreen(fresh);
            settle(fresh);
            const int typedAtOnce = found(fresh, word);
            close(fresh);
            const int everything = found(window, word);
            check(afterFilling > 0 && afterFilling == everything && typedAtOnce == everything && g_warnings == warned,
                u"search '%1': %2 found after filling, %3 typed at once, as with everything made (%4)"_s
                    .arg(word).arg(afterFilling).arg(typedAtOnce).arg(everything));
        }
        close(w);
    }
    return ok;
}

int runSettings(const QStringList& args)
{
    if (args.contains(u"--glass-controls"_s))
        SystemTheme::setBackdropAvailable(true); // before the QML CONSTANT is read
    if (args.contains(u"--inactive-backdrop"_s))
        SystemTheme::setBackdropAvailable(valueAfter(args, u"--renderer") != u"software" && win::backdropSupported());
    const QDir out(valueAfter(args, kSettingsFlag));
    siteicons::setFolder(Settings::dataDir() + u"/icons/web"_s);
    const bool dark = args.contains(u"--dark"_s);
    const bool tree = args.contains(u"--tree"_s);
    QTranslator translator;
    if (args.contains(u"--english"_s) && translator.load(u":/i18n/winshun_en.qm"_s))
        QCoreApplication::installTranslator(&translator);
    say(u"starting (dark %1, scale %2)"_s.arg(dark).arg(qEnvironmentVariable("QT_SCALE_FACTOR")));
    // Where Qt came from and what the text is set in: two runs of the same
    // build can differ in line heights when they resolve these differently.
    const QFont font = QGuiApplication::font();
    const QFontMetricsF metrics(font);
    say(u"platform %1, plugins %2, qml %3, library paths %4"_s.arg(QGuiApplication::platformName(), QLibraryInfo::path(QLibraryInfo::PluginsPath),
            QLibraryInfo::path(QLibraryInfo::QmlImportsPath), QCoreApplication::libraryPaths().join(u"; "_s)));
    say(u"font '%1' -> '%2' %3px: ascent %4, descent %5, height %6, line spacing %7, hinting %8"_s.arg(font.family(), QFontInfo(font).family())
            .arg(QFontInfo(font).pixelSize()).arg(metrics.ascent()).arg(metrics.descent()).arg(metrics.height()).arg(metrics.lineSpacing())
            .arg(static_cast<int>(font.hintingPreference())));

    QQmlApplicationEngine engine;
    engine.addImageProvider(u"fileicon"_s, new FileIconProvider);
    // The settings as the window gets them: the defaults, plus what makes
    // every row show (the AI switches on, two network shares).
    Settings settings = Settings::defaults();
    const QString corners = valueAfter(args, u"--corners", settings.corners);
    if (corners == u"round" || corners == u"square")
        settings.corners = corners;
    SystemTheme::setRoundedCorners(settings.corners == u"round");
    settings.uiStyle = SystemTheme::uiStyle();
    settings.uiSize = valueAfter(args, u"--size", settings.uiSize);
    settings.uiFont = valueAfter(args, u"--font", settings.uiFont);
    uifont::apply(settings.uiFont);
    if (args.contains(u"--font-picker"_s))
        settings.save(); // the probe starts from a persisted choice, including an unchanged selection
    SystemTheme::setUiSize(settings.uiSize);
    settings.mcp = true;
    settings.mcpPlugins = true;
    settings.taskbarSearchBox = true; // its style row
    settings.pasteFileEnabled = true; // its card's two ways in
    settings.dialogBar = true; // the rows they keep away when off, whatever the defaults
    settings.folderMenu = u"auto"_s;
    settings.pluginBuiltinsExpanded = args.contains(u"--builtins"_s); // 内置功能's cards on show
    const QString shareOnline = u"\\\\192.168.1.10\\资料"_s;
    const QString shareSignIn = u"\\\\nas\\photos"_s;
    const QString shareWsl = u"\\\\wsl.localhost\\Ubuntu-24.04\\home\\me\\项目"_s;
    settings.shares = {{shareOnline, u"公司资料"_s}, {shareSignIn, {}}, {shareWsl, {}}};
    if (args.contains(u"--share-excluded"_s)) { // a share in a folder of 排除目录: its card says so
        settings.shares.append({u"\\\\nas\\video"_s, {}});
        settings.excludedPaths.append(u"\\\\nas\\video"_s);
    }
    settings.hotkey = u"Alt+Space"_s;
    settings.doubleCtrlExcludedApps = {u"TheFinals.exe"_s};
    settings.clipboardHotkey = u"Win+Alt+V"_s;
    if (args.contains(u"--clipboard-disabled"_s))
        settings.clipboard = false;
    settings.mcpExcludedPaths = {u"%USERPROFILE%\\Documents\\私人"_s};
    // The real view-model and its real neighbours, as App::showSettings makes
    // them; the renderer "running" is the one set, so that no restart is asked for.
    auto* editor = new SettingsEditor(settings, Settings::resolveRenderer(settings.renderer), &engine);
    QObject::connect(editor, &SettingsEditor::changed, editor, [editor] {
        SystemTheme::setRoundedCorners(editor->corners() == u"round");
        SystemTheme::setUiStyle(editor->uiStyle());
        SystemTheme::setGlassAppearance(editor->glassTransparency(), editor->glassClarity());
        SystemTheme::setBackdropOn(editor->transparency() == u"on");
        SystemTheme::setUiSize(editor->uiSize());
    });
    auto* frame = new WindowFrame(true, editor);
    auto* updater = new Updater(editor); // never setAutomatic() or check(): no network
    editor->plugins()->setLocale(u"zh"_s);
    if (!args.contains(u"--plugin-gallery-settings"_s))
        editor->plugins()->loadPreview(); // a plugin::Host would start WinShunPluginHost.exe: made-up cards instead
    editor->setMcpStatus(u"已连接：Claude Desktop（12 次调用）"_s);
    if (!args.contains(u"--no-cli-tally"_s)) // (the row's text before any run)
        editor->setCliTally(u"38 次调用，最近 14:32（search_files）"_s);
    // The command line as the page would read it off this PC, made up so that
    // the longest texts and every button can be seen whatever this PC has:
    // rich (the default: on the user's PATH by WinShun, another ws in front),
    // notonpath, missing, system, pending (the PATH not looked at yet) or unreadable (--cli-state).
    {
        cli::Status status;
        status.bin = u"C:\\Program Files\\WinShun\\bin"_s;
        const QString which = valueAfter(args, u"--cli-state", u"rich"_s);
        if (which != u"missing")
            status.exe = status.bin + u"\\ws.exe"_s;
        if (which == u"rich") {
            status.where = cli::Status::Where::User;
            status.removable = true;
            status.shadow = u"C:\\Users\\me\\AppData\\Local\\Microsoft\\WindowsApps\\ws.exe"_s;
        } else if (which == u"system") {
            status.where = cli::Status::Where::System;
        }
        if (which == u"unreadable")
            status.pathError = ERROR_UNSUPPORTED_TYPE; // a PATH that is not text in the registry
        // "pending": the PATH not looked at yet (the first look is on its way): ws.exe known, the PATH not.
        if (which == u"pending") {
            editor->cli()->setNeverLooks(true);
        } else {
            editor->cli()->setProbe([status] { return status; });
            editor->cli()->refresh();
        }
        if (args.contains(u"--cli-message"_s))
            editor->cli()->enable(); // there is no ws.exe by this program: it says so (the result row)
    }
    // A Skill installed, for Claude Code's row (--skill): in a configuration folder of its own.
    QTemporaryDir claudeConfig;
    if (args.contains(u"--skill"_s) && claudeConfig.isValid()) {
        _wputenv_s(L"CLAUDE_CONFIG_DIR", QDir::toNativeSeparators(claudeConfig.path()).toStdWString().c_str()); // not qputenv: that is the ANSI code page
        QDir().mkpath(claudeConfig.filePath(u"skills/winshun"_s));
        QFile skill(claudeConfig.filePath(u"skills/winshun/SKILL.md"_s));
        if (skill.open(QIODevice::WriteOnly))
            skill.write("---\nname: winshun\n---\n");
    }
    editor->setShareStatuses({
        shareStatus(shareOnline, ShareIndexer::State::Online, share::Problem::None, 48213),
        shareStatus(shareSignIn, ShareIndexer::State::Problem, share::Problem::NeedsSignIn, 0),
        shareStatus(shareWsl, ShareIndexer::State::Problem, share::Problem::WslStopped, 1230),
    });
    SettingsEditor::IndexFolderState indexState;
    indexState.folder = u"C:\\Users\\me\\AppData\\Local\\WinShun"_s;
    indexState.size = u"112 MB"_s;
    editor->setIndexFolderState(indexState);
    editor->setContentIndexStatus(u"已收录 337,215 个文件，其中 5,815 个文档"_s);
    // The 运行状况 page (settings page 10): made-up current readings, indexing content or (--health-idle) at rest.
    editor->setMonitor(args.contains(u"--health-monitor"_s) ? new HealthMonitor({}, editor)
        : HealthMonitor::demo(editor, args.contains(u"--health-idle"_s)));
    editor->setHistoryCount(128);
    editor->setClipboardCount(args.contains(u"--clipboard-disabled"_s) ? -1 : 57);
    { // 打开时显示's list, as Clipboard::openTabChoices gives it with two groups of the user's
        QVariantList tabs;
        for (const auto& [value, text] : std::initializer_list<std::pair<QString, QString>> {{u"all"_s, u"全部"_s},
                 {u"last"_s, u"上次看的"_s}, {u"text"_s, u"文本"_s}, {u"links"_s, u"链接"_s}, {u"images"_s, u"图片"_s},
                 {u"files"_s, u"文件"_s}, {u"pinned"_s, u"固定"_s}, {u"group:2"_s, u"常用回复"_s}, {u"group:3"_s, u"地址"_s}})
            tabs.append(QVariantMap {{u"value"_s, value}, {u"text"_s, text}});
        editor->setClipboardOpenTabs(tabs);
    }
    if (!args.contains(u"--no-entry-hotkeys"_s)) { // 单条内容的快捷键 as App::entryHotkeyRows gives it: one set,
        // one refused by the system, the one 设置快捷键 was just chosen for
        QVariantList rows;
        const auto row = [&rows](qint64 id, const QString& title, const QString& detail, const QString& hotkey,
                             const QString& error = {}) {
            rows.append(QVariantMap {{u"id"_s, id}, {u"title"_s, title}, {u"detail"_s, detail}, {u"hotkey"_s, hotkey},
                {u"error"_s, error}});
        };
        row(12, u"lingcore@example.com"_s, u"文字 · 20 字"_s, u"Ctrl+Alt+1"_s);
        row(31, u"收货地址：上海市浦东新区世纪大道 100 号"_s, u"文字 · 21 字"_s, u"Ctrl+Alt+D"_s,
            u"“Ctrl+Alt+D” 已被其他程序或系统占用，请换一个。"_s);
        row(47, u"会议纪要.docx"_s, u"1 个文件"_s, {});
        editor->setClipboardEntryHotkeys(rows);
    }
    editor->setHotkeyError({});
    // What the app reads off this PC, read-only: the file managers
    // installed, the taskbar, the AI clients and their configurations.
    // Not refreshNumberKeys(): it probes with RegisterHotKey.
    editor->refreshFileManagers();
    editor->refreshTaskbarState();
    editor->refreshMcpClients();
    editor->refreshAutostart(); // the switch as this PC has it (App reads it in the background)
    // A few favorites, in a file of their own that goes with the preview:
    // a folder renamed and with a keyword, a file, a folder that is gone.
    QTemporaryDir favoritesDir;
    Favorites favorites(favoritesDir.filePath(u"favorites.json"_s));
    favorites.add({Favorite::Kind::Folder, QDir::toNativeSeparators(QDir::tempPath()), u"临时文件"_s, u"tmp"_s});
    favorites.add({Favorite::Kind::File, QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), {}, {}});
    favorites.add({Favorite::Kind::Folder, u"Z:\\项目\\已删除的文件夹"_s, {}, u"xm"_s});
    editor->favorites()->setStore(&favorites);
    const QString command = u"\"C:\\Tools\\Code.exe\" {query}"_s;
    if (editor->favorites()->saveCommand(-1, u"编辑器"_s, u"tmp"_s, command, {}).isEmpty()
        || editor->favorites()->saveCommand(-1, {}, u"code"_s, command, {}).isEmpty()
        || editor->favorites()->saveCommand(-1, u"编辑器"_s, u"code"_s, u"{query}"_s, {}).isEmpty()
        || favorites.items().size() != 3)
        qWarning("preview: invalid custom command changed favorites");
    if (!editor->favorites()->saveCommand(-1, u"用编辑器打开"_s, u"code"_s, command, u"D:\\Projects"_s).isEmpty()
        || favorites.items().size() != 4)
        qWarning("preview: custom command was not saved");
    say(u"loading SettingsWindow.qml"_s);

    Backup backup; // no folder: never backs anything up
    QQmlComponent component(&engine, u"WinShun"_s, u"SettingsWindow"_s);
    QObject* object = component.createWithInitialProperties({
        {u"editor"_s, QVariant::fromValue(editor)},
        {u"frame"_s, QVariant::fromValue(frame)},
        {u"updater"_s, QVariant::fromValue(updater)},
        {u"backup"_s, QVariant::fromValue(&backup)},
    });
    auto* window = qobject_cast<QQuickWindow*>(object);
    if (!window) {
        say(component.errorString());
        delete object;
        return 3;
    }
    editor->setWindow(window);
    QTimer watchdog;
    armWatchdog(watchdog, 600);
    if (args.contains(u"--command-form"_s) && args.contains(u"--compact"_s))
        window->resize(760, 420);
    if (const int width = valueAfter(args, u"--width").toInt(); width >= window->minimumWidth()) // narrower than usual, down to the window's least
        window->resize(width, window->height());
    showOffScreen(window);
    const int baseHeight = window->height(); // 720: what the page column gets on a screen
    settle(window);
    say(u"showing %1x%2 (dpr %3), active %4"_s.arg(window->width()).arg(window->height()).arg(window->devicePixelRatio()).arg(window->isActive()));
    // The pages are made when first needed (SettingsWindow.loadPage), the one on
    // show with the window. --lazy-pages looks at that; everything else below at
    // all of them, made now.
    if (args.contains(u"--lazy-pages"_s)) {
        const auto make = [&] {
            auto* made = qobject_cast<QQuickWindow*>(component.createWithInitialProperties({
                {u"editor"_s, QVariant::fromValue(editor)},
                {u"frame"_s, QVariant::fromValue(frame)},
                {u"updater"_s, QVariant::fromValue(updater)},
                {u"backup"_s, QVariant::fromValue(&backup)},
            }));
            if (made)
                editor->setWindow(made);
            return made;
        };
        const bool ok = previewLazyPages(window, make, &engine, favorites.items().constFirst().path, out);
        editor->setWindow(window);
        return finish(window, ok);
    }
    QMetaObject::invokeMethod(window, "loadAllPages");
    settle(window);

    if (args.contains(u"--inactive-backdrop"_s)) {
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            say(u"%1: %2"_s.arg(ok ? u"ok"_s : u"FAILED"_s, QString::fromLatin1(name)));
            passed = passed && ok;
        };
        check(!window->isActive(), "production settings window is inactive");
        for (const QString& style : {u"liquid"_s, u"fluent"_s, u"brand"_s, u"tactile"_s}) {
            editor->setUiStyle(style);
            editor->setTransparency(u"on"_s);
            settle(window);
            check((window->color().alpha() == 0) == (style == u"liquid" && SystemTheme::backdrop() && win::materialsEnabled()),
                qPrintable(style + u" inactive background"));
            editor->setTransparency(u"off"_s);
            settle(window);
            check(window->color().alpha() == 255, qPrintable(style + u" transparency off is opaque"));
        }
        return finish(window, passed);
    }

    if (args.contains(u"--glass-controls"_s)) {
        bool passed = true;
        QJsonArray checks;
        const auto check = [&](bool ok, const QString& name) {
            checks.append(QJsonObject {{u"name"_s, name}, {u"passed"_s, ok}});
            passed = passed && ok;
            if (!ok) qWarning().noquote() << "glass controls:" << name;
        };
        editor->setUiStyle(u"liquid"_s);
        editor->setTransparency(u"on"_s);
        editor->resetGlassAppearance();
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, u"appearance"_s), Q_ARG(QVariant, 0));
        settle(window);
        auto* transparencyRow = visualNamed(window->contentItem(), u"glassTransparencyRow"_s);
        auto* clarityRow = visualNamed(window->contentItem(), u"glassClarityRow"_s);
        auto* transparencyControl = visualNamed(window->contentItem(), u"glassTransparencyControl"_s);
        auto* clarityControl = visualNamed(window->contentItem(), u"glassClarityControl"_s);
        check(transparencyRow && clarityRow && transparencyControl && clarityControl, u"two production controls exist"_s);
        if (transparencyRow && clarityRow && transparencyControl && clarityControl) {
            if (auto* slider = visualNamed(transparencyControl, u"settingSlider"_s)) {
                if (auto* pageView = qobject_cast<QQuickItem*>(named(window, "flick"))) {
                    const qreal y = transparencyControl->mapToItem(pageView, QPointF()).y();
                    pageView->setProperty("contentY", std::max(0.0, pageView->property("contentY").toDouble() + y - 220));
                }
                settle(window);
                auto* handle = slider->property("handle").value<QQuickItem*>();
                if (handle) {
                    const QPointF start = handle->mapToScene({handle->width() / 2, handle->height() / 2});
                    const QPointF end = slider->mapToScene({slider->width() - 20, slider->height() / 2});
                    const auto mouse = [&](QEvent::Type type, const QPointF& at, Qt::MouseButton button, Qt::MouseButtons buttons) {
                        QMouseEvent event(type, at, window->mapToGlobal(at), button, buttons, {});
                        QCoreApplication::sendEvent(window, &event);
                        settle(window);
                    };
                    mouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
                    mouse(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
                    Settings duringDrag;
                    duringDrag.load();
                    check(slider->property("pressed").toBool() && SystemTheme::glassTransparency() > 50
                            && editor->glassTransparency() == 50 && duringDrag.glassTransparency == 50,
                        u"pointer drag previews without committing or writing intermediate values"_s);
                    mouse(QEvent::MouseButtonRelease, end, Qt::LeftButton, {});
                    Settings afterDrag;
                    afterDrag.load();
                    check(editor->glassTransparency() > 50 && afterDrag.glassTransparency == editor->glassTransparency()
                            && SystemTheme::glassTransparency() == editor->glassTransparency(),
                        u"pointer release persists the previewed setting"_s);
                } else check(false, u"slider handle exists for pointer input"_s);
                editor->resetGlassAppearance();
            }
            for (auto* control : {transparencyControl, clarityControl}) {
                auto* slider = visualNamed(control, u"settingSlider"_s);
                check(slider && slider->isEnabled(), u"slider is usable with materials available"_s);
                if (!slider) continue;
                auto* accessible = QAccessible::queryAccessibleInterface(slider);
                check(accessible && accessible->role() == QAccessible::Slider && !accessible->text(QAccessible::Name).isEmpty(),
                    u"slider exposes its name and native accessibility role"_s);
                if (auto* pageView = qobject_cast<QQuickItem*>(named(window, "flick"))) {
                    const qreal y = control->mapToItem(pageView, QPointF()).y();
                    pageView->setProperty("contentY", std::max(0.0, pageView->property("contentY").toDouble() + y - 220));
                }
                settle(window);
                slider->forceActiveFocus(Qt::TabFocusReason);
                const auto key = [&](int code) {
                    QKeyEvent press(QEvent::KeyPress, code, {}), release(QEvent::KeyRelease, code, {});
                    QCoreApplication::sendEvent(window, &press);
                    QCoreApplication::sendEvent(window, &release);
                    settle(window);
                };
                const bool background = control == transparencyControl;
                key(Qt::Key_End);
                check((background ? editor->glassTransparency() : editor->glassClarity()) == 100, u"End applies the maximum"_s);
                key(Qt::Key_Home);
                check((background ? editor->glassTransparency() : editor->glassClarity()) == 0, u"Home applies the minimum"_s);
                key(Qt::Key_Right);
                check((background ? editor->glassTransparency() : editor->glassClarity()) == 1, u"Right adjusts by one"_s);
            }
            Settings saved;
            saved.load();
            check(saved.glassTransparency == 1 && saved.glassClarity == 1, u"keyboard adjustments persist"_s);
            editor->previewGlassAppearance(72, 37);
            saved.load();
            check(saved.glassTransparency == 1 && saved.glassClarity == 1
                    && SystemTheme::glassTransparency() == 72 && SystemTheme::glassClarity() == 37,
                u"live preview updates surfaces without saving intermediate values"_s);
            editor->resetGlassAppearance();
            saved.load();
            check(saved.glassTransparency == 50 && saved.glassClarity == 50 && SystemTheme::glassTransparency() == 50
                    && SystemTheme::glassClarity() == 50, u"reset restores both persisted values and live appearance"_s);
            for (const int width : {760, 920}) {
                window->setWidth(width);
                settle(window);
                check(transparencyControl->width() > 0 && clarityControl->width() > 0
                        && transparencyControl->mapToItem(window->contentItem(), QPointF(transparencyControl->width(), 0)).x() <= width,
                    u"controls stay inside the window at width %1"_s.arg(width));
                check(saveGrab(window, out.filePath(u"glass-controls-%1.png"_s.arg(width))), u"render controls at width %1"_s.arg(width));
            }
            editor->setTransparency(u"off"_s);
            settle(window);
            check(!transparencyRow->property("shown").toBool() && !clarityRow->property("shown").toBool(), u"off hides both parameters"_s);
            editor->setTransparency(u"on"_s);
            editor->setUiStyle(u"fluent"_s);
            settle(window);
            check(!transparencyRow->property("shown").toBool(), u"Mica styles do not expose Acrylic tuning"_s);
            editor->setUiStyle(u"liquid"_s);
        }
        QFile report(out.filePath(u"glass-controls.json"_s));
        check(report.open(QIODevice::WriteOnly), u"write control report"_s);
        if (report.isOpen()) report.write(QJsonDocument(QJsonObject {{u"passed"_s, passed}, {u"checks"_s, checks}}).toJson());
        return finish(window, passed);
    }

    if (args.contains(u"--style-picker"_s)) {
        QJsonArray checks;
        bool ok = true;
        const auto check = [&](bool condition, const QString& message) {
            checks.append(QJsonObject {{u"name"_s, message}, {u"passed"_s, condition}});
            ok = condition && ok;
            if (!condition) qWarning().noquote() << "style picker:" << message;
        };
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, u"appearance"_s), Q_ARG(QVariant, 0));
        settle(window);
        auto* liquid = visualNamed(window->contentItem(), u"styleCard_liquid"_s);
        auto* tactile = visualNamed(window->contentItem(), u"styleCard_tactile"_s);
        auto* fluent = visualNamed(window->contentItem(), u"styleCard_fluent"_s);
        check(liquid && tactile && fluent, u"all style cards exist"_s);
        if (liquid && tactile && fluent) {
            auto* accessible = QAccessible::queryAccessibleInterface(liquid);
            check(accessible && accessible->role() == QAccessible::RadioButton
                    && !accessible->text(QAccessible::Name).isEmpty(), u"style card exposes a named radio option"_s);
            for (const int width : {window->minimumWidth(), 920, 1080}) {
                window->resize(width, baseHeight);
                settle(window);
                check(liquid->isVisible() && liquid->width() >= 136, u"fourth card is readable at width %1"_s.arg(width));
                if (width >= 920)
                    check(qFuzzyCompare(liquid->y(), fluent->y()), u"all four styles share one row at width %1"_s.arg(width));
                check(liquid->x() >= 0 && liquid->x() + liquid->width() <= liquid->parentItem()->width() + 0.01,
                    u"fourth card stays inside its row at width %1"_s.arg(width));
                auto* flick = qobject_cast<QQuickItem*>(named(window, "flick"));
                if (flick) {
                    flick->setProperty("contentY", 0);
                    settle(window, 2);
                }
                check(saveGrab(window, out.filePath(u"picker-%1.png"_s.arg(width))), u"picker screenshot %1"_s.arg(width));
            }
            editor->setUiStyle(u"fluent"_s);
            settle(window);
            const auto key = [&](int code) {
                QKeyEvent press(QEvent::KeyPress, code, {}), release(QEvent::KeyRelease, code, {});
                QCoreApplication::sendEvent(window, &press);
                QCoreApplication::sendEvent(window, &release);
                settle(window);
            };
            tactile->forceActiveFocus(Qt::TabFocusReason);
            key(Qt::Key_Right);
            check(liquid->hasActiveFocus(), u"Right reaches the fourth style"_s);
            key(Qt::Key_Space);
            check(editor->uiStyle() == u"liquid" && SystemTheme::uiStyle() == u"liquid"
                    && liquid->property("selected").toBool(), u"Space applies Liquid glass"_s);
            check(accessible && accessible->state().checked, u"assistive technology receives the selected state"_s);
            Settings reloaded;
            reloaded.load();
            check(reloaded.uiStyle == u"liquid", u"Liquid glass survives settings reload"_s);
            key(Qt::Key_Left);
            check(tactile->hasActiveFocus(), u"Left returns to the previous style"_s);
            fluent->forceActiveFocus(Qt::TabFocusReason);
            key(Qt::Key_Return);
            check(editor->uiStyle() == u"fluent", u"Enter restores the native style"_s);
            const QPointF at = liquid->mapToScene({liquid->width() / 2, liquid->height() / 2});
            QMouseEvent press(QEvent::MouseButtonPress, at, window->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, {});
            QMouseEvent release(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), Qt::LeftButton, {}, {});
            QCoreApplication::sendEvent(window, &press);
            QCoreApplication::sendEvent(window, &release);
            settle(window);
            check(editor->uiStyle() == u"liquid", u"pointer click selects Liquid glass"_s);
        }
        QFile report(out.filePath(u"picker.json"_s));
        check(report.open(QIODevice::WriteOnly), u"picker report opens"_s);
        if (report.isOpen()) report.write(QJsonDocument(QJsonObject {{u"passed"_s, ok}, {u"checks"_s, checks}}).toJson());
        return finish(window, ok);
    }

    if (args.contains(u"--combo-popup"_s)) {
        bool ok = true;
        const auto check = [&](bool condition, const QString& message) {
            say((condition ? u"ok: "_s : u"FAILED: "_s) + message);
            ok = condition && ok;
        };
        const auto checkPicker = [&](const QString& name, const QString& page) { // the page's id
            QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, page), Q_ARG(QVariant, 0));
            settle(window, 3);
            auto* picker = visualNamed(window->contentItem(), name);
            check(picker && picker->isVisible(), name + u" is visible"_s);
            if (!picker)
                return;
            if (auto* pageView = qobject_cast<QQuickItem*>(named(window, "flick"))) {
                const qreal y = picker->mapToItem(pageView, QPointF()).y();
                pageView->setProperty("contentY", std::max(0.0, pageView->property("contentY").toDouble() + y - 130));
                settle(window, 2);
            }
            auto* fieldFrame = picker->property("background").value<QQuickItem*>();
            auto* field = fieldFrame ? fieldFrame->property("built").value<QQuickItem*>() : nullptr;
            auto* content = picker->property("contentItem").value<QQuickItem*>();
            const QVariant normalInk = content ? content->property("color") : QVariant{};
            auto* innerBackground = content ? content->property("background").value<QQuickItem*>() : nullptr;
            check(field && field->property("focused").isValid(), name + u" uses the theme input surface"_s);
            check(!innerBackground || !innerBackground->isVisible(), name + u" has no nested native text-field background"_s);
            check(content && std::abs(content->x() + content->property("leftPadding").toReal() - 12) <= 1 / window->devicePixelRatio(),
                name + u" preserves twelve-pixel text alignment"_s);
            picker->forceActiveFocus(Qt::TabFocusReason);
            settle(window, 2);
            check(field && field->property("focused").toBool(), name + u" shows keyboard focus on the whole field"_s);
            check(saveGrab(window, out.filePath(name + u"-focused.png"_s)), name + u" focused captured"_s);
            picker->setEnabled(false);
            settle(window, 2);
            check(picker->opacity() < 1 || (field && field->opacity() < 1)
                    || (content && normalInk.isValid() && content->property("color") != normalInk),
                name + u" uses the theme's disabled appearance"_s);
            check(saveGrab(window, out.filePath(name + u"-disabled.png"_s)), name + u" disabled captured"_s);
            picker->setEnabled(true);
            auto* popup = picker->property("popup").value<QObject*>();
            check(popup && QMetaObject::invokeMethod(popup, "open"), name + u" opens"_s);
            settle(window, 3);
            if (!popup)
                return;
            check(field && field->property("focused").toBool(), name + u" retains its field focus while the menu is open"_s);
            auto* list = popup->property("contentItem").value<QQuickItem*>();
            auto* background = popup->property("background").value<QQuickItem*>();
            auto* surface = background ? background->property("built").value<QQuickItem*>() : nullptr;
            check(surface && (surface->property("radius").toReal() > 0) == (settings.corners == u"round"),
                name + u" follows the corner setting"_s);
            if (list) {
                const qreal menuTop = list->mapToScene(QPointF()).y() - popup->property("topPadding").toReal();
                const qreal menuBottom = menuTop + popup->property("height").toReal();
                const qreal gap = std::max(menuTop - picker->mapToScene(QPointF(0, picker->height())).y(),
                    picker->mapToScene(QPointF()).y() - menuBottom);
                check(gap >= 4 - 1 / window->devicePixelRatio(), name + u" leaves a gap beside its control"_s);
                // The rows are one level down, beside a search row or their scroll bar.
                if (auto* inner = list->property("list").value<QQuickItem*>())
                    list = inner;
            }
            check(popup->property("padding").toReal() > 0 && list
                    && list->width() < popup->property("width").toReal(),
                name + u" keeps rows inside the menu frame"_s);
            if (list) {
                for (auto* child : list->childItems()) {
                    check(!child->inherits("QQuickRectangle") || child->width() < list->width()
                            || child->height() < list->height(),
                        name + u" has no rectangular overlay around the list"_s);
                }
                auto* row = list->property("currentItem").value<QQuickItem*>();
                check(row && row->width() <= list->width(), name + u" fits its highlighted row (%1 in %2, index %3)"_s
                    .arg(row ? row->width() : -1).arg(list->width()).arg(list->property("currentIndex").toInt()));
            }
            check(saveGrab(window, out.filePath(name + u".png")), name + u" captured"_s);
            QMetaObject::invokeMethod(popup, "close");
        };
        checkPicker(u"doubleTapActionPicker"_s, u"keys"_s);
        checkPicker(u"uiFontPicker"_s, u"appearance"_s);
        return finish(window, ok);
    }

    if (args.contains(u"--health-monitor"_s)) {
        bool ok = true;
        const auto check = [&](bool condition, const char* message) {
            if (!condition) {
                ok = false;
                qWarning("health monitor: %s", message);
            }
        };
        auto* monitor = editor->monitor();
        int updates = 0;
        QObject::connect(monitor, &HealthMonitor::updated, monitor, [&] { ++updates; });
        const auto show = [&](int page) {
            QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, page), Q_ARG(QVariant, 0));
            settle(window);
        };
        const auto waitForProcesses = [&] {
            QElapsedTimer timer;
            timer.start();
            while (monitor->now().value(u"processes"_s).toList().isEmpty() && timer.elapsed() < 5000)
                pump(50);
            check(!monitor->now().value(u"processes"_s).toList().isEmpty(), "live process readings did not arrive");
        };
        check(!monitor->watching() && monitor->now().isEmpty(), "hidden page started monitoring");
        show(window->property("healthPage").toInt());
        check(monitor->watching(), "opening the page did not start monitoring");
        waitForProcesses();
        check(monitor->now().value(u"total"_s).toDouble() > 0, "live memory reading is empty");
        check(saveGrab(window, out.filePath(u"health-live.png"_s)), "health page did not render");
        monitor->refreshMemory();
        show(window->property("generalPage").toInt()); // leave while the delayed refresh may still be pending
        const int hiddenUpdates = updates;
        monitor->poke();
        monitor->refreshMemory();
        pump(1200);
        check(!monitor->watching() && monitor->now().isEmpty() && updates == hiddenUpdates,
            "hidden page kept publishing readings");
        show(window->property("healthPage").toInt());
        waitForProcesses();
        check(monitor->watching() && updates > hiddenUpdates, "reopening did not resume monitoring");
        const int beforeRefresh = updates;
        monitor->refreshMemory();
        pump(400);
        check(updates > beforeRefresh, "memory refresh did not publish a reading");
        { // the same breakdown as the diagnostics package gets it
            const double pageTotal = monitor->now().value(u"total"_s).toDouble();
            const QJsonObject snap = HealthMonitor::snapshot({}, 0, 0);
            const QJsonObject parts = snap.value(u"parts_mb"_s).toObject();
            double sum = 0;
            for (const auto& part : parts)
                sum += part.toDouble();
            const double total = snap.value(u"total_mb"_s).toDouble();
            check(total > 0 && std::abs(sum - total) < 2, "snapshot: the parts do not add up to the total");
            check(std::abs(total - pageTotal) < std::max(8.0, pageTotal * 0.1), "snapshot: not the page's total");
            check(snap.value(u"processes"_s).toArray().at(0).toObject().value(u"kind"_s).toString() == u"main",
                "snapshot: the main process is not listed first");
            check(snap.value(u"map_mb"_s).toObject().value(u"total"_s).toDouble() > 0, "snapshot: no page map");
            const QJsonObject counters = snap.value(u"main_counters"_s).toObject();
            check(counters.value(u"handles"_s).toInteger() > 0 && counters.value(u"peak_working_set_mb"_s).toDouble() > 0
                    && counters.contains(u"uptime_minutes"_s),
                "snapshot: the process's own counts are missing");
            QFile file(out.filePath(u"health-snapshot.json"_s));
            check(file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(snap).toJson()) > 0, "snapshot not written");
        }
        show(window->property("generalPage").toInt());
        return finish(window, ok);
    }

    if (args.contains(u"--navigation-settings"_s))
        return finish(window, checkNavigationSettings(window, editor, out));
    if (args.contains(u"--plugin-review-settings"_s))
        return finish(window, checkPluginReviewSettings(window, editor, out));
    if (args.contains(u"--plugin-gallery-settings"_s))
        return finish(window, checkPluginGallerySettings(window, editor, out));
    if (args.contains(u"--double-tap-settings"_s))
        return finish(window, checkDoubleTapSettings(window, editor, out));
    if (args.contains(u"--search-experience-settings"_s))
        return finish(window, checkSearchExperienceSettings(window, editor, out));
    if (args.contains(u"--plugin-readme-reading"_s)) // + --readme <a README.md>; else a sample of every kind of block
        return finish(window, checkPluginReadmeReading(window, editor, out, valueAfter(args, u"--readme")));
    if (args.contains(u"--logo-position-settings"_s))
        return finish(window, checkLogoPositionSettings(window, editor, out));
    if (args.contains(u"--dialog-rule-settings"_s)) // + --fixture tests\fixtures\wpf-file-window.ps1
        return finish(window, checkDialogRuleSettings(window, editor, out, valueAfter(args, u"--fixture")));
    if (args.contains(u"--optimization-settings"_s))
        return finish(window, checkOptimizationSettings(window, editor, out));
    if (args.contains(u"--donate-place"_s)) // the 捐赠 button to the 拾穗计划 page and back (GitHub #402)
        return finish(window, checkDonatePlace(window, editor, out));
    if (args.contains(u"--update-failed"_s)) // the update dialog after a failed install
        return finish(window, checkUpdateFailed(window, updater, out));

    // The three tables, side by side: a page without a keyword entry or a
    // tabs entry would read undefined.
    const QVariantList pages = window->property("pages").toList();
    const QVariantList keywords = window->property("pageKeywords").toList();
    const QVariantList tabs = window->property("pageTabs").toList();
    const int gleaningPage = window->property("gleaningPage").toInt();
    const int aiPage = window->property("aiPage").toInt();
    const int pluginsPage = window->property("pluginsPage").toInt();
    const int favoritesPage = window->property("favoritesPage").toInt();
    say(u"pages %1, pageKeywords %2, pageTabs %3"_s.arg(pages.size()).arg(keywords.size()).arg(tabs.size()));
    if (pages.size() != keywords.size() || pages.size() != tabs.size())
        qWarning("preview: pages, pageKeywords and pageTabs differ in length");
    if (pages.value(aiPage).toMap().value(u"title"_s).toString() != QCoreApplication::translate("SettingsWindow", "AI 助手"))
        qWarning("preview: aiPage is not the AI 助手 page");
    if (pages.value(pluginsPage).toMap().value(u"title"_s).toString() != QCoreApplication::translate("SettingsWindow", "插件"))
        qWarning("preview: pluginsPage is not the 插件 page");
    if (pages.value(favoritesPage).toMap().value(u"title"_s).toString() != QCoreApplication::translate("SettingsWindow", "收藏夹"))
        qWarning("preview: favoritesPage is not the 收藏夹 page");
    if (!pages.value(gleaningPage).toMap().value(u"gleaning"_s).toBool() || gleaningPage != pages.size() - 1)
        qWarning("preview: gleaningPage is not the last page, or not marked gleaning");

    QObject* flick = named(window, "flick");
    QObject* searchBox = named(window, "searchBox");
    QObject* settingsSearch = named(window, "settingsSearch");
    if (!flick || !searchBox || !settingsSearch)
        return finish(window, false);
    QQuickItem* input = nullptr; // the box's TextInput, what the user types into
    for (QQuickItem* item : searchBox->findChildren<QQuickItem*>()) {
        if (item->inherits("QQuickTextInput"))
            input = item;
    }
    if (!input) {
        qWarning("preview: no TextInput in the search box");
        return finish(window, false);
    }

    // GitHub #111: an item that reads Screen.* has a Screen attached object of
    // its own, and on every move to another monitor each of them leaves the
    // old QScreen (a disconnect that walks all of that screen's connections)
    // and joins the new one: the hitch grows with the square of their number.
    // The settings window keeps them few (SettingsWindow.dpr).
    if (args.contains(u"--screen-objects"_s)) {
        constexpr int budget = 500; // 1,631 here before #111, 355 after
        int count = 0;
        const std::function<void(QQuickItem*)> walk = [&](QQuickItem* item) {
            for (const QObject* child : item->children())
                count += qstrcmp(child->metaObject()->className(), "QQuickScreenAttached") == 0;
            for (QQuickItem* child : item->childItems())
                walk(child);
        };
        walk(window->contentItem());
        say(u"Screen attached objects: %1 (at most %2)"_s.arg(count).arg(budget));
        return finish(window, count > 0 && count <= budget);
    }

    // #163: the page's name and tabs stay (pageHeader, above the scrolled
    // page); another tab starts at the top; a row gone to by the search, by
    // the page's scrolling to it or by Tab lands whole below the header.
    if (args.contains(u"--page-scroll"_s)) {
        bool ok = true;
        const auto check = [&](bool condition, const QString& message) {
            say((condition ? u"ok: "_s : u"FAILED: "_s) + message);
            ok = condition && ok;
        };
        auto* flick = qobject_cast<QQuickItem*>(named(window, "flick"));
        auto* header = qobject_cast<QQuickItem*>(named(window, "pageHeader"));
        auto* page = qobject_cast<QQuickItem*>(named(window, "page"));
        if (!flick || !header || !page)
            return finish(window, false);
        const auto top = [&](QQuickItem* item) { return item->mapToItem(window->contentItem(), QPointF()).y(); };
        const auto contentY = [&] { return flick->property("contentY").toDouble(); };
        const auto show = [&](int pageIndex, int tab) {
            QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, pageIndex), Q_ARG(QVariant, tab));
            settle(window);
            pump(150);
        };
        const std::function<QQuickItem*(QQuickItem*, const QString&)> rowTitled = [&](QQuickItem* item, const QString& title) -> QQuickItem* {
            if (item->isVisible() && item->property("title").toString() == title && item->property("stackControl").isValid())
                return item;
            for (QQuickItem* child : item->childItems()) {
                if (QQuickItem* found = rowTitled(child, title))
                    return found;
            }
            return nullptr;
        };
        // Whole in the view: under the header, over the window's bottom.
        const auto inView = [&](QQuickItem* item) {
            return top(item) >= top(header) + header->height() - 0.5 && top(item) + item->height() <= window->height() + 0.5;
        };
        const QString deepTitle = u"删除，Ctrl+Z 撤销"_s;

        const int keysPage = window->property("keysPage").toInt();
        const int keysListTab = window->property("keysListTab").toInt();
        show(keysPage, keysListTab); // 快捷键 › 按键一览: many rows
        const double headerTop = top(header);
        check(flick->property("contentHeight").toDouble() > window->height() + 400, u"the page is longer than the window"_s);
        flick->setProperty("contentY", 900.0);
        settle(window, 2);
        check(contentY() > 800 && qFuzzyCompare(top(header), headerTop) && header->isVisible(), u"scrolled, the page's name and tabs stay"_s);
        saveGrab(window, out.filePath(u"page-scrolled.png"_s));
        QMetaObject::invokeMethod(window, "showTab", Q_ARG(QVariant, 0)); // 快捷键 has two tabs: back to 打开 Win顺
        settle(window, 2);
        check(contentY() == 0 && window->property("currentTab").toInt() == 0, u"another tab starts at the top"_s);

        // The search: Enter goes to the row, in view whole below the header.
        QMetaObject::invokeMethod(window, "showSetting", Q_ARG(QVariant, keysPage), Q_ARG(QVariant, keysListTab), Q_ARG(QVariant, deepTitle));
        pump(500);
        settle(window, 2);
        QQuickItem* deep = rowTitled(page, deepTitle);
        check(deep && contentY() > 0 && inView(deep), u"the search lands on a deep row, whole below the header"_s);
        saveGrab(window, out.filePath(u"page-searched.png"_s));

        // The page scrolled to a row (the plugin card, the capture's hotkeys).
        flick->setProperty("contentY", 0.0);
        settle(window, 2);
        if (deep) {
            QMetaObject::invokeMethod(window, "revealRow", Q_ARG(QVariant, QVariant::fromValue<QObject*>(deep)));
            settle(window, 2);
            check(inView(deep), u"revealRow brings a deep row in whole below the header"_s);
            const double before = top(deep);
            flick->setProperty("contentY", contentY() - 40); // a little too low
            settle(window, 2);
            QMetaObject::invokeMethod(window, "revealRow", Q_ARG(QVariant, QVariant::fromValue<QObject*>(deep)));
            settle(window, 2);
            check(inView(deep) && top(deep) >= top(header) + header->height() - 0.5 && before > 0, u"revealRow keeps a row from under the header"_s);
        }

        // Tab to controls out of view, below and then above.
        show(window->property("clipboardPage").toInt(), 0); // 剪贴板 › 记录: switch after switch
        QList<QQuickItem*> tabbable;
        const std::function<void(QQuickItem*)> collect = [&](QQuickItem* item) {
            if (item->isVisible() && item->activeFocusOnTab() && item->isEnabled())
                tabbable.append(item);
            for (QQuickItem* child : item->childItems())
                collect(child);
        };
        collect(page);
        check(tabbable.size() > 3 && top(tabbable.last()) > window->height(), u"the page has controls to Tab to, some below the view (%1)"_s.arg(tabbable.size()));
        if (tabbable.size() > 3) {
            flick->setProperty("contentY", 0.0);
            settle(window, 2);
            tabbable.last()->forceActiveFocus(Qt::TabFocusReason);
            pump(150);
            settle(window, 2);
            check(contentY() > 0 && inView(tabbable.last()), u"Tab to a control below scrolls it whole into view"_s);
            tabbable.first()->forceActiveFocus(Qt::TabFocusReason);
            pump(150);
            settle(window, 2);
            check(inView(tabbable.first()), u"Tab to a control above scrolls it whole into view, below the header"_s);
        }

        // The mouse, as the user's: moves, presses and the wheel through the window.
        const auto mouse = [&](QEvent::Type type, QPointF at, Qt::MouseButtons buttons) {
            QMouseEvent event(type, at, window->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, {});
            QCoreApplication::sendEvent(window, &event);
        };
        const auto wheel = [&](QPointF at, int notches) {
            for (int i = 0; i < std::abs(notches); ++i) {
                QWheelEvent event(at, window->mapToGlobal(at), {}, QPoint(0, notches < 0 ? 120 : -120), Qt::NoButton, {}, Qt::NoScrollPhase, false);
                QCoreApplication::sendEvent(window, &event);
            }
            pump(400); // the glides (SystemTheme.animations)
            settle(window, 2);
        };
        const auto centre = [&](QQuickItem* item) { return item->mapToItem(window->contentItem(), QPointF(item->width() / 2, item->height() / 2)); };

        // GitHub #229: the page's scroll bar is one to use: it widens under
        // the mouse, its thumb drags the page, a click beside it pages.
        auto* bar = qobject_cast<QQuickItem*>(visualNamed(window->contentItem(), u"settingsScrollBar"_s));
        // Never over a control at the page's right: the bar's whole width (all
        // of it takes the mouse) right of every switch on 剪贴板 › 记录.
        double rightmost = 0;
        for (QQuickItem* item : std::as_const(tabbable))
            if (item->isVisible())
                rightmost = std::max(rightmost, item->mapToScene(QPointF(item->width(), 0)).x());
        const double barLeft = bar ? bar->mapToScene({}).x() : 0.0;
        check(bar && rightmost > 0 && rightmost <= barLeft + 0.5,
            u"the scroll bar keeps clear of the page's controls (the rightmost ends at %1, the bar starts at %2)"_s.arg(rightmost).arg(barLeft));
        // Nor in the window's resize edge: on a real screen a press there
        // sizes the window, and the bar never widens (the system takes the mouse).
        const double barRight = bar ? bar->mapToScene(QPointF(bar->width(), 0)).x() : 0.0;
        const double edgeAt = window->width() - (frame ? frame->resizeMargin(window->devicePixelRatio()) : 0.0);
        check(bar && frame && barRight <= edgeAt + 0.5,
            u"the scroll bar keeps clear of the window's resize edge (it ends at %1, the edge starts at %2)"_s.arg(barRight).arg(edgeAt));
        show(keysPage, keysListTab); // 快捷键 › 按键一览 again: long
        check(bar && bar->isVisible() && bar->width() >= 10, u"the page has a scroll bar to grab"_s);
        if (bar && bar->isVisible()) {
            flick->setProperty("contentY", 0.0);
            settle(window, 2);
            const QPointF thumbAt = bar->mapToItem(window->contentItem(), QPointF(bar->width() - 5, 4 + 12)); // the thumb's top, at rest
            mouse(QEvent::MouseMove, thumbAt, Qt::NoButton);
            settle(window, 2);
            check(bar->property("expanded").toBool(), u"the scroll bar widens under the mouse"_s);
            mouse(QEvent::MouseButtonPress, thumbAt, Qt::LeftButton);
            for (int step = 1; step <= 6; ++step)
                mouse(QEvent::MouseMove, thumbAt + QPointF(0, step * 30), Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, thumbAt + QPointF(0, 180), Qt::NoButton);
            settle(window, 2);
            const double dragged = contentY();
            check(dragged > 300, u"dragging the thumb scrolls the page (to %1)"_s.arg(dragged));
            const QPointF below = bar->mapToItem(window->contentItem(), QPointF(bar->width() - 5, bar->height() - 8));
            mouse(QEvent::MouseButtonPress, below, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, below, Qt::NoButton);
            settle(window, 2);
            check(contentY() > dragged + flick->height() / 2, u"a click below the thumb goes a page down"_s);
            mouse(QEvent::MouseMove, centre(page), Qt::NoButton);
        }

        // GitHub #232: the wheel over the plugin message log scrolls the log,
        // never the page around it, at its ends too; over a log that fits,
        // the page as anywhere. A new message moves it only from the newest.
        auto* plugins = editor->plugins();
        if (!plugins->developerMode())
            plugins->setDeveloperMode(true);
        for (int i = 0; i < 40; ++i)
            plugins->log()->append({u"14:1%1:00.000"_s.arg(i % 10), u"lingcore.kill"_s, i % 2 ? u"←"_s : u"→"_s,
                u"result #%1"_s.arg(i), u"{\"jsonrpc\":\"2.0\",\"id\":%1}"_s.arg(i), 3});
        const int fullHeight = window->height();
        window->resize(window->width(), 560); // low enough that the page scrolls too
        settle(window, 2);
        show(window->property("pluginsPage").toInt(), 2); // 插件 › 开发者
        auto* logList = qobject_cast<QQuickItem*>(visualNamed(window->contentItem(), u"pluginLogList"_s));
        check(logList && logList->isVisible() && logList->property("interactive").toBool(), u"the plugin log is there, longer than its box"_s);
        if (logList && logList->isVisible()) {
            const auto logY = [&] { return logList->property("contentY").toDouble(); };
            QMetaObject::invokeMethod(window, "revealRow", Q_ARG(QVariant, QVariant::fromValue<QObject*>(logList)));
            settle(window, 2);
            check(logList->property("atYEnd").toBool(), u"the log opens at the newest message"_s);
            const double pageAt = contentY();
            say(u"page at %1 of %2, the log %3 of %4"_s.arg(pageAt).arg(flick->property("contentHeight").toDouble() - flick->height())
                    .arg(logY()).arg(logList->property("contentHeight").toDouble() - logList->height()));
            check(pageAt > 0, u"the page can scroll up from where the log is"_s);
            const QPointF over = centre(logList);
            wheel(over, -3); // up: the log moves, the page stays
            check(!logList->property("atYEnd").toBool() && qFuzzyCompare(contentY() + 1, pageAt + 1),
                u"the wheel up over the log scrolls the log only (page %1 → %2)"_s.arg(pageAt).arg(contentY()));
            wheel(over, -60); // far past its top
            check(logList->property("atYBeginning").toBool() && qFuzzyCompare(contentY() + 1, pageAt + 1),
                u"at the log's top more wheel leaves the page where it is (page %1 → %2)"_s.arg(pageAt).arg(contentY()));
            wheel(over, 60); // and past its bottom
            check(logList->property("atYEnd").toBool() && qFuzzyCompare(contentY() + 1, pageAt + 1),
                u"at the log's bottom more wheel leaves the page where it is (page %1 → %2)"_s.arg(pageAt).arg(contentY()));
            wheel(over, -3);
            const double reading = logY();
            plugins->log()->append({u"14:20:00.000"_s, u"lingcore.kill"_s, u"→"_s, u"query #new"_s, u"{}"_s, -1});
            settle(window, 2);
            check(qFuzzyCompare(logY() + 1, reading + 1), u"a new message leaves a log scrolled up to read where it is"_s);
            wheel(over, 60);
            plugins->log()->append({u"14:20:01.000"_s, u"lingcore.kill"_s, u"←"_s, u"result #new"_s, u"{}"_s, 2});
            settle(window, 2);
            check(logList->property("atYEnd").toBool(), u"a new message comes into view at the newest"_s);
            saveGrab(window, out.filePath(u"plugin-log.png"_s));
            // A log that fits: the wheel over it scrolls the page.
            plugins->clearLog();
            settle(window, 2);
            check(!logList->property("interactive").toBool(), u"an empty log has nothing to scroll"_s);
            QMetaObject::invokeMethod(window, "revealRow", Q_ARG(QVariant, QVariant::fromValue<QObject*>(logList)));
            settle(window, 2);
            const double before = contentY();
            check(before > 0, u"the page can scroll up from the empty log"_s);
            wheel(centre(logList), -2);
            check(contentY() < before, u"the wheel over a log that fits scrolls the page (%1 → %2)"_s.arg(before).arg(contentY()));
            // A message clicked open in a short log: the box grows to its
            // most, and the message stays where it was clicked, whole.
            for (int i = 0; i < 12; ++i)
                plugins->log()->append({u"14:3%1:00.000"_s.arg(i % 10), u"lingcore.kill"_s, u"→"_s, u"query #%1"_s.arg(i),
                    u"{\"jsonrpc\":\"2.0\",\"id\":%1,\"method\":\"query\",\"params\":{\"text\":\"%2\"}}"_s.arg(i).arg(QString(160, u'x')), -1});
            settle(window, 2);
            QMetaObject::invokeMethod(window, "revealRow", Q_ARG(QVariant, QVariant::fromValue<QObject*>(logList)));
            settle(window, 2);
            QQuickItem* first = nullptr;
            QMetaObject::invokeMethod(logList, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, first), Q_ARG(int, 0));
            check(first && !logList->property("interactive").toBool(), u"twelve messages fit the log"_s);
            if (first) {
                mouse(QEvent::MouseButtonPress, centre(first), Qt::LeftButton);
                mouse(QEvent::MouseButtonRelease, centre(first), Qt::NoButton);
                settle(window, 2);
                check(first->property("open").toBool() && logList->property("interactive").toBool() && logY() < 1
                        && first->height() <= logList->height(),
                    u"the first message opened stays in view, whole (log at %1, row %2 of %3)"_s.arg(logY()).arg(first->height()).arg(logList->height()));
            }
        }
        window->resize(window->width(), fullHeight);
        return finish(window, ok);
    }

    if (args.contains(u"--web-form"_s)) {
        if (args.contains(u"--compact"_s))
            window->resize(760, 420); // the supported minimum, also used on high-DPI screens
        return finish(window, previewWebForms(window, editor, out, tree));
    }

    if (args.contains(u"--plugin-recovery"_s)) {
        bool ok = true;
        const auto check = [&](bool condition, const char* message) {
            if (!condition) {
                qWarning("plugin recovery: %s", message);
                ok = false;
            }
        };
        auto* plugins = editor->plugins();
        plugins->setRecoveryState(App::tr("正在从插件库恢复 %1。原来的设置和数据会保留。")
            .arg(QCoreApplication::translate("MovedPlugins", "截图与录屏")), true);
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(pluginsPage)), Q_ARG(QVariant, QVariant(0)));
        window->resize(args.contains(u"--compact"_s) ? QSize(760, 420) : QSize(920, 720));
        settle(window, 3);
        QObject* row = visualNamed(window->contentItem(), u"pluginRecoveryRow"_s);
        QObject* retry = visualNamed(window->contentItem(), u"retryPluginRecovery"_s);
        QObject* local = visualNamed(window->contentItem(), u"installRecoveryArchive"_s);
        if (!row || !retry || !local)
            return finish(window, false);
        int requests = 0;
        QObject::connect(plugins, &PluginSettings::recoveryRequested, plugins, [&] {
            ++requests;
            plugins->setRecoveryState(App::tr("正在从插件库恢复 %1。原来的设置和数据会保留。")
                .arg(QCoreApplication::translate("MovedPlugins", "截图与录屏")), true);
        });
        const auto grabRecovery = [&](const QString& name) {
            settle(window, 3);
            ok = saveGrab(window, out.filePath(name + u".png"_s)) && ok;
        };
        plugins->setRecoveryState(App::tr("正在从插件库恢复 %1。原来的设置和数据会保留。")
            .arg(QCoreApplication::translate("MovedPlugins", "截图与录屏")), true);
        check(!retry->property("enabled").toBool() && !local->property("enabled").toBool(), "busy controls accept another operation");
        QMetaObject::invokeMethod(retry, "clicked");
        check(requests == 0, "busy recovery was requested again");
        grabRecovery(u"recovery-busy"_s);

        plugins->setRecoveryState(App::tr("以下功能尚未恢复：%1。原来的设置和数据仍在，可以重试或从本地安装插件包。")
            .arg(App::tr("%1：%2").arg(QCoreApplication::translate("MovedPlugins", "截图与录屏"),
                App::tr("网络连接失败"))));
        check(retry->property("enabled").toBool() && local->property("enabled").toBool(), "offline recovery has no enabled retry/local action");
        grabRecovery(u"recovery-offline"_s);
        QMetaObject::invokeMethod(retry, "clicked");
        check(requests == 1 && plugins->recovering(), "retry did not reach the view-model");
        grabRecovery(u"recovery-retry"_s);
        plugins->setRecoveryState({}, false);
        settle(window, 2);
        check(!row->property("shown").toBool(), "completed recovery kept its warning row");
        say(u"plugin recovery: real QML controls, busy/offline/retry/completed states; no network or file picker used"_s);
        return finish(window, ok);
    }

    // 快捷键 › 打开 Win顺's 截图的快捷键 row and the 按键一览 entry follow the capture plugin: not installed, off, on without
    // and with shortcuts (App::updateScreenshotPlugin hands them over).
    if (args.contains(u"--capture-hotkey"_s)) {
        bool ok = true;
        auto* row = window->findChild<QQuickItem*>(u"captureHotkeyRow"_s);
        auto* button = window->findChild<QQuickItem*>(u"captureHotkeyButton"_s);
        if (!row || !button)
            return finish(window, false);
        window->resize(args.contains(u"--compact"_s) ? QSize(760, 420) : QSize(920, 720));
        QMetaObject::invokeMethod(window, "goTo", Q_ARG(QVariant, QVariant::fromValue(row)));
        const auto overview = [&] { // the keys of the 截图 entry among the shortcuts
            for (const QVariant& section : window->property("shortcutSections").toList()) {
                for (const QVariant& entry : section.toMap().value(u"rows"_s).toList()) {
                    if (entry.toMap().value(u"text"_s).toString() == u"截图"_s)
                        return entry.toMap().value(u"keys"_s).toString();
                }
            }
            return u"(no entry)"_s;
        };
        struct Case {
            const char* name;
            bool installed, on;
            QStringList keys;
            QString description, button, keys0;
        };
        const Case cases[] = {
            {"not-installed", false, false, {}, u"可选插件"_s, u"去插件库"_s, {}},
            {"off", true, false, {}, u"插件还没打开"_s, u"去设置"_s, {}},
            {"no-key", true, true, {}, u"还没设"_s, u"去设置"_s, {}},
            {"key", true, true, {u"F1"_s, u"Ctrl+Alt+F2"_s}, u"现在是 F1 / Ctrl+Alt+F2"_s, u"去设置"_s, u"F1 / Ctrl+Alt+F2"_s},
        };
        for (const Case& c : cases) {
            editor->setScreenshotPlugin(c.installed, c.on, c.keys);
            settle(window, 3);
            const QString description = row->property("description").toString();
            const QString text = button->property("text").toString();
            const QString keys = overview();
            const bool good = description.contains(c.description) && text == c.button && keys == c.keys0
                && row->property("values").toStringList() == c.keys;
            say(u"capture-hotkey %1: \"%2\" / button \"%3\" / overview \"%4\"%5"_s.arg(QString::fromLatin1(c.name), description, text, keys,
                good ? QString() : u" -- WRONG"_s));
            ok = good && saveGrab(window, out.filePath(u"capture-hotkey-%1.png"_s.arg(QString::fromLatin1(c.name)))) && ok;
        }
        return finish(window, ok);
    }

    if (args.contains(u"--clipboard-screenshots"_s)) {
        bool ok = true;
        const auto check = [&](bool condition, const char* message) {
            if (!condition) {
                qWarning("clipboard screenshots: %s", message);
                ok = false;
            }
        };
        window->resize(QSize(920, 720));
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, window->property("clipboardPage")), Q_ARG(QVariant, QVariant(0)));
        settle(window, 3);
        auto* row = visualNamed(window->contentItem(), u"clipboardScreenshotsRow"_s);
        auto* tabs = visualNamed(window->contentItem(), u"clipboardScreenshotsTabs"_s);
        check(row && tabs, "clipboardScreenshotsRow or clipboardScreenshotsTabs not found");
        if (!row || !tabs)
            return finish(window, false);

        // Hidden while the screenshot plugin is off.
        editor->setScreenshotPlugin(true, false, {});
        settle(window, 3);
        check(!row->isVisible(), "row should stay hidden when screenshot plugin is off");

        // Shown once the screenshot plugin is on; clicking each segment updates state and persists.
        editor->setScreenshotPlugin(true, true, {u"F1"_s});
        settle(window, 3);
        check(row->isVisible(), "row should be visible when screenshot plugin is on");
        check(editor->clipboardScreenshots() == u"all"_s, "initial mode should be all");
        check(tabs->property("current").toInt() == 0, "initial tab should be 0");
        ok = saveGrab(window, out.filePath(u"clipboard-screenshots-all.png"_s)) && ok;

        const auto clickTab = [&](int index) {
            const QPointF center = tabs->mapToScene(QPointF(tabs->width() * (index + 0.5) / 3.0, tabs->height() * 0.5));
            QMouseEvent press(QEvent::MouseButtonPress, center, center, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, center, center, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &press);
            QCoreApplication::sendEvent(window, &release);
            settle(window, 3);
        };

        clickTab(1);
        check(editor->clipboardScreenshots() == u"copied"_s, "clicking second tab should select copied");
        check(tabs->property("current").toInt() == 1, "tab index should be 1");
        {
            Settings persisted;
            persisted.load();
            check(persisted.clipboardScreenshots == u"copied"_s, "copied mode should persist to Settings");
        }
        ok = saveGrab(window, out.filePath(u"clipboard-screenshots-copied.png"_s)) && ok;

        clickTab(2);
        check(editor->clipboardScreenshots() == u"off"_s, "clicking third tab should select off");
        check(tabs->property("current").toInt() == 2, "tab index should be 2");
        {
            Settings persisted;
            persisted.load();
            check(persisted.clipboardScreenshots == u"off"_s, "off mode should persist to Settings");
        }
        ok = saveGrab(window, out.filePath(u"clipboard-screenshots-off.png"_s)) && ok;

        // Invalid setter value is ignored.
        editor->setClipboardScreenshots(u"invalid"_s);
        check(editor->clipboardScreenshots() == u"off"_s, "invalid mode should be ignored");

        return finish(window, ok);
    }

    if (args.contains(u"--backup-settings"_s)) {
        bool ok = true;
        const auto check = [&](bool condition, const char* message) {
            if (!condition) {
                qWarning("backup settings: %s", message);
                ok = false;
            }
        };
        auto* row = window->findChild<QQuickItem*>(u"backupExportRow"_s);
        auto* button = window->findChild<QQuickItem*>(u"backupExportButton"_s);
        auto* status = window->findChild<QQuickItem*>(u"backupOperationStatus"_s);
        if (!row || !button || !status)
            return finish(window, false);
        window->resize(args.contains(u"--compact"_s) ? QSize(760, 420) : QSize(920, 720));
        QMetaObject::invokeMethod(window, "goTo", Q_ARG(QVariant, QVariant::fromValue(row)));
        settings.save(); // this preview's isolated profile, never the user's
        const auto waitForBackup = [&] {
            QElapsedTimer timeout;
            timeout.start();
            while (backup.busy() && timeout.elapsed() < 10000)
                pump(20);
            check(!backup.busy(), "operation did not finish");
            settle(window, 3);
            QMetaObject::invokeMethod(window, "revealRow", Q_ARG(QVariant, QVariant::fromValue(row)));
            settle(window, 3);
            auto* viewport = qobject_cast<QQuickItem*>(flick);
            const auto inView = [&](QQuickItem* item) {
                const QPointF top = item->mapToItem(viewport, QPointF());
                return top.x() >= 0 && top.y() >= 0 && top.x() + item->width() <= viewport->width() + 1
                    && top.y() + item->height() <= viewport->height() + 1;
            };
            check(inView(button) && inView(status), "export or its feedback is outside the viewport");
        };
        backup.backUpNow();
        check(!button->isEnabled(), "export stayed enabled during a backup");
        waitForBackup();
        check(backup.problem().isEmpty() && !backup.message().isEmpty(), "success was not reported");
        check(!QDir(Settings::dataDir() + u"/backups"_s).entryList({u"*.zip"_s}, QDir::Files).isEmpty(),
            "worker did not write a backup");
        check(status->property("text").toString() == backup.message(), "success did not reach the view");
        bool saved = saveGrab(window, out.filePath(u"backup-success.png"_s));
        const QString blocked = Settings::dataDir() + u"/blocked"_s;
        QFile file(blocked);
        check(file.open(QIODevice::WriteOnly), "cannot create the failure fixture");
        file.close();
        backup.setFolder(blocked); // a file where a directory is needed
        backup.backUpNow();
        waitForBackup();
        backup.setFolder({});
        settle(window, 3);
        check(!backup.problem().isEmpty() && status->property("text").toString().contains(backup.problem()),
            "failure was hidden with automatic backups off");
        check(button->isEnabled(), "export did not become available after failure");
        saved = saveGrab(window, out.filePath(u"backup-failure.png"_s)) && saved;
        if (tree)
            writeTree(window, out.filePath(u"backup.tree.txt"_s));
        say(u"backup worker, success/error feedback and disabled controls checked on an isolated profile"_s);
        return finish(window, ok && saved);
    }

    if (args.contains(u"--share-scan"_s)) {
        bool ok = true;
        const auto check = [&](bool condition, const char* message) {
            ok = condition && ok;
            if (!condition)
                qWarning("share scan: %s", message);
        };
        editor->setShareScan(0, true, u"23:15"_s, u"06:30"_s, 3);
        editor->setShareScan(0, true, u"26:00"_s, u"06:30"_s, 2);
        check(editor->shares().front().toMap().value(u"scanDepth"_s).toInt() == 3, "invalid time was accepted");
        editor->setShareScan(2, true, u"22:00"_s, u"07:00"_s, 2);
        editor->setShareStatuses({
            shareStatus(shareOnline, ShareIndexer::State::Paused, share::Problem::None, 48213),
            shareStatus(shareSignIn, ShareIndexer::State::Problem, share::Problem::NeedsSignIn, 0),
            shareStatus(shareWsl, ShareIndexer::State::Paused, share::Problem::None, 1230),
        });
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(u"scope"_s)), Q_ARG(QVariant, QVariant(3))); // 搜索范围 › 网络与 WSL
        window->resize(args.contains(u"--compact"_s) ? QSize(760, 420) : QSize(920, 720));
        settle(window, 3);
        std::function<QQuickItem*(QQuickItem*)> findControls = [&](QQuickItem* item) -> QQuickItem* {
            if (item->objectName() == u"shareScanControls" && item->property("share").toMap().value(u"path"_s).toString() == shareOnline)
                return item;
            for (auto* child : item->childItems()) {
                if (auto* found = findControls(child))
                    return found;
            }
            return nullptr;
        };
        auto* controls = findControls(window->contentItem());
        if (!controls)
            return finish(window, false);
        auto* expand = visualNamed(controls, u"scanExpand"_s);
        check(expand && QMetaObject::invokeMethod(expand, "clicked"), "scan controls did not expand");
        auto* from = visualNamed(controls, u"scanFrom"_s);
        auto* until = visualNamed(controls, u"scanUntil"_s);
        auto* depth = visualNamed(controls, u"scanDepth"_s);
        if (!from || !until || !depth)
            return finish(window, false);
        QMetaObject::invokeMethod(from, "takeFocus");
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &tab);
        check(until->property("inputFocused").toBool(), "Tab did not reach the end-time field");
        from->setProperty("text", u"23:45"_s);
        // A background status refresh must not replace an in-progress edit.
        editor->setShareContent(0, true);
        check(from->property("text").toString() == u"23:45", "status refresh discarded the time draft");
        QMetaObject::invokeMethod(from, "editingFinished");
        depth->setProperty("text", u"4"_s);
        QMetaObject::invokeMethod(depth, "editingFinished");
        const auto scan = editor->shares().front().toMap();
        check(scan.value(u"scanFrom"_s).toString() == u"23:45" && scan.value(u"scanDepth"_s).toInt() == 4, "QML edits did not reach settings");
        QMetaObject::invokeMethod(window, "goTo", Q_ARG(QVariant, QVariant::fromValue(partOf(window, u"scope"_s, "sharesRow"))));
        settle(window, 3);
        if (auto* viewport = qobject_cast<QQuickItem*>(flick)) {
            const auto top = controls->parentItem()->mapToItem(viewport, QPointF());
            flick->setProperty("contentY", flick->property("contentY").toDouble() + top.y() - 12);
            settle(window, 3);
        }
        check(saveGrab(window, out.filePath(u"share-scan.png"_s)), "settings did not render");
        if (args.contains(u"--compact"_s)) {
            const auto bottom = controls->mapToItem(qobject_cast<QQuickItem*>(flick), QPointF(0, controls->height()));
            const double height = flick->property("height").toDouble();
            flick->setProperty("contentY", flick->property("contentY").toDouble() + std::max(0.0, bottom.y() - height + 12));
            settle(window, 3);
            check(saveGrab(window, out.filePath(u"share-scan-bottom.png"_s)), "compact scan controls did not render");
        }
        if (tree)
            writeTree(window, out.filePath(u"share-scan.tree.txt"_s));
        say(u"share scan: per-folder settings, validation, background refresh and QML edits checked on an isolated profile"_s);
        return finish(window, ok);
    }

    if (args.contains(u"--wsl-folders"_s)) {
        bool saved = true;
        const auto check = [](bool condition, const char* message) {
            if (!condition)
                qWarning("WSL folders: %s", message);
        };
        const int count = editor->shares().size();
        check(!editor->addShare(u"\"\\\\wsl$\\Ubuntu-24.04\\home\\me\\项目\""_s).isEmpty(),
            "the second WSL host alias was not recognized as the same folder");
        check(editor->shares().size() == count, "a duplicate WSL folder was added");
        check(editor->addShare(u"\\\\wsl.localhost\\Ubuntu-24.04\\home\\me\\Case"_s).isEmpty(), "adding Case failed");
        check(editor->addShare(u"\\\\wsl.localhost\\Ubuntu-24.04\\home\\me\\case"_s).isEmpty(), "adding case failed");
        check(editor->shares().size() == count + 2, "case-sensitive folders were merged");
        editor->removeShare(count + 1);
        editor->removeShare(count);
        const auto stopped = editor->shares().last().toMap();
        check(stopped.value(u"startable"_s).toBool() && !stopped.value(u"signIn"_s).toBool(), "a stopped WSL folder must offer Start, never Sign in");
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(u"scope"_s)), Q_ARG(QVariant, QVariant(3))); // 搜索范围 › 网络与 WSL
        window->setWidth(args.contains(u"--compact"_s) ? 760 : 920);
        window->setHeight(args.contains(u"--compact"_s) ? 420 : 780);
        settle(window, 3);
        saved = saveGrab(window, out.filePath(u"wsl-stopped.png"_s)) && saved;
        editor->setShareStatuses({
            shareStatus(shareOnline, ShareIndexer::State::Online, share::Problem::None, 48213),
            shareStatus(shareSignIn, ShareIndexer::State::Problem, share::Problem::NeedsSignIn, 0),
            shareStatus(shareWsl, ShareIndexer::State::Online, share::Problem::None, 1234),
        });
        check(!editor->shares().last().toMap().value(u"startable"_s).toBool(), "a running WSL folder still offers Start");
        settle(window, 3);
        saved = saveGrab(window, out.filePath(u"wsl-online.png"_s)) && saved;
        if (args.contains(u"--compact"_s)) {
            flick->setProperty("contentY", std::max(0.0, flick->property("contentHeight").toDouble() - flick->property("height").toDouble()));
            settle(window, 3);
            saved = saveGrab(window, out.filePath(u"wsl-bottom.png"_s)) && saved;
        }
        return finish(window, saved);
    }

    if (args.contains(u"--indexing"_s)) {
        const auto check = [](bool ok, const char* message) {
            if (!ok)
                qWarning("indexing: %s", message);
        };
        SettingsEditor::IndexAccessState direct;
        direct.effective = u"direct"_s;
        direct.elevated = direct.administrator = true;
        direct.foldersPossible = false;
        editor->setIndexAccessState(direct);
        QTemporaryDir folder;
        check(folder.isValid(), "temporary folder was not created");
        check(editor->addFolder(folder.path()).isEmpty(), "adding a folder while elevated was rejected");
        // Manual input & %VAR% preservation (#421): indexed folders and excluded paths.
        check(editor->addFolder(u"\"%TEMP%/\""_s).isEmpty(), "adding %TEMP% indexed folder failed");
        check(!editor->addFolder(u"%TEMP%"_s).isEmpty(), "duplicate %TEMP% indexed folder was accepted");
        check(!editor->addFolder(u"%WINSHUN_NO_SUCH_VAR_421%\\Work"_s).isEmpty(), "unresolved indexed folder was accepted");
        const auto foldersList = editor->folders();
        const auto tempCard = std::ranges::find_if(foldersList, [](const QVariant& v) {
            return v.toMap().value(u"path"_s).toString() == u"%TEMP%";
        });
        check(tempCard != foldersList.end(), "%TEMP% indexed folder was auto-expanded or missing");
        if (tempCard != foldersList.end())
            check(!tempCard->toMap().value(u"resolved"_s).toString().isEmpty(), "%TEMP% indexed folder missing resolved preview");
        check(editor->addExcludedPath(u"\"%LOCALAPPDATA%/WinShunManualTest/\""_s).isEmpty(),
            "adding %LOCALAPPDATA% excluded folder failed");
        check(editor->excludedPaths().contains(u"%LOCALAPPDATA%\\WinShunManualTest"_s),
            "excluded folder %VAR% was auto-expanded");
        check(!editor->resolveEnvPath(u"%LOCALAPPDATA%\\WinShunManualTest"_s).isEmpty(),
            "resolveEnvPath did not expand %LOCALAPPDATA%");
        check(editor->addExcludedPath(u"%WINSHUN_CUSTOM_VAR_421%\\Cache"_s).isEmpty(),
            "unresolved portable %VAR% excluded folder was rejected");
        check(editor->excludedPaths().contains(u"%WINSHUN_CUSTOM_VAR_421%\\Cache"_s),
            "unresolved %VAR% excluded folder missing");
        check(!editor->addExcludedPath(u"node_modules"_s).isEmpty(), "bare folder name accepted as excluded path");
        check(!editor->addExcludedPath(u"%LOCALAPPDATA%\\WinShunManualTest"_s).isEmpty(),
            "duplicate %VAR% excluded folder was accepted");
        check(!editor->addExcludedPath(editor->resolveEnvPath(u"%LOCALAPPDATA%\\WinShunManualTest"_s)).isEmpty(),
            "duplicate expanded excluded folder was accepted");
        window->resize(args.contains(u"--compact"_s) ? QSize(760, 420) : QSize(920, 720));
        bool saved = true;
        const auto grab = [&](int tab, const QString& name) {
            QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(u"scope"_s)), Q_ARG(QVariant, QVariant(tab))); // 搜索范围
            flick->setProperty("contentY", 0);
            settle(window, 3);
            saved = saveGrab(window, out.filePath(name + u"-top.png"_s)) && saved;
            flick->setProperty("contentY", std::max(0.0, flick->property("contentHeight").toDouble() - flick->property("height").toDouble()));
            settle(window, 3);
            saved = saveGrab(window, out.filePath(name + u"-bottom.png"_s)) && saved;
        };
        grab(0, u"folders-admin"_s);
        editor->setAllDrives(false);
        for (const auto& value : editor->drives())
            editor->setDriveSelected(value.toMap().value(u"root"_s).toString(), false);
        check(!editor->allDrives(), "empty drive selection was treated as all drives");
        for (const auto& value : editor->drives())
            check(!value.toMap().value(u"selected"_s).toBool(), "drive remained selected");
        grab(2, u"drives-empty"_s);
        const auto drives = editor->drives();
        if (!drives.isEmpty())
            editor->setDriveSelected(drives.front().toMap().value(u"root"_s).toString(), true);
        editor->setDriveSelected(u"Z:"_s, true); // disconnected selections stay visible and can be removed
        grab(2, u"drives-selected"_s);
        editor->setAllDrives(true);
        check(editor->allDrives(), "restoring all drives failed");
        return finish(window, saved);
    }

    // --service-blocked: 搜索范围 › 磁盘 after an install no protected folder
    // would take (GitHub #426): what is wrong, and the commands to copy.
    if (args.contains(u"--service-blocked"_s)) {
        bool saved = true;
        const auto grab = [&](svcloc::Weakness::Kind kind, const wchar_t* path, const wchar_t* sid, const QString& name) {
            svcloc::Weakness weak;
            weak.kind = kind;
            weak.path = path;
            weak.sid = sid;
            SettingsEditor::IndexAccessState state;
            state.effective = u"walk"_s;
            state.administrator = state.serviceInstallable = state.portable = true;
            state.serviceState = u"not_installed"_s;
            state.serviceMessage = SettingsEditor::serviceBlockedMessage(weak);
            QStringList commands;
            for (const std::wstring& command : svcloc::repairCommands(weak))
                commands.append(QString::fromStdWString(command));
            state.serviceRepair = commands.join(u'\n');
            editor->setIndexAccessState(state);
            QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(u"scope"_s)), Q_ARG(QVariant, QVariant(2))); // 搜索范围 › 磁盘
            settle(window, 3);
            flick->setProperty("contentY", std::max(0.0, flick->property("contentHeight").toDouble() - flick->property("height").toDouble()));
            settle(window, 3);
            saved = saveGrab(window, out.filePath(name)) && saved;
        };
        window->resize(args.contains(u"--compact"_s) ? QSize(760, 560) : QSize(920, 720));
        grab(svcloc::Weakness::Kind::Owner, LR"(C:\)", L"S-1-5-32-545", u"service-blocked-owner.png"_s); // a user's in life; here a name every Windows has
        grab(svcloc::Weakness::Kind::Rights, LR"(C:\Program Files)", L"S-1-5-11", u"service-blocked-rights.png"_s);
        grab(svcloc::Weakness::Kind::Unreadable, LR"(C:\Program Files)", L"", u"service-blocked-unknown.png"_s);
        return finish(window, saved);
    }

    if (args.contains(u"--file-types"_s)) {
        bool passed = true;
        const auto check = [&](bool condition, const char* message) {
            passed = passed && condition;
            if (!condition)
                qWarning("file types: %s", message);
        };
        check(!editor->setFileTypeKeyword(0, u"folder"_s).isEmpty(), "folder prefix collision was accepted");
        check(!editor->setFileTypeKeyword(0, u"two words"_s).isEmpty(), "invalid prefix was accepted");
        check(editor->setFileTypeKeyword(0, u":DOC"_s).isEmpty(), "custom prefix was rejected");
        check(editor->fileTypes().value(0).toMap().value(u"keyword"_s).toString() == u"doc", "prefix was not normalized");
        check(!editor->setFileTypeKeyword(-1, u"doc"_s).isEmpty(), "type prefix collision was accepted");
        check(editor->setFileTypeKeyword(0, {}).isEmpty(), "disabling a prefix failed");
        check(editor->setFileTypeFolder(0, u"\"H:/工作 目录/\""_s).isEmpty(), "文件类型范围未接受完整路径");
        check(editor->fileTypes().value(0).toMap().value(u"folder"_s).toString() == u"H:\\工作 目录"_s,
            "文件类型范围未规范化或未暴露给界面");
        check(!editor->setFileTypeFolder(0, u"relative/path"_s).isEmpty(), "范围接受了相对路径");
        check(editor->fileTypes().value(0).toMap().value(u"folder"_s).toString() == u"H:\\工作 目录"_s,
            "无效范围覆盖了原来的路径");
        check(!editor->setFileTypeFolder(-1, {}).isEmpty(), "文件夹类型意外接受了文件类型范围");
        check(editor->setFileTypeFolder(0, {}).isEmpty(), "清空文件类型范围失败");
        check(editor->fileTypes().value(0).toMap().value(u"folder"_s).toString().isEmpty(), "范围未清空");
        editor->resetFileTypes();
        check(editor->addFileType(u"工作 CSV"_s, u"csv"_s).isEmpty(), "新增范围示例失败");
        const int csv = static_cast<int>(editor->fileTypes().size()) - 1;
        check(editor->setFileTypeKeyword(csv, u"cw"_s).isEmpty(), "设置范围示例前缀失败");
        check(editor->setFileTypeFolder(csv, u"H:\\__work__"_s).isEmpty(), "设置范围示例目录失败");
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(u"results"_s)), Q_ARG(QVariant, QVariant(3))); // 结果列表 › 类型与筛选
        window->resize(args.contains(u"--compact"_s) ? QSize(760, 420) : QSize(920, 720));
        settle(window, 3);
        auto* scopeButton = visualNamed(window->contentItem(), u"fileTypeScope_0"_s);
        check(scopeButton && scopeButton->isVisible(), "未显示范围按钮");
        if (scopeButton) {
            scopeButton->forceActiveFocus(Qt::TabFocusReason);
            settle(window, 3);
            const QPointF at = scopeButton->mapToScene({scopeButton->width() / 2, scopeButton->height() / 2});
            QMouseEvent press(QEvent::MouseButtonPress, at, window->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, {});
            QMouseEvent release(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), Qt::LeftButton, {}, {});
            QCoreApplication::sendEvent(window, &press);
            QCoreApplication::sendEvent(window, &release);
            settle(window, 3);
            auto* input = visualNamed(window->contentItem(), u"fileTypeFolder_0"_s);
            check(input && input->isVisible(), "点击范围按钮未展开路径输入框");
        }
        // The rows in the strip's order, 文件夹 among the kinds (csv + 1 kinds and it).
        const std::function<void(QQuickItem*, QQuickItem*)> inside = [&](QQuickItem* row, QQuickItem* item) {
            for (auto* child : item->childItems()) {
                if (!child->isVisible())
                    continue;
                check(child->mapToItem(row, QPointF(child->width(), 0)).x() <= row->width() + 0.5, "类型或范围控件超出卡片宽度");
                inside(row, child);
            }
        };
        for (int i = 0; i <= csv + 1; ++i) {
            auto* row = visualNamed(window->contentItem(), u"fileTypeRow_%1"_s.arg(i));
            check(row != nullptr, "文件类型行不存在");
            if (row)
                inside(row, row);
        }
        bool saved = saveGrab(window, out.filePath(u"file-types-top.png"_s));
        // 文件夹 by its handle two rows down, past 文档 and 图片; then 上移 one back.
        check(editor->folderChipAt() == 0, "文件夹 is not first");
        if (auto* first = visualNamed(window->contentItem(), u"fileTypeRow_0"_s); first) {
            // Its bottom to the third row's (文档's is taller, with its range open).
            auto* third = visualNamed(window->contentItem(), u"fileTypeRow_2"_s);
            const qreal by = third ? third->y() + third->height() - first->y() - first->height() : 0;
            const QPointF grip = first->mapToScene(QPointF(12, 16));
            systemMouse(window, QEvent::MouseButtonPress, grip);
            for (int i = 1; i <= 20; ++i) {
                systemMouse(window, QEvent::MouseMove, grip + QPointF(0, by * i / 20));
                QCoreApplication::processEvents();
            }
            pump(200);
            settle(window, 2);
            saved = saveGrab(window, out.filePath(u"file-types-drag.png"_s)) && saved;
            systemMouse(window, QEvent::MouseButtonRelease, grip + QPointF(0, by));
            settle(window, 3);
            check(by > 0 && editor->folderChipAt() == 2, "文件夹's handle did not move it two rows down");
            check(editor->fileTypes().value(0).toMap().value(u"name"_s).toString() == u"文档", "dragging 文件夹 moved a kind");
        }
        editor->moveStripChip(2, 1); // its 上移
        check(editor->folderChipAt() == 1 && !editor->fileTypesDefault(), "上移 did not move 文件夹, or the order counts as the default");
        editor->moveStripChip(3, 0); // a kind to the top, 文件夹 after it as before
        check(editor->fileTypes().value(0).toMap().value(u"name"_s).toString() == u"视频" && editor->folderChipAt() == 2,
            "moving a kind to the top did not keep 文件夹 where it was");
        editor->removeFileType(0); // 视频 gone: 文件夹 stays after 文档
        check(editor->folderChipAt() == 1, "removing a kind before 文件夹 did not keep it after the same kind");
        editor->resetFileTypes();
        check(editor->folderChipAt() == 0, "恢复默认 did not put 文件夹 first");
        check(editor->addFileType(u"工作 CSV"_s, u"csv"_s).isEmpty() && editor->setFileTypeKeyword(csv, u"cw"_s).isEmpty()
                && editor->setFileTypeFolder(csv, u"H:\\__work__"_s).isEmpty(),
            "putting the range example back failed");
        settle(window, 3);
        // The quick filters, below the types on the same tab.
        check(editor->quickFilters().isEmpty(), "quick filters: some before any was added");
        check(!editor->addQuickFilter(u" "_s, u"ext:pdf"_s).isEmpty(), "quick filters: one with no name was accepted");
        check(!editor->addQuickFilter(u"坏的"_s, u"regex:("_s).isEmpty(), "quick filters: an unreadable query was accepted");
        check(!editor->addQuickFilter(u"空的"_s, u"  "_s).isEmpty(), "quick filters: an empty query was accepted");
        check(editor->addQuickFilter(u" 排除回收站 "_s, u" !$RECYCLE.BIN\\ "_s).isEmpty(), "quick filters: a good one was rejected");
        check(!editor->addQuickFilter(u"排除回收站"_s, u"ext:pdf"_s).isEmpty(), "quick filters: a name was accepted twice");
        check(editor->addQuickFilter(u"大 PDF"_s, u"ext:pdf size:>10mb"_s).isEmpty(), "quick filters: a second one was rejected");
        check(editor->quickFilters().value(0).toMap().value(u"query"_s).toString() == u"!$RECYCLE.BIN\\", "quick filters: the query was not trimmed");
        check(!editor->renameQuickFilter(1, u"排除回收站"_s).isEmpty(), "quick filters: renamed to a name taken");
        check(!editor->setQuickFilterQuery(1, u"regex:("_s).isEmpty(), "quick filters: an unreadable query replaced a good one");
        check(editor->quickFilters().value(1).toMap().value(u"query"_s).toString() == u"ext:pdf size:>10mb", "quick filters: a rejected query was kept");
        check(!editor->setQuickFilterKeyword(0, u"two words"_s).isEmpty(), "quick filters: an unusable prefix was accepted");
        check(!editor->setQuickFilterKeyword(0, u"cw"_s).isEmpty(), "quick filters: a kind's prefix was accepted");
        check(!editor->setQuickFilterKeyword(0, editor->folderKeyword()).isEmpty(), "quick filters: the folders' prefix was accepted");
        check(editor->setQuickFilterKeyword(0, u":NoBin"_s).isEmpty(), "quick filters: a good prefix was rejected");
        check(editor->quickFilters().value(0).toMap().value(u"keyword"_s).toString() == u"nobin", "quick filters: the prefix was not normalized");
        check(!editor->setQuickFilterKeyword(1, u"nobin"_s).isEmpty(), "quick filters: a prefix was accepted twice");
        check(!editor->setFileTypeKeyword(0, u"nobin"_s).isEmpty(), "quick filters: a kind took a filter's prefix");
        editor->moveQuickFilter(1, -1);
        check(editor->quickFilters().value(0).toMap().value(u"name"_s).toString() == u"大 PDF", "quick filters: moving one up did nothing");
        // A sort of its own (GitHub #392): kept by name, an unknown one not at all.
        editor->setQuickFilterSort(0, u"largest"_s);
        check(editor->quickFilters().value(0).toMap().value(u"sort"_s).toString() == u"largest", "quick filters: its sort was not kept");
        editor->setQuickFilterSort(1, u"sideways"_s);
        check(editor->quickFilters().value(1).toMap().value(u"sort"_s).toString().isEmpty(), "quick filters: an unknown sort was kept");
        QMetaObject::invokeMethod(window, "showQuickFiltersPage", Q_ARG(QVariant, QVariant(u"!node_modules\\"_s)));
        settle(window, 3);
        auto* draft = visualNamed(window->contentItem(), u"quickFilterNewQuery"_s);
        check(draft && draft->isVisible() && draft->property("text").toString() == u"!node_modules\\", "quick filters: the draft is not in the new row");
        auto* newName = visualNamed(window->contentItem(), u"quickFilterNewName"_s);
        check(newName && newName->property("inputFocused").toBool(), "quick filters: the name box did not take the keyboard");
        for (int i = 0; i < 2; ++i) {
            auto* row = visualNamed(window->contentItem(), u"quickFilterRow_%1"_s.arg(i));
            check(row && row->isVisible(), "quick filters: a row is missing");
            if (row && row->parentItem()) {
                check(row->x() + row->width() <= row->parentItem()->width() + 0.5, "quick filters: a row is wider than its card");
            }
        }
        if (auto* sort = visualNamed(window->contentItem(), u"quickFilterSort_0"_s))
            check(sort->isVisible() && sort->property("currentIndex").toInt() == 7, "quick filters: the sort box does not show 大的在前");
        else
            check(false, "quick filters: the sort box is missing");
        // The card whole, its rows and the drafted new one: the page scrolled to its end
        // (an inactive preview never scrolls to the focused box by itself).
        flick->setProperty("contentY", std::max(0.0, flick->property("contentHeight").toDouble() - flick->property("height").toDouble()));
        settle(window, 3);
        saved = saveGrab(window, out.filePath(u"quick-filters.png"_s)) && saved;
        editor->removeQuickFilter(0);
        editor->removeQuickFilter(0);
        check(editor->quickFilters().isEmpty(), "quick filters: removing them left some");
        flick->setProperty("contentY", std::max(0.0, flick->property("contentHeight").toDouble() - flick->property("height").toDouble()));
        settle(window, 3);
        saved = saveGrab(window, out.filePath(u"file-types-bottom.png"_s)) && saved;
        return finish(window, saved && passed);
    }

    if (args.contains(u"--font-picker"_s)) {
        bool ok = true;
        const auto check = [&](bool condition, const QString& message) {
            say((condition ? u"ok: "_s : u"FAILED: "_s) + message);
            ok = condition && ok;
        };
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(u"appearance"_s)), Q_ARG(QVariant, QVariant(0)));
        settle(window, 3);
        auto* picker = visualNamed(window->contentItem(), u"uiFontPicker"_s);
        auto* sample = visualNamed(window->contentItem(), u"uiFontSample"_s);
        if (!picker || !sample)
            return finish(window, false);
        const QString running = QGuiApplication::font().family();
        check(running == uifont::resolvedFamily(settings.uiFont)
                && input->property("font").value<QFont>().family() == running,
            u"startup preference reaches the real settings search field"_s);
        const QString family = u"Microsoft YaHei UI"_s;
        check(editor->uiFonts().contains(family), u"YaHei UI is selectable"_s);
        check(!picker->property("editable").toBool(), u"the field itself takes no typing"_s);
        flick->setProperty("contentY", std::max(0.0, flick->property("contentY").toDouble()
            + picker->mapToItem(qobject_cast<QQuickItem*>(flick), QPointF()).y() - 130));
        settle(window, 2);
        auto* popup = picker->property("popup").value<QObject*>();
        check(popup && QMetaObject::invokeMethod(popup, "open"), u"font list opens"_s);
        settle(window, 2);
        auto* search = popup ? visualNamed(popup->property("contentItem").value<QQuickItem*>(), u"uiFontSearch"_s) : nullptr;
        auto* fontRows = popup ? popup->property("contentItem").value<QQuickItem*>()->property("list").value<QQuickItem*>() : nullptr;
        check(search && search->property("inputFocused").toBool(), u"the menu's search row takes the keyboard"_s);
        check(fontRows && std::abs(fontRows->height() - 5 * fontRows->property("currentItem").value<QQuickItem*>()->height()) < 0.5
                && fontRows->property("count").toInt() > 5,
            u"the menu shows five fonts and scrolls the rest"_s);
        check(saveGrab(window, out.filePath(u"font-list.png"_s)), u"font list captured"_s);
        if (search)
            search->setProperty("text", u"yahei ui"_s);
        settle(window, 2);
        check(picker->property("matches").toStringList().value(0) == family, u"typing in the menu narrows the list"_s);
        check(saveGrab(window, out.filePath(u"font-list-filtered.png"_s)), u"filtered font list captured"_s);
        check(QMetaObject::invokeMethod(picker, "acceptQuery"), u"found font accepted by the production picker"_s);
        settle(window, 2);
        check(popup && !popup->property("visible").toBool(), u"choosing closes the menu"_s);
        Settings saved;
        saved.load();
        check(saved.uiFont == family && editor->uiFont() == family, u"font preference persisted in the isolated profile"_s);
        check(sample->property("font").value<QFont>().family() == family, u"sample previews the selected font"_s);
        check(QGuiApplication::font().family() == running && editor->fontRestartRequired() == (running != family),
            u"existing windows stay consistent until explicit restart"_s);
        const qreal y = picker->mapToItem(qobject_cast<QQuickItem*>(flick), QPointF()).y();
        flick->setProperty("contentY", std::max(0.0, flick->property("contentY").toDouble() + y - 130));
        settle(window, 2);
        check(saveGrab(window, out.filePath(u"font-picker.png"_s)), u"font preview captured"_s);
        if (popup)
            QMetaObject::invokeMethod(popup, "open");
        settle(window, 2);
        if (search)
            search->setProperty("text", u"Missing-WinShun-Test-Font"_s);
        settle(window, 2);
        QMetaObject::invokeMethod(picker, "acceptQuery");
        check(editor->uiFont() == family, u"unknown typed font does not replace the preference"_s);
        if (popup)
            QMetaObject::invokeMethod(popup, "close");
        settle(window, 2);
        QMetaObject::invokeMethod(picker, "activated", Q_ARG(int, 0));
        settle(window, 2);
        saved.load();
        check(editor->uiFont().isEmpty() && saved.uiFont.isEmpty(), u"default font can be restored"_s);
        check(sample->property("font").value<QFont>().family() == uifont::resolvedFamily({}), u"default preview restored"_s);
        {
            // Exercise the real helper handshake with a hidden owned window.
            // No clipboard watcher, show(), global keys or service lease.
            QTemporaryDir clipProfile;
            ClipStore store(clipProfile.filePath(u"clipboard"_s));
            store.open(QDateTime::currentMSecsSinceEpoch());
            ClipboardWatcher watcher({}, {});
            Clipboard clipboard(&store, &watcher);
            Placement placement(clipProfile.filePath(u"placement.ini"_s), u"Clipboard"_s);
            ClipboardDisplay display(&clipboard, window, &placement, true);
            display.start();
            QElapsedTimer deadline;
            deadline.start();
            while (!display.ready() && deadline.elapsed() < 6000)
                pump(25);
            check(display.ready(), u"hidden clipboard display completes the authenticated font handshake"_s);
            pump(50);
            const QString output = display.probeOutput();
            say(output);
            check(output.contains(u"clipboard-display-font: "_s + running),
                u"independent clipboard search field inherits the running font"_s);
            check(!display.handle() || !::IsWindowVisible(display.handle()), u"clipboard font test never shows its window"_s);
        }
        return finish(window, ok);
    }

    // GitHub #397: 收藏夹's column headers clicked as its user does: by name,
    // the other way, by date (the disk asked off the GUI thread), by size,
    // the third click back to the order of before, 恢复默认 too; each
    // pictured (--favorites-sort, with --dark / --scale as ever).
    if (args.contains(u"--favorites-sort"_s)) {
        bool ok = true;
        bool saved = true;
        const auto check = [&](bool okay, const QString& what) {
            if (!okay)
                qWarning("favorites sort: %s", qUtf8Printable(what));
            ok = okay && ok;
            return okay;
        };
        // Files and a folder of known dates and sizes, beside the demo ones.
        const QDir demo(favoritesDir.path());
        const QDateTime now = QDateTime::currentDateTime();
        struct Made {
            QString name;
            qint64 bytes;
            int daysAgo;
        };
        for (const Made& m : {Made {u"季度报告 10.docx"_s, 48 << 10, 2}, Made {u"季度报告 2.docx"_s, 1200 << 10, 40},
                 Made {u"Photos.zip"_s, 3 << 20, 9}, Made {u"notes.txt"_s, 900, 1}}) {
            QFile file(demo.filePath(m.name));
            if (!file.open(QIODevice::WriteOnly) || file.write(QByteArray(m.bytes, 'x')) != m.bytes)
                return finish(window, check(false, u"demo file not written"_s));
            file.setFileTime(now.addDays(-m.daysAgo), QFileDevice::FileModificationTime);
            file.close();
            favorites.add({Favorite::Kind::File, QDir::toNativeSeparators(file.fileName()), {}, {}});
        }
        demo.mkpath(u"项目资料"_s);
        favorites.add({Favorite::Kind::Folder, QDir::toNativeSeparators(demo.filePath(u"项目资料"_s)), {}, u"xmzl"_s});
        QStringList manual;
        for (const Favorite& f : favorites.items())
            manual.append(f.path);
        const auto names = [&] {
            QStringList out;
            for (const Favorite& f : favorites.items())
                out.append(f.name);
            return out.join(u" | "_s);
        };
        const auto inOrder = [&](const QStringList& expected) {
            QStringList paths;
            for (const Favorite& f : favorites.items())
                paths.append(f.path);
            return paths == expected;
        };

        QMetaObject::invokeMethod(window, "showFavoritesPage", Q_ARG(QVariant, QVariant(QString())));
        window->resize(920, 1080);
        settle(window, 3);
        pump(400); // the disks asked (lookForMissing)
        QQuickItem* section = visualNamed(window->contentItem(), u"favoritesSection"_s);
        const auto header = [&](const QString& key) { return section ? visualNamed(section, u"sortColumn-"_s + key) : nullptr; };
        QQuickItem* restore = section ? visualNamed(section, u"restoreOrderButton"_s) : nullptr;
        if (!check(section && header(u"name"_s) && header(u"modified"_s) && header(u"type"_s) && header(u"size"_s) && restore,
                u"the headers or 恢复默认 missing"_s))
            return finish(window, false);
        // Down to the headers, a little room above them.
        const qreal top = header(u"name"_s)->mapToItem(flick->property("contentItem").value<QQuickItem*>(), QPointF()).y();
        flick->setProperty("contentY", std::max(0.0, top - 140));
        settle(window, 3);
        check(!restore->isVisible(), u"恢复默认 shown with nothing to go back to"_s);
        saved = saveGrab(window, out.filePath(u"favorites-sort-0-manual.png"_s)) && saved;

        const auto click = [&](QQuickItem* item) {
            const QPointF at = item->mapToScene({item->width() / 2, item->height() / 2});
            QMouseEvent press(QEvent::MouseButtonPress, at, window->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, {});
            QCoreApplication::sendEvent(window, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), Qt::LeftButton, {}, {});
            QCoreApplication::sendEvent(window, &release);
        };
        // Clicked, then waited for (by date or size: once the disk has said).
        const auto sortedBy = [&](const QString& key, FavoriteSort sort, bool descending, const QString& picture) {
            click(header(key));
            QElapsedTimer waited;
            waited.start();
            while ((favorites.sort() != sort || favorites.sortDescending() != descending) && waited.elapsed() < 5000)
                pump(50);
            settle(window, 3);
            const bool there = check(favorites.sort() == sort && favorites.sortDescending() == descending,
                u"%1 clicked: not sorted so (%2)"_s.arg(key, names()));
            check(header(key)->property("active").toBool(), key + u" header shows no caret"_s);
            check(restore->isVisible(), u"恢复默认 hidden while sorted"_s);
            saved = saveGrab(window, out.filePath(picture)) && saved;
            say(u"%1%2: %3"_s.arg(key, descending ? u" ↓"_s : u" ↑"_s, names()));
            return there;
        };
        sortedBy(u"name"_s, FavoriteSort::Name, false, u"favorites-sort-1-name.png"_s);
        check(favorites.items().constFirst().name == u"notes.txt"_s, u"by name: notes.txt not first (%1)"_s.arg(names()));
        sortedBy(u"name"_s, FavoriteSort::Name, true, u"favorites-sort-2-name-down.png"_s);
        sortedBy(u"modified"_s, FavoriteSort::Modified, true, u"favorites-sort-3-modified.png"_s);
        // The newest first: notes.txt (a day) or a folder made just now; the command and the gone one last.
        check(favorites.items().constLast().kind == Favorite::Kind::Command || !QFileInfo::exists(favorites.items().constLast().path),
            u"by date: one with no date not last (%1)"_s.arg(names()));
        sortedBy(u"type"_s, FavoriteSort::Type, false, u"favorites-sort-4-type.png"_s);
        check(favorites.items().constFirst().kind == Favorite::Kind::Folder && favorites.items().constLast().kind == Favorite::Kind::Command,
            u"by type: not folders first and commands last (%1)"_s.arg(names()));
        sortedBy(u"size"_s, FavoriteSort::Size, true, u"favorites-sort-5-size.png"_s);
        check(QFileInfo(favorites.items().constFirst().path).fileName() == u"WinShun.exe"_s
                || QFileInfo(favorites.items().constFirst().path).fileName() == u"Photos.zip"_s,
            u"by size: not the largest first (%1)"_s.arg(names()));
        sortedBy(u"size"_s, FavoriteSort::Size, false, u"favorites-sort-6-size-up.png"_s);
        click(header(u"size"_s)); // the third time: as before
        pump(100);
        settle(window, 3);
        check(favorites.sort() == FavoriteSort::None && !favorites.hasManualOrder() && inOrder(manual),
            u"the third click did not bring back the order of before (%1)"_s.arg(names()));
        check(!restore->isVisible(), u"恢复默认 still shown"_s);
        saved = saveGrab(window, out.filePath(u"favorites-sort-7-back.png"_s)) && saved;
        // 恢复默认 after a sort and a move by hand.
        sortedBy(u"type"_s, FavoriteSort::Type, false, u"favorites-sort-8-type.png"_s);
        favorites.move(0, 2);
        settle(window, 3);
        check(!header(u"type"_s)->property("active").toBool() && restore->isVisible(), u"a move by hand: caret kept or 恢复默认 gone"_s);
        click(restore);
        pump(100);
        settle(window, 3);
        check(favorites.sort() == FavoriteSort::None && !favorites.hasManualOrder() && inOrder(manual),
            u"恢复默认 did not bring back the order of before (%1)"_s.arg(names()));
        saved = saveGrab(window, out.filePath(u"favorites-sort-9-restored.png"_s)) && saved;
        say(u"favorites sort: %1"_s.arg(ok && saved ? u"all checks passed"_s : u"FAILED"_s));
        return finish(window, ok && saved);
    }

    // Every page, every tab: the window as tall as the page, so the
    // picture has all of it.
    bool saved = true;
    bool onePage = false;
    const int wantedPage = valueAfter(args, u"--page").toInt(&onePage);
    if (args.contains(u"--entry-hotkey-settings"_s)) {
        // 设置快捷键… from an entry's menu (App::showEntryHotkeySettings): its
        // row in 单条内容的快捷键 shown, recording; the keys pressed go to the app.
        window->resize(QSize(920, 720));
        settle(window, 3);
        bool okay = true;
        const auto check = [&okay](bool condition, const char* reason) {
            okay = condition && okay;
            if (!condition) qWarning("entry hotkey settings: %s", reason);
        };
        std::pair<qint64, QString> asked;
        QObject::connect(editor, &SettingsEditor::clipboardEntryHotkeyRequested, editor,
            [&asked](qint64 id, const QString& keys) { asked = {id, keys}; });
        QMetaObject::invokeMethod(window, "showClipboardEntryHotkey", Q_ARG(QVariant, QVariant(qint64(47))));
        settle(window, 3);
        std::vector<QQuickItem*> recorders;
        const std::function<void(QQuickItem*)> collect = [&](QQuickItem* item) {
            if (item->objectName() == u"entryHotkeyRecorder")
                recorders.push_back(item);
            for (QQuickItem* child : item->childItems())
                collect(child);
        };
        collect(window->contentItem());
        const auto focused = std::ranges::find_if(recorders, [](QQuickItem* r) { return r->hasActiveFocus(); });
        check(recorders.size() == 3, "the three entries' recorders are not there");
        check(focused != recorders.end() && (*focused)->property("hotkey").toString().isEmpty(),
            "the chosen entry's recorder is not the one recording");
        if (focused != recorders.end()) {
            const QRectF shown = (*focused)->mapRectToScene(QRectF(QPointF(), (*focused)->size()));
            check(shown.top() >= 0 && shown.bottom() <= window->height(), "the chosen entry's row is not in view");
            check(saveGrab(window, out.filePath(u"entry-hotkey-settings.png"_s)), "the settings did not render");
            QKeyEvent press(QEvent::KeyPress, Qt::Key_K, Qt::ControlModifier | Qt::AltModifier);
            QCoreApplication::sendEvent(*focused, &press);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_K, Qt::ControlModifier | Qt::AltModifier);
            QCoreApplication::sendEvent(*focused, &release);
            settle(window);
            check(asked == std::pair<qint64, QString>(47, u"Ctrl+Alt+K"_s), "the keys recorded did not go to the app for that entry");
            check(!(*focused)->hasActiveFocus(), "the recorder went on recording");
        }
        say(u"entry hotkey settings: 设置快捷键… shows the entry's row recording; Ctrl+Alt+K goes to the app for it"_s);
        return finish(window, okay, editor);
    }
    if (args.contains(u"--preview-hotkeys"_s)) {
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(u"results"_s)), Q_ARG(QVariant, QVariant(1))); // 结果列表 › 显示与预览
        window->resize(args.contains(u"--compact"_s) ? QSize(760, 420) : QSize(920, 720));
        settle(window, 3);
        auto* recorder = visualNamed(window->contentItem(), u"previewHotkeyRecorder"_s);
        if (!recorder) {
            qWarning("preview shortcut recorder missing from production settings");
            return finish(window, false);
        }
        const auto check = [](bool okay, const char* reason) {
            if (!okay) qWarning("preview shortcut settings: %s", reason);
        };
        const auto key = [&](int code, Qt::KeyboardModifiers modifiers = {}, const QString& text = {}) {
            recorder->forceActiveFocus();
            QKeyEvent event(QEvent::KeyPress, code, modifiers, text);
            QCoreApplication::sendEvent(recorder, &event);
        };
        for (const QString& name : {u"shortcutHelpToggle"_s, u"footerHintsToggle"_s}) {
            QObject* toggle = window->findChild<QObject*>(name);
            check(toggle != nullptr, "keyboard hint toggle is missing");
            if (!toggle)
                continue;
            const bool help = name == u"shortcutHelpToggle";
            for (const bool on : {false, true}) {
                QMetaObject::invokeMethod(toggle, "toggled", Q_ARG(bool, on));
                settle(window);
                Settings restored;
                restored.load();
                check((help ? editor->shortcutHelp() : editor->footerHints()) == on
                    && (help ? restored.shortcutHelp : restored.footerHints) == on
                    && toggle->property("checked").toBool() == on,
                    "keyboard hint toggle did not apply and persist");
            }
        }
        key(Qt::Key_Space, {}, u" "_s);
        check(editor->previewHotkey() == u"Space", "recording Space did not apply");
        Settings restored;
        restored.load();
        check(restored.previewHotkey == u"Space", "recorded key was not persisted");
        key(Qt::Key_Escape);
        check(editor->previewHotkey() == u"Space", "Escape changed the saved shortcut");
        key(Qt::Key_C, Qt::ControlModifier);
        check(editor->previewHotkey() == u"Space" && !recorder->property("partial").toString().isEmpty(),
            "a reserved editing shortcut was accepted or had no feedback");
        key(Qt::Key_Backspace);
        check(editor->previewHotkey().isEmpty(), "Backspace did not clear the shortcut");
        const QString previous = editor->hotkey();
        editor->setHotkey(u"Ctrl+P"_s);
        editor->setPreviewHotkey(u"Ctrl+P"_s);
        check(editor->previewHotkey().isEmpty() && !editor->previewHotkeyError().isEmpty(), "global shortcut conflict was accepted");
        editor->setHotkey(previous);
        editor->setPreviewHotkey(u"Space"_s);
        settle(window, 3);
        const QPointF at = recorder->mapToItem(qobject_cast<QQuickItem*>(flick), QPointF());
        flick->setProperty("contentY", std::max(0.0, at.y() - 90));
        settle(window, 3);
        bool result = saveGrab(window, out.filePath(u"preview-hotkey-settings.png"_s));
        editor->setPreviewHotkey(u"Alt+P"_s);
        check(editor->previewHotkey() == u"Alt+P", "restoring the default did not apply");
        say(u"preview shortcut settings: production recorder + SettingsEditor + persistence; record, cancel, reserved key, clear, global conflict, restore"_s);
        return finish(window, result, editor);
    }
    for (int page = 0; page < pages.size(); ++page) {
        if (args.contains(u"--command-form"_s))
            break;
        if (onePage && page != wantedPage)
            continue;
        const QVariantList pageTabs = tabs.value(page).toList();
        const int count = std::max<int>(1, static_cast<int>(pageTabs.size()));
        for (int tab = 0; tab < count; ++tab) {
            const QString title = pages[page].toMap().value(u"title"_s).toString() + (pageTabs.isEmpty() ? QString() : u" / "_s + pageTabs[tab].toString());
            QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(page)), Q_ARG(QVariant, QVariant(tab)));
            if (page == pluginsPage && tab == 0 && args.contains(u"--open"_s)) { // one plugin opened (翻译: settings, permissions), 粘贴为文件's 设置 unfolded
                if (QObject* section = partOf(window, pluginsPage, "pluginsSection"))
                    QMetaObject::invokeMethod(section, "expandAt", Q_ARG(QVariant, QVariant(1)));
                if (auto* paste = window->findChild<QQuickItem*>(u"pasteFileSettingsCard"_s))
                    paste->setProperty("expanded", true);
                else
                    qWarning("preview: no 粘贴为文件 card on the 插件 page");
            }
            // --favorites-by name|modified|type|size: 收藏夹 pictured sorted, its caret on (GitHub #397);
            // back to the order of before once grabbed, for the checks after the pages.
            const QString favoritesBy = page == favoritesPage ? valueAfter(args, u"--favorites-by") : QString();
            if (!favoritesBy.isEmpty()) {
                editor->favorites()->sortBy(favoritesBy);
                QElapsedTimer waited;
                waited.start();
                while (favorites.sort() == FavoriteSort::None && waited.elapsed() < 5000)
                    pump(50);
                if (favorites.sort() == FavoriteSort::None)
                    qWarning("preview: --favorites-by %s did not sort the favorites", qUtf8Printable(favoritesBy));
            }
            window->setHeight(baseHeight);
            settle(window, 3);
            int height = baseHeight;
            if (page != gleaningPage) { // the warm page is one screen of its own
                const int needed = static_cast<int>(std::ceil(flick->property("contentHeight").toReal() + flick->property("y").toReal()));
                height = std::max(baseHeight, needed);
                window->setHeight(height);
                settle(window, 3);
            } else {
                pump(3600); // the avatar's greeting (AuthorClock) has played out
            }
            pump(400); // the sliding marker, the asynchronous icons
            const QString file = out.filePath(u"p%1-t%2.png"_s.arg(page, 2, 10, u'0').arg(tab));
            say(u"page %1 tab %2 (%3): window %4x%5 asked %6, current %7/%8, content %9"_s.arg(page).arg(tab).arg(title)
                    .arg(window->width()).arg(window->height()).arg(height).arg(window->property("currentPage").toInt())
                    .arg(window->property("currentTab").toInt()).arg(flick->property("contentHeight").toReal()));
            if (window->property("currentPage").toInt() != page || window->property("currentTab").toInt() != tab)
                qWarning("preview: showPage(%d, %d) did not land there", page, tab);
            if (window->height() != height)
                qWarning("preview: the window is %d high, not %d", window->height(), height);
            if (tree)
                writeTree(window, file + u".tree.txt"_s);
            saved = saveGrab(window, file) && saved;
            if (!favoritesBy.isEmpty())
                favorites.restoreOrder();
            // --hover x,y (logical): the pointer resting there, as a second picture.
            if (const QStringList at = valueAfter(args, u"--hover").split(u','); at.size() == 2) {
                const QPointF point(at[0].toDouble(), at[1].toDouble());
                QMouseEvent move(QEvent::MouseMove, point, window->mapToGlobal(point), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(window, &move);
                pump(400); // past the hover fade
                saved = saveGrab(window, out.filePath(u"p%1-t%2-hover.png"_s.arg(page, 2, 10, u'0').arg(tab))) && saved;
                // --hover-frames N: N more pictures while it rests, 30 ms apart besides the grabbing,
                // to see a hover's motion frame by frame (the 捐赠 button's).
                for (int frame = 0, frames = valueAfter(args, u"--hover-frames").toInt(); frame < frames; ++frame) {
                    pump(30);
                    saved = saveGrab(window, out.filePath(u"p%1-t%2-hover-f%3.png"_s.arg(page, 2, 10, u'0').arg(tab).arg(frame, 2, 10, u'0'))) && saved;
                }
                QMouseEvent away(QEvent::MouseMove, QPointF(-50, -50), window->mapToGlobal(QPointF(-50, -50)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(window, &away);
                pump(400);
            }
            if (page == window->property("integrationPage").toInt() && tab == 1) { // 系统整合 › 文件管理器
                QObject* toggle = visualNamed(window->contentItem(), u"opusTypingToggle"_s);
                const bool explorerBefore = editor->explorerTyping();
                if (!toggle) {
                    qWarning("preview: Opus typing toggle missing");
                } else {
                    for (const bool enabled : {true, false}) {
                        QMetaObject::invokeMethod(toggle, "toggled", Q_ARG(bool, enabled));
                        settle(window, 2);
                        Settings read;
                        read.load();
                        if (editor->opusTyping() != enabled || read.opusTyping != enabled
                            || toggle->property("checked").toBool() != enabled || read.explorerTyping != explorerBefore)
                            qWarning("preview: Opus typing did not save independently or update its binding");
                    }
                    say(u"Opus typing setting: actual QML toggle, editor and isolated settings checked"_s);
                }
            }
            if (page == window->property("resultsPage").toInt() && tab == 1) { // 结果列表 › 显示与预览
                QObject* choice = visualNamed(window->contentItem(), u"favoriteFolderTabChoice"_s);
                if (!choice) {
                    qWarning("preview: favorite folder Tab choice missing");
                } else {
                    for (const bool enabled : {true, false}) {
                        if (!QMetaObject::invokeMethod(choice, "activated", Q_ARG(int, enabled ? 1 : 0)))
                            qWarning("preview: favorite folder Tab choice could not be activated");
                        settle(window, 2);
                        Settings read;
                        read.load();
                        if (editor->tabEntersFavoriteFolder() != enabled || read.tabEntersFavoriteFolder != enabled
                            || choice->property("current").toInt() != int(enabled))
                            qWarning("preview: favorite folder Tab choice did not save or update its binding");
                    }
                    say(u"favorite folder Tab setting: real QML control, editor and isolated settings round trip checked"_s);
                }
                QObject* sizeToggle = visualNamed(window->contentItem(), u"showFileSizeToggle"_s);
                if (!sizeToggle) {
                    qWarning("preview: file size toggle missing");
                } else {
                    for (const bool enabled : {true, false}) {
                        if (!QMetaObject::invokeMethod(sizeToggle, "toggled", Q_ARG(bool, enabled)))
                            qWarning("preview: file size toggle could not be activated");
                        settle(window, 2);
                        Settings read;
                        read.load();
                        if (editor->showFileSize() != enabled || read.showFileSize != enabled
                            || sizeToggle->property("checked").toBool() != enabled)
                            qWarning("preview: file size toggle did not save or update its binding");
                    }
                    say(u"file size setting: real QML control, editor and isolated settings round trip checked"_s);
                }
            }
        }
    }

    // A favorite's form open, as the launcher's 编辑收藏 opens it.
    if (!onePage) {
        QMetaObject::invokeMethod(window, "showFavoritesPage", Q_ARG(QVariant, QVariant(favorites.items().constFirst().path)));
        window->setHeight(baseHeight);
        settle(window, 3);
        pump(300);
        if (window->property("currentPage").toInt() != favoritesPage)
            qWarning("preview: showFavoritesPage() did not land on the 收藏夹 page");
        saved = saveGrab(window, out.filePath(u"favorite-form.png"_s)) && saved;
        QMetaObject::invokeMethod(window, "showFavoritesPage", Q_ARG(QVariant, QVariant(favorites.items().constLast().path)));
        settle(window, 3);
        pump(300);
        saved = saveGrab(window, out.filePath(u"command-form.png"_s)) && saved;
        flick->setProperty("contentY", std::max(0.0, flick->property("contentHeight").toDouble()
            - flick->property("height").toDouble()));
        settle(window, 3);
        saved = saveGrab(window, out.filePath(u"command-form-bottom.png"_s)) && saved;
        if (args.contains(u"--command-form"_s)) {
            if (QObject* section = partOf(window, favoritesPage, "favoritesSection")) {
                section->setProperty("editing", -1);
                section->setProperty("addingCommand", true);
                flick->setProperty("contentY", 0);
                settle(window, 3);
                saved = saveGrab(window, out.filePath(u"command-new.png"_s)) && saved;
                QObject* form = visualNamed(qobject_cast<QQuickItem*>(section), u"favoriteForm"_s);
                QObject* name = form ? form->findChild<QObject*>(u"favoriteName"_s) : nullptr;
                QObject* keyword = form ? form->findChild<QObject*>(u"favoriteKeyword"_s) : nullptr;
                QObject* commandBox = form ? form->findChild<QObject*>(u"favoriteCommand"_s) : nullptr;
                if (name && keyword && commandBox) {
                    name->setProperty("text", u"Preview command"_s);
                    keyword->setProperty("text", u"preview-command"_s);
                    commandBox->setProperty("text", u"notepad.exe {query}"_s);
                    saved = previewCommandIcons(window, editor, favorites, section, out) && saved;
                    section->setProperty("addingCommand", true);
                    settle(window, 3);
                    form = visualNamed(qobject_cast<QQuickItem*>(section), u"favoriteForm"_s);
                    name = form ? form->findChild<QObject*>(u"favoriteName"_s) : nullptr;
                    keyword = form ? form->findChild<QObject*>(u"favoriteKeyword"_s) : nullptr;
                    commandBox = form ? form->findChild<QObject*>(u"favoriteCommand"_s) : nullptr;
                    if (!name || !keyword || !commandBox)
                        return finish(window, false);
                    name->setProperty("text", u"Preview command"_s);
                    keyword->setProperty("text", u"preview-command"_s);
                    commandBox->setProperty("text", u"notepad.exe {query}"_s);
                    QMetaObject::invokeMethod(form, "save");
                    if (favorites.items().size() != 5 || section->property("addingCommand").toBool())
                        qWarning("preview: form did not save and close");
                    else
                        favorites.remove(favorites.items().constLast().path);
                } else {
                    qWarning("preview: command form fields missing");
                }
                section->setProperty("addingCommand", false);
                if (favorites.items().size() != 4)
                    qWarning("preview: cancelling the form added a command");
            } else {
                qWarning("preview: favorites section missing");
            }
            return finish(window, saved);
        }
    }

    // 命令行 › 启用命令行 with the connection switch off: it turns the switch on, which shows rows
    // above the section and the page's tabs (--hold-demo). The section has to stay where it was
    // on screen (SettingsWindow.holdInView): before and after pictured, the two positions compared.
    if (args.contains(u"--hold-demo"_s)) {
        editor->setMcp(false);
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(aiPage)), Q_ARG(QVariant, QVariant(0)));
        window->setHeight(480);
        settle(window, 3);
        auto* flickItem = qobject_cast<QQuickItem*>(flick);
        QQuickItem* section = visualNamed(window->contentItem(), u"cliSection"_s);
        QQuickItem* button = visualNamed(window->contentItem(), u"enableCliButton"_s);
        if (!flickItem || !section || !button) {
            qWarning("preview: --hold-demo: the section or its button is missing");
        } else {
            const auto top = [&] { return section->mapToItem(flickItem, QPointF(0, 0)).y(); };
            const double before = top();
            const double contentBefore = flickItem->property("contentHeight").toDouble();
            saved = saveGrab(window, out.filePath(u"hold-before.png"_s)) && saved;
            QMetaObject::invokeMethod(button, "clicked"); // the button's own handler, holdPlace(cli.enable)
            settle(window, 3);
            pump(300);
            const double after = top();
            saved = saveGrab(window, out.filePath(u"hold-after.png"_s)) && saved;
            say(u"hold-demo: the section stood at y %1, now %2 (the page grew from %3 to %4, scrolled to %5), switch %6"_s.arg(before)
                    .arg(after).arg(contentBefore).arg(flickItem->property("contentHeight").toDouble())
                    .arg(flickItem->property("contentY").toDouble()).arg(editor->mcp()));
            if (!editor->mcp())
                qWarning("preview: --hold-demo: the connection switch did not go on");
            if (std::abs(after - before) > 1.5)
                qWarning("preview: --hold-demo: the section moved from y %.1f to %.1f", before, after);
            if (flickItem->property("contentHeight").toDouble() <= contentBefore)
                qWarning("preview: --hold-demo: the page did not grow, nothing was tested");
        }
    }

    // The settings search: a word typed, the page it narrows to pictured,
    // then Enter (goToCurrent), which must land on the right page and tab.
    struct Probe {
        QString word;
        int page;
        int tab;
    };
    const QString quickPasteProbe = args.contains(u"--english"_s)
        ? QCoreApplication::translate("SettingsWindow", "不打开面板直接粘贴") : u"无需面板"_s;
    const int scopePage = window->property("scopePage").toInt();
    const int resultsPage = window->property("resultsPage").toInt();
    const int integrationPage = window->property("integrationPage").toInt();
    const Probe probes[] = {
        {quickPasteProbe, window->property("clipboardPage").toInt(), 1},
        {QCoreApplication::translate("SettingsWindow", "插件"), pluginsPage, 0},
        {QCoreApplication::translate("SettingsWindow", "插件库"), pluginsPage, 1},
        {QCoreApplication::translate("PluginDevSection", "消息记录"), pluginsPage, 2}, // the 开发者 tab
        {PluginSettings::tr("API 密钥"), pluginsPage, 0}, // a plugin's own setting
        {QCoreApplication::translate("PasteFileCard", "粘贴为文件"), pluginsPage, 0}, // a built-in card
        // Shown as 内置功能 cards too, the rows of their own page first (open_settings searches them by title).
        {QCoreApplication::translate("SettingsWindow", "在资源管理器或桌面弹出文件夹快捷菜单"), integrationPage, 2}, // 文件夹菜单
        {QCoreApplication::translate("SettingsWindow", "菜单里列出的内容"), integrationPage, 2},
        {QCoreApplication::translate("SettingsWindow", "收藏夹里显示的类型"), integrationPage, 2},
        {QCoreApplication::translate("SettingsWindow", "收藏夹显示方式"), integrationPage, 2},
        {QCoreApplication::translate("SettingsWindow", "文件夹的子列也列出文件"), integrationPage, 2}, // GitHub #384
        {QCoreApplication::translate("SettingsWindow", "对话框旁的搜索框"), integrationPage, 3}, // 对话框
        {QCoreApplication::translate("SettingsWindow", "在 Directory Opus 里直接打字搜索"), integrationPage, 1},
        {QCoreApplication::translate("SettingsWindow", "结果行按钮"), resultsPage, 1}, // its keys, with their icons
        {u"AI"_s, aiPage, 0},
        {u"命令行"_s, aiPage, 0},
        {u"ws"_s, aiPage, 0},
        {u"command line"_s, aiPage, 0},
        // A setting titled with the word comes first: 在终端中打开 (GitHub #386), above the AI page's keyword.
        {u"终端"_s, resultsPage, 2},
        // "Favorites" also matches the opening-menu option; use this page's exact setting title.
        {QCoreApplication::translate("FavoritesSection", "拖文件时按 ` 键，弹出收藏圆盘"), favoritesPage, 0},
        {QCoreApplication::translate("SettingsWindow", "网络与 WSL 文件夹"), scopePage, 3},
        {QCoreApplication::translate("SettingsWindow", "排序"), resultsPage, 0},
        {QCoreApplication::translate("SettingsWindow", "显示修改日期"), resultsPage, 1},
        {QCoreApplication::translate("SettingsWindow", "显示文件大小"), resultsPage, 1},
        {QCoreApplication::translate("SettingsWindow", "选中收藏文件夹时按 Tab"), resultsPage, 1},
        {QCoreApplication::translate("SettingsWindow", "每次打开时清空搜索"), resultsPage, 2},
        // "File types" also matches preferred types in English; its visible prefix is unambiguous.
        {args.contains(u"--english"_s) ? u":folder"_s : u"文件类型"_s, resultsPage, 3},
    };
    QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(u"general"_s)), Q_ARG(QVariant, QVariant(0)));
    window->setHeight(baseHeight);
    settle(window, 2);
    for (const Probe& probe : probes) {
        if (onePage && probe.page != wantedPage)
            continue;
        input->setProperty("text", probe.word);
        settle(window, 3);
        pump(300);
        QObject* current = settingsSearch->property("current").value<QObject*>();
        const int total = settingsSearch->property("total").toInt();
        const QString rowTitle = current ? current->property("title").toString() : u"(none)"_s;
        QObject* section = current ? current->property("section").value<QObject*>() : nullptr;
        const int sectionPage = section ? section->property("page").toInt() : -1;
        const int sectionTab = section ? section->property("tab").toInt() : -1;
        say(u"search '%1': %2 row(s), best '%3' on page %4 tab %5, searching %6"_s.arg(probe.word).arg(total).arg(rowTitle)
                .arg(sectionPage).arg(sectionTab).arg(window->property("searching").toBool()));
        saved = saveGrab(window, out.filePath(u"search-%1.png"_s.arg(probe.word))) && saved;
        QMetaObject::invokeMethod(window, "goToCurrent");
        pump(100);
        const int landedPage = window->property("currentPage").toInt();
        const int landedTab = window->property("currentTab").toInt();
        say(u"search '%1': Enter went to page %2 tab %3 (%4), searching %5"_s.arg(probe.word).arg(landedPage).arg(landedTab)
                .arg(pages.value(landedPage).toMap().value(u"title"_s).toString()).arg(window->property("searching").toBool()));
        if (total == 0 || !current)
            qWarning("preview: the search for '%s' found nothing", qUtf8Printable(probe.word));
        else if (landedPage != probe.page || landedTab != probe.tab)
            qWarning("preview: '%s' went to page %d tab %d, not %d/%d", qUtf8Printable(probe.word), landedPage, landedTab, probe.page, probe.tab);
        if (window->property("searching").toBool())
            qWarning("preview: still searching after Enter on '%s'", qUtf8Printable(probe.word));
        input->setProperty("text", QString());
        settle(window, 2);
    }

    // Cross-page SettingsLinkRow jumps and 快捷键 › 按键一览: every jump lands on
    // the expected page and tab, scrolls its target into view, and restores cleanly.
    if (!onePage) {
        const int generalPage = window->property("generalPage").toInt();
        const int keysPage = window->property("keysPage").toInt();
        const int appearancePage = window->property("appearancePage").toInt();
        const int clipboardPage = window->property("clipboardPage").toInt();
        struct LinkCheck {
            int fromPage;
            int fromTab;
            const char* objectName;
            const char* method;
            int expectedPage;
            int expectedTab;
        };
        const LinkCheck links[] = {
            {favoritesPage, 0, "favoritesFolderMenuLink", "trigger", integrationPage, 2},
            {favoritesPage, 0, "favoritesLauncherLink", "trigger", resultsPage, 1},
            {integrationPage, 2, "folderMenuManageFavoritesButton", "clicked", favoritesPage, 0},
            {keysPage, 0, "keysClipboardLink", "trigger", clipboardPage, 1},
            {keysPage, 0, "keysFolderMenuLink", "trigger", integrationPage, 2},
            {keysPage, 0, "keysWinSLink", "trigger", integrationPage, 0},
            {appearancePage, 0, "appearanceClipboardSizeLink", "trigger", clipboardPage, 0},
            {scopePage, 2, "scopeIndexFolderLink", "trigger", generalPage, 0},
            {clipboardPage, 1, "clipboardLetterJumpLink", "trigger", resultsPage, 2},
        };
        for (const LinkCheck& link : links) {
            QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(link.fromPage)), Q_ARG(QVariant, QVariant(link.fromTab)));
            settle(window, 2);
            QQuickItem* control = visualNamed(window->contentItem(), QString::fromLatin1(link.objectName));
            if (!control) {
                qWarning("preview: link '%s' missing on page %d tab %d", link.objectName, link.fromPage, link.fromTab);
                continue;
            }
            if (!QMetaObject::invokeMethod(control, link.method))
                qWarning("preview: link '%s' could not invoke %s", link.objectName, link.method);
            settle(window, 3);
            const int landedPage = window->property("currentPage").toInt();
            const int landedTab = window->property("currentTab").toInt();
            if (landedPage != link.expectedPage || landedTab != link.expectedTab)
                qWarning("preview: link '%s' went to page %d tab %d, expected %d/%d",
                    link.objectName, landedPage, landedTab, link.expectedPage, link.expectedTab);
        }
        // Smart contextual target from 收藏夹 when folder menu is turned off: lands on the main switch.
        const bool menuWasOn = editor->folderMenu();
        editor->setFolderMenu(false);
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(favoritesPage)), Q_ARG(QVariant, QVariant(0)));
        settle(window, 2);
        if (QQuickItem* link = visualNamed(window->contentItem(), u"favoritesFolderMenuLink"_s)) {
            if (link->property("targetPart").toString() != u"folderMenuRow"_s)
                qWarning("preview: favoritesFolderMenuLink targetPart when off was '%s'",
                    qUtf8Printable(link->property("targetPart").toString()));
        }
        editor->setFolderMenu(menuWasOn);
        // 按键一览 reflects Win+V, Win+S, and folderMenuHotkey.
        const bool winVBefore = editor->clipboardWinV();
        const bool winSBefore = editor->taskbarWinS();
        const QString fmKeyBefore = editor->folderMenuHotkey();
        editor->setClipboardWinV(true);
        editor->setTaskbarWinS(true);
        editor->setFolderMenuHotkey(u"Ctrl+Alt+D"_s);
        settle(window, 2);
        const QVariantList sections = window->property("shortcutSections").toList();
        bool sawWinS = false, sawWinV = false, sawFolderMenu = false;
        for (const QVariant& secVar : sections) {
            for (const QVariant& rowVar : secVar.toMap().value(u"rows"_s).toList()) {
                const QString keys = rowVar.toMap().value(u"keys"_s).toString();
                if (keys.contains(u"Win+S"_s)) sawWinS = true;
                if (keys.contains(u"Win+V"_s)) sawWinV = true;
                if (keys == u"Ctrl+Alt+D"_s) sawFolderMenu = true;
            }
        }
        if (!sawWinS || !sawWinV || !sawFolderMenu)
            qWarning("preview: shortcutSections missing global keys (Win+S=%d Win+V=%d folderMenu=%d)",
                int(sawWinS), int(sawWinV), int(sawFolderMenu));
        editor->setClipboardWinV(winVBefore);
        editor->setTaskbarWinS(winSBefore);
        editor->setFolderMenuHotkey(fmKeyBefore);
        // 序号提示 stays accessible whenever either number keys or 字母直达 is enabled.
        {
            const QString numKeysBefore = editor->numberKeys();
            const QString clipNumKeysBefore = editor->clipboardNumberKeys();
            const bool jumpBefore = editor->letterJump();
            editor->setNumberKeys(u"off"_s);
            editor->setClipboardNumberKeys(u"off"_s);
            editor->setLetterJump(true);
            QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(resultsPage)), Q_ARG(QVariant, QVariant(2)));
            settle(window, 2);
            QQuickItem* resultHintsRow = visualNamed(window->contentItem(), u"resultNumberHintsRow"_s);
            if (!resultHintsRow || !resultHintsRow->property("shown").toBool())
                qWarning("preview: resultNumberHintsRow hidden when numberKeys=off and letterJump=true");
            QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(clipboardPage)), Q_ARG(QVariant, QVariant(1)));
            settle(window, 2);
            QQuickItem* clipHintsRow = visualNamed(window->contentItem(), u"clipNumberHintsRow"_s);
            if (!clipHintsRow || !clipHintsRow->property("shown").toBool())
                qWarning("preview: clipNumberHintsRow hidden when clipboardNumberKeys=off and letterJump=true");
            editor->setLetterJump(false);
            settle(window, 2);
            if (clipHintsRow && clipHintsRow->property("shown").toBool())
                qWarning("preview: clipNumberHintsRow still shown when both clipboardNumberKeys=off and letterJump=false");
            QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(resultsPage)), Q_ARG(QVariant, QVariant(2)));
            settle(window, 2);
            if (resultHintsRow && resultHintsRow->property("shown").toBool())
                qWarning("preview: resultNumberHintsRow still shown when both numberKeys=off and letterJump=false");
            editor->setNumberKeys(numKeysBefore);
            editor->setClipboardNumberKeys(clipNumKeysBefore);
            editor->setLetterJump(jumpBefore);
            settle(window, 2);
        }
        say(u"cross-page settings links & global shortcuts list checked"_s);
    }
    return finish(window, saved);
}

// GitHub #186: the preview window by the real launcher on the real screen
// (GetWindowRect, physical): beside it while there is room (a launcher in
// the middle of the screen), and with no room, never over the names.
void checkPreviewPlacement(Launcher& launcher, QQuickWindow* window, FilePreview& preview, const QDir* output = nullptr)
{
    const auto check = [](bool okay, const QString& name) {
        if (!okay) qWarning("preview placement regression: %s", qUtf8Printable(name));
    };
    QScreen* screen = window->screen();
    if (!screen || launcher.results()->count() == 0) {
        check(false, u"no screen or no rows"_s);
        return;
    }
    const QRect area = screen->availableGeometry();
    const QRect before = window->geometry();
    const auto physical = [](QWindow* w) {
        RECT r {};
        ::GetWindowRect(reinterpret_cast<HWND>(w->winId()), &r);
        return QRect(QPoint(r.left, r.top), QPoint(r.right - 1, r.bottom - 1));
    };
    const QString path = launcher.results()->pathAt(0);
    const FilePreview::Item item {path, QFileInfo(path).fileName()};
    for (const bool roomy : {true, false}) {
        // Room beside: the default width in the middle, low (the sides go
        // above its top). No room: nearly as wide and tall as the screen.
        const QSize size = roomy ? QSize(760, 480) : area.size() - QSize(160, 200);
        window->setGeometry(area.left() + (area.width() - size.width()) / 2,
            roomy ? area.top() + area.height() * 2 / 5 : area.top() + 40, size.width(), size.height());
        settle(window);
        preview.show(item);
        QElapsedTimer loaded;
        loaded.start();
        while ((preview.kind() == FilePreview::Loading || !preview.window() || !preview.window()->isVisible())
            && loaded.elapsed() < 3000)
            pump(10);
        pump(50);
        QWindow* shown = preview.window();
        check(shown && shown->isVisible(), u"preview window shown"_s);
        if (shown) {
            const QRect launcherRect = physical(window), previewRect = physical(shown);
            say(u"preview placement (%1): launcher %2,%3 %4x%5, preview %6,%7 %8x%9 (physical)"_s
                    .arg(roomy ? u"room beside"_s : u"no room"_s)
                    .arg(launcherRect.x()).arg(launcherRect.y()).arg(launcherRect.width()).arg(launcherRect.height())
                    .arg(previewRect.x()).arg(previewRect.y()).arg(previewRect.width()).arg(previewRect.height()));
            if (roomy) {
                check(!previewRect.intersects(launcherRect), u"room beside: the preview covered the launcher"_s);
            } else {
                const int names = launcherRect.left() + launcherRect.width() * 40 / 100;
                check(previewRect.left() >= names || previewRect.right() <= launcherRect.right() - launcherRect.width() * 40 / 100,
                    u"no room: the preview covered the names"_s);
            }
            MONITORINFO monitor {.cbSize = sizeof(MONITORINFO)};
            ::GetMonitorInfoW(::MonitorFromWindow(reinterpret_cast<HWND>(window->winId()), MONITOR_DEFAULTTONEAREST), &monitor);
            const RECT& work = monitor.rcWork;
            check(QRect(QPoint(work.left, work.top), QPoint(work.right - 1, work.bottom - 1)).contains(previewRect),
                u"the preview stays in the work area"_s);
        }
        preview.close();
        pump(50);
    }
    // Enlarged (the key in its header, a double click on the header): the
    // work area's whole height, the launcher's names still never under it,
    // the same size for the next file; and back as it was.
    {
        const QSize size(760, 480);
        window->setGeometry(area.left() + (area.width() - size.width()) / 2, area.top() + area.height() * 2 / 5, size.width(), size.height());
        settle(window);
        const auto showAndWait = [&](const FilePreview::Item& file) {
            preview.show(file);
            QElapsedTimer loaded;
            loaded.start();
            while ((preview.kind() == FilePreview::Loading || !preview.isShowing(file.path) || !preview.window()
                       || !preview.window()->isVisible())
                && loaded.elapsed() < 3000)
                pump(10);
            pump(100);
        };
        showAndWait(item);
        QQuickWindow* shown = preview.window();
        QQuickItem* key = shown ? visualNamed(shown->contentItem(), u"previewEnlarge"_s) : nullptr;
        check(key && key->isVisible() && !preview.enlarged(), u"enlarged: the key is in the header, off at first"_s);
        if (key) {
            const auto hwnd = reinterpret_cast<HWND>(shown->winId());
            const qreal dpr = shown->devicePixelRatio();
            const auto screenPoint = [&](QPointF scene) {
                RECT bounds {};
                ::GetWindowRect(hwnd, &bounds);
                return MAKELPARAM(bounds.left + qRound(scene.x() * dpr), bounds.top + qRound(scene.y() * dpr));
            };
            const QRect usual = physical(shown);
            const QPointF onKey = key->mapToScene({key->width() / 2, key->height() / 2});
            check(::SendMessageW(hwnd, WM_NCHITTEST, 0, screenPoint(onKey)) == HTCLIENT, u"enlarged: the key is the window's own, not a drag of it"_s);
            systemMouse(shown, QEvent::MouseButtonPress, onKey);
            systemMouse(shown, QEvent::MouseButtonRelease, onKey);
            pump(150);
            const QRect large = physical(shown), launcherRect = physical(window);
            MONITORINFO monitor {.cbSize = sizeof(MONITORINFO)};
            ::GetMonitorInfoW(::MonitorFromWindow(reinterpret_cast<HWND>(window->winId()), MONITOR_DEFAULTTONEAREST), &monitor);
            const QRect work(QPoint(monitor.rcWork.left, monitor.rcWork.top), QPoint(monitor.rcWork.right - 1, monitor.rcWork.bottom - 1));
            const int gap = qCeil(8 * dpr) + 2;
            say(u"preview placement (enlarged): launcher %1,%2 %3x%4, preview %5,%6 %7x%8, before %9x%10 (physical)"_s
                    .arg(launcherRect.x()).arg(launcherRect.y()).arg(launcherRect.width()).arg(launcherRect.height())
                    .arg(large.x()).arg(large.y()).arg(large.width()).arg(large.height()).arg(usual.width()).arg(usual.height()));
            check(preview.enlarged() && large.width() > usual.width() && large.height() > usual.height(),
                u"enlarged: a click on the key did not enlarge the window"_s);
            check(work.contains(large) && large.top() - work.top() <= gap && work.bottom() - large.bottom() <= gap
                    && work.right() - large.right() <= gap,
                u"enlarged: not the work area's height to its right edge"_s);
            check(large.left() >= launcherRect.left() + launcherRect.width() * 40 / 100 - 1, u"enlarged: the preview covered the names"_s);
            const QString next = launcher.results()->pathAt(1);
            showAndWait({next, QFileInfo(next).fileName()});
            check(physical(shown) == large, u"enlarged: the next file changed the window's size"_s);
            if (output)
                check(saveGrab(shown, output->filePath(u"preview-enlarged.png"_s)), u"enlarged: screenshot"_s);
            QQuickItem* header = key->parentItem() ? key->parentItem()->parentItem() : nullptr;
            const QPointF onHeader = header ? header->mapToScene({header->width() / 3, header->height() / 2}) : QPointF();
            check(header && ::SendMessageW(hwnd, WM_NCHITTEST, 0, screenPoint(onHeader)) == HTCAPTION, u"enlarged: the header still drags the window"_s);
            ::SendMessageW(hwnd, WM_NCLBUTTONDBLCLK, HTCAPTION, screenPoint(onHeader));
            pump(150);
            showAndWait(item);
            check(!preview.enlarged() && physical(shown).size() == usual.size(),
                u"enlarged: a double click on the header did not put it back"_s);
        }
        preview.setEnlarged(false);
        preview.close();
        pump(50);
    }
    window->setGeometry(before);
    settle(window);
}

void checkSearchMenuKeys(Launcher& launcher, SystemCatalog& places, QQuickWindow* window, FilePreview& preview, const QDir& output)
{
    QJsonArray checks;
    bool passed = true;
    const auto check = [&](bool okay, const QString& name) {
        checks.append(QJsonObject {{u"name"_s, name}, {u"passed"_s, okay}});
        passed = okay && passed;
        if (!okay) qWarning("search navigation regression: %s", qUtf8Printable(name));
    };
    const auto key = [&](int code, Qt::KeyboardModifiers modifiers = {}, const QString& text = {}, bool repeated = false) {
        QKeyEvent event(QEvent::KeyPress, code, modifiers, text, repeated);
        QCoreApplication::sendEvent(window, &event);
    };
    const auto menuOpen = [&] { return window->property("menuOpen").toBool(); };
    const QString query = launcher.query();
    for (const bool panel : {false, true}) {
        launcher.setPanel(panel);
        window->resize(panel ? QSize(1200, 680) : QSize(760, 600));
        settle(window);
        const QString mode = panel ? u"panel: "_s : u"box: "_s;
        auto* field = visualNamed(window->contentItem(), panel ? u"panelSearchField"_s : u"searchField"_s);
        QObject* view = window->property("view").value<QObject*>();
        check(field && view, mode + u"production search field and list"_s);
        if (!field || !view) continue;
        field->forceActiveFocus();
        field->setProperty("cursorPosition", query.size());
        for (const auto [alias, arrow] : {std::pair{Qt::Key_J, Qt::Key_Down}, {Qt::Key_N, Qt::Key_Down},
                 {Qt::Key_K, Qt::Key_Up}, {Qt::Key_P, Qt::Key_Up}, {Qt::Key_U, Qt::Key_PageUp}, {Qt::Key_D, Qt::Key_PageDown}}) {
            for (const int start : {0, 2, 17}) {
                view->setProperty("currentIndex", start);
                key(arrow);
                const int expected = view->property("currentIndex").toInt();
                view->setProperty("currentIndex", start);
                key(alias, Qt::ControlModifier);
                check(view->property("currentIndex").toInt() == expected && launcher.query() == query && field->hasActiveFocus(),
                    mode + u"Ctrl+%1 matches %2 from row %3, retaining text and focus"_s.arg(int(alias)).arg(int(arrow)).arg(start));
            }
        }
        view->setProperty("currentIndex", 2);
        key(Qt::Key_J, Qt::ControlModifier, {}, true);
        check(view->property("currentIndex").toInt() == 3, mode + u"holding a navigation alias repeats"_s);
        view->setProperty("currentIndex", 2);
        key(Qt::Key_J, Qt::ControlModifier | Qt::AltModifier);
        check(view->property("currentIndex").toInt() == 2, mode + u"AltGr is not navigation"_s);
        QInputMethodEvent aliasComposition(u"shi"_s, {});
        QCoreApplication::sendEvent(field, &aliasComposition);
        key(Qt::Key_J, Qt::ControlModifier);
        check(view->property("currentIndex").toInt() == 2, mode + u"aliases do not move results during IME composition"_s);
        QInputMethodEvent aliasCompositionEnded;
        QCoreApplication::sendEvent(field, &aliasCompositionEnded);
        view->setProperty("currentIndex", 2);
        key(Qt::Key_Right);
        auto* menu = qobject_cast<QQuickWindow*>(window->property("contextMenu").value<QObject*>());
        check(menu && menuOpen(), mode + u"Right opens the selected row's menu"_s);
        if (!menu) continue;
        check(menu->property("row").toInt() == 2 && menu->property("highlighted").toInt() == 0,
            mode + u"menu starts on the first action for the selected file"_s);
        check(field->hasActiveFocus(), mode + u"menu keeps focus in the search field"_s);
        check(saveGrab(menu, output.filePath(panel ? u"search-menu-panel.png"_s : u"search-menu-box.png"_s)),
            mode + u"render the production menu"_s);
        const QVariantList entries = menu->property("entries").toList();
        const int count = static_cast<int>(entries.size());
        check(count > 0, mode + u"production menu entries are available to the probe"_s);
        const auto selectable = [&](int i) {
            const auto entry = entries[i].toMap();
            return !entry.value(u"separator"_s).toBool() && entry.value(u"enabled"_s, true).toBool();
        };
        key(Qt::Key_Up);
        int last = count - 1;
        while (last >= 0 && !selectable(last)) --last;
        check(last >= 0 && menu->property("highlighted").toInt() == last, mode + u"Up wraps to the last available action"_s);
        key(Qt::Key_Down);
        check(menu->property("highlighted").toInt() == 0, mode + u"Down wraps to the first action"_s);
        key(Qt::Key_Right, {}, {}, true);
        key(Qt::Key_O, Qt::ControlModifier, {}, true);
        check(menuOpen() && menu->property("highlighted").toInt() == 0, mode + u"holding a menu shortcut keeps the menu open"_s);
        bool skipped = count > 0;
        for (int i = 1; i < count; ++i) {
            if (!selectable(i)) continue;
            key(Qt::Key_Down);
            skipped = skipped && menu->property("highlighted").toInt() == i;
        }
        check(skipped, mode + u"Down visits available actions and skips separators"_s);
        key(Qt::Key_Left);
        check(!menuOpen() && field->hasActiveFocus() && launcher.query() == query
                && field->property("cursorPosition").toInt() == query.size() && view->property("currentIndex").toInt() == 2,
            mode + u"Left returns to the same result without editing the query"_s);
        key(Qt::Key_Right, {}, {}, true);
        check(!menuOpen(), mode + u"holding Right to edit does not open a menu"_s);
        field->setProperty("cursorPosition", 1);
        key(Qt::Key_Right);
        check(!menuOpen() && field->property("cursorPosition").toInt() == 2, mode + u"Right within the query moves the caret"_s);
        QMetaObject::invokeMethod(field, "select", Q_ARG(int, 0), Q_ARG(int, query.size()));
        key(Qt::Key_Right);
        check(!menuOpen() && field->property("selectedText").toString().isEmpty(), mode + u"Right collapses a text selection"_s);
        for (const auto modifier : {Qt::ShiftModifier, Qt::ControlModifier, Qt::AltModifier, Qt::MetaModifier}) {
            field->setProperty("cursorPosition", query.size());
            key(Qt::Key_Right, modifier);
            check(!menuOpen(), mode + u"modified Right stays out of the menu (%1)"_s.arg(int(modifier)));
        }
        field->setProperty("cursorPosition", query.size());
        QInputMethodEvent composing(u"shi"_s, {});
        QCoreApplication::sendEvent(field, &composing);
        check(field->property("preeditText").toString() == u"shi", mode + u"IME fixture is composing"_s);
        key(Qt::Key_Right);
        check(!menuOpen(), mode + u"Right does not open a menu during IME composition"_s);
        QInputMethodEvent composingForShortcut(u"shi"_s, {});
        QCoreApplication::sendEvent(field, &composingForShortcut);
        check(field->property("preeditText").toString() == u"shi", mode + u"Ctrl+O fixture is composing"_s);
        key(Qt::Key_O, Qt::ControlModifier);
        check(!menuOpen(), mode + u"Ctrl+O does not open a menu during IME composition"_s);
        QInputMethodEvent ended;
        QCoreApplication::sendEvent(field, &ended);
        field->setProperty("cursorPosition", 1);
        key(Qt::Key_O, Qt::ControlModifier);
        check(menuOpen(), mode + u"Ctrl+O opens the menu from any caret position"_s);
        key(Qt::Key_Escape);
        check(!menuOpen() && field->property("cursorPosition").toInt() == 1 && launcher.query() == query,
            mode + u"Escape returns without moving the caret or clearing the query"_s);
        key(Qt::Key_O, Qt::ControlModifier);
        QInputMethodEvent resumedComposition(u"shi"_s, {});
        QCoreApplication::sendEvent(field, &resumedComposition);
        // The letters being composed are searched where they will go (GitHub #157); the field keeps its own text.
        check(!menuOpen() && field->property("text").toString() == query
                && launcher.query() == query.left(1) + u"shi"_s + query.mid(1),
            mode + u"starting IME composition closes the menu, its letters in the search"_s);
        QCoreApplication::sendEvent(field, &ended);
        check(launcher.query() == query, mode + u"a composition given up leaves the query as typed"_s);
        QElapsedTimer rowsBack; // the rows of the query as typed, for the checks below
        rowsBack.start();
        while (launcher.searching() && rowsBack.elapsed() < 2000)
            pump(10);
        for (const auto modifier : {Qt::ControlModifier | Qt::ShiftModifier, Qt::ControlModifier | Qt::AltModifier,
                 Qt::ControlModifier | Qt::MetaModifier}) {
            key(Qt::Key_O, modifier);
            check(!menuOpen(), mode + u"Ctrl+O requires its exact modifiers (%1)"_s.arg(modifier.toInt()));
        }
        field->setProperty("cursorPosition", query.size());
        key(Qt::Key_Right);
        int previewAction = -1;
        for (int i = 0; i < count; ++i) {
            if (entries[i].toMap().value(u"action"_s, -1).toInt() == Launcher::Preview) previewAction = i;
        }
        check(previewAction >= 0, mode + u"the real file has a Preview action"_s);
        if (previewAction >= 0) {
            for (int i = 1; i <= previewAction; ++i)
                if (selectable(i)) key(Qt::Key_Down);
            key(Qt::Key_Return);
            check(!menuOpen() && preview.isOpen(), mode + u"Enter executes the highlighted production action"_s);
            preview.close();
        }
        key(Qt::Key_Right);
        key(Qt::Key_Z, {}, u"z"_s);
        check(!menuOpen() && launcher.query() == query + u'z' && field->hasActiveFocus(), mode + u"typing closes the menu and resumes search"_s);
        launcher.setQuery(query);
        QElapsedTimer timeout;
        timeout.start();
        while (launcher.searching() && timeout.elapsed() < 2000) pump(10);
        check(!launcher.searching() && launcher.results()->count() == 18, mode + u"the real search recovers after editing"_s);
        view->setProperty("currentIndex", -1);
        field->setProperty("cursorPosition", query.size());
        key(Qt::Key_Right);
        key(Qt::Key_O, Qt::ControlModifier);
        check(!menuOpen(), mode + u"no selected result opens no menu"_s);
        if (!panel && launcher.results()->at(0)) {
            const QString folder = QFileInfo(launcher.results()->at(0)->path).absolutePath();
            const QString prefix = QDir::toNativeSeparators(folder) + u"\\ "_s;
            launcher.setTypingFolder(prefix);
            launcher.setQuery(prefix + query);
            timeout.restart();
            while (launcher.searching() && timeout.elapsed() < 2000) pump(10);
            const bool only = launcher.typingFolderOnly();
            key(Qt::Key_D, Qt::ControlModifier);
            check(launcher.typingFolderOnly() == only, mode + u"Ctrl+D pages without changing the folder filter"_s);
            const bool level = launcher.typingFolderLevel();
            key(Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
            const bool onlyThen = launcher.typingFolderOnly();
            const bool levelThen = launcher.typingFolderLevel();
            check(onlyThen != only || levelThen != level, mode + u"Ctrl+Shift+D switches the current folder filter"_s);
            key(Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
            key(Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
            check(launcher.typingFolderOnly() == only && launcher.typingFolderLevel() == level,
                mode + u"Ctrl+Shift+D comes round to the start again (everywhere, folder, this level)"_s);
            launcher.setTypingFolder({});
            launcher.setQuery(query);
            timeout.restart();
            while (launcher.searching() && timeout.elapsed() < 2000) pump(10);
        }
        view->setProperty("currentIndex", 0);
    }
    const auto previousScope = launcher.scope();
    launcher.setScope(Launcher::All);
    launcher.setSearchKinds(false, false);
    places.setOwnPlaces({settingsPlace(u"Win顺 设置"_s, {})}); // as App gives them (GitHub #389)
    launcher.setQuery(u"设置"_s);
    QElapsedTimer settingsWait;
    settingsWait.start();
    while (launcher.searching() && settingsWait.elapsed() < 3000) pump(10);
    const auto* settingsRow = launcher.results()->at(0);
    check(settingsRow && settingsRow->isSettings(), u"settings are searchable with app and Windows place searches disabled"_s);
    check(launcher.results()->data(launcher.results()->index(0), ResultModel::SettingsActionRole).toBool()
            && !launcher.results()->data(launcher.results()->index(0), ResultModel::FolderRole).toString().contains(u"winshun:"),
        u"settings have an action-only row and a user-facing description"_s);
    int settingsOpened = 0;
    const auto settingsConnection = QObject::connect(&launcher, &Launcher::settingsRequested, [&] { ++settingsOpened; });
    auto* settingsField = visualNamed(window->contentItem(), u"panelSearchField"_s);
    if (settingsField) settingsField->forceActiveFocus();
    window->property("view").value<QObject*>()->setProperty("currentIndex", 0);
    key(Qt::Key_Return);
    check(settingsOpened == 1, u"Enter on the settings result dispatches the internal settings action"_s);
    key(Qt::Key_Comma, Qt::ControlModifier);
    key(Qt::Key_Comma, Qt::ControlModifier, {}, true);
    check(settingsOpened == 2, u"Ctrl+, opens settings once, ignoring auto-repeat"_s);
    // The second key for Enter (GitHub #293): through Main.qml, what Enter
    // does on the row; never Space; nothing with none set.
    key(Qt::Key_Q, Qt::AltModifier);
    check(settingsOpened == 2, u"Alt+Q does nothing with no open key set"_s);
    launcher.setOpenHotkey(u"Space"_s);
    check(launcher.openHotkey().isEmpty(), u"Space is refused as the open key"_s);
    launcher.setOpenHotkey(u"Alt+Q"_s);
    key(Qt::Key_Q, Qt::AltModifier | Qt::ShiftModifier);
    check(settingsOpened == 2, u"the open key needs its exact modifiers"_s);
    key(Qt::Key_Q, Qt::AltModifier);
    key(Qt::Key_Q, Qt::AltModifier, {}, true);
    check(settingsOpened == 3, u"the open key opens the row as Enter does, once a press"_s);
    key(Qt::Key_Return);
    check(settingsOpened == 4, u"Enter still opens with an open key set"_s);
    launcher.setOpenHotkey({});
    key(Qt::Key_Q, Qt::AltModifier);
    check(settingsOpened == 4, u"clearing the open key disables it"_s);
    // Its menu (GitHub #389, #390): open them, copy their link, keep them among the favorites; nothing of a file's.
    const QVariantList settingsMenu = launcher.menuItems(0);
    const auto said = [&](const QString& text) {
        return std::ranges::any_of(settingsMenu, [&](const QVariant& item) { return item.toMap().value(u"text"_s).toString() == text; });
    };
    check(!settingsMenu.isEmpty() && settingsMenu.first().toMap().value(u"text"_s).toString() == u"打开设置"_s && said(u"复制链接"_s)
            && !said(u"打开所在位置"_s) && !said(u"复制打开命令"_s),
        u"settings' menu opens them and copies their link, nothing of a file's"_s);
    settle(window, 3);
    check(saveGrab(window, output.filePath(u"search-settings-entry.png"_s)), u"render the settings entry"_s);
    QObject::disconnect(settingsConnection);
    launcher.setSearchKinds(true, true);
    launcher.setScope(previousScope);
    launcher.setQuery(query);
    settingsWait.restart();
    while (launcher.searching() && settingsWait.elapsed() < 3000) pump(10);
    QFile report(output.filePath(u"search-navigation.json"_s));
    check(report.open(QIODevice::WriteOnly), u"write the regression report"_s);
    if (report.isOpen()) report.write(QJsonDocument(QJsonObject {{u"passed"_s, passed}, {u"checks"_s, checks}}).toJson());
    say(u"search navigation: %1 checks, production Main.qml/Launcher/ContextMenu, local Qt key and IME events, %2"_s
        .arg(checks.size()).arg(passed ? u"passed" : u"FAILED"));
    if (!passed)
        throw std::runtime_error("Production search navigation regression failed; see search-navigation.json");
}

// GitHub #361 through the production Main.qml, with local Qt key events, in
// the box and in the panel: Tab on a folder the list put under the keyboard
// switches the scope (#81); on one the arrows chose it steps in, and on into
// the folders there; Ctrl+Backspace comes back the same way, the folder
// chosen again; then to the words and the scope searched. A part of a name
// typed completes; a path typed goes up a level; with the caret elsewhere
// Ctrl+Backspace edits; regex mode never steps in; → still opens the folder's
// menu. Folders of its own on disk, indexed, and one that is not.
QQuickItem* shownNamed(QQuickItem* root, const QString& name); // below: the box's footer, not the hidden panel's

void checkFolderSteps(Launcher& launcher, IndexService& index, QQuickWindow* window, const QDir& output)
{
    QJsonArray checks;
    bool passed = true;
    const auto check = [&](bool okay, const QString& name) {
        checks.append(QJsonObject {{u"name"_s, name}, {u"passed"_s, okay}});
        passed = okay && passed;
        if (!okay) qWarning("folder steps regression: %s", qUtf8Printable(name));
    };
    const auto key = [&](int code, Qt::KeyboardModifiers modifiers = {}, const QString& text = {}) {
        QKeyEvent event(QEvent::KeyPress, code, modifiers, text);
        QCoreApplication::sendEvent(window, &event);
    };
    const auto rows = [&] { // the rows of what the box holds now
        QElapsedTimer timeout;
        timeout.start();
        while (launcher.searching() && timeout.elapsed() < 3000)
            pump(10);
        pump(30);
    };
    const auto type = [&](const QString& text) {
        launcher.setQuery(text);
        rows();
    };
    QTemporaryDir temporary;
    const QString base = QDir::toNativeSeparators(temporary.path());
    QDir(temporary.path()).mkpath(u"合同资料/2026"_s);
    QDir(temporary.path()).mkpath(u"未索引"_s);
    for (const QString& name : {u"合同资料/说明.txt"_s, u"合同资料/2026/清单.txt"_s, u"未索引/备注.txt"_s}) {
        QFile file(temporary.filePath(name));
        check(file.open(QIODevice::WriteOnly) && file.write("x") == 1, u"create "_s + name);
    }
    const QString contracts = base + u"\\合同资料"_s;
    const QString year = contracts + u"\\2026"_s;
    const QString unindexed = base + u"\\未索引"_s;
    {
        const auto files = index.index();
        const auto lock = files->writeLock();
        const auto root = files->addRoot(base.toStdString());
        const auto folder = files->add(root, "合同资料", EntryFlag::Directory);
        files->add(folder, "说明.txt", 0);
        files->add(files->add(folder, "2026", EntryFlag::Directory), "清单.txt", 0);
    }
    const QString folderHint = QCoreApplication::translate("ws::Launcher", "进入文件夹");
    const QString backHint = QCoreApplication::translate("ws::Launcher", "返回上一级");
    const auto tabHint = [&] { return launcher.keyHints().value(u"tab"_s).toString(); };
    const auto rowOf = [&](const QString& path) { return launcher.results()->indexOf(path); };
    const auto previousScope = launcher.scope();
    const int previousType = launcher.fileType();
    const QString previousQuery = launcher.query();
    const QSize previousSize = window->size();
    for (const bool panel : {false, true}) {
        launcher.setPanel(panel);
        window->resize(panel ? QSize(1200, 680) : QSize(760, 600));
        settle(window, 2);
        const QString mode = panel ? u"panel: "_s : u"box: "_s;
        auto* field = visualNamed(window->contentItem(), panel ? u"panelSearchField"_s : u"searchField"_s);
        QObject* view = window->property("view").value<QObject*>();
        check(field && view, mode + u"production search field and list"_s);
        if (!field || !view)
            continue;
        field->forceActiveFocus();
        const auto current = [&] { return view->property("currentIndex").toInt(); };
        const auto caretAtEnd = [&] {
            return field->property("text").toString() == launcher.query()
                && field->property("cursorPosition").toInt() == launcher.query().size();
        };
        // The keyboard onto `row` as the user puts it there: an arrow from the
        // row next to it (↑ on the first row, which keeps it there).
        const auto arrive = [&](int row) {
            const int count = view->property("count").toInt();
            if (row < 0 || row >= count)
                return false;
            view->setProperty("currentIndex", row > 0 ? row - 1 : std::min(1, count - 1));
            key(row > 0 ? Qt::Key_Down : Qt::Key_Up);
            return current() == row;
        };
        launcher.setScope(Launcher::All);
        type(u"合同资料"_s);
        int row = rowOf(contracts);
        check(row >= 0, mode + u"the folder is found by its name"_s);
        view->setProperty("currentIndex", row); // where the list put the keyboard, nobody chose it
        key(Qt::Key_Tab);
        rows();
        check(launcher.scope() != Launcher::All && launcher.query() == u"合同资料", mode + u"Tab on a folder nobody chose switches the scope"_s);
        key(Qt::Key_Backtab, Qt::ShiftModifier);
        rows();
        check(launcher.scope() == Launcher::All, mode + u"Shift+Tab comes back to 全部"_s);
        row = rowOf(contracts);
        check(arrive(row) && tabHint() == folderHint, mode + u"chosen with the arrows, the footer says Tab goes into it"_s);
        key(Qt::Key_Tab);
        check(launcher.query() == contracts + u'\\' && launcher.scope() == Launcher::All && caretAtEnd(),
            mode + u"Tab puts the folder chosen in the box, the caret after it"_s);
        rows();
        check(current() == 0 && launcher.results()->pathAt(0) == contracts && rowOf(year) > 0
                && rowOf(year) < rowOf(contracts + u"\\说明.txt"_s),
            mode + u"its listing: the folder itself on top, folders first"_s);
        check(tabHint() == backHint && launcher.keyHints().value(u"tabKeys"_s).toString() == u"Ctrl+Backspace",
            mode + u"in the folder, the footer gives the way back"_s);
        check(arrive(rowOf(year)), mode + u"down onto the folder in it"_s);
        key(Qt::Key_Tab);
        rows();
        check(launcher.query() == year + u'\\' && rowOf(year + u"\\清单.txt"_s) > 0, mode + u"Tab goes on down"_s);
        key(Qt::Key_Backspace, Qt::ControlModifier);
        rows();
        check(launcher.query() == contracts + u'\\' && caretAtEnd() && current() == rowOf(year) && tabHint() == folderHint,
            mode + u"Ctrl+Backspace comes back, on the folder it left, chosen"_s);
        key(Qt::Key_Tab);
        rows();
        check(launcher.query() == year + u'\\', mode + u"Tab goes into it again"_s);
        key(Qt::Key_Backspace, Qt::ControlModifier);
        rows();
        key(Qt::Key_Backspace, Qt::ControlModifier);
        rows();
        check(launcher.query() == u"合同资料" && launcher.scope() == Launcher::All && caretAtEnd() && current() == rowOf(contracts)
                && tabHint() == folderHint,
            mode + u"Ctrl+Backspace twice: back at the words, on the folder"_s);
        check(field->hasActiveFocus() && !window->property("menuOpen").toBool(), mode + u"the keyboard stays in the box"_s);
        if (panel)
            continue;
        // In 文件夹: back to it.
        launcher.setScope(Launcher::Folders);
        type(u"合同资料"_s);
        check(arrive(rowOf(contracts)), mode + u"文件夹: on the folder"_s);
        key(Qt::Key_Tab);
        rows();
        check(launcher.scope() == Launcher::All && launcher.query() == contracts + u'\\', mode + u"文件夹: Tab goes in, in 全部"_s);
        settle(window, 2);
        const QQuickItem* footerHint = shownNamed(window->contentItem(), u"footerTabHint"_s);
        check(footerHint && footerHint->property("label").toString() == backHint
                && footerHint->property("keys").toString() == u"Ctrl+Backspace",
            mode + u"the footer shows Ctrl+Backspace 返回上一级"_s);
        saveGrab(window, output.filePath(u"folder-steps-box.png"_s));
        key(Qt::Key_Backspace, Qt::ControlModifier);
        rows();
        check(launcher.scope() == Launcher::Folders && launcher.query() == u"合同资料", mode + u"文件夹: Ctrl+Backspace comes back to it"_s);
        launcher.setScope(Launcher::All);
        // A part of a name typed: the folder it completes takes Tab at once.
        type(base + u"\\合同"_s);
        check(current() == 0 && launcher.results()->pathAt(0) == contracts && tabHint() == folderHint,
            mode + u"a part of a name typed: the folder it completes is offered"_s);
        key(Qt::Key_Tab);
        rows();
        check(launcher.query() == contracts + u'\\', mode + u"Tab completes the path"_s);
        key(Qt::Key_Backspace, Qt::ControlModifier);
        rows();
        check(launcher.query() == base + u"\\合同"_s, mode + u"Ctrl+Backspace brings back the path as typed"_s);
        key(Qt::Key_Backspace, Qt::ControlModifier);
        rows();
        check(launcher.query() == base + u'\\' && current() == 0, mode + u"then takes the name typed away"_s);
        // Tab a moment after typing, before the rows for it came: on those
        // rows, never on a folder of the last query's (未索引, for "…\未").
        type(base + u"\\未"_s);
        launcher.setQuery(base + u"\\合同"_s);
        const bool coming = launcher.searching();
        key(Qt::Key_Tab);
        rows();
        pump(50); // the Tab that waited goes after the rows are in
        rows();
        check(launcher.query() == contracts + u'\\' && launcher.scope() == Launcher::All,
            mode + u"Tab right after typing completes from the rows for it (they were %1 yet)"_s
                .arg(coming ? u"not there"_s : u"there"_s));
        key(Qt::Key_Backspace, Qt::ControlModifier);
        rows();
        key(Qt::Key_Backspace, Qt::ControlModifier);
        rows();
        check(launcher.query() == base + u'\\', mode + u"and back out to the folder typed"_s);
        // A folder not indexed, listed from the disk.
        check(arrive(rowOf(unindexed)), mode + u"onto a folder not indexed"_s);
        key(Qt::Key_Tab);
        rows();
        check(launcher.query() == unindexed + u'\\' && rowOf(unindexed + u"\\备注.txt"_s) > 0,
            mode + u"Tab lists a folder not indexed"_s);
        key(Qt::Key_Backspace, Qt::ControlModifier);
        rows();
        check(launcher.query() == base + u'\\' && current() == rowOf(unindexed), mode + u"and comes back out of it"_s);
        // A path typed, not stepped into: a level up, the folder left chosen there.
        type(year + u'\\');
        key(Qt::Key_Backspace, Qt::ControlModifier);
        rows();
        check(launcher.query() == contracts + u'\\' && current() == rowOf(year) && tabHint() == folderHint,
            mode + u"Ctrl+Backspace on a path typed goes up, on the folder it left"_s);
        // The caret elsewhere, or words selected: Ctrl+Backspace edits as ever.
        type(year + u'\\');
        field->setProperty("cursorPosition", 3);
        key(Qt::Key_Backspace, Qt::ControlModifier);
        rows();
        check(launcher.query() != contracts + u'\\' && launcher.query().size() < year.size() + 1,
            mode + u"with the caret inside the path, Ctrl+Backspace deletes a word"_s);
        type(year + u'\\');
        QMetaObject::invokeMethod(field, "selectAll");
        key(Qt::Key_Backspace, Qt::ControlModifier);
        rows();
        check(launcher.query() != contracts + u'\\', mode + u"with the words selected, Ctrl+Backspace deletes them"_s);
        // → on a folder: its menu, with 进入此文件夹搜索, as before.
        type(u"合同资料"_s);
        check(arrive(rowOf(contracts)), mode + u"onto the folder for its menu"_s);
        field->setProperty("cursorPosition", launcher.query().size());
        key(Qt::Key_Right);
        auto* menu = qobject_cast<QQuickWindow*>(window->property("contextMenu").value<QObject*>());
        bool browses = false;
        for (const QVariant& entry : menu ? menu->property("entries").toList() : QVariantList())
            browses = browses || entry.toMap().value(u"action"_s, -1).toInt() == Launcher::BrowseFolder;
        check(window->property("menuOpen").toBool() && browses, mode + u"→ opens the folder's menu, with its browse action"_s);
        key(Qt::Key_Escape);
        check(!window->property("menuOpen").toBool() && launcher.query() == u"合同资料", mode + u"Esc closes it, the words kept"_s);
        // Regex mode: the words are patterns, Tab is the scopes'.
        key(Qt::Key_R, Qt::ControlModifier);
        rows();
        check(launcher.regexMode() && arrive(rowOf(contracts)), mode + u"regex mode: on the folder"_s);
        key(Qt::Key_Tab);
        rows();
        check(launcher.query() == u"合同资料" && launcher.scope() != Launcher::All, mode + u"regex mode: Tab switches the scope"_s);
        key(Qt::Key_R, Qt::ControlModifier);
        launcher.setScope(Launcher::All);
        rows();
        check(!launcher.regexMode(), mode + u"regex mode off again"_s);
    }
    if (previousType >= 0)
        launcher.chooseFileType(previousType);
    else
        launcher.setScope(previousScope);
    window->resize(previousSize);
    type(previousQuery);
    QFile report(output.filePath(u"folder-steps.json"_s));
    check(report.open(QIODevice::WriteOnly), u"write the regression report"_s);
    if (report.isOpen()) report.write(QJsonDocument(QJsonObject {{u"passed"_s, passed}, {u"checks"_s, checks}}).toJson());
    report.close(); // written before a throw ends the process
    say(u"folder steps (#361): %1 checks, production Main.qml/Launcher, local Qt key events, %2"_s
        .arg(checks.size()).arg(passed ? u"passed" : u"FAILED"));
    if (!passed)
        throw std::runtime_error("Folder step regression failed; see folder-steps.json");
}

void checkDialogSearchKeys(IndexService& index, QQmlEngine* engine, const QDir& output)
{
    QTemporaryDir temporary;
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    SearchEngine search(&index, &apps, &places);
    History history(temporary.filePath(u"history.tsv"_s));
    Favorites favorites(temporary.filePath(u"favorites.json"_s));
    DialogBar bar(&search, &history, &favorites, {});
    QQmlComponent component(engine, u"WinShun"_s, u"DialogBarWindow"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"bar"_s, QVariant::fromValue(&bar)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { qWarning().noquote() << "dialog search navigation:" << component.errorString(); return; }
    bar.setWindow(window);
    window->resize(760, 360);
    showOffScreen(window);
    const QString query = u"测试照片"_s;
    bar.setQuery(query);
    QElapsedTimer timeout;
    timeout.start();
    while (bar.results()->count() != 18 && timeout.elapsed() < 2000) pump(10);
    QJsonArray checks;
    bool passed = true;
    const auto check = [&](bool okay, const QString& name) {
        checks.append(QJsonObject {{u"name"_s, name}, {u"passed"_s, okay}});
        passed = okay && passed;
        if (!okay) qWarning("dialog search navigation regression: %s", qUtf8Printable(name));
    };
    check(bar.results()->count() == 18, u"the dialog bar receives the real name search results"_s);
    auto* field = visualNamed(window->contentItem(), u"dialogSearchField"_s);
    auto* list = visualNamed(window->contentItem(), u"dialogResults"_s);
    check(field && list, u"production dialog field and result list"_s);
    settle(window);
    auto* logo = visualNamed(window->contentItem(), u"dialogSearchLogo"_s);
    check(logo && logo->isVisible() && logo->property("status").toInt() == 1,
        u"the dialog search bar loads its inline program logo"_s);
    check(logo && field && logo->x() + logo->width() + 8 <= field->x() && field->x() == 40,
        u"the program logo keeps the original input alignment and spacing"_s);
    const QString originalQuery = bar.query();
    const int originalHeight = window->height();
    bar.setQuery({});
    window->setHeight(bar.barHeight());
    settle(window);
    check(saveGrab(window, output.filePath(u"dialog-search-logo.png"_s)), u"render the dialog logo with the empty search field"_s);
    window->setHeight(originalHeight);
    bar.setQuery(originalQuery);
    timeout.restart();
    while (bar.results()->count() != 18 && timeout.elapsed() < 2000) pump(10);
    if (field && list) {
        // Native dialog placement/activation is outside this key test. Show
        // its real view offscreen without changing the desktop's focus.
        list->setVisible(true);
        list->setHeight(5 * bar.rowHeight());
        settle(window);
        // #245: two lines belong together; the gap to the next result must
        // remain larger than the space between a name and its folder.
        const QString previousSize = SystemTheme::uiSize();
        for (const QString& size : {u"dense"_s, u"compact"_s, u"standard"_s, u"roomy"_s}) {
            SystemTheme::setUiSize(size);
            DialogBar::setUiSize(size);
            list->setHeight(5 * bar.rowHeight());
            settle(window);
            auto* text = visualNamed(list, u"dialogResultText"_s);
            auto* name = text ? visualNamed(text, u"dialogResultName"_s) : nullptr;
            auto* folder = text ? visualNamed(text, u"dialogResultFolder"_s) : nullptr;
            check(text && name && folder, size + u": production dialog text layout"_s);
            if (text && name && folder) {
                const qreal innerGap = folder->y() - name->parentItem()->height();
                const qreal outerGap = bar.rowHeight() - text->height();
                check(text->y() >= 4 && outerGap >= std::max(8.0, innerGap + 2),
                    size + u": dialog result spacing (inner %1, outer %2)"_s.arg(innerGap).arg(outerGap));
                check(saveGrab(window, output.filePath(u"dialog-result-spacing-%1.png"_s.arg(size))), size + u": render dialog result spacing"_s);
            }
        }
        SystemTheme::setUiSize(previousSize);
        DialogBar::setUiSize(previousSize);
        list->setHeight(5 * bar.rowHeight());
        settle(window);
        field->forceActiveFocus();
        field->setProperty("cursorPosition", query.size());
        list->setProperty("currentIndex", 2);
        const auto key = [&](int code, Qt::KeyboardModifiers modifiers = {}, bool repeated = false) {
            QKeyEvent event(QEvent::KeyPress, code, modifiers, {}, repeated);
            QCoreApplication::sendEvent(window, &event);
        };
        const auto menuOpen = [&] { return window->property("menuOpen").toBool(); };
        for (const auto [alias, arrow] : {std::pair{Qt::Key_J, Qt::Key_Down}, {Qt::Key_N, Qt::Key_Down},
                 {Qt::Key_K, Qt::Key_Up}, {Qt::Key_P, Qt::Key_Up}, {Qt::Key_U, Qt::Key_PageUp}, {Qt::Key_D, Qt::Key_PageDown}}) {
            list->setProperty("currentIndex", 2);
            key(arrow);
            const int expected = list->property("currentIndex").toInt();
            list->setProperty("currentIndex", 2);
            key(alias, Qt::ControlModifier);
            check(list->property("currentIndex").toInt() == expected && bar.query() == query && field->hasActiveFocus(),
                u"dialog Ctrl+%1 matches the existing navigation key"_s.arg(int(alias)));
        }
        // 字母直达 as in the launcher and the clipboard (GitHub #357): the
        // numbers keep their column on the rows' left, the letters take it,
        // Shift+letter only selects, Esc only puts the numbers back.
        auto* hints = visualNamed(window->contentItem(), u"dialogNumberHints"_s);
        bar.setLetterJump(true, u"auto"_s, u"ASDFGHJKLQWERTYUIOPZXCVBNM"_s);
        settle(window);
        const auto letterOf = [&](int row) {
            QVariant letter;
            QMetaObject::invokeMethod(window, "letterOf", Q_RETURN_ARG(QVariant, letter), Q_ARG(QVariant, row));
            return letter.toString();
        };
        auto* number = visualNamed(list, u"dialogResultNumber"_s);
        check(hints && number && number->isVisible() && number->x() < 30, u"dialog rows keep their number on the left"_s);
        // Position indicator, scroll-bar wake-up and shortcut HoverTip:
        // left 1–9 stay viewport-relative shortcuts while the bar shows where
        // the viewport sits in the list and the scroll bar wakes on scroll.
        auto* posLabel = visualNamed(window->contentItem(), u"dialogPositionLabel"_s);
        auto* scrollBar = visualNamed(window->contentItem(), u"dialogListScrollBar"_s);
        check(posLabel && posLabel->isVisible() && posLabel->property("text").toString() == u"1–5 / 18",
            u"dialog position label shows the visible range at the top"_s);
        check(scrollBar && scrollBar->isVisible() && !scrollBar->property("scrolling").toBool(),
            u"dialog scroll bar is visible on an overflowing list and calm at rest"_s);
        list->setProperty("contentY", 7.0 * bar.rowHeight());
        check(scrollBar && scrollBar->property("scrolling").toBool(),
            u"dialog scroll bar wakes up while scrolling"_s);
        settle(window, 1);
        QVariant firstNumberAfterScroll;
        QMetaObject::invokeMethod(window, "numberOf", Q_RETURN_ARG(QVariant, firstNumberAfterScroll), Q_ARG(QVariant, 7));
        check(firstNumberAfterScroll.toInt() == 1, u"dialog shortcut numbers stay viewport-relative when scrolled"_s);
        check(posLabel && posLabel->isVisible() && posLabel->property("text").toString() == u"8–12 / 18",
            u"dialog position label tracks the scrolled range"_s);
        check(saveGrab(window, output.filePath(u"dialog-scrolled-position.png"_s)), u"render the scrolled dialog list with position label"_s);
        pump(1000);
        check(scrollBar && !scrollBar->property("scrolling").toBool(),
            u"dialog scroll bar relaxes after scrolling stops"_s);
        // Shrinking the list while scrolled shifts ListView.originY; shortcut
        // numbering and the range label must stay anchored to originY.
        if (const SearchResult* firstItem = bar.results()->at(0)) {
            bar.setQuery(firstItem->name);
            timeout.restart();
            while (bar.results()->count() >= 5 && timeout.elapsed() < 2000) pump(10);
            settle(window, 2);
            QVariant num0;
            QMetaObject::invokeMethod(window, "numberOf", Q_RETURN_ARG(QVariant, num0), Q_ARG(QVariant, 0));
            check(window->property("firstShownRow").toInt() == 0 && num0.toInt() == 1,
                u"shrinking a scrolled dialog list keeps shortcut 1 on row 0 despite originY"_s);
            bar.setQuery(originalQuery);
            timeout.restart();
            while (bar.results()->count() != 18 && timeout.elapsed() < 2000) pump(10);
            settle(window, 2);
            QMetaObject::invokeMethod(window, "numberOf", Q_RETURN_ARG(QVariant, num0), Q_ARG(QVariant, 0));
            check(window->property("firstShownRow").toInt() == 0 && num0.toInt() == 1
                    && posLabel && posLabel->property("text").toString() == u"1–5 / 18",
                u"re-querying after a scrolled shrink keeps row 0 numbered 1 and resets range"_s);
        }
        // Non-overflowing list: neither scroll bar nor position range shows.
        list->setProperty("contentY", list->property("originY").toDouble() - 4.0);
        list->setHeight(20 * bar.rowHeight());
        settle(window);
        check(scrollBar && !scrollBar->isVisible(), u"dialog scroll bar hides when all rows fit"_s);
        check(posLabel && !posLabel->isVisible(), u"dialog position label hides when all rows fit"_s);
        list->setHeight(5 * bar.rowHeight());
        list->setProperty("contentY", list->property("originY").toDouble() - 4.0);
        settle(window);
        // Hovering a left shortcut number explains the shortcut (and any conflict).
        auto* topNumber = visualNamed(list, u"dialogResultNumber"_s);
        auto* rowTip = topNumber && topNumber->parentItem() ? topNumber->parentItem()->findChild<QObject*>(u"dialogRowTip"_s) : nullptr;
        if (topNumber && rowTip) {
            const QPointF at = topNumber->mapToScene(QPointF(topNumber->width() / 2, topNumber->height() / 2));
            QMouseEvent move(QEvent::MouseMove, at, window->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &move);
            settle(window);
            check(rowTip->property("target").value<QObject*>() == topNumber
                    && rowTip->property("shortcut").toString() == u"Ctrl+1"
                    && !rowTip->property("text").toString().isEmpty(),
                u"hovering a dialog shortcut number explains the Ctrl+1 shortcut"_s);
            bar.setNumberKeysLeft(1u << 1);
            settle(window);
            check(rowTip->property("target").value<QObject*>() == topNumber
                    && rowTip->property("shortcut").toString().isEmpty()
                    && rowTip->property("text").toString().contains(u"Ctrl+1"_s),
                u"hovering a taken dialog shortcut number explains the conflict"_s);
            bar.setNumberKeysLeft(0);
            QMouseEvent away(QEvent::MouseMove, QPointF(window->width() - 4, 4), window->mapToGlobal(QPointF(window->width() - 4, 4)),
                Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &away);
            settle(window);
        } else {
            check(false, u"dialog shortcut HoverTip missing"_s);
        }
        if (hints) {
            hints->setProperty("jumping", true);
            settle(window);
            check(letterOf(0) == u"A" && letterOf(1) == u"S" && field->property("readOnly").toBool(),
                u"dialog letters on the rows shown, the box read-only meanwhile"_s);
            list->setProperty("currentIndex", 0);
            key(Qt::Key_D, Qt::ShiftModifier);
            check(list->property("currentIndex").toInt() == 2 && bar.query() == query && !hints->property("jumping").toBool()
                    && !field->property("readOnly").toBool(),
                u"dialog Shift+letter selects its row and leaves the letters"_s);
            hints->setProperty("jumping", true);
            key(Qt::Key_Escape);
            check(!hints->property("jumping").toBool() && bar.query() == query, u"dialog Esc only puts the numbers back"_s);
            hints->setProperty("jumping", true);
            key(Qt::Key_X);
            check(!hints->property("jumping").toBool() && bar.query() == query, u"dialog letter past the rows shown does nothing"_s);
            // With number keys off and numberHints = always, passive letters preview
            // calmly (editable box, no keycap) and pop into keycaps once armed.
            bar.setResultOptions(true, true, u"off"_s);
            bar.setNumberHints(u"always"_s);
            settle(window);
            auto* slot0 = visualNamed(list, u"dialogResultNumber"_s);
            auto* cap0 = slot0 ? visualNamed(slot0, u"hintKeyCap"_s) : nullptr;
            auto* hl = list->property("highlightItem").value<QQuickItem*>();
            check(!hl || !slot0 || !slot0->parentItem() || slot0->parentItem()->z() > hl->z(),
                u"dialog delegate stays above ListView highlightItem"_s);
            check(letterOf(0) == u"A" && letterOf(1) == u"S" && !field->property("readOnly").toBool()
                    && cap0 && !cap0->isVisible(),
                u"dialog passive letter preview shows calm letters without arming the box"_s);
            hints->setProperty("jumping", true);
            settle(window);
            check(letterOf(0) == u"A" && field->property("readOnly").toBool() && cap0 && cap0->isVisible(),
                u"arming dialog letter jump switches passive preview into accent keycaps"_s);
            hints->setProperty("jumping", false);
            bar.setResultOptions(true, true, u"ctrl"_s);
            settle(window);
        }
        bar.setLetterJump(false, u"auto"_s, u"ASDFGHJKLQWERTYUIOPZXCVBNM"_s);
        list->setProperty("currentIndex", 2);
        key(Qt::Key_Right);
        auto* menu = qobject_cast<QQuickWindow*>(window->property("contextMenu").value<QObject*>());
        check(menu && menuOpen() && menu->property("row").toInt() == 2, u"Right opens the selected dialog result's menu"_s);
        if (menu) {
            check(saveGrab(menu, output.filePath(u"search-menu-dialog.png"_s)), u"render the production dialog menu"_s);
            key(Qt::Key_Right, {}, true);
            key(Qt::Key_O, Qt::ControlModifier, true);
            check(menuOpen(), u"holding the shortcut keeps the dialog menu open"_s);
            key(Qt::Key_Left);
            check(!menuOpen() && field->hasActiveFocus() && bar.query() == query
                    && list->property("currentIndex").toInt() == 2 && field->property("cursorPosition").toInt() == query.size(),
                u"Left returns to the same dialog result and query"_s);
            field->setProperty("cursorPosition", 1);
            key(Qt::Key_Right);
            check(!menuOpen() && field->property("cursorPosition").toInt() == 2, u"Right within the dialog query edits its caret"_s);
            QMetaObject::invokeMethod(field, "select", Q_ARG(int, 0), Q_ARG(int, query.size()));
            key(Qt::Key_Right);
            check(!menuOpen() && field->property("selectedText").toString().isEmpty(), u"Right collapses a dialog text selection"_s);
            key(Qt::Key_Right, Qt::ShiftModifier);
            check(!menuOpen(), u"modified Right does not open a dialog menu"_s);
            field->setProperty("cursorPosition", 1);
            key(Qt::Key_O, Qt::ControlModifier);
            check(menuOpen(), u"Ctrl+O opens the dialog menu from within the query"_s);
            QInputMethodEvent composing(u"shi"_s, {});
            QCoreApplication::sendEvent(field, &composing);
            check(!menuOpen() && field->property("preeditText").toString() == u"shi", u"IME composition closes the dialog menu"_s);
            key(Qt::Key_Right);
            check(!menuOpen(), u"Right does not open a dialog menu while composing"_s);
            QInputMethodEvent composingForShortcut(u"shi"_s, {});
            QCoreApplication::sendEvent(field, &composingForShortcut);
            check(field->property("preeditText").toString() == u"shi", u"the dialog Ctrl+O fixture is composing"_s);
            key(Qt::Key_O, Qt::ControlModifier);
            check(!menuOpen(), u"Ctrl+O does not open a dialog menu while composing"_s);
            QInputMethodEvent ended;
            QCoreApplication::sendEvent(field, &ended);
            key(Qt::Key_O, Qt::ControlModifier);
            key(Qt::Key_Escape);
            check(!menuOpen() && bar.query() == query, u"Escape returns without clearing the dialog query"_s);
            list->setVisible(false);
            key(Qt::Key_O, Qt::ControlModifier);
            check(!menuOpen(), u"a hidden dialog list opens no result menu"_s);
        }
        // Favorite keyword tag matches the launcher's "收藏 · <keyword>", and
        // typing the keyword in DialogBar brings that favorite to the top.
        if (const SearchResult* firstItem = bar.results()->at(0)) {
            Favorite fav;
            fav.kind = firstItem->isDir ? Favorite::Kind::Folder : Favorite::Kind::File;
            fav.path = firstItem->path;
            fav.name = firstItem->name;
            fav.keyword = u"zp"_s;
            favorites.add(fav);
            bar.setQuery(u"zp"_s);
            timeout.restart();
            while ((bar.tags().isEmpty() || !bar.tags().first().contains(u"zp"_s)) && timeout.elapsed() < 2000) pump(10);
            check(!bar.tags().isEmpty() && bar.tags().first() == DialogBar::tr("收藏 · %1").arg(u"zp"_s),
                u"dialog favorite rows display their keyword tag"_s);
        }
    }
    QFile report(output.filePath(u"dialog-navigation.json"_s));
    check(report.open(QIODevice::WriteOnly), u"write the dialog regression report"_s);
    if (report.isOpen()) {
        report.write(QJsonDocument(QJsonObject {{u"passed"_s, passed}, {u"checks"_s, checks}}).toJson());
        report.close();
    }
    say(u"dialog search navigation: %1 checks, production DialogBarWindow/DialogBar, local Qt input, %2"_s
        .arg(checks.size()).arg(passed ? u"passed" : u"FAILED"));
    if (!passed)
        throw std::runtime_error("Production dialog search navigation regression failed; see dialog-navigation.json");
}

// The first item of that name that is shown (the launcher has a footer in its
// box and another in its panel).
QQuickItem* shownNamed(QQuickItem* root, const QString& name)
{
    if (root->objectName() == name && root->isVisible())
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = shownNamed(child, name))
            return found;
    }
    return nullptr;
}

// #185 and #174: the shortcut panels (one click opens them, a click elsewhere
// or on the button closes them, the last row asks for the settings) and the
// clipboard's own sizes. Production QML; the clicks go to the window as
// events, nothing is injected globally.
bool checkShortcutPanels(QQmlEngine* engine, const QDir& output)
{
    QTemporaryDir temporary;
    IndexService index(temporary.path(), IndexService::Options{});
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    History history(temporary.filePath(u"history.tsv"_s));
    SearchEngine search(&index, &apps, &places);
    Launcher launcher(&index, &apps, &places, &search, &history);
    ClipStore store(temporary.filePath(u"clipboard"_s));
    store.open(QDateTime::currentMSecsSinceEpoch());
    for (int i = 0; i < 12; ++i)
        store.add(ClipCapture {.kind = ClipKind::Text, .text = u"Clipboard entry number %1 with some text to read"_s.arg(i)},
            QDateTime::currentMSecsSinceEpoch() + i);
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    clipboard.setRowOptions(false, u"standard"_s, SystemTheme::uiSize());
    Placement placement(temporary.filePath(u"state.ini"_s), u"Launcher"_s);
    Placement clipPlacement(temporary.filePath(u"clipboard.ini"_s), u"Clipboard"_s);
    WindowFrame frame(false);
    WindowFrame clipFrame(false);
    int launcherAsked = 0, clipboardAsked = 0; // the last row's requests for the settings
    QObject::connect(&launcher, &Launcher::shortcutSettingsRequested, [&] { ++launcherAsked; });
    QObject::connect(&clipboard, &Clipboard::shortcutSettingsRequested, [&] { ++clipboardAsked; });
    QJsonArray checks;
    bool passed = true;
    const auto check = [&](bool okay, const QString& name) {
        checks.append(QJsonObject {{u"name"_s, name}, {u"passed"_s, okay}});
        passed = okay && passed;
        if (!okay) qWarning("shortcut panel regression: %s", qUtf8Printable(name));
    };
    const auto click = [](QQuickWindow* window, QQuickItem* item) {
        const QPointF at = item->mapToScene({item->width() / 2, item->height() / 2});
        QMouseEvent press(QEvent::MouseButtonPress, at, window->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, {});
        QCoreApplication::sendEvent(window, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), Qt::LeftButton, {}, {});
        QCoreApplication::sendEvent(window, &release);
        pump(120);
    };
    const auto tipWindow = [] {
        for (QWindow* window : QGuiApplication::allWindows()) {
            if (window->objectName() == u"shortcutTip" && window->isVisible())
                return qobject_cast<QQuickWindow*>(window);
        }
        return static_cast<QQuickWindow*>(nullptr);
    };
    // The panel of one window: opens on a click and not on hovering, shows
    // its keys in groups, closes on a click elsewhere and on the button,
    // and its last row asks for the settings.
    const auto panel = [&](QQuickWindow* window, int& asked, const QString& name,
                           const QStringList& expected, const QStringList& absent, QQuickItem* elsewhere) {
        QQuickItem* button = shownNamed(window->contentItem(), u"shortcutsButton"_s);
        check(button && button->isVisible(), name + u": the info button shows"_s);
        if (!button) return;
        const QPointF at = button->mapToScene({button->width() / 2, button->height() / 2});
        QMouseEvent hover(QEvent::MouseMove, at, window->mapToGlobal(at), Qt::NoButton, {}, {});
        QCoreApplication::sendEvent(window, &hover);
        pump(500);
        check(!tipWindow(), name + u": hovering opens nothing"_s);
        click(window, button);
        QQuickWindow* tip = tipWindow();
        check(tip && button->property("open").toBool(), name + u": a click opens the panel"_s);
        if (!tip) return;
        const QVariantList rows = tip->property("rows").toList();
        QStringList keys, groups;
        for (const QVariant& row : rows) {
            keys << row.toMap().value(u"keys"_s).toString();
            const QString group = row.toMap().value(u"group"_s).toString();
            if (!groups.contains(group)) groups << group;
        }
        check(groups.size() >= 2 && rows.size() < 20, name + u": compact and grouped (%1 keys in %2 groups)"_s.arg(rows.size()).arg(groups.size()));
        for (const QString& key : expected)
            check(keys.contains(key), name + u": lists " + key);
        for (const QString& key : absent)
            check(!keys.contains(key), name + u": leaves out " + key);
        check(visualNamed(tip->contentItem(), u"shortcutLink"_s) != nullptr, name + u": the last row leads to all shortcuts"_s);
        settle(tip, 2);
        saveGrab(tip, output.filePath(u"shortcut-panel-%1.png"_s.arg(name)));
        click(window, elsewhere);
        check(!tipWindow() && !button->property("open").toBool(), name + u": a click elsewhere closes it"_s);
        click(window, button);
        check(tipWindow() != nullptr, name + u": opens again"_s);
        click(window, button);
        check(!tipWindow(), name + u": the button closes it"_s);
        click(window, button);
        tip = tipWindow();
        QQuickItem* link = tip ? visualNamed(tip->contentItem(), u"shortcutLink"_s) : nullptr;
        if (link) click(tip, link);
        check(asked == 1 && !tipWindow(), name + u": the last row asks for the settings and closes the panel"_s);
    };

    {
        QQmlComponent component(engine, u"WinShun"_s, u"Main"_s);
        std::unique_ptr<QObject> object(component.createWithInitialProperties({
            {u"launcher"_s, QVariant::fromValue(&launcher)}, {u"clipboard"_s, QVariant::fromValue(&clipboard)},
            {u"frame"_s, QVariant::fromValue(&frame)}, {u"placement"_s, QVariant::fromValue(&placement)}}));
        auto* window = qobject_cast<QQuickWindow*>(object.get());
        if (!window) {
            qWarning().noquote() << "shortcut panel:" << component.errorString();
            return false;
        }
        window->create();
        launcher.setWindow(window);
        frame.setWindow(window);
        placement.setWindow(window);
        placement.rememberWidth(760, 640);
        showOffScreen(window);
        launcher.setQuery(u"panel"_s);
        launcher.results()->assign({SearchResult{.name = u"Example.txt"_s, .path = u"T:\\Example.txt"_s}}, {});
        placement.placeOn(window->screen());
        settle(window);
        QQuickItem* field = visualNamed(window->contentItem(), u"searchField"_s);
        saveGrab(window, output.filePath(u"launcher.png"_s));
        check(field != nullptr, u"launcher: the search field shows"_s);
        { // the footer's 更多 sort menu: Esc closes just it, and it is shut when the launcher hides
            QQuickItem* footer = visualNamed(window->contentItem(), u"launcherFooter"_s);
            QQuickItem* order = visualNamed(window->contentItem(), u"footerOrder"_s);
            check(footer && order, u"sort menu: the footer and its chips exist"_s);
            if (footer && order && field) {
                const auto isOpen = [&] { return footer->property("sortMenuOpen").toBool(); };
                const auto escape = [&] {
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                    QCoreApplication::sendEvent(window, &press);
                    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
                    QCoreApplication::sendEvent(window, &release);
                    pump(120);
                };
                field->forceActiveFocus();
                const QPointF at = order->mapToScene({order->width() - 18, order->height() / 2});
                QMouseEvent press(QEvent::MouseButtonPress, at, window->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, {});
                QCoreApplication::sendEvent(window, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), Qt::LeftButton, {}, {});
                QCoreApplication::sendEvent(window, &release);
                pump(150);
                check(isOpen(), u"sort menu: a click on 更多 opens it"_s);
                say(u"sort menu: open %1, field focus %2, window focus item %3"_s.arg(isOpen())
                        .arg(field->hasActiveFocus()).arg(window->activeFocusItem() ? window->activeFocusItem()->objectName() : u"none"_s));
                settle(window);
                saveGrab(window, output.filePath(u"launcher-sortmenu-open.png"_s));
                escape();
                check(!isOpen(), u"sort menu: Esc closes it"_s);
                check(window->isVisible(), u"sort menu: Esc left the launcher showing"_s);
                footer->setProperty("sortMenuOpen", true);
                pump(60);
                window->hide();
                pump(100);
                check(!isOpen(), u"sort menu: hiding the launcher closes it"_s);
                showOffScreen(window);
                pump(100);
                check(!isOpen(), u"sort menu: still shut when the launcher shows again"_s);
                settle(window);
            }
        }
        if (field)
            panel(window, launcherAsked, u"launcher"_s,
                  {u"Shift+Tab"_s, u"Ctrl+Tab"_s, u"Ctrl+S"_s, u"Ctrl+T"_s}, {u"Ctrl+C"_s, u"Ctrl+Shift+Enter"_s, u"Ctrl+J / N · Ctrl+K / P"_s}, field);

        Settings settings;
        SettingsEditor editor(settings, u"software"_s);
        QObject::connect(&editor, &SettingsEditor::edited, &launcher, [&launcher](const Settings& changed) {
            launcher.setKeyboardHints(changed.shortcutHelp, changed.footerHints);
        });
        QQuickItem* help = shownNamed(window->contentItem(), u"shortcutsButton"_s);
        QQuickItem* hints = shownNamed(window->contentItem(), u"footerKeyHints"_s);
        check(help && hints, u"launcher: help and footer controls exist"_s);
        if (help && hints) {
            const qreal withHelp = hints->mapToScene({hints->width(), 0}).x();
            click(window, help);
            editor.setShortcutHelp(false);
            settle(window);
            check(!tipWindow() && !help->isVisible(), u"launcher: disabling help closes its open panel"_s);
            check(hints->mapToScene({hints->width(), 0}).x() > withHelp,
                u"launcher: hiding help releases the button's space"_s);
            for (const bool showHelp : {false, true}) {
                for (const bool showFooter : {false, true}) {
                    editor.setShortcutHelp(showHelp);
                    editor.setFooterHints(showFooter);
                    settle(window);
                    check(help->isVisible() == showHelp && hints->isVisible() == showFooter,
                        u"launcher: help=%1 and footer=%2 apply independently"_s.arg(showHelp).arg(showFooter));
                    check(showFooter || hints->width() == 0, u"launcher: hidden hints release their width"_s);
                    Settings restored;
                    restored.load();
                    check(restored.shortcutHelp == showHelp && restored.footerHints == showFooter,
                        u"launcher: both hint options persist"_s);
                    saveGrab(window, output.filePath(u"hints-%1-%2.png"_s.arg(showHelp).arg(showFooter)));
                }
            }
        }
        for (const QString& query : {QString(), u"   "_s, u"no-matching-file"_s}) {
            launcher.setQuery(query);
            pump(200);
            launcher.results()->assign({}, {});
            launcher.setCurrentRow(-1);
            settle(window);
            QQuickItem* tab = hints ? visualNamed(hints, u"footerTabHint"_s) : nullptr;
            check(tab && tab->isVisible() == !query.trimmed().isEmpty(),
                u"launcher: scope hint follows nonblank input, including no results"_s);
        }
        struct AdminCase {
            QString name;
            bool expected;
            bool directory = false;
            bool missing = false;
            bool offline = false;
            AppKind app = AppKind::None;
        };
        const AdminCase adminCases[] {
            {u"Portable.ExE"_s, true}, {u"Setup.msi"_s, true}, {u"Script.cmd"_s, true}, {u"Notes.txt"_s, false},
            {.name = u"Folder.exe"_s, .expected = false, .directory = true},
            {.name = u"Missing.exe"_s, .expected = false, .missing = true},
            {.name = u"Offline.exe"_s, .expected = false, .offline = true},
            {.name = u"Installed.exe"_s, .expected = true, .app = AppKind::Desktop},
            {.name = u"StoreApp.exe"_s, .expected = false, .app = AppKind::Store},
        };
        for (const AdminCase& test : adminCases) {
            SearchResult result;
            result.name = test.name;
            result.path = temporary.filePath(test.name);
            result.isDir = test.directory;
            result.missing = test.missing;
            result.offline = test.offline;
            result.app = test.app;
            result.elevatable = test.app == AppKind::Desktop;
            launcher.results()->assign({result}, {});
            launcher.setCurrentRow(0);
            settle(window);
            check(launcher.results()->data(launcher.results()->index(0), ResultModel::ElevatableRole).toBool() == test.expected,
                u"launcher: elevation capability for "_s + test.name);
            QQuickItem* row = visualNamed(window->contentItem(), u"resultRow-0"_s);
            QQuickItem* shield = row ? visualNamed(row, u"adminAction"_s) : nullptr;
            check(shield && shield->isVisible() == test.expected, u"launcher: shield visibility for "_s + test.name);
            bool menuAdmin = false;
            for (const QVariant& item : launcher.menuItems(0))
                menuAdmin = menuAdmin || item.toMap().value(u"action"_s).toInt() == Launcher::RunAsAdmin;
            check(menuAdmin == test.expected, u"launcher: shield agrees with the menu for "_s + test.name);
            if (test.name == u"Portable.ExE")
                saveGrab(window, output.filePath(u"portable-admin.png"_s));
        }
        {
            // 显示删除按钮 (GitHub #316): a file's row has the button unless
            // the settings take it away; its other buttons and its menu's 更多选项 stay.
            SearchResult file;
            file.name = u"Notes.txt"_s;
            file.path = temporary.filePath(file.name);
            launcher.results()->assign({file}, {});
            launcher.setCurrentRow(0);
            settle(window);
            const auto shown = [&](const QString& name) {
                QQuickItem* row = visualNamed(window->contentItem(), u"resultRow-0"_s);
                QQuickItem* action = row ? visualNamed(row, name) : nullptr;
                return action && action->isVisible();
            };
            const auto menuHasAction = [&](int row, Launcher::Action action, bool* danger = nullptr) {
                for (const QVariant& item : launcher.menuItems(row)) {
                    const QVariantMap map = item.toMap();
                    if (map.value(u"action"_s, -1).toInt() == action) {
                        if (danger)
                            *danger = map.value(u"danger"_s).toBool();
                        return true;
                    }
                }
                return false;
            };
            check(shown(u"deleteAction"_s) && shown(u"copyPathAction"_s), u"launcher: a file's row has its delete button"_s);
            launcher.setRowActions({u"admin"_s, u"reveal"_s, u"terminal"_s, u"copy"_s, u"copyPath"_s});
            settle(window);
            check(!shown(u"deleteAction"_s) && shown(u"copyPathAction"_s),
                u"launcher: the delete button goes with its setting, the other buttons stay"_s);
            bool singleRecycleDanger = false;
            check(menuHasAction(0, Launcher::Recycle, &singleRecycleDanger) && singleRecycleDanger,
                u"launcher: quick context menu directly offers Move to Recycle Bin with danger styling"_s);
            check(menuHasAction(0, Launcher::SystemMenu), u"launcher: the row's menu still leads to Windows' own"_s);
            saveGrab(window, output.filePath(u"row-without-delete.png"_s));
            launcher.setRowActions(rowactions::defaults());
            settle(window);
            check(shown(u"deleteAction"_s), u"launcher: the delete button is back with its setting"_s);

            // The buttons chosen and ordered in the settings (Results/RowActions):
            // only those named, left to right as named; the ones that are not
            // there by default too.
            {
                const auto lefts = [&](const QStringList& names) {
                    QList<qreal> xs;
                    QQuickItem* row = visualNamed(window->contentItem(), u"resultRow-0"_s);
                    for (const QString& name : names) {
                        QQuickItem* action = row ? visualNamed(row, name) : nullptr;
                        xs.append(action && action->isVisible() ? action->mapToScene({0, 0}).x() : -1);
                    }
                    return xs;
                };
                check(!shown(u"previewAction"_s) && !shown(u"copyNameAction"_s) && !shown(u"favoriteAction"_s),
                    u"row buttons: the ones to be chosen show without having been"_s);
                launcher.setRowActions({u"delete"_s, u"preview"_s, u"copyPath"_s, u"favorite"_s, u"copyName"_s});
                settle(window);
                const QList<qreal> chosen = lefts({u"deleteAction"_s, u"previewAction"_s, u"copyPathAction"_s, u"favoriteAction"_s,
                    u"copyNameAction"_s});
                check(std::ranges::all_of(chosen, [](qreal x) { return x >= 0; }) && std::ranges::is_sorted(chosen)
                        && std::ranges::adjacent_find(chosen) == chosen.end(),
                    u"row buttons: the chosen five do not stand in the order chosen"_s);
                check(!shown(u"terminalAction"_s), u"row buttons: one not chosen still shows"_s);
                saveGrab(window, output.filePath(u"row-buttons-chosen.png"_s));
                launcher.setRowActions({});
                settle(window);
                check(!shown(u"deleteAction"_s) && !shown(u"copyPathAction"_s) && !shown(u"previewAction"_s),
                    u"row buttons: none chosen, some still show"_s);
                launcher.setRowActions(rowactions::defaults());
                settle(window);
                const QList<qreal> usual = lefts({u"terminalAction"_s, u"copyPathAction"_s, u"deleteAction"_s});
                check(std::ranges::all_of(usual, [](qreal x) { return x >= 0; }) && std::ranges::is_sorted(usual)
                        && !shown(u"previewAction"_s),
                    u"row buttons: back as they come, not in their usual order"_s);
            }

            // Cross-folder multi-select delete in quick context menu (#434):
            // supports files across distinct folders, prunes nested children, skips apps/drive roots,
            // keeps systemCommandRunning true while in flight, and removes deleted rows.
            QDir().mkpath(temporary.filePath(u"dirA/nested"_s));
            QDir().mkpath(temporary.filePath(u"dirB"_s));
            const QString fileA = QDir::toNativeSeparators(temporary.filePath(u"dirA/alpha.txt"_s));
            const QString dirNested = QDir::toNativeSeparators(temporary.filePath(u"dirA/nested"_s));
            const QString fileNested = QDir::toNativeSeparators(temporary.filePath(u"dirA/nested/inner.txt"_s));
            const QString fileB = QDir::toNativeSeparators(temporary.filePath(u"dirB/beta.txt"_s));
            for (const QString& p : {fileA, fileNested, fileB}) {
                QFile f(p);
                check(f.open(QIODevice::WriteOnly), u"launcher: create temp file for recycle check"_s);
                f.write("data");
            }
            SearchResult rA; rA.name = u"alpha.txt"_s; rA.path = fileA;
            SearchResult rNestedDir; rNestedDir.name = u"nested"_s; rNestedDir.path = dirNested; rNestedDir.isDir = true;
            SearchResult rNestedFile; rNestedFile.name = u"inner.txt"_s; rNestedFile.path = fileNested;
            SearchResult rB; rB.name = u"beta.txt"_s; rB.path = fileB;
            SearchResult rApp; rApp.name = u"FakeApp"_s; rApp.path = u"shell:AppsFolder\\Fake"_s; rApp.app = AppKind::Desktop;
            SearchResult rRoot; rRoot.name = u"C:\\"_s; rRoot.path = u"C:\\"_s; rRoot.isDir = true;
            launcher.results()->assign({rA, rNestedDir, rNestedFile, rB, rApp, rRoot}, {});
            check(!menuHasAction(4, Launcher::Recycle), u"launcher: app row does not offer Move to Recycle Bin"_s);
            check(!menuHasAction(5, Launcher::Recycle), u"launcher: drive root does not offer Move to Recycle Bin"_s);
            launcher.results()->setSelection({fileA, dirNested, fileNested, fileB, rApp.path, rRoot.path});
            bool multiRecycleDanger = false;
            check(menuHasAction(0, Launcher::Recycle, &multiRecycleDanger) && multiRecycleDanger,
                u"launcher: cross-folder multi-select menu offers Move to Recycle Bin with danger styling"_s);
            // Open the actual QML ContextMenu window for the multi-selection and grab it for visual inspection.
            QMetaObject::invokeMethod(window, "openContextMenu", Q_ARG(QVariant, 0), Q_ARG(QVariant, QVariant()));
            settle(window);
            if (auto* ctxMenu = qobject_cast<QQuickWindow*>(window->property("contextMenu").value<QObject*>())) {
                check(ctxMenu->isVisible(), u"launcher: multi-select context menu opens"_s);
                settle(ctxMenu);
                saveGrab(ctxMenu, output.filePath(u"context-menu-multi-recycle.png"_s));
                QMetaObject::invokeMethod(window, "closeContextMenu");
                settle(window);
            }
            // Footer's clear-history button uses Phosphor's broom at the same 16 px icon size
            // as the neighbouring ShortcutsButton (info) and other footer icons.
            history.record(fileA, u"alpha"_s, QDateTime::currentMSecsSinceEpoch());
            emit launcher.historyChanged();
            settle(window);
            if (QQuickItem* clearBtn = visualNamed(window->contentItem(), u"clearHistoryButton"_s)) {
                check(clearBtn->isVisible(), u"footer: clearHistory shows when history is non-empty"_s);
                check(clearBtn->property("iconName").toString() == u"broom"_s, u"footer: clearHistory uses broom icon"_s);
                check(qFuzzyCompare(clearBtn->property("iconSize").toReal(), 16.0),
                    u"footer: clearHistory iconSize matches info button (16 px)"_s);
                saveGrab(window, output.filePath(u"footer-clear-history.png"_s));
            }
            QStringList recycledBatch;
            bool runningDuringRecycle = false;
            launcher.setFileRecycler([&](const QStringList& batch, std::function<void(bool)> done) {
                recycledBatch = batch;
                runningDuringRecycle = launcher.systemCommandRunning();
                for (const QString& p : batch) {
                    QFileInfo info(p);
                    if (info.isDir())
                        QDir(p).removeRecursively();
                    else
                        QFile::remove(p);
                }
                done(true);
            });
            launcher.trigger(0, Launcher::Recycle);
            settle(window);
            launcher.setFileRecycler({});
            check(runningDuringRecycle, u"launcher: systemCommandRunning stays true while recycle runs"_s);
            check(!launcher.systemCommandRunning(), u"launcher: systemCommandRunning clears after recycle completes"_s);
            check(recycledBatch == QStringList({fileA, dirNested, fileB}),
                u"launcher: cross-folder multi-select recycle deduplicates and prunes nested descendants"_s);
            check(launcher.results()->count() == 2,
                u"launcher: deleted files and nested child rows are removed while non-deletable rows remain"_s);
        }
        window->hide();
    }
    {
        QQmlComponent component(engine, u"WinShun"_s, u"ClipboardWindow"_s);
        std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"clipboard"_s, QVariant::fromValue(&clipboard)},
            {u"placement"_s, QVariant::fromValue(&clipPlacement)}, {u"frame"_s, QVariant::fromValue(&clipFrame)}}));
        auto* window = qobject_cast<QQuickWindow*>(object.get());
        if (!window) {
            qWarning().noquote() << "shortcut panel:" << component.errorString();
            return false;
        }
        window->create();
        clipFrame.setWindow(window);
        clipPlacement.setWindow(window);
        clipboard.setActive(true);
        clipboard.handleShown();
        showOffScreen(window);
        settle(window);
        QQuickItem* field = visualNamed(window->contentItem(), u"clipboardSearchField"_s);
        check(field != nullptr, u"clipboard: the search field shows"_s);
        if (field)
            panel(window, clipboardAsked, u"clipboard"_s,
                  {u"Shift+Tab"_s, u"Ctrl+P"_s, u"Delete"_s}, {u"Ctrl+C"_s, u"Esc"_s}, field);
        // 剪贴板大小: every step's own type and row height, one line a row in 密集.
        struct Step { const char* size; int row; };
        const Step steps[] = {{"dense", 32}, {"compact", 52}, {"standard", 60}, {"roomy", 68}};
        for (const Step& step : steps) {
            const QString name = QString::fromLatin1(step.size);
            clipboard.setRowOptions(false, u"standard"_s, name);
            settle(window);
            pump(200);
            QQuickItem* row = visualNamed(window->contentItem(), u"clipboardRow-0"_s);
            check(row && qRound(row->height()) == step.row, name + u": a row is %1 px high"_s.arg(step.row));
            check(clipboard.uiSize() == name, name + u": the window follows the clipboard's size"_s);
            saveGrab(window, output.filePath(u"clipboard-%1.png"_s.arg(name)));
        }
        clipboard.setActive(false);
        window->hide();
    }
    QFile report(output.filePath(u"shortcut-panel.json"_s));
    if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(QJsonObject {{u"passed"_s, passed}, {u"checks"_s, checks}}).toJson());
    say(u"shortcut panels and clipboard sizes: %1 checks, production QML, %2"_s.arg(checks.size()).arg(passed ? u"passed" : u"FAILED"));
    return passed;
}

// #162: actual native hit testing/messages and production QML, isolated from
// the user's profile. No global mouse or keyboard input is injected.
bool checkSearchLogo(QQmlEngine* engine, const QDir& output)
{
    QTemporaryDir temporary;
    IndexService index(temporary.path(), IndexService::Options{});
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    History history(temporary.filePath(u"history.tsv"_s));
    SearchEngine search(&index, &apps, &places);
    Launcher launcher(&index, &apps, &places, &search, &history);
    ClipStore store(temporary.filePath(u"clipboard"_s));
    store.open(QDateTime::currentMSecsSinceEpoch());
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    Placement placement(temporary.filePath(u"state.ini"_s), u"Launcher"_s);
    WindowFrame frame(false);
    QQmlComponent component(engine, u"WinShun"_s, u"Main"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {u"launcher"_s, QVariant::fromValue(&launcher)}, {u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"frame"_s, QVariant::fromValue(&frame)}, {u"placement"_s, QVariant::fromValue(&placement)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) {
        qWarning().noquote() << "search logo:" << component.errorString();
        return false;
    }
    window->create();
    launcher.setWindow(window);
    frame.setWindow(window);
    frame.setSideSizing(true);
    placement.setWindow(window);
    placement.rememberWidth(760, 640);
    showOffScreen(window);
    launcher.setQuery(u"Logo 测试"_s);
    launcher.results()->assign({SearchResult{.name = u"Example.txt"_s, .path = u"T:\\Example.txt"_s}}, {});
    int settingsRequests = 0;
    int panelRequests = 0;
    QObject::connect(&launcher, &Launcher::panelToggleRequested, window, [&] { ++panelRequests; });
    QObject::connect(&launcher, &Launcher::settingsRequested, window, [&] { ++settingsRequests; });
    QJsonArray checks;
    bool passed = true;
    const auto check = [&](bool okay, const QString& name) {
        checks.append(QJsonObject {{u"name"_s, name}, {u"passed"_s, okay}});
        passed = okay && passed;
        if (!okay) qWarning("search logo regression: %s", qUtf8Printable(name));
    };
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    for (const bool panel : {false, true}) {
        launcher.setPanel(panel);
        frame.setResizable(panel);
        if (panel) placement.enterPanel(window->screen());
        else placement.placeOn(window->screen());
        settle(window);
        const QString mode = panel ? u"panel: "_s : u"box: "_s;
        auto* logo = visualNamed(window->contentItem(), panel ? u"panelSearchLogo"_s : u"searchLogo"_s);
        auto* field = visualNamed(window->contentItem(), panel ? u"panelSearchField"_s : u"searchField"_s);
        check(logo && field && logo->isVisible(), mode + u"production logo and field are visible"_s);
        if (!logo || !field) continue;
        auto* image = visualNamed(logo, u"searchLogoImage"_s);
        auto* badge = visualNamed(logo, u"searchLogoPinBadge"_s);
        check(image && image->property("status").toInt() == 1 && !image->property("source").toUrl().isEmpty(),
            mode + u"application logo loads at the requested display size"_s);
        check(logo->property("help").toString().contains(u'\n'), mode + u"all pointer actions have a multiline hint"_s);
        const auto point = [&] {
            const QPointF local = logo->mapToScene({logo->width() / 2, logo->height() / 2});
            POINT native {qRound(local.x() * window->devicePixelRatio()), qRound(local.y() * window->devicePixelRatio())};
            ::ClientToScreen(hwnd, &native);
            return MAKELPARAM(native.x, native.y);
        };
        const auto click = [&](UINT down, UINT up) {
            const LPARAM at = point();
            ::SendMessageW(hwnd, down, HTCAPTION, at);
            ::SendMessageW(hwnd, up, HTCAPTION, at);
            settle(window);
        };
        field->forceActiveFocus();
        const QString query = launcher.query();
        // #173: cover the whole visible handle, rather than only its centre.
        // Measure the native client extent independently of Qt's reported DPR.
        const QString previousUiSize = SystemTheme::uiSize();
        for (const QString& size : {u"dense"_s, u"compact"_s, u"standard"_s, u"roomy"_s}) {
            SystemTheme::setUiSize(size);
            settle(window);
            // #162: the type strip shown once (文件) and closed again keeps
            // its chips and + laid out over the header's lower half, clipped
            // away but still registered as controls. Not 内容: its scan's
            // stop warns.
            if (!panel) {
                for (const Launcher::Scope scope : {Launcher::Files, Launcher::All}) {
                    launcher.setScope(scope);
                    settle(window);
                }
            }
            RECT client {};
            ::GetClientRect(hwnd, &client);
            const qreal scaleX = qreal(client.right) / window->width();
            const qreal scaleY = qreal(client.bottom) / window->height();
            const auto nativePoint = [&](QPointF local) {
                const QPointF scene = logo->mapToScene(local);
                POINT native {qRound(scene.x() * scaleX), qRound(scene.y() * scaleY)};
                ::ClientToScreen(hwnd, &native);
                return MAKELPARAM(native.x, native.y);
            };
            for (const qreal x : {0.1, 0.5, 0.9}) {
                for (const qreal y : {0.1, 0.5, 0.9}) {
                    const LPARAM at = nativePoint({logo->width() * x, logo->height() * y});
                    const LRESULT hit = ::SendMessageW(hwnd, WM_NCHITTEST, 0, at);
                    ::SendMessageW(hwnd, WM_NCMOUSEMOVE, hit, at);
                    const bool hovered = logo->property("pointed").toBool();
                    const bool wasPinned = launcher.pinned();
                    ::SendMessageW(hwnd, WM_NCMBUTTONDOWN, hit, at);
                    ::SendMessageW(hwnd, WM_NCMBUTTONUP, hit, at);
                    pump(1); // the queued native handle action
                    check(hit == HTCAPTION && hovered && launcher.pinned() != wasPinned,
                        mode + u"%1 logo hover and action cover (%2,%3)"_s.arg(size).arg(x).arg(y));
                    launcher.setPinned(wasPinned);
                }
            }
            for (const QPointF& outside : {QPointF(-2, logo->height() / 2), QPointF(logo->width() + 2, logo->height() / 2),
                     QPointF(logo->width() / 2, -2), QPointF(logo->width() / 2, logo->height() + 2)}) {
                const LPARAM at = nativePoint(outside);
                const LRESULT hit = ::SendMessageW(hwnd, WM_NCHITTEST, 0, at);
                ::SendMessageW(hwnd, WM_NCMOUSEMOVE, hit, at);
                const bool hovered = logo->property("pointed").toBool();
                const bool wasPinned = launcher.pinned();
                ::SendMessageW(hwnd, WM_NCMBUTTONDOWN, hit, at);
                ::SendMessageW(hwnd, WM_NCMBUTTONUP, hit, at);
                pump(1);
                check(!hovered && launcher.pinned() == wasPinned,
                    mode + u"%1 does not activate outside the logo at (%2,%3)"_s.arg(size).arg(outside.x()).arg(outside.y()));
            }
        }
        SystemTheme::setUiSize(previousUiSize);
        launcher.results()->assign({SearchResult{.name = u"Example.txt"_s, .path = u"T:\\Example.txt"_s}}, {}); // the scopes searched again
        settle(window);
        check(::SendMessageW(hwnd, WM_NCHITTEST, 0, point()) == HTCAPTION, mode + u"logo retains the system drag region"_s);
        click(WM_NCMBUTTONDOWN, WM_NCMBUTTONUP);
        check(launcher.pinned() && badge && badge->isVisible() && logo->property("checked").toBool(),
            mode + u"native middle click pins and reveals the badge"_s);
        check(launcher.query() == query && field->hasActiveFocus(), mode + u"pinning keeps query and input focus"_s);
        check(saveGrab(window, output.filePath(panel ? u"logo-panel-pinned.png"_s : u"logo-box-pinned.png"_s)),
            mode + u"pinned state screenshot"_s);
        QKeyEvent pinKey(QEvent::KeyPress, Qt::Key_T, Qt::ControlModifier);
        QCoreApplication::sendEvent(window, &pinKey);
        settle(window);
        check(!launcher.pinned() && badge && !badge->isVisible(), mode + u"Ctrl+T clears the same visible pin state"_s);
        click(WM_NCMBUTTONDBLCLK, WM_NCMBUTTONUP);
        click(WM_NCMBUTTONDOWN, WM_NCMBUTTONUP);
        check(!launcher.pinned(), mode + u"repeated middle clicks toggle independently"_s);
        ::SendMessageW(hwnd, WM_NCMBUTTONUP, HTCAPTION, point());
        settle(window);
        check(!launcher.pinned(), mode + u"release without a logo press does nothing"_s);
        ::SendMessageW(hwnd, WM_NCMBUTTONDOWN, HTCAPTION, point());
        ::SendMessageW(hwnd, WM_MBUTTONUP, 0, 0);
        ::SendMessageW(hwnd, WM_NCMBUTTONUP, HTCAPTION, point());
        settle(window);
        check(!launcher.pinned(), mode + u"releasing outside cancels the logo action"_s);
        const LPARAM start = point();
        const LPARAM moved = MAKELPARAM(short(LOWORD(start)) + ::GetSystemMetricsForDpi(SM_CXDRAG, ::GetDpiForWindow(hwnd)),
            short(HIWORD(start)));
        ::SendMessageW(hwnd, WM_NCMBUTTONDOWN, HTCAPTION, start);
        ::SendMessageW(hwnd, WM_NCMOUSEMOVE, HTCAPTION, moved);
        ::SendMessageW(hwnd, WM_NCMBUTTONUP, HTCAPTION, start);
        settle(window);
        check(!launcher.pinned(), mode + u"moving beyond the drag threshold cancels even after returning to the logo"_s);
        // DragDetect consumes the queued release without global input.
        const int before = settingsRequests;
        ::PostMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, point());
        ::PostMessageW(hwnd, WM_LBUTTONUP, 0, 0);
        pump(100);
        check(settingsRequests == before + 1, mode + u"native left click requests settings exactly once"_s);
        logo->forceActiveFocus();
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, {});
        QCoreApplication::sendEvent(window, &enter);
        check(settingsRequests == before + 2, mode + u"keyboard activation opens settings"_s);
        field->forceActiveFocus();
        const QRect area = window->screen()->availableGeometry();
        ::SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
        window->setPosition(area.topLeft() + QPoint(24, 36));
        ::SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
        settle(window);
        pump(300);
        const QPoint draggedPosition = window->position();
        const QSize size = window->size();
        click(WM_NCRBUTTONDOWN, WM_NCRBUTTONUP);
        auto* positionMenu = window->findChild<QQuickWindow*>(panel ? u"panelHeaderMenu"_s : u"searchHeaderMenu"_s);
        check(positionMenu && positionMenu->isVisible(), mode + u"right click opens the position menu"_s);
        if (positionMenu) {
            auto* highlight = visualNamed(positionMenu->contentItem(), u"contextMenuHighlight"_s);
            for (const int rowIndex : {2, 1, 2, 1}) {
                auto* entry = visualNamed(positionMenu->contentItem(), u"contextMenuEntry-%1"_s.arg(rowIndex));
                if (!entry || !highlight) { check(false, mode + u"position menu exposes its entries"_s); break; }
                const QPointF at = entry->mapToScene({entry->width() / 2, entry->height() / 2});
                QMouseEvent move(QEvent::MouseMove, at, positionMenu->mapToGlobal(at), Qt::NoButton, {}, {});
                QCoreApplication::sendEvent(positionMenu, &move);
                pump(350);
                settle(positionMenu);
                check(positionMenu->property("highlighted").toInt() == rowIndex
                    && std::abs(entry->mapToScene({0, 0}).y() - highlight->mapToScene({0, 0}).y()) < 1,
                    mode + u"position menu paint follows pointer row %1"_s.arg(rowIndex));
            }
        }
        click(WM_NCRBUTTONDOWN, WM_NCRBUTTONUP);
        check(positionMenu && !positionMenu->isVisible(), mode + u"second Logo right click closes the menu"_s);
        click(WM_NCRBUTTONDOWN, WM_NCRBUTTONUP);
        check(positionMenu && positionMenu->isVisible(), mode + u"third Logo right click reopens the menu"_s);
        QKeyEvent closePosition(QEvent::KeyPress, Qt::Key_Escape, {});
        QCoreApplication::sendEvent(window, &closePosition);
        check(positionMenu && !positionMenu->isVisible() && window->isVisible(), mode + u"Escape closes only the position menu"_s);
        click(WM_NCRBUTTONDOWN, WM_NCRBUTTONUP);
        if (positionMenu) {
            QMetaObject::invokeMethod(positionMenu, "activate", Q_ARG(QVariant, 1));
            settle(window);
        }
        const QPoint wanted(area.x() + (area.width() - size.width()) / 2,
            area.y() + (area.height() - size.height()) / 2);
        check((window->position() - wanted).manhattanLength() <= 2,
            mode + u"position menu centers on the current monitor work area"_s);
        check(window->size() == size && launcher.query() == query && !launcher.pinned() && settingsRequests == before + 2,
            mode + u"centering retains size, query and pin state without opening settings"_s);
        placement.restoreDraggedPosition();
        pump(300);
        check((window->position() - draggedPosition).manhattanLength() <= 2,
            mode + u"restore returns to the dragged position after centering"_s);
        auto* positionBar = window->property("searchBar").value<QQuickItem*>();
        auto* expand = visualNamed(window->contentItem(), panel ? u"panelCollapseButton"_s : u"searchExpandButton"_s);
        auto* expandGlyph = expand ? visualNamed(expand, u"searchExpandGlyph"_s) : nullptr;
        check(positionBar && expandGlyph && expandGlyph->width() < expand->width() && expandGlyph->height() < expand->height(),
            mode + u"compact expand glyph retains the larger click target"_s);
        if (positionBar && positionMenu) {
            launcher.setLogoRightClick(u"center"_s);
            click(WM_NCRBUTTONDOWN, WM_NCRBUTTONUP);
            pump(350);
            check(positionMenu && !positionMenu->isVisible() && (window->position() - wanted).manhattanLength() <= 2,
                mode + u"configured center action returns in one native right click without a menu"_s);
            check(logo->property("help").toString().contains(QCoreApplication::translate("SearchBar", "居中")),
                mode + u"logo help reflects the configured center action"_s);
            click(WM_NCRBUTTONDOWN, WM_NCRBUTTONUP);
            pump(350);
            check((window->position() - wanted).manhattanLength() <= 2 && !positionMenu->isVisible(),
                mode + u"repeated direct right click keeps the chosen position"_s);
            if (!panel) {
                placement.attach(QRect(area.topLeft() + QPoint(24, 36), QSize(200, 40)), window->screen());
                settle(window);
            }
            launcher.setLogoRightClick(u"top"_s);
            click(WM_NCRBUTTONDOWN, WM_NCRBUTTONUP);
            pump(350);
            const QPoint home(area.x() + (area.width() - size.width()) / 2,
                std::clamp(qRound(area.y() + area.height() * 0.2), area.top(), std::max(area.top(), area.y() + area.height() - size.height())));
            check((window->position() - (panel ? wanted : home)).manhattanLength() <= 2 && !positionMenu->isVisible(),
                mode + u"top action moves the search box up and centers the expanded panel"_s);
            check(positionBar->property("positionAction").toString() == (panel ? u"center" : u"top"),
                mode + u"effective position action matches the current window mode"_s);
            check(saveGrab(window, output.filePath(panel ? u"logo-panel-quick-top.png"_s : u"logo-box-quick-top.png"_s)),
                mode + u"capture compact expand icon and direct positioning"_s);
            launcher.setLogoRightClick(u"restore"_s);
            click(WM_NCRBUTTONDOWN, WM_NCRBUTTONUP);
            pump(350);
            check((window->position() - draggedPosition).manhattanLength() <= 2 && !positionMenu->isVisible(),
                mode + u"direct restore returns to the separately remembered drag position"_s);
            check(window->size() == size && launcher.query() == query && !launcher.pinned() && settingsRequests == before + 2,
                mode + u"direct position actions preserve size, query, pin state and settings requests"_s);
            launcher.setLogoRightClick(u"menu"_s);
            click(WM_NCRBUTTONDOWN, WM_NCRBUTTONUP);
            check(positionMenu->isVisible(), mode + u"switching back to menu mode restores its native right-click entry"_s);
            launcher.setLogoRightClick(u"center"_s);
            check(!positionMenu->isVisible(), mode + u"changing the right-click setting closes an existing menu"_s);
            launcher.setLogoRightClick(u"menu"_s);
            settle(window); // finish the native menu's hide before the next synthetic hover
        }
        check(saveGrab(window, output.filePath(panel ? u"logo-panel.png"_s : u"logo-box.png"_s)), mode + u"unpinned state screenshot"_s);
        ::SendMessageW(hwnd, WM_NCMOUSEMOVE, HTCAPTION, point());
        // Keep the user's real cursor where it is: the synthetic hover owns
        // its leave message too, instead of TrackMouseEvent's real cursor check.
        TRACKMOUSEEVENT tracking {sizeof tracking, TME_NONCLIENT | TME_LEAVE | TME_CANCEL, hwnd, HOVER_DEFAULT};
        ::TrackMouseEvent(&tracking);
        MSG leave {};
        while (::PeekMessageW(&leave, hwnd, WM_NCMOUSELEAVE, WM_NCMOUSELEAVE, PM_REMOVE)) {}
        pump(400);
        settle(window);
        auto* hint = visualNamed(window->contentItem(), panel ? u"panelSearchLogoHint"_s : u"searchLogoHint"_s);
        check(hint && hint->isVisible() && hint->property("shownText").toString() == logo->property("help").toString(),
            mode + u"native hover displays every logo action"_s);
        if (!hint || !hint->isVisible())
            say(u"logo hover: pointer (%1,%2), pointed %3, hint %4"_s.arg(frame.pointer().x()).arg(frame.pointer().y())
                .arg(logo->property("pointed").toBool()).arg(hint ? hint->property("shown").toBool() : false));
        check(saveGrab(window, output.filePath(panel ? u"logo-panel-hint.png"_s : u"logo-box-hint.png"_s)), mode + u"hint screenshot"_s);
        ::SendMessageW(hwnd, WM_NCMOUSELEAVE, 0, 0);
        // #244: native hit regions and actual text selection, including the
        // whitespace that used to be interpreted as the window's title bar.
        auto* bar = field->parentItem();
        const auto hit = [&](QPointF scene) {
            const qreal dpr = window->devicePixelRatio();
            RECT bounds {};
            ::GetWindowRect(hwnd, &bounds);
            const QPoint local(qRound(scene.x() * dpr), qRound(scene.y() * dpr));
            return ::SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(bounds.left + local.x(), bounds.top + local.y()));
        };
        for (const qreal y : {1.0, field->height() / 2, field->height() - 1})
            check(hit(field->mapToScene({field->width() / 2, y})) == HTCLIENT,
                mode + u"field and vertical padding keep client hit testing at y=%1"_s.arg(y));
        check(hit(bar->mapToScene({bar->width() / 2 + 90, bar->height() - 2})) == HTCLIENT,
            mode + u"header whitespace does not move the window"_s);
        auto* modeGrip = visualNamed(window->contentItem(), panel ? u"panelSearchGrip"_s : u"searchGrip"_s);
        check(modeGrip && modeGrip->isVisible(), mode + u"every theme has a visible window movement grip"_s);
        const LRESULT gripHit = modeGrip ? hit(modeGrip->mapToScene({modeGrip->width() / 2, modeGrip->height() / 2})) : HTNOWHERE;
        check(modeGrip && (modeGrip->isVisible() ? gripHit == HTCAPTION : gripHit != HTCAPTION),
            mode + u"visible grip moves the window; hidden grip leaves no caption hotspot"_s);
        const QPoint beforeDrag = window->position();
        const QPointF from = field->mapToScene({3, field->height() / 2});
        const QPointF to = field->mapToScene({field->width() - 3, field->height() / 2});
        QMouseEvent press(QEvent::MouseButtonPress, from, window->mapToGlobal(from), Qt::LeftButton, Qt::LeftButton, {});
        QCoreApplication::sendEvent(window, &press);
        QMouseEvent move(QEvent::MouseMove, to, window->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, {});
        QCoreApplication::sendEvent(window, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, to, window->mapToGlobal(to), Qt::LeftButton, {}, {});
        QCoreApplication::sendEvent(window, &release);
        check(!field->property("selectedText").toString().isEmpty() && window->position() == beforeDrag,
            mode + u"drag selects real text without moving the window"_s);
        auto* scopes = visualNamed(window->contentItem(), panel ? u"panelScopeTabs"_s : u"searchScopeTabs"_s);
        if (scopes) {
            const auto selectScope = [&](int index) {
                const QVariantList segments = scopes->property("segments").toList();
                if (index >= segments.size()) return;
                const QVariantMap segment = segments[index].toMap();
                const QPointF at = scopes->mapToScene({segment.value(u"x"_s).toReal() + segment.value(u"width"_s).toReal() / 2, scopes->height() / 2});
                QMouseEvent scopePress(QEvent::MouseButtonPress, at, window->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, {});
                QCoreApplication::sendEvent(window, &scopePress);
                QMouseEvent scopeRelease(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), Qt::LeftButton, {}, {});
                QCoreApplication::sendEvent(window, &scopeRelease);
                settle(window);
            };
            selectScope(1);
            check(launcher.scope() == Launcher::Files, mode + u"Files tab selects Files in one click"_s);
            selectScope(2);
            check(launcher.scope() == Launcher::Content, mode + u"Content tab selects Content in one click"_s);
            selectScope(0);
            check(launcher.scope() == Launcher::All, mode + u"All tab selects All in one click"_s);
            launcher.setScope(Launcher::All);
            launcher.results()->assign({}, {});
            field->forceActiveFocus();
            QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, {});
            QCoreApplication::sendEvent(window, &tab);
            check(launcher.scope() == Launcher::Files, mode + u"Tab still advances the search scope"_s);
            QKeyEvent backTab(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
            QCoreApplication::sendEvent(window, &backTab);
            check(launcher.scope() == Launcher::All, mode + u"Shift+Tab still returns to All"_s);
            auto* rangeHint = visualNamed(window->contentItem(), u"footerTabHint"_s);
            check(rangeHint && !rangeHint->property("label").toString().isEmpty() && !rangeHint->isVisible(),
                mode + u"footer omits the duplicated scope-switch hint"_s);
        }
        // GitHub #433: the Logo switched off. The words start where its left
        // edge was and nothing there moves the window; the grip still does;
        // a pinned box says so by a key before the scopes, a click on which unpins.
        {
            const qreal logoInk = logo->mapToScene({logo->property("inkInset").toReal(), 0}).x();
            const QPointF logoSpot = logo->mapToScene({logo->width() / 2, logo->height() / 2});
            const qreal fieldEnd = field->mapToScene({field->width(), 0}).x();
            launcher.setSearchLogo(false);
            settle(window);
            auto* pinChip = visualNamed(window->contentItem(), panel ? u"panelPinKey"_s : u"searchPinKey"_s);
            check(!logo->isVisible() && std::abs(field->mapToScene({0, 0}).x() - logoInk) < 0.01,
                mode + u"Logo off: the words start where the Logo's left edge was"_s);
            check(hit(logoSpot) == HTCLIENT && modeGrip && hit(modeGrip->mapToScene({modeGrip->width() / 2, modeGrip->height() / 2})) == HTCAPTION,
                mode + u"Logo off: its place is the field's, the grip still moves the window"_s);
            check(pinChip && !pinChip->isVisible() && std::abs(field->mapToScene({field->width(), 0}).x() - fieldEnd) < 0.01,
                mode + u"Logo off, not pinned: no key more, the field as wide to the right"_s);
            check(saveGrab(window, output.filePath(panel ? u"logo-panel-off.png"_s : u"logo-box-off.png"_s)), mode + u"Logo off screenshot"_s);
            launcher.setPinned(true);
            settle(window);
            if (pinChip && scopes) {
                const QRectF pinRect = pinChip->mapRectToScene(QRectF(0, 0, pinChip->width(), pinChip->height()));
                check(pinChip->isVisible() && pinRect.right() <= scopes->mapToScene({0, 0}).x() - 5.99
                        && field->mapToScene({field->width(), 0}).x() <= pinRect.left() - 7.99,
                    mode + u"Logo off, pinned: the pin key stands between the field and the scopes"_s);
                check(saveGrab(window, output.filePath(panel ? u"logo-panel-off-pinned.png"_s : u"logo-box-off-pinned.png"_s)),
                    mode + u"Logo off pinned screenshot"_s);
                const QPointF at = pinRect.center();
                QMouseEvent pinPress(QEvent::MouseButtonPress, at, window->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, {});
                QCoreApplication::sendEvent(window, &pinPress);
                QMouseEvent pinRelease(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), Qt::LeftButton, {}, {});
                QCoreApplication::sendEvent(window, &pinRelease);
                settle(window);
                check(!launcher.pinned() && !pinChip->isVisible(), mode + u"Logo off: a click on the pin key unpins"_s);
            }
            launcher.setPinned(false);
            launcher.setSearchLogo(true);
            settle(window);
            check(logo->isVisible() && field->mapToScene({0, 0}).x() > logoInk + logo->property("inkInset").toReal(),
                mode + u"Logo back on: where it was, the words after it"_s);
        }
        if (!panel) {
            launcher.setPureSearch(true);
            launcher.setQuery({});
            settle(window);
            ::SendMessageW(hwnd, WM_NCMOUSEMOVE, HTCAPTION, point());
            ::TrackMouseEvent(&tracking);
            while (::PeekMessageW(&leave, hwnd, WM_NCMOUSELEAVE, WM_NCMOUSELEAVE, PM_REMOVE)) {}
            pump(400);
            settle(window);
            auto* tip = visualNamed(window->contentItem(), u"searchLogoHint"_s);
            auto* popup = tip ? tip->findChild<QQuickWindow*>(u"hoverTipWindow"_s) : nullptr;
            check(tip && tip->property("detached").toBool() && popup && popup->isVisible()
                && popup->height() > 20 && popup->width() > 150,
                u"empty short window shows complete logo help in a separate window"_s);
            check(popup && popup->flags().testFlag(Qt::WindowDoesNotAcceptFocus)
                && popup->flags().testFlag(Qt::WindowTransparentForInput), u"detached help takes neither focus nor mouse input"_s);
            if (popup) check(saveGrab(popup, output.filePath(u"logo-empty-hint.png"_s)), u"empty-window help screenshot"_s);
            launcher.setQuery(query);
            settle(window);
            check(tip && !tip->property("detached").toBool(), u"help returns inside when the host grows while hovered"_s);
            launcher.setQuery({});
            settle(window);
            check(tip && tip->property("detached").toBool(), u"help detaches again when the host shrinks while hovered"_s);
            ::SendMessageW(hwnd, WM_NCMOUSELEAVE, 0, 0);
            launcher.setPureSearch(false);
            launcher.setQuery(query);
            auto* grip = visualNamed(window->contentItem(), u"searchGrip"_s);
            check(grip && (grip->isVisible() ? hit(grip->mapToScene({grip->width() / 2, grip->height() / 2})) == HTCAPTION
                : hit(grip->mapToScene({grip->width() / 2, grip->height() / 2})) != HTCAPTION), u"grip hit testing follows actual visibility"_s);
            if (grip && grip->isVisible()) {
                const int previousRequests = panelRequests;
                const QPointF at = grip->mapToScene({grip->width() / 2, grip->height() / 2});
                RECT bounds {};
                ::GetWindowRect(hwnd, &bounds);
                ::SendMessageW(hwnd, WM_NCLBUTTONDBLCLK, HTCAPTION,
                    MAKELPARAM(bounds.left + qRound(at.x() * window->devicePixelRatio()), bounds.top + qRound(at.y() * window->devicePixelRatio())));
                settle(window);
                check(panelRequests == previousRequests + 1, u"double click on the grip requests panel expansion"_s);
                launcher.setPanel(false);
            }
        }
    }
    {
        // The native window made anew (Qt can; the old handle takes the
        // subclass with it): the logo and the grip still drag the window. A
        // user's did not until the program was started again.
        const int warned = g_warnings;
        window->destroy();
        window->create();
        showOffScreen(window);
        settle(window);
        // Placement and the frame each say so in the log (a warning: it is
        // rare, and worth a line in a user's diagnostics). Wanted here, so
        // not counted against the run.
        check(g_warnings == warned + 2, u"a native window made anew is logged once by the placement and once by the frame"_s);
        g_warnings = warned;
        const HWND fresh = reinterpret_cast<HWND>(window->winId());
        RECT client {};
        ::GetClientRect(fresh, &client);
        const auto hitOn = [&](const QString& boxName, const QString& panelName) {
            auto* item = visualNamed(window->contentItem(), launcher.panel() ? panelName : boxName);
            if (!item || !item->isVisible())
                return LRESULT(HTNOWHERE);
            const QPointF scene = item->mapToScene({item->width() / 2, item->height() / 2});
            POINT native {qRound(scene.x() * client.right / window->width()), qRound(scene.y() * client.bottom / window->height())};
            ::ClientToScreen(fresh, &native);
            return ::SendMessageW(fresh, WM_NCHITTEST, 0, MAKELPARAM(native.x, native.y));
        };
        check(!::IsWindow(hwnd) && fresh && hitOn(u"searchLogo"_s, u"panelSearchLogo"_s) == HTCAPTION
            && hitOn(u"searchGrip"_s, u"panelSearchGrip"_s) == HTCAPTION,
            u"a native window made anew keeps the logo and the grip as drag regions"_s);
        const bool wasMoving = placement.moving();
        ::SendMessageW(fresh, WM_ENTERSIZEMOVE, 0, 0);
        const bool moving = placement.moving();
        ::SendMessageW(fresh, WM_EXITSIZEMOVE, 0, 0);
        pump(1);
        check(!wasMoving && moving && !placement.moving(), u"a native window made anew is still followed while it is moved"_s);
    }
    window->hide();
    QQuickWindow appearance;
    QQmlComponent sample(engine, u"WinShun"_s, u"AppearancePreview"_s);
    std::unique_ptr<QObject> sampleObject(sample.create());
    auto* sampleRoot = qobject_cast<QQuickItem*>(sampleObject.get());
    check(sampleRoot != nullptr, u"appearance settings load the production size preview"_s);
    if (sampleRoot) {
        sampleRoot->setParentItem(appearance.contentItem());
        const QString previousSize = SystemTheme::uiSize();
        showOffScreen(&appearance);
        for (const QString& size : {u"dense"_s, u"compact"_s, u"standard"_s, u"roomy"_s}) {
            SystemTheme::setUiSize(size);
            sampleRoot->setSize({760, sampleRoot->implicitHeight()});
            appearance.resize(760, qRound(sampleRoot->height()));
            settle(&appearance);
            auto* logo = visualNamed(sampleRoot, u"appearanceSearchLogo"_s);
            check(logo && logo->isVisible() && logo->property("status").toInt() == 1,
                u"appearance preview shows the program logo at %1 size"_s.arg(size));
            check(saveGrab(&appearance, output.filePath(u"appearance-logo-%1.png"_s.arg(size))),
                u"appearance preview screenshot at %1 size"_s.arg(size));
        }
        SystemTheme::setUiSize(previousSize);
        appearance.hide();
    }
    QFile report(output.filePath(u"search-logo.json"_s));
    if (!report.open(QIODevice::WriteOnly)) {
        qWarning("Cannot write search logo report");
        return false;
    }
    report.write(QJsonDocument(QJsonObject {{u"passed"_s, passed}, {u"checks"_s, checks}}).toJson());
    say(u"search logo: %1 checks, native WindowFrame and production Main, %2"_s.arg(checks.size()).arg(passed ? u"passed" : u"FAILED"));
    return passed;
}

// The production window, real temporary images, and local Qt input events;
// no global shortcuts, external file opening, or user's profile involved.
void checkFileGallery(Launcher& launcher, IndexService& index, SystemCatalog& places, QQmlEngine* engine, const QDir& output)
{
    QTemporaryDir temporary;
    const auto check = [](bool okay, const char* reason) {
        if (!okay) qWarning("file gallery regression: %s", reason);
    };
    launcher.setPanel(true);
    ClipStore store(temporary.filePath(u"clipboard"_s));
    check(store.open(QDateTime::currentMSecsSinceEpoch()), "could not open isolated clipboard");
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    Placement placement(temporary.filePath(u"state.ini"_s), u"Launcher"_s);
    WindowFrame frame(false);
    QQmlComponent component(engine, u"WinShun"_s, u"Main"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {u"launcher"_s, QVariant::fromValue(&launcher)}, {u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"frame"_s, QVariant::fromValue(&frame)}, {u"placement"_s, QVariant::fromValue(&placement)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { qWarning().noquote() << "gallery main:" << component.errorString(); return; }
    window->create();
    window->resize(1200, 680);
    launcher.setWindow(window);
    frame.setWindow(window);
    placement.setWindow(window);
    showOffScreen(window);
    FilePreview filePreview;
    WindowFrame previewFrame(false);
    std::unique_ptr<QObject> previewObject;
    engine->addImageProvider(u"preview"_s, filePreview.imageProvider());
    filePreview.setBuiltInOnly(true);
    filePreview.setLauncherWindow(window);
    filePreview.setWindowFactory([&]() -> QQuickWindow* {
        QQmlComponent previewComponent(engine, u"WinShun"_s, u"PreviewWindow"_s);
        previewObject.reset(previewComponent.createWithInitialProperties({
            {u"preview"_s, QVariant::fromValue(&filePreview)}, {u"frame"_s, QVariant::fromValue(&previewFrame)}}));
        auto* previewWindow = qobject_cast<QQuickWindow*>(previewObject.get());
        if (previewWindow)
            previewFrame.setWindow(previewWindow);
        return previewWindow;
    });
    launcher.setPreview(&filePreview);
    SearchResults pictures;
    for (int i = 0; i < 18; ++i) {
        const bool portrait = i % 3 == 0;
        QImage image(portrait ? QSize(400, 720) : QSize(960, 540), QImage::Format_RGB32);
        image.fill(QColor::fromHsl((i * 39) % 360, 110, 150));
        QPainter painter(&image);
        painter.setPen(QPen(Qt::white, 8));
        painter.drawRect(image.rect().adjusted(30, 30, -30, -30));
        painter.setFont(QFont(u"Segoe UI"_s, 72));
        painter.drawText(image.rect(), Qt::AlignCenter, QString::number(i + 1));
        painter.end();
        const QString path = temporary.filePath(u"测试照片_%1.png"_s.arg(i + 1, 2, 10, QLatin1Char('0')));
        check(image.save(path), "could not create isolated image");
        pictures.append(SearchResult{.name = QFileInfo(path).fileName(), .path = QDir::toNativeSeparators(path),
            .modified = static_cast<FileTime>(QDateTime::currentSecsSinceEpoch())});
    }
    {
        const auto files = index.index();
        const auto lock = files->writeLock();
        const auto root = files->addRoot(QDir::toNativeSeparators(temporary.path()).toStdString());
        for (const SearchResult& picture : pictures) {
            if (!picture.name.endsWith(u".png"))
                continue;
            const auto id = files->add(root, picture.name.toStdString(), 0);
            files->setModified(id, picture.modified);
            files->setSize(id, packSize(QFileInfo(picture.path).size()));
        }
    }
    launcher.chooseFileType(1);
    launcher.setQuery(u"测试照片"_s);
    QElapsedTimer timeout;
    timeout.start();
    while (launcher.searching() && timeout.elapsed() < 2000)
        pump(10);
    check(!launcher.searching() && launcher.results()->count() == 18,
        "real image search did not produce the gallery rows");
    if (auto* field = visualNamed(window->contentItem(), u"panelSearchField"_s))
        field->setProperty("text", u"测试照片"_s);
    checkPreviewPlacement(launcher, window, filePreview, &output);
    checkSearchMenuKeys(launcher, places, window, filePreview, output);
    checkFolderSteps(launcher, index, window, output);
    checkDialogSearchKeys(index, engine, output);
    launcher.setGallery(true);
    settle(window);
    pump(1200);
    settle(window);
    auto* grid = visualNamed(window->contentItem(), u"panelGallery"_s);
    check(grid && grid->property("count").toInt() == 18, "production model not displayed");
    if (auto* image = visualNamed(window->contentItem(), u"fileGalleryThumbnail"_s)) {
        const qreal longest = std::max(image->implicitWidth(), image->implicitHeight());
        check(longest >= 128, "large previews reused a small list thumbnail");
    } else check(false, "gallery thumbnail missing");
    check(saveGrab(window, output.filePath(u"files-large-icons.png"_s)), "could not render the gallery");
    if (grid) {
        const auto key = [&](int code, Qt::KeyboardModifiers modifiers = {}, const QString& text = {}) {
            QKeyEvent event(QEvent::KeyPress, code, modifiers, text);
            QCoreApplication::sendEvent(window, &event);
        };
        auto* field = visualNamed(window->contentItem(), u"panelSearchField"_s);
        check(field != nullptr, "production search field missing");
        if (field) {
            field->forceActiveFocus();
            grid->setProperty("currentIndex", 0);
            const QString typed = launcher.query();
            launcher.setPreviewHotkey(u"Alt+P"_s);
            key(Qt::Key_P, Qt::AltModifier);
            check(filePreview.isOpen(), "default preview key did not reach FilePreview through Main.qml");
            key(Qt::Key_P, Qt::AltModifier);
            check(!filePreview.isOpen(), "default preview key did not close the preview");
            launcher.setPreviewHotkey(u"Space"_s);
            QInputMethodEvent composing(u"shi"_s, {});
            QCoreApplication::sendEvent(field, &composing);
            check(field->property("preeditText").toString() == u"shi", "IME preedit fixture did not reach the search field");
            key(Qt::Key_Space, {}, u" "_s);
            check(!filePreview.isOpen(), "preview stole Space during IME composition");
            QInputMethodEvent ended;
            QCoreApplication::sendEvent(field, &ended);
            launcher.setQuery(typed);
            timeout.restart();
            while (launcher.searching() && timeout.elapsed() < 2000)
                pump(10);
            key(Qt::Key_Space, {}, u" "_s);
            check(!filePreview.isOpen() && launcher.query() == typed + u' ', "Space right after typing did not type a space");
            launcher.setQuery(typed);
            timeout.restart();
            while (launcher.searching() && timeout.elapsed() < 2000)
                pump(10);
            key(Qt::Key_Up); // the selection was moved: Space is the preview key now
            key(Qt::Key_Space, {}, u" "_s);
            check(filePreview.isOpen() && launcher.query() == typed, "Space did not preview without changing the query");
            QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Space, {}, u" "_s, true);
            QCoreApplication::sendEvent(window, &repeat);
            check(filePreview.isOpen(), "holding the preview key repeatedly toggled the window");
            QElapsedTimer loaded;
            loaded.start();
            while (filePreview.kind() == FilePreview::Loading && loaded.elapsed() < 3000)
                pump(10);
            check(filePreview.kind() == FilePreview::Picture, "preview did not decode the actual selected file");
            if (filePreview.window())
                check(saveGrab(filePreview.window(), output.filePath(u"preview-space.png"_s)), "could not render the real preview window");
            key(Qt::Key_Space, {}, u" "_s);
            check(!filePreview.isOpen(), "Space did not close the preview");
            key(Qt::Key_Space, Qt::ShiftModifier, u" "_s);
            check(!filePreview.isOpen() && launcher.query() == typed + u' ', "Shift+Space did not preserve multiword search input");
            launcher.setQuery(typed);
            timeout.restart();
            while (launcher.searching() && timeout.elapsed() < 2000)
                pump(10);
            launcher.setPreviewHotkey(u"Ctrl+Shift+P"_s);
            key(Qt::Key_P, Qt::AltModifier);
            check(!filePreview.isOpen(), "old preview key remained active after reassignment");
            key(Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
            check(!filePreview.isOpen(), "preview accepted additional modifiers");
            key(Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
            check(filePreview.isOpen(), "custom preview chord did not open the preview");
            key(Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
            launcher.setPreviewHotkey({});
            key(Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
            check(!filePreview.isOpen(), "clearing the shortcut did not disable it");
            launcher.setPreviewHotkey(u"Alt+P"_s);
            say(u"preview hotkeys: production Main.qml + FilePreview; default, Space, IME preedit, repeat, Shift+Space, reassignment, modifiers, disabled"_s);
            key(Qt::Key_Down);
            const int row = grid->property("currentIndex").toInt();
            check(row == grid->property("columns").toInt(), "Down did not navigate a grid row");
            key(Qt::Key_Right);
            check(grid->property("currentIndex").toInt() == row + 1, "Right did not navigate a grid column");
            key(Qt::Key_O, Qt::ControlModifier);
            check(window->property("menuOpen").toBool(), "Ctrl+O did not open the gallery row's menu");
            key(Qt::Key_Left);
            check(!window->property("menuOpen").toBool() && grid->property("currentIndex").toInt() == row + 1,
                "Left did not return from the menu to the same gallery row");
            key(Qt::Key_Right, Qt::ShiftModifier);
            check(launcher.results()->selectedCount() == 2, "Shift+Right did not select a range");
            key(Qt::Key_Z, {}, u"z"_s);
            check(launcher.query().endsWith(u'z') && field->hasActiveFocus(), "typing from the grid did not resume search");
            launcher.setQuery(u"测试照片"_s);
            timeout.restart();
            while (launcher.searching() && timeout.elapsed() < 2000)
                pump(10);
            // A middle click previews that tile and opens nothing; an open preview follows the mouse after a moment,
            // and the arrow keys bring it back to the current tile.
            const auto tile = [&](int i) {
                QQuickItem* item = nullptr;
                QMetaObject::invokeMethod(grid, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i));
                return item;
            };
            const auto mouse = [&](int i, QEvent::Type type, Qt::MouseButton button) {
                QQuickItem* item = tile(i);
                if (!item) { check(false, "gallery tile missing"); return; }
                const QPointF at = item->mapToScene(QPointF(item->width() / 2 + (type == QEvent::MouseMove ? 3 : 0), item->height() / 2));
                QMouseEvent event(type, at, window->mapToGlobal(at.toPoint()), button,
                                  type == QEvent::MouseButtonPress ? button : Qt::NoButton, {});
                QCoreApplication::sendEvent(window, &event);
            };
            const auto middleClick = [&](int i) {
                mouse(i, QEvent::MouseButtonPress, Qt::MiddleButton);
                mouse(i, QEvent::MouseButtonRelease, Qt::MiddleButton);
            };
            grid->setProperty("currentIndex", 0);
            const QString before = launcher.query();
            middleClick(2);
            check(filePreview.isOpen() && filePreview.isShowing(launcher.results()->pathAt(2))
                    && grid->property("currentIndex").toInt() == 2 && launcher.query() == before,
                "middle click did not preview its tile");
            mouse(3, QEvent::MouseMove, Qt::NoButton);
            pump(60);
            check(filePreview.isShowing(launcher.results()->pathAt(2)), "the preview followed the mouse before its moment passed");
            pump(250);
            check(filePreview.isShowing(launcher.results()->pathAt(3)) && grid->property("currentIndex").toInt() == 2,
                "hovering a tile did not show it in the open preview");
            key(Qt::Key_Left);
            check(grid->property("currentIndex").toInt() == 1 && filePreview.isShowing(launcher.results()->pathAt(1)),
                "an arrow key did not bring the preview back to the current tile");
            middleClick(1);
            check(!filePreview.isOpen(), "middle click on the shown tile did not close the preview");
            mouse(3, QEvent::MouseMove, Qt::NoButton);
            pump(250);
            check(!filePreview.isOpen(), "hovering opened a closed preview");
            say(u"preview mouse: middle click toggles, hover follows after 100 ms, arrows return to the current tile"_s);
            // Text: dragging picks across lines instead of flicking, wheel still scrolls.
            {
                QFile text(temporary.filePath(u"lines.txt"_s));
                check(text.open(QIODevice::WriteOnly), "could not create the text fixture");
                for (int i = 1; i <= 600; ++i)
                    text.write(QStringLiteral("line %1 alpha beta gamma\n").arg(i).toUtf8());
                text.close();
                filePreview.show({.path = QDir::toNativeSeparators(text.fileName()), .name = u"lines.txt"_s});
                timeout.restart();
                while (filePreview.kind() != FilePreview::Text && timeout.elapsed() < 3000)
                    pump(10);
                pump(200);
                auto* previewWindow = qobject_cast<QQuickWindow*>(filePreview.window());
                auto* lines = previewWindow ? visualNamed(previewWindow->contentItem(), u"previewTextLines"_s) : nullptr;
                auto* picker = previewWindow ? visualNamed(previewWindow->contentItem(), u"previewTextPicker"_s) : nullptr;
                check(filePreview.kind() == FilePreview::Text && lines && picker, "text preview did not show the file");
                if (lines && picker) {
                    const auto send = [&](QEvent::Type type, QPointF at, Qt::MouseButtons buttons) {
                        QMouseEvent event(type, at, previewWindow->mapToGlobal(at.toPoint()), Qt::LeftButton, buttons, {});
                        QCoreApplication::sendEvent(previewWindow, &event);
                    };
                    const QPointF origin = picker->mapToScene(QPointF(0, 0));
                    const qreal top = lines->property("contentY").toReal();
                    send(QEvent::MouseButtonPress, origin + QPointF(150, 12), Qt::LeftButton);
                    send(QEvent::MouseMove, origin + QPointF(150, 60), Qt::LeftButton);
                    send(QEvent::MouseMove, origin + QPointF(200, 100), Qt::LeftButton);
                    send(QEvent::MouseButtonRelease, origin + QPointF(200, 100), Qt::NoButton);
                    pump(50);
                    const QString picked = filePreview.selectedText();
                    check(picked.startsWith(u"line 1") || picked.contains(u"alpha"), "dragging in the text did not pick any");
                    check(picked.count(u'\n') >= 2 && picked.contains(u"line 2 "), "the pick did not run across lines");
                    check(lines->property("contentY").toReal() == top, "dragging in the text moved it");
                    const QPointF over = origin + QPointF(150, 60);
                    QWheelEvent wheel(over, previewWindow->mapToGlobal(over.toPoint()), {}, QPoint(0, -240), Qt::NoButton, {}, Qt::NoScrollPhase, false);
                    QCoreApplication::sendEvent(previewWindow, &wheel);
                    pump(50);
                    check(lines->property("contentY").toReal() > top, "the wheel did not scroll the text");
                    filePreview.close();
                    check(filePreview.selectedText().isEmpty(), "the pick stayed after the preview closed");
                }
            }
            // Drawings and layered pictures (GitHub #297): an Illustrator
            // file by its PDF part, a Photoshop one by its flattened picture,
            // drawn in the document reader's process and shown as pictures
            // (fitted, the wheel zooms). A small drawing on a large artboard
            // comes without the empty paper. --preview-file <path>: that
            // file too, for a look at a real one.
            {
                QTemporaryDir drawings; // of their own: the folder listed keeps its rows
                check(drawings.isValid(), "could not create a folder for the drawings");
                const auto showDrawn = [&](const QString& path, const char* grab) -> QSize {
                    // The launcher's selected row may take the preview back
                    // once (the preview follows it, and a row under the
                    // pointer after 100 ms): shown again then, until it stays.
                    const QString native = QDir::toNativeSeparators(path);
                    timeout.restart();
                    for (;;) {
                        if (!filePreview.isShowing(native))
                            filePreview.show({.path = native, .name = QFileInfo(path).fileName()});
                        pump(10);
                        if (timeout.elapsed() >= 15000)
                            break;
                        if (filePreview.isShowing(native) && filePreview.kind() != FilePreview::Loading) {
                            pump(300);
                            if (filePreview.isShowing(native))
                                break;
                        }
                    }
                    if (filePreview.window())
                        check(saveGrab(filePreview.window(), output.filePath(QString::fromLatin1(grab))), "could not render a drawn preview");
                    return filePreview.kind() == FilePreview::Picture ? filePreview.imageSize() : QSize();
                };
                const auto big = [](QByteArray& to, quint64 value, int size) {
                    for (int i = size - 1; i >= 0; --i)
                        to.append(static_cast<char>(value >> (8 * i)));
                };
                // 320 x 200, RGB, raw: red grows to the right, green down.
                QByteArray psd("8BPS");
                big(psd, 1, 2);
                psd.append(6, '\0');
                big(psd, 3, 2);
                big(psd, 200, 4);
                big(psd, 320, 4);
                big(psd, 8, 2);
                big(psd, 3, 2);
                big(psd, 0, 4);
                big(psd, 0, 4);
                big(psd, 0, 4);
                big(psd, 0, 2);
                for (int plane = 0; plane < 3; ++plane) {
                    for (int y = 0; y < 200; ++y) {
                        for (int x = 0; x < 320; ++x)
                            psd.append(static_cast<char>(plane == 0 ? x * 255 / 319 : plane == 1 ? y * 255 / 199 : 64));
                    }
                }
                QFile psdFile(drawings.filePath(u"layers.psd"_s));
                check(psdFile.open(QIODevice::WriteOnly) && psdFile.write(psd) == psd.size(), "could not create the .psd fixture");
                psdFile.close();
                const QSize psdSize = showDrawn(psdFile.fileName(), "preview-psd.png");
                check(psdSize == QSize(320, 200), "a .psd was not shown as its picture");
                check(filePreview.summary().contains(u"320 × 200"), "a .psd's summary lacks its size");
                // A bag 60 x 150 points in the middle of a sheet 2400 x 1700.
                const std::string content = "0.29 0.33 0.19 rg 1170 775 60 150 re f";
                const std::vector<std::string> objects {"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
                    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 2400 1700] /Contents 4 0 R >>",
                    "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream"};
                std::string pdf = "%PDF-1.4\n";
                std::vector<std::size_t> offsets;
                for (std::size_t i = 0; i < objects.size(); ++i) {
                    offsets.push_back(pdf.size());
                    pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
                }
                const std::size_t xref = pdf.size();
                pdf += "xref\n0 5\n0000000000 65535 f \n";
                for (const std::size_t o : offsets) {
                    char line[24];
                    std::snprintf(line, sizeof line, "%010zu 00000 n \n", o);
                    pdf += line;
                }
                pdf += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
                QFile aiFile(drawings.filePath(u"bag.ai"_s));
                check(aiFile.open(QIODevice::WriteOnly) && aiFile.write(pdf.data(), static_cast<qint64>(pdf.size())) > 0,
                    "could not create the .ai fixture");
                aiFile.close();
                const QSize aiSize = showDrawn(aiFile.fileName(), "preview-ai.png");
                // The bag and a little paper: tall, not the sheet's wide shape, and large.
                check(!aiSize.isEmpty() && aiSize.height() > aiSize.width() && aiSize.height() >= 1000,
                    "an .ai was not shown as its drawing without the empty artboard");
                // One that is no PDF (Illustrator 8 and before) is not a picture: it goes on as before.
                QFile oldFile(drawings.filePath(u"old.ai"_s));
                check(oldFile.open(QIODevice::WriteOnly) && oldFile.write("%!PS-Adobe-3.0\n%%Creator: Adobe Illustrator(R) 8.0\n") > 0,
                    "could not create the old .ai fixture");
                oldFile.close();
                check(showDrawn(oldFile.fileName(), "preview-ai-old.png").isEmpty() && filePreview.kind() != FilePreview::Loading,
                    "an .ai without a PDF part did not fall back");
                say(u"drawn previews: .psd %1x%2, .ai %3x%4 (sheet 2400x1700), old .ai falls back"_s.arg(psdSize.width())
                        .arg(psdSize.height()).arg(aiSize.width()).arg(aiSize.height()));
                if (const QString real = valueAfter(QCoreApplication::arguments(), u"--preview-file"); !real.isEmpty()) {
                    const QSize size = showDrawn(real, "preview-file.png");
                    say(u"--preview-file %1: kind %2, picture %3x%4, %5"_s.arg(real).arg(int(filePreview.kind())).arg(size.width())
                            .arg(size.height()).arg(filePreview.summary()));
                }
                filePreview.close();
            }
            // Colours (GitHub #291): code, data and Markdown by the kind of
            // file, the words searched for marked, the first of them in sight.
            {
                const auto showText = [&](const QString& name, const QByteArray& content, const QStringList& words) -> QQuickWindow* {
                    QFile file(temporary.filePath(name));
                    check(file.open(QIODevice::WriteOnly), "could not create a fixture for the colours");
                    file.write(content);
                    file.close();
                    filePreview.show({.path = QDir::toNativeSeparators(file.fileName()), .name = name, .words = words});
                    timeout.restart();
                    while ((filePreview.kind() != FilePreview::Text || !filePreview.isShowing(QDir::toNativeSeparators(file.fileName())))
                        && timeout.elapsed() < 3000)
                        pump(10);
                    pump(300);
                    return qobject_cast<QQuickWindow*>(filePreview.window());
                };
                const auto kindsOf = [&](int row) {
                    QList<int> kinds;
                    const QList<int> spans = filePreview.spansAt(row);
                    for (qsizetype i = 2; i < spans.size(); i += 3)
                        kinds.append(spans[i]);
                    return kinds;
                };
                QByteArray code = "// A sample: comments, keywords, strings, numbers.\n#include <vector>\n\n"
                                  "/* A block comment\n   over two lines. */\nnamespace demo {\n\n"
                                  "constexpr int kLimit = 0x40; // how many\n\n"
                                  "bool contains(const std::vector<int>& values, int wanted)\n{\n"
                                  "    for (const int value : values) {\n        if (value == wanted)\n            return true;\n    }\n"
                                  "    return false;\n}\n\nconst char* name() { return \"Win\\\"shun\"; }\n\n";
                for (int i = 0; i < 60; ++i)
                    code += "int filler" + QByteArray::number(i) + "() { return " + QByteArray::number(i) + "; }\n";
                code += "\n// The needle is down here, far below the first screen.\nint needle() { return 42; }\n} // namespace demo\n";
                QQuickWindow* shown = showText(u"sample.cpp"_s, code, {u"needle"_s});
                auto* lines = shown ? visualNamed(shown->contentItem(), u"previewTextLines"_s) : nullptr;
                check(filePreview.kind() == FilePreview::Text && lines, "the code did not show as text");
                check(kindsOf(0) == QList<int> {0} && kindsOf(7).contains(3) && kindsOf(7).contains(4) && kindsOf(7).contains(2),
                    "the code's first lines have no colours (comment; keyword, type, number)");
                check(kindsOf(4) == QList<int> {0}, "the second line of a block comment is not a comment");
                check(filePreview.firstMarkRow() > 60 && filePreview.marksAt(filePreview.firstMarkRow()).size() == 2,
                    "the word searched for was not found in the text");
                check(lines && lines->property("contentY").toReal() > lines->property("originY").toReal(),
                    "the first line with the word searched for was not brought into sight");
                if (shown)
                    check(saveGrab(shown, output.filePath(u"preview-code.png"_s)), "could not save the code's picture");
                const QByteArray markdown = QString::fromUtf8(
                    "# 使用说明\n\n这是一段**很重要**的话，也有 *轻一点* 的，还有 `inline code` 和 [链接](https://example.com)。\n\n"
                    "## 第二章 安装\n\n- 第一步：下载\n- [x] 第二步：安装\n1. 有序的一项\n\n> 引用的一句话\n\n"
                    "```powershell\n# 这不是标题\nGet-ChildItem -Recurse\n```\n\n### 更深的标题\n\n普通的一行，带 chapter 这个词。\n").toUtf8();
                shown = showText(u"sample.md"_s, markdown, {u"chapter"_s});
                check(kindsOf(0).contains(7) && kindsOf(4).contains(8) && kindsOf(13) == QList<int> {12},
                    "Markdown's headings and fenced code are not told");
                // After a long file scrolled down, the rows are this file's (rows kept for reuse told the last one's).
                lines = shown ? visualNamed(shown->contentItem(), u"previewTextLines"_s) : nullptr;
                QQuickItem* firstRow = nullptr;
                if (lines)
                    QMetaObject::invokeMethod(lines, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, firstRow), Q_ARG(int, 0));
                check(firstRow && firstRow->property("content").toString() == QString::fromUtf8("# 使用说明"),
                    "the rows show the last file's text");
                if (shown)
                    check(saveGrab(shown, output.filePath(u"preview-markdown.png"_s)), "could not save Markdown's picture");
                const QByteArray json = "{\n  \"name\": \"Win顺\",\n  \"version\": 24,\n  \"plugins\": [\"ocr\", \"rename\"],\n  \"enabled\": true,\n  \"note\": null\n}\n";
                shown = showText(u"sample.json"_s, QString::fromUtf8(json).toUtf8(), {});
                check(kindsOf(1).contains(5) && kindsOf(2).contains(2) && kindsOf(4).contains(4), "JSON's keys, numbers and constants are not told");
                check(filePreview.firstMarkRow() < 0, "words marked with none searched for");
                if (shown)
                    check(saveGrab(shown, output.filePath(u"preview-json.png"_s)), "could not save JSON's picture");
                // A plain text file: no colours of its own, the words still marked.
                shown = showText(u"plain.txt"_s, "int x = 1; // not code\nthe chapter here\n", {u"chapter"_s});
                check(kindsOf(0).isEmpty() && filePreview.firstMarkRow() == 1, "a plain text file got colours, or lost its marks");
                filePreview.close();
                say(u"preview colours: code, Markdown, JSON by the file's kind; searched words marked and scrolled to"_s);
            }
            // A file whose place does not answer (GitHub #307: the preview
            // stayed at 正在读取… for good, for every file after it too). The
            // reading thread is left to that file and the next one is read
            // on another; the window says the place may not be answering.
            {
                const auto fixture = [&](const QString& name) {
                    QFile file(temporary.filePath(name));
                    check(file.open(QIODevice::WriteOnly) && file.write("# A chapter\n\nSome text.\n") > 0, "could not create a fixture for the stalled reading");
                    file.close();
                    return QDir::toNativeSeparators(file.fileName());
                };
                const QString stuck = fixture(u"stuck.md"_s);
                const QString fine = fixture(u"fine.md"_s);
                std::atomic<bool> answer {false};
                std::atomic<int> held {0};
                const int warned = g_warnings; // the log's two lines of it, wanted here
                FilePreview::setReadProbe([&](const QString& path) {
                    if (path.compare(stuck, Qt::CaseInsensitive) != 0)
                        return;
                    ++held;
                    while (!answer.load())
                        ::Sleep(10);
                    --held;
                });
                // Shown until `done` or `ms` passed (the launcher's row may take the preview back once).
                const auto showUntil = [&](const QString& path, int ms, const std::function<bool()>& done) {
                    timeout.restart();
                    do {
                        if (!filePreview.isShowing(path))
                            filePreview.show({.path = path, .name = QFileInfo(path).fileName()});
                        pump(10);
                    } while (!(filePreview.isShowing(path) && done()) && timeout.elapsed() < ms);
                    return filePreview.isShowing(path) && done();
                };
                check(!showUntil(stuck, 2300, [&] { return filePreview.kind() != FilePreview::Loading; }) && filePreview.slow(),
                    "a file that does not read is not shown as being read");
                check(!filePreview.stalled(), "a file is said not to answer within its first seconds");
                check(showUntil(fine, 2000, [&] { return filePreview.kind() == FilePreview::Text; }),
                    "after a file that does not read, the next one is not shown");
                check(held.load() == 1, "the file that does not read was asked for again behind itself");
                // Back on it: seconds later the window says so (and the log where it stands).
                check(showUntil(stuck, 6500, [&] { return filePreview.stalled(); }) && filePreview.kind() == FilePreview::Loading,
                    "a file that does not read for seconds is not said to be one");
                if (filePreview.window())
                    check(saveGrab(filePreview.window(), output.filePath(u"preview-stalled.png"_s)), "could not save the stalled preview's picture");
                // It answers at last: shown, and nothing said of waiting any more.
                answer = true;
                check(showUntil(stuck, 2000, [&] { return filePreview.kind() == FilePreview::Text; }) && !filePreview.slow() && !filePreview.stalled(),
                    "a file that answered at last is not shown");
                timeout.restart();
                while (held.load() > 0 && timeout.elapsed() < 2000)
                    pump(10);
                FilePreview::setReadProbe({});
                // Where the reading stood is in the log, for a report: the
                // thread left to the file, and the wait of seconds (twice
                // the first where the launcher's row took the preview back).
                check(g_warnings - warned >= 2, "the log does not tell of the file that did not read");
                g_warnings = warned;
                check(showUntil(fine, 2000, [&] { return filePreview.kind() == FilePreview::Text; }), "the threads freed do not read the next file");
                filePreview.close();
                say(u"preview of a file that does not read: the next file shown on another thread, the wait said after seconds"_s);
            }
        }
        const auto click = [&](QQuickItem* item) {
            if (!item) { check(false, "click target missing"); return; }
            const QPointF at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
            QMouseEvent press(QEvent::MouseButtonPress, at, window->mapToGlobal(at.toPoint()), Qt::LeftButton, Qt::LeftButton, {});
            QMouseEvent release(QEvent::MouseButtonRelease, at, window->mapToGlobal(at.toPoint()), Qt::LeftButton, {}, {});
            QCoreApplication::sendEvent(window, &press);
            QCoreApplication::sendEvent(window, &release);
        };
        const auto pressKey = [&](int code) {
            if (code == Qt::Key_F2) { // taken by the search field in use
                auto* panelField = visualNamed(window->contentItem(), u"panelSearchField"_s);
                auto* field = panelField && panelField->isVisible() ? panelField : visualNamed(window->contentItem(), u"searchField"_s);
                if (field)
                    field->forceActiveFocus();
            }
            QKeyEvent down(QEvent::KeyPress, code, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &down);
            QKeyEvent up(QEvent::KeyRelease, code, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &up);
            pump(60);
        };
        // GitHub #335: the rename box beside the current row's name, never
        // over it (a delegate without nameTop/nameBottom is one line), and
        // inside the list.
        const auto renameBesideName = [&](QObject* list) {
            auto* box = visualNamed(window->contentItem(), u"renameBox"_s);
            auto* listItem = qobject_cast<QQuickItem*>(list);
            auto* item = list ? list->property("currentItem").value<QQuickItem*>() : nullptr;
            if (!box || !box->isVisible() || !item || !listItem)
                return false;
            const QVariant top = item->property("nameTop");
            const QVariant bottom = item->property("nameBottom");
            const qreal nameTop = item->mapToScene(QPointF(0, top.isValid() ? top.toReal() : 0)).y();
            const qreal nameBottom = item->mapToScene(QPointF(0, bottom.isValid() ? bottom.toReal() : item->height())).y();
            const qreal boxTop = box->mapToScene(QPointF(0, 0)).y();
            const qreal boxBottom = boxTop + box->height();
            const qreal listTop = listItem->mapToScene(QPointF(0, 0)).y();
            const qreal listBottom = listTop + listItem->height();
            // Where the row says where its name starts, the box's text starts there too.
            const QVariant left = item->property("nameLeft");
            const bool aligned = !left.isValid()
                || std::abs(item->mapToScene(QPointF(left.toReal(), 0)).x()
                       - box->mapToScene(QPointF(box->property("textInset").toReal(), 0)).x()) <= 1;
            // Within its row or tile across, not over the next tile.
            const qreal boxLeft = box->mapToScene(QPointF(0, 0)).x();
            const bool across = boxLeft >= item->mapToScene(QPointF(0, 0)).x() - 1
                && boxLeft + box->width() <= item->mapToScene(QPointF(item->width(), 0)).x() + 1;
            return aligned && across && (boxTop >= nameBottom - 1 || boxBottom <= nameTop + 1) && boxTop >= listTop - 1
                && boxBottom <= listBottom + 1;
        };
        grid->setProperty("currentIndex", 5);
        const QString chosen = launcher.results()->pathAt(5);
        click(visualNamed(window->contentItem(), u"fileViewList"_s));
        settle(window);
        auto* view = window->property("view").value<QObject*>();
        check(!launcher.gallery() && view && launcher.results()->pathAt(view->property("currentIndex").toInt()) == chosen,
            "switching to list lost the current file");
        // Renaming in place (GitHub #285): F2 opens a box over the row with
        // the name up to its extension picked; a name Windows does not take
        // keeps the box; Enter renames the file on the disk and the row; Esc
        // leaves both alone. Put back afterwards (the checks below go by path).
        if (view) {
            const int row = view->property("currentIndex").toInt();
            const QString was = launcher.results()->pathAt(row);
            const QFileInfo file(was);
            // The search field in use: the panel's own while the panel is open.
            auto* panelField = visualNamed(window->contentItem(), u"panelSearchField"_s);
            auto* field = panelField && panelField->isVisible() ? panelField : visualNamed(window->contentItem(), u"searchField"_s);
            const auto press = [&](int code) {
                QKeyEvent down(QEvent::KeyPress, code, Qt::NoModifier);
                QCoreApplication::sendEvent(window, &down);
                QKeyEvent up(QEvent::KeyRelease, code, Qt::NoModifier);
                QCoreApplication::sendEvent(window, &up);
                pump(60);
            };
            const auto waitFor = [&](const QString& path) {
                QElapsedTimer waited;
                waited.start();
                while (!QFileInfo::exists(path) && waited.elapsed() < 5000)
                    pump(20);
                pump(150); // the row follows on the GUI thread
                return QFileInfo::exists(path);
            };
            if (field)
                field->forceActiveFocus();
            press(Qt::Key_F2);
            auto* box = visualNamed(window->contentItem(), u"renameBox"_s);
            check(file.exists() && box && box->isVisible() && box->property("text").toString() == file.fileName()
                    && box->property("inputFocused").toBool(),
                "F2 did not open the rename box with the file's name");
            check(renameBesideName(view), "the rename box covers the name in the panel's list");
            if (box && box->isVisible())
                check(saveGrab(window, output.filePath(u"rename-box.png"_s)), "could not save the rename box's picture");
            if (box && box->isVisible()) {
                const QString renamed = file.absolutePath() + u"/renamed-"_s + file.fileName();
                box->setProperty("text", u"bad:name"_s + u'.' + file.suffix());
                press(Qt::Key_Return);
                check(box->isVisible() && file.exists() && launcher.results()->pathAt(row) == was, "a name with a colon was taken");
                box->setProperty("text", u"renamed-"_s + file.fileName());
                press(Qt::Key_Return);
                check(waitFor(renamed) && !QFileInfo::exists(was), "Enter did not rename the file on the disk");
                check(!box->isVisible() && field && window->activeFocusItem() == field,
                    "the search box did not get the keyboard back after renaming");
                check(QDir::cleanPath(launcher.results()->pathAt(row)) == QDir::cleanPath(renamed)
                        && view->property("currentIndex").toInt() == row,
                    "the row did not take the new name where it was");
                press(Qt::Key_F2);
                check(box->isVisible() && box->property("text").toString() == u"renamed-"_s + file.fileName(), "F2 on the renamed row shows its new name");
                press(Qt::Key_Escape);
                check(!box->isVisible() && QFileInfo::exists(renamed) && window->isVisible(), "Esc in the box did more than close it");
                press(Qt::Key_F2);
                box->setProperty("text", file.fileName());
                press(Qt::Key_Return);
                check(waitFor(was) && QDir::cleanPath(launcher.results()->pathAt(row)) == QDir::cleanPath(was), "renaming back did not work");
                if (box->isVisible())
                    press(Qt::Key_Escape);
                say(u"rename in place: F2, a refused name, Enter (disk and row), Esc, and back"_s);
            }
        }
        click(visualNamed(window->contentItem(), u"fileViewGallery"_s));
        settle(window);
        check(launcher.gallery() && launcher.results()->pathAt(grid->property("currentIndex").toInt()) == chosen,
            "switching back to gallery lost the current file");
        pressKey(Qt::Key_F2);
        check(renameBesideName(grid), "the rename box covers the name on a gallery tile");
        check(saveGrab(window, output.filePath(u"rename-box-gallery.png"_s)), "could not save the gallery's rename box");
        pressKey(Qt::Key_Escape);
        launcher.setResultOptions(KindOrder::FilesFirst, true, false, false, u"ctrl"_s);
        check(launcher.results()->data(launcher.results()->index(0), ResultModel::ThumbnailRole).toString().isEmpty()
            && !launcher.results()->data(launcher.results()->index(0), ResultModel::GalleryThumbnailRole).toString().isEmpty(),
            "gallery previews depended on the compact list's thumbnail preference");
        grid->setProperty("currentIndex", 10);
        grid->setProperty("contentY", 300);
        launcher.setThumbnailSize(224);
        pump(100);
        check(grid->property("currentIndex").toInt() == 10 && grid->property("contentY").toReal() > 0,
            "zoom lost the selection or jumped to the beginning");
        const QPointF at = grid->mapToScene(QPointF(100, 100));
        QWheelEvent wheel(at, window->mapToGlobal(at.toPoint()), {}, QPoint(0, 120),
            Qt::NoButton, Qt::AltModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(window, &wheel);
        check(launcher.thumbnailSize() == 240, "Alt+wheel did not resize thumbnails");
        launcher.setThumbnailSize(320);
        window->resize(640, 480);
        settle(window);
        check(saveGrab(window, output.filePath(u"files-large-icons-compact.png"_s)), "could not render minimum width");

        // The box's own list, two lines a row: under the name, over its
        // folder; on the last row, above the name.
        launcher.setGallery(false);
        launcher.setPanel(false);
        window->resize(900, 600);
        settle(window);
        auto* list = window->property("view").value<QObject*>();
        if (list) {
            for (const int row : {0, list->property("count").toInt() - 1}) {
                list->setProperty("currentIndex", row);
                settle(window);
                pressKey(Qt::Key_F2);
                check(renameBesideName(list), row == 0 ? "the rename box covers the name in the box's list"
                                                       : "the rename box covers the name on the box's last row");
                check(saveGrab(window, output.filePath(row == 0 ? u"rename-box-list.png"_s : u"rename-box-list-last.png"_s)),
                    "could not save the box list's rename box");
                pressKey(Qt::Key_Escape);
            }
        } else {
            check(false, "no list in the box");
        }
        launcher.setPanel(true);
        settle(window);
        say(u"rename box beside the name: panel list, gallery tile, the box's first and last row"_s);
    }
    launcher.setGallery(false);
    settle(window);
    launcher.results()->clear();
    launcher.setGallery(true);
    settle(window);
    check(saveGrab(window, output.filePath(u"files-large-icons-empty.png"_s)), "could not render empty gallery");
    launcher.setGallery(false);
    say(u"large icons: production QML/model, real temporary PNGs, Alt+wheel, viewport retention, minimum width"_s);
}

// --drop-pie-hit [--renderer software|d3d11]: where Windows takes the pie's
// window to be (GitHub #150). The pie is see-through round its chips, and a
// drag reaches it only where WindowFromPoint finds it: a layered window (what
// the software renderer makes of one with an alpha channel) is not found
// where its pixels are fully transparent. Shown at the middle of the primary
// screen for a moment, never active, no input sent; drop-pie-hit.json says
// whether it is layered and how many of the points over its rectangle are
// its own. Another window above it there would count as a miss too: the two
// renderers are to be compared under the same conditions.
int checkDropPieHit(const QDir& output, const QStringList& args)
{
    const QString renderer = valueAfter(args, u"--renderer", u"d3d11"_s);
    QQuickWindow::setGraphicsApi(renderer == u"software" ? QSGRendererInterface::Software : QSGRendererInterface::Direct3D11);
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    QTemporaryDir temporary;
    QQmlEngine engine;
    Favorites favorites(temporary.filePath(u"pie-favorites.json"_s));
    for (const QString& name : {u"项目资料"_s, u"年度报告"_s, u"设计素材"_s, u"客户合同"_s, u"归档备份"_s}) {
        const QString folder = temporary.filePath(name);
        QDir().mkpath(folder);
        favorites.add({Favorite::Kind::Folder, QDir::toNativeSeparators(folder), name});
    }
    piedrop::FileOperations operations;
    DropPie pie(&favorites, nullptr, &operations, {});
    QQmlComponent component(&engine, u"WinShun"_s, u"DropPieWindow"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"dropPie"_s, QVariant::fromValue(&pie)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) {
        qWarning().noquote() << u"drop pie hit: no window: "_s + component.errorString();
        return 4;
    }
    pie.setWindow(window);
    QElapsedTimer waiting;
    waiting.start();
    while (pie.sectors().size() < 5 && waiting.elapsed() < 5000)
        pump(20);
    QScreen* primary = QGuiApplication::primaryScreen();
    const qreal scale = primary->devicePixelRatio();
    const QPoint middle = primary->geometry().center();
    // The window takes real drops while it is up, into folders that go with
    // this run: never shown while a button is held (a drag of the user's
    // could be under way).
    for (const int button : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON}) {
        if (::GetAsyncKeyState(button) & 0x8000) {
            qWarning("drop pie hit: a mouse button is held; not shown");
            return 5;
        }
    }
    const POINT at {LONG(std::lround(middle.x() * scale)), LONG(std::lround(middle.y() * scale))};
    // --unrevealed: the window as a summons leaves it until a drag comes
    // over it, shown with nothing of it drawn yet (DropPieWindow's reveal at
    // 0). Not through the summons itself: that waits for the drag where the
    // pointer is, and hides the window 150 ms on when the pointer has moved
    // (a person at the PC: the measure was of a hidden window, twice). Put
    // at the middle of the screen as it would lie there, and shown; the
    // pointer is not asked about, nor touched.
    const bool unrevealed = args.contains(u"--unrevealed"_s);
    if (unrevealed) {
        // Never opened, the pie is not revealed and takes no drop
        // (DropPie::dragEnter). About the size it has revealed.
        const QSize size(360, 240);
        window->setGeometry(QRect(middle - QPoint(size.width() / 2, size.height() / 2), size));
    } else {
        pie.preview(at, -1);
    }
    window->show();
    window->raise();
    pump(600); // its reveal if any, and the frame that carries it
    const bool shownStill = window->isVisible(); // hidden meanwhile, the answer would be about nothing
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    RECT rect {};
    ::GetWindowRect(hwnd, &rect);
    const auto ours = [hwnd](POINT pt) {
        const HWND under = ::WindowFromPoint(pt);
        return under && ::GetAncestor(under, GA_ROOT) == hwnd;
    };
    int points = 0;
    int hits = 0;
    for (LONG y = rect.top + 3; y < rect.bottom; y += 6) {
        for (LONG x = rect.left + 3; x < rect.right; x += 6) {
            ++points;
            hits += ours({x, y}) ? 1 : 0;
        }
    }
    // Unrevealed, it has no middle laid out (and nothing drawn to tell one
    // point from another): the middle of its rectangle.
    const POINT centre = unrevealed ? POINT {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2}
        : POINT {rect.left + LONG(std::lround(pie.centre().x() * scale)), rect.top + LONG(std::lround(pie.centre().y() * scale))};
    const POINT besideHub {centre.x + LONG(std::lround((pie.hubRadius() + 6) * scale)), centre.y};
    // What the window draws with, as it says itself: asked for is not had.
    const QSGRendererInterface* drawing = window->rendererInterface();
    const auto api = drawing ? drawing->graphicsApi() : QSGRendererInterface::Unknown;
    const QJsonObject report {
        {u"renderer"_s, renderer},
        {u"renderer_in_use"_s, api == QSGRendererInterface::Software ? u"software"_s
                               : api == QSGRendererInterface::Direct3D11 ? u"d3d11"_s : u"other (%1)"_s.arg(int(api))},
        {u"layered"_s, bool(::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED)},
        {u"state"_s, unrevealed ? u"unrevealed"_s : u"revealed"_s},
        {u"revealed"_s, pie.revealed()},
        {u"visible_when_measured"_s, shownStill && window->isVisible()},
        {u"window"_s, QJsonArray {int(rect.left), int(rect.top), int(rect.right - rect.left), int(rect.bottom - rect.top)}},
        {u"scale"_s, scale},
        {u"sectors"_s, int(pie.sectors().size())},
        {u"points"_s, points},
        {u"points_ours"_s, hits},
        {u"centre_ours"_s, ours(centre)},
        {u"beside_hub_ours"_s, ours(besideHub)},
    };
    window->hide();
    // All of it its own, in the state asked for and seen until measured, by
    // the renderer asked for: anything less and a drag may miss it. A
    // window of another program above it there fails this too (run again).
    const bool measured = report.value(u"visible_when_measured"_s).toBool() && pie.sectors().size() >= 5
        && report.value(u"renderer_in_use"_s).toString() == renderer && report.value(u"revealed"_s).toBool() != unrevealed;
    const bool passed = measured && hits == points && points > 0 && report.value(u"centre_ours"_s).toBool();
    QJsonObject written = report;
    written.insert(u"measured"_s, measured);
    written.insert(u"passed"_s, passed);
    QFile file(output.filePath(u"drop-pie-hit.json"_s));
    if (file.open(QIODevice::WriteOnly))
        file.write(QJsonDocument(written).toJson());
    say(u"drop pie hit (%1): layered %2, %3 of %4 points its own, centre %5, beside the hub %6"_s.arg(renderer)
            .arg(report.value(u"layered"_s).toBool() ? u"yes"_s : u"no"_s).arg(hits).arg(points)
            // From the report: asked again here, the window hidden by now, every point is another's.
            .arg(report.value(u"centre_ours"_s).toBool() ? u"yes"_s : u"no"_s,
                report.value(u"beside_hub_ours"_s).toBool() ? u"yes"_s : u"no"_s));
    say(!measured ? u"drop pie hit: NOT MEASURED (hidden meanwhile, not the state or the renderer asked for)"_s
            : passed ? u"drop pie hit: passed"_s : u"drop pie hit: FAILED"_s);
    return passed ? 0 : measured ? 4 : 6;
}

void checkDropPieHelper(const QDir& output)
{
    // Elevated, the helper gets the desktop user's environment, not this run's
    // PATH to Qt: from a build folder it cannot even load.
    if (integrity::processElevated()) {
        say(u"drop pie: skipped, elevated (run it with normal rights)"_s);
        return;
    }
    QTemporaryDir profile;
    Favorites favorites(profile.filePath(u"favorites.json"_s));
    favorites.load();
    for (int i = 0; i < 2; ++i) {
        const QString folder = profile.filePath(u"folder-%1"_s.arg(i));
        QDir().mkpath(folder);
        favorites.add({Favorite::Kind::Folder, folder, u"Folder %1"_s.arg(i)});
    }
    QStringList notices;
    std::function<void()> clickRetry; // the last notification's 重试
    // Each helper started says it runs (GitHub #150): its lines reach this
    // process's log, one warning each, counted below.
    const int before = g_warnings;
    qputenv("WINSHUN_LOG_INFO", "1");
    auto helper = std::make_unique<DropPieHelper>(&favorites,
        [&](const QString& title, const QString& text, std::function<void()> retry) {
            notices.push_back(title + u": "_s + text);
            clickRetry = std::move(retry);
        });
    Settings settings = Settings::defaults();
    helper->update(settings);
    const auto waitFor = [&](const std::function<bool(const QJsonObject&)>& wanted, int ms = 6000) {
        QElapsedTimer waiting;
        waiting.start();
        while (waiting.elapsed() < ms) {
            const auto state = helper->state();
            if (state.value(u"ready"_s).toBool() && wanted(state)) return true;
            pump(20);
        }
        return false;
    };
    const auto folders = [](int count) { return [count](const QJsonObject& state) { return state.value(u"folders"_s).toInt() == count; }; };
    bool passed = waitFor(folders(2));
    QJsonObject state = helper->state();
    passed = passed && !state.value(u"elevated"_s).toBool(true);
    const QString folder = profile.filePath(u"third-folder"_s);
    QDir().mkpath(folder);
    favorites.add({Favorite::Kind::Folder, folder, u"Third folder"_s});
    const bool updatedFavorites = waitFor(folders(3));
    // The appearance reaches it too (its ini where Settings::useProfile has it).
    settings.uiSize = u"compact"_s;
    settings.theme = u"dark"_s;
    helper->update(settings);
    const bool updatedAppearance = waitFor([](const QJsonObject& state) {
        return state.value(u"folders"_s).toInt() == 3 && state.value(u"theme"_s).toString() == u"dark";
    });
    // Ended from outside (or crashed): started again by itself (GitHub #172).
    // Its two log lines (the exit, then running again) are expected here.
    const qint64 first = helper->state().value(u"pid"_s).toInteger();
    QElapsedTimer restarting;
    restarting.start();
    if (win32::UniqueHandle killed(::OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(first))); killed.valid())
        ::TerminateProcess(killed.get(), 1);
    const bool restarted = waitFor([first](const QJsonObject& state) {
        return state.value(u"pid"_s).toInteger() != first && state.value(u"folders"_s).toInt() == 3;
    }, 12000); // a second after it ended, then a start as slow as the first may be
    const qint64 restartMs = restarting.elapsed();
    // Ended twice more, the third time in a row: told, with 重试 to click.
    // The notification outlives the helper (turned off, Win顺 no longer
    // elevated): clicked then, it does nothing. Three more log lines. Told
    // only with the desktop shell up a minute: not checked right after it started.
    const bool noticeChecked = [] {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(::GetShellWindow(), &pid);
        const win32::UniqueHandle shell(pid ? ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr);
        FILETIME created {}, exited {}, kernel {}, user {}, now {};
        if (!shell.valid() || !::GetProcessTimes(shell.get(), &created, &exited, &kernel, &user))
            return false;
        ::GetSystemTimeAsFileTime(&now);
        const auto ticks = [](const FILETIME& time) { return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
        return ticks(now) - ticks(created) > 65 * 10'000'000ull; // 100 ns ticks
    }();
    bool toldOnce = true;
    for (int failures = 2; failures <= 3; ++failures) {
        const qint64 pid = helper->state().value(u"pid"_s).toInteger();
        if (win32::UniqueHandle killed(::OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(pid))); killed.valid())
            ::TerminateProcess(killed.get(), 1);
        toldOnce = waitFor([pid](const QJsonObject& state) {
            return state.value(u"pid"_s).toInteger() != pid && state.value(u"folders"_s).toInt() == 3;
        }, 15000) && toldOnce && (!noticeChecked || notices.size() == failures - 2);
    }
    toldOnce = toldOnce && (!noticeChecked || (notices.size() == 1 && clickRetry));
    // Four started (the first, again after each of three ends): four lines,
    // the last one's a poll after it was ready.
    pump(600);
    state = helper->state();
    const int relayed = state.value(u"relayed_lines"_s).toInt();
    const bool relayedAll = relayed == 4;
    qunsetenv("WINSHUN_LOG_INFO");
    g_warnings = before + std::max(0, g_warnings - before - 5 - relayed);
    win32::UniqueHandle child(::OpenProcess(SYNCHRONIZE, FALSE, DWORD(state.value(u"pid"_s).toInteger())));
    QElapsedTimer stopping;
    stopping.start();
    helper.reset();
    const bool stopped = child.valid() && ::WaitForSingleObject(child.get(), 10'000) == WAIT_OBJECT_0;
    state.insert(u"stop_ms"_s, stopping.elapsed());
    if (clickRetry)
        clickRetry(); // the helper gone: nothing (it once was a use after free)
    passed = passed && updatedFavorites && updatedAppearance && restarted && toldOnce && stopped && relayedAll;
    state.insert(u"relayedAll"_s, relayedAll);
    state.insert(u"updatedFavorites"_s, updatedFavorites);
    state.insert(u"updatedAppearance"_s, updatedAppearance);
    state.insert(u"restarted"_s, restarted);
    state.insert(u"restart_ms"_s, restartMs);
    state.insert(u"toldOnce"_s, toldOnce);
    state.insert(u"notice_checked"_s, noticeChecked);
    state.insert(u"stopped"_s, stopped);
    state.insert(u"passed"_s, passed);
    QFile report(output.filePath(u"drop-pie-helper.json"_s));
    if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(state).toJson());
    if (!passed) qWarning().noquote() << "Drop pie helper regression failed:" << notices << state;
    else say(u"drop pie: real ordinary-user helper, OLE target registration, favorite and appearance refresh, restart after it ends, the notice after three failures (its retry harmless once the helper is gone), its log lines relayed and shutdown checked"_s);
}

// --scope-memory: the scope the user chose comes back at the next start
// (GitHub #263). The real Launcher over an empty index and a state.ini of
// this run; a "start" is a new Launcher given what App gives it, in App's
// order (the state file, the kinds and the options, the strip's choice, then
// restoreScope). What only passes through a scope must not be kept: a prefix
// typed, the scope set from inside (as a plugin's row or a hint sets it).
bool checkScopeMemory(QQuickWindow* window, const QDir& output)
{
    QTemporaryDir temporary;
    IndexService index(temporary.path(), IndexService::Options {});
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    History history(temporary.filePath(u"history.tsv"_s));
    SearchEngine engine(&index, &apps, &places);
    const QString stateFile = temporary.filePath(u"state.ini"_s);
    Settings settings;
    settings.fileTypes = Settings::defaultFileTypes();
    bool passed = true;
    QStringList failures;
    QString typedChoiceKept; // Search/FileType while a prefix was typed: reported, see there
    const auto check = [&](bool condition, const QString& message) {
        if (condition)
            return;
        passed = false;
        failures.append(message);
        qWarning().noquote() << u"scope memory: "_s + message;
    };
    const auto kept = [&] { return QSettings(stateFile, QSettings::IniFormat).value(u"Search/Scope"_s).toString(); };
    struct Options {
        bool content = true;
        bool merged = false;
    };
    // One run of the program: `body` is what the user does in it.
    const auto start = [&](const Options& options, const std::function<void(Launcher&)>& body) {
        Launcher launcher(&index, &apps, &places, &engine, &history);
        launcher.setWindow(window);
        launcher.setSwitches(nullptr, stateFile);
        launcher.setMergeFileScope(options.merged);
        launcher.setContentOptions(options.content, {u"txt"_s}, ContentSizeLimits {}, false, false);
        launcher.setFileTypes(settings.namedFileTypes());
        // The strip's choice is App's to keep (Search/FileType): as App does.
        launcher.setFileChoice(QSettings(stateFile, QSettings::IniFormat).value(u"Search/FileType"_s).toString());
        QObject::connect(&launcher, &Launcher::fileChoiceChanged, &launcher, [&launcher, &stateFile] {
            QSettings(stateFile, QSettings::IniFormat).setValue(u"Search/FileType"_s, launcher.fileChoice());
        });
        launcher.restoreScope();
        body(launcher);
        QElapsedTimer settling; // a search under way ends before its launcher goes
        settling.start();
        while (launcher.searching() && settling.elapsed() < 2000)
            pump(10);
        check(!launcher.searching(), u"a search did not settle before the run ended"_s);
        launcher.setSwitches(nullptr, {});
    };

    start({}, [&](Launcher& launcher) {
        check(launcher.scope() == Launcher::All && kept().isEmpty(), u"a first start is not in 全部 with nothing kept"_s);
        launcher.chooseScope(Launcher::Files);
        check(kept() == u"files", u"文件 chosen by its tab was not kept"_s);
    });
    start({}, [&](Launcher& launcher) {
        check(launcher.scope() == Launcher::Files, u"文件 did not come back"_s);
        launcher.chooseScope(Launcher::Content);
        check(kept() == u"content", u"内容 chosen by its tab was not kept"_s);
    });
    start({}, [&](Launcher& launcher) {
        check(launcher.scope() == Launcher::Content, u"内容 did not come back"_s);
        launcher.chooseScope(Launcher::All);
        check(kept() == u"all", u"全部 chosen by its tab was not kept"_s);
        // What only passes: set from inside, and a prefix typed and taken away.
        launcher.setScope(Launcher::Content);
        check(launcher.scope() == Launcher::Content, u"a scope set from inside did not take"_s);
        check(kept() == u"all", u"after a scope set from inside, what is kept is \"%1\", not the user's 全部"_s.arg(kept()));
        launcher.setScope(Launcher::All);
        launcher.setQuery(u":pictures"_s);
        check(launcher.scope() == Launcher::Files, u"the prefix typed did not take the scope to 文件"_s);
        check(kept() == u"all", u"while a prefix is typed, what is kept is \"%1\", not the user's 全部"_s.arg(kept()));
        // The strip shows the prefix's kind while it is typed; what is kept
        // of the strip stays the user's own (不限 here), or a quit with the
        // prefix still typed leaves the strip on that kind at the next start.
        typedChoiceKept = QSettings(stateFile, QSettings::IniFormat).value(u"Search/FileType"_s).toString();
        check(launcher.selectedFileTypes().size() == 1, u"the prefix typed did not show its kind on the strip"_s);
        check(typedChoiceKept.isEmpty(),
            u"while a prefix is typed, the strip's choice kept is \"%1\", not the user's 不限"_s.arg(typedChoiceKept));
        launcher.setQuery({});
        check(launcher.scope() == Launcher::All, u"the prefix gone, the scope is not 全部 again"_s);
        check(kept() == u"all", u"the prefix gone, what is kept is \"%1\", not the user's 全部"_s.arg(kept()));
        check(QSettings(stateFile, QSettings::IniFormat).value(u"Search/FileType"_s).toString().isEmpty(),
            u"the prefix gone, the strip's choice kept is not 不限 again"_s);
    });
    start({}, [&](Launcher& launcher) {
        check(launcher.scope() == Launcher::All, u"全部 did not come back after scopes that only passed"_s);
        // As a tab writes it: the property, through its WRITE (QML's launcher.scope = …).
        check(launcher.setProperty("scope", QVariant::fromValue(Launcher::Files)), u"the scope property could not be written"_s);
        check(launcher.scope() == Launcher::Files && kept() == u"files",
            u"the scope written as a tab writes it was not kept (what is kept is \"%1\")"_s.arg(kept()));
        launcher.chooseScope(Launcher::All);
        // A chip of 内容's strip clicked while the strip is on its way out
        // (TypeStrip keeps the departing chips a moment), the one without a
        // folder: into 内容 by it, the user's own.
        launcher.chooseFolderChip({});
        check(launcher.scope() == Launcher::Content && kept() == u"content",
            u"into 内容 by a chip of its strip was not kept (what is kept is \"%1\")"_s.arg(kept()));
        launcher.chooseScope(Launcher::All);
        // Tab goes 全部 → 文件 → 内容, each the user's.
        launcher.cycleScope(1);
        check(launcher.scope() == Launcher::Files && kept() == u"files", u"Tab into 文件 was not kept"_s);
        launcher.cycleScope(1);
        check(launcher.scope() == Launcher::Content && kept() == u"content", u"Tab into 内容 was not kept"_s);
        // A chip of the strip takes it to 文件夹, kept under 文件's tab.
        launcher.chooseFileType(Launcher::kFoldersChip);
        check(launcher.scope() == Launcher::Folders && kept() == u"files", u"the 文件夹 chip was not kept under 文件"_s);
    });
    start({}, [&](Launcher& launcher) {
        check(launcher.scope() == Launcher::Folders, u"文件夹 did not come back (the strip's choice with the scope)"_s);
        launcher.chooseFileType(-1); // 不限: 文件
        launcher.toggleFileType(1);
        check(launcher.scope() == Launcher::Files && launcher.selectedFileTypes() == (QList<int> {1}) && kept() == u"files",
            u"a kind's chip was not kept as 文件"_s);
    });
    start({}, [&](Launcher& launcher) {
        check(launcher.scope() == Launcher::Files && launcher.selectedFileTypes() == (QList<int> {1}),
            u"文件 with its kind did not come back"_s);
    });
    // Where the tabs have no 文件 (全部 and 内容 only): 文件 with a kind stays, without one it is 全部.
    start({.merged = true}, [&](Launcher& launcher) {
        check(launcher.scope() == Launcher::Files, u"merged tabs: 文件 with a kind chosen did not come back"_s);
        launcher.chooseFileType(-1);
        check(launcher.scope() == Launcher::All, u"merged tabs: 不限 did not go to 全部"_s);
    });
    QSettings(stateFile, QSettings::IniFormat).setValue(u"Search/Scope"_s, u"files"_s); // as left by a run with the three tabs
    start({.merged = true}, [&](Launcher& launcher) {
        check(launcher.scope() == Launcher::All, u"merged tabs: 文件 without a kind came back, where there is no such tab"_s);
    });
    // 内容 kept, then content search turned off in the settings: 全部.
    QSettings(stateFile, QSettings::IniFormat).setValue(u"Search/Scope"_s, u"content"_s);
    start({.content = false}, [&](Launcher& launcher) {
        check(launcher.scope() == Launcher::All && kept() == u"content", u"内容 came back with content search off (or what is kept changed)"_s);
    });
    start({}, [&](Launcher& launcher) {
        check(launcher.scope() == Launcher::Content, u"内容 did not come back once content search is on again"_s);
    });
    // A prefix typed, then a choice made: the prefix's kind is the user's own
    // from there on, kept as the strip shows it.
    QFile::remove(stateFile);
    QList<int> prefixKind;
    start({}, [&](Launcher& launcher) {
        launcher.setQuery(u":pictures"_s);
        prefixKind = launcher.selectedFileTypes();
        check(prefixKind.size() == 1, u"the prefix typed did not show one kind"_s);
        launcher.chooseFileType(prefixKind.value(0)); // the chip the prefix shows, clicked
        const QString keptChoice = QSettings(stateFile, QSettings::IniFormat).value(u"Search/FileType"_s).toString();
        check(launcher.query().isEmpty() && !keptChoice.isEmpty() && keptChoice == launcher.fileChoice() && kept() == u"files",
            u"the prefix's chip clicked: kept \"%1\" in \"%2\", the strip shows \"%3\""_s.arg(keptChoice, kept(), launcher.fileChoice()));
    });
    start({}, [&](Launcher& launcher) {
        check(launcher.scope() == Launcher::Files && launcher.selectedFileTypes() == prefixKind,
            u"the kind chosen while its prefix was typed did not come back"_s);
        launcher.chooseFileType(-1);
        launcher.chooseScope(Launcher::All);
        launcher.setQuery(u":pictures"_s);
        launcher.chooseScope(Launcher::All); // a tab clicked: the prefix goes, its kind stays on the strip
        const QString keptChoice = QSettings(stateFile, QSettings::IniFormat).value(u"Search/FileType"_s).toString();
        check(keptChoice == launcher.fileChoice(),
            u"a tab clicked while a prefix is typed: kept \"%1\", the strip shows \"%2\""_s.arg(keptChoice, launcher.fileChoice()));
    });
    // The strip's choice read at a start is not a choice of scope: nothing kept by it.
    QFile::remove(stateFile);
    QSettings(stateFile, QSettings::IniFormat).setValue(u"Search/FileType"_s, u"pictures"_s);
    start({}, [&](Launcher& launcher) {
        check(launcher.scope() == Launcher::All && kept().isEmpty(), u"reading the strip's choice at a start kept a scope"_s);
    });

    QFile report(output.filePath(u"scope-memory.json"_s));
    if (report.open(QIODevice::WriteOnly))
        report.write(QJsonDocument(QJsonObject {{u"passed"_s, passed}, {u"failures"_s, QJsonArray::fromStringList(failures)},
            {u"file_type_kept_while_prefix_typed"_s, typedChoiceKept}}).toJson());
    say(passed ? u"scope memory: the scope chosen by tab, Tab or chip comes back at the next start; what only passes is not kept"_s
               : u"scope memory: FAILED (%1)"_s.arg(failures.join(u"; "_s)));
    return passed;
}

// The footer's word about pinning (Launcher::tellPinHint): a result opened,
// the box up again within the minute with the same words. Real Launcher,
// SearchEngine and index over a temporary folder; what is opened is a folder
// handed to a folder opener of the check's own, so nothing opens for real.
bool checkPinHint(QQuickWindow* window, const QDir& output)
{
    QTemporaryDir temporary;
    IndexService index(temporary.path(), IndexService::Options {});
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    History history(temporary.filePath(u"history.tsv"_s));
    SearchEngine engine(&index, &apps, &places);
    const QString stateFile = temporary.filePath(u"state.ini"_s);
    bool passed = true;
    QStringList failures;
    const auto check = [&](bool condition, const QString& message) {
        if (condition)
            return;
        passed = false;
        failures.append(message);
        qWarning().noquote() << u"pin hint: "_s + message;
    };
    const QStringList names {u"zqpinhint 合同"_s, u"zqpinhint 报价"_s, u"zqother 图纸"_s};
    {
        const auto files = index.index();
        const auto lock = files->writeLock();
        const auto root = files->addRoot(QDir::toNativeSeparators(temporary.path()).toStdString());
        for (const QString& name : names) {
            check(QDir(temporary.path()).mkdir(name), u"could not make the folder "_s + name);
            files->add(root, name.toStdString(), EntryFlag::Directory);
        }
    }
    const auto told = [&] { return QSettings(stateFile, QSettings::IniFormat).value(u"Hints/PinAfterOpen"_s).toInt(); };
    QString shownHint;
    // One run of the program: `body` is what the user does in it.
    const auto start = [&](const std::function<void(Launcher&)>& body) {
        Launcher launcher(&index, &apps, &places, &engine, &history);
        launcher.setWindow(window);
        launcher.setSwitches(nullptr, stateFile);
        launcher.setContentOptions(false, {u"txt"_s}, ContentSizeLimits {}, false, false);
        body(launcher);
        QElapsedTimer settling;
        settling.start();
        while (launcher.searching() && settling.elapsed() < 2000)
            pump(10);
        launcher.setSwitches(nullptr, {});
    };
    const auto type = [&](Launcher& launcher, const QString& words, int rows) {
        launcher.setQuery(words);
        QElapsedTimer timeout;
        timeout.start();
        while ((launcher.searching() || launcher.results()->count() != rows) && timeout.elapsed() < 3000)
            pump(10);
        check(launcher.results()->count() == rows, u"“%1” found %2 rows, not %3"_s.arg(words).arg(launcher.results()->count()).arg(rows));
    };
    // Enter on the first row, the box hidden for it (as App hides it, unless
    // pinned), and up again: what the footer says then.
    // `revealed`: as App shows it, the same rows on the screen at once and
    // searched again only after (finishShownRefresh), which is when it is said.
    const auto openAndReturn = [&](Launcher& launcher, const QString& wordsOnReturn = {}, bool revealed = false) {
        int opened = 0, dismissed = 0;
        launcher.setFolderOpener([&](const QString&) { ++opened; return true; });
        const auto connection = QObject::connect(&launcher, &Launcher::dismissRequested, &launcher, [&] { ++dismissed; });
        launcher.trigger(0, Launcher::Open);
        QObject::disconnect(connection);
        launcher.setFolderOpener({});
        check(opened == 1 && dismissed == 1, u"Enter did not open the folder once and ask to be hidden"_s);
        if (!launcher.pinned())
            launcher.handleHidden();
        if (!wordsOnReturn.isEmpty())
            launcher.setQuery(wordsOnReturn);
        launcher.handleShown(revealed);
        if (revealed) {
            check(!launcher.statusText().contains(u"Ctrl+T"), u"said before the search that would have cleared it"_s);
            launcher.finishShownRefresh();
        }
        return launcher.statusText();
    };
    const auto isHint = [](const QString& status) { return status.contains(u"Ctrl+T"); };

    start([&](Launcher& launcher) {
        type(launcher, u"zqpinhint"_s, 2);
        check(!isHint(launcher.statusText()) && told() == 0, u"said before anything was opened"_s);
        shownHint = openAndReturn(launcher);
        check(isHint(shownHint) && told() == 1, u"opened and back with the same words: not said (“%1”, told %2)"_s.arg(shownHint).arg(told()));
        // Typing takes it away, as any word in the footer.
        type(launcher, u"zqpinhint 合"_s, 1);
        check(!isHint(launcher.statusText()), u"it stayed while typing"_s);
        // Back with other words: looking for something else, not said.
        const QString other = openAndReturn(launcher, u"zqother"_s);
        check(!isHint(other) && told() == 1, u"said though the words had changed"_s);
    });
    // The next run: once more, and then never again.
    start([&](Launcher& launcher) {
        type(launcher, u"zqpinhint"_s, 2);
        check(isHint(openAndReturn(launcher, {}, true)) && told() == 2, u"not said a second time (the box revealed first, searched after)"_s);
        check(!isHint(openAndReturn(launcher)) && told() == 2, u"said a third time"_s);
    });
    // Pinned: nothing is hidden, nothing to say; and who has pinned once knows.
    QFile::remove(stateFile);
    start([&](Launcher& launcher) {
        type(launcher, u"zqpinhint"_s, 2);
        launcher.setPinned(true);
        check(!isHint(openAndReturn(launcher)), u"said while pinned"_s);
        launcher.setPinned(false);
        check(!isHint(openAndReturn(launcher)) && told() == 2, u"said to someone who has pinned before"_s);
    });
    // Pinned, a result opened and the box back in front: the list as it was
    // left (the row, the order, no search), for the next result. With nothing
    // typed, the start list shown anew.
    start([&](Launcher& launcher) {
        type(launcher, u"zqpinhint"_s, 2);
        launcher.setPinned(true);
        launcher.setCurrentRow(1);
        const QString second = launcher.results()->at(1)->path;
        int opened = 0, replaced = 0, forward = 0, shown = 0;
        launcher.setFolderOpener([&](const QString&) { ++opened; return true; });
        launcher.trigger(1, Launcher::Open);
        launcher.setFolderOpener({});
        const auto c1 = QObject::connect(&launcher, &Launcher::resultsReplaced, &launcher, [&] { ++replaced; });
        const auto c2 = QObject::connect(&launcher, &Launcher::broughtForward, &launcher, [&] { ++forward; });
        const auto c3 = QObject::connect(&launcher, &Launcher::shown, &launcher, [&] { ++shown; });
        launcher.handleBroughtForward();
        pump(300);
        check(opened == 1 && forward == 1 && shown == 0, u"pinned and back in front: not told as such (%1 %2 %3)"_s.arg(opened).arg(forward).arg(shown));
        check(!launcher.searching() && replaced == 0, u"pinned and back in front: searched again"_s);
        check(launcher.currentRow() == 1 && launcher.results()->count() == 2 && launcher.results()->at(1)->path == second,
            u"pinned and back in front: the row or the order changed"_s);
        // 已打开: the row opened, not the other; kept as the same words are
        // searched again, forgotten with other words.
        const QString first = launcher.results()->at(0)->path;
        const auto tagged = [&](int row) { return launcher.results()->data(launcher.results()->index(row), ResultModel::OpenedRole).toBool(); };
        check(tagged(1) && !tagged(0), u"opened: the row opened is not the one tagged"_s);
        launcher.handleShown();
        pump(300);
        check(launcher.results()->opened(second) && !launcher.results()->opened(first), u"opened: lost as the same words were searched again"_s);
        launcher.setQuery({});
        pump(300);
        check(!launcher.results()->opened(second), u"opened: kept with other words"_s);
        launcher.setQuery(u"zqpinhint"_s);
        pump(300);
        check(!launcher.results()->opened(second), u"opened: back with the words typed again"_s);
        launcher.setQuery({});
        pump(300);
        forward = shown = 0;
        launcher.handleBroughtForward();
        pump(100);
        check(shown == 1 && forward == 0, u"nothing typed: the start list was not shown anew"_s);
        QObject::disconnect(c1);
        QObject::disconnect(c2);
        QObject::disconnect(c3);
        launcher.setPinned(false);
    });

    QFile report(output.filePath(u"pin-hint.json"_s));
    if (report.open(QIODevice::WriteOnly))
        report.write(QJsonDocument(QJsonObject {{u"passed"_s, passed}, {u"failures"_s, QJsonArray::fromStringList(failures)},
            {u"hint"_s, shownHint}}).toJson());
    say(passed ? u"pin hint: said when a result was opened and the box came back with the same words, twice at most, never once pinned (“%1”)"_s.arg(shownHint)
               : u"pin hint: FAILED (%1)"_s.arg(failures.join(u"; "_s)));
    return passed;
}

bool checkSwitchProvider(QQuickWindow* window, const QDir& output)
{
    // Real Launcher, SearchEngine and ResultModel with asynchronous state;
    // no App, clipboard listener or native system writes are started.
    QTemporaryDir temporary;
    IndexService index(temporary.path(), IndexService::Options {});
    AppCatalog apps;
    apps.setApps(std::make_shared<const AppList>());
    SystemCatalog places;
    PlaceInfo awake;
    awake.name = u"保持唤醒"_s;
    awake.key = u"winshun/switch-keep-awake"_s;
    awake.command = u"ms-settings:powersleep"_s;
    awake.kind = AppKind::Setting;
    awake.switchId = u"keep-awake"_s;
    awake.prepare();
    places.setPlaces(std::make_shared<const PlaceList>(PlaceList {awake}));
    History history(temporary.filePath(u"history.tsv"_s));
    SearchEngine engine(&index, &apps, &places);
    Launcher launcher(&index, &apps, &places, &engine, &history);
    launcher.setWindow(window);
    SwitchState state;
    state.availability = SwitchState::Availability::Ready;
    state.on = SwitchState::Value::Off;
    Switches::Remote remote;
    remote.ids = {u"keep-awake"_s};
    remote.read = [&](const QString&, std::function<void(SwitchState)> done) {
        QTimer::singleShot(0, &launcher, [done = std::move(done), state] { done(state); });
    };
    remote.write = [](const QString&, SwitchIntent, std::function<void(SwitchWrite)>) {};
    Switches cache(std::move(remote));
    launcher.setSwitches(&cache, temporary.filePath(u"state.ini"_s));
    const auto detach = qScopeGuard([&] { launcher.setSwitches(nullptr, {}); });
    const auto waitFor = [&](const auto& ready) {
        QElapsedTimer waiting;
        waiting.start();
        while (!ready() && waiting.elapsed() < 5000)
            pump(10);
        return ready();
    };
    const auto hasStatus = [&] {
        for (int row = 0; row < launcher.results()->count(); ++row)
            if (launcher.results()->at(row)->statusRow)
                return true;
        return false;
    };
    QJsonObject report;
    launcher.setQuery(awake.name);
    bool passed = waitFor([&] { return launcher.results()->count() == 1 && !launcher.busy(); });
    passed = passed && launcher.results()->data(launcher.results()->index(0), ResultModel::SwitchReadyRole).toBool();
    launcher.setSwitches(nullptr, temporary.filePath(u"state.ini"_s));
    const bool detached = !launcher.results()->data(launcher.results()->index(0), ResultModel::SwitchReadyRole).toBool();
    report.insert(u"detached_row_updated"_s, detached);
    passed = detached && passed;
    launcher.setSwitches(&cache, temporary.filePath(u"state.ini"_s));
    launcher.setQuery({});
    passed = waitFor([&] { return !launcher.busy() && !hasStatus(); }) && passed;
    state.on = SwitchState::Value::On;
    cache.refresh(u"keep-awake");
    const bool appeared = waitFor(hasStatus);
    report.insert(u"async_status_appeared"_s, appeared);
    passed = appeared && passed;
    state.on = SwitchState::Value::Unknown;
    state.availability = SwitchState::Availability::Blocked;
    cache.refresh(u"keep-awake");
    const bool removed = waitFor([&] { return !hasStatus() && !launcher.busy(); });
    report.insert(u"crash_status_removed"_s, removed);
    passed = removed && passed;
    state.on = SwitchState::Value::On;
    state.availability = SwitchState::Availability::Ready;
    cache.refresh(u"keep-awake");
    passed = waitFor(hasStatus) && passed;
    launcher.setSwitches(nullptr, temporary.filePath(u"state.ini"_s));
    const bool disabled = waitFor([&] { return !hasStatus() && !launcher.busy(); });
    report.insert(u"disabled_status_removed"_s, disabled);
    passed = disabled && passed;
    report.insert(u"passed"_s, passed);
    QFile file(output.filePath(u"switch-provider.json"_s));
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(report).toJson()) < 0)
        passed = false;
    if (!passed)
        qWarning("switch provider UI regression failed");
    return passed;
}

void checkLauncherFilters(QQuickWindow* window, QQmlEngine* qmlEngine, const QDir& output, bool gallery,
    bool previewOnly = false, bool* folderScopePassed = nullptr)
{
    QTemporaryDir temporary;
    IndexService index(temporary.path(), IndexService::Options{});
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    History history(temporary.filePath(u"history.tsv"_s));
    SearchEngine engine(&index, &apps, &places);
    Launcher launcher(&index, &apps, &places, &engine, &history);
    launcher.setWindow(window);
    Settings settings;
    settings.fileTypes = Settings::defaultFileTypes();
    launcher.setFileTypes(settings.namedFileTypes());
    launcher.setContentOptions(false, {u"txt"_s}, ContentSizeLimits{}, false, false);
    if (previewOnly) {
        checkFileGallery(launcher, index, places, qmlEngine, output);
        return;
    }
    {
        const auto files = index.index();
        const auto lock = files->writeLock();
        const auto root = files->addRoot("T:");
        files->add(root, "报告资料", EntryFlag::Directory);
        const auto report = files->add(root, "报告图片.png", 0);
        files->setSize(report, packSize(1234567));
        const auto empty = files->add(root, "照片.png", 0);
        files->setSize(empty, packSize(0));
        files->add(root, "假日视频.mp4", 0);
        files->add(root, "报告.txt", 0);
        const auto work = files->add(root, "__work__", EntryFlag::Directory);
        files->add(work, "direct.csv", 0);
        files->add(files->add(work, "sub", EntryFlag::Directory), "nested.CSV", 0);
        files->add(root, "outside.csv", 0);
    }
    const auto check = [folderScopePassed](bool condition, const char* reason) {
        if (!condition) {
            if (folderScopePassed)
                *folderScopePassed = false;
            qWarning("launcher regression: %s", reason);
        }
    };
    // #80: applying a preference updates the existing controller immediately,
    // independently of dates and without resetting the result model.
    check(!launcher.showFileSize(), "file size should be off by default");
    int optionChanges = 0;
    QObject::connect(&launcher, &Launcher::resultOptionsChanged, &launcher, [&] { ++optionChanges; });
    for (const bool enabled : {true, false}) {
        launcher.setResultOptions(KindOrder::FilesFirst, true, enabled, true, u"ctrl"_s);
        check(launcher.showFileSize() == enabled && launcher.showModified(), "file size preference did not apply independently");
        const int before = optionChanges;
        launcher.setResultOptions(KindOrder::FilesFirst, true, enabled, true, u"ctrl"_s);
        check(optionChanges == before, "unchanged preferences signalled again");
    }
    check(optionChanges == 2, "file size changes did not notify existing delegates");
    const auto query = [&](const QString& text) {
        launcher.setQuery(text);
        QElapsedTimer timeout;
        timeout.start();
        while (launcher.searching() && timeout.elapsed() < 2000)
            pump(10);
        check(!launcher.searching(), "search did not settle");
    };
    query(u":folder 报告"_s);
    check(launcher.scope() == Launcher::Folders && launcher.results()->count() == 1
        && launcher.results()->at(0)->isDir, "folder prefix did not reach the search engine");
    query(u":pictures 报告"_s);
    check(launcher.scope() == Launcher::Files && launcher.fileType() == 1 && launcher.results()->count() == 1
        && launcher.results()->at(0)->name.endsWith(u".png"), "type prefix did not filter extensions");
    check(launcher.results()->at(0) && launcher.results()->at(0)->size == 1234567
        && !launcher.results()->data(launcher.results()->index(0), ResultModel::SizeRole).toString().isEmpty(),
        "ordinary search lost the indexed file size");
    check(launcher.query() == u":pictures 报告", "typed prefix disappeared from the field");
    query(u":pictures"_s);
    check(launcher.results()->count() == 2, "prefix alone showed history instead of browsing the type");
    const int emptyRow = launcher.results()->indexOf(u"T:\\照片.png"_s);
    check(emptyRow >= 0 && launcher.results()->at(emptyRow)->size == 0
        && !launcher.results()->data(launcher.results()->index(emptyRow), ResultModel::SizeRole).toString().isEmpty(),
        "zero bytes were treated as unknown");
    query({});
    check(launcher.scope() == Launcher::All, "removing a prefix did not restore the original scope");
    launcher.chooseFileType(1);
    launcher.toggleFileType(2);
    query({});
    check(launcher.selectedFileTypes() == (QList<int>{1, 2}) && launcher.results()->count() == 3,
        "multiple categories did not union their extensions");
    const QString savedTypes = launcher.fileChoice();
    launcher.chooseFileType(-1);
    launcher.setFileChoice(savedTypes);
    check(launcher.selectedFileTypes() == (QList<int>{1, 2}), "multiple categories did not round trip through saved state");
    query(u":pictures,videos"_s);
    check(launcher.selectedFileTypes() == (QList<int>{1, 2}) && launcher.results()->count() == 3,
        "multiple type prefix did not reach the real engine");
    query({});
    check(launcher.selectedFileTypes() == (QList<int>{1, 2}), "removing the prefix lost the previous multiple selection");
    launcher.chooseFileType(-1);
    settings.fileTypes.append({u"工作 CSV"_s, {u"csv"_s}, {}, u"cw"_s, u"T:\\__work__"_s});
    launcher.setFileTypes(settings.namedFileTypes());
    query(u"cw:"_s);
    check(launcher.scope() == Launcher::Files && launcher.results()->count() == 2,
        "cw: 没有直接列出指定目录及子目录的 CSV");
    query(u"cw: nested"_s);
    check(launcher.results()->count() == 1, "范围前缀没有保留搜索文字");
    query(u"cw:"_s);
    settings.fileTypes.last().folder = u"T:\\missing"_s;
    launcher.setFileTypes(settings.namedFileTypes());
    query(u":cw"_s);
    check(launcher.results()->count() == 0, "修改范围后仍显示旧目录的结果");
    settings.fileTypes.last().folder = u"T:\\__work__"_s;
    launcher.setFileTypes(settings.namedFileTypes());
    query(u"cw:"_s);
    check(launcher.results()->count() == 2, "改回范围没有更新结果");
    query({});
    launcher.chooseFileType(static_cast<int>(settings.fileTypes.size()) - 1);
    query({});
    check(launcher.results()->count() == 2, "点击类型没有使用绑定范围");
    launcher.toggleFileType(1); // 同时选中不限范围的图片类型
    query({});
    check(launcher.results()->count() == 5, "不限范围的类型没有解除合并范围约束");
    launcher.chooseFileType(-1);
    settings.fileTypes = Settings::defaultFileTypes();
    launcher.setFileTypes(settings.namedFileTypes());
    // GitHub #392: a quick filter named alone (":pngq") is put in the order
    // chosen; one with an order of its own is in it while it is on, the one
    // chosen given back after and never taken for it (chosenSort).
    {
        const auto settle = [&] {
            QElapsedTimer timeout;
            timeout.start();
            while (launcher.searching() && timeout.elapsed() < 2000)
                pump(10);
        };
        const auto first = [&] {
            const SearchResult* row = launcher.results()->at(0);
            return row ? row->path : QString();
        };
        const QString big = u"T:\\报告图片.png"_s; // 1234567 bytes
        const QString tiny = u"T:\\照片.png"_s; // none
        launcher.setScope(Launcher::All);
        query({});
        launcher.setSort(Launcher::BestMatch);
        launcher.setQuickFilters({{u"大图"_s, u"ext:png"_s, u"bigq"_s, u"largest"_s}, {u"图片"_s, u"ext:png"_s, u"pngq"_s, {}}});
        query(u":pngq"_s);
        check(launcher.results()->count() == 2 && launcher.sort() == Launcher::BestMatch, "a quick filter named alone did not list its own");
        launcher.setSort(Launcher::LargestFirst);
        settle();
        check(first() == big, "#392: a sort chosen did not order a quick filter named alone (largest)");
        launcher.setSort(Launcher::SmallestFirst);
        settle();
        check(first() == tiny, "#392: a sort chosen did not order a quick filter named alone (smallest)");
        launcher.setSort(Launcher::BestMatch);
        query(u":bigq"_s);
        check(launcher.sort() == Launcher::LargestFirst && launcher.chosenSort() == Launcher::BestMatch && first() == big,
            "#392: a quick filter's own order was not in force");
        query(u":bigq 照片"_s); // typed on: still its order
        check(launcher.sort() == Launcher::LargestFirst, "#392: a quick filter's own order went with a word typed");
        query({});
        check(launcher.sort() == Launcher::BestMatch, "#392: the order chosen was not given back after the quick filter");
        query(u":bigq"_s);
        launcher.setSort(Launcher::SmallestFirst); // chosen over it: kept, and remembered as chosen
        settle();
        check(launcher.chosenSort() == Launcher::SmallestFirst && first() == tiny, "#392: a sort chosen did not set the filter's aside");
        query(u":bigq 图"_s);
        check(launcher.sort() == Launcher::SmallestFirst, "#392: a filter's order set aside came back while it stayed on");
        query({});
        check(launcher.sort() == Launcher::SmallestFirst, "#392: the sort chosen over a filter's own was not kept");
        launcher.setSort(Launcher::BestMatch);
        launcher.setQuickFilters({});
    }
    if (folderScopePassed)
        return; // 独立范围探针到此结束，不跑后面的网络、剪贴板等验证。
    query(u"regex:["_s);
    check(!launcher.queryError().isEmpty() && launcher.results()->count() == 0,
        "invalid regex was not reported or retained stale results");
    query(u"regex:^.*\\.mp4$ | ^照片"_s);
    check(launcher.queryError().isEmpty() && launcher.results()->count() == 2,
        "OR and regex did not reach the real engine");
    launcher.setScope(Launcher::All);
    query({});
    query(u":folder 报告"_s);
    launcher.chooseFileType(0);
    check(launcher.query() == u"报告" && launcher.fileType() == 0 && launcher.scope() == Launcher::Files,
        "a manual type choice did not replace the prefix");
    struct SortCase { Launcher::Sort sort; SortKey key; bool descending; };
    const SortCase sorts[]{{Launcher::BestMatch, SortKey::Rank, false}, {Launcher::NewestFirst, SortKey::Modified, true},
        {Launcher::OldestFirst, SortKey::Modified, false}, {Launcher::ByName, SortKey::Name, false},
        {Launcher::ByFolder, SortKey::Path, false}, {Launcher::ByType, SortKey::Type, false},
        {Launcher::LargestFirst, SortKey::Size, true}, {Launcher::SmallestFirst, SortKey::Size, false},
        {Launcher::MostOpened, SortKey::Rank, false}};
    for (const auto& test : sorts) {
        launcher.setPanel(false);
        launcher.setSort(test.sort);
        launcher.setPanel(true);
        check(launcher.panelSortKey() == int(test.key) && launcher.panelSortDescending() == test.descending,
            "expanding the panel lost the compact list's sort");
    }
    // #81: the actual controller, hint, completion and menu paths, without
    // touching the running user's launcher or opening anything in the shell.
    launcher.setPanel(false);
    QDir(temporary.path()).mkpath(u"favorite"_s);
    QDir(temporary.path()).mkpath(u"ordinary"_s);
    const QString favoritePath = QDir::toNativeSeparators(temporary.filePath(u"favorite"_s));
    const QString ordinaryPath = QDir::toNativeSeparators(temporary.filePath(u"ordinary"_s));
    SearchResult folder {.name = u"Favorite"_s, .path = favoritePath, .isDir = true, .favorite = true};
    const auto showRow = [&](const SearchResult& row) {
        launcher.setScope(Launcher::All);
        query({});
        launcher.results()->assign({row}, {});
        launcher.setCurrentRow(-1);
        launcher.setCurrentRow(0);
    };
    // A picture file's row: 贴到屏幕 and 编辑 by the screenshot plugin, only
    // while one does them; 编辑 hands the file over, the launcher going first.
    {
        const QString picturePath = QDir::toNativeSeparators(temporary.filePath(u"图表.png"_s));
        QImage picture(32, 24, QImage::Format_RGB32);
        picture.fill(Qt::darkCyan);
        check(picture.save(picturePath), "the picture file could not be written");
        const auto offers = [&](int action) {
            return std::ranges::any_of(launcher.menuItems(0), [action](const QVariant& entry) {
                const QVariantMap item = entry.toMap();
                return item.contains(u"action"_s) && item.value(u"action"_s).toInt() == action;
            });
        };
        const SearchResult pictureRow {.name = u"图表.png"_s, .path = picturePath};
        showRow(pictureRow);
        check(!offers(Launcher::PinPicture) && !offers(Launcher::EditPicture),
            "a picture file's row offered the screenshot plugin's verbs without the plugin");
        launcher.setPictureVerbs(true, true);
        showRow(pictureRow);
        check(offers(Launcher::PinPicture) && offers(Launcher::EditPicture), "a picture file's row lacks 贴到屏幕 or 编辑");
        QString edited;
        int dismissedFirst = 0;
        const auto editing = QObject::connect(&launcher, &Launcher::editPictureRequested, &launcher,
            [&](const QString& path) { edited = path; });
        const auto dismissing = QObject::connect(&launcher, &Launcher::dismissRequested, &launcher,
            [&] { dismissedFirst += edited.isEmpty(); });
        launcher.trigger(0, Launcher::EditPicture);
        QObject::disconnect(editing);
        QObject::disconnect(dismissing);
        check(edited == picturePath && dismissedFirst == 1, "编辑 did not hand the picture file over after the launcher went");
        showRow(SearchResult {.name = u"说明.txt"_s, .path = QDir::toNativeSeparators(temporary.filePath(u"说明.txt"_s))});
        check(!offers(Launcher::PinPicture) && !offers(Launcher::EditPicture), "a text file's row offered the picture verbs");
        launcher.setPictureVerbs(false, false);
    }
    const auto tabHint = [&] { return launcher.keyHints().value(u"tab"_s).toString(); };
    const QString folderHint = QCoreApplication::translate("ws::Launcher", "进入文件夹");
    const auto hasBrowseAction = [&] {
        const QVariantList menu = launcher.menuItems(0);
        return std::ranges::any_of(menu, [](const QVariant& entry) {
            const QVariantMap item = entry.toMap();
            return item.contains(u"action"_s) && item.value(u"action"_s).toInt() == Launcher::BrowseFolder;
        });
    };
    showRow(folder);
    check(tabHint().isEmpty() && !launcher.autocomplete(0) && launcher.query().isEmpty(),
        "favorite folder stole Tab with the default setting");
    launcher.cycleScope(1);
    check(launcher.scope() == Launcher::Files, "Tab did not advance the scope on a favorite folder");
    launcher.cycleScope(-1);
    check(launcher.scope() == Launcher::All, "Shift+Tab did not reverse the scope");
    showRow(folder);
    launcher.setTabEntersFavoriteFolder(true);
    check(tabHint() == folderHint, "changing the Tab setting did not update the footer immediately");
    check(launcher.autocomplete(0) && launcher.query() == favoritePath + u'\\',
        "opted-in Tab did not browse the favorite folder");
    folder.favorite = false;
    folder.path = ordinaryPath;
    showRow(folder);
    check(tabHint().isEmpty() && !launcher.autocomplete(0) && hasBrowseAction(),
        "an ordinary folder stole Tab or lost its independent browse action");
    launcher.setScope(Launcher::Folders);
    query({});
    launcher.results()->assign({folder}, {});
    launcher.trigger(0, Launcher::BrowseFolder);
    check(launcher.scope() == Launcher::All && launcher.query() == ordinaryPath + u'\\',
        "the folder menu did not browse inside WinShun or retained a folders-only filter");
    folder.favorite = true;
    folder.offline = true;
    showRow(folder);
    check(!launcher.autocomplete(0) && !hasBrowseAction(), "an offline folder can be browsed");
    folder.offline = false;
    folder.missing = true;
    showRow(folder);
    check(!launcher.autocomplete(0) && !hasBrowseAction(), "a missing folder can be browsed");
    folder.missing = false;
    showRow(folder);
    launcher.setTabEntersFavoriteFolder(false);
    check(tabHint().isEmpty() && !launcher.autocomplete(0), "turning the Tab setting off left stale behavior or hints");
    {
        // GitHub #361, the controller's side (Main.qml's keys: checkFolderSteps):
        // Tab steps into a folder chosen (chooseRow) or completing the path
        // typed, never into one the list put under the keyboard by itself
        // (#81); stepOut goes back the way Tab came, the folder chosen again,
        // and on a path typed a level up.
        QDir(temporary.path()).mkpath(u"ordinary/内层"_s);
        const QString base = QDir::toNativeSeparators(temporary.path());
        const QString inner = ordinaryPath + u"\\内层"_s;
        const QString backHint = QCoreApplication::translate("ws::Launcher", "返回上一级");
        const auto settled = [&] {
            QElapsedTimer timeout;
            timeout.start();
            while (launcher.searching() && timeout.elapsed() < 2000)
                pump(10);
        };
        const auto rowOf = [&](const QString& path) { return launcher.results()->indexOf(path); };
        const auto startRow = [&] { // as Main.qml's selectFirst puts the keyboard
            const int row = launcher.results()->defaultRow();
            launcher.setCurrentRow(row);
            return launcher.results()->pathAt(row);
        };
        const auto tabKeys = [&] { return launcher.keyHints().value(u"tabKeys"_s).toString(); };
        launcher.setScope(Launcher::All);
        query(base + u'\\');
        check(startRow() == base && tabHint() == backHint && tabKeys() == u"Ctrl+Backspace",
            "#361 a folder's listing did not start on the folder itself, offering the way up");
        int row = rowOf(ordinaryPath);
        launcher.setCurrentRow(row);
        check(row > 0 && tabHint() != folderHint && !launcher.autocomplete(row) && launcher.query() == base + u'\\',
            "#361 Tab stepped into a folder nobody chose");
        launcher.chooseRow(row);
        check(tabHint() == folderHint && launcher.autocomplete(row) && launcher.query() == ordinaryPath + u'\\',
            "#361 Tab did not step into the folder chosen");
        settled();
        row = rowOf(inner);
        launcher.setCurrentRow(row);
        launcher.chooseRow(row);
        check(row > 0 && launcher.autocomplete(row) && launcher.query() == inner + u'\\', "#361 Tab did not step on down");
        settled();
        check(launcher.stepOut() && launcher.query() == ordinaryPath + u'\\', "#361 stepOut did not come back a level");
        settled();
        check(startRow() == inner && tabHint() == folderHint, "#361 back out, the folder left was not chosen again");
        check(launcher.stepOut() && launcher.query() == base + u'\\', "#361 stepOut did not come back to the first folder");
        settled();
        check(startRow() == ordinaryPath && tabHint() == folderHint, "#361 back out twice, its folder was not chosen again");
        // A part of a name typed: the folder it completes takes Tab by itself.
        query(base + u"\\ordin"_s);
        check(startRow() == ordinaryPath && tabHint() == folderHint && launcher.autocomplete(0)
                && launcher.query() == ordinaryPath + u'\\',
            "#361 Tab did not complete the folder typed in part");
        settled();
        check(launcher.stepOut() && launcher.query() == base + u"\\ordin"_s, "#361 stepOut did not bring back the path as typed");
        settled();
        check(launcher.stepOut() && launcher.query() == base + u'\\', "#361 stepOut did not take the name typed away first");
        // Typed, not stepped into: a level up, the folder left chosen there.
        query(inner + u'\\');
        check(launcher.stepOut() && launcher.query() == ordinaryPath + u'\\', "#361 stepOut on a path typed did not go up");
        settled();
        check(startRow() == inner && tabHint() == folderHint, "#361 up from a path typed, its folder was not chosen");
        // From the words of a search, in 文件夹: back to them, and to 文件夹.
        launcher.setScope(Launcher::Folders);
        query(u"报告"_s);
        row = rowOf(u"T:\\报告资料"_s);
        launcher.setCurrentRow(row);
        check(row >= 0 && !launcher.autocomplete(row) && launcher.query() == u"报告", "#361 Tab took a folder found but not chosen");
        launcher.chooseRow(row);
        check(launcher.autocomplete(row) && launcher.scope() == Launcher::All && launcher.query() == u"T:\\报告资料\\",
            "#361 Tab did not step into a folder found by the words");
        settled();
        check(launcher.stepOut() && launcher.scope() == Launcher::Folders && launcher.query() == u"报告",
            "#361 stepOut did not bring back the words and their scope");
        settled();
        check(startRow() == u"T:\\报告资料" && tabHint() == folderHint, "#361 back at the words, the folder was not chosen again");
        check(!launcher.stepOut() && launcher.query() == u"报告", "#361 stepOut did something to words that are no path");
        // A prefix's scope comes back with the prefix.
        launcher.setScope(Launcher::All);
        query(u":folder 报告"_s);
        row = rowOf(u"T:\\报告资料"_s);
        launcher.setCurrentRow(row);
        launcher.chooseRow(row);
        check(launcher.autocomplete(row) && launcher.scope() == Launcher::All && launcher.query() == u"T:\\报告资料\\",
            "#361 Tab did not step out of a :folder search");
        settled();
        check(launcher.stepOut() && launcher.query() == u":folder 报告" && launcher.scope() == Launcher::Folders,
            "#361 stepOut lost the :folder prefix");
        settled();
        query({});
        check(launcher.scope() == Launcher::All, "#361 the prefix went, its scope did not");
        // Regex mode: the words are patterns, no names to step into.
        launcher.setRegexMode(true);
        query(u"报告"_s);
        row = rowOf(u"T:\\报告资料"_s);
        launcher.setCurrentRow(row);
        launcher.chooseRow(row);
        check(row >= 0 && !launcher.autocomplete(row) && launcher.query() == u"报告", "#361 Tab stepped into a folder in regex mode");
        launcher.setRegexMode(false);
        query({});
        // GitHub #454, typed in a folder's window: the words are a path from
        // that folder. A part of a name is completed by its folder there, the
        // box going on as the path in full; stepOut brings the window's folder
        // back with the words, searched as it was. A folder found elsewhere,
        // chosen by nobody, keeps Tab for the scopes.
        const QString prefix = base + u"\\ "_s;
        const auto typeIn = [&](const QString& words) {
            launcher.setTypingFolder(prefix);
            query(prefix + words);
            settled();
        };
        typeIn(u"ordin"_s);
        check(startRow() == ordinaryPath && tabHint() == folderHint && launcher.autocomplete(0)
                && launcher.query() == ordinaryPath + u'\\' && launcher.typingFolder().isEmpty(),
            "#454 Tab did not complete the folder typed in part in a folder's window");
        settled();
        check(launcher.stepOut() && launcher.query() == prefix + u"ordin"_s && !launcher.typingFolder().isEmpty(),
            "#454 stepOut did not come back to the folder's window and its words");
        settled();
        check(startRow() == ordinaryPath && tabHint() == folderHint, "#454 back out, the folder left was not chosen again");
        typeIn(u"ordinary\\内"_s);
        row = rowOf(inner);
        launcher.setCurrentRow(row);
        check(row >= 0 && launcher.autocomplete(row) && launcher.query() == inner + u'\\',
            "#454 Tab did not complete a path typed from the window's folder");
        settled();
        check(launcher.stepOut() && launcher.query() == prefix + u"ordinary\\内"_s, "#454 stepOut lost the path typed from the window's folder");
        typeIn(u"报告"_s);
        row = rowOf(u"T:\\报告资料"_s);
        launcher.setCurrentRow(row);
        check(row >= 0 && !launcher.autocomplete(row) && launcher.query() == prefix + u"报告"_s,
            "#454 Tab took a folder found elsewhere that nobody chose");
        launcher.chooseRow(row);
        check(launcher.autocomplete(row) && launcher.query() == u"T:\\报告资料\\", "#454 Tab did not step into the folder chosen");
        settled();
        check(launcher.stepOut() && launcher.query() == prefix + u"报告"_s && !launcher.typingFolder().isEmpty(),
            "#454 stepOut from a folder chosen did not come back to the folder's window");
        launcher.setTypingFolder({});
        query({});
    }
    SearchResult suggestion {.name = u"Search suggestion"_s, .app = AppKind::Web,
        .keyword = u"example"_s, .words = u"report"_s, .suggestion = true};
    showRow(suggestion);
    check(launcher.autocomplete(0) && launcher.query() == u"example report", "web suggestion completion changed");
    {
        // GitHub #303: a site's own row (found by a part of its keyword, or
        // by its name) takes Tab too: its keyword and a space, to type the
        // words after. A page that searches nothing keeps Tab for the scopes.
        launcher.setWebShortcuts({{.keyword = u"example"_s, .name = u"Example"_s, .url = u"https://example.com/?q=%s"_s},
            {.keyword = u"page"_s, .name = u"Page"_s, .url = u"https://example.com/page"_s}});
        const QString completeHint = QCoreApplication::translate("ws::Launcher", "补全");
        const SearchResult site {.name = u"Example"_s, .app = AppKind::Web, .keyword = u"example"_s};
        showRow(site);
        check(tabHint() == completeHint && launcher.autocomplete(0) && launcher.query() == u"example ",
            "Tab on a site's row did not put its keyword in the box");
        launcher.results()->assign({site}, {});
        launcher.setCurrentRow(-1);
        launcher.setCurrentRow(0);
        check(!launcher.autocomplete(0) && launcher.query() == u"example ", "Tab completed a site's keyword already in the box");
        showRow(SearchResult {.name = u"Page"_s, .app = AppKind::Web, .keyword = u"page"_s});
        check(tabHint().isEmpty() && !launcher.autocomplete(0), "Tab completed the keyword of a page that takes no words");
        launcher.setWebShortcuts({});
    }
    {
        // GitHub #116: Enter on a file whose server does not answer
        // (TEST-NET-1, nothing there) says "正在连接 …" at once, the window
        // never waiting for the network; "连不上 …" once the probe and
        // Windows gave up, off the UI thread.
        const SearchResult remote {.name = u"报告.txt"_s, .path = u"\\\\192.0.2.1\\share\\报告.txt"_s};
        showRow(remote);
        QElapsedTimer enter;
        enter.start();
        quint64 checks = launcher.serverChecksStarted();
        launcher.trigger(0, Launcher::Open);
        const qint64 returned = enter.elapsed();
        const QString connecting = QCoreApplication::translate("ws::Launcher", "正在连接 %1…").arg(u"192.0.2.1"_s);
        check(returned < 100 && launcher.statusText() == connecting, "Enter on an unreachable share held the window up or said nothing");
        const QString cannot = QCoreApplication::translate("ws::Launcher", "连不上 %1（离线或不在同一网络）").arg(u"192.0.2.1"_s);
        while (launcher.statusText() != cannot && enter.elapsed() < 30000)
            pump(50);
        check(launcher.statusText() == cannot, "no 连不上 for an unreachable share");
        say(u"#116 Enter on \\\\192.0.2.1\\share\\报告.txt: 正在连接 shown as Enter returned, %1 ms; 连不上 after %2 ms; checks started %3"_s
                .arg(returned)
                .arg(enter.elapsed())
                .arg(launcher.serverChecksStarted() - checks));

        // Its menu opened while it waits (Main.qml's openContextMenu): the
        // wait is dropped, and nothing is opened or said for it later.
        showRow(remote);
        launcher.trigger(0, Launcher::Open);
        const bool waited = launcher.statusText() == connecting;
        launcher.dropServerWait();
        const bool cleared = launcher.statusText() != connecting;
        QElapsedTimer menu;
        menu.start();
        bool saidLater = false;
        while (!saidLater && menu.elapsed() < 10000) { // the check ends after 8 s
            pump(50);
            saidLater = launcher.statusText() == cannot;
        }
        check(waited && cleared && !saidLater, "a menu opened while waiting for a server left the wait on");
        say(u"#116 right-click while waiting: 正在连接 %1, gone at once %2, 连不上 in the next 10 s %3"_s
                .arg(waited ? u"shown"_s : u"NOT shown"_s, cleared ? u"yes"_s : u"NO"_s, saidLater ? u"YES"_s : u"no"_s));

        // Enter held (repeating at 30 a second): one check for the row,
        // answered once, for the newest press.
        showRow(remote);
        checks = launcher.serverChecksStarted();
        QElapsedTimer held;
        held.start();
        qint64 slowest = 0;
        for (int i = 0; i < 30; ++i) {
            QElapsedTimer press;
            press.start();
            launcher.trigger(0, Launcher::Open);
            slowest = std::max(slowest, press.elapsed());
            pump(33);
        }
        const qint64 pressedFor = held.elapsed();
        const quint64 started = launcher.serverChecksStarted() - checks;
        const bool stillWaiting = launcher.statusText() == connecting;
        while (launcher.statusText() != cannot && held.elapsed() < 30000)
            pump(50);
        check(started == 1 && stillWaiting && slowest < 100 && launcher.statusText() == cannot,
            "Enter held on an unreachable share started a check per press, or held the window up");
        say(u"#116 Enter held, 30 presses over %1 ms: checks started %2, slowest press %3 ms, 正在连接 after the last %4, 连不上 %5 ms after the first"_s
                .arg(pressedFor)
                .arg(started)
                .arg(slowest)
                .arg(stillWaiting ? u"yes"_s : u"NO"_s)
                .arg(held.elapsed()));

        // Ctrl+C on two selected rows there: copied at once, no server asked
        // (Explorer finds out on pasting), as for one row; nothing lost. The
        // clipboard (and the user's history in it) left alone.
        const SearchResult second {.name = u"附件.txt"_s, .path = u"\\\\192.0.2.1\\share\\附件.txt"_s};
        launcher.setScope(Launcher::All);
        query({});
        launcher.results()->assign({remote, second}, {});
        launcher.setCurrentRow(0);
        launcher.selectRange(0, 1, false);
        QStringList copied;
        launcher.setFileCopier([&copied](const QStringList& files) { copied = files; });
        checks = launcher.serverChecksStarted();
        QElapsedTimer copy;
        copy.start();
        launcher.triggerSelection(Launcher::CopyItem); // Main.qml's act() with rows selected
        const qint64 copyTook = copy.elapsed();
        const QString copiedText = QCoreApplication::translate("ws::Launcher", "已复制 %Ln 项，可在资源管理器中粘贴", nullptr, 2);
        check(copied.size() == 2 && copyTook < 100 && launcher.statusText() == copiedText
                && launcher.serverChecksStarted() == checks,
            "Ctrl+C on a selection on an unreachable share was lost or waited for the server");
        say(u"#116 Ctrl+C on 2 selected rows there: %1 file(s) handed to the clipboard in %2 ms, checks started %3, footer \"%4\""_s
                .arg(copied.size())
                .arg(copyTook)
                .arg(launcher.serverChecksStarted() - checks)
                .arg(launcher.statusText()));
        launcher.setFileCopier({});
        launcher.clearSelection();
    }
    query({}); // let the last search settle before its controller and engine go away
    say(u"launcher prefix, panel-sort and favorite Tab regressions: real controllers/engine over isolated fixtures"_s);

    // GitHub #344: a program command's words looked for as paths, listed
    // above its own row; the first one current, Enter named by the command.
    {
        const QString suggestFolder = temporary.filePath(u"plugins/test.suggest"_s);
        QDir().mkpath(suggestFolder);
        QFile suggest(QDir(suggestFolder).filePath(u"plugin.json"_s));
        check(suggest.open(QIODevice::WriteOnly), "could not create the isolated suggest plugin");
        suggest.write(R"({"manifest":1,"id":"test.suggest","name":"Suggest fixture","version":"1.0.0","commands":[
            {"id":"open","keyword":"edit","title":"用编辑器打开","run":{"program":"C:\\none\\editor.exe","args":"%s","suggest":"paths"}},
            {"id":"dir","keyword":"indir","title":"在终端打开","run":{"program":"C:\\none\\term.exe","args":"-d %s","suggest":"folders"}},
            {"id":"txt","keyword":"note","title":"用记事本打开","run":{"program":"C:\\none\\note.exe","args":"%s","suggest":"files"}}]})");
        suggest.close();
        plugin::Host host({.userPluginDir = temporary.filePath(u"plugins"_s)});
        host.catalog().scan();
        host.setEnabled(u"test.suggest"_s, true);
        launcher.setPluginHost(&host);
        launcher.setPanel(false);
        const auto rows = [&] {
            QStringList names;
            for (int i = 0; i < launcher.results()->count(); ++i) {
                const SearchResult* r = launcher.results()->at(i);
                names.append(r->isPlugin() ? u"[" + r->name + u"]" : r->name);
            }
            return names;
        };
        const auto settled = [&](const QString& text) {
            query(text);
            pump(100);
            query(text); // the plugin's row and the names', both in
        };
        settled(u"edit 报告"_s);
        const QStringList paths = rows();
        check(paths.size() == 4 && paths.last() == u"[用编辑器打开]" && !paths.first().startsWith(u'['),
            "#344: paths were not listed above the command's row");
        check(paths.contains(u"报告资料"_s) && paths.contains(u"报告.txt"_s), "#344: a file or a folder was missing");
        check(launcher.currentRow() == 0 && launcher.keyHints().value(u"enter"_s).toString() == u"用编辑器打开",
            "#344: the first path was not current, or Enter did not say what it does");
        say(u"#344 edit 报告: %1; Enter: %2"_s.arg(paths.join(u", "_s), launcher.keyHints().value(u"enter"_s).toString()));
        settled(u"indir 报告"_s);
        check(rows() == QStringList({u"报告资料"_s, u"[在终端打开]"_s}), "#344: folders only listed something else");
        say(u"#344 indir 报告: %1"_s.arg(rows().join(u", "_s)));
        settled(u"note 报告"_s);
        check(!rows().contains(u"报告资料"_s) && rows().contains(u"报告.txt"_s) && rows().last() == u"[用记事本打开]",
            "#344: files only listed a folder");
        say(u"#344 note 报告: %1"_s.arg(rows().join(u", "_s)));
        settled(u"edit "_s);
        check(rows() == QStringList({u"[用编辑器打开]"_s}), "#344: the keyword alone listed paths");
        query({});
        launcher.setPluginHost(nullptr);
    }
    // ... and Enter on one: the program started with that path as %s.
    {
        const QString picked = QDir::toNativeSeparators(temporary.filePath(u"picked.txt"_s));
        const QString folder = temporary.filePath(u"plugins/test.pick"_s);
        QDir().mkpath(folder);
        QFile manifest(QDir(folder).filePath(u"plugin.json"_s));
        check(manifest.open(QIODevice::WriteOnly), "could not create the isolated pick plugin");
        const QJsonObject run {{u"program"_s, u"%SystemRoot%\\System32\\cmd.exe"_s},
            {u"args"_s, u"/d /c echo %s>\""_s + picked + u'"'}, {u"suggest"_s, u"paths"_s}};
        manifest.write(QJsonDocument(QJsonObject {{u"manifest"_s, 1}, {u"id"_s, u"test.pick"_s}, {u"name"_s, u"Pick fixture"_s},
            {u"version"_s, u"1.0.0"_s}, {u"permissions"_s, QJsonArray {u"processes"_s}},
            {u"commands"_s, QJsonArray {QJsonObject {{u"id"_s, u"pick"_s}, {u"keyword"_s, u"pick"_s}, {u"title"_s, u"交给 cmd"_s}, {u"run"_s, run}}}}})
                .toJson());
        manifest.close();
        plugin::Host host({.userPluginDir = temporary.filePath(u"plugins"_s)});
        host.catalog().scan();
        host.setEnabled(u"test.pick"_s, true);
        launcher.setPluginHost(&host);
        query(u"pick outside"_s);
        pump(100);
        query(u"pick outside"_s);
        const SearchResult* first = launcher.results()->at(0);
        check(first && !first->isPlugin() && first->name == u"outside.csv", "#344: the path was not the first row");
        launcher.trigger(0, Launcher::Open);
        QElapsedTimer wait;
        wait.start();
        QByteArray got;
        while (wait.elapsed() < 5000 && got.trimmed().isEmpty()) {
            pump(50);
            QFile out(picked);
            if (out.open(QIODevice::ReadOnly))
                got = out.readAll();
        }
        // An ASCII path: the same in whatever code page cmd writes.
        check(QString::fromLatin1(got.trimmed()) == QDir::toNativeSeparators(first ? first->path : QString()),
            "#344: Enter did not hand the path over");
        say(u"#344 Enter on %1: the program got \"%2\""_s.arg(first ? first->path : QString(), QString::fromLatin1(got.trimmed())));
        query({});
        launcher.setPluginHost(nullptr);
    }
    // GitHub #395: what is typed after a plugin's keyword goes into the
    // search history too, unless its command keeps it out ("history": false).
    {
        const QString folder = temporary.filePath(u"plugins/test.history"_s);
        QDir().mkpath(folder);
        QFile manifest(QDir(folder).filePath(u"plugin.json"_s));
        check(manifest.open(QIODevice::WriteOnly), "could not create the isolated history plugin");
        manifest.write(R"({"manifest":1,"id":"test.history","name":"History fixture","version":"1.0.0","commands":[
            {"id":"sum","keyword":"sum","title":"Sum","run":{"items":[{"id":"r","title":"21"}]}},
            {"id":"hide","keyword":"hide","title":"Hidden","history":false,"run":{"items":[{"id":"r","title":"x"}]}}]})");
        manifest.close();
        plugin::Host host({.userPluginDir = temporary.filePath(u"plugins"_s)});
        host.catalog().scan();
        host.setEnabled(u"test.history"_s, true);
        launcher.setPluginHost(&host);
        query(u"sum 3*7"_s);
        launcher.rememberQuery();
        check(launcher.pluginKeyword() == u"sum" && launcher.queryHistory().value(0) == u"sum 3*7",
            "#395: a plugin keyword's search did not go into the search history");
        query(u"hide p@ss"_s);
        launcher.rememberQuery();
        check(launcher.pluginKeyword() == u"hide" && launcher.queryHistory().value(0) == u"sum 3*7"
                && !launcher.queryHistory().contains(u"hide p@ss"_s),
            "#395: a command with \"history\": false was kept in the search history");
        say(u"#395 search history after \"sum 3*7\" and \"hide p@ss\": %1"_s.arg(launcher.queryHistory().join(u" | "_s)));
        // GitHub #411: the keyword and its words looked for as names instead (Shift+Tab).
        query(u"sum 3*7"_s);
        check(launcher.keyHints().value(u"tabKeys"_s).toString() == u"Shift+Tab", "#411: keyword mode did not offer Shift+Tab");
        check(launcher.searchKeywordAsWords(), "#411: Shift+Tab was refused in keyword mode");
        query(launcher.query());
        check(launcher.query() == u"\"sum\" 3*7" && launcher.pluginKeyword().isEmpty() && launcher.queryError().isEmpty(),
            "#411: Shift+Tab did not turn the keyword into a quoted word of the name search");
        check(!launcher.searchKeywordAsWords(), "#411: Shift+Tab claimed a keyword with none there");
        say(u"#411 Shift+Tab on \"sum 3*7\": the box reads %1, keyword \"%2\""_s.arg(launcher.query(), launcher.pluginKeyword()));
        query({});
        launcher.setPluginHost(nullptr);
    }

    const QString pluginFolder = temporary.filePath(u"plugins/test.columns"_s);
    QDir().mkpath(pluginFolder);
    QFile manifest(QDir(pluginFolder).filePath(u"plugin.json"_s));
    check(manifest.open(QIODevice::WriteOnly), "could not create the isolated metadata plugin");
    manifest.write(R"({"manifest":1,"id":"test.columns","name":"Metadata fixture","version":"1.0.0",
        "commands":[{"id":"rows","keyword":"metadata","title":"Metadata","run":{"items":[
            {"id":"old","title":"Old.pdf","modified":"2024-01-01T00:00:00Z","size":10},
            {"id":"new","title":"New.pdf","modified":"2026-10-01T00:00:00Z","size":2097152},
            {"id":"zero","title":"Zero.pdf","modified":1790812800,"size":0},
            {"id":"legacy","title":"Legacy.pdf"}]}}]})");
    manifest.close();
    plugin::Host host({.userPluginDir = temporary.filePath(u"plugins"_s)});
    host.catalog().scan();
    host.setEnabled(u"test.columns"_s, true);
    launcher.setPluginHost(&host);
    launcher.setPanel(true);
    launcher.chooseFileType(-1);
    query(u"metadata "_s);
    check(launcher.results()->count() == 4, "metadata plugin did not supply its full list");
    launcher.setDateRange(u"2026-01-01"_s, u"2026-12-31"_s);
    pump(50);
    check(launcher.results()->count() == 3, "plugin date filtering or legacy compatibility failed");
    // The boxes take a unit as well as a bare number of MB, and mark what they cannot read (GitHub #394).
    check(launcher.readsAsSize(u"40 kb"_s) && launcher.readsAsSize(u"1.5"_s) && launcher.readsAsSize(u" "_s)
            && !launcher.readsAsSize(u"40 kilo"_s) && launcher.readsAsDate(u"2026"_s) && launcher.readsAsDate(u"2026/3/5"_s)
            && !launcher.readsAsDate(u"2026-13"_s),
        "the panel's size or date boxes misread what was typed");
    launcher.setSizeRange({}, u"1 KB"_s);
    pump(50);
    bool bigKept = false;
    for (int i = 0; i < launcher.results()->count(); ++i)
        bigKept = bigKept || (launcher.results()->at(i) && launcher.results()->at(i)->name == u"New.pdf");
    check(!bigKept, "a size typed with its unit (1 KB) was not taken as the bound");
    launcher.setSizeRange(u"1"_s, {});
    pump(50);
    check(launcher.results()->count() == 2, "plugin size filtering or unknown sizes failed");
    launcher.sortBy(int(SortKey::Size));
    check(launcher.results()->at(0) && launcher.results()->at(0)->name == u"New.pdf", "plugin size sorting failed");
    launcher.chooseFileType(0);
    pump(50);
    check(launcher.pluginKeyword() == u"metadata" && launcher.results()->count() == 2,
        "choosing a file type left the active plugin command");
    launcher.clearFilters();
    query({});
    launcher.setPluginHost(nullptr);
    say(u"plugin date/size/type filters and local sorting: real host, merge and launcher with isolated zero-code plugin"_s);

    // The actual search panel: local mouse events over its headers exercise
    // resizing, moving and sorting. Its column preferences use only this
    // temporary profile, never the running user's INI file.
    launcher.panelColumns()->load(temporary.filePath(u"panel.ini"_s));
    launcher.panelColumns()->reset();
    launcher.setPanel(true);
    launcher.chooseFileType(-1);
    launcher.setScope(Launcher::All);
    query(u"报告"_s);
    WindowFrame frame(false);
    Placement placement(temporary.filePath(u"state.ini"_s), u"Launcher"_s);
    QQmlComponent component(qmlEngine);
    component.setData(R"(
        import QtQuick
        import WinShun
        Window {
            id: panelFixture
            required property Launcher controller
            required property WindowFrame frame
            required property Placement placement
            width: 1200; height: 650; color: Theme.background
            // The style's match colour and weight, as Main.qml gives the real one.
            Binding { target: panelFixture.controller.results; property: "highlightColor"; value: Theme.matchColor }
            Binding { target: panelFixture.controller.results; property: "highlightWeight"; value: Theme.matchWeight }
            Panel { anchors.fill: parent; launcher: panelFixture.controller; frame: panelFixture.frame; placement: panelFixture.placement }
        }
    )", QUrl(u"qrc:/qt/qml/WinShun/PanelRegression.qml"_s));
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {u"controller"_s, QVariant::fromValue(&launcher)}, {u"frame"_s, QVariant::fromValue(&frame)},
        {u"placement"_s, QVariant::fromValue(&placement)}}));
    auto* panelWindow = qobject_cast<QQuickWindow*>(object.get());
    if (!panelWindow) { qWarning().noquote() << "panel regression:" << component.errorString(); return; }
    panelWindow->create();
    launcher.setWindow(panelWindow);
    frame.setWindow(panelWindow);
    frame.setResizable(true);
    placement.setWindow(panelWindow);
    showOffScreen(panelWindow);
    pump(250);
    settle(panelWindow);
    check(saveGrab(panelWindow, output.filePath(u"panel-default.png"_s)), "could not render default panel");
    // The 自定义 size boxes (GitHub #394): "40 kb" taken, what cannot be read marked.
    launcher.setSizeRange(u"40 kb"_s, u"abc"_s);
    settle(panelWindow);
    check(saveGrab(panelWindow, output.filePath(u"panel-size-bounds.png"_s)), "could not render the size boxes");
    launcher.setSizeRange({}, {});
    launcher.setSizeChoice(0);
    settle(panelWindow);
    bool sawFolderSize = false;
    for (int row = 0; row < launcher.results()->count(); ++row) {
        if (!launcher.results()->at(row)->isDir) continue;
        sawFolderSize = true;
        const QModelIndex item = launcher.results()->index(row);
        QElapsedTimer wait;
        wait.start();
        while (!launcher.results()->data(item, ResultModel::SizeRole).toString().startsWith(u"≈") && wait.elapsed() < 2000)
            pump(10);
        check(launcher.results()->data(item, ResultModel::SizeRole).toString().startsWith(u"≈"),
            "folder size did not reach the production result model asynchronously");
        check(launcher.results()->data(item, ResultModel::SizeRole).toString() == u"≈ %1"_s.arg(sizeText(0)),
            "empty folder estimate used a different size format from file rows");
    }
    check(sawFolderSize, "folder size regression did not include a folder result");
    {
        SearchResults original;
        for (int row = 0; row < launcher.results()->count(); ++row) original.push_back(*launcher.results()->at(row));
        SearchResults extra;
        if (!original.isEmpty()) {
            for (int i = 0; i < 60; ++i) extra.push_back(original.first());
            launcher.results()->append(extra);
            settle(panelWindow, 2);
            auto* scroll = visualNamed(panelWindow->contentItem(), u"panelResultScrollBar"_s);
            check(scroll && scroll->isVisible(), "scrollbar regression did not exercise an overflowing list");
            if (scroll && scroll->isVisible()) {
                const HWND handle = reinterpret_cast<HWND>(panelWindow->winId());
                POINT origin {};
                ::ClientToScreen(handle, &origin);
                RECT client {};
                ::GetClientRect(handle, &client);
                const qreal dpr = panelWindow->devicePixelRatio();
                const QPointF centre = scroll->mapToScene(QPointF(scroll->width() / 2, scroll->height() / 2));
                const int y = origin.y + qRound(centre.y() * dpr);
                check(::SendMessageW(handle, WM_NCHITTEST, 0, MAKELPARAM(origin.x + client.right - 1, y)) == HTRIGHT,
                    "visible scrollbar stole the native right resize edge");
                check(::SendMessageW(handle, WM_NCHITTEST, 0, MAKELPARAM(origin.x + qRound(centre.x() * dpr), y)) == HTCLIENT,
                    "scrollbar's thumb was consumed by window resizing");
                check(saveGrab(panelWindow, output.filePath(u"panel-scroll-resize.png"_s)), "could not render scrollbar resize boundary");
            }
            launcher.results()->assign(std::move(original), {u"报告"_s});
            settle(panelWindow, 2);
        }
    }
    // The columns' own scroll bar: there only while they are wider than the
    // list, and no room is kept for it under the rows otherwise (GitHub #310:
    // it lay across the whole list whatever the widths).
    {
        auto* list = visualNamed(panelWindow->contentItem(), u"panelResultList"_s);
        auto* bar = visualNamed(panelWindow->contentItem(), u"panelColumnsScrollBar"_s);
        check(list && bar, "panel list or its columns scroll bar missing");
        if (list && bar) {
            const auto widths = [&] {
                int total = 0;
                for (const QVariant& column : launcher.panelColumns()->visibleColumns())
                    total += column.toMap().value(u"width"_s).toInt();
                return total;
            };
            const int name = launcher.panelColumns()->visibleColumns()[0].toMap().value(u"width"_s).toInt();
            launcher.panelColumns()->resize(u"name"_s, 120);
            settle(panelWindow, 2);
            check(widths() < list->width(), "columns scroll bar regression needs columns narrower than the list");
            check(!bar->isVisible(), "columns scroll bar shown although every column fits");
            check(list->property("bottomMargin").toReal() == 4, "room kept under the rows for a scroll bar that is not there");
            check(saveGrab(panelWindow, output.filePath(u"panel-columns-fit.png"_s)), "could not render fitting columns");
            launcher.panelColumns()->resize(u"name"_s, 2000);
            settle(panelWindow, 2);
            check(widths() > list->width() && bar->isVisible() && bar->property("size").toReal() < 1,
                "columns scroll bar missing although the columns are wider than the list");
            check(list->property("bottomMargin").toReal() == 14, "rows not kept clear of the columns scroll bar");
            check(saveGrab(panelWindow, output.filePath(u"panel-columns-wide.png"_s)), "could not render overflowing columns");
            launcher.panelColumns()->resize(u"name"_s, name);
            settle(panelWindow, 2);
            check(launcher.panelColumns()->visibleColumns()[0].toMap().value(u"width"_s).toInt() == name,
                "columns scroll bar regression did not put the name column back");
        }
    }
    const auto mouse = [&](QEvent::Type type, const QPointF& at, Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent event(type, at, panelWindow->mapToGlobal(at.toPoint()), button, held, {});
        QCoreApplication::sendEvent(panelWindow, &event);
    };
    const auto drag = [&](const QString& name, QPointF delta) {
        auto* item = visualNamed(panelWindow->contentItem(), name);
        if (!item) { check(false, "panel header or divider missing"); return; }
        const QPointF at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        mouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, at + delta, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, at + delta, Qt::LeftButton, {});
        settle(panelWindow, 2);
    };
    drag(u"columnResize-name"_s, QPointF(80, 0));
    check(launcher.panelColumns()->visibleColumns()[0].toMap().value(u"width"_s).toInt() == 360,
        "dragging the separator did not resize the column");
    drag(u"columnHeader-path"_s, QPointF(-400, 0));
    check(launcher.panelColumns()->visibleColumns()[0].toMap().value(u"id"_s).toString() == u"path",
        "dragging the header did not move the column");
    launcher.panelColumns()->toggle(u"created"_s);
    launcher.panelColumns()->toggle(u"type"_s);
    launcher.panelColumns()->toggle(u"source"_s);
    launcher.panelColumns()->toggle(u"folder"_s);
    launcher.panelColumns()->toggle(u"path"_s);
    launcher.panelColumns()->resize(u"name"_s, 230);
    launcher.panelColumns()->resize(u"modified"_s, 170);
    launcher.panelColumns()->move(3, 1); // put Created next to Name
    const auto columns = launcher.panelColumns()->visibleColumns();
    PanelColumns reloaded;
    reloaded.load(temporary.filePath(u"panel.ini"_s));
    check(reloaded.visibleColumns() == columns, "panel column preferences did not persist");
    SearchResult plugin {.name = u"2026 年项目发票.pdf"_s, .path = u"plugin:fixture:invoice"_s,
        .modified = 1792857600, .created = 1782864000, .size = 245760, .app = AppKind::Plugin};
    plugin.plugin.row = std::make_shared<PluginRow>(PluginRow {.source = u"发票库"_s, .icon = u"glyph:U+EA86"_s,
        .openKind = PluginRow::OpenKind::Path, .openTarget = u"C:\\发票\\2026 年项目发票.pdf"_s});
    SearchResult legacy {.name = u"未提供文件信息的插件条目"_s, .path = u"plugin:fixture:legacy"_s, .app = AppKind::Plugin};
    legacy.plugin.row = std::make_shared<PluginRow>(PluginRow {.source = u"旧版插件"_s, .icon = u"glyph:U+EA86"_s});
    {
        // GitHub #448: 大图 of a plugin's row that opens a picture (图片文字搜索's)
        // is that picture, as the file's own row's; with no file, the icon.
        SearchResult picture {.name = u"牛蛙.jpg"_s, .path = u"plugin:fixture:ocr"_s, .modified = 1792857600, .app = AppKind::Plugin};
        picture.plugin.row = std::make_shared<PluginRow>(PluginRow {.source = u"图片文字搜索"_s, .icon = u"glyph:U+EB9F"_s,
            .openKind = PluginRow::OpenKind::Path, .openTarget = u"C:/图片/牛蛙.jpg"_s});
        SearchResult mail = legacy;
        mail.path = u"plugin:fixture:mail"_s;
        mail.plugin.row = std::make_shared<PluginRow>(PluginRow {.source = u"邮件"_s, .openKind = PluginRow::OpenKind::Path,
            .openTarget = u"C:\\邮件\\牛蛙.jpg"_s, .item = QJsonObject {{u"details"_s, true}}});
        launcher.results()->assign({picture, legacy, mail}, {});
        const auto bigPicture = [&](int row) {
            return launcher.results()->data(launcher.results()->index(row), ResultModel::GalleryThumbnailRole).toString();
        };
        check(bigPicture(0) == ThumbnailProvider::url(u"C:\\图片\\牛蛙.jpg"_s, 1792857600),
            "大图 of a plugin's picture row is not that picture (GitHub #448)");
        check(bigPicture(1).isEmpty() && bigPicture(2).isEmpty(), "大图 of a plugin's row with no file of its own is not its icon");
    }
    launcher.results()->assign({plugin, legacy}, {});
    settle(panelWindow);
    check(saveGrab(panelWindow, output.filePath(u"panel-plugin-metadata.png"_s)), "could not render plugin metadata columns");
    if (auto* header = visualNamed(panelWindow->contentItem(), u"columnHeader-created"_s)) {
        const QPointF at = header->mapToScene(QPointF(header->width() / 2, header->height() / 2));
        mouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, {});
        check(launcher.panelSortKey() == int(SortKey::Created), "created column did not sort on click");
        mouse(QEvent::MouseButtonPress, at, Qt::RightButton, Qt::RightButton);
        mouse(QEvent::MouseButtonRelease, at, Qt::RightButton, {});
        pump(200);
        if (auto* menu = panelWindow->findChild<QObject*>(u"panelColumnsMenu"_s)) {
            check(menu->property("visible").toBool(), "column context menu did not open on right click");
            QMetaObject::invokeMethod(menu, "close");
        } else check(false, "column context menu missing");
    } else check(false, "created column header missing");
    if (gallery) {
        launcher.clearFilters();
        launcher.chooseFileType(-1);
        launcher.setScope(Launcher::All);
        query({});
        const QStringList names {u"年度报告 2026.pdf"_s, u"季度销售报告.xlsx"_s,
            u"项目验收报告.docx"_s, u"财务分析报告.pptx"_s, u"年度报告 封面.png"_s};
        {
            const auto files = index.index();
            const auto lock = files->writeLock();
            const auto root = files->addRoot("F:");
            const auto demo = files->add(root, "Win顺演示", EntryFlag::Directory);
            const auto reports = files->add(demo, "报告", EntryFlag::Directory);
            for (qsizetype i = 0; i < names.size(); ++i) {
                const auto id = files->add(reports, names[i].toStdString(), 0);
                files->setModified(id, static_cast<FileTime>(1792857600 - i * 86400));
                files->setCreated(id, static_cast<FileTime>(1782864000 + i * 86400));
                files->setSize(id, packSize((i + 1) * 245760));
            }
        }
        query(u"报告"_s);
        // The application normally writes programmatic queries into its
        // current SearchBar. This fixture embeds Panel directly, without App.
        if (auto* field = visualNamed(panelWindow->contentItem(), u"panelSearchField"_s))
            field->setProperty("text", u"报告"_s);
        launcher.panelColumns()->reset();
        launcher.panelColumns()->toggle(u"path"_s);
        launcher.panelColumns()->toggle(u"type"_s);
        launcher.panelColumns()->toggle(u"created"_s);
        settle(panelWindow);
        check(saveGrab(panelWindow, output.filePath(u"panel-gallery.png"_s)), "could not render the panel gallery");
        // The columns menu is a top-level window of its own (ContextMenu): open it
        // the production way (right click -> Panel.openColumns), grab both
        // windows and lay the menu over the panel at its real position.
        // A header away from the right edge: the menu opens at the click and
        // would run past the panel's image (on screen it may, it is a window).
        if (auto* header = visualNamed(panelWindow->contentItem(), u"columnHeader-type"_s)) {
            const QPointF at = header->mapToScene(QPointF(header->width() / 2, header->height() / 2));
            mouse(QEvent::MouseButtonPress, at, Qt::RightButton, Qt::RightButton);
            mouse(QEvent::MouseButtonRelease, at, Qt::RightButton, {});
            settle(panelWindow);
            auto* menu = panelWindow->findChild<QQuickWindow*>(u"panelColumnsMenu"_s);
            bool composed = false;
            if (menu && menu->isVisible()) {
                menu->contentItem()->polish();
                pump(150);
                const QImage base = panelWindow->grabWindow();
                QImage popup = menu->grabWindow();
                const qreal dpr = panelWindow->devicePixelRatio();
                if (!base.isNull() && !popup.isNull()) {
                    // Grabs carry the window's DPR; paint in device pixels.
                    QImage image = base.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                    image.setDevicePixelRatio(1);
                    popup.setDevicePixelRatio(1);
                    const QPoint menuAt((menu->position() - panelWindow->position()) * dpr);
                    QPainter painter(&image);
                    painter.drawImage(menuAt, popup);
                    painter.end();
                    composed = QRect(QPoint(), image.size()).contains(QRect(menuAt, popup.size()))
                        && image.save(output.filePath(u"panel-gallery-menu.png"_s), "PNG");
                    say(u"%1 panel-gallery-menu.png (menu %2x%3 at %4,%5 of %6x%7)"_s
                            .arg(composed ? u"saved"_s : u"NOT saved"_s).arg(popup.width()).arg(popup.height())
                            .arg(menuAt.x()).arg(menuAt.y()).arg(image.width()).arg(image.height()));
                }
            }
            check(composed, "could not render the columns menu inside the panel image");
            if (menu)
                QMetaObject::invokeMethod(menu, "close");
        }
    }
    launcher.setWindow(window);
    query({});
    if (gallery) {
        launcher.setWindow(panelWindow);
        checkFileGallery(launcher, index, places, qmlEngine, output);
        launcher.setWindow(window);
        query({});
    }
    say(u"panel column drag/resize/persistence and plugin metadata: production QML with isolated fixtures"_s);
}

bool checkClipboardSizing(QQuickWindow* window, Clipboard& clipboard, Placement& placement,
    ClipStore& store, QQuickItem* page, const QDir& output)
{
    bool ok = true;
    QJsonObject results;
    const auto check = [&](const char* name, bool passed) {
        results.insert(QString::fromLatin1(name), passed);
        ok = ok && passed;
        if (!passed) qWarning("clipboard sizing: %s", name);
    };
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    for (int i = 0; i < 25; ++i)
        store.add(ClipCapture {.kind = ClipKind::Text,
            .text = u"第 %1 条剪贴板记录：整理项目资料，检查窗口大小与滚动体验。\n"_s.arg(i + 1).repeated(50)},
            QDateTime::currentMSecsSinceEpoch() + i);
    clipboard.historyChanged();
    clipboard.handleShown();
    // Use the largest work area for the unconstrained resize assertions.
    // An offscreen preview may start on a smaller secondary monitor.
    auto* screen = QGuiApplication::primaryScreen();
    for (auto* candidate : QGuiApplication::screens()) {
        const auto a = candidate->availableGeometry();
        const auto b = screen->availableGeometry();
        if (qint64(a.width()) * a.height() > qint64(b.width()) * b.height())
            screen = candidate;
    }
    window->setScreen(screen);
    const auto area = screen->availableGeometry();
    // Place through the production API so `room` matches the actual spot.
    // A direct setPosition left the old room limit behind; releasing the
    // first width drag then legitimately changed the automatic row count.
    placement.attach(QRect(area.topLeft() + QPoint(50, 20), QSize(1, 0)), screen);
    settle(window);
    auto* numberedRow = visualNamed(page, u"clipboardRow-0"_s);
    auto* numberSlot = numberedRow ? visualNamed(numberedRow, u"clipboardNumberSlot"_s) : nullptr;
    auto* number = numberedRow ? visualNamed(numberedRow, u"clipboardNumber"_s) : nullptr;
    auto* icon = numberedRow ? visualNamed(numberedRow, u"clipboardIconSlot"_s) : nullptr;
    check("number_column_separate_from_thumbnail", numberSlot && icon
        && numberSlot->x() + numberSlot->width() + 6 <= icon->x());
    check("number_column_centred_and_digit_only", number && numberSlot && numberedRow
        && number->property("text").toString() == u"1"
        && numberSlot->height() == numberedRow->height());
    const qreal numberedIconX = icon ? icon->x() : -1;
    clipboard.setNumberHints(u"held"_s);
    settle(window);
    pump(200);
    check("held_number_keeps_column_alignment", icon && numberSlot
        && icon->x() == numberedIconX && numberSlot->opacity() == 0);
    clipboard.setNumberHints(u"off"_s);
    settle(window);
    check("disabled_numbers_release_column", icon && numberSlot
        && icon->x() == numberedIconX - 28 && !numberSlot->isVisible());
    clipboard.setNumberHints(u"always"_s);
    clipboard.setNumberKeys(u"ctrl"_s);
    clipboard.setNumberKeysLeft(1u << 1);
    settle(window);
    check("occupied_number_is_struck_through", number && numberedRow
        && numberedRow->property("hintModifier").toString() == u"Ctrl"
        && number->property("font").value<QFont>().strikeOut());
    clipboard.setNumberKeysLeft(0);
    // With clipboard number keys off and 字母直达 on, passive letters preview calmly and arm into keycaps.
    clipboard.setNumberKeys(u"off"_s);
    clipboard.setLetterJump(true, u"auto"_s, u"ASDFGHJKLQWERTYUIOPZXCVBNM"_s);
    settle(window);
    auto* clipHints = visualNamed(page, u"clipboardNumberHints"_s);
    auto* clipCap = numberSlot ? visualNamed(numberSlot, u"hintKeyCap"_s) : nullptr;
    check("clipboard_passive_letter_preview", number && numberSlot && clipCap
        && numberSlot->isVisible() && number->property("text").toString() == u"A" && !clipCap->isVisible());
    if (clipHints) {
        clipHints->setProperty("jumping", true);
        settle(window);
        check("clipboard_armed_letter_keycap", number && clipCap
            && number->property("text").toString() == u"A" && clipCap->isVisible());
        clipHints->setProperty("jumping", false);
        settle(window);
    }
    clipboard.setLetterJump(false, u"auto"_s, u"ASDFGHJKLQWERTYUIOPZXCVBNM"_s);
    clipboard.setNumberKeys(u"alt"_s);
    settle(window);
    const auto resize = [&](int edge, int dw, int dh, bool cancel = false) {
        RECT original {};
        ::GetWindowRect(hwnd, &original);
        RECT proposed = original;
        const LONG dx = qRound(dw * window->devicePixelRatio());
        const LONG dy = qRound(dh * window->devicePixelRatio());
        if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) proposed.left -= dx;
        else proposed.right += dx;
        if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) proposed.top -= dy;
        else proposed.bottom += dy;
        ::SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
        ::SendMessageW(hwnd, WM_SIZING, edge, reinterpret_cast<LPARAM>(&proposed));
        ::SetWindowPos(hwnd, nullptr, proposed.left, proposed.top, proposed.right - proposed.left, proposed.bottom - proposed.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        pump(35);
        check("live_page_tracks_window", std::abs(page->height() - window->height()) < 1 && std::abs(page->width() - window->width()) < 1);
        if (cancel)
            ::SetWindowPos(hwnd, nullptr, original.left, original.top, original.right - original.left, original.bottom - original.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        ::SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
        settle(window, 2);
    };
    const auto hit = [&](int x, int y) {
        RECT rect {};
        ::GetWindowRect(hwnd, &rect);
        const int px = x < 0 ? rect.right + x : rect.left + x;
        const int py = y < 0 ? rect.bottom + y : rect.top + y;
        return ::SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(px, py));
    };
    const int midX = qRound(window->width() * window->devicePixelRatio() / 2);
    const int midY = qRound(window->height() * window->devicePixelRatio() / 2);
    check("all_eight_resize_edges", hit(1, 1) == HTTOPLEFT && hit(-2, 1) == HTTOPRIGHT
        && hit(1, -2) == HTBOTTOMLEFT && hit(-2, -2) == HTBOTTOMRIGHT
        && hit(midX, 1) == HTTOP && hit(midX, -2) == HTBOTTOM
        && hit(1, midY) == HTLEFT && hit(-2, midY) == HTRIGHT);
    const auto scrollbarWorks = [&](const QString& name) {
        auto* bar = visualNamed(page, name);
        if (!bar || !bar->isVisible()) return false;
        const auto at = bar->mapToGlobal(QPointF(bar->width() - 3, bar->height() / 2));
        const auto origin = window->mapToGlobal(QPoint(0, 0));
        const auto code = hit(qRound((at.x() - origin.x()) * window->devicePixelRatio()),
                   qRound((at.y() - origin.y()) * window->devicePixelRatio()));
        return code == HTCLIENT;
    };
    check("list_scrollbar_does_not_resize", scrollbarWorks(u"clipboardListScrollBar"_s));
    const int initialHeight = window->height();
    resize(WMSZ_RIGHT, 100, 0);
    say(u"clipboard sizing: work %1x%2, DPR %3; initial height %4; width drag -> %5x%6, saved %7x%8"_s
        .arg(area.width()).arg(area.height()).arg(window->devicePixelRatio()).arg(initialHeight)
        .arg(window->width()).arg(window->height()).arg(clipboard.listWidth()).arg(clipboard.windowHeight()));
    check("width_saved_without_fixing_auto_height", clipboard.listWidth() == 580 && window->width() == 580
        && clipboard.windowHeight() == 0 && window->height() == initialHeight && store.value(u"listWidth"_s) == u"580");
    resize(WMSZ_BOTTOM, 0, 60);
    const int chosenHeight = window->height();
    say(u"clipboard sizing: height drag -> %1, saved %2, expected %3"_s
        .arg(chosenHeight).arg(clipboard.windowHeight()).arg(initialHeight + 60));
    check("height_saved", chosenHeight == initialHeight + 60 && clipboard.windowHeight() == chosenHeight
        && store.value(u"windowHeight"_s).toInt() == chosenHeight);
    resize(WMSZ_TOPLEFT, 20, 20);
    check("corner_resize", clipboard.listWidth() == 600 && clipboard.windowHeight() == chosenHeight + 20);
    const QSize chosen = window->size();
    resize(WMSZ_BOTTOMRIGHT, 40, 40, true);
    check("cancel_keeps_preferences", window->size() == chosen && clipboard.listWidth() == chosen.width()
        && clipboard.windowHeight() == chosen.height());
    clipboard.resumeHistory();
    check("preferences_reload", clipboard.listWidth() == chosen.width() && clipboard.windowHeight() == chosen.height());
    window->hide();
    window->show();
    settle(window);
    check("reopen_keeps_size", window->size() == chosen);
    QMetaObject::invokeMethod(page, "showPreview");
    settle(window);
    const int previewWidth = window->property("previewWidth").toInt();
    const int beforeList = window->property("listWidth").toInt();
    resize(WMSZ_RIGHT, 30, 0);
    check("preview_resize_keeps_preview_width", window->property("previewWidth").toInt() == previewWidth
        && clipboard.listWidth() == beforeList + 30);
    check("preview_scrollbar_does_not_resize", scrollbarWorks(u"clipboardPreviewScrollBar"_s));
    check("preview_screenshot", saveGrab(window, output.filePath(u"clipboard-sizing-preview.png"_s)));
    QMetaObject::invokeMethod(page, "closePreview", Q_ARG(QVariant, QVariant(false))); // closePreview(keepWanted)
    settle(window);
    check("preview_close_keeps_list_size", window->width() == clipboard.listWidth() && window->height() == clipboard.windowHeight());
    clipboard.setWindowHeight(qRound(area.height() * 0.75));
    settle(window);
    const int rememberedHeight = clipboard.windowHeight();
    placement.attach(QRect(area.center(), QSize(1, 0)), window->screen());
    settle(window);
    check("temporary_room_does_not_overwrite_height", window->height() < rememberedHeight
        && clipboard.windowHeight() == rememberedHeight);
    placement.placeOn(window->screen());
    settle(window);
    check("larger_room_restores_height", window->height() == rememberedHeight);
    clipboard.setLayoutOptions(true, true);
    placement.attach(QRect(QPoint(area.left() + 50, area.bottom() - 1), QSize(1, 0)), window->screen());
    settle(window);
    check("above_caret_layout_is_flipped", placement.above() && page->property("flipped").toBool());
    check("flipped_layout_screenshot", saveGrab(window, output.filePath(u"clipboard-sizing-flipped.png"_s)));
    clipboard.setLayoutOptions(false, false);
    placement.placeOn(window->screen());
    ::SendMessageW(hwnd, WM_NCLBUTTONDBLCLK, HTRIGHT, 0);
    settle(window);
    check("edge_double_click_resets", clipboard.listWidth() == 480 && clipboard.windowHeight() == 0 && window->width() == 480);
    clipboard.setWindowHeight(-1);
    check("invalid_height_clamped", clipboard.windowHeight() == 0);
    check("default_screenshot", saveGrab(window, output.filePath(u"clipboard-sizing-default.png"_s)));
    {
        // Real mouse input exercises the system size loop and its no-activate
        // route. Only this fixture is on screen; the cursor is put back.
        const HWND foreground = ::GetForegroundWindow();
        POINT cursor {};
        ::GetCursorPos(&cursor);
        const auto restoreCursor = qScopeGuard([&] { ::SetCursorPos(cursor.x, cursor.y); });
        ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        settle(window);
        RECT rect {};
        ::GetWindowRect(hwnd, &rect);
        const QSize before = window->size();
        // The pointer goes to the corner first: where the user left it, it
        // may be on a row, and that row's hover preview would lie over the
        // corner and take the press. Until the corner is the window's own.
        const POINT corner {rect.right - 2, rect.bottom - 2};
        ::SetCursorPos(corner.x, corner.y);
        QElapsedTimer clear;
        clear.start();
        while (::GetAncestor(::WindowFromPoint(corner), GA_ROOT) != hwnd && clear.elapsed() < 3000)
            pump(50);
        check("corner_is_the_windows_own", ::GetAncestor(::WindowFromPoint(corner), GA_ROOT) == hwnd);
        std::jthread input([rect] {
            const auto button = [](DWORD flags) {
                INPUT event {};
                event.type = INPUT_MOUSE;
                event.mi.dwFlags = flags;
                ::SendInput(1, &event, sizeof event);
            };
            ::SetCursorPos(rect.right - 2, rect.bottom - 2);
            ::Sleep(50);
            button(MOUSEEVENTF_LEFTDOWN);
            ::Sleep(100);
            ::SetCursorPos(rect.right + 28, rect.bottom + 28);
            ::Sleep(100);
            button(MOUSEEVENTF_LEFTUP);
        });
        pump(600);
        input.join();
        settle(window);
        check("real_mouse_corner_drag", window->width() > before.width() && window->height() > before.height()
            && clipboard.listWidth() == window->width() && clipboard.windowHeight() == window->height() && !placement.windowSizing());
        check("resize_does_not_activate", ::GetForegroundWindow() == foreground && !window->isActive());
    }
    QFile report(output.filePath(u"clipboard-sizing.json"_s));
    if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(results).toJson(QJsonDocument::Indented));
    say(u"clipboard native edge/corner sizing, persistence, preview and temporary fitting: isolated production QML"_s);
    return ok;
}

bool checkClipboardInteractions(QQmlEngine* engine, const QDir& output, const QString& galleryImage, bool sizingOnly = false)
{
    QTemporaryDir temporary;
    ClipStore store(temporary.path());
    bool ok = true;
    const auto check = [&](bool condition, const char* reason) {
        ok = condition && ok;
        if (!condition) qWarning("clipboard regression: %s", reason);
    };
    if (!store.open(QDateTime::currentMSecsSinceEpoch())) { check(false, "temporary history could not open"); return false; }
    // Missing files stop before the native clipboard writer. The double
    // click still runs the production paste handler, safely on this fixture.
    store.add(ClipCapture {.kind = ClipKind::Files,
        .text = QDir::toNativeSeparators(temporary.filePath(u"不存在的演示文件.txt"_s))}, QDateTime::currentMSecsSinceEpoch());
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    clipboard.setRowOptions(false, u"standard"_s, SystemTheme::uiSize());
    clipboard.setPreviewOpen(false);
    clipboard.setActive(true);
    clipboard.handleShown();
    Placement placement(temporary.filePath(u"state.ini"_s), u"Clipboard"_s);
    WindowFrame frame(false);
    QQmlComponent component(engine, u"WinShun"_s, u"ClipboardWindow"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"placement"_s, QVariant::fromValue(&placement)}, {u"frame"_s, QVariant::fromValue(&frame)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { qWarning().noquote() << "clipboard regression:" << component.errorString(); return false; }
    window->create();
    frame.setWindow(window);
    placement.setWindow(window);
    placement.enableWindowSizing();
    frame.setResizable(true);
    frame.setNoActivate(true);
    showOffScreen(window);
    clipboard.handlePlaced();
    settle(window);
    auto* row = visualNamed(window->contentItem(), u"clipboardRow-0"_s);
    auto* page = visualNamed(window->contentItem(), u"clipboardPage"_s);
    if (!row || !page) { check(false, "production clipboard row missing"); return false; }
    auto* logo = visualNamed(window->contentItem(), u"clipboardSearchLogo"_s);
    auto* searchField = visualNamed(window->contentItem(), u"clipboardSearchField"_s);
    check(logo && logo->isVisible() && logo->property("status").toInt() == 1
            && logo->property("source").toUrl() == QUrl(u"qrc:/branding/logo.svg"_s),
        "clipboard search must load the shared program logo");
    if (logo && searchField) {
        for (const bool searchAtBottom : {false, true}) {
            clipboard.setLayoutOptions(true, searchAtBottom);
            settle(window);
            const QRectF mark(logo->mapToScene(QPointF()), logo->size());
            const QRectF field(searchField->mapToScene(QPointF()), searchField->size());
            check(qAbs(field.left() - mark.right() - 14) < 0.01
                    && qAbs(field.center().y() - mark.center().y()) <= 1 && field.width() > 0,
                "clipboard logo must preserve the search field spacing in either header position");
            check(QMetaObject::invokeMethod(page, "focusSearch") && searchField->hasFocus(),
                "clipboard search must retain its logical input focus");
            check(saveGrab(window, output.filePath(searchAtBottom ? u"clipboard-search-bottom.png"_s : u"clipboard-search-top.png"_s)),
                "clipboard search logo must render in either header position");
        }
        clipboard.setLayoutOptions(true, false);
        // An entry's note (GitHub #267): 备注 in its menu opens the box the
        // groups are named in; the note then leads the entry's line.
        if (!store.clips().empty()) {
            const qint64 id = store.clips().front().id;
            qint64 asked = 0;
            const QMetaObject::Connection heard
                = QObject::connect(&clipboard, &Clipboard::noteRequested, &clipboard, [&asked](qint64 entry) { asked = entry; });
            clipboard.trigger(0, Clipboard::EditNote);
            settle(window);
            QObject::disconnect(heard);
            check(asked == id, "备注 did not ask for the entry's note");
            check(saveGrab(window, output.filePath(u"clipboard-note-box.png"_s)), "the note's box did not render");
            // The box offers the colours (GitHub #398): Tab picks the next one.
            QQuickItem* tabs = page->findChild<QQuickItem*>(u"clipTabs"_s);
            check(tabs && tabs->property("colored").toBool(), "the note's box offers no colours");
            if (tabs) {
                const auto press = [&](int code, Qt::KeyboardModifiers modifiers = {}) {
                    QKeyEvent down(QEvent::KeyPress, code, modifiers), up(QEvent::KeyRelease, code, modifiers);
                    QCoreApplication::sendEvent(window, &down);
                    QCoreApplication::sendEvent(window, &up);
                };
                press(Qt::Key_Tab);
                press(Qt::Key_Tab);
                check(tabs->property("noteColor").toInt() == 2, "Tab does not pick the next colour");
                for (int i = 0; i < 3; ++i)
                    press(Qt::Key_Backtab, Qt::ShiftModifier);
                check(tabs->property("noteColor").toInt() == ClipStore::kNoteColors, "Shift+Tab does not go round to the last colour");
                check(tabs->property("naming").toBool(), "Tab left the note's box");
                tabs->setProperty("noteColor", 5);
                settle(window);
            }
            check(saveGrab(window, output.filePath(u"clipboard-note-colors.png"_s)), "the note's colours did not render");
            clipboard.setNote(id, u"学校 收件地址"_s, 0);
            QMetaObject::invokeMethod(page, "focusSearch");
            settle(window);
            const QString title = clipboard.items()->data(clipboard.items()->index(0), ClipModel::TitleRole).toString();
            check(title.startsWith(u"<b>学校 收件地址</b>"), "the note does not lead the entry's line");
            check(saveGrab(window, output.filePath(u"clipboard-note.png"_s)), "the noted entry did not render");
            // In a colour of the theme's own (Theme.noteColors), the rest of the line as it was.
            const QStringList palette = clipboard.items()->notePalette();
            check(palette.size() == ClipStore::kNoteColors, "the clipboard has no note colours");
            clipboard.setNote(id, u"学校 收件地址"_s, 5);
            settle(window);
            const QString blue = clipboard.items()->data(clipboard.items()->index(0), ClipModel::TitleRole).toString();
            check(palette.size() == ClipStore::kNoteColors
                    && blue.startsWith(u"<font color=\"%1\"><b>学校 收件地址</b></font>"_s.arg(QColor::fromString(palette.at(4)).name())),
                "the note is not in its colour");
            check(saveGrab(window, output.filePath(u"clipboard-note-blue.png"_s)), "the coloured note did not render");
            clipboard.setNote(id, QString(), 5); // no note, no colour
            check(store.clip(id) && store.clip(id)->noteColor == 0, "a colour stayed without its note");
            clipboard.setNote(id, u"学校 收件地址"_s, 0);
        }
        // An entry's own words changed (GitHub #267): 修改文字 in its menu puts
        // an editor where the preview is; Ctrl+Enter saves, Esc does not, and
        // words another entry has already are refused with the editor open.
        {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const qint64 other = store.add(ClipCapture {.kind = ClipKind::Text, .text = u"another entry"_s}, now + 5);
            const qint64 id = store.add(ClipCapture {.kind = ClipKind::Text, .text = u"sk-old-key"_s}, now + 6);
            clipboard.historyChanged();
            settle(window);
            QQuickItem* const editor = window->findChild<QQuickItem*>(u"clipboardTextEditor"_s);
            const auto editing = [&] { return page->property("editingId").toLongLong(); };
            const auto key = [&](int code, Qt::KeyboardModifiers modifiers, const QString& text = {}) {
                QKeyEvent press(QEvent::KeyPress, code, modifiers, text);
                QCoreApplication::sendEvent(window, &press);
                QKeyEvent release(QEvent::KeyRelease, code, modifiers, text);
                QCoreApplication::sendEvent(window, &release);
            };
            const auto type = [&](const QString& words) {
                key(Qt::Key_A, Qt::ControlModifier);
                for (const QChar c : words)
                    key(0, {}, QString(c));
            };
            const auto listed = [&] {
                for (const QVariant& item : clipboard.menuItems(0)) {
                    if (item.toMap().value(u"action"_s).toInt() == Clipboard::EditText)
                        return true;
                }
                return false;
            };
            check(editor && listed(), "修改文字 is not in a text entry's menu");
            clipboard.trigger(0, Clipboard::EditText);
            settle(window);
            check(editing() == id && page->property("previewOpen").toBool() && !clipboard.previewOpen(),
                "修改文字 did not open the editor in the preview's place");
            check(editor && editor->isVisible() && window->activeFocusItem()
                    && window->activeFocusItem()->parentItem() && window->activeFocusItem()->property("text").toString() == u"sk-old-key",
                "the editor does not hold the entry's words and the keys");
            check(saveGrab(window, output.filePath(u"clipboard-edit-text.png"_s)), "the editor did not render");
            type(u"another entry"_s);
            key(Qt::Key_Return, Qt::ControlModifier);
            settle(window);
            check(editing() == id && store.clip(id)->text == u"sk-old-key"
                    && clipboard.statusText() == Clipboard::tr("已有一条内容相同的记录"),
                "another entry's words were not refused with the editor open");
            type(u"sk-new-key"_s);
            key(Qt::Key_Return, Qt::ControlModifier);
            settle(window);
            check(editing() == 0 && store.clip(id)->text == u"sk-new-key" && !page->property("previewOpen").toBool(),
                "Ctrl+Enter did not save the words and close the editor");
            check(clipboard.items()->data(clipboard.items()->index(0), ClipModel::TitleRole).toString().contains(u"sk-new-key"),
                "the list does not show the new words");
            check(saveGrab(window, output.filePath(u"clipboard-edit-saved.png"_s)), "the edited entry did not render");
            // Esc: as it was, and nothing put by for the next time.
            clipboard.trigger(0, Clipboard::EditText);
            settle(window);
            type(u"thrown away"_s);
            key(Qt::Key_Escape, {});
            settle(window);
            check(editing() == 0 && store.clip(id)->text == u"sk-new-key", "Esc did not leave the words as they were");
            clipboard.trigger(0, Clipboard::EditText);
            settle(window);
            check(window->activeFocusItem() && window->activeFocusItem()->property("text").toString() == u"sk-new-key",
                "words thrown away with Esc came back");
            // Left for the search field: put by, and there again.
            type(u"half done"_s);
            QMetaObject::invokeMethod(page, "focusSearch");
            settle(window);
            check(editing() == 0 && store.clip(id)->text == u"sk-new-key", "leaving the editor saved or kept it open");
            clipboard.trigger(0, Clipboard::EditText);
            settle(window);
            check(window->activeFocusItem() && window->activeFocusItem()->property("text").toString() == u"half done",
                "words typed and left were lost");
            key(Qt::Key_Escape, {});
            settle(window);
            std::vector<qint64> gone {id, other};
            store.remove(gone);
            clipboard.historyChanged();
            QMetaObject::invokeMethod(page, "focusSearch");
            settle(window);
        }
        // 显示搜索框 off (feedback 2026-11-08): the row keeps its buttons, the
        // field is hidden yet still takes the keys; letters search nothing,
        // arrows move through the list, and naming a group shows it again.
        {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const std::vector<qint64> added {store.add(ClipCapture {.kind = ClipKind::Text, .text = u"第一条"_s}, now + 10),
                store.add(ClipCapture {.kind = ClipKind::Text, .text = u"第二条"_s}, now + 11)};
            clipboard.historyChanged();
            clipboard.setLayoutOptions(true, false, false);
            QMetaObject::invokeMethod(page, "focusSearch");
            settle(window);
            auto* list = visualNamed(window->contentItem(), u"clipboardEntries"_s);
            const auto press = [&](int code, const QString& text = {}) {
                QKeyEvent down(QEvent::KeyPress, code, {}, text);
                QCoreApplication::sendEvent(window, &down);
                QKeyEvent up(QEvent::KeyRelease, code, {}, text);
                QCoreApplication::sendEvent(window, &up);
            };
            check(!page->property("fieldShown").toBool() && searchField->opacity() == 0
                    && searchField->property("readOnly").toBool() && searchField->hasFocus(),
                "with the search box off the field must be hidden, read-only and still take the keys");
            for (const QChar c : u"abc"_s)
                press(0, QString(c));
            settle(window);
            check(clipboard.query().isEmpty() && searchField->property("text").toString().isEmpty(),
                "with the search box off typed letters must search nothing");
            // From the middle row ↑ moves one way or the other (flipped or not).
            if (list) list->setProperty("currentIndex", 1);
            press(Qt::Key_Up);
            settle(window);
            check(list && qAbs(list->property("currentIndex").toInt() - 1) == 1,
                "with the search box off the arrows must still move through the list");
            check(saveGrab(window, output.filePath(u"clipboard-no-search.png"_s)), "the window without a search box did not render");
            QMetaObject::invokeMethod(page, "beginGroupSearch");
            settle(window);
            check(page->property("fieldShown").toBool() && searchField->opacity() == 1 && !searchField->property("readOnly").toBool(),
                "naming a group to find must show the field even with the search box off");
            check(saveGrab(window, output.filePath(u"clipboard-no-search-group.png"_s)), "naming a group without a search box did not render");
            QMetaObject::invokeMethod(page, "cancelGroupSearch");
            settle(window);
            check(!page->property("fieldShown").toBool(), "the field must hide again once the group is named");
            clipboard.setLayoutOptions(true, false, true);
            settle(window);
            check(page->property("fieldShown").toBool() && searchField->opacity() == 1 && !searchField->property("readOnly").toBool(),
                "the search box must come back when switched on");
            std::vector<qint64> gone = added;
            store.remove(gone);
            clipboard.historyChanged();
            QMetaObject::invokeMethod(page, "focusSearch");
            settle(window);
        }
        // The tab it opens on (GitHub #404): 全部 as before, a group chosen in
        // the settings or in its tab's menu, or the one it was left on.
        {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const qint64 work = store.addGroup(u"常用回复"_s);
            const qint64 entry = store.add(ClipCapture {.kind = ClipKind::Text, .text = u"收到，马上处理"_s}, now + 20);
            store.setGroup(std::array {entry}, work);
            clipboard.historyChanged();
            const auto indexOf = [&](qint64 group) {
                const QVariantList tabs = clipboard.categories();
                for (int i = 0; i < tabs.size(); ++i)
                    if (tabs[i].toMap().value(u"group"_s).toLongLong() == group)
                        return i;
                return -1;
            };
            const int workTab = indexOf(work);
            const auto reopen = [&] {
                clipboard.handleHidden();
                clipboard.handleShown();
                settle(window);
            };
            check(workTab > 0, "a new group must have a tab");
            clipboard.setCategory(workTab);
            reopen();
            check(clipboard.category() == 0, "by default the clipboard must open on 全部 whatever tab it was left on");

            const QString workKey = u"group:"_s + QString::number(work);
            const QVariantList choices = clipboard.openTabChoices();
            const auto choice = [&](int i, const char* field) { return choices.value(i).toMap().value(QString::fromLatin1(field)).toString(); };
            check(choices.size() == clipboard.categories().size() + 1 && choice(0, "value") == u"all" && choice(1, "value") == u"last"
                    && choice(1, "text") == u"上次看的" && choice(workTab + 1, "value") == workKey && choice(workTab + 1, "text") == u"常用回复",
                "the settings must offer 全部, 上次看的 and then every tab, groups by their names");

            clipboard.setOpenTab(workKey);
            reopen();
            check(clipboard.category() == workTab && clipboard.items()->count() == 1,
                "a group chosen in the settings must be the tab the clipboard opens on");
            check(saveGrab(window, output.filePath(u"clipboard-open-tab.png"_s)), "the clipboard opened on a group did not render");

            clipboard.setOpenTab(u"last"_s);
            clipboard.setCategory(1); // 文本
            reopen();
            store.flush();
            check(clipboard.category() == 1 && store.value(u"lastTab"_s) == u"text",
                "上次看的 must open on the tab it was left on, and keep it for the next run");

            QString told;
            const auto connection = QObject::connect(&clipboard, &Clipboard::openTabChosen, [&](const QString& key) { told = key; });
            QVariantList menu = clipboard.categoryMenuItems(workTab);
            check(!menu.isEmpty() && menu.first().toMap().value(u"action"_s).toInt() == Clipboard::OpenHere
                    && !menu.first().toMap().value(u"checked"_s).toBool() && menu.size() == 4,
                "a group's tab menu must start with 打开时显示这里, then rename and remove");
            check(clipboard.categoryMenuItems(0).size() == 1 && clipboard.categoryMenuItems(indexOf(ClipStore::kPinned)).size() == 1,
                "全部 and 固定 must have 打开时显示这里 alone in their menus");
            // Through the tab's own menu, as a right click opens it and a click picks the entry.
            const QPoint at = window->mapToGlobal(QPoint(200, 60));
            check(QMetaObject::invokeMethod(page, "openCategoryMenu", Q_ARG(QVariant, QVariant(workTab)), Q_ARG(QVariant, QVariant(QPointF(at)))),
                "a group's tab menu did not open");
            if (auto* tabMenu = qobject_cast<QQuickWindow*>(page->property("contextMenu").value<QObject*>())) {
                settle(tabMenu);
                check(saveGrab(tabMenu, output.filePath(u"clipboard-tab-menu.png"_s)), "a group's tab menu did not render");
                QMetaObject::invokeMethod(tabMenu, "triggered", Q_ARG(int, workTab), Q_ARG(int, Clipboard::OpenHere));
                QMetaObject::invokeMethod(tabMenu, "dismiss");
                settle(window);
            } else {
                check(false, "the production tab menu is missing");
            }
            menu = clipboard.categoryMenuItems(workTab);
            check(told == workKey && clipboard.openTab() == workKey && menu.first().toMap().value(u"checked"_s).toBool(),
                "打开时显示这里 must choose that tab, tell the settings and be checked afterwards");
            clipboard.triggerCategory(workTab, Clipboard::OpenHere);
            check(told == u"all" && clipboard.openTab() == u"all", "打开时显示这里 picked again must go back to 全部");
            QObject::disconnect(connection);

            clipboard.setOpenTab(workKey);
            store.removeGroup(work);
            clipboard.historyChanged();
            reopen();
            check(clipboard.category() == 0, "a chosen group since removed must open on 全部");
            store.discardImages(std::array {entry});
            clipboard.setOpenTab(u"all"_s);
            reopen();
            QMetaObject::invokeMethod(page, "focusSearch");
        }
        settle(window);
        say(u"clipboard search logo: shared SVG loaded; spacing, focus and both header positions checked"_s);
    } else {
        check(false, "clipboard search logo and field must be present");
    }
    if (sizingOnly) {
        const bool passed = checkClipboardSizing(window, clipboard, placement, store, page, output);
        check(!watcher.running(), "sizing probe must never capture or write the native clipboard");
        window->hide();
        return ok && passed;
    }
    const auto mouse = [&](QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent event(type, at, window->mapToGlobal(at.toPoint()), button, held, {});
        QCoreApplication::sendEvent(window, &event);
    };
    const QPointF at = row->mapToScene(QPointF(130, row->height() / 2));
    mouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, {});
    check(!page->property("previewOpen").toBool(), "first click widened the window before a possible double click");
    mouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonDblClick, at, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, {});
    pump(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 80);
    check(!clipboard.previewOpen() && !page->property("previewOpen").toBool(), "double click opened or remembered a preview");
    check(clipboard.statusText() == Clipboard::tr("这些文件已经不存在了"), "double click did not reach the file paste handler");
    check(checkClipboardSizing(window, clipboard, placement, store, page, output), "clipboard sizing regression");
    showOffScreen(window);
    settle(window);
    check(saveGrab(window, output.filePath(u"clipboard-resized.png"_s)), "clipboard window did not render");
    {
        const QString words = u"历史里选中的文字，直接发送。"_s;
        store.add(ClipCapture {.kind = ClipKind::Text, .text = words}, QDateTime::currentMSecsSinceEpoch() + 1);
        clipboard.historyChanged();
        int setup = 0;
        QString sent;
        QStringList sentFiles;
        const auto setupConnection = QObject::connect(&clipboard, &Clipboard::lanSendSetupRequested, &clipboard, [&] { ++setup; });
        const auto sendConnection = QObject::connect(&clipboard, &Clipboard::lanSendRequested, &clipboard,
            [&](const QString& text, const QStringList& files) { sent = text; sentFiles = files; });
        clipboard.trigger(0, Clipboard::SendLan);
        check(setup == 1 && sent.isEmpty(), "disabled plugin did not offer setup");
        clipboard.setLanSendAvailable(true);
        settle(window);
        check(QMetaObject::invokeMethod(page, "openContextMenu", Q_ARG(QVariant, QVariant(0)), Q_ARG(QVariant, QVariant())),
            "clipboard context menu did not open");
        auto* menu = qobject_cast<QQuickWindow*>(page->property("contextMenu").value<QObject*>());
        if (menu) {
            settle(menu);
            check(saveGrab(menu, output.filePath(u"clipboard-send-menu.png"_s)), "send menu did not render");
            QMetaObject::invokeMethod(menu, "triggered", Q_ARG(int, 0), Q_ARG(int, Clipboard::SendLan));
            QMetaObject::invokeMethod(menu, "dismiss");
        } else {
            check(false, "production send menu is missing");
        }
        check(sent == words && sentFiles.isEmpty(), "menu did not send the chosen history text");
        QObject::disconnect(setupConnection);
        QObject::disconnect(sendConnection);
    }
    {
        // #137: own-window browsing uses the actual QML selection and wraps
        // without the external-target hold's release-to-paste hint.
        auto* entries = visualNamed(window->contentItem(), u"clipboardEntries"_s);
        check(entries && entries->property("count").toInt() >= 2, "browse fixture needs multiple entries");
        if (entries) {
            QMetaObject::invokeMethod(page, "selectFirst");
            const int count = entries->property("count").toInt();
            const QString before = clipboard.statusText();
            for (int i = 1; i <= count + 2; ++i) {
                emit clipboard.browseStepRequested();
                check(entries->property("currentIndex").toInt() == i % count, "hotkey browsing did not cycle rows");
            }
            check(!clipboard.holdHint() && clipboard.statusText() == before, "browsing started a paste or a release hint");
            say(u"clipboard browse hotkey: production QML cycles and wraps without a release paste"_s);
        }
    }
    if (!galleryImage.isEmpty()) {
        std::vector<qint64> ids;
        for (const auto& clip : store.clips()) ids.push_back(clip.id);
        store.remove(ids);
        const auto now = QDateTime::currentMSecsSinceEpoch();
        store.add(ClipCapture {.kind = ClipKind::Text,
            .text = u"会议纪要：周五前确认年度报告，补齐销售图表和项目验收记录。"_s, .source = u"记事本"_s}, now - 180000);
        store.add(ClipCapture {.kind = ClipKind::Link,
            .text = u"https://github.com/LingCore/WinShun"_s, .source = u"浏览器"_s}, now - 120000);
        const QString imageFile = temporary.filePath(u"区域销售额.png"_s);
        check(QFile::copy(galleryImage, imageFile), "could not copy the gallery image into the temporary history");
        store.add(ClipCapture {.kind = ClipKind::Files,
            .text = QDir::toNativeSeparators(imageFile), .source = u"资源管理器"_s}, now - 60000);
        store.add(ClipCapture {.kind = ClipKind::Text,
            .text = u"第三季度销售额：华东 428 万，华南 395 万，西南 371 万。"_s, .source = u"Excel"_s}, now);
        clipboard.handleShown();
        settle(window);
        check(saveGrab(window, output.filePath(u"clipboard-gallery.png"_s)), "could not render the clipboard gallery");
        // Notes in colour (GitHub #398) for the poster: entries noted as the
        // release notes tell, the newest one's box open on its colours.
        std::vector<qint64> shown;
        for (const auto& clip : store.clips()) shown.push_back(clip.id);
        store.remove(shown);
        struct Noted { ClipKind kind; QString text, source, note; int color; };
        const Noted rows[] { // oldest first, as copied; colours 1 red … 7 grey
            {ClipKind::Text, u"SELECT region, SUM(amount) FROM sales GROUP BY region;"_s, u"记事本"_s, u"常用查询"_s, 6},
            {ClipKind::Link, u"https://github.com/LingCore/WinShun"_s, u"浏览器"_s, {}, 0},
            {ClipKind::Text, u"下周一 10:00 前提交董事会汇报讲稿"_s, u"记事本"_s, u"待办"_s, 1},
            {ClipKind::Files, QDir::toNativeSeparators(imageFile), u"资源管理器"_s, u"汇报用图"_s, 2},
            {ClipKind::Text, u"demo@example.com"_s, u"浏览器"_s, u"测试账号"_s, 5},
            {ClipKind::Text, u"上海市浦东新区张江路 88 号 3 号楼 502 室"_s, u"微信"_s, u"收件地址"_s, 4},
        };
        std::vector<std::pair<qint64, const Noted*>> added;
        qint64 at = now - 600000;
        for (const Noted& row : rows)
            added.emplace_back(store.add(ClipCapture {.kind = row.kind, .text = row.text, .source = row.source}, at += 60000), &row);
        clipboard.handleShown();
        for (const auto& [id, row] : added)
            if (!row->note.isEmpty()) clipboard.setNote(id, row->note, row->color);
        settle(window);
        clipboard.trigger(0, Clipboard::EditNote);
        settle(window);
        check(saveGrab(window, output.filePath(u"clipboard-notes.png"_s)), "could not render the notes in colour");
        QMetaObject::invokeMethod(page, "focusSearch"); // the box done with, as a user leaves it
        settle(window);
    }
    check(!watcher.running(), "fixture must never start the native clipboard writer");
    window->hide();
    say(u"clipboard double-click preview and list resizing: real QML, temporary history; native clipboard untouched"_s);
    return ok;
}

// --clipboard-crashed-reading: a picture whose reading the program did not
// live through (the recognizer's guard file, found at a start) is read once
// more, after the others; the second time it is given up as a picture
// without words, and later starts leave it alone. Three starts of the real
// Clipboard over a temporary history; nothing is recognized.
bool checkClipboardCrashedReading()
{
    bool ok = true;
    const auto check = [&](bool passed, const char* what) {
        if (!passed) {
            say(u"crashed reading: "_s + QString::fromUtf8(what));
            ok = false;
        }
    };
    QTemporaryDir temporary;
    const QString folder = temporary.filePath(u"clipboard"_s);
    const QString guard = folder + u"/reading-picture"_s;
    qint64 id = 0;
    {
        ClipStore store(folder);
        check(store.open(QDateTime::currentMSecsSinceEpoch()), "could not open the temporary history");
        QImage image(64, 32, QImage::Format_RGB32);
        image.fill(Qt::white);
        const QString png = temporary.filePath(u"picture.png"_s);
        QFile file(png);
        check(image.save(png) && file.open(QIODevice::ReadOnly), "could not make the picture");
        ClipCapture picture;
        picture.kind = ClipKind::Image;
        picture.imageData = file.readAll();
        picture.hash = "crashed-reading";
        picture.width = image.width();
        picture.height = image.height();
        id = store.add(picture, QDateTime::currentMSecsSinceEpoch());
        store.close(); // its file written
    }
    check(id > 0, "the picture was not kept");
    // A start after a run that ended while reading it (or not), then what it left.
    const auto start = [&](bool endedWhileReading, const auto& look) {
        if (endedWhileReading) {
            QFile file(guard);
            check(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(QByteArray::number(id)) > 0,
                "could not leave the guard file");
        }
        ClipStore store(folder);
        check(store.open(QDateTime::currentMSecsSinceEpoch()), "could not open the history again");
        ClipboardWatcher watcher({}, {});
        Clipboard clipboard(&store, &watcher); // gone again long before its backlog's 20 s
        const std::vector<qint64> unread = store.unreadPictures();
        look(store.value(u"crashedReading"_s), std::ranges::find(unread, id) != unread.end());
        check(!QFile::exists(guard), "the guard file was left behind");
    };
    start(true, [&](const QString& noted, bool unread) {
        check(noted == QString::number(id), "the first time, the picture was not noted");
        check(unread, "the first time, the picture was given up already");
    });
    start(true, [&](const QString& noted, bool unread) {
        check(noted.isEmpty(), "the second time, the note stayed");
        check(!unread, "the second time, the picture was still to be read");
    });
    start(false, [&](const QString& noted, bool unread) {
        check(noted.isEmpty() && !unread, "a start after that went back to the picture");
    });
    say(u"crashed reading: read once more the first time, given up the second, left alone after: %1"_s
            .arg(ok ? u"checked"_s : u"FAILED"_s));
    return ok;
}

// --clipboard-file (GitHub #324): one copied text file, in the preview
// beside the clipboard's list, shows as the search's preview shows it (its
// lines, FilePreview) in place of the list of files; another row's entry
// closes it, coming back shows it again, the window put away closes it. The
// hover's light preview keeps the file's start (Clipboard.preview). A
// temporary history and file; the native clipboard untouched.
bool checkClipboardFilePreview(QQmlEngine* engine, const QDir& output)
{
    QTemporaryDir temporary;
    ClipStore store(temporary.filePath(u"history"_s));
    bool ok = true;
    const auto check = [&](bool condition, const char* reason) {
        ok = condition && ok;
        if (!condition) qWarning("clipboard file preview regression: %s", reason);
    };
    const auto waitFor = [](const auto& condition, int ms) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < ms)
            pump(50);
        return condition();
    };
    if (!store.open(QDateTime::currentMSecsSinceEpoch())) { check(false, "temporary history could not open"); return false; }
    const QString textFile = QDir::toNativeSeparators(temporary.filePath(u"会议纪要.md"_s));
    {
        QFile file(textFile);
        check(file.open(QIODevice::WriteOnly) && file.write(u"# 会议纪要\n\n- 周五前确认年度报告\n- 补齐销售图表\n"_s.toUtf8()) > 0,
            "the text file could not be written");
    }
    const QString pictureFile = QDir::toNativeSeparators(temporary.filePath(u"图表.png"_s));
    {
        QImage picture(240, 160, QImage::Format_RGB32);
        picture.fill(QColor(0x2f, 0x7d, 0xd8));
        check(picture.save(pictureFile), "the picture file could not be written");
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    store.add(ClipCapture {.kind = ClipKind::Files, .text = pictureFile, .source = u"资源管理器"_s}, now - 2000);
    store.add(ClipCapture {.kind = ClipKind::Text, .text = u"一段文字"_s, .source = u"记事本"_s}, now - 1000);
    store.add(ClipCapture {.kind = ClipKind::Files, .text = textFile, .source = u"资源管理器"_s}, now);
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    engine->addImageProvider(u"clippreview"_s, clipboard.filePreview()->imageProvider());
    clipboard.setRowOptions(false, u"standard"_s, SystemTheme::uiSize());
    clipboard.setActive(true);
    clipboard.handleShown();
    Placement placement(temporary.filePath(u"state.ini"_s), u"Clipboard"_s);
    WindowFrame frame(false);
    QQmlComponent component(engine, u"WinShun"_s, u"ClipboardWindow"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"placement"_s, QVariant::fromValue(&placement)}, {u"frame"_s, QVariant::fromValue(&frame)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { qWarning().noquote() << "clipboard file preview regression:" << component.errorString(); return false; }
    window->create();
    frame.setWindow(window);
    placement.setWindow(window);
    placement.enableWindowSizing();
    showOffScreen(window);
    clipboard.handlePlaced();
    clipboard.setSeen(true); // as App does once the window is uncloaked
    settle(window);
    auto* page = visualNamed(window->contentItem(), u"clipboardPage"_s);
    auto* entries = visualNamed(window->contentItem(), u"clipboardEntries"_s);
    if (!page || !entries) { check(false, "production clipboard page missing"); return false; }
    FilePreview* preview = clipboard.filePreview();
    const auto fileView = [&] { return visualNamed(window->contentItem(), u"clipboardPreviewFile"_s); };
    const auto content = [&] { return visualNamed(window->contentItem(), u"clipboardPreviewContent"_s); };
    const auto shownAsFile = [&] {
        QQuickItem* view = fileView();
        QQuickItem* list = content();
        return view && view->isVisible() && list && !list->isVisible();
    };
    QMetaObject::invokeMethod(page, "togglePreview", Q_ARG(QVariant, QVariant(0)));
    settle(window);
    check(page->property("previewOpen").toBool() && entries->property("currentIndex").toInt() == 0,
        "the copied file's row did not open the preview");
    check(shownAsFile(), "one copied file does not show as the search's preview");
    check(waitFor([&] { return preview->kind() == FilePreview::Text; }, 5000) && preview->lineCount() >= 4
            && preview->lineAt(0) == u"# 会议纪要" && !preview->summary().isEmpty(),
        "the copied text file's lines are not shown");
    // Its lines picked with the mouse: what Ctrl+C copies (not copied here).
    preview->setSelection(2, 2, 3, 4);
    check(preview->selectedText() == u"周五前确认年度报告\n- 补齐", "the picked lines are not what Ctrl+C would copy");
    preview->setSelection(0, 0, 0, 0);
    settle(window);
    check(saveGrab(window, output.filePath(u"clipboard-file-text.png"_s)), "the copied file's preview did not render");
    // The hover's preview: still the light look, the file's start.
    check(waitFor([&] { return !clipboard.preview(0).value(u"fileText"_s).toString().isEmpty(); }, 5000)
            && clipboard.preview(0).value(u"filePath"_s).toString() == textFile,
        "the entry no longer tells the file and its start");
    // Another row's entry: the list's own look, the file closed.
    entries->setProperty("currentIndex", 1);
    settle(window);
    check(!preview->isOpen() && fileView() && !fileView()->isVisible() && content() && content()->isVisible(),
        "a text entry still shows the file's preview");
    // A copied picture: its pixels, through this preview's own image provider.
    entries->setProperty("currentIndex", 2);
    settle(window);
    check(shownAsFile() && waitFor([&] { return preview->kind() == FilePreview::Picture; }, 5000)
            && preview->imageUrl().startsWith(u"image://clippreview/"),
        "the copied picture is not shown as a picture");
    settle(window);
    auto* picture = visualNamed(window->contentItem(), u"previewPicture"_s);
    check(picture && picture->property("status").toInt() == 1 && picture->property("sourceSize").toSize() == QSize(240, 160)
            && picture->width() > 0,
        "the copied picture's pixels did not load");
    check(saveGrab(window, output.filePath(u"clipboard-file-picture.png"_s)), "the copied picture's preview did not render");
    entries->setProperty("currentIndex", 0);
    settle(window);
    check(shownAsFile() && waitFor([&] { return preview->kind() == FilePreview::Text; }, 5000),
        "the copied file did not show again");
    clipboard.setSeen(false); // put away (or left behind the display process's window)
    check(!preview->isOpen(), "the file stays open with the window away");
    check(!watcher.running(), "fixture must never start the native clipboard writer");
    window->hide();
    say(u"clipboard file preview: a copied text file shows its lines beside the list, follows rows, closes with the window"_s);
    return ok;
}

// --clipboard-pin-files: pictures copied as files (微信, QQ, Explorer) go on
// the screen and into the editing window as the history's own pictures do:
// 贴到屏幕 (F3) and 编辑 (Ctrl+E) in a 文件 entry's menu when every file is
// a picture Windows reads; several at once, each in a slot of its own, at
// most Clipboard::kPinAtOnce; picked entries together; files gone said so.
// A temporary history; the native clipboard untouched; nothing is pinned
// (the requests are caught where App would hand them to the plugin).
bool checkClipboardPinFiles(QQmlEngine* engine, const QDir& output)
{
    QTemporaryDir temporary;
    ClipStore store(temporary.filePath(u"history"_s));
    bool ok = true;
    const auto check = [&](bool condition, const char* reason) {
        ok = condition && ok;
        if (!condition) qWarning("clipboard pin files regression: %s", reason);
    };
    if (!store.open(QDateTime::currentMSecsSinceEpoch())) { check(false, "temporary history could not open"); return false; }
    const auto pictureFile = [&](const QString& name, const QColor& color) {
        const QString path = QDir::toNativeSeparators(temporary.filePath(name));
        QImage picture(160, 120, QImage::Format_RGB32);
        picture.fill(color);
        check(picture.save(path), "a picture file could not be written");
        return path;
    };
    const QString chat = pictureFile(u"微信图片_20261124.png"_s, QColor(0x2f, 0x7d, 0xd8));
    const QString second = pictureFile(u"截图 2.bmp"_s, QColor(0x3c, 0xa0, 0x5a));
    const QString notes = QDir::toNativeSeparators(temporary.filePath(u"说明.txt"_s));
    {
        QFile file(notes);
        check(file.open(QIODevice::WriteOnly) && file.write("notes") > 0, "the text file could not be written");
    }
    const QString cleared = QDir::toNativeSeparators(temporary.filePath(u"已清理的缓存.jpg"_s)); // never written
    QStringList dozen;
    for (int i = 1; i <= Clipboard::kPinAtOnce + 2; ++i)
        dozen.append(pictureFile(u"照片 %1.png"_s.arg(i), QColor::fromHsv(i * 25, 160, 220)));
    QByteArray png;
    {
        QBuffer buffer(&png);
        QImage picture(200, 140, QImage::Format_RGB32);
        picture.fill(QColor(0xd8, 0x6a, 0x2f));
        check(buffer.open(QIODevice::WriteOnly) && picture.save(&buffer, "PNG"), "the picture could not be made");
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Newest first: rows 0 (one picture from 微信) to 6 (a dozen photos).
    store.add(ClipCapture {.kind = ClipKind::Files, .text = dozen.join(u'\n'), .source = u"资源管理器"_s}, now - 6000);
    store.add(ClipCapture {.kind = ClipKind::Files, .text = cleared, .source = u"微信"_s}, now - 5000);
    store.add(ClipCapture {.kind = ClipKind::Files, .text = chat + u'\n' + notes, .source = u"资源管理器"_s}, now - 4000);
    ClipCapture copied;
    copied.kind = ClipKind::Image;
    copied.imageData = png;
    copied.hash = "pin-files";
    copied.width = 200;
    copied.height = 140;
    copied.source = u"QQ"_s;
    const qint64 pictureId = store.add(copied, now - 3000);
    store.add(ClipCapture {.kind = ClipKind::Text, .text = u"一段文字"_s, .source = u"记事本"_s}, now - 2000);
    store.add(ClipCapture {.kind = ClipKind::Files, .text = chat + u'\n' + second, .source = u"资源管理器"_s}, now - 1000);
    store.add(ClipCapture {.kind = ClipKind::Files, .text = chat, .source = u"微信"_s}, now);
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    engine->addImageProvider(u"clippreview"_s, clipboard.filePreview()->imageProvider());
    clipboard.setRowOptions(false, u"standard"_s, SystemTheme::uiSize());
    clipboard.setActive(true);
    clipboard.handleShown();
    Placement placement(temporary.filePath(u"state.ini"_s), u"Clipboard"_s);
    WindowFrame frame(false);
    QQmlComponent component(engine, u"WinShun"_s, u"ClipboardWindow"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"placement"_s, QVariant::fromValue(&placement)}, {u"frame"_s, QVariant::fromValue(&frame)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { qWarning().noquote() << "clipboard pin files regression:" << component.errorString(); return false; }
    window->create();
    frame.setWindow(window);
    placement.setWindow(window);
    placement.enableWindowSizing();
    showOffScreen(window);
    clipboard.handlePlaced();
    clipboard.setSeen(true);
    settle(window);
    auto* page = visualNamed(window->contentItem(), u"clipboardPage"_s);
    auto* entries = visualNamed(window->contentItem(), u"clipboardEntries"_s);
    if (!page || !entries) { check(false, "production clipboard page missing"); return false; }

    struct Asked {
        qint64 id = 0;
        QString file;
        int slot = -1;
        bool operator==(const Asked&) const = default;
    };
    std::vector<Asked> pinned;
    std::vector<Asked> edited;
    int dismissed = 0;
    QObject::connect(&clipboard, &Clipboard::pinRequested, &clipboard,
        [&](qint64 id, const QString& file, int slot) { pinned.push_back({id, file, slot}); });
    QObject::connect(&clipboard, &Clipboard::editRequested, &clipboard,
        [&](qint64 id, const QString& file) { edited.push_back({id, file}); });
    QObject::connect(&clipboard, &Clipboard::dismissRequested, &clipboard, [&] { dismissed += edited.empty(); });
    const auto item = [&](int row, int action) {
        for (const QVariant& entry : clipboard.menuItems(row)) {
            const QVariantMap map = entry.toMap();
            if (map.contains(u"action"_s) && map.value(u"action"_s).toInt() == action)
                return map;
        }
        return QVariantMap();
    };
    const auto title = [&](int row, int action) { return item(row, action).value(u"text"_s).toString(); };
    const auto shortcut = [&](int row, int action) { return item(row, action).value(u"shortcut"_s).toString(); };
    const QString pinOne = Clipboard::tr("贴到屏幕");

    // No plugin that pins: nothing new in the menu.
    check(item(0, Clipboard::PinImage).isEmpty() && item(0, Clipboard::EditImage).isEmpty(),
        "a picture file offered 贴到屏幕 or 编辑 without the screenshot plugin");
    clipboard.setPictureVerbs(true, true);
    // One picture copied in 微信: as a picture of the history.
    check(title(0, Clipboard::PinImage) == pinOne && shortcut(0, Clipboard::PinImage) == u"F3"_s,
        "one copied picture file has no 贴到屏幕 (F3)");
    check(title(0, Clipboard::EditImage) == Clipboard::tr("编辑") && shortcut(0, Clipboard::EditImage) == u"Ctrl+E"_s,
        "one copied picture file has no 编辑 (Ctrl+E)");
    check(!item(0, Clipboard::Reveal).isEmpty() && item(0, Clipboard::OpenImage).isEmpty(),
        "a copied file's own entries changed");
    // Two: both go, the editing window takes one only.
    check(title(1, Clipboard::PinImage) == Clipboard::tr("贴 %Ln 张到屏幕", nullptr, 2) && item(1, Clipboard::EditImage).isEmpty(),
        "two copied picture files are not offered together");
    // Words and the history's own picture: as before.
    check(title(2, Clipboard::PinImage) == pinOne && item(2, Clipboard::EditImage).isEmpty(), "words lost their 贴到屏幕");
    check(title(3, Clipboard::PinImage) == pinOne && !item(3, Clipboard::EditImage).isEmpty()
            && !item(3, Clipboard::OpenImage).isEmpty(),
        "a copied picture lost its own entries");
    // A picture with a text file: not pictures.
    check(item(4, Clipboard::PinImage).isEmpty() && item(4, Clipboard::EditImage).isEmpty(),
        "a picture copied with a text file was offered as pictures");
    // A dozen: the first ten.
    check(title(6, Clipboard::PinImage) == Clipboard::tr("贴前 %Ln 张到屏幕", nullptr, Clipboard::kPinAtOnce),
        "a dozen copied pictures do not say only the first go");

    clipboard.trigger(1, Clipboard::PinImage);
    check(pinned == std::vector<Asked> {{0, chat, 0}, {0, second, 1}}, "two picture files were not pinned in order, a slot each");
    pinned.clear();
    clipboard.trigger(6, Clipboard::PinImage);
    check(std::ssize(pinned) == Clipboard::kPinAtOnce && pinned.front() == Asked {0, dozen.front(), 0}
            && pinned.back() == Asked {0, dozen[Clipboard::kPinAtOnce - 1], Clipboard::kPinAtOnce - 1},
        "a dozen pictures did not pin the first ten in their slots");
    pinned.clear();
    // 微信 cleared its cache since: said, nothing asked of the plugin.
    clipboard.trigger(5, Clipboard::PinImage);
    check(pinned.empty() && clipboard.statusText() == Clipboard::tr("这些文件已经不存在了"), "a file gone was not said");
    clipboard.trigger(5, Clipboard::EditImage);
    check(edited.empty() && dismissed == 0, "a file gone was handed to the editing window");
    // Picked together: the history's picture and the file's, in the list's order.
    clipboard.toggleSelected(0);
    clipboard.toggleSelected(3);
    check(title(0, Clipboard::PinImage) == Clipboard::tr("贴 %Ln 张到屏幕", nullptr, 2) && item(0, Clipboard::EditImage).isEmpty(),
        "a picture file and a picture picked together are not offered together");
    clipboard.trigger(0, Clipboard::PinImage);
    const auto pinnedOnce = [&](qint64 id, const QString& file) {
        return std::ranges::count_if(pinned, [&](const Asked& a) { return a.id == id && a.file == file; }) == 1;
    };
    check(pinned.size() == 2 && pinned[0].slot == 0 && pinned[1].slot == 1 && pinnedOnce(0, chat) && pinnedOnce(pictureId, {}),
        "picked pictures were not pinned together");
    pinned.clear();
    clipboard.toggleSelected(2); // words among them: no 贴到屏幕 for all
    check(item(0, Clipboard::PinImage).isEmpty(), "words picked with pictures were offered as pictures");
    clipboard.clearSelection();
    settle(window);

    // The menu as shown (the screenshot plugin's entries under 打开所在位置).
    entries->setProperty("currentIndex", 0);
    settle(window);
    check(QMetaObject::invokeMethod(page, "openContextMenu", Q_ARG(QVariant, QVariant(0)), Q_ARG(QVariant, QVariant())),
        "clipboard context menu did not open");
    if (auto* menu = qobject_cast<QQuickWindow*>(page->property("contextMenu").value<QObject*>())) {
        settle(menu);
        check(saveGrab(menu, output.filePath(u"clipboard-pin-files-menu.png"_s)), "the menu did not render");
        QMetaObject::invokeMethod(menu, "dismiss");
        settle(window);
    } else {
        check(false, "production clipboard menu is missing");
    }
    check(saveGrab(window, output.filePath(u"clipboard-pin-files.png"_s)), "the clipboard window did not render");

    // Its keys with the menu closed (GitHub #379): F3 pins, Ctrl+E edits, the window going first.
    const auto key = [&](int code, Qt::KeyboardModifiers modifiers) {
        QKeyEvent press(QEvent::KeyPress, code, modifiers), release(QEvent::KeyRelease, code, modifiers);
        QCoreApplication::sendEvent(window, &press);
        QCoreApplication::sendEvent(window, &release);
        settle(window);
    };
    if (QQuickItem* field = visualNamed(window->contentItem(), u"clipboardSearchField"_s))
        field->forceActiveFocus(Qt::OtherFocusReason);
    key(Qt::Key_F3, Qt::NoModifier);
    check(pinned == std::vector<Asked> {{0, chat, 0}}, "F3 did not pin the copied picture file");
    key(Qt::Key_E, Qt::ControlModifier);
    check(edited == std::vector<Asked> {{0, chat}} && dismissed == 1, "Ctrl+E did not hand the picture file to the editing window");
    // The plugin turned off: gone again.
    clipboard.setPictureVerbs(false, false);
    check(item(0, Clipboard::PinImage).isEmpty() && item(0, Clipboard::EditImage).isEmpty(),
        "the picture verbs stayed with the plugin off");
    check(!watcher.running(), "fixture must never start the native clipboard writer");
    window->hide();
    say(u"clipboard pin files: picture files copied in 微信/QQ/Explorer pin (F3, a slot each, ten at most) and edit (Ctrl+E); native clipboard untouched"_s);
    return ok;
}

// --clipboard-entry-hotkey: an entry's own shortcut (Clip::hotkey) in the
// window. Its badge says the keys (in its group's place); the menu offers
// 设置快捷键… on one entry, 更改快捷键（keys）… and 去掉快捷键 once it has
// some, nothing on several picked; 设置快捷键 asks the settings for it
// (hotkeyRequested), 去掉快捷键 takes the keys away and leaves it pinned.
// A temporary history; the native clipboard untouched. With poster (the
// --entry-hotkey-poster flag): clipboard-hotkeys-poster.png after.
bool checkClipboardEntryHotkey(QQmlEngine* engine, const QDir& output, bool poster)
{
    QTemporaryDir temporary;
    ClipStore store(temporary.filePath(u"history"_s));
    bool ok = true;
    const auto check = [&](bool condition, const char* reason) {
        ok = condition && ok;
        if (!condition) qWarning("clipboard entry hotkey regression: %s", reason);
    };
    if (!store.open(QDateTime::currentMSecsSinceEpoch())) { check(false, "temporary history could not open"); return false; }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Newest first: rows 0 (no keys) to 2 (keys, a note).
    const qint64 mailId = store.add(ClipCapture {.kind = ClipKind::Text, .text = u"lingcore@example.com"_s, .source = u"浏览器"_s}, now - 3000);
    const qint64 addressId = store.add(ClipCapture {.kind = ClipKind::Text,
        .text = u"上海市浦东新区世纪大道 100 号 3 号楼 502 室"_s, .source = u"微信"_s}, now - 2000);
    store.add(ClipCapture {.kind = ClipKind::Text, .text = u"会议纪要：周五前确认年度报告。"_s, .source = u"记事本"_s}, now - 1000);
    check(store.setHotkey(mailId, u"Ctrl+Alt+1"_s) && store.setHotkey(addressId, u"Ctrl+Alt+D"_s), "the keys were not kept");
    store.setNote(mailId, u"工作邮箱"_s, 0);

    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    engine->addImageProvider(u"clippreview"_s, clipboard.filePreview()->imageProvider());
    clipboard.setRowOptions(false, u"standard"_s, SystemTheme::uiSize());
    clipboard.setActive(true);
    clipboard.handleShown();
    Placement placement(temporary.filePath(u"state.ini"_s), u"Clipboard"_s);
    WindowFrame frame(false);
    QQmlComponent component(engine, u"WinShun"_s, u"ClipboardWindow"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"placement"_s, QVariant::fromValue(&placement)}, {u"frame"_s, QVariant::fromValue(&frame)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { qWarning().noquote() << "clipboard entry hotkey regression:" << component.errorString(); return false; }
    window->create();
    frame.setWindow(window);
    placement.setWindow(window);
    placement.enableWindowSizing();
    showOffScreen(window);
    clipboard.handlePlaced();
    clipboard.setSeen(true);
    settle(window);
    auto* page = visualNamed(window->contentItem(), u"clipboardPage"_s);
    if (!page) { check(false, "production clipboard page missing"); return false; }

    ClipModel& model = *clipboard.items();
    const auto rowOf = [&](qint64 id) {
        for (int row = 0; row < model.rowCount(); ++row)
            if (model.data(model.index(row), ClipModel::IdRole).toLongLong() == id)
                return row;
        return -1;
    };
    const auto texts = [&](int row) {
        QStringList words;
        for (const QVariant& entry : clipboard.menuItems(row))
            words << entry.toMap().value(u"text"_s).toString();
        return words;
    };
    const int mail = rowOf(mailId);
    const int plain = rowOf(store.clips().front().id);
    check(mail >= 0 && plain >= 0, "the entries are not listed");
    check(model.data(model.index(mail), ClipModel::HotkeyRole).toString() == u"Ctrl+Alt+1"_s
            && model.data(model.index(plain), ClipModel::HotkeyRole).toString().isEmpty(),
        "the rows do not say their keys");
    // The keys in the badge, in the rows as drawn (the current row shows its buttons there instead).
    auto* entries = visualNamed(window->contentItem(), u"clipboardEntries"_s);
    if (!entries) { check(false, "production clipboard list missing"); return false; }
    entries->setProperty("currentIndex", plain);
    settle(window);
    const std::function<bool(QQuickItem*, const QString&)> shows = [&shows](QQuickItem* item, const QString& words) {
        if (item->isVisible() && item->inherits("QQuickText") && item->property("text").toString() == words)
            return true;
        return std::ranges::any_of(item->childItems(), [&](QQuickItem* child) { return shows(child, words); });
    };
    check(shows(window->contentItem(), u"Ctrl+Alt+1"_s) && shows(window->contentItem(), u"Ctrl+Alt+D"_s),
        "the rows' badges do not show their keys");
    check(texts(plain).contains(Clipboard::tr("设置快捷键…")) && !texts(plain).contains(Clipboard::tr("去掉快捷键")),
        "an entry without keys is not offered 设置快捷键…");
    check(texts(mail).contains(Clipboard::tr("更改快捷键（%1）…").arg(u"Ctrl+Alt+1"_s))
            && texts(mail).contains(Clipboard::tr("去掉快捷键")),
        "an entry with keys is not offered 更改快捷键 and 去掉快捷键");
    clipboard.toggleSelected(mail);
    clipboard.toggleSelected(plain);
    check(!texts(mail).join(u'|').contains(u"快捷键"_s), "several picked are offered one shortcut");
    clipboard.clearSelection();

    // The menu as shown, then the window.
    check(QMetaObject::invokeMethod(page, "openContextMenu", Q_ARG(QVariant, QVariant(mail)), Q_ARG(QVariant, QVariant())),
        "clipboard context menu did not open");
    if (auto* menu = qobject_cast<QQuickWindow*>(page->property("contextMenu").value<QObject*>())) {
        settle(menu);
        check(saveGrab(menu, output.filePath(u"clipboard-entry-hotkey-menu.png"_s)), "the menu did not render");
        QMetaObject::invokeMethod(menu, "dismiss");
        settle(window);
    } else {
        check(false, "production clipboard menu is missing");
    }
    check(saveGrab(window, output.filePath(u"clipboard-entry-hotkey.png"_s)), "the clipboard window did not render");

    // 设置快捷键… asks the settings for that entry.
    qint64 asked = 0;
    QObject::connect(&clipboard, &Clipboard::hotkeyRequested, &clipboard, [&](qint64 id) { asked = id; });
    clipboard.trigger(plain, Clipboard::SetHotkey);
    check(asked == store.clips().front().id, "设置快捷键… did not ask for the entry's keys");
    // 去掉快捷键: gone from the entry and the window's list, still pinned.
    int listChanged = 0;
    QObject::connect(&clipboard, &Clipboard::entryHotkeysChanged, &clipboard, [&] { ++listChanged; });
    clipboard.trigger(rowOf(mailId), Clipboard::RemoveHotkey);
    const Clip* removed = store.clip(mailId);
    check(removed && removed->hotkey.isEmpty() && removed->group == ClipStore::kPinned, "去掉快捷键 did not take the keys away, or unpinned it");
    check(listChanged == 1 && clipboard.entryHotkeys().size() == 1 && clipboard.entryHotkeys().front().first == addressId,
        "the shortcuts to register were not said again");
    if (poster) { // the 0.47.0 poster: what a person keeps keys for, among what they copied lately
        std::vector<qint64> shown;
        for (const auto& clip : store.clips()) shown.push_back(clip.id);
        store.remove(shown);
        struct Entry { ClipKind kind; QString text, source, note, keys; };
        const Entry rows[] { // oldest first
            {ClipKind::Text, u"SELECT region, SUM(amount) FROM sales GROUP BY region;"_s, u"记事本"_s, {}, {}},
            {ClipKind::Text, u"上海市浦东新区张江路 88 号 3 号楼 502 室"_s, u"微信"_s, u"收件地址"_s, u"Ctrl+Alt+3"_s},
            {ClipKind::Link, u"https://github.com/LingCore/WinShun"_s, u"浏览器"_s, {}, {}},
            {ClipKind::Text, u"demo@example.com"_s, u"浏览器"_s, u"工作邮箱"_s, u"Ctrl+Alt+2"_s},
            {ClipKind::Text, u"会议纪要：周五前确认年度报告，补齐销售图表。"_s, u"记事本"_s, {}, {}},
            {ClipKind::Text, u"收到，我马上处理，稍后给您回复。"_s, u"微信"_s, {}, u"Ctrl+Alt+1"_s},
            {ClipKind::Text, u"第三季度销售额：华东 428 万，华南 395 万。"_s, u"Excel"_s, {}, {}},
        };
        qint64 at = now - 600000;
        for (const Entry& row : rows) {
            const qint64 id = store.add(ClipCapture {.kind = row.kind, .text = row.text, .source = row.source}, at += 60000);
            if (!row.note.isEmpty()) store.setNote(id, row.note, 0);
            if (!row.keys.isEmpty()) store.setHotkey(id, row.keys);
        }
        clipboard.handleShown();
        QMetaObject::invokeMethod(page, "focusSearch");
        entries->setProperty("currentIndex", 0); // the newest, without keys: every badge in sight
        settle(window);
        check(saveGrab(window, output.filePath(u"clipboard-hotkeys-poster.png"_s)), "the poster did not render");
    }
    check(!watcher.running(), "fixture must never start the native clipboard writer");
    window->hide();
    say(u"clipboard entry hotkey: badge shows the keys, menu sets/changes/removes them (one entry only), removing keeps it pinned; native clipboard untouched"_s);
    return ok;
}

// --clipboard-paste-plain: 默认粘贴为纯文本 (Settings::clipboardPlainByDefault).
// Off, as ever: a paste keeps the words' formatting, Shift drops it. On: the
// usual paste (Enter, a click, Alt+1…9) drops it, Shift+Enter and the menu's
// 保留格式粘贴 keep it; files and pictures paste as they are either way, a
// copy keeps every format. The status line, the shortcut list and the
// preview's hint say which. A temporary history; what each paste would
// write is caught before the native clipboard (PasteWriteProbe).
bool checkClipboardPastePlain(QQmlEngine* engine, const QDir& output)
{
    QTemporaryDir temporary;
    ClipStore store(temporary.filePath(u"history"_s));
    bool ok = true;
    const auto check = [&](bool condition, const char* reason) {
        ok = condition && ok;
        if (!condition) qWarning("clipboard paste plain regression: %s", reason);
    };
    if (!store.open(QDateTime::currentMSecsSinceEpoch())) { check(false, "temporary history could not open"); return false; }
    const QString notes = QDir::toNativeSeparators(temporary.filePath(u"会议纪要.docx"_s));
    {
        QFile file(notes);
        check(file.open(QIODevice::WriteOnly) && file.write("notes") > 0, "the file could not be written");
    }
    QByteArray png;
    {
        QBuffer buffer(&png);
        QImage picture(200, 140, QImage::Format_RGB32);
        picture.fill(QColor(0x2f, 0x7d, 0xd8));
        check(buffer.open(QIODevice::WriteOnly) && picture.save(&buffer, "PNG"), "the picture could not be made");
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Newest first: rows 0 (words from Word, formatted) to 3 (a picture).
    const qint64 pictureId = store.add(ClipCapture {.kind = ClipKind::Image, .imageData = png, .hash = "paste-plain",
        .width = 200, .height = 140, .source = u"QQ"_s}, now - 3000);
    const qint64 fileId = store.add(ClipCapture {.kind = ClipKind::Files, .text = notes, .source = u"资源管理器"_s}, now - 2000);
    const qint64 plainId = store.add(ClipCapture {.kind = ClipKind::Text, .text = u"记事本里的一行字"_s, .source = u"记事本"_s}, now - 1000);
    const QString words = u"季度总结：收入增长 12%"_s;
    const qint64 wordId = store.add(ClipCapture {.kind = ClipKind::Text, .text = words,
        .html = "Version:0.9 <html><body><!--StartFragment--><b>季度总结</b>：收入增长 <span style=\"color:red\">12%</span><!--EndFragment--></body></html>",
        .rtf = "{\\rtf1\\ansi {\\b Q}}", .source = u"Microsoft Word"_s}, now);
    const Clip* word = store.clip(wordId);
    const Clip* plain = store.clip(plainId);
    const Clip* file = store.clip(fileId);
    const Clip* image = store.clip(pictureId);
    if (!word || !plain || !file || !image) { check(false, "the entries were not kept"); return false; }
    check(word->html && word->rtf && !plain->html && !plain->rtf, "the fixture's formatting is not as meant");

    // Which paste drops the formatting: words by the setting, Shift the other; the rest only by Shift.
    using Clips = std::vector<const Clip*>;
    check(!Clipboard::pastesPlain(Clips {word}, false, false) && Clipboard::pastesPlain(Clips {word}, false, true),
        "off: the usual paste of words is not the formatted one");
    check(Clipboard::pastesPlain(Clips {word}, true, false) && !Clipboard::pastesPlain(Clips {word}, true, true),
        "on: the usual paste of words kept the formatting");
    check(!Clipboard::pastesPlain(Clips {file}, true, false) && Clipboard::pastesPlain(Clips {file}, true, true)
            && !Clipboard::pastesPlain(Clips {image}, true, false) && !Clipboard::pastesPlain(Clips {word, file}, true, false)
            && !Clipboard::pastesPlain({}, true, false),
        "on: files or a picture went as plain text the usual way");

    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    engine->addImageProvider(u"clippreview"_s, clipboard.filePreview()->imageProvider());
    clipboard.setRowOptions(false, u"standard"_s, SystemTheme::uiSize());
    clipboard.setActive(true);
    clipboard.handleShown();
    Placement placement(temporary.filePath(u"state.ini"_s), u"Clipboard"_s);
    WindowFrame frame(false);
    QQmlComponent component(engine, u"WinShun"_s, u"ClipboardWindow"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"placement"_s, QVariant::fromValue(&placement)}, {u"frame"_s, QVariant::fromValue(&frame)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { qWarning().noquote() << "clipboard paste plain regression:" << component.errorString(); return false; }
    window->create();
    frame.setWindow(window);
    placement.setWindow(window);
    placement.enableWindowSizing();
    showOffScreen(window);
    clipboard.handlePlaced();
    clipboard.setSeen(true);
    settle(window);
    auto* page = visualNamed(window->contentItem(), u"clipboardPage"_s);
    auto* entries = visualNamed(window->contentItem(), u"clipboardEntries"_s);
    if (!page || !entries) { check(false, "production clipboard page missing"); return false; }

    const auto pasted = [&](int row, bool shift, bool quick = false) {
        if (quick)
            clipboard.quickPaste(row, shift);
        else
            clipboard.paste(row, shift);
        return PasteWriteProbe::takeWrite(clipboard);
    };
    const auto formatted = [](const ClipWrite& w) { return !w.html.isEmpty() && !w.rtf.isEmpty(); };
    const auto bare = [&](const ClipWrite& w) { return w.html.isEmpty() && w.rtf.isEmpty() && w.text == words; };
    const auto item = [&](int row, int action) {
        for (const QVariant& entry : clipboard.menuItems(row)) {
            const QVariantMap map = entry.toMap();
            if (map.contains(u"action"_s) && map.value(u"action"_s).toInt() == action)
                return map;
        }
        return QVariantMap();
    };
    const auto title = [&](int row, int action) { return item(row, action).value(u"text"_s).toString(); };
    const auto shortcut = [&](int row, int action) { return item(row, action).value(u"shortcut"_s).toString(); };
    const QString asPlain = Clipboard::tr("粘贴为纯文本");
    const QString keepFormat = Clipboard::tr("保留格式粘贴");
    // The shortcut list as the footer's info button gives it this window.
    const auto shortcutTexts = [&] {
        QQmlExpression list(qmlContext(page), page,
            u"Shortcuts.entries('clipboard', {numberModifier: 'Alt', intoField: false, holdKey: '', "
            u"plainByDefault: clipboard.pastePlain}, false).map(e => e.text).join('|')"_s);
        return list.evaluate().toString().split(u'|');
    };
    entries->setProperty("currentIndex", 0);
    QMetaObject::invokeMethod(page, "togglePreview", Q_ARG(QVariant, QVariant(0)));
    settle(window);
    auto* hint = visualNamed(window->contentItem(), u"clipboardPreviewHint"_s);
    check(hint && hint->isVisible(), "the preview of formatted words has no hint");
    const auto hintText = [&] {
        settle(window);
        return hint ? hint->property("text").toString() : QString();
    };

    // Off, as ever.
    check(!clipboard.pastePlain(), "默认粘贴为纯文本 is on before it was asked for");
    check(formatted(pasted(0, false)) && bare(pasted(0, true)), "off: Enter dropped or Shift+Enter kept the formatting");
    check(title(0, Clipboard::PasteOther) == asPlain && shortcut(0, Clipboard::PasteOther) == u"Shift+Enter"_s,
        "off: the menu lost 粘贴为纯文本 (Shift+Enter)");
    check(item(1, Clipboard::PasteOther).value(u"text"_s) == asPlain, "off: words without formatting lost 粘贴为纯文本");
    QStringList texts = shortcutTexts();
    check(texts.contains(u"粘贴为纯文本"_s) && texts.contains(u"第几条粘贴为纯文本"_s) && !texts.contains(u"保留格式粘贴"_s),
        "off: the shortcut list changed");
    check(hintText().contains(u"Shift+Enter 粘贴为纯文本"_s), "off: the preview's hint changed");

    // On.
    int optionsChanged = 0;
    QObject::connect(&clipboard, &Clipboard::rowOptionsChanged, &clipboard, [&] { ++optionsChanged; });
    clipboard.setPastePlain(true);
    check(clipboard.pastePlain() && optionsChanged == 1, "默认粘贴为纯文本 was not taken");
    clipboard.setPastePlain(true);
    check(optionsChanged == 1, "the same setting again was said as a change");
    check(bare(pasted(0, false)) && bare(pasted(0, false, true)), "on: Enter or Alt+1 kept the formatting");
    check(formatted(pasted(0, true)) && formatted(pasted(0, true, true)), "on: Shift+Enter or Alt+Shift+1 dropped the formatting");
    check(title(0, Clipboard::PasteOther) == keepFormat && shortcut(0, Clipboard::PasteOther) == u"Shift+Enter"_s,
        "on: formatted words have no 保留格式粘贴 (Shift+Enter)");
    check(item(1, Clipboard::PasteOther).isEmpty(), "on: words without formatting offer a second paste that is the same");
    check(pasted(1, false).text == u"记事本里的一行字"_s, "on: words without formatting did not paste");
    // Files as files, their paths by Shift; the picture as itself.
    const ClipWrite files = pasted(2, false);
    check(files.files == QStringList {notes} && files.text.isEmpty() && pasted(2, true).text == notes,
        "on: a copied file went as its path, or Shift+Enter lost the path");
    check(title(2, Clipboard::PasteOther) == asPlain, "on: a copied file lost 粘贴为纯文本");
    check(!pasted(3, false).imagePath.isEmpty() && item(3, Clipboard::PasteOther).isEmpty(),
        "on: the picture did not paste as a picture");
    // Several picked go as their joined words either way: nothing to keep.
    clipboard.toggleSelected(0);
    clipboard.toggleSelected(1);
    check(item(0, Clipboard::PasteOther).isEmpty(), "on: several picked offer 保留格式粘贴, which keeps nothing");
    clipboard.clearSelection();
    // A copy keeps every format.
    check(clipboard.pasteEntry(wordId, nullptr) && formatted(PasteWriteProbe::takeWrite(clipboard)),
        "on: 复制 dropped the formatting");
    // What the window says.
    clipboard.setPinned(true);
    check(clipboard.statusText().endsWith(u"双击粘贴纯文本"_s), "on: the pinned status line does not say a double click pastes plain text");
    clipboard.setRowOptions(true, u"standard"_s, SystemTheme::uiSize());
    check(clipboard.statusText().endsWith(u"单击粘贴纯文本"_s), "on: the pinned status line does not say a click pastes plain text");
    clipboard.setRowOptions(false, u"standard"_s, SystemTheme::uiSize());
    clipboard.setPinned(false);
    texts = shortcutTexts();
    check(texts.contains(u"保留格式粘贴"_s) && texts.contains(u"第几条保留格式粘贴"_s) && !texts.contains(u"粘贴为纯文本"_s),
        "on: the shortcut list still says Shift pastes plain text");
    check(hintText().contains(u"Shift+Enter 保留格式粘贴"_s), "on: the preview's hint still says Shift+Enter pastes plain text");

    // The menu as shown.
    check(QMetaObject::invokeMethod(page, "openContextMenu", Q_ARG(QVariant, QVariant(0)), Q_ARG(QVariant, QVariant())),
        "clipboard context menu did not open");
    if (auto* menu = qobject_cast<QQuickWindow*>(page->property("contextMenu").value<QObject*>())) {
        settle(menu);
        check(saveGrab(menu, output.filePath(u"clipboard-paste-plain-menu.png"_s)), "the menu did not render");
        QMetaObject::invokeMethod(menu, "dismiss");
        settle(window);
    } else {
        check(false, "production clipboard menu is missing");
    }
    check(saveGrab(window, output.filePath(u"clipboard-paste-plain.png"_s)), "the clipboard window did not render");
    check(!watcher.running(), "fixture must never start the native clipboard writer");
    window->hide();
    say(u"clipboard paste plain: 默认粘贴为纯文本 drops the words' formatting on Enter/click/Alt+1, Shift+Enter keeps it; files, pictures and copies as ever; native clipboard untouched"_s);
    return ok;
}

// --clipboard-picture-view (GitHub #414): a copied picture's words folded
// under their heading until asked for (remembered); a click on the picture,
// or Space with nothing typed, shows it large beside the window
// (ClipPictureViewer), zoomed by the wheel; ↑ ↓ to a row without a picture,
// Esc, the window put away close it. One copied picture file too, through
// its file preview's pixels. A temporary history; the native clipboard untouched.
// With --gallery-image <picture>: the poster, clipboard-picture.png.
bool checkClipboardPictureView(QQmlEngine* engine, const QDir& output, const QString& galleryImage = {})
{
    QTemporaryDir temporary;
    ClipStore store(temporary.filePath(u"history"_s));
    bool ok = true;
    const auto check = [&](bool condition, const char* reason) {
        ok = condition && ok;
        if (!condition) qWarning("clipboard picture view regression: %s", reason);
    };
    const auto waitFor = [](const auto& condition, int ms) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < ms)
            pump(50);
        return condition();
    };
    if (!store.open(QDateTime::currentMSecsSinceEpoch())) { check(false, "temporary history could not open"); return false; }
    const auto drawn = [](QSize size, QColor a, QColor b) { // squares, so a zoom shows
        QImage image(size, QImage::Format_RGB32);
        QPainter painter(&image);
        for (int y = 0; y < size.height(); y += 40)
            for (int x = 0; x < size.width(); x += 40)
                painter.fillRect(x, y, 40, 40, ((x + y) / 40) % 2 ? a : b);
        painter.setPen(Qt::black);
        painter.setFont(QFont(u"Microsoft YaHei UI"_s, 48));
        painter.drawText(QRect(QPoint(), size), Qt::AlignCenter, u"Win顺 #414"_s);
        return image;
    };
    const QString pictureFile = QDir::toNativeSeparators(temporary.filePath(u"截图.png"_s));
    check(drawn({1200, 800}, QColor(0xd8, 0xe8, 0xfb), QColor(0xf4, 0xf8, 0xfe)).save(pictureFile),
        "the picture file could not be written");
    QByteArray png;
    {
        QBuffer buffer(&png);
        check(buffer.open(QIODevice::WriteOnly) && drawn({2400, 1500}, QColor(0xfb, 0xe3, 0xc8), QColor(0xfe, 0xf6, 0xec))
                                                       .save(&buffer, "PNG"),
            "the picture could not be made");
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    store.add(ClipCapture {.kind = ClipKind::Files, .text = pictureFile, .source = u"资源管理器"_s}, now - 2000);
    store.add(ClipCapture {.kind = ClipKind::Text, .text = u"一段文字"_s, .source = u"记事本"_s}, now - 1000);
    ClipCapture picture;
    picture.kind = ClipKind::Image;
    picture.imageData = png;
    picture.hash = "picture-view";
    picture.width = 2400;
    picture.height = 1500;
    picture.source = u"PixPin"_s;
    const qint64 pictureId = store.add(picture, now);
    store.setPictureText(pictureId, u"Win顺 #414\n图里的第二行字"_s);
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    engine->addImageProvider(u"clippreview"_s, clipboard.filePreview()->imageProvider());
    clipboard.setRowOptions(false, u"standard"_s, SystemTheme::uiSize());
    clipboard.setActive(true);
    clipboard.handleShown();
    Placement placement(temporary.filePath(u"state.ini"_s), u"Clipboard"_s);
    WindowFrame frame(false);
    QQmlComponent component(engine, u"WinShun"_s, u"ClipboardWindow"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"placement"_s, QVariant::fromValue(&placement)}, {u"frame"_s, QVariant::fromValue(&frame)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { qWarning().noquote() << "clipboard picture view regression:" << component.errorString(); return false; }
    window->create();
    frame.setWindow(window);
    placement.setWindow(window);
    placement.enableWindowSizing();
    showOffScreen(window);
    clipboard.handlePlaced();
    clipboard.setSeen(true);
    settle(window);
    auto* page = visualNamed(window->contentItem(), u"clipboardPage"_s);
    auto* entries = visualNamed(window->contentItem(), u"clipboardEntries"_s);
    if (!page || !entries) { check(false, "production clipboard page missing"); return false; }
    const auto named = [&](const QString& name) { return visualNamed(window->contentItem(), name); };
    const auto viewer = [&]() -> QQuickWindow* {
        for (QWindow* shown : QGuiApplication::topLevelWindows())
            if (shown->objectName() == u"clipboardPictureViewer"_s && shown->isVisible())
                return qobject_cast<QQuickWindow*>(shown);
        return nullptr;
    };
    const auto viewerItem = [&](const QString& name) { return viewer() ? visualNamed(viewer()->contentItem(), name) : nullptr; };
    const auto press = [&](int code, const QString& text = {}) {
        QKeyEvent down(QEvent::KeyPress, code, {}, text);
        QKeyEvent up(QEvent::KeyRelease, code, {}, text);
        QCoreApplication::sendEvent(window, &down);
        QCoreApplication::sendEvent(window, &up);
        pump(50);
    };

    // The picture's row in the preview: its words folded, then shown and remembered.
    QMetaObject::invokeMethod(page, "togglePreview", Q_ARG(QVariant, QVariant(0)));
    settle(window);
    check(page->property("previewOpen").toBool() && entries->property("currentIndex").toInt() == 0,
        "the picture's row did not open the preview");
    auto* words = named(u"clipboardPictureWords"_s);
    auto* heading = named(u"clipboardPictureWordsToggle"_s);
    check(!clipboard.pictureWords() && heading && heading->isVisible() && words && !words->isVisible(),
        "a picture's words are not folded at first");
    check(saveGrab(window, output.filePath(u"clipboard-picture-folded.png"_s)), "the folded preview did not render");
    QMetaObject::invokeMethod(page, "togglePreview", Q_ARG(QVariant, QVariant(0)));
    settle(window);
    clipboard.setPictureWords(true);
    QMetaObject::invokeMethod(page, "togglePreview", Q_ARG(QVariant, QVariant(0)));
    settle(window);
    words = named(u"clipboardPictureWords"_s);
    // (Read again by Windows' own OCR, the words given may become what it reads.)
    check(words && words->isVisible() && !words->property("text").toString().isEmpty(),
        "the words do not show once asked for");
    check(store.value(u"pictureWords"_s) == u"shown"_s, "showing the words is not remembered");
    check(saveGrab(window, output.filePath(u"clipboard-picture-words.png"_s)), "the preview with words did not render");
    clipboard.setPictureWords(false);
    settle(window);

    // A click on the picture: large beside the window, fitted, then zoomed.
    auto* zoom = named(u"clipboardPictureZoom"_s);
    check(waitFor([&] { return zoom && zoom->property("enabled").toBool(); }, 5000), "the picture cannot be clicked");
    if (zoom) {
        const QPointF at = zoom->mapToScene(QPointF(zoom->width() / 2, zoom->height() / 2));
        QMouseEvent move(QEvent::MouseMove, at, window->mapToGlobal(at), Qt::NoButton, {}, {});
        QMouseEvent down(QEvent::MouseButtonPress, at, window->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, {});
        QMouseEvent up(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), Qt::LeftButton, {}, {});
        QCoreApplication::sendEvent(window, &move);
        QCoreApplication::sendEvent(window, &down);
        QCoreApplication::sendEvent(window, &up);
    }
    check(waitFor([&] { return viewer() != nullptr; }, 3000), "a click on the picture did not show it large");
    auto* image = viewerItem(u"clipboardPictureViewerImage"_s);
    check(waitFor([&] { image = viewerItem(u"clipboardPictureViewerImage"_s); return image && image->property("status").toInt() == 1; }, 5000),
        "the large picture did not load");
    if (QQuickWindow* large = viewer()) {
        const QRect host = window->geometry();
        const QRect area = large->screen()->availableGeometry();
        check(area.contains(large->geometry()), "the large picture is not on the screen");
        check(!large->geometry().intersects(host) || large->geometry().center().x() == area.center().x()
                  || std::abs(large->geometry().center().x() - area.center().x()) <= 1,
            "the large picture covers the window without being in the middle of the screen");
        check(large->width() > zoom->width() && large->height() > zoom->height(), "the large picture is not larger");
        settle(large);
        check(saveGrab(large, output.filePath(u"clipboard-picture-viewer.png"_s)), "the large picture did not render");
        auto* view = viewerItem(u"clipboardPictureViewerView"_s);
        const qreal fitted = view ? view->property("scale").toReal() : 0;
        check(view && QMetaObject::invokeMethod(view, "zoomAt", Q_ARG(QVariant, 3.0), Q_ARG(QVariant, view->width() / 2),
                                                Q_ARG(QVariant, view->height() / 2))
                  && std::abs(view->property("scale").toReal() - std::min(16.0, fitted * 3)) < 1e-6,
            "the large picture does not zoom");
        settle(large);
        check(saveGrab(large, output.filePath(u"clipboard-picture-viewer-zoomed.png"_s)), "the zoomed picture did not render");
    }
    check(page->property("pictureViewOpen").toBool(), "the page does not know the picture is shown large");

    // ↑ ↓ to a row without a picture: it goes.
    entries->setProperty("currentIndex", 1);
    settle(window);
    check(waitFor([&] { return viewer() == nullptr && !page->property("pictureViewOpen").toBool(); }, 2000),
        "the large picture stays over a text row");

    // Space with nothing typed: on a text row it is typed; on the picture's row it shows it, Esc closes it.
    QMetaObject::invokeMethod(page, "focusSearch");
    press(Qt::Key_Space, u" "_s);
    check(viewer() == nullptr && clipboard.query() == u" "_s, "Space on a text row was not typed");
    press(Qt::Key_Backspace);
    check(clipboard.query().isEmpty(), "the space typed did not go");
    entries->setProperty("currentIndex", 0);
    settle(window);
    press(Qt::Key_Space, u" "_s);
    check(waitFor([&] { return viewer() != nullptr; }, 3000) && clipboard.query().isEmpty(),
        "Space with nothing typed did not show the picture large");
    press(Qt::Key_Escape);
    check(waitFor([&] { return viewer() == nullptr; }, 2000) && window->isVisible(),
        "Esc did not close the large picture first");

    // One copied picture file: through its file preview's pixels.
    entries->setProperty("currentIndex", 2);
    settle(window);
    FilePreview* preview = clipboard.filePreview();
    check(waitFor([&] { return preview->kind() == FilePreview::Picture; }, 5000), "the copied picture file is not shown");
    settle(window);
    auto* badge = named(u"clipboardFilePictureZoom"_s);
    check(badge != nullptr, "the copied picture file has no way to be shown large");
    bool opened = false;
    QMetaObject::invokeMethod(page, "togglePictureView", Q_RETURN_ARG(bool, opened));
    check(opened && waitFor([&] { return viewer() != nullptr; }, 3000), "the copied picture file did not show large");
    check(waitFor([&] {
        image = viewerItem(u"clipboardPictureViewerImage"_s);
        return image && image->property("status").toInt() == 1
            && image->property("source").toString().startsWith(u"image://clippreview/"_s);
    }, 5000), "the copied picture file's pixels did not load large");
    if (QQuickWindow* large = viewer()) {
        settle(large);
        check(saveGrab(large, output.filePath(u"clipboard-picture-file-viewer.png"_s)), "the large picture file did not render");
    }

    // The poster (--gallery-image): a day's copies, the picture's row open
    // and large beside the window, the two windows side by side as on the
    // screen (their grabs put together: off the screen they are nowhere near
    // each other). Its words are left folded, as they come.
    if (!galleryImage.isEmpty() && ok) {
        bool closed = false;
        if (page->property("pictureViewOpen").toBool())
            QMetaObject::invokeMethod(page, "togglePictureView", Q_RETURN_ARG(bool, closed));
        waitFor([&] { return viewer() == nullptr; }, 2000);
        std::vector<qint64> ids;
        for (const auto& clip : store.clips()) ids.push_back(clip.id);
        store.remove(ids);
        QFile file(galleryImage);
        const QImage chart(galleryImage);
        check(file.open(QIODevice::ReadOnly) && !chart.isNull(), "the gallery picture could not be read");
        const qint64 at = QDateTime::currentMSecsSinceEpoch();
        store.add(ClipCapture {.kind = ClipKind::Text,
            .text = u"会议纪要：周五前确认年度报告，补齐销售图表和项目验收记录。"_s, .source = u"记事本"_s}, at - 240000);
        store.add(ClipCapture {.kind = ClipKind::Link, .text = u"https://github.com/LingCore/WinShun"_s, .source = u"浏览器"_s},
            at - 180000);
        store.add(ClipCapture {.kind = ClipKind::Text, // the chart's own figures
            .text = u"第三季度销售额：华东 402 万，华南 371 万，华北 279 万，西南 208 万。"_s, .source = u"Excel"_s}, at - 60000);
        ClipCapture copied;
        copied.kind = ClipKind::Image;
        copied.imageData = file.readAll();
        copied.hash = "picture-view-gallery";
        copied.width = chart.width();
        copied.height = chart.height();
        copied.source = u"PixPin"_s;
        store.setPictureText(store.add(copied, at), u"区域销售额\n华东 华南 西南 华北"_s);
        clipboard.handleShown();
        settle(window);
        entries->setProperty("currentIndex", 0);
        if (!page->property("previewOpen").toBool())
            QMetaObject::invokeMethod(page, "togglePreview", Q_ARG(QVariant, QVariant(0)));
        settle(window);
        zoom = named(u"clipboardPictureZoom"_s);
        check(waitFor([&] { return zoom && zoom->property("enabled").toBool(); }, 5000), "the gallery picture is not in the preview");
        bool large = false;
        QMetaObject::invokeMethod(page, "togglePictureView", Q_RETURN_ARG(bool, large));
        check(large && waitFor([&] {
            image = viewerItem(u"clipboardPictureViewerImage"_s);
            return image && image->property("status").toInt() == 1;
        }, 5000), "the gallery picture did not show large");
        if (QQuickWindow* beside = viewer()) {
            settle(beside);
            settle(window);
            QImage host = window->grabWindow(), shown = beside->grabWindow();
            const qreal dpr = window->devicePixelRatio();
            host.setDevicePixelRatio(1);
            shown.setDevicePixelRatio(1);
            const int gap = qRound(beside->property("gap").toReal() * dpr);
            const bool leftOfIt = beside->x() < window->x();
            const int hostY = std::max(0, (shown.height() - host.height()) / 2);
            const int shownY = std::max(0, (host.height() - shown.height()) / 2); // centred on the window, as with room
            QImage both(host.width() + gap + shown.width(), std::max(host.height(), shown.height()),
                QImage::Format_ARGB32_Premultiplied);
            both.fill(Qt::transparent);
            QPainter painter(&both);
            painter.drawImage(leftOfIt ? shown.width() + gap : 0, hostY, host);
            painter.drawImage(leftOfIt ? 0 : host.width() + gap, shownY, shown);
            painter.end();
            const bool saved = !host.isNull() && !shown.isNull() && both.save(output.filePath(u"clipboard-picture.png"_s), "PNG");
            say(u"%1 clipboard-picture.png %2x%3 (window %4x%5, picture %6x%7)"_s.arg(saved ? u"saved"_s : u"NOT saved"_s)
                    .arg(both.width()).arg(both.height()).arg(host.width()).arg(host.height())
                    .arg(shown.width()).arg(shown.height()));
            check(saved, "the poster of the large picture did not render");
        }
    }

    // The window put away: the large picture with it.
    clipboard.setSeen(false);
    window->hide();
    QMetaObject::invokeMethod(page, "windowHidden");
    check(waitFor([&] { return viewer() == nullptr; }, 2000), "the large picture stays with the window away");
    check(!watcher.running(), "fixture must never start the native clipboard writer");
    say(u"clipboard picture view: words folded and remembered, large on a click or Space, zoomed, closed by rows, Esc, the window: %1"_s
            .arg(ok ? u"checked"_s : u"FAILED"_s));
    return ok;
}

// --clipboard-select (GitHub #375): part of a text entry, selected with the
// mouse in the preview beside the list, without taking the keys from the
// search field: the hint says Ctrl+C copies it and Enter pastes it, the lines
// keep the 1.15 spacing they had as a Text, another entry drops the
// selection. Nothing is copied or pasted here (that writes the real
// clipboard): those keys are tried live. A temporary history.
bool checkClipboardTextSelection(QQmlEngine* engine, const QDir& output)
{
    QTemporaryDir temporary;
    ClipStore store(temporary.filePath(u"history"_s));
    bool ok = true;
    const auto check = [&](bool condition, const char* reason) {
        ok = condition && ok;
        if (!condition) qWarning("clipboard text selection regression: %s", reason);
    };
    if (!store.open(QDateTime::currentMSecsSinceEpoch())) { check(false, "temporary history could not open"); return false; }
    const QString text = u"账号：lingcore\n密码：在密码管理器里\n备注：周五前把年度报告发给财务，顺便确认销售图表的口径"_s;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    store.add(ClipCapture {.kind = ClipKind::Text, .text = u"另一段文字"_s, .source = u"记事本"_s}, now - 1000);
    store.add(ClipCapture {.kind = ClipKind::Text, .text = text, .source = u"记事本"_s}, now);
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    clipboard.setRowOptions(false, u"standard"_s, SystemTheme::uiSize());
    clipboard.setActive(true);
    clipboard.handleShown();
    Placement placement(temporary.filePath(u"state.ini"_s), u"Clipboard"_s);
    WindowFrame frame(false);
    QQmlComponent component(engine, u"WinShun"_s, u"ClipboardWindow"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"placement"_s, QVariant::fromValue(&placement)}, {u"frame"_s, QVariant::fromValue(&frame)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { qWarning().noquote() << "clipboard text selection regression:" << component.errorString(); return false; }
    window->create();
    frame.setWindow(window);
    placement.setWindow(window);
    placement.enableWindowSizing();
    showOffScreen(window);
    clipboard.handlePlaced();
    clipboard.setSeen(true);
    settle(window);
    auto* page = visualNamed(window->contentItem(), u"clipboardPage"_s);
    auto* entries = visualNamed(window->contentItem(), u"clipboardEntries"_s);
    if (!page || !entries) { check(false, "production clipboard page missing"); return false; }
    QMetaObject::invokeMethod(page, "togglePreview", Q_ARG(QVariant, QVariant(0)));
    settle(window);
    auto* body = visualNamed(window->contentItem(), u"clipboardPreviewText"_s);
    auto* hint = visualNamed(window->contentItem(), u"clipboardPreviewHint"_s);
    if (!body || !hint || !body->isVisible()) { check(false, "the entry's text is not in the preview"); return false; }
    check(body->property("text").toString() == text, "the preview does not show the whole entry");
    const auto selected = [&] { return body->property("selectedText").toString(); };
    // The lines as far apart as the Text's lineHeight 1.15 put them.
    const int lines = body->property("lineCount").toInt();
    const QFontMetricsF metrics(body->property("font").value<QFont>());
    const double perLine = lines > 0 ? body->property("contentHeight").toDouble() / lines : 0;
    check(lines >= 3 && perLine >= metrics.height() * 1.1 && perLine <= metrics.height() * 1.25,
        "the selectable text lost its line spacing");
    // Dragged across the second line: that part selected, the field keeping the keys.
    const auto mouse = [&](QEvent::Type type, const QPointF& at, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, at, window->mapToGlobal(at), button, buttons, {});
        QCoreApplication::sendEvent(window, &event);
        settle(window);
    };
    QQuickItem* field = visualNamed(window->contentItem(), u"clipboardSearchField"_s);
    const double lineY = perLine * 1.5;
    const QPointF from = body->mapToScene({1, lineY});
    const QPointF to = body->mapToScene({metrics.horizontalAdvance(u"密码：在密码"_s), lineY});
    mouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, (from + to) / 2, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, {});
    const QString part = selected();
    check(part.startsWith(u"密码"_s) && part.size() >= 4 && part.size() < 12 && !part.contains(u'\n'),
        "a drag across a line did not select that part of it");
    check(!body->hasFocus() && (!field || field->hasFocus()), "selecting took the keys from the search field");
    const QString told = hint->property("text").toString();
    check(hint->isVisible() && told.contains(u"Ctrl+C"_s) && told.contains(u"Enter"_s)
            && told.contains(QString::number(part.size())),
        "the hint does not say how to copy or paste the selected part");
    check(saveGrab(window, output.filePath(u"clipboard-text-selected.png"_s)), "the selection did not render");
    // Another entry: the selection gone, the hint back to the entry's own.
    entries->setProperty("currentIndex", 1);
    settle(window);
    check(selected().isEmpty() && !hint->property("text").toString().contains(u"Ctrl+C"_s),
        "another entry kept the selection");
    // GitHub #379: the menu's keys, shown beside its entries, do them with
    // the menu closed. Ctrl+E changes the words where the preview is;
    // Ctrl+T pins the window as its button does.
    const auto shortcutOf = [&](int row, int action) {
        for (const QVariant& item : clipboard.menuItems(row))
            if (item.toMap().value(u"action"_s).toInt() == action)
                return item.toMap().value(u"shortcut"_s).toString();
        return QString();
    };
    check(shortcutOf(1, Clipboard::EditText) == u"Ctrl+E"_s && shortcutOf(1, Clipboard::SaveFiles) == u"Ctrl+S"_s,
        "the menu does not show Ctrl+E and Ctrl+S beside their entries");
    const auto key = [&](int code, Qt::KeyboardModifiers modifiers) {
        QKeyEvent press(QEvent::KeyPress, code, modifiers), release(QEvent::KeyRelease, code, modifiers);
        QCoreApplication::sendEvent(window, &press);
        QCoreApplication::sendEvent(window, &release);
        settle(window);
    };
    if (field)
        field->forceActiveFocus(Qt::OtherFocusReason);
    key(Qt::Key_E, Qt::ControlModifier);
    check(page->property("editingId").toLongLong() > 0, "Ctrl+E did not open the entry's words to change");
    key(Qt::Key_Escape, Qt::NoModifier);
    check(page->property("editingId").toLongLong() == 0, "Esc did not close the words being changed");
    if (field)
        field->forceActiveFocus(Qt::OtherFocusReason);
    const bool pinned = clipboard.pinned();
    key(Qt::Key_T, Qt::ControlModifier);
    check(clipboard.pinned() != pinned, "Ctrl+T did not pin the window");
    key(Qt::Key_T, Qt::ControlModifier);
    check(clipboard.pinned() == pinned, "Ctrl+T again did not unpin it");
    window->hide();
    say(u"clipboard text selection and menu keys: real QML, temporary history; native clipboard untouched"_s);
    return ok;
}

// --clipboard-hover-copy (GitHub #439): text selected with the mouse in the
// preview that resting on a row brings up (a window that never takes the
// keys) is what the page's Ctrl+C copies and Enter pastes, not the row the
// keyboard is on, as in the preview beside the list (#375). Real QML and a
// temporary history. Ctrl+C is the last step: its copy is read back from
// the history, which it goes into at once, and the check returns before the
// clipboard would be written (the copies marked as a test's meanwhile, so
// that no history would record them even so).
bool checkClipboardHoverCopy(QQmlEngine* engine, const QDir& output)
{
    qputenv("WINSHUN_TEST_NO_CLIP_HISTORY", "1");
    QTemporaryDir temporary;
    ClipStore store(temporary.filePath(u"history"_s));
    bool ok = true;
    const auto check = [&](bool condition, const char* reason) {
        ok = condition && ok;
        if (!condition) qWarning("clipboard hover copy regression: %s", reason);
    };
    if (!store.open(QDateTime::currentMSecsSinceEpoch())) { check(false, "temporary history could not open"); return false; }
    const QString text = u"账号：lingcore\n密码：在密码管理器里\n备注：周五前把年度报告发给财务"_s;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    store.add(ClipCapture {.kind = ClipKind::Text, .text = text, .source = u"记事本"_s}, now - 1000);
    store.add(ClipCapture {.kind = ClipKind::Text, .text = u"另一段文字"_s, .source = u"记事本"_s}, now);
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    clipboard.setRowOptions(false, u"standard"_s, SystemTheme::uiSize());
    clipboard.setPreviewOpen(false);
    clipboard.setActive(true);
    clipboard.handleShown();
    Placement placement(temporary.filePath(u"state.ini"_s), u"Clipboard"_s);
    WindowFrame frame(false);
    QQmlComponent component(engine, u"WinShun"_s, u"ClipboardWindow"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"placement"_s, QVariant::fromValue(&placement)}, {u"frame"_s, QVariant::fromValue(&frame)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { qWarning().noquote() << "clipboard hover copy regression:" << component.errorString(); return false; }
    window->create();
    frame.setWindow(window);
    placement.setWindow(window);
    placement.enableWindowSizing();
    showOffScreen(window);
    clipboard.handlePlaced();
    clipboard.setSeen(true);
    settle(window);
    auto* page = visualNamed(window->contentItem(), u"clipboardPage"_s);
    auto* entries = visualNamed(window->contentItem(), u"clipboardEntries"_s);
    auto* field = visualNamed(window->contentItem(), u"clipboardSearchField"_s);
    auto* row = visualNamed(window->contentItem(), u"clipboardRow-1"_s);
    if (!page || !entries || !field || !row) { check(false, "production clipboard page missing"); return false; }
    check(entries->property("currentIndex").toInt() == 0, "the newest entry is not the keyboard's row");
    // The pointer rests on the second row: its preview comes up beside the
    // list, as page.hoverTimer brings it.
    const QPointF corner = row->mapToGlobal(QPointF(0, 0));
    page->setProperty("hoverIndex", 1);
    page->setProperty("hoverRect", QRectF(corner.x(), corner.y(), row->width(), row->height()));
    page->setProperty("hoverShown", true);
    QQuickWindow* hover = nullptr;
    for (QWindow* w : QGuiApplication::topLevelWindows()) {
        if (w->objectName() == u"clipboardHoverPreview" && w->isVisible())
            hover = qobject_cast<QQuickWindow*>(w);
    }
    if (!hover) { check(false, "resting on a row brought up no preview"); return false; }
    // It keeps to the screen; moved off it at once (before any event), its
    // place no longer bound, beside its host.
    QQmlProperty::write(hover, u"x"_s, window->x() + window->width() + 8);
    QQmlProperty::write(hover, u"y"_s, window->y());
    settle(hover);
    check(hover->x() > window->x(), "the preview could not be kept off the screen");
    auto* body = visualNamed(hover->contentItem(), u"clipboardPreviewText"_s);
    auto* hint = visualNamed(hover->contentItem(), u"clipboardPreviewHint"_s);
    if (!body || !hint) { check(false, "the preview's text missing"); return false; }
    check(body->property("text").toString() == text, "the preview does not show the row under the pointer");
    // Dragged across its second line: that part selected, the page's field keeping the keys.
    const int lines = body->property("lineCount").toInt();
    const double perLine = lines > 0 ? body->property("contentHeight").toDouble() / lines : 0;
    const QFontMetricsF metrics(body->property("font").value<QFont>());
    const auto mouse = [&](QEvent::Type type, const QPointF& at, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, at, hover->mapToGlobal(at), button, buttons, {});
        QCoreApplication::sendEvent(hover, &event);
        settle(hover);
    };
    const QPointF from = body->mapToScene({1, perLine * 1.5});
    const QPointF to = body->mapToScene({metrics.horizontalAdvance(u"密码：在密码"_s), perLine * 1.5});
    mouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, (from + to) / 2, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, {});
    const QString part = body->property("selectedText").toString();
    check(part.startsWith(u"密码"_s) && part.size() >= 4 && part.size() < 12 && !part.contains(u'\n'),
        "a drag in the preview did not select part of a line");
    check(hover->isVisible(), "selecting closed the preview");
    check(page->property("previewSelection").toString() == part, "Enter would not paste the part selected in the preview");
    check(hint->property("text").toString().contains(u"Ctrl+C"_s), "the preview does not say that Ctrl+C copies the part");
    check(saveGrab(hover, output.filePath(u"clipboard-hover-selected.png"_s)), "the preview did not render");
    // Ctrl+C in the page, whose field has the keys: the part, on top of the
    // history as a new entry; not the row the keyboard is on.
    field->forceActiveFocus(Qt::OtherFocusReason);
    const std::size_t before = store.clips().size();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QCoreApplication::sendEvent(window, &press);
    check(store.clips().size() == before + 1 && store.clips().front().text == part,
        "Ctrl+C did not copy the part selected in the preview the pointer brought up");
    // The new entry moved the rows: that preview closed, nothing left selected.
    check(!page->property("hoverShown").toBool() && page->property("previewSelection").toString().isEmpty(),
        "the preview stayed over rows that moved once the part was copied");
    say(u"clipboard hover preview: a part selected there is what Ctrl+C copies and Enter pastes (real QML, temporary history)"_s);
    return ok; // no event loop from here on: the clipboard is never written
}

// --content-kinds (GitHub #447): 内容 narrowed to kinds of file with 文件's
// chips (the panel's 筛选): it stays 内容 and reads only those kinds, nor
// counts other kinds of text as passed over; the placeholder and its strip
// name them, the strip's chip takes one off, 文件's own kind is untouched,
// and the choice outlives a trip to 文件. Real files read by the production
// launcher and search workers, Main.qml shown.
bool checkContentKinds(QQmlEngine* qml, const QDir& output)
{
    QTemporaryDir temporary;
    IndexService index(temporary.filePath(u"index"_s), IndexService::Options{});
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    History history(temporary.filePath(u"history.tsv"_s));
    SearchEngine search(&index, &apps, &places);
    Launcher launcher(&index, &apps, &places, &search, &history);
    launcher.setContentOptions(true, {u"txt"_s, u"md"_s}, ContentSizeLimits{}, false, false);
    launcher.setFileTypes({FileType {u"文本"_s, {u"txt"_s}, u"text"_s}, FileType {u"笔记"_s, {u"md"_s}, u"notes"_s},
        FileType {u"图片"_s, {u"jpg"_s, u"png"_s}, u"pictures"_s}});
    ClipStore store(temporary.filePath(u"clipboard"_s));
    if (!store.open(QDateTime::currentMSecsSinceEpoch())) return false;
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    Placement placement(temporary.filePath(u"state.ini"_s), u"Launcher"_s);
    WindowFrame frame(false);
    QQmlComponent component(qml, u"WinShun"_s, u"Main"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {u"launcher"_s, QVariant::fromValue(&launcher)}, {u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"frame"_s, QVariant::fromValue(&frame)}, {u"placement"_s, QVariant::fromValue(&placement)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { say(component.errorString()); return false; }
    window->create();
    launcher.setWindow(window);
    frame.setWindow(window);
    placement.setWindow(window);
    showOffScreen(window);
    bool ok = true;
    const auto check = [&](bool condition, const char* reason) {
        ok = condition && ok;
        if (!condition) qWarning("content kinds regression: %s", reason);
    };
    const auto files = index.index();
    // Indexed as a drive is, from "C:" down, so that the folder is one a chip can choose.
    EntryId root = kNoEntry;
    {
        const auto lock = files->writeLock();
        const QStringList parts = QDir::toNativeSeparators(temporary.path()).split(u'\\', Qt::SkipEmptyParts);
        for (const QString& part : parts) {
            const std::string name = part.toUtf8().toStdString();
            root = root == kNoEntry ? files->addRoot(name) : files->add(root, name, EntryFlag::Directory);
        }
    }
    const auto write = [&](const QString& name, const QByteArray& bytes) {
        {
            QFile file(temporary.filePath(name));
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return false;
        }
        // Read back once: the antivirus's first look at a new file (up to
        // a second) taken here, not in the first search's time.
        QFile back(temporary.filePath(name));
        if (!back.open(QIODevice::ReadOnly) || back.readAll() != bytes) return false;
        const auto lock = files->writeLock();
        return files->add(root, name.toUtf8().toStdString(), 0) != kNoEntry;
    };
    check(write(u"牛蛙.txt"_s, "关于牛蛙的笔记\n") && write(u"牛蛙.md"_s, "# 牛蛙\n") && write(u"牛蛙.py"_s, "# 牛蛙\n"),
        "the files could not be written");
    int finishes = 0;
    QObject::connect(&search, &SearchEngine::contentProgress, &launcher,
        [&](quint64, int, int, bool finished) { if (finished) ++finishes; });
    // A change, then the content search it starts until it is over.
    const auto run = [&](const auto& change) {
        const int before = finishes;
        change();
        QElapsedTimer timer;
        timer.start();
        while (!(finishes > before && !launcher.searching()) && timer.elapsed() < 20000)
            pump(10);
        settle(window);
        return finishes > before;
    };
    const auto names = [&] {
        QStringList listed;
        for (int i = 0; i < launcher.results()->count(); ++i)
            listed.append(launcher.results()->at(i)->name);
        listed.sort();
        return listed;
    };
    const auto otherTypesOffered = [&] {
        return std::ranges::any_of(launcher.leftOutHints(), [](const QVariant& h) { return h.toMap().value(u"kind"_s) == u"types"_s; });
    };
    const auto firstChip = [&] { return launcher.folderChips().value(0).toMap(); };

    launcher.setScope(Launcher::Content);
    check(run([&] { launcher.setQuery(u"牛蛙"_s); }), "the content search did not run");
    check(names() == (QStringList {u"牛蛙.md"_s, u"牛蛙.txt"_s}), "内容 did not find the words in both kinds");
    check(otherTypesOffered(), "the .py file passed over was not offered");

    // 文本 from the panel's 筛选 (as Panel.chooseType): still 内容, the .txt alone.
    check(run([&] { launcher.toggleFileType(0); }), "choosing a kind did not search again");
    check(launcher.scope() == Launcher::Content && launcher.contentFileTypes() == QList<int> {0},
        "choosing a kind in 内容 left 内容 (GitHub #447)");
    check(names() == QStringList {u"牛蛙.txt"_s}, "内容 was not narrowed to 文本");
    check(launcher.placeholder() == u"搜索文本中的文字", "the placeholder does not name the kind");
    check(firstChip().value(u"kind"_s).toInt() == 0 && firstChip().value(u"type"_s).toString().startsWith(u"type:"),
        "the strip does not show the kind first");
    check(launcher.selectedFileTypes().isEmpty() && launcher.fileType() == -1, "文件's own kind was changed");

    // 图片: no file of it has the words, and the .py is not of it either.
    check(run([&] { launcher.chooseFileType(2); }), "another kind did not search again");
    check(launcher.contentFileTypes() == QList<int> {2} && names().isEmpty(), "图片 did not narrow 内容 to pictures");
    check(!otherTypesOffered(), "text of another kind was offered while narrowed to pictures");

    // Two kinds; then the strip's chip of one takes it off.
    check(run([&] { launcher.toggleFileType(1); }), "a second kind did not search again");
    check(launcher.contentFileTypes() == (QList<int> {2, 1}) && names() == QStringList {u"牛蛙.md"_s},
        "two kinds were not both looked in");
    auto* strip = visualNamed(window->contentItem(), u"searchTypeStrip"_s);
    auto* chip = visualNamed(window->contentItem(), u"searchTypeChiptype:pictures"_s);
    check(strip && strip->property("shown").toBool() && chip && chip->property("active").toBool()
            && chip->property("glyph").toString().size() > 0,
        "the strip does not show 图片 on, with its icon");
    check(saveGrab(window, output.filePath(u"content-kinds.png"_s)), "the window did not render");
    check(run([&] { launcher.chooseFolderChip(firstChip().value(u"type"_s).toString()); }), "the chip did not search again");
    check(launcher.contentFileTypes() == QList<int> {1} && names() == QStringList {u"牛蛙.md"_s}, "the chip did not take 图片 off");
    check(launcher.placeholder() == u"搜索笔记中的文字", "the placeholder does not name what is left");

    // Kept through 文件 and back; 不限 clears it; 文件夹 is 文件夹's scope.
    launcher.setScope(Launcher::Files);
    check(launcher.selectedFileTypes().isEmpty(), "文件 took 内容's kind");
    check(run([&] { launcher.setScope(Launcher::Content); }) && launcher.contentFileTypes() == QList<int> {1}
            && names() == QStringList {u"牛蛙.md"_s},
        "内容's kind was lost on the way through 文件");
    // The strip's 不限 (在所有文件里搜) is off while kinds are on; a click takes them off.
    const auto anyChip = [&] { return visualNamed(window->contentItem(), u"searchTypeChip"_s); }; // its key ""
    check(anyChip() && !anyChip()->property("active").toBool(), "the strip's 不限 shows on while 内容 is narrowed");
    check(run([&] { launcher.chooseFolderChip(QString()); }) && launcher.contentFileTypes().isEmpty()
            && names() == (QStringList {u"牛蛙.md"_s, u"牛蛙.txt"_s}) && anyChip() && anyChip()->property("active").toBool(),
        "the strip's 不限 did not give every kind back");
    // Ctrl+Tab goes round the folders only: the kinds stay.
    check(run([&] { launcher.toggleFileType(0); }) && launcher.contentFileTypes() == QList<int> {0}, "文本 again did not narrow");
    const QString folder = QDir::toNativeSeparators(temporary.path());
    const bool searchedThere = run([&] { launcher.chooseFolderChip(u"pin:"_s + folder); });
    check(searchedThere && launcher.folderChip() == u"pin:"_s + folder
            && launcher.contentFileTypes() == QList<int> {0} && names() == QStringList {u"牛蛙.txt"_s},
        "a folder with a kind did not search the kind there");
    check(launcher.placeholder().contains(u"文本中的文字"_s), "the placeholder does not name the kind in a folder");
    check(run([&] { launcher.cycleFolderChip(1); }) && launcher.folderChip().isEmpty(), "Ctrl+Tab did not go to 不限");
    check(launcher.contentFileTypes() == QList<int> {0} && names() == QStringList {u"牛蛙.txt"_s},
        "Ctrl+Tab over the folders took the kind off");
    // And the panel's 不限.
    check(run([&] { launcher.chooseFileType(-1); }), "不限 did not search again");
    check(launcher.contentFileTypes().isEmpty() && names() == (QStringList {u"牛蛙.md"_s, u"牛蛙.txt"_s}),
        "不限 did not give every kind back");

    // The files typed among the words ("牛蛙 *.md", "ext:txt;py 牛蛙"): only
    // those are read, nor others counted as passed over; the strip shows
    // what was understood first, on, and a click on it takes the words out
    // of the box. In quotes it is text to find.
    check(run([&] { launcher.setQuery(u"牛蛙 *.md"_s); }) && names() == QStringList {u"牛蛙.md"_s}, "\"*.md\" typed did not narrow 内容");
    check(firstChip().value(u"type"_s).toString() == u"files" && firstChip().value(u"name"_s).toString() == u"*.md",
        "the strip does not show the files typed first");
    check(!otherTypesOffered(), "the .py file was offered while narrowed to *.md");
    auto* typedChip = visualNamed(window->contentItem(), u"searchTypeChipfiles"_s);
    check(typedChip && typedChip->property("active").toBool() && typedChip->property("glyph").toString().size() > 0,
        "the strip's chip of the files typed is not on, with its icon");
    check(launcher.folderHint() == u"或在词后加“文件夹名/”", "the hint still offers what was typed");
    check(anyChip() && !anyChip()->property("active").toBool(), "the strip's 不限 shows on while files are typed");
    check(saveGrab(window, output.filePath(u"content-files.png"_s)), "the window did not render");
    check(run([&] { launcher.setQuery(u"ext:txt;py 牛蛙"_s); }) && names() == QStringList {u"牛蛙.txt"_s} && otherTypesOffered(),
        "\"ext:txt;py\" typed did not narrow 内容 to those types");
    check(run([&] { launcher.setQuery(u"牛蛙 !*.txt"_s); }) && names() == QStringList {u"牛蛙.md"_s}, "\"!*.txt\" typed did not leave the .txt out");
    check(run([&] { launcher.chooseFolderChip(QString()); }) && launcher.query() == u"牛蛙" && anyChip() && anyChip()->property("active").toBool(),
        "the strip's 不限 did not take the files typed out of the box");
    check(run([&] { launcher.setQuery(u"牛蛙 !*.txt"_s); }), "the files typed again did not search");
    check(run([&] { launcher.chooseFolderChip(u"files"_s); }) && launcher.query() == u"牛蛙"
            && names() == (QStringList {u"牛蛙.md"_s, u"牛蛙.txt"_s}) && firstChip().value(u"type"_s).toString().isEmpty(),
        "the chip of the files typed did not take them out of the box");
    check(launcher.folderHint().contains(u"*.docx"_s), "the hint does not say a type can be typed");
    check(run([&] { launcher.setQuery(u"牛蛙 \"*.md\""_s); }) && names().isEmpty() && firstChip().value(u"type"_s).toString().isEmpty(),
        "a quoted \"*.md\" was taken for files, not text");
    check(run([&] { launcher.setQuery(u"牛蛙"_s); }), "the words alone did not search again");

    launcher.chooseFileType(Launcher::kFoldersChip);
    check(launcher.scope() == Launcher::Folders, "文件夹 in 内容 is not 文件夹's scope");
    window->hide();
    say(u"content kinds: 内容 narrowed by 文件's chips, real files and search workers"_s);
    return ok;
}

// --type-strip-scroll: more kinds than the strip has room for, all chosen,
// in the 界面风格 --style names. A chip cut off at either end is hidden and
// takes no clicks through its MouseArea alone: the chip itself stays enabled,
// since a style's key may change with that (tactile's chosen key turns bold,
// wider), and `whole` reads the chip's width (a binding loop, 2026-11-24).
// --preview-results <dir> --type-strip-counts: 文件's chips with how many of
// what is typed each lists (Launcher.fileTypeCounts): a tag after each name,
// the chip as much wider; none for no numbers, and the chips as before.
bool checkTypeStripCounts(QQmlEngine* qml, const QDir& output)
{
    QTemporaryDir temporary;
    IndexService index(temporary.filePath(u"index"_s), IndexService::Options{});
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    History history(temporary.filePath(u"history.tsv"_s));
    SearchEngine search(&index, &apps, &places);
    Launcher launcher(&index, &apps, &places, &search, &history);
    FileTypes types;
    for (const auto& [name, extension] : {std::pair {u"文档"_s, u"docx"_s}, {u"图片"_s, u"jpg"_s}, {u"视频"_s, u"mp4"_s},
             {u"音频"_s, u"mp3"_s}, {u"压缩包"_s, u"zip"_s}})
        types.append(FileType {name, {extension}});
    launcher.setFileTypes(types);
    ClipStore store(temporary.filePath(u"clipboard"_s));
    if (!store.open(QDateTime::currentMSecsSinceEpoch())) return false;
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    Placement placement(temporary.filePath(u"state.ini"_s), u"Launcher"_s);
    WindowFrame frame(false);
    QQmlComponent component(qml, u"WinShun"_s, u"Main"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {u"launcher"_s, QVariant::fromValue(&launcher)}, {u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"frame"_s, QVariant::fromValue(&frame)}, {u"placement"_s, QVariant::fromValue(&placement)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { say(component.errorString()); return false; }
    window->create();
    launcher.setWindow(window);
    frame.setWindow(window);
    placement.setWindow(window);
    showOffScreen(window);
    bool ok = true;
    const auto check = [&](bool condition, const char* reason) {
        ok = condition && ok;
        if (!condition) qWarning("type strip counts regression: %s", reason);
    };
    launcher.setScope(Launcher::Files);
    launcher.toggleFileType(0); // 文档 chosen: the others' numbers stay
    settle(window);
    const auto chipOf = [&](int type) { return visualNamed(window->contentItem(), u"searchTypeChip%1"_s.arg(type)); };
    const auto tagOf = [&](int type) { QQuickItem* chip = chipOf(type); return chip ? visualNamed(chip, u"searchTypeChipCount"_s) : nullptr; };
    const QList<int> shown {-1, -2, 0, 1, 2, 3, 4};
    QList<qreal> plain;
    for (const int type : shown) {
        QQuickItem* chip = chipOf(type);
        QQuickItem* tag = tagOf(type);
        if (!chip || !tag) { check(false, "a chip or its tag is missing"); return false; }
        check(!tag->isVisible(), "a tag shows with no numbers");
        plain.append(chip->width());
        // Laid out by the icon's container: an even square, as far from the chip's left edge as from
        // its top and bottom, the name a fixed gap after it.
        QQuickItem* icon = visualNamed(chip, u"searchTypeChipIcon"_s);
        QQuickItem* name = visualNamed(chip, u"searchTypeChipName"_s);
        if (!icon || !name) { check(false, "a chip's icon or name is missing"); return false; }
        if (icon->isVisible()) {
            const qreal left = icon->x();
            const qreal top = icon->y();
            const qreal bottom = chip->height() - top - icon->height();
            const qreal gap = name->x() - (left + icon->width());
            if (icon->width() != icon->height() || std::fmod(icon->width(), 2) != 0 || std::abs(left - top) > 0.01 || std::abs(top - bottom) > 0.7
                || std::abs(gap - 4) > 0.01) {
                qWarning("type strip counts regression: chip %d: icon box %.1fx%.1f, %.2f from the left, %.2f from the top, "
                         "%.2f from the bottom, %.2f before the name", type, icon->width(), icon->height(), left, top, bottom, gap);
                ok = false;
            }
        }
    }
    check(saveGrab(window, output.filePath(u"type-strip-counts-none.png"_s)), "the window did not render");
    // all, folders, then the kinds: small, none, and too long to write out.
    launcher.setFileTypeCounts({QVariant(1'254'380), QVariant(3), QVariant(128), QVariant(0), QVariant(9'999), QVariant(10'000), QVariant(356'000)});
    settle(window);
    const qreal dpr = window->effectiveDevicePixelRatio();
    for (int i = 0; i < shown.size(); ++i) {
        QQuickItem* chip = chipOf(shown.at(i));
        QQuickItem* tag = tagOf(shown.at(i));
        check(tag->isVisible(), "a tag does not show with numbers");
        check(chip->width() > plain.at(i) + tag->width(), "a chip is not wider by its tag");
        // Inside its chip, clear of the edge, on whole device pixels.
        const QPointF at = tag->mapToItem(chip, QPointF(0, 0));
        check(at.x() + tag->width() <= chip->width() - 4 && at.x() > 0, "a tag is not inside its chip");
        const qreal left = tag->mapToScene(QPointF(0, 0)).x() * dpr;
        check(std::abs(left - std::round(left)) < 0.01, "a tag is not on a whole device pixel");
    }
    check(launcher.countLabel(128) == u"128" && launcher.countLabel(0) == u"0" && launcher.countLabel(9'999) == u"9999",
        "a small count is not written out");
    check(launcher.countLabel(10'000) == u"1万" && launcher.countLabel(356'000) == u"35万"
            && launcher.countLabel(1'254'380) == u"125万" && launcher.countLabel(19'999) == u"1.9万"
            && launcher.countLabel(230'000'000) == u"2.3亿",
        "a large count is not shortened");
    check(saveGrab(window, output.filePath(u"type-strip-counts.png"_s)), "the window did not render");
    // Switched off: the numbers go, the chips as wide as before.
    launcher.setTypeCounts(false);
    settle(window);
    for (int i = 0; i < shown.size(); ++i)
        check(!tagOf(shown.at(i))->isVisible() && chipOf(shown.at(i))->width() == plain.at(i), "switched off, a tag or its room stays");
    return ok;
}

// --preview-results <dir> --type-strip-drag: 文件's chips held and dragged with
// the mouse, as the user does it. 文件夹 and the kinds go anywhere after 不限
// (Launcher.moveFileChip, applied as the app applies it: Settings::moveStripChip,
// then setFileTypes); the others make way while one is dragged; the kind chosen
// and the numbers stay with their kinds; Ctrl+Tab goes in the new order; a press
// without a drag is still a click; 不限 stays first; with more kinds than room,
// one held at the end scrolls the strip on.
bool checkTypeStripDrag(QQmlEngine* qml, const QDir& output)
{
    static QtMessageHandler counted = nullptr;
    static int loops = 0;
    loops = 0;
    counted = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& context, const QString& message) {
        if (message.contains(u"Binding loop detected"_s))
            ++loops;
        counted(type, context, message);
    });
    const auto restore = qScopeGuard([] { qInstallMessageHandler(counted); });
    QTemporaryDir temporary;
    IndexService index(temporary.filePath(u"index"_s), IndexService::Options{});
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    History history(temporary.filePath(u"history.tsv"_s));
    SearchEngine search(&index, &apps, &places);
    Launcher launcher(&index, &apps, &places, &search, &history);
    Settings settings;
    for (const auto& [name, extension] : {std::pair {u"文档"_s, u"docx"_s}, {u"图片"_s, u"jpg"_s}, {u"视频"_s, u"mp4"_s},
             {u"音频"_s, u"mp3"_s}, {u"压缩包"_s, u"zip"_s}})
        settings.fileTypes.append(FileType {name, {extension}});
    int moves = 0;
    QObject::connect(&launcher, &Launcher::fileChipMoveRequested, &launcher, [&](int from, int to) {
        ++moves;
        if (settings.moveStripChip(from, to))
            launcher.setFileTypes(settings.fileTypes, settings.folderChipAt);
    });
    launcher.setFileTypes(settings.fileTypes, settings.folderChipAt);
    ClipStore store(temporary.filePath(u"clipboard"_s));
    if (!store.open(QDateTime::currentMSecsSinceEpoch())) return false;
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    Placement placement(temporary.filePath(u"state.ini"_s), u"Launcher"_s);
    WindowFrame frame(false);
    QQmlComponent component(qml, u"WinShun"_s, u"Main"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {u"launcher"_s, QVariant::fromValue(&launcher)}, {u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"frame"_s, QVariant::fromValue(&frame)}, {u"placement"_s, QVariant::fromValue(&placement)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { say(component.errorString()); return false; }
    window->create();
    launcher.setWindow(window);
    frame.setWindow(window);
    placement.setWindow(window);
    showOffScreen(window);
    bool ok = true;
    const auto check = [&](bool condition, const char* reason) {
        ok = condition && ok;
        if (!condition) qWarning("type strip drag regression: %s", reason);
    };
    auto* strip = visualNamed(window->contentItem(), u"searchTypeStrip"_s);
    if (!strip) { check(false, "there is no strip"); return false; }
    launcher.setScope(Launcher::Files);
    launcher.toggleFileType(1); // 图片
    settle(window);
    // all, folders, then 文档 图片 视频 音频 压缩包
    launcher.setFileTypeCounts({QVariant(100), QVariant(7), QVariant(10), QVariant(11), QVariant(12), QVariant(13), QVariant(14)});
    settle(window);

    const auto chipOf = [&](int type) { return visualNamed(window->contentItem(), u"searchTypeChip%1"_s.arg(type)); };
    const auto countOn = [&](int type) {
        QQuickItem* chip = chipOf(type);
        QQuickItem* tag = chip ? visualNamed(chip, u"searchTypeChipCount"_s) : nullptr;
        return tag ? tag->property("count").toInt() : -100;
    };
    const auto order = [&] {
        QStringList names;
        for (const QVariant& chip : strip->property("chips").toList())
            names.append(chip.toMap().value(u"name"_s).toString());
        return names;
    };
    const auto centre = [&](QQuickItem* item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)); };
    // Pressed on the chip, moved sideways by `by` (inside the window) a few
    // pixels at a time, held `hold` ms, `midway` before it is let go; then
    // the glide into its place.
    const auto drag = [&](QQuickItem* chip, qreal by, const std::function<void()>& midway, int hold = 0) {
        const QPointF from = centre(chip);
        by = std::clamp(from.x() + by, 2.0, window->width() - 2.0) - from.x();
        systemMouse(window, QEvent::MouseButtonPress, from);
        const int steps = std::max(1, static_cast<int>(std::abs(by) / 4));
        for (int i = 1; i <= steps; ++i) {
            systemMouse(window, QEvent::MouseMove, from + QPointF(by * i / steps, 0));
            QCoreApplication::processEvents();
        }
        pump(250 + hold); // the others make way
        settle(window, 2);
        if (midway)
            midway();
        systemMouse(window, QEvent::MouseButtonRelease, from + QPointF(by, 0));
        pump(300);
        settle(window, 2);
    };
    // The chips shown side by side as they rest: none over another, the gaps
    // between the kinds the row's, each on a whole device pixel.
    // (Scrolling, the room left is shared out between the gaps: as wide each,
    // and the first chip shown starts at the view's left edge, none left out before it.)
    const qreal dpr = window->effectiveDevicePixelRatio();
    auto* view = visualNamed(strip, u"searchTypeView"_s);
    if (!view) { check(false, "there is no view of the chips"); return false; }
    const auto laidOut = [&](const char* when) {
        const QVariantList chips = strip->property("chips").toList();
        qreal right = -1;
        qreal gap = strip->property("scrolls").toBool() ? -1 : 4;
        for (int i = 0; i < chips.size(); ++i) {
            QQuickItem* chip = chipOf(chips.at(i).toMap().value(u"type"_s).toInt());
            if (!chip || !chip->property("whole").toBool())
                continue;
            const qreal left = chip->mapToScene(QPointF(0, 0)).x();
            if (right < 0 && strip->property("scrolls").toBool()
                && (i != strip->property("firstChip").toInt() || std::abs(left - view->mapToScene(QPointF(0, 0)).x()) > 0.6)) {
                qWarning("type strip drag regression: %s: chip %d is the first shown, %.2f from the left edge; firstChip is %d", when, i,
                    left - view->mapToScene(QPointF(0, 0)).x(), strip->property("firstChip").toInt());
                ok = false;
            }
            if (right >= 0 && i > 1 && gap < 0)
                gap = left - right;
            if (right >= 0 && (left < right - 0.5 || (i > 1 && std::abs(left - right - gap) > 0.6))) {
                qWarning("type strip drag regression: %s: chip %d starts at %.2f, the one before ends at %.2f", when, i, left, right);
                ok = false;
            }
            check(std::abs(left * dpr - std::round(left * dpr)) < 0.01, "a chip is not on a whole device pixel");
            check(chip->property("way").toReal() == 0 && !chip->property("dragged").toBool(), "a chip still makes way or is dragged");
            right = left + chip->width();
        }
    };
    laidOut("at the start");
    check(order() == QStringList({u"不限"_s, u"文件夹"_s, u"文档"_s, u"图片"_s, u"视频"_s, u"音频"_s, u"压缩包"_s}), "the strip does not start 不限, 文件夹");

    // 文件夹 past 文档 and 图片: those two make way while it is held.
    QQuickItem* documents = chipOf(0);
    QQuickItem* pictures = chipOf(1);
    drag(chipOf(-2), documents->width() + pictures->width() + 8, [&] {
        check(strip->property("dragFrom").toInt() == 1 && strip->property("dragTo").toInt() == 3, "the strip does not know what is dragged where");
        check(documents->property("makeWay").toReal() < 0 && pictures->property("makeWay").toReal() < 0
                && chipOf(2)->property("makeWay").toReal() == 0,
            "文档 and 图片 do not make way for 文件夹, or 视频 does");
        check(saveGrab(window, output.filePath(u"type-strip-drag.png"_s)), "the window did not render");
    });
    check(moves == 1 && order() == QStringList({u"不限"_s, u"文档"_s, u"图片"_s, u"文件夹"_s, u"视频"_s, u"音频"_s, u"压缩包"_s}),
        "文件夹 did not go after 图片");
    check(settings.folderChipAt == 2 && launcher.folderChipAt() == 2, "the settings do not have 文件夹 after 图片");
    check(strip->property("dragFrom").toInt() == -1, "the drag did not end");
    check(launcher.scope() == Launcher::Files && launcher.selectedFileTypes() == QList<int> {1}, "the drag changed what is chosen");
    laidOut("文件夹 dropped");
    check(saveGrab(window, output.filePath(u"type-strip-dropped.png"_s)), "the window did not render");

    // 压缩包 dragged far to the left: first after 不限, no further; the kinds' own order with it.
    drag(chipOf(4), -1000, {});
    check(moves == 2 && order() == QStringList({u"不限"_s, u"压缩包"_s, u"文档"_s, u"图片"_s, u"文件夹"_s, u"视频"_s, u"音频"_s}),
        "压缩包 did not go first after 不限");
    check(settings.fileTypes.value(0).name == u"压缩包" && settings.folderChipAt == 3, "the settings do not have 压缩包 first");
    check(launcher.selectedFileTypes() == QList<int> {2}, "图片 was not still chosen where it went");
    check(countOn(0) == 14 && countOn(2) == 11 && countOn(-2) == 7 && countOn(-1) == 100, "the numbers did not go with their kinds");
    laidOut("压缩包 dropped");

    // A press without a drag is a click: 视频 chosen too.
    QQuickItem* videos = chipOf(3);
    systemMouse(window, QEvent::MouseButtonPress, centre(videos));
    systemMouse(window, QEvent::MouseButtonRelease, centre(videos));
    settle(window);
    check(moves == 2 && launcher.selectedFileTypes() == (QList<int> {2, 3}), "a click on a chip did not choose it");
    // 不限 does not move: dragged a little it is clicked; far, nothing.
    drag(chipOf(-1), 300, {});
    check(moves == 2 && order().first() == u"不限", "不限 moved");
    // Ctrl+Tab in the strip's order.
    launcher.chooseFileType(-1);
    QList<int> stops;
    for (int i = 0; i < 7; ++i) {
        launcher.cycleFileType(1);
        stops.append(launcher.scope() == Launcher::Folders ? Launcher::kFoldersChip : launcher.fileType());
    }
    check(stops == QList<int>({0, 1, 2, Launcher::kFoldersChip, 3, 4, -1}), "Ctrl+Tab does not go in the strip's order");
    settle(window);

    // More kinds than room: one held at the right end scrolls the chips on, and goes there.
    for (const auto& [name, extension] : {std::pair {u"表格"_s, u"xlsx"_s}, {u"演示"_s, u"pptx"_s}, {u"PDF"_s, u"pdf"_s},
             {u"程序"_s, u"exe"_s}, {u"代码"_s, u"cpp"_s}, {u"文本"_s, u"txt"_s}, {u"电子书"_s, u"epub"_s}, {u"设计稿"_s, u"psd"_s},
             {u"字体"_s, u"ttf"_s}})
        settings.fileTypes.append(FileType {name, {extension}});
    launcher.setFileTypes(settings.fileTypes, settings.folderChipAt);
    launcher.chooseFileType(0);
    settle(window);
    check(strip->property("scrolls").toBool() && strip->property("firstChip").toInt() == 0, "the strip does not scroll, or does not start at 不限");
    const int count = static_cast<int>(strip->property("chips").toList().size());
    drag(chipOf(1), 2000, [&] { // 文档
        check(strip->property("firstChip").toInt() > 0, "held at the end, the chips did not scroll on");
        check(chipOf(1)->property("whole").toBool() && chipOf(1)->opacity() == 1, "the chip dragged was hidden");
        // The chips that made way past the left edge show cut off by it: no room left empty there.
        const qreal viewLeft = view->mapToScene(QPointF(0, 0)).x();
        qreal shownFrom = viewLeft + view->width();
        for (const QVariant& each : strip->property("chips").toList()) {
            QQuickItem* chip = chipOf(each.toMap().value(u"type"_s).toInt());
            const qreal left = chip ? chip->mapToScene(QPointF(0, 0)).x() : 0;
            if (chip && chip->opacity() > 0.99 && left + chip->width() > viewLeft + 0.5)
                shownFrom = std::min(shownFrom, std::max(left, viewLeft));
        }
        check(shownFrom - viewLeft <= 4 + strip->property("spreadGap").toReal() + 1, "held at the end, the strip left room empty at its start");
        check(saveGrab(window, output.filePath(u"type-strip-drag-scroll.png"_s)), "the window did not render");
    }, 2000);
    const QStringList last = order();
    check(moves == 3 && last.indexOf(u"文档"_s) > 6, "文档 did not go towards the end");
    check(last.size() == count && last.first() == u"不限", "a chip was lost or 不限 moved");
    check(strip->property("firstChip").toInt() > 0, "the drop scrolled back to the start");
    QQuickItem* dropped = chipOf(strip->property("chips").toList().value(last.indexOf(u"文档"_s)).toMap().value(u"type"_s).toInt());
    check(dropped && dropped->property("whole").toBool(), "文档, let go at the end, is not in view");
    laidOut("scrolled and dropped");
    check(loops == 0, "a binding loop while dragging");
    window->hide();
    say(u"type strip drag: 文件夹 and the kinds reordered by the mouse (%1)"_s.arg(SystemTheme::uiStyle()));
    return ok;
}

bool checkTypeStripScroll(QQmlEngine* qml, const QDir& output)
{
    static QtMessageHandler counted = nullptr;
    static int loops = 0;
    loops = 0;
    counted = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& context, const QString& message) {
        if (message.contains(u"Binding loop detected"_s))
            ++loops;
        counted(type, context, message);
    });
    const auto restore = qScopeGuard([] { qInstallMessageHandler(counted); });
    QTemporaryDir temporary;
    IndexService index(temporary.filePath(u"index"_s), IndexService::Options{});
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    History history(temporary.filePath(u"history.tsv"_s));
    SearchEngine search(&index, &apps, &places);
    Launcher launcher(&index, &apps, &places, &search, &history);
    FileTypes types;
    for (const auto& [name, extension] : {std::pair {u"图片"_s, u"jpg"_s}, {u"文档"_s, u"docx"_s}, {u"表格"_s, u"xlsx"_s},
             {u"演示"_s, u"pptx"_s}, {u"PDF"_s, u"pdf"_s}, {u"音乐"_s, u"mp3"_s}, {u"视频"_s, u"mp4"_s},
             {u"压缩包"_s, u"zip"_s}, {u"程序"_s, u"exe"_s}, {u"代码"_s, u"cpp"_s}, {u"文本"_s, u"txt"_s},
             {u"电子书"_s, u"epub"_s}, {u"设计稿"_s, u"psd"_s}, {u"字体"_s, u"ttf"_s}})
        types.append(FileType {name, {extension}});
    launcher.setFileTypes(types);
    ClipStore store(temporary.filePath(u"clipboard"_s));
    if (!store.open(QDateTime::currentMSecsSinceEpoch())) return false;
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    Placement placement(temporary.filePath(u"state.ini"_s), u"Launcher"_s);
    WindowFrame frame(false);
    QQmlComponent component(qml, u"WinShun"_s, u"Main"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {u"launcher"_s, QVariant::fromValue(&launcher)}, {u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"frame"_s, QVariant::fromValue(&frame)}, {u"placement"_s, QVariant::fromValue(&placement)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { say(component.errorString()); return false; }
    window->create();
    launcher.setWindow(window);
    frame.setWindow(window);
    placement.setWindow(window);
    showOffScreen(window);
    bool ok = true;
    const auto check = [&](bool condition, const char* reason) {
        ok = condition && ok;
        if (!condition) qWarning("type strip scroll regression: %s", reason);
    };
    launcher.setScope(Launcher::Files);
    for (int i = 0; i < types.size(); ++i)
        launcher.toggleFileType(i);
    settle(window);
    auto* strip = visualNamed(window->contentItem(), u"searchTypeStrip"_s);
    check(strip && strip->property("shown").toBool() && strip->property("scrolls").toBool()
            && launcher.selectedFileTypes().size() == types.size(),
        "the strip does not scroll with every kind chosen");
    if (!strip) return false;
    const auto chipOf = [&](int type) { return visualNamed(window->contentItem(), u"searchTypeChip%1"_s.arg(type)); };
    const auto areaOf = [](QQuickItem* chip) { return visualNamed(chip, u"searchTypeChipArea"_s); };
    // Each kind's chip: shown whole and clickable, or cut off, hidden and
    // not; enabled either way, as wide wherever it is.
    QList<qreal> widths;
    const auto agree = [&](const char* where) {
        int shown = 0;
        int cut = 0;
        QList<qreal> now;
        for (int type = 0; type < types.size(); ++type) {
            QQuickItem* chip = chipOf(type);
            QQuickItem* area = chip ? areaOf(chip) : nullptr;
            if (!chip || !area) { check(false, "a kind's chip or its MouseArea is missing"); return; }
            const bool whole = chip->property("whole").toBool();
            (whole ? shown : cut) += 1;
            now.append(chip->width());
            // MouseArea.enabled is its own (mouse events only), not Item's isEnabled().
            const bool clickable = area->property("enabled").toBool();
            if (!chip->isEnabled() || clickable != whole || (chip->opacity() > 0.5) != whole) {
                qWarning("type strip scroll regression: %s: chip %d whole %d enabled %d clickable %d opacity %.2f", where, type,
                    whole, chip->isEnabled(), clickable, chip->opacity());
                ok = false;
            }
        }
        check(shown > 1 && cut > 0, "the strip does not show some chips whole and cut others off");
        check(widths.isEmpty() || widths == now, "a chip's width changed with where it is");
        widths = now;
    };
    agree("at the start");
    check(saveGrab(window, output.filePath(u"type-strip-start.png"_s)), "the window did not render");
    // Scrolled to the end, as Ctrl+Tab past the last chip shows it: the first ones cut off now.
    QQmlExpression toEnd(qmlContext(strip), strip, u"scrollTo(lastFirst(), false)"_s);
    toEnd.evaluate();
    check(!toEnd.hasError(), "the strip could not be scrolled");
    settle(window);
    check(strip->property("firstChip").toInt() > 0 && !chipOf(0)->property("whole").toBool()
            && chipOf(types.size() - 1)->property("whole").toBool(),
        "the strip did not scroll to its last chip");
    agree("at the end");
    check(saveGrab(window, output.filePath(u"type-strip-end.png"_s)), "the window did not render");
    // A whole chip still takes its click: the last kind off.
    QQuickItem* last = chipOf(types.size() - 1);
    const QPointF at = last->mapToScene(QPointF(last->width() / 2, last->height() / 2));
    for (const auto& [type, buttons] : {std::pair {QEvent::MouseButtonPress, Qt::MouseButtons(Qt::LeftButton)},
             {QEvent::MouseButtonRelease, Qt::MouseButtons()}}) {
        QMouseEvent event(type, at, window->mapToGlobal(at), Qt::LeftButton, buttons, {});
        QCoreApplication::sendEvent(window, &event);
    }
    settle(window);
    check(!launcher.selectedFileTypes().contains(int(types.size() - 1)) && launcher.selectedFileTypes().size() == types.size() - 1,
        "a click on a whole chip did not take its kind off");
    check(loops == 0, "a binding loop while the chips scroll");
    window->hide();
    say(u"type strip scroll: cut-off chips hidden and not clickable, enabled and as wide (%1)"_s.arg(SystemTheme::uiStyle()));
    return ok;
}

// --apps-grid (GitHub #296): 应用 as the settings bring it, with this PC's own
// apps: the tab last among the scopes and gone with the setting, the grid in
// place of the rows, the strip's three choices, words typed matched as 全部
// matches apps, the star, and the keys along the tiles.
bool checkAppsGrid(QQmlEngine* qmlEngine, const QDir& output)
{
    bool ok = true;
    const auto check = [&](bool condition, const QString& reason) {
        ok = condition && ok;
        if (!condition)
            qWarning().noquote() << u"apps grid: "_s + reason;
    };
    QTemporaryDir temporary;
    IndexService index(temporary.path(), IndexService::Options {});
    AppCatalog apps;
    SystemCatalog places;
    std::stop_source stop;
    apps.setApps(std::make_shared<const AppList>(loadInstalledApps(stop.get_token())));
    places.setPlaces(std::make_shared<const PlaceList>());
    History history(temporary.filePath(u"history.tsv"_s));
    Favorites favorites(temporary.filePath(u"favorites.json"_s));
    SearchEngine search(&index, &apps, &places);
    const QString stateFile = temporary.filePath(u"state.ini"_s);
    Launcher launcher(&index, &apps, &places, &search, &history);
    launcher.setFavorites(&favorites);
    launcher.setSwitches(nullptr, stateFile);
    launcher.setContentOptions(true, {u"txt"_s}, ContentSizeLimits {}, false, false);
    Settings settings;
    settings.fileTypes = Settings::defaultFileTypes();
    launcher.setFileTypes(settings.namedFileTypes());
    ClipStore store(temporary.filePath(u"clipboard"_s));
    check(store.open(QDateTime::currentMSecsSinceEpoch()), u"could not open isolated history"_s);
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    Placement placement(stateFile, u"Launcher"_s);
    WindowFrame frame(false);
    QQmlComponent component(qmlEngine, u"WinShun"_s, u"Main"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {u"launcher"_s, QVariant::fromValue(&launcher)}, {u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"frame"_s, QVariant::fromValue(&frame)}, {u"placement"_s, QVariant::fromValue(&placement)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { qWarning().noquote() << component.errorString(); return false; }
    window->create();
    launcher.setWindow(window);
    frame.setWindow(window);
    placement.setWindow(window);
    showOffScreen(window);
    settle(window);
    const auto key = [&](int code, Qt::KeyboardModifiers modifiers = {}) {
        QKeyEvent event(QEvent::KeyPress, code, modifiers);
        QCoreApplication::sendEvent(window, &event);
        settle(window, 2);
    };
    const auto count = [&] { return launcher.results()->count(); };
    const auto name = [&](int row) { return launcher.results()->data(launcher.results()->index(row), ResultModel::NameRole).toString(); };
    const auto favorite = [&](int row) { return launcher.results()->data(launcher.results()->index(row), ResultModel::FavoriteRole).toBool(); };
    auto* tabs = visualNamed(window->contentItem(), u"searchScopeTabs"_s);
    auto* grid = visualNamed(window->contentItem(), u"appsGrid"_s);
    auto* strip = visualNamed(window->contentItem(), u"searchTypeStrip"_s);
    auto* field = visualNamed(window->contentItem(), u"searchField"_s);
    check(tabs && grid && strip && field, u"the production tabs, grid, strip and field"_s);
    if (!tabs || !grid || !strip || !field)
        return false;
    field->forceActiveFocus();

    // Off, as it comes: no tab, Tab never reaches it, and it cannot be set.
    check(tabs->property("labels").toList().size() == 3, u"without the setting there are not three tabs"_s);
    launcher.setScope(Launcher::Apps);
    check(launcher.scope() == Launcher::All, u"应用 could be chosen without its tab"_s);
    for (int i = 0; i < 3; ++i)
        launcher.cycleScope(1);
    check(launcher.scope() == Launcher::All, u"Tab did not go 全部, 文件, 内容 and back without the tab"_s);

    launcher.setAppsTab(true);
    settle(window);
    check(tabs->property("labels").toList().size() == 4 && tabs->property("labels").toList().last().toString() == u"应用",
        u"应用 is not the fourth and last tab"_s);
    launcher.cycleScope(-1); // Shift+Tab from 全部
    settle(window, 6);
    check(launcher.scope() == Launcher::Apps && tabs->property("current").toInt() == 3, u"Shift+Tab from 全部 is not 应用, shown current"_s);
    check(grid->isVisible() && window->property("view").value<QObject*>() == grid, u"the grid is not the window's view"_s);
    check(strip->property("shown").toBool() && strip->property("apps").toBool() && strip->property("chips").toList().size() == 4,
        u"the strip does not show the apps' four choices"_s);
    const QList<int> counts = launcher.appCounts();
    say(u"apps: %1 frequent, %2 favorites, %3 in all; %4 columns"_s.arg(counts.value(0)).arg(counts.value(1)).arg(counts.value(2))
            .arg(grid->property("columns").toInt()));
    check(counts.value(2) > 10, u"this PC's apps were not read"_s);
    check(launcher.appsView() == Launcher::FrequentApps && count() == counts.value(0), u"常用 does not list the frequent apps"_s);
    check(saveGrab(window, output.filePath(u"apps-frequent.png"_s)), u"render 常用"_s);

    key(Qt::Key_Tab, Qt::ControlModifier);
    check(launcher.appsView() == Launcher::FavoriteApps && count() == 0, u"Ctrl+Tab did not go on to an empty 收藏"_s);
    check(saveGrab(window, output.filePath(u"apps-favorites-empty.png"_s)), u"render 收藏, empty"_s);
    key(Qt::Key_Tab, Qt::ControlModifier);
    check(launcher.appsView() == Launcher::AllApps && count() == counts.value(2), u"全部 does not list every app"_s);
    check(QSettings(stateFile, QSettings::IniFormat).value(u"Search/AppsView"_s).toString() == u"all", u"the strip's choice was not kept"_s);
    check(grid->property("count").toInt() == count() && grid->property("currentIndex").toInt() == 0, u"the grid's tiles are not the rows, the first current"_s);
    settle(window, 8);
    check(saveGrab(window, output.filePath(u"apps-all.png"_s)), u"render 全部"_s);

    // The keys along the tiles: nothing typed, so ← → move too.
    const int columns = grid->property("columns").toInt();
    key(Qt::Key_Right);
    check(grid->property("currentIndex").toInt() == 1, u"→ did not go to the second tile"_s);
    key(Qt::Key_Down);
    check(grid->property("currentIndex").toInt() == 1 + columns, u"↓ did not go one row of tiles down"_s);
    key(Qt::Key_Left);
    check(grid->property("currentIndex").toInt() == columns, u"← did not go one tile back"_s);
    key(Qt::Key_Up);
    check(grid->property("currentIndex").toInt() == 0, u"↑ did not go one row of tiles up"_s);
    key(Qt::Key_PageDown);
    const int paged = grid->property("currentIndex").toInt();
    check(paged > 0 && paged % columns == 0, u"Page Down did not go whole rows down (%1)"_s.arg(paged));
    check(grid->property("contentY").toReal() > 0, u"the grid did not scroll to the current tile"_s);
    check(saveGrab(window, output.filePath(u"apps-all-paged.png"_s)), u"render a page down"_s);

    // The star: the row's own favorite action.
    grid->setProperty("currentIndex", 0);
    const QString starred = name(0);
    launcher.trigger(0, Launcher::AddFavorite);
    settle(window, 4);
    check(favorite(0) && launcher.appCounts().value(1) == 1, u"the star did not make the first app a favorite"_s);
    launcher.setAppsView(Launcher::FavoriteApps);
    settle(window, 4);
    check(count() == 1 && name(0) == starred && favorite(0), u"收藏 does not list the starred app"_s);
    check(saveGrab(window, output.filePath(u"apps-favorites.png"_s)), u"render 收藏"_s);
    launcher.trigger(0, Launcher::RemoveFavorite);
    settle(window, 4);
    check(count() == 0 && launcher.appCounts().value(1) == 0, u"the star taken away, the app is still in 收藏"_s);

    // Words typed: as 全部 finds apps, among the ones chosen.
    launcher.setAppsView(Launcher::AllApps);
    launcher.setQuery(u"jsq"_s);
    settle(window, 4);
    say(u"jsq: %1 tile(s), first \"%2\""_s.arg(count()).arg(count() > 0 ? name(0) : QString()));
    check(count() > 0 && count() < counts.value(2), u"the pinyin initials jsq did not narrow the apps"_s);
    check(saveGrab(window, output.filePath(u"apps-typed.png"_s)), u"render the typed search"_s);
    // What Windows lists the software under, not what its shortcuts are called
    // (said, not checked: it is this PC's own software).
    for (const QString& typed : {u"microsoft office"_s, u"office ltsc"_s, u"visio"_s, u"blender foundation"_s}) {
        launcher.setQuery(typed);
        settle(window, 4);
        QStringList found;
        for (int i = 0; i < qMin(count(), 6); ++i)
            found.append(name(i));
        say(u"%1: %2 tile(s): %3"_s.arg(typed).arg(count()).arg(found.join(u", "_s)));
    }
    launcher.setQuery(u"zzzqqqxxx"_s);
    settle(window, 4);
    check(count() == 0 && !window->property("showEmptyState").toBool(), u"nothing found is not the grid's own line alone"_s);
    check(saveGrab(window, output.filePath(u"apps-none.png"_s)), u"render nothing found"_s);
    launcher.setQuery({});
    settle(window, 4);
    check(count() == counts.value(2), u"the words gone, the apps are not all back"_s);

    // 运行: the windows open now as tiles (this probe's own are not among
    // them), no star on them, narrowed by the words typed; kept as the choice.
    launcher.setAppsView(Launcher::RunningApps);
    settle(window, 6);
    const int running = launcher.appCounts().value(Launcher::RunningApps);
    say(u"运行: %1 window(s), first \"%2\""_s.arg(running).arg(count() > 0 ? name(0) : QString()));
    check(running > 0 && count() == running, u"运行 does not list the open windows"_s);
    check(count() == 0 || launcher.results()->data(launcher.results()->index(0), ResultModel::WindowResultRole).toBool(),
        u"运行's tiles are not windows"_s);
    check(QSettings(stateFile, QSettings::IniFormat).value(u"Search/AppsView"_s).toString() == u"running", u"运行 was not kept as the choice"_s);
    check(saveGrab(window, output.filePath(u"apps-running.png"_s)), u"render 运行"_s);
    grid->setProperty("currentIndex", qMin(1, count() - 1));
    pump(1700); // past a tick: the same windows are not laid out again
    check(grid->property("currentIndex").toInt() == qMin(1, count() - 1), u"运行's tick took the selection back to the first tile"_s);
    launcher.setQuery(u"zzzqqqxxx"_s);
    settle(window, 4);
    check(count() == 0, u"words no window has still list windows"_s);
    launcher.setQuery({});
    key(Qt::Key_Tab, Qt::ControlModifier);
    check(launcher.appsView() == Launcher::FrequentApps, u"Ctrl+Tab from 运行 is not 常用"_s);
    launcher.setAppsView(Launcher::AllApps);
    settle(window, 4);

    // The panel has no grid: 应用 gives way to 全部 there, and is back with the box.
    launcher.setPanel(true);
    settle(window, 6);
    check(launcher.scope() == Launcher::All && !launcher.appsTab(), u"应用 stayed in the panel"_s);
    launcher.setPanel(false);
    settle(window, 6);
    check(launcher.scope() == Launcher::Apps, u"应用 did not come back with the box"_s);
    launcher.cycleScope(1);
    check(launcher.scope() == Launcher::All, u"Tab from 应用 is not 全部"_s);
    launcher.setScope(Launcher::Apps);
    launcher.setAppsTab(false);
    settle(window, 4);
    check(launcher.scope() == Launcher::All && tabs->property("labels").toList().size() == 3 && !grid->isVisible(),
        u"the setting off, 应用 is still there"_s);
    QElapsedTimer settling;
    settling.start();
    while (launcher.searching() && settling.elapsed() < 2000)
        pump(10);
    launcher.setSwitches(nullptr, {});
    return ok;
}

bool checkNumberColumn(QQmlEngine* qmlEngine, const QDir& output, int width)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* reason) {
        ok = condition && ok;
        if (!condition)
            qWarning("number column: %s", reason);
    };
    QTemporaryDir temporary;
    IndexService index(temporary.path(), IndexService::Options {});
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    History history(temporary.filePath(u"history.tsv"_s));
    SearchEngine search(&index, &apps, &places);
    Launcher launcher(&index, &apps, &places, &search, &history);
    launcher.setResultOptions(KindOrder::FilesFirst, true, false, false, u"ctrl"_s);
    launcher.setNumberHints(u"always"_s);
    launcher.setNumberKeysLeft(0);
    EntryId firstFolderId = kNoEntry;
    {
        const auto files = index.index();
        const auto lock = files->writeLock();
        const auto root = files->addRoot(QDir::toNativeSeparators(temporary.path()).toStdString());
        for (int i = 1; i <= 24; ++i) {
            const QString name = u"编号_%1"_s.arg(i, 2, 10, QLatin1Char('0'));
            check(QDir(temporary.path()).mkdir(name), "could not create isolated folder");
            const EntryId id = files->add(root, name.toStdString(), EntryFlag::Directory);
            if (i == 1)
                firstFolderId = id;
        }
    }
    ClipStore store(temporary.filePath(u"clipboard"_s));
    check(store.open(QDateTime::currentMSecsSinceEpoch()), "could not open isolated history");
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    Placement placement(temporary.filePath(u"state.ini"_s), u"Launcher"_s);
    WindowFrame frame(false);
    QQmlComponent component(qmlEngine, u"WinShun"_s, u"Main"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {u"launcher"_s, QVariant::fromValue(&launcher)}, {u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"frame"_s, QVariant::fromValue(&frame)}, {u"placement"_s, QVariant::fromValue(&placement)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { qWarning().noquote() << component.errorString(); return false; }
    window->create();
    window->resize(width, 620);
    launcher.setWindow(window);
    frame.setWindow(window);
    placement.setWindow(window);
    showOffScreen(window);
    const auto query = [&](const QString& text) {
        launcher.setQuery(text);
        QElapsedTimer timer;
        timer.start();
        while (launcher.searching() && timer.elapsed() < 3000)
            pump(10);
        check(!launcher.searching(), "search did not settle");
        settle(window, 3);
    };
    launcher.setScope(Launcher::Folders);
    query(u"编号"_s);
    check(launcher.results()->count() == 24, "fixture results missing");
    if (launcher.results()->count() < 3)
        return false;
    auto* view = window->property("view").value<QObject*>();
    const auto verifyRows = [&] {
        const int first = window->property("firstShownRow").toInt();
        const int last = window->property("lastShownRow").toInt();
        check(last >= first && last - first < 9, "invalid visible shortcut range");
        for (int i = first; i <= last; ++i) {
            auto* row = visualNamed(window->contentItem(), u"resultRow-%1"_s.arg(i));
            auto* number = row ? visualNamed(row, u"resultNumber"_s) : nullptr;
            auto* slot = row ? visualNamed(row, u"resultNumberSlot"_s) : nullptr;
            auto* icon = row ? visualNamed(row, u"resultIcon"_s) : nullptr;
            if (!row || !number || !slot || !icon) { check(false, "production number delegate missing"); continue; }
            check(number->property("text").toString() == QString::number(i - first + 1), "number differs from shortcut target");
            check(slot->x() + slot->width() <= icon->x(), "number overlaps icon");
        }
    };
    verifyRows();
    auto* footerStatus = visualNamed(window->contentItem(), u"footerStatus"_s);
    check(footerStatus && footerStatus->property("text").toString().isEmpty(),
        "launcher footer stays quiet at the top of the list");
    check(saveGrab(window, output.filePath(u"number-column-main.png"_s)), "main did not render");
    auto* firstRow = visualNamed(window->contentItem(), u"resultRow-0"_s);
    auto* icon = firstRow ? visualNamed(firstRow, u"resultIcon"_s) : nullptr;
    auto* text = firstRow ? visualNamed(firstRow, u"resultText"_s) : nullptr;
    if (!icon || !text || !view)
        return false;
    // Hovering a left shortcut number explains the shortcut (and conflict when taken).
    auto* firstSlot = visualNamed(firstRow, u"resultNumberSlot"_s);
    auto* firstTip = firstRow->findChild<QObject*>(u"resultRowTip"_s);
    if (firstSlot && firstTip) {
        const QPointF at = firstSlot->mapToScene(QPointF(firstSlot->width() / 2, firstSlot->height() / 2));
        QMouseEvent move(QEvent::MouseMove, at, window->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &move);
        settle(window);
        check(firstTip->property("target").value<QObject*>() == firstSlot
                && firstTip->property("shortcut").toString() == u"Ctrl+1"
                && !firstTip->property("text").toString().isEmpty(),
            "hovering a launcher shortcut number explains Ctrl+1");
        launcher.setNumberKeysLeft(1u << 1);
        settle(window);
        check(firstTip->property("target").value<QObject*>() == firstSlot
                && firstTip->property("shortcut").toString().isEmpty()
                && firstTip->property("text").toString().contains(u"Ctrl+1"_s),
            "hovering a taken launcher shortcut number explains the conflict");
        launcher.setNumberKeysLeft(0);
        QMouseEvent away(QEvent::MouseMove, QPointF(window->width() - 4, 4), window->mapToGlobal(QPointF(window->width() - 4, 4)),
            Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &away);
        settle(window);
    } else {
        check(false, "launcher shortcut HoverTip missing");
    }
    const qreal iconX = icon->x();
    const qreal textX = text->x();
    launcher.setNumberHints(u"held"_s);
    pump(150);
    check(icon->x() == iconX && text->x() == textX, "hiding numbers moved result content");
    launcher.setNumberHints(u"off"_s);
    settle(window);
    check(icon->x() < iconX, "disabled hints kept an empty column");
    launcher.setNumberHints(u"always"_s);
    settle(window);
    QString opened;
    launcher.setFolderOpener([&](const QString& path) { opened = path; return true; }); // never open Explorer
    const auto press = [&](int digit, Qt::KeyboardModifier modifier) {
        auto* bar = window->property("searchBar").value<QObject*>();
        QMetaObject::invokeMethod(bar, "focusAtEnd");
        QKeyEvent event(QEvent::KeyPress, Qt::Key_0 + digit, modifier);
        QCoreApplication::sendEvent(window, &event); // Qt events only; no system hotkeys
        settle(window);
    };
    launcher.setNumberKeysLeft(1u << 3);
    press(3, Qt::ControlModifier);
    check(opened.isEmpty(), "occupied shortcut opened a result");
    auto* third = visualNamed(window->contentItem(), u"resultRow-2"_s);
    auto* taken = third ? visualNamed(third, u"resultNumber"_s) : nullptr;
    check(taken && taken->property("font").value<QFont>().strikeOut(), "occupied shortcut was not marked");
    launcher.setNumberKeysLeft(0);
    const QString expected = launcher.results()->at(2)->path;
    press(3, Qt::ControlModifier);
    check(opened == expected, "Ctrl+3 did not open the numbered result");
    // Smooth wheel gliding keeps the left 1–9 slots visually steady via hintOffsetY
    // instead of sliding and snapping every half-row, then returns hintOffsetY to 0.
    if (auto* wheelScroll = window->contentItem()->findChild<QObject*>(u"boxWheelScroll"_s)) {
        const auto* topRow = visualNamed(window->contentItem(), u"resultRow-0"_s);
        const auto* topSlot = topRow ? visualNamed(const_cast<QQuickItem*>(topRow), u"resultNumberSlot"_s) : nullptr;
        const qreal restingSlotY = topSlot ? topSlot->mapToScene(QPointF(0, 0)).y() : -1;
        wheelScroll->setProperty("animate", true);
        QMetaObject::invokeMethod(wheelScroll, "wheel", Q_ARG(QVariant, -240));
        pump(60); // mid-glide
        if (wheelScroll->property("running").toBool()) {
            const int midFirst = window->property("firstShownRow").toInt();
            auto* midRow = visualNamed(window->contentItem(), u"resultRow-%1"_s.arg(midFirst));
            auto* midSlot = midRow ? visualNamed(midRow, u"resultNumberSlot"_s) : nullptr;
            if (midSlot && restingSlotY >= 0) {
                const qreal midY = midSlot->mapToScene(QPointF(0, 0)).y();
                check(std::abs(midY - restingSlotY) <= 1.5,
                    "shortcut number 1 drifted vertically during smooth wheel scroll");
            }
        }
        pump(300); // settle after 200ms glide
        settle(window, 2);
        check(qFuzzyIsNull(window->property("hintOffsetY").toDouble()),
            "hintOffsetY did not return to 0 after wheel scroll settled");
        verifyRows();
    }
    const double rowHeight = window->property("rowHeight").toDouble();
    view->setProperty("contentY", 11.25 * rowHeight);
    settle(window, 3);
    verifyRows();
    const int first = window->property("firstShownRow").toInt();
    const int lastInView = window->property("lastRowInView").toInt();
    check(first > 0, "list did not scroll");
    const QString expectedRange = u"%1–%2 / 24"_s.arg(first + 1).arg(lastInView + 1);
    check(footerStatus && footerStatus->property("text").toString() == expectedRange,
        "launcher footer shows the scrolled range while 1-9 stay shortcuts");
    opened.clear();
    const auto* target = launcher.results()->at(first + 1);
    const QString scrolled = target ? target->path : QString();
    launcher.setResultOptions(KindOrder::FilesFirst, true, false, false, u"alt"_s);
    press(2, Qt::AltModifier);
    check(!scrolled.isEmpty() && opened == scrolled, "Alt+2 did not follow the scrolled numbering");
    check(saveGrab(window, output.filePath(u"number-column-scrolled.png"_s)), "scrolled results did not render");
    // Reproduces scrolling past page 1 and then replacing with a short list
    // (e.g. Ctrl+A, Ctrl+X -> recent rows): Qt ListView retains a non-zero
    // originY, which must never hide or offset the 1–9 hints or shortcuts.
    query(u"编号_01"_s);
    check(launcher.results()->count() == 1, "single-result query did not replace the scrolled result set");
    verifyRows();
    check(window->property("firstShownRow").toInt() == 0,
        "shrinking a scrolled launcher list lost firstShownRow == 0 (originY drift)");
    check(footerStatus && footerStatus->property("text").toString().isEmpty(),
        "launcher footer stayed non-empty after returning to the top");
    opened.clear();
    press(1, Qt::AltModifier);
    check(opened == launcher.results()->at(0)->path, "Alt+1 failed after shrinking a scrolled list");
    // Re-searching after an originY shift must keep row 0 numbered 1 (no blank leading rows).
    query(u"编号"_s);
    check(launcher.results()->count() == 24, "re-query did not restore 24 results");
    verifyRows();
    check(window->property("firstShownRow").toInt() == 0,
        "re-query after scrolled shrink left leading rows unnumbered");
    opened.clear();
    press(1, Qt::AltModifier);
    check(opened == launcher.results()->at(0)->path, "Alt+1 did not open row 0 after re-query");
    // Scrolling a 6–9 row list in a 5-row viewport keeps every row's icon aligned.
    // GitHub #425: Open in Terminal as administrator from context menu and Shift+click on the row's terminal button.
    {
        QString termFolder;
        bool termAdmin = false;
        int termCalls = 0;
        launcher.setTerminalOpener([&](const QString& folder, bool asAdmin) {
            termFolder = folder;
            termAdmin = asAdmin;
            ++termCalls;
            return true;
        });
        const QVariantList menu = launcher.menuItems(0);
        int idxTerminal = -1, idxTerminalAdmin = -1;
        for (int i = 0; i < menu.size(); ++i) {
            const int act = menu[i].toMap().value(u"action"_s, -1).toInt();
            if (act == Launcher::OpenTerminal)
                idxTerminal = i;
            else if (act == Launcher::OpenTerminalAdmin)
                idxTerminalAdmin = i;
        }
        check(idxTerminal >= 0 && idxTerminalAdmin == idxTerminal + 1,
            "context menu offers Open in Terminal as administrator right below Open in Terminal");
        const QString row0Path = launcher.results()->at(0)->path;
        launcher.trigger(0, Launcher::OpenTerminal);
        check(termCalls == 1 && termFolder == row0Path && !termAdmin,
            "OpenTerminal opens the folder with normal rights");
        launcher.trigger(0, Launcher::OpenTerminalAdmin);
        check(termCalls == 2 && termFolder == row0Path && termAdmin,
            "OpenTerminalAdmin opens the folder as administrator");
        if (auto* row0 = visualNamed(window->contentItem(), u"resultRow-0"_s)) {
            if (auto* termBtn = visualNamed(row0, u"terminalAction"_s)) {
                const QPointF center = termBtn->mapToScene(QPointF(termBtn->width() / 2, termBtn->height() / 2));
                const QPointF global = window->mapToGlobal(center);
                auto clickBtn = [&](Qt::KeyboardModifiers mods) {
                    QMouseEvent down(QEvent::MouseButtonPress, center, global, Qt::LeftButton, Qt::LeftButton, mods);
                    QMouseEvent up(QEvent::MouseButtonRelease, center, global, Qt::LeftButton, Qt::NoButton, mods);
                    QCoreApplication::sendEvent(window, &down);
                    QCoreApplication::sendEvent(window, &up);
                    settle(window);
                };
                clickBtn(Qt::NoModifier);
                check(termCalls == 3 && termFolder == row0Path && !termAdmin,
                    "clicking the terminal button opens normally");
                clickBtn(Qt::ShiftModifier);
                check(termCalls == 4 && termFolder == row0Path && termAdmin,
                    "Shift+clicking the terminal button opens as administrator");
            } else {
                check(false, "terminalAction button missing on row 0");
            }
        }
        // On a file inside a folder, OpenTerminalAdmin opens the file's parent folder as admin.
        const QString folder1Path = QDir(temporary.path()).filePath(u"编号_01"_s);
        const QString subFile = QDir(folder1Path).filePath(u"readme.txt"_s);
        {
            QFile f(subFile);
            check(f.open(QIODevice::WriteOnly), "could not create fixture file for terminal admin check");
            if (f.isOpen()) f.write("ok");
        }
        {
            const auto files = index.index();
            const auto lock = files->writeLock();
            if (firstFolderId != kNoEntry)
                files->add(firstFolderId, "readme.txt", 0);
        }
        launcher.setScope(Launcher::All);
        query(u"readme.txt"_s);
        check(launcher.results()->count() >= 1 && !launcher.results()->at(0)->isDir,
            "fixture file not found for OpenTerminalAdmin parent-folder check");
        if (launcher.results()->count() >= 1) {
            launcher.trigger(0, Launcher::OpenTerminalAdmin);
            check(termCalls == 5 && termAdmin
                    && QDir::toNativeSeparators(termFolder).compare(QDir::toNativeSeparators(folder1Path), Qt::CaseInsensitive) == 0,
                "OpenTerminalAdmin on a file opens its parent folder as administrator");
        }
        launcher.setScope(Launcher::Folders);
        query(u"编号"_s);
    }
    // 字母直达 with number keys off: passive preview follows numberHints ("always" | "held" | "off"),
    // stays distinct from armed mode (no keycap, editable search box), and pops into accent keycaps on tap.
    {
        auto* hints = visualNamed(window->contentItem(), u"launcherNumberHints"_s);
        auto* bar = window->property("searchBar").value<QObject*>();
        check(hints && bar, "launcher numberHints or searchBar missing");
        if (hints && bar) {
            launcher.setResultOptions(KindOrder::FilesFirst, true, false, false, u"off"_s);
            launcher.setNumberHints(u"always"_s);
            launcher.setLetterJump(true, u"auto"_s, u"ASDFGHJKLQWERTYUIOPZXCVBNM"_s);
            settle(window, 2);
            auto* row0 = visualNamed(window->contentItem(), u"resultRow-0"_s);
            auto* row1 = visualNamed(window->contentItem(), u"resultRow-1"_s);
            auto* slot0 = row0 ? visualNamed(row0, u"resultNumberSlot"_s) : nullptr;
            auto* num0 = row0 ? visualNamed(row0, u"resultNumber"_s) : nullptr;
            auto* num1 = row1 ? visualNamed(row1, u"resultNumber"_s) : nullptr;
            auto* cap0 = slot0 ? visualNamed(slot0, u"hintKeyCap"_s) : nullptr;
            auto* row0Icon = row0 ? visualNamed(row0, u"resultIcon"_s) : nullptr;
            auto* tip0 = row0 ? row0->findChild<QObject*>(u"resultRowTip"_s) : nullptr;
            check(slot0 && num0 && num1 && cap0 && row0Icon && tip0,
                "letter preview row slot/cap/tip missing");
            if (auto* hl = view->property("highlightItem").value<QQuickItem*>())
                check(row0 && row0->z() > hl->z(), "result delegate fell behind ListView highlightItem");
            if (slot0 && num0 && num1 && cap0 && row0Icon && tip0) {
                check(slot0->isVisible() && slot0->opacity() > 0.99
                        && num0->property("text").toString() == u"A"
                        && num1->property("text").toString() == u"S"
                        && !cap0->isVisible()
                        && !bar->property("keysOnly").toBool(),
                    "passive letter preview shows calm letters without arming search box");
                const QPointF at = slot0->mapToScene(QPointF(slot0->width() / 2, slot0->height() / 2));
                QMouseEvent move(QEvent::MouseMove, at, window->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(window, &move);
                settle(window);
                check(tip0->property("target").value<QObject*>() == slot0
                        && tip0->property("shortcut").toString().contains(u"Ctrl"_s)
                        && tip0->property("shortcut").toString().contains(u"A"_s),
                    "hovering passive letter explains how to tap jump key then letter");
                QMouseEvent away(QEvent::MouseMove, QPointF(window->width() - 4, 4),
                    window->mapToGlobal(QPointF(window->width() - 4, 4)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(window, &away);
                settle(window);
                check(saveGrab(window, output.filePath(u"number-column-letter-preview.png"_s)),
                    "passive letter preview did not render");

                // Arm letter jump: accent keycaps appear, search box becomes read-only, letter opens row.
                hints->setProperty("jumping", true);
                settle(window, 2);
                check(cap0->isVisible() && bar->property("keysOnly").toBool()
                        && num0->property("text").toString() == u"A",
                    "arming letter jump switches passive letters into accent keycaps");
                check(saveGrab(window, output.filePath(u"number-column-letter-armed.png"_s)),
                    "armed letter jump did not render");
                opened.clear();
                const QString expectedD = launcher.results()->at(2)->path;
                QMetaObject::invokeMethod(bar, "focusAtEnd");
                QKeyEvent pressD(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier);
                QCoreApplication::sendEvent(window, &pressD);
                settle(window, 2);
                check(opened == expectedD && !hints->property("jumping").toBool()
                        && !cap0->isVisible() && num0->property("text").toString() == u"A",
                    "pressing D while armed opens row 3 and returns to calm passive preview");

                // With numberHints = off: passive letters hide, gutter stays reserved, arming still shows keycaps.
                launcher.setNumberHints(u"off"_s);
                settle(window, 2);
                check(!slot0->isVisible() && row0Icon->x() == iconX,
                    "numberHints=off hides passive letters while keeping letter-jump gutter");
                hints->setProperty("jumping", true);
                settle(window, 2);
                check(slot0->isVisible() && cap0->isVisible() && num0->property("text").toString() == u"A",
                    "arming letter jump still shows accent keycaps when numberHints=off");
                hints->setProperty("jumping", false);
                settle(window);
            }
            launcher.setLetterJump(false, u"auto"_s, u"ASDFGHJKLQWERTYUIOPZXCVBNM"_s);
            launcher.setNumberHints(u"always"_s);
            launcher.setResultOptions(KindOrder::FilesFirst, true, false, false, u"alt"_s);
            settle(window, 2);
        }
    }
    launcher.setMaxRows(5);
    query(u"编号_0"_s);
    check(launcher.results()->count() == 9, "expected 9 results for 5-row alignment check");
    view->setProperty("contentY", view->property("originY").toDouble() + 2.0 * rowHeight);
    settle(window, 3);
    verifyRows();
    if (auto* row1 = visualNamed(window->contentItem(), u"resultRow-1"_s)) {
        auto* row2 = visualNamed(window->contentItem(), u"resultRow-2"_s);
        auto* icon1 = visualNamed(row1, u"resultIcon"_s);
        auto* icon2 = row2 ? visualNamed(row2, u"resultIcon"_s) : nullptr;
        check(icon1 && icon2 && icon1->x() == icon2->x(),
            "scrolling a <=9-row list shifted unnumbered rows horizontally");
    }
    launcher.setMaxRows(8);
    query(u"没有这个结果145"_s);
    opened.clear();
    press(1, Qt::AltModifier);
    check(opened.isEmpty(), "empty results retained a shortcut target");
    check(!watcher.running(), "fixture must not start clipboard capture");
    window->hide();
    say(u"number column: real Main/Launcher, Ctrl/Alt targets, occupied digits, scrolling, refresh and stable layout checked"_s);
    return ok;
}

// #153: exercise the production launcher and search workers over real files,
// with no background indexing, hooks, clipboard capture or user data.
bool checkSearchWorkload(QQmlEngine* qml, const QDir& output)
{
    QTemporaryDir temporary;
    IndexService index(temporary.filePath(u"index"_s), IndexService::Options{});
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    History history(temporary.filePath(u"history.tsv"_s));
    SearchEngine search(&index, &apps, &places);
    Launcher launcher(&index, &apps, &places, &search, &history);
    launcher.setContentOptions(true, {u"txt"_s}, ContentSizeLimits{}, false, true);
    ClipStore store(temporary.filePath(u"clipboard"_s));
    if (!store.open(QDateTime::currentMSecsSinceEpoch())) return false;
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    Placement placement(temporary.filePath(u"state.ini"_s), u"Launcher"_s);
    WindowFrame frame(false);
    QQmlComponent component(qml, u"WinShun"_s, u"Main"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {u"launcher"_s, QVariant::fromValue(&launcher)}, {u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"frame"_s, QVariant::fromValue(&frame)}, {u"placement"_s, QVariant::fromValue(&placement)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { say(component.errorString()); return false; }
    window->create();
    launcher.setWindow(window);
    frame.setWindow(window);
    placement.setWindow(window);
    showOffScreen(window);
    // A nested event loop sleeps between events. The static preview's pump()
    // intentionally spins, which would invalidate a CPU measurement here.
    const auto wait = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    const auto waitFor = [&](const auto& ready) {
        QElapsedTimer timer;
        timer.start();
        while (!ready() && timer.elapsed() < 30000) wait(10);
        return ready();
    };
    QJsonObject report;
    bool passed = true;
    const auto check = [&](const QString& name, bool okay) {
        report.insert(name, okay);
        passed = passed && okay;
        if (!okay) say(u"search workload failed: "_s + name);
    };
    const auto files = index.index();
    const EntryId root = files->addRoot(QDir::toNativeSeparators(temporary.path()).toUtf8().toStdString());
    const auto write = [&](const QString& name, const QByteArray& bytes) {
        QFile file(temporary.filePath(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return kNoEntry;
        const auto lock = files->writeLock();
        return files->add(root, name.toUtf8().toStdString(), 0);
    };
    // Real OOXML parsing work, not a sleeping/fake extractor. None of these
    // documents matches: an automatic scan would have to parse every one.
    const QByteArray xml = "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>"
        + QByteArray("<w:p><w:r><w:t>unrelated document text</w:t></w:r></w:p>").repeated(25000)
        + "</w:body></w:document>";
    const auto document = [](const QByteArray& body) {
        mz_zip_archive zip {};
        if (!mz_zip_writer_init_heap(&zip, 0, 0)) return QByteArray();
        const QByteArray types = "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/><Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/></Types>";
        const QByteArray rels = "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/></Relationships>";
        bool zipped = mz_zip_writer_add_mem(&zip, "[Content_Types].xml", types.constData(), types.size(), MZ_DEFAULT_LEVEL)
            && mz_zip_writer_add_mem(&zip, "_rels/.rels", rels.constData(), rels.size(), MZ_DEFAULT_LEVEL)
            && mz_zip_writer_add_mem(&zip, "word/document.xml", body.constData(), body.size(), MZ_DEFAULT_LEVEL);
        void* buffer = nullptr;
        size_t length = 0;
        zipped = zipped && mz_zip_writer_finalize_heap_archive(&zip, &buffer, &length);
        const QByteArray result = zipped ? QByteArray(static_cast<const char*>(buffer), static_cast<qsizetype>(length)) : QByteArray();
        mz_free(buffer);
        mz_zip_writer_end(&zip);
        return result;
    };
    const QByteArray docx = document(xml);
    if (docx.isEmpty()) return false;
    for (int i = 0; i < 64; ++i)
        check(u"fixture_%1"_s.arg(i), write(u"document-%1.docx"_s.arg(i), docx) != kNoEntry);
    const QByteArray text("first line\nneedle in the document\n");
    const EntryId cachedText = write(u"indexed.txt"_s, text);
    const EntryId uncachedText = write(u"pending.txt"_s, text);
    const QByteArray matchingDoc = document("<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body><w:p><w:r><w:t>first line</w:t></w:r></w:p><w:p><w:r><w:t>needle in the document</w:t></w:r></w:p></w:body></w:document>");
    const EntryId cachedDoc = write(u"cached.docx"_s, matchingDoc);
    const EntryId uncachedDoc = write(u"pending-match.docx"_s, matchingDoc);
    const EntryId dirtyDoc = write(u"changed.docx"_s, matchingDoc);
    const EntryId missingText = write(u"missing-cache.docx"_s, docx);
    auto content = std::const_pointer_cast<ContentIndex>(index.contentIndex());
    const auto read = ContentIndexer::readFile(QDir::toNativeSeparators(temporary.filePath(u"indexed.txt"_s)).toStdWString(), 0, {}, nullptr);
    doctext::DocText cached;
    cached.text = text.toStdString();
    const QByteArray packed = ContentIndex::packText(cached);
    check(u"seed_text"_s, content->add(cachedText, read.keys, false, read.stamp, content->changeSequence()));
    check(u"seed_document"_s, content->add(cachedDoc, read.keys, false, read.stamp, content->changeSequence(), packed));
    check(u"seed_missing_cache"_s, content->add(missingText, read.keys, false, read.stamp, content->changeSequence()));
    check(u"seed_changed_document"_s, content->add(dirtyDoc, read.keys, false, read.stamp, content->changeSequence(), packed));
    const EntryId changed[] {dirtyDoc};
    content->markChanged(changed);
    int starts = 0;
    int finishes = 0;
    QObject::connect(&search, &SearchEngine::contentProgress, &launcher,
        [&](quint64, int, int, bool finished) { if (finished) ++finishes; });
    QObject::connect(&search, &SearchEngine::resultsReady, &launcher,
        [&](quint64, const SearchResults&, qint64, qint64) { ++starts; });
    const auto contains = [&](EntryId id) {
        return launcher.results()->indexOf(QString::fromStdWString(files->wpath(id))) >= 0;
    };
    const auto cpuMs = [](HANDLE process) {
        FILETIME created {}, exited {}, kernel {}, user {};
        if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0.0;
        const auto ticks = [](FILETIME time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
        return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
    };
    std::map<DWORD, win32::UniqueHandle> extractors;
    const auto sample = [&] {
        const win32::UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        PROCESSENTRY32W entry {sizeof(entry)};
        if (snapshot.valid() && Process32FirstW(snapshot.get(), &entry)) {
            do {
                if (entry.th32ParentProcessID == GetCurrentProcessId() && _wcsicmp(entry.szExeFile, L"WinShunExtract.exe") == 0
                    && !extractors.contains(entry.th32ProcessID)) {
                    win32::UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
                    if (process.valid()) extractors.emplace(entry.th32ProcessID, std::move(process));
                }
            } while (Process32NextW(snapshot.get(), &entry));
        }
    };
    QTimer sampler;
    sampler.setInterval(50);
    QObject::connect(&sampler, &QTimer::timeout, &launcher, sample);
    sampler.start();
    const double cpuBefore = cpuMs(GetCurrentProcess());
    QElapsedTimer elapsed;
    elapsed.start();
    launcher.setQuery(u"needle"_s);
    check(u"automatic_finished"_s, waitFor([&] { return finishes > 0; }));
    report.insert(u"automatic_ms"_s, elapsed.elapsed());
    sample();
    sampler.stop();
    report.insert(u"automatic_app_cpu_ms"_s, cpuMs(GetCurrentProcess()) - cpuBefore);
    double extractorCpu = 0;
    for (const auto& [pid, process] : extractors) extractorCpu += cpuMs(process.get());
    report.insert(u"automatic_extractor_cpu_ms"_s, extractorCpu);
    report.insert(u"automatic_extractors"_s, static_cast<int>(extractors.size()));
    check(u"automatic_starts_no_extractors"_s, extractors.empty());
    check(u"automatic_cached_text"_s, contains(cachedText));
    check(u"automatic_cached_document"_s, contains(cachedDoc));
    check(u"automatic_defers_uncached_text"_s, !contains(uncachedText));
    check(u"automatic_defers_uncached_document"_s, !contains(uncachedDoc));
    check(u"automatic_defers_changed_document"_s, !contains(dirtyDoc));
    QVariantMap deferred;
    int deferredIndex = -1;
    const QVariantList hints = launcher.leftOutHints();
    for (qsizetype i = 0; i < hints.size(); ++i) {
        if (hints[i].toMap().value(u"kind"_s) == u"deferred"_s) {
            deferred = hints[i].toMap();
            deferredIndex = static_cast<int>(i);
        }
    }
    check(u"offers_content_search"_s, !deferred.isEmpty());
    check(u"counts_unindexed_changed_and_missing_cache"_s, deferred.value(u"text"_s).toString().contains(u"68"));
    const int settled = starts;
    const double idleBefore = cpuMs(GetCurrentProcess());
    wait(2000);
    report.insert(u"stationary_app_cpu_ms"_s, cpuMs(GetCurrentProcess()) - idleBefore);
    check(u"stationary_query_not_restarted"_s, starts == settled);
    settle(window);
    check(u"automatic_screenshot"_s, saveGrab(window, output.filePath(u"search-workload-all.png"_s)));
    const int before = finishes;
    check(u"qml_continuation_action"_s, QMetaObject::invokeMethod(window, "takeHint", Q_ARG(QVariant, QVariant(deferredIndex))));
    check(u"explicit_content_scope"_s, launcher.scope() == Launcher::Content);
    check(u"explicit_preserves_query"_s, launcher.query() == u"needle");
    check(u"explicit_finished"_s, !deferred.isEmpty() && waitFor([&] { return finishes > before; }));
    check(u"explicit_reads_uncached_text"_s, contains(uncachedText));
    check(u"explicit_reads_uncached_document"_s, contains(uncachedDoc));
    check(u"explicit_reads_changed_document"_s, contains(dirtyDoc));
    check(u"explicit_keeps_cached_document"_s, contains(cachedDoc));
    settle(window);
    check(u"explicit_screenshot"_s, saveGrab(window, output.filePath(u"search-workload-content.png"_s)));
    const int beforePanel = finishes;
    launcher.setScope(Launcher::All);
    check(u"panel_automatic_finished"_s, waitFor([&] { return finishes > beforePanel && !launcher.searching(); }));
    launcher.setPanel(true);
    window->resize(760, 500);
    auto* contentAction = window->findChild<QQuickItem*>(u"panelContentSearch"_s);
    check(u"panel_continuation_visible"_s, waitFor([&] {
        return !launcher.searching() && !launcher.stale() && contentAction && contentAction->isVisible();
    }));
    // App normally owns panel sizing. These off-screen windows receive no
    // native expose/resize events: synchronize the viewport before the grab.
    window->resize(760, 500);
    settle(window);
    const auto actionPainted = [&] {
        if (!contentAction) return false;
        const QImage image = window->grabWindow();
        if (image.isNull()) return false;
        const qreal scale = image.width() / qreal(window->width());
        const QRectF bounds = contentAction->mapRectToItem(window->contentItem(), contentAction->boundingRect());
        const QRect pixels = QRectF(bounds.topLeft() * scale, bounds.size() * scale).toAlignedRect().intersected(image.rect());
        if (pixels.isEmpty()) return false;
        const QColor background = image.pixelColor(pixels.topLeft());
        int ink = 0;
        for (int y = pixels.top(); y <= pixels.bottom(); ++y) {
            for (int x = pixels.left(); x <= pixels.right(); ++x) {
                const QColor color = image.pixelColor(x, y);
                if (std::abs(color.red() - background.red()) + std::abs(color.green() - background.green())
                    + std::abs(color.blue() - background.blue()) > 128) ++ink;
            }
        }
        return ink > 20;
    };
    check(u"panel_continuation_painted"_s, actionPainted());
    check(u"panel_screenshot"_s, saveGrab(window, output.filePath(u"search-workload-panel.png"_s)));
    launcher.setGallery(true);
    settle(window);
    check(u"gallery_continuation_visible"_s, contentAction && contentAction->isVisible());
    check(u"gallery_continuation_painted"_s, actionPainted());
    check(u"gallery_screenshot"_s, saveGrab(window, output.filePath(u"search-workload-gallery.png"_s)));
    launcher.setGallery(false);
    const int beforeKey = finishes;
    if (contentAction) {
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QCoreApplication::sendEvent(contentAction, &enter);
    }
    check(u"panel_keyboard_content_scope"_s, launcher.scope() == Launcher::Content);
    check(u"panel_keyboard_content_finished"_s, waitFor([&] { return finishes > beforeKey && !launcher.searching(); }));
    check(u"panel_keyboard_finds_document"_s, contains(uncachedDoc));
    const int beforeShort = finishes;
    launcher.setScope(Launcher::All);
    launcher.setQuery(u"ne"_s); // no usable gram: do not scan every indexed file
    check(u"short_query_finished"_s, waitFor([&] { return finishes > beforeShort && !launcher.searching(); }));
    bool contentRow = false;
    for (int i = 0; i < launcher.results()->count(); ++i)
        contentRow = contentRow || launcher.results()->at(i)->line > 0;
    check(u"short_query_does_not_scan_contents"_s, !contentRow);
    check(u"short_query_offers_explicit_search"_s, !launcher.leftOutHints().isEmpty());
    const int beforeFiles = finishes;
    launcher.setScope(Launcher::Files);
    launcher.setQuery(u"indexed"_s);
    check(u"file_search_finished"_s, waitFor([&] { return !launcher.searching(); }));
    check(u"file_search_keeps_name_results"_s, contains(cachedText));
    check(u"file_search_does_not_start_content_scan"_s, finishes == beforeFiles);
    window->hide();
    report.insert(u"passed"_s, passed);
    QFile result(output.filePath(u"search-workload.json"_s));
    if (!result.open(QIODevice::WriteOnly) || result.write(QJsonDocument(report).toJson()) < 0) return false;
    return passed;
}

// --excluded-recovery: production Main.qml/Panel.qml/Launcher/SearchEngine
// covering excluded-folder discovery & one-off walk, everythingCompat hidden
// matches, skipped file reveal / auto-unhide, and typed excluded paths.
bool checkExcludedRecovery(QQmlEngine* qml, const QDir& output)
{
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    const QString excludedRoot = QDir::toNativeSeparators(temporary.filePath(u"excluded_temp"_s));
    const QString otherExcluded = QDir::toNativeSeparators(temporary.filePath(u"excluded_other"_s));
    const QString targetFile = QDir::toNativeSeparators(
        temporary.filePath(u"excluded_temp/claude/f3848bdcdc85cc89/ac8bbb5d/scratchpad/members-agy.png"_s));
    const QString indexedBak = QDir::toNativeSeparators(temporary.filePath(u"indexed/report.bak"_s));
    const QString indexedDoc = QDir::toNativeSeparators(temporary.filePath(u"indexed/notes.txt"_s));
    QDir().mkpath(QFileInfo(targetFile).absolutePath());
    QDir().mkpath(otherExcluded);
    QDir().mkpath(QFileInfo(indexedBak).absolutePath());
    for (const QString& p : {targetFile, indexedBak, indexedDoc}) {
        QFile f(p);
        if (!f.open(QIODevice::WriteOnly) || f.write("fixture\n") < 0) return false;
    }

    const auto ensureFolder = [](FileIndex& files, const QString& nativePath) {
        const QStringList parts = QDir::toNativeSeparators(nativePath).split(u'\\', Qt::SkipEmptyParts);
        if (parts.isEmpty()) return kNoEntry;
        EntryId cur = kNoEntry;
        const std::string rootName = parts.first().toUtf8().toStdString();
        for (const EntryId r : files.roots()) {
            if (files.name(r) == rootName) {
                cur = r;
                break;
            }
        }
        if (cur == kNoEntry)
            cur = files.addRoot(rootName);
        for (qsizetype i = 1; i < parts.size(); ++i) {
            const std::string seg = parts[i].toUtf8().toStdString();
            EntryId child = files.findChild(cur, seg);
            if (child == kNoEntry)
                child = files.add(cur, seg, EntryFlag::Directory);
            cur = child;
        }
        return cur;
    };

    IndexService::Options opts;
    opts.content.enabled = false;
    IndexService index(temporary.filePath(u"index"_s), opts);
    {
        const auto files = index.index();
        const auto lock = files->writeLock();
        const EntryId indexedDir = ensureFolder(*files, temporary.filePath(u"indexed"_s));
        files->add(indexedDir, "report.bak", 0);
        files->add(indexedDir, "notes.txt", 0);
    }

    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    History history(temporary.filePath(u"history.tsv"_s));
    SearchEngine search(&index, &apps, &places);
    Launcher launcher(&index, &apps, &places, &search, &history);
    launcher.setContentOptions(false, {u"txt"_s}, ContentSizeLimits{}, false, false);
    launcher.setExcludedFolders({excludedRoot, otherExcluded}, {u"node_modules"_s}, false);
    launcher.setExcludedFiles({u"bak"_s}, {u"~$*"_s});

    int excludedSettingsCalls = 0;
    QObject::connect(&launcher, &Launcher::excludedSettingsRequested, &launcher, [&] { ++excludedSettingsCalls; });

    ClipStore store(temporary.filePath(u"clipboard"_s));
    if (!store.open(QDateTime::currentMSecsSinceEpoch())) return false;
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    Placement placement(temporary.filePath(u"state.ini"_s), u"Launcher"_s);
    WindowFrame frame(false);
    QQmlComponent component(qml, u"WinShun"_s, u"Main"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {u"launcher"_s, QVariant::fromValue(&launcher)}, {u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"frame"_s, QVariant::fromValue(&frame)}, {u"placement"_s, QVariant::fromValue(&placement)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { say(component.errorString()); return false; }
    window->create();
    launcher.setWindow(window);
    frame.setWindow(window);
    placement.setWindow(window);
    showOffScreen(window);

    const auto wait = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    const auto waitFor = [&](const auto& ready) {
        QElapsedTimer timer;
        timer.start();
        while (!ready() && timer.elapsed() < 15000) wait(10);
        return ready();
    };
    const auto findHint = [&](const QString& kind) -> QVariantMap {
        for (const QVariant& h : launcher.leftOutHints()) {
            const QVariantMap m = h.toMap();
            if (m.value(u"kind"_s).toString() == kind) return m;
        }
        return {};
    };
    const auto hasPath = [&](const QString& path) {
        return launcher.results()->indexOf(path) >= 0;
    };

    QJsonObject report;
    bool passed = true;
    const auto check = [&](const QString& name, bool okay) {
        report.insert(name, okay);
        passed = passed && okay;
        if (!okay) say(u"excluded recovery failed: "_s + name);
    };

    // 1. Default mode: searching members-agy.png gives 0 rows + excluded-folders hint #0.
    launcher.setScope(Launcher::All);
    launcher.setQuery(u"members-agy.png"_s);
    check(u"empty_search_settled"_s, waitFor([&] {
        return !launcher.searching() && window->property("showEmptyState").toBool() && !launcher.leftOutHints().isEmpty();
    }));
    const QVariantList emptyHints = launcher.leftOutHints();
    check(u"empty_zero_rows"_s, launcher.results()->count() == 0);
    check(u"excluded_hint_first"_s, !emptyHints.isEmpty() && emptyHints.first().toMap().value(u"kind"_s) == u"excluded-folders"_s);
    check(u"excluded_hint_has_settings"_s, !emptyHints.isEmpty() && emptyHints.first().toMap().value(u"kind2"_s) == u"excluded-settings"_s);
    wait(60);
    check(u"empty_screenshot"_s, saveGrab(window, output.filePath(u"excluded-empty.png"_s)));

    // Clicking settings link opens ExcludedPaths settings.
    launcher.searchLeftOut(u"excluded-settings"_s);
    check(u"settings_link_fires"_s, excludedSettingsCalls == 1);

    // Taking the excluded-folders hint walks disk and finds members-agy.png.
    launcher.searchLeftOut(u"excluded-folders"_s);
    check(u"walk_finds_target"_s, waitFor([&] {
        return !launcher.busy() && hasPath(targetFile);
    }));
    wait(60);
    check(u"found_screenshot"_s, saveGrab(window, output.filePath(u"excluded-found.png"_s)));

    // 2. Typing an excluded folder path directly offers walking that folder + settings.
    launcher.setQuery(excludedRoot + u"\\members-agy.png"_s);
    check(u"typed_excluded_settled"_s, waitFor([&] {
        return !launcher.searching() && !findHint(u"folder:"_s + excludedRoot).isEmpty();
    }));
    const QVariantMap folderHint = findHint(u"folder:"_s + excludedRoot);
    check(u"typed_excluded_hint_settings"_s, folderHint.value(u"kind2"_s) == u"excluded-settings"_s);
    launcher.searchLeftOut(u"folder:"_s + excludedRoot);
    check(u"typed_excluded_walk_finds"_s, waitFor([&] { return !launcher.busy() && hasPath(targetFile); }));

    // 3. Skipped extension: typing report.bak auto-unhides; typing report offers excluded-files reveal.
    launcher.setQuery(u"report.bak"_s);
    check(u"typed_skipped_ext_found"_s, waitFor([&] { return !launcher.searching() && hasPath(indexedBak); }));
    launcher.setQuery(u"report"_s);
    check(u"skipped_files_hint_offered"_s, waitFor([&] {
        return !launcher.searching() && !findHint(u"excluded-files"_s).isEmpty();
    }));
    check(u"skipped_files_hidden_initially"_s, !hasPath(indexedBak));
    launcher.searchLeftOut(u"excluded-files"_s);
    check(u"skipped_files_revealed"_s, waitFor([&] { return !launcher.busy() && hasPath(indexedBak); }));

    // 4. Panel empty state shows recovery hint cleanly.
    launcher.setPanel(true);
    window->resize(760, 500);
    launcher.setQuery(u"members-agy.png"_s);
    check(u"panel_empty_hint"_s, waitFor([&] {
        return !launcher.searching() && launcher.results()->count() == 0 && !findHint(u"excluded-folders"_s).isEmpty();
    }));
    wait(60);
    check(u"panel_screenshot"_s, saveGrab(window, output.filePath(u"excluded-panel-empty.png"_s)));
    // Panel folder filter inside an excluded subtree also offers walking that subtree directly.
    const QString excludedSub = QDir::toNativeSeparators(temporary.filePath(u"excluded_temp/claude"_s));
    launcher.setFolderFilter(excludedSub);
    check(u"panel_excluded_sub_hint"_s, waitFor([&] {
        return !launcher.searching() && !findHint(u"folder:"_s + excludedSub).isEmpty();
    }));
    launcher.searchLeftOut(u"folder:"_s + excludedSub);
    check(u"panel_excluded_sub_walk_finds"_s, waitFor([&] { return !launcher.busy() && hasPath(targetFile); }));
    launcher.setFolderFilter({});
    launcher.setPanel(false);

    // 5. everythingCompat = true: in-memory HiddenFolders count + instant reveal + typed-path auto-unhide.
    const QString fullExcluded = QString::fromStdWString(Settings::excludedCompareKey(excludedRoot));
    {
        IndexService::Options compatOpts;
        compatOpts.content.enabled = false;
        compatOpts.hidden.excludedPaths = {Settings::excludedCompareKey(excludedRoot)};
        index.setOptions(compatOpts);
        const auto files = index.index();
        const auto lock = files->writeLock();
        const EntryId sp = ensureFolder(*files, fullExcluded + u"\\claude\\f3848bdcdc85cc89\\ac8bbb5d\\scratchpad"_s);
        files->add(sp, "members-agy.png", 0);
    }
    const QString fullTarget = QString::fromStdWString(Settings::excludedCompareKey(targetFile));
    launcher.setQuery({});
    launcher.setExcludedFolders({excludedRoot, otherExcluded}, {u"node_modules"_s}, true);
    launcher.setQuery(u"members-agy.png"_s);
    check(u"compat_hidden_hint_offered"_s, waitFor([&] {
        return !launcher.searching() && launcher.results()->count() == 0 && !findHint(u"hidden-folders"_s).isEmpty();
    }));
    launcher.searchLeftOut(u"hidden-folders"_s);
    check(u"compat_hidden_revealed"_s, waitFor([&] { return !launcher.busy() && hasPath(fullTarget); }));
    launcher.setQuery(fullExcluded + u"\\members-agy.png"_s);
    check(u"compat_typed_path_auto_unhides"_s, waitFor([&] { return !launcher.searching() && hasPath(fullTarget); }));

    window->hide();
    report.insert(u"passed"_s, passed);
    QFile result(output.filePath(u"excluded-recovery.json"_s));
    if (!result.open(QIODevice::WriteOnly) || result.write(QJsonDocument(report).toJson()) < 0) return false;
    return passed;
}

// --style-buttons: production controls, process-local input and temporary data.
// Keeps icon choices distinct from text tabs and an open action marked after hover.
bool checkStyleButtons(QQmlEngine* engine, const QDir& output)
{
    QTemporaryDir temporary;
    IndexService index(temporary.path(), IndexService::Options{});
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    History history(temporary.filePath(u"history.tsv"_s));
    SearchEngine search(&index, &apps, &places);
    Launcher launcher(&index, &apps, &places, &search, &history);
    QQmlComponent component(engine);
    component.setData(R"(
        import QtQuick
        import WinShun
        Window {
            id: fixture
            required property Launcher launcher
            width: 560; height: 280; color: Theme.page
            property alias views: views
            property alias preview: preview
            property alias tabs: tabs
            property alias primary: primary
            property alias checkedFace: checkedFace
            property alias disabledButton: disabledButton
            property color accent: Theme.accent
            property color whiteInk: Theme.accentText
            Text { x: 24; y: 14; text: "图标切换 / 文字分页"; color: Theme.text; font.pixelSize: 16 }
            FileViewSwitch {
                id: views; x: 24; y: 48
                launcher: fixture.launcher
                onModeRequested: gallery => fixture.launcher.gallery = gallery
            }
            PageTabs { id: tabs; x: 272; y: 42; labels: ["常规", "高级"]; onActivated: i => current = i }
            Text { x: 24; y: 108; text: "普通操作 / 预览已打开 / 主操作 / 已开启 / 禁用"; color: Theme.subtext; font.pixelSize: 14 }
            RowAction { x: 24; y: 144; glyph: "\uE8A7" }
            RowAction { id: preview; x: 104; y: 144; glyph: "\uE8A7"; checked: true }
            FlatButton { id: primary; x: 184; y: 142; text: "确认"; highlighted: true }
            Item {
                x: 304; y: 144; width: 32; height: 32
                Hook { id: checkedFace; anchors.fill: parent; part: Theme.keySurface; checked: true; flat: true; role: "toolbar"; dpr: Screen.devicePixelRatio }
                Glyph { anchors.centerIn: parent; anchors.verticalCenterOffset: checkedFace.out("contentOffsetY", 0); glyph: "\uE71C"; size: 16; color: checkedFace.out("foreground", Theme.text) }
            }
            FlatButton { id: disabledButton; x: 400; y: 142; glyph: "\uE713"; enabled: false }
            Text { x: 24; y: 216; text: "当前项靠图标颜色与底面区分；鼠标移开仍保留状态。"; color: Theme.subtext; font.pixelSize: 14 }
        }
    )", QUrl(u"qrc:/qt/qml/WinShun/StyleButtons.qml"_s));
    std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"launcher"_s, QVariant::fromValue(&launcher)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) {
        qWarning().noquote() << "style buttons:" << component.errorString();
        return false;
    }
    showOffScreen(window);
    settle(window);
    QJsonArray checks;
    bool passed = true;
    const auto check = [&](bool okay, const QString& name) {
        checks.append(QJsonObject {{u"name"_s, name}, {u"passed"_s, okay}});
        passed = passed && okay;
        if (!okay) qWarning("style buttons: %s", qUtf8Printable(name));
    };
    const auto evaluate = [&](QObject* target, const QString& expression) {
        QQmlExpression run(qmlContext(target), target, expression);
        const QVariant result = run.evaluate();
        check(!run.hasError(), expression);
        if (run.hasError()) say(run.error().toString());
        return result;
    };
    auto* views = window->property("views").value<QObject*>();
    auto* preview = window->property("preview").value<QObject*>();
    auto* tabs = window->property("tabs").value<QObject*>();
    // A component's private ids are not in its caller's QQmlContext. Inspect
    // the actual visual Hook instead of evaluating "track" from that context.
    std::function<QObject*(QQuickItem*)> findHook = [&](QQuickItem* item) -> QObject* {
        if (item->property("built").isValid() && item->property("names").isValid()) return item;
        for (QQuickItem* child : item->childItems())
            if (QObject* found = findHook(child)) return found;
        return nullptr;
    };
    auto* viewHook = findHook(qobject_cast<QQuickItem*>(views));
    auto* tabHook = findHook(qobject_cast<QQuickItem*>(tabs));
    check(viewHook && tabHook, u"production segmented parts exist"_s);
    if (!viewHook || !tabHook) return false;
    const auto colour = [&](QObject* target, const QString& expression) {
        return evaluate(target, expression).value<QColor>();
    };
    const QColor accent = window->property("accent").value<QColor>();
    check(colour(viewHook, u"call('labelColor', [0], 'transparent')"_s) == accent, u"selected list icon uses accent ink"_s);
    check(colour(preview, u"foreground"_s) == accent, u"open preview stays marked without hover"_s);
    check(colour(window->property("primary").value<QObject*>(), u"foreground"_s) == window->property("whiteInk").value<QColor>(),
        u"primary action keeps white ink"_s);
    if (SystemTheme::uiStyle() == u"liquid")
        check(window->property("disabledButton").value<QObject*>()->property("opacity").toReal() < 1,
            u"disabled action dims its surface and content together"_s);
    if (SystemTheme::uiStyle() == u"fluent") {
        check(colour(window->property("checkedFace").value<QObject*>(), u"out('foreground', 'transparent')"_s)
                == window->property("whiteInk").value<QColor>(), u"filled checked key keeps white ink"_s);
        check(!evaluate(viewHook, u"item.children.some(c => c.visible && c.height <= 4 && c.width === 16)"_s).toBool(),
            u"icon choices have no underline"_s);
        check(evaluate(tabHook, u"item.children.some(c => c.visible && c.height <= 4 && c.width === 16)"_s).toBool(),
            u"text tabs retain their selection indicator"_s);
    }
    check(saveGrab(window, output.filePath(u"buttons-list.png"_s)), u"list state screenshot"_s);
    auto* gallery = visualNamed(window->contentItem(), u"fileViewGallery"_s);
    check(gallery, u"production gallery button exists"_s);
    if (gallery) {
        const QPointF at = gallery->mapToScene({gallery->width() / 2, gallery->height() / 2});
        const auto mouse = [&](QEvent::Type type, QPointF point, Qt::MouseButtons buttons) {
            QMouseEvent event(type, point, window->mapToGlobal(point), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, {});
            QCoreApplication::sendEvent(window, &event);
            settle(window, 2);
        };
        mouse(QEvent::MouseMove, at, {});
        check(saveGrab(window, output.filePath(u"buttons-hover.png"_s)), u"hover screenshot"_s);
        mouse(QEvent::MouseButtonPress, at, Qt::LeftButton);
        check(viewHook->property("pressedIndex").toInt() == 1, u"pointer press reaches the segmented part"_s);
        check(saveGrab(window, output.filePath(u"buttons-pressed.png"_s)), u"pressed screenshot"_s);
        mouse(QEvent::MouseButtonRelease, at, {});
        mouse(QEvent::MouseMove, {-20, -20}, {});
        check(launcher.gallery(), u"click changes the real launcher's view mode"_s);
        check(colour(viewHook, u"call('labelColor', [1], 'transparent')"_s) == accent, u"selected gallery icon stays coloured after pointer leaves"_s);
        check(colour(viewHook, u"call('labelColor', [0], 'transparent')"_s) != accent, u"previous choice loses its accent"_s);
        check(saveGrab(window, output.filePath(u"buttons-gallery.png"_s)), u"gallery state screenshot"_s);
    }
    preview->setProperty("checked", false);
    settle(window, 2);
    check(colour(preview, u"foreground"_s) != accent, u"closing preview clears its mark"_s);
    auto* primary = qobject_cast<QQuickItem*>(window->property("primary").value<QObject*>());
    primary->forceActiveFocus(Qt::TabFocusReason);
    check(primary->hasActiveFocus(), u"keyboard focus remains available"_s);
    check(saveGrab(window, output.filePath(u"buttons-focus.png"_s)), u"focus screenshot"_s);
    window->hide();
    QFile report(output.filePath(u"buttons.json"_s));
    if (!report.open(QIODevice::WriteOnly)) return false;
    report.write(QJsonDocument(QJsonObject {{u"passed"_s, passed}, {u"checks"_s, checks}}).toJson());
    return passed;
}

// --drop-pie-pages: the drop pie with more folders than a page holds (#405),
// as DropPie and DropPieWindow.qml make it, off the screen: twenty favorites
// (three pages: the first, the second with the pointer on a chip, the last of
// four), a turn caught half way through its fade, and forty, forty-eight and
// fifty-six (five and six pages: dots; seven: the middle says "4/7"), and eight
// (one page: nothing of paging shows). `--scale` as for the others.
bool checkDropPiePages(QQmlEngine* engine, const QDir& output)
{
    bool passed = true;
    const auto check = [&](bool condition, const QString& what) {
        if (!condition)
            say(u"drop pie pages: %1 FAILED"_s.arg(what));
        passed = passed && condition;
    };
    QTemporaryDir temporary;
    const QStringList names {u"项目资料"_s, u"年度报告"_s, u"设计素材"_s, u"客户合同"_s, u"归档备份"_s, u"Downloads"_s,
        u"Client Projects 2026"_s, u"财务报销单据与发票"_s, u"Invoices"_s, u"Meeting Notes and Minutes"_s, u"照片"_s,
        u"Source Code"_s, u"学习资料"_s, u"Tax"_s, u"旅行计划与行程安排"_s, u"Music"_s, u"备份"_s, u"Screenshots"_s,
        u"工作周报"_s, u"合同扫描件"_s};
    // `count` favorites, the pie opened on the screen's middle, then each of `shots` taken:
    // {page, sector the pointer is on, file name}.
    struct Shot {
        int page;
        int hovered;
        QString name;
    };
    const auto run = [&](int count, const QString& tag, const QList<Shot>& shots, bool fade) {
        Favorites favorites(temporary.filePath(tag + u".json"_s));
        for (int i = 0; i < count; ++i) {
            const QString name = names.value(i % names.size()) + (i >= names.size() ? u" "_s + QString::number(i / names.size() + 1) : QString());
            const QString folder = temporary.filePath(tag + u"/"_s + name);
            QDir().mkpath(folder);
            favorites.add({Favorite::Kind::Folder, QDir::toNativeSeparators(folder), name});
        }
        piedrop::FileOperations operations;
        DropPie pie(&favorites, nullptr, &operations, {});
        QQmlComponent component(engine, u"WinShun"_s, u"DropPieWindow"_s);
        std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"dropPie"_s, QVariant::fromValue(&pie)}}));
        auto* window = qobject_cast<QQuickWindow*>(object.get());
        check(window, tag + u": "_s + component.errorString());
        if (!window)
            return;
        pie.setWindow(window);
        QElapsedTimer waiting;
        waiting.start();
        while (pie.folderCount() < count && waiting.elapsed() < 5000)
            pump(20);
        check(pie.folderCount() == count, tag + u": its folders"_s);
        check(pie.pageCount() == (count + 7) / 8, tag + u": its pages"_s);
        QScreen* primary = QGuiApplication::primaryScreen();
        const QPoint centre = primary->geometry().center();
        const qreal scale = primary->devicePixelRatio();
        pie.preview(POINT {LONG(std::lround(centre.x() * scale)), LONG(std::lround(centre.y() * scale))}, -1);
        const QSize size = window->size();
        showOffScreen(window);
        window->resize(size);
        pump(400);
        const auto shoot = [&](const QString& name) {
            settle(window);
            pump(300);
            settle(window, 2);
            const QRectF drawn = pie.drawnRect();
            check(saveGrab(window, output.filePath(name + u".png"_s), drawn), name);
            // Where the middle is in the picture (device pixels), for measuring the dots on its pixels.
            const qreal dpr = window->devicePixelRatio();
            const QPoint corner = cropPixels(window, drawn).topLeft();
            say(u"%1: hub centre %2,%3 radius %4 (device pixels)"_s.arg(name)
                    .arg(window->property("cx").toReal() * dpr - corner.x())
                    .arg(window->property("cy").toReal() * dpr - corner.y()).arg(pie.hubRadius() * dpr));
        };
        for (const Shot& shot : shots) {
            pie.previewPage(shot.page, shot.hovered);
            pump(100);
            check(pie.page() == shot.page, shot.name + u": the page"_s);
            check(pie.sectors().size() == std::min(8, count - 8 * shot.page), shot.name + u": its chips"_s);
            // The hint to turn the page is there with several pages and nothing chosen, and only then.
            if (shot.hovered < 0)
                check(pie.statusHint().isEmpty() == (pie.pageCount() == 1), shot.name + u": the page hint"_s);
            shoot(shot.name);
        }
        if (fade) {
            // Tab at the first page: the chips of both pages on the screen at once.
            pie.previewPage(0);
            pump(100);
            pie.pageKey(false);
            pump(50);
            const qreal turn = window->property("turn").toReal();
            check(pie.page() == 1 && !pie.leavingSectors().isEmpty(), tag + u": a turn"_s);
            check(saveGrab(window, output.filePath(tag + u"-fade.png"_s), pie.drawnRect()), tag + u"-fade"_s);
            say(u"drop pie pages: the fade was at %1 when grabbed"_s.arg(turn));
            pump(400);
            check(window->property("turn").toReal() == 1, tag + u": the fade ends"_s);
            // Shift+Tab back to the first, and round to the last.
            pie.pageKey(true);
            pump(50);
            check(pie.page() == 0, tag + u": back"_s);
            pie.pageKey(true);
            pump(50);
            check(pie.page() == pie.pageCount() - 1, tag + u": round to the last"_s);
            pie.pageKey(false);
            pump(50);
            check(pie.page() == 0, tag + u": round to the first"_s);
        }
        window->hide();
    };
    run(20, u"droppie-pages"_s, {{0, -1, u"droppie-pages-1"_s}, {1, -1, u"droppie-pages-2"_s}, {1, 2, u"droppie-pages-2-hover"_s},
        {2, 3, u"droppie-pages-3"_s}}, true);
    run(56, u"droppie-pages-many"_s, {{3, -1, u"droppie-pages-many-4"_s}}, false);
    run(48, u"droppie-pages-six"_s, {{5, -1, u"droppie-pages-six-6"_s}}, false);
    run(40, u"droppie-pages-five"_s, {{2, -1, u"droppie-pages-five-3"_s}}, false);
    run(12, u"droppie-pages-two"_s, {{0, 1, u"droppie-pages-two-1"_s}}, false);
    // Eight or fewer: no pages, no dots, no hint, as before.
    run(8, u"droppie-pages-single"_s, {{0, -1, u"droppie-pages-single-1"_s}}, false);
    say(u"drop pie pages: %1"_s.arg(passed ? u"passed"_s : u"FAILED"_s));
    return passed;
}

// --drop-pie-columns: the folders inside a chip's folder, and inside a row's
// (GitHub #463), as DropPie and DropPieWindow.qml make them, off the screen:
// a column on the right with the pointer on a row (droppie-columns-right),
// one further in (-second), one on the left (-left), one too long for the
// screen scrolled down a way with the pointer on its lower strip (-scroll),
// and an empty folder's (-empty). `--scale` as for the others.
bool checkDropPieColumns(QQmlEngine* engine, const QDir& output)
{
    bool passed = true;
    const auto check = [&](bool condition, const QString& what) {
        if (!condition)
            say(u"drop pie columns: %1 FAILED"_s.arg(what));
        passed = passed && condition;
    };
    QTemporaryDir temporary;
    const auto make = [&](const QString& path) {
        QDir().mkpath(temporary.filePath(path));
        return QDir::toNativeSeparators(temporary.filePath(path));
    };
    // Six chips: 0 above, 1 and 2 on the right, 3 below, 4 and 5 on the left.
    Favorites favorites(temporary.filePath(u"favorites.json"_s));
    const QStringList favoriteNames {u"年度报告"_s, u"项目资料"_s, u"照片"_s, u"空文件夹"_s, u"客户合同"_s, u"设计素材"_s};
    for (const QString& name : favoriteNames)
        favorites.add({Favorite::Kind::Folder, make(name), name});
    // As File Explorer orders them: 2 before 10.
    const QStringList projects {u"2024"_s, u"2025"_s, u"2026"_s, u"客户 A"_s, u"客户 B"_s, u"设计稿"_s, u"归档"_s};
    for (const QString& name : projects)
        make(u"项目资料/"_s + name);
    for (int month = 1; month <= 12; ++month)
        make(u"项目资料/2026/%1 月"_s.arg(month));
    for (const QString& name : {u"财务年报"_s, u"季度报告"_s})
        make(u"年度报告/"_s + name);
    for (int day = 1; day <= 60; ++day)
        make(u"照片/2026-03-%1 旅行"_s.arg(day, 2, 10, u'0'));
    make(u"客户合同/已签"_s);
    make(u"设计素材/图标"_s);

    piedrop::FileOperations operations;
    DropPie pie(&favorites, nullptr, &operations, {});
    QQmlComponent component(engine, u"WinShun"_s, u"DropPieWindow"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"dropPie"_s, QVariant::fromValue(&pie)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    check(window, component.errorString());
    if (!window)
        return false;
    pie.setWindow(window);
    QElapsedTimer waiting;
    waiting.start();
    while (pie.folderCount() < favoriteNames.size() && waiting.elapsed() < 5000)
        pump(20);
    check(pie.folderCount() == favoriteNames.size(), u"its folders"_s);
    QScreen* primary = QGuiApplication::primaryScreen();
    const QPoint centre = primary->geometry().center();
    const qreal scale = primary->devicePixelRatio();
    const auto open = [&] {
        pie.preview(POINT {LONG(std::lround(centre.x() * scale)), LONG(std::lround(centre.y() * scale))}, -1);
        pump(50);
    };
    open();
    const QSize size = window->size();
    showOffScreen(window);
    window->resize(size);
    pump(400);
    const auto shoot = [&](const QString& name) {
        settle(window);
        pump(300);
        settle(window, 2);
        check(saveGrab(window, output.filePath(name + u".png"_s), pie.drawnRect()), name);
    };
    const auto box = [&](int column) {
        const QVariantMap item = pie.columns().value(column).toMap();
        return QRectF(item.value(u"x"_s).toReal(), item.value(u"y"_s).toReal(), item.value(u"width"_s).toReal(),
            item.value(u"height"_s).toReal());
    };
    const auto texts = [&](int column) {
        QStringList shown;
        for (const QVariant& row : pie.columns().value(column).toMap().value(u"rows"_s).toList())
            shown.append(row.toMap().value(u"text"_s).toString());
        return shown;
    };
    const auto chip = [&](int sector) {
        const QVariantMap item = pie.sectors().value(sector).toMap();
        return QRectF(pie.centre() + QPointF(item.value(u"x"_s).toReal(), item.value(u"y"_s).toReal()),
            QSizeF(item.value(u"width"_s).toReal(), pie.chipHeight()));
    };
    const QRectF screen(QPointF(0, 0), QSizeF(window->size()));

    // On the right: beside the pie, its first row level with the chip, clear of the line under the chips.
    check(pie.previewOpen(1), u"right: opened"_s);
    pie.previewColumnPointer(0, 2);
    check(pie.columns().size() == 1, u"right: one column"_s);
    check(texts(0).size() == projects.size() && texts(0).first(3) == projects.first(3),
        u"right: its rows in File Explorer's order (%1)"_s.arg(texts(0).join(u", ")));
    check(box(0).left() >= chip(1).right(), u"right: beside the chip"_s);
    check(!box(0).intersects(pie.statusRect()), u"right: clear of the line under the chips"_s);
    check(screen.contains(box(0)), u"right: on the screen"_s);
    check(pie.statusName() == projects[2] && pie.statusDetail().contains(projects[2]), u"right: the line names the row"_s);
    check(pie.columnStates().value(0).toMap().value(u"target"_s).toBool(), u"right: the row is where they go"_s);
    shoot(u"droppie-columns-right"_s);

    // Further in: the next beside the first, level with the row it came from.
    check(pie.previewOpen(2), u"second: opened"_s);
    pie.previewColumnPointer(1, 5);
    check(pie.columns().size() == 2, u"second: two columns"_s);
    check(texts(1).size() == 12 && texts(1).first() == u"1 月"_s && texts(1).at(9) == u"10 月"_s, u"second: 1 to 12"_s);
    check(box(1).left() >= box(0).right(), u"second: beside the first"_s);
    check(pie.columnStates().value(0).toMap().value(u"opener"_s).toInt() == 2, u"second: the row it came from shown"_s);
    shoot(u"droppie-columns-second"_s);

    // A chip on the left: its column on the left.
    open();
    check(pie.previewOpen(5), u"left: opened"_s);
    pie.previewColumnPointer(0, 0);
    check(box(0).right() <= chip(5).left(), u"left: beside the chip"_s);
    shoot(u"droppie-columns-left"_s);

    // Too long for the screen: the strips, scrolled.
    open();
    check(pie.previewOpen(2), u"scroll: opened"_s);
    const QVariantMap tall = pie.columns().value(0).toMap();
    check(tall.value(u"scrolls"_s).toBool() && tall.value(u"shown"_s).toInt() < 60, u"scroll: scrolls"_s);
    check(screen.contains(box(0)), u"scroll: on the screen"_s);
    pie.previewColumnPointer(0, -1, 10, 1);
    const QVariantMap scrolled = pie.columnStates().value(0).toMap();
    check(scrolled.value(u"first"_s).toInt() == 10 && scrolled.value(u"up"_s).toBool() && scrolled.value(u"down"_s).toBool(),
        u"scroll: scrolled a way"_s);
    check(scrolled.value(u"strip"_s).toInt() == 1, u"scroll: the pointer on the lower strip"_s);
    shoot(u"droppie-columns-scroll"_s);

    // An empty folder: one row that says so, nothing to drop on.
    open();
    check(pie.previewOpen(3), u"empty: opened"_s);
    pie.previewColumnPointer(0, -1);
    check(texts(0).size() == 1 && pie.columns().value(0).toMap().value(u"rows"_s).toList().value(0).toMap()
        .value(u"message"_s).toBool(), u"empty: says so"_s);
    shoot(u"droppie-columns-empty"_s);

    // Driven as a drag drives it: a DragOver at every move, and again every
    // 50 ms while the pointer is still (OLE's own asking), the pie's timers
    // and its reading of folders as they are.
    open();
    QPointF at = pie.centre();
    pie.previewPointer(at);
    const auto moveTo = [&](QPointF to, int steps, const std::function<void()>& each = {}) {
        const QPointF from = at;
        for (int i = 1; i <= steps; ++i) {
            at = from + (to - from) * (double(i) / steps);
            pie.previewPointer(at);
            pump(16);
            if (each)
                each();
        }
    };
    const auto rest = [&](int ms) {
        for (int waited = 0; waited < ms; waited += 50) {
            pie.previewPointer(at);
            pump(50);
        }
    };
    const auto state = [&](int column) { return pie.columnStates().value(column).toMap(); };
    const auto rowAt = [&](int column, int row) {
        const QVariantMap item = pie.columns().value(column).toMap();
        const double strip = item.value(u"scrolls"_s).toBool() ? item.value(u"strip"_s).toReal() : 0;
        return QPointF(box(column).left() + 40, box(column).top() + window->property("columnPadding").toReal() + strip
                + (row - state(column).value(u"first"_s).toInt() + 0.5) * window->property("columnRow").toReal());
    };

    // Passing over chips opens nothing; resting on one opens its column.
    moveTo(chip(0).center(), 6);
    moveTo(chip(1).center(), 6);
    check(pie.columns().isEmpty() && pie.hovered() == 1, u"drag: passing opens nothing"_s);
    rest(600);
    check(pie.columns().size() == 1 && pie.openedSector() == 1 && texts(0).size() == projects.size(),
        u"drag: resting on a chip opens its column"_s);

    // On the way to a lower row the pointer crosses the next chip's direction:
    // the chip it came from stays chosen, its column up.
    bool crossed = false;
    bool kept = true;
    const QPointF lower = rowAt(0, 5);
    moveTo(lower, 14, [&] {
        crossed = crossed || (!box(0).contains(at) && pie::sectorAt(pie::angleOf(at - pie.centre()), 6) == 2);
        kept = kept && pie.hovered() != 2 && pie.columns().size() == 1;
    });
    check(crossed, u"drag: the way to the row crosses the next chip's direction"_s);
    check(kept, u"drag: aiming at the column keeps the chip it came from"_s);
    rest(100);
    check(state(0).value(u"hovered"_s).toInt() == 5 && state(0).value(u"target"_s).toBool()
            && pie.statusName() == texts(0).value(5) && pie.previewPointer(at) == DROPEFFECT_COPY,
        u"drag: on the row, letting go puts them there"_s);

    // Resting on a row opens the next column beside it.
    moveTo(rowAt(0, 2), 6);
    rest(600);
    check(pie.columns().size() == 2 && texts(1).size() == 12 && state(0).value(u"opener"_s).toInt() == 2,
        u"drag: resting on a row opens its column"_s);

    // Heading for that column but stopping on the row below: the row it came
    // from stays chosen a moment (kAim), then the one under the pointer is,
    // and resting there opens its own column (an empty folder's) instead.
    moveTo(QPointF(at.x() + (box(1).left() - at.x()) / 2, rowAt(0, 3).y()), 4);
    rest(50);
    check(state(0).value(u"hovered"_s).toInt() == 2, u"drag: aimed past, the row it came from still chosen"_s);
    rest(300);
    check(state(0).value(u"hovered"_s).toInt() == 3, u"drag: stopped, the row under it chosen"_s);
    rest(500);
    check(pie.columns().size() == 2 && state(0).value(u"opener"_s).toInt() == 3
            && pie.columns().value(1).toMap().value(u"rows"_s).toList().value(0).toMap().value(u"message"_s).toBool(),
        u"drag: its column in place of the other"_s);

    // Back to the middle: the columns go.
    moveTo(pie.centre(), 12);
    rest(600);
    check(pie.columns().isEmpty() && pie.inMiddle() && pie.previewPointer(at) == DROPEFFECT_NONE,
        u"drag: resting in the middle closes them"_s);

    // A long column scrolls while the pointer rests on its strip.
    moveTo(chip(2).center(), 8);
    rest(600);
    const QVariantMap photos = pie.columns().value(0).toMap();
    check(photos.value(u"scrolls"_s).toBool(), u"drag: the long column scrolls"_s);
    const double strip = photos.value(u"strip"_s).toReal();
    moveTo(QPointF(box(0).center().x(), box(0).bottom() - window->property("columnPadding").toReal() - strip / 2), 10);
    rest(1000);
    const int down = state(0).value(u"first"_s).toInt();
    check(down >= 8 && state(0).value(u"strip"_s).toInt() == 1 && state(0).value(u"up"_s).toBool(),
        u"drag: resting on the lower strip scrolls it down (%1 rows)"_s.arg(down));
    moveTo(QPointF(box(0).center().x(), box(0).top() + window->property("columnPadding").toReal() + strip / 2), 10);
    rest(300);
    check(state(0).value(u"first"_s).toInt() < down, u"drag: the upper strip scrolls it back"_s);

    // Resting just past a chip, where its column then comes: that folder stays
    // chosen (letting go still puts them there) until the pointer moves.
    open();
    at = pie.centre();
    pie.previewPointer(at);
    double right = 0;
    for (qsizetype i = 0; i < pie.sectors().size(); ++i)
        right = std::max(right, chip(int(i)).right());
    moveTo(QPointF(right + 20, chip(1).center().y()), 8);
    rest(600);
    check(pie.columns().size() == 1 && box(0).contains(at), u"drag: the column came under the pointer"_s);
    check(pie.hovered() == 1 && pie.statusName() == favoriteNames[1] && pie.previewPointer(at) == DROPEFFECT_COPY,
        u"drag: under it, the chip stays chosen"_s);
    moveTo(at + QPointF(0, 8), 2);
    check(pie.hovered() == -1 && state(0).value(u"hovered"_s).toInt() >= 0, u"drag: moved, the row under it"_s);

    // Out past the columns' and the chips' reach: it goes.
    moveTo(QPointF(1, 1), 6);
    check(!pie.revealed() && pie.columns().isEmpty(), u"drag: leaving it, it goes"_s);

    window->hide();
    say(u"drop pie columns: %1"_s.arg(passed ? u"passed"_s : u"FAILED"_s));
    return passed;
}

// --style-windows: the small windows of every 界面风格 (themes/README.md),
// each as the app makes it, off the screen: a menu dropped from a button and
// a field with its caret (style-controls), the screenshot question
// (style-confirm), the first-run setup (style-onboarding), a row's name tip
// (style-nametip), the folder menu (style-foldermenu) and the drop pie
// (style-droppie), the pointer on an entry or sector in each.
bool checkStyleWindows(QQmlEngine* engine, const QDir& output)
{
    bool passed = true;
    const auto check = [&](bool condition, const QString& what) {
        if (!condition)
            say(u"style windows: %1 FAILED"_s.arg(what));
        passed = passed && condition;
    };
    const auto shoot = [&](QQuickWindow* window, const QString& name, const QRectF& crop = {}) {
        settle(window);
        pump(300);
        settle(window, 2);
        check(saveGrab(window, output.filePath(name + u".png"_s), crop), name);
    };
    const auto evaluate = [](QObject* object, const QString& expression) {
        QQmlExpression run(qmlContext(object), object, expression);
        const QVariant value = run.evaluate();
        if (run.hasError())
            say(u"style windows: %1"_s.arg(run.error().toString()));
        return value;
    };
    QTemporaryDir temporary;

    { // DropMenu and InputBox's caret
        QQmlComponent component(engine);
        component.setData(R"(
            import QtQuick
            import WinShun
            Window {
                id: fixture
                property alias box: box
                width: 420; height: 260; color: Theme.page
                WindowEdge { fill: Theme.page }
                FlatButton { id: add; x: 220; y: 20; width: 180; text: "添加插件" }
                DropMenu {
                    id: menu
                    anchor: add
                    menuWidth: 180
                    entries: [{ text: "从文件安装…", glyph: "\uE8E5" }, { text: "打开插件文件夹", glyph: "\uE838" },
                              { text: "删除", glyph: "\uE74D", danger: true }]
                }
                InputBox { id: box; x: 20; y: 200; width: 240; text: "可以编辑的文字" }
            }
        )", QUrl(u"qrc:/qt/qml/WinShun/StyleControls.qml"_s));
        std::unique_ptr<QObject> object(component.create());
        auto* window = qobject_cast<QQuickWindow*>(object.get());
        check(window, u"controls: "_s + component.errorString());
        if (window) {
            showOffScreen(window);
            auto* box = window->property("box").value<QObject*>();
            pump(100); // its content item sized: DropMenu.place() reads it
            // Dropped once the window has its size (DropMenu.place()), the pointer on its second entry.
            evaluate(window, u"menu.open = true; menu.hovered = 1"_s);
            // The caret shown without making the window active (the user's focus stays where it is).
            evaluate(box, u"takeFocus(); for (const c of children) if (c.cursorVisible !== undefined) { c.cursorPosition = 4; c.cursorVisible = true }"_s);
            shoot(window, u"style-controls"_s);
            window->hide();
        }
    }

    { // ScreenshotConfirm
        WindowFrame frame(false);
        QQmlComponent component(engine, u"WinShun"_s, u"ScreenshotConfirm"_s);
        std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"frame"_s, QVariant::fromValue(&frame)},
            {u"client"_s, u"Claude Code"_s}, {u"monitor"_s, u"显示器 1（2560 × 1600，主显示器）"_s}}));
        auto* window = qobject_cast<QQuickWindow*>(object.get());
        check(window, u"confirm: "_s + component.errorString());
        if (window) {
            frame.setWindow(window);
            showOffScreen(window);
            shoot(window, u"style-confirm"_s);
            window->hide();
        }
    }

    { // OnboardingWindow
        Onboarding setup(Settings::defaults());
        WindowFrame frame(false);
        QQmlComponent component(engine, u"WinShun"_s, u"OnboardingWindow"_s);
        std::unique_ptr<QObject> object(component.createWithInitialProperties(
            {{u"setup"_s, QVariant::fromValue(&setup)}, {u"frame"_s, QVariant::fromValue(&frame)}}));
        auto* window = qobject_cast<QQuickWindow*>(object.get());
        check(window, u"onboarding: "_s + component.errorString());
        if (window) {
            frame.setWindow(window);
            showOffScreen(window);
            shoot(window, u"style-onboarding"_s);
            window->hide();
        }
    }

    { // NameTip
        QObject host;
        QQmlComponent component(engine, u"WinShun"_s, u"NameTip"_s);
        std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"host"_s, QVariant::fromValue(&host)},
            {u"name"_s, u"2026 年度工程设计与交付文档（最终确认版本）.docx"_s},
            {u"folder"_s, u"F:\\项目资料\\2026\\客户项目\\工程设计与交付文档"_s},
            {u"size"_s, u"1.2 MB"_s}, {u"modified"_s, u"2026-10-15 14:32"_s}}));
        auto* window = qobject_cast<QQuickWindow*>(object.get());
        check(window, u"name tip: "_s + component.errorString());
        if (window) {
            showOffScreen(window);
            pump(100);
            window->resize(qCeil(evaluate(window, u"lines.width + 2 * padX"_s).toReal()),
                qCeil(evaluate(window, u"lines.height + 2 * padY"_s).toReal()));
            shoot(window, u"style-nametip"_s);
            window->hide();
        }
    }

    { // FolderMenuWindow
        Favorites favorites(temporary.filePath(u"favorites.json"_s));
        FolderMenu menu(nullptr, &favorites);
        menu.setRecordHistory(false);
        QQmlComponent component(engine, u"WinShun"_s, u"FolderMenuWindow"_s);
        std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"folderMenu"_s, QVariant::fromValue(&menu)}}));
        auto* window = qobject_cast<QQuickWindow*>(object.get());
        check(window, u"folder menu: "_s + component.errorString());
        if (window) {
            menu.setWindow(window);
            const auto entry = [](const QString& text, const QString& glyph, const QString& detail = {}) {
                return QVariantMap {{u"text"_s, text}, {u"detail"_s, detail}, {u"glyph"_s, glyph}, {u"action"_s, 0}};
            };
            const QVariantList folders {entry(u"项目资料"_s, u"\uE8B7"_s), entry(u"年度报告"_s, u"\uE8B7"_s)};
            menu.previewEntries({
                QVariantMap {{u"text"_s, u"收藏夹"_s}, {u"glyph"_s, u"\uE734"_s}, {u"children"_s, folders}, {u"detail"_s, u"2"_s}},
                QVariantMap {{u"text"_s, u"最近的文件夹"_s}, {u"glyph"_s, u"\uE81C"_s}, {u"children"_s, QVariantList()},
                    {u"detail"_s, u"0"_s}, {u"disabled"_s, true}},
                QVariantMap {{u"separator"_s, true}},
                entry(u"在“项目资料”里搜索"_s, u"\uE721"_s), entry(u"在此打开命令提示符"_s, u"\uE756"_s),
                entry(u"收藏这个文件夹"_s, u"\uE734"_s),
                QVariantMap {{u"separator"_s, true}},
                entry(u"Win顺 设置…"_s, u"\uE713"_s)});
            showOffScreen(window);
            pump(100);
            window->setProperty("highlighted", 3);
            shoot(window, u"style-foldermenu"_s);
            // 收藏夹 opened by the keys: its column beside the menu (#348),
            // the two as they stand side by side.
            QMetaObject::invokeMethod(window, "activate", Q_ARG(QVariant, 0), Q_ARG(QVariant, 0), Q_ARG(QVariant, false));
            auto* column = window->findChild<QQuickWindow*>(u"folderMenuColumn1"_s);
            check(column && column->isVisible(), u"folder menu column"_s);
            if (column) {
                settle(window);
                settle(column);
                pump(300);
                const qreal dpr = window->devicePixelRatio();
                const QRect bounds = window->geometry() | column->geometry();
                QImage image(bounds.size() * dpr, QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
                QPainter painter(&image);
                for (QQuickWindow* shown : {window, column}) {
                    QImage grab = shown->grabWindow();
                    grab.setDevicePixelRatio(1);
                    painter.drawImage(QPointF((shown->x() - bounds.x()) * dpr, (shown->y() - bounds.y()) * dpr), grab);
                }
                painter.end();
                check(image.save(output.filePath(u"style-foldermenu-cascade.png"_s)), u"style-foldermenu-cascade"_s);
            }
            menu.dismiss();
            window->hide();
        }
    }

    { // FolderMenuWindow with its groups at the first level (收藏夹 and 最近的文件夹 全部,
      // the favorite apps' section unfolded, 已打开的文件夹 a submenu): the rows as
      // FolderMenu groups them (favoriteGroupEntries, groupEntries, assembleGroups).
        Favorites favorites(temporary.filePath(u"favorites-inline.json"_s));
        FolderMenu menu(nullptr, &favorites);
        menu.setRecordHistory(false);
        QQmlComponent component(engine, u"WinShun"_s, u"FolderMenuWindow"_s);
        std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"folderMenu"_s, QVariant::fromValue(&menu)}}));
        auto* window = qobject_cast<QQuickWindow*>(object.get());
        check(window, u"folder menu inline: "_s + component.errorString());
        if (window) {
            menu.setWindow(window);
            const auto row = [](const QString& text, const QString& glyph, const QString& icon, const QString& detail = {}) {
                return QVariantMap {{u"text"_s, text}, {u"detail"_s, detail}, {u"glyph"_s, glyph}, {u"icon"_s, icon},
                    {u"action"_s, 0}};
            };
            const QString folderIcon = FileIconProvider::iconUrl({}, true);
            const QVariantList primary {row(u"项目资料"_s, u""_s, folderIcon, u"xm"_s),
                row(u"年度报告"_s, u""_s, folderIcon, u"nb"_s),
                row(u"合同扫描件.pdf"_s, u""_s, FileIconProvider::iconUrl(u"合同扫描件.pdf"_s, false))};
            const QVariantList apps {
                row(u"微信"_s, u""_s,
                    FileIconProvider::appIconUrl(u"{6D809377-6AF0-444B-8957-A3773F02200E}\\Tencent\\Weixin\\Weixin.exe"_s, false, false)),
                row(u"Visual Studio Code"_s, u""_s, FileIconProvider::appIconUrl(u"Microsoft.VisualStudioCode"_s, false, false)),
                row(u"终端"_s, u""_s, FileIconProvider::appIconUrl(u"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App"_s, true, false))};
            const QVariantList recent {row(u"2026 第三季度"_s, u""_s, folderIcon), row(u"客户资料"_s, u""_s, folderIcon),
                row(u"下载"_s, u""_s, folderIcon)};
            const QVariantList windows {row(u"发票"_s, u""_s, folderIcon), row(u"照片"_s, u""_s, folderIcon)};
            QVariantList entries = FolderMenu::assembleGroups({
                FolderMenu::favoriteGroupEntries(u"收藏夹"_s, u""_s, primary, apps, true, FolderMenu::kInlineAll),
                FolderMenu::groupEntries(u"最近的文件夹"_s, u""_s, recent, FolderMenu::kInlineAll),
                FolderMenu::groupEntries(u"已打开的文件夹"_s, u""_s, windows, 0)});
            entries.append(QVariantMap {{u"separator"_s, true}});
            entries.append(row(u"在“项目资料”里搜索"_s, u""_s, {}));
            entries.append(row(u"在此打开命令提示符"_s, u""_s, {}));
            entries.append(QVariantMap {{u"separator"_s, true}});
            entries.append(row(u"Win顺 设置…"_s, u""_s, {}));
            int highlight = -1; // Visual Studio Code's row
            for (int i = 0; i < entries.size(); ++i) {
                if (entries.at(i).toMap().value(u"text"_s).toString() == u"Visual Studio Code"_s)
                    highlight = i;
            }
            check(highlight >= 0, u"folder menu inline: the apps' rows unfolded"_s);
            menu.previewEntries(entries);
            showOffScreen(window);
            pump(400); // the icons, read on the provider's threads
            window->setProperty("highlighted", highlight);
            shoot(window, u"style-foldermenu-inline"_s);
            menu.dismiss();
            window->hide();
        }
    }

    { // DropPieWindow
        Favorites favorites(temporary.filePath(u"pie-favorites.json"_s));
        for (const QString& name : {u"项目资料"_s, u"年度报告"_s, u"设计素材"_s, u"客户合同"_s, u"归档备份"_s}) {
            const QString folder = temporary.filePath(name);
            QDir().mkpath(folder);
            favorites.add({Favorite::Kind::Folder, QDir::toNativeSeparators(folder), name});
        }
        piedrop::FileOperations operations;
        DropPie pie(&favorites, nullptr, &operations, {});
        QQmlComponent component(engine, u"WinShun"_s, u"DropPieWindow"_s);
        std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"dropPie"_s, QVariant::fromValue(&pie)}}));
        auto* window = qobject_cast<QQuickWindow*>(object.get());
        check(window, u"drop pie: "_s + component.errorString());
        if (window) {
            pie.setWindow(window);
            QElapsedTimer waiting;
            waiting.start();
            while (pie.sectors().size() < 5 && waiting.elapsed() < 5000)
                pump(20);
            check(pie.sectors().size() >= 5, u"drop pie: its folders"_s);
            QScreen* primary = QGuiApplication::primaryScreen();
            const QPoint centre = primary->geometry().center();
            const qreal scale = primary->devicePixelRatio();
            pie.preview(POINT {LONG(std::lround(centre.x() * scale)), LONG(std::lround(centre.y() * scale))}, 1);
            const QSize size = window->size();
            showOffScreen(window);
            window->resize(size);
            pump(400); // its reveal
            shoot(window, u"style-droppie"_s, pie.drawnRect());
            window->hide();
        }
    }
    say(u"style windows: %1"_s.arg(passed ? u"passed"_s : u"FAILED"_s));
    return passed;
}

int runResults(const QStringList& args)
{
    QTranslator translator;
    if (args.contains(u"--english"_s) && translator.load(u":/i18n/winshun_en.qm"_s))
        QCoreApplication::installTranslator(&translator);
    const QDir out(valueAfter(args, kResultsFlag));
    QDir().mkpath(out.path());
    const QString baseline = valueAfter(args, u"--rows-source");
    SystemTheme::setUiSize(valueAfter(args, u"--size", u"standard"_s));
    const QString corners = valueAfter(args, u"--corners", Settings::defaults().corners);
    if (corners == u"round" || corners == u"square")
        SystemTheme::setRoundedCorners(corners == u"round");
    if (args.contains(u"--drop-pie-hit"_s)) // its own renderer, set before any window is made
        return checkDropPieHit(out, args);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    QFont font = QGuiApplication::font();
    if (const QString family = valueAfter(args, u"--font"); !family.isEmpty())
        font.setFamily(family); // 界面字体, as chosen in the settings
    font.setStyleStrategy(QFont::NoSubpixelAntialias);
    QGuiApplication::setFont(font);
    if (args.contains(u"--clipboard-scroll"_s)) // its own engine: --qml-debug comes before one exists
        return runClipScrollBench(out, args);
    QQmlApplicationEngine engine;
    engine.addImageProvider(u"fileicon"_s, new FileIconProvider);
    engine.addImageProvider(u"clipthumb"_s, new ClipThumbnailProvider); // the clipboard rows' pictures
    if (args.contains(u"--foreground-memory"_s))
        return checkForegroundMemory(&engine, out, args.contains(u"--require-lazy-panel"_s), args.contains(u"--preload-panel"_s))
            ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--preview-interactions"_s))
        return checkPreviewInteractions(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--style-buttons"_s))
        return checkStyleButtons(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--drop-pie-pages"_s))
        return checkDropPiePages(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--drop-pie-columns"_s))
        return checkDropPieColumns(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--clipboard-sizing"_s))
        return checkClipboardInteractions(&engine, out, {}, true) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--clipboard-file"_s))
        return checkClipboardFilePreview(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--clipboard-pin-files"_s))
        return checkClipboardPinFiles(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--clipboard-paste-plain"_s))
        return checkClipboardPastePlain(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--clipboard-entry-hotkey"_s))
        return checkClipboardEntryHotkey(&engine, out, args.contains(u"--entry-hotkey-poster"_s)) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--clipboard-picture-view"_s))
        return checkClipboardPictureView(&engine, out, valueAfter(args, u"--gallery-image")) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--clipboard-select"_s))
        return checkClipboardTextSelection(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--clipboard-hover-copy"_s))
        return checkClipboardHoverCopy(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--content-kinds"_s))
        return checkContentKinds(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--type-strip-scroll"_s))
        return checkTypeStripScroll(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--type-strip-drag"_s))
        return checkTypeStripDrag(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--type-strip-counts"_s)) // by its own checks: the empty index's first searches may warn of being slow on a busy PC
        return checkTypeStripCounts(&engine, out) ? 0 : 4;
    if (args.contains(u"--clipboard-crashed-reading"_s)) // its warning when it gives up is expected
        return checkClipboardCrashedReading() ? 0 : 4;
    if (const qsizetype at = args.indexOf(u"--terminal-launch"_s); at >= 0 && at + 2 < args.size()) {
        // 在终端中打开 for real (GitHub #386, #425): <id> <folder> [command] [--admin]. Only
        // started here; the script that runs it finds the new shell, checks
        // its folder and closes it.
        const bool asAdmin = args.contains(u"--admin"_s);
        const QString id = args[at + 1], folder = args[at + 2];
        const QString candidate = args.value(at + 3);
        const QString command = candidate == u"--admin"_s ? QString() : candidate;
        auto result = std::make_shared<std::atomic<int>>(-1);
        shell::openTerminalIn(id, command, folder, [result](bool ok) { *result = ok ? 1 : 0; }, asAdmin);
        QElapsedTimer waited;
        waited.start();
        while (*result < 0 && waited.elapsed() < 10000)
            pump(50);
        say(u"terminal %1 in %2: %3"_s.arg(id, folder, *result == 1 ? u"started"_s : *result == 0 ? u"failed"_s : u"no answer"_s));
        return *result == 1 ? 0 : 4;
    }
    if (args.contains(u"--navigation-workflows"_s))
        return checkNavigationWorkflows(&engine, out, args.contains(u"--flipped"_s)) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--name-filter-poster"_s))
        return checkNameFilterPoster(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--search-experience"_s))
        return checkSearchExperience(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--pure-search"_s))
        return checkPureSearch(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--apps-grid"_s))
        return checkAppsGrid(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--result-row-spacing"_s))
        return checkResultRowSpacing(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--window-search-menu"_s))
        return checkWindowSearchAndMenu(&engine, out) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--clipboard-alignment"_s))
        return checkClipboardAlignment(&engine, out, args.contains(u"--flipped"_s)) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--content-pane"_s)) {
        engine.addImageProvider(u"thumbnail"_s, new ThumbnailProvider);
        return checkContentPane(&engine, out) ? std::min(g_warnings, 100) : 4;
    }
    if (args.contains(u"--search-workload"_s)) {
        engine.addImageProvider(u"thumbnail"_s, new ThumbnailProvider);
        return checkSearchWorkload(&engine, out) ? std::min(g_warnings, 100) : 4;
    }
    if (args.contains(u"--excluded-recovery"_s)) {
        engine.addImageProvider(u"thumbnail"_s, new ThumbnailProvider);
        return checkExcludedRecovery(&engine, out) ? std::min(g_warnings, 100) : 4;
    }
    ResultModel results;
    results.setThumbnails(false); // synthetic paths: no thumbnail handler or real file access
    engine.addImageProvider(u"thumbnail"_s, new ThumbnailProvider);
    SearchResults rows;
    for (const QString& name : {u"设计"_s, u"图纸"_s, u"照片"_s, u"合同"_s, u"资料"_s, u"归档"_s, u"参考"_s, u"备份"_s}) {
        SearchResult row;
        row.name = name;
        row.path = u"F:\\项目资料\\2026\\客户项目\\工程设计与交付文档\\机械设备\\最终确认版本\\"_s + name;
        row.isDir = true;
        row.recent = true;
        row.modified = static_cast<std::uint32_t>(QDateTime::currentSecsSinceEpoch() - 86400);
        rows.push_back(row);
    }
    results.assign(std::move(rows), {u"项目资料"_s});
    engine.rootContext()->setContextProperty(u"previewResults"_s, &results);
    QString qml = uR"(
        import QtQuick
        import WinShun
        %1
        Window {
            id: window
            width: 800; height: 60 + 8 * Theme.rowHeight
            color: Theme.background
            property alias hints: hints
            property alias field: field
            property alias resultList: resultList
            property bool sizeOrder: false
            property bool showFileSize: false
            property bool showDate: true
            Binding { target: previewResults; property: "highlightColor"; value: Theme.matchColor }
            Binding { target: previewResults; property: "highlightWeight"; value: Theme.matchWeight }
            NumberHints { id: hints; key: Qt.Key_Control; listening: true }
            TextInput {
                id: field
                x: 20; y: 16; width: parent.width - 40
                text: "文件夹 · 可读性预览"; color: Theme.text; font.pixelSize: Theme.uiTitle
                focus: true; cursorVisible: false
                Keys.onPressed: event => hints.press(event)
                Keys.onReleased: event => hints.release(event)
            }
            ListView {
                id: resultList
                y: 60; width: parent.width; height: 8 * Theme.rowHeight
                model: previewResults; currentIndex: -1
                delegate: %2ResultRow {
                    objectName: "metadataRow-" + index
                    width: resultList.width; height: Theme.rowHeight
                    hint: index + 1; hintTaken: index < 3
                    %3
                }
            }
        }
    )"_s.arg(baseline.isEmpty() ? QString() : u"import \"%1\" as Baseline"_s.arg(QUrl::fromLocalFile(baseline).toString()),
             baseline.isEmpty() ? QString() : u"Baseline."_s,
             baseline.isEmpty() ? u"hintOpacity: hints.hintOpacity; reserveHintSpace: hints.key >= 0 && hints.mode !== 'off'; showSize: window.showFileSize || window.sizeOrder; showModified: window.showDate"_s : QString());
    QQmlComponent component(&engine);
    component.setData(qml.toUtf8(), QUrl(u"qrc:/results-preview.qml"_s));
    auto* window = qobject_cast<QQuickWindow*>(component.create());
    if (!window) {
        say(component.errorString());
        return 3;
    }
    QTimer watchdog;
    // GitHub #116's readouts wait about 30 s for a server that never answers,
    // and the file gallery's checks have grown past a minute and a half
    // (cut short at 90 s, the clipboard's and the last rows' never ran).
    armWatchdog(watchdog, 240);
    showOffScreen(window);
    settle(window);
    pump(500);
    auto* hints = window->property("hints").value<QObject*>();
    auto* field = window->property("field").value<QObject*>();
    bool saved = true;
    const auto grab = [&](const QString& name) {
        saved = saveGrab(window, out.filePath(name + u".png"_s)) && saved;
    };
    const auto key = [&](QEvent::Type type) {
        QKeyEvent event(type, Qt::Key_Control, Qt::ControlModifier);
        QCoreApplication::sendEvent(field, &event);
    };
    hints->setProperty("mode", u"always"_s);
    const bool picked = args.contains(u"--picked"_s);
    if (picked) { // rows 2 and 5 picked (Ctrl+click): the style's highlight on rows that are not current
        const QString folder = u"F:\\项目资料\\2026\\客户项目\\工程设计与交付文档\\机械设备\\最终确认版本\\"_s;
        results.setSelection({folder + u"图纸"_s, folder + u"资料"_s});
        pump(600); // past any fade
    }
    pump(150);
    grab(u"always"_s);
    if (!baseline.isEmpty() || picked)
        return finish(window, saved);
    hints->setProperty("mode", u"held"_s);
    pump(100);
    grab(u"held-idle"_s);
    key(QEvent::KeyPress);
    pump(100);
    grab(u"held-100ms"_s);
    pump(200);
    grab(u"held-300ms"_s);
    if (!hints->property("shown").toBool())
        qWarning("preview: held Ctrl did not show hints");
    key(QEvent::KeyRelease);
    // Off-screen windows do not get every display frame: finish the fade
    // with explicit frames before recording its settled state.
    QElapsedTimer fade;
    fade.start();
    do {
        pump(20);
        (void)window->grabWindow();
    } while (hints->property("hintOpacity").toReal() > 0 && fade.elapsed() < 500);
    settle(window, 2); // the GPU grab can still hold the preceding frame
    grab(u"held-released"_s);
    for (int tap = 0; tap < 2; ++tap) {
        key(QEvent::KeyPress);
        pump(40);
        key(QEvent::KeyRelease);
        pump(40);
        if (hints->property("hintOpacity").toReal() != 0)
            qWarning("preview: quick Ctrl tap flashed hints");
    }
    grab(u"quick-taps"_s);
    hints->setProperty("mode", u"off"_s);
    key(QEvent::KeyPress);
    pump(300);
    grab(u"off"_s);
    key(QEvent::KeyRelease);
    if (args.contains(u"--switch-provider-probe"_s))
        return finish(window, checkSwitchProvider(window, out) && saved);
    if (args.contains(u"--scope-memory"_s))
        return finish(window, checkScopeMemory(window, out) && saved);
    if (args.contains(u"--pin-hint"_s))
        return finish(window, checkPinHint(window, out) && saved);
    if (args.contains(u"--number-column"_s))
        return finish(window, checkNumberColumn(&engine, out, valueAfter(args, u"--width", u"760"_s).toInt()) && saved);
    if (args.contains(u"--clipboard-send"_s))
        return finish(window, checkClipboardInteractions(&engine, out, {}) && saved);
    if (args.contains(u"--shortcut-panel"_s))
        return finish(window, checkShortcutPanels(&engine, out) && saved);
    if (args.contains(u"--search-logo"_s)) {
        return finish(window, checkSearchLogo(&engine, out) && saved);
    }
    if (args.contains(u"--style-windows"_s))
        return finish(window, checkStyleWindows(&engine, out) && saved);
    if (args.contains(u"--preview-hotkeys"_s) || args.contains(u"--search-navigation"_s)) {
        checkLauncherFilters(window, &engine, out, true, true);
        return finish(window, saved);
    }
    if (args.contains(u"--drop-pie-check"_s)) checkDropPieHelper(out);
    if (args.contains(u"--file-type-folder"_s)) {
        bool passed = true;
        checkLauncherFilters(window, &engine, out, false, false, &passed);
        say(passed ? u"文件类型范围：cw:、范围更新、直接选择和多选验证通过"_s : u"文件类型范围验证失败"_s);
        return finish(window, passed && saved);
    }
    if (args.contains(u"--file-metadata"_s)) {
        const QString galleryImage = valueAfter(args, u"--gallery-image");
        checkLauncherFilters(window, &engine, out, !galleryImage.isEmpty() || args.contains(u"--file-gallery"_s));
        saved = checkClipboardInteractions(&engine, out, galleryImage) && saved;
        SearchResults files;
        const QDateTime when(QDate(2024, 3, 5), QTime(14, 32));
        const QDateTime now(QDate(2026, 10, 25), QTime(15, 0));
        const auto stamp = static_cast<std::uint32_t>(when.toSecsSinceEpoch());
        const QString withTime = ResultModel::modifiedText(stamp, now, true);
        const QString withoutTime = ResultModel::modifiedText(stamp, now, false);
        if (withTime == withoutTime || !withTime.contains(QLocale().toString(when.time(), QLocale::ShortFormat)))
            qWarning("preview: older dates lost their modification time");
        for (int i = 0; i < 8; ++i) {
            SearchResult file;
            file.name = u"项目交付与验收报告-%1.pdf"_s.arg(i + 1);
            file.path = u"F:\\项目资料\\2026\\"_s + file.name;
            file.modified = stamp;
            file.size = (8 - i) * 1234567;
            files.append(file);
        }
        results.assign(std::move(files), {});
        results.setShowModifiedTime(true);
        field->setProperty("text", u"文件修改时间"_s);
        settle(window);
        grab(u"date-time"_s);
        window->setProperty("showFileSize", true);
        field->setProperty("text", u"文件大小与修改时间"_s);
        settle(window);
        grab(u"size-and-date"_s);
        window->setProperty("showDate", false);
        field->setProperty("text", u"只显示文件大小"_s);
        settle(window);
        grab(u"size-only"_s);
        window->setProperty("showFileSize", false);
        window->setProperty("sizeOrder", true);
        field->setProperty("text", u"按大小排序"_s);
        settle(window);
        grab(u"size-order"_s);

        // Real delegates/model over edge fixtures: unknown is blank, zero is
        // not, and folders/apps never pretend to have an aggregate size.
        SearchResults edges;
        for (int i = 0; i < 8; ++i)
            edges.append(*results.at(i));
        edges[1].size = 0;
        edges[2].size = -1;
        edges[3].isDir = true;
        edges[4].app = AppKind::Desktop;
        edges[5].modified = 0;
        edges[6].line = 12;
        edges[6].snippet = u"在文档中找到的内容"_s;
        edges[7].name = u"名称很长的项目交付与验收报告-最终确认版本-v3.pdf"_s;
        results.assign(std::move(edges), {});
        auto* list = window->property("resultList").value<QObject*>();
        struct MetadataMode { bool size; bool date; bool sort; };
        const MetadataMode modes[]{{false, true, false}, {true, true, false}, {true, false, false},
            {false, false, false}, {false, false, true}, {false, true, true}};
        for (const int width : {640, 760}) {
            window->setWidth(width);
            for (const auto mode : modes) {
                window->setProperty("showFileSize", mode.size);
                window->setProperty("showDate", mode.date);
                window->setProperty("sizeOrder", mode.sort);
                for (const int current : {-1, 0}) {
                    list->setProperty("currentIndex", current);
                    settle(window, 2);
                    for (int i = 0; i < results.count(); ++i) {
                        QQuickItem* row = visualNamed(window->contentItem(), u"metadataRow-%1"_s.arg(i));
                        QQuickItem* size = row ? visualNamed(row, u"fileSizeLabel"_s) : nullptr;
                        QQuickItem* date = row ? visualNamed(row, u"modifiedLabel"_s) : nullptr;
                        QQuickItem* meta = row ? visualNamed(row, u"resultMetadata"_s) : nullptr;
                        QQuickItem* text = row ? visualNamed(row, u"resultText"_s) : nullptr;
                        if (!size || !date || !meta || !text) {
                            qWarning("preview: missing metadata delegate %d", i);
                            continue;
                        }
                        const QString sizeText = results.data(results.index(i), ResultModel::SizeRole).toString();
                        const QString dateText = results.data(results.index(i), ResultModel::ModifiedRole).toString();
                        const bool sizeVisible = (mode.size || mode.sort) && !sizeText.isEmpty();
                        const bool dateVisible = mode.date && !dateText.isEmpty();
                        if (size->isVisible() != sizeVisible || date->isVisible() != dateVisible
                            || size->property("text").toString() != sizeText || date->property("text").toString() != dateText)
                            qWarning("preview: wrong metadata for row %d", i);
                        if (meta->isVisible() && (text->width() < 0 || text->x() + text->width() + 15 > meta->x()
                            || meta->x() + meta->width() > row->width()))
                            qWarning("preview: metadata overlaps text or leaves row %d", i);
                        // Two lines set on the name's and the folder's baselines (ResultRow):
                        // their boxes may touch, their letters must not. A line's letters
                        // stand on its baseline, no taller than its font.
                        const auto baseline = [](QQuickItem* label) { return label->y() + label->property("baselineOffset").toReal(); };
                        // 密集 (GitHub #352): side by side on one baseline, 12 apart.
                        const bool sideBySide = SystemTheme::uiSize() == u"dense";
                        if (sizeVisible && dateVisible
                            && (sideBySide ? size->x() + size->width() + 12 > date->x() + 0.5
                                           : baseline(date) - baseline(size) < date->property("font").value<QFont>().pixelSize()))
                            qWarning("preview: size and date overlap in row %d", i);
                    }
                }
            }
            grab(u"metadata-edges-%1"_s.arg(width));
        }
        say(u"file metadata: real delegates/model, six display combinations at 640/760px, selected/idle, zero/unknown/folder/app/content checked"_s);
    }
    say(u"renderer D3D11, DPR %1; Qt events only, not a global double-Ctrl gesture"_s.arg(window->devicePixelRatio()));
    return finish(window, saved);
}

// ---- --preview-ai-cards -----------------------------------------------------------------

// The AI settings screens (SettingsConfirm.qml, SettingsNotice.qml) over
// made-up changes: the card asking, once with the "from now on" box and once
// without it (a security setting in the batch, a long explanation, a long list),
// the notice after a change, and after 撤销.
int runAiCards(const QStringList& args)
{
    const QDir out(valueAfter(args, kAiCardsFlag));
    QQmlApplicationEngine engine;
    const auto change = [](const QString& title, const QString& oldText, const QString& newText, bool askAlways = false,
                            const QString& diff = {}, const QString& effect = u"now"_s) {
        return QVariantMap{{u"key"_s, u"Preview/"_s + title}, {u"title"_s, title}, {u"old_text"_s, oldText}, {u"new_text"_s, newText},
            {u"diff_text"_s, diff}, {u"is_list"_s, !diff.isEmpty()}, {u"ask_always"_s, askAlways}, {u"changed"_s, true}, {u"effect"_s, effect}};
    };
    const QVariantList few {
        change(u"启动器热键"_s, u"双击 Ctrl"_s, u"Alt+Space"_s),
        change(u"搜索结果排序"_s, u"最匹配"_s, u"最近修改"_s),
        change(u"不索引的文件夹"_s, {}, {}, false, u"+ node_modules, .git / − Downloads"_s),
    };
    QVariantList many = few;
    many << change(u"开机自启"_s, u"关"_s, u"开"_s, true)
         << change(u"用 Win+V 打开剪贴板历史"_s, u"关"_s, u"开"_s, false, {}, u"explorer_restart"_s)
         << change(u"剪贴板保留条数"_s, u"1000"_s, u"5000"_s)
         << change(u"网站搜索"_s, {}, {}, true, u"+ gh（GitHub 搜索）"_s);

    bool saved = true;
    const auto card = [&](const QString& name, const QString& reason, const QVariantList& items, bool askAlways) {
        auto* frame = new WindowFrame(false, &engine);
        QQmlComponent component(&engine, u"WinShun"_s, u"SettingsConfirm"_s);
        QObject* object = component.createWithInitialProperties({{u"frame"_s, QVariant::fromValue(frame)}, {u"client"_s, u"Claude Desktop"_s},
            {u"reason"_s, reason}, {u"items"_s, items}, {u"askAlways"_s, askAlways}});
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) {
            say(component.errorString());
            delete object;
            saved = false;
            return;
        }
        showOffScreen(window);
        settle(window);
        pump(300);
        saved = saveGrab(window, out.filePath(name)) && saved;
        window->hide();
        delete window;
    };
    card(u"confirm.png"_s, u"你说想换一个顺手的热键，并且别让下载文件夹出现在搜索里。"_s, few, false);
    card(u"confirm-always.png"_s,
        u"用户说这台电脑常处理代码，希望开机就能用，剪贴板也想用 Win+V；顺便把常用的几项都调好：排序、热键、不索引的文件夹，以及网站搜索里加 GitHub。这一句故意写得很长，看看超过三行时会不会被截断，而不是把窗口撑得很高。"_s,
        many, true);

    const auto notice = [&](const QString& name, bool undone) {
        auto* frame = new WindowFrame(false, &engine);
        auto* host = new QObject(&engine);
        QQmlComponent component(&engine, u"WinShun"_s, u"SettingsNotice"_s);
        QObject* object = component.createWithInitialProperties({{u"frame"_s, QVariant::fromValue(frame)}, {u"host"_s, QVariant::fromValue(host)}});
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) {
            say(component.errorString());
            delete object;
            saved = false;
            return;
        }
        showOffScreen(window);
        const QVariantMap batch {{u"id"_s, 1}, {u"client"_s, u"Claude Desktop"_s}, {u"count"_s, 6},
            {u"explorerRestart"_s, true}, {u"items"_s, many.mid(0, 6)}};
        QMetaObject::invokeMethod(window, "present", Q_ARG(QVariant, batch));
        if (undone) {
            window->setProperty("outcome", u"已撤销"_s);
            window->setProperty("outcomeOk", true);
        }
        settle(window);
        pump(300);
        saved = saveGrab(window, out.filePath(name)) && saved;
        window->hide();
        delete window;
    };
    notice(u"notice.png"_s, false);
    notice(u"notice-undone.png"_s, true);
    say(u"%1 warning(s)"_s.arg(g_warnings));
    return saved ? std::min(g_warnings, 100) : 4;
}

// Starts the production App with an isolated profile and no global shortcuts,
// shell integration, update checks or service access. Optional clipboard
// recording reads the system clipboard but never changes it. Reports contain
// lifecycle and resource counters, never clipboard contents or user file names.
bool forgetRuntimeIdentity()
{
    // LocalSend persists a named CNG key as well as files. Forget only the
    // identity created in this probe's private profile, before removing it.
    QFile file(Settings::profile() + u"/plugins/winshun.localsend.data/identity.key"_s);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QString name = QString::fromUtf8(file.readAll());
    if (!name.startsWith(u"WinShun.LocalSend."))
        return false;
    HMODULE module = LoadLibraryExW(L"ncrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        return false;
    const auto unload = qScopeGuard([&] { FreeLibrary(module); });
    const auto openProvider = reinterpret_cast<decltype(&NCryptOpenStorageProvider)>(GetProcAddress(module, "NCryptOpenStorageProvider"));
    const auto openKey = reinterpret_cast<decltype(&NCryptOpenKey)>(GetProcAddress(module, "NCryptOpenKey"));
    const auto deleteKey = reinterpret_cast<decltype(&NCryptDeleteKey)>(GetProcAddress(module, "NCryptDeleteKey"));
    const auto freeObject = reinterpret_cast<decltype(&NCryptFreeObject)>(GetProcAddress(module, "NCryptFreeObject"));
    if (!openProvider || !openKey || !deleteKey || !freeObject)
        return false;
    NCRYPT_PROV_HANDLE provider = 0;
    NCRYPT_KEY_HANDLE key = 0;
    const auto release = qScopeGuard([&] {
        if (key)
            freeObject(key);
        if (provider)
            freeObject(provider);
    });
    if (openProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS)
        return false;
    const SECURITY_STATUS opened = openKey(provider, &key, name.toStdWString().c_str(), 0, NCRYPT_SILENT_FLAG);
    if (opened == NTE_BAD_KEYSET || opened == NTE_NOT_FOUND)
        return true;
    if (opened != ERROR_SUCCESS || deleteKey(key, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS)
        return false;
    key = 0;
    return true;
}

// Every place and size a window is given while this lives, as Windows
// tells it (WM_WINDOWPOSCHANGED): where its bottom edge has been.
class BottomWatch : public QAbstractNativeEventFilter {
public:
    explicit BottomWatch(HWND window)
        : m_window(window)
    {
        QCoreApplication::instance()->installNativeEventFilter(this);
    }
    ~BottomWatch() override { QCoreApplication::instance()->removeNativeEventFilter(this); }
    bool nativeEventFilter(const QByteArray&, void* message, qintptr*) override
    {
        const MSG* msg = static_cast<const MSG*>(message);
        RECT at {};
        if (msg->hwnd == m_window && msg->message == WM_WINDOWPOSCHANGED && ::GetWindowRect(m_window, &at)) {
            ++changes;
            lowest = std::max(lowest, at.bottom);
            highest = std::min(highest, at.bottom);
            tallest = std::max(tallest, at.bottom - at.top);
        }
        return false;
    }
    int changes = 0;
    LONG lowest = LONG_MIN, highest = LONG_MAX, tallest = 0;

private:
    HWND m_window;
};

bool probeFolderTyping(App& app, const QDir& out, const QString& folder, QJsonObject& report)
{
    using Microsoft::WRL::ComPtr;
    bool ok = true;
    QJsonArray checks;
    const auto check = [&](bool passed, const char* name) {
        checks.append(QJsonObject {{u"name"_s, QString::fromUtf8(name)}, {u"passed"_s, passed}});
        ok = passed && ok;
        if (!passed) qWarning("folder typing: %s", name);
        report.insert(u"folder_typing"_s, checks);
    };
    const HRESULT initialized = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto uninitialize = qScopeGuard([&] { if (SUCCEEDED(initialized)) ::CoUninitialize(); });
    ComPtr<IShellWindows> windows;
    if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows))))
        return false;
    const auto browsers = [&] {
        std::vector<ComPtr<IWebBrowser2>> found;
        long count = 0;
        windows->get_Count(&count);
        for (long i = 0; i < count; ++i) {
            VARIANT at {}; at.vt = VT_I4; at.lVal = i;
            ComPtr<IDispatch> dispatch;
            ComPtr<IWebBrowser2> web;
            if (windows->Item(at, &dispatch) == S_OK && dispatch && SUCCEEDED(dispatch.As(&web)))
                found.push_back(web);
        }
        return found;
    };
    std::vector<SHANDLE_PTR> before;
    for (const auto& web : browsers()) { SHANDLE_PTR hwnd = 0; web->get_HWND(&hwnd); before.push_back(hwnd); }
    const HWND previous = ::GetForegroundWindow();
    HWND root = nullptr, tab = nullptr;
    ComPtr<IWebBrowser2> owned;
    const auto cleanup = qScopeGuard([&] {
        app.hideLauncher();
        if (owned) {
            int tabs = 0;
            for (const auto& web : browsers()) {
                SHANDLE_PTR hwnd = 0; web->get_HWND(&hwnd);
                if (reinterpret_cast<HWND>(hwnd) == root) ++tabs;
            }
            // Never close a pre-existing window, or user-added tabs.
            const QString shown = QString::fromStdWString(filemanager::explorerFolder(tab));
            if (tabs == 1 && (shown == folder || shown.startsWith(folder + u'\\'))) {
                ::PostMessageW(root, WM_CLOSE, 0, 0); // Quit() leaves a Windows 11 tab window open
                QElapsedTimer closing; closing.start();
                while (::IsWindow(root) && closing.elapsed() < 2000) pump(20);
            }
        }
        if (::IsWindow(previous)) ::SetForegroundWindow(previous);
    });
    const std::wstring command = L"/n,/separate,\"" + folder.toStdWString() + L"\"";
    if (reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", L"explorer.exe", command.c_str(), nullptr, SW_SHOWNORMAL)) <= 32)
        return false;
    QElapsedTimer waiting; waiting.start();
    while (!owned && waiting.elapsed() < 15000) {
        for (const auto& web : browsers()) {
            SHANDLE_PTR hwnd = 0; web->get_HWND(&hwnd);
            if (std::ranges::find(before, hwnd) != before.end()) continue;
            ComPtr<IServiceProvider> services;
            ComPtr<IShellBrowser> browser;
            HWND candidate = nullptr;
            if (SUCCEEDED(web.As(&services)) && SUCCEEDED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))
                && SUCCEEDED(browser->GetWindow(&candidate))
                && QString::fromStdWString(filemanager::explorerFolder(candidate)) == folder) {
                owned = web; root = reinterpret_cast<HWND>(hwnd); tab = candidate; break;
            }
        }
        pump(30);
    }
    check(owned != nullptr, "own Explorer fixture opened in a new window");
    if (!owned) return false;
    MONITORINFO monitor {sizeof monitor};
    ::GetMonitorInfoW(::MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT area = monitor.rcWork;
    ::SetWindowPos(root, nullptr, area.left + 8, area.top + 8, (area.right - area.left) / 3,
        area.bottom - area.top - 16, SWP_NOZORDER | SWP_NOACTIVATE);
    pump(300);
    ComPtr<IServiceProvider> services;
    ComPtr<IShellBrowser> browser;
    owned.As(&services);
    if (!services || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))) return false;
    IShellBrowser* shown = browser.Get(); // whose selection is read: a tab of its own later
    const auto selected = [&] {
        ComPtr<IShellView> shellView;
        ComPtr<IFolderView2> view;
        ComPtr<IShellItemArray> selection;
        ComPtr<IShellItem> item;
        DWORD count = 0;
        int focused = -1;
        if (FAILED(shown->QueryActiveShellView(&shellView)) || FAILED(shellView.As(&view))
            || FAILED(view->GetSelection(FALSE, &selection)) || !selection
            || FAILED(selection->GetCount(&count)) || count != 1
            || FAILED(view->GetFocusedItem(&focused)) || focused < 0
            || FAILED(view->GetItem(focused, IID_PPV_ARGS(&item)))) return QString();
        PWSTR path = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return QString();
        const QString result = QString::fromWCharArray(path);
        ::CoTaskMemFree(path);
        return result;
    };
    ::SetForegroundWindow(root);
    app.showFolderSearch(root, tab, App::TypingIn::Explorer, folder);
    QQuickWindow* window = nullptr;
    Launcher* launcher = nullptr;
    for (auto* candidate : QGuiApplication::allWindows()) {
        if (auto* controller = candidate->property("launcher").value<Launcher*>()) {
            window = qobject_cast<QQuickWindow*>(candidate); launcher = controller; break;
        }
    }
    if (!window || !launcher) return false;
    const QString prefix = folder + u"\\ "_s;
    int lowTall = 0; // the box with kFolderTypingMany rows in a window low on the screen
    const auto search = [&](const QString& words, int count) {
        launcher->setQuery(prefix + words);
        waiting.restart();
        while ((launcher->searching() || launcher->results()->count() != count) && waiting.elapsed() < 5000)
            pump(20);
        return !launcher->searching() && launcher->results()->count() == count;
    };
    const bool found = search(u"follow_"_s, 3);
    check(found, "production search found three real direct children");
    if (!found) return false;
    settle(window, 3);
    auto* field = window->property("searchBar").value<QObject*>();
    if (field) QMetaObject::invokeMethod(field, "focusAtEnd");
    const auto key = [&](int code) {
        QKeyEvent event(QEvent::KeyPress, code, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event);
    };
    const auto follows = [&] {
        const auto* row = launcher->results()->at(launcher->currentRow());
        if (!row) return false;
        const QString expected = row->path;
        waiting.restart();
        while (selected() != expected && waiting.elapsed() < 3000) pump(20);
        return selected() == expected;
    };
    check(launcher->currentRow() == 0 && follows(), "initial result selected and focused in the real Shell view");
    key(Qt::Key_Down);
    check(launcher->currentRow() == 1 && follows(), "Down follows the production QML current row");
    key(Qt::Key_Up);
    check(launcher->currentRow() == 0 && follows(), "Up follows the production QML current row");
    key(Qt::Key_Down); key(Qt::Key_Down); key(Qt::Key_Up); key(Qt::Key_Up); key(Qt::Key_Down);
    check(launcher->currentRow() == 1 && follows(), "rapid navigation selects the latest row");
    check(::GetForegroundWindow() == reinterpret_cast<HWND>(window->winId()) && window->isActive(),
        "Explorer selection never took keyboard focus");
    RECT searchRect {}, sourceRect {};
    ::GetWindowRect(reinterpret_cast<HWND>(window->winId()), &searchRect);
    ::GetWindowRect(root, &sourceRect);
    const RECT fileList = folderview::explorerTypingCorner(root, tab);
    check(searchRect.right <= sourceRect.right && searchRect.left >= sourceRect.left
            && searchRect.top >= fileList.top && searchRect.bottom <= fileList.bottom + 1, // Qt rounds its top and its height each
        "compact search stays inside the originating Explorer file area");
    report.insert(u"compact_box"_s, QJsonArray {int(searchRect.left), int(searchRect.top), int(searchRect.right), int(searchRect.bottom)});
    report.insert(u"compact_list"_s, QJsonArray {int(fileList.left), int(fileList.top), int(fileList.right), int(fileList.bottom)});
    report.insert(u"compact_window"_s, QJsonArray {int(sourceRect.left), int(sourceRect.top), int(sourceRect.right), int(sourceRect.bottom)});
    check(window->width() <= 460, "folder search uses a compact width");
    check(field && field->property("text").toString() == u"follow_"_s,
        "folder input shows only the query, keeping its path out of editable text");
    if (field) {
        field->setProperty("text", u"follow_2"_s);
        waiting.restart();
        while ((launcher->searching() || launcher->results()->count() != 1) && waiting.elapsed() < 5000)
            pump(20);
        check(launcher->query() == prefix + u"follow_2"_s && launcher->results()->count() == 1,
            "editing the visible query preserves the folder context and searches it");
        field->setProperty("text", QString());
        check(launcher->query() == prefix && !launcher->typingFolder().isEmpty(),
            "clearing the query keeps the folder context");
        check(search(u"follow_"_s, 3), "query restored after input editing");
        settle(window, 3);
    }
    const auto bounds = [](const RECT& rect) { return QJsonArray {int(rect.left), int(rect.top),
        int(rect.right - rect.left), int(rect.bottom - rect.top)}; };
    report.insert(u"source_bounds"_s, bounds(sourceRect));
    report.insert(u"search_bounds"_s, bounds(searchRect));
    if (window->screen()) {
        // Read the composed desktop so native Mica/title bars are captured
        // correctly. Save only the two test-owned window regions.
        const auto desktop = window->screen()->grabWindow(0);
        const auto region = [&](const RECT& rect) {
            return QRect(rect.left - monitor.rcMonitor.left, rect.top - monitor.rcMonitor.top,
                rect.right - rect.left, rect.bottom - rect.top);
        };
        check(desktop.copy(region(searchRect)).save(out.filePath(u"typing-search.png"_s)), "production search screenshot saved");
        check(desktop.copy(region(sourceRect)).save(out.filePath(u"typing-explorer.png"_s)), "Explorer selection screenshot saved");
    }

    // GitHub #246: only the folder searched, and more rows than the box
    // shows. Where it looked and the ways to widen or narrow that stay in
    // sight under the rows (ScopeBar), not after the last row; its link,
    // clicked, searches everywhere.
    const std::function<QQuickItem*(QQuickItem*, const QString&)> named = [&](QQuickItem* item, const QString& name) -> QQuickItem* {
        if (item->objectName() == name)
            return item;
        for (QQuickItem* child : item->childItems())
            if (QQuickItem* found = named(child, name))
                return found;
        return nullptr;
    };
    launcher->setTypingScope(Launcher::TypingScope::Folder);
    if (search(u"many_"_s, kFolderTypingMany)) {
        settle(window, 3);
        const auto* rows = window->property("view").value<QQuickItem*>();
        check(rows && rows->property("contentHeight").toReal() > rows->height(), "folder only: more rows than the box shows");
        QQuickItem* bar = named(window->contentItem(), u"scopeBar"_s);
        const QRectF barRect = bar ? bar->mapRectToScene(QRectF(0, 0, bar->width(), bar->height())) : QRectF();
        check(bar && bar->isVisible() && barRect.height() > 0 && barRect.top() >= 0 && barRect.bottom() <= window->height(),
            "the scope bar is in sight without scrolling");
        const QVariantList footer = window->property("hints").toList();
        check(std::none_of(footer.cbegin(), footer.cend(), [](const QVariant& hint) {
            const QString kind = hint.toMap().value(u"kind"_s).toString();
            return kind == u"everywhere" || kind == u"level";
        }), "the scope links are not repeated after the last row");
        report.insert(u"scope_bar"_s, QJsonArray {barRect.x(), barRect.y(), barRect.width(), barRect.height()});
        if (window->screen()) {
            RECT box {};
            ::GetWindowRect(reinterpret_cast<HWND>(window->winId()), &box);
            const QPoint origin(box.left - monitor.rcMonitor.left, box.top - monitor.rcMonitor.top);
            const QPixmap picture = window->screen()->grabWindow(0).copy(QRect(origin, QSize(box.right - box.left, box.bottom - box.top)));
            check(picture.save(out.filePath(u"typing-scope.png"_s)), "scope bar screenshot saved");
        }
        QQuickItem* widen = named(window->contentItem(), u"scopeLink-everywhere"_s);
        if (widen) {
            const QPointF at = widen->mapToScene(QPointF(widen->width() / 2, widen->height() / 2));
            QMouseEvent press(QEvent::MouseButtonPress, at, window->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, {});
            QCoreApplication::sendEvent(window, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), Qt::LeftButton, {}, {});
            QCoreApplication::sendEvent(window, &release);
            pump(100);
        }
        waiting.restart();
        while ((launcher->searching() || launcher->typingFolderOnly()) && waiting.elapsed() < 5000)
            pump(20);
        settle(window, 2);
        check(widen && !launcher->typingFolderOnly() && !window->property("showScopeBar").toBool(),
            "its link, clicked, searches everywhere and the bar goes");
    } else {
        check(false, "folder only: every row of a long list found");
    }
    launcher->setTypingScope(Launcher::TypingScope::Everywhere); // as the setting (FolderTyping=first)
    check(search(u"follow_"_s, 3), "query restored after the scope bar");
    {
        // A kind of file named with the words (GitHub #366): the folder first,
        // then elsewhere, as without it; not "nothing found" for a folder
        // that holds none, and only the kind named.
        const QString childPrefix = folder + u"\\child\\ "_s;
        launcher->setTypingFolder(childPrefix);
        launcher->setTypingScope(Launcher::TypingScope::Everywhere);
        bool kindElsewhere = false;
        for (const QString& words : {u"documents: type-elsewhere"_s, u"type-elsewhere :documents"_s}) {
            launcher->setQuery(childPrefix + words);
            waiting.restart();
            while ((launcher->searching() || launcher->results()->count() != 1) && waiting.elapsed() < 5000)
                pump(20);
            const QString row = launcher->results()->count() == 1 ? launcher->results()->pathAt(0) : QString();
            kindElsewhere = !launcher->searching() && launcher->typingFolder() == u"child"_s
                && QDir::toNativeSeparators(row).compare(folder + u"\\type-elsewhere.docx"_s, Qt::CaseInsensitive) == 0;
            report.insert(u"kind_elsewhere"_s, QJsonObject {{u"words"_s, words}, {u"rows"_s, launcher->results()->count()},
                {u"first"_s, row}, {u"typing_folder"_s, launcher->typingFolder()}});
            if (!kindElsewhere)
                break;
        }
        check(kindElsewhere, "a kind of file typed in a folder still lists what is found elsewhere");
        launcher->setTypingFolder(prefix);
        launcher->setTypingScope(Launcher::TypingScope::Everywhere);
        check(search(u"follow_"_s, 3), "query restored after the kind of file");
    }
    settle(window, 3);
    check(search(u"follow_1"_s, 1) && launcher->currentRow() == 0 && follows(),
        "a unique result is selected in the source view");
    check(search(u"follow_2"_s, 1) && launcher->currentRow() == 0 && follows(),
        "replacing a unique result follows its new path without changing the row or count");
    report.insert(u"same_size_replacement"_s, QJsonObject {
        {u"row"_s, launcher->currentRow()},
        {u"expected"_s, launcher->results()->pathAt(launcher->currentRow())},
        {u"selected"_s, selected()}});
    const QString chosen = selected();
    check(!filemanager::selectInExplorer(tab, (folder + u"\\child"_s).toStdWString(), chosen.toStdWString(), [] { return true; }),
        "changed source folder rejected without navigation");
    check(!filemanager::selectInExplorer(tab, folder.toStdWString(), (folder + u"\\child\\nested-only.txt"_s).toStdWString(), [] { return true; }),
        "nested result never navigates the original folder");
    int guards = 0;
    check(!filemanager::selectInExplorer(tab, folder.toStdWString(), chosen.toStdWString(), [&] { return ++guards < 3; })
        && guards >= 3 && selected() == chosen, "superseded work rejected after COM lookup");
    launcher->setQuery(prefix + u"nested-only"_s);
    pump(500);
    check(selected() == chosen && QString::fromStdWString(filemanager::explorerFolder(tab)) == folder,
        "browsing a nested result preserves the source directory and selection");
    launcher->setQuery(prefix + u"follow_"_s);
    pump(300);
    const QString beforeHide = selected();
    key(Qt::Key_Down);
    app.hideLauncher();
    pump(300);
    check(selected() == beforeHide, "hiding cancels the pending selection");

    // The second tab of the window (GitHub #182): typed in there, the arrow
    // keys follow in that tab; a result in a subfolder is reached by Ctrl+Enter,
    // which says so in the footer, and takes that tab to it.
    // Explorer's own buttons, by UI Automation: the tab strip's "+" and the
    // "x" of its last tab (keys injected into it never opened a tab here).
    const auto press = [&](const wchar_t* id, bool last) {
        ComPtr<IUIAutomation> automation;
        ComPtr<IUIAutomationElement> window;
        ComPtr<IUIAutomationCondition> condition;
        ComPtr<IUIAutomationElementArray> found;
        ComPtr<IUIAutomationElement> button;
        ComPtr<IUnknown> pattern;
        ComPtr<IUIAutomationInvokePattern> invoke;
        VARIANT name {};
        name.vt = VT_BSTR;
        name.bstrVal = ::SysAllocString(id);
        const auto release = qScopeGuard([&] { ::VariantClear(&name); });
        int count = 0;
        return SUCCEEDED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))
            && SUCCEEDED(automation->ElementFromHandle(root, &window))
            && SUCCEEDED(automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, name, &condition))
            && SUCCEEDED(window->FindAll(TreeScope_Descendants, condition.Get(), &found))
            && SUCCEEDED(found->get_Length(&count)) && count > 0
            && SUCCEEDED(found->GetElement(last ? count - 1 : 0, &button))
            && SUCCEEDED(button->GetCurrentPattern(UIA_InvokePatternId, &pattern)) && SUCCEEDED(pattern.As(&invoke))
            && SUCCEEDED(invoke->Invoke());
    };
    const auto tabsOf = [&] {
        std::vector<ComPtr<IShellBrowser>> found;
        for (const auto& web : browsers()) {
            SHANDLE_PTR hwnd = 0; web->get_HWND(&hwnd);
            ComPtr<IServiceProvider> provider;
            ComPtr<IShellBrowser> inTab;
            if (reinterpret_cast<HWND>(hwnd) == root && SUCCEEDED(web.As(&provider))
                && SUCCEEDED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&inTab))))
                found.push_back(inTab);
        }
        return found;
    };
    app.hideLauncher();
    const auto firstTabs = tabsOf();
    ComPtr<IShellBrowser> second;
    if (press(L"AddButton", false)) {
        waiting.restart();
        while (!second && waiting.elapsed() < 5000) {
            pump(50);
            for (const auto& inTab : tabsOf()) {
                HWND candidate = nullptr;
                inTab->GetWindow(&candidate);
                if (candidate != tab && std::ranges::none_of(firstTabs, [&](const auto& known) {
                        HWND other = nullptr;
                        known->GetWindow(&other);
                        return other == candidate;
                    }))
                    second = inTab;
            }
        }
    }
    check(second != nullptr, "a second tab opened in the own Explorer window");
    if (!second)
        return false;
    HWND secondTab = nullptr;
    second->GetWindow(&secondTab);
    filemanager::goTo(secondTab, folder.toStdWString());
    waiting.restart();
    while (QString::fromStdWString(filemanager::explorerFolder(secondTab)) != folder && waiting.elapsed() < 5000)
        pump(50);
    pump(500);
    const auto closeSecond = qScopeGuard([&] {
        app.hideLauncher();
        if (::IsWindow(secondTab) && press(L"CloseButton", true)) {
            QElapsedTimer closing; closing.start();
            while (tabsOf().size() > 1 && closing.elapsed() < 3000) pump(50);
        }
    });
    shown = second.Get();
    ::SetForegroundWindow(root);
    app.showFolderSearch(root, secondTab, App::TypingIn::Explorer, folder);
    const bool again = search(u"follow_"_s, 3);
    check(again, "production search found the three children again for the second tab");
    if (!again)
        return false;
    settle(window, 3);
    if (field) QMetaObject::invokeMethod(field, "focusAtEnd");
    check(launcher->currentRow() == 0 && follows(), "initial result selected in the second tab");
    key(Qt::Key_Down);
    check(launcher->currentRow() == 1 && follows(), "Down follows in the second tab");
    key(Qt::Key_Down);
    check(launcher->currentRow() == 2 && follows(), "Down follows to the last child in the second tab");
    check(launcher->keyHints().value(u"second"_s).toString() == QCoreApplication::translate("ws::Launcher", "打开位置"),
        "a child of the folder keeps the plain 打开位置 hint");
    const QString nestedFile = QDir::toNativeSeparators(folder + u"\\child\\nested-only.txt"_s);
    launcher->setQuery(prefix + u"nested-only"_s);
    waiting.restart();
    while ((launcher->searching() || launcher->results()->count() < 1) && waiting.elapsed() < 5000)
        pump(20);
    pump(300);
    check(launcher->keyHints().value(u"second"_s).toString() == QCoreApplication::translate("ws::Launcher", "在原窗口定位"),
        "a result in a subfolder says Ctrl+Enter locates it in the original window");
    check(selected() != nestedFile && QString::fromStdWString(filemanager::explorerFolder(secondTab)) == folder,
        "moving onto the subfolder result left the second tab where it was");
    const int warned = g_warnings;
    QKeyEvent reveal(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier);
    QCoreApplication::sendEvent(window, &reveal);
    waiting.restart();
    while ((QString::fromStdWString(filemanager::explorerFolder(secondTab)) != folder + u"\\child"_s
               || QDir::fromNativeSeparators(selected()).compare(QDir::fromNativeSeparators(nestedFile), Qt::CaseInsensitive) != 0)
        && waiting.elapsed() < 5000)
        pump(50);
    check(QString::fromStdWString(filemanager::explorerFolder(secondTab)) == folder + u"\\child"_s
            && QDir::fromNativeSeparators(selected()).compare(QDir::fromNativeSeparators(nestedFile), Qt::CaseInsensitive) == 0,
        "Ctrl+Enter took the second tab to the subfolder and selected the file");

    // A window too large to keep clear of, centred (GitHub #203: the box went
    // to the top left of the screen): in the bottom right corner of the file
    // list of the tab typed in, within the window.
    app.hideLauncher();
    pump(300);
    ::SetWindowPos(root, nullptr, area.left + (area.right - area.left) / 8, area.top + (area.bottom - area.top) / 8,
        (area.right - area.left) * 3 / 4, (area.bottom - area.top) * 3 / 4, SWP_NOZORDER | SWP_NOACTIVATE);
    pump(500);
    ::SetForegroundWindow(root);
    app.showFolderSearch(root, secondTab, App::TypingIn::Explorer, QString::fromStdWString(filemanager::explorerFolder(secondTab)));
    pump(600);
    settle(window, 3);
    {
        const RECT list = folderview::explorerTypingCorner(root, secondTab);
        RECT box {}, client {};
        ::GetWindowRect(reinterpret_cast<HWND>(window->winId()), &box);
        ::GetClientRect(root, &client);
        ::MapWindowPoints(root, nullptr, reinterpret_cast<POINT*>(&client), 2);
        const int gap = qRound(windowavoidance::kExplorerListGap * (window->screen() ? window->screen()->devicePixelRatio() : 1.0));
        check(box.left >= client.left && box.top >= client.top && box.right <= client.right && box.bottom <= client.bottom,
            "too large to keep clear of: the box is inside the Explorer window");
        check(std::abs(list.right - box.right - gap) <= 2
                && std::abs(std::min(client.bottom, area.bottom) - box.bottom) <= 2,
            "too large to keep clear of: at the right of its file list, down to the window's bottom (over the status bar)");
        check(box.left != area.left || box.top != area.top, "too large to keep clear of: not at the top left of the screen");
        // Low on the screen, its rows have the room it opened with above it,
        // not what is left under its top (GitHub #234).
        const bool many = search(u"many_"_s, kFolderTypingMany);
        settle(window, 3);
        RECT grown {};
        ::GetWindowRect(reinterpret_cast<HWND>(window->winId()), &grown);
        lowTall = window->height();
        check(many && std::abs(grown.bottom - box.bottom) <= 1, "low on the screen: grown with its rows, its bottom where it was");
        report.insert(u"low_grown_height"_s, lowTall);
        launcher->setQuery(prefix);
        pump(200);
        report.insert(u"centred_explorer_list"_s, QJsonArray {int(list.left), int(list.top), int(list.right - list.left),
            int(list.bottom - list.top)});
        report.insert(u"centred_explorer_box"_s, QJsonArray {int(box.left), int(box.top), int(box.right - box.left),
            int(box.bottom - box.top)});
    }
    app.hideLauncher();
    pump(300);

    // GitHub #234: more rows and wider columns than the list shows, so that
    // Explorer shows both its scroll bars. The box sits against them, in the
    // corner they leave, as it sits in the corner of a list without any, and
    // over neither. Opened by typing it lists nothing until the words come,
    // and as its rows come its bottom stays where it is, the window made
    // taller and moved up in one step.
    {
        // The probe's own folder: rows enough for the bar on the right.
        filemanager::goTo(secondTab, folder.toStdWString());
        waiting.restart();
        while (QString::fromStdWString(filemanager::explorerFolder(secondTab)) != folder && waiting.elapsed() < 5000)
            pump(50);
        pump(500);
        const qreal scale = window->screen() ? window->screen()->devicePixelRatio() : 1.0;
        if (ComPtr<IShellView> shellView; SUCCEEDED(second->QueryActiveShellView(&shellView))) {
            if (ComPtr<IFolderView> folderView; SUCCEEDED(shellView.As(&folderView)))
                folderView->SetCurrentViewMode(FVM_DETAILS);
            // A name column wider than the list: the bar at the bottom.
            UINT count = 0;
            if (ComPtr<IColumnManager> columns; SUCCEEDED(shellView.As(&columns))
                && SUCCEEDED(columns->GetColumnCount(CM_ENUM_VISIBLE, &count)) && count > 0) {
                std::vector<PROPERTYKEY> keys(count);
                CM_COLUMNINFO name {sizeof(CM_COLUMNINFO), CM_MASK_WIDTH};
                name.uWidth = UINT(qRound(900 * scale));
                if (SUCCEEDED(columns->GetColumns(CM_ENUM_VISIBLE, keys.data(), count)))
                    columns->SetColumnInfo(keys.front(), &name);
            }
        }
        struct Bars {
            HWND view = nullptr;
            RECT vertical {}, horizontal {};
        };
        const auto barsOf = [secondTab] {
            Bars bars;
            ::EnumChildWindows(secondTab, [](HWND child, LPARAM param) -> BOOL {
                auto& bars = *reinterpret_cast<Bars*>(param);
                wchar_t name[32] {};
                ::GetClassNameW(child, name, 32);
                RECT at {};
                if (std::wstring_view(name) == L"SHELLDLL_DefView" && ::IsWindowVisible(child) && !bars.view)
                    bars.view = child;
                else if (std::wstring_view(name) == L"ScrollBar" && bars.view && ::IsChild(bars.view, child)
                    && ::IsWindowVisible(child) && ::GetWindowRect(child, &at))
                    (at.bottom - at.top > at.right - at.left ? bars.vertical : bars.horizontal) = at;
                return TRUE;
            }, reinterpret_cast<LPARAM>(&bars));
            return bars;
        };
        // The list 560 px wide whatever the navigation pane takes: room for
        // the box beside the bar on the right, too little for the columns.
        const int height = qRound(600 * scale);
        int width = qRound(760 * scale);
        ::SetWindowPos(root, nullptr, area.left + 40, area.top + 40, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        pump(500);
        if (RECT view {}; ::GetWindowRect(barsOf().view, &view))
            width += qRound(560 * scale) - (view.right - view.left);
        ::SetWindowPos(root, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
        pump(800);
        const Bars bars = barsOf();
        const bool both = !::IsRectEmpty(&bars.vertical) && !::IsRectEmpty(&bars.horizontal);
        check(both, "a folder of more rows and columns than fit: Explorer shows both scroll bars");
        ::SetForegroundWindow(root);
        app.showFolderSearch(root, secondTab, App::TypingIn::Explorer, folder);
        pump(600);
        settle(window, 3);
        RECT box {}, overlap {};
        ::GetWindowRect(reinterpret_cast<HWND>(window->winId()), &box);
        const int gap = qRound(windowavoidance::kExplorerListGap * scale);
        check(launcher->results()->count() == 0 && !launcher->searching(),
            "opened by typing: nothing is listed before the words typed");
        if (both) {
            RECT client {};
            ::GetClientRect(root, &client);
            ::MapWindowPoints(root, nullptr, reinterpret_cast<POINT*>(&client), 2);
            check(!::IntersectRect(&overlap, &box, &bars.vertical), "with scroll bars: the box is not over the one on the right");
            check(std::abs(bars.vertical.left - box.right - gap) <= 2
                    && std::abs(std::min(client.bottom, area.bottom) - box.bottom) <= 2,
                "with scroll bars: against the one on the right, down to the window's bottom");
        }
        {
            const int shortBox = box.bottom - box.top;
            BottomWatch watch(reinterpret_cast<HWND>(window->winId()));
            const bool listed = search(u"many_"_s, kFolderTypingMany);
            settle(window, 3);
            check(listed, "the words typed: their rows are listed");
            check(watch.changes > 0 && watch.tallest > shortBox, "the box grew taller with its rows");
            check(watch.lowest == box.bottom && watch.highest == box.bottom,
                "growing, its bottom never left the corner: taller and moved up in one step");
            report.insert(u"grow_changes"_s, watch.changes);
            report.insert(u"grow_bottoms"_s, QJsonArray {int(watch.highest), int(watch.lowest), int(box.bottom)});
            ::GetWindowRect(reinterpret_cast<HWND>(window->winId()), &box);
        }
        // Opened by a key whose rows come while it is still kept from view:
        // first seen with them, as tall as they make it (GitHub #234).
        {
            app.hideLauncher();
            pump(300);
            ::SetForegroundWindow(root);
            app.showFolderSearch(root, secondTab, App::TypingIn::Explorer, folder);
            const HWND own = reinterpret_cast<HWND>(window->winId());
            waiting.restart();
            while (window->height() == 0 && waiting.elapsed() < 200)
                pump(5);
            pump(80); // its first frame is drawn; the key's rows are awaited (App::kTypedRowsCap is 700 ms)
            const bool held = window->isVisible() && isCloaked(own);
            const int emptyHeight = window->height();
            launcher->setQuery(prefix + u"many_"_s);
            int seenAt = 0;
            waiting.restart();
            while (waiting.elapsed() < 2000) {
                pump(2);
                if (!isCloaked(own)) {
                    seenAt = window->height();
                    break;
                }
            }
            const bool rows = launcher->results()->count() == kFolderTypingMany;
            BottomWatch watch(own);
            settle(window, 3);
            pump(300);
            check(held, "opened by a key typed: kept from view until that key's rows are in it");
            check(seenAt > emptyHeight && rows, "first seen with the rows of the key typed, taller than the empty box");
            check(window->height() == seenAt, "seen once, at its final height: it does not grow after");
            check(lowTall == seenAt, "low on the screen: as tall with as many rows as anywhere");
            report.insert(u"typed_reveal"_s, QJsonArray {held, emptyHeight, seenAt, window->height(), int(watch.changes)});
            ::GetWindowRect(own, &box);
        }
        const auto rectOf = [](const RECT& r) { return QJsonArray {int(r.left), int(r.top), int(r.right - r.left), int(r.bottom - r.top)}; };
        report.insert(u"scrollbars_vertical"_s, rectOf(bars.vertical));
        report.insert(u"scrollbars_horizontal"_s, rectOf(bars.horizontal));
        report.insert(u"scrollbars_box"_s, rectOf(box));
        if (window->screen()) {
            RECT frame {};
            ::GetWindowRect(root, &frame);
            const QRect region(frame.left - monitor.rcMonitor.left, frame.top - monitor.rcMonitor.top,
                frame.right - frame.left, frame.bottom - frame.top);
            check(window->screen()->grabWindow(0).copy(region).save(out.filePath(u"typing-scrollbars.png"_s)),
                "with scroll bars: screenshot saved");
        }
    }
    app.hideLauncher();
    pump(300);

    // Typed in Directory Opus (GitHub #82), by a window of its class standing
    // in for a lister (production needs the real one only to read its folder):
    // the box in the bottom right corner of the file list, the folder before
    // the words or, as set (--opus-everywhere), nothing.
    WNDCLASSW listerClass {};
    listerClass.lpfnWndProc = ::DefWindowProcW;
    listerClass.hInstance = ::GetModuleHandleW(nullptr);
    listerClass.lpszClassName = L"dopus.lister";
    ::RegisterClassW(&listerClass);
    const HWND lister = ::CreateWindowExW(0, L"dopus.lister", L"Opus stand-in", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        area.left + 20, area.top + 20, std::min<int>(1600, area.right - area.left - 40), area.bottom - area.top - 40,
        nullptr, nullptr, listerClass.hInstance, nullptr);
    RECT client {};
    if (lister)
        ::GetClientRect(lister, &client);
    const HWND list = lister ? ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 300, 120,
                                    client.right - 320, client.bottom - 140, lister, nullptr, listerClass.hInstance, nullptr)
                             : nullptr;
    const auto closeLister = qScopeGuard([&] {
        app.hideLauncher();
        if (lister)
            ::DestroyWindow(lister);
        ::UnregisterClassW(L"dopus.lister", listerClass.hInstance);
    });
    check(list != nullptr, "a stand-in Opus lister opened");
    if (!list)
        return false;
    pump(300);
    app.showFolderSearch(lister, list, App::TypingIn::Opus, folder);
    pump(600);
    settle(window, 3);
    RECT listRect {}, boxRect {};
    ::GetWindowRect(list, &listRect);
    ::GetWindowRect(reinterpret_cast<HWND>(window->winId()), &boxRect);
    const qreal scale = window->screen() ? window->screen()->devicePixelRatio() : 1.0;
    const int gap = qRound(12 * scale);
    const bool everywhere = QCoreApplication::arguments().contains(u"--opus-everywhere"_s);
    check(boxRect.left >= listRect.left && boxRect.top >= listRect.top && boxRect.right <= listRect.right
            && boxRect.bottom <= listRect.bottom,
        "the Opus box is inside the file list");
    check(std::abs(listRect.right - boxRect.right - gap) <= 2, "the Opus box sits 12 px from the right edge of the list");
    report.insert(u"opus_list"_s, QJsonArray {int(listRect.left), int(listRect.top), int(listRect.right - listRect.left),
        int(listRect.bottom - listRect.top)});
    report.insert(u"opus_box"_s, QJsonArray {int(boxRect.left), int(boxRect.top), int(boxRect.right - boxRect.left),
        int(boxRect.bottom - boxRect.top)});
    check(everywhere ? launcher->typingFolder().isEmpty()
                     : launcher->query() == prefix && !launcher->typingFolder().isEmpty(),
        everywhere ? "the whole PC: no folder before the words" : "the tab's folder before the words");

    // The compact width is temporary; measure the user's regular launcher
    // before testing another folder opening (GitHub #234).
    app.hideLauncher();
    pump(300);
    app.showLauncher();
    pump(300);
    settle(window, 3);
    const int userWidth = window->width();
    app.hideLauncher();
    pump(300);
    ::MoveWindow(list, 700, 120, qRound(540 * scale), client.bottom - 140, TRUE);
    app.showFolderSearch(lister, list, App::TypingIn::Opus, folder);
    pump(600);
    settle(window, 3);
    ::GetWindowRect(list, &listRect);
    ::GetWindowRect(reinterpret_cast<HWND>(window->winId()), &boxRect);
    RECT clientOnScreen = client;
    ::MapWindowPoints(lister, nullptr, reinterpret_cast<POINT*>(&clientOnScreen), 2);
    check(std::abs(int(boxRect.right - boxRect.left) - qRound(460 * scale)) <= 2, "folder typing is 460 logical px wide");
    check(std::abs(listRect.right - boxRect.right - gap) <= 2 && boxRect.bottom <= listRect.bottom - gap + 2
            && boxRect.left >= listRect.left && boxRect.left >= clientOnScreen.left,
        "over a narrow list: compact search fits inside, 12 px from its right and above its bottom");
    report.insert(u"opus_narrow_list"_s, QJsonArray {int(listRect.left), int(listRect.top), int(listRect.right - listRect.left),
        int(listRect.bottom - listRect.top)});
    report.insert(u"opus_narrow_box"_s, QJsonArray {int(boxRect.left), int(boxRect.top), int(boxRect.right - boxRect.left),
        int(boxRect.bottom - boxRect.top)});
    app.hideLauncher();
    pump(300);
    app.showLauncher();
    pump(600);
    settle(window, 3);
    check(std::abs(window->width() - userWidth) <= 1, "opened elsewhere afterwards: the user's width again");
    app.hideLauncher();
    // The tab and the stand-in come to the front as the launcher goes: the
    // warnings about its losing the front that this makes are no fault.
    pump(500);
    g_warnings = warned;
    return ok;
}

// Typing in the bar under a dialog, a key every 120 ms, over the few thousand
// files --file-dialog-typing makes: for each key, how long setQuery kept this
// thread, when the rows came, when the frame with them was shown, the longest
// this thread was kept by one thing meanwhile, and the height of the window
// (it grows and shrinks with the rows).
// Which event kept this thread longest: told before each one is delivered,
// so one's time runs till the next is, or till done() after the loop's turn.
class SlowestEvent : public QObject {
public:
    SlowestEvent() { m_clock.start(); qApp->installEventFilter(this); }
    void done() { close(); m_from = -1; }
    QString take()
    {
        const QString told = m_slowestNs > 0 ? u"%1 ms: %2"_s.arg(m_slowestNs / 1e6, 0, 'f', 1).arg(m_slowest) : QString();
        m_slowestNs = 0;
        return told;
    }

protected:
    bool eventFilter(QObject* receiver, QEvent* event) override
    {
        close();
        m_from = m_clock.nsecsElapsed();
        m_what = u"%1 %2 event %3"_s.arg(QLatin1StringView(receiver->metaObject()->className()), receiver->objectName())
                     .arg(static_cast<int>(event->type()));
        return false;
    }

private:
    void close()
    {
        if (m_from >= 0 && m_clock.nsecsElapsed() - m_from > m_slowestNs) {
            m_slowestNs = m_clock.nsecsElapsed() - m_from;
            m_slowest = m_what;
        }
    }
    QElapsedTimer m_clock;
    qint64 m_from = -1;
    qint64 m_slowestNs = 0;
    QString m_what;
    QString m_slowest;
};

// `keys`: each letter as a key of the keyboard (SendInput, a Unicode key: no
// input method composes it), sent only while the bar's window is in front;
// else the text is set as the field sets it.
QJsonArray typeInDialogBar(DialogBar* bar, QQuickWindow* window, bool keys_)
{
    QJsonArray keys;
    SlowestEvent slowest;
    const QString words[] = {u"report 12"_s, u"notes"_s, u"report 1234 x"_s};
    for (const QString& word : words) {
        bar->setQuery({});
        pump(400);
        for (qsizetype typed = 1; typed <= word.size(); ++typed) {
            QElapsedTimer since;
            since.start();
            qint64 rowsAt = -1;
            std::atomic<qint64> frameAt {-1};
            std::atomic<bool> rowsIn {false};
            const auto onRows = QObject::connect(bar, &DialogBar::resultsReplaced, bar, [&] {
                if (rowsAt < 0) {
                    rowsAt = since.nsecsElapsed();
                    rowsIn = true;
                    window->update();
                }
            });
            const auto onFrame = QObject::connect(window, &QQuickWindow::frameSwapped, window, [&] {
                if (rowsIn && frameAt < 0)
                    frameAt = since.nsecsElapsed();
            }, Qt::DirectConnection);
            qint64 queryAt = -1;
            const auto onQuery = QObject::connect(bar, &DialogBar::queryChanged, bar, [&] {
                if (queryAt < 0)
                    queryAt = since.nsecsElapsed();
            });
            if (keys_) {
                if (GetForegroundWindow() != reinterpret_cast<HWND>(window->winId())) {
                    QObject::disconnect(onRows);
                    QObject::disconnect(onFrame);
                    QObject::disconnect(onQuery);
                    keys.append(QJsonObject {{u"query"_s, word.left(typed)}, {u"skipped"_s, u"the bar is not in front"_s}});
                    return keys;
                }
                INPUT strokes[2] {};
                strokes[0].type = strokes[1].type = INPUT_KEYBOARD;
                strokes[0].ki.wScan = strokes[1].ki.wScan = word[typed - 1].unicode();
                strokes[0].ki.dwFlags = KEYEVENTF_UNICODE;
                strokes[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
                since.restart();
                SendInput(2, strokes, sizeof(INPUT));
            } else {
                bar->setQuery(word.left(typed));
            }
            const qint64 call = since.nsecsElapsed();
            qint64 last = since.nsecsElapsed();
            qint64 kept = 0;
            while (since.elapsed() < 120 || (frameAt < 0 && since.elapsed() < 1500)) {
                QCoreApplication::processEvents(QEventLoop::AllEvents);
                slowest.done();
                const qint64 now = since.nsecsElapsed();
                kept = std::max(kept, now - last);
                QThread::usleep(300);
                last = since.nsecsElapsed();
            }
            QObject::disconnect(onRows);
            QObject::disconnect(onFrame);
            QObject::disconnect(onQuery);
            const auto ms = [](qint64 ns) { return ns < 0 ? -1.0 : qRound(ns / 10000.0) / 100.0; };
            keys.append(QJsonObject {{u"query"_s, bar->query()}, {u"set_query_ms"_s, ms(call)}, {u"query_ms"_s, ms(queryAt)}, {u"rows_ms"_s, ms(rowsAt)},
                {u"frame_ms"_s, ms(frameAt.load())}, {u"longest_kept_ms"_s, ms(kept)}, {u"slowest_event"_s, slowest.take()}, {u"rows"_s, bar->rows()},
                {u"found"_s, bar->results()->rowCount()}, {u"height"_s, window->height()}});
        }
    }
    bar->setQuery({});
    pump(300);
    return keys;
}

bool probeFileDialog(const QStringList& args, const QDir& out, const QString& selected, QJsonObject& report)
{
    const auto samePath = [&selected](const QString& path) {
        return QDir::cleanPath(QDir::fromNativeSeparators(path)).compare(QDir::cleanPath(selected), Qt::CaseInsensitive) == 0;
    };
    const HWND target = reinterpret_cast<HWND>(valueAfter(args, u"--file-dialog-window").toULongLong());
    const DWORD expected = valueAfter(args, u"--file-dialog-pid").toULong();
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    const bool valid = expected && pid == expected && filedialog::kind(target) != filedialog::Kind::None;
    report.insert(u"dialog_target_valid"_s, valid);
    if (!valid)
        return false;
    INPUT nothing {};
    nothing.type = INPUT_MOUSE; // production's foreground permission handoff; no move/buttons/keys
    SendInput(1, &nothing, sizeof nothing);
    SetForegroundWindow(target);
    QQuickWindow* window = nullptr;
    DialogBar* bar = nullptr;
    QElapsedTimer waiting;
    waiting.start();
    while (!bar && waiting.elapsed() < 5000) {
        pump(30);
        for (QWindow* candidate : QGuiApplication::allWindows()) {
            auto* quick = qobject_cast<QQuickWindow*>(candidate);
            auto* controller = quick ? quick->property("bar").value<DialogBar*>() : nullptr;
            if (controller && controller->isShown() && quick->isVisible()) {
                window = quick;
                bar = controller;
                break;
            }
        }
    }
    report.insert(u"dialog_bar_visible"_s, bar != nullptr);
    if (!bar)
        return false;
    if (!window->isActive())
        bar->toggleFocus(); // the same foreground handoff as double Ctrl
    waiting.restart();
    while (!window->isActive() && waiting.elapsed() < 2000)
        pump(20);
    report.insert(u"dialog_bar_focused"_s, window->isActive());
    if (!window->isActive())
        return false;
    if (args.contains(u"--file-dialog-typing"_s))
        report.insert(u"dialog_typing"_s, typeInDialogBar(bar, window, args.contains(u"--file-dialog-keys"_s)));
    bar->setQuery(QFileInfo(selected).completeBaseName());
    int row = -1;
    waiting.restart();
    while (row < 0 && waiting.elapsed() < 10000) {
        pump(50);
        for (int at = 0; at < bar->results()->rowCount(); ++at) {
            if (samePath(bar->results()->pathAt(at))) {
                row = at;
                break;
            }
        }
    }
    report.insert(u"dialog_search_found"_s, row >= 0);
    report.insert(u"dialog_query"_s, bar->query());
    report.insert(u"dialog_result_count"_s, bar->results()->rowCount());
    waiting.restart();
    while (bar->rows() <= 0 && waiting.elapsed() < 1000)
        pump(20);
    report.insert(u"dialog_result_rows_visible"_s, bar->rows());
    report.insert(u"dialog_can_open"_s, bar->canOpen());
    if (row < 0 || bar->rows() <= 0 || !saveGrab(window, out.filePath(u"dialog-bar.png"_s)))
        return false;
    RECT dialog {}, panel {};
    if (GetWindowRect(target, &dialog) && GetWindowRect(reinterpret_cast<HWND>(window->winId()), &panel)) {
        RECT combined {};
        UnionRect(&combined, &dialog, &panel);
        QGuiApplication::primaryScreen()->grabWindow(0, combined.left, combined.top,
            combined.right - combined.left, combined.bottom - combined.top).save(out.filePath(u"dialog-with-search.png"_s));
    }
    // The production controller hands focus back and calls DialogJump::go.
    // The fixture's independent observer confirms Blender actually changed;
    // with none (a window that takes folders), the window itself tells where
    // it is now.
    bar->choose(row, false);
    waiting.restart();
    bool selectedInDialog = false;
    const bool observed = args.contains(u"--file-dialog-state"_s);
    // Bandizip answers nothing while its tree opens down to the folder: for
    // seconds, with the folder made here under a Temp of thousands.
    while (!selectedInDialog && waiting.elapsed() < (observed ? 5000 : 20000)) {
        pump(50);
        if (!observed) {
            selectedInDialog = samePath(QString::fromStdWString(filedialog::currentFolder(target)));
            continue;
        }
        QFile observer(valueAfter(args, u"--file-dialog-state"));
        if (!observer.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject state = QJsonDocument::fromJson(observer.readAll()).object();
        if (state.value(u"pid"_s).toInteger() != expected)
            continue;
        for (const QJsonValue& item : state.value(u"browsers"_s).toArray()) {
            const QJsonObject browser = item.toObject();
            const QString path = QDir(browser.value(u"directory"_s).toString()).filePath(browser.value(u"filename"_s).toString());
            selectedInDialog = samePath(path);
            if (selectedInDialog)
                break;
        }
    }
    report.insert(u"dialog_selection_confirmed"_s, selectedInDialog);
    report.insert(u"dialog_selection_ms"_s, waiting.elapsed());
    if (!observed)
        report.insert(u"dialog_folder_shown"_s, QString::fromStdWString(filedialog::currentFolder(target)));
    report.insert(u"dialog_still_open"_s, IsWindow(target) != FALSE);
    return selectedInDialog && IsWindow(target);
}

bool probeOnboarding(const QStringList& args, const QDir& out, QJsonObject& report)
{
    const QString action = valueAfter(args, u"--onboarding-probe");
    bool passed = true;
    QJsonObject checks;
    const auto check = [&](const QString& name, bool value) {
        checks.insert(name, value);
        passed = passed && value;
    };
    const auto guideWindow = []() -> QQuickWindow* {
        for (QWindow* candidate : QGuiApplication::allWindows()) {
            if (candidate->property("setup").value<Onboarding*>())
                return qobject_cast<QQuickWindow*>(candidate);
        }
        return nullptr;
    };
    const auto launcherWindow = []() -> QQuickWindow* {
        for (QWindow* candidate : QGuiApplication::allWindows()) {
            if (candidate->property("launcher").value<Launcher*>())
                return qobject_cast<QQuickWindow*>(candidate);
        }
        return nullptr;
    };
    pump(100);
    QPointer<QQuickWindow> window = guideWindow();
    if (action == u"legacy") {
        check(u"existing_profile_has_no_guide"_s, !window);
        check(u"existing_profile_starts_runtime"_s, launcherWindow());
        report.insert(u"checks"_s, checks);
        return passed;
    }
    check(u"guide_created"_s, window);
    if (!window) {
        report.insert(u"checks"_s, checks);
        return false;
    }
    QPointer<Onboarding> setup = window->property("setup").value<Onboarding*>();
    QObject::connect(setup.data(), &Onboarding::accepted, qApp, [] { say(u"onboarding: accepted"_s); });
    QObject::connect(window.data(), &QObject::destroyed, qApp, [] { say(u"onboarding: window destroyed"_s); });
    const QSize guideSize = window->size();
    const auto visible = [&](const QString& name) {
        const auto* item = visualNamed(window->contentItem(), name);
        return item && item->isVisible();
    };
    const auto navigate = [&](const QString& name) {
        auto* button = visualNamed(window->contentItem(), name);
        const bool clicked = button && button->isVisible() && QMetaObject::invokeMethod(button, "clicked");
        settle(window);
        if (!window || !setup || setup->committed()) {
            report.insert(u"checks"_s, checks);
            throw std::runtime_error("The guide closed or committed during navigation");
        }
        return clicked;
    };
    SystemTheme::setBackdropOn(false); // grabWindow does not capture the native Mica behind it
    showOffScreen(window);
    settle(window);
    check(u"no_launcher_before_confirmation"_s, !launcherWindow());
    check(u"no_clipboard_database_before_confirmation"_s,
        QDir(Settings::dataDir()).entryList({u"*clip*.db*"_s, u"*clip*.sqlite*"_s}, QDir::Files).isEmpty());
    for (const QString& name : {u"onboardingSearch"_s, u"onboardingActivation"_s, u"onboardingClipboard"_s, u"onboardingCapture"_s})
        check(name, visualNamed(window->contentItem(), name));
    check(u"initial_screenshot"_s, saveGrab(window, out.filePath(u"onboarding.png"_s)));
    {
        // A window of one size (WindowFrame::setFixedSize), asked as Windows asks: its edges do not
        // size it and nothing maximises it; its top drags it, but for the caption buttons.
        const HWND hwnd = reinterpret_cast<HWND>(window->winId());
        RECT rect {};
        ::GetWindowRect(hwnd, &rect);
        const auto hit = [hwnd](long x, long y) { return ::SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(x, y)); };
        const long midX = (rect.left + rect.right) / 2;
        const long midY = (rect.top + rect.bottom) / 2;
        const long inCaption = rect.top + std::lround(20 * window->effectiveDevicePixelRatio());
        const long inCloseButton = rect.right - std::lround(20 * window->effectiveDevicePixelRatio());
        check(u"edges_do_not_size"_s, hit(rect.left + 1, midY) == HTCLIENT && hit(rect.right - 2, midY) == HTCLIENT
            && hit(midX, rect.bottom - 2) == HTCLIENT && hit(midX, rect.top + 1) == HTCAPTION
            && hit(rect.left + 1, rect.top + 1) == HTCAPTION);
        check(u"nothing_maximises"_s, !(::GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_MAXIMIZEBOX));
        check(u"top_drags_the_window"_s, hit(midX, inCaption) == HTCAPTION && hit(rect.left + 40, inCaption) == HTCAPTION);
        check(u"close_button_is_its_own"_s, hit(inCloseButton, inCaption) == HTCLIENT);
    }
    check(u"page_one_groups"_s, setup->page() == 0 && visible(u"onboardingSearch"_s) && visible(u"onboardingActivation"_s)
        && !visible(u"onboardingClipboard"_s) && !visible(u"onboardingCapture"_s));
    check(u"next_button"_s, navigate(u"onboardingNext"_s));
    check(u"page_two_groups"_s, setup->page() == 1 && !visible(u"onboardingSearch"_s) && !visible(u"onboardingActivation"_s)
        && visible(u"onboardingClipboard"_s) && visible(u"onboardingCapture"_s));
    if (auto* summary = visualNamed(window->contentItem(), u"onboardingSummary"_s)) {
        report.insert(u"summary_geometry"_s, QJsonObject {{u"width"_s, summary->width()}, {u"height"_s, summary->height()},
            {u"implicit_height"_s, summary->implicitHeight()}, {u"visible"_s, summary->isVisible()},
            {u"text"_s, summary->property("text").toString()}});
        const QImage frame = window->grabWindow();
        const qreal dpr = window->effectiveDevicePixelRatio();
        const QRectF bounds = summary->mapRectToItem(window->contentItem(), QRectF(QPointF(), summary->size()));
        const QRect pixels(qRound(bounds.x() * dpr), qRound(bounds.y() * dpr), qRound(bounds.width() * dpr), qRound(bounds.height() * dpr));
        const QColor ink = summary->property("color").value<QColor>();
        int count = 0;
        const QRect clipped = pixels.intersected(frame.rect());
        for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
            for (int x = clipped.left(); x <= clipped.right(); ++x) {
                const QColor pixel = frame.pixelColor(x, y);
                if (std::max({std::abs(pixel.red() - ink.red()), std::abs(pixel.green() - ink.green()), std::abs(pixel.blue() - ink.blue())}) < 48)
                    ++count;
            }
        }
        check(u"page_two_summary_painted"_s, summary->isVisible() && summary->height() > 0 && count > 12);
    } else {
        check(u"page_two_summary_painted"_s, false);
    }
    for (const QString& name : {u"onboardingClipboardToggle"_s, u"onboardingCaptureToggle"_s}) {
        auto* toggle = visualNamed(window->contentItem(), name);
        check(name + u"_visible"_s, toggle && toggle->isVisible() && toggle->width() >= toggle->implicitWidth()
            && toggle->width() > 0 && toggle->height() > 0);
    }
    check(u"next_does_not_start_runtime"_s, !setup->committed() && !launcherWindow());
    check(u"second_page_screenshot"_s, saveGrab(window, out.filePath(u"onboarding-page2.png"_s)));
    writeTree(window, out.filePath(u"onboarding-page2.tree.txt"_s));
    check(u"previous_button"_s, navigate(u"onboardingPrevious"_s));
    check(u"back_to_page_one"_s, setup->page() == 0 && visible(u"onboardingSearch"_s));

    // Real empty-profile startup is inspected without starting its default
    // disk scan or enabling any global feature on the test machine.
    if (action == u"fresh") {
        for (int pass = 0; pass < 8; ++pass) {
            check(u"rapid_next_%1"_s.arg(pass), navigate(u"onboardingNext"_s) && setup->page() == 1);
            check(u"rapid_back_%1"_s.arg(pass), navigate(u"onboardingPrevious"_s) && setup->page() == 0);
        }
        Settings loaded;
        loaded.load();
        check(u"fresh_profile_still_pending"_s, !loaded.onboardingComplete);
        report.insert(u"checks"_s, checks);
        return passed;
    }

    auto* toggle = visualNamed(window->contentItem(), u"onboardingContentToggle"_s);
    check(u"draft_toggle"_s, toggle && QMetaObject::invokeMethod(toggle, "toggled", Q_ARG(bool, true)));
    Settings draft;
    draft.load();
    check(u"draft_not_saved"_s, !draft.onboardingComplete && !draft.contentIndex && !draft.clipboard);
    {
        // Exercise opt-in persistence through the production controller without
        // recording the user's clipboard. Restore this probe's draft afterwards.
        Onboarding consent(draft);
        consent.setProperty("clipboard", true);
        consent.setProperty("clipboardWinV", true);
        consent.apply();
        Settings optedIn;
        optedIn.load();
        check(u"clipboard_explicit_opt_in_saved"_s, optedIn.clipboard && optedIn.clipboardWinV && optedIn.onboardingComplete);
        check(u"restore_private_draft"_s, draft.saveChecked());
    }
    for (int mode = 0; mode < 3; ++mode) {
        setup->setProperty("activation", mode);
        setup->beginTest();
        const int key = mode == 0 ? Qt::Key_Control : mode == 1 ? Qt::Key_Alt : Qt::Key_Shift;
        for (int tap = 0; tap < 2; ++tap) {
            QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &press);
            QCoreApplication::sendEvent(window, &release);
        }
        check(u"double_tap_test_%1"_s.arg(mode), !setup->testing() && !setup->testMessage().isEmpty());
    }

    std::vector<int> freeKeys;
    for (int number = 24; number >= 13 && freeKeys.size() < 2; --number) {
        if (::RegisterHotKey(nullptr, 0x7351, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F1 + number - 1)) {
            ::UnregisterHotKey(nullptr, 0x7351);
            freeKeys.push_back(number);
        }
    }
    check(u"isolated_shortcuts_available"_s, freeKeys.size() == 2);
    if (freeKeys.size() != 2) {
        report.insert(u"checks"_s, checks);
        return false;
    }
    const QString launchHotkey = u"Ctrl+Alt+Shift+F%1"_s.arg(freeKeys[0]);
    const QString captureHotkey = u"Ctrl+Alt+Shift+F%1"_s.arg(freeKeys[1]);
    setup->setProperty("activation", 3);
    check(u"invalid_first_page_does_not_advance"_s, navigate(u"onboardingNext"_s)
        && setup->page() == 0 && !setup->message().isEmpty() && !setup->committed());
    setup->setProperty("launchHotkey", launchHotkey);
    setup->beginTest();
    QKeyEvent testKey(QEvent::KeyPress, Qt::Key_F1 + freeKeys[0] - 1, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
    QCoreApplication::sendEvent(window, &testKey);
    check(u"local_shortcut_test"_s, !setup->testing() && !setup->testMessage().isEmpty());

    check(u"next_with_choices"_s, navigate(u"onboardingNext"_s) && setup->page() == 1);
    {
        Settings uncommitted;
        uncommitted.load();
        check(u"next_keeps_choices_in_draft"_s, !uncommitted.onboardingComplete && !uncommitted.contentIndex
            && uncommitted.hotkey.isEmpty() && !launcherWindow());
    }
    setup->setProperty("clipboard", true);
    setup->setProperty("clipboardWinV", true);
    setup->setProperty("capture", true);
    setup->setProperty("captureHotkey", captureHotkey);
    setup->beginTest(true);
    setup->setRecording(true);
    check(u"back_preserves_all_choices"_s, navigate(u"onboardingPrevious"_s) && setup->page() == 0
        && setup->property("contentIndex").toBool() && setup->property("launchHotkey").toString() == launchHotkey
        && setup->property("clipboard").toBool() && setup->property("clipboardWinV").toBool()
        && setup->property("capture").toBool() && setup->property("captureHotkey").toString() == captureHotkey);
    check(u"navigation_releases_key_capture"_s, !setup->testing() && !setup->recording());
    // The window is as tall as its taller step with everything on it: nothing is scrolled.
    const auto wholeStepShown = [&] {
        const auto* scroll = qobject_cast<QQuickItem*>(visualNamed(window->contentItem(), u"onboardingScroll"_s));
        return scroll && !scroll->property("interactive").toBool()
            && scroll->property("contentHeight").toDouble() <= scroll->height() + 0.5;
    };
    check(u"first_page_needs_no_scrolling"_s, wholeStepShown());
    check(u"first_page_choices_screenshot"_s, saveGrab(window, out.filePath(u"onboarding-page1-choices.png"_s)));
    check(u"return_to_page_two"_s, navigate(u"onboardingNext"_s) && setup->page() == 1);
    setup->setProperty("clipboard", false); // layout/consent tests never record the user's clipboard
    setup->setProperty("clipboardWinV", false);

    // An enabled screenshot without a shortcut must keep the draft open.
    setup->setProperty("captureHotkey", QString());
    auto* start = visualNamed(window->contentItem(), u"onboardingStart"_s);
    QMetaObject::invokeMethod(start, "clicked");
    check(u"invalid_shortcut_stays_in_guide"_s, !setup->committed() && !setup->message().isEmpty() && !launcherWindow());
    setup->setProperty("captureHotkey", launchHotkey);
    QMetaObject::invokeMethod(start, "clicked");
    check(u"duplicate_shortcuts_rejected"_s, !setup->committed() && !setup->message().isEmpty());
    setup->setProperty("captureHotkey", captureHotkey);
    setup->beginTest(true);
    QKeyEvent captureKey(QEvent::KeyPress, Qt::Key_F1 + freeKeys[1] - 1, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
    QCoreApplication::sendEvent(window, &captureKey);
    check(u"local_capture_shortcut_test"_s, !setup->testing() && !setup->testMessage().isEmpty());
    setup->setProperty("launchHotkey", QString());
    QMetaObject::invokeMethod(start, "clicked");
    settle(window);
    check(u"final_validation_returns_to_first_page"_s, setup->page() == 0 && !setup->committed()
        && !setup->message().isEmpty() && visible(u"onboardingActivation"_s));
    setup->setProperty("launchHotkey", launchHotkey);
    check(u"repair_first_page_and_continue"_s, navigate(u"onboardingNext"_s) && setup->page() == 1
        && setup->property("captureHotkey").toString() == captureHotkey);
    const bool capturing = action == u"capture" || action == u"offline" || action == u"incompatible" || action == u"retry";
    setup->setProperty("capture", capturing);
    {
        QTemporaryDir blocked;
        const QString originalProfile = Settings::profile();
        Settings::useProfile(blocked.path());
        const auto restore = qScopeGuard([&] { Settings::useProfile(originalProfile); });
        check(u"blocked_settings_fixture"_s, blocked.isValid() && QDir().mkpath(Settings::filePath()));
        QMetaObject::invokeMethod(start, "clicked");
        check(u"save_failure_keeps_guide_and_draft"_s, !setup->committed() && !setup->message().isEmpty()
            && !launcherWindow() && setup->property("contentIndex").toBool());
    }
    settle(window);
    check(u"save_error_screenshot"_s, saveGrab(window, out.filePath(u"onboarding-save-error.png"_s)));
    setup->setProperty("contentIndex", false);
    setup->setProperty("contentIndex", true);

    setup->setProperty("clipboard", true);
    setup->setProperty("clipboardWinV", true);
    setup->setProperty("capture", true);
    settle(window);
    check(u"second_page_needs_no_scrolling"_s, wholeStepShown());
    // A screen with less room than that: a shorter window, whose step scrolls.
    const QVariant room = window->property("roomHeight");
    const int shortRoom = 400;
    window->setProperty("roomHeight", shortRoom);
    settle(window);
    check(u"short_screen_gets_short_window"_s, window->height() == shortRoom);
    auto* flick = qobject_cast<QQuickItem*>(visualNamed(window->contentItem(), u"onboardingScroll"_s));
    if (flick)
        flick->setProperty("contentY", std::max(0.0, flick->property("contentHeight").toDouble() - flick->height()));
    settle(window);
    auto* capture = qobject_cast<QQuickItem*>(visualNamed(window->contentItem(), u"onboardingCapture"_s));
    check(u"last_feature_reachable_at_minimum_size"_s, capture && flick
        && capture->mapToItem(flick, QPointF(0, capture->height())).y() <= flick->height() + 1);
    check(u"compact_screenshot"_s, saveGrab(window, out.filePath(u"onboarding-compact.png"_s)));
    window->setProperty("roomHeight", room);
    if (flick) flick->setProperty("contentY", 0);
    settle(window);
    check(u"window_back_to_its_size"_s, window->size() == guideSize);
    check(u"choices_screenshot"_s, saveGrab(window, out.filePath(u"onboarding-choices.png"_s)));
    setup->setProperty("clipboard", false);
    setup->setProperty("clipboardWinV", false);
    setup->setProperty("capture", capturing);
    if (action == u"later")
        check(u"later_from_first_page"_s, navigate(u"onboardingPrevious"_s) && setup->page() == 0);

    const QString button = action == u"later" ? u"onboardingLater"_s : u"onboardingStart"_s;
    QMetaObject::invokeMethod(visualNamed(window->contentItem(), button), "clicked");
    if (capturing) {
        QElapsedTimer waiting;
        waiting.start();
        bool permissionSeen = false;
        bool retried = false;
        while (waiting.elapsed() < 30000) {
            pump(20);
            // Turning the screenshot on was the yes: no question over the setup.
            for (QObject* asked : window->findChildren<QObject*>()) {
                if (asked->objectName() == u"pluginDialogCard" && asked->property("visible").toBool())
                    permissionSeen = true;
            }
            if (setup->canRetry() && action == u"retry" && !retried) {
                check(u"offline_before_retry"_s, true);
                // Keep Windows' Unicode environment: qgetenv/qputenv would
                // lose the Chinese characters in this workspace's fixture path.
                _wputenv_s(L"WINSHUN_PLUGIN_INDEX", qEnvironmentVariable("WINSHUN_ONBOARDING_RETRY_INDEX").toStdWString().c_str());
                QMetaObject::invokeMethod(visualNamed(window->contentItem(), u"onboardingRetry"_s), "clicked");
                retried = true;
                continue;
            }
            if (setup->captureReady() || setup->canRetry())
                break;
        }
        check(u"capture_committed"_s, setup->committed());
        const bool accepted = action == u"capture" || action == u"retry";
        check(u"no_second_permission_question"_s, !permissionSeen);
        check(u"capture_expected_state"_s, accepted ? setup->captureReady() : setup->canRetry());
        QSettings captureIni(Settings::dataDir() + u"/plugins/winshun.capture.data/capture.ini"_s, QSettings::IniFormat);
        check(u"capture_shortcut_saved"_s, captureIni.value(u"Capture/Hotkey"_s).toString() == captureHotkey);
        check(u"printscreen_unchanged"_s, !captureIni.value(u"Capture/PrintScreen"_s, true).toBool());
        if (accepted) {
            bool registered = false;
            QElapsedTimer waitingForKey;
            waitingForKey.start();
            while (waitingForKey.elapsed() < 5000) {
                if (::RegisterHotKey(nullptr, 0x7351, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F1 + freeKeys[1] - 1))
                    ::UnregisterHotKey(nullptr, 0x7351);
                else {
                    registered = ::GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED;
                    break;
                }
                pump(20);
            }
            check(u"capture_shortcut_registered_by_real_component"_s, registered);
        }
        report.insert(u"capture_message"_s, setup->message());
        settle(window);
        check(u"capture_status_screenshot"_s, saveGrab(window, out.filePath(u"onboarding-capture.png"_s)));
        QMetaObject::invokeMethod(visualNamed(window->contentItem(), setup->captureReady() ? u"onboardingStart"_s : u"onboardingLater"_s), "clicked");
    }
    pump(150);
    check(u"guide_closes_after_completion"_s, !guideWindow());
    check(u"runtime_created_after_confirmation"_s, launcherWindow());
    check(u"launcher_visible_after_setup"_s, launcherWindow() && launcherWindow()->isVisible());
    Settings saved;
    saved.load();
    check(u"completion_saved"_s, saved.onboardingComplete);
    check(u"index_choice_saved"_s, saved.contentIndex == (action != u"later"));
    check(u"no_clipboard_without_opt_in"_s, !saved.clipboard && !saved.clipboardWinV);
    check(u"capture_pending_saved"_s, saved.onboardingCapturePending == (capturing && action != u"capture" && action != u"retry"));
    check(u"activation_choice_saved"_s, action == u"later" ? saved.doubleCtrl && saved.doubleTapKey == u"ctrl"
        : !saved.doubleCtrl && saved.hotkey == launchHotkey);
    report.insert(u"checks"_s, checks);
    return passed;
}

// Production App, launcher, settings model and plugin installation on the
// runtime probe's isolated profile. Opens no browser and uses no user data.
// --ws-plugin-check <ws.exe> (GitHub #251): the real ws.exe against this App
// over a pipe of the probe's own, through everything between: the server,
// the tools kept for ws, PluginCommands, the 插件 page and its dialogs. What
// the user would click is answered here through the dialog's own accept and
// reject. The library is a list and two packages made beside the report
// (wsPluginFixture, before the App starts).
const QString kWsPluginPipe = u"WinShun.Probe.WsPlugin.%1"_s.arg(QCoreApplication::applicationPid());

QByteArray probePluginManifest(const QString& id, const QString& version, bool program = false)
{
    QJsonObject o {{u"manifest"_s, 1}, {u"id"_s, id}, {u"name"_s, QJsonObject {{u"zh"_s, u"探针插件"_s}, {u"en"_s, u"Probe plugin"_s}}},
        {u"version"_s, version}, {u"permissions"_s, QJsonArray {u"network"_s}},
        {u"commands"_s, QJsonArray {QJsonObject {{u"id"_s, u"list"_s}, {u"keyword"_s, u"zzprobe"_s}, {u"title"_s, u"List"_s},
                            {u"run"_s, QJsonObject {{u"items"_s, QJsonArray {QJsonObject {{u"id"_s, u"a"_s}, {u"title"_s, u"Alpha"_s}}}}}}}}}};
    if (program) { // a program of its own: the library lists it as nobody's review
        o.insert(u"runtime"_s, QJsonObject {{u"mode"_s, u"once"_s}, {u"program"_s, u"probe.exe"_s}});
        o.insert(u"minWinShun"_s, u"0.22.0"_s);
        QJsonArray commands = o.value(u"commands"_s).toArray();
        QJsonObject command = commands[0].toObject();
        command.remove(u"run"_s);
        commands[0] = command;
        o.insert(u"commands"_s, commands);
    }
    return QJsonDocument(o).toJson();
}

bool wsPluginFixture(const QDir& out)
{
    const auto package = [&](const QString& id, const QString& review, bool program) -> QJsonObject {
        mz_zip_archive zip {};
        if (!mz_zip_writer_init_heap(&zip, 0, 0))
            return {};
        const QByteArray manifest = probePluginManifest(id, u"1.0.0"_s, program);
        bool ok = mz_zip_writer_add_mem(&zip, "plugin.json", manifest.constData(), static_cast<size_t>(manifest.size()), MZ_DEFAULT_LEVEL);
        if (program)
            ok = ok && mz_zip_writer_add_mem(&zip, "probe.exe", "MZ", 2, MZ_DEFAULT_LEVEL);
        void* buffer = nullptr;
        size_t size = 0;
        ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size);
        mz_zip_writer_end(&zip);
        if (!ok)
            return {};
        const QByteArray bytes(static_cast<const char*>(buffer), static_cast<qsizetype>(size));
        mz_free(buffer);
        const QString name = id + u"-1.0.0.wsplugin"_s;
        QFile file(out.filePath(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
            return {};
        QJsonObject entry {{u"id"_s, id}, {u"name"_s, QJsonObject {{u"zh"_s, u"探针插件"_s}}}, {u"version"_s, u"1.0.0"_s},
            {u"url"_s, name}, {u"sha256"_s, QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
            {u"size"_s, bytes.size()}, {u"review"_s, review}, {u"permissions"_s, QJsonArray {u"network"_s}}};
        if (program)
            entry.insert(u"runtime"_s, QJsonObject {{u"mode"_s, u"once"_s}, {u"program"_s, u"probe.exe"_s}});
        return entry;
    };
    const QJsonObject listed = package(u"probe.listed"_s, u"reviewed"_s, false);
    const QJsonObject native = package(u"probe.native"_s, u"unreviewed-native"_s, true);
    if (listed.isEmpty() || native.isEmpty())
        return false;
    QFile index(out.filePath(u"ws-plugin-index.json"_s));
    if (!index.open(QIODevice::WriteOnly)
        || index.write(QJsonDocument(QJsonObject {{u"schema"_s, 1}, {u"plugins"_s, QJsonArray {listed, native}}}).toJson()) <= 0)
        return false;
    _wputenv_s(L"WINSHUN_PLUGIN_INDEX", QDir::toNativeSeparators(index.fileName()).toStdWString().c_str());
    QSettings settings(Settings::filePath(), QSettings::IniFormat);
    settings.setValue(u"Mcp/Enabled"_s, true); // the connection ws shares with AI clients
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool probeWsPlugin(App& app, const QStringList& args, const QDir& out, QJsonObject& report)
{
    Q_UNUSED(app);
    QJsonObject checks;
    QJsonArray runs;
    bool passed = true;
    const auto check = [&](bool ok, const QString& key) {
        checks.insert(key, ok);
        if (!ok) { qWarning().noquote() << "ws plugin:" << key; passed = false; }
        return ok;
    };
    const auto finish = qScopeGuard([&] { report.insert(u"ws_plugin"_s, QJsonObject {{u"checks"_s, checks}, {u"runs"_s, runs}, {u"passed"_s, passed}}); });
    const QString wsExe = valueAfter(args, u"--ws-plugin-check");
    if (!check(QFileInfo::exists(wsExe), u"ws_exe_given"_s))
        return false;

    struct Ran {
        int code = -1;
        QString out;
        QString err;
    };
    // ws as a person or an agent runs it, this App answering meanwhile.
    const auto ws = [&](const QStringList& words) {
        QProcess process;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(u"WS_SETTINGS"_s, QDir::toNativeSeparators(Settings::filePath()));
        environment.insert(u"WS_LANG"_s, u"zh"_s);
        environment.insert(u"WS_ENCODING"_s, u"utf8"_s);
        environment.remove(u"QT_QPA_PLATFORM"_s);
        process.setProcessEnvironment(environment);
        process.start(wsExe, QStringList {u"--pipe"_s, kWsPluginPipe, u"--no-start"_s, u"plugin"_s} + words);
        QElapsedTimer timer;
        timer.start();
        while (process.state() != QProcess::NotRunning && timer.elapsed() < 60000)
            pump(20);
        Ran ran;
        if (process.state() != QProcess::NotRunning)
            process.kill();
        else
            ran.code = process.exitCode();
        ran.out = QString::fromUtf8(process.readAllStandardOutput());
        ran.err = QString::fromUtf8(process.readAllStandardError());
        runs.push_back(QJsonObject {{u"ws"_s, words.join(u' ')}, {u"code"_s, ran.code}, {u"out"_s, ran.out}, {u"err"_s, ran.err}});
        return ran;
    };
    const auto stateOf = [&](const QString& id) -> QString {
        const Ran listed = ws({u"list"_s, u"--json"_s});
        for (const QJsonValue plugin : QJsonDocument::fromJson(listed.out.toUtf8()).array()) {
            if (plugin.toObject().value(u"id"_s).toString() == id)
                return plugin.toObject().value(u"version"_s).toString() + u' ' + plugin.toObject().value(u"state"_s).toString();
        }
        return listed.code == 0 ? u"absent"_s : u"list failed"_s;
    };
    QQuickWindow* window = nullptr;
    const auto settingsWindow = [&]() -> QQuickWindow* {
        for (auto* candidate : QGuiApplication::allWindows()) {
            if (candidate->property("editor").value<SettingsEditor*>())
                return qobject_cast<QQuickWindow*>(candidate);
        }
        return nullptr;
    };
    // The page's question, as it stands over the settings window: its kind while it is up.
    const auto question = [&]() -> QQuickItem* {
        window = settingsWindow();
        if (!window || !window->isVisible())
            return nullptr;
        settle(window, 2);
        QQuickItem* card = visualNamed(window->contentItem(), u"pluginDialogCard"_s);
        QQuickItem* dialog = card ? card->parentItem() : nullptr;
        return dialog && dialog->property("open").toBool() ? dialog : nullptr;
    };
    const auto questionKind = [&] {
        QQuickItem* dialog = question();
        return dialog ? dialog->property("kind").toString() : QString();
    };
    const auto answer = [&](bool yes) {
        QQuickItem* dialog = question();
        if (!dialog)
            return false;
        const bool called = yes ? QMetaObject::invokeMethod(dialog, "accept", Q_ARG(QVariant, QVariant(false)))
                                : QMetaObject::invokeMethod(dialog, "reject");
        pump(300);
        return called;
    };
    const QString userDir = Settings::dataDir() + u"/plugins"_s;
    const QString source = out.filePath(u"ws-plugin-src"_s);
    const auto writeSource = [&](const QString& version) {
        QDir().mkpath(source);
        QFile file(source + u"/plugin.json"_s);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(probePluginManifest(u"probe.hello"_s, version)) > 0;
    };

    // A folder of one's own: installed at once, off, and no window comes up for it.
    check(ws({u"list"_s}).code <= 1, u"list_answers"_s); // 1: a build folder without the plugins that come with Win顺
    check(writeSource(u"1.0.0"_s), u"source_written"_s);
    Ran ran = ws({u"install"_s, QDir::toNativeSeparators(source)});
    check(ran.code == 0 && ran.out.contains(u"ws plugin enable probe.hello"_s), u"install_folder_done"_s);
    check(QFileInfo::exists(userDir + u"/probe.hello/plugin.json"_s), u"install_folder_in_place"_s);
    check(stateOf(u"probe.hello"_s) == u"1.0.0 off", u"installed_is_off"_s);
    check(!settingsWindow() || !settingsWindow()->isVisible(), u"install_folder_shows_no_window"_s);

    // Turned on for the first time: the user's to answer, in the settings window.
    ran = ws({u"enable"_s, u"probe.hello"_s});
    check(ran.code == 1, u"enable_first_time_asks"_s);
    check(questionKind() == u"permissions", u"permission_dialog_up"_s);
    check(window && saveGrab(window, out.filePath(u"ws-plugin-permissions.png"_s)), u"permission_screenshot"_s);
    check(stateOf(u"probe.hello"_s) == u"1.0.0 off", u"off_until_answered"_s);
    check(answer(false) && stateOf(u"probe.hello"_s) == u"1.0.0 off", u"declined_stays_off"_s);
    check(ws({u"enable"_s, u"probe.hello"_s}).code == 1 && questionKind() == u"permissions", u"asks_again_after_no"_s);
    check(answer(true) && stateOf(u"probe.hello"_s) == u"1.0.0 on", u"agreed_turns_on"_s);
    check(ws({u"enable"_s, u"probe.hello"_s}).code == 0 && questionKind().isEmpty(), u"enable_again_asks_nothing"_s);

    // The loop of someone writing a plugin: changed, installed again, still on.
    check(writeSource(u"1.1.0"_s), u"source_changed"_s);
    ran = ws({u"install"_s, QDir::toNativeSeparators(source)});
    check(ran.code == 0 && questionKind().isEmpty(), u"reinstall_done_unasked"_s);
    check(stateOf(u"probe.hello"_s) == u"1.1.0 on", u"reinstall_stays_on"_s);
    check(ws({u"disable"_s, u"probe.hello"_s}).code == 0 && stateOf(u"probe.hello"_s) == u"1.1.0 off", u"disable"_s);
    check(ws({u"enable"_s, u"probe.hello"_s}).code == 0 && stateOf(u"probe.hello"_s) == u"1.1.0 on", u"enable_once_agreed_unasked"_s);

    // The library: found, installed on its page; a package nobody reviewed is asked about.
    ran = ws({u"search"_s, u"probe"_s});
    check(ran.code == 0 && ran.out.contains(u"probe.listed"_s) && ran.out.contains(u"probe.native"_s), u"search_lists_library"_s);
    ran = ws({u"install"_s, u"probe.listed"_s});
    check(ran.code == 0 && QFileInfo::exists(userDir + u"/probe.listed/plugin.json"_s), u"install_listed_done"_s);
    check(stateOf(u"probe.listed"_s) == u"1.0.0 off", u"listed_is_off"_s);
    check(ws({u"install"_s, u"probe.listed"_s}).code == 0, u"install_listed_again_is_done"_s);
    ran = ws({u"install"_s, u"probe.native"_s});
    check(ran.code == 1 && questionKind() == u"install", u"unreviewed_package_asks"_s);
    check(window && saveGrab(window, out.filePath(u"ws-plugin-unreviewed.png"_s)), u"unreviewed_screenshot"_s);
    check(answer(false) && !QFileInfo::exists(userDir + u"/probe.native"_s), u"unreviewed_declined_not_installed"_s);
    check(ws({u"install"_s, u"probe.absent"_s}).code == 7, u"unknown_id_fails"_s);

    // Removing: always the user's to answer.
    ran = ws({u"remove"_s, u"probe.hello"_s});
    check(ran.code == 1 && questionKind() == u"uninstall", u"remove_asks"_s);
    check(window && saveGrab(window, out.filePath(u"ws-plugin-remove.png"_s)), u"remove_screenshot"_s);
    check(QFileInfo::exists(userDir + u"/probe.hello/plugin.json"_s), u"there_until_answered"_s);
    check(answer(true) && !QFileInfo::exists(userDir + u"/probe.hello"_s), u"confirmed_removes"_s);
    check(stateOf(u"probe.hello"_s) == u"absent", u"removed_is_gone"_s);
    check(ws({u"enable"_s, u"probe.hello"_s}).code == 6, u"gone_is_not_found"_s);
    if (QQuickWindow* shown = settingsWindow()) { // put away as the user would
        shown->close();
        pump(300);
    }
    // The cards the list made again (a plugin came, went, changed) were left
    // to deleteLater, which nothing runs here: this probe has no event loop,
    // only pump(). Left alive, a card would outlive the theme at the App's
    // end, and the glyph of its switch, its font's family then empty
    // (Toggle.qml), has Qt look for its default "MS Sans Serif": a
    // DirectWrite warning that is this probe's, not the App's.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return passed;
}

bool probeWebPlugin(App& app, const QStringList& args, const QDir& out, QJsonObject& report)
{
    QJsonObject checks;
    bool passed = true;
    const auto check = [&](bool ok, const QString& key) {
        checks.insert(key, ok);
        if (!ok) { qWarning().noquote() << "web plugin:" << key; passed = false; }
        return ok;
    };
    app.showSettings();
    pump(100);
    QQuickWindow* settingsWindow = nullptr;
    SettingsEditor* editor = nullptr;
    for (auto* candidate : QGuiApplication::allWindows()) {
        if (auto* found = candidate->property("editor").value<SettingsEditor*>()) {
            editor = found;
            settingsWindow = qobject_cast<QQuickWindow*>(candidate);
            break;
        }
    }
    if (!check(settingsWindow && editor, u"production_settings"_s)) return false;
    QMetaObject::invokeMethod(settingsWindow, "showWebPage");
    settle(settingsWindow, 3);
    const auto* addButton = visualNamed(settingsWindow->contentItem(), u"webAddButton"_s);
    check(addButton && addButton->isVisible()
        && QRectF(QPointF(), settingsWindow->size()).contains(addButton->mapRectToItem(settingsWindow->contentItem(), addButton->boundingRect())),
        u"plugin_settings_reveals_editor"_s);
    check(editor->saveWebShortcut(-1, u"zzwebfixture"_s, u"迁移验证站点"_s,
        u"https://fixture.invalid/?q=%s"_s, u"http://127.0.0.1:9/"_s).isEmpty(), u"production_editor_saves_site"_s);
    auto* searchBox = named(settingsWindow, "searchBox");
    auto* settingsSearch = named(settingsWindow, "settingsSearch");
    QMetaObject::invokeMethod(searchBox, "setText", Q_ARG(QVariant, QVariant(u"zzwebfixture"_s)));
    settle(settingsWindow, 3);
    check(settingsSearch && settingsSearch->property("current").value<QObject*>() != nullptr
        && settingsWindow->property("currentPage") == settingsWindow->property("pluginsPage"), u"settings_search_finds_edited_site"_s);
    QMetaObject::invokeMethod(searchBox, "setText", Q_ARG(QVariant, QVariant(QString())));
    QMetaObject::invokeMethod(settingsWindow, "showWebPage");
    settle(settingsWindow, 3);
    check(saveGrab(settingsWindow, out.filePath(u"web-plugin-settings.png"_s)), u"settings_screenshot"_s);
    const auto pages = settingsWindow->property("pages").value<QJSValue>().toVariant().toList();
    check(std::none_of(pages.cbegin(), pages.cend(), [](const QVariant& page) {
        return page.toMap().value(u"title"_s).toString() == QCoreApplication::translate("SettingsWindow", "网页搜索");
    }), u"no_duplicate_sidebar_page"_s);
    app.showLauncher(u"wsweb 中文 & spaces"_s);
    pump(100);
    Launcher* launcher = nullptr;
    QQuickWindow* searchWindow = nullptr;
    for (auto* candidate : QGuiApplication::allWindows()) {
        if (auto* found = candidate->property("launcher").value<Launcher*>()) {
            launcher = found;
            searchWindow = qobject_cast<QQuickWindow*>(candidate);
            break;
        }
    }
    if (!check(launcher && searchWindow, u"production_launcher"_s)) return false;
    const auto webRows = [&] {
        int count = 0;
        for (int i = 0; i < launcher->results()->count(); ++i)
            count += launcher->results()->at(i)->isWeb();
        return count;
    };
    const auto wait = [&](const auto& predicate) {
        QElapsedTimer timer; timer.start();
        do { pump(20); if (predicate()) return true; } while (timer.elapsed() < 5000);
        return false;
    };
    check(wait([&] { return webRows() >= 1; }), u"enabled_keyword_search"_s);
    check(launcher->results()->at(0) && launcher->results()->at(0)->isWeb(), u"keyword_ranks_first"_s);
    if (!valueAfter(args, u"--web-suggest-url").isEmpty())
        check(wait([&] { return webRows() >= 3; }), u"live_suggestions"_s);
    settle(searchWindow, 3); // Let the popup resize after asynchronous suggestions.
    check(saveGrab(searchWindow, out.filePath(u"web-plugin-search.png"_s)), u"search_screenshot"_s);
    app.showLauncher(u"网页测试"_s);
    check(wait([&] { return webRows() >= 1; }), u"site_name_search"_s);
    app.showLauncher(u"wycs"_s);
    check(wait([&] { return webRows() >= 1; }), u"site_pinyin_search"_s);
    const QString favoritePath = QDir::toNativeSeparators(Settings::profile() + u"/fixture/startup-probe.txt"_s);
    editor->favorites()->addPaths({QVariant(favoritePath)});
    const int favorite = editor->favorites()->indexOf(favoritePath);
    check(favorite >= 0, u"favorite_keyword_fixture"_s);
    check(favorite >= 0 && !editor->favorites()->save(favorite, u"Keyword fixture"_s, u"wsweb"_s).isEmpty(),
        u"enabled_reserves_keyword"_s);
    editor->plugins()->setEnabled(u"winshun.web"_s, false);
    check(favorite >= 0 && editor->favorites()->save(favorite, u"Keyword fixture"_s, u"wsweb"_s).isEmpty(),
        u"disabled_releases_keyword"_s);
    if (favorite >= 0)
        editor->favorites()->remove(favorite);
    app.showLauncher(u"wsweb disabled"_s);
    check(wait([&] { return webRows() == 0; }), u"disabled_removes_rows"_s);
    editor->plugins()->confirmEnable(u"winshun.web"_s);
    app.showLauncher(u"wsweb restored"_s);
    check(wait([&] { return webRows() >= 1; }), u"reenable_restores_rows"_s);
    const auto before = editor->webShortcuts();
    editor->plugins()->confirmUninstall(u"winshun.web"_s, false);
    app.showLauncher(u"wsweb uninstalled"_s);
    check(wait([&] { return webRows() == 0; }), u"uninstall_removes_rows"_s);
    check(editor->webShortcuts() == before, u"uninstall_keeps_configuration"_s);
    Settings saved; saved.load();
    check(!saved.plugin(u"winshun.web")->enabled && !saved.webShortcuts.isEmpty(), u"uninstall_persists_disabled_data"_s);
    QString error;
    const QString archive = valueAfter(args, u"--web-plugin-archive");
    auto package = plugin::install::inspect(archive.isEmpty()
        ? QDir(valueAfter(args, u"--runtime-plugins")).filePath(u"winshun.web"_s) : archive);
    check(package && package->manifest && package->manifest->webSearch, u"reinstall_package_valid"_s);
    if (!archive.isEmpty())
        check(package && package->archive, u"reinstall_from_archive"_s);
    if (package && package->manifest) {
        check(!plugin::install::place(*package, Settings::profile() + u"/plugins"_s, &error).isEmpty(), u"reinstall_package"_s);
        plugin::install::discard(*package);
        check(wait([&] { return editor->plugins()->list()->card(u"winshun.web"_s) != nullptr; }), u"reinstall_discovered"_s);
        editor->plugins()->confirmEnable(u"winshun.web"_s);
        app.showLauncher(u"wsweb reinstalled"_s);
        check(wait([&] { return webRows() >= 1; }), u"reinstall_restores_search"_s);
        check(editor->webShortcuts() == before, u"reinstall_keeps_configuration"_s);
    }
    report.insert(u"web_plugin"_s, checks);
    report.insert(u"web_plugin_passed"_s, passed);
    return passed;
}

// Production clipboard entry points against a Shell-like receiver or native
// Start. No clipboard data is copied or pasted by this probe.
bool probeClipboardShell(const QString& receiver, const QDir& out, QJsonObject& report, bool splitDisplay = false,
    bool requireUiAccess = false)
{
    QProcess child;
    child.setProcessChannelMode(QProcess::MergedChannels);
    const bool nativeShell = receiver == u"native";
    if (!nativeShell)
        child.start(receiver, {u"--shell-receiver"_s});
    const auto cleanup = qScopeGuard([&] {
        if (child.state() != QProcess::NotRunning) {
            child.terminate();
            if (!child.waitForFinished(2000)) {
                child.kill(); // this probe's own temporary receiver only
                child.waitForFinished(2000);
            }
        }
    });
    QByteArray output;
    QElapsedTimer deadline;
    deadline.start();
    while (!nativeShell && !output.contains('\n') && deadline.elapsed() < 3000) {
        child.waitForReadyRead(50);
        output += child.readAll();
        pump(10);
    }
    const auto match = QRegularExpression(u"window:(\\d+)"_s).match(QString::fromUtf8(output));
    HWND target = match.hasMatch() ? reinterpret_cast<HWND>(quintptr(match.captured(1).toULongLong())) : nullptr;
    if (nativeShell) {
        // SC_TASKLIST opens Start without synthesizing a Windows-key shortcut.
        ::PostMessageW(::GetShellWindow(), WM_SYSCOMMAND, SC_TASKLIST, 0);
        pump(1000);
        target = ::GetForegroundWindow();
    }
    const auto closeNativeMenu = qScopeGuard([&] {
        if (nativeShell && win::isShellFlyout(target) && ::GetForegroundWindow() == target)
            ::PostMessageW(::GetShellWindow(), WM_SYSCOMMAND, SC_TASKLIST, 0);
    });
    QJsonObject checks;
    bool passed = true;
    const auto check = [&](const QString& name, bool ok) {
        checks.insert(name, ok);
        passed &= ok;
        if (!ok) qWarning().noquote() << "clipboard shell probe:" << name;
    };
    const auto saveChecks = qScopeGuard([&] {
        report.insert(u"clipboard_shell"_s, checks);
        report.insert(u"clipboard_shell_passed"_s, passed);
    });
    check(u"receiver_ready"_s, target && ::IsWindow(target));
    if (!passed)
        return false;
    std::unique_ptr<QWindow> foreign;
    if (!nativeShell) {
        foreign.reset(QWindow::fromWinId(reinterpret_cast<WId>(target)));
        win::bringToFront(foreign.get(), win::ShellFlyout::Preserve);
    }
    pump(100);
    check(u"receiver_is_shell"_s, win::isShellFlyout(target));
    check(u"receiver_in_front"_s, ::GetForegroundWindow() == target);
    if (nativeShell) {
        report.insert(u"qt_dpr"_s, QGuiApplication::primaryScreen()->devicePixelRatio());
        report.insert(u"foreground_program"_s, foreground::programOf(target));
        report.insert(u"foreground_class"_s, [&] { wchar_t name[128] {}; ::GetClassNameW(target, name, 128); return QString::fromWCharArray(name); }());
    }
    // Hit the same production WM_HOTKEY handler as Win+V and the custom key,
    // without changing global key registration or sending the Windows key.
    HWND messages = nullptr;
    for (HWND candidate = nullptr; (candidate = ::FindWindowExW(nullptr, candidate, L"WinShun.MessageWindow", nullptr));) {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(candidate, &pid);
        if (pid == ::GetCurrentProcessId()) { messages = candidate; break; }
    }
    check(u"message_window"_s, messages != nullptr);
    if (!passed)
        return false;
    ClipboardDisplay* display = nullptr;
    if (splitDisplay) {
        ::PostMessageW(messages, WM_HOTKEY, 3, 0);
        QElapsedTimer warming;
        warming.start();
        while (warming.elapsed() < 5000) {
            pump(25);
            for (QWindow* window : QGuiApplication::topLevelWindows())
                if (auto* peer = qobject_cast<ClipboardDisplay*>(window->property("displayController").value<QObject*>()))
                    display = peer;
            if (display && display->ready()) break;
        }
        check(u"display_ready"_s, display && display->ready());
        check(requireUiAccess ? u"real_uiaccess"_s : u"probe_is_not_uiaccess"_s,
            display && display->uiAccess() == requireUiAccess);
        if (requireUiAccess && display && display->ready()) {
            using GetBand = BOOL(WINAPI*)(HWND, DWORD*);
            const auto getBand = reinterpret_cast<GetBand>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetWindowBand"));
            DWORD band = 0;
            check(u"native_uiaccess_band"_s, getBand && getBand(display->handle(), &band) && band == 2);
            report.insert(u"display_band"_s, int(band));
            DWORD peerPid = 0; ::GetWindowThreadProcessId(display->handle(), &peerPid);
            report.insert(u"display_pid"_s, int(peerPid));
        }
        if (display) report.insert(u"display_output"_s, display->probeOutput());
        pump(500); // the first source frame must be revealed before its hotkey can close it
        ::PostMessageW(messages, WM_HOTKEY, 3, 0);
        pump(300);
        for (QWindow* window : QGuiApplication::topLevelWindows())
            if (window->property("displayController").value<QObject*>() == display)
                check(u"warmup_closed"_s, !window->isVisible());
        if (!passed) return false;
    }
    for (const auto& trigger : {QPair {3, u"winv"_s}, QPair {4, u"custom"_s}}) {
        output.clear();
        if (!nativeShell)
            child.readAll();
        ::PostMessageW(messages, WM_HOTKEY, WPARAM(trigger.first), 0);
        pump(450);
        QQuickWindow* clipboard = nullptr;
        for (QWindow* window : QGuiApplication::topLevelWindows())
            if (QByteArray(window->metaObject()->className()).startsWith("ClipboardWindow_"))
                clipboard = qobject_cast<QQuickWindow*>(window);
        check(trigger.second + u"_shown"_s, clipboard && clipboard->isVisible());
        if (!clipboard)
            break;
        if (splitDisplay) {
            check(trigger.second + u"_peer_active"_s, display->active());
            if (!display->active()) { report.insert(u"display_output"_s, display->probeOutput()); break; }
            RECT targetBounds {};
            ::GetWindowRect(target, &targetBounds);
            auto* placement = qobject_cast<Placement*>(clipboard->property("placement").value<QObject*>());
            if (placement) placement->placeDisplay({targetBounds.left + 30, targetBounds.top + 100,
                clipboard->width(), clipboard->height()}, 1000, false);
            pump(150);
        }
        const HWND panel = splitDisplay ? display->handle() : reinterpret_cast<HWND>(clipboard->winId());
        check(trigger.second + u"_native_visible"_s, ::IsWindowVisible(panel));
        DWORD panelCloaked = 0;
        ::DwmGetWindowAttribute(panel, DWMWA_CLOAKED, &panelCloaked, sizeof panelCloaked);
        check(trigger.second + u"_uncloaked"_s, panelCloaked == 0);
        RECT panelRect {}, targetRect {};
        ::GetWindowRect(panel, &panelRect);
        ::GetWindowRect(target, &targetRect);
        if (nativeShell) {
            const QRect all = win::shellFlyoutBounds(target);
            check(trigger.second + u"_shell_bounds_valid"_s, !all.isEmpty()
                && all.contains(QRect(targetRect.left, targetRect.top, targetRect.right - targetRect.left, targetRect.bottom - targetRect.top)));
            targetRect = {all.x(), all.y(), all.x() + all.width(), all.y() + all.height()};
        }
        RECT overlap {};
        if (splitDisplay)
            check(trigger.second + u"_overlays_receiver"_s, ::IntersectRect(&overlap, &panelRect, &targetRect));
        else
            check(trigger.second + u"_not_covered"_s, !::IntersectRect(&overlap, &panelRect, &targetRect));
        bool hitVisible = true;
        QJsonArray blockers;
        for (int x : {1, 2, 3})
            for (int y : {1, 2, 3}) {
                const POINT point {panelRect.left + (panelRect.right - panelRect.left) * x / 4,
                    panelRect.top + (panelRect.bottom - panelRect.top) * y / 4};
                const HWND hit = ::WindowFromPoint(point);
                hitVisible &= hit && ::GetAncestor(hit, GA_ROOT) == panel;
                if (hit && ::GetAncestor(hit, GA_ROOT) != panel) {
                    const HWND root = ::GetAncestor(hit, GA_ROOT);
                    RECT rect {};
                    ::GetWindowRect(root, &rect);
                    blockers.append(QJsonObject {{u"program"_s, foreground::programOf(root)},
                        {u"rect"_s, QJsonArray {int(rect.left), int(rect.top), int(rect.right), int(rect.bottom)}}});
                }
            }
        check(trigger.second + u"_hit_test_visible"_s, hitVisible);
        if (!blockers.isEmpty()) checks.insert(trigger.second + u"_blockers"_s, blockers);
        checks.insert(trigger.second + u"_panel_rect"_s, QJsonArray {int(panelRect.left), int(panelRect.top), int(panelRect.right), int(panelRect.bottom)});
        checks.insert(trigger.second + u"_shell_rect"_s, QJsonArray {int(targetRect.left), int(targetRect.top), int(targetRect.right), int(targetRect.bottom)});
        if (!nativeShell && !splitDisplay) {
            // The initial stdout is cleared between triggers, so locate the
            // fixture's deliberately separate sidebar by its fixed title.
            const HWND side = ::FindWindowW(L"WinShun.KeyRouterTest", L"WinShun shell companion test");
            RECT sideRect {};
            check(trigger.second + u"_companion_avoided"_s,
                side && ::GetWindowRect(side, &sideRect) && !::IntersectRect(&overlap, &panelRect, &sideRect));
        }
        check(trigger.second + u"_focus_kept"_s, ::GetForegroundWindow() == target);
        check(trigger.second + u"_passive"_s, (::GetWindowLongPtrW(panel, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0);
        QObject* model = clipboard->property("clipboard").value<QObject*>();
        check(trigger.second + u"_keys_routed"_s, model && model->property("keysRouted").toBool());
        auto* controller = qobject_cast<Clipboard*>(model);
        check(trigger.second + u"_paste_target"_s, controller && controller->target() == target);
        check(trigger.second + u"_saved_width_kept"_s, model && model->property("listWidth").toInt() == 480);
        if ((!nativeShell || requireUiAccess) && ::GetForegroundWindow() == target && model && model->property("keysRouted").toBool()) {
            INPUT keys[2] {};
            keys[0].type = keys[1].type = INPUT_KEYBOARD;
            keys[0].ki.wVk = keys[1].ki.wVk = 'A';
            keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
            check(trigger.second + u"_input_sent"_s, ::SendInput(2, keys, sizeof(INPUT)) == 2);
            pump(150);
            check(trigger.second + u"_search_works"_s, model && model->property("query").toString() == u"a");
            if (splitDisplay) {
                checks.insert(trigger.second + u"_query"_s, model->property("query").toString());
                checks.insert(trigger.second + u"_routed_after_key"_s, model->property("keysRouted").toBool());
                checks.insert(trigger.second + u"_foreground_after_key"_s, foreground::programOf(::GetForegroundWindow()));
                DWORD foregroundPid = 0;
                ::GetWindowThreadProcessId(::GetForegroundWindow(), &foregroundPid);
                checks.insert(trigger.second + u"_foreground_pid_after_key"_s, int(foregroundPid));
                checks.insert(trigger.second + u"_source_pid"_s, int(::GetCurrentProcessId()));
                report.insert(u"display_output"_s, report.value(u"display_output"_s).toString() + display->probeOutput());
            }
        }
        if (splitDisplay && (!nativeShell || requireUiAccess) && controller && display->active() && ::GetForegroundWindow() == target) {
            const auto key = [&](WORD vk, WORD modifier = 0) {
                if (::GetForegroundWindow() != target) return false;
                for (const int held : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN})
                    if (::GetAsyncKeyState(held) & 0x8000) return false;
                std::vector<INPUT> inputs;
                const auto append = [&](WORD code, bool release) {
                    INPUT input {}; input.type = INPUT_KEYBOARD; input.ki.wVk = code;
                    input.ki.dwFlags = release ? KEYEVENTF_KEYUP : 0;
                    inputs.push_back(input);
                };
                if (modifier) append(modifier, false);
                append(vk, false); append(vk, true);
                if (modifier) append(modifier, true);
                const bool sent = ::SendInput(UINT(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size();
                pump(250);
                return sent;
            };
            check(trigger.second + u"_clear_search_sent"_s, key('A', VK_CONTROL) && key(VK_BACK));
            check(trigger.second + u"_search_cleared"_s, controller->query().isEmpty());
            check(trigger.second + u"_select_all_sent"_s, key('A', VK_CONTROL));
            check(trigger.second + u"_selected_all"_s, controller->items()->selectedCount() >= 2);
            check(trigger.second + u"_clear_selection_sent"_s, key(VK_ESCAPE));
            check(trigger.second + u"_selection_cleared"_s, controller->items()->selectedCount() == 0 && display->active());
            const int before = controller->items()->count();
            check(trigger.second + u"_remove_sent"_s, key(VK_DELETE));
            check(trigger.second + u"_remove_applied"_s, controller->items()->count() == before - 1 && controller->canUndo());
            check(trigger.second + u"_undo_sent"_s, key('Z', VK_CONTROL));
            check(trigger.second + u"_undo_applied"_s, controller->items()->count() == before);
            check(trigger.second + u"_preview_sent"_s, key('P', VK_MENU));
            check(trigger.second + u"_preview_preference"_s, controller->previewOpen());
            RECT preview {}; ::GetWindowRect(display->handle(), &preview);
            check(trigger.second + u"_preview_widened"_s, preview.right - preview.left > panelRect.right - panelRect.left);
            ::PostMessageW(messages, WM_HOTKEY, WPARAM(trigger.first), 0);
            pump(300);
            ::PostMessageW(messages, WM_HOTKEY, WPARAM(trigger.first), 0);
            pump(450);
            RECT reopened {}; ::GetWindowRect(display->handle(), &reopened);
            check(trigger.second + u"_preview_restored_on_reopen"_s, display->active()
                && controller->previewOpen() && reopened.right - reopened.left > panelRect.right - panelRect.left);
            check(trigger.second + u"_preview_closed_sent"_s, key('P', VK_MENU));
            pump(300);
            RECT collapsed {}; ::GetWindowRect(display->handle(), &collapsed);
            check(trigger.second + u"_preview_collapsed_stays"_s, !controller->previewOpen()
                && collapsed.right - collapsed.left == panelRect.right - panelRect.left);
            check(trigger.second + u"_menu_sent"_s, key(VK_APPS));
            const auto peerMenu = [&] {
                DWORD peerPid = 0; ::GetWindowThreadProcessId(display->handle(), &peerPid);
                for (HWND candidate = nullptr; (candidate = ::FindWindowExW(nullptr, candidate, nullptr, nullptr));) {
                    DWORD pid = 0; ::GetWindowThreadProcessId(candidate, &pid);
                    if (pid == peerPid && candidate != display->handle() && ::IsWindowVisible(candidate)
                        && (::GetWindowLongPtrW(candidate, GWL_STYLE) & WS_POPUP)
                        && !(::GetWindowLongPtrW(candidate, GWL_EXSTYLE) & WS_EX_TRANSPARENT)) return candidate;
                }
                return HWND(nullptr);
            };
            const HWND menu = peerMenu();
            check(trigger.second + u"_peer_menu_visible"_s, menu != nullptr);
            if (requireUiAccess && menu) {
                using GetBand = BOOL(WINAPI*)(HWND, DWORD*);
                const auto getBand = reinterpret_cast<GetBand>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetWindowBand"));
                DWORD band = 0;
                RECT rect {}; ::GetWindowRect(menu, &rect);
                const POINT center {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
                check(trigger.second + u"_menu_uiaccess_band"_s, getBand && getBand(menu, &band) && band == 2);
                check(trigger.second + u"_menu_on_top"_s, ::GetAncestor(::WindowFromPoint(center), GA_ROOT) == menu);
            }
            check(trigger.second + u"_menu_closed_sent"_s, key(VK_ESCAPE));
            check(trigger.second + u"_menu_closed_only"_s, peerMenu() == nullptr && display->active());
            check(trigger.second + u"_interaction_focus_kept"_s, ::GetForegroundWindow() == target);
            QGuiApplication::primaryScreen()->grabWindow(0, panelRect.left, panelRect.top,
                panelRect.right - panelRect.left, panelRect.bottom - panelRect.top)
                .save(out.filePath(u"peer-"_s + trigger.second + u".png"_s));
        }
        clipboard->grabWindow().save(out.filePath(u"clipboard-"_s + trigger.second + u".png"_s));
        if (nativeShell) {
            QGuiApplication::primaryScreen()->grabWindow(0)
                .save(out.filePath(u"desktop-"_s + trigger.second + u".png"_s));
        }
        // The same hotkey closes the clipboard only, leaving the target up.
        ::PostMessageW(messages, WM_HOTKEY, WPARAM(trigger.first), 0);
        pump(200);
        check(trigger.second + u"_closed"_s, !clipboard->isVisible());
        if (splitDisplay) check(trigger.second + u"_peer_closed"_s, !::IsWindowVisible(display->handle()));
        check(trigger.second + u"_target_kept"_s, ::IsWindowVisible(target) && ::GetForegroundWindow() == target);
        if (!nativeShell) {
            output += child.readAll();
            if (splitDisplay) checks.insert(trigger.second + u"_receiver_output"_s, QString::fromUtf8(output));
            check(trigger.second + u"_no_escape_or_activation"_s,
                !output.contains("down:27") && !output.contains("deactivated"));
        }
    }
    if (splitDisplay && !requireUiAccess && display && display->ready()) {
        ::PostMessageW(messages, WM_HOTKEY, 3, 0);
        pump(400);
        check(u"peer_reopened"_s, display->active());
        DWORD pid = 0;
        ::GetWindowThreadProcessId(display->handle(), &pid);
        // This exact peer is the medium-token process this probe created.
        // Never terminate an installed UIAccess helper or the user's instance.
        const HANDLE process = pid && !display->uiAccess() ? ::OpenProcess(PROCESS_TERMINATE, FALSE, pid) : nullptr;
        check(u"probe_peer_terminated"_s, process && ::TerminateProcess(process, 0));
        if (process) ::CloseHandle(process);
        pump(500);
        check(u"peer_loss_detected"_s, !display->ready() && !display->active());
        for (QWindow* window : QGuiApplication::topLevelWindows()) {
            auto* clipboard = qobject_cast<QQuickWindow*>(window);
            if (!clipboard || clipboard->property("displayController").value<QObject*>() != display) continue;
            DWORD cloaked = 0;
            const HWND fallback = reinterpret_cast<HWND>(clipboard->winId());
            ::DwmGetWindowAttribute(fallback, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
            check(u"ordinary_fallback_shown"_s, clipboard->isVisible() && ::IsWindowVisible(fallback) && !cloaked);
            check(u"fallback_passive"_s, (::GetWindowLongPtrW(fallback, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0);
            const auto* model = qobject_cast<Clipboard*>(clipboard->property("clipboard").value<QObject*>());
            check(u"fallback_target_and_routing_kept"_s, model && model->target() == target && model->keysRouted());
        }
        ::PostMessageW(messages, WM_HOTKEY, 3, 0);
        pump(150);
        report.insert(u"display_output"_s, report.value(u"display_output"_s).toString() + display->probeOutput());
    }
    return passed;
}

// Exercise the real App -> plugin host -> capture process path, with keyboard
// input confined to an owned foreground fixture and the plugin's own overlay.
bool probeCaptureHotkeys(App& app, const QStringList& args, const QDir& out, QJsonObject& report)
{
    QJsonObject checks;
    bool passed = true;
    const auto check = [&](bool ok, const QString& name) {
        checks.insert(name, ok);
        report.insert(u"capture_hotkey_checks"_s, checks);
        if (!ok) { qWarning().noquote() << "capture hotkeys:" << name; passed = false; }
        return ok;
    };
    const auto wait = [&](const auto& condition) {
        QElapsedTimer timer; timer.start();
        while (!condition() && timer.elapsed() < 8000) pump(20);
        return condition();
    };
    app.showSettings();
    pump(100);
    SettingsEditor* editor = nullptr;
    for (auto* window : QGuiApplication::allWindows()) {
        if (auto* found = window->property("editor").value<SettingsEditor*>()) {
            editor = found;
            window->hide();
            break;
        }
    }
    if (!check(editor != nullptr, u"settings_model"_s)) return false;
    editor->plugins()->setDeveloperMode(true);
    const HWND previous = ::GetForegroundWindow();
    const HWND fixture = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"WinShun issue 183 keyboard fixture",
        WS_OVERLAPPEDWINDOW, 80, 80, 640, 400, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!check(fixture != nullptr, u"owned_foreground_fixture"_s)) return false;
    ::ShowWindow(fixture, SW_SHOWMAXIMIZED);
    const auto cleanup = qScopeGuard([&] {
        ::DestroyWindow(fixture);
        if (previous && ::IsWindow(previous)) ::SetForegroundWindow(previous);
    });
    const auto captureWindow = [&]() -> HWND {
        HWND found = nullptr;
        const QString root = QDir::toNativeSeparators(Settings::profile() + u"/plugins/winshun.capture/"_s);
        while ((found = ::FindWindowExW(HWND_MESSAGE, found, L"WinShun.Capture.Messages", nullptr))) {
            DWORD pid = 0; ::GetWindowThreadProcessId(found, &pid);
            win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
            wchar_t path[32768]; DWORD size = DWORD(std::size(path));
            if (process.valid() && ::QueryFullProcessImageNameW(process.get(), 0, path, &size)
                && QString::fromWCharArray(path, size).startsWith(root, Qt::CaseInsensitive)) return found;
        }
        return nullptr;
    };
    const auto windowOf = [&](bool settings) -> HWND {
        DWORD pid = 0; ::GetWindowThreadProcessId(captureWindow(), &pid);
        struct Find { DWORD pid; bool settings; HWND found = nullptr; } find {pid, settings};
        ::EnumWindows([](HWND window, LPARAM data) -> BOOL {
            auto& f = *reinterpret_cast<Find*>(data);
            DWORD owner = 0; ::GetWindowThreadProcessId(window, &owner);
            if (!f.pid || owner != f.pid || !::IsWindowVisible(window)) return TRUE;
            wchar_t text[256]; ::GetWindowTextW(window, text, int(std::size(text)));
            const QString title = QString::fromWCharArray(text);
            const bool isSettings = title.contains(u"截图设置") || title.contains(u"Screenshot settings", Qt::CaseInsensitive);
            if (f.settings ? isSettings : !isSettings && (title.startsWith(u"截图") || title.startsWith(u"Screenshot"))) {
                f.found = window; return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&find));
        return find.found;
    };
    const auto freeKey = [&](const QString& shortcut) {
        UINT modifiers = 0, vk = 0;
        if (!MessageWindow::parseHotkey(shortcut, &modifiers, &vk)) return false;
        if (!::RegisterHotKey(nullptr, 0x7119, modifiers, vk)) return false;
        ::UnregisterHotKey(nullptr, 0x7119);
        return true;
    };
    const auto keyEvent = [](WORD vk, bool up) {
        INPUT input {}; input.type = INPUT_KEYBOARD; input.ki.wVk = vk;
        input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        return ::SendInput(1, &input, sizeof input) == 1;
    };
    const auto fire = [&](const QString& shortcut, const QString& name) {
        ::SetForegroundWindow(fixture);
        pump(100);
        if (!check(::GetForegroundWindow() == fixture, name + u"_foreground")) return false;
        UINT modifiers = 0, vk = 0;
        MessageWindow::parseHotkey(shortcut, &modifiers, &vk);
        const std::pair<UINT, WORD> modifierKeys[] {{MOD_CONTROL, VK_CONTROL}, {MOD_ALT, VK_MENU},
            {MOD_SHIFT, VK_SHIFT}, {MOD_WIN, VK_LWIN}};
        for (const auto& [flag, key] : modifierKeys) if (modifiers & flag) keyEvent(key, false);
        const bool sent = keyEvent(WORD(vk), false);
        pump(30);
        keyEvent(WORD(vk), true);
        for (auto it = std::rbegin(modifierKeys); it != std::rend(modifierKeys); ++it)
            if (modifiers & it->first) keyEvent(it->second, true);
        if (!check(sent && wait([&] { return windowOf(false) != nullptr; }), name + u"_overlay")) return false;
        const HWND overlay = windowOf(false);
        check(wait([&] { return ::GetForegroundWindow() == overlay; }), name + u"_focus");
        if (::GetForegroundWindow() != overlay) return false;
        if (name == u"after_settings")
            // Capture windows exclude themselves from screen capture. This
            // records the fixture behind it, not proof of the overlay's pixels.
            QGuiApplication::primaryScreen()->grabWindow(WId(overlay)).save(out.filePath(u"foreground-fixture.png"_s));
        keyEvent(VK_ESCAPE, false); keyEvent(VK_ESCAPE, true);
        return check(wait([&] { return !windowOf(false); }), name + u"_cancel");
    };
    const QString original = valueAfter(args, u"--probe-hotkey", u"F1"_s);
    if (!check(wait([&] { return captureWindow() && !freeKey(original); }), u"registered_at_startup"_s)) return false;
    pump(500); // older plugins briefly expose an overlay while warming it
    if (!check(wait([&] { return !windowOf(false); }), u"idle_overlay_hidden"_s)) return false;
    const auto integrityOf = [](HANDLE process) -> DWORD {
        HANDLE raw = nullptr;
        if (!::OpenProcessToken(process, TOKEN_QUERY, &raw)) return 0;
        win32::UniqueHandle token(raw);
        alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[128]; DWORD size = 0;
        if (!::GetTokenInformation(token.get(), TokenIntegrityLevel, buffer, sizeof buffer, &size)) return 0;
        const PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
        return *::GetSidSubAuthority(sid, *::GetSidSubAuthorityCount(sid) - 1);
    };
    DWORD pid = 0; ::GetWindowThreadProcessId(captureWindow(), &pid);
    win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    report.insert(u"host_integrity_rid"_s, int(integrityOf(::GetCurrentProcess())));
    report.insert(u"capture_integrity_rid"_s, int(integrityOf(process.get())));
    if (!fire(original, u"before_settings"_s)) return false;
    editor->plugins()->showSettings(u"winshun.capture"_s);
    if (!check(wait([&] { return windowOf(true) != nullptr; }), u"open_capture_settings"_s)) return false;
    ::PostMessageW(windowOf(true), WM_CLOSE, 0, 0);
    check(wait([&] { return !windowOf(true); }), u"close_capture_settings"_s);
    if (!fire(original, u"after_settings"_s)) return false;
    editor->plugins()->setEnabled(u"winshun.capture"_s, false);
    check(wait([&] { return freeKey(original); }), u"disable_releases_shortcut"_s);
    check(wait([&] { return !captureWindow(); }), u"disabled_process_exits"_s);
    const QString rebound = u"Ctrl+Alt+Shift+F23"_s;
    if (!check(freeKey(rebound), u"rebound_shortcut_available"_s)) return false;
    {
        QSettings config(Settings::profile() + u"/plugins/winshun.capture.data/capture.ini"_s, QSettings::IniFormat);
        config.setValue(u"Capture/Hotkey"_s, rebound); config.sync();
    }
    editor->plugins()->setEnabled(u"winshun.capture"_s, true);
    check(wait([&] { return captureWindow() && !freeKey(rebound); }), u"reenable_registers_rebound"_s);
    check(freeKey(original), u"old_shortcut_released"_s);
    fire(rebound, u"rebound"_s);
    editor->plugins()->setEnabled(u"winshun.capture"_s, false);
    check(wait([&] { return freeKey(rebound); }), u"final_release"_s);
    report.insert(u"capture_hotkey_checks"_s, checks);
    report.insert(u"capture_hotkeys_passed"_s, passed);
    QFile pluginLog(Settings::profile() + u"/plugins.log"_s);
    if (pluginLog.open(QIODevice::ReadOnly)) {
        QFile copy(out.filePath(u"capture-hotkey-plugins.log"_s));
        if (copy.open(QIODevice::WriteOnly)) copy.write(pluginLog.readAll());
    }
    return passed;
}

// ---- --settings-reopen-check (runtime probe) ----------------------------------------
// The settings window is made once and kept (App::showSettings): closed, it is
// hidden and puts itself back as a window made anew would be
// (SettingsWindow.resetForReopen). Opened, left in a state, closed the way the
// title bar closes it (QWindow::close), opened again and looked at; then the
// ways in from elsewhere on the kept window, and --settings-opens more
// openings timed. Shown without activation, no input sent (StartOptions::settingsProbe).

QQuickWindow* productionSettingsWindow()
{
    for (QWindow* candidate : QGuiApplication::allWindows()) {
        if (candidate->property("editor").value<SettingsEditor*>())
            return qobject_cast<QQuickWindow*>(candidate);
    }
    return nullptr;
}

// A JavaScript expression in SettingsWindow.qml's own scope (its ids, functions).
QVariant evaluateIn(QQuickWindow* window, const QString& expression)
{
    QQmlExpression e(qmlContext(window), window, expression);
    QVariant value = e.evaluate();
    if (e.hasError()) {
        qWarning().noquote() << u"settings reopen: %1: %2"_s.arg(expression, e.error().toString());
        return {};
    }
    if (value.metaType() == QMetaType::fromType<QJSValue>())
        value = value.value<QJSValue>().toVariant();
    return value;
}

// Every Timer, animation or other object of the window that says it is
// running (Repeater and Loader items are reached through the visual tree).
QStringList runningInWindow(QQuickWindow* window)
{
    QSet<QObject*> seen;
    QStringList running;
    const std::function<void(QObject*)> walk = [&](QObject* object) {
        if (!object || seen.contains(object))
            return;
        seen.insert(object);
        const QMetaObject* meta = object->metaObject();
        const int index = meta->indexOfProperty("running");
        if (index >= 0 && meta->property(index).read(object).toBool()) {
            const QObject* owner = object->parent();
            running.push_back(u"%1 (in %2 %3)"_s.arg(QString::fromLatin1(meta->className()),
                owner ? QString::fromLatin1(owner->metaObject()->className()) : u"-"_s, owner ? owner->objectName() : QString()));
        }
        for (QObject* child : object->children())
            walk(child);
        if (auto* item = qobject_cast<QQuickItem*>(object)) {
            for (QQuickItem* child : item->childItems())
                walk(child);
        }
    };
    walk(window);
    walk(window->contentItem());
    return running;
}

// The GUI thread left alone (an event loop that sleeps, not pump()'s spin) for
// `ms`: how often it woke and how much CPU it took. Work done on a timer or
// for an animation (a kept window's) shows as tens of wakes a second.
QJsonObject guiThreadIdle(int ms)
{
    const auto cpuMs = [] {
        FILETIME created {}, exited {}, kernel {}, user {};
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
            return -1.0;
        const auto ticks = [](const FILETIME& t) { return (static_cast<quint64>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
    };
    int wakes = 0;
    const QMetaObject::Connection counting = QObject::connect(QAbstractEventDispatcher::instance(),
        &QAbstractEventDispatcher::awake, [&wakes] { ++wakes; });
    const double before = cpuMs();
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
    const double cpu = cpuMs() - before;
    QObject::disconnect(counting);
    return {{u"ms"_s, ms}, {u"wakes"_s, wakes}, {u"gui_cpu_ms"_s, cpu}};
}

bool probeSettingsReopen(App& app, const QStringList& args, const QDir& out, QJsonObject& report)
{
    QJsonObject checks;
    QJsonArray openings;
    bool passed = true;
    const auto check = [&](bool ok, const QString& key) {
        checks.insert(key, ok);
        if (!ok) {
            qWarning().noquote() << "settings reopen:" << key;
            passed = false;
        }
        return ok;
    };
    // Shown, then pumped until its first frame was drawn and what waited for it ran.
    const auto open = [&](const std::function<void()>& how, const QString& what,
                          const std::function<void(QQuickWindow*)>& drawn = {}) -> QQuickWindow* {
        const int before = app.settingsOpening().count;
        how();
        QElapsedTimer waited;
        waited.start();
        while (waited.elapsed() < 5000
            && (app.settingsOpening().count == before || app.settingsOpening().firstFrameMs < 0))
            pump(10);
        if (drawn && productionSettingsWindow())
            drawn(productionSettingsWindow());
        pump(150);
        const App::SettingsOpening& o = app.settingsOpening();
        openings.append(QJsonObject {{u"what"_s, what}, {u"reused"_s, o.reused}, {u"firstFrameMs"_s, o.firstFrameMs}});
        say(u"settings %1: %2, first frame after %3 ms"_s.arg(what, o.reused ? u"shown again"_s : u"made"_s).arg(o.firstFrameMs));
        return productionSettingsWindow();
    };
    const auto close = [&](QQuickWindow* window) {
        window->close(); // as the title bar's ✕, Alt+F4 or the taskbar's 关闭窗口
        pump(300);
    };
    const auto js = [&](QQuickWindow* window, const QString& expression) { return evaluateIn(window, expression); };

    const int warningsBefore = g_warnings; // a QML error in the reset is a warning
    const QJsonObject idleBefore = guiThreadIdle(3000); // the app with no settings window yet
    report.insert(u"gui_idle_before_first_open"_s, idleBefore);

    // The pages made after the first frame (SettingsWindow.fillTimer), with frames
    // kept coming meanwhile (the page nudged by a pixel every 16 ms, as hovering or
    // typing would redraw it): the longest wait for a frame, each page's making.
    // Then the same for a second with nothing being made, to compare.
    const auto watchFilling = [&](QQuickWindow* w) {
        // Made for the first frame: the page it opened on, and no other.
        const QString madeFirst = js(w, u"[0, 1, 2, 3, 4, 5, 6, 7, 8, 9].filter(i => pageItem(i) !== null).join(',')"_s).toString();
        report.insert(u"made_for_first_frame"_s, madeFirst);
        // --settings-first-open all: everything made before the first frame (as before
        // the pages were made when needed), to compare with.
        const bool allFirst = valueAfter(args, u"--settings-first-open") == u"all";
        if (!allFirst)
            check(madeFirst == js(w, u"String(currentPage)"_s).toString(), u"first_frame_made_one_page"_s);
        // After the first frame nothing more by itself (refreshAfterOpen): the
        // pages cost ~20 MB each. The search box taking the keyboard makes the
        // rest (fillInBackground); a letter typed (--settings-type-at) does too.
        pump(500);
        say(u"settings after the first frame: made %1, pointer on %2"_s.arg(
            js(w, u"[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10].filter(i => pageItem(i) !== null).join(',')"_s).toString(),
            js(w, u"String(navHovered)"_s).toString()));
        if (!allFirst)
            check(js(w, u"[0, 1, 2, 3, 4, 5, 6, 7, 8, 9].filter(i => pageItem(i) !== null).join(',')"_s).toString() == madeFirst,
                u"nothing_made_after_first_frame"_s);
        if (valueAfter(args, u"--settings-type-at").isEmpty())
            js(w, u"fillInBackground()"_s);
        // --settings-fill-interval 16: as fast as while the search box has the keyboard.
        if (const int interval = valueAfter(args, u"--settings-fill-interval").toInt(); interval > 0)
            js(w, u"fillTimer.interval = %1"_s.arg(interval));
        QElapsedTimer clock;
        clock.start();
        QMutex lock;
        QList<qint64> frames;
        const QMetaObject::Connection counting = QObject::connect(w, &QQuickWindow::frameSwapped, w, [&] {
            QMutexLocker locked(&lock);
            frames.append(clock.elapsed());
        }, Qt::DirectConnection); // emitted on the render thread
        QTimer nudge;
        nudge.setTimerType(Qt::PreciseTimer);
        nudge.setInterval(16);
        QObject::connect(&nudge, &QTimer::timeout, [&] { js(w, u"flick.contentY = flick.contentY > 0 ? 0 : 1"_s); });
        nudge.start();
        const auto longestGap = [&](qint64 from, qint64 to) {
            QMutexLocker locked(&lock);
            qint64 longest = 0;
            qint64 last = from;
            int count = 0;
            for (qint64 at : frames) {
                if (at < from || at > to)
                    continue;
                longest = std::max(longest, at - last);
                last = at;
                ++count;
            }
            return std::pair {std::max(longest, to - last), count};
        };
        // --settings-type-at N: a letter typed into the search box N ms after the
        // first frame. Nothing made at once for it: the filling goes as fast as it
        // can and the results come once everything is made (searchReady).
        QJsonObject typed;
        const QString typeAt = valueAfter(args, u"--settings-type-at");
        qint64 typedAt = -1, readyAt = -1, resultsAt = -1, typedBlock = 0, lastPoll = 0;
        bool nothingFoundMeanwhile = false;
        QTimer typing;
        typing.setSingleShot(true);
        QObject::connect(&typing, &QTimer::timeout, [&] {
            typed.insert(u"made_before"_s, js(w, u"[0, 1, 2, 3, 4, 5, 6, 7, 8, 9].filter(i => pageItem(i) !== null).length"_s).toInt());
            typedAt = clock.elapsed();
            QElapsedTimer took;
            took.start();
            // --settings-focus: the box given the keyboard first (Ctrl+F), then the letter.
            if (args.contains(u"--settings-focus"_s))
                js(w, u"searchBox.focusAll()"_s);
            js(w, u"searchBox.setText('a')"_s);
            typed.insert(u"took_ms"_s, took.elapsed()); // the keystroke's own work
            typed.insert(u"box_has_keyboard"_s, js(w, u"searchBox.editing"_s).toBool());
        });
        if (!typeAt.isEmpty())
            typing.start(typeAt.toInt());
        // Every 2 ms: when the search became ready and the results showed; the
        // GUI thread's longest block from the letter typed to its results.
        QTimer poll;
        poll.setTimerType(Qt::PreciseTimer);
        poll.setInterval(2);
        QObject::connect(&poll, &QTimer::timeout, [&] {
            const qint64 now = clock.elapsed();
            if (typedAt >= 0 && resultsAt < 0)
                typedBlock = std::max(typedBlock, now - std::max(lastPoll, typedAt));
            lastPoll = now;
            if (readyAt < 0 && w->property("searchReady").toBool())
                readyAt = now;
            if (typedAt >= 0 && resultsAt < 0) {
                if (w->property("searching").toBool())
                    resultsAt = now;
                else if (QObject* none = named(w, "noResults"); none && none->property("visible").toBool())
                    nothingFoundMeanwhile = true;
            }
        });
        poll.start();
        const auto loaded = [&] { return js(w, u"searchReady"_s).toBool(); };
        const auto done = [&] { return readyAt >= 0 && !typing.isActive() && (typeAt.isEmpty() || resultsAt >= 0); };
        qint64 took = 0;
        const qint64 block = longestBlock(6000, done, &took);
        if (readyAt >= 0)
            took = readyAt; // the filling's own end, not the typed letter's
        if (typedAt >= 0) {
            pump(150); // the frames after it
            poll.stop();
            const auto [typedGap, typedFrames] = longestGap(typedAt, std::max(typedAt, resultsAt) + 100);
            typed.insert(u"at_ms"_s, typedAt);
            typed.insert(u"results_after_ms"_s, resultsAt >= 0 ? resultsAt - typedAt : -1);
            // From App::showSettings called to the results shown (this clock starts
            // as the first frame is seen, within a few ms of it).
            typed.insert(u"open_to_results_ms"_s, resultsAt >= 0 ? app.settingsOpening().firstFrameMs + resultsAt : -1);
            typed.insert(u"gui_longest_block_ms"_s, typedBlock);
            typed.insert(u"longest_frame_gap_ms"_s, typedGap);
            typed.insert(u"frames"_s, typedFrames);
            typed.insert(u"nothing_found_meanwhile"_s, nothingFoundMeanwhile);
            typed.insert(u"found"_s, js(w, u"results().length"_s).toInt());
            js(w, u"searchBox.clear()"_s);
            pump(100);
            report.insert(u"typed"_s, typed);
            say(u"settings typed during filling: %1"_s.arg(QString::fromUtf8(QJsonDocument(typed).toJson(QJsonDocument::Compact))));
            check(typed.value(u"found"_s).toInt() > 0 && !nothingFoundMeanwhile && resultsAt >= 0, u"typed_search_finds"_s);
        }
        poll.stop();
        const auto [fillGap, fillFrames] = longestGap(0, took);
        const qint64 calmFrom = clock.elapsed();
        const qint64 calmBlock = longestBlock(1000, [] { return false; });
        const auto [calmGap, calmFrames] = longestGap(calmFrom, clock.elapsed());
        nudge.stop();
        QObject::disconnect(counting);
        js(w, u"flick.contentY = 0"_s);
        QJsonArray times;
        double slowest = 0;
        for (int i = 0; i < kSettingsPages; ++i) {
            const double ms = js(w, u"pageSlots[%1].loadMs"_s.arg(i)).toDouble();
            times.append(ms);
            if (i > 0)
                slowest = std::max(slowest, ms);
        }
        const QJsonObject filling {{u"all_made"_s, loaded()}, {u"took_ms"_s, took}, {u"page_ms"_s, times},
            {u"slowest_page_after_first_ms"_s, slowest}, {u"longest_frame_gap_ms"_s, fillGap}, {u"frames"_s, fillFrames},
            {u"gui_longest_block_ms"_s, block}, {u"calm_longest_frame_gap_ms"_s, calmGap}, {u"calm_frames"_s, calmFrames},
            {u"calm_gui_longest_block_ms"_s, calmBlock}};
        report.insert(u"filling"_s, filling);
        say(u"settings filling: %1"_s.arg(QString::fromUtf8(QJsonDocument(filling).toJson(QJsonDocument::Compact))));
        check(filling.value(u"all_made"_s).toBool(), u"pages_made_after_first_frame"_s);
    };
    // A way in from elsewhere, as App's own call it (App::showClipboardSettings …):
    // where it opens, given before the window is shown.
    const auto showAt = [&](const char* method, const QVariant& value = {}) {
        app.showSettings([method, value](QQuickWindow* w) {
            if (value.isValid())
                QMetaObject::invokeMethod(w, method, Q_ARG(QVariant, value));
            else
                QMetaObject::invokeMethod(w, method);
        });
    };
    // --settings-first-open clipboard: the first opening by such a way in, which makes that page only.
    const bool firstByLink = valueAfter(args, u"--settings-first-open") == u"clipboard";
    // No white flash: the page is made as the window is told to show
    // (SettingsWindow.onVisibleChanged, connected first), before the native window shows.
    QJsonObject shown;
    QMetaObject::Connection showing;
    QQuickWindow* window = open([&] {
        app.showSettings([&](QQuickWindow* w) {
            showing = QObject::connect(w, &QWindow::visibleChanged, w, [&, w](bool visible) {
                if (!visible || !shown.isEmpty())
                    return;
                shown.insert(u"native_window_visible"_s, IsWindowVisible(reinterpret_cast<HWND>(w->winId())) != FALSE);
                shown.insert(u"page_made"_s, js(w, u"pageItem(currentPage) !== null"_s).toBool());
            }, Qt::DirectConnection);
            if (firstByLink)
                QMetaObject::invokeMethod(w, "showClipboardPage");
            else if (valueAfter(args, u"--settings-first-open") == u"all") // see watchFilling
                QMetaObject::invokeMethod(w, "loadAllPages");
        });
    }, firstByLink ? u"first (clipboard page)"_s : u"first"_s, watchFilling);
    QObject::disconnect(showing);
    report.insert(u"when_shown"_s, shown);
    check(shown.value(u"page_made"_s).toBool() && shown.contains(u"native_window_visible"_s)
            && !shown.value(u"native_window_visible"_s).toBool(),
        u"page_made_before_the_native_window_shows"_s);
    if (firstByLink)
        check(window && js(window, u"currentPage === clipboardPage"_s).toBool(), u"first_opening_on_clipboard_page"_s);
    if (window) {
        // Whether this engine makes objects asynchronously at all (an incubation
        // controller: QQmlApplicationEngine's, the launcher's window, now hidden):
        // a page made again in the background, then thrown away.
        QQmlEngine* engine = qmlEngine(window);
        QJsonObject incubation {{u"controller"_s, engine && engine->incubationController() != nullptr}};
        int othersShown = 0;
        for (QWindow* other : QGuiApplication::allWindows())
            othersShown += other != window && other->isVisible();
        incubation.insert(u"other_windows_shown"_s, othersShown);
        QQmlExpression start(qmlContext(window), window, u"scopeSlot.content.incubateObject(scopeSlot, {}, Qt.Asynchronous)"_s);
        const QJSValue incubator = start.evaluate().value<QJSValue>();
        const int atOnce = incubator.property(u"status"_s).toInt(); // Component.Ready 1, Loading 2
        qint64 took = 0;
        const qint64 block = longestBlock(5000, [&] { return incubator.property(u"status"_s).toInt() != 2; }, &took);
        incubation.insert(u"status_at_once"_s, atOnce == 2 ? u"Loading"_s : atOnce == 1 ? u"Ready"_s : QString::number(atOnce));
        incubation.insert(u"status_after"_s, incubator.property(u"status"_s).toInt() == 1 ? u"Ready"_s : u"not ready"_s);
        incubation.insert(u"took_ms"_s, took);
        incubation.insert(u"gui_longest_block_ms"_s, block);
        delete incubator.property(u"object"_s).toQObject(); // at once, before anything else changes the window
        pump(50);
        report.insert(u"incubation"_s, incubation);
        say(u"settings incubation: %1"_s.arg(QString::fromUtf8(QJsonDocument(incubation).toJson(QJsonDocument::Compact))));
    }
    if (!check(window && window->isVisible(), u"opened"_s))
        return false;
    QPointer<QQuickWindow> made(window);
    const WId handle = window->winId();
    check(!app.settingsOpening().reused && app.settingsOpening().firstFrameMs >= 0, u"first_opening_made_and_drawn"_s);

    // Left in a state: another page, scrolled, a search typed, dialogs and a
    // form open, a card unfolded, a field half filled, a hotkey being recorded.
    js(window, u"showPage('results', 2)"_s);
    settle(window, 2);
    js(window, u"flick.contentY = 200"_s);
    js(window, u"searchBox.setText('主题')"_s);
    pump(100);
    js(window, u"updateDialog.show()"_s);
    js(window, u"pluginDialog.ask('uninstall', {id: 'zz.fixture', name: 'Fixture'})"_s);
    js(window, u"webEditing = -1"_s);
    js(window, u"part('scope', 'nameInput').text = '半截'"_s);
    js(window, u"part('clipboard', 'captureKeywordError').text = 'stale'"_s);
    js(window, u"part(pluginsPage, 'pluginsSection').expand('zz.fixture')"_s);
    js(window, u"editor.plugins.recordingHotkey = true"_s);
    pump(100);
    check(js(window, u"searching && beforeSearch !== null && updateDialog.open && pluginDialog.open && webDialog.active"_s).toBool(),
        u"state_before_close"_s);
    check(js(window, u"editor.recordingHotkey"_s).toBool(), u"recording_suspends_hotkeys_before_close"_s);

    close(window);
    check(made && !made->isVisible(), u"close_hides_and_keeps"_s);
    if (!made)
        return false;
    check(made->handle() && made->winId() == handle, u"close_keeps_native_window"_s);
    check(!js(made, u"editor.recordingHotkey || editor.plugins.recordingHotkey"_s).toBool(), u"recording_stopped_on_hide"_s);
    // What runs while hidden: animations that run to the end of their round
    // (some 1.2 s) are allowed that long.
    pump(1500);
    const QStringList running = runningInWindow(made);
    report.insert(u"running_while_hidden"_s, QJsonArray::fromStringList(running));
    check(running.isEmpty(), u"nothing_runs_while_hidden"_s);
    const QJsonObject idleHidden = guiThreadIdle(3000);
    report.insert(u"gui_idle_while_hidden"_s, idleHidden);
    // A timer or animation left running would wake it some 60 times a second.
    check(idleHidden.value(u"wakes"_s).toInt() <= idleBefore.value(u"wakes"_s).toInt() + 15, u"hidden_window_does_not_wake_the_app"_s);

    window = open([&] { app.showSettings(); }, u"again"_s);
    check(window == made && window->winId() == handle, u"reopened_same_window"_s);
    check(app.settingsOpening().reused && app.settingsOpening().firstFrameMs >= 0, u"reopening_reused_and_drawn"_s);
    check(js(window, u"currentPage === 0 && currentTab === 0 && lastTabs.length === 0"_s).toBool(), u"reopened_on_first_page"_s);
    check(js(window, u"flick.contentY === 0"_s).toBool(), u"reopened_at_top"_s);
    check(js(window, u"searchBox.query === '' && !searching && beforeSearch === null && settingsSearch.current === null"_s).toBool(),
        u"reopened_search_empty"_s);
    check(js(window, u"!updateDialog.open && !pluginDialog.open && !webDialog.active && webEditing === -2"_s).toBool(),
        u"reopened_no_dialogs"_s);
    check(js(window, u"part('scope', 'nameInput').text === '' && part('clipboard', 'captureKeywordError').text === ''"
                     u" && part(pluginsPage, 'pluginsSection').expandedIds.length === 0"_s).toBool(),
        u"reopened_fields_cleared"_s);
    check(!js(window, u"editor.recordingHotkey || editor.plugins.recordingHotkey"_s).toBool(), u"reopened_not_recording"_s);

    // The ways in from elsewhere, on the kept window: each shows it again, at its place.
    close(window);
    window = open([&] { showAt("showClipboardPage"); }, u"clipboard page"_s);
    check(js(window, u"currentPage === clipboardPage"_s).toBool(), u"deep_link_clipboard_page"_s);

    close(window);
    window = open([&] { showAt("showPlace", u"results,2"_s); }, u"place results,2"_s);
    check(js(window, u"currentPage === resultsPage && currentTab === 2"_s).toBool(), u"deep_link_place"_s);

    close(window);
    const QString key = u"Launcher/Autostart"_s;
    const aisettings::Entry* entry = aisettings::find(key);
    const aisettings::Section* section = entry ? aisettings::sectionOf(entry->section) : nullptr;
    window = open([&] { app.showSettingsFor(key); }, u"setting "_s + key);
    pump(300); // showSetting finds the row after its search (Qt.callLater)
    check(section && js(window, u"pages[currentPage].id"_s).toString() == section->page
            && (entry->tab < 0 || js(window, u"currentTab"_s).toInt() == entry->tab)
            && js(window, u"!searching"_s).toBool(),
        u"deep_link_setting"_s);

    close(window);
    QString pluginId;
    if (auto* list = qobject_cast<QAbstractItemModel*>(window->property("editor").value<SettingsEditor*>()->plugins()->property("list").value<QObject*>());
        list && list->rowCount() > 0)
        pluginId = list->data(list->index(0, 0), list->roleNames().key("pluginId")).toString();
    if (!pluginId.isEmpty()) {
        window = open([&] { showAt("showPluginPage", pluginId); }, u"plugin "_s + pluginId);
        check(js(window, u"currentPage === pluginsPage && currentTab === 0 && part(pluginsPage, 'pluginsSection').expandedIds.indexOf('%1') >= 0"_s.arg(pluginId)).toBool(),
            u"deep_link_plugin_card"_s);
        close(window);
    } else {
        report.insert(u"deep_link_plugin_card"_s, u"skipped: no plugin installed"_s);
    }

    window = open([&] { app.showUpdate(); }, u"update dialog"_s);
    check(js(window, u"updateDialog.open && currentPage === generalPage"_s).toBool(), u"deep_link_update_dialog"_s);
    close(window);

    // Minimized, then closed (the taskbar's 关闭窗口): opens normal again.
    window = open([&] { app.showSettings(); }, u"before minimizing"_s);
    window->setWindowStates(Qt::WindowMinimized);
    pump(300);
    close(window);
    window = open([&] { app.showSettings(); }, u"after minimized"_s);
    check(!(window->windowStates() & Qt::WindowMinimized) && window->isVisible(), u"reopened_not_minimized"_s);
    close(window);

    // Closed a while: the pages made for that opening are put away, all but
    // the first (SettingsWindow.unloadPages, GitHub #459), and what they held
    // goes back; the next opening is as any, each page made again when gone to.
    {
        const auto commitMb = [] {
            PROCESS_MEMORY_COUNTERS counters {sizeof counters};
            GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters);
            return static_cast<double>(counters.PagefileUsage) / (1024.0 * 1024.0);
        };
        const QString others = u"[1, 2, 3, 4, 5, 6, 7, 8, 9, 10].filter(i => pageItem(i) !== null).length"_s;
        window = open([&] { app.showSettings(); }, u"all pages"_s);
        js(window, u"loadAllPages()"_s);
        pump(1500); // drawn, and what the pages set going has ended
        check(js(window, others).toInt() == 10 && js(window, u"searchReady"_s).toBool(), u"unload_all_pages_were_made"_s);
        const double withPages = commitMb();
        app.setSettingsUnloadAfter(400);
        close(window);
        check(js(window, others).toInt() == 10, u"unload_not_at_once"_s); // opened again soon: as it was left
        guiThreadIdle(2500); // in an event loop that runs: destroy() is a deleteLater, which pump() never carries out
        const double after = commitMb();
        check(js(window, others).toInt() == 0 && js(window, u"pageItem(0) !== null && !allPagesLoaded && !searchReady"_s).toBool(),
            u"unload_pages_put_away"_s);
        check(runningInWindow(window).isEmpty(), u"unload_leaves_nothing_running"_s);
        report.insert(u"unload_commit_mb"_s, QJsonObject {{u"with_pages"_s, std::round(withPages)}, {u"after"_s, std::round(after)}});
        say(u"settings pages put away: commit with all pages %1 MB, after %2 MB"_s.arg(std::round(withPages)).arg(std::round(after)));
        // Of the ~100 MB of heap blocks the ten pages held (650,000 of them), most
        // stays the heaps' own, free for what comes next and out of memory once
        // it was given back; ~38 MB go back to Windows at once (measured at 150%).
        check(withPages - after > 20, u"unload_gives_memory_back"_s);

        window = open([&] { app.showSettings(); }, u"after pages were put away"_s);
        check(app.settingsOpening().reused && js(window, u"currentPage === 0 && pageItem(0) !== null"_s).toBool()
                && js(window, others).toInt() == 0,
            u"unload_reopened_on_first_page"_s);
        js(window, u"showPage(keysPage, keysListTab)"_s); // 按键一览, its sections made anew
        pump(500);
        check(js(window, u"currentPage === keysPage && pageItem(keysPage) !== null && shortcutModel.count === shortcutSections.length"_s).toBool(),
            u"unload_page_made_again"_s);
        js(window, u"loadAllPages()"_s);
        check(js(window, u"searchReady"_s).toBool() && js(window, others).toInt() == 10, u"unload_search_ready_again"_s);

        // Left open: the pages left behind go too, all but the one on show
        // (SettingsWindow.putPagesAway, GitHub #467), and one gone back to is
        // made again where it was left. Not while the search has them.
        const QString madeNow = u"[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10].filter(i => pageItem(i) !== null).join(',')"_s;
        // What Task Manager's 内存 follows: the freed pages' memory is given back
        // at once though the window is in use (App, IdleTrim::freed).
        const auto workingSetMb = [] {
            PROCESS_MEMORY_COUNTERS counters {sizeof counters};
            GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters);
            return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
        };
        const double openWithPages = workingSetMb();
        js(window, u"leftPagesMs = 300"_s);
        js(window, u"showPage(scopePage)"_s);
        pump(300);
        const int leftAt = js(window, u"flick.contentY = Math.min(120, Math.max(0, flick.contentHeight - flick.height)); flick.contentY"_s).toInt();
        guiThreadIdle(4000); // put away one per turn, then given back a second later, on a thread
        // The page under the real pointer is kept (hoverNav): said, to tell that from a page left by mistake.
        say(u"settings pages left: made %1, pointer on %2"_s.arg(js(window, madeNow).toString(), js(window, u"String(navHovered)"_s).toString()));
        check(js(window, madeNow).toString() == js(window, u"String(scopePage)"_s).toString()
                && js(window, u"visible && !allPagesLoaded && !searchReady"_s).toBool(),
            u"left_pages_put_away"_s);
        const double openAfter = workingSetMb();
        report.insert(u"left_pages_working_set_mb"_s, QJsonObject {{u"with_pages"_s, std::round(openWithPages)}, {u"after"_s, std::round(openAfter)}});
        say(u"settings pages left behind put away: working set with all pages %1 MB, after %2 MB"_s.arg(std::round(openWithPages)).arg(std::round(openAfter)));
        check(openWithPages - openAfter > 50, u"left_pages_give_memory_back"_s);
        js(window, u"showPage(resultsPage)"_s);
        guiThreadIdle(1500);
        check(js(window, madeNow).toString() == js(window, u"String(resultsPage)"_s).toString(), u"left_pages_follow_the_page_on_show"_s);
        js(window, u"navigateBack()"_s);
        pump(400);
        check(js(window, u"currentPage === scopePage && pageItem(scopePage) !== null"_s).toBool()
                && js(window, u"Math.round(flick.contentY)"_s).toInt() == leftAt,
            u"left_page_made_again_where_it_was"_s);
        js(window, u"searchBox.setText('a')"_s); // the search makes them all, and keeps them while it is on
        QElapsedTimer filling;
        filling.start();
        while (filling.elapsed() < 5000 && !js(window, u"searchReady"_s).toBool())
            pump(20);
        guiThreadIdle(1500);
        check(js(window, u"searchReady"_s).toBool() && js(window, others).toInt() == 10, u"left_pages_kept_for_the_search"_s);
        js(window, u"searchBox.clear()"_s);
        guiThreadIdle(2500);
        check(js(window, madeNow).toString() == js(window, u"String(currentPage)"_s).toString(), u"left_pages_put_away_after_the_search"_s);
        js(window, u"leftPagesMs = 30000"_s);

        app.setSettingsUnloadAfter(60'000); // not during what follows
        close(window);
    }

    // More openings, for their times.
    const int more = std::clamp(valueAfter(args, u"--settings-opens", u"5"_s).toInt(), 0, 100);
    for (int i = 0; i < more; ++i) {
        window = open([&] { app.showSettings(); }, u"timed"_s);
        close(window);
        pump(200);
    }
    check(made && made->winId() == handle, u"one_native_window_throughout"_s);
    report.insert(u"warnings"_s, g_warnings - warningsBefore);
    check(g_warnings == warningsBefore, u"no_warnings"_s);

    report.insert(u"checks"_s, checks);
    report.insert(u"openings"_s, openings);
    report.insert(u"passed"_s, passed);
    QFile json(out.filePath(u"settings-reopen.json"_s));
    if (json.open(QIODevice::WriteOnly))
        json.write(QJsonDocument(report).toJson());
    return passed;
}

// --probe-runtime <folder> --live: the production App on the real desktop, at
// the screen's own scale, beside the Win顺 that runs (this one never took the
// instance mutex), until <folder>\stop appears. Lines in <folder>\cmd.txt
// (taken and deleted as they come) open its windows for a look at them:
//   show [text] | hide | clip | settings [place] | style <fluent|brand|tactile>
//   | theme <light|dark|system> | set <SettingsEditor property> <value>
//   | call <SettingsEditor method> (one without arguments: exportDiagnostics)
//   | windows (their places into windows.json) | press <objectName> (its clicked())
//   | profile (the profile folder into profile.txt)
// Each batch done is counted in <folder>\ack.txt.
int runLive(App& app, const QDir& out)
{
    // Synthetic colours behind the real HWND make native blur/fallback visible.
    // grabWindow() alone cannot capture DWM's backdrop or prove inactive Acrylic.
    QQmlEngine backdropEngine;
    std::unique_ptr<QObject> backdropFixture;
    std::unique_ptr<QQuickWindow> backdropFocus;
    if (QCoreApplication::arguments().contains(u"--backdrop-check"_s)) {
        QQmlComponent fixture(&backdropEngine);
        fixture.setData(R"(
            import QtQuick
            Window {
                title: "WinShun backdrop fixture"
                width: 1200; height: 850; visible: true
                Rectangle {
                    anchors.fill: parent
                    gradient: Gradient {
                        GradientStop { position: 0; color: "#E56138" }
                        GradientStop { position: 0.5; color: "#3257CF" }
                        GradientStop { position: 1; color: "#44BBA4" }
                    }
                }
            }
        )", QUrl());
        backdropFixture.reset(fixture.create());
        if (!backdropFixture) return 4;
        if (auto* background = qobject_cast<QWindow*>(backdropFixture.get())) {
            // STARTUPINFO may hide the process's first window, even with visible:true.
            background->hide();
            background->show();
        }
        backdropFocus = std::make_unique<QQuickWindow>();
        backdropFocus->setTitle(u"WinShun backdrop focus"_s);
        backdropFocus->setFlags(Qt::Window);
        backdropFocus->setGeometry(20, 60, 160, 100);
        backdropFocus->setColor(Qt::white);
        backdropFocus->show();
    }
    // A second start of the user's own copy (Explorer's verbs, --query) finds
    // the first message window in z order: never this one.
    for (HWND candidate = nullptr; (candidate = ::FindWindowExW(nullptr, candidate, L"WinShun.MessageWindow", nullptr));) {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(candidate, &pid);
        if (pid == ::GetCurrentProcessId())
            ::SetWindowPos(candidate, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    const auto editor = [&app]() -> SettingsEditor* {
        for (QWindow* window : QGuiApplication::allWindows()) {
            if (auto* found = window->property("editor").value<SettingsEditor*>())
                return found;
        }
        app.handleCommand(settingscommand::make({})); // made once, then kept
        pump(300);
        for (QWindow* window : QGuiApplication::allWindows()) {
            if (auto* found = window->property("editor").value<SettingsEditor*>())
                return found;
        }
        return nullptr;
    };
    // pump() runs outside exec(): destroy()/deleteLater() would never happen
    // there, and a style switch would leave every part of the style before
    // under the new one. The app's own loop deletes them; so does this.
    const auto settle = [](int ms) {
        pump(ms);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    };
    int batches = 0;
    while (!QFile::exists(out.filePath(u"stop"_s))) {
        settle(100);
        QFile commands(out.filePath(u"cmd.txt"_s));
        if (!commands.exists() || !commands.open(QIODevice::ReadOnly))
            continue;
        const QStringList lines = QString::fromUtf8(commands.readAll()).split(u'\n', Qt::SkipEmptyParts);
        commands.close();
        commands.remove();
        for (const QString& raw : lines) {
            const QString line = raw.trimmed();
            const QString verb = line.section(u' ', 0, 0);
            const QString rest = line.section(u' ', 1);
            if (verb == u"show")
                app.showLauncher(rest);
            else if (verb == u"hide")
                app.hideLauncher();
            else if (verb == u"clip")
                app.showClipboard();
            else if (verb == u"dismiss") { // the clipboard's Esc: it never takes the focus, so no key can be sent to it
                for (QWindow* window : QGuiApplication::allWindows()) {
                    if (auto* clipboard = window->property("clipboard").value<QObject*>())
                        QMetaObject::invokeMethod(clipboard, "dismiss");
                }
            }
            else if (verb == u"settings")
                app.handleCommand(settingscommand::make(rest));
            else if (verb == u"style" || verb == u"theme") {
                if (SettingsEditor* found = editor())
                    found->setProperty(verb == u"style" ? "uiStyle" : "theme", rest);
            } else if (verb == u"set") { // set <SettingsEditor property> <value>
                if (SettingsEditor* found = editor())
                    found->setProperty(rest.section(u' ', 0, 0).toLatin1().constData(), rest.section(u' ', 1));
            } else if (verb == u"call") { // call <SettingsEditor method without arguments>: what its button does
                // The diagnostics journal is off in a probe: for its export, kept
                // in <folder>\journal, where the package is found afterwards.
                if (rest == u"exportDiagnostics" && !diagnostics::current())
                    diagnostics::initialize(out.filePath(u"journal"_s));
                if (SettingsEditor* found = editor())
                    QMetaObject::invokeMethod(found, rest.toLatin1().constData());
            } else if (verb == u"windows") {
                QJsonArray list;
                for (QWindow* window : QGuiApplication::topLevelWindows()) {
                    if (!window->isVisible())
                        continue;
                    const QRect r = window->geometry();
                    list.append(QJsonObject {{u"title"_s, window->title()}, {u"object"_s, window->objectName()},
                        {u"class"_s, QString::fromLatin1(window->metaObject()->className())},
                        {u"hwnd"_s, QString::number(window->winId())}, {u"dpr"_s, window->devicePixelRatio()},
                        {u"active"_s, window->isActive()},
                        {u"foreground"_s, ::GetForegroundWindow() == reinterpret_cast<HWND>(window->winId())},
                        {u"color"_s, qobject_cast<QQuickWindow*>(window) ? qobject_cast<QQuickWindow*>(window)->color().name(QColor::HexArgb) : QString()},
                        {u"x"_s, r.x()}, {u"y"_s, r.y()}, {u"w"_s, r.width()}, {u"h"_s, r.height()}});
                }
                QFile json(out.filePath(u"windows.json"_s));
                if (json.open(QIODevice::WriteOnly))
                    json.write(QJsonDocument(list).toJson());
            } else if (verb == u"profile") { // profile.txt: this run's profile folder (its state.ini, between two runs)
                QFile text(out.filePath(u"profile.txt"_s));
                if (text.open(QIODevice::WriteOnly))
                    text.write(QDir::toNativeSeparators(Settings::profile()).toUtf8());
            } else if (verb == u"press") { // press <objectName>: its clicked(), as a click would, to look at what it opens
                // By the items on show (a list's rows are not QObject children of the list).
                const std::function<QQuickItem*(QQuickItem*)> find = [&](QQuickItem* item) -> QQuickItem* {
                    if (item->objectName() == rest && item->isVisible())
                        return item;
                    for (QQuickItem* child : item->childItems())
                        if (QQuickItem* found = find(child))
                            return found;
                    return nullptr;
                };
                for (QWindow* window : QGuiApplication::topLevelWindows()) {
                    auto* quick = qobject_cast<QQuickWindow*>(window);
                    QQuickItem* found = quick && window->isVisible() ? find(quick->contentItem()) : nullptr;
                    if (found && QMetaObject::invokeMethod(found, "clicked"))
                        break;
                }
            } else if (verb == u"parts") { // parts.txt: every style part alive in the visible windows
                QStringList found;
                const std::function<void(QQuickItem*, int)> walk = [&](QQuickItem* item, int depth) {
                    const QQmlContext* context = qmlContext(item);
                    const QString url = context ? context->baseUrl().toString() : QString();
                    const qsizetype at = url.indexOf(u"/themes/"_s);
                    if (at >= 0 && depth < 400) {
                        const QQuickItem* host = item->parentItem();
                        const QQmlContext* hostContext = host ? qmlContext(host) : nullptr;
                        found << u"%1 visible=%2 %3x%4 in %5 %6"_s.arg(url.mid(at + 8)).arg(item->isVisible())
                                     .arg(item->width()).arg(item->height())
                                     .arg(host ? QString::fromLatin1(host->metaObject()->className()) : QString())
                                     .arg(hostContext ? hostContext->baseUrl().fileName() : QString());
                        return; // the part's own insides are its business
                    }
                    for (QQuickItem* child : item->childItems())
                        walk(child, depth + 1);
                };
                for (QWindow* window : QGuiApplication::topLevelWindows()) {
                    auto* quick = qobject_cast<QQuickWindow*>(window);
                    if (!quick || !window->isVisible())
                        continue;
                    found << u"== %1"_s.arg(window->title());
                    walk(quick->contentItem(), 0);
                }
                QFile text(out.filePath(u"parts.txt"_s));
                if (text.open(QIODevice::WriteOnly))
                    text.write(found.join(u'\n').toUtf8());
            }
            settle(50);
        }
        QFile ack(out.filePath(u"ack.txt"_s));
        if (ack.open(QIODevice::WriteOnly))
            ack.write(QByteArray::number(++batches));
    }
    return 0;
}

int runRuntime(const QStringList& args)
{
    const QDir out(valueAfter(args, kRuntimeFlag));
    const QString probeHotkey = valueAfter(args, u"--probe-hotkey", u"Ctrl+Alt+Shift+F24"_s);
    // Redirect this process's HKCU writes too: profiles alone do not isolate
    // shell verbs and Win+V/Win+S ownership. Child plugins use their own data
    // folders; the capture fixture below never takes PrtScn.
    const std::wstring registryPath = (u"Software\\WinShun\\RuntimeTests\\"_s + QUuid::createUuid().toString(QUuid::WithoutBraces)).toStdWString();
    HKEY realUser = nullptr;
    HKEY sandbox = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"", 0, KEY_ALL_ACCESS, &realUser) != ERROR_SUCCESS)
        return 4;
    if (RegCreateKeyExW(realUser, registryPath.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &sandbox, nullptr) != ERROR_SUCCESS) {
        RegCloseKey(realUser);
        return 4;
    }
    const auto registryCleanup = qScopeGuard([&] {
        RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        RegCloseKey(sandbox);
        RegDeleteTreeW(realUser, registryPath.c_str());
        RegCloseKey(realUser);
    });
    if (RegOverridePredefKey(HKEY_CURRENT_USER, sandbox) != ERROR_SUCCESS)
        return 4;
    for (const wchar_t* path : {L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
             L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced"}) {
        HKEY key = nullptr;
        if (RegCreateKeyExW(sandbox, path, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return 4;
        RegCloseKey(key);
    }
    Settings settings = Settings::defaults();
    const QString onboardingAction = valueAfter(args, u"--onboarding-probe");
    settings.onboardingComplete = onboardingAction.isEmpty() || onboardingAction == u"legacy";
    settings.uiStyle = SystemTheme::uiStyle(); // --style, set in run()
    settings.language = args.contains(u"--english"_s) ? u"en"_s : u"zh"_s;
    if (args.contains(u"--dark"_s))
        settings.theme = u"dark"_s;
    if (!onboardingAction.isEmpty())
        settings.transparency = u"off"_s;
    if (args.contains(u"--ocr-probe"_s)) {
        settings.theme = args.contains(u"--dark"_s) ? u"dark"_s : u"light"_s;
        settings.transparency = u"off"_s; // grabWindow cannot capture the native Mica behind a transparent surface
    }
    settings.doubleCtrl = false;
    settings.hotkey.clear();
    settings.dialogJump = false;
    settings.dialogBar = args.contains(u"--file-dialog-window"_s);
    settings.dialogAutoJumpAsked = true;
    settings.dialogBarPlace = u"below"_s;
    settings.dialogAutoJump = false;
    settings.explorerMenu = false;
    settings.explorerPinPicture = false;
    settings.uriProtocol = false;
    settings.explorerTyping = false;
    settings.opusTyping = false;
    settings.folderMenu = u"off"_s;
    settings.folderMenuHotkey.clear();
    settings.taskbarSearchBox = false;
    settings.autoUpdate = false;
    settings.usageReport = false;
    settings.proxyMode = u"none"_s; // loopback/file fixtures never use the user's proxy
    settings.mcp = false;
    settings.clipboard = args.contains(u"--with-clipboard"_s);
    if (args.contains(u"--clipboard-shell-probe"_s)) {
        settings.clipboard = true;
        settings.clipboardImages = false;
        settings.clipboardHoldToStep = false;
    }
    settings.clipboardWinV = false;
    settings.clipboardHotkey.clear();
    settings.pluginConfig(u"winshun.switches"_s).enabled = false; // optional unless --runtime-plugins installs it
    settings.indexAccess = u"walk"_s;
    settings.indexScope = u"folders"_s;
    settings.selectedDrives = QStringList {};
    settings.indexFoldersChosen = true;
    settings.rescanOnStartup = false;
    settings.indexFolders = {Settings::profile() + u"/fixture"_s};
    std::unique_ptr<QTemporaryDir> dialogFiles;
    const bool typingProbe = args.contains(u"--folder-typing"_s);
    if (settings.dialogBar || typingProbe) {
        // App's data directory is always excluded. Keep the searchable
        // fixture in a separate owned folder, outside the private profile.
        dialogFiles = std::make_unique<QTemporaryDir>(QDir::temp().filePath(u"WinShun-dialog-files-XXXXXX"_s));
        if (!dialogFiles->isValid())
            return 4;
        settings.indexFolders = {dialogFiles->path()};
        settings.selectedDrives = QStringList {QDir::toNativeSeparators(QStorageInfo(dialogFiles->path()).rootPath())};
        settings.excludedPaths = {Settings::profile() + u"/excluded"_s}; // an empty list reloads the TEMP-excluding defaults
    }
    // --idle-open-index: folders with enough in them for an index of a real size.
    if (const QString folders = valueAfter(args, u"--idle-open-index"); !folders.isEmpty()) {
        settings.indexFolders = folders.split(u';', Qt::SkipEmptyParts);
        QStringList drives;
        for (const QString& folder : std::as_const(settings.indexFolders)) {
            const QString drive = QDir::toNativeSeparators(QStorageInfo(folder).rootPath());
            if (!drives.contains(drive, Qt::CaseInsensitive))
                drives.push_back(drive);
        }
        settings.selectedDrives = drives;
    }
    QDir().mkpath(settings.indexFolders.front());
    if (typingProbe) {
        settings.opusTypingScope = args.contains(u"--opus-everywhere"_s) ? u"all"_s : u"folder"_s;
        settings.save();
        const QDir files(settings.indexFolders.front());
        for (int i = 1; i <= 3; ++i) {
            QFile file(files.filePath(u"follow_%1.txt"_s.arg(i)));
            if (!file.open(QIODevice::WriteOnly) || file.write("isolated Explorer selection fixture\n") < 0) return 4;
        }
        for (int i = 1; i <= kFolderTypingMany; ++i) { // more rows than the box shows (GitHub #246)
            QFile file(files.filePath(u"many_%1.txt"_s.arg(i, 2, 10, QChar(u'0'))));
            if (!file.open(QIODevice::WriteOnly)) return 4;
        }
        files.mkdir(u"child"_s);
        QFile nested(files.filePath(u"child/nested-only.txt"_s));
        if (!nested.open(QIODevice::WriteOnly)) return 4;
        // Found only outside child\, and only one of them a document (GitHub #366).
        for (const QString& name : {u"type-elsewhere.docx"_s, u"type-elsewhere.png"_s}) {
            QFile file(files.filePath(name));
            if (!file.open(QIODevice::WriteOnly)) return 4;
        }
    }
    // Not into a folder of the user's (--idle-open-index).
    QDir().mkpath(Settings::profile());
    QFile fixture(args.contains(u"--idle-open-index"_s) ? Settings::profile() + u"/startup-probe.txt"_s
                                                        : settings.indexFolders.front() + u"/startup-probe.txt"_s);
    if (!fixture.open(QIODevice::WriteOnly))
        return 4;
    fixture.write("WinShun isolated startup probe\n");
    fixture.close();
    // What the bar is to find and give the window: a file copied here, or,
    // with no sample (a window that takes folders: a path box, a folder
    // picker), a folder made here.
    const bool dialogFolder = settings.dialogBar && !args.contains(u"--file-dialog-sample"_s);
    const QString dialogSample = QDir(settings.indexFolders.front()).filePath(
        dialogFolder ? u"WinShun-dialog-验证 文件夹"_s : u"WinShun-dialog-验证.blend"_s);
    if (settings.dialogBar
        && !(dialogFolder ? QDir().mkpath(dialogSample) : QFile::copy(valueAfter(args, u"--file-dialog-sample"), dialogSample)))
        return 4;
    if (args.contains(u"--file-dialog-typing"_s)) { // something to search through
        const QDir files(settings.indexFolders.front());
        for (int i = 0; i < 6000; ++i) {
            const QString group = u"group %1"_s.arg(i / 200, 2, 10, QLatin1Char('0'));
            if (i % 200 == 0 && !files.mkpath(group))
                return 4;
            QFile file(files.filePath(group + (i % 3 ? u"/report %1 季度.xlsx"_s : u"/notes %1 会议.docx"_s).arg(i, 4, 10, QLatin1Char('0'))));
            if (!file.open(QIODevice::WriteOnly))
                return 4;
        }
    }

    const QDir plugins(valueAfter(args, u"--runtime-plugins"));
    if (args.contains(u"--runtime-plugins"_s)) {
        for (const QFileInfo& folder : plugins.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            auto candidate = plugin::install::inspect(folder.absoluteFilePath());
            if (!candidate || !candidate->manifest)
                return 4;
            const QString id = candidate->manifest->id;
            QString error;
            if (plugin::install::place(*candidate, Settings::profile() + u"/plugins"_s, &error).isEmpty()) {
                qWarning("runtime probe install: %s", qPrintable(error));
                return 4;
            }
            PluginConfig& config = settings.pluginConfig(id);
            config.enabled = true;
            config.confirmed = candidate->manifest->permissions.names();
            if (id == u"winshun.localsend")
                config.settings.append({u"receive"_s, u"false"_s});
            if (id == u"winshun.capture") {
                QSettings capture(Settings::profile() + u"/plugins/winshun.capture.data/capture.ini"_s, QSettings::IniFormat);
                capture.setValue(u"Capture/Hotkey"_s, probeHotkey);
                capture.setValue(u"Capture/PrintScreen"_s, false);
                if (args.contains(u"--live"_s)) // the presenter too, for a look at it; a key nobody has
                    capture.setValue(u"Capture/PresentHotkey"_s, u"Ctrl+Alt+Shift+F23"_s);
            }
        }
    }
    if (args.contains(u"--web-plugin-check"_s)) {
        settings.theme = args.contains(u"--dark"_s) ? u"dark"_s : u"light"_s;
        settings.transparency = u"off"_s; // A grabWindow image cannot capture the desktop's Mica material.
        settings.webShortcuts = {{u"wsweb"_s, u"网页测试"_s, u"https://example.invalid/search?q=%s"_s, u"http://127.0.0.1:9/"_s}};
        settings.webShortcuts[0].suggest = valueAfter(args, u"--web-suggest-url");
        settings.webSuggestions = !settings.webShortcuts[0].suggest.isEmpty();
    }
    const bool live = args.contains(u"--live"_s);
    if (live)
        settings.clipboard = true; // its window to look at; the history below, nothing of the user's shown
    // --scope-restore-check <all|files|content> [--clear-on-open]: App's part
    // of GitHub #263. state.ini of this run's profile says the scope was left
    // there; the real App::start is to have the launcher in it (in 全部 with
    // 每次打开时清空搜索 on). Read off the launcher the windows are given.
    const QString scopeLeft = valueAfter(args, u"--scope-restore-check");
    bool scopeRestoreOk = true;
    if (!scopeLeft.isEmpty() && args.contains(u"--clear-on-open"_s))
        settings.clearOnOpen = true;
    if (args.contains(u"--paste-plain-check"_s)) { // GitHub #408: a chord nobody else has
        if (!pastePlainChordFree()) {
            qWarning("paste plain: %s is someone else's already", qPrintable(QString(kPastePlainProbeChord)));
            return 4;
        }
        settings.pastePlainHotkey = QString(kPastePlainProbeChord);
    }
    const bool duplicatesCheck = args.contains(u"--duplicates-check"_s);
    const bool entryHotkey = duplicatesCheck || args.contains(u"--entry-hotkey-check"_s);
    if (duplicatesCheck)
        settings.clipboardKeepPlace = args.contains(u"--keep-place"_s);
    if (entryHotkey) { // an entry's own shortcut: a chord nobody else has
        if (!entryHotkeyChordFree()) {
            qWarning("entry hotkey: %s is someone else's already", qPrintable(QString(kEntryHotkeyProbeChord)));
            return 4;
        }
        settings.clipboard = true;
        settings.clipboardDuplicates = true;
    }
    if (args.contains(u"--command-hotkey-check"_s)) { // GitHub #466: a plugin command's own shortcut, a chord nobody else has
        if (!commandHotkeyChordFree()) {
            qWarning("command hotkey: %s is someone else's already", qPrintable(QString(kCommandHotkeyProbeChord)));
            return 4;
        }
        if (!seedCommandHotkey(settings))
            return 4;
    }
    const bool activationPaste = args.contains(u"--activation-paste-check"_s);
    const bool gameCheck = args.contains(u"--full-screen-game-check"_s); // GitHub #452
    const bool fieldFocus = gameCheck || args.contains(u"--field-focus-check"_s);
    if (activationPaste || fieldFocus) { // GitHub #432: the clipboard's own chord, nobody else's
        if (!activationPasteChordFree()) {
            qWarning("activation paste: %s is someone else's already", qPrintable(QString(kActivationPasteProbeChord)));
            return 4;
        }
        settings.clipboard = true;
        settings.clipboardHotkey = QString(kActivationPasteProbeChord);
    }
    settings.save(); // prepared profiles normally skip onboarding
    if (entryHotkey && !seedEntryHotkey(Settings::dataDir() + u"/clipboard"_s))
        return 4;
    if (activationPaste
        && !seedActivationPastePicture(Settings::dataDir() + u"/clipboard"_s,
            std::clamp(valueAfter(args, u"--picture-width", u"240"_s).toInt(), 16, 6000)))
        return 4;
    if (fieldFocus && !seedFieldFocus(Settings::dataDir() + u"/clipboard"_s))
        return 4;
    if (!scopeLeft.isEmpty()) {
        QSettings state(Settings::dataDir() + u"/state.ini"_s, QSettings::IniFormat);
        state.setValue(u"Search/Scope"_s, scopeLeft);
        state.sync();
    }
    if (args.contains(u"--clipboard-display-probe"_s) || args.contains(u"--clipboard-display-service-probe"_s) || live
        || args.contains(u"--idle-open-check"_s) || args.contains(u"--foreground-use-check"_s)
        || args.contains(u"--clipboard-animation-check"_s)) {
        ClipStore store(Settings::dataDir() + u"/clipboard"_s);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!store.open(now)) return 4;
        const QStringList texts = live
            ? QStringList {u"会议纪要：下周三前提交验收报告"_s, u"https://example.com/docs/winshun"_s, u"F:\\项目资料\\2026\\验收报告.pdf"_s,
                  u"SELECT name, size FROM files WHERE size > 1048576;"_s, u"alpha clipboard display fixture"_s, u"beta clipboard display fixture"_s}
            : QStringList {u"alpha clipboard display fixture"_s, u"beta clipboard display fixture"_s};
        for (const QString& text : texts) {
            ClipCapture clip;
            clip.text = text;
            clip.source = u"Display probe"_s;
            if (!store.add(clip, now)) return 4;
        }
        store.flush();
    }
    if (onboardingAction == u"fresh")
        QFile::remove(Settings::filePath()); // only this probe's private profile
    if (onboardingAction == u"legacy") {
        QSettings legacy(Settings::filePath(), QSettings::IniFormat);
        legacy.remove(u"Setup/Completed"_s);
        legacy.sync();
    }
    if (args.contains(u"--legacy-semantic"_s)) {
        QSettings legacy(Settings::filePath(), QSettings::IniFormat);
        legacy.setValue(u"Plugin.winshun.semantic/Enabled"_s, true);
    }
    if (args.contains(u"--legacy-capture"_s)) {
        QSettings legacy(Settings::filePath(), QSettings::IniFormat);
        legacy.setValue(u"Capture/Enabled"_s, true);
        legacy.setValue(u"Capture/Hotkey"_s, probeHotkey);
        legacy.setValue(u"Capture/PrintScreen"_s, false);
    }
    QJsonObject report;
    if (!onboardingAction.isEmpty()) {
        bool passed = false;
        try {
            App app;
            passed = app.start({.background = true}) && probeOnboarding(args, out, report);
        } catch (const std::exception& error) {
            report.insert(u"error"_s, QString::fromUtf8(error.what()));
            say(u"onboarding probe aborted: "_s + QString::fromUtf8(error.what()));
        }
        QByteArray logged;
        if (QFile log(g_logPath); log.open(QIODevice::ReadOnly))
            logged = log.readAll();
        // The probe's own: showOffScreen gives the guide, shown already, new
        // window flags, and Qt a new native handle for them (WindowFrame says so).
        qsizetype expectedWarnings = logged.count("WARNING: Window frame: the window has a new native handle (onboardingWindow), hooked again");
        if (onboardingAction == u"offline" || onboardingAction == u"retry")
            expectedWarnings += logged.count("WARNING: Plugin library: no list from http://127.0.0.1:1/unreachable.json");
        // The screenshot's copy made unreadable on purpose (verify-onboarding.ps1, included-broken): the library instead.
        if (args.contains(u"--copy-fallback"_s))
            expectedWarnings += logged.count("WARNING: Onboarding: screenshot not installed from");
        report.insert(u"warnings"_s, g_warnings);
        report.insert(u"expected_warnings"_s, static_cast<int>(expectedWarnings));
        const int unexpected = std::max(0, g_warnings - static_cast<int>(expectedWarnings));
        report.insert(u"unexpected_warnings"_s, unexpected);
        report.insert(u"passed"_s, passed);
        QFile json(out.filePath(u"onboarding.json"_s));
        const bool saved = json.open(QIODevice::WriteOnly) && json.write(QJsonDocument(report).toJson()) > 0;
        const QFileInfo privateProfile(Settings::profile());
        if (privateProfile.fileName().startsWith(u"WinShun-preview-")
            && QDir::cleanPath(privateProfile.absolutePath()).compare(QDir::cleanPath(QDir::tempPath()), Qt::CaseInsensitive) == 0)
            QDir(privateProfile.absoluteFilePath()).removeRecursively();
        else
            return 4;
        return passed && saved ? std::min(unexpected, 100) : 4;
    }
    if (args.contains(u"--ws-plugin-check"_s)) { // a check of its own: none of the startup probe's other measurements
        bool passed = wsPluginFixture(out);
        if (passed) {
            App app;
            passed = app.start({.background = true, .settingsProbe = true, .mcpPipe = kWsPluginPipe}) && probeWsPlugin(app, args, out, report);
        }
        const int warnings = g_warnings; // the App's end included
        report.insert(u"warnings"_s, warnings);
        QFile json(out.filePath(u"ws-plugin.json"_s));
        const bool saved = json.open(QIODevice::WriteOnly) && json.write(QJsonDocument(report).toJson()) > 0;
        QDir(Settings::profile()).removeRecursively();
        say(u"ws plugin: %1, %2 warning(s)"_s.arg(passed ? u"passed"_s : u"FAILED"_s).arg(warnings));
        return passed && saved ? std::min(warnings, 100) : 4;
    }
    {
        // --idle-open-check, --live …: its profile goes once the App is gone.
        // Removed while the App lived, the folder came back as it saved its
        // index or its clipboard on the way out (left in %TEMP%, a run each).
        const auto probeProfile = qScopeGuard([&args] {
            const QFileInfo profile(Settings::profile());
            if ((args.contains(u"--idle-open-check"_s) || args.contains(u"--foreground-use-check"_s) || args.contains(u"--live"_s)
                    || args.contains(u"--clipboard-animation-check"_s) || args.contains(u"--settings-motion-check"_s)
                    || args.contains(u"--search-shadow-check"_s) || args.contains(u"--paste-plain-check"_s)
                    || args.contains(u"--activation-paste-check"_s) || args.contains(u"--entry-hotkey-check"_s)
                    || args.contains(u"--command-hotkey-check"_s)
                    || args.contains(u"--duplicates-check"_s) || args.contains(u"--field-focus-check"_s)
                    || args.contains(u"--full-screen-game-check"_s)
                    || args.contains(u"--launcher-settings-check"_s) || args.contains(u"--launcher-open-check"_s)
                    || args.contains(u"--pinned-stacking-check"_s))
                && profile.fileName().startsWith(u"WinShun-preview-")
                && QDir::cleanPath(profile.absolutePath()).compare(QDir::cleanPath(QDir::tempPath()), Qt::CaseInsensitive) == 0)
                QDir(profile.absoluteFilePath()).removeRecursively();
        });
        // Before the App is made, so that what it holds from its start is told too.
        if (args.contains(u"--foreground-use-trace"_s) && !(memory::allocationTrace::start() && checkAllocationTrace()))
            return 4;
        if (args.contains(u"--settings-motion-check"_s))
            copyGalleryCacheForMotionCheck();
        App app;
        QElapsedTimer since;
        since.start();
        if (!app.start({.background = true, .clipboardDisplayProbe = args.contains(u"--clipboard-display-probe"_s),
                .settingsProbe = args.contains(u"--settings-reopen-check"_s) || args.contains(u"--settings-motion-check"_s)}))
            return 4;
        if (args.contains(u"--live"_s))
            return runLive(app, out); // its profile: probeProfile
        if (args.contains(u"--capture-hotkey-probe"_s) && !probeCaptureHotkeys(app, args, out, report)) {
            QFile failed(out.filePath(u"capture-hotkey-failed.json"_s));
            if (failed.open(QIODevice::WriteOnly)) failed.write(QJsonDocument(report).toJson());
            return 4;
        }
        if (args.contains(u"--web-plugin-check"_s) && !probeWebPlugin(app, args, out, report)) {
            QFile failed(out.filePath(u"web-plugin-failed.json"_s));
            if (failed.open(QIODevice::WriteOnly)) failed.write(QJsonDocument(report).toJson());
            return 4;
        }
        report.insert(u"start_ms"_s, static_cast<double>(since.elapsed()));
        pump(std::clamp(valueAfter(args, u"--settle-ms", u"3000"_s).toInt(), 100, 60000));
        if (!scopeLeft.isEmpty()) {
            // Not returned from here: the profile and what else this probe
            // made are put away at its end.
            QObject* launcher = nullptr;
            for (QWindow* candidate : QGuiApplication::allWindows()) {
                launcher = candidate->property("launcher").value<QObject*>();
                if (launcher)
                    break;
            }
            const int expected = settings.clearOnOpen || scopeLeft == u"all" ? Launcher::All
                : scopeLeft == u"content" ? Launcher::Content : Launcher::Files;
            const int scope = launcher ? launcher->property("scope").toInt() : -1;
            scopeRestoreOk = launcher && scope == expected;
            report.insert(u"scope_restore"_s, QJsonObject {{u"left"_s, scopeLeft}, {u"clear_on_open"_s, settings.clearOnOpen},
                {u"state_file_says"_s, QSettings(Settings::dataDir() + u"/state.ini"_s, QSettings::IniFormat).value(u"Search/Scope"_s).toString()},
                {u"launcher_found"_s, launcher != nullptr}, {u"scope"_s, scope}, {u"expected"_s, expected}, {u"passed"_s, scopeRestoreOk}});
            if (!launcher)
                qWarning("scope restore: NOT MEASURED, no window with the launcher was found");
            else if (!scopeRestoreOk)
                qWarning("scope restore: FAILED, the launcher is in scope %d, expected %d", scope, expected);
            else
                say(u"scope restore: left in %1%2, the started App's launcher is in scope %3"_s
                        .arg(scopeLeft, settings.clearOnOpen ? u" with clear-on-open"_s : QString()).arg(scope));
        }
        if (args.contains(u"--double-tap-plugin"_s) && !checkDoubleTapPlugin(app, out))
            return 4;
        if (args.contains(u"--idle-open-check"_s))
            return checkIdleOpen(app, args, out) ? 0 : 4; // its profile goes after the App (probeProfile)
        if (args.contains(u"--foreground-use-check"_s))
            return checkForegroundUse(app, args, out) ? 0 : 4; // its profile likewise
        if (args.contains(u"--clipboard-animation-check"_s))
            return checkClipboardAnimation(app, args, out) ? 0 : 4; // its profile likewise
        if (args.contains(u"--launcher-settings-check"_s))
            return checkLauncherToSettings(app, args, out) ? 0 : 4; // its profile likewise
        if (args.contains(u"--launcher-open-check"_s))
            return checkLauncherOpen(app, args, out) ? 0 : 4; // its profile likewise
        if (args.contains(u"--pinned-stacking-check"_s))
            return checkPinnedStacking(app, args, out) ? 0 : 4; // its profile likewise
        if (args.contains(u"--search-shadow-check"_s))
            return checkSearchShadow(app, args, out) ? 0 : 4; // its profile likewise
        if (args.contains(u"--paste-plain-check"_s))
            return checkPastePlain(app, args, out) ? 0 : 4; // its profile likewise
        if (args.contains(u"--activation-paste-check"_s))
            return checkActivationPaste(app, args, out) ? 0 : 4; // its profile likewise
        if (args.contains(u"--full-screen-game-check"_s))
            return checkFullScreenGame(app, args, out) ? 0 : 4; // its profile likewise
        if (args.contains(u"--field-focus-check"_s))
            return checkFieldFocus(app, args, out) ? 0 : 4; // its profile likewise
        if (args.contains(u"--entry-hotkey-check"_s))
            return checkEntryHotkey(app, args, out) ? 0 : 4; // its profile likewise
        if (args.contains(u"--command-hotkey-check"_s))
            return checkCommandHotkey(app, args, out) ? 0 : 4; // its profile likewise
        if (args.contains(u"--duplicates-check"_s))
            return checkDuplicates(app, args, out) ? 0 : 4; // its profile likewise
        if (args.contains(u"--settings-reopen-check"_s))
            return probeSettingsReopen(app, args, out, report) ? 0 : 4;
        if (args.contains(u"--settings-motion-check"_s))
            return checkSettingsMotion(app, args, out) ? 0 : 4; // its profile likewise
        if (args.contains(u"--clipboard-shell-probe"_s)) {
            const bool ok = probeClipboardShell(valueAfter(args, u"--clipboard-shell-probe"_s), out, report,
                args.contains(u"--clipboard-display-probe"_s) || args.contains(u"--clipboard-display-service-probe"_s),
                args.contains(u"--clipboard-display-service-probe"_s));
            QFile result(out.filePath(u"clipboard-shell.json"_s));
            if (result.open(QIODevice::WriteOnly)) result.write(QJsonDocument(report).toJson());
            return ok ? 0 : 4;
        }
        if ((settings.dialogBar && !probeFileDialog(args, out, dialogSample, report))
            || (typingProbe && !probeFolderTyping(app, out, QDir::toNativeSeparators(settings.indexFolders.front()), report))) {
            QFile failed(out.filePath(u"runtime.json"_s));
            if (failed.open(QIODevice::WriteOnly))
                failed.write(QJsonDocument(report).toJson());
            return 4;
        }

        // Private bytes can be summed; working sets also include shared pages.
        std::vector<DWORD> owned {GetCurrentProcessId()};
        QJsonArray processes;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return 4;
        PROCESSENTRY32W entry {sizeof(entry)};
        std::vector<PROCESSENTRY32W> all;
        if (Process32FirstW(snapshot, &entry)) {
            do { all.push_back(entry); } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        double totalPrivate = 0;
        for (std::size_t i = 0; i < owned.size(); ++i) {
            for (const auto& process : all) {
                if (process.th32ParentProcessID == owned[i] && std::ranges::find(owned, process.th32ProcessID) == owned.end())
                    owned.push_back(process.th32ProcessID);
            }
            const DWORD pid = owned[i];
            HANDLE handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (!handle)
                continue;
            PROCESS_MEMORY_COUNTERS_EX memory {sizeof(memory)};
            if (GetProcessMemoryInfo(handle, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
                const auto found = std::ranges::find(all, pid, &PROCESSENTRY32W::th32ProcessID);
                processes.append(QJsonObject {{u"pid"_s, static_cast<double>(pid)},
                    {u"name"_s, found == all.end() ? QString() : QString::fromWCharArray(found->szExeFile)},
                    {u"private_bytes"_s, static_cast<double>(memory.PrivateUsage)},
                    {u"working_set_bytes"_s, static_cast<double>(memory.WorkingSetSize)}});
                totalPrivate += static_cast<double>(memory.PrivateUsage);
            }
            CloseHandle(handle);
        }
        report.insert(u"processes"_s, processes);
        report.insert(u"total_private_bytes"_s, totalPrivate);
        report.insert(u"clipboard"_s, settings.clipboard);
        report.insert(u"qt"_s, QString::fromLatin1(qVersion()));
        if (args.contains(u"--runtime-ui"_s)) {
            app.showSettings();
            pump(100);
            QQuickWindow* window = nullptr;
            SettingsEditor* editor = nullptr;
            for (QWindow* candidate : QGuiApplication::allWindows()) {
                if (auto* found = candidate->property("editor").value<SettingsEditor*>()) {
                    editor = found;
                    window = qobject_cast<QQuickWindow*>(candidate);
                    break;
                }
            }
            if (!window || !editor)
                return 4;
            showOffScreen(window);
            if (args.contains(u"--ocr-probe"_s)) {
                if (args.contains(u"--compact"_s))
                    window->resize(760, 420);
                QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, window->property("clipboardPage")), Q_ARG(QVariant, QVariant(0)));
                settle(window, 3);
                auto* row = qobject_cast<QQuickItem*>(visualNamed(window->contentItem(), u"enhancedOcrRow"_s));
                QObject* button = visualNamed(window->contentItem(), u"downloadEnhancedOcr"_s);
                if (!row || !button)
                    return 4;
                QMetaObject::invokeMethod(window, "goTo", Q_ARG(QVariant, QVariant::fromValue(row)));
                settle(window, 3);
                auto* model = editor->ocrPackage();
                report.insert(u"ocr_before"_s, model->available());
                report.insert(u"ocr_title"_s, row->property("title").toString());
                report.insert(u"ocr_description"_s, row->property("description").toString());
                report.insert(u"ocr_button"_s, button->property("text").toString());
                auto* buttonItem = qobject_cast<QQuickItem*>(button);
                const QRectF buttonRect = buttonItem ? buttonItem->mapRectToItem(row, QRectF(QPointF(), buttonItem->size())) : QRectF();
                report.insert(u"ocr_button_inside_row"_s, !buttonRect.isEmpty() && QRectF(QPointF(-1, -1), row->size() + QSizeF(2, 2)).contains(buttonRect));
                if (!saveGrab(window, out.filePath(u"ocr-before.png"_s)))
                    return 4;
                QJsonArray attempts;
                const auto attempt = [&](const QString& source, bool local, bool cancel, int number) {
                    if (local) {
                        model->installFile(source);
                    } else {
                        _wputenv_s(L"WINSHUN_OCR_PACKAGE", source.toStdWString().c_str());
                        QMetaObject::invokeMethod(button, "clicked");
                    }
                    QElapsedTimer waiting;
                    waiting.start();
                    while (model->busy() && model->progress() < 0.03 && waiting.elapsed() < 15000)
                        pump(20);
                    settle(window, 2);
                    if (!saveGrab(window, out.filePath(u"ocr-%1-active.png"_s.arg(number))))
                        return false;
                    if (cancel)
                        QMetaObject::invokeMethod(button, "clicked");
                    while (model->busy() && waiting.elapsed() < 20000)
                        pump(20);
                    attempts.append(QJsonObject {{u"available"_s, model->available()}, {u"busy"_s, model->busy()},
                        {u"status"_s, model->status()}, {u"progress"_s, model->progress()}});
                    settle(window, 3);
                    return saveGrab(window, out.filePath(u"ocr-%1-result.png"_s.arg(number))) && !model->busy();
                };
                const QString source = valueAfter(args, u"--ocr-source");
                const QString local = valueAfter(args, u"--ocr-local");
                if ((!source.isEmpty() || !local.isEmpty()) && !attempt(local.isEmpty() ? source : local, !local.isEmpty(), args.contains(u"--ocr-cancel"_s), 1))
                    return 4;
                const QString retry = valueAfter(args, u"--ocr-retry-source");
                if (!retry.isEmpty() && !attempt(retry, false, false, 2))
                    return 4;
                // --ocr-precise <the larger reading model's file>: its row shows
                // once the package is there; installed from the file, in use,
                // and removed again by the row's own button.
                if (const QString precise = valueAfter(args, u"--ocr-precise"); !precise.isEmpty()) {
                    auto* preciseRow = qobject_cast<QQuickItem*>(visualNamed(window->contentItem(), u"preciseOcrRow"_s));
                    auto* preciseButton = qobject_cast<QQuickItem*>(visualNamed(window->contentItem(), u"preciseOcrButton"_s));
                    OcrModel* larger = model->preciseModel();
                    if (!preciseRow || !preciseButton)
                        return 4;
                    report.insert(u"precise_shown_with_package"_s, preciseRow->isVisible() == model->available());
                    QMetaObject::invokeMethod(window, "goTo", Q_ARG(QVariant, QVariant::fromValue(preciseRow)));
                    settle(window, 3);
                    report.insert(u"precise_offer"_s, preciseRow->property("description").toString());
                    report.insert(u"precise_offer_button"_s, preciseButton->property("text").toString());
                    if (!saveGrab(window, out.filePath(u"ocr-precise-offer.png"_s)))
                        return 4;
                    larger->installFile(precise);
                    QElapsedTimer waiting;
                    waiting.start();
                    while (larger->busy() && waiting.elapsed() < 20000)
                        pump(20);
                    settle(window, 3);
                    const QRectF inRow = preciseButton->mapRectToItem(preciseRow, QRectF(QPointF(), preciseButton->size()));
                    report.insert(u"precise_installed"_s, larger->installed() && !ocr::preciseModel().isEmpty());
                    report.insert(u"precise_status"_s, larger->status());
                    report.insert(u"precise_installed_text"_s, preciseRow->property("description").toString());
                    report.insert(u"precise_installed_button"_s, preciseButton->property("text").toString());
                    report.insert(u"precise_button_inside_row"_s, QRectF(QPointF(-1, -1), preciseRow->size() + QSizeF(2, 2)).contains(inRow));
                    if (!saveGrab(window, out.filePath(u"ocr-precise-installed.png"_s)))
                        return 4;
                    QMetaObject::invokeMethod(preciseButton, "clicked");
                    settle(window, 3);
                    report.insert(u"precise_removed"_s, !larger->installed() && ocr::preciseModel().isEmpty());
                    report.insert(u"precise_removed_button"_s, preciseButton->property("text").toString());
                }
                // --ocr-own <a reading model of the user's>: tried and taken
                // through the row's model, named in the row, removed by its button.
                if (const QString mine = valueAfter(args, u"--ocr-own"); !mine.isEmpty()) {
                    auto* preciseRow = qobject_cast<QQuickItem*>(visualNamed(window->contentItem(), u"preciseOcrRow"_s));
                    auto* preciseButton = qobject_cast<QQuickItem*>(visualNamed(window->contentItem(), u"preciseOcrButton"_s));
                    auto* ownButton = qobject_cast<QQuickItem*>(visualNamed(window->contentItem(), u"chooseOwnOcrModel"_s));
                    OcrModel* larger = model->preciseModel();
                    if (!preciseRow || !preciseButton || !ownButton)
                        return 4;
                    QMetaObject::invokeMethod(window, "goTo", Q_ARG(QVariant, QVariant::fromValue(preciseRow)));
                    settle(window, 3);
                    report.insert(u"own_offer_button"_s, ownButton->property("text").toString());
                    QElapsedTimer waiting;
                    waiting.start();
                    larger->useOwnFile(mine);
                    while (larger->busy() && waiting.elapsed() < 60000)
                        pump(20);
                    settle(window, 3);
                    const QRectF inRow = ownButton->mapRectToItem(preciseRow, QRectF(QPointF(), ownButton->size()));
                    report.insert(u"own_ms"_s, waiting.elapsed());
                    report.insert(u"own_name"_s, larger->ownName());
                    report.insert(u"own_status"_s, larger->status());
                    report.insert(u"own_text"_s, preciseRow->property("description").toString());
                    report.insert(u"own_button"_s, preciseButton->property("text").toString());
                    report.insert(u"own_change_button"_s, ownButton->property("text").toString());
                    report.insert(u"own_buttons_inside_row"_s, QRectF(QPointF(-1, -1), preciseRow->size() + QSizeF(2, 2)).contains(inRow));
                    if (!saveGrab(window, out.filePath(u"ocr-own.png"_s)))
                        return 4;
                    QMetaObject::invokeMethod(preciseButton, "clicked");
                    settle(window, 3);
                    report.insert(u"own_removed"_s, larger->ownName().isEmpty() && ocr::ownModel().file.isEmpty());
                }
                report.insert(u"ocr_attempts"_s, attempts);
            } else {
            QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, window->property("pluginsPage")), Q_ARG(QVariant, QVariant(0)));
            settle(window, 3);
            report.insert(u"recovery_before"_s, editor->plugins()->recoveryMessage());
            if (args.contains(u"--install-gallery"_s)) {
                auto requests = std::make_shared<int>(0);
                const auto connection = QObject::connect(editor->plugins(), &PluginSettings::confirmationNeeded, editor,
                    [requests](const QVariantMap&) { ++*requests; });
                const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
                auto* gallery = editor->plugins()->gallery();
                QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, window->property("pluginsPage")), Q_ARG(QVariant, QVariant(1)));
                gallery->setActive(true);
                QElapsedTimer waiting;
                waiting.start();
                while (gallery->fetching() && waiting.elapsed() < 15000)
                    pump(20);
                const QString id = valueAfter(args, u"--install-gallery");
                settle(window, 3);
                QObject* install = visualNamed(window->contentItem(), u"installPlugin-"_s + id);
                if (!install || !install->property("enabled").toBool())
                    return 4;
                QMetaObject::invokeMethod(install, "clicked");
                while (gallery->downloading() && waiting.elapsed() < 15000)
                    pump(20);
                report.insert(u"permission_requests"_s, *requests);
                report.insert(u"enabled_before_consent"_s, QSettings(Settings::filePath(), QSettings::IniFormat).value(u"Plugin."_s + id + u"/Enabled"_s).toBool());
                settle(window, 3);
                if (!saveGrab(window, out.filePath(u"install-permission.png"_s)))
                    return 4;
                if (args.contains(u"--approve-install"_s))
                    editor->plugins()->confirmEnable(id);
                else
                    editor->plugins()->declineEnable(id);
                report.insert(u"enabled_after_consent"_s, QSettings(Settings::filePath(), QSettings::IniFormat).value(u"Plugin."_s + id + u"/Enabled"_s).toBool());
            }
            if (args.contains(u"--retry-index"_s)) {
                _wputenv_s(L"WINSHUN_PLUGIN_INDEX", valueAfter(args, u"--retry-index").toStdWString().c_str());
                QObject* retry = visualNamed(window->contentItem(), u"retryPluginRecovery"_s);
                if (!retry)
                    return 4;
                QMetaObject::invokeMethod(retry, "clicked");
            }
            if (args.contains(u"--local-package"_s))
                editor->plugins()->install(valueAfter(args, u"--local-package"));
            QElapsedTimer waiting;
            waiting.start();
            while (editor->plugins()->recovering() && waiting.elapsed() < 15000)
                pump(20);
            const bool captureInstalled = QFileInfo::exists(Settings::profile() + u"/plugins/winshun.capture/plugin.json"_s);
            const auto captureRunning = [&] {
                auto* list = editor->plugins()->list();
                for (int row = 0; row < list->rowCount(); ++row) {
                    const QModelIndex at = list->index(row);
                    if (list->data(at, PluginListModel::IdRole).toString() == u"winshun.capture")
                        return list->data(at, PluginListModel::StatusRole).toString().startsWith(PluginSettings::tr("运行中"));
                }
                return false;
            };
            const QString readyFile = valueAfter(args, u"--probe-ready-file");
            waiting.restart();
            while (captureInstalled && (!captureRunning() || (!readyFile.isEmpty() && !QFileInfo::exists(readyFile)))
                && waiting.elapsed() < 5000)
                pump(20);
            report.insert(u"recovery_after"_s, editor->plugins()->recoveryMessage());
            report.insert(u"recovering"_s, editor->plugins()->recovering());
            report.insert(u"capture_installed"_s, captureInstalled);
            report.insert(u"capture_running"_s, captureRunning());
            QSettings capture(Settings::profile() + u"/plugins/winshun.capture.data/capture.ini"_s, QSettings::IniFormat);
            report.insert(u"capture_hotkey"_s, capture.value(u"Capture/Hotkey"_s).toString());
            if (!saveGrab(window, out.filePath(u"runtime-recovery.png"_s)))
                return 4;
            }
        }
        report.insert(u"warnings"_s, g_warnings);
    }
    QFile json(out.filePath(u"runtime.json"_s));
    const bool saved = json.open(QIODevice::WriteOnly) && json.write(QJsonDocument(report).toJson()) > 0;
    if (!forgetRuntimeIdentity()) {
        qWarning("runtime probe: could not forget its temporary LocalSend identity; profile kept at %s", qPrintable(Settings::profile()));
        return 4;
    }
    QDir(Settings::profile()).removeRecursively();
    say(u"production App startup: isolated profile, %1 process(es), %2 MiB private, %3 warning(s)"_s
        .arg(report.value(u"processes"_s).toArray().size()).arg(report.value(u"total_private_bytes"_s).toDouble() / 1048576.0, 0, 'f', 1).arg(g_warnings));
    if (!scopeRestoreOk)
        return 4; // --scope-restore-check: said above, and in runtime.json
    return saved ? std::min(g_warnings, 100) : 4;
}

} // namespace

bool wanted(const QStringList& args)
{
    return args.contains(kFlag) || args.contains(kSettingsFlag) || args.contains(kResultsFlag) || args.contains(kAiCardsFlag) || args.contains(kRuntimeFlag);
}

void note(const QString& line)
{
    say(line);
}

void prepare(const QStringList& args)
{
    if (args.contains(u"--composited-frame"_s))
        win::useCompositedFramesForPreview();
    if (args.contains(kRuntimeFlag))
        qputenv("QT_D3D_NO_VBLANK_THREAD", "1"); // the production startup's renderer setting
    const bool resultsMode = args.contains(kResultsFlag);
    const QString folder = valueAfter(args, args.contains(kRuntimeFlag) ? kRuntimeFlag : resultsMode ? kResultsFlag : args.contains(kAiCardsFlag) ? kAiCardsFlag : kSettingsFlag);
    if (!folder.isEmpty()) {
        QDir().mkpath(folder);
        g_logPath = QDir(folder).filePath(u"preview.log"_s);
    } else {
        g_logPath = valueAfter(args, kFlag) + u".log"_s;
    }
    QFile::remove(g_logPath);
    say(u"preparing"_s);
    // The scale asked for, exactly: not on top of the screen's own. A live
    // runtime probe without --scale keeps the screen's, as the app does.
    if (!(args.contains(kRuntimeFlag) && args.contains(u"--live"_s) && !args.contains(u"--scale"_s))) {
        qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
        qputenv("QT_SCALE_FACTOR", valueAfter(args, u"--scale", u"1"_s).toLatin1());
    }
    const bool webForm = args.contains(u"--web-form"_s);
    // --renderer d3d11: the window as the desktop shows it (for counting edge pixels), not the software renderer's.
    const bool nativeWeb = (webForm || args.contains(QString(kSettingsFlag)) || args.contains(kAiCardsFlag) || args.contains(kRuntimeFlag))
        && valueAfter(args, u"--renderer") == u"d3d11";
    if (nativeWeb)
        qputenv("QSG_RHI_BACKEND", "d3d11");
    if (resultsMode || nativeWeb)
        qunsetenv("QT_QUICK_BACKEND"); // the same native text path as the launcher
    else
        qputenv("QT_QUICK_BACKEND", "software"); // renders without a window on screen
    qputenv("QT_QPA_PLATFORM", resultsMode || nativeWeb ? "windows:fontengine=freetype" : "windows:fontengine=gdi");
    if (args.contains(u"--preview-interactions"_s)) {
        if (!args.contains(u"--native-ui-probe"_s))
            qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("QT_QUICK_BACKEND", "software");
        qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "/Fonts");
    }
    // Nothing of the user's is read or written: a profile folder of its own.
    Settings::useProfile(QDir::temp().filePath(u"WinShun-preview-"_s + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)));
}

int run(const QStringList& args)
{
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    const bool settingsMode = args.contains(kSettingsFlag);
    const bool resultsMode = args.contains(kResultsFlag);
    const bool cardsMode = args.contains(kAiCardsFlag);
    const bool runtimeMode = args.contains(kRuntimeFlag);
    // --style fluent|brand|tactile: every preview in that 界面风格.
    SystemTheme::setUiStyle(valueAfter(args, u"--style", u"fluent"_s));
    // QML singletons may still watch components/ after the window and its
    // controller are gone. Clean up only after the preview's engine unwinds.
    const QString profile = Settings::profile();
    const auto cleanup = qScopeGuard([profile, runtimeMode] {
        const QFileInfo folder(profile);
        if (!runtimeMode && folder.fileName().startsWith(u"WinShun-preview-")
            && QDir::cleanPath(folder.absolutePath()).compare(QDir::cleanPath(QDir::tempPath()), Qt::CaseInsensitive) == 0)
            QDir(profile).removeRecursively();
    });
    if (valueAfter(args, runtimeMode ? kRuntimeFlag : resultsMode ? kResultsFlag : cardsMode ? kAiCardsFlag : settingsMode ? kSettingsFlag : kFlag).isEmpty()) {
        std::fprintf(stderr, "usage: WinShun --preview-plugins <file.png> [--dark] [--scale 1.5] [--dialog | --dialog-allow | --dialog-aside] [--updates] [--log-many] [--page ai] [--elevated] [--open]\n"
                             "       WinShun --preview-settings <folder> [--dark] [--scale 1.5] [--tree] [--edges] [--renderer d3d11] [--builtins] [--open]\n");
        return 2;
    }
    g_previous = qInstallMessageHandler(countWarnings);
    g_edges = args.contains(u"--edges"_s) || args.contains(u"--tree"_s);
    QGuiApplication::styleHints()->setColorScheme(args.contains(u"--dark"_s) ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    SystemTheme::setBackdropAvailable(false); // plain colours, as a window without Mica
    return runtimeMode ? runRuntime(args) : resultsMode ? runResults(args) : cardsMode ? runAiCards(args) : settingsMode ? runSettings(args) : runPlugins(args);
}

} // namespace ws::preview
