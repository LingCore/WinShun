#include "OnnxRuntime.h"

#include <QDir>

#include <windows.h>

#include <algorithm>
#include <mutex>

using namespace Qt::StringLiterals;

namespace ws::onnx {

namespace {

// ONNX Runtime's threads, below normal priority as the readers' own.
struct ThreadStart {
    OrtThreadWorkerFn work;
    void* param;
};

DWORD WINAPI threadMain(void* p)
{
    const ThreadStart start = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    start.work(start.param);
    return 0;
}

OrtCustomThreadHandle createThread(void*, OrtThreadWorkerFn work, void* param)
{
    auto* start = new ThreadStart {work, param};
    HANDLE thread = ::CreateThread(nullptr, 0, threadMain, start, CREATE_SUSPENDED, nullptr);
    if (!thread) {
        delete start;
        return nullptr;
    }
    ::SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
    ::ResumeThread(thread);
    return reinterpret_cast<OrtCustomThreadHandle>(thread);
}

void joinThread(OrtCustomThreadHandle handle)
{
    HANDLE thread = const_cast<HANDLE>(reinterpret_cast<const void*>(handle));
    ::WaitForSingleObject(thread, INFINITE);
    ::CloseHandle(thread);
}

} // namespace

bool load(const QString& folder, QString* error)
{
    static std::mutex mutex;
    static const OrtApi* api = nullptr;
    static QString failure;
    const std::lock_guard lock(mutex);
    if (!api && failure.isEmpty()) {
        const QString dll = QDir::toNativeSeparators(folder + u"/onnxruntime.dll"_s);
        HMODULE module = ::LoadLibraryExW(reinterpret_cast<const wchar_t*>(dll.utf16()), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
        const auto getBase = module ? reinterpret_cast<GetApiBase>(::GetProcAddress(module, "OrtGetApiBase")) : nullptr;
        const OrtApiBase* base = getBase ? getBase() : nullptr;
        api = base ? base->GetApi(ORT_API_VERSION) : nullptr;
        if (api)
            Ort::InitApi(api);
        else
            failure = module ? u"onnxruntime.dll is not version %1 or later"_s.arg(ORT_API_VERSION)
                             : u"onnxruntime.dll did not load (error %1)"_s.arg(::GetLastError());
    }
    if (!api && error)
        *error = failure;
    return api;
}

Ort::Env environment(int threads)
{
    Ort::ThreadingOptions threading;
    threading.SetGlobalIntraOpNumThreads(std::max(1, threads))
        .SetGlobalInterOpNumThreads(1)
        .SetGlobalSpinControl(0)
        .SetGlobalCustomCreateThreadFn(createThread)
        .SetGlobalCustomJoinThreadFn(joinThread);
    return Ort::Env(threading, ORT_LOGGING_LEVEL_ERROR, "WinShun");
}

Ort::SessionOptions sessionOptions()
{
    Ort::SessionOptions options;
    options.DisablePerSessionThreads()
        .SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL)
        .DisableCpuMemArena() // memory back after each picture
        // A planned block per input size is ~135 MB more than the text
        // finder's own peak when a tall picture's strips repeat a size
        // (measured on 2000x4000..3200x12500), and no faster.
        .DisableMemPattern();
    return options;
}

Ort::Session session(const Ort::Env& env, const QString& file, const Ort::SessionOptions& options)
{
    const QString native = QDir::toNativeSeparators(file);
    return Ort::Session(env, reinterpret_cast<const wchar_t*>(native.utf16()), options);
}

} // namespace ws::onnx
