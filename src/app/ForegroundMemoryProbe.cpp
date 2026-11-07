#include "ForegroundMemoryProbe.h"

#include "AppCatalog.h"
#include "Clipboard.h"
#include "History.h"
#include "IndexService.h"
#include "Launcher.h"
#include "Placement.h"
#include "Preview.h"
#include "SearchEngine.h"
#include "SystemCatalog.h"
#include "WindowFrame.h"
#include "platform/ClipboardWatcher.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QRegularExpression>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimer>

#include <memory>
#include <windows.h>
#include <psapi.h>

using namespace Qt::StringLiterals;

namespace ws::preview {
namespace {
void settle(int ms = 350)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

QJsonObject memory(const QString& stage, QObject* root)
{
    PROCESS_MEMORY_COUNTERS_EX2 counters {};
    counters.cb = sizeof(counters);
    const bool known = ::GetProcessMemoryInfo(::GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
    return {{u"stage"_s, stage}, {u"memory_known"_s, known},
        {u"private_bytes"_s, static_cast<double>(counters.PrivateUsage)},
        {u"private_working_set_bytes"_s, static_cast<double>(counters.PrivateWorkingSetSize)},
        {u"working_set_bytes"_s, static_cast<double>(counters.WorkingSetSize)},
        {u"qml_objects"_s, root->findChildren<QObject*>().size()}};
}

QString typeName(const QObject* object)
{
    // An object that declares properties of its own is of a type derived once more ("RowAction_QMLTYPE_54_QML_60").
    static const QRegularExpression suffix(u"(_QML(TYPE)?_\\d+)+$"_s);
    return QString::fromLatin1(object->metaObject()->className()).remove(suffix);
}

QString idOf(QObject* object)
{
    for (QQmlContext* context = qmlContext(object); context; context = context->parentContext()) {
        const QString name = context->nameForObject(object);
        if (!name.isEmpty()) return name;
    }
    return {};
}

// Where the window's objects are: every subtree of 12 or more by size, and
// the classes made most. Walks the visual tree too: a list's rows have no
// QObject parent in the window.
QJsonObject breakdown(QObject* root)
{
    QHash<QObject*, QObject*> above; // each object, reached first from
    QList<QObject*> order;
    QList<QObject*> pending {root};
    above.insert(root, nullptr);
    while (!pending.isEmpty()) {
        QObject* object = pending.takeLast();
        order.append(object);
        QList<QObject*> next = object->children();
        if (auto* item = qobject_cast<QQuickItem*>(object))
            for (QQuickItem* child : item->childItems()) next.append(child);
        if (auto* window = qobject_cast<QQuickWindow*>(object)) next.append(window->contentItem());
        for (QObject* child : next)
            if (child && !above.contains(child)) {
                above.insert(child, object);
                pending.append(child);
            }
    }
    QHash<QObject*, int> sizes;
    for (auto it = order.crbegin(); it != order.crend(); ++it) {
        sizes[*it] += 1;
        if (QObject* parent = above.value(*it)) sizes[parent] += sizes[*it];
    }
    QJsonArray subtrees, classes;
    QHash<QString, int> byClass;
    QList<std::pair<int, QObject*>> sized;
    for (QObject* object : order) {
        ++byClass[typeName(object)];
        if (object != root && sizes[object] >= 12) sized.append({sizes[object], object});
    }
    std::sort(sized.begin(), sized.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [size, object] : sized) {
        QObject* parent = above.value(object);
        subtrees.append(QJsonObject {{u"type"_s, typeName(object)}, {u"id"_s, idOf(object)},
            {u"parent"_s, parent ? typeName(parent) + u" "_s + idOf(parent) : QString()}, {u"objects"_s, size}});
    }
    QList<std::pair<int, QString>> counted;
    for (auto it = byClass.cbegin(); it != byClass.cend(); ++it) counted.append({it.value(), it.key()});
    std::sort(counted.begin(), counted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (int i = 0; i < std::min<qsizetype>(40, counted.size()); ++i)
        classes.append(QJsonObject {{u"class"_s, counted[i].second}, {u"count"_s, counted[i].first}});
    return {{u"reachable"_s, static_cast<int>(order.size())}, {u"subtrees"_s, subtrees}, {u"classes"_s, classes}};
}
}

bool checkForegroundMemory(QQmlEngine* engine, const QDir& output, bool requireLazy, bool preload)
{
    bool passed = true;
    QJsonArray checks, samples, switches, queries;
    const auto check = [&](bool okay, const QString& name) {
        passed &= okay;
        checks.append(QJsonObject {{u"check"_s, name}, {u"passed"_s, okay}});
        note((okay ? u"ok: "_s : u"FAILED: "_s) + name);
    };
    QTemporaryDir profile;
    IndexService index(profile.filePath(u"index"_s), {});
    {
        const auto lock = index.index()->writeLock();
        const auto drive = index.index()->addRoot("D:");
        for (int i = 0; i < 2000; ++i)
            index.index()->add(drive, "memory-report-" + std::to_string(i) + ".txt", 0);
    }
    AppCatalog apps;
    apps.setApps(std::make_shared<const AppList>());
    SystemCatalog places;
    places.setPlaces(std::make_shared<const PlaceList>());
    SearchEngine search(&index, &apps, &places);
    History history(profile.filePath(u"history.txt"_s));
    Launcher launcher(&index, &apps, &places, &search, &history);
    launcher.results()->setThumbnails(false);
    // A button the probe clicks must not reach the user's clipboard or Explorer.
    launcher.setFileCopier([](const QStringList&) {});
    launcher.setFolderOpener([](const QString&) { return true; });
    launcher.setContentOptions(false, {}, {}, true, true);
    ClipStore store(profile.filePath(u"clipboard"_s));
    check(store.open(QDateTime::currentMSecsSinceEpoch()), u"isolated clipboard opens"_s);
    ClipboardWatcher watcher({.record = false}, {});
    Clipboard clipboard(&store, &watcher);
    Placement placement(profile.filePath(u"state.ini"_s), u"Launcher"_s);
    WindowFrame frame(false);
    // What the process holds before Main, and after making it but before its first frame.
    QObject nothing;
    samples.append(memory(u"before_main"_s, &nothing));
    QElapsedTimer creation;
    creation.start();
    QQmlComponent component(engine, u"WinShun"_s, u"Main"_s);
    std::unique_ptr<QObject> root(component.createWithInitialProperties({
        {u"launcher"_s, QVariant::fromValue(&launcher)}, {u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"placement"_s, QVariant::fromValue(&placement)}, {u"frame"_s, QVariant::fromValue(&frame)}}));
    const double creationMs = creation.nsecsElapsed() / 1e6;
    if (root) samples.append(memory(u"created"_s, root.get()));
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    check(window != nullptr, u"production Main loads: "_s + component.errorString());
    if (!window) return false;
    window->setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    window->setProperty("_q_showWithoutActivating", true);
    window->setPosition(24000, 24000);
    window->create();
    frame.setWindow(window);
    placement.setWindow(window);
    placement.rememberWidth(760, 640);
    launcher.setWindow(window);
    window->resize(760, 420);
    window->show();
    settle();
    const auto panel = [&] { return window->property("panelPage").value<QObject*>(); };
    if (requireLazy) check(!panel(), u"compact launcher does not instantiate the full panel"_s);
    samples.append(memory(u"compact_empty"_s, root.get()));
    const QJsonObject compactBreakdown = breakdown(root.get());
    double preloadMs = -1, preloadBlockMs = -1;
    if (preload) { // as the app does at start-up (App::createWindow)
        // The app's engine (QQmlApplicationEngine loading Main) makes objects in
        // the background through its window; this one is given that window's.
        if (!engine->incubationController())
            engine->setIncubationController(window->incubationController());
        // The launcher is hidden then: it must get made all the same.
        window->hide();
        window->setProperty("preloadPanel", true);
        check(!panel(), u"panel preload does not make it all at once"_s);
        // Made between other events: the longest the event loop is held at a
        // time (a 5 ms wait that takes longer was kept waiting by the making).
        QElapsedTimer wait, step;
        wait.start();
        double longest = 0;
        while (!panel() && wait.elapsed() < 3000) {
            step.start();
            settle(5);
            longest = std::max(longest, step.nsecsElapsed() / 1e6 - 5);
        }
        preloadMs = wait.nsecsElapsed() / 1e6;
        preloadBlockMs = longest;
        check(panel() != nullptr, u"panel preload completes in the background while hidden"_s);
        check(longest < 25, u"panel preload holds the event loop under 25 ms at a time (%1 ms)"_s.arg(longest, 0, 'f', 1));
        check(!window->property("panel").toBool(), u"preloading keeps the compact launcher"_s);
        window->show();
        settle();
        samples.append(memory(u"compact_preloaded"_s, root.get()));
    }
    for (const auto& query : {u"memory-report-1"_s, u"memory-report-2"_s, u"memory-report-3"_s, u"memory-report-1"_s}) {
        QElapsedTimer timer;
        timer.start();
        launcher.setQuery(query);
        while (launcher.searching() && timer.elapsed() < 5000) settle(1);
        queries.append(QJsonObject {{u"ms"_s, timer.nsecsElapsed() / 1e6}, {u"rows"_s, launcher.results()->count()}});
        check(!launcher.searching() && launcher.results()->count() > 0, u"real name search completes"_s);
    }
    settle();
    samples.append(memory(u"compact_results"_s, root.get()));
    const QJsonObject resultsBreakdown = breakdown(root.get());
    // Down the rows twice, as with the arrow keys. A row's buttons are made
    // with the row, so the first pass should cost no more than the second;
    // "scrolled" marks the step that moved the list and so made a new row.
    const auto untilFrame = [&] {
        QEventLoop frameLoop;
        const auto connection = QObject::connect(window, &QQuickWindow::frameSwapped, &frameLoop, &QEventLoop::quit, Qt::QueuedConnection);
        QTimer::singleShot(1000, &frameLoop, &QEventLoop::quit);
        window->update();
        frameLoop.exec();
        QObject::disconnect(connection);
    };
    QJsonArray stepping;
    if (auto* list = window->property("view").value<QObject*>(); list && list->property("count").toInt() > 9) {
        for (int pass = 0; pass < 2; ++pass) {
            QJsonArray steps;
            for (int i = 1; i <= 8; ++i) {
                const double contentY = list->property("contentY").toDouble();
                QElapsedTimer timer;
                timer.start();
                list->setProperty("currentIndex", i);
                const double setMs = timer.nsecsElapsed() / 1e6;
                untilFrame();
                steps.append(QJsonObject {{u"set_ms"_s, setMs}, {u"frame_ms"_s, timer.nsecsElapsed() / 1e6},
                    {u"scrolled"_s, list->property("contentY").toDouble() != contentY}});
                settle(30);
            }
            stepping.append(steps);
        }
        list->setProperty("currentIndex", 0);
        settle();
    }
    // The rows borrow their buttons (RowActionsPool): three sets for the
    // list, made with the first results, and never one more.
    int buttonSets = 0;
    for (QObject* object : root->findChildren<QObject*>())
        buttonSets += typeName(object) == u"ResultActions"_s;
    check(buttonSets == 3, u"result rows share three sets of buttons (%1)"_s.arg(buttonSets));
    check(window->grabWindow().save(output.filePath(u"compact.png"_s)), u"compact launcher renders"_s);
    QObject* kept = nullptr;
    QJsonObject panelBreakdown;
    for (int i = 0; i < 4; ++i) {
        QElapsedTimer timer;
        timer.start();
        launcher.setPanel(true);
        window->resize(1100, 650);
        const double dispatchMs = timer.nsecsElapsed() / 1e6;
        QEventLoop frameLoop;
        bool painted = false;
        const auto connection = QObject::connect(window, &QQuickWindow::frameSwapped, &frameLoop, [&] {
            painted = true;
            frameLoop.quit();
        }, Qt::QueuedConnection);
        QTimer::singleShot(2000, &frameLoop, &QEventLoop::quit);
        window->update();
        frameLoop.exec();
        QObject::disconnect(connection);
        switches.append(QJsonObject {{u"dispatch_ms"_s, dispatchMs}, {u"frame_ms"_s, timer.nsecsElapsed() / 1e6}});
        check(painted, u"panel opening produces a frame before timeout"_s);
        settle();
        const auto* bar = window->property("searchBar").value<QObject*>();
        check(bar && bar->property("text").toString() == launcher.query(), u"panel search bar preserves the current query"_s);
        if (requireLazy) {
            check(panel() != nullptr && (!kept || kept == panel()), u"panel is retained for fast repeat openings"_s);
            kept = panel();
        }
        if (i == 0) {
            samples.append(memory(u"panel_first"_s, root.get()));
            panelBreakdown = breakdown(root.get());
            check(window->grabWindow().save(output.filePath(u"panel.png"_s)), u"full panel renders"_s);
        }
        launcher.setPanel(false);
        window->resize(760, 420);
        settle();
        check(!window->property("panel").toBool(), u"return to compact launcher"_s);
    }
    samples.append(memory(u"compact_after_panel"_s, root.get()));
    // A tooltip makes its bubble the first time it shows (after the mouse has
    // rested on its target); time that first showing against a repeat.
    QJsonArray tipShows;
    QObject* tip = nullptr;
    for (QObject* object : root->findChildren<QObject*>())
        if (typeName(object) == u"HoverTip"_s && !object->property("bubble").value<QObject*>()) {
            tip = object;
            break;
        }
    if (tip) {
        for (int i = 0; i < 2; ++i) {
            tip->setProperty("text", u"Probe tip"_s);
            tip->setProperty("target", QVariant::fromValue(window->contentItem()));
            QElapsedTimer timer;
            timer.start();
            QMetaObject::invokeMethod(tip, "latch");
            QMetaObject::invokeMethod(tip, "place");
            tip->setProperty("shown", true);
            const double setMs = timer.nsecsElapsed() / 1e6;
            untilFrame();
            tipShows.append(QJsonObject {{u"set_ms"_s, setMs}, {u"frame_ms"_s, timer.nsecsElapsed() / 1e6}});
            tip->setProperty("target", QVariant::fromValue<QQuickItem*>(nullptr));
            tip->setProperty("shown", false);
            settle(300);
        }
    }
    check(tipShows.size() == 2 && tip->property("bubble").value<QObject*>(), u"a tooltip makes its bubble when it first shows"_s);
    // The rows borrow their buttons: they follow the current row and the one
    // under the mouse, both at once, each with its own row's buttons, and
    // handing them on to rows of every kind makes nothing new.
    QJsonArray lending;
    if (auto* list = window->property("view").value<QQuickItem*>()) {
        QList<SearchResult> kinds;
        const auto add = [&](const QString& name, const QString& path, auto&& setUp) {
            SearchResult result;
            result.name = name;
            result.path = path;
            setUp(result);
            kinds.append(result);
        };
        add(u"report.docx"_s, u"D:/work/report.docx"_s, [](SearchResult&) {});
        add(u"work"_s, u"D:/work"_s, [](SearchResult& r) { r.isDir = true; });
        add(u"notes.txt"_s, u"D:/work/notes.txt"_s, [](SearchResult&) {});
        add(u"Demo"_s, u"C:/ProgramData/Microsoft/Windows/Start Menu/Demo.lnk"_s, [](SearchResult& r) {
            r.app = AppKind::Desktop;
            r.elevatable = true;
            r.target = u"C:/Program Files/Demo/demo.exe"_s;
        });
        add(u"Notes - Editor"_s, u"window:1234"_s, [](SearchResult& r) { r.app = AppKind::Window; });
        add(u"显示"_s, u"ms-settings:display"_s, [](SearchResult& r) { r.app = AppKind::Setting; });
        add(u"Example"_s, u"https://example.com"_s, [](SearchResult& r) { r.app = AppKind::Web; });
        launcher.results()->assign(kinds, {});
        settle();
        const auto find = [](QQuickItem* from, const QString& name) {
            QList<QQuickItem*> pending {from};
            while (!pending.isEmpty()) {
                QQuickItem* item = pending.takeLast();
                if (item->objectName() == name) return item;
                pending.append(item->childItems());
            }
            return static_cast<QQuickItem*>(nullptr);
        };
        const auto rowAt = [&](int i) { return find(window->contentItem(), u"resultRow-%1"_s.arg(i)); };
        const auto rowNamed = [&](const QString& name) {
            for (int i = 0; i < kinds.size(); ++i)
                if (QQuickItem* row = rowAt(i); row && row->property("name").toString() == name) return row;
            return static_cast<QQuickItem*>(nullptr);
        };
        const auto buttonsOf = [](QQuickItem* row) {
            QList<QQuickItem*> shown, pending;
            if (row) pending.append(row);
            while (!pending.isEmpty()) {
                QQuickItem* item = pending.takeFirst();
                if (typeName(item) == u"RowAction"_s && item->isVisible()) shown.append(item);
                pending.append(item->childItems());
            }
            std::sort(shown.begin(), shown.end(), [](QQuickItem* a, QQuickItem* b) { return a->x() < b->x(); });
            return shown;
        };
        const auto describe = [&](QQuickItem* row) {
            QJsonArray tips;
            for (QQuickItem* button : buttonsOf(row))
                tips.append(button->property("tip").toString() + button->property("text").toString());
            return tips;
        };
        QList<int> reachable;
        for (int pass = 0; pass < 2; ++pass) {
            QJsonArray rows;
            for (int i = 0; i < kinds.size(); ++i) {
                list->setProperty("currentIndex", i);
                settle(30);
                QQuickItem* row = rowAt(i);
                rows.append(QJsonObject {{u"row"_s, row ? row->property("name").toString() : QString()}, {u"buttons"_s, describe(row)}});
            }
            lending.append(rows);
            reachable.append(breakdown(root.get()).value(u"reachable"_s).toInt());
        }
        check(reachable[0] == reachable[1], u"handing the buttons on to rows of every kind makes nothing (%1, %2 objects)"_s
            .arg(reachable[0]).arg(reachable[1]));
        check(lending[0] == lending[1], u"each row shows its own buttons whichever set it is lent"_s);
        const auto move = [&](QPointF at) {
            QMouseEvent event(QEvent::MouseMove, at, at, window->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &event);
            settle(60);
        };
        QQuickItem* current = rowNamed(u"report.docx"_s);
        QQuickItem* hovered = rowNamed(u"notes.txt"_s);
        list->setProperty("currentIndex", current ? current->property("index").toInt() : 0);
        settle(60);
        if (hovered) move(hovered->mapToScene(QPointF(hovered->width() / 3, hovered->height() / 2)));
        check(hovered && buttonsOf(current).size() == 5 && buttonsOf(hovered).size() == 5,
            u"the current row and the one under the mouse show their buttons at once"_s);
        check(window->grabWindow().save(output.filePath(u"buttons-hover.png"_s)), u"hovered buttons render"_s);
        // Only the delete button, and only once: the first click arms it, the second would recycle.
        QQuickItem* remove = hovered ? find(hovered, u"deleteAction"_s) : nullptr;
        check(remove && remove->isVisible(), u"the hovered row's delete button is found"_s);
        if (remove && remove->isVisible() && !hovered->property("deleteArmed").toBool()) {
            const QPointF at = remove->mapToScene(QPointF(remove->width() / 2, remove->height() / 2));
            move(at);
            for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                QMouseEvent event(type, at, at, window->mapToGlobal(at), Qt::LeftButton,
                    type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(window, &event);
            }
            settle(60);
            check(hovered->property("deleteArmed").toBool() && remove->property("text").toString() == u"确认删除"_s,
                u"a lent delete button arms its own row"_s);
            check(window->grabWindow().save(output.filePath(u"buttons-armed.png"_s)), u"armed delete renders"_s);
        }
        move(QPointF(window->width() / 2.0, 4));
        check(hovered && !hovered->property("deleteArmed").toBool() && buttonsOf(hovered).isEmpty() && buttonsOf(current).size() == 5,
            u"the row the mouse left gives its buttons back, disarmed"_s);
    }
    check(!window->isActive(), u"probe never takes desktop focus"_s);
    if (requireLazy) {
        // The full panel may already be selected before Main is constructed.
        window->hide();
        launcher.setPanel(true);
        std::unique_ptr<QObject> initiallyExpanded(component.createWithInitialProperties({
            {u"launcher"_s, QVariant::fromValue(&launcher)}, {u"clipboard"_s, QVariant::fromValue(&clipboard)},
            {u"placement"_s, QVariant::fromValue(&placement)}, {u"frame"_s, QVariant::fromValue(&frame)}}));
        const auto* initialPanel = initiallyExpanded ? initiallyExpanded->property("panelPage").value<QObject*>() : nullptr;
        const auto* initialBar = initiallyExpanded ? initiallyExpanded->property("searchBar").value<QObject*>() : nullptr;
        check(initialPanel && initialBar && initialBar->property("text").toString() == launcher.query(),
            u"initially expanded launcher creates its panel with the current query"_s);
    }
    QFile report(output.filePath(u"foreground-memory.json"_s));
    if (!report.open(QIODevice::WriteOnly)) return false;
    report.write(QJsonDocument(QJsonObject {{u"passed"_s, passed}, {u"checks"_s, checks},
        {u"samples"_s, samples}, {u"creation_ms"_s, creationMs},
        {u"preload_ms"_s, preloadMs}, {u"preload_longest_block_ms"_s, preloadBlockMs}, {u"queries"_s, queries}, {u"panel_openings"_s, switches},
        {u"compact_breakdown"_s, compactBreakdown},
        {u"results_breakdown"_s, resultsBreakdown}, {u"row_stepping"_s, stepping}, {u"tip_showing"_s, tipShows}, {u"lending"_s, lending},{u"panel_breakdown"_s, panelBreakdown}}).toJson());
    return passed;
}
}
