#include "ChangeWatcher.h"

#include "Win32Util.h"

#include <windows.h>

#include <algorithm>
#include <memory>
#include <string_view>
#include <unordered_set>

namespace ws {

namespace {

constexpr std::size_t kBufferBytes = 64 * 1024;
constexpr std::size_t kVolumeBufferBytes = 1024 * 1024; // GitHub #52: lost events cost a walk
constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
constexpr DWORD kWriteFilter = FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE; // GitHub #114

struct Watch {
    std::wstring root;
    win32::UniqueHandle dir;
    win32::UniqueHandle event;
    OVERLAPPED overlapped {};
    std::unique_ptr<DWORD[]> buffer; // DWORD-aligned, as the API requires
    DWORD bytes = 0;
    DWORD filter = kNotifyFilter;
    bool pending = false;

    void resize(std::size_t size)
    {
        bytes = static_cast<DWORD>(size);
        buffer = std::make_unique_for_overwrite<DWORD[]>(size / sizeof(DWORD));
    }

    bool arm()
    {
        ::ResetEvent(event.get());
        overlapped = OVERLAPPED {};
        overlapped.hEvent = event.get();
        pending = ::ReadDirectoryChangesW(
            dir.get(), buffer.get(), bytes, TRUE, filter, nullptr, &overlapped, nullptr);
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
    const bool writes = (w.filter & kWriteFilter) != 0;
    // A file written in many pieces, its size and its time apart: once.
    std::unordered_set<std::wstring_view> written;
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
        case FILE_ACTION_MODIFIED:
            kind = FsChange::Kind::Modified;
            known = writes && written.insert(rel).second;
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

std::unique_ptr<Watch> openWatch(const std::wstring& root, bool writes)
{
    auto w = std::make_unique<Watch>();
    w->root = root;
    w->filter = writes ? kNotifyFilter | kWriteFilter : kNotifyFilter;
    const std::wstring dirPath = root + L'\\';
    w->dir.reset(::CreateFileW(dirPath.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr));
    if (!w->dir.valid())
        return nullptr;
    w->event.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    for (std::size_t bytes = ChangeWatcher::bufferBytes(root);; bytes = kBufferBytes) {
        w->resize(bytes);
        if (w->arm())
            return w;
        // Never less than before: a drive letter that leads to the network
        // after all (subst of a share, say) takes no more than 64 KB
        // (ERROR_INVALID_PARAMETER), and memory to lock may run short.
        if (bytes == kBufferBytes)
            return nullptr;
    }
}

} // namespace

ChangeWatcher::ChangeWatcher(Handler handler, Tell tell)
    : m_handler(std::move(handler))
    , m_tell(tell)
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

std::size_t ChangeWatcher::bufferBytes(const std::wstring& root)
{
    // GetDriveTypeW reads the drive letters this process sees: no disk is touched.
    const bool volume = root.size() == 2 && root[1] == L':' && ::GetDriveTypeW((root + L'\\').c_str()) != DRIVE_REMOTE;
    return volume ? kVolumeBufferBytes : kBufferBytes;
}

std::size_t ChangeWatcher::bufferAfter(std::size_t bytes, bool completed, unsigned long error)
{
    return !completed && error != ERROR_NOTIFY_ENUM_DIR && bytes > kBufferBytes ? kBufferBytes : bytes;
}

void ChangeWatcher::setRoots(std::vector<std::wstring> roots, std::vector<Fallback> instead)
{
    std::unique_lock lock(m_mutex);
    m_wanted = std::move(roots);
    m_instead = std::move(instead);
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
    // Brings the watches in line with m_wanted (and m_instead).
    const auto apply = [&] {
        std::vector<std::wstring> wanted;
        std::vector<Fallback> instead;
        std::uint64_t ticket = 0;
        {
            std::lock_guard lock(m_mutex);
            wanted = m_wanted;
            instead = m_instead;
            ticket = m_requested;
        }
        // What is not asked for any more goes first, before anything is
        // opened (which may take a while on a share): a volume being ejected
        // is let go of at once.
        const auto asked = [&](const std::wstring& path) {
            const auto wantedRoot = [&](const std::wstring& r) { return std::find(wanted.begin(), wanted.end(), r) != wanted.end(); };
            return wantedRoot(path) || std::any_of(instead.begin(), instead.end(), [&](const Fallback& f) {
                return wantedRoot(f.root) && std::find(f.folders.begin(), f.folders.end(), path) != f.folders.end();
            });
        };
        std::erase_if(watches, [&](const std::unique_ptr<Watch>& w) {
            if (asked(w->root))
                return false;
            w->cancel(); // then the handle closes: the volume can go
            return true;
        });
        // In the order wanted, those watched already kept as they are.
        std::vector<std::unique_ptr<Watch>> next;
        const auto take = [&](const std::wstring& path) {
            if (std::any_of(next.begin(), next.end(), [&](const auto& w) { return w->root == path; }))
                return true; // named twice
            if (next.size() >= kMaxWatches)
                return false;
            for (auto& w : watches) {
                if (w && w->root == path) {
                    next.push_back(std::move(w));
                    return true;
                }
            }
            if (auto w = openWatch(path, m_tell == Tell::Writes)) {
                next.push_back(std::move(w));
                return true;
            }
            return false;
        };
        std::vector<std::wstring> failed;
        for (const auto& root : wanted) {
            if (!take(root))
                failed.push_back(root);
        }
        for (const auto& root : failed) {
            for (const Fallback& f : instead) {
                if (f.root != root)
                    continue;
                for (const std::wstring& folder : f.folders)
                    take(folder);
            }
        }
        for (auto& w : watches) {
            if (w)
                w->cancel(); // folders no longer in a root's place, or past kMaxWatches
        }
        watches = std::move(next);
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
        const DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
        w.pending = false;

        std::vector<FsChange> changes;
        if (!ok || bytes == 0)
            changes.push_back({FsChange::Kind::Overflow, w.root}); // buffer overflowed: events were lost
        else
            parse(w, bytes, changes);

        // GitHub #52: a big buffer whose read failed, 64 KB from now on
        // (bufferAfter); one that cannot be armed again, too (as openWatch).
        if (const std::size_t next = bufferAfter(w.bytes, ok, error); next != w.bytes)
            w.resize(next);
        // Re-arm before handing the batch over so no events slip through.
        bool armed = w.arm();
        if (!armed && w.bytes > kBufferBytes) {
            w.resize(kBufferBytes);
            armed = w.arm();
        }
        if (!armed) {
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
