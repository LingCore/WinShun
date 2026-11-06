#include "InteractionOptimizationProbe.h"
#include "App.h"
#include "Host.h"
#include "AppCatalog.h"
#include "Clipboard.h"
#include "DoubleTapAction.h"
#include "FolderMenu.h"
#include "History.h"
#include "IndexService.h"
#include "Launcher.h"
#include "Placement.h"
#include "Preview.h"
#include "SettingsEditor.h"
#include "SystemCatalog.h"
#include "SystemTheme.h"
#include "WindowFrame.h"
#include "platform/ClipboardWatcher.h"

#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QScopeGuard>

using namespace Qt::StringLiterals;

namespace ws::preview {
namespace {
struct Checks {
    bool passed = true;
    QJsonArray checks;
    void operator()(bool okay, const QString& name)
    {
        passed &= okay;
        checks.append(QJsonObject {{u"check"_s, name}, {u"passed"_s, okay}});
        note((okay ? u"ok: "_s : u"FAILED: "_s) + name);
    }
    bool save(const QDir& output, const QString& name) const
    {
        QFile file(output.filePath(name));
        return file.open(QIODevice::WriteOnly)
            && file.write(QJsonDocument(QJsonObject {{u"passed"_s, passed}, {u"checks"_s, checks}}).toJson()) > 0 && passed;
    }
};

void settle()
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 250)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

QQuickItem* find(QQuickItem* root, const QString& name)
{
    if (root->objectName() == name) return root;
    for (auto* child : root->childItems())
        if (auto* result = find(child, name)) return result;
    return nullptr;
}

void offscreen(QQuickWindow* window)
{
    window->setFlag(Qt::WindowDoesNotAcceptFocus);
    window->setProperty("_q_showWithoutActivating", true);
    const int offset = static_cast<int>(24000 / window->devicePixelRatio());
    window->setPosition(offset, offset);
    window->show();
    settle();
}

void key(QQuickItem* target, int value)
{
    QKeyEvent down(QEvent::KeyPress, value, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &down);
    QKeyEvent up(QEvent::KeyRelease, value, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &up);
}
}

bool checkDoubleTapSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output)
{
    Checks check;
    const QString action = doubletap::encode(u"winshun.capture"_s, u"Win+Shift+S"_s);
    editor->setDoubleTapPluginActions({QVariantMap {{u"value"_s, action}, {u"text"_s, u"截图录屏 · Win+Shift+S"_s}}});
    QMetaObject::invokeMethod(window, "showSetting", Q_ARG(QVariant, 0), Q_ARG(QVariant, 0),
        Q_ARG(QVariant, QCoreApplication::translate("SettingsWindow", "双击后做什么")));
    settle();
    auto* picker = find(window->contentItem(), u"doubleTapActionPicker"_s);
    check(picker && picker->isVisible(), u"double-tap destination is reachable in production settings"_s);
    if (!picker) return false;
    check(picker->property("count").toInt() == 3, u"launcher, clipboard and registered plugin action are listed"_s);
    const auto choose = [&](int index) {
        picker->setProperty("currentIndex", index);
        QMetaObject::invokeMethod(picker, "activated", Q_ARG(int, index));
        settle();
    };
    choose(1);
    Settings saved;
    saved.load();
    check(saved.doubleTapAction == u"clipboard" && editor->doubleTapAction() == u"clipboard",
        u"choosing clipboard saves through the real control and editor"_s);
    const QString chord = saved.clipboardHotkey;
    editor->setDoubleTapKey(u"shift"_s);
    saved.load();
    check(saved.doubleTapKey == u"shift" && saved.doubleTapAction == u"clipboard" && saved.clipboardHotkey == chord,
        u"changing the tap key retains the destination and conventional shortcut"_s);
    choose(2);
    saved.load();
    check(saved.doubleTapAction == action, u"plugin identity and original shortcut round trip"_s);
    check(window->grabWindow().save(output.filePath(u"double-tap-settings.png"_s)), u"capture double-tap settings"_s);
    auto* popup = picker->property("popup").value<QObject*>();
    check(popup != nullptr, u"destination uses a standard keyboard-accessible popup"_s);
    if (popup) {
        QMetaObject::invokeMethod(popup, "open");
        settle();
        check(popup->property("visible").toBool() && popup->property("height").toReal() <= 280,
            u"action menu opens with bounded height"_s);
        check(window->grabWindow().save(output.filePath(u"double-tap-options.png"_s)), u"capture themed action menu"_s);
        QMetaObject::invokeMethod(popup, "close");
        settle();
    }
    editor->setDoubleTapPluginActions({});
    settle();
    check(editor->doubleTapAction() == action && picker->property("currentIndex").toInt() == 2
        && picker->property("currentText").toString().contains(u"winshun.capture"),
        u"unavailable plugin keeps the selection and displays its identity"_s);
    editor->setDoubleTapAction(u"plugin:unknown/Alt+X"_s);
    check(editor->doubleTapAction() == action, u"unregistered destinations cannot be selected"_s);
    choose(0);
    editor->setDoubleTapKey(u"ctrl"_s);
    key(picker, Qt::Key_Down);
    settle();
    check(editor->doubleTapAction() == u"clipboard", u"keyboard selection uses the same save path"_s);
    check(picker->mapToItem(window->contentItem(), QPointF()).x() + picker->width() <= window->width(),
        u"destination control fits the settings width"_s);
    check(!window->isActive(), u"settings validation never activates the window"_s);
    return check.save(output, u"double-tap-settings.json"_s);
}

bool checkWindowSearchAndMenu(QQmlEngine* engine, const QDir& output)
{
    Checks check;
    const QString originalStyle = SystemTheme::uiStyle();
    const auto restore = qScopeGuard([&] { SystemTheme::setUiStyle(originalStyle); });
    for (const QString& style : {u"fluent"_s, u"brand"_s, u"tactile"_s}) {
        SystemTheme::setUiStyle(style);
        FolderMenu menu(nullptr, nullptr);
        QQmlComponent component(engine, u"WinShun"_s, u"FolderMenuWindow"_s);
        std::unique_ptr<QObject> root(component.createWithInitialProperties({{u"folderMenu"_s, QVariant::fromValue(&menu)}}));
        auto* window = qobject_cast<QQuickWindow*>(root.get());
        check(window != nullptr, style + u": folder menu loads: "_s + component.errorString());
        if (!window) return false;
        menu.setWindow(window);
        QVariantList entries;
        for (int i = 0; i < 25; ++i)
            entries.append(QVariantMap {{u"text"_s, u"项目资料 %1 · 客户交付和年度报告"_s.arg(i + 1)},
                {u"glyph"_s, u"\uE8B7"_s}, {u"action"_s, i}});
        menu.previewEntries(entries);
        window->setProperty("availableArea", QRectF(0, 0, 640, 340));
        offscreen(window);
        const int height = window->property("itemHeight").toInt();
        const bool compact = SystemTheme::uiSize() == u"compact";
        check(height == (style == u"fluent" ? 36 : 34) + (compact ? -6 : 0), style + u": menu density follows the selected size"_s);
        check(window->width() == (compact ? 300 : 360) && window->height() <= 324,
            style + u": compact width and bounded scrolling fit the work area"_s);
        QMetaObject::invokeMethod(window, "handleKey", Q_ARG(QVariant, Qt::Key_End), Q_ARG(QVariant, QString()));
        settle();
        check(window->property("highlighted").toInt() == 24, style + u": keyboard reaches the last item"_s);
        auto* last = find(window->contentItem(), u"folderMenuEntry24"_s);
        check(last && last->mapToItem(window->contentItem(), QPointF()).y() + last->height() <= window->height() + 1,
            style + u": last row is fully visible after scrolling"_s);
        check(window->grabWindow().save(output.filePath(style + u"-compact-menu.png"_s)), style + u": capture menu"_s);
        check(!window->isActive(), style + u": menu stays inactive"_s);
        window->hide();
    }
    SystemTheme::setUiStyle(originalStyle);
    QTemporaryDir profile;
    IndexService index(profile.filePath(u"index"_s), {});
    AppCatalog apps;
    apps.setApps(std::make_shared<const AppList>());
    SystemCatalog places;
    places.setPlaces(std::make_shared<const PlaceList>());
    SearchEngine search(&index, &apps, &places);
    History history(profile.filePath(u"history.txt"_s));
    Launcher launcher(&index, &apps, &places, &search, &history);
    launcher.setContentOptions(false, {}, {}, true, true);
    launcher.results()->setThumbnails(false);
    ClipStore store(profile.filePath(u"clipboard"_s));
    store.open(QDateTime::currentMSecsSinceEpoch());
    ClipboardWatcher watcher({.record = false}, {});
    Clipboard clipboard(&store, &watcher);
    Placement placement(profile.filePath(u"state.ini"_s), u"Launcher"_s);
    WindowFrame frame(false);
    QQmlComponent component(engine, u"WinShun"_s, u"Main"_s);
    std::unique_ptr<QObject> root(component.createWithInitialProperties({
        {u"launcher"_s, QVariant::fromValue(&launcher)}, {u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"placement"_s, QVariant::fromValue(&placement)}, {u"frame"_s, QVariant::fromValue(&frame)}}));
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    check(window != nullptr, u"production launcher loads: "_s + component.errorString());
    if (!window) return false;
    window->create();
    frame.setWindow(window);
    launcher.setWindow(window);
    offscreen(window);
    SearchResult active;
    active.app = AppKind::Window;
    active.name = u"年度报告.docx — 文档编辑器"_s;
    active.path = u"winshun-window:0/0/0"_s;
    active.target = QCoreApplication::applicationFilePath();
    SearchResult file;
    file.name = u"年度报告.docx"_s;
    file.path = u"D:\\项目资料\\年度报告.docx"_s;
    launcher.results()->assign({active, file}, {u"报告"_s});
    launcher.setCurrentRow(1);
    launcher.setCurrentRow(0);
    settle();
    const auto at = launcher.results()->index(0);
    check(launcher.results()->data(at, ResultModel::SourceRole).toString()
        == QCoreApplication::translate("ws::ResultModel", "已打开"), u"live results have an explicit already-open badge"_s);
    check(!launcher.results()->data(at, ResultModel::RevealableRole).toBool()
        && !launcher.results()->data(at, ResultModel::ElevatableRole).toBool(),
        u"live windows offer no misleading file reveal or administrator action"_s);
    check(launcher.keyHints().value(u"enter"_s).toString() == Launcher::tr("切换到窗口"),
        u"Enter describes switching instead of launching a duplicate"_s);
    check(window->grabWindow().save(output.filePath(u"open-window-results.png"_s)), u"capture open-window results"_s);
    launcher.trigger(0, Launcher::Open);
    check(launcher.statusText() == Launcher::tr("窗口已关闭或内容已改变，请重新搜索"),
        u"stale window activation stays in the launcher with actionable feedback"_s);
    check(!window->isActive(), u"launcher validation never activates a user window"_s);
    return check.save(output, u"window-search-menu.json"_s);
}

struct DoubleTapActionsProbe {
    static bool run(App& app, const QDir& output)
    {
        Checks check;
        const QString id = u"test.tap"_s;
        const QString shortcut = u"Ctrl+Alt+Shift+F24"_s;
        QElapsedTimer timer;
        timer.start();
        while (!app.m_pluginTapActions.contains(id) && timer.elapsed() < 10000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        check(app.m_pluginHost && app.m_pluginTapActions.contains(id)
            && app.m_pluginTapActions.at(id).contains(shortcut),
            u"real plugin registers a shortcut through the production App tool"_s);
        if (!app.m_pluginHost || !app.m_pluginTapActions.contains(id))
            return check.save(output, u"double-tap-plugin.json"_s);
        // Runtime startup also constructs its preview windows. Establish a
        // hidden starting state before checking which surface the tap opens.
        if (app.m_window) app.m_window->hide();
        if (app.m_clipWindow) app.m_clipWindow->hide();
        settle();
        check((!app.m_window || !app.m_window->isVisible()) && (!app.m_clipWindow || !app.m_clipWindow->isVisible()),
            u"search and clipboard are hidden before the plugin gesture"_s);
        const Settings original = app.m_settings;
        const auto restore = qScopeGuard([&] { app.m_settings = original; });
        app.m_settings.doubleTapAction = doubletap::encode(id, shortcut);
        app.m_settings.doubleCtrlPauseInGames = false;
        app.m_settings.doubleCtrlPauseInFullScreen = false;
        app.m_settings.doubleCtrlExcludedApps.clear();
        const QDir data(app.m_pluginHost->dataDir(id));
        const auto received = [&] {
            QFile file(data.filePath(u"received.json"_s));
            return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).array() : QJsonArray();
        };
        check(received().isEmpty(), u"plugin has received no action before the gesture"_s);
        KeyListener::Tap tap;
        tap.key = VK_LCONTROL;
        tap.time = tap.toldAt = ::GetTickCount();
        tap.firstPress = tap.time - 100;
        app.onDoubleCtrl(tap); // the same callback the verified detector delivers
        timer.restart();
        while (received().isEmpty() && timer.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const QJsonArray events = received();
        check(events.size() == 1 && events.first().toString() == shortcut,
            u"double-tap callback delivers exactly one matching action through the real plugin process"_s);
        check((!app.m_window || !app.m_window->isVisible()) && (!app.m_clipWindow || !app.m_clipWindow->isVisible()),
            u"plugin gesture does not also open the search or clipboard window"_s);
        QFile evidence(output.filePath(u"plugin-received.json"_s));
        check(evidence.open(QIODevice::WriteOnly) && evidence.write(QJsonDocument(events).toJson()) > 0,
            u"save the plugin's own received-action evidence"_s);
        app.m_pluginHost->setEnabled(id, false);
        timer.restart();
        while (app.m_pluginTapActions.contains(id) && timer.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        check(!app.m_pluginTapActions.contains(id), u"stopping the plugin removes its executable double-tap actions"_s);
        return check.save(output, u"double-tap-plugin.json"_s);
    }
};

bool checkDoubleTapPlugin(App& app, const QDir& output)
{
    return DoubleTapActionsProbe::run(app, output);
}
}
