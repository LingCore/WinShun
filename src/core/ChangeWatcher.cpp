#include "ChangeWatcher.h"

#include "Win32Util.h"

#include <windows.h>

#include <algorithm>
#include <memory>

namespace ws {

namespace {

constexpr DWORD kBufferBytes = 64 * 1024;
constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;

struct Watch {
    std::wstring root;
    win32::UniqueHandle dir;
    win32::UniqueHandle event;
    OVERLAPPED overlapped {};
    std::unique_ptr<DWORD[]> buffer; // DWORD-aligned, as the API requires
    bool pending = false;

    bool arm()
    {
        ::ResetEvent(event.get());
        overlapped = OVERLAPPED {};
        overlapped.hEvent = event.get();
        pending = ::ReadDirectoryChangesW(
            dir.get(), buffer.get(), kBufferBytes, TRUE, kNotifyFilter, nullptr, &overlapped, nullptr);
        return pending;
    }

    void cancel()
    {
        if (!pending)
            return;
        ::CancelIoEx(dir.get(), &overlapped);
        DWORD ignored = 0;
        ::GetOverlappedResult(dir.get(), &overlapped, &ignored, TRUE);
        pending = false;
    }
};

void parse(const Watch& w, DWORD bytes, std::vector<FsChange>& out)
{
    const auto* base = reinterpret_cast<const std::byte*>(w.buffer.get());
    const std::byte* p = base;
    for (;;) {
        if (p + sizeof(FILE_NOTIFY_INFORMATION) > base + bytes)
            break;
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
        const std::wstring_view rel(info->FileName, info->FileNameLength / sizeof(wchar_t));
        FsChange::Kind kind {};
        bool known = true;
        switch (info->Action) {
        case FILE_ACTION_ADDED:
            kind = FsChange::Kind::Added;
            break;
        case FILE_ACTION_REMOVED:
            kind = FsChange::Kind::Removed;
            break;
        case FILE_ACTION_RENAMED_OLD_NAME:
            kind = FsChange::Kind::RenamedFrom;
            break;
        case FILE_ACTION_RENAMED_NEW_NAME:
            kind = FsChange::Kind::RenamedTo;
            break;
        default:
            known = false;
            break;
        }
        if (known && !rel.empty())
            out.push_back({kind, w.root + L'\\' + std::wstring(rel)});
        if (info->NextEntryOffset == 0)
            break;
        p += info->NextEntryOffset;
    }
}

std::unique_ptr<Watch> openWatch(const std::wstring& root)
{
    auto w = std::make_unique<Watch>();
    w->root = root;
    const std::wstring dirPath = root + L'\\';
    w->dir.reset(::CreateFileW(dirPath.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr));
    if (!w->dir.valid())
        return nullptr;
    w->event.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    w->buffer = std::make_unique<DWORD[]>(kBufferBytes / sizeof(DWORD));
    return w->arm() ? std::move(w) : nullptr;
}

} // namespace

ChangeWatcher::ChangeWatcher(Handler handler)
    : m_handler(std::move(handler))
    , m_wake(::CreateEventW(nullptr, FALSE, FALSE, nullptr))
{
    m_thread = std::jthread([this](std::stop_token stop) { run(stop); });
}

ChangeWatcher::~ChangeWatcher()
{
    m_thread.request_stop();
    if (m_thread.joinable())
        m_thread.join();
    if (m_wake)
        ::CloseHandle(m_wake);
}

void ChangeWatcher::setRoots(std::vector<std::wstring> roots)
{
    std::unique_lock lock(m_mutex);
    m_wanted = std::move(roots);
    const std::uint64_t ticket = ++m_requested;
    ::SetEvent(m_wake);
    m_applied.wait(lock, [&] { return m_done >= ticket; });
}

std::vector<std::wstring> ChangeWatcher::roots() const
{
    std::lock_guard lock(m_mutex);
    return m_watching;
}

void ChangeWatcher::run(std::stop_token stop)
{
    win32::UniqueHandle stopEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    std::stop_callback onStop(stop, [h = stopEvent.get()] { ::SetEvent(h); });

    std::vector<std::unique_ptr<Watch>> watches; // stable addresses: OVERLAPPED must not move
    const auto publish = [&](std::uint64_t done) {
        {
            std::lock_guard lock(m_mutex);
            m_watching.clear();
            for (const auto& w : watches)
                m_watching.push_back(w->root);
            m_done = std::max(m_done, done);
        }
        m_applied.notify_all();
    };
    // Brings the watches in line with m_wanted.
    const auto apply = [&] {
        std::vector<std::wstring> wanted;
        std::uint64_t ticket = 0;
        {
            std::lock_guard lock(m_mutex);
            wanted = m_wanted;
            ticket = m_requested;
        }
        std::erase_if(watches, [&](const std::unique_ptr<Watch>& w) {
            if (std::find(wanted.begin(), wanted.end(), w->root) != wanted.end())
                return false;
            w->cancel(); // then the handle closes: the volume can go
            return true;
        });
        for (const auto& root : wanted) {
            const bool watched = std::any_of(watches.begin(), watches.end(), [&](const auto& w) { return w->root == root; });
            if (!watched) {
                if (auto w = openWatch(root))
                    watches.push_back(std::move(w));
            }
        }
        publish(ticket);
    };

    while (!stop.stop_requested()) {
        std::vector<HANDLE> handles;
        handles.reserve(watches.size() + 2);
        handles.push_back(stopEvent.get());
        handles.push_back(m_wake);
        for (const auto& w : watches)
            handles.push_back(w->event.get());

        const DWORD r = ::WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED)
            break;
        if (r == WAIT_OBJECT_0 + 1) {
            apply();
            continue;
        }
        const std::size_t idx = r - WAIT_OBJECT_0 - 2;
        if (idx >= watches.size())
            break;

        Watch& w = *watches[idx];
        DWORD bytes = 0;
        const BOOL ok = ::GetOverlappedResult(w.dir.get(), &w.overlapped, &bytes, FALSE);
        w.pending = false;

        std::vector<FsChange> changes;
        if (!ok || bytes == 0)
            changes.push_back({FsChange::Kind::Overflow, w.root}); // buffer overflowed: events were lost
        else
            parse(w, bytes, changes);

        // Re-arm before handing the batch over so no events slip through.
        if (!w.arm()) {
            watches.erase(watches.begin() + static_cast<std::ptrdiff_t>(idx)); // volume went away
            publish(0);
        }

        if (!changes.empty() && m_handler)
            m_handler(std::move(changes));
    }

    for (auto& w : watches)
        w->cancel();
    watches.clear();
    {
        std::lock_guard lock(m_mutex);
        m_done = ~std::uint64_t {0}; // nobody waits for this thread any more
    }
    m_applied.notify_all();
}

} // namespace ws
