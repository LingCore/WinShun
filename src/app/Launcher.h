#pragma once

#include "AppCatalog.h"
#include "ContentIndex.h"
#include "ResultModel.h"
#include "SearchTypes.h"
#include "WebShortcut.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QVariantList>
#include <QTimer>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace ws {

class History;
class IndexService;
class SearchEngine;
class SystemCatalog;

// The view-model behind the search window: query and scope in, results and
// status out, plus the actions on a result (open, reveal, copy, ...).
class Launcher : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged FINAL)
    Q_PROPERTY(Scope scope READ scope WRITE setScope NOTIFY scopeChanged FINAL)
    Q_PROPERTY(ws::ResultModel* results READ results CONSTANT FINAL)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool searching READ searching NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool stale READ stale NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString placeholder READ placeholder NOTIFY scopeChanged FINAL)
    Q_PROPERTY(QString contentFilesLabel READ contentFilesLabel NOTIFY scopeChanged FINAL)
    Q_PROPERTY(bool searchesContent READ searchesContent NOTIFY statusChanged FINAL)
    // History is kept and has something in it (the footer's clear button).
    Q_PROPERTY(bool canClearHistory READ canClearHistory NOTIFY historyChanged FINAL)
    // The apps opened most, for the launcher over the taskbar with nothing
    // typed: for each {name, icon}. Read again each time it is shown; none
    // while history is not kept.
    Q_PROPERTY(QVariantList frequentApps READ frequentApps NOTIFY frequentAppsChanged FINAL)
    // 文件 and 文件夹: newest first instead of best match first (the footer's switch).
    Q_PROPERTY(bool rankByTime READ rankByTime WRITE setRankByTime NOTIFY rankByTimeChanged FINAL)
    // Rows say when each file or folder was last written.
    Q_PROPERTY(bool showModified READ showModified NOTIFY resultOptionsChanged FINAL)
    // The modifier that opens the nth row shown with a digit: "ctrl", "alt",
    // or "" for none (Settings::numberKeys).
    Q_PROPERTY(QString numberKeys READ numberKeys NOTIFY resultOptionsChanged FINAL)
    // The search box has an input method's composition: its keys are the
    // input method's (NumberKeys takes no digits meanwhile).
    Q_PROPERTY(bool composing READ composing WRITE setComposing NOTIFY composingChanged FINAL)
    // Opened over Win顺's search box on the taskbar (taskbar::SearchBox):
    // what is typed shows there, its own field is out of sight (SearchBar.qml
    // reports the field, the app draws it in the box).
    Q_PROPERTY(bool inTaskbarBox READ inTaskbarBox NOTIFY inTaskbarBoxChanged FINAL)

public:
    // As ws::Scope. The tabs show 文件夹 within 文件: Tab goes 全部, 文件,
    // 文件夹, 内容 (cycleScope).
    enum Scope { All, Files, Content, Folders };
    Q_ENUM(Scope)
    enum Action { Open, Reveal, RunAsAdmin, CopyPath, CopyName, CopyItem, Recycle, ForgetRecent, EditWebShortcuts };
    Q_ENUM(Action)

    Launcher(IndexService* index, AppCatalog* apps, SystemCatalog* places, SearchEngine* engine, History* history,
        QObject* parent = nullptr);

    QString query() const { return m_query; }
    void setQuery(const QString& query);
    Scope scope() const { return m_scope; }
    void setScope(Scope scope);
    ResultModel* results() { return &m_results; }
    QString statusText() const { return m_flash.isEmpty() ? m_status : m_flash; }
    bool busy() const;
    // More results for the query are on the way. Not while quietly
    // refreshing results that were already complete.
    bool searching() const
    {
        return !m_quiet && (m_pending || m_contentRunning || m_contentDebounce.isActive());
    }
    // The rows on screen are for the previous query; the new one has found
    // nothing yet (内容 takes a while).
    bool stale() const { return m_replacePending && m_results.count() > 0; }
    // A name search for the current query is still running (window reveal
    // waits for it, see App::showLauncher).
    bool waitingForNames() const { return m_pending && !m_contentRunning; }
    QString placeholder() const;
    QString contentFilesLabel() const; // ".txt / .md / .log 等文件"
    // Whether the current query also looks inside files: always in 内容,
    // and in 全部 unless the query is name syntax or starts with a web
    // shortcut's keyword (see contentNeedle()).
    bool searchesContent() const { return m_scope == Content || m_withContent; }

    void setWindow(QWindow* window) { m_window = window; }
    void setContentOptions(
        QStringList extensions, const ContentSizeLimits& sizeLimits, bool inLowPriority, bool documents);
    void setWebShortcuts(const WebShortcuts& shortcuts);
    void setResultOptions(KindOrder kindOrder, bool showModified, const QString& numberKeys);
    bool rankByTime() const { return m_rankByTime; }
    void setRankByTime(bool byTime);
    bool showModified() const { return m_showModified; }
    QString numberKeys() const { return m_numberKeys; }
    bool composing() const { return m_composing; }
    void setComposing(bool composing);
    bool inTaskbarBox() const { return m_inTaskbarBox; }
    void setInTaskbarBox(bool on);
    // The search field changed (text, caret, selection, composition): fieldReported.
    Q_INVOKABLE void reportField(
        const QString& text, int cursor, int selectionStart, int selectionEnd, const QString& composition);
    // Ctrl+n (or Alt+n): the nth row shown opens (numberPressed), once the
    // rows on screen are the query's (holdUntilShown).
    Q_INVOKABLE void pressNumber(int number);
    // Enter or a number pressed while the rows on screen are still the
    // previous query's (it was typed a moment ago): true, and heldKey()
    // brings it back once the new rows are shown. Typing on drops it.
    Q_INVOKABLE bool holdUntilShown(int action, int number);
    // Off: nothing opened is remembered, and what was is not shown (it stays
    // until cleared in the settings).
    void setRecordHistory(bool on);
    bool canClearHistory() const;
    Q_INVOKABLE void clearHistory();
    QVariantList frequentApps() const;
    Q_INVOKABLE void openFrequentApp(int index);
    void handleShown();
    void handleHidden();
    void retranslate(); // the language changed: status, placeholder, rows

    // An action on a row; on a row of a selection of several, on all of it.
    Q_INVOKABLE void trigger(int row, int action);
    Q_INVOKABLE void triggerSelection(int action); // the keyboard's, with rows selected
    // Selecting several rows: Ctrl toggles one, Shift takes the rows from an
    // anchor to here (instead of the selection, or with Ctrl added to it).
    Q_INVOKABLE void toggleSelected(int row);
    Q_INVOKABLE void selectRange(int from, int to, bool add);
    Q_INVOKABLE void clearSelection();
    // Context menu (ContextMenu.qml): its entries for a row, the usable
    // screen area around a point, and the native styling of its window.
    Q_INVOKABLE QVariantList menuItems(int row) const;
    Q_INVOKABLE QRectF screenArea(QPointF globalPos) const;
    Q_INVOKABLE void prepareMenuWindow(QWindow* menu) const;
    Q_INVOKABLE void cycleScope(int delta);
    Q_INVOKABLE void dismiss();

signals:
    void queryChanged();
    void scopeChanged();
    void statusChanged();
    void shown();
    void resultsReplaced(); // results for a new query: select the first row
    void namesShown(); // a name search finished and its rows are in the model
    void dismissRequested();
    // Not started: it would have needed administrator rights to start at all
    // (see shell::open). The launcher is closed by then.
    void openFailed(const QString& name);
    void contextMenuKeyPressed(); // Menu key / Shift+F10 (arrive as a context-menu event, not a key)
    void historyChanged();
    void frequentAppsChanged();
    void webSettingsRequested(); // the settings window, on its 网页搜索 page
    void rankByTimeChanged();
    void resultOptionsChanged();
    void composingChanged();
    void inTaskbarBoxChanged();
    void fieldReported(const QString& text, int cursor, int selectionStart, int selectionEnd, const QString& composition);
    // Pressed in the box on the taskbar: the field's caret goes there, or
    // its word is selected; dragged there, the text from `anchor` is selected.
    void caretRequested(int position, bool word);
    void selectionRequested(int anchor, int position);
    void numberPressed(int number); // 1…9: open that row, counted from the first one shown
    void heldKey(int action, int number); // see holdUntilShown(); `number` 0: the action on the current row

private:
    void search();
    QString contentNeedle() const;
    static bool looksLikeCode(QStringView text);
    QString shownExtensions() const;
    void startContentSearch();
    void showRows(SearchResults rows, QStringList highlights);
    void perform(const SearchResult& result, Action action);
    void performApp(const SearchResult& app, Action action);
    void performPlace(const SearchResult& place, Action action);
    void performWeb(const SearchResult& web, Action action);
    void performMany(const SearchResults& items, Action action);
    void remember(const QString& path); // opened: into the history, if it is kept
    void forgetRecent(const QStringList& paths);
    bool forgetRecycled(const SearchResult& result); // its row and count; false if it is still there
    std::function<void(bool)> reportFailure(const QString& name);
    void onRecycled(const SearchResult& result, bool ok);
    void onRecycledMany(const SearchResults& items, bool ok);
    void onResults(quint64 id, const SearchResults& results, qint64 total, qint64 elapsedUs);
    void onContentResults(quint64 id, const SearchResults& results);
    void onContentProgress(quint64 id, int scanned, int total, bool finished);
    void refreshStatus();
    void flash(const QString& message);
    void refreshFrequentApps();

    struct FrequentApp {
        QString name;
        QString path; // launch path
        QString icon;
        bool operator==(const FrequentApp&) const = default;
    };

    IndexService* m_index;
    AppCatalog* m_apps;
    SystemCatalog* m_places;
    SearchEngine* m_engine;
    History* m_history;
    ResultModel m_results;
    QPointer<QWindow> m_window;

    QString m_query;
    Scope m_scope = All;
    quint64 m_requestId = 0;
    bool m_pending = false;
    bool m_haveResults = false;
    // What the rows on screen are for. Searching it again (window reopened,
    // index updated) refreshes them in place instead of starting over.
    QString m_shownText;
    Scope m_shownScope = All;
    bool m_shownByTime = false;
    KindOrder m_shownKindOrder = KindOrder::FilesFirst;
    bool m_refresh = false; // the current search is such a refresh
    bool m_quiet = false; // ...of complete results: it shows no progress
    bool m_scanComplete = false; // the content scan behind the rows on screen ran to the end
    bool m_replacePending = false; // the current search is for a new query and has shown nothing yet
    // Refresh: content matches kept from the last scan that the running one
    // has not found again yet. Removed if it finishes without them.
    QSet<QString> m_unconfirmed;
    qint64 m_totalMatches = 0;
    qint64 m_elapsedUs = 0;
    bool m_contentRunning = false;
    bool m_withContent = false; // 全部: content matches follow the name matches
    int m_contentScanned = 0;
    int m_contentTotal = 0;
    int m_contentHits = 0; // 全部: content rows added below the name matches

    bool m_recordHistory = true;
    KindOrder m_kindOrder = KindOrder::FilesFirst;
    bool m_rankByTime = false;
    bool m_showModified = true;
    QString m_numberKeys = QStringLiteral("ctrl");
    bool m_composing = false;
    bool m_inTaskbarBox = false;
    std::optional<std::pair<int, int>> m_held; // holdUntilShown(): action, number
    std::vector<FrequentApp> m_frequent;
    std::vector<AppUse> m_windowsUses; // Windows' record of what was started, read at most every minute
    QElapsedTimer m_windowsUsesAge;
    QStringList m_contentExtensions {QStringLiteral("txt")};
    ContentSizeLimits m_contentSizeLimits;
    bool m_contentInLowPriority = false; // 内容 also looks in system, program and tool folders
    WebShortcuts m_web;
    bool m_contentDocuments = true; // and in Word, Excel, PowerPoint and PDF files

    QString m_status;
    QString m_flash;
    QTimer m_contentDebounce;
    QTimer m_flashTimer;
    QTimer m_statusPoll;
    // A new query's rows a while in coming: the status says so, not the last
    // query's figures under its dimmed rows.
    QTimer m_slowSearch;
    bool m_searchSlow = false;
};

} // namespace ws
