#include "Aside.h"

#include "Crawler.h"
#include "Visibility.h"
#include "Win32Util.h"

#include <windows.h>

#include <algorithm>
#include <string>

namespace ws::mcp {

AsideWorkers::AsideWorkers(int threads)
    : m_running(static_cast<std::size_t>(std::max(1, threads)), std::stop_source(std::nostopstate))
{
    m_threads.reserve(m_running.size());
    for (std::size_t slot = 0; slot < m_running.size(); ++slot)
        m_threads.emplace_back([this, slot] { work(slot); });
}

AsideWorkers::~AsideWorkers()
{
    std::vector<std::stop_source> stops;
    {
        const std::lock_guard lock(m_mutex);
        m_quitting = true;
        for (const Item& item : m_queue)
            stops.push_back(item.stop);
        stops.insert(stops.end(), m_running.begin(), m_running.end());
    }
    m_wake.notify_all();
    // Every job told at once (a check on a share that went away may take
    // seconds to give up), and only then the threads waited for.
    for (std::stop_source& stop : stops)
        stop.request_stop();
    m_threads.clear();
}

std::stop_source AsideWorkers::post(Job job)
{
    Item item {std::stop_source(), std::move(job)};
    std::stop_source stop = item.stop;
    {
        const std::lock_guard lock(m_mutex);
        m_queue.push_back(std::move(item));
    }
    m_wake.notify_one();
    return stop;
}

void AsideWorkers::work(std::size_t slot)
{
    for (;;) {
        Item item;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_quitting || !m_queue.empty(); });
            if (m_quitting)
                return;
            item = std::move(m_queue.front());
            m_queue.pop_front();
            if (item.stop.stop_requested())
                continue; // cancelled while it waited: never run, and let go of here
            m_running[slot] = item.stop;
        }
        item.job(item.stop.get_token());
        {
            const std::lock_guard lock(m_mutex);
            m_running[slot] = std::stop_source(std::nostopstate);
        }
        // `item`, and all its job held, goes here: on this thread.
    }
}

QuietIo::QuietIo()
    : QuietIo(win32::userAway())
{
}

QuietIo::QuietIo(bool userAway)
    : m_away(userAway)
{
}

QuietIo::~QuietIo()
{
    if (m_background)
        ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
}

void QuietIo::at(const QString& path)
{
    // A network path goes at the network's pace, not a disk's of this computer.
    const bool quiet = !m_away && diskOf(path) == Disk::Seeks;
    if (quiet == m_background)
        return;
    // Begun on a thread in background mode already, it fails: only on a change.
    if (quiet)
        m_background = ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE;
    else if (::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_END))
        m_background = false;
}

bool AsideContext::sees(const QString& path)
{
    quiet.at(path);
    return view.sees(path);
}

bool AsideContext::canList(const QString& folder)
{
    quiet.at(folder);
    return view.canList(folder);
}

bool AsideContext::canRead(const QString& file)
{
    quiet.at(file);
    return view.canRead(file);
}

bool AsideContext::canReadAttributes(const QString& path)
{
    quiet.at(path);
    return view.canReadAttributes(path);
}

void AsideContext::seeAhead(const QStringList& paths)
{
    // Fewer new folders than this go one at a time: a millisecond at most.
    // An AI client's hundred items are checked at once too, the spare rows
    // the engine gives past them with them (for "a": 110 checks, not 101,
    // in 0.5 ms, not 2.5).
    constexpr qsizetype kAtOnce = 32;
    if (!pool || paths.size() < kAtOnce)
        return;
    QStringList folders;
    folders.reserve(paths.size());
    for (const QString& path : paths) {
        if (quiet.solid(path))
            folders.push_back(Visibility::parentOf(path));
    }
    view.checkAhead(folders, *pool, stop, kAtOnce);
}

bool QuietIo::solid(const QString& path)
{
    return diskOf(path) == Disk::Solid;
}

QuietIo::Disk QuietIo::diskOf(const QString& path)
{
    if (path.size() < 2 || path[1] != u':' || path[0].unicode() >= 128 || !path[0].isLetter())
        return Disk::Other;
    const char16_t drive = path[0].toUpper().unicode();
    if (const auto it = m_disks.find(drive); it != m_disks.end())
        return it->second;
    const std::wstring root {static_cast<wchar_t>(drive), L':'};
    // placementOf counts a volume it cannot ask as one that seeks: only the
    // computer's own fixed drives are asked (a mapped network drive is not one).
    Disk disk = Disk::Other;
    if (::GetDriveTypeW((root + L'\\').c_str()) == DRIVE_FIXED)
        disk = placementOf(root).seeks ? Disk::Seeks : Disk::Solid;
    m_disks.emplace(drive, disk);
    return disk;
}

} // namespace ws::mcp
