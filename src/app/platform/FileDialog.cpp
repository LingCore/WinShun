#include "FileDialog.h"

#include "PathText.h"

#include <QDebug>
#include <QString>

#include <ole2.h> // before uiautomation.h: WIN32_LEAN_AND_MEAN leaves it out of windows.h

#include <commctrl.h>
#include <shlobj.h> // BFFM_SETSELECTIONW
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

namespace ws::filedialog {

namespace {

// Controls of the common file dialogs (dlgs.h).
constexpr int kFileNameEdit = 0x480; // edt1; a folder picker's "Folder:" box
constexpr int kFileNameCombo = 0x47C; // cmb13
constexpr int kFileTypeCombo = 0x470; // cmb1
constexpr int kBrowseTree = 0x3741; // SHBrowseForFolder's tree, in the old style (no BIF_NEWDIALOGSTYLE)

bool hasClass(HWND hwnd, const wchar_t* name)
{
    wchar_t buffer[64] {};
    return ::GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer))) > 0 && ::wcscmp(buffer, name) == 0;
}

// Down a path of window classes, taking the first child of each; nullptr
// where one is missing.
HWND descend(HWND hwnd, std::initializer_list<const wchar_t*> classes)
{
    for (const wchar_t* name : classes) {
        if (!hwnd)
            break; // FindWindowEx would search the top-level windows
        hwnd = ::FindWindowExW(hwnd, nullptr, name, nullptr);
    }
    return hwnd;
}

// The address bar of a dialog of Windows Vista and later: crumbs, and a text
// box in their place while it is being edited.
HWND addressBar(HWND dialog)
{
    return descend(dialog, {L"WorkerW", L"ReBarWindow32", L"Address Band Root", L"msctls_progress32"});
}

HWND crumbBar(HWND dialog)
{
    return descend(addressBar(dialog), {L"Breadcrumb Parent", L"ToolbarWindow32"});
}

// The address bar's text box, shown or hidden; none until it is first made.
HWND addressBox(HWND dialog)
{
    return descend(addressBar(dialog), {L"ComboBoxEx32", L"ComboBox", L"Edit"});
}

// The file name box: in an Open dialog (and the Windows XP style) a combo
// box with an edit field, or an edit field alone; in a Save dialog the first
// combo box with an edit field inside the dialog's view. A folder picker's
// is its "Folder:" box.
HWND fileNameBox(HWND dialog)
{
    if (const HWND combo = ::GetDlgItem(dialog, kFileNameCombo)) {
        const HWND list = hasClass(combo, L"ComboBoxEx32") ? ::FindWindowExW(combo, nullptr, L"ComboBox", nullptr) : combo;
        if (const HWND edit = list ? ::FindWindowExW(list, nullptr, L"Edit", nullptr) : nullptr)
            return edit;
    }
    if (const HWND view = descend(dialog, {L"DUIViewWndClassName", L"DirectUIHWND"})) {
        for (HWND sink = ::FindWindowExW(view, nullptr, L"FloatNotifySink", nullptr); sink;
            sink = ::FindWindowExW(view, sink, L"FloatNotifySink", nullptr)) {
            if (const HWND edit = descend(sink, {L"ComboBox", L"Edit"}))
                return edit;
        }
    }
    return ::GetDlgItem(dialog, kFileNameEdit);
}

// Inno Setup uses its own folder picker, not SHBrowseForFolder. Its Start
// Menu picker has the same form class but a different tree: leave that alone.
// See upstream Setup.SelectFolderForm.pas (formerly TSelectDirDlg).
HWND installerFolderBox(HWND dialog)
{
    if ((!hasClass(dialog, L"TSelectFolderForm") && !hasClass(dialog, L"TSelectDirDlg"))
        || !::FindWindowExW(dialog, nullptr, L"TFolderTreeView", nullptr))
        return nullptr;
    for (const wchar_t* name : {L"TNewPathEdit", L"TNewEdit", L"TEdit"}) {
        if (const HWND edit = ::FindWindowExW(dialog, nullptr, name, nullptr);
            edit && ::IsWindowVisible(edit) && ::IsWindowEnabled(edit)
            && !(::GetWindowLongPtrW(edit, GWL_STYLE) & ES_READONLY))
            return edit;
    }
    return nullptr;
}

bool send(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    DWORD_PTR result = 0;
    return ::SendMessageTimeoutW(hwnd, msg, wParam, lParam, SMTO_ABORTIFHUNG, 2000, &result) != 0;
}

// What a control of another program holds: GetWindowText does not ask
// another program's controls (WM_GETTEXT).
std::wstring controlText(HWND hwnd)
{
    DWORD_PTR length = 0;
    if (!::SendMessageTimeoutW(hwnd, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, 2000, &length))
        return {};
    std::wstring text(length + 1, L'\0');
    DWORD_PTR copied = 0;
    if (!::SendMessageTimeoutW(hwnd, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()), SMTO_ABORTIFHUNG,
            2000, &copied))
        return {};
    text.resize(std::min<std::size_t>(copied, length));
    return text;
}

// A window's own caption: the crumbs keep "Address: <folder>" in theirs.
std::wstring caption(HWND hwnd)
{
    std::wstring text(static_cast<std::size_t>(::GetWindowTextLengthW(hwnd)) + 1, L'\0');
    text.resize(static_cast<std::size_t>(::GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()))));
    return text;
}

bool setText(HWND hwnd, const std::wstring& text)
{
    return send(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

bool isFolder(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool waitFor(std::predicate auto done, std::chrono::milliseconds limit)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= end)
            return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

HWND focusedControl(HWND dialog)
{
    GUITHREADINFO info {sizeof(GUITHREADINFO)};
    const DWORD thread = ::GetWindowThreadProcessId(dialog, nullptr);
    return thread && ::GetGUIThreadInfo(thread, &info) ? info.hwndFocus : nullptr;
}

// Types the folder into the address bar, as in Explorer. Unlike the file
// name box, the address bar never accepts the dialog: in a folder picker,
// OK with a folder in the box would choose it and close the dialog. It goes
// in with a trailing backslash, which the address bar resolves only as a
// folder (a folder named "a.txt" is never taken for a file to open), and is
// checked again right before, as by the service's helper (DialogNav.cpp).
bool goByAddress(HWND dialog, const std::wstring& folder)
{
    const HWND bar = addressBar(dialog);
    const HWND crumbs = crumbBar(dialog);
    if (!bar || !crumbs)
        return false;
    const HWND focus = focusedControl(dialog);
    const std::wstring shown = caption(crumbs); // "Address: <folder>"
    // The text box: shown while the user edits the address, else hidden
    // since an earlier edit (it takes a path and Enter just as well), else
    // not made yet. Only then a click past the last crumb, to make it: such
    // clicks have gone unanswered, for a reason not found (docs/pitfalls.md).
    // In a program that is not DPI aware the click makes the box but never
    // shows it, and the hidden box is taken all the same (docs/pitfalls.md).
    HWND box = addressBox(dialog);
    if (!box) {
        // In the dialog's own coordinates: a program that is not DPI aware has scaled ones.
        RECT client {};
        const DPI_AWARENESS_CONTEXT ours = ::SetThreadDpiAwarenessContext(::GetWindowDpiAwarenessContext(crumbs));
        ::GetClientRect(crumbs, &client);
        ::SetThreadDpiAwarenessContext(ours);
        const LPARAM at = MAKELPARAM(std::max<LONG>(client.right - 4, 0), client.bottom / 2);
        for (int attempt = 0; attempt < 3 && !box; ++attempt) {
            ::PostMessageW(crumbs, WM_LBUTTONDOWN, MK_LBUTTON, at);
            ::PostMessageW(crumbs, WM_LBUTTONUP, 0, at);
            waitFor([&] { return (box = addressBox(dialog)) && ::IsWindowVisible(box); }, 300ms);
        }
        if (!box)
            return false;
    }
    // Enter only once the box holds the folder (still one) just typed: never
    // on what it held before.
    const std::wstring typed = folder.ends_with(L'\\') ? folder : folder + L'\\';
    if (!isFolder(folder) || !setText(box, typed) || controlText(box) != typed
        || !send(box, WM_KEYDOWN, VK_RETURN, 0x001C0001) || !send(box, WM_KEYUP, VK_RETURN, 0xC01C0001))
        return false;
    // Gone there (or it was there already): the crumbs show it, and the text
    // box has closed, which leaves the keyboard focus on the crumbs. Back to
    // where it was (usually the file name); not if the user has gone on
    // meanwhile (to the bar): the dialog would take the front back.
    if (!waitFor([&] { return !::IsWindowVisible(box) && caption(crumbs) != shown; }, 1500ms))
        qWarning() << "The file dialog had not shown the folder typed into its address bar after 1.5 s";
    if (focus && ::GetForegroundWindow() == dialog && focusedControl(dialog) != focus && !::IsChild(bar, focus)
        && ::IsChild(dialog, focus) && ::IsWindowVisible(focus))
        send(dialog, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(focus), TRUE);
    return true;
}

// The folder into the file name box, and OK opens it; then the name typed
// before comes back. With the trailing backslash a Save dialog cannot take it
// for a file name: a folder that is gone only gets an error message. The
// only way in the Windows XP style, which has no address bar; never in a
// folder picker, which would choose the folder and close.
bool goByName(HWND dialog, const std::wstring& folder)
{
    const HWND box = fileNameBox(dialog);
    const HWND ok = ::GetDlgItem(dialog, IDOK);
    const HWND crumbs = crumbBar(dialog);
    if (!box || !ok)
        return false;
    const std::wstring typed = controlText(box);
    const std::wstring shown = crumbs ? caption(crumbs) : std::wstring();
    const std::wstring target = folder.ends_with(L'\\') ? folder : folder + L'\\';
    if (!isFolder(folder) || !setText(box, target)
        || !send(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), reinterpret_cast<LPARAM>(ok)))
        return false;
    // A dialog of Windows Vista and later goes there after OK returns, and
    // then clears the box.
    if (crumbs)
        waitFor([&] { return caption(crumbs) != shown; }, 1500ms);
    setText(box, typed);
    return true;
}

// SHBrowseForFolder's dialog ("浏览文件夹", from VBA macros and
// Shell.Application.BrowseForFolder): a tree of folders and OK, no address
// bar. The tree sits in a control of its own (BIF_NEWDIALOGSTYLE) or right
// in the dialog.
HWND browseTree(HWND dialog)
{
    if (const HWND tree = descend(dialog, {L"SHBrowseForFolder ShellNameSpace Control", L"SysTreeView32"}))
        return tree;
    const HWND tree = ::GetDlgItem(dialog, kBrowseTree);
    return tree && hasClass(tree, L"SysTreeView32") ? tree : nullptr;
}

// The tree opened at the folder and the folder selected, as the program
// itself would from its callback: OK then picks it. Both messages take the
// path as a pointer into the dialog's own memory, so it is written there
// first (the dialog's program has our rights or less: DialogJump leaves an
// elevated one alone).
bool goByTree(HWND dialog, const std::wstring& folder)
{
    DWORD process = 0;
    if (!::GetWindowThreadProcessId(dialog, &process))
        return false;
    const HANDLE handle = ::OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, process);
    if (!handle)
        return false;
    const SIZE_T bytes = (folder.size() + 1) * sizeof(wchar_t);
    void* remote = ::VirtualAllocEx(handle, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool answered = true;
    bool went = false;
    if (remote && ::WriteProcessMemory(handle, remote, folder.c_str(), bytes, nullptr)) {
        const LPARAM path = reinterpret_cast<LPARAM>(remote);
        // Expanded first, so that the selection, made last, is where the
        // tree scrolls to.
        answered = send(dialog, BFFM_SETEXPANDED, TRUE, path) && send(dialog, BFFM_SETSELECTIONW, TRUE, path);
        went = answered;
    }
    // Unanswered (hung): it may still read the path later; a few bytes are
    // left there rather than freed under it.
    if (remote && answered)
        ::VirtualFreeEx(handle, remote, 0, MEM_RELEASE);
    ::CloseHandle(handle);
    return went;
}

// WPS Office's own file dialog. Its address bar shows crumbs, and a text box
// in their place once the space after them is clicked (KcfdLocSpaceButton);
// Enter there goes, as in Explorer. A folder it cannot go to gets a message
// box of its own ("输入位置有误").
namespace kingsoft {

// The programs of WPS Office that show it.
constexpr const wchar_t* kPrograms[] {L"wps.exe", L"et.exe", L"wpp.exe", L"wpspdf.exe", L"wpsoffice.exe"};

// What probe() found for each window it asked (None: not one).
std::mutex g_mutex;
std::vector<std::pair<HWND, Kind>> g_known;

std::optional<Kind> known(HWND hwnd)
{
    std::scoped_lock lock(g_mutex);
    const auto it = std::ranges::find(g_known, hwnd, &std::pair<HWND, Kind>::first);
    return it != g_known.end() ? std::optional(it->second) : std::nullopt;
}

bool isOne(HWND dialog)
{
    const std::optional<Kind> k = known(dialog);
    return k && *k != Kind::None;
}

void remember(HWND hwnd, Kind k)
{
    std::scoped_lock lock(g_mutex);
    std::erase_if(g_known, [hwnd](const auto& seen) { return seen.first == hwnd || !::IsWindow(seen.first); });
    g_known.emplace_back(hwnd, k);
}

std::wstring programOf(HWND hwnd)
{
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    std::wstring path(MAX_PATH, L'\0');
    DWORD size = MAX_PATH;
    const bool ok = ::QueryFullProcessImageNameW(process, 0, path.data(), &size);
    ::CloseHandle(process);
    if (!ok)
        return {};
    path.resize(size);
    return path.substr(path.find_last_of(L'\\') + 1);
}

// The dialog through UI Automation, made for each use: given up on soon
// should the program hang.
class Dialog {
public:
    explicit Dialog(HWND hwnd)
    {
        if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_uia)))
            && FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_uia))))
            return;
        ComPtr<IUIAutomation2> timed;
        if (SUCCEEDED(m_uia.As(&timed))) {
            timed->put_ConnectionTimeout(1000);
            timed->put_TransactionTimeout(1000);
        }
        if (FAILED(m_uia->ElementFromHandle(hwnd, &m_root)) || !m_root)
            return;
        m_root->get_CurrentProcessId(&m_process);
    }

    explicit operator bool() const { return m_root != nullptr; }
    IUIAutomationElement* root() const { return m_root.Get(); }

    // The first under `in` (the dialog) of that class, as Qt names its widgets.
    ComPtr<IUIAutomationElement> find(const wchar_t* className, IUIAutomationElement* in = nullptr,
        TreeScope scope = TreeScope_Descendants) const
    {
        ComPtr<IUIAutomationElement> found;
        ComPtr<IUIAutomationCondition> condition;
        if (byClass(className, condition))
            (in ? in : m_root.Get())->FindFirst(scope, condition.Get(), &found);
        return found;
    }

    // What has the keyboard focus, if in the dialog's program.
    ComPtr<IUIAutomationElement> focused() const
    {
        ComPtr<IUIAutomationElement> element;
        int process = 0;
        if (FAILED(m_uia->GetFocusedElement(&element)) || !element || FAILED(element->get_CurrentProcessId(&process))
            || process != m_process)
            return nullptr;
        return element;
    }

    bool focusIn(const wchar_t* className) const
    {
        const ComPtr<IUIAutomationElement> element = focused();
        return element && classOf(element.Get()) == className;
    }

    // Open (or Save, Select…): the first push button of the row at the bottom, before Cancel.
    ComPtr<IUIAutomationElement> acceptButton() const
    {
        const ComPtr<IUIAutomationElement> row = find(L"kd::KDDialogButtonBox");
        return row ? find(L"kd::KDButton", row.Get(), TreeScope_Children) : nullptr;
    }

    // In the box for the file name: an edit field in a combo box.
    ComPtr<IUIAutomationElement> nameBox() const
    {
        const ComPtr<IUIAutomationElement> filter = find(L"KcfdFilterWidget");
        return filter ? find(L"kd::KDTextField", filter.Get()) : nullptr;
    }

    static std::wstring classOf(IUIAutomationElement* element) { return text(element, &IUIAutomationElement::get_CurrentClassName); }
    static std::wstring nameOf(IUIAutomationElement* element) { return text(element, &IUIAutomationElement::get_CurrentName); }

    static bool setValue(IUIAutomationElement* element, const std::wstring& value)
    {
        ComPtr<IUIAutomationValuePattern> pattern;
        if (FAILED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pattern))) || !pattern)
            return false;
        BSTR text = ::SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
        const bool ok = SUCCEEDED(pattern->SetValue(text));
        ::SysFreeString(text);
        return ok;
    }

    static bool invoke(IUIAutomationElement* element)
    {
        ComPtr<IUIAutomationInvokePattern> pattern;
        return SUCCEEDED(element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&pattern))) && pattern
            && SUCCEEDED(pattern->Invoke());
    }

private:
    bool byClass(const wchar_t* className, ComPtr<IUIAutomationCondition>& condition) const
    {
        VARIANT value;
        ::VariantInit(&value);
        value.vt = VT_BSTR;
        value.bstrVal = ::SysAllocString(className);
        const bool ok = SUCCEEDED(m_uia->CreatePropertyCondition(UIA_ClassNamePropertyId, value, &condition));
        ::VariantClear(&value);
        return ok;
    }

    static std::wstring text(IUIAutomationElement* element, HRESULT (STDMETHODCALLTYPE IUIAutomationElement::*get)(BSTR*))
    {
        BSTR value = nullptr;
        if (!element || FAILED((element->*get)(&value)) || !value)
            return {};
        std::wstring result(value, ::SysStringLen(value));
        ::SysFreeString(value);
        return result;
    }

    ComPtr<IUIAutomation> m_uia;
    ComPtr<IUIAutomationElement> m_root;
    int m_process = 0;
};

Kind probe(HWND hwnd)
{
    const Dialog dialog(hwnd);
    if (!dialog)
        return Kind::None;
    // Until Qt has its accessibility up (the first question turns it on), the
    // window answers for itself: "Qt5QWindowIcon".
    std::wstring name;
    waitFor([&] {
        name = Dialog::classOf(dialog.root());
        return !name.starts_with(L"Qt");
    }, 500ms);
    if (name != L"KcfdFileDialog")
        return Kind::None;
    // Its buttons may come a moment after it is in front.
    ComPtr<IUIAutomationElement> accept;
    waitFor([&] { return (accept = dialog.acceptButton()) != nullptr; }, 500ms);
    const std::wstring label = accept ? Dialog::nameOf(accept.Get()) : std::wstring(); // "保存(S)", "打开(O)"
    const bool open = label.find(L"打开") != std::wstring::npos || label.find(L"開啟") != std::wstring::npos
        || ::_wcsnicmp(label.c_str(), L"Open", 4) == 0;
    return open ? Kind::Open : Kind::Save;
}

bool goTo(HWND hwnd, const std::wstring& folder)
{
    if (!isFolder(folder))
        return false; // spared its message box
    const Dialog dialog(hwnd);
    if (!dialog)
        return false;
    const ComPtr<IUIAutomationElement> before = dialog.focused(); // usually the file name
    ComPtr<IUIAutomationElement> box = dialog.find(L"KcfdLocNavigationLineEdit");
    if (!box) {
        if (const ComPtr<IUIAutomationElement> space = dialog.find(L"KcfdLocSpaceButton"))
            Dialog::invoke(space.Get());
        waitFor([&] { return (box = dialog.find(L"KcfdLocNavigationLineEdit")) != nullptr; }, 500ms);
    }
    if (!box)
        return false;
    // Enter goes to whatever has the focus: never with it elsewhere, as in the
    // file name box, where it would save.
    constexpr const wchar_t* kBox = L"KcfdLocNavigationLineEdit";
    box->SetFocus();
    if (!dialog.focusIn(kBox) || !Dialog::setValue(box.Get(), folder) || !dialog.focusIn(kBox))
        return false;
    ::PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0x001C0001);
    ::PostMessageW(hwnd, WM_KEYUP, VK_RETURN, 0xC01C0001);
    // Gone there: the crumbs are back. The focus then is on the space after
    // them; to the file name, where it was or where the user goes on from the
    // bar, unless the user has gone on meanwhile.
    const bool gone = waitFor([&] { return !dialog.find(kBox); }, 1500ms);
    if (gone && ::GetForegroundWindow() == hwnd && dialog.focusIn(L"KcfdLocSpaceButton")) {
        const ComPtr<IUIAutomationElement> name =
            before && Dialog::classOf(before.Get()) == L"kd::KDTextField" ? before : dialog.nameBox();
        if (name)
            name->SetFocus();
    }
    return gone;
}

bool waitForLocation(HWND hwnd)
{
    const Dialog dialog(hwnd);
    return dialog && waitFor([&] { return dialog.find(L"kd::KDBreadcrumb") != nullptr; }, 1500ms);
}

bool setFileName(HWND hwnd, const std::wstring& name)
{
    const Dialog dialog(hwnd);
    const ComPtr<IUIAutomationElement> box = dialog ? dialog.nameBox() : nullptr;
    if (!box || !Dialog::setValue(box.Get(), name))
        return false;
    box->SetFocus();
    return true;
}

// Invoke waits while the program handles the press, which a message box of
// its own may hold up: given up on after a second, the press goes on.
bool accept(HWND hwnd)
{
    const Dialog dialog(hwnd);
    const ComPtr<IUIAutomationElement> button = dialog ? dialog.acceptButton() : nullptr;
    return button && Dialog::invoke(button.Get());
}

} // namespace kingsoft

} // namespace

Kind kind(HWND hwnd)
{
    if (!hwnd)
        return Kind::None;
    if (installerFolderBox(hwnd))
        return Kind::Folder;
    if (!hasClass(hwnd, L"#32770"))
        return kingsoft::known(hwnd).value_or(Kind::None);
    // Windows Vista and later: IFileDialog, and GetOpenFileName without a hook.
    if (::FindWindowExW(hwnd, nullptr, L"DUIViewWndClassName", nullptr)) {
        if (!crumbBar(hwnd))
            return Kind::None;
        const HWND combo = ::GetDlgItem(hwnd, kFileNameCombo);
        if (combo && hasClass(combo, L"ComboBoxEx32"))
            return Kind::Open;
        // A folder picker names the folder in a plain edit field; a Save
        // dialog keeps its file name box inside the view.
        return ::GetDlgItem(hwnd, kFileNameEdit) ? Kind::Folder : Kind::Save;
    }
    // The Windows XP style, still shown for GetOpenFileName with a hook or template.
    if (::FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr) && fileNameBox(hwnd) && ::GetDlgItem(hwnd, IDOK))
        return Kind::Legacy;
    if (browseTree(hwnd) && ::GetDlgItem(hwnd, IDOK))
        return Kind::Browse;
    return Kind::None;
}

bool mayBeKingsoft(HWND hwnd)
{
    wchar_t name[64] {};
    if (!hwnd || !::GetClassNameW(hwnd, name, static_cast<int>(std::size(name))))
        return false;
    const std::wstring_view className(name); // "Qt5QWindowIcon"
    if (!className.starts_with(L"Qt") || !className.ends_with(L"QWindowIcon") || !::GetWindow(hwnd, GW_OWNER))
        return false;
    const std::wstring program = kingsoft::programOf(hwnd);
    return std::ranges::any_of(kingsoft::kPrograms, [&](const wchar_t* p) { return ::_wcsicmp(program.c_str(), p) == 0; });
}

bool probed(HWND hwnd)
{
    return kingsoft::known(hwnd).has_value();
}

Kind probe(HWND hwnd)
{
    if (const std::optional<Kind> k = kingsoft::known(hwnd))
        return *k;
    const Kind k = mayBeKingsoft(hwnd) ? kingsoft::probe(hwnd) : Kind::None;
    if (::IsWindow(hwnd))
        kingsoft::remember(hwnd, k);
    return k;
}

std::wstring currentFolder(HWND dialog)
{
    if (const HWND edit = installerFolderBox(dialog))
        return controlText(edit);
    const HWND crumbs = kingsoft::isOne(dialog) ? nullptr : crumbBar(dialog);
    return crumbs ? pathtext::folderFromAddress(QString::fromStdWString(caption(crumbs))).toStdWString()
                  : std::wstring();
}

std::wstring fileType(HWND dialog)
{
    if (kind(dialog) != Kind::Open || kingsoft::isOne(dialog))
        return {};
    const HWND types = ::GetDlgItem(dialog, kFileTypeCombo); // a drop-down list: its text is the chosen entry
    return types && hasClass(types, L"ComboBox") ? controlText(types) : std::wstring();
}

bool waitForFront(HWND dialog)
{
    return waitFor([dialog] { return ::GetForegroundWindow() == dialog && focusedControl(dialog); }, 1000ms);
}

bool waitForLocation(HWND dialog)
{
    if (installerFolderBox(dialog))
        return true;
    if (kingsoft::isOne(dialog))
        return kingsoft::waitForLocation(dialog);
    const HWND crumbs = crumbBar(dialog);
    return crumbs && waitFor([crumbs] {
        DWORD_PTR buttons = 0;
        return ::SendMessageTimeoutW(crumbs, TB_BUTTONCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &buttons) && buttons > 0
            && !caption(crumbs).empty();
    }, 1500ms);
}

bool goTo(HWND dialog, const std::wstring& folder)
{
    if (const HWND edit = installerFolderBox(dialog)) {
        // Changing the edit fires Inno's PathEditChange. Do not send Enter:
        // that would accept the picker, rather than let the user review it.
        return setText(edit, folder) && controlText(edit) == folder;
    }
    if (!isFolder(folder))
        return false; // gone (or a file): nothing is typed anywhere
    const Kind k = kind(dialog);
    if (k == Kind::Legacy)
        return goByName(dialog, folder);
    if (k == Kind::Browse)
        return goByTree(dialog, folder);
    if (k == Kind::None)
        return false;
    if (kingsoft::isOne(dialog))
        return kingsoft::goTo(dialog, folder);
    if (pathtext::sameFolder(QString::fromStdWString(currentFolder(dialog)), QString::fromStdWString(folder)))
        return true;
    if (goByAddress(dialog, folder))
        return true;
    if (k == Kind::Folder)
        return false;
    qWarning() << "The file dialog's address bar did not take the folder: through the file name box instead";
    return goByName(dialog, folder);
}

bool setFileName(HWND dialog, const std::wstring& name)
{
    if (installerFolderBox(dialog))
        return false;
    if (kingsoft::isOne(dialog))
        return kingsoft::setFileName(dialog, name);
    const HWND box = fileNameBox(dialog);
    if (!box || !setText(box, name))
        return false;
    send(dialog, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(box), TRUE); // which also selects the name
    return true;
}

bool accept(HWND dialog)
{
    if (installerFolderBox(dialog))
        return false;
    if (kingsoft::isOne(dialog))
        return kingsoft::accept(dialog);
    const HWND ok = ::GetDlgItem(dialog, IDOK);
    return ok && ::PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), reinterpret_cast<LPARAM>(ok));
}

} // namespace ws::filedialog
