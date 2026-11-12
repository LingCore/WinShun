#include "NavigationProbe.h"

#include "AppCatalog.h"
#include "Clipboard.h"
#include "ClipboardDisplay.h"
#include "FolderMenu.h"
#include "History.h"
#include "IndexService.h"
#include "Launcher.h"
#include "Placement.h"
#include "PluginReadme.h"
#include "PluginSettings.h"
#include "Host.h"
#include "Preview.h"
#include "SearchEngine.h"
#include "Settings.h"
#include "SettingsEditor.h"
#include "SystemCatalog.h"
#include "SystemTheme.h"
#include "WindowFrame.h"
#include "platform/ClipboardWatcher.h"
#include "platform/WindowEffects.h"

#include <QCoreApplication>
#include <cstdlib>
#include <QBuffer>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <memory>
#include <dwmapi.h>

using namespace Qt::StringLiterals;

namespace ws::preview {
namespace {
struct Checks {
    bool passed = true;
    QJsonArray rows;
    void operator()(bool okay, const QString& name)
    {
        passed &= okay;
        rows.append(QJsonObject {{u"check"_s, name}, {u"passed"_s, okay}});
        note((okay ? u"ok: "_s : u"FAILED: "_s) + name);
    }
};

QQuickItem* item(QQuickItem* root, const QString& name)
{
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems())
        if (auto* found = item(child, name))
            return found;
    return nullptr;
}

void settle(QQuickWindow* window)
{
    for (int i = 0; i < 3; ++i) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 70)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        window->grabWindow();
    }
}

void showOffscreen(QQuickWindow* window)
{
    window->setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    window->setProperty("_q_showWithoutActivating", true);
    const int offset = static_cast<int>(std::floor(24000 / window->devicePixelRatio()));
    window->setPosition(offset, offset);
    window->show();
    settle(window);
}

void key(QQuickItem* field, int code, Qt::KeyboardModifiers modifiers = {})
{
    QKeyEvent event(QEvent::KeyPress, code, modifiers);
    QCoreApplication::sendEvent(field, &event);
}

void clipboardGroups(QQmlEngine* engine, const QDir& output, bool flipped, Checks& check)
{
    QTemporaryDir profile;
    ClipStore store(profile.filePath(u"clipboard"_s));
    if (!store.open(QDateTime::currentMSecsSinceEpoch())) {
        check(false, u"open isolated clipboard database"_s);
        return;
    }
    const qint64 work = store.addGroup(u"工作资料"_s);
    const qint64 personal = store.addGroup(u"Personal notes"_s);
    const qint64 empty = store.addGroup(u"空组"_s);
    store.addGroup(u"客户项目与交付文档：一个很长的分组名称，检查省略与对齐"_s);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 workEntry = store.add(ClipCapture {.kind = ClipKind::Text, .text = u"meeting: work agenda"_s}, now);
    const qint64 privateEntry = store.add(ClipCapture {.kind = ClipKind::Text, .text = u"meeting: private plans"_s}, now + 1);
    store.setGroup(std::span(&workEntry, 1), work);
    store.setGroup(std::span(&privateEntry, 1), personal);
    ClipboardWatcher watcher({.record = false}, {});
    Clipboard clipboard(&store, &watcher);
    clipboard.setRowOptions(false, false, SystemTheme::uiSize());
    clipboard.setState(true, false, u"Win+V"_s);
    clipboard.setActive(true);
    Placement placement(profile.filePath(u"state.ini"_s), u"Clipboard"_s);
    WindowFrame frame(false);
    QQmlComponent component(engine, u"WinShun"_s, u"ClipboardWindow"_s);
    std::unique_ptr<QObject> root(component.createWithInitialProperties({
        {u"clipboard"_s, QVariant::fromValue(&clipboard)}, {u"placement"_s, QVariant::fromValue(&placement)},
        {u"frame"_s, QVariant::fromValue(&frame)}}));
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    check(window != nullptr, u"production clipboard window loads: "_s + component.errorString());
    if (!window) return;
    window->create();
    frame.setWindow(window);
    clipboard.handleShown();
    showOffscreen(window);
    auto* page = item(window->contentItem(), u"clipboardPage"_s);
    auto* field = item(window->contentItem(), u"clipboardSearchField"_s);
    auto* picker = item(window->contentItem(), u"clipboardGroupPicker"_s);
    auto* list = item(window->contentItem(), u"clipboardGroupResults"_s);
    check(page && field && picker && list, u"group picker and search field exist"_s);
    if (!page || !field || !picker || !list) return;
    page->setProperty("above", flipped);
    clipboard.setLayoutOptions(flipped, false);
    field->setProperty("text", u"meeting"_s);
    key(field, Qt::Key_G, Qt::ControlModifier);
    check(page->property("findingGroup").toBool() && clipboard.query() == u"meeting", u"Ctrl+G opens groups and preserves the content query"_s);
    field->setProperty("text", u"工作"_s);
    settle(window);
    check(picker->property("matches").toList().size() == 1 && clipboard.query() == u"meeting", u"group names are searched independently"_s);
    key(field, Qt::Key_Return);
    settle(window);
    check(!page->property("findingGroup").toBool() && clipboard.query().isEmpty()
        && clipboard.categories().value(clipboard.category()).toMap().value(u"group"_s).toLongLong() == work,
        u"Enter enters the group and clears only the group-name query"_s);
    field->setProperty("text", u"meeting"_s);
    settle(window);
    check(clipboard.items()->count() == 1 && clipboard.items()->idAt(0) == workEntry,
        u"subsequent content search excludes the same words in other groups"_s);
    key(field, Qt::Key_G, Qt::ControlModifier);
    field->setProperty("text", u"不存在的分组"_s);
    key(field, Qt::Key_Return);
    check(page->property("findingGroup").toBool() && clipboard.query() == u"meeting", u"Enter on an empty group search cannot paste a content row"_s);
    key(field, Qt::Key_Escape);
    check(!page->property("findingGroup").toBool() && field->property("text").toString() == u"meeting"
        && clipboard.items()->idAt(0) == workEntry, u"Escape restores the scoped content search"_s);
    emit clipboard.groupSearchRequested(); // same route as owned Ctrl+G above a file dialog
    field->setProperty("text", u"PERSONAL"_s);
    settle(window);
    check(picker->property("matches").toList().size() == 1, u"group matching ignores Latin case"_s);
    field->setProperty("text", QString());
    settle(window);
    const int before = list->property("currentIndex").toInt();
    key(field, flipped ? Qt::Key_Up : Qt::Key_Down);
    check(list->property("currentIndex").toInt() == before + 1, u"arrow navigation follows the group's visual direction"_s);
    check(window->grabWindow().save(output.filePath(u"clipboard-groups.png"_s)), u"capture the group picker"_s);
    field->setProperty("text", u"空组"_s);
    key(field, Qt::Key_Return);
    settle(window);
    check(!page->property("findingGroup").toBool() && clipboard.items()->count() == 0
        && clipboard.categories().value(clipboard.category()).toMap().value(u"group"_s).toLongLong() == empty,
        u"an empty group opens as an empty group"_s);
    check(!clipboard.enterGroup(999999) && clipboard.items()->count() == 0, u"a removed or unknown group cannot change the current scope"_s);
    clipboard.enterGroup(work);
    field->setProperty("text", u"meeting"_s);
    settle(window);
    check(window->grabWindow().save(output.filePath(u"clipboard-group-content.png"_s)), u"capture searching within the chosen group"_s);
    check(!window->isActive(), u"clipboard checks keep the native window inactive"_s);
    window->hide();
}

void panelSidebar(QQmlEngine* engine, const QDir& output, Checks& check)
{
    QTemporaryDir profile;
    IndexService index(profile.filePath(u"index"_s), {});
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    SearchEngine search(&index, &apps, &places);
    History history(profile.filePath(u"history.txt"_s));
    Launcher launcher(&index, &apps, &places, &search, &history);
    launcher.setPanel(true);
    launcher.setPanelSidebarVisible(true);
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
    check(window != nullptr, u"production full search window loads: "_s + component.errorString());
    if (!window) return;
    window->create();
    window->resize(1100, 650);
    frame.setWindow(window);
    launcher.setWindow(window);
    showOffscreen(window);
    auto* sidebar = item(window->contentItem(), u"panelSidebar"_s);
    auto* toggle = item(window->contentItem(), u"panelSidebarToggle"_s);
    auto* results = item(window->contentItem(), u"panelResultList"_s);
    auto* field = item(window->contentItem(), u"panelSearchField"_s);
    check(sidebar && toggle && results && field, u"full-panel controls are available"_s);
    if (!sidebar || !toggle || !results || !field) return;
    const QPointF buttonPoint = toggle->mapToScene(QPointF(toggle->width() / 2, toggle->height() / 2));
    POINT screenPoint {std::lround(buttonPoint.x() * window->devicePixelRatio()),
        std::lround(buttonPoint.y() * window->devicePixelRatio())};
    const auto handle = reinterpret_cast<HWND>(window->winId());
    ::ClientToScreen(handle, &screenPoint);
    check(::SendMessageW(handle, WM_NCHITTEST, 0, MAKELPARAM(screenPoint.x, screenPoint.y)) == HTCLIENT,
        u"native title-bar hit testing treats the sidebar toggle as a control, not a drag area"_s);
    launcher.setQuery(u"report"_s);
    launcher.setDateChoice(2);
    settle(window);
    const qreal shownWidth = results->width();
    check(sidebar->isVisible() && sidebar->width() > 0, u"filter sidebar starts visible"_s);
    check(window->grabWindow().save(output.filePath(u"panel-sidebar.png"_s)), u"capture the expanded filter sidebar"_s);
    QMetaObject::invokeMethod(toggle, "activate");
    settle(window);
    check(!launcher.panelSidebarVisible() && !sidebar->isVisible() && sidebar->width() == 0
        && results->width() >= shownWidth + 289, u"hiding filters gives their width to results"_s);
    check(launcher.query() == u"report" && launcher.filtersActive() && launcher.dateChoice() == 2,
        u"hiding filters keeps the query and active criteria"_s);
    auto* indicator = item(window->contentItem(), u"panelActiveFiltersIndicator"_s);
    check(indicator && indicator->isVisible(), u"active filters remain visibly indicated while their sidebar is hidden"_s);
    check(window->activeFocusItem() == field, u"the sidebar control returns keyboard focus to search"_s);
    Launcher reopened(&index, &apps, &places, &search, &history);
    check(!reopened.panelSidebarVisible(), u"sidebar visibility survives a new controller"_s);
    check(window->grabWindow().save(output.filePath(u"panel-no-sidebar.png"_s)), u"capture full-width results with the sidebar hidden"_s);
    QMetaObject::invokeMethod(toggle, "activate");
    settle(window);
    check(sidebar->isVisible() && std::abs(results->width() - shownWidth) < 1, u"showing the sidebar restores the layout"_s);
    launcher.setPanel(false);
    settle(window);
    check(!toggle->isVisible(), u"the compact launcher does not show the panel-only control"_s);
    check(!window->isActive(), u"panel checks keep the native window inactive"_s);
    window->hide();
}

// The panel's quick filters: the chips, a real click on one, the rows it
// leaves, several at once, 清除全部, and the + with what is typed.
void panelQuickFilters(QQmlEngine* engine, const QDir& output, Checks& check)
{
    QTemporaryDir profile;
    IndexService index(profile.filePath(u"index"_s), {});
    {
        const auto files = index.index();
        const auto lock = files->writeLock();
        const EntryId drive = files->addRoot("Q:");
        const EntryId work = files->add(drive, "work", EntryFlag::Directory);
        const EntryId bin = files->add(drive, "$RECYCLE.BIN", EntryFlag::Directory);
        files->add(work, "wsquick report.docx", 0);
        files->add(work, "wsquick report.log", 0);
        files->add(bin, "wsquick report.docx", 0);
        files->add(bin, "wsquick report.log", 0);
    }
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    SearchEngine search(&index, &apps, &places);
    History history(profile.filePath(u"history.txt"_s));
    Launcher launcher(&index, &apps, &places, &search, &history);
    launcher.setPanel(true);
    launcher.setPanelSidebarVisible(true);
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
    check(window != nullptr, u"quick filters: the search window loads: "_s + component.errorString());
    if (!window) return;
    window->create();
    window->resize(1100, 650);
    frame.setWindow(window);
    launcher.setWindow(window);
    showOffscreen(window);
    const auto files = [&] {
        QStringList paths;
        for (int i = 0; i < launcher.results()->count(); ++i) {
            if (const QString path = launcher.results()->pathAt(i); path.contains(u"wsquick"_s))
                paths.append(path);
        }
        paths.sort();
        return paths;
    };
    const auto click = [&](QQuickItem* target) {
        const QPointF at = target->mapToScene({target->width() / 2, target->height() / 2});
        QMouseEvent press(QEvent::MouseButtonPress, at, window->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, {});
        QMouseEvent release(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), Qt::LeftButton, {}, {});
        QCoreApplication::sendEvent(window, &press);
        QCoreApplication::sendEvent(window, &release);
        settle(window);
    };
    const auto on = [&](int i) { return launcher.activeQuickFilters().contains(launcher.quickFilters().value(i).toMap().value(u"name"_s).toString()); };

    // None yet: the group is there all the same, its + saying what it is for.
    auto* add = item(window->contentItem(), u"panelQuickFilterAdd"_s);
    check(add && add->isVisible() && add->property("text").toString().size() > 1, u"quick filters: with none saved the + names itself"_s);
    check(item(window->contentItem(), u"panelQuickFilter_0"_s) == nullptr, u"quick filters: no chip before one is saved"_s);

    launcher.setQuickFilters({{u"排除回收站"_s, u"!$RECYCLE.BIN\\"_s}, {u"只看日志"_s, u"ext:log"_s}});
    launcher.setQuery(u"wsquick"_s);
    settle(window);
    auto* first = item(window->contentItem(), u"panelQuickFilter_0"_s);
    auto* second = item(window->contentItem(), u"panelQuickFilter_1"_s);
    add = item(window->contentItem(), u"panelQuickFilterAdd"_s);
    check(first && second && add && first->isVisible() && second->isVisible(), u"quick filters: a chip for each saved filter"_s);
    if (!first || !second || !add) return;
    check(add->property("text").toString() == u"+", u"quick filters: the + is brief beside chips"_s);
    auto* sidebar = item(window->contentItem(), u"panelSidebar"_s);
    for (QQuickItem* chip : {first, second, add}) {
        const qreal right = chip->mapToItem(sidebar, QPointF(chip->width(), 0)).x();
        check(sidebar && right <= sidebar->width() - 8, u"quick filters: chips stay inside the side bar"_s);
    }
    check(files().size() == 4 && !launcher.filtersActive(), u"quick filters: off, every match is listed"_s);

    click(first);
    check(on(0) && !on(1) && launcher.filtersActive(), u"quick filters: a click switches one on"_s);
    check(first->property("active").toBool(), u"quick filters: the chip shows it is on"_s);
    check(files() == QStringList({u"Q:\\work\\wsquick report.docx"_s, u"Q:\\work\\wsquick report.log"_s}),
        u"quick filters: what the filter excludes leaves the rows: "_s + files().join(u" | "_s));
    check(launcher.query() == u"wsquick", u"quick filters: the search box keeps what was typed"_s);
    click(second);
    check(on(0) && on(1), u"quick filters: a second one goes on beside the first"_s);
    check(files() == QStringList({u"Q:\\work\\wsquick report.log"_s}), u"quick filters: two on, both hold: "_s + files().join(u" | "_s));
    check(window->grabWindow().save(output.filePath(u"panel-quick-filters.png"_s)), u"capture the quick filters, two on"_s);
    click(first);
    check(!on(0) && on(1), u"quick filters: a click on one that is on switches it off"_s);
    check(files() == QStringList({u"Q:\\$RECYCLE.BIN\\wsquick report.log"_s, u"Q:\\work\\wsquick report.log"_s}),
        u"quick filters: the rest holds alone: "_s + files().join(u" | "_s));

    // Nothing typed: the panel's list, narrowed all the same.
    launcher.setQuery({});
    settle(window);
    check(files() == QStringList({u"Q:\\$RECYCLE.BIN\\wsquick report.log"_s, u"Q:\\work\\wsquick report.log"_s}),
        u"quick filters: with nothing typed the list is narrowed: "_s + files().join(u" | "_s));

    // Edited in the settings while on: its rows, in place; removed: off.
    launcher.setQuery(u"wsquick"_s);
    launcher.setQuickFilters({{u"排除回收站"_s, u"!$RECYCLE.BIN\\"_s}, {u"只看日志"_s, u"ext:docx"_s}});
    settle(window);
    check(on(1) && files() == QStringList({u"Q:\\$RECYCLE.BIN\\wsquick report.docx"_s, u"Q:\\work\\wsquick report.docx"_s}),
        u"quick filters: one edited while on takes effect at once: "_s + files().join(u" | "_s));
    launcher.setQuickFilters({{u"排除回收站"_s, u"!$RECYCLE.BIN\\"_s}});
    settle(window);
    check(!launcher.filtersActive() && files().size() == 4, u"quick filters: one removed is off"_s);

    // 清除全部 switches them off with the other filters.
    launcher.toggleQuickFilter(0);
    launcher.setSizeChoice(1);
    settle(window);
    check(launcher.filtersActive() && on(0), u"quick filters: on again"_s);
    launcher.clearFilters();
    settle(window);
    check(!on(0) && !launcher.filtersActive() && launcher.sizeChoice() == 0 && files().size() == 4, u"quick filters: 清除全部 switches them off"_s);

    // The +: the settings, with what is typed as a new one's query when it can be one.
    QStringList drafts;
    QObject::connect(&launcher, &Launcher::quickFilterSettingsRequested, window, [&](const QString& draft) { drafts.append(draft); });
    launcher.setQuery(u"!node_modules\\ ext:js"_s);
    settle(window);
    click(item(window->contentItem(), u"panelQuickFilterAdd"_s));
    launcher.setQuery(u"regex:("_s);
    settle(window);
    click(item(window->contentItem(), u"panelQuickFilterAdd"_s));
    check(drafts == QStringList({u"!node_modules\\ ext:js"_s, QString()}), u"quick filters: the + hands over a usable query only: "_s + drafts.join(u" | "_s));
    check(!window->isActive(), u"quick filter checks keep the native window inactive"_s);
    window->hide();
}

void displayGroups(Checks& check)
{
    QTemporaryDir profile;
    ClipStore store(profile.filePath(u"clipboard"_s));
    store.open(QDateTime::currentMSecsSinceEpoch());
    const qint64 group = store.addGroup(u"Personal notes"_s);
    // Empty history also makes a failed group-navigation assertion incapable
    // of copying or pasting into the user's real clipboard.
    ClipboardWatcher watcher({.record = false}, {});
    Clipboard clipboard(&store, &watcher);
    clipboard.setState(true, false, u"Win+V"_s);
    clipboard.setActive(true);
    QQuickWindow source;
    source.create();
    const int offset = static_cast<int>(std::floor(24000 / source.devicePixelRatio()));
    Placement placement(profile.filePath(u"state.ini"_s), u"Clipboard"_s);
    source.setGeometry(offset, offset, 480, 640);
    ClipboardDisplay display(&clipboard, &source, &placement, true);
    display.start();
    const auto pump = [](int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    };
    QElapsedTimer startup;
    startup.start();
    while (!display.ready() && startup.elapsed() < 6000) pump(20);
    check(display.ready(), u"independent clipboard display authenticates with the isolated controller"_s);
    if (!display.ready()) return;
    DWORD cloak = 0;
    const bool cloaked = SUCCEEDED(::DwmGetWindowAttribute(display.handle(), DWMWA_CLOAKED, &cloak, sizeof cloak)) && cloak != 0;
    check(cloaked, u"test display is cloaked before it can show"_s);
    if (!cloaked) return;
    check(display.show(), u"independent display opens without activation"_s);
    pump(300);
    RECT bounds {};
    const bool invisible = SUCCEEDED(::DwmGetWindowAttribute(display.handle(), DWMWA_CLOAKED, &cloak, sizeof cloak))
        && cloak != 0 && ::GetForegroundWindow() != display.handle();
    check(invisible, u"independent display stays cloaked and never takes foreground focus"_s);
    ::GetWindowRect(display.handle(), &bounds);
    note(u"display bounds %1,%2; source %3,%4; peer: %5"_s.arg(bounds.left).arg(bounds.top)
        .arg(source.x()).arg(source.y()).arg(display.probeOutput()));
    if (!invisible) { display.hide(); pump(50); return; }
    clipboard.setQuery(u"old content"_s);
    pump(100);
    emit clipboard.groupSearchRequested();
    pump(100);
    display.key({.key = Qt::Key_P, .text = u"PERSONAL"_s});
    pump(100);
    check(clipboard.query() == u"old content", u"display-side group typing does not mutate the controller's content query"_s);
    display.key({.key = Qt::Key_Return});
    pump(300);
    check(clipboard.categories().value(clipboard.category()).toMap().value(u"group"_s).toLongLong() == group
        && clipboard.query().isEmpty(), u"display Enter reaches enterGroup over IPC and acknowledges the new scope"_s);
    display.key({.key = Qt::Key_M, .text = u"meeting"_s});
    pump(200);
    check(clipboard.query() == u"meeting", u"display returns to content search after entering the group"_s);
    display.key({.key = Qt::Key_G, .modifiers = Qt::ControlModifier});
    pump(100);
    display.key({.key = Qt::Key_X, .text = u"missing group"_s});
    display.key({.key = Qt::Key_Escape});
    pump(200);
    check(clipboard.query() == u"meeting" && display.active(), u"display Escape leaves group search without closing or losing the content query"_s);
    note(u"display final query '%1', category %2; peer: %3"_s.arg(clipboard.query()).arg(clipboard.category()).arg(display.probeOutput()));
    display.hide();
    pump(50);
}

void folderGroups(QQmlEngine* engine, const QDir& output, Checks& check)
{
    FolderMenu menu(nullptr, nullptr);
    QVariantList children;
    for (int i = 0; i < 12; ++i)
        children.append(QVariantMap {{u"text"_s, u"项目资料 %1"_s.arg(i + 1)}, {u"action"_s, i}, {u"glyph"_s, u"\uE8B7"_s}});
    const auto entries = FolderMenu::groupEntries(u"最近的文件夹"_s, u"\uE81C"_s, children, 3);
    QQmlComponent component(engine, u"WinShun"_s, u"FolderMenuWindow"_s);
    std::unique_ptr<QObject> root(component.createWithInitialProperties({{u"folderMenu"_s, QVariant::fromValue(&menu)}}));
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    check(window != nullptr, u"folder menu with direct rows loads: "_s + component.errorString());
    if (!window) return;
    const int offset = static_cast<int>(std::floor(24000 / window->devicePixelRatio()));
    root->setProperty("availableArea", QRectF(offset, offset, 1200, 1000));
    root->setProperty("popupAnchor", QPointF(offset + 8, offset + 8));
    root->setProperty("page", QVariantMap {{u"text"_s, u"文件夹菜单"_s}, {u"children"_s, entries}});
    showOffscreen(window);
    QMetaObject::invokeMethod(root.get(), "handleKey", Q_ARG(QVariant, Qt::Key_Down), Q_ARG(QVariant, QString()));
    check(root->property("highlighted").toInt() == 1, u"keyboard navigation skips the inline section heading"_s);
    check(window->grabWindow().save(output.filePath(u"folder-direct.png"_s)), u"capture direct folders and More"_s);
    QMetaObject::invokeMethod(root.get(), "activate", Q_ARG(QVariant, 4));
    settle(window);
    const auto page = root->property("page").toMap();
    check(page.value(u"pageTitle"_s).toString() == u"最近的文件夹" && page.value(u"children"_s).toList().size() == 9,
        u"More opens the remaining folders under their original group title"_s);
    check(window->grabWindow().save(output.filePath(u"folder-more.png"_s)), u"capture remaining folder rows"_s);
    window->hide();
}
} // namespace

bool checkNavigationWorkflows(QQmlEngine* engine, const QDir& output, bool flipped)
{
    Checks check;
    QObject context;
    int warnings = 0;
    QObject::connect(engine, &QQmlEngine::warnings, &context, [&](const QList<QQmlError>& errors) { warnings += errors.size(); });
    clipboardGroups(engine, output, flipped, check);
    displayGroups(check);
    panelSidebar(engine, output, check);
    panelQuickFilters(engine, output, check);
    folderGroups(engine, output, check);
    check(warnings == 0, u"production QML emits no warnings"_s);
    QFile file(output.filePath(u"navigation-workflows.json"_s));
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(QJsonObject {{u"passed"_s, check.passed}, {u"checks"_s, check.rows}}).toJson());
    return check.passed;
}

bool checkClipboardAlignment(QQmlEngine* engine, const QDir& output, bool flipped)
{
    Checks check;
    QTemporaryDir profile;
    ClipStore store(profile.filePath(u"clipboard"_s));
    store.open(QDateTime::currentMSecsSinceEpoch());
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 11; i >= 0; --i) {
        if (i == 1 || i == 3 || i == 10) {
            const QSize size = i == 1 ? QSize(80, 180) : QSize(320, 180);
            QImage image(size, QImage::Format_RGB32);
            image.fill(i == 1 ? QColor(u"#2783b8"_s) : QColor(u"#397761"_s));
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            store.add(ClipCapture {.kind = ClipKind::Image, .imageData = bytes, .hash = QByteArray::number(i),
                .width = size.width(), .height = size.height(), .source = u"测试图片"_s}, now + 12 - i);
        } else {
            store.add(ClipCapture {.kind = ClipKind::Text, .text = u"第 %1 条：文字与图片属于各自独立的条目"_s.arg(i + 1),
                .source = u"测试内容"_s}, now + 12 - i);
        }
    }
    ClipboardWatcher watcher({.record = false}, {});
    Clipboard clipboard(&store, &watcher);
    clipboard.setRowOptions(false, false, SystemTheme::uiSize());
    clipboard.setNumberHints(u"always"_s);
    clipboard.setState(true, false, u"Win+V"_s);
    clipboard.setActive(true);
    Placement placement(profile.filePath(u"state.ini"_s), u"Clipboard"_s);
    WindowFrame frame(false);
    QQmlComponent component(engine, u"WinShun"_s, u"ClipboardWindow"_s);
    std::unique_ptr<QObject> root(component.createWithInitialProperties({
        {u"clipboard"_s, QVariant::fromValue(&clipboard)}, {u"placement"_s, QVariant::fromValue(&placement)},
        {u"frame"_s, QVariant::fromValue(&frame)}}));
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    check(window != nullptr, u"mixed clipboard production window loads: "_s + component.errorString());
    if (!window) return false;
    window->create();
    frame.setWindow(window);
    clipboard.handleShown();
    clipboard.setWindowHeight(640);
    showOffscreen(window);
    auto* page = item(window->contentItem(), u"clipboardPage"_s);
    auto* list = item(window->contentItem(), u"clipboardEntries"_s);
    check(page && list, u"clipboard list and layout controls exist"_s);
    if (!page || !list) return false;
    page->setProperty("above", flipped);
    clipboard.setLayoutOptions(flipped, false);
    const auto at = [&](int index) {
        QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, index), Q_ARG(int, 0));
        settle(window);
        return item(list, u"clipboardRow-%1"_s.arg(index));
    };
    auto* textRow = at(0);
    auto* firstIcon = textRow ? item(textRow, u"clipboardIconSlot"_s) : nullptr;
    const qreal leading = firstIcon ? firstIcon->x() : -1;
    check(firstIcon && leading > 16, u"numbered content keeps a separate leading gutter"_s);
    auto* picture = at(1);
    auto* slot = picture ? item(picture, u"clipboardIconSlot"_s) : nullptr;
    auto* thumbnail = picture ? item(picture, u"clipboardThumbnail"_s) : nullptr;
    check(slot && thumbnail && thumbnail->property("ready").toBool() && slot->x() == leading && thumbnail->x() == 0,
        u"portrait thumbnail starts at the same leading edge as text icons"_s);
    check(thumbnail && slot && thumbnail->width() < slot->width() && thumbnail->height() <= slot->height(),
        u"portrait preview keeps its full aspect ratio without cropping or enlargement"_s);
    auto* separator = picture ? item(picture, u"clipboardRowSeparator"_s) : nullptr;
    check(separator && separator->isVisible() && separator->x() == leading
        && (flipped ? separator->y() == 0 : separator->y() + separator->height() == picture->height()),
        u"entry separator follows the layout direction and leading edge"_s);
    at(0);
    check(window->grabWindow().save(output.filePath(u"clipboard-mixed.png"_s)), u"capture mixed text and portrait/landscape images"_s);
    const int screenHeight = window->maximumHeight();
    window->setMaximumHeight(1400); // simulate a tall viewport, without changing the user's display
    clipboard.setWindowHeight(1300);
    at(0); // all twelve rows fit; only nine have number shortcuts
    auto* later = at(10);
    auto* laterIcon = later ? item(later, u"clipboardIconSlot"_s) : nullptr;
    check(later && later->property("hint").toInt() == 0 && laterIcon && laterIcon->x() == leading,
        u"rows beyond the first nine retain the same alignment"_s);
    clipboard.setNumberHints(u"held"_s);
    settle(window);
    check(laterIcon && laterIcon->x() == leading, u"fading number hints does not shift later rows"_s);
    clipboard.setNumberHints(u"off"_s);
    settle(window);
    check(laterIcon && laterIcon->x() == leading - 28, u"turning number hints off releases the gutter consistently"_s);
    window->setMaximumHeight(screenHeight);
    clipboard.setWindowHeight(640);
    clipboard.setRowOptions(false, true, SystemTheme::uiSize());
    at(0);
    picture = at(1);
    slot = picture ? item(picture, u"clipboardIconSlot"_s) : nullptr;
    thumbnail = picture ? item(picture, u"clipboardThumbnail"_s) : nullptr;
    check(slot && slot->width() == 208 && slot->height() == 130 && thumbnail && thumbnail->x() == 0,
        u"large-picture mode keeps its preview dimensions and left alignment"_s);
    check(window->grabWindow().save(output.filePath(u"clipboard-large-pictures.png"_s)), u"capture large pictures"_s);
    check(!window->isActive(), u"clipboard alignment checks never activate the native window"_s);
    window->hide();
    QFile file(output.filePath(u"clipboard-alignment.json"_s));
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(QJsonObject {{u"passed"_s, check.passed}, {u"checks"_s, check.rows}}).toJson());
    return check.passed;
}

bool checkOptimizationSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output)
{
    Checks check;
    const int plugins = window->property("pluginsPage").toInt();
    QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, plugins), Q_ARG(QVariant, 0));
    settle(window);
    auto* section = item(window->contentItem(), u"pluginsSection"_s);
    auto* builtins = item(window->contentItem(), u"pluginBuiltinsRow"_s);
    auto* toggle = item(window->contentItem(), u"pluginBuiltinsToggle"_s);
    auto* cards = item(window->contentItem(), u"pluginBuiltinCards"_s);
    auto* installed = item(window->contentItem(), u"installedPluginsRow"_s);
    auto* search = item(window->contentItem(), u"settingsSearchBox"_s);
    check(section && builtins && toggle && cards && installed && search, u"production plugin fold controls load"_s);
    if (!section || !builtins || !toggle || !cards || !installed || !search) return false;
    const auto top = [&] { return installed->mapToItem(window->contentItem(), QPointF()).y(); };
    check(!editor->pluginBuiltinsExpanded() && !cards->isVisible(), u"built-in cards start folded"_s);
    const qreal foldedTop = top();
    check(window->grabWindow().save(output.filePath(u"plugins-folded.png"_s)), u"capture installed plugins within reach"_s);
    key(toggle, Qt::Key_Space);
    settle(window);
    Settings saved;
    saved.load();
    check(saved.pluginBuiltinsExpanded && cards->isVisible() && top() > foldedTop + 200,
        u"Space expands built-ins and persists the preference"_s);
    QMetaObject::invokeMethod(section, "resetForReopen");
    settle(window);
    check(cards->isVisible(), u"reopening settings retains an explicitly expanded group"_s);
    check(window->grabWindow().save(output.filePath(u"plugins-expanded.png"_s)), u"capture expanded built-ins"_s);
    key(toggle, Qt::Key_Return);
    settle(window);
    saved.load();
    check(!saved.pluginBuiltinsExpanded && !cards->isVisible() && std::abs(top() - foldedTop) < 1,
        u"Enter folds the group and restores the installed-plugin position"_s);
    QMetaObject::invokeMethod(search, "setText", Q_ARG(QVariant, QCoreApplication::translate("PluginsSection", "内置功能")));
    settle(window);
    check(cards->isVisible() && !editor->pluginBuiltinsExpanded(), u"search reveals matching built-ins without changing the preference"_s);
    QMetaObject::invokeMethod(search, "clear");
    settle(window);
    check(!cards->isVisible(), u"clearing the search restores the folded preference"_s);
    QMetaObject::invokeMethod(builtins, "revealRequested");
    settle(window);
    check(cards->isVisible() && !editor->pluginBuiltinsExpanded(), u"jumping to a built-in reveals it without changing the preference"_s);
    QMetaObject::invokeMethod(section, "resetForReopen");
    settle(window);
    check(!cards->isVisible(), u"temporary search reveal clears when settings reopen"_s);
    // The installed plugins' own filter (GitHub #275): from a number of them
    // on; the preview's few are under it.
    auto* filter = item(window->contentItem(), u"installedPluginFilter"_s);
    auto* noMatch = item(window->contentItem(), u"installedPluginNoMatch"_s);
    check(filter && noMatch && !filter->isVisible(), u"a short list of plugins has no filter box"_s);
    if (!filter || !noMatch) return false;
    section->setProperty("installedFilter", u"lingcore.translate"_s);
    settle(window);
    const int all = section->property("installedShown").toInt();
    check(all >= 6, u"a filter with no box to show it leaves the list whole"_s);
    section->setProperty("filterFrom", 1);
    settle(window);
    check(filter->isVisible() && filter->property("text").toString() == u"lingcore.translate"
            && section->property("installedShown").toInt() == 1,
        u"the installed filter finds a plugin by its id"_s);
    section->setProperty("installedFilter", u" Translate  LINGCORE "_s);
    settle(window);
    check(section->property("installedShown").toInt() == 1, u"every word typed counts, case and order aside"_s);
    section->setProperty("installedFilter", u"lingcore"_s);
    settle(window);
    const int some = section->property("installedShown").toInt(); // by the id or anything else they are found by
    check(some >= 2 && some < all && !noMatch->isVisible()
            && installed->property("description").toString().contains(QString::number(all))
            && installed->property("description").toString().contains(QString::number(some)),
        u"the row says how many of the plugins fit"_s);
    check(window->grabWindow().save(output.filePath(u"plugins-filter.png"_s)), u"capture the filtered installed plugins"_s);
    section->setProperty("installedFilter", u"no-such-plugin"_s);
    settle(window);
    check(section->property("installedShown").toInt() == 0 && noMatch->isVisible(), u"nothing found is said, not left blank"_s);
    QMetaObject::invokeMethod(search, "setText", Q_ARG(QVariant, QCoreApplication::translate("PluginsSection", "内置功能")));
    settle(window);
    check(section->property("installedShown").toInt() == all && !noMatch->isVisible(),
        u"searching the settings sets the installed filter aside"_s);
    QMetaObject::invokeMethod(search, "clear");
    section->setProperty("installedFilter", QString());
    section->setProperty("filterFrom", 8);
    settle(window);
    check(section->property("installedShown").toInt() == all && !filter->isVisible(), u"the filter cleared restores the list"_s);
    SettingsEditor::IndexAccessState access;
    access.administrator = access.portable = access.serviceInstalled = access.serviceInstallable = true;
    editor->setIndexAccessState(access);
    editor->setIndexAccess(u"direct"_s);
    QMetaObject::invokeMethod(window, "showSetting", Q_ARG(QVariant, 2), Q_ARG(QVariant, 2),
        Q_ARG(QVariant, QCoreApplication::translate("SettingsWindow", "索引方式")));
    settle(window);
    auto* service = item(window->contentItem(), u"indexServiceRow"_s);
    check(service && service->isVisible() && service->property("title").toString() == QCoreApplication::translate("SettingsWindow", "索引服务"),
        u"service management does not label an adjacent-data installation as a portable edition"_s);
    check(window->grabWindow().save(output.filePath(u"index-access.png"_s)), u"capture the startup-permission explanation"_s);
    check(!window->isActive(), u"settings probes keep the native window inactive"_s);
    QFile file(output.filePath(u"optimization-settings.json"_s));
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(QJsonObject {{u"passed"_s, check.passed}, {u"checks"_s, check.rows}}).toJson());
    return check.passed;
}

bool checkPureSearch(QQmlEngine* engine, const QDir& output)
{
    Checks check;
    QTemporaryDir profile;
    IndexService index(profile.filePath(u"index"_s), {});
    {
        const auto lock = index.index()->writeLock();
        const auto drive = index.index()->addRoot("D:");
        index.index()->add(drive, "report.txt", 0);
    }
    AppCatalog apps;
    apps.setApps(std::make_shared<const AppList>());
    SystemCatalog places;
    places.setPlaces(std::make_shared<const PlaceList>());
    SearchEngine search(&index, &apps, &places);
    History history(profile.filePath(u"history.txt"_s));
    Launcher launcher(&index, &apps, &places, &search, &history);
    launcher.results()->setThumbnails(false);
    launcher.setContentOptions(false, {}, {}, true, true);
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
    check(window != nullptr, u"pure search loads production Main: "_s + component.errorString());
    if (!window) return false;
    window->create();
    frame.setWindow(window);
    launcher.setWindow(window);
    placement.setWindow(window);
    placement.rememberWidth(760, 640);
    showOffscreen(window);
    auto* field = item(window->contentItem(), u"searchField"_s);
    auto* footer = item(window->contentItem(), u"launcherFooter"_s);
    auto* tabs = item(window->contentItem(), u"searchScopeTabs"_s);
    auto* expand = item(window->contentItem(), u"searchExpandButton"_s);
    check(field && footer && tabs && expand, u"production controls are available"_s);
    if (!field || !footer || !tabs || !expand) return false;
    check(footer->isVisible() && tabs->isVisible() && expand->isVisible(), u"default keeps familiar controls"_s);
    launcher.setPureSearch(true);
    launcher.setQuery(u"   "_s);
    settle(window);
    check(window->property("pureEmpty").toBool() && !footer->isVisible() && !tabs->isVisible() && !expand->isVisible(),
        u"whitespace-only query hides auxiliary controls"_s);
    check(launcher.results()->count() == 0 && !launcher.searching(), u"empty pure search has no hidden actionable results or pending search"_s);
    const int emptyHeight = window->height();
    check(field->width() > window->width() * 0.8, u"input reclaims the controls' width"_s);
    check(emptyHeight < 100, u"empty launcher keeps no results or footer gap"_s);
    launcher.setQuery({});
    settle(window);
    check(window->grabWindow().save(output.filePath(u"pure-empty.png"_s)), u"capture empty pure search"_s);
    QInputMethodEvent composing(u"zhong"_s, {});
    QCoreApplication::sendEvent(field, &composing);
    check(!window->property("pureEmpty").toBool(), u"IME composition restores controls before committing the query"_s);
    QInputMethodEvent endComposition;
    QCoreApplication::sendEvent(field, &endComposition);
    launcher.setQuery(u"report"_s);
    settle(window);
    check(launcher.results()->count() == 1, u"typing searches the real index"_s);
    check(!window->property("pureEmpty").toBool() && footer->isVisible() && tabs->isVisible() && expand->isVisible(),
        u"typing restores all existing controls"_s);
    check(window->height() > emptyHeight, u"results expand the window"_s);
    check(window->grabWindow().save(output.filePath(u"pure-results.png"_s)), u"capture populated search"_s);
    launcher.setQuery({});
    settle(window);
    check(window->height() == emptyHeight && launcher.results()->count() == 0, u"clearing returns to one input and removes stale results"_s);
    bool dismissed = false;
    const auto connection = QObject::connect(&launcher, &Launcher::dismissRequested, [&] { dismissed = true; });
    key(field, Qt::Key_Return);
    check(!dismissed, u"Enter on the empty input cannot open a hidden result"_s);
    QObject::disconnect(connection);
    key(field, Qt::Key_Tab);
    check(launcher.scope() == Launcher::Files, u"hidden scope controls keep Tab navigation"_s);
    launcher.setPanel(true);
    settle(window);
    check(!window->property("pureEmpty").toBool(), u"full search panel is not collapsed by pure mode"_s);
    launcher.setPanel(false);
    launcher.setFolderSearch(true);
    settle(window);
    check(!window->property("pureEmpty").toBool() && footer->isVisible(), u"folder search keeps its context and controls"_s);
    launcher.setFolderSearch(false);
    launcher.setInTaskbarBox(true);
    settle(window);
    check(!window->property("pureEmpty").toBool(), u"taskbar input keeps its existing presentation"_s);
    launcher.setInTaskbarBox(false);
    launcher.setPureSearch(false);
    settle(window);
    check(footer->isVisible() && tabs->isVisible(), u"switching off restores the original presentation"_s);
    check(!window->isActive(), u"probe never activates the user's desktop"_s);
    QFile report(output.filePath(u"pure-search.json"_s));
    if (!report.open(QIODevice::WriteOnly)) return false;
    report.write(QJsonDocument(QJsonObject {{u"passed"_s, check.passed}, {u"checks"_s, check.rows}}).toJson());
    return check.passed;
}

bool checkResultRowSpacing(QQmlEngine* engine, const QDir& output)
{
    Checks check;
    QTemporaryDir profile;
    IndexService index(profile.filePath(u"index"_s), {});
    {
        const auto lock = index.index()->writeLock();
        const auto drive = index.index()->addRoot("D:");
        const auto samples = index.index()->add(index.index()->add(drive, "Downloads", EntryFlag::Directory), "samples",
            EntryFlag::Directory);
        for (const char* name : {"ok.jpg", "ocr.txt", "ok2.jpg", "ocr-apostrophe-hyphen.png", "ocr-highlight-test.png"})
            index.index()->add(samples, name, 0);
    }
    AppCatalog apps;
    apps.setApps(std::make_shared<const AppList>());
    SystemCatalog places;
    places.setPlaces(std::make_shared<const PlaceList>());
    SearchEngine search(&index, &apps, &places);
    History history(profile.filePath(u"history.txt"_s));
    Launcher launcher(&index, &apps, &places, &search, &history);
    launcher.results()->setThumbnails(false);
    launcher.setContentOptions(false, {}, {}, true, true);
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
    check(window != nullptr, u"row spacing loads production Main: "_s + component.errorString());
    if (!window) return false;
    window->create();
    frame.setWindow(window);
    launcher.setWindow(window);
    placement.setWindow(window);
    placement.rememberWidth(760, 640);
    showOffscreen(window);

    // The ink of each scanline of `area`: anything off the colour most of it has.
    const auto inkLines = [](const QImage& picture, const QRect& area) {
        QHash<QRgb, int> counts;
        for (int y = area.top(); y <= area.bottom(); ++y)
            for (int x = area.left(); x <= area.right(); ++x)
                ++counts[picture.pixel(x, y)];
        const QRgb ground = std::max_element(counts.cbegin(), counts.cend()).key();
        std::vector<bool> lines;
        for (int y = area.top(); y <= area.bottom(); ++y) {
            bool ink = false;
            for (int x = area.left(); x <= area.right() && !ink; ++x) {
                const QRgb pixel = picture.pixel(x, y);
                ink = std::max({std::abs(qRed(pixel) - qRed(ground)), std::abs(qGreen(pixel) - qGreen(ground)),
                          std::abs(qBlue(pixel) - qBlue(ground))}) > 48;
            }
            lines.push_back(ink);
        }
        return lines;
    };
    QJsonObject report;
    for (const bool folder : {true, false}) {
        const QString mode = folder ? u"folder box"_s : u"launcher"_s;
        launcher.setFolderSearch(folder);
        launcher.setQuery(u"o"_s);
        settle(window);
        check(launcher.results()->count() >= 3, mode + u": rows to measure"_s);
        const QImage picture = window->grabWindow().convertToFormat(QImage::Format_RGB32);
        const qreal dpr = picture.devicePixelRatio();
        // Per row, from the top: where its name's ink starts and ends, then its folder's.
        struct Lines { int nameTop = -1, nameBottom = -1, folderTop = -1, folderBottom = -1; };
        std::vector<Lines> rows;
        for (int i = 0; i < 3; ++i) {
            auto* row = item(window->contentItem(), u"resultRow-%1"_s.arg(i));
            auto* text = row ? item(row, u"resultText"_s) : nullptr;
            if (!row || !text) break;
            const QPointF textAt = text->mapToScene({0, 0});
            const QPointF rowAt = row->mapToScene({0, 0});
            const QRect area = QRectF(textAt.x() * dpr, rowAt.y() * dpr, text->width() * dpr, row->height() * dpr)
                                   .toAlignedRect().intersected(picture.rect());
            const std::vector<bool> lines = inkLines(picture, area);
            std::vector<std::pair<int, int>> runs; // [top, bottom] of each run of ink
            for (int y = 0; y < int(lines.size()); ++y) {
                if (!lines[y]) continue;
                if (runs.empty() || runs.back().second != y - 1)
                    runs.push_back({y, y});
                else
                    runs.back().second = y;
            }
            if (runs.size() != 2) {
                check(false, mode + u": row %1 shows two lines of ink (%2)"_s.arg(i).arg(runs.size()));
                break;
            }
            rows.push_back({area.top() + runs[0].first, area.top() + runs[0].second, area.top() + runs[1].first,
                area.top() + runs[1].second});
        }
        check(rows.size() == 3, mode + u": three rows measured"_s);
        if (rows.size() != 3) continue;
        qreal inner = 0, outer = 1e9;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            inner = std::max<qreal>(inner, (rows[i].folderTop - rows[i].nameBottom - 1) / dpr);
            if (i + 1 < rows.size())
                outer = std::min<qreal>(outer, (rows[i + 1].nameTop - rows[i].folderBottom - 1) / dpr);
        }
        report.insert(folder ? u"folder"_s : u"launcher"_s, QJsonObject {{u"dpr"_s, dpr}, {u"name_to_folder"_s, inner},
            {u"row_to_row"_s, outer}});
        // The folder box's short rows must tell clearly, with any font and
        // scale; the launcher's roomy ones only must not be the other way round.
        check(folder ? outer >= inner + 3 : outer > inner, mode + u": name to folder %1, row to row %2 (logical px of ink)"_s.arg(inner).arg(outer));
        check(picture.save(output.filePath(folder ? u"rows-folder-box.png"_s : u"rows-launcher.png"_s)), mode + u": saved"_s);
    }
    launcher.setFolderSearch(false);
    QFile file(output.filePath(u"result-row-spacing.json"_s));
    if (!file.open(QIODevice::WriteOnly)) return false;
    report.insert(u"font"_s, QGuiApplication::font().family());
    report.insert(u"passed"_s, check.passed);
    report.insert(u"checks"_s, check.rows);
    file.write(QJsonDocument(report).toJson());
    return check.passed;
}

bool checkPluginReviewSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output)
{
    Checks check;
    QMetaObject::invokeMethod(window, "showSetting", Q_ARG(QVariant, 3), Q_ARG(QVariant, 1),
        Q_ARG(QVariant, QCoreApplication::translate("SettingsWindow", "纯净搜索模式")));
    settle(window);
    auto* toggle = item(window->contentItem(), u"pureSearchToggle"_s);
    check(toggle && toggle->isVisible(), u"pure mode has a searchable settings entry"_s);
    if (!toggle) return false;
    QMetaObject::invokeMethod(toggle, "toggled", Q_ARG(bool, true));
    Settings restored;
    restored.load();
    check(restored.pureSearch && editor->pureSearch(), u"the real toggle saves pure mode"_s);
    check(window->grabWindow().save(output.filePath(u"pure-setting.png"_s)), u"capture pure mode setting"_s);
    QTemporaryDir temporary;
    plugin::Host host({.userPluginDir = temporary.filePath(u"installed"_s), .dataRoot = temporary.filePath(u"data"_s),
        .elevatedHost = false, .elevationRoot = temporary.filePath(u"elevated"_s), .elevationProtected = false});
    editor->plugins()->setHost(&host);
    const auto detach = qScopeGuard([&] { editor->plugins()->setHost(nullptr); });
    const QString candidate = temporary.filePath(u"candidate"_s);
    QDir().mkpath(candidate);
    const auto manifest = [&](const QString& version) {
        QFile file(QDir(candidate).filePath(u"plugin.json"_s));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        file.write(QJsonDocument(QJsonObject {{u"manifest"_s, 1}, {u"id"_s, u"probe.native"_s},
            {u"name"_s, u"社区原生工具 / Community native tool"_s}, {u"version"_s, version},
            {u"homepage"_s, u"https://example.test/native"_s},
            {u"runtime"_s, QJsonObject {{u"mode"_s, u"once"_s}, {u"program"_s, u"tool.exe"_s}}},
            {u"commands"_s, QJsonArray {QJsonObject {{u"id"_s, u"go"_s}, {u"keyword"_s, u"probe"_s}}}}}).toJson());
        return true;
    };
    check(manifest(u"1.0.0"_s), u"create isolated native candidate"_s);
    QFile executable(QDir(candidate).filePath(u"tool.exe"_s));
    if (!executable.open(QIODevice::WriteOnly)) return false;
    executable.write("MZ not executable: this probe must never run a plugin");
    executable.close();
    const auto request = [&] { editor->plugins()->installFrom(candidate, u"probe.native"_s, u"Native"_s, false, true); settle(window); };
    request();
    auto* warning = item(window->contentItem(), u"unreviewedPluginWarning"_s);
    auto* accept = item(window->contentItem(), u"pluginDialogAccept"_s);
    auto* card = item(window->contentItem(), u"pluginDialogCard"_s);
    check(warning && warning->isVisible() && accept && card, u"first install shows the real risk confirmation"_s);
    if (!warning || !accept || !card) return false;
    auto* source = item(window->contentItem(), u"pluginInstallSource"_s);
    check(source && source->isVisible(), u"the confirmation provides an explicit web-source action"_s);
    check(!host.catalog().find(u"probe.native"_s), u"nothing installed before confirmation"_s);
    check(window->grabWindow().save(output.filePath(u"native-confirm.png"_s)), u"capture native installation warning"_s);
    key(card, Qt::Key_Escape);
    settle(window);
    check(!warning->isVisible() && !host.catalog().find(u"probe.native"_s), u"Escape cancels without installing"_s);
    request();
    QMetaObject::invokeMethod(accept, "clicked");
    settle(window);
    const auto installedVersion = [&] {
        const auto* entry = host.catalog().find(u"probe.native"_s);
        return entry && entry->manifest ? entry->manifest->version : QString();
    };
    check(installedVersion() == u"1.0.0", u"explicit confirmation installs the candidate"_s);
    check(!host.catalog().enabled(u"probe.native"_s), u"installation does not enable or execute the native plugin"_s);
    check(manifest(u"1.1.0"_s), u"prepare an update with identical permissions"_s);
    request();
    check(warning->isVisible() && installedVersion() == u"1.0.0", u"same-permission update still requires confirmation"_s);
    key(card, Qt::Key_Escape);
    settle(window);
    check(installedVersion() == u"1.0.0", u"rejecting an update preserves the old version"_s);
    request();
    QMetaObject::invokeMethod(accept, "clicked");
    settle(window);
    check(installedVersion() == u"1.1.0", u"confirmed update reaches the installed catalog"_s);
    // Read a local index through the production gallery. Preview sample rows
    // cannot prove that new model roles reach the actual delegate.
    const QString indexPath = temporary.filePath(u"插件库索引.json"_s);
    QFile indexFile(indexPath);
    if (!indexFile.open(QIODevice::WriteOnly)) return false;
    indexFile.write(QJsonDocument(QJsonObject {{u"schema"_s, 1}, {u"plugins"_s, QJsonArray {QJsonObject {
        {u"id"_s, u"probe.native"_s}, {u"name"_s, u"Community native tool"_s}, {u"version"_s, u"1.2.0"_s},
        {u"description"_s, u"An open-source native utility. Review its source before installing."_s},
        {u"runtime"_s, QJsonObject {{u"mode"_s, u"once"_s}, {u"program"_s, u"tool.exe"_s}}},
        {u"review"_s, u"unreviewed-native"_s}, {u"url"_s, temporary.filePath(u"unused.wsplugin"_s)},
        {u"size"_s, 1}, {u"sha256"_s, QString(64, u'a')}}}}}).toJson());
    indexFile.close();
    const std::wstring oldIndex = qEnvironmentVariable("WINSHUN_PLUGIN_INDEX").toStdWString();
    const bool hadIndex = qEnvironmentVariableIsSet("WINSHUN_PLUGIN_INDEX");
    // qputenv interprets bytes in the Windows ANSI code page. Keep the local
    // index path Unicode even when that code page cannot represent its name.
    check(::_wputenv_s(L"WINSHUN_PLUGIN_INDEX", indexPath.toStdWString().c_str()) == 0
            && qEnvironmentVariable("WINSHUN_PLUGIN_INDEX") == indexPath,
        u"configure a Unicode local plugin index"_s);
    const auto restoreIndex = qScopeGuard([&] {
        ::_wputenv_s(L"WINSHUN_PLUGIN_INDEX", hadIndex ? oldIndex.c_str() : L"");
    });
    Settings gallerySettings;
    SettingsEditor galleryEditor(gallerySettings, u"software"_s);
    galleryEditor.plugins()->setHost(&host);
    auto* gallery = galleryEditor.plugins()->gallery();
    gallery->refresh();
    QElapsedTimer fetchTime;
    fetchTime.start();
    while (gallery->fetching() && fetchTime.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    auto* model = gallery->list();
    check(model->rowCount() == 1, u"production gallery loads the unreviewed native index entry"_s);
    check(gallery->updatableCount() == 0, u"unreviewed update is excluded from Update all"_s);
    QVariantMap properties {{u"gallery"_s, QVariant::fromValue(gallery)}};
    const auto roles = model->roleNames();
    for (auto it = roles.begin(); it != roles.end(); ++it)
        properties.insert(QString::fromUtf8(it.value()), model->data(model->index(0), it.key()));
    check(properties.value(u"unreviewed"_s).toBool() && !properties.value(u"reviewLabel"_s).toString().isEmpty(),
        u"gallery exposes the real risk flag and readable label"_s);
    QQmlComponent entry(qmlEngine(window), u"WinShun"_s, u"PluginGalleryEntry"_s);
    std::unique_ptr<QObject> entryObject(entry.createWithInitialProperties(properties));
    auto* entryItem = qobject_cast<QQuickItem*>(entryObject.get());
    check(entryItem != nullptr, u"production gallery delegate accepts the actual model roles: "_s + entry.errorString());
    if (entryItem) {
        QQuickWindow galleryWindow;
        galleryWindow.setColor(window->color()); // the card's transparent surface belongs on the current theme
        galleryWindow.setWidth(640);
        entryItem->setParentItem(galleryWindow.contentItem());
        entryItem->setPosition({20, 20});
        entryItem->setWidth(600);
        galleryWindow.setHeight(static_cast<int>(std::ceil(entryItem->implicitHeight())) + 40);
        showOffscreen(&galleryWindow);
        check(galleryWindow.grabWindow().save(output.filePath(u"native-gallery-card.png"_s)), u"capture native classification on the real card"_s);
        entryItem->setParentItem(nullptr);
    }
    galleryEditor.plugins()->setHost(nullptr);
    check(!window->isActive(), u"settings checks leave desktop focus untouched"_s);
    QFile report(output.filePath(u"plugin-review-settings.json"_s));
    if (!report.open(QIODevice::WriteOnly)) return false;
    report.write(QJsonDocument(QJsonObject {{u"passed"_s, check.passed}, {u"checks"_s, check.rows}}).toJson());
    return check.passed;
}

bool checkSearchExperience(QQmlEngine* engine, const QDir& output)
{
    Checks check;
    QTemporaryDir profile;
    IndexService index(profile.filePath(u"index"_s), {});
    {
        const auto lock = index.index()->writeLock();
        const auto root = index.index()->addRoot("D:");
        index.index()->add(root, "report.docx", 0);
        index.index()->add(root, "report.png", 0);
        index.index()->add(root, "report-folder", EntryFlag::Directory);
    }
    AppCatalog apps;
    AppInfo app {.name = u"Report"_s, .id = u"WinShun.Probe.Report"_s, .target = u"D:\\Report.exe"_s};
    app.prepare();
    apps.setApps(std::make_shared<const AppList>(AppList {app}));
    SystemCatalog places;
    places.setPlaces(std::make_shared<const PlaceList>());
    SearchEngine search(&index, &apps, &places);
    History history(profile.filePath(u"history.txt"_s));
    Launcher launcher(&index, &apps, &places, &search, &history);
    launcher.setRecordHistory(true);
    launcher.results()->setThumbnails(false); // synthetic index paths have no image files on disk
    launcher.setFileTypes({{u"文档"_s, {u"docx"_s}, u"docs"_s, u"doc"_s},
        {u"图片"_s, {u"png"_s}, u"pictures"_s, u"pic"_s}});
    launcher.setFolderKeyword(u"folder"_s);
    launcher.setContentOptions(false, {}, {}, true, true);
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
    check(window != nullptr, u"production search window loads: "_s + component.errorString());
    if (!window) return false;
    window->create();
    frame.setWindow(window);
    launcher.setWindow(window);
    showOffscreen(window);
    auto* field = item(window->contentItem(), u"searchField"_s);
    auto* strip = item(window->contentItem(), u"searchTypeStrip"_s);
    auto* button = item(window->contentItem(), u"queryHistoryButton"_s);
    check(field && strip && button, u"history and type controls exist"_s);
    if (!field || !strip || !button) return false;
    launcher.setScope(Launcher::All);
    check(!strip->property("shown").toBool(), u"default keeps separate All and Files tabs"_s);
    launcher.cycleScope(1);
    check(launcher.scope() == Launcher::Files, u"default Tab still reaches Files"_s);
    launcher.setScope(Launcher::All);
    launcher.setMergeFileScope(true);
    launcher.setQuery(u"report"_s);
    settle(window);
    check(strip->property("shown").toBool(), u"merged All keeps the type strip visible"_s);
    check(launcher.results()->count() == 4 && launcher.results()->at(0)->isApp(),
        u"merged All finds both files and applications, with the application first"_s);
    launcher.toggleFileType(0);
    settle(window);
    check(launcher.scope() == Launcher::Files && launcher.results()->count() == 1
        && launcher.results()->pathAt(0).endsWith(u"report.docx"), u"a document chip filters actual search results"_s);
    launcher.toggleFileType(0);
    settle(window);
    check(launcher.scope() == Launcher::All && launcher.results()->count() == 4, u"clearing the last chip restores applications and files"_s);
    key(field, Qt::Key_Tab);
    check(launcher.scope() == Launcher::All, u"merged Tab skips the redundant Files scope"_s);
    launcher.setContentOptions(true, {}, {}, true, true);
    key(field, Qt::Key_Tab);
    check(launcher.scope() == Launcher::Content, u"merged Tab still reaches enabled content search"_s);
    key(field, Qt::Key_Backtab, Qt::ShiftModifier);
    check(launcher.scope() == Launcher::All, u"Shift+Tab returns from content to merged All"_s);
    launcher.setContentOptions(false, {}, {}, true, true);
    auto* documentChip = item(window->contentItem(), u"searchTypeChip0"_s);
    const qreal chipWidth = documentChip ? documentChip->width() : 0;
    // Ctrl held by itself a moment, not each press of it (GitHub #279).
    const auto heldAWhile = [window] {
        for (int i = 0; i < 3; ++i) // over Main.qml's keywordPeek
            settle(window);
    };
    key(field, Qt::Key_Control, Qt::ControlModifier);
    check(!strip->property("showKeywords").toBool(), u"a press of Ctrl leaves the type names"_s);
    key(field, Qt::Key_F24, Qt::ControlModifier); // a shortcut: a key of no meaning here
    heldAWhile();
    check(!strip->property("showKeywords").toBool(), u"a shortcut with Ctrl never brings the prefixes"_s);
    QKeyEvent tapEnd(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
    QCoreApplication::sendEvent(field, &tapEnd);
    key(field, Qt::Key_Control, Qt::ControlModifier);
    QCoreApplication::sendEvent(field, &tapEnd);
    heldAWhile();
    check(!strip->property("showKeywords").toBool(), u"a tap of Ctrl never brings the prefixes"_s);
    key(field, Qt::Key_Control, Qt::ControlModifier);
    for (int i = 0; i < 3; ++i) { // held, the keyboard repeats it: no new press each time
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier, QString(), true);
        QCoreApplication::sendEvent(field, &repeat);
        settle(window);
    }
    check(strip->property("showKeywords").toBool(), u"holding Ctrl exposes type prefixes"_s);
    check(window->grabWindow().save(output.filePath(u"merged-scope-prefixes.png"_s)), u"capture merged type prefixes"_s);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
    QCoreApplication::sendEvent(field, &release);
    check(!strip->property("showKeywords").toBool(), u"releasing Ctrl restores type names"_s);
    settle(window);
    // (A prefix is as wide as it is: the chips make room while it is read.)
    check(documentChip && documentChip->width() == chipWidth, u"the chips are back at their widths with the names"_s);
    launcher.chooseFileType(-2);
    settle(window);
    check(launcher.scope() == Launcher::Folders && launcher.results()->count() == 1, u"folder chip remains available"_s);
    launcher.chooseFileType(-1);
    launcher.setQuery(u":doc \"Annual  Report\""_s);
    launcher.rememberQuery();
    launcher.setQuery(u"中文 搜索"_s);
    launcher.rememberQuery();
    launcher.setQuery(u"unfinished draft"_s);
    key(field, Qt::Key_Up, Qt::ControlModifier);
    check(launcher.query() == u"中文 搜索", u"Ctrl+Up recalls the latest complete query"_s);
    key(field, Qt::Key_Up, Qt::ControlModifier);
    check(launcher.query() == u":doc \"Annual  Report\"", u"older query preserves case, spacing, quotes and prefix"_s);
    key(field, Qt::Key_Down, Qt::ControlModifier);
    key(field, Qt::Key_Down, Qt::ControlModifier);
    check(launcher.query() == u"unfinished draft", u"Ctrl+Down restores unfinished input"_s);
    launcher.setHistoryKeys(u"alt"_s);
    key(field, Qt::Key_Up, Qt::AltModifier);
    check(launcher.query() == u"中文 搜索", u"configured Alt+Up recalls history"_s);
    launcher.setHistoryKeys(u"off"_s);
    launcher.setQuery(u"keyboard navigation off"_s);
    key(field, Qt::Key_Up, Qt::AltModifier);
    check(launcher.query() == u"keyboard navigation off", u"disabled history shortcut leaves input alone"_s);
    launcher.setHistoryKeys(u"ctrl"_s);
    // Cloak the native popup before showing it; it calculates a real monitor's
    // bounds and must never cover another agent's or the user's window. The
    // pickers make their windows on first use: made here, still hidden.
    for (QObject* picker : window->findChildren<QObject*>(u"queryHistoryPicker"_s))
        QMetaObject::invokeMethod(picker, "prepare");
    auto* popup = window->findChild<QQuickWindow*>(u"queryHistoryPopup"_s);
    check(popup != nullptr, u"native history popup is available"_s);
    if (popup) {
        popup->create();
        win::setCloaked(popup, true);
        DWORD cloaked = 0;
        const bool hidden = SUCCEEDED(::DwmGetWindowAttribute(reinterpret_cast<HWND>(popup->winId()), DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked;
        check(hidden, u"history popup is cloaked before opening"_s);
        if (hidden) {
            QMetaObject::invokeMethod(button, "clicked");
            settle(popup);
            check(popup->isVisible(), u"history button opens browsable query list"_s);
            const int shortWidth = popup->width();
            check(shortWidth >= 240 && shortWidth < 440, u"short query history shrinks to its content"_s);
            check(popup->grabWindow().save(output.filePath(u"search-history.png"_s)), u"capture the history list"_s);
            launcher.setQuery(u"查询包含很长的文件名、完整文件夹路径，以及需要省略显示的多组搜索条件 "_s.repeated(4));
            launcher.rememberQuery();
            settle(popup);
            check(popup->isVisible() && popup->width() > shortWidth && popup->width() <= 440,
                u"long query history grows within the width limit while open"_s);
            check(popup->grabWindow().save(output.filePath(u"search-history-long.png"_s)), u"capture long history entries"_s);
            const int originalWidth = window->width();
            window->setWidth(320);
            settle(popup);
            auto* anchor = popup->property("anchor").value<QQuickItem*>();
            check(anchor && anchor->width() <= 320 && popup->width() <= anchor->width(),
                u"open query history follows a narrower search bar"_s);
            const QRectF area = popup->property("area").toRectF();
            check(popup->x() >= area.left() + 8 && popup->x() + popup->width() <= area.right() - 8,
                u"history width changes preserve screen edge clearance"_s);
            check(popup->grabWindow().save(output.filePath(u"search-history-narrow.png"_s)), u"capture narrow history popup"_s);
            window->setWidth(originalWidth);
            launcher.removeQuery(0);
            settle(popup);
            check(popup->width() == shortWidth, u"removing the longest history entry shrinks the open popup"_s);
            key(field, Qt::Key_Down);
            key(field, Qt::Key_Return);
            check(!popup->isVisible() && launcher.query() == u":doc \"Annual  Report\"", u"popup arrows and Enter recall the selected query"_s);
            QMetaObject::invokeMethod(button, "clicked");
            key(field, Qt::Key_Delete);
            check(launcher.queryHistoryCount() == 1, u"Delete removes only the selected query"_s);
            key(field, Qt::Key_Escape);
            check(!popup->isVisible() && window->isVisible(), u"Escape closes only the history popup"_s);
        }
    }
    launcher.setPanel(true);
    window->resize(1100, 650);
    settle(window);
    auto* panelField = item(window->contentItem(), u"panelSearchField"_s);
    check(panelField && panelField->isVisible(), u"full panel uses the same history-enabled search field"_s);
    if (panelField) {
        launcher.setQuery(u"panel draft"_s);
        key(panelField, Qt::Key_Up, Qt::ControlModifier);
        check(launcher.query() == u":doc \"Annual  Report\"", u"history recall also works in the full panel"_s);
        key(panelField, Qt::Key_Down, Qt::ControlModifier);
        check(launcher.query() == u"panel draft", u"full-panel recall also restores its draft"_s);
    }
    launcher.setQuery(u"report"_s);
    launcher.chooseFileType(-1);
    settle(window);
    check(window->grabWindow().save(output.filePath(u"merged-panel.png"_s)), u"capture merged full-panel filters"_s);
    launcher.clearHistory();
    launcher.handleHidden();
    check(launcher.queryHistoryCount() == 0, u"clearing history is not undone when the search window hides"_s);
    launcher.setRecordHistory(false);
    launcher.setQuery(u"not recorded"_s);
    launcher.rememberQuery();
    auto* panelHistory = item(window->contentItem(), u"panelHistoryButton"_s);
    check(launcher.queryHistoryCount() == 0 && !launcher.queryHistoryEnabled() && panelHistory && !panelHistory->isVisible(),
        u"history disabled hides the control and saves no queries"_s);
    check(!window->isActive(), u"search probes never activate the native window"_s);
    window->hide();
    QFile file(output.filePath(u"search-experience.json"_s));
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(QJsonObject {{u"passed"_s, check.passed}, {u"checks"_s, check.rows}}).toJson());
    return check.passed;
}

bool checkLogoPositionSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output)
{
    Checks check;
    check(editor->logoRightClick() == u"menu", u"existing profiles keep the position menu by default"_s);
    QMetaObject::invokeMethod(window, "showSetting", Q_ARG(QVariant, 3), Q_ARG(QVariant, 2),
        Q_ARG(QVariant, QCoreApplication::translate("SettingsWindow", "右键 Logo 的行为")));
    settle(window);
    auto* choices = item(window->contentItem(), u"logoRightClickChoice"_s);
    auto* row = item(window->contentItem(), u"logoRightClickRow"_s);
    check(choices && choices->isVisible() && row && row->isVisible(), u"logo position choices appear in interaction settings"_s);
    if (!choices || !row) return false;
    const QStringList actions {u"menu"_s, u"top"_s, u"center"_s, u"restore"_s};
    for (int i = 0; i < actions.size(); ++i) {
        QMetaObject::invokeMethod(choices, "activated", Q_ARG(int, i));
        settle(window);
        Settings saved;
        saved.load();
        check(saved.logoRightClick == actions[i] && editor->logoRightClick() == actions[i]
            && choices->property("current").toInt() == i, u"settings choice persists and updates the control: "_s + actions[i]);
    }
    const QPointF start = choices->mapToItem(row, QPointF());
    check(start.x() >= 0 && start.x() + choices->width() <= row->width() + 1,
        u"translated position choices fit inside the settings row"_s);
    check(window->grabWindow().save(output.filePath(u"logo-position-settings.png"_s)), u"capture logo position settings"_s);
    QMetaObject::invokeMethod(choices, "activated", Q_ARG(int, 0));
    editor->setLogoRightClick(u"invalid"_s);
    check(editor->logoRightClick() == u"menu", u"settings editor rejects unknown logo actions"_s);
    check(!window->isActive(), u"logo settings probe leaves desktop focus untouched"_s);
    QFile report(output.filePath(u"logo-position-settings.json"_s));
    if (!report.open(QIODevice::WriteOnly)) return false;
    report.write(QJsonDocument(QJsonObject {{u"passed"_s, check.passed}, {u"checks"_s, check.rows}}).toJson());
    return check.passed;
}

bool checkSearchExperienceSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output)
{
    Checks check;
    QMetaObject::invokeMethod(window, "showSetting", Q_ARG(QVariant, 3), Q_ARG(QVariant, 1),
        Q_ARG(QVariant, QCoreApplication::translate("SettingsWindow", "合并“全部”和“文件”")));
    settle(window);
    auto* merge = item(window->contentItem(), u"mergeFileScopeToggle"_s);
    check(merge && merge->isVisible(), u"merged-scope preference is visible"_s);
    if (!merge) return false;
    QMetaObject::invokeMethod(merge, "toggled", Q_ARG(bool, true));
    Settings saved;
    saved.load();
    check(saved.mergeFileScope, u"merged-scope setting persists"_s);
    QMetaObject::invokeMethod(window, "showSetting", Q_ARG(QVariant, 0), Q_ARG(QVariant, 3),
        Q_ARG(QVariant, QCoreApplication::translate("SettingsWindow", "搜索历史快捷键")));
    settle(window);
    auto* keys = item(window->contentItem(), u"queryHistoryKeys"_s);
    check(keys && keys->isVisible(), u"history shortcut setting is visible"_s);
    if (!keys) return false;
    QMetaObject::invokeMethod(keys, "activated", Q_ARG(int, 1));
    saved.load();
    check(saved.queryHistoryKeys == u"alt", u"history modifier choice persists"_s);
    check(window->grabWindow().save(output.filePath(u"history-settings.png"_s)), u"capture query history settings"_s);
    QTemporaryDir plugin;
    const QString pluginDir = plugin.filePath(u"plugins/example.guide"_s);
    QDir().mkpath(pluginDir);
    QFile manifest(QDir(pluginDir).filePath(u"plugin.json"_s));
    if (!manifest.open(QIODevice::WriteOnly)) return false;
    manifest.write(R"({"manifest":1,"id":"example.guide","name":"示例插件 / Example plugin","version":"1.0.0","commands":[{"id":"go","keyword":"guide","run":{"folder":"C:\\"}}]})");
    manifest.close();
    plugin::Host host({.userPluginDir = plugin.filePath(u"plugins"_s), .dataRoot = plugin.filePath(u"data"_s),
        .elevatedHost = false, .elevationRoot = plugin.filePath(u"elevated"_s), .elevationProtected = false});
    host.catalog().scan();
    check(host.catalog().find(u"example.guide"_s) != nullptr, u"real catalog discovers the isolated disabled plugin"_s);
    editor->plugins()->setHost(&host);
    QFile readme(QDir(pluginDir).filePath(u"README.md"_s));
    check(readme.open(QIODevice::WriteOnly), u"create isolated plugin documentation"_s);
    readme.write("# 使用说明 / Quick start\n\n**快捷搜索**：输入关键词，再按 Enter。\n\n- 支持中文和 English\n- 使用 `:doc report` 查找文档\n\n[项目主页](https://github.com/LingCore/WinShun)\n\n");
    for (int i = 0; i < 30; ++i) readme.write("### 操作提示\n\n说明可以滚动阅读、选中文字复制。Esc / Q 关闭后回到设置。\n\n");
    readme.close();
    const auto document = readme::load(pluginDir);
    check(document.error.isEmpty(), u"load real Markdown file with the production reader"_s);
    editor->plugins()->openReadme(u"example.guide"_s);
    settle(window);
    auto* dialog = item(window->contentItem(), u"pluginReadmeDialog"_s);
    auto* text = item(window->contentItem(), u"pluginReadmeText"_s);
    auto* scroll = item(window->contentItem(), u"pluginReadmeScroll"_s);
    check(dialog && text && scroll && dialog->isVisible(), u"plugin documentation opens inside settings"_s);
    if (!dialog || !text || !scroll) return false;
    check(text->property("readOnly").toBool() && text->property("selectByMouse").toBool(), u"reader allows selection without editing"_s);
    check(scroll->property("contentHeight").toReal() > scroll->height(), u"long documentation is scrollable"_s);
    check(window->grabWindow().save(output.filePath(u"plugin-readme.png"_s)), u"capture themed documentation reader"_s);
    key(text, Qt::Key_PageDown);
    check(scroll->property("contentY").toReal() > 0, u"PageDown scrolls the document"_s);
    key(text, Qt::Key_Q);
    check(!dialog->isVisible() && window->isVisible(), u"Q closes the reader and keeps settings open"_s);
    if (!readme.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    readme.write("# Second\n\nFresh content");
    readme.close();
    editor->plugins()->openReadme(u"example.guide"_s);
    settle(window);
    check(scroll->property("contentY").toReal() == 0 && text->property("text").toString().contains(u"Fresh content"), u"reopening replaces the document and resets scrolling"_s);
    key(text, Qt::Key_Escape);
    check(!dialog->isVisible() && window->isVisible(), u"Escape closes only the reader"_s);
    check(!window->isActive(), u"reader probes never activate the native window"_s);
    editor->plugins()->setHost(nullptr);
    QFile report(output.filePath(u"search-experience-settings.json"_s));
    if (!report.open(QIODevice::WriteOnly)) return false;
    report.write(QJsonDocument(QJsonObject {{u"passed"_s, check.passed}, {u"checks"_s, check.rows}}).toJson());
    return check.passed;
}

bool checkNavigationSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output)
{
    Checks check;
    const auto reveal = [&](const char* title, int tab) {
        QMetaObject::invokeMethod(window, "showSetting", Q_ARG(QVariant, 0), Q_ARG(QVariant, tab),
            Q_ARG(QVariant, QCoreApplication::translate("SettingsWindow", title)));
        settle(window);
    };
    reveal("直接打开搜索面板", 0);
    auto* recorder = item(window->contentItem(), u"panelHotkeyRecorder"_s);
    auto* recent = item(window->contentItem(), u"folderMenuRecentInline"_s);
    auto* windows = item(window->contentItem(), u"folderMenuWindowsInline"_s);
    check(recorder && recent && windows, u"new settings use production controls"_s);
    if (!recorder || !recent || !windows) return false;
    check(editor->panelHotkey().isEmpty(), u"full-panel shortcut defaults to unassigned"_s);
    // Deliver to the real recorder's QML handler without starting its global
    // keyboard capture or registering shortcuts in another user's session.
    key(recorder, Qt::Key_F, Qt::ControlModifier | Qt::AltModifier);
    Settings saved;
    saved.load();
    check(editor->panelHotkey() == u"Ctrl+Alt+F" && saved.panelHotkey == u"Ctrl+Alt+F"
        && saved.hotkey == u"Alt+Space", u"recording a full-panel chord persists independently of the launcher chord"_s);
    settle(window);
    check(window->grabWindow().save(output.filePath(u"panel-hotkey-settings.png"_s)), u"capture the full-panel shortcut setting"_s);
    key(recorder, Qt::Key_Backspace);
    saved.load();
    check(editor->panelHotkey().isEmpty() && saved.panelHotkey.isEmpty() && saved.hotkey == u"Alt+Space",
        u"clearing the full-panel chord preserves the launcher shortcut"_s);
    reveal("直接显示最近的文件夹", 2);
    recent->setProperty("text", u"3"_s);
    QMetaObject::invokeMethod(recent, "editingFinished");
    windows->setProperty("text", u"10"_s);
    QMetaObject::invokeMethod(windows, "editingFinished");
    saved.load();
    check(saved.folderMenuRecentInline == 3 && saved.folderMenuWindowsInline == 10,
        u"recent and open-window counts persist independently"_s);
    recent->setProperty("text", u"11"_s);
    QMetaObject::invokeMethod(recent, "editingFinished");
    windows->setProperty("text", QString());
    QMetaObject::invokeMethod(windows, "editingFinished");
    check(recent->property("text").toString() == u"3" && windows->property("text").toString() == u"10",
        u"invalid and empty counts restore the saved values"_s);
    settle(window);
    auto* actions = item(window->contentItem(), u"folderMenuActionChoices"_s);
    bool choicesFit = actions != nullptr;
    for (int i = 0; i < 6; ++i) {
        auto* choice = item(window->contentItem(), u"folderMenuAction%1"_s.arg(i));
        choicesFit &= choice && choice->x() >= 0 && choice->x() + choice->width() <= actions->width() + 1;
    }
    check(choicesFit, u"all six menu action choices fit and wrap within the settings page"_s);
    if (auto* choice = item(window->contentItem(), u"folderMenuAction0"_s)) {
        QMetaObject::invokeMethod(choice, "toggled", Q_ARG(int, 0), Q_ARG(bool, false));
        saved.load();
        check(!saved.folderMenuItemSearch, u"wrapped action choices still persist their individual switches"_s);
        QMetaObject::invokeMethod(choice, "toggled", Q_ARG(int, 0), Q_ARG(bool, true));
    }
    check(window->grabWindow().save(output.filePath(u"folder-count-settings.png"_s)), u"capture direct-folder count settings"_s);
    recent->setProperty("text", u"0"_s);
    QMetaObject::invokeMethod(recent, "editingFinished");
    saved.load();
    check(saved.folderMenuRecentInline == 0 && saved.folderMenuWindowsInline == 10,
        u"zero restores the recent-folders submenu without changing open windows"_s);
    check(!window->isActive(), u"settings checks keep the native window inactive"_s);
    QFile file(output.filePath(u"navigation-settings.json"_s));
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(QJsonObject {{u"passed"_s, check.passed}, {u"checks"_s, check.rows}}).toJson());
    return check.passed;
}
}
