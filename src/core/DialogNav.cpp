#include "DialogNav.h"

#include "ServiceLocation.h"

#include <ole2.h> // before uiautomation.h
#include <commctrl.h>
#include <shellapi.h> // SHGetLocalizedName (WIN32_LEAN_AND_MEAN leaves it out of windows.h)
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

namespace ws::dialognav {
namespace {

// Controls of the common file dialogs (dlgs.h).
constexpr int kFileNameEdit = 0x480; // edt1; a folder picker's "Folder:" box
constexpr int kFileNameCombo = 0x47C; // cmb13
constexpr int kBrowseTree = 0x3741; // SHBrowseForFolder's tree, old style

bool hasClass(HWND hwnd, const wchar_t* name)
{
    wchar_t buffer[64] {};
    return hwnd && ::GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer))) > 0
        && ::wcscmp(buffer, name) == 0;
}

HWND descend(HWND hwnd, std::initializer_list<const wchar_t*> classes)
{
    for (const wchar_t* name : classes) {
        if (!hwnd)
            break;
        hwnd = ::FindWindowExW(hwnd, nullptr, name, nullptr);
    }
    return hwnd;
}

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

// Inno Setup's own destination-folder picker (not SHBrowseForFolder); its
// Start Menu picker shares the form class but a different tree, left alone.
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

HWND browseTree(HWND dialog)
{
    if (const HWND tree = descend(dialog, {L"SHBrowseForFolder ShellNameSpace Control", L"SysTreeView32"}))
        return tree;
    const HWND tree = ::GetDlgItem(dialog, kBrowseTree);
    return tree && hasClass(tree, L"SysTreeView32") ? tree : nullptr;
}

bool send(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    DWORD_PTR result = 0;
    return ::SendMessageTimeoutW(hwnd, msg, wParam, lParam, SMTO_ABORTIFHUNG, 2000, &result) != 0;
}

// The text of another program's control, read only to compare it with what we
// typed (a folder, and its trailing backslash in the address bar): at most one
// character more than that can be (so a longer text still differs), however
// long that program says it is.
std::wstring controlText(HWND hwnd)
{
    constexpr std::size_t kLimit = kMaxFolderChars + 2;
    DWORD_PTR length = 0;
    if (!::SendMessageTimeoutW(hwnd, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, 2000, &length))
        return {};
    const std::size_t wanted = std::min<std::size_t>(length, kLimit);
    std::wstring text(wanted + 1, L'\0');
    DWORD_PTR copied = 0;
    if (!::SendMessageTimeoutW(hwnd, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()),
            SMTO_ABORTIFHUNG, 2000, &copied))
        return {};
    text.resize(std::min<std::size_t>(copied, wanted));
    return text;
}

// A window's caption ("Address: <folder>" on the crumb bar), only to tell
// whether it changed. Kept by the system for another process's window (no
// message is sent), and capped all the same.
std::wstring caption(HWND hwnd)
{
    constexpr int kLimit = 4 * static_cast<int>(kMaxFolderChars);
    std::wstring text(static_cast<std::size_t>(std::clamp(::GetWindowTextLengthW(hwnd), 0, kLimit)) + 1, L'\0');
    text.resize(static_cast<std::size_t>(::GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()))));
    return text;
}

bool setText(HWND hwnd, const std::wstring& text)
{
    return send(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

template <typename Predicate> bool waitFor(Predicate done, std::chrono::milliseconds limit)
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

// Types the folder into the address bar, as in Explorer. The address bar
// never accepts the dialog (in a folder picker, OK would choose and close):
// Enter there only navigates. Mirrors FileDialog.cpp's goByAddress: the
// folder is checked again right before it is typed and goes in with a
// trailing backslash (an address bar resolves "X\" only as a folder, never as
// a file it would open); a hidden text box is taken too, as a dialog that is
// not DPI aware never shows the one a click makes; and Enter goes only once
// the box holds that folder.
bool goByAddress(HWND dialog, const std::wstring& folder)
{
    const HWND bar = addressBar(dialog);
    const HWND crumbs = crumbBar(dialog);
    if (!bar || !crumbs)
        return false;
    const HWND focus = focusedControl(dialog);
    const std::wstring shown = caption(crumbs); // "Address: <folder>"
    HWND box = addressBox(dialog);
    if (!box) {
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
    const std::wstring typed = folder.ends_with(L'\\') ? folder : folder + L'\\';
    if (!isExistingDirectory(folder.c_str()) || !setText(box, typed) || controlText(box) != typed
        || !send(box, WM_KEYDOWN, VK_RETURN, 0x001C0001) || !send(box, WM_KEYUP, VK_RETURN, 0xC01C0001))
        return false;
    waitFor([&] { return !::IsWindowVisible(box) && caption(crumbs) != shown; }, 1500ms);
    if (focus && ::GetForegroundWindow() == dialog && focusedControl(dialog) != focus && !::IsChild(bar, focus)
        && ::IsChild(dialog, focus) && ::IsWindowVisible(focus))
        send(dialog, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(focus), TRUE);
    return true;
}

// ---- UI Automation walk of SHBrowseForFolder's folder tree -------------------
// The elevated-safe route (window-message BFFM needs the path written into the
// dialog's process, which the helper may not do: OpenProcess(VM_WRITE) ->
// ERROR_ACCESS_DENIED). uiAccess lets the helper drive the higher-integrity
// tree through ExpandCollapse / SelectionItem instead (measured to work).

// The UI Automation client object, made by System32's UIAutomationCore.dll
// through its own class factory rather than CoCreateInstance: the helper runs
// at high integrity with uiAccess, and must not take the class from a
// registry lookup the user's normal programs can steer (HKCU\Software\Classes
// is theirs to write). Loaded once; it stays for the process's short life.
// CUIAutomation8 where there is one (Windows 8 and later), which alone takes
// the timeouts below (CUIAutomation answers E_NOINTERFACE for them, measured
// 2026-10-27): a dialog that hangs is given up on after them, rather than the
// 20 s a call may wait by default, past the service's 10 s for the helper.
ComPtr<IUIAutomation> createAutomation()
{
    static const HMODULE core = ::LoadLibraryExW(L"UIAutomationCore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!core)
        return nullptr;
    using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    const auto getClassObject = reinterpret_cast<GetClassObject>(::GetProcAddress(core, "DllGetClassObject"));
    if (!getClassObject)
        return nullptr;
    for (const CLSID& id : {__uuidof(CUIAutomation8), __uuidof(CUIAutomation)}) {
        ComPtr<IClassFactory> factory;
        ComPtr<IUIAutomation> uia;
        if (FAILED(getClassObject(id, IID_PPV_ARGS(&factory))) || !factory
            || FAILED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&uia))) || !uia)
            continue;
        if (ComPtr<IUIAutomation2> timed; SUCCEEDED(uia.As(&timed))) {
            timed->put_ConnectionTimeout(1000);
            timed->put_TransactionTimeout(2000);
        }
        return uia;
    }
    return nullptr;
}

// One goByTreeUia walk, handed down to every step (nothing process-wide, so
// two walks never share state). `cache` makes FindAll fetch each item's Name
// along with it, so reading it is local rather than a per-node cross-process
// call (the tree of an elevated dialog is reached over RPC; naming dozens of
// nodes one at a time is what made the walk time out).
struct Walk {
    IUIAutomation* uia = nullptr;
    IUIAutomationElement* root = nullptr; // the dialog
    IUIAutomationCacheRequest* cache = nullptr; // null: names are read live
};

std::wstring nameOf(const Walk& walk, IUIAutomationElement* element)
{
    if (!element)
        return {};
    BSTR value = nullptr;
    if (walk.cache && SUCCEEDED(element->get_CachedName(&value)) && value) {
        std::wstring result(value, ::SysStringLen(value));
        ::SysFreeString(value);
        return result;
    }
    value = nullptr;
    if (FAILED(element->get_CurrentName(&value)) || !value)
        return {};
    std::wstring result(value, ::SysStringLen(value));
    ::SysFreeString(value);
    return result;
}

bool containsNoCase(const std::wstring& haystack, const std::wstring& needle)
{
    if (needle.empty() || haystack.size() < needle.size())
        return needle.empty();
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](wchar_t a, wchar_t b) { return ::towlower(a) == ::towlower(b); });
    return it != haystack.end();
}

// Expands `node` if it can be (its children load a moment later).
void expand(IUIAutomationElement* node)
{
    ComPtr<IUIAutomationExpandCollapsePattern> pattern;
    if (FAILED(node->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&pattern))) || !pattern)
        return;
    ExpandCollapseState state = ExpandCollapseState_Collapsed;
    pattern->get_CurrentExpandCollapseState(&state);
    if (state == ExpandCollapseState_Collapsed || state == ExpandCollapseState_PartiallyExpanded)
        pattern->Expand();
}

// The TreeItem children of `parent` (the whole tree when `parent` is null).
ComPtr<IUIAutomationElementArray> treeItems(const Walk& walk, IUIAutomationElement* parent, TreeScope scope)
{
    VARIANT v;
    ::VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = UIA_TreeItemControlTypeId;
    ComPtr<IUIAutomationCondition> condition;
    if (FAILED(walk.uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &condition)) || !condition)
        return nullptr;
    ComPtr<IUIAutomationElementArray> found;
    IUIAutomationElement* from = parent ? parent : walk.root;
    if (walk.cache)
        from->FindAllBuildCache(scope, condition.Get(), walk.cache, &found);
    else
        from->FindAll(scope, condition.Get(), &found);
    return found;
}

// Scans the currently-realised tree items for the drive node ("... (C:)").
ComPtr<IUIAutomationElement> scanForDrive(const Walk& walk, const std::wstring& token)
{
    const ComPtr<IUIAutomationElementArray> items = treeItems(walk, nullptr, TreeScope_Descendants);
    int count = 0;
    if (items)
        items->get_Length(&count);
    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> item;
        if (SUCCEEDED(items->GetElement(i, &item)) && item && containsNoCase(nameOf(walk, item.Get()), token))
            return item;
    }
    return nullptr;
}

// The children (tree items) of `node`, waited for after an expand.
ComPtr<IUIAutomationElementArray> childrenOf(const Walk& walk, IUIAutomationElement* node)
{
    ComPtr<IUIAutomationElementArray> children;
    for (int round = 0; round < 10; ++round) {
        children = treeItems(walk, node, TreeScope_Children);
        int count = 0;
        if (children)
            children->get_Length(&count);
        if (count > 0)
            break;
        std::this_thread::sleep_for(60ms);
    }
    return children;
}

// The drive node ("... (C:)"). The tree may open collapsed (only Desktop / This
// PC realised) and auto-expand to the current folder a moment later, so we do
// not wait for that: a bounded breadth-first walk expands only the top two
// levels (Desktop, then its children such as This PC) until a child is the
// drive, returning as soon as it appears -- never the folders below them (the
// user folder alone can hold ~90, which used to eat the whole budget). The
// already-realised case is handled first, for free. An elevated new-style
// tree opened without a start folder may list no This PC at all (measured on
// Windows 11: Desktop shows only the user folder); then there is no drive to
// find and the caller keeps the copy fallback. Installers start it at their
// install folder (NSIS: BFFM_SETSELECTION), which puts This PC and its drives
// in the tree.
ComPtr<IUIAutomationElement> findDrive(const Walk& walk, const std::wstring& token)
{
    // Usually the tree has auto-expanded to the current folder by now, so the
    // drive is already realised (and found cheaply, without touching the tree).
    if (ComPtr<IUIAutomationElement> drive = scanForDrive(walk, token))
        return drive;
    // The tree's top items (Desktop / This PC) live under the Tree control, not
    // directly under the dialog element, so descend to the Tree control first.
    ComPtr<IUIAutomationElement> treeBase = walk.root;
    {
        VARIANT v;
        ::VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = UIA_TreeControlTypeId;
        ComPtr<IUIAutomationCondition> condition;
        if (SUCCEEDED(walk.uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &condition)) && condition) {
            ComPtr<IUIAutomationElement> tree;
            if (SUCCEEDED(walk.root->FindFirst(TreeScope_Descendants, condition.Get(), &tree)) && tree)
                treeBase = tree;
        }
    }
    std::vector<ComPtr<IUIAutomationElement>> frontier;
    if (const ComPtr<IUIAutomationElementArray> roots = treeItems(walk, treeBase.Get(), TreeScope_Children)) {
        int n = 0;
        roots->get_Length(&n);
        for (int i = 0; i < n; ++i) {
            ComPtr<IUIAutomationElement> item;
            if (SUCCEEDED(roots->GetElement(i, &item)) && item)
                frontier.push_back(item);
        }
    }
    int budget = 40; // containers to expand at most
    for (int depth = 0; depth < 2 && !frontier.empty(); ++depth) {
        std::vector<ComPtr<IUIAutomationElement>> next;
        for (const ComPtr<IUIAutomationElement>& node : frontier) {
            if (budget-- <= 0)
                return nullptr;
            if (containsNoCase(nameOf(walk, node.Get()), token))
                return node;
            expand(node.Get());
            const ComPtr<IUIAutomationElementArray> children = childrenOf(walk, node.Get());
            int count = 0;
            if (children)
                children->get_Length(&count);
            for (int i = 0; i < count; ++i) {
                ComPtr<IUIAutomationElement> child;
                if (FAILED(children->GetElement(i, &child)) || !child)
                    continue;
                if (containsNoCase(nameOf(walk, child.Get()), token))
                    return child; // the drive, as a child of This PC
                if (next.size() < 40)
                    next.push_back(child);
            }
        }
        frontier = std::move(next);
    }
    return nullptr;
}

HTREEITEM treeItem(HWND tree, UINT which, HTREEITEM from)
{
    DWORD_PTR result = 0;
    if (!::SendMessageTimeoutW(tree, TVM_GETNEXTITEM, which, reinterpret_cast<LPARAM>(from), SMTO_ABORTIFHUNG, 2000,
            &result))
        return nullptr;
    return reinterpret_cast<HTREEITEM>(result);
}

void pressArrow(HWND tree, WORD key)
{
    const LPARAM down = 1 | (static_cast<LPARAM>(::MapVirtualKeyW(key, MAPVK_VK_TO_VSC)) << 16) | (1 << 24); // extended
    send(tree, WM_KEYDOWN, key, down);
    send(tree, WM_KEYUP, key, down | (static_cast<LPARAM>(3) << 30));
}

// The new-style tree (a shell namespace control) highlights an item selected
// through UI Automation -- a programmatic, TVC_UNKNOWN change -- but does not
// tell the dialog (no BFFM_SELCHANGED; NSIS, for one, takes the folder it
// installs to from that notification). It does follow the keyboard, as when
// the user moves with the arrows. So, with the target selected, step off it
// and back: the last change onto it is then TVC_BYKEYBOARD, which it reports.
// Two arrow keys sent to the tree window and nothing else; it needs neither
// focus nor the foreground (measured). Ctrl+arrow only scrolls a tree, so a
// Ctrl still held from Ctrl+G is waited out, and every step is checked on the
// tree's caret.
bool settleByKeyboard(HWND tree, IUIAutomationSelectionItemPattern* target)
{
    BOOL selected = FALSE;
    waitFor([&] { return SUCCEEDED(target->get_CurrentIsSelected(&selected)) && selected; }, 500ms);
    const HTREEITEM item = selected ? treeItem(tree, TVGN_CARET, nullptr) : nullptr;
    if (!item)
        return false;
    const bool upFirst = treeItem(tree, TVGN_PREVIOUSVISIBLE, item) != nullptr;
    if (!upFirst && !treeItem(tree, TVGN_NEXTVISIBLE, item))
        return false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        waitFor([] { return !(::GetAsyncKeyState(VK_CONTROL) & 0x8000); }, 1500ms);
        std::this_thread::sleep_for(attempt ? 150ms : 30ms); // its key-up reaches the dialog first
        pressArrow(tree, upFirst ? VK_UP : VK_DOWN);
        if (treeItem(tree, TVGN_CARET, nullptr) == item)
            continue; // did not move (a Ctrl the dialog still sees)
        pressArrow(tree, upFirst ? VK_DOWN : VK_UP);
        if (treeItem(tree, TVGN_CARET, nullptr) == item)
            return true;
        break;
    }
    target->Select(); // at least leave the target highlighted
    return false;
}

// The child of `parent` that is the folder `path` named `name`: by that name,
// else by the localized name the tree shows for it ("用户" for C:\Users),
// looked up only then.
ComPtr<IUIAutomationElement> childNamed(
    const Walk& walk, IUIAutomationElement* parent, const std::wstring& name, const std::wstring& path)
{
    expand(parent);
    const ComPtr<IUIAutomationElementArray> items = childrenOf(walk, parent);
    int count = 0;
    if (items)
        items->get_Length(&count);
    std::vector<std::pair<ComPtr<IUIAutomationElement>, std::wstring>> named;
    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> item;
        if (FAILED(items->GetElement(i, &item)) || !item)
            continue;
        std::wstring itemName = nameOf(walk, item.Get());
        if (::_wcsicmp(itemName.c_str(), name.c_str()) == 0)
            return item;
        named.emplace_back(std::move(item), std::move(itemName));
    }
    if (named.empty())
        return nullptr;
    const std::wstring shown = localizedName(path);
    if (shown.empty())
        return nullptr;
    for (const auto& [item, itemName] : named) {
        if (::_wcsicmp(itemName.c_str(), shown.c_str()) == 0)
            return item;
    }
    return nullptr;
}

bool goByTreeUia(HWND dialog, const std::wstring& folder)
{
    // The new style (BIF_NEWDIALOGSTYLE) hosts a shell namespace control
    // around the tree; it needs the keyboard settle after the select (above).
    const HWND namespaceHost = ::FindWindowExW(dialog, nullptr, L"SHBrowseForFolder ShellNameSpace Control", nullptr);

    // folder is an existing absolute local path, e.g. "C:\Program Files\Oray".
    std::wstring path = folder;
    while (!path.empty() && path.back() == L'\\')
        path.pop_back();
    if (path.size() < 2 || path[1] != L':')
        return false; // only drive-letter paths; UNC / libraries not handled

    std::vector<std::wstring> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\') {
            if (i > start)
                parts.emplace_back(path.substr(start, i - start));
            start = i + 1;
        }
    }
    if (parts.empty())
        return false;

    const ComPtr<IUIAutomation> uia = createAutomation();
    if (!uia)
        return false;
    // Cache each tree item's Name so the walk reads it locally.
    ComPtr<IUIAutomationCacheRequest> cache;
    uia->CreateCacheRequest(&cache);
    if (cache) {
        cache->AddProperty(UIA_NamePropertyId);
        cache->put_AutomationElementMode(AutomationElementMode_Full); // still expandable / navigable
    }
    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(dialog, &root)) || !root)
        return false;
    const Walk walk {uia.Get(), root.Get(), cache.Get()};

    const std::wstring driveToken = L"(" + parts.front() + L")"; // "(C:)"
    ComPtr<IUIAutomationElement> node = findDrive(walk, driveToken);
    if (!node)
        return false;
    std::wstring prefix = parts.front(); // "C:"
    for (std::size_t i = 1; i < parts.size(); ++i) {
        prefix += L'\\';
        prefix += parts[i];
        node = childNamed(walk, node.Get(), parts[i], prefix);
        if (!node)
            return false;
    }
    if (ComPtr<IUIAutomationScrollItemPattern> scroll;
        SUCCEEDED(node->GetCurrentPatternAs(UIA_ScrollItemPatternId, IID_PPV_ARGS(&scroll))) && scroll)
        scroll->ScrollIntoView();
    // Checked again right before the one action that picks it.
    ComPtr<IUIAutomationSelectionItemPattern> select;
    if (!isExistingDirectory(folder.c_str())
        || FAILED(node->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&select))) || !select
        || FAILED(select->Select()))
        return false;
    if (!namespaceHost)
        return true; // the old-style tree follows the select
    const HWND tree = ::FindWindowExW(namespaceHost, nullptr, L"SysTreeView32", nullptr);
    return tree && settleByKeyboard(tree, select.Get());
}

} // namespace

Kind classify(HWND dialog)
{
    if (!dialog)
        return Kind::None;
    if (installerFolderBox(dialog))
        return Kind::Folder;
    if (!hasClass(dialog, L"#32770"))
        return Kind::None;
    // Windows Vista and later: IFileDialog, and GetOpenFileName without a hook.
    if (::FindWindowExW(dialog, nullptr, L"DUIViewWndClassName", nullptr)) {
        if (!crumbBar(dialog))
            return Kind::None;
        const HWND combo = ::GetDlgItem(dialog, kFileNameCombo);
        if (combo && hasClass(combo, L"ComboBoxEx32"))
            return Kind::Open;
        return ::GetDlgItem(dialog, kFileNameEdit) ? Kind::Folder : Kind::Save;
    }
    if (::FindWindowExW(dialog, nullptr, L"SHELLDLL_DefView", nullptr) && ::GetDlgItem(dialog, IDOK)) {
        // The Windows XP style: no address bar, so only the file-name box with
        // OK would navigate -- left to the copy-only fallback (we never press OK).
        return Kind::Legacy;
    }
    if (browseTree(dialog) && ::GetDlgItem(dialog, IDOK))
        return Kind::Browse;
    return Kind::None;
}

bool isExistingDirectory(const wchar_t* folder)
{
    if (!folder || !*folder)
        return false;
    const DWORD attributes = ::GetFileAttributesW(folder);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring localizedName(const std::wstring& folder)
{
    // desktop.ini's LocalizedResourceName: "%SystemRoot%\system32\shell32.dll"
    // and 21813 (measured 2026-10-27: positive already, the environment
    // variables left in). Expanded with this process's own environment, whose
    // system variables the service fixed for its helpers.
    wchar_t module[MAX_PATH] {};
    int id = 0;
    if (FAILED(::SHGetLocalizedName(folder.c_str(), module, MAX_PATH, &id)) || !module[0])
        return {};
    const long long resource = id < 0 ? -static_cast<long long>(id) : id; // "@module,-id" kept its minus
    if (resource <= 0 || resource > 0xFFFF)
        return {};
    wchar_t expanded[MAX_PATH] {};
    const DWORD length = ::ExpandEnvironmentStringsW(module, expanded, MAX_PATH);
    if (!length || length > MAX_PATH)
        return {};
    // A local absolute path, or a bare name for System32's.
    std::wstring path = expanded;
    if (path.find_first_of(L"\\/:") == std::wstring::npos) {
        wchar_t system[MAX_PATH] {};
        const UINT n = ::GetSystemDirectoryW(system, MAX_PATH);
        if (!n || n >= MAX_PATH)
            return {};
        path = std::wstring(system) + L'\\' + path;
    }
    if (path.size() < 3 || path[1] != L':' || path[2] != L'\\')
        return {};
    // Strings from a module only administrators can change (desktop.ini is the
    // user's to write), mapped as data: no code of it runs.
    if (!svcloc::pathProtected(path))
        return {};
    const HMODULE strings = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!strings)
        return {};
    wchar_t text[256] {};
    const int copied = ::LoadStringW(strings, static_cast<UINT>(resource), text, static_cast<int>(std::size(text)));
    ::FreeLibrary(strings);
    return copied > 0 ? std::wstring(text, static_cast<std::size_t>(copied)) : std::wstring();
}

bool isValidFolderRequest(std::wstring_view folder)
{
    if (folder.empty() || folder.size() > kMaxFolderChars)
        return false;
    for (std::size_t i = 0; i < folder.size(); ++i) {
        const wchar_t c = folder[i];
        if (c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c == L'?'
            || c == L'*' || c == L'/' || (c == L':' && i != 1))
            return false;
    }
    std::size_t rest = 0; // where the segments after the root start
    if (folder.size() >= 3 && ((folder[0] >= L'A' && folder[0] <= L'Z') || (folder[0] >= L'a' && folder[0] <= L'z'))
        && folder[1] == L':' && folder[2] == L'\\') {
        rest = 3; // "C:\..."
    } else if (folder.size() > 2 && folder[0] == L'\\' && folder[1] == L'\\') {
        // "\\server\share\...": a named server, the share checked below as the
        // first segment; "\\.\" is the device namespace (and "\\?\" never gets
        // here: '?' is refused above).
        const std::size_t serverEnd = folder.find(L'\\', 2);
        if (serverEnd == std::wstring_view::npos || serverEnd == 2 || serverEnd + 1 == folder.size())
            return false;
        if (const std::wstring_view server = folder.substr(2, serverEnd - 2); server == L"." || server == L"..")
            return false;
        rest = serverEnd + 1;
    } else {
        return false; // relative, drive-relative ("C:x"), rooted ("\x") or with a ':' elsewhere
    }
    // Each segment named: no ".", "..", nor an empty one but after a trailing '\'.
    for (std::size_t start = rest; start < folder.size();) {
        const std::size_t end = std::min(folder.find(L'\\', start), folder.size());
        const std::wstring_view segment = folder.substr(start, end - start);
        if (segment.empty() || segment == L"." || segment == L"..")
            return false;
        start = end + 1;
    }
    return true;
}

std::wstring quoteArgument(std::wstring_view argument)
{
    // CommandLineToArgvW / the CRT: 2n backslashes then a quote are n
    // backslashes and a delimiter; 2n+1 then a quote, n and a literal quote;
    // backslashes not before a quote are literal.
    std::wstring quoted(1, L'"');
    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        for (; i < argument.size() && argument[i] == L'\\'; ++i)
            ++backslashes;
        if (i == argument.size()) {
            quoted.append(backslashes * 2, L'\\'); // before the closing quote
            break;
        }
        quoted.append(argument[i] == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        quoted += argument[i];
    }
    quoted += L'"';
    return quoted;
}

bool navigateToFolder(HWND dialog, const std::wstring& folder)
{
    if (!dialog || !::IsWindow(dialog) || !isValidFolderRequest(folder) || !isExistingDirectory(folder.c_str()))
        return false;
    if (const HWND edit = installerFolderBox(dialog)) {
        // Inno fires PathEditChange on the edit; never send Enter (that accepts
        // the picker). Checked again right before it goes in; confirm it took.
        return isExistingDirectory(folder.c_str()) && setText(edit, folder) && controlText(edit) == folder;
    }
    switch (classify(dialog)) {
    case Kind::Open:
    case Kind::Save:
    case Kind::Folder:
        return goByAddress(dialog, folder);
    case Kind::Browse:
        return goByTreeUia(dialog, folder);
    case Kind::Legacy:
    case Kind::None:
        return false;
    }
    return false;
}

} // namespace ws::dialognav
