#pragma once

#include "ContentIndex.h"
#include "SearchTypes.h"
#include "WebShortcut.h"
#include "WorkerPool.h"

#include <QObject>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace ws {

class AppCatalog;
class DocExtractor;
class IndexService;
class RecentOnDisk;
class SystemCatalog;

// Runs searches on a worker thread. Only the newest request matters: a new
// submit() cancels whatever is running, so typing never queues up work.
// Name searches also go through the installed apps, the places in Windows
// (`apps` and `places` may be null) and the web shortcuts.
// Signals are emitted from the worker thread (queued to the receivers).
class SearchEngine : public QObject {
    Q_OBJECT

public:
    struct Request {
        QString text;
        Scope scope = Scope::All;
        KindOrder kindOrder = KindOrder::FilesFirst; // 全部, 文件
        RankBy rankBy = RankBy::Match; // 文件, 文件夹 (全部 always by match)
        int limit = 100;
        QStringList history; // newest first
        QStringList contentExtensions; // e.g. {"txt"}
        ContentSizeLimits contentSizeLimits; // by kind of file
        bool contentDocuments = true; // and Word, Excel, PowerPoint, PDF (documentExtensions())
        int maxContentResults = 300;
        // Also inside system, program and tool folders (EntryFlag::LowPriority).
        bool contentInLowPriority = false;
        // Files read at once (each open waits for the antivirus, so more
        // than processors). Asked for (内容): many; 全部's own: a few.
        int contentThreads = 4;
        // 全部: a keyword typed first puts its page or search on top; names find them too.
        WebShortcuts web;
    };

    explicit SearchEngine(
        IndexService* index, AppCatalog* apps = nullptr, SystemCatalog* places = nullptr, QObject* parent = nullptr);
    ~SearchEngine() override;

    quint64 submit(Request request);
    void cancel();

signals:
    // Name search, or the (empty) start of a content search.
    void resultsReady(quint64 requestId, const ws::SearchResults& results, qint64 totalMatches, qint64 elapsedUs);
    // Content search delivers matches incrementally.
    void contentResults(quint64 requestId, const ws::SearchResults& results);
    void contentProgress(quint64 requestId, int scanned, int total, bool finished);

private:
    struct Job {
        quint64 id = 0;
        Request request;
        std::chrono::steady_clock::time_point submitted;
    };

    void workerLoop(std::stop_token stop);
    void runNameSearch(const Job& job);
    void runContentSearch(const Job& job);
    bool isStale(quint64 id) const noexcept { return m_latest.load(std::memory_order_relaxed) != id; }

    IndexService* m_index;
    std::unique_ptr<DocExtractor> m_documents; // reads documents the content index has no text of
    AppCatalog* m_apps;
    SystemCatalog* m_places;
    std::unique_ptr<RecentOnDisk> m_onDisk; // whether recent items are still there
    WorkerPool m_pool {WorkerPool::defaultThreadCount()};
    std::mutex m_mutex;
    std::condition_variable_any m_cv;
    std::optional<Job> m_pending;
    quint64 m_counter = 0;
    std::atomic<quint64> m_latest {0};
    std::jthread m_worker; // last: started after everything above exists
};

} // namespace ws
