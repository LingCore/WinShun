// GitHub #435: does a job's UI restriction stop GetClipboardData of a format
// the owner renders only when asked (Explorer's files)? jobclip <hex mask>...
// The parent owns the clipboard with a delayed CF_HDROP (marked: no history,
// no monitors), a child in a job with each mask reads it. The clipboard's
// text is put back after.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_rendered = 0;

static HGLOBAL makeDrop()
{
    static const wchar_t file[] = L"C:\\Windows\\win.ini\0";
    const SIZE_T size = sizeof(DROPFILES) + sizeof(file) + sizeof(wchar_t);
    HGLOBAL g = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
    auto* d = static_cast<DROPFILES*>(::GlobalLock(g));
    d->pFiles = sizeof(DROPFILES);
    d->fWide = TRUE;
    memcpy(reinterpret_cast<char*>(d) + sizeof(DROPFILES), file, sizeof(file));
    ::GlobalUnlock(g);
    return g;
}

static HGLOBAL dword(DWORD v)
{
    HGLOBAL g = ::GlobalAlloc(GMEM_MOVEABLE, sizeof v);
    *static_cast<DWORD*>(::GlobalLock(g)) = v;
    ::GlobalUnlock(g);
    return g;
}

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_RENDERFORMAT && wp == CF_HDROP) {
        ::SetClipboardData(CF_HDROP, makeDrop());
        ++g_rendered;
        return 0;
    }
    if (m == WM_RENDERALLFORMATS)
        return 0; // nothing kept once we are gone
    return ::DefWindowProcW(w, m, wp, lp);
}

static int child()
{
    if (!::OpenClipboard(nullptr)) {
        std::printf("    child: OpenClipboard failed %lu\n", ::GetLastError());
        return 2;
    }
    const BOOL available = ::IsClipboardFormatAvailable(CF_HDROP);
    ::SetLastError(0);
    const HANDLE first = ::GetClipboardData(CF_HDROP);
    const DWORD firstError = ::GetLastError();
    ::SetLastError(0);
    const HANDLE h = first ? first : ::GetClipboardData(CF_HDROP);
    const DWORD error = ::GetLastError();
    std::printf("    child: first %s (error %lu)\n", first ? "ok" : "NULL", firstError);
    wchar_t path[MAX_PATH] = L"";
    if (h)
        ::DragQueryFileW(static_cast<HDROP>(h), 0, path, MAX_PATH);
    ::CloseClipboard();
    std::printf("    child: available %d, data %s, error %lu, %ls\n", available, h ? "ok" : "NULL", error, path);
    return h ? 0 : 1;
}

static bool own(HWND window)
{
    for (int i = 0; i < 20; ++i) {
        if (::OpenClipboard(window))
            return true;
        ::Sleep(20);
    }
    return false;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc > 1 && std::wstring(argv[1]) == L"child")
        return child();
    WNDCLASSW type {};
    type.lpfnWndProc = proc;
    type.hInstance = ::GetModuleHandleW(nullptr);
    type.lpszClassName = L"jobclip";
    ::RegisterClassW(&type);
    HWND window = ::CreateWindowExW(0, type.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, type.hInstance, nullptr);
    const UINT exclude = ::RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    const UINT history = ::RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    const UINT cloud = ::RegisterClipboardFormatW(L"CanUploadToCloudClipboard");

    std::wstring savedText;
    bool hadText = false;
    if (own(window)) {
        if (const HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* p = static_cast<const wchar_t*>(::GlobalLock(h))) {
                savedText = p;
                hadText = true;
                ::GlobalUnlock(h);
            }
        }
        ::CloseClipboard();
    }

    wchar_t self[MAX_PATH];
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    for (int i = 1; i < argc; ++i) {
        const bool immediate = argv[i][0] == L'i';
        const DWORD mask = std::wcstoul(argv[i] + (immediate ? 1 : 0), nullptr, 16);
        if (!own(window)) {
            std::printf("mask 0x%02lx: could not open the clipboard\n", mask);
            continue;
        }
        ::EmptyClipboard();
        ::SetClipboardData(exclude, dword(0));
        ::SetClipboardData(history, dword(0));
        ::SetClipboardData(cloud, dword(0));
        ::SetClipboardData(CF_HDROP, immediate ? makeDrop() : nullptr); // else rendered when asked
        ::CloseClipboard();
        g_rendered = 0;

        HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_BASIC_UI_RESTRICTIONS ui {mask};
        if (mask && !::SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof ui))
            std::printf("mask 0x%02lx: SetInformationJobObject failed %lu\n", mask, ::GetLastError());
        std::wstring command = L"\"" + std::wstring(self) + L"\" child";
        STARTUPINFOW si {sizeof si};
        PROCESS_INFORMATION pi {};
        if (!::CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
            std::printf("mask 0x%02lx: CreateProcess failed %lu\n", mask, ::GetLastError());
            continue;
        }
        ::AssignProcessToJobObject(job, pi.hProcess);
        std::printf("mask 0x%02lx%s:\n", mask, immediate ? " (data put at once)" : " (rendered when asked)");
        std::fflush(stdout);
        ::ResumeThread(pi.hThread);
        for (;;) {
            const DWORD r = ::MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, 10000, QS_ALLINPUT);
            if (r == WAIT_OBJECT_0 || r == WAIT_TIMEOUT)
                break;
            MSG msg;
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                ::DispatchMessageW(&msg);
        }
        DWORD code = 0;
        ::GetExitCodeProcess(pi.hProcess, &code);
        std::printf("    exit %lu, rendered %d\n", code, g_rendered);
        std::fflush(stdout);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(job);
    }

    if (own(window)) {
        ::EmptyClipboard();
        ::SetClipboardData(exclude, dword(0));
        ::SetClipboardData(history, dword(0));
        ::SetClipboardData(cloud, dword(0));
        if (hadText) {
            const SIZE_T size = (savedText.size() + 1) * sizeof(wchar_t);
            HGLOBAL g = ::GlobalAlloc(GMEM_MOVEABLE, size);
            memcpy(::GlobalLock(g), savedText.c_str(), size);
            ::GlobalUnlock(g);
            ::SetClipboardData(CF_UNICODETEXT, g);
        }
        ::CloseClipboard();
    }
    ::DestroyWindow(window);
    return 0;
}
