#include "Selection.h"

#include <ole2.h> // WIN32_LEAN_AND_MEAN leaves COM out of windows.h
#include <UIAutomation.h>
#include <wrl/client.h>

namespace gestures {

namespace {

using Microsoft::WRL::ComPtr;

// UI Automation asks the program itself; one that hangs must not hold the
// gesture up for long.
constexpr DWORD kTimeoutMs = 400;
// How far up from the focused element a text to read is looked for: a
// browser's focus is often on a link or a button inside the page, whose
// document holds the selection.
constexpr int kLevelsUp = 8;

std::wstring readSelection(IUIAutomation* automation, HWND root)
{
    // The selection belongs to the window in front: a gesture drawn on
    // another one means nothing selected there.
    if (::GetAncestor(::GetForegroundWindow(), GA_ROOT) != root)
        return {};
    DWORD pid = 0;
    ::GetWindowThreadProcessId(root, &pid);
    ComPtr<IUIAutomationElement> element;
    if (FAILED(automation->GetFocusedElement(&element)) || !element)
        return {};
    int elementPid = 0;
    if (FAILED(element->get_CurrentProcessId(&elementPid)) || static_cast<DWORD>(elementPid) != pid)
        return {};
    ComPtr<IUIAutomationTreeWalker> walker;
    automation->get_ControlViewWalker(&walker);
    ComPtr<IUIAutomationTextPattern> text;
    for (int level = 0; element && level < kLevelsUp; ++level) {
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text))) && text)
            break;
        text.Reset();
        ComPtr<IUIAutomationElement> parent;
        if (!walker || FAILED(walker->GetParentElement(element.Get(), &parent)))
            break;
        element = parent;
    }
    if (!text)
        return {};
    ComPtr<IUIAutomationTextRangeArray> ranges;
    if (FAILED(text->GetSelection(&ranges)) || !ranges)
        return {};
    int count = 0;
    ranges->get_Length(&count);
    std::wstring words;
    for (int i = 0; i < count && static_cast<int>(words.size()) < SelectionReader::kMaxLength; ++i) {
        ComPtr<IUIAutomationTextRange> range;
        if (FAILED(ranges->GetElement(i, &range)) || !range)
            continue;
        BSTR got = nullptr;
        if (SUCCEEDED(range->GetText(SelectionReader::kMaxLength - static_cast<int>(words.size()), &got)) && got) {
            if (!words.empty() && ::SysStringLen(got) > 0)
                words += L'\n';
            words.append(got, ::SysStringLen(got));
            ::SysFreeString(got);
        }
    }
    // Only spaces (a caret in an empty line, a selected blank): none.
    if (words.find_first_not_of(L" \t\r\n\x3000\xA0") == std::wstring::npos)
        return {};
    return words;
}

} // namespace

SelectionReader::SelectionReader()
    : m_thread([this] { run(); })
{
}

SelectionReader::~SelectionReader()
{
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
    }
    m_wake.notify_one();
    if (m_thread.joinable())
        m_thread.join();
}

void SelectionReader::ask(HWND root, std::function<void(std::wstring)> answer)
{
    {
        std::lock_guard lock(m_mutex);
        m_root = root;
        m_answer = std::move(answer);
    }
    m_wake.notify_one();
}

void SelectionReader::run()
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IUIAutomation> automation;
        if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))))
            ::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
        if (ComPtr<IUIAutomation2> timed; automation && SUCCEEDED(automation.As(&timed))) {
            timed->put_ConnectionTimeout(kTimeoutMs);
            timed->put_TransactionTimeout(kTimeoutMs);
        }
        for (;;) {
            HWND root = nullptr;
            std::function<void(std::wstring)> answer;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, [this] { return m_quit || m_answer; });
                if (m_quit)
                    break;
                root = m_root;
                answer = std::move(m_answer);
                m_answer = nullptr;
            }
            answer(automation ? readSelection(automation.Get(), root) : std::wstring());
        }
    }
    if (SUCCEEDED(com))
        ::CoUninitialize();
}

} // namespace gestures
