#include "PreviewHost.h"

#include "WindowEffects.h"

#include <QPointer>

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

namespace ws {

namespace {

// Word, Excel and PowerPoint start their program the first time (3–6 s
// measured); a handler still busy after this counts as failed.
constexpr auto kTimeout = 10s;
constexpr UINT kShow = WM_APP + 1;
constexpr UINT kMove = WM_APP + 2;
constexpr UINT kHide = WM_APP + 3;
constexpr UINT kQuit = WM_APP + 4;
constexpr UINT_PTR kWaitChild = 1; // timer: the handler's window has not come yet
constexpr UINT_PTR kIdle = 2; // timer: no preview for a while, the class factories go
constexpr UINT kWaitChildMs = 50;
constexpr int kWaitChildTimes = 60; // 3 s: then shown as it is
constexpr UINT kIdleMs = 60'000;
constexpr wchar_t kClass[] = L"WinShun.PreviewHost";

// Handlers that hung once: not tried again while Win顺 runs (the MAPI mail
// handler without Outlook never returns from CreateInstance).
std::mutex g_hungMutex;
std::set<QUuid> g_hung;

// The host's side of the handler (IPreviewHandlerFrame), what Word asks for:
// no accelerators of ours (the keys stay in the launcher anyway: the popup
// is never active).
class Frame final : public IPreviewHandlerFrame {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPreviewHandlerFrame) {
            *out = static_cast<IPreviewHandlerFrame*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --m_refs;
        if (left == 0)
            delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE GetWindowContext(PREVIEWHANDLERFRAMEINFO* info) override
    {
        if (!info)
            return E_POINTER;
        info->haccel = nullptr;
        info->cAccelEntries = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG*) override { return S_FALSE; }

private:
    std::atomic<ULONG> m_refs {1};
};

QString registryText(const std::wstring& key, const wchar_t* value)
{
    wchar_t text[MAX_PATH * 2] {};
    DWORD size = sizeof text;
    if (::RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, text, &size)
        != ERROR_SUCCESS)
        return {};
    return QString::fromWCharArray(text);
}

// The file name of the program that serves the handler (lower case): its
// LocalServer32 ("WINWORD.EXE" -Embedding), else the surrogate its AppID
// names (prevhost.exe), else COM's own surrogate.
QString serverProgram(const QUuid& handler)
{
    const std::wstring clsid = L"CLSID\\" + handler.toString(QUuid::WithBraces).toStdWString();
    QString command = registryText(clsid + L"\\LocalServer32", nullptr);
    if (command.isEmpty()) {
        const QString appId = registryText(clsid, L"AppID");
        if (!appId.isEmpty())
            command = registryText(L"AppID\\" + appId.toStdWString(), L"DllSurrogate");
        if (command.isEmpty())
            command = u"dllhost.exe"_s;
    }
    command.remove(u'"');
    const qsizetype exe = command.indexOf(u".exe"_s, 0, Qt::CaseInsensitive);
    if (exe >= 0)
        command.truncate(exe + 4);
    command = command.mid(command.lastIndexOf(u'\\') + 1);
    return command.trimmed().toLower();
}

QString programOf(HWND window)
{
    DWORD process = 0;
    ::GetWindowThreadProcessId(window, &process);
    const HANDLE handle = process ? ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process) : nullptr;
    if (!handle)
        return {};
    wchar_t path[MAX_PATH * 2] {};
    DWORD length = MAX_PATH * 2;
    const BOOL ok = ::QueryFullProcessImageNameW(handle, 0, path, &length);
    ::CloseHandle(handle);
    if (!ok)
        return {};
    const QString full = QString::fromWCharArray(path, static_cast<qsizetype>(length));
    return full.mid(full.lastIndexOf(u'\\') + 1).toLower();
}

} // namespace

// The thread, its popup and the handler in it. Shared with the thread, which
// may outlive the PreviewHost (a handler that hangs, the program exiting).
struct PreviewHost::Worker {
    struct Request {
        quint64 ticket = 0;
        std::wstring path;
        GUID handler {};
        RECT rect {};
        HWND window = nullptr; // the preview window: the popup goes just above it
        bool dark = false;
    };

    QPointer<PreviewHost> owner; // the results go to it, queued
    std::mutex mutex;
    std::optional<Request> pending; // the latest show() not taken up yet
    std::optional<RECT> moveTo;
    bool hideWanted = false;
    std::atomic<HWND> popup {nullptr};
    std::atomic<DWORD> handlerProcess {0}; // the process the handler's window is in (prevhost.exe, WINWORD.EXE…)
    HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // On the thread.
    ComPtr<IPreviewHandler> handler;
    ComPtr<Frame> frame;
    std::map<QUuid, ComPtr<IClassFactory>> factories; // kept (locked) while previews come: Word stays up
    HWND above = nullptr; // the preview window, which the popup goes just above
    bool busy = false; // inside a call to the handler: COM's loop may bring our messages meanwhile
    COLORREF background = RGB(249, 249, 249);
    quint64 shownTicket = 0; // waiting for the handler's window (kWaitChild)
    int waits = 0;

    ~Worker() { ::CloseHandle(ready); }

    void post(UINT message)
    {
        if (const HWND hwnd = popup.load())
            ::PostMessageW(hwnd, message, 0, 0);
    }

    // To the GUI thread: how a request ended.
    void report(quint64 ticket, bool ok)
    {
        QPointer<PreviewHost> to;
        {
            std::lock_guard lock(mutex);
            to = owner;
        }
        if (!to)
            return;
        QMetaObject::invokeMethod(to, [to, ticket, ok] {
            if (!to || to->m_waiting != ticket)
                return;
            to->m_watchdog.stop();
            to->m_waiting = 0;
            if (!ok)
                emit to->failed(ticket);
        }, Qt::QueuedConnection);
    }

    void unload()
    {
        const HWND hwnd = popup.load();
        ::KillTimer(hwnd, kWaitChild);
        shownTicket = 0;
        handlerProcess = 0;
        if (!handler)
            return;
        busy = true;
        handler->Unload();
        if (ComPtr<IObjectWithSite> site; SUCCEEDED(handler.As(&site)))
            site->SetSite(nullptr);
        busy = false;
        handler.Reset();
    }

    void doShow()
    {
        if (busy)
            return; // a call to the handler is under way (COM's loop dispatched this): after it
        std::optional<Request> request;
        {
            std::lock_guard lock(mutex);
            request.swap(pending);
            hideWanted = false;
        }
        if (!request)
            return;
        const HWND hwnd = popup.load();
        ::KillTimer(hwnd, kIdle);
        ::ShowWindow(hwnd, SW_HIDE);
        unload();
        background = request->dark ? RGB(32, 32, 32) : RGB(249, 249, 249);
        const bool ok = start(*request, hwnd);
        if (!ok) {
            unload();
            report(request->ticket, false);
        } else {
            // Shown once the handler's window is there (Edge's PDF handler
            // returns at once and draws a moment later): until then the
            // preview window says it is reading, not an empty box.
            shownTicket = request->ticket;
            waits = 0;
            ::SetTimer(hwnd, kWaitChild, kWaitChildMs, nullptr);
            waitChild();
        }
        // What came in meanwhile.
        std::lock_guard lock(mutex);
        if (pending)
            ::PostMessageW(hwnd, kShow, 0, 0);
        else if (hideWanted)
            ::PostMessageW(hwnd, kHide, 0, 0);
        else if (moveTo)
            ::PostMessageW(hwnd, kMove, 0, 0);
    }

    void waitChild()
    {
        const HWND hwnd = popup.load();
        if (!shownTicket)
            return;
        const HWND child = ::GetWindow(hwnd, GW_CHILD);
        if (!child && ++waits < kWaitChildTimes)
            return;
        ::KillTimer(hwnd, kWaitChild);
        if (child) {
            DWORD process = 0;
            ::GetWindowThreadProcessId(child, &process);
            handlerProcess = process;
        }
        ::ShowWindow(hwnd, SW_SHOWNA);
        report(std::exchange(shownTicket, 0), true);
    }

    ComPtr<IClassFactory> factory(const GUID& clsid)
    {
        const QUuid key(clsid);
        if (const auto at = factories.find(key); at != factories.end())
            return at->second;
        // Out of process, as Explorer has them: in the surrogate the handler's
        // AppID names (prevhost.exe), or the program that serves it (WINWORD.EXE);
        // never inside Win顺, where one that crashes would take it along.
        ComPtr<IClassFactory> made;
        if (FAILED(::CoGetClassObject(clsid, CLSCTX_LOCAL_SERVER, nullptr, IID_PPV_ARGS(&made))))
            return nullptr;
        made->LockServer(TRUE);
        factories.emplace(key, made);
        return made;
    }

    bool start(const Request& r, HWND hwnd)
    {
        busy = true;
        const ComPtr<IClassFactory> source = factory(r.handler);
        ComPtr<IPreviewHandler> made;
        if (!source || FAILED(source->CreateInstance(nullptr, IID_PPV_ARGS(&made)))) {
            busy = false;
            return false;
        }
        if (!frame)
            frame.Attach(new Frame);
        if (ComPtr<IObjectWithSite> site; SUCCEEDED(made.As(&site)))
            site->SetSite(frame.Get());
        // Stream, file, item: whichever it takes (Word, Excel, PowerPoint and
        // Edge's PDF handler a path, the text and Adobe's PDF handlers a stream).
        bool initialized = false;
        if (ComPtr<IInitializeWithStream> init; SUCCEEDED(made.As(&init))) {
            ComPtr<IStream> stream;
            if (SUCCEEDED(::SHCreateStreamOnFileEx(r.path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE | STGM_FAILIFTHERE,
                    FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream)))
                initialized = SUCCEEDED(init->Initialize(stream.Get(), STGM_READ));
        }
        if (!initialized) {
            if (ComPtr<IInitializeWithFile> init; SUCCEEDED(made.As(&init)))
                initialized = SUCCEEDED(init->Initialize(r.path.c_str(), STGM_READ));
        }
        if (!initialized) {
            if (ComPtr<IInitializeWithItem> init; SUCCEEDED(made.As(&init))) {
                ComPtr<IShellItem> item;
                if (SUCCEEDED(::SHCreateItemFromParsingName(r.path.c_str(), nullptr, IID_PPV_ARGS(&item))))
                    initialized = SUCCEEDED(init->Initialize(item.Get(), STGM_READ));
            }
        }
        if (!initialized) {
            if (ComPtr<IObjectWithSite> site; SUCCEEDED(made.As(&site)))
                site->SetSite(nullptr);
            busy = false;
            return false;
        }
        // The theme's colours, for the few that take them (PowerPoint's);
        // the popup's own background is the theme's too.
        if (ComPtr<IPreviewHandlerVisuals> visuals; SUCCEEDED(made.As(&visuals))) {
            visuals->SetBackgroundColor(background);
            visuals->SetTextColor(r.dark ? RGB(243, 243, 243) : RGB(27, 27, 27));
        }
        above = r.window;
        place(hwnd, r.rect, r.window);
        handler = made;
        // Physical pixels, the popup's client area; never empty (PowerToys'
        // handlers put off DoPreview until it is not).
        RECT client {0, 0, std::max(1L, r.rect.right - r.rect.left), std::max(1L, r.rect.bottom - r.rect.top)};
        HRESULT hr = handler->SetWindow(hwnd, &client);
        if (SUCCEEDED(hr))
            hr = handler->DoPreview();
        busy = false;
        return SUCCEEDED(hr);
    }

    // Over the preview window's content area, just above it (and so under
    // the launcher, which is just above the preview window). Still hidden.
    // (Again on every move: the preview window itself may have gone in
    // front of the launcher meanwhile, FilePreview::showWindow.)
    static void place(HWND hwnd, const RECT& r, HWND window)
    {
        const HWND after = window ? ::GetWindow(window, GW_HWNDPREV) : nullptr;
        UINT flags = SWP_NOACTIVATE;
        if (after == hwnd)
            flags |= SWP_NOZORDER; // there already
        ::SetWindowPos(hwnd, after ? after : HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, flags);
    }

    void doMove()
    {
        if (busy)
            return; // taken up after the call (doShow)
        std::optional<RECT> rect;
        {
            std::lock_guard lock(mutex);
            rect.swap(moveTo);
        }
        const HWND hwnd = popup.load();
        if (!rect)
            return;
        place(hwnd, *rect, above);
        if (handler) {
            RECT client {0, 0, std::max(1L, rect->right - rect->left), std::max(1L, rect->bottom - rect->top)};
            busy = true;
            handler->SetRect(&client);
            busy = false;
        }
    }

    void doHide()
    {
        if (busy) {
            std::lock_guard lock(mutex);
            hideWanted = true;
            return;
        }
        const HWND hwnd = popup.load();
        ::ShowWindow(hwnd, SW_HIDE);
        unload();
        ::SetTimer(hwnd, kIdle, kIdleMs, nullptr);
    }

    void releaseFactories()
    {
        if (busy || handler)
            return;
        ::KillTimer(popup.load(), kIdle);
        busy = true;
        for (auto& [clsid, f] : factories)
            f->LockServer(FALSE);
        factories.clear();
        busy = false;
    }

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<Worker*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE; // a click in it leaves the launcher in front
        case WM_ERASEBKGND:
            if (self) {
                RECT r;
                ::GetClientRect(hwnd, &r);
                const HBRUSH brush = ::CreateSolidBrush(self->background);
                ::FillRect(reinterpret_cast<HDC>(wParam), &r, brush);
                ::DeleteObject(brush);
                return 1;
            }
            break;
        case WM_TIMER:
            if (self && wParam == kWaitChild)
                self->waitChild();
            else if (self && wParam == kIdle)
                self->releaseFactories();
            return 0;
        case kShow:
            if (self)
                self->doShow();
            return 0;
        case kMove:
            if (self)
                self->doMove();
            return 0;
        case kHide:
            if (self)
                self->doHide();
            return 0;
        case kQuit:
            if (self && !self->busy) {
                self->unload();
                self->frame.Reset();
                self->releaseFactories();
                ::DestroyWindow(hwnd);
            }
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static void run(std::shared_ptr<Worker> self)
    {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        // Per monitor, as the rest of Win顺; the handler's window (another
        // process, often system DPI aware) is a child of it all the same.
        ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        ::SetThreadDpiHostingBehavior(DPI_HOSTING_BEHAVIOR_MIXED);
        static const ATOM atom = [] {
            WNDCLASSEXW wc {sizeof wc};
            wc.lpfnWndProc = windowProc;
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kClass;
            return ::RegisterClassExW(&wc);
        }();
        // No owner, no parent: either would join this thread's input to the
        // launcher's (see the class).
        const HWND hwnd = atom ? ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kClass, L"",
                                     WS_POPUP | WS_CLIPCHILDREN, 0, 0, 0, 0, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr)
                               : nullptr;
        if (hwnd) {
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self.get()));
            win::setWindowCorners(hwnd, true); // a page inside the preview window
            self->popup.store(hwnd);
        }
        ::SetEvent(self->ready);
        if (hwnd) {
            MSG msg;
            while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
        }
        self->popup.store(nullptr);
        if (SUCCEEDED(com))
            ::CoUninitialize();
    }
};

PreviewHost::PreviewHost()
{
    m_watchdog.setSingleShot(true);
    m_watchdog.setInterval(kTimeout);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        // Still inside the handler: given up on, thread and all (a call
        // into a server that hangs cannot even be released); the next file
        // gets a new thread. Its popup goes out of sight without waiting.
        const quint64 ticket = std::exchange(m_waiting, 0);
        if (m_worker) {
            if (const HWND hwnd = m_worker->popup.load())
                ::ShowWindowAsync(hwnd, SW_HIDE);
            {
                std::lock_guard lock(m_worker->mutex);
                m_worker->owner = nullptr;
                m_worker->pending.reset();
            }
            m_worker->post(kQuit); // taken up once (if ever) the handler returns
            m_worker.reset();
        }
        if (!m_handler.isNull()) {
            std::lock_guard lock(g_hungMutex);
            g_hung.insert(m_handler);
        }
        if (ticket)
            emit failed(ticket);
    });
}

PreviewHost::~PreviewHost()
{
    if (m_worker) {
        {
            std::lock_guard lock(m_worker->mutex);
            m_worker->owner = nullptr;
        }
        m_worker->post(kQuit); // not waited for
    }
}

std::shared_ptr<PreviewHost::Worker> PreviewHost::worker()
{
    if (m_worker && m_worker->popup.load())
        return m_worker;
    auto made = std::make_shared<Worker>();
    made->owner = this;
    std::thread(&Worker::run, made).detach();
    ::WaitForSingleObject(made->ready, 2000); // a window made, no more
    if (!made->popup.load())
        return nullptr;
    m_worker = made;
    return m_worker;
}

std::optional<QUuid> PreviewHost::handlerFor(const QString& path)
{
    const qsizetype dot = path.lastIndexOf(u'.');
    if (dot < 0 || path.indexOf(u'\\', dot) >= 0 || path.indexOf(u'/', dot) >= 0)
        return std::nullopt;
    const std::wstring extension = path.mid(dot).toStdWString();
    wchar_t clsid[64] {};
    DWORD length = 64;
    // IPreviewHandler's shellex key as Explorer finds it: the type's ProgID
    // (the user's choice) first, then the extension, its perceived type,
    // SystemFileAssociations, the user's own handlers (HKCU) among them.
    if (::AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_SHELLEXTENSION, extension.c_str(),
            L"{8895b1c6-b41f-4c1c-a562-0d564250836f}", clsid, &length)
        != S_OK)
        return std::nullopt;
    GUID guid;
    if (FAILED(::CLSIDFromString(clsid, &guid)))
        return std::nullopt;
    const QUuid handler(guid);
    std::lock_guard lock(g_hungMutex);
    if (g_hung.contains(handler))
        return std::nullopt;
    return handler;
}

void PreviewHost::show(quint64 ticket, const QString& path, const QUuid& handler, int left, int top, int right,
    int bottom, HWND window, bool dark)
{
    const std::shared_ptr<Worker> w = worker();
    if (!w) {
        QMetaObject::invokeMethod(this, [this, ticket] { emit failed(ticket); }, Qt::QueuedConnection);
        return;
    }
    {
        std::lock_guard lock(w->mutex);
        Worker::Request r;
        r.ticket = ticket;
        r.path = QString(path).replace(u'/', u'\\').toStdWString();
        r.handler = static_cast<GUID>(handler);
        r.rect = RECT {left, top, right, bottom};
        r.window = window;
        r.dark = dark;
        w->pending = std::move(r);
        w->moveTo.reset();
    }
    m_waiting = ticket;
    m_handler = handler;
    m_since.start();
    m_server = serverProgram(handler);
    m_watchdog.start();
    w->post(kShow);
}

void PreviewHost::move(int left, int top, int right, int bottom)
{
    if (!m_worker)
        return;
    {
        std::lock_guard lock(m_worker->mutex);
        if (m_worker->pending) { // not shown yet: there, when it is
            m_worker->pending->rect = RECT {left, top, right, bottom};
            return;
        }
        m_worker->moveTo = RECT {left, top, right, bottom};
    }
    m_worker->post(kMove);
}

void PreviewHost::hide()
{
    m_watchdog.stop();
    m_waiting = 0;
    if (!m_worker)
        return;
    {
        std::lock_guard lock(m_worker->mutex);
        m_worker->pending.reset();
        m_worker->moveTo.reset();
    }
    if (const HWND hwnd = m_worker->popup.load())
        ::ShowWindowAsync(hwnd, SW_HIDE); // at once, whatever the thread is doing
    m_worker->post(kHide);
}

std::optional<QRect> PreviewHost::visibleRect() const
{
    const HWND popup = m_worker ? m_worker->popup.load() : nullptr;
    RECT rect {};
    if (!popup || !::IsWindowVisible(popup) || !::GetWindowRect(popup, &rect)) return {};
    return QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
}

bool PreviewHost::owns(HWND window) const
{
    if (!m_worker || !window)
        return false;
    const HWND popup = m_worker->popup.load();
    if (popup && (window == popup || ::GetAncestor(window, GA_ROOT) == popup))
        return true;
    // A window of the handler's program in front just after a file was
    // given to it (Word, say, taking the focus as it starts): the preview's
    // too. Not later: the user's own Word window, clicked, is Word's.
    if (!m_since.isValid() || m_since.elapsed() > 8000)
        return false;
    DWORD process = 0;
    ::GetWindowThreadProcessId(window, &process);
    if (process != 0 && process == m_worker->handlerProcess.load())
        return true;
    // Before its window is there (Excel shows its own for a moment as it
    // starts): by the program's name.
    return !m_server.isEmpty() && programOf(window) == m_server;
}

} // namespace ws
