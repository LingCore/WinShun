#include "PathBoxDialog.h"

#include "Win32Util.h"

#include <commctrl.h>

#include <algorithm>
#include <cwchar>
#include <initializer_list>
#include <iterator>

namespace ws::pathbox {
namespace {

constexpr int kBrowseTree = 0x3741; // SHBrowseForFolder's tree, old style: DialogNav's and FileDialog's Browse kind

// 7-Zip's "Extract" (ExtractDialogRes.h).
constexpr int kSevenZipPath = 100; // IDC_EXTRACT_PATH
constexpr int kSevenZipBrowse = 101; // IDB_EXTRACT_SET_PATH
constexpr int kSevenZipPathMode = 102; // IDC_EXTRACT_PATH_MODE
constexpr int kSevenZipOverwrite = 103; // IDC_EXTRACT_OVERWRITE_MODE

// WinRAR's "Extraction path and options" (Listary.FileAppPlugin.WinRAR).
constexpr int kWinRarTabs = 0x3020;
constexpr int kWinRarPath = 0x65;
constexpr int kWinRarShow = 0x66;

// A path is read to show where the window is and to see that it took ours:
// never more of another program's text than a path can be.
constexpr std::size_t kLongestText = 1024;

bool hasClass(HWND hwnd, const wchar_t* name)
{
    wchar_t buffer[64] {};
    return hwnd && ::GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer))) > 0
        && ::wcscmp(buffer, name) == 0;
}

bool send(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    DWORD_PTR result = 0;
    return ::SendMessageTimeoutW(hwnd, msg, wParam, lParam, SMTO_ABORTIFHUNG, 2000, &result) != 0;
}

std::wstring controlText(HWND hwnd)
{
    DWORD_PTR length = 0;
    if (!::SendMessageTimeoutW(hwnd, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, 2000, &length))
        return {};
    const std::size_t wanted = std::min<std::size_t>(length, kLongestText);
    std::wstring text(wanted + 1, L'\0');
    DWORD_PTR copied = 0;
    if (!::SendMessageTimeoutW(hwnd, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()), SMTO_ABORTIFHUNG,
            2000, &copied))
        return {};
    text.resize(std::min<std::size_t>(copied, wanted));
    return text;
}

bool usable(HWND control)
{
    return control && ::IsWindowVisible(control) && ::IsWindowEnabled(control);
}

LONG_PTR styleOf(HWND control)
{
    return ::GetWindowLongPtrW(control, GWL_STYLE);
}

// The edit field of a combo box the user can type in (CBS_DROPDOWN, CBS_SIMPLE).
HWND comboEdit(HWND combo)
{
    if (!hasClass(combo, L"ComboBox") || (styleOf(combo) & 3) == CBS_DROPDOWNLIST)
        return nullptr;
    const HWND edit = ::FindWindowExW(combo, nullptr, L"Edit", nullptr);
    return edit && !(styleOf(edit) & ES_READONLY) ? edit : nullptr;
}

bool dropDownList(HWND control)
{
    return hasClass(control, L"ComboBox") && (styleOf(control) & 3) == CBS_DROPDOWNLIST;
}

bool programIs(HWND window, std::initializer_list<const wchar_t*> names)
{
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    const win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    wchar_t image[1024] {};
    DWORD length = static_cast<DWORD>(std::size(image));
    if (!process.valid() || !::QueryFullProcessImageNameW(process.get(), 0, image, &length))
        return false;
    const wchar_t* slash = ::wcsrchr(image, L'\\');
    const wchar_t* leaf = slash ? slash + 1 : image;
    return std::ranges::any_of(names, [leaf](const wchar_t* name) { return ::_wcsicmp(leaf, name) == 0; });
}

// Inno Setup uses its own folder picker, not SHBrowseForFolder. Its Start
// Menu picker has the same form class but a different tree: left alone. See
// upstream Setup.SelectFolderForm.pas (formerly TSelectDirDlg).
Box innoBox(HWND dialog)
{
    if ((!hasClass(dialog, L"TSelectFolderForm") && !hasClass(dialog, L"TSelectDirDlg"))
        || !::FindWindowExW(dialog, nullptr, L"TFolderTreeView", nullptr))
        return {};
    for (const wchar_t* name : {L"TNewPathEdit", L"TNewEdit", L"TEdit"}) {
        if (const HWND edit = ::FindWindowExW(dialog, nullptr, name, nullptr);
            usable(edit) && !(styleOf(edit) & ES_READONLY))
            return {.edit = edit};
    }
    return {};
}

Box sevenZipBox(HWND dialog)
{
    const HWND combo = ::GetDlgItem(dialog, kSevenZipPath);
    const HWND edit = comboEdit(combo);
    if (!edit || !usable(combo) || !hasClass(::GetDlgItem(dialog, kSevenZipBrowse), L"Button")
        || !dropDownList(::GetDlgItem(dialog, kSevenZipPathMode)) || !dropDownList(::GetDlgItem(dialog, kSevenZipOverwrite))
        || !::GetDlgItem(dialog, IDOK) || !programIs(dialog, {L"7zG.exe", L"7zFM.exe"}))
        return {};
    return {.edit = edit, .combo = combo};
}

// The path box is on the first page ("General"), which is hidden while the
// other one is shown: it is not asked to be visible, and takes a folder then
// as well.
Box winRarBox(HWND dialog)
{
    if (!hasClass(::GetDlgItem(dialog, kWinRarTabs), L"SysTabControl32"))
        return {};
    const HWND page = ::FindWindowExW(dialog, nullptr, L"#32770", nullptr);
    const HWND combo = page ? ::GetDlgItem(page, kWinRarPath) : nullptr;
    const HWND edit = comboEdit(combo);
    if (!edit || !::IsWindowEnabled(combo) || !programIs(dialog, {L"WinRAR.exe"}))
        return {};
    const HWND show = ::GetDlgItem(page, kWinRarShow);
    return {.edit = edit, .combo = combo, .show = hasClass(show, L"Button") ? show : nullptr};
}

// A folder tree, one box the user can type in, OK and Cancel, right in the
// dialog. Anything that makes it another kind of window, or leaves a doubt
// about which box is meant, and it is none: a list (a file dialog of some
// kind, or a settings window), two boxes, SHBrowseForFolder's own tree.
Box treeBox(HWND dialog)
{
    if (!hasClass(::GetDlgItem(dialog, IDOK), L"Button") || !hasClass(::GetDlgItem(dialog, IDCANCEL), L"Button"))
        return {};
    Box box;
    int trees = 0;
    for (HWND child = ::GetWindow(dialog, GW_CHILD); child; child = ::GetWindow(child, GW_HWNDNEXT)) {
        if (!::IsWindowVisible(child))
            continue;
        if (hasClass(child, L"SysListView32") || hasClass(child, L"SHELLDLL_DefView")
            || hasClass(child, L"DUIViewWndClassName") || hasClass(child, L"SHBrowseForFolder ShellNameSpace Control")
            || hasClass(child, L"SysTabControl32"))
            return {};
        if (hasClass(child, L"SysTreeView32")) {
            if (++trees > 1 || ::GetDlgCtrlID(child) == kBrowseTree)
                return {};
            continue;
        }
        HWND edit = nullptr;
        HWND combo = nullptr;
        if (hasClass(child, L"Edit")) {
            if (styleOf(child) & (ES_READONLY | ES_PASSWORD | ES_MULTILINE | ES_NUMBER))
                continue; // shows something, or takes something else
            edit = child;
        } else if ((edit = comboEdit(child))) {
            combo = child;
        } else {
            continue;
        }
        if (box)
            return {}; // two boxes: which one takes the folder is a guess
        if (!::IsWindowEnabled(child))
            return {};
        box = {.edit = edit, .combo = combo};
    }
    return trees == 1 ? box : Box {};
}

} // namespace

bool looksLikePath(std::wstring_view text)
{
    const auto letter = [](wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); };
    if (text.size() >= 3 && letter(text[0]) && text[1] == L':' && text[2] == L'\\')
        return true;
    // "\\server\share": a server named, and something after it.
    if (!text.starts_with(L"\\\\") || text.starts_with(L"\\\\.\\") || text.starts_with(L"\\\\?\\"))
        return false;
    const std::size_t share = text.find(L'\\', 2);
    return share != std::wstring_view::npos && share > 2 && share + 1 < text.size();
}

Box find(HWND dialog, bool known)
{
    if (!dialog)
        return {};
    if (const Box inno = innoBox(dialog))
        return inno;
    if (!hasClass(dialog, L"#32770"))
        return {};
    if (const Box sevenZip = sevenZipBox(dialog))
        return sevenZip;
    if (const Box winRar = winRarBox(dialog))
        return winRar;
    const Box box = treeBox(dialog);
    return box && (known || looksLikePath(controlText(box.edit))) ? box : Box {};
}

std::wstring folder(const Box& box)
{
    return box ? controlText(box.edit) : std::wstring();
}

bool setFolder(const Box& box, const std::wstring& folder)
{
    if (!box || folder.empty() || folder.size() > kLongestText)
        return false;
    // No entry of the list chosen: a program may take that one for the answer
    // rather than the text (7-Zip's OnOK). It empties the edit field, which
    // gets the folder next.
    if (box.combo && !send(box.combo, CB_SETCURSEL, static_cast<WPARAM>(-1), 0))
        return false;
    if (!send(box.edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(folder.c_str())) || controlText(box.edit) != folder)
        return false;
    // WM_SETTEXT is not a user edit: the owner hears of one from the combo
    // box (CBN_EDITCHANGE), as when the user types. Bandizip's tree goes to
    // the folder on that. A plain edit field tells its owner by itself
    // (EN_CHANGE: Inno Setup's PathEditChange). Posted: the box has the
    // folder already, and what the program does on hearing of it is not
    // waited for (Bandizip opens its tree down to the folder, which took it
    // 5 s for one inside a Temp folder of thousands; measured 2026-11-13).
    if (box.combo)
        ::PostMessageW(::GetParent(box.combo), WM_COMMAND, MAKEWPARAM(::GetDlgCtrlID(box.combo), CBN_EDITCHANGE),
            reinterpret_cast<LPARAM>(box.combo));
    // The program's own "show it" button, as its click: not BM_CLICK, which
    // needs its window active, and ours may be in front. Posted as well.
    if (box.show && ::IsWindowEnabled(box.show))
        ::PostMessageW(::GetParent(box.show), WM_COMMAND, MAKEWPARAM(::GetDlgCtrlID(box.show), BN_CLICKED),
            reinterpret_cast<LPARAM>(box.show));
    return true;
}

} // namespace ws::pathbox
