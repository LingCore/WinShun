#include "SelectedText.h"

#include "ComWorker.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <ole2.h> // WIN32_LEAN_AND_MEAN leaves COM out of windows.h
#include <UIAutomation.h>
#include <windows.h>
#include <wrl/client.h>

#include <string_view>

using namespace Qt::StringLiterals;
using Microsoft::WRL::ComPtr;

namespace ws::selection {

namespace {

// UI Automation asks the program itself; one that hangs must not hold the
// answer up for long (a gesture waits for it).
constexpr DWORD kTimeoutMs = 400;
// How far up from the focused element a text to read is looked for: the
// focus is often on a part inside the document that holds the selection.
constexpr int kLevelsUp = 8;

struct Answer {
    QString text;
    bool readable = true;
};

// Windows that must not be asked (see the header): Chromium's frames, its
// page, its GPU surface, CEF's, and Firefox's. The same names as
// WindowSnapshot::isChromiumClass, plus Firefox.
bool neverAsked(HWND window)
{
    wchar_t name[96] = {};
    if (!::GetClassNameW(window, name, static_cast<int>(std::size(name))))
        return false;
    const std::wstring_view c(name);
    return c.starts_with(L"Chrome_WidgetWin_") || c == L"Chrome_RenderWidgetHostHWND" || c == L"Intermediate D3D Window"
        || c == L"CefBrowserWindow" || c == L"MozillaWindowClass";
}

// The windows UI Automation talks to here: the focused one and those above it
// up to the top-level window. None of them may be one not to ask.
bool askable(HWND root)
{
    if (neverAsked(root))
        return false;
    GUITHREADINFO info {sizeof info};
    if (!::GetGUIThreadInfo(::GetWindowThreadProcessId(root, nullptr), &info))
        return true;
    for (HWND w = info.hwndFocus; w && w != root; w = ::GetAncestor(w, GA_PARENT)) {
        if (neverAsked(w))
            return false;
    }
    return true;
}

Answer read(IUIAutomation* automation, HWND wanted)
{
    const HWND front = ::GetAncestor(::GetForegroundWindow(), GA_ROOT);
    if (!front || (wanted && front != wanted))
        return {}; // another window in front: nothing selected in the one meant
    if (!askable(front))
        return {QString(), false};
    if (!automation)
        return {QString(), false};
    DWORD pid = 0;
    ::GetWindowThreadProcessId(front, &pid);
    ComPtr<IUIAutomationElement> element;
    if (FAILED(automation->GetFocusedElement(&element)) || !element)
        return {QString(), false}; // an administrator's window, or one that did not answer
    int elementPid = 0;
    if (FAILED(element->get_CurrentProcessId(&elementPid)) || static_cast<DWORD>(elementPid) != pid)
        return {};
    ComPtr<IUIAutomationTreeWalker> walker;
    automation->get_ControlViewWalker(&walker);
    ComPtr<IUIAutomationTextPattern> pattern;
    for (int level = 0; element && level < kLevelsUp; ++level) {
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&pattern))) && pattern)
            break;
        pattern.Reset();
        ComPtr<IUIAutomationElement> parent;
        if (!walker || FAILED(walker->GetParentElement(element.Get(), &parent)))
            break;
        element = parent;
    }
    if (!pattern)
        return {}; // no text there (a list, a button): nothing selected
    ComPtr<IUIAutomationTextRangeArray> ranges;
    if (FAILED(pattern->GetSelection(&ranges)) || !ranges)
        return {};
    int count = 0;
    ranges->get_Length(&count);
    QString text;
    for (int i = 0; i < count && text.size() < kMostCharacters; ++i) {
        ComPtr<IUIAutomationTextRange> range;
        if (FAILED(ranges->GetElement(i, &range)) || !range)
            continue;
        BSTR got = nullptr;
        if (SUCCEEDED(range->GetText(kMostCharacters - static_cast<int>(text.size()), &got)) && got) {
            const QString piece = QString::fromWCharArray(got, static_cast<qsizetype>(::SysStringLen(got)));
            ::SysFreeString(got);
            if (!text.isEmpty() && !piece.isEmpty())
                text += u'\n';
            text += piece;
        }
    }
    // Only spaces (a caret in an empty line, a selected blank; U+3000 and
    // U+00A0 count as spaces for trimmed()): none.
    if (text.trimmed().isEmpty())
        return {};
    return {text.left(kMostCharacters), true};
}

// The reader's thread and what lives there: COM's multithreaded apartment and
// one IUIAutomation, made at the first ask, released as the thread ends.
struct Reader {
    ComPtr<IUIAutomation> automation;
    bool made = false;
    ComWorker worker {ComWorker::Apartment::MultiThreaded, [this] { automation.Reset(); }};

    IUIAutomation* get()
    {
        if (!std::exchange(made, true)) {
            if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))))
                ::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
            if (ComPtr<IUIAutomation2> timed; automation && SUCCEEDED(automation.As(&timed))) {
                timed->put_ConnectionTimeout(kTimeoutMs);
                timed->put_TransactionTimeout(kTimeoutMs);
            }
        }
        return automation.Get();
    }
};

Reader& reader()
{
    static Reader r;
    return r;
}

} // namespace

void run(const QJsonObject& arguments, std::function<void(mcp::ToolResult)> done)
{
    // The words never go into the log: they are the user's.
    // A number, or the text of one (an AI's tool call may send either).
    const auto wanted = reinterpret_cast<HWND>(static_cast<quintptr>(arguments.value(u"window"_s).toVariant().toULongLong()));
    reader().worker.post([wanted, done = std::move(done)]() mutable {
        const Answer answer = read(reader().get(), wanted);
        QCoreApplication* app = QCoreApplication::instance();
        if (!app)
            return; // Win顺 is closing
        QMetaObject::invokeMethod(
            app,
            [done = std::move(done), answer] {
                done({QJsonObject {{u"text"_s, answer.text}, {u"readable"_s, answer.readable}}});
            },
            Qt::QueuedConnection);
    });
}

} // namespace ws::selection
