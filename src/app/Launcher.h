#pragma once

#include "ResultModel.h"
#include "SearchTypes.h"

#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QVariantList>
#include <QTimer>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

#include <functional>

namespace ws {

class AppCatalog;
class History;
class IndexService;
class SearchEngine;

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

public:
    enum Scope { All, Apps, Files, Folders, Content }; // as ws::Scope
    Q_ENUM(Scope)
    enum Action { Open, Reveal, RunAsAdmin, CopyPath, CopyName, CopyItem, Recycle };
    Q_ENUM(Action)

    Launcher(IndexService* index, AppCatalog* apps, SearchEngine* engine, History* history, QObject* parent = nullptr);

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
    // and in 全部 unless the query is name syntax (see contentNeedle()).
    bool searchesContent() const { return m_scope == Content || m_withContent; }

    void setWindow(QWindow* window) { m_window = window; }
    void setContentOptions(QStringList extensions, qint64 maxFileBytes, bool inLowPriority);
    void handleShown();
    void handleHidden();
    void retranslate(); // the language changed: status, placeholder, rows

    Q_INVOKABLE void trigger(int row, int action);
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

private:
    void search();
    QString contentNeedle() const;
    QString shownExtensions() const;
    void startContentSearch();
    void showRows(SearchResults rows, QStringList highlights);
    void perform(const SearchResult& result, Action action);
    void performApp(const SearchResult& app, Action action);
    std::function<void(bool)> reportFailure(const QString& name);
    void onRecycled(const SearchResult& result, bool ok);
    void onResults(quint64 id, const SearchResults& results, qint64 total, qint64 elapsedUs);
    void onContentResults(quint64 id, const SearchResults& results);
    void onContentProgress(quint64 id, int scanned, int total, bool finished);
    void refreshStatus();
    void flash(const QString& message);

    IndexService* m_index;
    AppCatalog* m_apps;
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

    QStringList m_contentExtensions {QStringLiteral("txt")};
    qint64 m_maxContentBytes = 64ll << 20;
    bool m_contentInLowPriority = false; // 内容 also looks in system, program and tool folders

    QString m_status;
    QString m_flash;
    QTimer m_contentDebounce;
    QTimer m_flashTimer;
    QTimer m_statusPoll;
};

} // namespace ws
