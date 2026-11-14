#include "PreviewInteractionProbe.h"
#include "AppCatalog.h"
#include "Clipboard.h"
#include "ContentPane.h"
#include "FilePreview.h"
#include "History.h"
#include "IndexService.h"
#include "Launcher.h"
#include "Placement.h"
#include "Preview.h"
#include "SearchEngine.h"
#include "SystemCatalog.h"
#include "SystemTheme.h"
#include "WindowFrame.h"
#include "platform/ClipboardWatcher.h"

#include <QDateTime>
#include <QBuffer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <memory>
#include <windows.h>

using namespace Qt::StringLiterals;
namespace ws::preview {
namespace {
void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
QQuickItem* named(QQuickItem* root, QStringView name)
{
    if (root->objectName() == name) return root;
    for (auto* child : root->childItems())
        if (auto* found = named(child, name)) return found;
    return nullptr;
}
QQuickWindow* hoverWindow()
{
    for (auto* window : QGuiApplication::allWindows())
        if (window->objectName() == u"clipboardHoverPreview" && window->isVisible())
            return qobject_cast<QQuickWindow*>(window);
    return nullptr;
}
}

bool checkPreviewInteractions(QQmlEngine* engine, const QDir& output)
{
    // The fixture may place popups at screen edges; never run it on the
    // user's desktop or acquire the global clipboard/keyboard.
    if (QGuiApplication::platformName() != u"offscreen") {
        wchar_t desktop[256] {};
        DWORD bytes = 0;
        if (!::GetUserObjectInformationW(::GetThreadDesktop(::GetCurrentThreadId()), UOI_NAME, desktop, sizeof desktop, &bytes)
            || !QString::fromWCharArray(desktop).startsWith(u"WinShun-preview-")) return false;
    }
    bool passed = true;
    QJsonArray checks;
    const auto check = [&](bool okay, const QString& name) {
        passed &= okay;
        checks.append(QJsonObject {{u"check"_s, name}, {u"passed"_s, okay}});
        note((okay ? u"ok: "_s : u"FAILED: "_s) + name);
    };
    const bool native = QCoreApplication::arguments().contains(u"--native-ui-probe"_s);
    check(!native || QGuiApplication::platformName() == u"windows", u"requested native probe uses the Windows platform"_s);
    QTemporaryDir profile;
    ClipStore store(profile.filePath(u"clipboard"_s));
    check(store.open(QDateTime::currentMSecsSinceEpoch()), u"isolated clipboard database"_s);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QImage picture(600, 400, QImage::Format_RGB32);
    picture.fill(QColor(u"#2783b8"_s));
    QByteArray png;
    QBuffer encoded(&png);
    encoded.open(QIODevice::WriteOnly);
    picture.save(&encoded, "PNG");
    store.add(ClipCapture {.kind = ClipKind::Image, .imageData = png, .hash = QByteArrayLiteral("hover-fixture"),
        .width = picture.width(), .height = picture.height()}, now - 1);
    const qint64 longId = store.add(ClipCapture {.kind = ClipKind::Text,
        .text = QStringLiteral("一段完整的长文本 / Long clipboard content.\n").repeated(80)}, now);
    const qint64 shortId = store.add(ClipCapture {.kind = ClipKind::Text, .text = u"短文本 / Short note"_s}, now + 1);
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
    check(window, u"production clipboard QML loads: "_s + component.errorString());
    if (!window) return false;
    window->setPosition(100, 80);
    window->show();
    clipboard.handleShown();
    clipboard.handlePlaced();
    wait(250);
    window->grabWindow();
    auto* page = named(window->contentItem(), u"clipboardPage");
    auto* list = named(window->contentItem(), u"clipboardEntries");
    if (!page || !list) return false;
    const QRect original = window->geometry();
    const int selected = list->property("currentIndex").toInt();
    const auto hover = [&](int row, QPointF point) {
        check(QMetaObject::invokeMethod(page, "hoverRow", Q_ARG(int, row), Q_ARG(QPointF, point)), u"dispatch row hover"_s);
    };
    check(!hoverWindow(), u"opening clipboard starts without a preview"_s);
    hover(0, {130, 170});
    wait(80);
    check(!hoverWindow(), u"a quick pass does not open a preview"_s);
    hover(0, {132, 171});
    wait(160);
    auto* popup = hoverWindow();
    check(popup, u"200ms dwell opens without requiring a click or a perfectly still hand"_s);
    if (!popup) return false;
    wait(50);
    popup->grabWindow();
    const int shortHeight = popup->height();
    check(window->geometry() == original && list->property("currentIndex").toInt() == selected,
        u"hover leaves the list geometry and keyboard selection unchanged"_s);
    check(popup->flags().testFlag(Qt::WindowDoesNotAcceptFocus), u"preview does not take keyboard focus"_s);
    if (native)
        check((popup->color().lightness() < 128) == QCoreApplication::arguments().contains(u"--dark"_s),
            u"rendered preview uses the requested light/dark palette"_s);
    check(popup->property("info").toMap().value(u"text"_s).toString() == u"短文本 / Short note", u"short entry content"_s);
    check(popup->grabWindow().save(output.filePath(u"clipboard-short.png"_s)), u"short preview screenshot"_s);
    hover(1, {132, 220});
    wait(240);
    popup = hoverWindow();
    check(popup && popup->height() > shortHeight && popup->height() <= 480, u"long content grows within the height limit"_s);
    if (!popup) return false;
    check(popup->property("info").toMap().value(u"text"_s).toString().startsWith(u"一段完整"), u"moving rows shows the correct full text"_s);
    check(popup->grabWindow().save(output.filePath(u"clipboard-long.png"_s)), u"long preview screenshot"_s);
    auto* content = named(popup->contentItem(), u"clipboardPreviewContent");
    check(content, u"preview scroll area exists"_s);
    if (content) {
        const QPointF at = content->mapToScene({content->width() / 2, content->height() / 2});
        QWheelEvent wheel(at, popup->mapToGlobal(at), {}, {0, -120}, {}, {}, Qt::ScrollUpdate, false);
        QCoreApplication::sendEvent(popup, &wheel);
        wait(220);
        check(content->property("contentY").toReal() > 0, u"wheel scroll reads long content in the transient preview"_s);
        check(window->geometry() == original, u"scrolling preview does not move or resize the clipboard list"_s);
    }
    hover(2, {132, 270});
    wait(300);
    popup = hoverWindow();
    check(popup && popup->property("info").toMap().value(u"kind"_s).toInt() == 4, u"hover follows a real image entry"_s);
    if (!popup) return false;
    const int imageHeight = popup->height();
    wait(100);
    check(imageHeight > shortHeight && imageHeight < 480 && popup->height() == imageHeight,
        u"image height fits its content without a sizing loop"_s);
    check(popup->grabWindow().save(output.filePath(u"clipboard-image.png"_s)), u"image preview screenshot"_s);
    // Work area with a top taskbar and negative monitor coordinates.
    const QRect area(-900, -100, 900, 1200);
    popup->setProperty("availableArea", area);
    popup->setProperty("anchorRect", QRect(-480, 1020, 480, 50));
    wait(50);
    check(area.adjusted(8, 8, -8, -8).contains(popup->geometry()), u"popup stays inside a negative-coordinate work area at its bottom edge"_s);
    QMetaObject::invokeMethod(page, "clearHover");
    wait(20);
    check(!hoverWindow(), u"leaving/scrolling clears the transient preview"_s);
    // Exercise the actual MouseArea route as well as timed row transitions.
    auto* row = list->property("currentItem").value<QQuickItem*>();
    if (row) {
        const QPointF at = row->mapToScene({row->width() / 2, row->height() / 2});
        QMouseEvent move(QEvent::MouseMove, at, window->mapToGlobal(at), Qt::NoButton, {}, {});
        QCoreApplication::sendEvent(window, &move);
        wait(250);
        check(hoverWindow(), u"production row MouseArea forwards a real Qt pointer event"_s);
        QMouseEvent leaveRow(QEvent::MouseMove, QPointF(20, 10), window->mapToGlobal(QPointF(20, 10)), Qt::NoButton, {}, {});
        QCoreApplication::sendEvent(window, &leaveRow);
        wait(180);
        check(!hoverWindow(), u"moving off a row dismisses its transient preview"_s);
    } else check(false, u"current clipboard row exists"_s);
    QMetaObject::invokeMethod(page, "togglePreview", Q_ARG(QVariant, 0));
    wait(70);
    check(page->property("previewOpen").toBool() && !hoverWindow(), u"manual preview remains available and dismisses the transient one"_s);
    window->hide();
    clipboard.handleHidden();
    window->show();
    clipboard.handleShown();
    clipboard.handlePlaced();
    wait(70);
    check(!page->property("previewOpen").toBool() && !hoverWindow(), u"reopening does not restore either preview"_s);
    // A pinned row's handle, dragged onto the next row, moves it there in its
    // group's own order. The row's buttons are lent (RowActionsPool): they
    // stay on it while the handle is held, though the mouse leaves the row.
    {
        const qint64 pinned[] {longId, shortId};
        store.setGroup(pinned, ClipStore::kPinned);
        clipboard.setPinnedFirst(true);
        wait(150);
        QQuickItem* first = named(window->contentItem(), u"clipboardRow-0");
        QQuickItem* second = named(window->contentItem(), u"clipboardRow-1");
        const int movedId = first ? first->property("clipId").toInt() : -1;
        const auto send = [&](QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons buttons) {
            QMouseEvent event(type, at, window->mapToGlobal(at), button, buttons, {});
            QCoreApplication::sendEvent(window, &event);
        };
        const auto handleOf = [](QQuickItem* row) {
            QList<QQuickItem*> pending {row};
            while (!pending.isEmpty()) {
                QQuickItem* item = pending.takeFirst();
                if (item->isVisible() && item->property("draggable").toBool()) return item;
                pending.append(item->childItems());
            }
            return static_cast<QQuickItem*>(nullptr);
        };
        if (first && second)
            send(QEvent::MouseMove, first->mapToScene({first->width() / 2, first->height() / 2}), Qt::NoButton, Qt::NoButton);
        wait(60);
        QQuickItem* handle = first ? handleOf(first) : nullptr;
        check(handle && first->property("movable").toBool(), u"a pinned row under the mouse shows its handle"_s);
        if (handle && second) {
            const QPointF from = handle->mapToScene({handle->width() / 2, handle->height() / 2});
            const qreal to = second->mapToScene({0, second->height() - 4}).y();
            send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
            bool kept = true;
            for (int step = 1; step <= 8; ++step) {
                send(QEvent::MouseMove, {from.x(), from.y() + (to - from.y()) * step / 8}, Qt::NoButton, Qt::LeftButton);
                wait(30);
                kept = kept && handle->isVisible() && list->property("rowDragging").toBool();
            }
            QQuickItem* moved = named(window->contentItem(), u"clipboardRow-1");
            check(moved && moved->property("clipId").toInt() == movedId, u"dragging the handle moves the row to the next place"_s);
            check(kept, u"the dragged row keeps its buttons while the handle is held"_s);
            send(QEvent::MouseButtonRelease, {from.x(), to}, Qt::LeftButton, Qt::NoButton);
            wait(80);
            check(!list->property("rowDragging").toBool(), u"letting the handle go ends the drag"_s);
            check(window->grabWindow().save(output.filePath(u"clipboard-moved.png"_s)), u"moved row screenshot"_s);
        }
    }
    window->hide();

    IndexService index(profile.filePath(u"index"_s), {});
    AppCatalog apps;
    apps.setApps(std::make_shared<const AppList>());
    SystemCatalog places;
    places.setPlaces(std::make_shared<const PlaceList>());
    SearchEngine search(&index, &apps, &places);
    History history(profile.filePath(u"history.txt"_s));
    Launcher launcher(&index, &apps, &places, &search, &history);
    FilePreview filePreview;
    filePreview.setBuiltInOnly(true);
    launcher.setPreview(&filePreview);
    launcher.setScope(Launcher::Files);
    filePreview.show({.path = u"winshun-plugin:fixture"_s, .name = u"Fixture"_s,
        .text = std::make_shared<const QString>(u"Preview content"_s)});
    check(filePreview.isOpen(), u"production file preview opens"_s);
    launcher.setScope(Launcher::All);
    check(!filePreview.isOpen(), u"switching Files to All closes the old floating preview"_s);
    wait(100);
    check(!filePreview.isOpen(), u"queued search updates do not reopen the old preview"_s);
    launcher.setPanel(true);
    launcher.setContentOptions(true, {}, {}, true, true);
    filePreview.show({.path = u"winshun-plugin:fixture"_s, .name = u"Fixture"_s,
        .text = std::make_shared<const QString>(u"Preview content"_s)});
    launcher.setScope(Launcher::Content);
    check(!filePreview.isOpen() && launcher.contentPane()->tab() == ContentPane::PreviewTab,
        u"entering content details still hands the floating preview to its embedded tab"_s);
    QFile report(output.filePath(u"preview-interactions.json"_s));
    if (!report.open(QIODevice::WriteOnly)) return false;
    report.write(QJsonDocument(QJsonObject {{u"passed"_s, passed}, {u"platform"_s, QGuiApplication::platformName()},
        {u"checks"_s, checks}}).toJson());
    return passed;
}
}
