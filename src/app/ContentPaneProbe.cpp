#include "ContentPaneProbe.h"
#include "AppCatalog.h"
#include "Clipboard.h"
#include "ContentPane.h"
#include "ContentIndexer.h"
#include "FilePreview.h"
#include "History.h"
#include "IndexService.h"
#include "Launcher.h"
#include "Placement.h"
#include "SystemCatalog.h"
#include "WindowFrame.h"
#include "platform/ClipboardWatcher.h"
#include "platform/ContentOpen.h"
#include "platform/PreviewHost.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <miniz.h>

using namespace Qt::StringLiterals;
namespace ws::preview {
namespace {
void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
template<typename Fn> bool until(Fn ready, int timeout = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < timeout) wait(10);
    return ready();
}
QQuickItem* named(QQuickItem* root, const QString& name)
{
    if (!root) return nullptr;
    if (root->objectName() == name) return root;
    for (auto* item : root->childItems()) if (auto* found = named(item, name)) return found;
    return nullptr;
}
QByteArray spreadsheet()
{
    mz_zip_archive zip {};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) return {};
    const auto put = [&](const char* path, const QByteArray& bytes) {
        return mz_zip_writer_add_mem(&zip, path, bytes.constData(), static_cast<size_t>(bytes.size()), MZ_BEST_SPEED);
    };
    const bool ok = put("[Content_Types].xml", "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/><Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/></Types>")
        && put("_rels/.rels", "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>")
        && put("xl/workbook.xml", u"<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets><sheet name=\"北区\" sheetId=\"1\" r:id=\"rId1\"/><sheet name=\"South\" sheetId=\"2\" r:id=\"rId2\"/></sheets></workbook>"_s.toUtf8())
        && put("xl/_rels/workbook.xml.rels", "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/><Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet2.xml\"/></Relationships>")
        && put("xl/worksheets/sheet1.xml", u"<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData><row r=\"7\"><c r=\"A7\" t=\"inlineStr\"><is><t>needle 华北销售</t></is></c><c r=\"D7\" t=\"inlineStr\"><is><t>region</t></is></c></row><row r=\"53\"><c r=\"C53\" t=\"inlineStr\"><is><t>预算说明</t></is></c></row><row r=\"90\"><c r=\"B90\" t=\"inlineStr\"><is><t>needle</t></is></c></row></sheetData></worksheet>"_s.toUtf8())
        && put("xl/worksheets/sheet2.xml", "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData><row r=\"5\"><c r=\"A5\" t=\"inlineStr\"><is><t>needle South</t></is></c></row></sheetData></worksheet>");
    void* bytes = nullptr;
    size_t size = 0;
    const bool finished = ok && mz_zip_writer_finalize_heap_archive(&zip, &bytes, &size);
    QByteArray result;
    if (finished) result = QByteArray(static_cast<const char*>(bytes), static_cast<qsizetype>(size));
    mz_free(bytes);
    mz_zip_writer_end(&zip);
    return result;
}
}

bool checkContentPane(QQmlEngine* qml, const QDir& output)
{
    QTemporaryDir temp;
    IndexService index(temp.filePath(u"index"_s), IndexService::Options{});
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    History history(temp.filePath(u"history.tsv"_s));
    SearchEngine search(&index, &apps, &places);
    Launcher launcher(&index, &apps, &places, &search, &history);
    launcher.setContentOptions(true, {u"txt"_s}, ContentSizeLimits{}, false, true);
    qml->addImageProvider(u"panepreview"_s, launcher.contentPane()->filePreview()->imageProvider());
    // The launcher's own preview, beside the box (no window here: made by the app).
    FilePreview side;
    side.setBuiltInOnly(true); // never QuickLook or Seer, should one run here
    launcher.setPreview(&side);
    ClipStore store(temp.filePath(u"clipboard"_s));
    if (!store.open(QDateTime::currentMSecsSinceEpoch())) return false;
    ClipboardWatcher watcher({}, {});
    Clipboard clipboard(&store, &watcher);
    Placement placement(temp.filePath(u"placement.ini"_s), u"Launcher"_s);
    WindowFrame frame(false);
    QQmlComponent component(qml, u"WinShun"_s, u"Main"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {u"launcher"_s, QVariant::fromValue(&launcher)}, {u"clipboard"_s, QVariant::fromValue(&clipboard)},
        {u"frame"_s, QVariant::fromValue(&frame)}, {u"placement"_s, QVariant::fromValue(&placement)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { qWarning().noquote() << component.errorString(); return false; }
    window->create();
    launcher.setWindow(window);
    frame.setWindow(window);
    placement.setWindow(window);
    window->setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    window->setProperty("_q_showWithoutActivating", true);
    const int offscreen = static_cast<int>(24000 / window->devicePixelRatio());
    window->setPosition(offscreen, offscreen);
    window->show();
    launcher.setPanel(true);
    window->resize(1300, 820);
    window->contentItem()->setSize(QSizeF(window->size()));
    auto* pane = launcher.contentPane();
    pane->loadPreferences(temp.filePath(u"pane.ini"_s));
    const auto files = index.index();
    const EntryId root = files->addRoot(QDir::toNativeSeparators(temp.path()).toUtf8().toStdString());
    const auto write = [&](const QString& name, const QByteArray& bytes) {
        QFile file(temp.filePath(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return QString();
        file.close();
        const auto lock = files->writeLock();
        files->add(root, name.toUtf8().toStdString(), 0);
        return QDir::toNativeSeparators(file.fileName());
    };
    const QString text = write(u"季度经营分析.txt"_s,
        u"2026 年第三季度经营分析\nneedle 华东 region：收入增长 18%\n对照：NEEDLE regional\nneedle needle 重点项目\n后续安排与说明\n"_s.toUtf8());
    const QString sheet = write(u"区域销售.xlsx"_s, spreadsheet());
    const QString other = write(u"另一个文件.txt"_s, "needle other\n");
    bool passed = true;
    QJsonObject results;
    const auto check = [&](const QString& key, bool value) {
        results.insert(key, value);
        passed = passed && value;
        if (!value) qWarning().noquote() << "content pane failed:" << key;
    };
    const auto snap = [&](const QString& name) {
        window->contentItem()->setSize(QSizeF(window->size()));
        window->requestUpdate();
        wait(120);
        QImage image = window->grabWindow();
        return !image.isNull() && image.save(output.filePath(name + u".png"_s));
    };
    const auto choose = [&](const QString& path) {
        auto* list = named(window->contentItem(), u"panelResultList"_s);
        if (!list) return false;
        list->setProperty("currentIndex", launcher.results()->indexOf(path));
        return until([&] { return !pane->busy() && pane->path() == path && pane->hits()->rowCount() > 0; });
    };
    launcher.setScope(Launcher::Content);
    launcher.setQuery(u"needle"_s);
    check(u"real_search"_s, until([&] { return !launcher.searching() && launcher.results()->count() == 3; }));
    check(u"selected_text_load"_s, choose(text));
    const auto termCount = [&] { return pane->terms().isEmpty() ? qint64(-1) : pane->terms()[0].toMap().value(u"count"_s).toLongLong(); };
    check(u"all_occurrences"_s, termCount() == 4);
    auto* paneItem = named(window->contentItem(), u"contentPaneView"_s);
    check(u"pane_geometry"_s, paneItem && paneItem->isVisible() && paneItem->width() >= 320);
    check(u"hits_screenshot"_s, snap(u"hits"_s));
    pane->setFilter(u"region"_s, false, true, false);
    check(u"whole_word_refinement"_s, until([&] { return !pane->busy() && pane->hits()->rowCount() == 3; }));
    check(u"refinement_chip_count"_s, pane->terms().size() == 2 && pane->terms()[1].toMap().value(u"count"_s).toInt() == 1);
    pane->toggleTerm(1);
    check(u"refinement_chip_toggle"_s, until([&] { return !pane->busy() && pane->hits()->rowCount() == 5; }));
    pane->setFilter(u"["_s, false, false, true);
    check(u"invalid_regex_feedback"_s, until([&] { return !pane->busy() && pane->hits()->rowCount() == 0 && !pane->message().isEmpty(); }));
    pane->setFilter({}, false, false, false);
    check(u"refinement_restored"_s, until([&] { return !pane->busy() && termCount() == 4; }));
    int positions = 0;
    QObject::connect(pane, &ContentPane::positionRequested, pane, [&](int, int) { ++positions; });
    pane->nextMatch(1);
    check(u"next_match"_s, positions > 0);
    const auto key = [&](int key, Qt::KeyboardModifiers modifiers) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers);
        QCoreApplication::sendEvent(window, &event);
    };
    key(Qt::Key_3, Qt::ControlModifier | Qt::ShiftModifier);
    check(u"tab_shortcut"_s, until([&] { return pane->tab() == 2; }));
    check(u"spreadsheet_load"_s, choose(sheet));
    check(u"real_sheets"_s, pane->sheets().size() == 2 && termCount() == 3);
    pane->setSheet(0);
    check(u"sheet_filter"_s, until([&] { return !pane->busy() && pane->textLines()->rowCount() == 3; }));
    const auto first = pane->textLines()->index(0, 0);
    check(u"real_row_number"_s, pane->textLines()->data(first, ContentLines::Location).toString().contains(u"7"));
    check(u"separated_cells"_s, pane->textLines()->data(first, ContentLines::Value).toString().contains(u'\t'));
    check(u"text_screenshot"_s, snap(u"text-sheets"_s));
    auto* resultList = named(window->contentItem(), u"panelResultList"_s);
    for (int i = 0; i < 25; ++i) if (resultList) resultList->setProperty("currentIndex", launcher.results()->indexOf(i % 2 ? text : sheet));
    if (resultList) resultList->setProperty("currentIndex", launcher.results()->indexOf(other));
    check(u"rapid_selection_latest_wins"_s, until([&] { return !pane->busy() && pane->path() == other && termCount() == 1; }));
    QFile changed(other);
    check(u"modify_fixture"_s, changed.open(QIODevice::WriteOnly));
    changed.write("needle needle changed\n"); changed.close();
    pane->refresh();
    check(u"version_invalidation"_s, until([&] { return !pane->busy() && termCount() == 2; }));
    pane->setOpened(false);
    check(u"closed_has_no_detail_work"_s, !pane->busy());
    pane->setPreferredWidth(480);
    ContentPane restored(&index);
    restored.loadPreferences(temp.filePath(u"pane.ini"_s));
    check(u"preferences_persist"_s, !restored.opened() && restored.preferredWidth() == 480 && restored.tab() == 2);
    pane->setOpened(true);
    window->resize(640, 520);
    check(u"narrow_geometry"_s, until([&] {
        auto* detail = named(window->contentItem(), u"contentPaneView"_s);
        return detail && detail->width() == window->width();
    }));
    check(u"narrow_screenshot"_s, snap(u"narrow"_s));
    window->resize(1300, 820);
    pane->setTab(0);
    check(u"summary_screenshot"_s, snap(u"summary"_s));
    pane->setTab(4);
    check(u"report_choices_loaded"_s, until([&] { return named(window->contentItem(), u"contentReportScope"_s) != nullptr; }));
    for (const QString& name : {u"contentReportScope"_s, u"contentReportFormat"_s}) {
        auto* choice = named(window->contentItem(), name);
        check(name + u"_themed"_s, choice && choice->property("flat").toBool());
        if (!choice) continue;
        auto* content = choice->property("contentItem").value<QQuickItem*>();
        auto* inner = content ? content->property("background").value<QQuickItem*>() : nullptr;
        check(name + u"_single_surface"_s, !inner || !inner->isVisible());
        auto* popup = choice->property("popup").value<QObject*>();
        check(name + u"_opens"_s, popup && QMetaObject::invokeMethod(popup, "open"));
        wait(120);
        check(name + u"_screenshot"_s, snap(name));
        if (popup) QMetaObject::invokeMethod(popup, "close");
    }
    pane->prepareReport(false, false, true, true, true);
    check(u"current_report"_s, until([&] { return !pane->reporting() && pane->reportReady(); }));
    auto report = pane->reportSnapshot();
    check(u"report_has_every_match"_s, report && report->tables.size() >= 2 && report->tables[1].rows.size() == 9);
    if (report) {
        for (int i = 0; i < 4; ++i) {
            const QStringList extensions {u"txt"_s, u"csv"_s, u"html"_s, u"xlsx"_s};
            QFile file(output.filePath(u"report."_s + extensions[i]));
            const QByteArray bytes = contentreport::render(*report, static_cast<contentreport::Format>(i));
            check(u"export_%1"_s.arg(i), file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && bytes.size() > 50);
        }
    }
    check(u"report_screenshot"_s, snap(u"report"_s));
    pane->setReportOptions(1, true, false, false);
    check(u"options_invalidate_snapshot"_s, !pane->reportReady());
    launcher.results()->setSelection({sheet});
    pane->prepareReport(true, false, true, true, false);
    check(u"selected_report_ready"_s, until([&] { return !pane->reporting() && pane->reportReady(); }));
    report = pane->reportSnapshot();
    check(u"selected_report_scope"_s, report && report->tables.front().rows.size() == 1
        && report->tables.front().rows.front()[1] == sheet);
    launcher.clearSelection();
    for (int i = 0; i < 305; ++i) write(u"more-%1.txt"_s.arg(i), "needle more\n");
    pane->prepareReport(false, true, true, true, false);
    check(u"full_report_beyond_page_limit"_s, until([&] { return !pane->reporting() && pane->reportReady(); }, 30000));
    report = pane->reportSnapshot();
    check(u"full_report_file_count"_s, report && !report->tables.empty() && report->tables[0].rows.size() == 308);
    write(u"bad.xlsx"_s, QByteArray("PK\x03\x04", 4) + QByteArray(64, '\0'));
    write(u"over-limit.txt"_s, QByteArray((16 << 20) + 1, 'x'));
    pane->prepareReport(false, true, true, true, false);
    check(u"skipped_report_ready"_s, until([&] { return !pane->reporting() && pane->reportReady(); }, 30000));
    report = pane->reportSnapshot();
    bool badListed = false, largeListed = false;
    if (report) for (const auto& row : report->tables.front().rows) {
        badListed = badListed || row[1].endsWith(u"bad.xlsx");
        largeListed = largeListed || row[1].endsWith(u"over-limit.txt");
    }
    check(u"skips_and_failures_in_report"_s, badListed && largeListed);
    results.insert(u"bad_document_listed"_s, badListed);
    results.insert(u"oversize_listed"_s, largeListed);
    pane->prepareReport(false, true, true, true, true);
    pane->cancelReport();
    wait(100);
    check(u"cancel_does_not_publish_partial_report"_s, !pane->reporting() && !pane->reportReady());
    pane->setTab(3);
    if (resultList) resultList->setProperty("currentIndex", launcher.results()->indexOf(sheet));
    FilePreview* tabPreview = pane->filePreview();
    // A document: through its handler, or (none here, or it failed) a thumbnail and what is known.
    const bool previewSettled = until([&] {
        return tabPreview->path() == sheet
            && ((tabPreview->kind() == FilePreview::Handler && pane->previewRect().has_value()) || tabPreview->kind() == FilePreview::Facts);
    }, 15000);
    check(u"real_handler_or_explicit_fallback"_s, previewSettled);
    results.insert(u"real_xlsx_preview_visible"_s, pane->previewRect().has_value());
    if (const auto bounds = pane->previewRect()) {
        // The page's width, less the area's margins (8 each side, PreviewBody.qml).
        check(u"preview_viewport_width"_s, std::abs(bounds->width() - qRound((pane->preferredWidth() - 16) * window->devicePixelRatio())) < 4);
        pane->setTab(2);
        check(u"preview_hidden_on_tab_change"_s, until([&] { return !pane->previewRect(); }));
        pane->setTab(3);
    }
    check(u"preview_tab_screenshot"_s, snap(u"preview"_s));
    results.insert(u"xlsx_handler_installed"_s, PreviewHost::handlerFor(sheet).has_value());

    // GitHub #224: the 预览 tab shows what the launcher's own preview does: a
    // Markdown file (no handler for it here, as on most PCs) as its text.
    launcher.setContentOptions(true, {u"txt"_s, u"md"_s}, ContentSizeLimits{}, false, true);
    const QString markdown = write(u"说明.md"_s, QStringList {u"# 标题"_s, {}, u"mdneedle 第一段"_s, {}, u"- 一"_s, u"- 二"_s, {}}
        .join(QChar::LineFeed).toUtf8());
    launcher.setQuery(u"mdneedle"_s);
    check(u"markdown_found"_s, until([&] { return !launcher.searching() && launcher.results()->indexOf(markdown) >= 0; }));
    if (resultList) resultList->setProperty("currentIndex", launcher.results()->indexOf(markdown));
    check(u"markdown_selected"_s, until([&] { return pane->path() == markdown && !pane->busy(); })); // 预览: no matches read
    check(u"markdown_preview_as_text"_s, until([&] {
        return tabPreview->path() == markdown && tabPreview->kind() == FilePreview::Text && tabPreview->lineCount() == 6;
    }));
    check(u"markdown_preview_summary"_s, tabPreview->summary().contains(u"UTF-8"_s));
    auto* previewBody = named(window->contentItem(), u"contentPreviewBody"_s);
    auto* previewLines = named(window->contentItem(), u"previewTextLines"_s);
    check(u"markdown_preview_drawn"_s, previewBody && previewBody->isVisible() && previewLines && previewLines->isVisible()
        && previewLines->property("count").toInt() == 6 && previewLines->width() > 200);
    check(u"markdown_preview_screenshot"_s, snap(u"preview-markdown"_s));
    // The current row moves: the tab follows, no window of its own anywhere.
    if (resultList) resultList->setProperty("currentIndex", launcher.results()->indexOf(markdown));
    check(u"tab_preview_has_no_window"_s, tabPreview->window() == nullptr);

    // GitHub #224: the preview window beside the box gives way to the 预览
    // tab when the panel's details come, and comes back when they go.
    const int mdRow = launcher.results()->indexOf(markdown);
    pane->setTab(1);
    launcher.setPanel(false);
    launcher.togglePreview(mdRow);
    check(u"box_preview_opens"_s, side.isOpen() && pane->tab() == 1);
    launcher.setPanel(true);
    check(u"panel_takes_over_preview"_s, !side.isOpen() && pane->tab() == ContentPane::PreviewTab);
    check(u"taken_over_preview_in_tab"_s, until([&] { return tabPreview->path() == markdown && tabPreview->kind() == FilePreview::Text; }));
    launcher.togglePreview(mdRow); // Alt+P in the details: back to the tab before
    check(u"alt_p_in_details_toggles_tab"_s, !side.isOpen() && pane->tab() == 1);
    launcher.setPanel(false);
    check(u"preview_put_away_in_details_not_back"_s, !side.isOpen());
    launcher.togglePreview(mdRow);
    launcher.setPanel(true);
    check(u"panel_takes_over_again"_s, !side.isOpen() && pane->tab() == ContentPane::PreviewTab);
    launcher.togglePreview(mdRow);
    launcher.togglePreview(mdRow);
    check(u"alt_p_in_details_shows_tab"_s, !side.isOpen() && pane->tab() == ContentPane::PreviewTab);
    launcher.setPanel(false);
    check(u"box_gets_preview_back"_s, side.isOpen() && side.path() == markdown);
    launcher.togglePreview(mdRow); // closed by the user in the box: not back in the panel either
    launcher.setPanel(true);
    check(u"closed_preview_stays_closed"_s, !side.isOpen() && pane->tab() == ContentPane::PreviewTab);
    launcher.setPanel(false);
    check(u"closed_preview_not_back"_s, !side.isOpen());
    launcher.setPanel(true);
    check(u"line_aware_code_args"_s, contentopen::arguments(u"C:\\Code\\Code.exe"_s, text, 42)
        == std::optional(u"--goto \"%1:42\""_s.arg(text)));
    check(u"line_aware_notepadpp_args"_s, contentopen::arguments(u"notepad++.exe"_s, text, 42)
        == std::optional(u"-n42 \"%1\""_s.arg(text)));
    const auto args = [&](const QString& program) { return contentopen::arguments(program, text, 42); };
    check(u"line_aware_vscode_family"_s, args(u"C:\\VS Code\\Code - Insiders.exe"_s) == std::optional(u"--goto \"%1:42\""_s.arg(text))
        && args(u"VSCodium.exe"_s) == args(u"Code.exe"_s) && args(u"Cursor.exe"_s) == args(u"Code.exe"_s) && args(u"Windsurf.exe"_s) == args(u"Code.exe"_s));
    check(u"line_aware_sublime_zed"_s, args(u"sublime_text.exe"_s) == std::optional(u"\"%1:42\""_s.arg(text))
        && args(u"subl.exe"_s) == args(u"sublime_text.exe"_s) && args(u"Zed.exe"_s) == args(u"subl.exe"_s));
    check(u"line_aware_jetbrains"_s, args(u"idea64.exe"_s) == std::optional(u"--line 42 \"%1\""_s.arg(text))
        && args(u"PyCharm64.exe"_s) == args(u"idea64.exe"_s) && args(u"rider64.exe"_s) == args(u"idea64.exe"_s));
    check(u"line_aware_notepad_family"_s, args(u"Notepad4.exe"_s) == std::optional(u"/g 42 \"%1\""_s.arg(text))
        && args(u"Notepad3.exe"_s) == args(u"Notepad4.exe"_s) && args(u"notepad2.exe"_s) == args(u"Notepad4.exe"_s));
    check(u"line_aware_emeditor_gvim"_s, args(u"EmEditor.exe"_s) == std::optional(u"/l 42 \"%1\""_s.arg(text))
        && args(u"gvim.exe"_s) == std::optional(u"+42 \"%1\""_s.arg(text)));
    check(u"line_unaware_editors_keep_plain_open"_s, !args(u"notepad.exe"_s) && !contentopen::arguments(u"Code.exe"_s, text, 0)
        && !contentopen::arguments(u"Code.exe"_s, u"C:\\a\"b.txt"_s, 42));
    check(u"clipboard_capture_off"_s, !watcher.running());
    {
        // GitHub #202: a plugin's row that is no file, its details asked for,
        // then given as text, then not to be had.
        ContentPane details(&index);
        details.loadPreferences(temp.filePath(u"details-pane.ini"_s));
        details.setOpened(true);
        details.setTab(1);
        details.setActive(true);
        SearchEngine::Request request;
        request.text = u"needle"_s;
        ContentPane::Subject subject {u"winshun-plugin:test.mail/main/m1"_s, {}, u"发票邮件"_s, {}, false,
            ContentPane::tr("正在向插件获取详情…"), true};
        const auto show = [&] { details.update(request, u"needle"_s, u"needle"_s, {}, {}, subject, 0, 0, true, false, {}, 2); };
        show();
        check(u"plugin_details_waiting"_s, details.busy() && details.title() == u"发票邮件"_s
            && details.matchStatus() == ContentPane::tr("正在向插件获取详情…"));
        check(u"plugin_rows_in_summary"_s, details.summary().contains(ContentPane::tr("插件结果（不是文件，不计入报告）：%1").arg(2)));
        subject.message.clear();
        subject.waiting = false;
        subject.text = std::make_shared<const QString>(u"From: a\r\nSubject: needle\r\n\r\nneedle and NEEDLE\r\n"_s);
        show();
        const auto count = [&] { return details.terms().isEmpty() ? -1 : details.terms()[0].toMap().value(u"count"_s).toInt(); };
        check(u"plugin_details_text"_s, until([&] { return !details.busy() && count() == 3; })
            && details.textLines()->rowCount() >= 3 && details.path().isEmpty() && details.title() == u"发票邮件"_s);
        subject.text.reset();
        subject.message = u"无法获取详情：插件没有回答"_s;
        show();
        check(u"plugin_details_failed"_s, !details.busy() && details.message() == subject.message && details.hits()->rowCount() == 0);
        details.setActive(false);
    }
    pane->setActive(false);
    window->hide();
    QFile log(output.filePath(u"content-pane.json"_s));
    if (log.open(QIODevice::WriteOnly)) log.write(QJsonDocument(results).toJson(QJsonDocument::Indented));
    return passed;
}
}
