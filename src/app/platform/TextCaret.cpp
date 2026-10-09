#include "TextCaret.h"

#include <QDebug>

#include <ole2.h> // before oleacc.h: WIN32_LEAN_AND_MEAN keeps it out of windows.h

#include <oleacc.h>
#include <shellscalingapi.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace ws::win {

namespace {

constexpr UINT kAnswerMs = 200; // for the caret object
constexpr DWORD kAutomationMs = 300; // for each of UI Automation's calls

// Physical pixels per pixel of `hwnd`'s own: Windows scales a program that
// is not DPI aware (or only of the system's DPI, on another monitor).
double scaleOf(HWND hwnd)
{
    UINT dpiX = 0;
    UINT dpiY = 0;
    const UINT own = ::GetDpiForWindow(hwnd);
    if (!own || FAILED(::GetDpiForMonitor(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        return 1;
    return double(dpiX) / own;
}

// `rect` in `hwnd`'s client area, in its own pixels: on the screen, physical.
QRect clientToScreen(HWND hwnd, const RECT& rect)
{
    const double scale = scaleOf(hwnd);
    POINT topLeft {std::lround(rect.left * scale), std::lround(rect.top * scale)};
    if (!::ClientToScreen(hwnd, &topLeft))
        return {};
    return QRect(int(topLeft.x), int(topLeft.y), std::max(1, int(std::lround((rect.right - rect.left) * scale))),
        int(std::lround((rect.bottom - rect.top) * scale)));
}

// `rect` on the screen as `hwnd`'s program sees it: physical.
QRect ownScreenToScreen(HWND hwnd, const RECT& rect)
{
    if (scaleOf(hwnd) == 1)
        return QRect(QPoint(rect.left, rect.top), QPoint(rect.right - 1, rect.bottom - 1));
    POINT origin {}; // of the client area, in its pixels
    const DPI_AWARENESS_CONTEXT ours = ::SetThreadDpiAwarenessContext(::GetWindowDpiAwarenessContext(hwnd));
    ::ClientToScreen(hwnd, &origin);
    ::SetThreadDpiAwarenessContext(ours);
    return clientToScreen(hwnd, {rect.left - origin.x, rect.top - origin.y, rect.right - origin.x, rect.bottom - origin.y});
}

// A caret that is there: as tall as a line, on the window. Taller than a
// quarter of the screen it is not a caret but a field's frame.
bool plausible(const QRect& caret, HWND window)
{
    RECT frame {};
    MONITORINFO monitor {sizeof monitor};
    if (caret.height() <= 0 || !::GetWindowRect(window, &frame)
        || !::GetMonitorInfoW(::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
        return false;
    const QRect area(QPoint(frame.left, frame.top), QPoint(frame.right - 1, frame.bottom - 1));
    return caret.height() <= (monitor.rcWork.bottom - monitor.rcWork.top) / 4 && area.intersects(caret);
}

std::optional<QRect> systemCaret(const GUITHREADINFO& info, HWND window)
{
    if (!info.hwndCaret || !::IsWindowVisible(info.hwndCaret) || ::GetAncestor(info.hwndCaret, GA_ROOT) != window)
        return {};
    const QRect caret = clientToScreen(info.hwndCaret, info.rcCaret);
    return plausible(caret, window) ? std::optional(caret) : std::nullopt;
}

std::optional<QRect> accessibleCaret(HWND focus, HWND window)
{
    DWORD_PTR result = 0;
    if (!::SendMessageTimeoutW(focus, WM_GETOBJECT, 0, static_cast<LPARAM>(OBJID_CARET), SMTO_ABORTIFHUNG, kAnswerMs, &result)
        || static_cast<LRESULT>(result) <= 0)
        return {};
    ComPtr<IAccessible> object;
    if (FAILED(::ObjectFromLresult(static_cast<LRESULT>(result), __uuidof(IAccessible), 0,
            reinterpret_cast<void**>(object.GetAddressOf())))
        || !object)
        return {};
    VARIANT self {};
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    VARIANT state {};
    const bool hidden = SUCCEEDED(object->get_accState(self, &state)) && state.vt == VT_I4
        && (state.lVal & STATE_SYSTEM_INVISIBLE);
    ::VariantClear(&state);
    if (hidden)
        return {}; // not in a text field
    long x = 0;
    long y = 0;
    long width = 0;
    long height = 0;
    if (FAILED(object->accLocation(&x, &y, &width, &height, self)))
        return {};
    const QRect caret = ownScreenToScreen(focus, {x, y, x + std::max(width, 1L), y + height});
    return plausible(caret, window) ? std::optional(caret) : std::nullopt;
}

// The last of `range`'s rectangles (one per line), physical pixels.
std::optional<QRect> lastRect(IUIAutomationTextRange* range)
{
    SAFEARRAY* rects = nullptr;
    if (FAILED(range->GetBoundingRectangles(&rects)) || !rects)
        return {};
    std::optional<QRect> last;
    LONG lower = 0;
    LONG upper = -1;
    double* values = nullptr; // left, top, width, height; for each
    if (SUCCEEDED(::SafeArrayGetLBound(rects, 1, &lower)) && SUCCEEDED(::SafeArrayGetUBound(rects, 1, &upper))
        && upper - lower + 1 >= 4 && SUCCEEDED(::SafeArrayAccessData(rects, reinterpret_cast<void**>(&values)))) {
        const double* r = values + ((upper - lower + 1) / 4 - 1) * 4;
        last = QRect(int(std::lround(r[0])), int(std::lround(r[1])), std::max(1, int(std::lround(r[2]))),
            int(std::lround(r[3])));
        ::SafeArrayUnaccessData(rects);
    }
    ::SafeArrayDestroy(rects);
    return last;
}

// The window has the keyboard, so the element with the focus is in it.
std::optional<QRect> automationCaret(IUIAutomation* automation, HWND window)
{
    ComPtr<IUIAutomationElement> focused;
    if (FAILED(automation->GetFocusedElement(&focused)) || !focused)
        return {};
    ComPtr<IUIAutomationTextRange> range;
    ComPtr<IUIAutomationTextPattern2> caretText;
    if (SUCCEEDED(focused->GetCurrentPatternAs(UIA_TextPattern2Id, IID_PPV_ARGS(&caretText))) && caretText) {
        BOOL active = FALSE;
        caretText->GetCaretRange(&active, &range);
    }
    if (!range) { // the end of the selection, where the caret is
        ComPtr<IUIAutomationTextPattern> text;
        ComPtr<IUIAutomationTextRangeArray> selection;
        int count = 0;
        if (FAILED(focused->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text))) || !text
            || FAILED(text->GetSelection(&selection)) || !selection || FAILED(selection->get_Length(&count))
            || count < 1 || FAILED(selection->GetElement(count - 1, &range)) || !range)
            return {};
    }
    std::optional<QRect> caret = lastRect(range.Get());
    // An empty range has no rectangle with most: the character after it
    // gives its left edge, else the one before it (at the end) its right.
    ComPtr<IUIAutomationTextRange> next;
    if (!caret && SUCCEEDED(range->Clone(&next)) && SUCCEEDED(next->ExpandToEnclosingUnit(TextUnit_Character))) {
        if ((caret = lastRect(next.Get())))
            caret->setWidth(1);
    }
    ComPtr<IUIAutomationTextRange> before;
    int moved = 0;
    if (!caret && SUCCEEDED(range->Clone(&before))
        && SUCCEEDED(before->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character, -1, &moved))
        && moved != 0) {
        if ((caret = lastRect(before.Get())))
            *caret = QRect(caret->x() + caret->width(), caret->y(), 1, caret->height());
    }
    return caret && plausible(*caret, window) ? caret : std::nullopt;
}

} // namespace

struct TextCaret::Shared {
    struct Question {
        HWND window = nullptr;
        HWND focus = nullptr; // in it
        bool automation = false;
        std::uint64_t id = 0;
    };

    std::mutex mutex;
    std::condition_variable asked; // the thread waits on it
    std::condition_variable answered;
    std::optional<Question> question;
    bool busy = false; // on a question
    bool stopping = false;
    std::uint64_t lastId = 0;
    std::uint64_t answerId = 0;
    std::optional<QRect> answer;

    void run();
};

void TextCaret::Shared::run()
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IUIAutomation2> automation;
    if (SUCCEEDED(com)
        && SUCCEEDED(::CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) {
        // Not the default 2 and 20 s: a hung program would hold the next questions up.
        automation->put_ConnectionTimeout(kAutomationMs);
        automation->put_TransactionTimeout(kAutomationMs);
    }
    std::unique_lock lock(mutex);
    for (;;) {
        asked.wait(lock, [this] { return stopping || question; });
        if (stopping)
            break;
        const Question q = *question;
        question.reset();
        busy = true;
        lock.unlock();
        std::optional<QRect> caret = accessibleCaret(q.focus, q.window);
        if (!caret && q.automation && automation)
            caret = automationCaret(automation.Get(), q.window);
        lock.lock();
        busy = false;
        answer = caret;
        answerId = q.id;
        answered.notify_all();
    }
    lock.unlock();
    automation.Reset();
    if (SUCCEEDED(com))
        ::CoUninitialize();
}

TextCaret::TextCaret()
    : m_shared(std::make_shared<Shared>())
{
    std::thread([shared = m_shared] { shared->run(); }).detach();
}

TextCaret::~TextCaret()
{
    std::lock_guard lock(m_shared->mutex);
    m_shared->stopping = true;
    m_shared->asked.notify_all();
}

std::optional<QRect> TextCaret::find(HWND window, std::chrono::milliseconds limit)
{
    // A console's window names the program in it, not conhost.exe, whose
    // thread has the window: then the thread in front, the window being there.
    GUITHREADINFO info {sizeof info};
    const DWORD thread = ::GetWindowThreadProcessId(window, nullptr);
    if (!(thread && ::GetGUIThreadInfo(thread, &info))
        && !(::GetAncestor(::GetForegroundWindow(), GA_ROOT) == window && ::GetGUIThreadInfo(0, &info)))
        return {};
    if (const std::optional<QRect> caret = systemCaret(info, window))
        return caret;

    wchar_t className[64] {};
    ::GetClassNameW(window, className, static_cast<int>(std::size(className)));
    const bool chromeOrFirefox = std::wcsncmp(className, L"Chrome_WidgetWin_", 17) == 0
        || std::wcscmp(className, L"MozillaWindowClass") == 0;
    std::unique_lock lock(m_shared->mutex);
    if (m_shared->busy || m_shared->question)
        return {}; // still on the last one: that program hangs
    const std::uint64_t id = ++m_shared->lastId;
    m_shared->question = Shared::Question {window, info.hwndFocus ? info.hwndFocus : window, !chromeOrFirefox, id};
    m_shared->asked.notify_one();
    if (!m_shared->answered.wait_for(lock, limit, [&] { return m_shared->answerId == id; })) {
        qWarning().noquote() << "No text caret from" << QString::fromWCharArray(className) << "within" << limit.count() << "ms";
        return {};
    }
    return m_shared->answer;
}

} // namespace ws::win
