#pragma once

#include "SearchTypes.h"
#include "WorkerPool.h"

#include <QObject>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace qf {

class AppCatalog;
class IndexService;

// Runs searches on a worker thread. Only the newest request matters: a new
// submit() cancels whatever is running, so typing never queues up work.
// Name searches also go through the installed apps (`apps` may be null).
// Signals are emitted from the worker thread (queued to the receivers).
class SearchEngine : public QObject {
    Q_OBJECT

public:
    struct Request {
        QString text;
        Scope scope = Scope::All;
        int limit = 100;
        QStringList history; // newest first
        QStringList contentExtensions; // e.g. {"txt"}
        qint64 maxContentFileBytes = 64ll << 20;
        int maxContentResults = 300;
    };

    explicit SearchEngine(IndexService* index, AppCatalog* apps = nullptr, QObject* parent = nullptr);
    ~SearchEngine() override;

    quint64 submit(Request request);
    void cancel();

signals:
    // Name search, or the (empty) start of a content search.
    void resultsReady(quint64 requestId, const qf::SearchResults& results, qint64 totalMatches, qint64 elapsedUs);
    // Content search delivers matches incrementally.
    void contentResults(quint64 requestId, const qf::SearchResults& results);
    void contentProgress(quint64 requestId, int scanned, int total, bool finished);

private:
    struct Job {
        quint64 id = 0;
        Request request;
    };

    void workerLoop(std::stop_token stop);
    void runNameSearch(const Job& job);
    void runAppSearch(const Job& job);
    void runContentSearch(const Job& job);
    bool isStale(quint64 id) const noexcept { return m_latest.load(std::memory_order_relaxed) != id; }

    IndexService* m_index;
    AppCatalog* m_apps;
    WorkerPool m_pool {WorkerPool::defaultThreadCount()};
    std::mutex m_mutex;
    std::condition_variable_any m_cv;
    std::optional<Job> m_pending;
    quint64 m_counter = 0;
    std::atomic<quint64> m_latest {0};
    std::jthread m_worker; // last: started after everything above exists
};

} // namespace qf
