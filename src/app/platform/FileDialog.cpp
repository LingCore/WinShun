#include "FileDialog.h"

#include "PathText.h"

#include <QDebug>
#include <QString>

#include <commctrl.h>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <initializer_list>
#include <thread>

using namespace std::chrono_literals;

namespace ws::filedialog {

namespace {

// Controls of the common file dialogs (dlgs.h).
constexpr int kFileNameEdit = 0x480; // edt1; a folder picker's "Folder:" box
constexpr int kFileNameCombo = 0x47C; // cmb13
constexpr int kFileTypeCombo = 0x470; // cmb1

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

HWND addressBox(HWND dialog) // only while it is shown
{
    const HWND edit = descend(addressBar(dialog), {L"ComboBoxEx32", L"ComboBox", L"Edit"});
    return edit && ::IsWindowVisible(edit) ? edit : nullptr;
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
// OK with a folder in the box would choose it and close the dialog.
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
    HWND box = descend(bar, {L"ComboBoxEx32", L"ComboBox", L"Edit"});
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
            waitFor([&] { return (box = addressBox(dialog)) != nullptr; }, 300ms);
        }
        if (!box)
            return false;
    }
    if (!setText(box, folder) || !send(box, WM_KEYDOWN, VK_RETURN, 0x001C0001)
        || !send(box, WM_KEYUP, VK_RETURN, 0xC01C0001))
        return false;
    // Gone there (or it was there already): the crumbs show it, and the text
    // box has closed, which leaves the keyboard focus on the crumbs. Back to
    // where it was (usually the file name); not if the user has gone on
    // meanwhile (to the bar): the dialog would take the front back.
    waitFor([&] { return !::IsWindowVisible(box) && caption(crumbs) != shown; }, 1500ms);
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
    if (!setText(box, target) || !send(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), reinterpret_cast<LPARAM>(ok)))
        return false;
    // A dialog of Windows Vista and later goes there after OK returns, and
    // then clears the box.
    if (crumbs)
        waitFor([&] { return caption(crumbs) != shown; }, 1500ms);
    setText(box, typed);
    return true;
}

} // namespace

Kind kind(HWND hwnd)
{
    if (!hwnd || !hasClass(hwnd, L"#32770"))
        return Kind::None;
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
    return Kind::None;
}

std::wstring currentFolder(HWND dialog)
{
    const HWND crumbs = crumbBar(dialog);
    return crumbs ? pathtext::folderFromAddress(QString::fromStdWString(caption(crumbs))).toStdWString()
                  : std::wstring();
}

std::wstring fileType(HWND dialog)
{
    if (kind(dialog) != Kind::Open)
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
    const HWND crumbs = crumbBar(dialog);
    return crumbs && waitFor([crumbs] {
        DWORD_PTR buttons = 0;
        return ::SendMessageTimeoutW(crumbs, TB_BUTTONCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &buttons) && buttons > 0
            && !caption(crumbs).empty();
    }, 1500ms);
}

bool goTo(HWND dialog, const std::wstring& folder)
{
    const Kind k = kind(dialog);
    if (k == Kind::Legacy)
        return goByName(dialog, folder);
    if (k == Kind::None)
        return false;
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
    const HWND box = fileNameBox(dialog);
    if (!box || !setText(box, name))
        return false;
    send(dialog, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(box), TRUE); // which also selects the name
    return true;
}

bool accept(HWND dialog)
{
    const HWND ok = ::GetDlgItem(dialog, IDOK);
    return ok && ::PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), reinterpret_cast<LPARAM>(ok));
}

} // namespace ws::filedialog
