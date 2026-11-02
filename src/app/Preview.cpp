#include "Preview.h"
#include "ContentPaneProbe.h"

#include "Backup.h"
#include "App.h"
#include "Clipboard.h"
#include "ClipboardDisplay.h"
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
#include "Settings.h"
#include "SettingsEditor.h"
#include "SystemTheme.h"
#include "Switches.h"
#include "Updater.h"
#include "WindowFrame.h"
#include "DropPieHelper.h"
#include "Query.h"
#include "Host.h"
#include "platform/Integrity.h"
#include "platform/SiteIcons.h"
#include "platform/FileDialog.h"
#include "platform/FileManagers.h"
#include "platform/WindowEffects.h"
#include "platform/Foreground.h"
#include "platform/MessageWindow.h"
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
#include <QProcess>
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QLibraryInfo>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <stdexcept>
#include <QScreen>
#include <QScopeGuard>
#include <QSettings>
#include <QStyleHints>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QTime>
#include <QTranslator>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

namespace ws::preview {

namespace {

constexpr auto kFlag = u"--preview-plugins";
constexpr auto kSettingsFlag = u"--preview-settings";
constexpr auto kResultsFlag = u"--preview-results";
constexpr auto kAiCardsFlag = u"--preview-ai-cards";
constexpr auto kRuntimeFlag = u"--probe-runtime";

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

int finish(QQuickWindow* window, bool saved, QObject* controller = nullptr)
{
    window->hide();
    delete window;
    delete controller; // stop profile watchers before removing their directories
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
    const bool allow = args.contains(u"--dialog-allow"_s); // the 更新并允许 question instead
    const bool updatesOnly = args.contains(u"--updates"_s); // the 插件库 tab as the badge leads to it

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
    settings.uiSize = valueAfter(args, u"--size", settings.uiSize);
    SystemTheme::setUiSize(settings.uiSize);
    settings.mcp = true;
    settings.mcpPlugins = true;
    settings.taskbarSearchBox = true; // its style row
    settings.pasteFileEnabled = true; // its card's two ways in
    settings.dialogBar = true; // off for a new install: the rows they keep away
    settings.folderMenu = u"auto"_s;
    const QString shareOnline = u"\\\\192.168.1.10\\资料"_s;
    const QString shareSignIn = u"\\\\nas\\photos"_s;
    const QString shareWsl = u"\\\\wsl.localhost\\Ubuntu-24.04\\home\\me\\项目"_s;
    settings.shares = {{shareOnline, u"公司资料"_s}, {shareSignIn, {}}, {shareWsl, {}}};
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
        SystemTheme::setUiSize(editor->uiSize());
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
        shareStatus(shareWsl, ShareIndexer::State::Problem, share::Problem::WslStopped, 1230),
    });
    SettingsEditor::IndexFolderState indexState;
    indexState.folder = u"C:\\Users\\me\\AppData\\Local\\WinShun"_s;
    indexState.size = u"112 MB"_s;
    editor->setIndexFolderState(indexState);
    editor->setContentIndexStatus(u"已收录 337,215 个文件，其中 5,815 个文档"_s);
    editor->setHistoryCount(128);
    editor->setClipboardCount(args.contains(u"--clipboard-disabled"_s) ? -1 : 57);
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
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(2)), Q_ARG(QVariant, QVariant(3)));
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
        QMetaObject::invokeMethod(window, "goTo", Q_ARG(QVariant, QVariant::fromValue(named(window, "sharesRow"))));
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
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(2)), Q_ARG(QVariant, QVariant(3)));
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
        window->resize(args.contains(u"--compact"_s) ? QSize(760, 420) : QSize(920, 720));
        bool saved = true;
        const auto grab = [&](int tab, const QString& name) {
            QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(2)), Q_ARG(QVariant, QVariant(tab)));
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

    if (args.contains(u"--file-types"_s)) {
        const auto check = [](bool condition, const char* message) {
            if (!condition)
                qWarning("file types: %s", message);
        };
        check(!editor->setFileTypeKeyword(0, u"folder"_s).isEmpty(), "folder prefix collision was accepted");
        check(!editor->setFileTypeKeyword(0, u"two words"_s).isEmpty(), "invalid prefix was accepted");
        check(editor->setFileTypeKeyword(0, u":DOC"_s).isEmpty(), "custom prefix was rejected");
        check(editor->fileTypes().value(0).toMap().value(u"keyword"_s).toString() == u"doc", "prefix was not normalized");
        check(!editor->setFileTypeKeyword(-1, u"doc"_s).isEmpty(), "type prefix collision was accepted");
        check(editor->setFileTypeKeyword(0, {}).isEmpty(), "disabling a prefix failed");
        editor->resetFileTypes();
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(3)), Q_ARG(QVariant, QVariant(3)));
        window->resize(args.contains(u"--compact"_s) ? QSize(760, 420) : QSize(920, 720));
        settle(window, 3);
        bool saved = saveGrab(window, out.filePath(u"file-types-top.png"_s));
        flick->setProperty("contentY", std::max(0.0, flick->property("contentHeight").toDouble() - flick->property("height").toDouble()));
        settle(window, 3);
        saved = saveGrab(window, out.filePath(u"file-types-bottom.png"_s)) && saved;
        return finish(window, saved);
    }

    // Every page, every tab: the window as tall as the page, so the
    // picture has all of it.
    bool saved = true;
    bool onePage = false;
    const int wantedPage = valueAfter(args, u"--page").toInt(&onePage);
    if (args.contains(u"--preview-hotkeys"_s)) {
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(3)), Q_ARG(QVariant, QVariant(1)));
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
            if (page == pluginsPage && tab == 0) { // the cards come folded: one unfolded too (翻译: settings, permissions)
                if (QObject* section = named(window, "pluginsSection"))
                    QMetaObject::invokeMethod(section, "expandAt", Q_ARG(QVariant, QVariant(1)));
                if (auto* paste = window->findChild<QQuickItem*>(u"pasteFileSettingsCard"_s))
                    paste->setProperty("expanded", true);
                else
                    qWarning("preview: no 粘贴为文件 card on the 插件 page");
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
            if (page == 0 && tab == 2) {
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
            if (page == 3 && tab == 1) {
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
            if (QObject* section = named(window, "favoritesSection")) {
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
    const Probe probes[] = {
        {quickPasteProbe, window->property("clipboardPage").toInt(), 1},
        {QCoreApplication::translate("SettingsWindow", "插件"), pluginsPage, 0},
        {QCoreApplication::translate("SettingsWindow", "插件库"), pluginsPage, 1},
        {QCoreApplication::translate("PluginDevSection", "消息记录"), pluginsPage, 2}, // the 开发者 tab
        {PluginSettings::tr("API 密钥"), pluginsPage, 0}, // a plugin's own setting
        {QCoreApplication::translate("PasteFileCard", "粘贴为文件"), pluginsPage, 0}, // a built-in card
        // Shown as 内置功能 cards too, the rows of their own page first (open_settings searches them by title).
        {QCoreApplication::translate("SettingsWindow", "双击空白处弹出文件夹菜单"), 0, 2},
        {QCoreApplication::translate("SettingsWindow", "菜单里列出的内容"), 0, 2},
        {QCoreApplication::translate("SettingsWindow", "对话框旁的搜索框"), 0, 2},
        {QCoreApplication::translate("SettingsWindow", "在 Directory Opus 里直接打字搜索"), 0, 2},
        {u"AI"_s, aiPage, 0},
        {u"命令行"_s, aiPage, 0},
        {u"ws"_s, aiPage, 0},
        {u"command line"_s, aiPage, 0},
        {u"终端"_s, aiPage, 0},
        // "Favorites" also matches the opening-menu option; use this page's exact setting title.
        {QCoreApplication::translate("FavoritesSection", "拖文件时按 ` 键，弹出收藏圆盘"), favoritesPage, 0},
        {QCoreApplication::translate("SettingsWindow", "网络与 WSL 文件夹"), 2, 3},
        {QCoreApplication::translate("SettingsWindow", "排序"), 3, 0},
        {QCoreApplication::translate("SettingsWindow", "显示修改日期"), 3, 1},
        {QCoreApplication::translate("SettingsWindow", "显示文件大小"), 3, 1},
        {QCoreApplication::translate("SettingsWindow", "选中收藏文件夹时按 Tab"), 3, 1},
        {QCoreApplication::translate("SettingsWindow", "每次打开时清空搜索"), 3, 2},
        // "File types" also matches preferred types in English; its visible prefix is unambiguous.
        {args.contains(u"--english"_s) ? u":folder"_s : u"文件类型"_s, 3, 3},
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

void checkSearchMenuKeys(Launcher& launcher, QQuickWindow* window, FilePreview& preview, const QDir& output)
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
        check(!menuOpen() && launcher.query() == query, mode + u"starting IME composition closes the menu"_s);
        QCoreApplication::sendEvent(field, &ended);
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
            key(Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
            check(launcher.typingFolderOnly() != only, mode + u"Ctrl+Shift+D switches the current folder filter"_s);
            key(Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
            check(launcher.typingFolderOnly() == only, mode + u"Ctrl+Shift+D switches it back"_s);
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
    check(launcher.menuItems(0).size() == 1, u"settings expose only their applicable Open action"_s);
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

void checkDialogSearchKeys(IndexService& index, QQmlEngine* engine, const QDir& output)
{
    QTemporaryDir temporary;
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    SearchEngine search(&index, &apps, &places);
    History history(temporary.filePath(u"history.tsv"_s));
    DialogBar bar(&search, &history, nullptr, {});
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
    }
    QFile report(output.filePath(u"dialog-navigation.json"_s));
    check(report.open(QIODevice::WriteOnly), u"write the dialog regression report"_s);
    if (report.isOpen()) report.write(QJsonDocument(QJsonObject {{u"passed"_s, passed}, {u"checks"_s, checks}}).toJson());
    say(u"dialog search navigation: %1 checks, production DialogBarWindow/DialogBar, local Qt input, %2"_s
        .arg(checks.size()).arg(passed ? u"passed" : u"FAILED"));
    if (!passed)
        throw std::runtime_error("Production dialog search navigation regression failed; see dialog-navigation.json");
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
        for (const QString& size : {u"compact"_s, u"standard"_s, u"roomy"_s}) {
            SystemTheme::setUiSize(size);
            settle(window);
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
        window->setPosition(area.topLeft() + QPoint(24, 36));
        settle(window);
        const QSize size = window->size();
        click(WM_NCRBUTTONDOWN, WM_NCRBUTTONUP);
        const QPoint wanted(area.x() + (area.width() - size.width()) / 2,
            area.y() + (area.height() - size.height()) / 2);
        check((window->position() - wanted).manhattanLength() <= 2,
            mode + u"native right click centers on the current monitor work area"_s);
        check(window->size() == size && launcher.query() == query && !launcher.pinned() && settingsRequests == before + 2,
            mode + u"centering retains size, query and pin state without opening settings"_s);
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
        for (const QString& size : {u"compact"_s, u"standard"_s, u"roomy"_s}) {
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
void checkFileGallery(Launcher& launcher, IndexService& index, QQmlEngine* engine, const QDir& output)
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
    checkSearchMenuKeys(launcher, window, filePreview, output);
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
        }
        const auto click = [&](QQuickItem* item) {
            if (!item) { check(false, "click target missing"); return; }
            const QPointF at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
            QMouseEvent press(QEvent::MouseButtonPress, at, window->mapToGlobal(at.toPoint()), Qt::LeftButton, Qt::LeftButton, {});
            QMouseEvent release(QEvent::MouseButtonRelease, at, window->mapToGlobal(at.toPoint()), Qt::LeftButton, {}, {});
            QCoreApplication::sendEvent(window, &press);
            QCoreApplication::sendEvent(window, &release);
        };
        grid->setProperty("currentIndex", 5);
        const QString chosen = launcher.results()->pathAt(5);
        click(visualNamed(window->contentItem(), u"fileViewList"_s));
        settle(window);
        auto* view = window->property("view").value<QObject*>();
        check(!launcher.gallery() && view && launcher.results()->pathAt(view->property("currentIndex").toInt()) == chosen,
            "switching to list lost the current file");
        click(visualNamed(window->contentItem(), u"fileViewGallery"_s));
        settle(window);
        check(launcher.gallery() && launcher.results()->pathAt(grid->property("currentIndex").toInt()) == chosen,
            "switching back to gallery lost the current file");
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
    auto helper = std::make_unique<DropPieHelper>(&favorites,
        [&](const QString& title, const QString& text, auto&&) { notices.push_back(title + u": "_s + text); });
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
    const int warnings = g_warnings;
    const qint64 first = helper->state().value(u"pid"_s).toInteger();
    QElapsedTimer restarting;
    restarting.start();
    if (win32::UniqueHandle killed(::OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(first))); killed.valid())
        ::TerminateProcess(killed.get(), 1);
    const bool restarted = waitFor([first](const QJsonObject& state) {
        return state.value(u"pid"_s).toInteger() != first && state.value(u"folders"_s).toInt() == 3;
    }, 12000); // a second after it ended, then a start as slow as the first may be
    const qint64 restartMs = restarting.elapsed();
    g_warnings = warnings + std::max(0, g_warnings - warnings - 2);
    state = helper->state();
    win32::UniqueHandle child(::OpenProcess(SYNCHRONIZE, FALSE, DWORD(state.value(u"pid"_s).toInteger())));
    QElapsedTimer stopping;
    stopping.start();
    helper.reset();
    const bool stopped = child.valid() && ::WaitForSingleObject(child.get(), 10'000) == WAIT_OBJECT_0;
    state.insert(u"stop_ms"_s, stopping.elapsed());
    passed = passed && updatedFavorites && updatedAppearance && restarted && stopped && notices.isEmpty();
    state.insert(u"updatedFavorites"_s, updatedFavorites);
    state.insert(u"updatedAppearance"_s, updatedAppearance);
    state.insert(u"restarted"_s, restarted);
    state.insert(u"restart_ms"_s, restartMs);
    state.insert(u"stopped"_s, stopped);
    state.insert(u"passed"_s, passed);
    QFile report(output.filePath(u"drop-pie-helper.json"_s));
    if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(state).toJson());
    if (!passed) qWarning().noquote() << "Drop pie helper regression failed:" << notices << state;
    else say(u"drop pie: real ordinary-user helper, OLE target registration, favorite and appearance refresh, restart after it ends and shutdown checked"_s);
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

void checkLauncherFilters(QQuickWindow* window, QQmlEngine* qmlEngine, const QDir& output, bool gallery, bool previewOnly = false)
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
        checkFileGallery(launcher, index, qmlEngine, output);
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
    }
    const auto check = [](bool condition, const char* reason) {
        if (!condition)
            qWarning("launcher regression: %s", reason);
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
        {Launcher::LargestFirst, SortKey::Size, true}, {Launcher::SmallestFirst, SortKey::Size, false}};
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
    const auto tabHint = [&] { return launcher.keyHints().value(u"tab"_s).toString(); };
    const QString scopeHint = QCoreApplication::translate("ws::Launcher", "切换范围");
    const QString folderHint = QCoreApplication::translate("ws::Launcher", "进入文件夹");
    const auto hasBrowseAction = [&] {
        const QVariantList menu = launcher.menuItems(0);
        return std::ranges::any_of(menu, [](const QVariant& entry) {
            const QVariantMap item = entry.toMap();
            return item.contains(u"action"_s) && item.value(u"action"_s).toInt() == Launcher::BrowseFolder;
        });
    };
    showRow(folder);
    check(tabHint() == scopeHint && !launcher.autocomplete(0) && launcher.query().isEmpty(),
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
    check(tabHint() == scopeHint && !launcher.autocomplete(0) && hasBrowseAction(),
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
    check(tabHint() == scopeHint && !launcher.autocomplete(0), "turning the Tab setting off left stale behavior or hints");
    SearchResult suggestion {.name = u"Search suggestion"_s, .app = AppKind::Web,
        .keyword = u"example"_s, .words = u"report"_s, .suggestion = true};
    showRow(suggestion);
    check(launcher.autocomplete(0) && launcher.query() == u"example report", "web suggestion completion changed");
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
        checkFileGallery(launcher, index, qmlEngine, output);
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
    QMetaObject::invokeMethod(page, "closePreview");
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
    clipboard.setRowOptions(false, false);
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
    }
    check(!watcher.running(), "fixture must never start the native clipboard writer");
    window->hide();
    say(u"clipboard double-click preview and list resizing: real QML, temporary history; native clipboard untouched"_s);
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
    {
        const auto files = index.index();
        const auto lock = files->writeLock();
        const auto root = files->addRoot(QDir::toNativeSeparators(temporary.path()).toStdString());
        for (int i = 1; i <= 24; ++i) {
            const QString name = u"编号_%1"_s.arg(i, 2, 10, QLatin1Char('0'));
            check(QDir(temporary.path()).mkdir(name), "could not create isolated folder");
            files->add(root, name.toStdString(), EntryFlag::Directory);
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
    check(saveGrab(window, output.filePath(u"number-column-main.png"_s)), "main did not render");
    auto* firstRow = visualNamed(window->contentItem(), u"resultRow-0"_s);
    auto* icon = firstRow ? visualNamed(firstRow, u"resultIcon"_s) : nullptr;
    auto* text = firstRow ? visualNamed(firstRow, u"resultText"_s) : nullptr;
    if (!icon || !text || !view)
        return false;
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
    const double rowHeight = window->property("rowHeight").toDouble();
    view->setProperty("contentY", 11.25 * rowHeight);
    settle(window, 3);
    verifyRows();
    const int first = window->property("firstShownRow").toInt();
    check(first > 0, "list did not scroll");
    opened.clear();
    const auto* target = launcher.results()->at(first + 1);
    const QString scrolled = target ? target->path : QString();
    launcher.setResultOptions(KindOrder::FilesFirst, true, false, false, u"alt"_s);
    press(2, Qt::AltModifier);
    check(!scrolled.isEmpty() && opened == scrolled, "Alt+2 did not follow the scrolled numbering");
    check(saveGrab(window, output.filePath(u"number-column-scrolled.png"_s)), "scrolled results did not render");
    query(u"编号_0"_s);
    check(launcher.results()->count() > 0 && launcher.results()->count() < 24, "query did not replace the result set");
    verifyRows();
    // A programmatic refresh may retain the viewport. Its visible "1"
    // still means that first visible result, rather than absolute row 1.
    const int refreshedFirst = window->property("firstShownRow").toInt();
    const auto* refreshed = launcher.results()->at(refreshedFirst);
    const QString refreshedPath = refreshed ? refreshed->path : QString();
    opened.clear();
    press(1, Qt::AltModifier);
    check(!refreshedPath.isEmpty() && opened == refreshedPath, "refreshed numbering did not match Alt+1");
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
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    QFont font = QGuiApplication::font();
    font.setStyleStrategy(QFont::NoSubpixelAntialias);
    QGuiApplication::setFont(font);
    QQmlApplicationEngine engine;
    engine.addImageProvider(u"fileicon"_s, new FileIconProvider);
    if (args.contains(u"--clipboard-sizing"_s))
        return checkClipboardInteractions(&engine, out, {}, true) ? std::min(g_warnings, 100) : 4;
    if (args.contains(u"--content-pane"_s)) {
        engine.addImageProvider(u"thumbnail"_s, new ThumbnailProvider);
        return checkContentPane(&engine, out) ? std::min(g_warnings, 100) : 4;
    }
    if (args.contains(u"--search-workload"_s)) {
        engine.addImageProvider(u"thumbnail"_s, new ThumbnailProvider);
        return checkSearchWorkload(&engine, out) ? std::min(g_warnings, 100) : 4;
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
    armWatchdog(watchdog, 90); // GitHub #116's readouts wait about 30 s for a server that never answers
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
    if (args.contains(u"--switch-provider-probe"_s))
        return finish(window, checkSwitchProvider(window, out) && saved);
    if (args.contains(u"--number-column"_s))
        return finish(window, checkNumberColumn(&engine, out, valueAfter(args, u"--width", u"760"_s).toInt()) && saved);
    if (args.contains(u"--clipboard-send"_s))
        return finish(window, checkClipboardInteractions(&engine, out, {}) && saved);
    if (args.contains(u"--search-logo"_s)) {
        return finish(window, checkSearchLogo(&engine, out) && saved);
    }
    if (args.contains(u"--preview-hotkeys"_s) || args.contains(u"--search-navigation"_s)) {
        checkLauncherFilters(window, &engine, out, true, true);
        return finish(window, saved);
    }
    if (args.contains(u"--drop-pie-check"_s)) checkDropPieHelper(out);
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
                        if (sizeVisible && dateVisible && size->y() + size->height() > date->y())
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
    app.showFolderSearch(root, tab, false, folder);
    QQuickWindow* window = nullptr;
    Launcher* launcher = nullptr;
    for (auto* candidate : QGuiApplication::allWindows()) {
        if (auto* controller = candidate->property("launcher").value<Launcher*>()) {
            window = qobject_cast<QQuickWindow*>(candidate); launcher = controller; break;
        }
    }
    if (!window || !launcher) return false;
    const QString prefix = folder + u"\\ "_s;
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
    RECT overlap {};
    check(!::IntersectRect(&overlap, &searchRect, &sourceRect), "search keeps clear of the Explorer fixture");
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
    app.showFolderSearch(root, secondTab, false, folder);
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
    check(launcher->keyHints().value(u"second"_s).toString() == u"打开位置"_s,
        "a child of the folder keeps the plain 打开位置 hint");
    const QString nestedFile = QDir::toNativeSeparators(folder + u"\\child\\nested-only.txt"_s);
    launcher->setQuery(prefix + u"nested-only"_s);
    waiting.restart();
    while ((launcher->searching() || launcher->results()->count() < 1) && waiting.elapsed() < 5000)
        pump(20);
    pump(300);
    check(launcher->keyHints().value(u"second"_s).toString() == u"在原窗口定位"_s,
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
    app.showFolderSearch(lister, list, true, folder);
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
    // The tab and the stand-in come to the front as the launcher goes: the
    // warnings about its losing the front that this makes are no fault.
    pump(500);
    g_warnings = warned;
    return ok;
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
    // The fixture's independent observer confirms Blender actually changed.
    bar->choose(row, false);
    waiting.restart();
    bool selectedInDialog = false;
    while (!selectedInDialog && waiting.elapsed() < 5000) {
        pump(50);
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
    return selectedInDialog;
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
    const bool capturing = action == u"capture" || action == u"decline" || action == u"offline" || action == u"incompatible" || action == u"retry";
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
    window->resize(660, 480);
    settle(window);
    auto* flick = qobject_cast<QQuickItem*>(visualNamed(window->contentItem(), u"onboardingScroll"_s));
    if (flick)
        flick->setProperty("contentY", std::max(0.0, flick->property("contentHeight").toDouble() - flick->height()));
    settle(window);
    auto* capture = qobject_cast<QQuickItem*>(visualNamed(window->contentItem(), u"onboardingCapture"_s));
    check(u"last_feature_reachable_at_minimum_size"_s, capture && flick
        && capture->mapToItem(flick, QPointF(0, capture->height())).y() <= flick->height() + 1);
    check(u"compact_screenshot"_s, saveGrab(window, out.filePath(u"onboarding-compact.png"_s)));
    window->resize(guideSize);
    if (flick) flick->setProperty("contentY", 0);
    settle(window);
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
            auto* permissions = window->findChild<QObject*>(u"onboardingPermissions"_s);
            if (permissions && permissions->property("open").toBool()) {
                permissionSeen = true;
                settle(window);
                check(u"permission_screenshot"_s, saveGrab(window, out.filePath(u"onboarding-permissions.png"_s)));
                window->resize(660, 480);
                settle(window);
                auto* permissionCard = permissions->findChild<QQuickItem*>(u"pluginDialogCard"_s);
                auto* permissionScroll = permissions->findChild<QQuickItem*>(u"pluginDialogScroll"_s);
                auto* acceptButton = permissions->findChild<QQuickItem*>(u"pluginDialogAccept"_s);
                check(u"compact_permission_card_fits"_s, permissionCard
                    && QRectF(QPointF(), window->size()).contains(permissionCard->mapRectToItem(window->contentItem(), QRectF(QPointF(), permissionCard->size()))));
                if (acceptButton) acceptButton->forceActiveFocus(Qt::TabFocusReason);
                settle(window);
                check(u"compact_permission_button_reachable"_s, acceptButton
                    && QRectF(QPointF(), window->size()).contains(acceptButton->mapRectToItem(window->contentItem(), QRectF(QPointF(), acceptButton->size()))));
                check(u"compact_permission_screenshot"_s, saveGrab(window, out.filePath(u"onboarding-permissions-compact.png"_s)));
                window->resize(guideSize);
                if (permissionScroll) permissionScroll->setProperty("contentY", 0);
                settle(window);
                if (action == u"decline")
                    QMetaObject::invokeMethod(permissions, "reject");
                else
                    QMetaObject::invokeMethod(permissions, "accept", Q_ARG(QVariant, QVariant(false)));
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
        check(u"permission_is_explicit"_s, accepted || action == u"decline" ? permissionSeen : !permissionSeen);
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
    settings.explorerTyping = false;
    settings.opusTyping = false;
    settings.folderMenu = u"off"_s;
    settings.folderMenuHotkey.clear();
    settings.taskbarSearchBox = false;
    settings.autoUpdate = false;
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
    QDir().mkpath(settings.indexFolders.front());
    if (typingProbe) {
        settings.opusTypingScope = args.contains(u"--opus-everywhere"_s) ? u"all"_s : u"folder"_s;
        settings.save();
        const QDir files(settings.indexFolders.front());
        for (int i = 1; i <= 3; ++i) {
            QFile file(files.filePath(u"follow_%1.txt"_s.arg(i)));
            if (!file.open(QIODevice::WriteOnly) || file.write("isolated Explorer selection fixture\n") < 0) return 4;
        }
        files.mkdir(u"child"_s);
        QFile nested(files.filePath(u"child/nested-only.txt"_s));
        if (!nested.open(QIODevice::WriteOnly)) return 4;
    }
    QFile fixture(settings.indexFolders.front() + u"/startup-probe.txt"_s);
    if (!fixture.open(QIODevice::WriteOnly))
        return 4;
    fixture.write("WinShun isolated startup probe\n");
    fixture.close();
    const QString dialogSample = QDir(settings.indexFolders.front()).filePath(u"WinShun-dialog-验证.blend"_s);
    if (settings.dialogBar && !QFile::copy(valueAfter(args, u"--file-dialog-sample"), dialogSample))
        return 4;

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
    settings.save(); // prepared profiles normally skip onboarding
    if (args.contains(u"--clipboard-display-probe"_s) || args.contains(u"--clipboard-display-service-probe"_s)) {
        ClipStore store(Settings::dataDir() + u"/clipboard"_s);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!store.open(now)) return 4;
        for (const QString& text : {u"alpha clipboard display fixture"_s, u"beta clipboard display fixture"_s}) {
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
        int expectedWarnings = 0;
        if (onboardingAction == u"offline" || onboardingAction == u"retry") {
            QFile log(g_logPath);
            if (log.open(QIODevice::ReadOnly))
                expectedWarnings = static_cast<int>(log.readAll().count("WARNING: Plugin library: no list from http://127.0.0.1:1/unreachable.json"));
        }
        report.insert(u"warnings"_s, g_warnings);
        report.insert(u"expected_network_diagnostics"_s, expectedWarnings);
        const int unexpected = std::max(0, g_warnings - expectedWarnings);
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
    {
        App app;
        QElapsedTimer since;
        since.start();
        if (!app.start({.background = true, .clipboardDisplayProbe = args.contains(u"--clipboard-display-probe"_s)}))
            return 4;
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
    // The scale asked for, exactly: not on top of the screen's own.
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    qputenv("QT_SCALE_FACTOR", valueAfter(args, u"--scale", u"1"_s).toLatin1());
    const bool webForm = args.contains(u"--web-form"_s);
    // --renderer d3d11: the window as the desktop shows it (for counting edge pixels), not the software renderer's.
    const bool nativeWeb = (webForm || args.contains(QString(kSettingsFlag)) || args.contains(kRuntimeFlag)) && valueAfter(args, u"--renderer") == u"d3d11";
    if (nativeWeb)
        qputenv("QSG_RHI_BACKEND", "d3d11");
    if (resultsMode || nativeWeb)
        qunsetenv("QT_QUICK_BACKEND"); // the same native text path as the launcher
    else
        qputenv("QT_QUICK_BACKEND", "software"); // renders without a window on screen
    qputenv("QT_QPA_PLATFORM", resultsMode || nativeWeb ? "windows:fontengine=freetype" : "windows:fontengine=gdi");
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
        std::fprintf(stderr, "usage: WinShun --preview-plugins <file.png> [--dark] [--scale 1.5] [--dialog | --dialog-allow] [--updates] [--page ai]\n"
                             "       WinShun --preview-settings <folder> [--dark] [--scale 1.5] [--tree] [--edges] [--renderer d3d11]\n");
        return 2;
    }
    g_previous = qInstallMessageHandler(countWarnings);
    g_edges = args.contains(u"--edges"_s) || args.contains(u"--tree"_s);
    QGuiApplication::styleHints()->setColorScheme(args.contains(u"--dark"_s) ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    SystemTheme::setBackdropAvailable(false); // plain colours, as a window without Mica
    return runtimeMode ? runRuntime(args) : resultsMode ? runResults(args) : cardsMode ? runAiCards(args) : settingsMode ? runSettings(args) : runPlugins(args);
}

} // namespace ws::preview
