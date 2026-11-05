#include "BackgroundStatus.h"

#include "IndexService.h"
#include "Updater.h"

#include <QCoreApplication>
#include <QLocale>
#include <QVariantMap>

#include <algorithm>
#include <chrono>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr int kSampleMs = 5000;
constexpr std::size_t kBigPass = 1000; // files: fewer is a small round, not told of
constexpr std::int64_t kStaleMs = 10 * 60 * 1000; // a pass cut short, not taken up again within this: a new job

QString number(std::size_t n)
{
    return QLocale().toString(static_cast<qulonglong>(n));
}

} // namespace

QString BackgroundStatus::Task::line() const
{
    QString text = name;
    if (!count.isEmpty())
        text += u' ' + count;
    if (!remaining.isEmpty())
        text += BackgroundStatus::tr("，") + remaining;
    return text;
}

std::int64_t BackgroundStatus::nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

QString BackgroundStatus::idleText()
{
    return tr("现在没有在忙，闲着时会把内存还给系统");
}

BackgroundStatus::BackgroundStatus(IndexService* index, Updater* updater, QObject* parent)
    : QObject(parent)
    , m_index(index)
    , m_updater(updater)
{
    m_sampler.setInterval(kSampleMs);
    connect(&m_sampler, &QTimer::timeout, this, &BackgroundStatus::update);
    connect(m_index, &IndexService::stateChanged, this, &BackgroundStatus::update);
    connect(m_index, &IndexService::activityChanged, this, &BackgroundStatus::update);
    if (m_updater)
        connect(m_updater, &Updater::installChanged, this, &BackgroundStatus::update);
}

void BackgroundStatus::rebuildRequested()
{
    m_rebuilding = true;
    if (m_fileStarted == 0)
        m_fileStarted = nowMs();
    update();
}

void BackgroundStatus::sample()
{
    const ContentIndexer::Status content = m_index->contentIndexer()->status();
    if (content.reading)
        m_contentRate.feed(nowMs(), content.done);
}

std::vector<BackgroundStatus::Task> BackgroundStatus::tasks()
{
    sample();
    std::vector<Task> out;
    using State = IndexService::State;
    const State state = m_index->state();
    if (state == State::Idle || state == State::Loading) {
        out.push_back({tr("正在加载索引…"), {}, {}, -1, false});
    } else if (state == State::Building) {
        out.push_back({tr("正在建立文件索引，已收录 %1 项").arg(number(m_index->itemCount())), {}, {}, -1, true});
    } else if (m_index->isRefreshing()) {
        out.push_back({m_rebuilding ? tr("正在重建文件索引") : tr("正在同步文件的变化"), {}, {}, -1, m_rebuilding});
    }
    const ContentIndexer::Status content = m_index->contentIndexer()->status();
    if (content.reading && state == State::Ready) {
        activity::Progress p;
        p.done = content.done;
        p.total = content.total;
        p.perSecond = m_contentRate.perSecond();
        p.sampleMs = m_contentRate.sampleMs();
        p.pause = static_cast<activity::Pause>(content.pause);
        p.finishing = content.finishing;
        Task t;
        t.name = content.total >= kBigPass ? tr("正在建立内容索引") : tr("正在更新内容索引");
        if (!content.finishing)
            t.count = number(content.done) + u" / " + number(content.total);
        t.remaining = activity::remainingText(p);
        t.fraction = content.total > 0 ? std::min(1.0, static_cast<double>(content.done) / static_cast<double>(content.total)) : -1;
        t.longRunning = content.total >= kBigPass;
        out.push_back(std::move(t));
    }
    if (m_index->moving())
        out.push_back({tr("正在搬索引文件夹 %1%").arg(m_movePercent), {}, {}, m_movePercent / 100.0, true});
    if (m_updater && m_updater->installStage() == u"downloading"_s) {
        out.push_back({tr("正在下载更新 %1%").arg(qRound(m_updater->installProgress() * 100)), {}, {},
            m_updater->installProgress(), true});
    }
    if (m_index->saving())
        out.push_back({tr("正在保存索引"), {}, {}, -1, false});
    return out;
}

QStringList BackgroundStatus::lines(int most)
{
    QStringList out;
    for (const Task& t : tasks()) {
        if (out.size() >= most)
            break;
        out.push_back(t.line());
    }
    return out;
}

QVariantList BackgroundStatus::toVariant()
{
    QVariantList out;
    for (const Task& t : tasks()) {
        out.push_back(QVariantMap {{u"name"_s, t.name}, {u"count"_s, t.count}, {u"remaining"_s, t.remaining},
            {u"fraction"_s, t.fraction}});
    }
    return out;
}

void BackgroundStatus::update()
{
    const std::int64_t now = nowMs();
    using State = IndexService::State;
    const State state = m_index->state();
    sample();

    // The file index: built for the first time, or again on request.
    const bool fileBusy = state == State::Building || (m_rebuilding && m_index->isRefreshing());
    if (fileBusy && m_fileStarted == 0)
        m_fileStarted = now;
    if (!fileBusy && state == State::Ready && !m_index->isRefreshing()) {
        if (m_fileStarted != 0 && now - m_fileStarted >= kNoticeAfterMs && onFinished)
            onFinished(tr("文件索引建好了"), tr("用了 %1。").arg(activity::tookText(now - m_fileStarted)));
        m_fileStarted = 0;
        m_rebuilding = false;
    }

    // The content index: one reading, maybe in several passes cut short by the
    // user's own searches; told once a big one went through to its end.
    const ContentIndexer::Status content = m_index->contentIndexer()->status();
    if (content.reading && !m_contentWasReading) {
        if (m_contentStarted == 0 || now - m_contentLastEnded > kStaleMs) {
            m_contentStarted = now;
            m_contentRate.reset();
        }
    } else if (!content.reading && m_contentWasReading) {
        m_contentLastEnded = now;
        if (content.done >= content.total) {
            if (content.total >= kBigPass && now - m_contentStarted >= kNoticeAfterMs && onFinished) {
                onFinished(tr("内容索引建好了"),
                    tr("用了 %1。现在可以搜文件里的字了。").arg(activity::tookText(now - m_contentStarted)));
            }
            m_contentStarted = 0;
        }
    }
    m_contentWasReading = content.reading;

    // Sampled only while something long runs.
    const bool running = fileBusy || content.reading || m_index->moving();
    if (running && !m_sampler.isActive())
        m_sampler.start();
    else if (!running && m_sampler.isActive())
        m_sampler.stop();
}

} // namespace ws
