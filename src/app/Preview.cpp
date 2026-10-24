#include "Preview.h"

#include "Backup.h"
#include "CliSetup.h"
#include "CommandLine.h"
#include "FavoritesSettings.h"
#include "FileIconProvider.h"
#include "PluginSettings.h"
#include "ResultModel.h"
#include "Settings.h"
#include "SettingsEditor.h"
#include "SystemTheme.h"
#include "Updater.h"
#include "WindowFrame.h"
#include "platform/SiteIcons.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLibraryInfo>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>
#include <QUuid>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

using namespace Qt::StringLiterals;

namespace ws::preview {

namespace {

constexpr auto kFlag = u"--preview-plugins";
constexpr auto kSettingsFlag = u"--preview-settings";
constexpr auto kResultsFlag = u"--preview-results";
constexpr auto kAiCardsFlag = u"--preview-ai-cards";

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
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
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

// A window off the screen gets no resize or expose from Windows, so its
// content item would stay 0x0 (and every `parent.width` with it): sized
// here, more than once, as the height follows the width through the
// wrapping. Items are laid out (polished) only for a frame, and a frame
// comes only with a grab here: a grab per pass, thrown away, until the
// height the page asks for is the window's.
void settle(QQuickWindow* window, int passes = 4)
{
    for (int pass = 0; pass < passes; ++pass) {
        window->contentItem()->setSize(QSizeF(window->width(), window->height()));
        pump(200);
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

bool saveGrab(QQuickWindow* window, const QString& path)
{
    const QImage image = window->grabWindow();
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

int finish(QQuickWindow* window, bool saved)
{
    window->hide();
    delete window;
    QDir(Settings::dataDir()).removeRecursively();
    say(u"%1 warning(s)"_s.arg(g_warnings));
    return saved ? std::min(g_warnings, 100) : 4;
}

// ---- --preview-plugins ------------------------------------------------------------------

int runPlugins(const QStringList& args)
{
    const QString out = valueAfter(args, kFlag);
    const bool dark = args.contains(u"--dark"_s);
    const bool dialog = args.contains(u"--dialog"_s);
    const bool aiPage = valueAfter(args, u"--page") == u"ai"; // the AI 助手 page instead (its plugin switches)

    say(u"starting (dark %1, dialog %2)"_s.arg(dark).arg(dialog));
    QQmlApplicationEngine engine;
    engine.addImageProvider(u"fileicon"_s, new FileIconProvider);
    Settings settings = Settings::defaults();
    if (aiPage) { // every row on show: the switches above the plugins' ones on
        settings.mcp = true;
        settings.mcpPlugins = true;
    }
    auto* editor = new SettingsEditor(settings, u"software"_s, &engine);
    editor->plugins()->setLocale(u"zh"_s);
    editor->plugins()->loadPreview();
    if (aiPage)
        editor->setMcpStatus(u"已连接：Claude Desktop（12 次调用）"_s);
    say(u"loading the page"_s);

    QQmlComponent component(&engine, u"WinShun"_s, u"PluginsPreview"_s);
    QObject* object = component.createWithInitialProperties({
        {u"plugins"_s, QVariant::fromValue(editor->plugins())},
        {u"editor"_s, QVariant::fromValue(editor)},
        {u"dialogShown"_s, dialog},
        {u"aiPage"_s, aiPage},
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
    }
    say(u"grabbing"_s);
    if (args.contains(u"--tree"_s))
        writeTree(window, out + u".tree.txt"_s);
    const bool saved = saveGrab(window, out);
    say(u"dpr %1"_s.arg(window->devicePixelRatio()));
    return finish(window, saved);
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
    QObject* add = named(window, "webAddButton");
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

int runSettings(const QStringList& args)
{
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
    settings.mcp = true;
    settings.mcpPlugins = true;
    settings.taskbarSearchBox = true; // its style row
    settings.captureEnabled = true; // show the screenshot's options too; no Capture is created in this preview
    const QString shareOnline = u"\\\\192.168.1.10\\资料"_s;
    const QString shareSignIn = u"\\\\nas\\photos"_s;
    settings.shares = {{shareOnline, u"公司资料"_s}, {shareSignIn, {}}};
    settings.hotkey = u"Alt+Space"_s;
    settings.doubleCtrlExcludedApps = {u"TheFinals.exe"_s};
    settings.clipboardHotkey = u"Win+Alt+V"_s;
    settings.mcpExcludedPaths = {u"%USERPROFILE%\\Documents\\私人"_s};
    // The real view-model and its real neighbours, as App::showSettings makes
    // them; the renderer "running" is the one set, so that no restart is asked for.
    auto* editor = new SettingsEditor(settings, Settings::resolveRenderer(settings.renderer), &engine);
    QObject::connect(editor, &SettingsEditor::changed, editor, [editor] {
        SystemTheme::setRoundedCorners(editor->corners() == u"round");
    });
    auto* frame = new WindowFrame(true, editor);
    auto* updater = new Updater(editor); // never setAutomatic() or check(): no network
    editor->plugins()->setLocale(u"zh"_s);
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
    });
    SettingsEditor::IndexFolderState indexState;
    indexState.folder = u"C:\\Users\\me\\AppData\\Local\\WinShun"_s;
    indexState.size = u"112 MB"_s;
    editor->setIndexFolderState(indexState);
    editor->setContentIndexStatus(u"已收录 337,215 个文件，其中 5,815 个文档"_s);
    editor->setHistoryCount(128);
    editor->setClipboardCount(57);
    editor->setHotkeyError({});
    // What the app reads off this PC, read-only: the file managers
    // installed, the taskbar, the AI clients and their configurations.
    // Not refreshNumberKeys(): it probes with RegisterHotKey.
    editor->refreshFileManagers();
    editor->refreshTaskbarState();
    editor->refreshMcpClients();
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

    if (args.contains(u"--web-form"_s)) {
        if (args.contains(u"--compact"_s))
            window->resize(760, 420); // the supported minimum, also used on high-DPI screens
        return finish(window, previewWebForms(window, editor, out, tree));
    }

    if (args.contains(u"--plugin-layout"_s)) {
        if (args.contains(u"--compact"_s))
            window->resize(760, 420);
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(pluginsPage)), Q_ARG(QVariant, QVariant(0)));
        settle(window);
        auto* card = window->findChild<QQuickItem*>(u"captureSettingsCard"_s);
        auto* expand = window->findChild<QQuickItem*>(u"captureExpandButton"_s);
        if (!card || !expand) {
            qWarning("plugin layout: missing capture card or settings button");
            return finish(window, false);
        }
        bool saved = true;
        const auto grab = [&](const QString& name) {
            settle(window);
            if (tree)
                writeTree(window, out.filePath(name + u".tree.txt"_s));
            saved = saveGrab(window, out.filePath(name + u".png"_s)) && saved;
        };
        const auto check = [](bool condition, const char* message) {
            if (!condition)
                qWarning("plugin layout: %s", message);
        };
        const auto key = [&](int code) {
            QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &press);
            QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &release);
        };
        check(!card->property("expanded").toBool(), "capture options must start folded");
        say(u"plugins folded: card %1, page content %2"_s.arg(card->height()).arg(flick->property("contentHeight").toReal()));
        grab(u"plugins-folded"_s);
        expand->forceActiveFocus(Qt::TabFocusReason);
        key(Qt::Key_Space);
        settle(window);
        check(card->property("expanded").toBool(), "Space did not expand the capture card");
        auto* first = visualNamed(card, u"captureGroup0"_s);
        if (!first) {
            qWarning("plugin layout: missing category buttons");
            return finish(window, false);
        }
        first->forceActiveFocus(Qt::TabFocusReason);
        for (int group = 0; group < 6; ++group) {
            if (group > 0)
                key(Qt::Key_Right);
            settle(window);
            check(card->property("currentGroup").toInt() == group, "arrow key did not switch category");
            int visible = 0;
            for (int i = 0; i < 6; ++i) {
                auto* panel = window->findChild<QQuickItem*>(u"capturePanel%1"_s.arg(i));
                if (panel && panel->isVisible())
                    ++visible;
            }
            check(visible == 1, "more or fewer than one category visible");
            say(u"capture group %1: card %2, page content %3"_s.arg(group).arg(card->height()).arg(flick->property("contentHeight").toReal()));
            // Full content for inspecting every control; folded.png above stays at the actual viewport.
            const int viewport = window->height();
            window->setHeight(std::max(viewport, static_cast<int>(std::ceil(flick->property("contentHeight").toReal() + flick->property("y").toReal()))));
            grab(u"capture-group-%1"_s.arg(group));
            window->setHeight(viewport);
            settle(window);
        }
        key(Qt::Key_Right);
        check(card->property("currentGroup").toInt() == 0, "category navigation did not wrap");
        expand->forceActiveFocus(Qt::TabFocusReason);
        key(Qt::Key_Return);
        check(!card->property("expanded").toBool(), "Enter did not fold the capture card");
        QMetaObject::invokeMethod(card, "revealGroup", Q_ARG(QVariant, QVariant(4)));
        settle(window);
        auto* words = visualNamed(card, u"captureSmartWords"_s);
        auto* screenshotGroup = visualNamed(card, u"captureGroup1"_s);
        auto* delay = visualNamed(card, u"captureDelay3"_s);
        if (!words || !screenshotGroup || !delay) {
            qWarning("plugin layout: missing screenshot settings controls");
            return finish(window, false);
        }
        // Switching groups must commit a focused text editor and preserve changed values.
        QMetaObject::invokeMethod(words, "takeFocus");
        words->setProperty("text", u"preview-project,测试客户"_s);
        QMetaObject::invokeMethod(screenshotGroup, "clicked");
        settle(window);
        check(editor->captureSmartWords() == u"preview-project,测试客户", "switching category lost the text edit");
        QMetaObject::invokeMethod(delay, "clicked");
        QMetaObject::invokeMethod(expand, "clicked");
        QMetaObject::invokeMethod(expand, "clicked");
        check(editor->captureDelay() == 3 && card->property("currentGroup").toInt() == 1,
              "folding lost the setting or selected category");
        // Search must reveal the category, and Enter must retain it after clearing the query.
        const char* queries[] = {"PrtScn", "复制截图时同时贴图", "中键", "双击贴图", "智能打码", "工具条"};
        for (int group = 0; group < 6; ++group) {
            const QString query = QCoreApplication::translate("CaptureCard", queries[group]);
            input->setProperty("text", query);
            settle(window);
            say(u"capture search '%1': group %2 (wanted %3)"_s.arg(query).arg(card->property("currentGroup").toInt()).arg(group));
            check(card->property("expanded").toBool(), "search did not expand the capture card");
            check(card->property("currentGroup").toInt() == group, "search showed the wrong category");
            QMetaObject::invokeMethod(window, "goToCurrent");
            settle(window);
            check(window->property("currentPage").toInt() == pluginsPage && window->property("currentTab").toInt() == 0,
                  "search did not land on installed plugins");
            check(!window->property("searching").toBool(), "Enter did not leave search");
            check(card->property("currentGroup").toInt() == group, "leaving search lost the selected category");
        }
        grab(u"capture-search-landed"_s);
        return finish(window, saved);
    }

    // Every page, every tab: the window as tall as the page, so the
    // picture has all of it.
    bool saved = true;
    bool onePage = false;
    const int wantedPage = valueAfter(args, u"--page").toInt(&onePage);
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
            if (page == pluginsPage && tab == 0) { // the cards come folded: one unfolded too (翻译: settings, permissions)
                if (QObject* section = named(window, "pluginsSection"))
                    QMetaObject::invokeMethod(section, "expandAt", Q_ARG(QVariant, QVariant(1)));
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
            if (QObject* section = named(window, "favoritesSection")) {
                section->setProperty("editing", -1);
                section->setProperty("addingCommand", true);
                flick->setProperty("contentY", 0);
                settle(window, 3);
                saved = saveGrab(window, out.filePath(u"command-new.png"_s)) && saved;
                QObject* form = section->findChild<QObject*>(u"favoriteForm"_s);
                QObject* name = form ? form->findChild<QObject*>(u"favoriteName"_s) : nullptr;
                QObject* keyword = form ? form->findChild<QObject*>(u"favoriteKeyword"_s) : nullptr;
                QObject* commandBox = form ? form->findChild<QObject*>(u"favoriteCommand"_s) : nullptr;
                if (name && keyword && commandBox) {
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
    const Probe probes[] = {
        {u"无需面板"_s, window->property("clipboardPage").toInt(), 1},
        {u"插件"_s, pluginsPage, 0},
        {u"插件库"_s, pluginsPage, 1},
        {u"消息记录"_s, pluginsPage, 2}, // the 开发者 tab (developer mode is on in the preview)
        {u"API 密钥"_s, pluginsPage, 0}, // a plugin's own setting: its card unfolds (search-API 密钥.png)
        {u"AI"_s, aiPage, 0},
        {u"命令行"_s, aiPage, 0},
        {u"ws"_s, aiPage, 0},
        {u"command line"_s, aiPage, 0},
        {u"终端"_s, aiPage, 0},
        {u"收藏"_s, favoritesPage, 0},
        {u"网络共享"_s, 2, 3},
        {u"排序"_s, 3, 0},
        {u"显示修改日期"_s, 3, 1},
        {u"每次打开时清空搜索"_s, 3, 2},
        {u"文件类型"_s, 3, 3},
    };
    QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(0)), Q_ARG(QVariant, QVariant(0)));
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
    return finish(window, saved);
}

// The production delegates and model, with made-up folders and local Qt key
// events: no global shortcuts, file opening, or user's profile involved.
int runResults(const QStringList& args)
{
    const QDir out(valueAfter(args, kResultsFlag));
    QDir().mkpath(out.path());
    const QString baseline = valueAfter(args, u"--rows-source");
    SystemTheme::setUiSize(valueAfter(args, u"--size", u"standard"_s));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    QFont font = QGuiApplication::font();
    font.setStyleStrategy(QFont::NoSubpixelAntialias);
    QGuiApplication::setFont(font);
    QQmlApplicationEngine engine;
    engine.addImageProvider(u"fileicon"_s, new FileIconProvider);
    ResultModel results;
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
            Binding { target: previewResults; property: "highlightColor"; value: Theme.accent }
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
                y: 60; width: parent.width; height: 8 * Theme.rowHeight
                model: previewResults; currentIndex: -1
                delegate: %2ResultRow {
                    width: 800; height: Theme.rowHeight
                    hint: index + 1; hintTaken: index < 3
                    %3
                }
            }
        }
    )"_s.arg(baseline.isEmpty() ? QString() : u"import \"%1\" as Baseline"_s.arg(QUrl::fromLocalFile(baseline).toString()),
             baseline.isEmpty() ? QString() : u"Baseline."_s,
             baseline.isEmpty() ? u"hintOpacity: hints.hintOpacity"_s : QString());
    QQmlComponent component(&engine);
    component.setData(qml.toUtf8(), QUrl(u"qrc:/results-preview.qml"_s));
    auto* window = qobject_cast<QQuickWindow*>(component.create());
    if (!window) {
        say(component.errorString());
        return 3;
    }
    QTimer watchdog;
    armWatchdog(watchdog, 45);
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
    pump(150);
    grab(u"always"_s);
    if (!baseline.isEmpty())
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
    QDir(Settings::dataDir()).removeRecursively();
    say(u"%1 warning(s)"_s.arg(g_warnings));
    return saved ? std::min(g_warnings, 100) : 4;
}

} // namespace

bool wanted(const QStringList& args)
{
    return args.contains(kFlag) || args.contains(kSettingsFlag) || args.contains(kResultsFlag) || args.contains(kAiCardsFlag);
}

void note(const QString& line)
{
    say(line);
}

void prepare(const QStringList& args)
{
    const bool resultsMode = args.contains(kResultsFlag);
    const QString folder = valueAfter(args, resultsMode ? kResultsFlag : args.contains(kAiCardsFlag) ? kAiCardsFlag : kSettingsFlag);
    if (!folder.isEmpty()) {
        QDir().mkpath(folder);
        g_logPath = QDir(folder).filePath(u"preview.log"_s);
    } else {
        g_logPath = valueAfter(args, kFlag) + u".log"_s;
    }
    QFile::remove(g_logPath);
    say(u"preparing"_s);
    // The scale asked for, exactly: not on top of the screen's own.
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    qputenv("QT_SCALE_FACTOR", valueAfter(args, u"--scale", u"1"_s).toLatin1());
    const bool webForm = args.contains(u"--web-form"_s);
    // --renderer d3d11: the window as the desktop shows it (for counting edge pixels), not the software renderer's.
    const bool nativeWeb = (webForm || args.contains(QString(kSettingsFlag))) && valueAfter(args, u"--renderer") == u"d3d11";
    if (nativeWeb)
        qputenv("QSG_RHI_BACKEND", "d3d11");
    if (resultsMode || nativeWeb)
        qunsetenv("QT_QUICK_BACKEND"); // the same native text path as the launcher
    else
        qputenv("QT_QUICK_BACKEND", "software"); // renders without a window on screen
    qputenv("QT_QPA_PLATFORM", webForm && !nativeWeb ? "windows:fontengine=gdi" : "windows:fontengine=freetype");
    // Nothing of the user's is read or written: a profile folder of its own.
    Settings::useProfile(QDir::temp().filePath(u"WinShun-preview-"_s + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)));
}

int run(const QStringList& args)
{
    const bool settingsMode = args.contains(kSettingsFlag);
    const bool resultsMode = args.contains(kResultsFlag);
    const bool cardsMode = args.contains(kAiCardsFlag);
    if (valueAfter(args, resultsMode ? kResultsFlag : cardsMode ? kAiCardsFlag : settingsMode ? kSettingsFlag : kFlag).isEmpty()) {
        std::fprintf(stderr, "usage: WinShun --preview-plugins <file.png> [--dark] [--scale 1.5] [--dialog] [--page ai]\n"
                             "       WinShun --preview-settings <folder> [--dark] [--scale 1.5] [--tree] [--edges] [--renderer d3d11]\n");
        return 2;
    }
    g_previous = qInstallMessageHandler(countWarnings);
    g_edges = args.contains(u"--edges"_s) || args.contains(u"--tree"_s);
    QGuiApplication::styleHints()->setColorScheme(args.contains(u"--dark"_s) ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    SystemTheme::setBackdropAvailable(false); // plain colours, as a window without Mica
    return resultsMode ? runResults(args) : cardsMode ? runAiCards(args) : settingsMode ? runSettings(args) : runPlugins(args);
}

} // namespace ws::preview
