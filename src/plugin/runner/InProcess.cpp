#include "InProcess.h"

#include "Tools.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace ws::plugin::inprocess {
namespace {

// Qt's QCoreApplication may come and go on one thread, not on a new one each
// time: the first thread that uses Qt stays its main thread. One thread,
// kept for the life of the host, runs every start of the Qt plugin in turn.
class QtLane {
public:
    static QtLane& get()
    {
        static QtLane* lane = new QtLane; // never ends: its thread outlives everything
        return *lane;
    }
    void post(std::function<void()> job)
    {
        {
            const std::lock_guard lock(m_mutex);
            m_jobs.push_back(std::move(job));
        }
        m_wake.notify_one();
    }

private:
    QtLane()
    {
        std::thread([this] {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock lock(m_mutex);
                    m_wake.wait(lock, [this] { return !m_jobs.empty(); });
                    job = std::move(m_jobs.front());
                    m_jobs.pop_front();
                }
                job();
            }
        }).detach();
    }
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::function<void()>> m_jobs;
};

std::wstring fullPath(const std::wstring& path)
{
    std::wstring full(32768, L'\0');
    const DWORD n = ::GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (!n || n >= full.size())
        return {};
    full.resize(n);
    return full;
}

std::wstring folderOf(const std::wstring& path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

using SwitchesServe = int(__cdecl*)(HANDLE, HANDLE, HANDLE);

// WinShunSwitchesApp.dll beside the plugin's program, loaded once and kept
// (Qt is never unloaded); its Qt DLLs from Win顺's folder (WS_APP_DIR, as
// WinShunSwitches.exe finds them; installed, that is the host's own).
SwitchesServe switchesEntry(const std::wstring& exe, const std::wstring& appDir, std::string* problem)
{
    static std::mutex mutex;
    static SwitchesServe entry = nullptr;
    const std::lock_guard lock(mutex);
    if (entry)
        return entry;
    // Win顺 asks this only of the copy installed with it (Host.cpp,
    // inProcessKind); the runner starts any program Win顺 names anyway.
    const std::wstring program = fullPath(exe);
    if (program.empty()) {
        *problem = "bad path: " + lite::utf8(exe);
        return nullptr;
    }
    const std::wstring dll = folderOf(program) + L"\\WinShunSwitchesApp.dll";
    if (!appDir.empty())
        ::AddDllDirectory(appDir.c_str()); // only for loads that ask for it (LOAD_LIBRARY_SEARCH_USER_DIRS)
    const HMODULE module = ::LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
        *problem = "LoadLibrary " + lite::utf8(dll) + " error " + std::to_string(::GetLastError());
        return nullptr;
    }
    entry = reinterpret_cast<SwitchesServe>(::GetProcAddress(module, "winshunSwitchesServe"));
    if (!entry)
        *problem = "no winshunSwitchesServe in " + lite::utf8(dll);
    return entry;
}

} // namespace

bool known(const std::string& kind)
{
    return kind == "tools" || kind == "switches";
}

std::shared_ptr<Run> start(const std::string& kind, const std::wstring& exe, const std::wstring& appDir, HANDLE in, HANDLE out,
    HANDLE err, std::string* problem)
{
    const auto closeEnds = [in, out, err] {
        ::CloseHandle(in);
        ::CloseHandle(out);
        ::CloseHandle(err);
    };
    auto run = std::make_shared<Run>();
    const auto finish = [run, closeEnds](DWORD code) {
        closeEnds(); // its reader sees the end of its output
        run->code = code;
        ::SetEvent(run->done.get());
    };
    if (kind == "tools") {
        std::thread([finish, in, out] {
            DWORD code = 3;
            try {
                code = static_cast<DWORD>(tools::serve(in, out));
            } catch (...) {
            }
            finish(code);
        }).detach();
        return run;
    }
    if (kind == "switches") {
        const SwitchesServe entry = switchesEntry(exe, appDir, problem);
        if (!entry) {
            closeEnds();
            return nullptr;
        }
        QtLane::get().post([finish, entry, in, out, err] {
            DWORD code = 3;
            try {
                code = static_cast<DWORD>(entry(in, out, err));
            } catch (...) {
            }
            finish(code);
        });
        return run;
    }
    *problem = "unknown: " + kind;
    closeEnds();
    return nullptr;
}

} // namespace ws::plugin::inprocess
