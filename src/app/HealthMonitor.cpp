#include "HealthMonitor.h"

#include "BackgroundStatus.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>
#include <QPointer>
#include <QThreadPool>

#include <algorithm>
#include <cwchar>

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
    m_coarse.setInterval(kCoarseMs);
    m_coarse.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_coarse, &QTimer::timeout, this, [this] { take(false); });
    m_fine.setInterval(kFineMs);
    connect(&m_fine, &QTimer::timeout, this, [this] { take(true); });
    m_soon.setSingleShot(true);
    m_soon.setInterval(200);
    connect(&m_soon, &QTimer::timeout, this, [this] { take(false, true); });
    m_coarse.start();
    take(false); // the start: the index loading is seen from its first moment
}

void HealthMonitor::setWatching(bool on)
{
    if (m_watching == on)
        return;
    m_watching = on;
    qInfo("Health page: %s", on ? "watching, a reading every second" : "not watching");
    if (!m_demo) {
        if (on) {
            m_fineBase = {};
            m_fineServiceBase = {};
            m_fineCount = 0;
            m_mapsToHeaps = 1; // the second map: the window made by then
            take(true); // the baseline (and the memory map); the next second is the first reading
            m_fine.start();
        } else {
            m_fine.stop();
            m_processes = {};
            m_mapped.reset();
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
    m_events.instant(health::EventKind::ManualTrim, atMs, m_reading.totalKb(), static_cast<std::uint32_t>(std::max<qint64>(0, megabytes)));
    m_lastTrimSeen = atMs;
    m_fineCount = 0; // the memory map again at once: what was given back shows
    m_mapsToHeaps = 0;
    poke();
}

void HealthMonitor::rebase(Baseline& baseline, const std::vector<const memory::Process*>& procs, std::int64_t nowFt)
{
    baseline.totals.clear();
    for (const memory::Process* p : procs)
        baseline.totals.push_back({p->pid, p->created, p->cpu, p->readBytes});
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

void HealthMonitor::take(bool fine, bool whole)
{
    if (m_demo)
        return;
    m_wanted |= (fine ? kFine : kCoarse) | (whole || fine ? kWhole : 0);
    if (m_looking)
        return; // the one under way serves this one too
    const std::int64_t nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_contentReading && nowMs - m_lastWholeMs >= kWholeMs)
        m_wanted |= kWhole; // a document reader under the service may have grown
    const bool again = !(m_wanted & kWhole) && !m_known.empty();

    // The memory map every third second while watching. The file index's
    // blocks are only addresses to it: asked for under the index's lock here,
    // compared with (never read) on the pool thread.
    std::optional<Mapped> wantMap;
    Blocks blocks;
    bool heaps = false;
    if ((m_wanted & kFine) && m_fineCount++ % 3 == 0) {
        std::uint64_t tables = 0;
        if (!m_sources.fileIndex || m_sources.fileIndex(blocks, tables)) {
            wantMap.emplace();
            wantMap->fileIndexTables = tables;
            heaps = m_mapsToHeaps-- <= 0;
            if (heaps)
                m_mapsToHeaps = kMapsPerHeaps;
            m_fileIndexHeld = 0;
            for (const auto& block : blocks)
                m_fileIndexHeld += block.second;
        }
    }
    m_looking = true;
    QThreadPool::globalInstance()->start([self = QPointer<HealthMonitor>(this), hint = m_sizeHint,
                                             known = again ? m_known : std::vector<memory::Process> {},
                                             wantMap = std::move(wantMap), blocks = std::move(blocks),
                                             heaps, pictures = m_sources.cachedPictures]() mutable {
        memory::Look look = known.empty() ? memory::Look {} : memory::again(known);
        if (!look.ok)
            look = memory::look(hint);
        if (wantMap) {
            wantMap->map = memory::map(std::move(blocks), heaps);
            wantMap->pictures = pictures ? pictures() : 0;
        }
        QMetaObject::invokeMethod(qApp, [self, hint, look = std::move(look), wantMap = std::move(wantMap)]() mutable {
            if (!self)
                return;
            self->m_sizeHint = hint;
            self->m_looking = false;
            const int wanted = std::exchange(self->m_wanted, 0);
            if (!look.ok || self->m_demo) // made up since it was asked (the previews)
                return;
            if ((wanted & kWhole) && !look.full)
                self->take(false, true); // asked for while a partial one was under way
            if (wanted & kCoarse)
                self->apply(look, false, std::nullopt);
            if (wanted & kFine)
                self->apply(look, true, std::move(wantMap));
        }, Qt::QueuedConnection);
    });
}

void HealthMonitor::apply(const memory::Look& look, bool fine, std::optional<Mapped> mapped)
{
    const std::int64_t nowMs = look.atMs;
    const std::int64_t nowFt = look.atFt;
    const std::vector<const memory::Process*> mine = memory::family(look.processes, look.self);
    std::vector<const memory::Process*> service;
    if (look.full) {
        for (const memory::Process& p : look.processes) {
            if (named(p, L"WinShunService.exe") && std::find(mine.begin(), mine.end(), &p) == mine.end()) {
                service = memory::family(look.processes, p.pid);
                break;
            }
        }
        m_known.clear();
        for (const memory::Process* p : mine)
            m_known.push_back(*p);
        m_lastWholeMs = nowMs;
    }

    Reading r;
    for (const memory::Process* p : mine) {
        if (p->pid == look.self) {
            r.main = p->privateBytes;
        } else if (document(*p)) {
            r.documents += p->privateBytes;
            r.biggestDocumentKb = std::max(r.biggestDocumentKb, static_cast<std::uint32_t>(p->privateBytes / 1024));
        } else {
            r.plugins += p->privateBytes;
        }
    }
    if (look.full) {
        Reading s;
        for (const memory::Process* p : service) {
            s.service += p->privateBytes;
            if (document(*p))
                s.biggestDocumentKb = std::max(s.biggestDocumentKb, static_cast<std::uint32_t>(p->privateBytes / 1024));
        }
        m_lastService = s;
    }
    r.service = m_lastService.service; // between whole looks: as last seen
    r.biggestDocumentKb = std::max(r.biggestDocumentKb, m_lastService.biggestDocumentKb);

    if (fine) { // CPU and reads over the last second, and each process's share
        const bool first = m_fineBase.atNs == 0;
        const auto over = [nowFt](const Baseline& b) { // 100 ns since it; 0: no baseline yet
            return b.atNs == 0 ? 0.0 : static_cast<double>(std::max<std::int64_t>(1, nowFt - b.atNs));
        };
        const auto percent = [this](std::uint64_t cpu, double elapsed) {
            return elapsed <= 0 ? 0.0 : std::min(100.0, cpu * 100.0 / (elapsed * m_processors));
        };
        const auto rate = [](std::uint64_t bytes, double elapsed) { return elapsed <= 0 ? 0.0 : bytes / kMb / (elapsed / 1e7); };
        const double elapsed = over(m_fineBase);
        const double serviceElapsed = over(m_fineServiceBase);
        const Group a = since(m_fineBase, mine);
        const Group b = since(m_fineServiceBase, service);
        r.cpu = percent(a.cpu, elapsed);
        r.read = rate(a.read, elapsed);
        r.serviceCpu = percent(b.cpu, serviceElapsed);
        r.serviceRead = rate(b.read, serviceElapsed);
        if (!first) {
            m_processes.clear();
            const auto add = [&](const memory::Process* p, const QString& kind, const Baseline& base, double span) {
                const Group one = since(base, {p});
                m_processes.push_back(QVariantMap {
                    {u"name"_s, processName(*p, p->pid == look.self)},
                    {u"kind"_s, kind},
                    {u"mb"_s, p->privateBytes / kMb},
                    {u"cpu"_s, percent(one.cpu, span)},
                    {u"read"_s, rate(one.read, span)},
                });
            };
            for (const memory::Process* p : mine)
                add(p, p->pid == look.self ? u"main"_s : document(*p) ? u"document"_s : u"plugin"_s, m_fineBase, elapsed);
            for (const memory::Process* p : service)
                add(p, u"service"_s, m_fineServiceBase, serviceElapsed);
        }
        rebase(m_fineBase, mine, nowFt);
        rebase(m_fineServiceBase, service, nowFt);
        if (mapped) {
            const memory::Map& m = mapped->map;
            if (m.heaps > 0)
                m_heapSpare = m.heapSpare;
            m_mapped = std::move(mapped);
        }
    } else if (m_haveReading) { // CPU, reads: only ever the last second's
        r.cpu = m_reading.cpu;
        r.read = m_reading.read;
        r.serviceCpu = m_reading.serviceCpu;
        r.serviceRead = m_reading.serviceRead;
    }
    m_reading = r;
    m_haveReading = true;

    if (!fine) { // what happened: from the 10-second looks and pokes
        health::Facts facts = m_sources.facts ? m_sources.facts() : health::Facts {};
        m_contentReading = facts.contentReading;
        facts.biggestDocumentKb = r.biggestDocumentKb;
        m_tracker.observe(facts, nowMs, r.totalKb());
        if (m_sources.lastTrim) {
            const auto [atMs, mb] = m_sources.lastTrim();
            if (atMs != 0 && atMs != m_lastTrimSeen) {
                m_lastTrimSeen = atMs;
                m_events.instant(health::EventKind::AutoTrim, atMs, r.totalKb(), static_cast<std::uint32_t>(std::max<qint64>(0, mb)));
            }
        }
    }
    if (m_watching)
        publish();
}

void HealthMonitor::publish()
{
    if (!m_watching || !m_haveReading) {
        if (!m_now.isEmpty()) {
            m_now.clear(); // nothing kept for a page that is not there
            emit updated();
        }
        return;
    }
    const Reading& r = m_reading;
    QVariantMap now;
    now[u"total"_s] = (r.main + r.plugins + r.documents) / kMb;
    now[u"mainMb"_s] = r.main / kMb;

    // The main process told apart (memory::map, at most two seconds old,
    // scaled to the newest reading: what changed since is in the windows'
    // part). Children: plugins, document readers.
    QVariantList parts;
    const auto part = [&](const QString& key, const QString& name, double bytes, const QString& note) {
        parts.push_back(QVariantMap {{u"key"_s, key}, {u"name"_s, name}, {u"mb"_s, std::max(0.0, bytes) / kMb}, {u"note"_s, note}});
    };
    if (m_mapped) {
        const memory::Map& m = m_mapped->map;
        const double main = static_cast<double>(r.main);
        // On the heap, resident or not: the tables, the pictures. Counted no
        // further than what is resident of the heap (`other`).
        double heap = static_cast<double>(m.other);
        const double tables = std::min<double>(m_mapped->fileIndexTables, heap);
        heap -= tables;
        const double pictures = std::min<double>(m_mapped->pictures, heap);
        heap -= pictures;
        // Freed, kept by the heaps, in memory (the last walk): at most what is left.
        const double spare = std::min<double>(m_heapSpare, heap);
        heap -= spare;
        const double program = static_cast<double>(m.ownProgram + m.qt + m.graphics + m.system + m.stacks);
        const double fileIndex = static_cast<double>(m.fileIndex) + tables;
        const double windows = std::max(0.0, main - fileIndex - pictures - spare - program);
        const double parked = std::max(0.0, static_cast<double>(m_fileIndexHeld) - static_cast<double>(m.fileIndex)) / kMb;
        part(u"fileIndex"_s, tr("文件索引"), fileIndex,
            parked >= 4 ? tr("搜文件名用的；另有 %1 MB 已暂存到磁盘，用到时读回").arg(qRound(parked)) : tr("搜文件名用的"));
        part(u"windows"_s, tr("窗口和数据"), windows, tr("搜索框、设置、剪贴板历史等正在用的"));
        part(u"pictures"_s, tr("缩略图"), pictures, tr("列表和剪贴板里的小图，再看时不用重读"));
        part(u"spare"_s, tr("用过空着的"), spare, tr("刚才用过、已经不用的，归还后就没了"));
        part(u"program"_s, tr("程序本身"), program, tr("Win顺、Qt 和系统的代码，各个线程"));
    } else {
        part(u"windows"_s, tr("主程序"), static_cast<double>(r.main), tr("正在细分…"));
    }
    part(u"plugins"_s, tr("插件"), static_cast<double>(r.plugins), tr("各自一个进程，见下面"));
    part(u"documents"_s, tr("读文档"), static_cast<double>(r.documents), tr("读完 30 秒后退出"));
    now[u"parts"_s] = parts;

    now[u"cpu"_s] = r.cpu;
    now[u"read"_s] = r.read;
    now[u"serviceMb"_s] = r.service / kMb;
    now[u"serviceCpu"_s] = r.serviceCpu;
    now[u"serviceRead"_s] = r.serviceRead;
    now[u"hasService"_s] = r.service > 0;
    now[u"processes"_s] = m_processes;
    const QVariantList tasks = m_sources.tasks ? m_sources.tasks() : QVariantList();
    now[u"tasks"_s] = tasks;
    now[u"busy"_s] = !tasks.isEmpty();
    now[u"idleText"_s] = BackgroundStatus::idleText();
    const auto [atMs, freed] = m_sources.lastTrim ? m_sources.lastTrim() : std::pair<qint64, qint64>(0, 0);
    now[u"lastTrim"_s] = atMs == 0 ? tr("还没有归还过") : tr("上次归还：%1，还了 %2 MB").arg(clock(atMs)).arg(freed);
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
        });
    }
    return out;
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
    m->m_events.clear();
    const std::int64_t end = QDateTime::currentMSecsSinceEpoch();
    constexpr std::uint64_t mb = 1024 * 1024;

    Reading r;
    r.main = (idle ? 21 : 352) * mb;
    r.plugins = 13 * mb;
    r.documents = idle ? 0 : 118 * mb;
    r.service = (idle ? 9 : 104) * mb;
    r.cpu = idle ? 0.1 : 12.4;
    r.read = idle ? 0 : 38;
    r.serviceCpu = idle ? 0 : 13;
    r.serviceRead = idle ? 0 : 120;
    m->m_reading = r;
    m->m_haveReading = true;
    Mapped mapped;
    mapped.map.total = r.main;
    mapped.map.fileIndex = (idle ? 2 : 96) * mb;
    mapped.map.ownProgram = 1 * mb;
    mapped.map.qt = 2 * mb;
    mapped.map.graphics = 2 * mb;
    mapped.map.system = 3 * mb;
    mapped.map.stacks = 4 * mb;
    mapped.map.other = r.main - mapped.map.fileIndex - 12 * mb;
    mapped.map.heapHandedOut = (idle ? 180 : 300) * mb;
    mapped.map.heapSpare = (idle ? 1 : 14) * mb;
    mapped.map.heaps = 5;
    mapped.fileIndexTables = (idle ? 0 : 14) * mb;
    mapped.pictures = (idle ? 0 : 9) * mb;
    m->m_heapSpare = mapped.map.heapSpare;
    m->m_mapped = mapped;
    m->m_fileIndexHeld = 112 * mb;

    using K = health::EventKind;
    constexpr std::int64_t h = 3600 * 1000LL, mi = 60 * 1000LL;
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
    const auto proc = [&](const QString& name, const QString& kind, double mbs, double cpu) {
        processes.push_back(QVariantMap {{u"name"_s, name}, {u"kind"_s, kind}, {u"mb"_s, mbs}, {u"cpu"_s, cpu}, {u"read"_s, 0.0}});
    };
    proc(tr("Win顺 主程序"), u"main"_s, r.main / kMb, idle ? 0.1 : 12.4);
    proc(tr("插件运行环境"), u"plugin"_s, 3, 0);
    proc(tr("截图录屏"), u"plugin"_s, 6, 0);
    proc(tr("图片文字搜索"), u"plugin"_s, 4, 0);
    if (!idle)
        proc(tr("读文档"), u"document"_s, 118, 4.2);
    proc(tr("后台服务"), u"service"_s, r.service / kMb, idle ? 0 : 13.0);
    m->m_processes = processes;
    m->m_sources.tasks = [idle] {
        QVariantList tasks;
        if (!idle)
            tasks.push_back(QVariantMap {{u"name"_s, tr("正在建立内容索引")}, {u"count"_s, u"18,204 / 41,382"_s},
                {u"remaining"_s, tr("约 3–5 分钟")}, {u"fraction"_s, 0.44}});
        return tasks;
    };
    m->m_sources.lastTrim = [end] { return std::pair<qint64, qint64>(end - 2 * 3600 * 1000LL - 60'000, 132); };
    return m;
}

} // namespace ws
