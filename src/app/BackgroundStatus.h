#pragma once

#include "ActivityText.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ws {

class IndexService;
class Updater;

// What Win顺 is busy with in the background, in words: the file index loading
// or being built, the content index reading, the snapshot saved, the index
// folder moved, an update downloaded. One place says it, for the tray menu,
// the settings' 运行状况 card and the notice when a big job is done.
// Reads state only; no thread of its own. While a long job runs it samples its
// progress every few seconds (for a speed that can be believed), and stops
// when none runs.
class BackgroundStatus : public QObject {
    Q_OBJECT

public:
    struct Task {
        QString name; // "正在建立内容索引"
        QString count; // "1,234 / 5,000", or ""
        QString remaining; // "约 3–5 分钟", "快好了", why it waits, or ""
        double fraction = -1; // 0..1, -1: unknown
        bool longRunning = false; // minutes, not seconds
        QString line() const; // "正在建立内容索引 1,234 / 5,000，约 3–5 分钟"
    };

    BackgroundStatus(IndexService* index, Updater* updater, QObject* parent = nullptr);

    // The user asked for the index to be built again (the tray's 重建索引).
    void rebuildRequested();
    void setMovePercent(int percent) { m_movePercent = percent; }
    // A job of more than two minutes ended: title and text for a notice.
    std::function<void(const QString& title, const QString& text)> onFinished;

    std::vector<Task> tasks();
    // The most important jobs' lines, at most `most`.
    QStringList lines(int most = 2);
    QVariantList toVariant(); // [{name, count, remaining, fraction}] for the settings card
    // "现在没有在忙，闲着时会把内存还给系统": said when nothing runs.
    static QString idleText();

    static constexpr std::int64_t kNoticeAfterMs = activity::kNoticeAfterMs;

private:
    void update(); // a signal or the sampler: sample, and tell what ended
    void sample();
    static std::int64_t nowMs();

    IndexService* m_index;
    Updater* m_updater;
    QTimer m_sampler;
    activity::RateEstimator m_contentRate;
    int m_movePercent = 0;
    bool m_rebuilding = false;
    // The big jobs, for the notice: when each began (0: not under way).
    std::int64_t m_fileStarted = 0;
    std::int64_t m_contentStarted = 0;
    std::int64_t m_contentLastEnded = 0;
    bool m_contentWasReading = false;
};

} // namespace ws
