#include "NavigationProbe.h"

#include "AppCatalog.h"
#include "Clipboard.h"
#include "DialogRuleStore.h"
#include "ClipboardDisplay.h"
#include "Favorites.h"
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
#include "Updater.h"
#include "WindowFrame.h"
#include "platform/ClipboardWatcher.h"
#include "platform/DialogJump.h"
#include "platform/DialogRules.h"
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
#include <QMouseEvent>
#include <QProcess>
#include <QThread>
#include <QInputMethodEvent>
#include <QPainter>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QAbstractTextDocumentLayout>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSet>
#include <QSettings>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
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
    clipboard.setRowOptions(false, u"standard"_s, SystemTheme::uiSize());
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
        && results->width() >= shownWidth + 175, u"hiding filters gives their width to results"_s);
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
    auto* strip = item(window->contentItem(), u"panelFilterBar"_s);
    launcher.setPanelFiltersOnTop(true);
    settle(window);
    check(strip && strip->isVisible() && strip->height() > 0 && !sidebar->isVisible() && results->width() >= shownWidth + 175,
        u"filters on top leave the rows the whole width"_s);
    check(window->grabWindow().save(output.filePath(u"panel-filters-top.png"_s)), u"capture the filters as a strip over the rows"_s);
    launcher.setPanelFiltersOnTop(false);
    settle(window);
    check(strip && !strip->isVisible() && sidebar->isVisible() && std::abs(results->width() - shownWidth) < 1,
        u"filters back at the left restore the layout"_s);
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

    // None yet: the + is there all the same, saying what it is for.
    launcher.setPanelFiltersOnTop(false);
    settle(window);
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
    auto* sidebar = item(window->contentItem(), u"panelSidebar"_s);
    for (QQuickItem* chip : {first, second, add}) {
        const qreal right = chip->mapToItem(sidebar, QPointF(chip->width(), 0)).x();
        check(sidebar && right <= sidebar->width() + 0.5, u"quick filters: rows stay inside the side bar"_s);
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

    // 名称 (GitHub #457): three boxes under its row, what they say a condition as a quick filter's.
    auto* nameRow = item(window->contentItem(), u"panelFilterName"_s);
    auto* nameStarts = item(window->contentItem(), u"panelNameStarts"_s);
    auto* nameLacks = item(window->contentItem(), u"panelNameLacks"_s);
    check(nameRow && nameRow->isVisible() && nameStarts && nameLacks && !nameStarts->isVisible(), u"name filter: a row, its boxes away until asked for"_s);
    if (!nameRow || !nameStarts || !nameLacks) return;
    click(nameRow);
    check(nameStarts->isVisible() && nameLacks->isVisible() && !launcher.filtersActive(), u"name filter: a click shows the boxes, nothing narrowed yet"_s);
    check(nameLacks->mapToItem(sidebar, QPointF(nameLacks->width(), 0)).x() <= sidebar->width() + 0.5, u"name filter: the boxes stay inside the side bar"_s);
    click(nameRow);
    check(!nameStarts->isVisible(), u"name filter: empty, a second click puts the boxes away"_s);
    launcher.setNameFilter({}, u"REPORT"_s, {});
    settle(window);
    check(launcher.filtersActive() && launcher.nameFilterSet() && nameStarts->isVisible() && files().size() == 4,
        u"name filter: an end is the end before the extension, any case: "_s + files().join(u" | "_s));
    launcher.setNameFilter(u"report"_s, {}, {});
    settle(window);
    check(files().isEmpty(), u"name filter: a beginning is not found in the middle: "_s + files().join(u" | "_s));
    launcher.setNameFilter(u"WSQuick rep"_s, u".log"_s, {});
    settle(window);
    check(files() == QStringList({u"Q:\\$RECYCLE.BIN\\wsquick report.log"_s, u"Q:\\work\\wsquick report.log"_s}),
        u"name filter: a beginning with a space in it and an extension as the end: "_s + files().join(u" | "_s));
    launcher.toggleQuickFilter(0);
    launcher.setNameFilter(u"wsquick"_s, {}, u"docx !nothing"_s);
    settle(window);
    check(files() == QStringList({u"Q:\\work\\wsquick report.log"_s}), u"name filter: words kept out, beside a quick filter: "_s + files().join(u" | "_s));
    check(nameRow->property("value").toString() == u"2 项" && nameRow->property("marked").toBool(),
        u"name filter: the row says how many of its boxes are set: "_s + nameRow->property("value").toString());
    check(launcher.query() == u"wsquick", u"name filter: the search box keeps what was typed"_s);
    check(window->grabWindow().save(output.filePath(u"panel-name-filter.png"_s)), u"capture the name filter, set"_s);
    click(nameRow);
    check(nameStarts->isVisible(), u"name filter: set, its boxes stay"_s);
    launcher.clearFilters();
    settle(window);
    check(!launcher.filtersActive() && !launcher.nameFilterSet() && launcher.nameStarts().isEmpty() && files().size() == 4,
        u"name filter: 清除全部 empties it"_s);
    click(nameRow);
    settle(window);

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

    // Named by keyword among what is typed: in the plain box too, with no chip.
    launcher.setQuickFilters({{u"排除回收站"_s, u"!$RECYCLE.BIN\\"_s, u"nobin"_s}, {u"只看日志"_s, u"ext:log"_s, u"logs"_s}});
    launcher.setPanel(false);
    launcher.setQuery(u"wsquick"_s);
    settle(window);
    check(files().size() == 4, u"quick filter keywords: none typed, every match"_s);
    launcher.setQuery(u":nobin wsquick"_s);
    settle(window);
    check(files() == QStringList({u"Q:\\work\\wsquick report.docx"_s, u"Q:\\work\\wsquick report.log"_s}),
        u"quick filter keywords: one typed in the box narrows it: "_s + files().join(u" | "_s));
    check(launcher.query() == u":nobin wsquick" && !launcher.filtersActive(), u"quick filter keywords: the box keeps what was typed, no chip goes on"_s);
    launcher.setQuery(u"wsquick logs: :NOBIN"_s);
    settle(window);
    check(files() == QStringList({u"Q:\\work\\wsquick report.log"_s}), u"quick filter keywords: two typed, both hold: "_s + files().join(u" | "_s));
    launcher.setQuery(u"wsquick"_s);
    settle(window);
    check(files().size() == 4, u"quick filter keywords: taken out of the box, the rows come back: "_s + files().join(u" | "_s));
    // A keyword taken away in the settings: the word is text again.
    launcher.setQuery(u":nobin wsquick"_s);
    launcher.setQuickFilters({{u"排除回收站"_s, u"!$RECYCLE.BIN\\"_s}});
    settle(window);
    check(files().isEmpty(), u"quick filter keywords: without its keyword the word is searched for"_s);
    launcher.setQuickFilters({{u"排除回收站"_s, u"!$RECYCLE.BIN\\"_s, u"nobin"_s}});
    settle(window);
    check(files().size() == 2, u"quick filter keywords: given back, it narrows at once"_s);
    // In the panel a chip and a keyword together.
    launcher.setPanel(true);
    launcher.setQuickFilters({{u"排除回收站"_s, u"!$RECYCLE.BIN\\"_s, u"nobin"_s}, {u"只看日志"_s, u"ext:log"_s, u"logs"_s}});
    launcher.toggleQuickFilter(1);
    settle(window);
    check(files() == QStringList({u"Q:\\work\\wsquick report.log"_s}), u"quick filter keywords: a chip and a keyword together: "_s + files().join(u" | "_s));
    launcher.clearFilters();
    settle(window);

    // The filters over the rows: the same as chips in the one line.
    launcher.setQuery(u"wsquick"_s);
    launcher.setPanelFiltersOnTop(true);
    settle(window);
    auto* chip = item(window->contentItem(), u"panelQuickFilter_0"_s);
    auto* bar = item(window->contentItem(), u"panelFilterBar"_s);
    check(chip && bar && chip->isVisible() && item(bar, u"panelQuickFilter_0"_s) == chip && item(bar, u"panelQuickFilterAdd"_s),
        u"quick filters: chips in the line over the rows"_s);
    if (chip) {
        click(chip);
        check(on(0) && chip->property("active").toBool() && files().size() == 2, u"quick filters: a chip over the rows switches one on"_s);
        check(window->grabWindow().save(output.filePath(u"panel-quick-filters-top.png"_s)), u"capture the quick filters over the rows"_s);
    }
    // 名称 there: a chip, its boxes beside it in the line.
    launcher.clearFilters();
    settle(window);
    auto* nameChip = item(bar, u"panelFilterName"_s);
    auto* topStarts = item(bar, u"panelNameStarts"_s);
    check(nameChip && topStarts && nameChip->isVisible() && !topStarts->isVisible(), u"name filter: a chip over the rows, its boxes away"_s);
    if (nameChip && topStarts) {
        click(nameChip);
        check(topStarts->isVisible(), u"name filter: the chip shows the boxes in the line"_s);
        auto* topLacks = item(bar, u"panelNameLacks"_s);
        auto* clear = item(bar, u"panelFilterLayoutKey"_s);
        const qreal right = topLacks ? topLacks->mapToItem(bar, QPointF(topLacks->width(), 0)).x() : 0;
        check(topLacks && clear && right > 0 && right <= clear->mapToItem(bar, QPointF(0, 0)).x(),
            u"name filter: in a narrow window the line moves to show the boxes: right edge at %1"_s.arg(right));
        launcher.setNameFilter(u"wsquick"_s, u".log"_s, {});
        settle(window);
        check(nameChip->property("active").toBool() && files().size() == 2, u"name filter: set over the rows, the chip is on: "_s + files().join(u" | "_s));
        auto* clearLink = item(bar, u"panelFilterClear"_s);
        const qreal edge = topLacks ? topLacks->mapToItem(bar, QPointF(topLacks->width(), 0)).x() : 0;
        check(clearLink && clearLink->isVisible() && edge <= clearLink->mapToItem(bar, QPointF(0, 0)).x(),
            u"name filter: the boxes stay in view when 清除全部 comes beside them: right edge at %1"_s.arg(edge));
        check(window->grabWindow().save(output.filePath(u"panel-name-filter-top.png"_s)), u"capture the name filter over the rows"_s);
    }
    launcher.clearFilters();
    launcher.setPanelFiltersOnTop(false);
    settle(window);
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

// The menu's columns as they stand on the screen, side by side, each its own
// grab (off the screen, nothing else is there to take).
bool saveColumns(const QList<QQuickWindow*>& windows, const QString& path)
{
    QRect bounds;
    for (const QQuickWindow* window : windows)
        bounds |= window->geometry();
    const qreal dpr = windows.first()->devicePixelRatio();
    QImage image(bounds.size() * dpr, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    for (QQuickWindow* window : windows) {
        QImage grab = window->grabWindow();
        grab.setDevicePixelRatio(1);
        painter.drawImage(QPointF((window->x() - bounds.x()) * dpr, (window->y() - bounds.y()) * dpr), grab);
    }
    painter.end();
    return image.save(path);
}

void folderGroups(QQmlEngine* engine, const QDir& output, Checks& check)
{
    FolderMenu menu(nullptr, nullptr);
    QPoint pointer(-100000, -100000); // where the menu sees the system's pointer (#348)
    menu.setCursorSource([&pointer] { return pointer; });
    QVariantList children;
    for (int i = 0; i < 12; ++i)
        children.append(QVariantMap {{u"text"_s, u"项目资料 %1"_s.arg(i + 1)}, {u"action"_s, i}, {u"glyph"_s, u"\uE8B7"_s}});
    menu.previewEntries(FolderMenu::groupEntries(u"最近的文件夹"_s, u"\uE81C"_s, children, 3));
    QQmlComponent component(engine, u"WinShun"_s, u"FolderMenuWindow"_s);
    std::unique_ptr<QObject> root(component.createWithInitialProperties({{u"folderMenu"_s, QVariant::fromValue(&menu)}}));
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    check(window != nullptr, u"folder menu with direct rows loads: "_s + component.errorString());
    if (!window) return;
    const int offset = static_cast<int>(std::floor(24000 / window->devicePixelRatio()));
    root->setProperty("availableArea", QRectF(offset, offset, 1200, 1000));
    root->setProperty("popupAnchor", QPointF(offset + 8, offset + 8));
    showOffscreen(window);
    const auto value = [&](const QString& script) {
        return QQmlExpression(qmlContext(root.get()), root.get(), script).evaluate();
    };
    const auto depth = [&] { return root->property("depth").toInt(); };
    const auto columnAt = [&](int visual) { return root->findChild<QQuickWindow*>(u"folderMenuColumn%1"_s.arg(visual)); };
    const auto rowTop = [](QQuickWindow* shown, int row) {
        QQuickItem* entry = shown ? item(shown->contentItem(), u"folderMenuEntry%1"_s.arg(row)) : nullptr;
        return entry ? entry->mapToGlobal(QPointF()).y() : -1e6;
    };
    QMetaObject::invokeMethod(root.get(), "handleKey", Q_ARG(QVariant, Qt::Key_Down), Q_ARG(QVariant, QString()));
    check(root->property("highlighted").toInt() == 1, u"keyboard navigation skips the inline section heading"_s);
    check(window->grabWindow().save(output.filePath(u"folder-direct.png"_s)), u"capture direct folders and More"_s);
    // More, chosen by the keys: the remaining folders in a column of their
    // own beside the menu (#348), the first one chosen, More still highlighted.
    QMetaObject::invokeMethod(root.get(), "activate", Q_ARG(QVariant, 0), Q_ARG(QVariant, 4), Q_ARG(QVariant, false));
    settle(window);
    QQuickWindow* more = columnAt(1);
    check(more && more->isVisible() && depth() == 2, u"More opens a column beside the menu"_s);
    if (!more) {
        menu.dismiss();
        return;
    }
    settle(more);
    check(value(u"pages[0].pageTitle"_s).toString() == u"最近的文件夹" && value(u"pages[0].children.length"_s).toInt() == 9,
        u"More opens the remaining folders under their original group title"_s);
    check(root->property("highlighted").toInt() == 0 && value(u"rowShown(0)"_s).toInt() == 4,
        u"its first folder is chosen and More stays highlighted"_s);
    // Over its edge by the border they share: Windows' own, 2 px at 144 dpi
    // while Qt's scale here is 1 (tst_foldermenu checks it on the screen's pixels).
    const int overlap = window->x() + window->width() - more->x();
    check(more->width() == window->width() && overlap >= 1 && overlap <= 2,
        u"the column is as wide as the menu, beside it over its edge (x %1, menu %2 + %3)"_s.arg(more->x())
            .arg(window->x()).arg(window->width()));
    check(std::abs(rowTop(more, 0) - rowTop(window, 4)) <= 1,
        u"its first row is level with More (%1, %2)"_s.arg(rowTop(more, 0)).arg(rowTop(window, 4)));
    check(saveColumns({window, more}, output.filePath(u"folder-more.png"_s)), u"capture the menu and the column of the remaining folders"_s);
    QMetaObject::invokeMethod(root.get(), "handleKey", Q_ARG(QVariant, Qt::Key_Left), Q_ARG(QVariant, QString()));
    check(depth() == 1 && !more->isVisible() && root->property("highlighted").toInt() == 4,
        u"Left closes the column, back on More"_s);

    // The menu stays where it went while columns come and go beside it
    // (#349: a shorter page used to drop below a pointer near the foot).
    QVariantList rows;
    for (int i = 0; i < 10; ++i)
        rows.append(QVariantMap {{u"text"_s, u"文件夹 %1"_s.arg(i + 1)}, {u"action"_s, i}, {u"glyph"_s, u"\uE8B7"_s}});
    rows.append(QVariantMap {{u"text"_s, u"收藏夹"_s}, {u"glyph"_s, u"\uE734"_s},
        {u"children"_s, QVariantList {QVariantMap {{u"text"_s, u"v10-dto"_s}, {u"action"_s, 10}},
                            QVariantMap {{u"text"_s, u"Repos"_s}, {u"action"_s, 11}}}}});
    menu.previewEntries(rows);
    root->setProperty("popupAnchor", QPointF(offset + 400, offset + 900));
    settle(window);
    QMetaObject::invokeMethod(root.get(), "place");
    settle(window);
    const QPoint at = window->position();
    check(at.y() + window->height() <= offset + 900 + 1, u"a tall menu near the screen's foot opens above the pointer"_s);
    QMetaObject::invokeMethod(root.get(), "activate", Q_ARG(QVariant, 0), Q_ARG(QVariant, 10), Q_ARG(QVariant, false));
    settle(window);
    settle(more);
    check(depth() == 2 && more->isVisible() && window->position() == at,
        u"a column coming keeps the menu where it was (%1,%2, was %3,%4)"_s.arg(window->x()).arg(window->y()).arg(at.x()).arg(at.y()));
    check(more->y() >= offset + 8 && more->y() + more->height() <= offset + 1000 - 8
            && std::abs(rowTop(more, 0) - rowTop(window, 10)) <= 1,
        u"the column's first row is level with its row, inside the work area (%1, %2)"_s.arg(rowTop(more, 0)).arg(rowTop(window, 10)));
    QMetaObject::invokeMethod(root.get(), "back");
    settle(window);
    check(depth() == 1 && !more->isVisible() && window->position() == at,
        u"closing it leaves the menu where it was (%1,%2, was %3,%4)"_s.arg(window->x()).arg(window->y()).arg(at.x()).arg(at.y()));

    // A column that comes under a pointer at rest opens nothing until the
    // pointer moves (#348: one hover went all the way down).
    const QVariantMap deepest {{u"text"_s, u"最深"_s}, {u"children"_s, QVariantList {QVariantMap {{u"text"_s, u"底"_s}, {u"action"_s, 13}}}}};
    const QVariantMap deeper {{u"text"_s, u"更深"_s}, {u"children"_s, QVariantList {deepest}}};
    const QVariantMap deep {{u"text"_s, u"深"_s}, {u"children"_s, QVariantList {deeper}}};
    menu.previewEntries({deep});
    settle(window);
    const auto pump = [](int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    };
    const QVariant noRow = QVariant::fromValue<QObject*>(nullptr);
    QMetaObject::invokeMethod(root.get(), "open", Q_ARG(QVariant, 0), Q_ARG(QVariant, 0), Q_ARG(QVariant, true));
    settle(window);
    QMetaObject::invokeMethod(root.get(), "pointed", Q_ARG(QVariant, 1), Q_ARG(QVariant, 0), Q_ARG(QVariant, noRow));
    pump(600);
    check(depth() == 2, u"a row that comes under the pointer at rest opens nothing (%1 columns)"_s.arg(depth()));
    pointer += QPoint(10, 0); // the pointer moved since
    QMetaObject::invokeMethod(root.get(), "pointed", Q_ARG(QVariant, 1), Q_ARG(QVariant, 0), Q_ARG(QVariant, noRow));
    pump(600);
    check(depth() == 3 && value(u"pages[1].text"_s).toString() == u"更深",
        u"once the pointer moves, resting on the row opens its column (%1 columns)"_s.arg(depth()));
    menu.dismiss();
    check(!window->isVisible() && !more->isVisible(), u"closing the menu hides every column"_s);
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

namespace {
// A believable folder of office files under D: (sizes and dates filled,
// nothing real), for the posters below.
struct DemoReport {
    QString folder; // under D:, split by /
    QString name;
    std::uint64_t kilobytes;
    int daysAgo;
    int minute; // of the day, local time
    bool listed; // what the name filter's boxes let through
};
const QList<DemoReport> kDemoReports {
    {u"工作/2026 年度/总结"_s, u"2026 年度工作报告.docx"_s, 1280, 1, 17 * 60 + 42, true},
    {u"工作/2026 年度/总结"_s, u"2026 年度工作报告 草稿.docx"_s, 1236, 6, 11 * 60 + 5, false},
    {u"工作/2026 年度/总结"_s, u"2026 年度工作报告.pdf"_s, 2410, 1, 17 * 60 + 50, false},
    {u"工作/2026 年度/销售"_s, u"2026 Q1 销售报告.docx"_s, 482, 62, 10 * 60 + 14, true},
    {u"工作/2026 年度/销售"_s, u"2026 Q1 销售报告 旧.docx"_s, 455, 70, 15 * 60 + 30, false},
    {u"工作/2026 年度/销售"_s, u"2026 Q2 销售报告.docx"_s, 516, 33, 9 * 60 + 48, true},
    {u"工作/2026 年度/销售"_s, u"2026 Q3 销售报告.docx"_s, 538, 5, 14 * 60 + 26, true},
    {u"工作/2026 年度/市场"_s, u"2026 市场调研报告.docx"_s, 2348, 12, 16 * 60 + 8, true},
    {u"工作/2026 年度/项目"_s, u"2026 项目验收报告.docx"_s, 724, 3, 11 * 60 + 36, true},
    {u"工作/2026 年度/财务"_s, u"2026 财务分析报告.docx"_s, 356, 8, 13 * 60 + 52, true},
    {u"工作/2026 年度/财务"_s, u"2026 财务分析报告.xlsx"_s, 218, 8, 13 * 60 + 40, false},
    {u"工作/2026 年度/人事"_s, u"2026 年中述职报告.docx"_s, 118, 143, 9 * 60 + 20, true},
    {u"工作/2025 年度/总结"_s, u"2025 年度工作报告.docx"_s, 1192, 330, 16 * 60 + 15, false},
    {u"工作/模板"_s, u"报告模板.docx"_s, 64, 210, 10 * 60 + 2, false},
};

// The search window (Main.qml) on those files, in a profile of its own,
// shown off screen at the posters' size; `panel`: the folder panel's layout.
struct DemoSearch {
    DemoSearch(QQmlEngine* engine, bool panel);

    QTemporaryDir profile;
    IndexService index {profile.filePath(u"index"_s), {}};
    AppCatalog apps;
    SystemCatalog places;
    SearchEngine search {&index, &apps, &places};
    History history {profile.filePath(u"history.txt"_s)};
    Launcher launcher {&index, &apps, &places, &search, &history};
    ClipStore store {profile.filePath(u"clipboard"_s)};
    ClipboardWatcher watcher {{.record = false}, {}};
    Clipboard clipboard {&store, &watcher};
    Placement placement {profile.filePath(u"state.ini"_s), u"Launcher"_s};
    WindowFrame frame {false};
    QString error; // the window did not load
    std::unique_ptr<QObject> root; // last: gone first
    QQuickWindow* window = nullptr;
};

DemoSearch::DemoSearch(QQmlEngine* engine, bool panel)
{
    constexpr qint64 today = 1795132800 + 4 * 86400 - 8 * 3600; // 2026-11-24 00:00 in UTC+8
    {
        const auto files = index.index();
        const auto lock = files->writeLock();
        const EntryId drive = files->addRoot("D:");
        QHash<QString, EntryId> folders;
        for (const DemoReport& entry : kDemoReports) {
            EntryId parent = drive;
            QString path;
            for (const QString& part : entry.folder.split(u'/')) {
                path += u'/' + part;
                if (!folders.contains(path))
                    folders.insert(path, files->add(parent, part.toStdString(), EntryFlag::Directory));
                parent = folders.value(path);
            }
            const EntryId id = files->add(parent, entry.name.toStdString(), 0);
            files->setModified(id, static_cast<FileTime>(today - entry.daysAgo * 86400 + entry.minute * 60));
            files->setSize(id, packSize(entry.kilobytes * 1024));
        }
    }
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    launcher.setPanel(panel);
    launcher.setPanelSidebarVisible(panel);
    store.open(QDateTime::currentMSecsSinceEpoch());
    QQmlComponent component(engine, u"WinShun"_s, u"Main"_s);
    root.reset(component.createWithInitialProperties({
        {u"launcher"_s, QVariant::fromValue(&launcher)}, {u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"placement"_s, QVariant::fromValue(&placement)}, {u"frame"_s, QVariant::fromValue(&frame)}}));
    window = qobject_cast<QQuickWindow*>(root.get());
    if (!window) {
        error = component.errorString();
        return;
    }
    window->create();
    window->resize(1060, 560);
    frame.setWindow(window);
    launcher.setWindow(window);
    showOffscreen(window);
    launcher.results()->setThumbnails(false); // synthetic paths: no thumbnail handler or real file access
    launcher.setScope(Launcher::Files); // names only: no hint about files whose content was not searched
    launcher.setQuery(u"报告"_s);
    launcher.sortBy(static_cast<int>(SortKey::Modified)); // the newest first, as clicking 修改时间 does
    settle(window);
}

bool writeChecks(const Checks& check, const QDir& output, const QString& name)
{
    QFile file(output.filePath(name));
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(QJsonObject {{u"passed"_s, check.passed}, {u"checks"_s, check.rows}}).toJson());
    return check.passed;
}
} // namespace

// --name-filter-poster: the picture of the panel's 名称 filter (GitHub #457)
// for the release poster. Not one of the workflow checks above: it sets the
// three boxes as someone looking for this year's reports would.
bool checkNameFilterPoster(QQmlEngine* engine, const QDir& output)
{
    Checks check;
    DemoSearch demo(engine, true);
    check(demo.window != nullptr, u"name filter poster: the search window loads: "_s + demo.error);
    if (!demo.window) return false;
    QQuickWindow* window = demo.window;
    Launcher& launcher = demo.launcher;
    const auto listed = [&] {
        QStringList names;
        for (int i = 0; i < launcher.results()->count(); ++i) {
            if (const QString path = launcher.results()->pathAt(i); path.startsWith(u"D:\\"_s))
                names.append(path.mid(path.lastIndexOf(u'\\') + 1));
        }
        names.sort();
        return names;
    };
    check(listed().size() == kDemoReports.size(), u"name filter poster: every report is listed before narrowing: "_s + QString::number(listed().size()));
    launcher.setNameFilter(u"2026"_s, u".docx"_s, u"草稿 旧"_s);
    settle(window);
    QStringList expected;
    for (const DemoReport& entry : kDemoReports) {
        if (entry.listed)
            expected.append(entry.name);
    }
    expected.sort();
    check(listed() == expected, u"name filter poster: only what begins 2026, ends .docx and has no 草稿 or 旧: "_s + listed().join(u" | "_s));
    check(launcher.filtersActive() && launcher.nameFilterSet(), u"name filter poster: the filter is on"_s);
    for (int i = 0; i < 3; ++i) // the file icons, read on the provider's threads
        settle(window);
    check(window->grabWindow().save(output.filePath(u"name-filter-poster.png"_s)), u"capture the name filter on real-looking files"_s);
    launcher.clearFilters();
    settle(window);
    window->hide();
    return writeChecks(check, output, u"name-filter-poster.json"_s);
}

bool checkRowStatesPoster(QQmlEngine* engine, const QDir& output)
{
    Checks check;
    DemoSearch demo(engine, false);
    check(demo.window != nullptr, u"row states: the search window loads: "_s + demo.error);
    if (!demo.window) return false;
    QQuickWindow* window = demo.window;
    Launcher& launcher = demo.launcher;
    check(launcher.results()->count() >= 3, u"row states: the reports are listed: "_s + QString::number(launcher.results()->count()));
    launcher.setCurrentRow(0);
    settle(window);
    // Off screen the window has room for three rows: the keyboard's, one at rest, the pointer's.
    QQuickItem* pointed = item(window->contentItem(), u"resultRow-2"_s);
    check(pointed != nullptr, u"row states: the third row is drawn"_s);
    if (pointed) {
        const QPointF at = pointed->mapToScene(QPointF(pointed->width() * 0.6, pointed->height() / 2));
        QMouseEvent move(QEvent::MouseMove, at, window->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &move);
    }
    for (int i = 0; i < 3; ++i) // the hover's fade, the file icons
        settle(window);
    check(launcher.currentRow() == 0, u"row states: the pointer leaves the keyboard's row where it was"_s);
    check(pointed && pointed->mapToScene(QPointF(0, pointed->height())).y() <= window->height(), u"row states: the pointer's row is in the window"_s);
    check(window->grabWindow().save(output.filePath(u"row-states.png"_s)), u"row states: picture saved"_s);
    window->hide();
    return writeChecks(check, output, u"row-states.json"_s);
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
    clipboard.setRowOptions(false, u"standard"_s, SystemTheme::uiSize());
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
    clipboard.setRowOptions(false, u"large"_s, SystemTheme::uiSize());
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
    QMetaObject::invokeMethod(window, "showSetting", Q_ARG(QVariant, u"scope"_s), Q_ARG(QVariant, 2), // 搜索范围 › 磁盘
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
        for (int i = 0; i < 25; ++i) // more than the most rows the box can be set to
            index.index()->add(drive, QByteArray("ledger-") + QByteArray::number(i) + ".txt", 0);
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
    // Its letters are searched before a candidate is chosen (GitHub #157), Microsoft Pinyin's separators left out.
    check(launcher.query() == u"zhong"_s && field->property("text").toString().isEmpty(),
        u"IME composition: the letters are the query, the field's text is the input method's"_s);
    QInputMethodEvent separated(u"re'port"_s, {});
    QCoreApplication::sendEvent(field, &separated);
    settle(window);
    check(launcher.query() == u"report"_s && launcher.results()->count() == 1, u"IME composition finds rows by its letters"_s);
    QInputMethodEvent endComposition;
    QCoreApplication::sendEvent(field, &endComposition);
    check(launcher.query().isEmpty(), u"IME composition given up: nothing typed"_s);
    launcher.setQuery(u"report"_s);
    settle(window);
    check(launcher.results()->count() == 1, u"typing searches the real index"_s);
    // The scope tabs and the footer come back; the two keys right of the tabs
    // (the history, the panel) stay away in this mode (GitHub #185, #244),
    // and the tabs take their place at the box's edge.
    check(!window->property("pureEmpty").toBool() && footer->isVisible() && tabs->isVisible(),
        u"typing restores the results' controls"_s);
    {
        auto* historyKey = item(window->contentItem(), u"queryHistoryButton"_s);
        check(!expand->isVisible() && historyKey && !historyKey->isVisible(), u"pure search keeps the history and panel keys away while typing"_s);
        const qreal tabsRight = tabs->mapToItem(window->contentItem(), QPointF(tabs->width(), 0)).x();
        check(window->width() - tabsRight < 32, u"the scope tabs end at the box's edge"_s);
    }
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
    {
        // 范围标签 › 合成一个按钮 (GitHub #185, #244): one key saying the
        // current scope in the tabs' place; a click goes to the next, as Tab does.
        launcher.setQuery(u"report"_s);
        launcher.setScope(Launcher::All);
        settle(window);
        auto* scopeKey = item(window->contentItem(), u"searchScopeKey"_s);
        check(scopeKey && !scopeKey->isVisible() && tabs->isVisible(), u"the scopes are tabs side by side unless chosen"_s);
        launcher.setScopeButton(true);
        settle(window);
        check(scopeKey && scopeKey->isVisible() && !tabs->isVisible(), u"one scope key takes the tabs' place"_s);
        if (scopeKey) {
            const qreal keyRight = scopeKey->mapToItem(window->contentItem(), QPointF(scopeKey->width(), 0)).x();
            const qreal fieldRight = field->mapToItem(window->contentItem(), QPointF(field->width(), 0)).x();
            check(fieldRight <= scopeKey->mapToItem(window->contentItem(), QPointF(0, 0)).x() && keyRight <= window->width(),
                u"the field ends before the scope key, the key inside the box"_s);
            const Launcher::Scope before = launcher.scope();
            QMetaObject::invokeMethod(scopeKey, "next");
            settle(window);
            check(launcher.scope() != before, u"a click on the scope key goes to the next scope"_s);
            check(window->grabWindow().save(output.filePath(u"scope-key.png"_s)), u"capture the scope key"_s);
            key(field, Qt::Key_Tab);
            settle(window);
            check(scopeKey->isVisible(), u"Tab moves through the scopes with the key as with the tabs"_s);
        }
        launcher.setScopeButton(false);
        launcher.setScope(Launcher::All);
        launcher.setQuery({});
        settle(window);
    }
    settle(window);
    check(footer->isVisible() && tabs->isVisible(), u"switching off restores the original presentation"_s);
    // 设置 › 外观 › 搜索框显示行数 (Settings::maxRows): the box is as tall as
    // that many rows, where the screen has the room.
    launcher.setScope(Launcher::All);
    launcher.setQuery(u"ledger"_s);
    settle(window);
    const int rowHeight = window->property("rowHeight").toInt();
    const auto rows = [&] { return window->property("listRows").toInt(); };
    check(launcher.results()->count() == 25, u"rows: 25 found, got %1"_s.arg(launcher.results()->count()));
    check(window->property("maxRows").toInt() == 8 && rows() == 8, u"rows: 8 by default, shows %1"_s.arg(rows()));
    const int eightHeight = window->height();
    launcher.setMaxRows(12);
    settle(window);
    const int fit = window->property("fitRows").toInt();
    check(window->property("maxRows").toInt() == 12 && fit > 8 && rows() == fit,
        u"rows: set to 12, fits %1, shows %2"_s.arg(fit).arg(rows()));
    check(window->height() == eightHeight + (fit - 8) * rowHeight,
        u"rows: the window grows by the rows added (%1 -> %2, rows of %3)"_s.arg(eightHeight).arg(window->height()).arg(rowHeight));
    check(window->grabWindow().save(output.filePath(u"rows-12.png"_s)), u"capture 12 rows"_s);
    launcher.setMaxRows(4);
    settle(window);
    check(rows() == 4 && window->height() == eightHeight - 4 * rowHeight, u"rows: set to 4, shows %1"_s.arg(rows()));
    launcher.setFolderSearch(true);
    settle(window);
    check(window->property("maxRows").toInt() == 4, u"rows: a folder's box follows a setting under 6"_s);
    launcher.setMaxRows(20);
    settle(window);
    check(window->property("maxRows").toInt() == 6, u"rows: a folder's box stays at 6"_s);
    launcher.setFolderSearch(false);
    launcher.setMaxRows(99);
    check(launcher.maxRows() == 20, u"rows: never more than 20"_s);
    launcher.setMaxRows(8);
    launcher.setQuery({});
    settle(window);
    {
        // Web searches (GitHub #320): a site's keyword after the words puts
        // its search on top as before them; nothing found, a row searches the
        // first site that does (没有结果时用网页搜索), unless switched off.
        const auto shows = [&](int count) {
            QElapsedTimer waiting;
            waiting.start();
            while ((launcher.searching() || launcher.results()->count() != count) && waiting.elapsed() < 3000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            settle(window);
            return !launcher.searching() && launcher.results()->count() == count;
        };
        const auto first = [&] { return launcher.results()->at(0); };
        launcher.setWebShortcuts({{u"page"_s, u"A page"_s, u"https://example.com/page"_s, {}},
            {u"gg"_s, u"Google"_s, u"https://www.google.com/search?q=%s"_s, {}},
            {u"bd"_s, u"Baidu"_s, u"https://www.baidu.com/s?wd=%s"_s, {}}});
        launcher.setQuery(u"report gg"_s);
        check(shows(1) && first()->isWeb() && first()->keyword == u"gg" && first()->words == u"report",
            u"web: the keyword after the words searches its site for them"_s);
        launcher.setQuery(u"how to cook eggs"_s);
        check(shows(1) && first()->isWeb() && first()->keyword == u"gg"
                && first()->target == u"https://www.google.com/search?q=how%20to%20cook%20eggs",
            u"web: nothing found, a row searches the first site that searches"_s);
        check(!window->property("showEmptyState").toBool(), u"web: that row stands in for the empty state"_s);
        check(window->grabWindow().save(output.filePath(u"web-fallback.png"_s)), u"capture the web search row"_s);
        launcher.setQuery(u"report"_s);
        check(shows(1) && !first()->isWeb(), u"web: something found, no such row"_s);
        launcher.setWebFallback(false);
        launcher.setQuery(u"how to cook eggs"_s);
        check(shows(0), u"web: switched off, nothing found lists nothing"_s);
        settle(window);
        check(window->property("showEmptyState").toBool(), u"web: and says so as before"_s);
        launcher.setWebFallback(true);
        launcher.setWebShortcuts({});
        launcher.setQuery({});
        settle(window);
    }
    {
        // 搜索面板空着时列出全部文件 (Settings::panelListsAll, GitHub #321):
        // on, the empty panel lists every file as Everything does; off,
        // nothing until something is typed or a filter chosen.
        const auto lists = [&](int count) {
            QElapsedTimer waiting;
            waiting.start();
            while ((launcher.searching() || launcher.results()->count() != count) && waiting.elapsed() < 3000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            settle(window);
            return !launcher.searching() && launcher.results()->count() == count;
        };
        window->resize(900, 600);
        launcher.setPanel(true);
        launcher.handleShown(); // asks again, as the app does after sizing the window
        check(lists(26), u"panel: empty, it lists every file by default"_s);
        launcher.setPanelListsAll(false);
        check(lists(0), u"panel: set so, empty it lists nothing"_s);
        check(window->grabWindow().save(output.filePath(u"panel-empty.png"_s)), u"capture the empty panel"_s);
        launcher.setQuery(u"ledger"_s);
        check(lists(25), u"panel: words typed are searched as ever"_s);
        launcher.setQuery({});
        check(lists(0), u"panel: cleared, nothing again"_s);
        launcher.setFolderFilter(u"D:\\"_s);
        check(lists(26), u"panel: a folder chosen is something asked for, and lists"_s);
        launcher.setFolderFilter({});
        check(lists(0), u"panel: the filter gone, nothing again"_s);
        launcher.setPanelListsAll(true);
        check(lists(26), u"panel: switched back on, every file at once"_s);
        launcher.setPanel(false);
        launcher.handleShown(); // asks again, as the app does after sizing the window
        settle(window);
    }
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
        // GitHub #391: an empty folder and a file of 0 bytes, tagged after their names.
        index.index()->add(samples, "zqempty-dir", EntryFlag::Directory);
        index.index()->setSize(index.index()->add(samples, "zqempty-file.txt", 0), packSize(0));
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
        // 密集 (GitHub #352): the launcher's rows are one line, the folder after the name.
        const bool oneLine = !folder && SystemTheme::uiSize() == u"dense";
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
            if (runs.size() != (oneLine ? 1u : 2u)) {
                check(false, mode + u": row %1 shows %2 of ink (%3)"_s.arg(i).arg(oneLine ? u"one line"_s : u"two lines"_s).arg(runs.size()));
                break;
            }
            rows.push_back({area.top() + runs[0].first, area.top() + runs[0].second, area.top() + runs.back().first,
                area.top() + runs.back().second});
        }
        check(rows.size() == 3, mode + u": three rows measured"_s);
        if (rows.size() != 3) continue;
        if (oneLine) {
            check(picture.save(output.filePath(u"rows-launcher.png"_s)), mode + u": saved"_s);
            continue; // no second line to keep apart from the next name
        }
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

    // GitHub #391: 空文件夹 / 空文件 right after the name, on the selected
    // row (its buttons on the right) as on the others, clear of what is on the right.
    launcher.setQuery(u"zqempty"_s);
    settle(window);
    check(launcher.results()->count() == 2, u"empty tag: the empty folder and file found"_s);
    QJsonArray tags;
    for (int i = 0; i < 2; ++i) {
        auto* row = item(window->contentItem(), u"resultRow-%1"_s.arg(i));
        auto* tag = row ? item(row, u"emptyTag"_s) : nullptr;
        check(tag && tag->isVisible(), u"empty tag: row %1 shows it"_s.arg(i));
        if (!row || !tag) continue;
        const QRectF at = tag->mapRectToItem(row, QRectF(0, 0, tag->width(), tag->height()));
        const qreal room = row->width() - row->property("trailingSpace").toReal();
        const bool current = row->property("current").toBool();
        check(at.left() > 0 && at.right() <= room, u"empty tag: row %1 (%2) at %3–%4, clear of the right side from %5"_s
            .arg(i).arg(current ? u"selected"_s : u"not selected"_s).arg(at.left()).arg(at.right()).arg(room));
        check(at.top() >= 0 && at.bottom() <= row->height(), u"empty tag: row %1 within the row"_s.arg(i));
        tags.append(QJsonObject {{u"row"_s, i}, {u"selected"_s, current}, {u"left"_s, at.left()}, {u"right"_s, at.right()},
            {u"room"_s, room}, {u"top"_s, at.top()}, {u"height"_s, at.height()}, {u"row_height"_s, row->height()}});
    }
    report.insert(u"empty_tags"_s, tags);
    check(window->grabWindow().save(output.filePath(u"rows-empty-tag.png"_s)), u"empty tag: saved"_s);

    // 已打开, in the same place: a row opened from this list says so (with
    // what it said before, if anything), the others do not; still clear of
    // the right side.
    if (launcher.results()->count() == 2) {
        launcher.results()->markOpened(launcher.results()->at(1)->path);
        settle(window);
        for (int i = 0; i < 2; ++i) {
            auto* row = item(window->contentItem(), u"resultRow-%1"_s.arg(i));
            auto* tag = row ? item(row, u"emptyTag"_s) : nullptr;
            if (!row || !tag) continue;
            const QString said = row->property("nameTag").toString();
            const QString before = row->property("emptyTag").toString();
            check(i == 1 ? said.startsWith(u"已打开") && said.endsWith(before) && said != before : said == before,
                u"opened tag: row %1 says “%2”"_s.arg(i).arg(said));
            const QRectF at = tag->mapRectToItem(row, QRectF(0, 0, tag->width(), tag->height()));
            const qreal room = row->width() - row->property("trailingSpace").toReal();
            check(tag->isVisible() && at.left() > 0 && at.right() <= room,
                u"opened tag: row %1 at %2–%3, clear of the right side from %4"_s.arg(i).arg(at.left()).arg(at.right()).arg(room));
        }
        check(window->grabWindow().save(output.filePath(u"rows-opened-tag.png"_s)), u"opened tag: saved"_s);
        launcher.results()->clearOpened();
        settle(window);
        auto* row = item(window->contentItem(), u"resultRow-1"_s);
        check(row && row->property("nameTag").toString() == row->property("emptyTag").toString(), u"opened tag: gone once forgotten"_s);
    }
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
    QMetaObject::invokeMethod(window, "showSetting", Q_ARG(QVariant, u"appearance"_s), Q_ARG(QVariant, 0),
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

bool checkPluginGallerySettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output)
{
    Checks check;
    QTemporaryDir temporary;
    const QString userDir = temporary.filePath(u"installed"_s);
    const QString bundledDir = temporary.filePath(u"bundled"_s);
    const auto writePlugin = [&](const QString& root, const QString& id, const QJsonObject& manifest, bool dummyExe = false) {
        const QString dir = QDir(root).filePath(id);
        QDir().mkpath(dir);
        QFile file(QDir(dir).filePath(u"plugin.json"_s));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        file.write(QJsonDocument(manifest).toJson());
        if (dummyExe) {
            QFile exe(QDir(dir).filePath(u"tool.exe"_s));
            if (!exe.open(QIODevice::WriteOnly))
                return false;
            exe.write("MZ dummy");
        }
        return true;
    };
    const auto zeroCmd = [](const QString& kw) {
        return QJsonObject {{u"id"_s, u"go"_s}, {u"title"_s, u"运行"_s}, {u"keyword"_s, kw},
            {u"run"_s, QJsonObject {{u"folder"_s, u"C:\\"_s}}}};
    };
    check(writePlugin(userDir, u"probe.manifest"_s, QJsonObject {
              {u"manifest"_s, 1}, {u"id"_s, u"probe.manifest"_s},
              {u"name"_s, QJsonObject {{u"zh"_s, u"配置插件"_s}, {u"en"_s, u"Configured plugin"_s}}},
              {u"version"_s, u"1.0.0"_s}, {u"author"_s, u"LingCore"_s},
              {u"description"_s, u"带有清单设置项的已安装插件"_s},
              {u"commands"_s, QJsonArray {zeroCmd(u"cfg"_s)}},
              {u"settings"_s, QJsonArray {QJsonObject {{u"key"_s, u"token"_s}, {u"type"_s, u"text"_s},
                                  {u"title"_s, u"令牌"_s}, {u"default"_s, u"abc"_s}}}}}),
        u"write installed manifest-settings plugin fixture"_s);
    check(writePlugin(userDir, u"probe.window"_s, QJsonObject {
              {u"manifest"_s, 1}, {u"id"_s, u"probe.window"_s},
              {u"name"_s, QJsonObject {{u"zh"_s, u"独立设置插件"_s}, {u"en"_s, u"Window settings plugin"_s}}},
              {u"version"_s, u"1.0.0"_s}, {u"author"_s, u"LingCore"_s},
              {u"description"_s, u"带独立设置窗口的已安装插件"_s},
              {u"runtime"_s, QJsonObject {{u"mode"_s, u"resident"_s}, {u"program"_s, u"tool.exe"_s}, {u"start"_s, u"lazy"_s}}},
              {u"permissions"_s, QJsonArray {u"ui"_s}},
              {u"settingsWindow"_s, true},
              {u"commands"_s, QJsonArray {QJsonObject {{u"id"_s, u"cap"_s}, {u"title"_s, u"独立设置"_s}, {u"keyword"_s, u"cap"_s}}}}}, true),
        u"write installed settingsWindow plugin fixture"_s);
    check(writePlugin(userDir, u"probe.disabled"_s, QJsonObject {
              {u"manifest"_s, 1}, {u"id"_s, u"probe.disabled"_s},
              {u"name"_s, QJsonObject {{u"zh"_s, u"已停用插件"_s}, {u"en"_s, u"Disabled plugin"_s}}},
              {u"version"_s, u"1.0.0"_s}, {u"author"_s, u"LingCore"_s},
              {u"description"_s, u"安装但当前已停用的插件"_s},
              {u"runtime"_s, QJsonObject {{u"mode"_s, u"resident"_s}, {u"program"_s, u"tool.exe"_s}, {u"start"_s, u"lazy"_s}}},
              {u"permissions"_s, QJsonArray {u"ui"_s}},
              {u"settingsWindow"_s, true},
              {u"commands"_s, QJsonArray {QJsonObject {{u"id"_s, u"off"_s}, {u"title"_s, u"停用"_s}, {u"keyword"_s, u"off"_s}}}}}, true),
        u"write installed disabled plugin fixture"_s);
    check(writePlugin(userDir, u"probe.update"_s, QJsonObject {
              {u"manifest"_s, 1}, {u"id"_s, u"probe.update"_s},
              {u"name"_s, QJsonObject {{u"zh"_s, u"可更新插件"_s}, {u"en"_s, u"Updatable plugin"_s}}},
              {u"version"_s, u"1.0.0"_s}, {u"author"_s, u"LingCore"_s},
              {u"description"_s, u"已安装且插件库有新版本"_s},
              {u"commands"_s, QJsonArray {zeroCmd(u"upd"_s)}},
              {u"settings"_s, QJsonArray {QJsonObject {{u"key"_s, u"flag"_s}, {u"type"_s, u"bool"_s},
                                  {u"title"_s, u"开关选项"_s}, {u"default"_s, true}}}}}),
        u"write updatable plugin fixture"_s);
    check(writePlugin(bundledDir, u"probe.bundled"_s, QJsonObject {
              {u"manifest"_s, 1}, {u"id"_s, u"probe.bundled"_s},
              {u"name"_s, QJsonObject {{u"zh"_s, u"随包插件"_s}, {u"en"_s, u"Bundled plugin"_s}}},
              {u"version"_s, u"1.0.0"_s}, {u"author"_s, u"LingCore"_s},
              {u"description"_s, u"随 Win顺 自带的插件"_s},
              {u"commands"_s, QJsonArray {zeroCmd(u"bnd"_s)}},
              {u"settings"_s, QJsonArray {QJsonObject {{u"key"_s, u"mode"_s}, {u"type"_s, u"text"_s},
                                  {u"title"_s, u"模式"_s}, {u"default"_s, u"fast"_s}}}}}),
        u"write bundled plugin fixture"_s);

    plugin::Host host({.userPluginDir = userDir, .bundledPluginDir = bundledDir, .dataRoot = temporary.filePath(u"data"_s),
        .version = QStringLiteral(WINSHUN_VERSION), .elevatedHost = false,
        .elevationRoot = temporary.filePath(u"elevated"_s), .elevationProtected = false});
    host.catalog().scan();
    editor->plugins()->setHost(&host);
    const auto detach = qScopeGuard([&] { editor->plugins()->setHost(nullptr); });
    const auto applyEnabled = [&] {
        for (const QString& id : {u"probe.manifest"_s, u"probe.window"_s, u"probe.update"_s, u"probe.bundled"_s}) {
            editor->plugins()->confirmEnable(id);
            host.setEnabled(id, true);
        }
        editor->plugins()->setEnabled(u"probe.disabled"_s, false);
        host.setEnabled(u"probe.disabled"_s, false);
    };
    applyEnabled();

    const auto indexItem = [&](const QString& id, const QString& name, const QString& version, bool bundled = false, bool fullLinks = false) {
        QJsonObject obj {
            {u"id"_s, id}, {u"name"_s, name}, {u"version"_s, version}, {u"author"_s, u"LingCore"_s},
            {u"description"_s, name + u" 的功能说明"_s}, {u"homepage"_s, u"https://github.com/LingCore/WinShun"_s},
            {u"review"_s, u"official"_s}, {u"url"_s, temporary.filePath(id + u".wsplugin"_s)},
            {u"size"_s, 1}, {u"sha256"_s, QString(64, u'a')}, {u"bundled"_s, bundled}};
        if (fullLinks) {
            obj.insert(u"readme"_s, u"https://example.test/README.md"_s);
            obj.insert(u"readmeSha256"_s, QString(64, u'b'));
            obj.insert(u"notes"_s, u"修复了一些问题"_s);
        }
        return obj;
    };
    const QString indexPath = temporary.filePath(u"gallery-settings-index.json"_s);
    QFile indexFile(indexPath);
    if (!indexFile.open(QIODevice::WriteOnly)) return false;
    indexFile.write(QJsonDocument(QJsonObject {{u"schema"_s, 1}, {u"plugins"_s, QJsonArray {
        indexItem(u"probe.manifest"_s, u"配置插件"_s, u"1.0.0"_s, false, true),
        indexItem(u"probe.window"_s, u"独立设置插件"_s, u"1.0.0"_s, false, true),
        indexItem(u"probe.disabled"_s, u"已停用插件"_s, u"1.0.0"_s, false, true),
        indexItem(u"probe.update"_s, u"可更新插件"_s, u"1.2.0"_s, false, true),
        indexItem(u"probe.bundled"_s, u"随包插件"_s, u"1.0.0"_s, true, true),
        indexItem(u"probe.uninstalled"_s, u"未安装插件"_s, u"1.0.0"_s, false, true)
    }}}).toJson());
    indexFile.close();
    const std::wstring oldIndex = qEnvironmentVariable("WINSHUN_PLUGIN_INDEX").toStdWString();
    const bool hadIndex = qEnvironmentVariableIsSet("WINSHUN_PLUGIN_INDEX");
    check(::_wputenv_s(L"WINSHUN_PLUGIN_INDEX", indexPath.toStdWString().c_str()) == 0,
        u"configure local gallery index for settings probe"_s);
    const auto restoreIndex = qScopeGuard([&] {
        ::_wputenv_s(L"WINSHUN_PLUGIN_INDEX", hadIndex ? oldIndex.c_str() : L"");
    });

    auto* gallery = editor->plugins()->gallery();
    gallery->refresh();
    QElapsedTimer fetchTime;
    fetchTime.start();
    while (gallery->fetching() && fetchTime.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);

    QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, u"plugins"_s), Q_ARG(QVariant, 1));
    settle(window);

    auto* gallerySection = item(window->contentItem(), u"pluginGallerySection"_s);
    check(gallerySection != nullptr && window->property("currentTab").toInt() == 1,
        u"settings window opens the 插件库 tab"_s);
    if (!gallerySection) return false;

    auto* manifestBtn = item(window->contentItem(), u"gallerySettings-probe.manifest"_s);
    auto* manifestAction = item(window->contentItem(), u"installPlugin-probe.manifest"_s);
    auto* manifestStatus = item(window->contentItem(), u"pluginStatus-probe.manifest"_s);
    check(manifestBtn && manifestBtn->isVisible() && manifestBtn->property("text").toString() == u"设置"
            && manifestAction && !manifestAction->isVisible()
            && manifestStatus && manifestStatus->isVisible() && manifestStatus->property("text").toString().contains(u"已安装"_s),
        u"installed plugin shows 设置 button and 已安装 status in 插件库"_s);

    auto* windowBtn = item(window->contentItem(), u"gallerySettings-probe.window"_s);
    auto* disabledBtn = item(window->contentItem(), u"gallerySettings-probe.disabled"_s);
    auto* disabledStatus = item(window->contentItem(), u"pluginStatus-probe.disabled"_s);
    check(windowBtn && windowBtn->isVisible() && windowBtn->property("text").toString() == u"设置",
        u"settingsWindow plugin shows 设置 button in 插件库"_s);
    check(disabledBtn && disabledBtn->isVisible() && disabledStatus && disabledStatus->property("text").toString().contains(u"已停用"_s),
        u"disabled installed plugin shows 已停用 status and 设置 button"_s);

    auto* updateBtn = item(window->contentItem(), u"gallerySettings-probe.update"_s);
    auto* updateAction = item(window->contentItem(), u"installPlugin-probe.update"_s);
    check(updateBtn && updateBtn->isVisible() && updateBtn->property("text").toString().isEmpty()
            && updateAction && updateAction->isVisible() && updateAction->property("text").toString() == u"更新到 1.2.0",
        u"updatable plugin shows compact gear button alongside 更新到 button"_s);

    auto* bundledBtn = item(window->contentItem(), u"gallerySettings-probe.bundled"_s);
    auto* bundledStatus = item(window->contentItem(), u"pluginStatus-probe.bundled"_s);
    check(bundledBtn && bundledBtn->isVisible() && bundledBtn->property("text").toString() == u"设置"
            && bundledStatus && bundledStatus->property("text").toString().contains(u"自带"_s),
        u"bundled plugin shows 自带 status and 设置 button in 插件库"_s);

    auto* uninstalledBtn = item(window->contentItem(), u"gallerySettings-probe.uninstalled"_s);
    auto* uninstalledAction = item(window->contentItem(), u"installPlugin-probe.uninstalled"_s);
    check(uninstalledBtn && !uninstalledBtn->isVisible()
            && uninstalledAction && uninstalledAction->isVisible() && uninstalledAction->property("text").toString() == u"安装",
        u"uninstalled plugin hides 设置 button and shows 安装"_s);

    // Verify no overlap between the left link icons and the right action buttons on every visible card.
    bool noOverlap = true;
    for (const QString& id : {u"probe.manifest"_s, u"probe.window"_s, u"probe.disabled"_s, u"probe.update"_s, u"probe.bundled"_s}) {
        auto* notes = item(window->contentItem(), u"pluginNotes-"_s + id);
        auto* gear = item(window->contentItem(), u"gallerySettings-"_s + id);
        if (!notes || !gear) {
            noOverlap = false;
            continue;
        }
        const QRectF notesRect = notes->mapRectToScene(QRectF(0, 0, notes->width(), notes->height()));
        const QRectF gearRect = gear->mapRectToScene(QRectF(0, 0, gear->width(), gear->height()));
        if (notesRect.intersects(gearRect))
            noOverlap = false;
    }
    check(noOverlap, u"footer links and settings/update buttons never overlap on gallery cards"_s);
    check(window->grabWindow().save(output.filePath(u"gallery-grid.png"_s)), u"capture 插件库 grid with settings actions"_s);

    // 1. Clicking 设置 on a manifest-settings plugin opens its PluginCard in-place right inside 插件库.
    QMetaObject::invokeMethod(manifestBtn, "clicked");
    settle(window);
    auto* backBtn = item(window->contentItem(), u"galleryPluginBack"_s);
    auto* tokenInput = item(window->contentItem(), u"pluginSetting-probe.manifest/token"_s);
    check(window->property("currentTab").toInt() == 1
            && gallerySection->property("detailOpen").toBool()
            && gallerySection->property("detailPluginId").toString() == u"probe.manifest"
            && backBtn && backBtn->isVisible()
            && tokenInput && tokenInput->isVisible(),
        u"clicking 设置 opens manifest-settings PluginCard in-place on 插件库 tab"_s);
    check(window->grabWindow().save(output.filePath(u"gallery-detail.png"_s)), u"capture in-place PluginCard in 插件库"_s);

    // 2. Titlebar/history Back returns to the 插件库 grid without switching tabs.
    check(window->property("canNavigateBack").toBool()
            && window->property("backTargetLabel").toString().contains(u"插件库"_s),
        u"navigation history points back to 插件库 from opened plugin card"_s);
    QMetaObject::invokeMethod(window, "navigateBack");
    settle(window);
    check(window->property("currentTab").toInt() == 1 && !gallerySection->property("detailOpen").toBool(),
        u"navigateBack closes in-place detail and returns to 插件库 grid"_s);

    // 3. Clicking 设置 on a disabled settingsWindow plugin opens its PluginCard in-place so the user can enable it.
    QMetaObject::invokeMethod(disabledBtn, "clicked");
    settle(window);
    check(window->property("currentTab").toInt() == 1
            && gallerySection->property("detailPluginId").toString() == u"probe.disabled",
        u"disabled settingsWindow plugin opens PluginCard in-place"_s);
    QMetaObject::invokeMethod(backBtn, "clicked");
    settle(window);
    check(!gallerySection->property("detailOpen").toBool(), u"返回插件库 button closes detail card"_s);

    // 4. Clicking 设置 on an enabled settingsWindow plugin calls showSettings directly without leaving the grid.
    editor->plugins()->clearMessage();
    QMetaObject::invokeMethod(windowBtn, "clicked");
    settle(window);
    check(window->property("currentTab").toInt() == 1
            && !gallerySection->property("detailOpen").toBool()
            && !editor->plugins()->message().isEmpty(),
        u"enabled settingsWindow plugin invokes showSettings directly from 插件库"_s);
    editor->plugins()->clearMessage();

    // 4b. Clicking the card body of an enabled settingsWindow plugin opens its PluginCard in-place (for toggle/keyword/uninstall).
    if (QQuickItem* cardBody = windowBtn->parentItem() ? windowBtn->parentItem()->parentItem() : nullptr) {
        if (QQuickItem* entryItem = cardBody->parentItem()) {
            const QPointF pt = entryItem->mapToScene(QPointF(entryItem->width() / 2.0, 28.0));
            QMouseEvent press(QEvent::MouseButtonPress, pt, window->mapToGlobal(pt), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, pt, window->mapToGlobal(pt), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &press);
            QCoreApplication::sendEvent(window, &release);
            settle(window);
        }
    }
    check(window->property("currentTab").toInt() == 1
            && gallerySection->property("detailPluginId").toString() == u"probe.window",
        u"clicking installed settingsWindow card body opens its PluginCard in-place"_s);
    QMetaObject::invokeMethod(backBtn, "clicked");
    settle(window);

    // 5. Clicking the compact gear button on an updatable plugin opens its PluginCard in-place.
    QMetaObject::invokeMethod(updateBtn, "clicked");
    settle(window);
    check(window->property("currentTab").toInt() == 1
            && gallerySection->property("detailPluginId").toString() == u"probe.update",
        u"updatable plugin gear button opens PluginCard in-place"_s);
    QMetaObject::invokeMethod(backBtn, "clicked");
    settle(window);

    // 6. Narrow card (248 px) with 3 links + gear + 更新到 1.2.0 stacks footer cleanly with zero overlap.
    {
        auto* model = gallery->list();
        int updateRow = -1;
        for (int i = 0; i < model->rowCount(); ++i) {
            if (model->data(model->index(i), PluginGalleryModel::IdRole).toString() == u"probe.update") {
                updateRow = i;
                break;
            }
        }
        check(updateRow >= 0, u"find probe.update row in gallery model"_s);
        if (updateRow >= 0) {
            QVariantMap props {{u"gallery"_s, QVariant::fromValue(gallery)}};
            const auto roles = model->roleNames();
            for (auto it = roles.begin(); it != roles.end(); ++it)
                props.insert(QString::fromUtf8(it.value()), model->data(model->index(updateRow), it.key()));
            QQmlComponent comp(qmlEngine(window), u"WinShun"_s, u"PluginGalleryEntry"_s);
            std::unique_ptr<QObject> obj(comp.createWithInitialProperties(props));
            auto* cardItem = qobject_cast<QQuickItem*>(obj.get());
            check(cardItem != nullptr, u"instantiate narrow PluginGalleryEntry"_s);
            if (cardItem) {
                QQuickWindow narrowWin;
                narrowWin.setColor(window->color());
                narrowWin.setWidth(288);
                cardItem->setParentItem(narrowWin.contentItem());
                cardItem->setPosition({20, 20});
                cardItem->setWidth(248);
                narrowWin.setHeight(static_cast<int>(std::ceil(cardItem->implicitHeight())) + 40);
                showOffscreen(&narrowWin);
                auto* notes = item(cardItem, u"pluginNotes-probe.update"_s);
                auto* gear = item(cardItem, u"gallerySettings-probe.update"_s);
                const QRectF notesRect = notes ? notes->mapRectToScene(QRectF(0, 0, notes->width(), notes->height())) : QRectF();
                const QRectF gearRect = gear ? gear->mapRectToScene(QRectF(0, 0, gear->width(), gear->height())) : QRectF();
                check(notes && gear && !notesRect.intersects(gearRect),
                    u"narrow 248px card stacks footer links and actions without overlap"_s);
                check(narrowWin.grabWindow().save(output.filePath(u"gallery-narrow.png"_s)), u"capture narrow stacked gallery card"_s);
                cardItem->setParentItem(nullptr);
            }
        }
    }

    // 6b. 不感兴趣 (GitHub #442): the eye on a tile puts a plugin not installed
    // away, kept in the settings; typing finds it all the same; the count by
    // the filter lists those alone, where the eye brings it back.
    {
        auto* hide = item(window->contentItem(), u"pluginHide-probe.uninstalled"_s);
        auto* hideInstalled = item(window->contentItem(), u"pluginHide-probe.manifest"_s);
        auto* hiddenButton = item(window->contentItem(), u"galleryHiddenButton"_s);
        auto* hiddenChip = item(window->contentItem(), u"galleryHiddenChip"_s);
        auto* hiddenStatus = item(window->contentItem(), u"pluginStatus-probe.uninstalled"_s);
        check(hide && hide->isVisible() && hideInstalled && !hideInstalled->isVisible()
                && hiddenButton && !hiddenButton->isVisible() && hiddenChip && !hiddenChip->isVisible() && gallery->hiddenCount() == 0,
            u"only a plugin not installed offers 不感兴趣; nothing by the filter before one is put away"_s);
        if (hide && hiddenButton && hiddenChip && hiddenStatus) {
            const int all = gallery->shownCount();
            const QRectF hideRect = hide->mapRectToScene(QRectF(0, 0, hide->width(), hide->height()));
            const QRectF installRect = uninstalledAction->mapRectToScene(QRectF(0, 0, uninstalledAction->width(), uninstalledAction->height()));
            check(!hideRect.intersects(installRect), u"不感兴趣 and 安装 do not overlap on the tile"_s);
            QMetaObject::invokeMethod(hide, "clicked");
            settle(window);
            Settings saved;
            saved.load();
            check(saved.pluginGalleryHidden == QStringList {u"probe.uninstalled"_s} && gallery->hiddenCount() == 1
                    && gallery->shownCount() == all - 1 && !uninstalledAction->isVisible()
                    && hiddenButton->isVisible() && hiddenButton->property("text").toString().contains(u"1"_s),
                u"不感兴趣 puts the tile away, saves it and shows the count by the filter"_s);
            check(window->grabWindow().save(output.filePath(u"gallery-hidden-count.png"_s)), u"capture the 不感兴趣 count by the filter"_s);
            gallery->setFilter(u"未安装"_s);
            settle(window);
            check(uninstalledAction->isVisible() && hiddenStatus->property("text").toString().contains(u"不感兴趣"_s),
                u"a plugin put away is still found by typing for it, and says so"_s);
            gallery->setFilter({});
            settle(window);
            QMetaObject::invokeMethod(hiddenButton, "clicked");
            settle(window);
            check(gallery->hiddenOnly() && gallery->shownCount() == 1 && uninstalledAction->isVisible()
                    && hiddenChip->isVisible() && !hiddenButton->isVisible(),
                u"the count lists the plugins put away alone"_s);
            check(window->grabWindow().save(output.filePath(u"gallery-hidden-only.png"_s)), u"capture the 不感兴趣 view"_s);
            QMetaObject::invokeMethod(hide, "clicked");
            settle(window);
            Settings restored;
            restored.load();
            check(restored.pluginGalleryHidden.isEmpty() && gallery->hiddenCount() == 0 && !gallery->hiddenOnly()
                    && gallery->shownCount() == all && !hiddenChip->isVisible() && !hiddenButton->isVisible(),
                u"the eye brings the last one back, and the whole list with it"_s);
            // Installed all the same: never away.
            gallery->setHidden(u"probe.manifest"_s, true);
            settle(window);
            check(gallery->hiddenCount() == 0 && gallery->shownCount() == all, u"an installed plugin is never put away"_s);
            gallery->setHidden(u"probe.manifest"_s, false);
            settle(window);
        }
    }

    // 7. Removing an open plugin closes the in-place detail card automatically.
    QMetaObject::invokeMethod(manifestBtn, "clicked");
    settle(window);
    check(gallerySection->property("detailPluginId").toString() == u"probe.manifest", u"reopen probe.manifest in 插件库"_s);
    QDir(QDir(userDir).filePath(u"probe.manifest"_s)).removeRecursively();
    host.catalog().scan();
    settle(window);
    check(!gallerySection->property("detailOpen").toBool(), u"uninstalling open plugin returns 插件库 to grid"_s);

    check(!window->isActive(), u"gallery settings probe leaves desktop focus untouched"_s);
    QFile report(output.filePath(u"plugin-gallery-settings.json"_s));
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
    AppInfo app {.name = u"Report"_s, .id = u"WinShun.Probe.Report"_s, .target = u"D:\\Report.exe"_s, .elevatable = true};
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
    {
        // 记住以管理员身份运行 (GitHub #313): only with the setting on does
        // a program last run so say 管理员 and offer the way back.
        const QString appPath = launcher.results()->pathAt(0);
        const auto menuHas = [&](Launcher::Action action) {
            const QVariantList items = launcher.menuItems(0);
            return std::ranges::any_of(items, [&](const QVariant& i) { return i.toMap().value(u"action"_s).toInt() == action; });
        };
        const auto adminRow = [&] {
            return launcher.results()->data(launcher.results()->index(0), ResultModel::AdminRunRole).toBool();
        };
        const auto searchAgain = [&] {
            launcher.setQuery(u"repor"_s);
            settle(window);
            launcher.setQuery(u"report"_s);
            settle(window);
        };
        history.record(appPath, u"report"_s, QDateTime::currentMSecsSinceEpoch());
        history.setAdmin(appPath, true);
        searchAgain();
        check(launcher.results()->pathAt(0) == appPath && !adminRow() && menuHas(Launcher::RunAsAdmin) && !menuHas(Launcher::RunAsUser),
            u"remembering administrator runs is off by default"_s);
        launcher.setRememberAdmin(true);
        searchAgain();
        check(adminRow() && menuHas(Launcher::RunAsUser) && !menuHas(Launcher::RunAsAdmin),
            u"a program last run as administrator says so and offers normal rights"_s);
        check(launcher.currentRow() == 0
                && launcher.keyHints().value(u"enter"_s).toString() == QCoreApplication::translate("ws::Launcher", "以管理员身份运行"),
            u"the footer says what Enter does on it"_s);
        check(window->grabWindow().save(output.filePath(u"remembered-admin-enter.png"_s)), u"capture the footer's hint"_s);
        key(field, Qt::Key_Down);
        settle(window);
        check(launcher.currentRow() == 1
                && launcher.keyHints().value(u"enter"_s).toString() == QCoreApplication::translate("ws::Launcher", "打开"),
            u"and on the row after it"_s);
        check(!launcher.results()->data(launcher.results()->index(1), ResultModel::AdminRunRole).toBool(),
            u"other rows are not marked"_s);
        check(window->grabWindow().save(output.filePath(u"remembered-admin.png"_s)), u"capture the administrator mark"_s);
        key(field, Qt::Key_Up);
        settle(window);
        history.setAdmin(appPath, false);
        searchAgain();
        check(!adminRow() && menuHas(Launcher::RunAsAdmin) && !menuHas(Launcher::RunAsUser),
            u"run with normal rights, it is an ordinary row again"_s);
        launcher.setRememberAdmin(false);
        history.remove(appPath);
        searchAgain();
    }
    {
        // A command whose words find files (GitHub #319): "ed report" lists
        // the files and folders "report" finds, Enter hands the one chosen
        // to the command; its own row, the words as they are, comes last.
        // No app is listed: a command cannot be handed one. A command
        // without the choice is as before.
        Favorites favorites(profile.filePath(u"favorites.json"_s));
        Favorite editor {Favorite::Kind::Command, u"winshun-command:probe-editor"_s, u"编辑器"_s, u"ed"_s, u"D:\\Editor.exe {query}"_s};
        editor.searchFiles = true;
        Favorite plain {Favorite::Kind::Command, u"winshun-command:probe-plain"_s, u"回显"_s, u"pl"_s, u"D:\\Echo.exe {query}"_s};
        favorites.add(editor);
        favorites.add(plain);
        launcher.setFavorites(&favorites);
        const auto typed = [&](const QString& text) {
            launcher.setQuery(text);
            QElapsedTimer waiting;
            waiting.start();
            do {
                settle(window);
            } while (launcher.searching() && waiting.elapsed() < 3000);
        };
        const auto enterHint = [&] { return launcher.keyHints().value(u"enter"_s).toString(); };
        typed(u"ed report"_s);
        const int count = launcher.results()->count();
        QStringList paths;
        for (int row = 0; row < count; ++row)
            paths.append(launcher.results()->pathAt(row));
        check(count == 4 && paths.last() == editor.path, u"a command's words list the files found, its own row last: "_s + paths.join(u" | "_s));
        check(std::ranges::none_of(paths, [](const QString& p) { return p.contains(u"Report.exe") || p.startsWith(u"shell:"); }),
            u"no app among a command's files"_s);
        launcher.setCurrentRow(0);
        settle(window);
        check(enterHint() == QCoreApplication::translate("ws::Launcher", "用“%1”打开").arg(editor.name),
            u"Enter on a file found says the command it goes to: "_s + enterHint());
        check(window->grabWindow().save(output.filePath(u"command-files.png"_s)), u"capture a command's files"_s);
        launcher.setCurrentRow(count - 1);
        settle(window);
        check(enterHint() == QCoreApplication::translate("ws::Launcher", "原样传入“%1”").arg(u"report"_s),
            u"Enter on the command's own row says the words go as they are: "_s + enterHint());
        typed(u"ed"_s);
        check(launcher.results()->count() >= 1 && launcher.results()->pathAt(0) == editor.path,
            u"its keyword alone is the command, as before"_s);
        typed(u"pl report"_s);
        launcher.setCurrentRow(0);
        settle(window);
        check(launcher.results()->count() >= 1 && launcher.results()->pathAt(0) == plain.path
                && enterHint() == QCoreApplication::translate("ws::Launcher", "打开"),
            u"a command without the choice takes its words as before: "_s + enterHint());
        launcher.setFavorites(nullptr);
        typed(u"report"_s);
    }
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
    QMetaObject::invokeMethod(window, "showSetting", Q_ARG(QVariant, u"results"_s), Q_ARG(QVariant, 2), // 结果列表 › 操作习惯
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
    // GitHub #433: the Logo's own switch above it; off, there is nothing to right-click.
    auto* logoSwitch = item(window->contentItem(), u"searchLogoSwitch"_s);
    check(editor->searchLogo() && logoSwitch && logoSwitch->isVisible() && logoSwitch->property("checked").toBool(),
        u"the search box shows its Logo by default, with a switch for it"_s);
    if (logoSwitch) {
        QMetaObject::invokeMethod(logoSwitch, "toggled", Q_ARG(bool, false));
        settle(window);
        Settings off;
        off.load();
        check(!off.searchLogo && !editor->searchLogo() && !logoSwitch->property("checked").toBool() && !row->isVisible(),
            u"switching the Logo off is saved and puts the right-click row away"_s);
        check(window->grabWindow().save(output.filePath(u"logo-off-settings.png"_s)), u"capture the Logo switched off"_s);
        QMetaObject::invokeMethod(logoSwitch, "toggled", Q_ARG(bool, true));
        settle(window);
        Settings on;
        on.load();
        check(on.searchLogo && editor->searchLogo() && row->isVisible(), u"switching the Logo back on restores the row"_s);
    }
    QMetaObject::invokeMethod(choices, "activated", Q_ARG(int, 0));
    editor->setLogoRightClick(u"invalid"_s);
    check(editor->logoRightClick() == u"menu", u"settings editor rejects unknown logo actions"_s);
    check(!window->isActive(), u"logo settings probe leaves desktop focus untouched"_s);
    QFile report(output.filePath(u"logo-position-settings.json"_s));
    if (!report.open(QIODevice::WriteOnly)) return false;
    report.write(QJsonDocument(QJsonObject {{u"passed"_s, check.passed}, {u"checks"_s, check.rows}}).toJson());
    return check.passed;
}

// The 捐赠 button's two places (GitHub #402), by mouse events to the window
// on the preview's own profile: the × beside it in the title bar asks first
// (DonateMoveDialog), and Esc or a click beside the card leave it there;
// 确认 moves it, saved; on the 拾穗计划 page the arrow beside it puts it
// back. 捐赠 and the website are not clicked (they open the browser): the
// thanks 捐赠 brings is set directly.
bool checkDonatePlace(QQuickWindow* window, SettingsEditor* editor, const QDir& output)
{
    Checks check;
    const auto wait = [window](int ms) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        settle(window);
    };
    const auto clickAt = [window](QPointF at) {
        const QPointF global = window->mapToGlobal(at);
        QMouseEvent move(QEvent::MouseMove, at, global, Qt::NoButton, Qt::NoButton, {});
        QCoreApplication::sendEvent(window, &move);
        QMouseEvent press(QEvent::MouseButtonPress, at, global, Qt::LeftButton, Qt::LeftButton, {});
        QCoreApplication::sendEvent(window, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, global, Qt::LeftButton, Qt::NoButton, {});
        QCoreApplication::sendEvent(window, &release);
    };
    const auto click = [&](QQuickItem* target) {
        clickAt(target->mapToScene({target->width() / 2, target->height() / 2}));
        wait(250); // past the dialog's fade
    };
    const auto hover = [&](QQuickItem* target) {
        const QPointF at = target->mapToScene({target->width() / 2, target->height() / 2});
        QMouseEvent move(QEvent::MouseMove, at, window->mapToGlobal(at), Qt::NoButton, Qt::NoButton, {});
        QCoreApplication::sendEvent(window, &move);
        wait(700); // past the tip's delay
    };
    const auto saved = [] {
        Settings settings;
        settings.load();
        return settings.donateOnGleaningPage;
    };
    QQuickItem* root = window->contentItem();
    auto* titleButton = item(root, u"titleDonateButton"_s);
    auto* tuck = item(root, u"titleDonateTuck"_s);
    auto* card = item(root, u"donateMoveCard"_s);
    auto* dialog = card ? card->parentItem() : nullptr;
    auto* confirm = item(root, u"donateMoveConfirm"_s);
    auto* word = item(root, u"donateMoveWord"_s);
    auto* site = item(root, u"donateMoveSite"_s);
    check(titleButton && tuck && dialog && confirm && word && site, u"the title bar's button, its × and the dialog are there"_s);
    if (!titleButton || !tuck || !dialog || !confirm || !word || !site)
        return false;
    check(!editor->donateOnGleaningPage() && !saved() && titleButton->isVisible() && tuck->isVisible(),
        u"a new profile has the button and its × in the title bar"_s);
    hover(tuck);
    check(window->grabWindow().save(output.filePath(u"donate-tuck.png"_s)), u"capture the × under the pointer"_s);

    click(tuck);
    check(dialog->property("open").toBool(), u"the × asks first"_s);
    check(site->isVisible(), u"the question offers the website"_s);
    check(window->grabWindow().save(output.filePath(u"donate-dialog.png"_s)), u"capture the question"_s);
    key(card, Qt::Key_Escape);
    wait(250);
    check(!dialog->property("open").toBool() && !editor->donateOnGleaningPage() && titleButton->isVisible(),
        u"Esc leaves the button where it is"_s);

    click(tuck);
    clickAt(QPointF(12, window->height() - 12));
    wait(250);
    check(!dialog->property("open").toBool() && !editor->donateOnGleaningPage(), u"a click beside the card leaves it there too"_s);

    click(tuck);
    const QString before = word->property("text").toString();
    dialog->setProperty("thanked", true);
    settle(window);
    check(word->property("text").toString() != before && dialog->property("open").toBool(),
        u"after 捐赠 the question stays up and thanks"_s);
    check(window->grabWindow().save(output.filePath(u"donate-dialog-thanked.png"_s)), u"capture the thanks"_s);
    click(confirm);
    check(!dialog->property("open").toBool() && editor->donateOnGleaningPage() && saved() && !titleButton->isVisible(),
        u"确认 moves it off the title bar, saved"_s);

    QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, window->property("gleaningPage")), Q_ARG(QVariant, 0));
    wait(3600); // the avatar's greeting (AuthorClock) has played out
    auto* pageButton = item(root, u"pageDonateButton"_s);
    auto* back = item(root, u"pageDonateBack"_s);
    check(pageButton && pageButton->isVisible() && back && back->isVisible(), u"the 拾穗计划 page shows it, with the arrow back"_s);
    check(window->grabWindow().save(output.filePath(u"donate-on-page.png"_s)), u"capture the page"_s);
    if (pageButton && back) {
        hover(back);
        check(window->grabWindow().save(output.filePath(u"donate-page-back.png"_s)), u"capture the arrow under the pointer"_s);
        click(back);
        check(!editor->donateOnGleaningPage() && !saved() && titleButton->isVisible() && !pageButton->isVisible(),
            u"the arrow puts it back in the title bar, saved"_s);
    }
    check(!window->isActive(), u"the probe leaves desktop focus untouched"_s);
    QFile report(output.filePath(u"donate-place.json"_s));
    if (!report.open(QIODevice::WriteOnly))
        return false;
    report.write(QJsonDocument(QJsonObject {{u"passed"_s, check.passed}, {u"checks"_s, check.rows}}).toJson());
    return check.passed;
}

bool checkUpdateFailed(QQuickWindow* window, Updater* updater, const QDir& output)
{
    Checks check;
    const auto wait = [window](int ms) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        settle(window);
    };
    QQuickItem* root = window->contentItem();
    auto* card = item(root, u"updateCard"_s);
    auto* text = item(root, u"updateText"_s);
    auto* skip = item(root, u"updateSkip"_s);
    auto* manual = item(root, u"updateManual"_s);
    auto* later = item(root, u"updateLater"_s);
    auto* primary = item(root, u"updatePrimary"_s);
    check(card && text && skip && manual && later && primary, u"the dialog's words and buttons are there"_s);
    if (!card || !text || !skip || !manual || !later || !primary)
        return false;
    const auto left = [card](QQuickItem* button) { return button->mapToItem(card, QPointF(0, 0)).x(); };
    const auto right = [card](QQuickItem* button) { return button->mapToItem(card, QPointF(button->width(), 0)).x(); };
    const auto top = [card](QQuickItem* button) { return button->mapToItem(card, QPointF(0, 0)).y(); };
    const qreal margin = 24; // UpdateDialog's content.x
    QString problems[2];
    for (const bool blocked : {true, false}) {
        const QString which = blocked ? u"blocked"_s : u"mismatch"_s;
        updater->showFailedForPreview(u"9.9.0"_s, u"这一版改进了更新：安装包被安全软件拦下时说清楚原因，并给出下载页。"_s, blocked);
        QMetaObject::invokeMethod(window, "showUpdateDialog");
        wait(400); // past the dialog's fade
        problems[blocked ? 0 : 1] = text->property("text").toString();
        check(!problems[blocked ? 0 : 1].isEmpty() && problems[blocked ? 0 : 1] == updater->installProblem(),
            u"%1: the dialog says why it was not installed"_s.arg(which));
        check(manual->isVisible() && later->isVisible() && primary->isVisible() && skip->isVisible(),
            u"%1: 打开下载页, 以后再说 and 重试 are offered, 跳过 too"_s.arg(which));
        check(primary->property("text").toString() == QCoreApplication::translate("UpdateDialog", "重试"),
            u"%1: the main button says 重试"_s.arg(which));
        check(left(skip) >= margin - 0.5 && right(primary) <= card->width() - margin + 0.5,
            u"%1: the buttons stay inside the card (%2..%3 of %4)"_s.arg(which).arg(left(skip)).arg(right(primary)).arg(card->width()));
        check(right(skip) + 8 <= left(later) + 0.5,
            u"%1: 跳过这个版本 and 以后再说 keep apart (%2 / %3)"_s.arg(which).arg(right(skip)).arg(left(later)));
        check(qAbs(top(skip) - top(later)) < 0.5 && qAbs(top(later) - top(primary)) < 0.5, u"%1: the buttons in one row"_s.arg(which));
        check(left(manual) >= margin - 0.5 && right(manual) <= card->width() - margin + 0.5 && top(manual) < top(skip) - 8,
            u"%1: 打开下载页 is a link above the buttons, inside the card (%2..%3)"_s.arg(which).arg(left(manual)).arg(right(manual)));
        check(window->grabWindow().save(output.filePath(u"update-failed-%1.png"_s.arg(which))), u"capture %1"_s.arg(which));
    }
    check(problems[0] != problems[1], u"blocked here and a bad package read differently"_s);
    QFile report(output.filePath(u"update-failed.json"_s));
    if (!report.open(QIODevice::WriteOnly))
        return false;
    report.write(QJsonDocument(QJsonObject {{u"passed"_s, check.passed}, {u"checks"_s, check.rows}}).toJson());
    return check.passed;
}

// The row 程序自己的选文件窗口 and what stands behind it (GitHub #272), with
// a WPF window in another process standing in for a program's own file
// window (`fixture`: tests/fixtures/wpf-file-window.ps1): 识别窗口 counts
// down and writes a rule for the window brought to the front, the dialogs'
// following (DialogJump) takes the window from then on, a folder and a file
// go into its box, and a rule taken out of the file lets the window go.
bool checkDialogRuleSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output, const QString& fixture)
{
    Checks check;
    const auto waitUntil = [](const std::function<bool()>& done, int ms) {
        QElapsedTimer clock;
        clock.start();
        while (!done()) {
            if (clock.elapsed() > ms)
                return false;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(10);
        }
        return true;
    };
    QMetaObject::invokeMethod(window, "showSetting", Q_ARG(QVariant, u"integration"_s), Q_ARG(QVariant, 2), // 系统整合 › 对话框
        Q_ARG(QVariant, QCoreApplication::translate("SettingsWindow", "程序自己的选文件窗口")));
    settle(window);
    auto* row = item(window->contentItem(), u"dialogRulesRow"_s);
    auto* learn = item(window->contentItem(), u"dialogRuleLearnButton"_s);
    auto* fileButton = item(window->contentItem(), u"dialogRuleFileButton"_s);
    check(row && row->isVisible() && learn && learn->isVisible() && fileButton && fileButton->isVisible(),
        u"the row and its two buttons are on the dialogs tab"_s);
    if (!row || !learn || !fileButton)
        return false;
    const auto inside = [&](QQuickItem* button) {
        const QPointF at = button->mapToItem(row, QPointF());
        return at.x() >= 0 && at.x() + button->width() <= row->width() + 1 && at.y() >= 0 && at.y() + button->height() <= row->height() + 1;
    };
    check(inside(learn) && inside(fileButton), u"both buttons fit inside the row"_s);

    // The store as App wires it, on a folder of its own.
    const QString folder = output.filePath(u"rules"_s);
    QDir(folder).removeRecursively();
    QDir().mkpath(folder);
    DialogRuleStore store(folder);
    QObject::connect(editor, &SettingsEditor::dialogRuleLearnRequested, &store, [&] { store.learn(); });
    QObject::connect(&store, &DialogRuleStore::status, editor,
        [&](const QString& text, bool done) { editor->setDialogRuleStatus(text, !done); });
    HWND followed = nullptr;
    DialogJump::Callbacks callbacks;
    callbacks.setHotkey = [](bool) { return true; };
    callbacks.dialogChanged = [&](HWND dialog) { followed = dialog; };
    DialogJump jump(std::move(callbacks));
    QObject::connect(&store, &DialogRuleStore::changed, editor, [&] { jump.lookAgain(); });

    const QString starter = store.ensureFile();
    QFile started(starter);
    check(!starter.isEmpty() && started.open(QIODevice::ReadOnly) && dialogrules::parse(started.readAll()).size() == 1,
        u"规则文件 makes a file with one example rule in it"_s);
    started.close();

    QTemporaryDir places;
    const QString went = places.filePath(u"went.txt"_s);
    const QString target = places.filePath(u"工程 数据.gdb"_s);
    QDir().mkpath(target);
    QFile data(QDir(target).filePath(u"roads.shp"_s));
    check(data.open(QIODevice::WriteOnly), u"a folder and a file to go to"_s);
    data.close();
    QProcess program;
    program.start(u"powershell.exe"_s, {u"-NoProfile"_s, u"-STA"_s, u"-ExecutionPolicy"_s, u"Bypass"_s, u"-File"_s,
                                           QDir::toNativeSeparators(fixture), u"-Result"_s, QDir::toNativeSeparators(went)});
    const auto stop = qScopeGuard([&] {
        program.kill();
        program.waitForFinished(3000);
    });
    HWND other = nullptr;
    check(waitUntil([&] { return (other = ::FindWindowW(nullptr, L"WsRuleFixture 添加数据")) != nullptr; }, 20000),
        u"the stand-in window is up"_s);
    if (!other)
        return false;
    const auto toFront = [&] {
        INPUT nothing {};
        nothing.type = INPUT_MOUSE;
        ::SendInput(1, &nothing, sizeof nothing); // as a user's input: Windows then lets the front be handed on
        ::SetForegroundWindow(other);
        return waitUntil([&] { return ::GetForegroundWindow() == other; }, 3000);
    };
    check(toFront(), u"the stand-in window comes to the front"_s);
    waitUntil([] { return false; }, 400);
    check(!followed, u"without a rule the window is nobody's file dialog"_s);

    QMetaObject::invokeMethod(learn, "clicked");
    settle(window);
    check(editor->dialogRuleLearning() && editor->dialogRuleStatus().contains(u'5') && !learn->property("enabled").toBool(),
        u"识别窗口 counts down and cannot be clicked twice: "_s + editor->dialogRuleStatus());
    check(window->grabWindow().save(output.filePath(u"dialog-rule-learning.png"_s)), u"capture the countdown"_s);
    check(toFront(), u"the user switches to the window meanwhile"_s);
    check(waitUntil([&] { return !editor->dialogRuleLearning(); }, 12000), u"the countdown ends"_s);
    check(editor->dialogRuleStatus().contains(u"WsRuleFixture 添加数据"_s) && editor->dialogRuleStatus().contains(u"powershell.exe"_s),
        u"the row says which window it learned: "_s + editor->dialogRuleStatus());
    settle(window);
    check(inside(learn) && inside(fileButton), u"the buttons still fit with the status line shown"_s);
    check(window->grabWindow().save(output.filePath(u"dialog-rule-learned.png"_s)), u"capture the learned state"_s);
    QFile written(store.filePath());
    const std::vector<dialogrules::Rule> rules = written.open(QIODevice::ReadOnly) ? dialogrules::parse(written.readAll()) : std::vector<dialogrules::Rule>();
    written.close();
    check(rules.size() == 2 && rules.back().program == L"powershell.exe" && rules.back().box.id == L"NameBox",
        u"the rule is in the file beside the example, with the box that takes the name"_s);
    check(waitUntil([&] { return followed == other; }, 3000), u"the window is followed at once, still in front"_s);

    const std::wstring folderPath = QDir::toNativeSeparators(target).toStdWString();
    const auto wentTo = [&] {
        QFile file(went);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    };
    jump.go(other, folderPath, false);
    check(waitUntil([&] { return wentTo() == QString::fromStdWString(folderPath + L'\\'); }, 5000),
        u"a folder is typed into its box with a trailing backslash and confirmed"_s);
    const std::wstring filePath = folderPath + L"\\roads.shp";
    jump.go(other, filePath, true, true);
    check(waitUntil([&] { return wentTo() == QString::fromStdWString(filePath); }, 5000),
        u"a file goes in as its whole path and is confirmed when asked"_s);

    // The rule taken out again, in an editor: the window is let go without a restart.
    QFile emptied(store.filePath());
    check(emptied.open(QIODevice::WriteOnly | QIODevice::Truncate) && emptied.write("{\"dialogs\": []}") > 0, u"the file is edited"_s);
    emptied.close();
    check(waitUntil([&] { return followed == nullptr; }, 5000), u"a saved change applies at once: the window is let go"_s);
    dialogrules::setRules({});

    QFile report(output.filePath(u"dialog-rule-settings.json"_s));
    if (!report.open(QIODevice::WriteOnly)) return false;
    report.write(QJsonDocument(QJsonObject {{u"passed"_s, check.passed}, {u"checks"_s, check.rows}}).toJson());
    return check.passed;
}

bool checkSearchExperienceSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output)
{
    Checks check;
    QMetaObject::invokeMethod(window, "showSetting", Q_ARG(QVariant, u"results"_s), Q_ARG(QVariant, 1), // 结果列表 › 显示与预览
        Q_ARG(QVariant, QCoreApplication::translate("SettingsWindow", "合并“全部”和“文件”")));
    settle(window);
    auto* merge = item(window->contentItem(), u"mergeFileScopeToggle"_s);
    check(merge && merge->isVisible(), u"merged-scope preference is visible"_s);
    if (!merge) return false;
    QMetaObject::invokeMethod(merge, "toggled", Q_ARG(bool, true));
    Settings saved;
    saved.load();
    check(saved.mergeFileScope, u"merged-scope setting persists"_s);
    QMetaObject::invokeMethod(window, "showSetting", Q_ARG(QVariant, u"results"_s), Q_ARG(QVariant, 2), // 结果列表 › 操作习惯 (使用记录)
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
    settle(window); // it glides there, as the wheel does
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

namespace {
// A README with every kind of block a plugin's has, long enough that its HTML
// is past the 10 000 characters where Qt's TextEdit drew only what was in view.
QByteArray sampleReadme()
{
    QByteArray text = R"md(# 截图与录屏 / Screenshot and recording

Win顺 的官方可选插件，需要 Win顺 0.18.0 或更新版本。新装的 Win顺 安装包里已带着这个插件，也可以在 设置 → 插件 → 插件库 里安装，确认权限后启用。输入 `cap` 能找到截图的各个命令，托盘菜单的“截图”里也都有。

## 能做什么

- **截图**：拖选一块，或单击窗口、按钮、输入框；框选前按 `Tab` 只框整个窗口。滚轮扩大到整个窗口、整块屏幕。
- **标注**：矩形、椭圆、箭头、直线、画笔、荧光笔、文字、序号、马赛克。
  - 粗细、字号、马赛克格子用滚轮或 `[` `]` 细调。
  - 聚光灯 `G` 可以框几块，其余压暗。
- **贴图**：贴在屏幕上，滚轮缩放、`Ctrl`+滚轮调透明度；点一下贴图再按 `E` 就地编辑。

### 三步截一张图

1. 按 `F1` 开始截图。
2. 拖选一块区域，松开鼠标。
3. 按 `Ctrl+S` 保存，或按 `Esc` 取消。

> 截图和点住贴图时，左下角列出能按的键，`?` 或 `F1` 显示 / 隐藏；设置里可以关。

### 常用按键

| 按键 | 作用 |
| --- | --- |
| `F1` | 截图 |
| `Ctrl+Shift+D` | 延时截图（3、5、10 秒） |
| `F3` | 贴图 |

在搜索框里输入下面的命令：

```
cap 白板
cap 录屏
```

问题和建议请到 [项目主页](https://github.com/LingCore/WinShun) 反馈。

## 最近的版本

)md";
    for (int i = 40; i > 0; --i)
        text += "- **1.1." + QByteArray::number(i) + "**：贴图上直接选字复制；编辑时 `Esc` 先放下工具、取消选择；文字框编辑时也能拖，长截图后屏幕上不再留下一个空框。\n";
    text += R"md(
## 权限

- `ui`：显示截图、编辑、贴图和录屏窗口，注册快捷键。
- `clipboard:read`、`clipboard:write`：读要贴的图片，复制截图和识别的文字。

---

## What it does

- **Screenshots**: drag over an area, or click a window, button or box; `Tab` before selecting outlines whole windows only. The wheel widens the selection to the window and the monitor.
- **Pins**: wheel to zoom, `Ctrl`+wheel for opacity. Click a pin, then `E` edits it in place.
)md";
    return text;
}

// The share of the device pixels in `area` that differ from `ground`: the ink of words.
double inkShare(const QImage& image, const QRect& area, QRgb ground, int threshold = 60)
{
    const QRect inside = area.intersected(image.rect());
    if (inside.isEmpty())
        return 0;
    qint64 ink = 0;
    for (int y = inside.top(); y <= inside.bottom(); ++y)
        for (int x = inside.left(); x <= inside.right(); ++x) {
            const QRgb p = image.pixel(x, y);
            if (std::max({std::abs(qRed(p) - qRed(ground)), std::abs(qGreen(p) - qGreen(ground)), std::abs(qBlue(p) - qBlue(ground))}) > threshold)
                ++ink;
        }
    return double(ink) / (qint64(inside.width()) * inside.height());
}
} // namespace

bool checkPluginReadmeReading(QQuickWindow* window, SettingsEditor* editor, const QDir& output, const QString& readmePath)
{
    Checks check;
    QTemporaryDir plugin;
    const QString pluginDir = plugin.filePath(u"plugins/example.reading"_s);
    QDir().mkpath(pluginDir);
    QFile manifest(QDir(pluginDir).filePath(u"plugin.json"_s));
    if (!manifest.open(QIODevice::WriteOnly)) return false;
    manifest.write(R"({"manifest":1,"id":"example.reading","name":"截图与录屏","version":"1.1.35","commands":[{"id":"go","keyword":"reading","run":{"folder":"C:\\"}}]})");
    manifest.close();
    QByteArray readme = sampleReadme();
    if (!readmePath.isEmpty()) {
        QFile given(readmePath);
        check(given.open(QIODevice::ReadOnly), u"read "_s + readmePath);
        readme = given.readAll();
    }
    QFile file(QDir(pluginDir).filePath(u"README.md"_s));
    if (!file.open(QIODevice::WriteOnly) || file.write(readme) != readme.size()) return false;
    file.close();
    plugin::Host host({.userPluginDir = plugin.filePath(u"plugins"_s), .dataRoot = plugin.filePath(u"data"_s),
        .elevatedHost = false, .elevationRoot = plugin.filePath(u"elevated"_s), .elevationProtected = false});
    host.catalog().scan();
    editor->plugins()->setHost(&host);
    const auto detach = qScopeGuard([&] { editor->plugins()->setHost(nullptr); });
    editor->plugins()->openReadme(u"example.reading"_s);
    settle(window);

    auto* dialog = item(window->contentItem(), u"pluginReadmeDialog"_s);
    auto* text = item(window->contentItem(), u"pluginReadmeText"_s);
    auto* scroll = item(window->contentItem(), u"pluginReadmeScroll"_s);
    auto* bar = item(window->contentItem(), u"pluginReadmeScrollBar"_s);
    auto* thumb = bar ? item(bar, u"scrollBarThumb"_s) : nullptr;
    auto* title = item(window->contentItem(), u"pluginReadmeTitle"_s);
    check(dialog && text && scroll && bar && thumb && title && dialog->isVisible(), u"the 使用说明 opens"_s);
    if (!dialog || !text || !scroll || !bar || !thumb || !title) return false;
    const qreal dpr = dialog->property("dpr").toReal();
    QJsonObject report {{u"dpr"_s, dpr}, {u"html"_s, dialog->property("html").toString().size()}};
    check(title->property("text").toString() == u"截图与录屏", u"headed with the plugin's name"_s);

    // Laid out for reading (ReadingStyle), and drawn whole.
    const int body = text->property("font").value<QFont>().pixelSize();
    auto* quick = text->property("textDocument").value<QQuickTextDocument*>();
    QTextDocument* document = quick ? quick->textDocument() : nullptr;
    check(document != nullptr, u"the text's document"_s);
    if (!document) return false;
    int heading = 0;
    qreal lines = 0;
    QRectF rule; // the first ---, in the text's own coordinates
    QTextBlock quote; // the first > (toHtml keeps its margins, not that it is one)
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        const QTextBlockFormat format = block.blockFormat();
        if (!quote.isValid() && block.text().contains(u"左下角列出"))
            quote = block;
        if (rule.isNull() && format.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)) {
            rule = document->documentLayout()->blockBoundingRect(block);
            report.insert(u"rule"_s, QJsonObject {{u"y"_s, rule.y()}, {u"height"_s, rule.height()},
                {u"colour"_s, format.background().color().name(QColor::HexArgb)},
                {u"width"_s, format.lengthProperty(QTextFormat::BlockTrailingHorizontalRulerWidth).rawValue()}});
        }
        if (heading == 0 && format.headingLevel() == 1)
            heading = block.begin().fragment().charFormat().intProperty(QTextFormat::FontPixelSize);
        if (lines == 0 && format.headingLevel() == 0 && !block.textList() && block.length() > 40
            && format.lineHeightType() == QTextBlockFormat::ProportionalHeight)
            lines = format.lineHeight();
    }
    report.insert(u"body"_s, body);
    report.insert(u"heading"_s, heading);
    report.insert(u"lineHeight"_s, lines);
    check(heading == qRound(body * 1.43), u"the title a step over the body (%1 px over %2)"_s.arg(heading).arg(body));
    check(lines >= 100, u"paragraph lines spaced for reading (%1 %)"_s.arg(lines));
    if (readmePath.isEmpty()) { // the sample has one
        const QColor quiet = quote.isValid() ? quote.begin().fragment().charFormat().foreground().color() : QColor();
        const QTextBlockFormat quoted = quote.blockFormat();
        report.insert(u"quote"_s, QJsonObject {{u"level"_s, quoted.intProperty(QTextFormat::BlockQuoteLevel)},
            {u"left"_s, quoted.leftMargin()}, {u"right"_s, quoted.rightMargin()}, {u"colour"_s, quiet.name(QColor::HexArgb)}});
        check(quote.isValid() && quoted.intProperty(QTextFormat::BlockQuoteLevel) == 1 && quoted.rightMargin() == 0
                  && quiet.isValid() && quiet != text->property("color").value<QColor>(),
              u"the > quote set as one: quieter, to the right edge (left %1, right %2)"_s
                  .arg(quoted.leftMargin()).arg(quoted.rightMargin()));
    }
    check(dialog->property("html").toString().size() > 10000, u"its HTML is past the 10 000 characters where Qt drew in part"_s);
    check(!(text->flags() & QQuickItem::ItemObservesViewport), u"all of it drawn, not only the part in view"_s);

    const QRectF area = scroll->mapRectToScene(QRectF(0, 0, scroll->width(), scroll->height()));
    const QRect textArea(QPoint(qCeil(area.left() * dpr), qCeil(area.top() * dpr)),
                         QPoint(qFloor(area.right() * dpr) - 1, qFloor(area.bottom() * dpr) - 1));
    const auto inked = [&](const QString& name) {
        const QImage image = window->grabWindow();
        image.save(output.filePath(name));
        const QPointF margin = scroll->mapToScene(QPointF(-10, scroll->height() / 2)); // the card's own colour
        return inkShare(image, textArea, image.pixel(qRound(margin.x() * dpr), qRound(margin.y() * dpr)));
    };
    const double top = inked(u"readme-top.png"_s);
    check(top > 0.01, u"words at the top (%1 of the pixels)"_s.arg(top, 0, 'f', 3));

    // Down the scroll bar in big jumps, each one drawn, as a mouse drags it;
    // then part of the way back (it showed an empty page there).
    const QPointF grip = thumb->mapToScene(QPointF(thumb->width() / 2, thumb->height() / 2));
    const qreal travel = bar->height() - 8 - thumb->height();
    const auto mouse = [&](QEvent::Type type, const QPointF& at) {
        QMouseEvent event(type, at, window->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, {});
        QCoreApplication::sendEvent(window, &event);
        window->grabWindow();
    };
    mouse(QEvent::MouseButtonPress, grip);
    for (int i = 1; i <= 6; ++i)
        mouse(QEvent::MouseMove, grip + QPointF(0, travel * 0.8 * i / 6));
    const qreal maxY = scroll->property("contentHeight").toReal() + scroll->property("bottomMargin").toReal() - scroll->height();
    const qreal down = scroll->property("contentY").toReal();
    const double dragged = inked(u"readme-dragged-down.png"_s);
    check(down > maxY * 0.6, u"the scroll bar's drag scrolls it (%1 of %2)"_s.arg(down).arg(maxY));
    check(dragged > 0.01, u"words where the drag took it (%1 of the pixels)"_s.arg(dragged, 0, 'f', 3));
    for (int i = 1; i <= 4; ++i)
        mouse(QEvent::MouseMove, grip + QPointF(0, travel * (0.8 - 0.55 * i / 4)));
    const double back = inked(u"readme-dragged-back.png"_s);
    check(back > 0.01, u"words where it came back to (%1 of the pixels)"_s.arg(back, 0, 'f', 3));
    mouse(QEvent::MouseButtonRelease, grip + QPointF(0, travel * 0.25));

    // A wheel turned on, three notches: every frame of the glide has the text
    // on the same device-pixel phase (whole pixels, a fiftieth lower).
    key(text, Qt::Key_Home);
    settle(window);
    const QPointF middle = scroll->mapToScene(QPointF(scroll->width() / 2, scroll->height() / 2));
    QJsonArray frames;
    QSet<qint64> positions;
    double worst = 0;
    for (int notch = 0; notch < 3; ++notch) {
        QWheelEvent wheel(middle, window->mapToGlobal(middle), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(window, &wheel);
        QElapsedTimer timer;
        timer.start();
        // No grab in between: one takes longer than the glide. Where the
        // text is drawn follows each step of it at once (bindings).
        while (timer.elapsed() < (notch == 2 ? 400 : 70)) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
            const qreal y = text->mapToScene(QPointF()).y() * dpr;
            const qreal contentY = scroll->property("contentY").toReal();
            worst = std::max(worst, std::abs(y - std::floor(y) - 0.02));
            if (!positions.contains(qRound64(contentY * 1000))) {
                positions.insert(qRound64(contentY * 1000));
                frames.append(QJsonObject {{u"contentY"_s, contentY}, {u"deviceY"_s, y}});
            }
        }
    }
    report.insert(u"frames"_s, frames);
    check(positions.size() >= 6, u"the wheel glides (%1 positions)"_s.arg(positions.size()));
    check(worst < 0.005, u"the text on the same device-pixel phase in every frame (off by %1 at worst)"_s.arg(worst, 0, 'f', 4));
    const qreal rest = scroll->property("contentY").toReal() * dpr;
    check(std::abs(rest - std::round(rest)) < 0.01, u"the glide stops on a whole device pixel"_s);
    inked(u"readme-wheel.png"_s);

    key(text, Qt::Key_End);
    settle(window);
    check(std::abs(scroll->property("contentY").toReal() - maxY) < 1, u"End goes to the end"_s);
    inked(u"readme-end.png"_s);
    if (!readmePath.isEmpty() && rule.isNull()) {
        // no --- in the README given
    } else {
        // The ---, brought into view (a jump there): a line across the column, in the rule's colour.
        const qreal there = std::clamp(rule.center().y() - scroll->height() / 2, 0.0, maxY);
        scroll->setProperty("contentY", std::round(there * dpr) / dpr);
        settle(window);
        const QImage image = window->grabWindow();
        image.save(output.filePath(u"readme-rule.png"_s));
        const QPointF centre = text->mapToScene(QPointF(text->width() / 2, rule.center().y()));
        const QPointF margin = scroll->mapToScene(QPointF(-10, scroll->height() / 2));
        const QRgb ground = image.pixel(qRound(margin.x() * dpr), qRound(margin.y() * dpr));
        const double drawn = inkShare(image, QRect(qRound(area.left() * dpr), qRound(centre.y() * dpr) - 2,
                                                   qRound(area.width() * dpr), 5), ground, 6);
        report.insert(u"ruleRowInk"_s, drawn);
        check(!rule.isNull() && drawn > 0.15, u"the --- drawn as a line (%1 of its rows)"_s.arg(drawn, 0, 'f', 3));
    }
    const QString html = dialog->property("html").toString();
    key(text, Qt::Key_Escape);
    check(!dialog->isVisible(), u"Esc closes it"_s);

    // 更新内容 (PluginNotesDialog), set the same way: the same long text, a jump
    // to its middle, words there.
    emit editor->plugins()->notesRequested(QVariantMap {{u"name"_s, u"截图与录屏"_s}, {u"version"_s, u"1.1.35"_s}}, html);
    settle(window);
    auto* notes = item(window->contentItem(), u"pluginNotesDialog"_s);
    auto* notesText = item(window->contentItem(), u"pluginNotesText"_s);
    auto* notesScroll = item(window->contentItem(), u"pluginNotesScroll"_s);
    check(notes && notes->isVisible() && notesText && notesScroll, u"the 更新内容 opens with the same text"_s);
    if (notes && notesText && notesScroll) {
        check(!(notesText->flags() & QQuickItem::ItemObservesViewport), u"the 更新内容 drawn whole too"_s);
        const qreal notesMax = notesScroll->property("contentHeight").toReal() - notesScroll->height();
        notesScroll->setProperty("contentY", std::round(notesMax / 2 * dpr) / dpr);
        settle(window);
        const QImage image = window->grabWindow();
        image.save(output.filePath(u"notes-middle.png"_s));
        const QRectF shown = notesScroll->mapRectToScene(QRectF(0, 0, notesScroll->width(), notesScroll->height()));
        const QRect shownArea = QRect(qCeil(shown.left() * dpr), qCeil(shown.top() * dpr),
                                      qFloor(shown.width() * dpr) - 1, qFloor(shown.height() * dpr) - 1);
        const QPointF plate = notesScroll->mapToScene(QPointF(-6, notesScroll->height() / 2));
        const double ink = inkShare(image, shownArea, image.pixel(qRound(plate.x() * dpr), qRound(plate.y() * dpr)));
        report.insert(u"notesInk"_s, ink);
        check(ink > 0.01, u"words in the middle of the 更新内容 (%1 of the pixels)"_s.arg(ink, 0, 'f', 3));
        key(notesText, Qt::Key_Escape);
        check(!notes->isVisible(), u"Esc closes the 更新内容"_s);
    }

    report.insert(u"passed"_s, check.passed);
    report.insert(u"checks"_s, check.rows);
    QFile json(output.filePath(u"plugin-readme-reading.json"_s));
    if (json.open(QIODevice::WriteOnly))
        json.write(QJsonDocument(report).toJson());
    return check.passed;
}

bool checkNavigationSettings(QQuickWindow* window, SettingsEditor* editor, const QDir& output)
{
    Checks check;
    const auto reveal = [&](const QString& page, const char* title, int tab) {
        QMetaObject::invokeMethod(window, "showSetting", Q_ARG(QVariant, page), Q_ARG(QVariant, tab),
            Q_ARG(QVariant, QCoreApplication::translate("SettingsWindow", title)));
        settle(window);
    };
    reveal(u"keys"_s, "直接打开搜索面板", 0); // 快捷键 › 打开 Win顺
    auto* recorder = item(window->contentItem(), u"panelHotkeyRecorder"_s);
    auto* settingsRecorder = item(window->contentItem(), u"settingsHotkeyRecorder"_s);
    auto* favoritesMode = item(window->contentItem(), u"folderMenuFavoritesInlineMode"_s);
    auto* recentMode = item(window->contentItem(), u"folderMenuRecentInlineMode"_s);
    auto* windowsMode = item(window->contentItem(), u"folderMenuWindowsInlineMode"_s);
    auto* favorites = item(window->contentItem(), u"folderMenuFavoritesInline"_s);
    auto* recent = item(window->contentItem(), u"folderMenuRecentInline"_s);
    auto* windows = item(window->contentItem(), u"folderMenuWindowsInline"_s);
    check(recorder && settingsRecorder && favoritesMode && recentMode && windowsMode && favorites && recent && windows,
        u"new settings use production controls"_s);
    if (!recorder || !settingsRecorder || !favoritesMode || !recentMode || !windowsMode || !favorites || !recent || !windows)
        return false;
    check(editor->panelHotkey().isEmpty(), u"full-panel shortcut defaults to unassigned"_s);
    check(editor->settingsHotkey().isEmpty(), u"settings shortcut defaults to unassigned"_s);
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
    key(settingsRecorder, Qt::Key_X, Qt::AltModifier);
    saved.load();
    check(editor->settingsHotkey() == u"Alt+X" && saved.settingsHotkey == u"Alt+X"
        && saved.hotkey == u"Alt+Space", u"recording a settings chord persists independently"_s);
    key(settingsRecorder, Qt::Key_Backspace);
    saved.load();
    check(editor->settingsHotkey().isEmpty() && saved.settingsHotkey.isEmpty() && saved.hotkey == u"Alt+Space",
        u"clearing the settings chord preserves the launcher shortcut"_s);
    reveal(u"integration"_s, "中键单击托盘图标", 0); // 系统整合 › 任务栏 (托盘图标)
    auto* trayLeft = item(window->contentItem(), u"trayLeftActionPicker"_s);
    auto* trayMiddle = item(window->contentItem(), u"trayMiddleActionPicker"_s);
    check(trayLeft && trayMiddle, u"tray click action pickers render under System integration › Taskbar"_s);
    if (trayLeft && trayMiddle) {
        const auto hasAction = [](const QVariantList& list, const QString& value) {
            return std::ranges::any_of(list, [&](const QVariant& item) {
                return item.toMap().value(u"value"_s).toString() == value;
            });
        };
        check(editor->trayLeftAction() == u"launcher" && editor->trayMiddleAction() == u"settings",
            u"tray clicks default to launcher on left and settings on middle"_s);
        check(!hasAction(editor->trayMiddleActions(), u"screenshots"_s),
            u"screenshot history stays hidden when the screenshot plugin is off"_s);
        settle(window);
        check(window->grabWindow().save(output.filePath(u"tray-click-settings.png"_s)),
            u"capture the tray click settings section"_s);
        editor->setScreenshotPlugin(true, true, {u"F1"_s});
        check(hasAction(editor->trayMiddleActions(), u"screenshots"_s),
            u"enabling the screenshot plugin adds screenshot history to tray click actions"_s);
        editor->setTrayLeftAction(u"panel"_s);
        editor->setTrayMiddleAction(u"screenshots"_s);
        saved.load();
        check(saved.trayLeftAction == u"panel" && saved.trayMiddleAction == u"screenshots",
            u"tray click actions persist independently"_s);
        editor->setScreenshotPlugin(true, false, {});
        settle(window);
        check(hasAction(editor->trayMiddleActions(), u"screenshots"_s)
                && !hasAction(editor->trayLeftActions(), u"screenshots"_s)
                && trayMiddle->property("currentIndex").toInt() >= 0,
            u"disabling the screenshot plugin keeps a fallback entry only on the picker using it"_s);
        const QString pluginAction = u"plugin:winshun.capture/F1"_s;
        editor->setDoubleTapPluginActions({QVariantMap {{u"value"_s, pluginAction}, {u"text"_s, u"截图 · F1"_s}}});
        editor->setTrayMiddleAction(pluginAction);
        saved.load();
        check(saved.trayMiddleAction == pluginAction, u"tray middle click accepts and persists a plugin action"_s);
        editor->setTrayMiddleAction(u"plugin:unknown/Alt+Z"_s);
        check(editor->trayMiddleAction() == pluginAction, u"unregistered plugin actions are rejected for tray clicks"_s);
        editor->setDoubleTapPluginActions({});
        settle(window);
        check(hasAction(editor->trayMiddleActions(), pluginAction)
                && !hasAction(editor->trayLeftActions(), pluginAction)
                && trayMiddle->property("currentIndex").toInt() >= 0,
            u"removing a plugin action keeps a fallback entry only on the picker using it"_s);
        editor->setTrayLeftAction(u"launcher"_s);
        editor->setTrayMiddleAction(u"settings"_s);
    }
    reveal(u"integration"_s, "收藏夹显示方式", 1); // 系统整合 › 文件管理器 (文件夹快捷菜单)
    // All directly (-1), first N (3), first N (20).
    QMetaObject::invokeMethod(favoritesMode, "activated", Q_ARG(int, 2));
    QMetaObject::invokeMethod(recentMode, "activated", Q_ARG(int, 1));
    recent->setProperty("text", u"3"_s);
    QMetaObject::invokeMethod(recent, "editingFinished");
    QMetaObject::invokeMethod(windowsMode, "activated", Q_ARG(int, 1));
    windows->setProperty("text", u"20"_s);
    QMetaObject::invokeMethod(windows, "editingFinished");
    saved.load();
    check(saved.folderMenuFavoritesInline == Settings::kFolderMenuInlineAll && saved.folderMenuRecentInline == 3
            && saved.folderMenuWindowsInline == 20,
        u"favorites all-inline, recent and open-window counts persist independently"_s);
    recent->setProperty("text", u"21"_s);
    QMetaObject::invokeMethod(recent, "editingFinished");
    windows->setProperty("text", QString());
    QMetaObject::invokeMethod(windows, "editingFinished");
    check(recent->property("text").toString() == u"3" && windows->property("text").toString() == u"20",
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
    QMetaObject::invokeMethod(favoritesMode, "activated", Q_ARG(int, 0));
    QMetaObject::invokeMethod(recentMode, "activated", Q_ARG(int, 0));
    saved.load();
    check(saved.folderMenuFavoritesInline == 0 && saved.folderMenuRecentInline == 0 && saved.folderMenuWindowsInline == 20,
        u"submenu restores favorites and recent-folders submenus without changing open windows"_s);
    // GitHub #493: the menu's rows go with its switch, as under every other
    // switch, and a group's own rows with the group; none stays greyed out.
    auto* filesSwitch = item(window->contentItem(), u"folderMenuFilesSwitch"_s);
    check(filesSwitch && filesSwitch->isVisible() && favoritesMode->isVisible() && actions && actions->isVisible(),
        u"the folder menu's rows show while it is on"_s);
    editor->setProperty("folderMenuFavorites", false);
    settle(window);
    check(!favoritesMode->isVisible() && recentMode->isVisible() && windowsMode->isVisible()
            && !item(window->contentItem(), u"folderMenuFavoriteKindChoices"_s)->isVisible(),
        u"a group not listed takes its own rows with it, the other groups' stay"_s);
    editor->setProperty("folderMenuFavorites", true);
    editor->setProperty("folderMenu", false);
    settle(window);
    check(filesSwitch && !filesSwitch->isVisible() && !favoritesMode->isVisible() && !recentMode->isVisible()
            && !windowsMode->isVisible() && actions && !actions->isVisible(),
        u"the folder menu turned off, its rows are gone"_s);
    check(window->grabWindow().save(output.filePath(u"folder-menu-off.png"_s)), u"capture the folder menu turned off"_s);
    editor->setProperty("folderMenu", true);
    settle(window);
    check(filesSwitch && filesSwitch->isVisible() && favoritesMode->isVisible(), u"turned on again, they are back"_s);
    check(!window->isActive(), u"settings checks keep the native window inactive"_s);
    QFile file(output.filePath(u"navigation-settings.json"_s));
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(QJsonObject {{u"passed"_s, check.passed}, {u"checks"_s, check.rows}}).toJson());
    return check.passed;
}
}
