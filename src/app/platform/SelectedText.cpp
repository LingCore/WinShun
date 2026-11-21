#include "SelectedText.h"

#include "ClipboardBorrow.h"
#include "ComWorker.h"
#include "OwnInput.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QScopeGuard>

#include <ole2.h> // WIN32_LEAN_AND_MEAN leaves COM out of windows.h
#include <UIAutomation.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstring>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// `copy`: how long the shortcut's own keys may stay held (Ctrl+C pressed
// with Alt or Win still down would be another key), how long the program
// in front has to answer Ctrl+C (a browser asks its page), and the most a
// clipboard may hold to be put back as it was.
constexpr DWORD kKeysUpMs = 1000;
constexpr DWORD kCopyWaitMs = 600;
constexpr DWORD kSecondWriteWaitMs = 120; // follow-up SetClipboardData after EmptyClipboard in a two-step copy
constexpr SIZE_T kMostKeptBytes = 64 * 1024 * 1024;

struct Answer {
    QString text;
    bool readable = true;
    bool known = true; // false: no text found to ask (a program UI Automation tells nothing of)
};

std::wstring windowClassName(HWND window)
{
    wchar_t name[96] = {};
    const int n = window ? ::GetClassNameW(window, name, static_cast<int>(std::size(name))) : 0;
    return n > 0 ? std::wstring(name, static_cast<std::size_t>(n)) : std::wstring();
}

bool usesCtrlInsert(std::wstring_view className) noexcept
{
    return className == L"mintty" || className == L"PuTTY";
}

// Windows that must not be asked (see the header): Chromium's frames, its
// page, its GPU surface, CEF's, and Firefox's. The same names as
// WindowSnapshot::isChromiumClass, plus Firefox.
bool neverAsked(HWND window)
{
    const std::wstring name = windowClassName(window);
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
        return {QString(), true, false}; // no text there (a list, a button), or none UI Automation knows of
    ComPtr<IUIAutomationTextRangeArray> ranges;
    if (FAILED(pattern->GetSelection(&ranges)) || !ranges)
        return {QString(), true, false};
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

// ---- `copy`: the clipboard borrowed ----

bool openClipboard(HWND owner)
{
    for (int attempt = 0; attempt < 20; ++attempt) { // the program in front may be writing it
        if (::OpenClipboard(owner))
            return true;
        ::Sleep(10);
    }
    return false;
}

// One format of what was on the clipboard: its bytes, or a copy of its
// enhanced metafile.
struct Kept {
    UINT format = 0;
    QByteArray bytes;
    HENHMETAFILE metafile = nullptr;
};

struct KeptClipboard {
    std::vector<Kept> formats;
    KeptClipboard() = default;
    KeptClipboard(const KeptClipboard&) = delete;
    KeptClipboard& operator=(const KeptClipboard&) = delete;
    KeptClipboard(KeptClipboard&&) = default;
    ~KeptClipboard()
    {
        for (const Kept& k : formats) {
            if (k.metafile)
                ::DeleteEnhMetaFile(k.metafile);
        }
    }
};

// Formats that are not kept: handles that are not memory (bitmaps, palettes,
// old metafiles: Windows makes them again from the DIB and the enhanced
// metafile), a program's private handles, and OLE's own, which would point
// Word and the like to a data object that is gone.
bool kept(UINT format)
{
    switch (format) {
    case CF_BITMAP: case CF_METAFILEPICT: case CF_PALETTE: case CF_OWNERDISPLAY:
    case CF_DSPBITMAP: case CF_DSPMETAFILEPICT: case CF_DSPENHMETAFILE:
        return false;
    default:
        break;
    }
    if ((format >= CF_PRIVATEFIRST && format <= CF_PRIVATELAST) || (format >= CF_GDIOBJFIRST && format <= CF_GDIOBJLAST))
        return false;
    wchar_t name[64] = {};
    if (format >= 0xC000 && ::GetClipboardFormatNameW(format, name, static_cast<int>(std::size(name))) > 0) {
        const std::wstring_view n(name);
        if (n == L"DataObject" || n == L"Ole Private Data" || n == L"OleClipboardPersistOnFlush")
            return false;
    }
    return true;
}

// What is on the clipboard, to be put back. None when it cannot be opened or
// holds more than kMostKeptBytes: then it is not borrowed at all.
std::optional<KeptClipboard> keep()
{
    if (!openClipboard(nullptr))
        return std::nullopt;
    const auto close = qScopeGuard([] { ::CloseClipboard(); });
    KeptClipboard out;
    SIZE_T total = 0;
    for (UINT format = ::EnumClipboardFormats(0); format != 0; format = ::EnumClipboardFormats(format)) {
        if (format == CF_ENHMETAFILE) {
            if (const HANDLE h = ::GetClipboardData(format)) {
                if (const HENHMETAFILE copy = ::CopyEnhMetaFileW(static_cast<HENHMETAFILE>(h), nullptr))
                    out.formats.push_back({format, {}, copy});
            }
            continue;
        }
        if (!kept(format))
            continue;
        const HANDLE h = ::GetClipboardData(format);
        const SIZE_T size = h ? ::GlobalSize(h) : 0;
        if (size == 0)
            continue;
        total += size;
        if (total > kMostKeptBytes)
            return std::nullopt;
        const void* data = ::GlobalLock(h);
        if (!data)
            continue;
        out.formats.push_back({format, QByteArray(static_cast<const char*>(data), static_cast<qsizetype>(size)), nullptr});
        ::GlobalUnlock(h);
    }
    return out;
}

// What was kept, on the clipboard again. Its owner is a window made for it,
// gone right after: the data stays, owned by nobody (SetClipboardData needs
// an owner while it is written).
void putBack(KeptClipboard& kept)
{
    static const wchar_t* const kClass = [] {
        WNDCLASSW wc {};
        wc.lpfnWndProc = ::DefWindowProcW;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WinShun.SelectionCopy";
        ::RegisterClassW(&wc);
        return wc.lpszClassName;
    }();
    const HWND owner = ::CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!owner)
        return;
    const auto destroy = qScopeGuard([owner] { ::DestroyWindow(owner); });
    if (!openClipboard(owner))
        return;
    ::EmptyClipboard();
    const auto put = [](UINT format, const void* bytes, std::size_t size) {
        const HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(size));
        if (!memory)
            return;
        if (void* data = ::GlobalLock(memory)) {
            std::memcpy(data, bytes, size);
            ::GlobalUnlock(memory);
            if (::SetClipboardData(format, memory))
                return;
        }
        ::GlobalFree(memory);
    };
    for (Kept& k : kept.formats) {
        if (k.metafile) {
            if (::SetClipboardData(k.format, k.metafile))
                k.metafile = nullptr; // the clipboard's now
            continue;
        }
        put(k.format, k.bytes.constData(), static_cast<std::size_t>(k.bytes.size()));
    }
    // Not a new copy: Windows' history (Win+V) has it already, and clipboard
    // managers leave it alone.
    const DWORD zero = 0;
    for (const wchar_t* name : {L"CanIncludeInClipboardHistory", L"CanUploadToCloudClipboard",
             L"ExcludeClipboardContentFromMonitorProcessing"})
        put(::RegisterClipboardFormatW(name), &zero, sizeof zero);
    ::CloseClipboard();
}

QString clipboardText()
{
    if (!openClipboard(nullptr))
        return {};
    QString text;
    if (const HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* data = static_cast<const wchar_t*>(::GlobalLock(h))) {
            const std::size_t most = ::GlobalSize(h) / sizeof(wchar_t);
            text = QString::fromWCharArray(data, static_cast<qsizetype>(::wcsnlen(data, most)));
            ::GlobalUnlock(h);
        }
    }
    ::CloseClipboard();
    return text;
}

bool modifiersHeld()
{
    for (const int vk : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN}) {
        if (::GetAsyncKeyState(vk) & 0x8000)
            return true;
    }
    return false;
}

void pressCopy(bool ctrlInsert)
{
    INPUT in[4] = {};
    const auto key = [&](int i, WORD vk, DWORD flags) {
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = vk;
        in[i].ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        in[i].ki.dwFlags = flags;
        in[i].ki.dwExtraInfo = ws::kOwnInput;
    };
    const WORD target = ctrlInsert ? VK_INSERT : 'C';
    const DWORD extended = ctrlInsert ? KEYEVENTF_EXTENDEDKEY : 0;
    key(0, VK_CONTROL, 0);
    key(1, target, extended);
    key(2, target, extended | KEYEVENTF_KEYUP);
    key(3, VK_CONTROL, KEYEVENTF_KEYUP);
    ::SendInput(4, in, sizeof(INPUT));
}

// The words selected in the window in front read by copying them: what is
// on the clipboard kept, Ctrl+C (or Ctrl+Insert in mintty/PuTTY) pressed there,
// the text it copied read, and what was kept put back at once. Win顺's
// clipboard history records none of it (ClipboardBorrow.h). Nothing copied:
// nothing selected; `readable` then says whether that can be believed (a window
// of an administrator's program takes no keys from Win顺 either).
Answer copied(HWND wanted, bool ctrlInsert, bool readableIfNothing)
{
    // By the clock: a Sleep lasts a tick of Windows' timer (15.6 ms), not what it is asked.
    for (const ULONGLONG until = ::GetTickCount64() + kKeysUpMs; modifiersHeld(); ::Sleep(5)) {
        if (::GetTickCount64() >= until)
            return {QString(), false};
    }
    const HWND front = ::GetAncestor(::GetForegroundWindow(), GA_ROOT);
    if (!front || (wanted && front != wanted))
        return {};
    std::optional<KeptClipboard> kept = keep();
    if (!kept)
        return {QString(), false}; // too much on it to put back as it was: left alone
    clipboard_borrow::begin();
    DWORD through = ::GetClipboardSequenceNumber();
    const auto end = qScopeGuard([&through] { clipboard_borrow::end(through); });
    const DWORD before = through;
    pressCopy(ctrlInsert);
    DWORD seq = before;
    for (const ULONGLONG until = ::GetTickCount64() + kCopyWaitMs; seq == before && ::GetTickCount64() < until;) {
        seq = ::GetClipboardSequenceNumber();
        if (seq == before)
            ::Sleep(5);
    }
    if (seq == before)
        return {QString(), readableIfNothing};
    QString text = clipboardText(); // opens once the program has written it all
    // Some programs empty the clipboard in one lock and write CF_UNICODETEXT in a
    // second lock right after: wait briefly for a follow-up sequence bump if empty.
    if (text.trimmed().isEmpty()) {
        for (const ULONGLONG until = ::GetTickCount64() + kSecondWriteWaitMs; ::GetTickCount64() < until;) {
            const DWORD now = ::GetClipboardSequenceNumber();
            if (now != seq) {
                seq = now;
                text = clipboardText();
                if (!text.trimmed().isEmpty())
                    break;
            }
            ::Sleep(5);
        }
    }
    putBack(*kept);
    through = ::GetClipboardSequenceNumber();
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

bool isTerminalClass(std::wstring_view className) noexcept
{
    return className == L"ConsoleWindowClass" || className == L"CASCADIA_HOSTING_WINDOW_CLASS"
        || className == L"mintty" || className == L"PuTTY" || className == L"VirtualConsoleClass"
        || className == L"org.wezfurlong.wezterm" || className == L"Alacritty" || className == L"Ghostty";
}

CopyPlan planCopyFallback(bool allowCopy, bool hasText, bool readable, bool known, bool askableWindow,
    std::wstring_view windowClass) noexcept
{
    if (!allowCopy || hasText)
        return {false, false, readable};
    if (isTerminalClass(windowClass)) {
        if (usesCtrlInsert(windowClass))
            return {true, true, true};
        return {false, false, readable};
    }
    return {true, false, readable || !known || !askableWindow};
}

void run(const QJsonObject& arguments, std::function<void(mcp::ToolResult)> done)
{
    // The words never go into the log: they are the user's.
    // A number, or the text of one (an AI's tool call may send either).
    const auto wanted = reinterpret_cast<HWND>(static_cast<quintptr>(arguments.value(u"window"_s).toVariant().toULongLong()));
    const bool copy = arguments.value(u"copy"_s).toBool();
    reader().worker.post([wanted, copy, done = std::move(done)]() mutable {
        Answer answer = read(reader().get(), wanted);
        // When invoked by a user shortcut (`copy`), fall back to copying whenever UI Automation
        // yielded no text — covering browsers, Firefox, custom controls, and chat windows like QQ
        // where focus stays on the edit box while a message bubble is selected (skipping Ctrl+C
        // in terminals so an empty selection never sends SIGINT).
        if (copy && answer.text.isEmpty()) {
            const HWND front = ::GetAncestor(::GetForegroundWindow(), GA_ROOT);
            if (front && (!wanted || front == wanted)) {
                const CopyPlan plan = planCopyFallback(copy, !answer.text.isEmpty(), answer.readable, answer.known,
                    askable(front), windowClassName(front));
                if (plan.copy)
                    answer = copied(wanted, plan.ctrlInsert, plan.readableIfNothing);
            }
        }
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
