#include "HealthMonitor.h"

#include "BackgroundStatus.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>
#include <QPointer>
#include <QThreadPool>

#include <algorithm>
#include <cwchar>
#include <cmath>
#include <random>

#include <windows.h>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr double kMb = 1024.0 * 1024.0;

bool named(const memory::Process& p, const wchar_t* exe)
{
    return ::_wcsicmp(p.exe, exe) == 0;
}

bool document(const memory::Process& p)
{
    return ::_wcsnicmp(p.exe, L"WinShunExtract", 14) == 0;
}

// The words for a program of Win顺's in the list of processes.
QString processName(const memory::Process& p, bool self)
{
    if (self)
        return HealthMonitor::tr("Win顺 主程序");
    struct Known {
        const wchar_t* exe;
        const char* name;
    };
    static const Known known[] = {
        {L"WinShunService.exe", QT_TRANSLATE_NOOP("ws::HealthMonitor", "后台服务")},
        {L"WinShunExtract.exe", QT_TRANSLATE_NOOP("ws::HealthMonitor", "读文档")},
        {L"WinShunPluginHost.exe", QT_TRANSLATE_NOOP("ws::HealthMonitor", "插件运行环境")},
        {L"WinShunCapture.exe", QT_TRANSLATE_NOOP("ws::HealthMonitor", "截图录屏")},
        {L"WinShunOcrSearch.exe", QT_TRANSLATE_NOOP("ws::HealthMonitor", "图片文字搜索")},
        {L"WinShunSemantic.exe", QT_TRANSLATE_NOOP("ws::HealthMonitor", "语义搜索")},
        {L"WinShunTranslate.exe", QT_TRANSLATE_NOOP("ws::HealthMonitor", "翻译")},
        {L"WinShunBookmarks.exe", QT_TRANSLATE_NOOP("ws::HealthMonitor", "书签")},
        {L"WinShunBreak.exe", QT_TRANSLATE_NOOP("ws::HealthMonitor", "休息提醒")},
        {L"WinShunLocalSend.exe", QT_TRANSLATE_NOOP("ws::HealthMonitor", "LocalSend")},
        {L"WinShunExplorerTabs.exe", QT_TRANSLATE_NOOP("ws::HealthMonitor", "资源管理器标签")},
        {L"WinShunSwitches.exe", QT_TRANSLATE_NOOP("ws::HealthMonitor", "系统开关")},
        {L"WinShunTools.exe", QT_TRANSLATE_NOOP("ws::HealthMonitor", "小工具")},
        {L"WinShunMcp.exe", QT_TRANSLATE_NOOP("ws::HealthMonitor", "AI 助手连接")},
        {L"WinShunUpdate.exe", QT_TRANSLATE_NOOP("ws::HealthMonitor", "更新程序")},
    };
    for (const Known& k : known) {
        if (named(p, k.exe))
            return QCoreApplication::translate("ws::HealthMonitor", k.name);
    }
    QString exe = QString::fromWCharArray(p.exe);
    if (exe.endsWith(u".exe"_s, Qt::CaseInsensitive))
        exe.chop(4);
    return exe;
}

QString megabytes(double mb)
{
    return mb >= 1 ? HealthMonitor::tr("%1 MB").arg(qRound(mb)) : HealthMonitor::tr("不到 1 MB");
}

QString clock(std::int64_t ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms).toString(u"HH:mm"_s);
}

QString lasted(std::int64_t ms)
{
    if (ms < 60'000)
        return HealthMonitor::tr("%1 秒").arg(std::max<std::int64_t>(1, ms / 1000));
    if (ms < 3'600'000)
        return HealthMonitor::tr("%1 分钟").arg(ms / 60'000);
    return HealthMonitor::tr("%1 小时 %2 分").arg(ms / 3'600'000).arg(ms % 3'600'000 / 60'000);
}

} // namespace

HealthMonitor::HealthMonitor(Sources sources, QObject* parent)
    : QObject(parent)
    , m_sources(std::move(sources))
{
    m_processors = std::max<unsigned>(1, ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    m_coarse.setInterval(static_cast<int>(health::History::kCoarseMs));
    m_coarse.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_coarse, &QTimer::timeout, this, [this] { take(false); });
    m_fine.setInterval(static_cast<int>(health::History::kFineMs));
    connect(&m_fine, &QTimer::timeout, this, [this] { take(true); });
    m_soon.setSingleShot(true);
    m_soon.setInterval(200);
    connect(&m_soon, &QTimer::timeout, this, [this] { take(false); });
    m_coarse.start();
    take(false); // the start: the index loading is seen from its first moment
}

void HealthMonitor::setWatching(bool on)
{
    if (m_watching == on)
        return;
    m_watching = on;
    if (!m_demo) {
        if (on) {
            m_fineBase = {};
            take(true); // the baseline; the next second is the first reading
            m_fine.start();
        } else {
            m_fine.stop();
            m_history.clearFine();
            m_processes.clear();
            m_processes.squeeze();
        }
    }
    publish();
    emit watchingChanged();
}

void HealthMonitor::poke()
{
    if (!m_demo && !m_soon.isActive())
        m_soon.start();
}

void HealthMonitor::noteGiveBack(qint64 megabytes, qint64 atMs)
{
    const std::uint32_t total = m_history.coarse().size() > 0 ? m_history.coarse().back().totalKb() : 0;
    m_events.instant(health::EventKind::ManualTrim, atMs, total, static_cast<std::uint32_t>(std::max<qint64>(0, megabytes)));
    m_lastTrimSeen = atMs;
    poke();
}

void HealthMonitor::rebase(Baseline& baseline, const std::vector<const memory::Process*>& a,
    const std::vector<const memory::Process*>& b, std::int64_t nowFt)
{
    baseline.totals.clear();
    for (const auto* list : {&a, &b}) {
        for (const memory::Process* p : *list)
            baseline.totals.push_back({p->pid, p->created, p->cpu, p->readBytes});
    }
    baseline.atNs = nowFt;
}

HealthMonitor::Group HealthMonitor::since(const Baseline& baseline, const std::vector<const memory::Process*>& procs) const
{
    Group g;
    if (baseline.atNs == 0)
        return g;
    for (const memory::Process* p : procs) {
        const auto it = std::find_if(baseline.totals.begin(), baseline.totals.end(),
            [p](const Totals& t) { return t.pid == p->pid && t.created == p->created; });
        if (it != baseline.totals.end()) {
            g.cpu += p->cpu > it->cpu ? p->cpu - it->cpu : 0;
            g.read += p->readBytes > it->read ? p->readBytes - it->read : 0;
        } else if (p->created >= baseline.atNs) { // began since: all it did is new
            g.cpu += p->cpu;
            g.read += p->readBytes;
        }
    }
    return g;
}

void HealthMonitor::take(bool fine)
{
    if (m_demo)
        return;
    m_wanted |= fine ? kFine : kCoarse;
    if (m_looking)
        return; // the one under way serves this one too
    m_looking = true;
    // The process list off the GUI thread (~20 ms with 950 processes).
    QThreadPool::globalInstance()->start([self = QPointer<HealthMonitor>(this), hint = m_sizeHint]() mutable {
        memory::Look look = memory::look(hint);
        QMetaObject::invokeMethod(qApp, [self, hint, look = std::move(look)]() mutable {
            if (!self)
                return;
            self->m_sizeHint = hint;
            self->m_looking = false;
            const int wanted = std::exchange(self->m_wanted, 0);
            if (!look.ok)
                return;
            if (wanted & kCoarse)
                self->apply(look, false);
            if (wanted & kFine)
                self->apply(look, true);
        }, Qt::QueuedConnection);
    });
}

void HealthMonitor::apply(const memory::Look& look, bool fine)
{
    const std::int64_t nowMs = look.atMs;
    const std::int64_t nowFt = look.atFt;
    const std::vector<memory::Process>& all = look.processes;
    const std::vector<const memory::Process*> mine = memory::family(all, look.self);
    std::vector<const memory::Process*> service;
    for (const memory::Process& p : all) {
        if (named(p, L"WinShunService.exe") && std::find(mine.begin(), mine.end(), &p) == mine.end()) {
            service = memory::family(all, p.pid);
            break;
        }
    }

    std::uint64_t main = 0, plugins = 0, documents = 0, serviceBytes = 0;
    std::uint32_t biggestDocumentKb = 0;
    for (const memory::Process* p : mine) {
        if (p->pid == look.self) {
            main = p->privateBytes;
        } else if (document(*p)) {
            documents += p->privateBytes;
            biggestDocumentKb = std::max(biggestDocumentKb, static_cast<std::uint32_t>(p->privateBytes / 1024));
        } else {
            plugins += p->privateBytes;
        }
    }
    for (const memory::Process* p : service) {
        serviceBytes += p->privateBytes;
        if (document(*p))
            biggestDocumentKb = std::max(biggestDocumentKb, static_cast<std::uint32_t>(p->privateBytes / 1024));
    }
    // The file index's pages in the working set, the same measure as the
    // total (~1.3 ms for 128 MB). Not under a writer's lock: the last reading stands.
    if (m_sources.fileIndexBlocks && m_sources.fileIndexBlocks(m_blocks)) {
        m_fileIndexHeld = 0;
        for (const auto& block : m_blocks)
            m_fileIndexHeld += block.second;
        m_fileIndexBytes = memory::residentBytes(m_blocks);
        m_blocks.clear(); // the addresses only meant something under the lock
        m_blocks.shrink_to_fit();
    }
    const std::uint64_t fileIndex = std::min(m_fileIndexBytes, main);

    health::Sample s {};
    s.atMs = nowMs;
    s.partKb[health::FileIndex] = static_cast<std::uint32_t>(fileIndex / 1024);
    s.partKb[health::Other] = static_cast<std::uint32_t>((main - fileIndex) / 1024);
    s.partKb[health::Plugins] = static_cast<std::uint32_t>(plugins / 1024);
    s.partKb[health::Documents] = static_cast<std::uint32_t>(documents / 1024);
    s.serviceKb = static_cast<std::uint32_t>(serviceBytes / 1024);

    Baseline& base = fine ? m_fineBase : m_coarseBase;
    const bool first = base.atNs == 0;
    const Group a = since(base, mine);
    const Group b = since(base, service);
    const double elapsed = first ? 0.0 : static_cast<double>(std::max<std::int64_t>(1, nowFt - base.atNs)); // 100 ns
    const auto percent = [&](std::uint64_t cpu) {
        return elapsed <= 0 ? std::uint16_t(0)
                            : static_cast<std::uint16_t>(std::min(10000.0, cpu * 10000.0 / (elapsed * m_processors)));
    };
    const auto kbps = [&](std::uint64_t bytes) {
        return elapsed <= 0 ? 0u : static_cast<std::uint32_t>(bytes / 1024.0 / (elapsed / 1e7));
    };
    s.cpu = percent(a.cpu);
    s.serviceCpu = percent(b.cpu);
    s.readKbps = kbps(a.read);
    s.serviceReadKbps = kbps(b.read);

    // The page's list of processes: each one's own share, over the last second.
    if (fine && !first) {
        m_processes.clear();
        const auto add = [&](const memory::Process* p, const QString& kind) {
            const Group one = since(base, {p});
            m_processes.push_back(QVariantMap {
                {u"name"_s, processName(*p, p->pid == look.self)},
                {u"kind"_s, kind},
                {u"mb"_s, p->privateBytes / kMb},
                {u"cpu"_s, percent(one.cpu) / 100.0},
                {u"read"_s, kbps(one.read) / 1024.0},
            });
        };
        for (const memory::Process* p : mine)
            add(p, p->pid == look.self ? u"main"_s : document(*p) ? u"document"_s : u"plugin"_s);
        for (const memory::Process* p : service)
            add(p, u"service"_s);
    }
    rebase(base, mine, service, nowFt);

    if (!first) {
        // A 10-second reading also while watching: its CPU and reads over its own ten seconds.
        if (fine)
            m_history.addFine(s);
        else
            m_history.addCoarse(s);
    }

    if (!fine) { // what happened: once per look, from the 10-second ones and pokes
        health::Facts facts = m_sources.facts ? m_sources.facts() : health::Facts {};
        facts.biggestDocumentKb = biggestDocumentKb;
        m_tracker.observe(facts, nowMs, s.totalKb());
        if (m_sources.lastTrim) {
            const auto [atMs, mb] = m_sources.lastTrim();
            if (atMs != 0 && atMs != m_lastTrimSeen) {
                m_lastTrimSeen = atMs;
                m_events.instant(health::EventKind::AutoTrim, atMs, s.totalKb(), static_cast<std::uint32_t>(std::max<qint64>(0, mb)));
            }
        }
    }
    if (fine || m_watching)
        publish();
}
void HealthMonitor::publish()
{
    ++m_revision;
    if (!m_watching) {
        if (!m_now.isEmpty()) {
            m_now.clear(); // nothing kept for a page that is not there
            emit updated();
        }
        return;
    }
    const health::Ring& ring = m_history.fine().size() > 0 ? m_history.fine() : m_history.coarse();
    if (ring.size() == 0) {
        emit updated();
        return;
    }
    const health::Sample& s = ring.back();
    QVariantMap now;
    const auto mb = [](std::uint32_t kb) { return kb / 1024.0; };
    now[u"total"_s] = mb(s.totalKb());
    QVariantList parts;
    const auto part = [&](const QString& key, const QString& name, std::uint32_t kb, const QString& note) {
        parts.push_back(QVariantMap {{u"key"_s, key}, {u"name"_s, name}, {u"mb"_s, mb(kb)}, {u"note"_s, note}});
    };
    const double parked = std::max(0.0, (static_cast<double>(m_fileIndexHeld) - m_fileIndexBytes) / kMb);
    part(u"fileIndex"_s, tr("文件索引"), s.partKb[health::FileIndex],
        parked >= 4 ? tr("另有 %1 MB 已暂存，用到时再读回").arg(qRound(parked)) : tr("搜索靠它"));
    part(u"other"_s, tr("界面和其他"), s.partKb[health::Other], tr("窗口、剪贴板历史、内容索引的缓冲"));
    part(u"plugins"_s, tr("插件"), s.partKb[health::Plugins], QString());
    part(u"documents"_s, tr("读文档"), s.partKb[health::Documents], tr("读完 30 秒后退出"));
    now[u"parts"_s] = parts;
    now[u"cpu"_s] = s.cpu / 100.0;
    now[u"read"_s] = s.readKbps / 1024.0;
    now[u"serviceMb"_s] = mb(s.serviceKb);
    now[u"serviceCpu"_s] = s.serviceCpu / 100.0;
    now[u"serviceRead"_s] = s.serviceReadKbps / 1024.0;
    now[u"hasService"_s] = s.serviceKb > 0;
    now[u"processes"_s] = m_processes;
    const QVariantList tasks = m_sources.tasks ? m_sources.tasks() : QVariantList();
    now[u"tasks"_s] = tasks;
    now[u"busy"_s] = !tasks.isEmpty();
    now[u"idleText"_s] = BackgroundStatus::idleText();
    const auto [atMs, freed] = m_sources.lastTrim ? m_sources.lastTrim() : std::pair<qint64, qint64>(0, 0);
    now[u"lastTrim"_s] = atMs == 0 ? tr("还没有归还过") : tr("上次归还：%1，还了 %2 MB").arg(clock(atMs)).arg(freed);
    now[u"historyKb"_s] = static_cast<qint64>(m_history.bytes() / 1024);
    m_now = now;
    emit updated();
}

QVariantList HealthMonitor::recent() const
{
    QVariantList out;
    if (!m_watching)
        return out;
    for (const health::Event& e : m_events.recent(8)) {
        out.push_back(QVariantMap {
            {u"title"_s, eventTitle(e)},
            {u"detail"_s, eventDetail(e)},
            {u"time"_s, clock(e.startMs)},
            {u"running"_s, e.open()},
            {u"trim"_s, e.instant()},
        });
    }
    return out;
}

QString HealthMonitor::eventsAt(qint64 ms) const
{
    QStringList titles;
    // A give-back is a moment: near enough, within half a minute.
    for (const health::Event& e : m_events.between(ms - 30'000, ms + 30'000)) {
        const bool over = e.instant() ? std::abs(e.startMs - ms) <= 30'000 : (e.startMs <= ms && (e.open() || e.endMs >= ms));
        if (over && !titles.contains(eventTitle(e)))
            titles.push_back(eventTitle(e));
    }
    return titles.join(tr(" · "));
}

QString HealthMonitor::eventTitle(const health::Event& e)
{
    using K = health::EventKind;
    switch (e.kind) {
    case K::IndexLoad: return tr("加载文件索引");
    case K::IndexBuild: return tr("第一次建立文件索引");
    case K::IndexRebuild: return tr("重建文件索引");
    case K::ContentRound:
        return e.count > 0 ? tr("读文件内容，%1 个文件").arg(QLocale().toString(static_cast<qulonglong>(e.count))) : tr("读文件内容");
    case K::BigDocument: return tr("读一个大文档");
    case K::IndexSave: return tr("保存索引");
    case K::IndexMove: return tr("搬索引文件夹");
    case K::AutoTrim: return tr("闲着时归还了 %1 MB").arg(e.freedMb);
    case K::ManualTrim: return tr("手动归还了 %1 MB").arg(e.freedMb);
    }
    return {};
}

QString HealthMonitor::eventDetail(const health::Event& e)
{
    if (e.instant())
        return tr("页面换出到系统的备用内存，用到时再读回");
    const double peak = e.peakKb / 1024.0;
    if (e.open())
        return tr("进行中，已经 %1，内存最高 %2").arg(lasted(QDateTime::currentMSecsSinceEpoch() - e.startMs), megabytes(peak));
    QString text = tr("%1–%2，用了 %3，内存最高 %4").arg(clock(e.startMs), clock(e.endMs), lasted(e.endMs - e.startMs), megabytes(peak));
    if (e.kind == health::EventKind::BigDocument && e.count > 0)
        text += tr("（这个读文档的进程最多 %1）").arg(megabytes(e.count / 1024.0));
    return text;
}

HealthMonitor* HealthMonitor::demo(QObject* parent, bool idle)
{
    auto* m = new HealthMonitor(Sources {}, parent);
    m->m_demo = true;
    m->m_coarse.stop();
    m->m_history.clear();
    m->m_events.clear();
    const std::int64_t end = QDateTime::currentMSecsSinceEpoch();
    const std::int64_t start = end - 24 * 3600 * 1000LL;
    std::mt19937 random(7);
    std::uniform_real_distribution<double> jitter(0.0, 1.0);
    // What happens when (ms before the end), as the readings will show it.
    struct Job {
        std::int64_t from, to;
        double otherMb, docMb, cpu, read, serviceMb, serviceCpu, serviceRead;
    };
    const std::int64_t h = 3600 * 1000LL;
    const std::int64_t mi = 60 * 1000LL;
    std::vector<Job> jobs = {
        {23 * h + 50 * mi, 23 * h + 49 * mi, 210, 0, 6, 120, 30, 2, 40}, // start: the index loads
        {9 * h + 30 * mi, 9 * h + 18 * mi, 380, 160, 22, 45, 120, 18, 160}, // a content round
        {9 * h + 26 * mi, 9 * h + 24 * mi, 0, 420, 9, 20, 0, 0, 0}, // a big PDF inside it
        {4 * h + 10 * mi, 4 * h + 8 * mi, 120, 0, 3, 2, 0, 0, 0}, // the launcher, settings
        {2 * h + 5 * mi, 2 * h + 2 * mi, 90, 30, 7, 25, 25, 4, 30}, // a small content round
    };
    if (!idle)
        jobs.push_back({6 * mi, 0, 260, 120, 18, 38, 95, 14, 120}); // reading right now
    const auto add = [&](std::int64_t step, std::int64_t from, bool fine) {
        for (std::int64_t t = from; t <= end; t += step) {
            const std::int64_t before = end - t;
            if (!fine && before > 14 * h + 30 * mi && before < 15 * h) // asleep: a hole
                continue;
            health::Sample s {};
            s.atMs = t;
            double other = 12 + 3 * jitter(random), docs = 0, cpu = 0.05 + 0.1 * jitter(random), read = 0.01;
            double serviceMb = 9, serviceCpu = 0.02, serviceRead = 0;
            double fileIndex = 2;
            for (const Job& j : jobs) {
                if (before <= j.from && before >= j.to) {
                    const double wave = 0.75 + 0.25 * std::sin(t / 23000.0) + 0.1 * jitter(random);
                    other += j.otherMb * wave;
                    docs += j.docMb * wave;
                    cpu += j.cpu * wave;
                    read += j.read * wave;
                    serviceMb += j.serviceMb * wave;
                    serviceCpu += j.serviceCpu * wave;
                    serviceRead += j.serviceRead * wave;
                    fileIndex = 96;
                }
            }
            s.partKb[health::FileIndex] = static_cast<std::uint32_t>(fileIndex * 1024);
            s.partKb[health::Other] = static_cast<std::uint32_t>(other * 1024);
            s.partKb[health::Plugins] = static_cast<std::uint32_t>((before < 20 * h ? 13 : 0) * 1024);
            s.partKb[health::Documents] = static_cast<std::uint32_t>(docs * 1024);
            s.serviceKb = static_cast<std::uint32_t>(serviceMb * 1024);
            s.cpu = static_cast<std::uint16_t>(std::min(100.0, cpu) * 100);
            s.serviceCpu = static_cast<std::uint16_t>(std::min(100.0, serviceCpu) * 100);
            s.readKbps = static_cast<std::uint32_t>(read * 1024);
            s.serviceReadKbps = static_cast<std::uint32_t>(serviceRead * 1024);
            if (fine)
                m->m_history.addFine(s);
            else
                m->m_history.addCoarse(s);
        }
    };
    add(health::History::kCoarseMs, start, false);
    add(health::History::kFineMs, end - 10 * mi, true);
    using K = health::EventKind;
    const auto span = [&](K kind, std::int64_t from, std::int64_t to, double peakMb, std::uint64_t count) {
        const int id = m->m_events.begin(kind, end - from, static_cast<std::uint32_t>(peakMb * 1024));
        m->m_events.setCount(id, count);
        if (to > 0)
            m->m_events.end(id, end - to);
    };
    span(K::IndexLoad, 23 * h + 50 * mi, 23 * h + 50 * mi - 4000, 248, 0);
    m->m_events.instant(K::AutoTrim, end - 23 * h - 49 * mi, 20 * 1024, 231);
    span(K::ContentRound, 9 * h + 30 * mi, 9 * h + 18 * mi, 712, 41'382);
    span(K::BigDocument, 9 * h + 26 * mi, 9 * h + 24 * mi, 702, 431 * 1024);
    m->m_events.instant(K::AutoTrim, end - 9 * h - 17 * mi, 30 * 1024, 587);
    m->m_events.instant(K::ManualTrim, end - 4 * h - 7 * mi, 140 * 1024, 121);
    span(K::ContentRound, 2 * h + 5 * mi, 2 * h + 2 * mi, 160, 214);
    m->m_events.instant(K::AutoTrim, end - 2 * h - mi, 40 * 1024, 132);
    if (!idle)
        span(K::ContentRound, 6 * mi, 0, 520, 18'204);
    QVariantList processes;
    const auto proc = [&](const QString& name, const QString& kind, double mb, double cpu) {
        processes.push_back(QVariantMap {{u"name"_s, name}, {u"kind"_s, kind}, {u"mb"_s, mb}, {u"cpu"_s, cpu}, {u"read"_s, 0.0}});
    };
    proc(tr("Win顺 主程序"), u"main"_s, idle ? 16 : 352, idle ? 0.1 : 12.4);
    proc(tr("插件运行环境"), u"plugin"_s, 3, 0);
    proc(tr("截图录屏"), u"plugin"_s, 6, 0);
    proc(tr("图片文字搜索"), u"plugin"_s, 4, 0);
    if (!idle)
        proc(tr("读文档"), u"document"_s, 118, 4.2);
    proc(tr("后台服务"), u"service"_s, idle ? 9 : 104, idle ? 0 : 13.0);
    m->m_processes = processes;
    m->m_sources.tasks = [idle] {
        QVariantList tasks;
        if (!idle)
            tasks.push_back(QVariantMap {{u"name"_s, tr("正在建立内容索引")}, {u"count"_s, u"18,204 / 41,382"_s},
                {u"remaining"_s, tr("约 3–5 分钟")}, {u"fraction"_s, 0.44}});
        return tasks;
    };
    m->m_sources.lastTrim = [end] { return std::pair<qint64, qint64>(end - 2 * 3600 * 1000LL - 60'000, 132); };
    m->m_fileIndexHeld = static_cast<std::uint64_t>(idle ? 98 * kMb : 96 * kMb);
    m->m_fileIndexBytes = static_cast<std::uint64_t>(idle ? 0 : 96 * kMb);
    return m;
}

} // namespace ws
