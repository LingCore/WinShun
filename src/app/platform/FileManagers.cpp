#include "FileManagers.h"
#include "SearchTyping.h"

#include "FolderViewHit.h"
#include "Integrity.h"
#include "OneCommanderTabs.h"
#include "TessoaQuery.h"
#include "PathText.h"
#include "Shell.h"
#include "Unc.h"
#include "Win32Util.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#include <ole2.h> // before exdisp.h: WIN32_LEAN_AND_MEAN keeps it out of windows.h

#include <exdisp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

using Microsoft::WRL::ComPtr;
using namespace Qt::StringLiterals;

namespace ws::filemanager {

namespace {

constexpr UINT kTotalCommanderAsk = WM_USER + 50; // its interface for other programs
// XYplorer's WM_COPYDATA: dwData says what lpData is. A script to run (as
// its /script= switch takes it, "::" first); what its `copydata` command
// sends in mode 0, its answer to a window of ours.
constexpr ULONG_PTR kXYplorerScript = 0x00400001;
constexpr ULONG_PTR kXYplorerAnswer = 0x00400000;
constexpr DWORD kXYplorerWait = 3000; // ms for a script to finish: it answers while the message is being sent

// A window's own text, as Windows keeps it: no message to the program.
std::wstring caption(HWND hwnd)
{
    std::wstring text(static_cast<std::size_t>(::GetWindowTextLengthW(hwnd)) + 1, L'\0');
    text.resize(static_cast<std::size_t>(::GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()))));
    return text;
}

bool isFolder(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool isFile(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring quoted(const std::wstring& path)
{
    return L'"' + path + L'"';
}

std::wstring folderOf(const std::wstring& file)
{
    const std::size_t slash = file.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : file.substr(0, slash);
}

// The program file of the process a window belongs to.
std::wstring programOf(HWND window)
{
    DWORD pid = 0;
    if (!window || !::GetWindowThreadProcessId(window, &pid) || pid == 0)
        return {};
    const win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!process.valid() || !::QueryFullProcessImageNameW(process.get(), 0, path.data(), &size))
        return {};
    path.resize(size);
    return path;
}

// A text value, %VARIABLES% expanded; empty when there is none. `flags`:
// which registry view (RRF_SUBKEY_WOW6432KEY for a 32-bit program's).
std::wstring registryText(HKEY root, const wchar_t* key, const wchar_t* value, DWORD flags = 0)
{
    flags |= RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
    DWORD size = 0;
    if (::RegGetValueW(root, key, value, flags, nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t))
        return {};
    std::wstring text(size / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(root, key, value, flags, nullptr, text.data(), &size) != ERROR_SUCCESS)
        return {};
    text.resize(::wcsnlen(text.c_str(), text.size()));
    return text;
}

// A portable copy (on a USB stick, in a folder of the user's) leaves nothing
// in the registry: it was known only while it ran, so folders could not be
// opened in a Total Commander that was closed (a chat report, 2026-11-27).
// The program a running one was seen as is kept in the state file
// ([FileManagers]) and used while that file is still there.
struct ProgramMemory {
    QMutex mutex;
    QString file; // none: nothing kept (tests, the previews)
    QHash<QString, QString> seen; // as read and as written
};
ProgramMemory& programMemory()
{
    static ProgramMemory memory;
    return memory;
}

std::wstring remembering(const char* name, std::wstring running)
{
    ProgramMemory& memory = programMemory();
    const QMutexLocker lock(&memory.mutex);
    const QString key = QString::fromLatin1(name);
    const QString path = QString::fromStdWString(running);
    if (!memory.file.isEmpty() && memory.seen.value(key).compare(path, Qt::CaseInsensitive) != 0) {
        memory.seen.insert(key, path);
        QSettings state(memory.file, QSettings::IniFormat);
        state.setValue(u"FileManagers/"_s + key, path);
    }
    return running;
}

std::wstring remembered(const char* name)
{
    ProgramMemory& memory = programMemory();
    const QMutexLocker lock(&memory.mutex);
    const std::wstring path = memory.seen.value(QString::fromLatin1(name)).toStdWString();
    return !path.empty() && isFile(path) ? path : std::wstring();
}

std::wstring totalCommanderProgram()
{
    if (const HWND running = ::FindWindowW(L"TTOTAL_CMD", nullptr)) {
        if (std::wstring program = programOf(running); !program.empty())
            return remembering("TotalCommander", std::move(program));
    }
    constexpr const wchar_t* kKey = L"Software\\Ghisler\\Total Commander";
    for (const std::wstring& dir : {registryText(HKEY_CURRENT_USER, kKey, L"InstallDir"),
             registryText(HKEY_LOCAL_MACHINE, kKey, L"InstallDir", RRF_SUBKEY_WOW6464KEY),
             registryText(HKEY_LOCAL_MACHINE, kKey, L"InstallDir", RRF_SUBKEY_WOW6432KEY)}) {
        if (dir.empty())
            continue;
        for (const wchar_t* name : {L"\\TOTALCMD64.EXE", L"\\TOTALCMD.EXE"}) {
            if (isFile(dir + name))
                return dir + name;
        }
    }
    return remembered("TotalCommander");
}

std::wstring opusProgram()
{
    std::wstring dopus = programOf(::FindWindowW(L"dopus.lister", nullptr));
    constexpr const wchar_t* kKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\dopus.exe";
    for (const HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        if (dopus.empty())
            dopus = registryText(root, kKey, nullptr);
    }
    if (dopus.size() >= 2 && dopus.front() == L'"' && dopus.back() == L'"')
        dopus = dopus.substr(1, dopus.size() - 2);
    const std::wstring rt = folderOf(dopus) + L"\\dopusrt.exe";
    return !dopus.empty() && isFile(rt) ? rt : std::wstring();
}

// Starts `program` with the user's normal rights. `wait`: until it has
// handed its command over, so that the next one comes after it (a program
// that keeps running, a Total Commander started this way, is waited for a
// few seconds).
bool run(const std::wstring& program, const std::wstring& args, bool wait, const std::wstring& cwd = {})
{
    const win32::UniqueHandle process = shell::startUnelevated(program, args, cwd);
    if (!process.valid()) {
        qWarning() << "Could not start" << QString::fromStdWString(program);
        return false;
    }
    if (wait)
        ::WaitForSingleObject(process.get(), 3000);
    return true;
}

// The file name of a program's path, "XYplorer.exe".
std::wstring_view baseName(std::wstring_view path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? path : path.substr(slash + 1);
}

// XYplorer's own executable: standard/portable ("XYplorer.exe"), Free
// ("XYplorerFree.exe"), 64-bit/repacked names ("XYplorer64.exe",
// "XYplorerPortable.exe"); excludes helper binaries (XYcopy.exe, XY64*.exe).
bool isXYplorerExecutable(std::wstring_view name)
{
    constexpr std::wstring_view kPrefix = L"XYplorer";
    constexpr std::wstring_view kSuffix = L".exe";
    if (name.size() < kPrefix.size() + kSuffix.size())
        return false;
    return win32::equalsIgnoreCase(name.substr(0, kPrefix.size()), kPrefix)
        && win32::equalsIgnoreCase(name.substr(name.size() - kSuffix.size()), kSuffix);
}

// Whether `window` is the main window of an XYplorer (both 32-bit VB6 and
// 64-bit twinBASIC use ThunderRT6FormDC, owned by the runtime's ThunderRT6Main
// window or unowned, while XYplorer's own dialogs are owned by the form).
bool isXYplorerMain(HWND window)
{
    const HWND owner = ::GetWindow(window, GW_OWNER);
    wchar_t name[32] {};
    if (owner && (::GetClassNameW(owner, name, static_cast<int>(std::size(name))) == 0 || ::wcscmp(name, L"ThunderRT6Main") != 0))
        return false;
    return isXYplorerExecutable(baseName(programOf(window)));
}

// An XYplorer that runs, by the runtime's hidden window of its process (there
// whether its main window is shown or in the tray); null if none.
HWND xyplorerRuntimeWindow()
{
    for (HWND hwnd = nullptr; (hwnd = ::FindWindowExW(nullptr, hwnd, L"ThunderRT6Main", nullptr));) {
        if (isXYplorerExecutable(baseName(programOf(hwnd))))
            return hwnd;
    }
    return nullptr;
}

// The main window of an XYplorer in front of the others; null when none is open.
HWND xyplorerWindow()
{
    HWND found = nullptr;
    ::EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            if (!::IsWindowVisible(hwnd) || kindOf(hwnd) != Kind::XYplorer)
                return TRUE;
            *reinterpret_cast<HWND*>(param) = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

std::wstring xyplorerProgram()
{
    if (std::wstring program = programOf(xyplorerRuntimeWindow()); !program.empty())
        return remembering("XYplorer", std::move(program));
    if (std::wstring program = programOf(xyplorerWindow()); !program.empty())
        return remembering("XYplorer", std::move(program));
    for (const wchar_t* key : {L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\XYplorer.exe",
             L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\XYplorerFree.exe"}) {
        for (std::wstring exe : {registryText(HKEY_CURRENT_USER, key, nullptr),
                 registryText(HKEY_LOCAL_MACHINE, key, nullptr, RRF_SUBKEY_WOW6464KEY),
                 registryText(HKEY_LOCAL_MACHINE, key, nullptr, RRF_SUBKEY_WOW6432KEY)}) {
            if (exe.size() >= 2 && exe.front() == L'"' && exe.back() == L'"')
                exe = exe.substr(1, exe.size() - 2);
            if (!exe.empty() && isFile(exe))
                return exe;
        }
    }
    return remembered("XYplorer"); // the portable one
}

// The window XYplorer answers to (askXYplorer): what it sends is kept in
// the std::wstring the window's user data points to.
LRESULT CALLBACK xyplorerAnswerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg != WM_COPYDATA)
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
    auto* answer = reinterpret_cast<std::wstring*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (answer && data && data->dwData == kXYplorerAnswer && data->lpData) {
        // UTF-16, cbData the bytes without a terminator (27.20); should one come, it goes.
        std::wstring_view text(static_cast<const wchar_t*>(data->lpData), data->cbData / sizeof(wchar_t));
        while (!text.empty() && text.back() == L'\0')
            text.remove_suffix(1);
        answer->append(text);
    }
    return TRUE;
}

struct XYplorerAsked {
    bool delivered = false; // the script reached it (it may still be at it: a slow folder)
    DWORD error = ERROR_SUCCESS;
    std::wstring answer; // what it sent to our window (copydata <ours>, …, 0), in order
};

// Runs a script in the XYplorer window `window`: `script` makes it from
// our window's handle (decimal), for its answers. XYplorer runs it while
// the message is being sent, and its answers arrive meanwhile. Serialized
// across worker threads (DialogBar + DialogJump auto-jump both query on dialog
// open) so XYplorer's single-threaded script engine is never re-entered mid-
// SendMessage. Not delivered: it is gone, hung, or runs elevated while Win顺
// does not (UIPI refuses the message).
XYplorerAsked askXYplorer(HWND window, const std::function<std::wstring(const std::wstring& ours)>& script,
    bool warnOnError = true)
{
    static std::mutex g_xyplorerMutex;
    const std::lock_guard lock(g_xyplorerMutex);
    constexpr const wchar_t* kClass = L"WinShun.XYplorerAnswer";
    static std::once_flag registered;
    std::call_once(registered, [] {
        WNDCLASSEXW wc {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = xyplorerAnswerProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        ::RegisterClassExW(&wc);
    });
    XYplorerAsked asked;
    const HWND ours = ::CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!ours)
        return asked;
    ::SetWindowLongPtrW(ours, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&asked.answer));
    // XYplorer runs with the user's rights; Win顺 may be elevated (管理员直读).
    ::ChangeWindowMessageFilterEx(ours, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    // USER handles are 32-bit sign-extended on x64; truncate to uint32 so bit-31
    // handles never become 64-bit decimal strings that overflow XYplorer's copydata.
    std::wstring text = script(std::to_wstring(HandleToULong(ours)));
    COPYDATASTRUCT data {};
    data.dwData = kXYplorerScript;
    data.cbData = static_cast<DWORD>(text.size() * sizeof(wchar_t)); // no terminator: it would be read as a line
    data.lpData = text.data();
    DWORD_PTR result = 0;
    const bool sent = ::SendMessageTimeoutW(window, WM_COPYDATA, reinterpret_cast<WPARAM>(ours),
                          reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, kXYplorerWait, &result)
        != 0;
    asked.error = sent ? ERROR_SUCCESS : ::GetLastError();
    ::DestroyWindow(ours);
    asked.delivered = sent || asked.error == ERROR_TIMEOUT;
    if (warnOnError && !asked.delivered) {
        static std::atomic<bool> warned {false};
        if (!warned.exchange(true))
            qWarning() << "XYplorer did not take a script (error" << asked.error
                       << (asked.error == ERROR_ACCESS_DENIED ? "): it runs as administrator while Win顺 does not" : ")");
    } else if (!sent && asked.delivered) {
        qWarning() << "XYplorer took more than" << kXYplorerWait << "ms over a script";
    }
    return asked;
}

// Bounded cross-process WM_GETTEXT for a child control (empty on hang/UIPI).
std::wstring controlText(HWND hwnd, UINT timeout = 300)
{
    DWORD_PTR length = 0;
    if (!::SendMessageTimeoutW(hwnd, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, timeout, &length) || length == 0
        || length > 32767)
        return {};
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    DWORD_PTR copied = 0;
    if (!::SendMessageTimeoutW(hwnd, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()), SMTO_ABORTIFHUNG,
            timeout, &copied))
        return {};
    text.resize(std::min<std::size_t>(static_cast<std::size_t>(copied), static_cast<std::size_t>(length)));
    return text;
}

// Resolves XYplorer's bare special-folder address-bar labels ("Desktop",
// "Documents", "桌面", "文档", ...) to their real disk folder. Used only on
// the primary address bar (never the live-filter box or window title).
QString xyplorerKnownFolder(QString text)
{
    if (const qsizetype search = text.indexOf(u'?'); search >= 0)
        text.truncate(search);
    text = text.trimmed();
    if (text.isEmpty())
        return {};
    const auto known = [](const KNOWNFOLDERID& id) -> QString {
        PWSTR path = nullptr;
        const HRESULT hr = ::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path);
        const std::wstring folder = SUCCEEDED(hr) && path ? path : L"";
        ::CoTaskMemFree(path);
        return !folder.empty() && isFolder(folder) ? QString::fromStdWString(folder) : QString();
    };
    const auto isAny = [&](std::initializer_list<QStringView> names) {
        return std::ranges::any_of(names, [&](QStringView name) { return text.compare(name, Qt::CaseInsensitive) == 0; });
    };
    if (isAny({u"Desktop", u"桌面"}))
        return known(FOLDERID_Desktop);
    if (isAny({u"Documents", u"Personal", u"My Documents", u"文档", u"我的文档"}))
        return known(FOLDERID_Documents);
    if (isAny({u"Downloads", u"下载"}))
        return known(FOLDERID_Downloads);
    if (isAny({u"Pictures", u"My Pictures", u"图片"}))
        return known(FOLDERID_Pictures);
    if (isAny({u"Music", u"My Music", u"音乐"}))
        return known(FOLDERID_Music);
    if (isAny({u"Videos", u"My Videos", u"视频"}))
        return known(FOLDERID_Videos);
    return {};
}

// Fallback 1 for XYplorer: read its visible address bar (ThunderRT6FormDC >
// ThunderRT6PictureBoxDC > Edit), which tracks the active pane in both single-
// and dual-pane modes across 32-bit VB6 (27.x) and 64-bit twinBASIC (28.x).
// Used when WM_COPYDATA scripting does not answer (XYplorerFree, modal popup).
QString xyplorerAddressFolder(HWND window)
{
    QString bestPath;
    LONG bestPathWidth = -1;
    LONG bestPathLeft = 0;
    QString widestRaw;
    LONG widestWidth = -1;
    LONG widestLeft = 0;
    for (HWND box = ::FindWindowExW(window, nullptr, L"ThunderRT6PictureBoxDC", nullptr); box;
        box = ::FindWindowExW(window, box, L"ThunderRT6PictureBoxDC", nullptr)) {
        if (!::IsWindowVisible(box))
            continue;
        for (HWND edit = ::FindWindowExW(box, nullptr, L"Edit", nullptr); edit;
            edit = ::FindWindowExW(box, edit, L"Edit", nullptr)) {
            if (!::IsWindowVisible(edit))
                continue;
            RECT rect {};
            const LONG width = ::GetWindowRect(edit, &rect) ? std::max<LONG>(0, rect.right - rect.left) : 0;
            const QString raw = QString::fromStdWString(controlText(edit));
            // The address bar sits left of and wider than the live-filter Edit.
            if (widestWidth < 0 || width > widestWidth || (width == widestWidth && rect.left < widestLeft)) {
                widestRaw = raw;
                widestWidth = width;
                widestLeft = rect.left;
            }
            const QString folder = pathtext::xyplorerFolderFromAddress(raw);
            if (folder.isEmpty() || !isFolder(folder.toStdWString()))
                continue;
            if (bestPath.isEmpty() || width > bestPathWidth || (width == bestPathWidth && rect.left < bestPathLeft)) {
                bestPath = folder;
                bestPathWidth = width;
                bestPathLeft = rect.left;
            }
        }
    }
    if (!bestPath.isEmpty())
        return bestPath;
    return widestWidth > 0 ? xyplorerKnownFolder(widestRaw) : QString();
}

// Fallback 2 for XYplorer: top-level window caption via GetWindowTextW.
// Reads the kernel caption without cross-process messages, so it works even
// when XYplorer runs elevated (UIPI blocks WM_COPYDATA and WM_GETTEXT) or the
// address bar is hidden.
QString xyplorerTitleFolder(HWND window)
{
    const QString folder = pathtext::folderFromXYplorerTitle(QString::fromStdWString(caption(window)));
    return !folder.isEmpty() && isFolder(folder.toStdWString()) ? folder : QString();
}

// The folders an XYplorer window shows, as pathtext::xyplorerTabs reads
// its answer: each pane's tabs (the active pane first, the tab on top
// first), the other pane's only while it is shown. Falls back to the visible
// address bar and window title when scripting is unavailable or blocked.
QList<pathtext::XYplorerTab> xyplorerTabs(HWND window)
{
    const bool elevated = integrity::higherThanOurs(window);
    if (!elevated) {
        const XYplorerAsked asked = askXYplorer(
            window,
            [](const std::wstring& ours) {
                // pathreal(): a tab's path may be a special one ("Documents", the
                // user's name for the profile). Flag e: no empty token, whose
                // pathreal() would be the item focused.
                return L"::$a = ''; foreach($t, get('tabs_sf', '|', 'a'), '|', 'e') { $a = $a . '|' . pathreal($t); }"
                       L" $b = ''; foreach($t, get('tabs_sf', '|', 'i'), '|', 'e') { $b = $b . '|' . pathreal($t); }"
                       L" copydata "
                    + ours + L", dualpane() . \"<crlf>\" . $a . \"<crlf>\" . $b, 0;";
            },
            false);
        if (asked.delivered && !asked.answer.empty()) {
            QList<pathtext::XYplorerTab> tabs = pathtext::xyplorerTabs(QString::fromStdWString(asked.answer));
            tabs.removeIf([](const pathtext::XYplorerTab& tab) { return !isFolder(tab.path.toStdWString()); }); // archives, gone
            if (!tabs.isEmpty())
                return tabs;
        }
        if (asked.error != ERROR_ACCESS_DENIED) {
            if (QString folder = xyplorerAddressFolder(window); !folder.isEmpty()) {
                qInfo() << "XYplorer: read active folder from address bar fallback";
                return {{std::move(folder), 1}};
            }
        }
        if (QString folder = xyplorerTitleFolder(window); !folder.isEmpty()) {
            qInfo() << "XYplorer: read active folder from window title fallback";
            return {{std::move(folder), 1}};
        }
        if (!asked.delivered) {
            static std::atomic<bool> warned {false};
            if (!warned.exchange(true))
                qWarning() << "XYplorer did not take a script (error" << asked.error
                           << (asked.error == ERROR_ACCESS_DENIED ? "): it runs as administrator while Win顺 does not" : ")");
        } else if (asked.answer.empty()) {
            qWarning() << "XYplorer did not say which folders it shows";
        }
        return {};
    }
    if (QString folder = xyplorerTitleFolder(window); !folder.isEmpty()) {
        qInfo() << "XYplorer (elevated): read active folder from window title";
        return {{std::move(folder), 1}};
    }
    static std::atomic<bool> warnedElevated {false};
    if (!warnedElevated.exchange(true))
        qWarning() << "XYplorer runs as administrator while Win顺 does not, and its title bar shows no folder path";
    return {};
}

// A script that brings the tab showing `folder` in the active pane to the
// front, or opens one, and selects `names` there (none: nothing). Scripts
// run in order: the list is loaded by the time it is selected in.
std::wstring xyplorerGoScript(const QString& folder, const QStringList& names)
{
    QString key = folder.toLower();
    while (key.size() > 2 && key.endsWith(u'\\'))
        key.chop(1); // pathreal() gives "C:" for a drive
    QString script = u"::$p = %1; $k = 0; $n = 0; foreach($t, get('tabs', '|', 'a'), '|', 'e') { $n++;"
                     u" if ($k == 0 && recase(pathreal($t)) == $p) { $k = $n; } }"
                     u" if ($k > 0) { seltab $k; } else { tab('new', %2); }"_s.arg(
        pathtext::xyplorerQuoted(key), pathtext::xyplorerQuoted(folder));
    if (!names.isEmpty())
        script += u" selectitems %1, , 1;"_s.arg(pathtext::xyplorerQuoted(names.join(u'|')));
    return (script + u" focus 'XY';"_s).toStdWString();
}

// XYplorer's window to the front, as Total Commander and Opus come by
// themselves: the user just pressed Enter in Win顺, which may hand the front on.
void bringToFront(HWND window)
{
    if (::IsIconic(window))
        ::ShowWindowAsync(window, SW_RESTORE);
    if (::GetForegroundWindow() != window)
        ::SetForegroundWindow(window);
}

// OneCommander's public integration points, supplied by its author:
// github.com/m1l/AutomationTest/blob/main/OCInteractionTest/OCInteractionTest/MainWindow.xaml.cs
// Read the active pane live, including after focus has moved to a dialog.
// Never focus a control, simulate typing or use the clipboard to ask it.
class OneCommanderAutomation {
public:
    explicit OneCommanderAutomation(HWND window)
    {
        if (kindOf(window) != Kind::OneCommander
            || FAILED(
                ::CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_uia))))
            return;
        ComPtr<IUIAutomation2> timed;
        if (FAILED(m_uia.As(&timed)) || FAILED(timed->put_ConnectionTimeout(1000))
            || FAILED(timed->put_TransactionTimeout(1000)))
            return;
        m_uia->ElementFromHandle(window, &m_root);
    }

    std::wstring folder() const
    {
        const auto pattern = value(L"CurrentPathGet");
        BSTR text = nullptr;
        const HRESULT result = pattern ? pattern->get_CurrentValue(&text) : E_FAIL;
        const Text owned(text, ::SysFreeString);
        if (FAILED(result) || !owned)
            return {};
        std::wstring path(owned.get(), ::SysStringLen(owned.get()));
        return isFolder(path) ? path : std::wstring();
    }

    // A file path selects that item in its folder; a folder navigates there.
    bool goTo(const std::wstring& path) const
    {
        const auto pattern = value(L"CurrentPathSet");
        const Text text(::SysAllocStringLen(path.data(), static_cast<UINT>(path.size())), ::SysFreeString);
        return pattern && text && SUCCEEDED(pattern->SetValue(text.get()));
    }

    // What has the keyboard (onecommander::typingFocus), its file list's
    // place on the screen with it. Other: not in this window, or no answer.
    std::pair<onecommander::TypingFocus, RECT> typingFocus(HWND window) const
    {
        constexpr std::pair<onecommander::TypingFocus, RECT> other {onecommander::TypingFocus::Other, RECT {}};
        ComPtr<IUIAutomationElement> element;
        ComPtr<IUIAutomationTreeWalker> walker;
        DWORD process = 0;
        int focusProcess = 0;
        if (!m_root || !::GetWindowThreadProcessId(window, &process) || FAILED(m_uia->GetFocusedElement(&element))
            || !element || FAILED(element->get_CurrentProcessId(&focusProcess))
            || static_cast<DWORD>(focusProcess) != process || FAILED(m_uia->get_ControlViewWalker(&walker)))
            return other;
        struct Step {
            ComPtr<IUIAutomationElement> element;
            CONTROLTYPEID type = 0;
            std::wstring id;
            std::wstring className;
        };
        std::vector<Step> steps;
        for (int i = 0; element && i < 16; ++i) {
            Step step;
            step.element = element;
            BSTR text = nullptr;
            element->get_CurrentControlType(&step.type);
            if (SUCCEEDED(element->get_CurrentAutomationId(&text)) && text)
                step.id.assign(text, ::SysStringLen(text));
            ::SysFreeString(text);
            text = nullptr;
            if (SUCCEEDED(element->get_CurrentClassName(&text)) && text)
                step.className.assign(text, ::SysStringLen(text));
            ::SysFreeString(text);
            UIA_HWND handle = nullptr;
            const bool top = SUCCEEDED(element->get_CurrentNativeWindowHandle(&handle))
                && static_cast<HWND>(handle) == window;
            steps.push_back(std::move(step));
            if (top) {
                std::vector<onecommander::FocusStep> chain;
                for (const Step& s : steps)
                    chain.push_back({static_cast<long>(s.type), s.id, s.className});
                const onecommander::TypingFocus focus = onecommander::typingFocus(chain);
                RECT list {};
                if (focus == onecommander::TypingFocus::FileList) {
                    const Step& box = steps[0].type == onecommander::kListControl ? steps[0] : steps[1];
                    box.element->get_CurrentBoundingRectangle(&list);
                }
                return {focus, list};
            }
            ComPtr<IUIAutomationElement> parent;
            if (FAILED(walker->GetParentElement(element.Get(), &parent)))
                break;
            element = parent;
        }
        return other;
    }

    QList<pathtext::XYplorerTab> tabs(HWND window, const std::wstring& active) const
    {
        const auto state = tabState();
        if (state.size() != 2)
            return {};
        const QString folder = QString::fromStdWString(active);
        QDir settings(QFileInfo(QString::fromStdWString(programOf(window))).dir().filePath(u"Settings"_s));
        if (!settings.exists())
            settings = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/OneCommander/Settings"_s);
        const QStringList sessions = settings.entryList({u"Tabs*1.json"_s}, QDir::Files, QDir::Name);
        if (sessions.size() > 64)
            return {};
        const auto read = [&](const QString& name) {
            QFile file(settings.filePath(name));
            return file.size() <= 1024 * 1024 && file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        std::optional<QList<pathtext::XYplorerTab>> found;
        for (const QString& name : sessions) {
            const auto candidate = onecommander::sessionTabs({read(name), read(name.chopped(6) + u"2.json"_s)}, state, folder);
            if (!candidate)
                continue;
            if (found)
                return {}; // several saved windows fit: never show a closed window's tabs
            found = candidate;
        }
        return found.value_or(QList<pathtext::XYplorerTab>());
    }

private:
    using Text = std::unique_ptr<OLECHAR, decltype(&::SysFreeString)>;

    std::vector<onecommander::TabState> tabState() const
    {
        if (!m_root)
            return {};
        const Text name(::SysAllocString(L"tabs"), ::SysFreeString);
        VARIANT id {};
        id.vt = VT_BSTR;
        id.bstrVal = name.get();
        ComPtr<IUIAutomationCondition> condition;
        ComPtr<IUIAutomationElementArray> panes;
        int paneCount = 0;
        if (!name || FAILED(m_uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &condition))
            || FAILED(m_root->FindAll(TreeScope_Descendants, condition.Get(), &panes)) || !panes
            || FAILED(panes->get_Length(&paneCount)) || paneCount != 2)
            return {};
        VARIANT type {};
        type.vt = VT_I4;
        type.lVal = UIA_ListItemControlTypeId;
        if (FAILED(m_uia->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &condition)))
            return {};
        std::vector<onecommander::TabState> state;
        for (int p = 0; p < paneCount; ++p) {
            ComPtr<IUIAutomationElement> pane;
            ComPtr<IUIAutomationElementArray> tabs;
            onecommander::TabState live;
            if (FAILED(panes->GetElement(p, &pane)) || !pane
                || FAILED(pane->FindAll(TreeScope_Children, condition.Get(), &tabs)) || !tabs
                || FAILED(tabs->get_Length(&live.count)) || live.count > 256)
                return {};
            BOOL offscreen = FALSE;
            if (SUCCEEDED(pane->get_CurrentIsOffscreen(&offscreen)))
                live.visible = !offscreen;
            for (int i = 0; i < live.count; ++i) {
                ComPtr<IUIAutomationElement> tab;
                ComPtr<IUIAutomationSelectionItemPattern> selected;
                BOOL onTop = FALSE;
                if (SUCCEEDED(tabs->GetElement(i, &tab)) && tab
                    && SUCCEEDED(tab->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&selected)))
                    && SUCCEEDED(selected->get_CurrentIsSelected(&onTop)) && onTop)
                    live.selected = i;
            }
            state.push_back(live);
        }
        return state;
    }

    ComPtr<IUIAutomationValuePattern> value(const wchar_t* id) const
    {
        if (!m_root)
            return nullptr;
        const Text text(::SysAllocString(id), ::SysFreeString);
        if (!text)
            return nullptr;
        VARIANT property {};
        property.vt = VT_BSTR;
        property.bstrVal = text.get();
        ComPtr<IUIAutomationCondition> condition;
        ComPtr<IUIAutomationElement> element;
        ComPtr<IUIAutomationValuePattern> pattern;
        if (SUCCEEDED(m_uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, property, &condition))
            && SUCCEEDED(m_root->FindFirst(TreeScope_Descendants, condition.Get(), &element)) && element)
            element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pattern));
        return pattern;
    }

    ComPtr<IUIAutomation> m_uia;
    ComPtr<IUIAutomationElement> m_root;
};

// Main windows only, in Z order. Settings and other owned WPF dialogs are
// excluded by kindOf; a portable copy need not leave registry entries.
HWND oneCommanderWindow(bool visibleOnly = true)
{
    struct Search {
        bool visibleOnly;
        HWND found = nullptr;
    } search {visibleOnly};
    ::EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(param);
            if ((!search.visibleOnly || ::IsWindowVisible(hwnd)) && kindOf(hwnd) == Kind::OneCommander) {
                search.found = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

} // namespace

std::optional<OneCommanderTyping> oneCommanderTyping(HWND window)
{
    const OneCommanderAutomation automation(window);
    const auto [focus, list] = automation.typingFocus(window);
    if (focus == onecommander::TypingFocus::Other)
        return std::nullopt;
    return OneCommanderTyping {automation.folder(), list};
}

bool goToInOneCommanderWindow(HWND window, const std::wstring& path)
{
    return OneCommanderAutomation(window).goTo(path);
}

namespace {

std::wstring oneCommanderProgram()
{
    if (const std::wstring running = programOf(oneCommanderWindow(false)); !running.empty())
        return running;
    constexpr const wchar_t* kKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\OneCommander.exe";
    for (const HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (const DWORD view : {RRF_SUBKEY_WOW6464KEY, RRF_SUBKEY_WOW6432KEY}) {
            std::wstring exe = registryText(root, kKey, nullptr, view);
            if (exe.size() >= 2 && exe.front() == L'"' && exe.back() == L'"')
                exe = exe.substr(1, exe.size() - 2);
            if (!exe.empty() && isFile(exe))
                return exe;
        }
    }
    // MSI's default install and the Store's execution alias. No process is
    // started merely to detect it or to collect folders for Ctrl+G.
    for (const KNOWNFOLDERID& id : {FOLDERID_ProgramFiles, FOLDERID_LocalAppData}) {
        PWSTR folder = nullptr;
        const HRESULT result = ::SHGetKnownFolderPath(id, 0, nullptr, &folder);
        const std::wstring base = SUCCEEDED(result) ? folder : L"";
        ::CoTaskMemFree(folder);
        if (base.empty())
            continue;
        for (const wchar_t* suffix :
            {L"\\OneCommander\\OneCommander.exe", L"\\Microsoft\\WindowsApps\\OneCommander.exe"}) {
            if (isFile(base + suffix))
                return base + suffix;
        }
    }
    return {};
}

// Pulse (GitHub #41), an open-source file manager that draws its windows
// itself: nothing to ask, nothing for UI Automation but their titles. What
// they show is in its session file, which it saves at most every 3 s once
// something changed, from a timer of 100 ms (200 ms with its windows
// hidden): kPulseSaveWait is a little more.
constexpr const wchar_t* kPulseClass = L"PulseMainWindow";
constexpr auto kPulseSaveWait = std::chrono::milliseconds(3500);
// Its setup's entry under Programs and Features (an Inno Setup AppId, which
// its own setup kept): where it was installed, C:\Program Files\Pulse.
constexpr const wchar_t* kPulseUninstallKey
    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{A3F47C2E-9D1B-4E58-8C6A-2B5D0F9E1734}_is1";

bool isPulseProgram(std::wstring_view path)
{
    return win32::equalsIgnoreCase(baseName(path), L"pulse.exe");
}

// Pulse's main windows, front to back; shown or not (one closed to the tray is hidden).
std::vector<HWND> pulseMainWindows()
{
    std::vector<HWND> windows;
    for (HWND hwnd = nullptr; (hwnd = ::FindWindowExW(nullptr, hwnd, kPulseClass, nullptr));) {
        if (isPulseProgram(programOf(hwnd)))
            windows.push_back(hwnd);
    }
    return windows;
}

std::wstring pulseProgram()
{
    for (HWND hwnd = nullptr; (hwnd = ::FindWindowExW(nullptr, hwnd, kPulseClass, nullptr));) {
        if (std::wstring program = programOf(hwnd); isPulseProgram(program))
            return remembering("Pulse", std::move(program)); // maybe the portable one, which leaves no trace in the registry
    }
    const std::pair<HKEY, DWORD> places[] {
        {HKEY_LOCAL_MACHINE, RRF_SUBKEY_WOW6464KEY}, {HKEY_LOCAL_MACHINE, RRF_SUBKEY_WOW6432KEY}, {HKEY_CURRENT_USER, 0}};
    for (const auto& [root, flags] : places) {
        for (const wchar_t* value : {L"InstallLocation", L"Inno Setup: App Path"}) {
            std::wstring dir = registryText(root, kPulseUninstallKey, value, flags);
            while (dir.ends_with(L'\\'))
                dir.pop_back();
            if (!dir.empty() && isFile(dir + L"\\pulse.exe"))
                return dir + L"\\pulse.exe";
        }
    }
    return remembered("Pulse");
}

// The Pulse that runs may take the front for what it is handed: that comes
// through a second pulse.exe (its single-instance hand-over), which can pass
// on only what foreground rights it was started with.
void letPulseToFront()
{
    DWORD pid = 0;
    if (const HWND window = ::FindWindowW(kPulseClass, nullptr); window && ::GetWindowThreadProcessId(window, &pid) && pid)
        ::AllowSetForegroundWindow(pid);
}

// The command lines of Kind::Custom (setCustomCommands).
std::mutex g_customMutex;
QString g_customOpen;
QString g_customReveal;

// Runs a custom command line for `path`; false when there is none, its
// program is not there, or it did not start.
bool runCustom(const QString& commandLine, const std::wstring& path, bool wait)
{
    const pathtext::CustomCommand command = pathtext::customCommand(commandLine, QString::fromStdWString(path));
    const QString program = customProgram(commandLine);
    if (command.program.isEmpty() || program.isEmpty()) {
        qWarning() << "The file manager command has no program to run:" << commandLine;
        return false;
    }
    const std::wstring exe = QDir::toNativeSeparators(program).toStdWString();
    return run(exe, command.arguments.toStdWString(), wait, folderOf(exe));
}

// The folder on disk that an Explorer tab shows; empty for Home, This PC, a
// search and the like.
std::wstring folderPath(IShellBrowser* browser)
{
    ComPtr<IShellView> view;
    ComPtr<IFolderView> folderView;
    ComPtr<IPersistFolder2> folder;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&folderView))
        || FAILED(folderView->GetFolder(IID_PPV_ARGS(&folder))) || FAILED(folder->GetCurFolder(&pidl)) || !pidl)
        return {};
    std::wstring path;
    PWSTR name = nullptr;
    if (SUCCEEDED(::SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &name))) {
        path = name;
        ::CoTaskMemFree(name);
    }
    ::ILFree(pidl);
    return path;
}

struct ExplorerTab {
    HWND window;
    HWND tab;
    std::wstring path;
};

// Each tab of each Explorer window that shows a folder on disk.
std::vector<ExplorerTab> explorerTabs()
{
    ComPtr<IShellWindows> windows;
    long count = 0;
    if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))
        || FAILED(windows->get_Count(&count)))
        return {};
    std::vector<ExplorerTab> tabs;
    for (long i = 0; i < count; ++i) {
        VARIANT index {};
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> item;
        ComPtr<IServiceProvider> services;
        ComPtr<IShellBrowser> browser;
        HWND tab = nullptr;
        if (windows->Item(index, &item) != S_OK || !item || FAILED(item.As(&services))
            || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))
            || FAILED(browser->GetWindow(&tab)) || !tab)
            continue;
        std::wstring path = folderPath(browser.Get());
        if (!path.empty())
            tabs.push_back({folderview::frameOf(tab), tab, std::move(path)});
    }
    return tabs;
}

// The shell's window (an IWebBrowser2) of an Explorer tab, and its browser.
bool findTab(HWND tab, ComPtr<IWebBrowser2>& web, ComPtr<IShellBrowser>& browser)
{
    ComPtr<IShellWindows> windows;
    long count = 0;
    if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))
        || FAILED(windows->get_Count(&count)))
        return false;
    for (long i = 0; i < count; ++i) {
        VARIANT index {};
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> item;
        ComPtr<IServiceProvider> services;
        HWND window = nullptr;
        if (windows->Item(index, &item) != S_OK || !item || FAILED(item.As(&services))
            || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))
            || FAILED(browser->GetWindow(&window)) || window != tab)
            continue;
        return SUCCEEDED(item.As(&web));
    }
    return false;
}

// The tab goes to `folder`, as going there in it would (its Back button
// comes back).
bool navigate(IWebBrowser2* web, const std::wstring& folder)
{
    VARIANT target {};
    target.vt = VT_BSTR;
    target.bstrVal = ::SysAllocString(folder.c_str());
    VARIANT empty {};
    const HRESULT hr = web->Navigate2(&target, &empty, &empty, &empty, &empty);
    ::VariantClear(&target);
    return SUCCEEDED(hr);
}

// Whether the thread of the Explorer window `root` has the keyboard on its
// files and folders (a DirectUIHWND under SHELLDLL_DefView in that window).
bool filesFocused(HWND root)
{
    GUITHREADINFO info {sizeof(GUITHREADINFO)};
    if (!::GetGUIThreadInfo(::GetWindowThreadProcessId(root, nullptr), &info) || !info.hwndFocus)
        return false;
    const folderview::Hit hit = folderview::classify(info.hwndFocus, {});
    return hit.surface == folderview::Surface::Explorer && hit.root == root;
}

bool waitFor(std::predicate auto done, std::chrono::milliseconds limit)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// The folders a Total Commander window shows: the panel with the focus first.
std::vector<std::wstring> totalCommanderFolders(HWND window)
{
    DWORD_PTR active = 0; // 1: the left panel, 2: the right one
    if (!::SendMessageTimeoutW(window, kTotalCommanderAsk, 1000, 0, SMTO_ABORTIFHUNG, 1000, &active))
        return {};
    std::vector<std::wstring> folders;
    // The text over the left file list (9), the right one (10): "c:\Windows\*.*".
    for (const WPARAM side : active == 2 ? std::initializer_list<WPARAM> {10, 9} : std::initializer_list<WPARAM> {9, 10}) {
        DWORD_PTR label = 0;
        if (!::SendMessageTimeoutW(window, kTotalCommanderAsk, side, 0, SMTO_ABORTIFHUNG, 1000, &label) || !label)
            continue;
        const std::wstring folder = pathtext::folderFromTotalCommander(
            QString::fromStdWString(caption(reinterpret_cast<HWND>(label)))).toStdWString();
        if (!folder.empty() && isFolder(folder)) // not inside an archive
            folders.push_back(folder);
    }
    return folders;
}

// What Directory Opus answers when asked which folders its windows show (the
// XML of pathtext::opusTabs); nothing when it did not, or has no window open:
// asking would start it.
std::optional<QByteArray> opusPathsAnswer()
{
    if (!::FindWindowW(L"dopus.lister", nullptr))
        return std::nullopt;
    const std::wstring rt = opusProgram();
    wchar_t temp[MAX_PATH + 1] {};
    const DWORD n = ::GetTempPathW(MAX_PATH + 1, temp);
    if (rt.empty() || n == 0 || n > MAX_PATH)
        return std::nullopt;
    static std::atomic<unsigned> asked {0};
    const std::wstring file = std::wstring(temp, n) + L"WinShun-opus-" + std::to_wstring(::GetCurrentProcessId()) + L'-'
        + std::to_wstring(++asked) + L".xml";
    // It answers in a file, written by the time dopusrt.exe ends (some 50 ms).
    const win32::UniqueHandle process = shell::startUnelevated(rt, L"/info " + quoted(file) + L",paths");
    if (!process.valid())
        return std::nullopt;
    if (::WaitForSingleObject(process.get(), 3000) != WAIT_OBJECT_0) {
        ::TerminateProcess(process.get(), 1);
        ::DeleteFileW(file.c_str());
        qWarning() << "Directory Opus did not say which folders it shows";
        return std::nullopt;
    }
    QFile answer(QString::fromStdWString(file));
    std::optional<QByteArray> xml;
    if (answer.open(QIODevice::ReadOnly))
        xml = answer.readAll();
    answer.close();
    answer.remove();
    return xml;
}

// The tabs of Directory Opus's windows; none while it has no window open.
QList<pathtext::OpusTab> opusTabs(bool existingOnly = true)
{
    QList<pathtext::OpusTab> tabs = pathtext::opusTabs(opusPathsAnswer().value_or({}));
    if (existingOnly)
        tabs.removeIf([](const pathtext::OpusTab& tab) { return !isFolder(tab.path.toStdWString()); }); // archives
    return tabs;
}

// Where Pulse keeps its session, the portable copy too: every window's place,
// tabs and panes.
std::wstring pulseSessionFile()
{
    PWSTR folder = nullptr;
    std::wstring file;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)))
        file = std::wstring(folder) + L"\\Pulse\\session.json";
    ::CoTaskMemFree(folder);
    return file;
}

// When a file was written last; zero when it is not there.
FILETIME writtenAt(const std::wstring& file)
{
    WIN32_FILE_ATTRIBUTE_DATA data {};
    return ::GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data) ? data.ftLastWriteTime : FILETIME {};
}

struct SharedRead {
    QByteArray bytes;
    FILETIME written {};
};

// A file's bytes and when they were written, read with every sharing allowed:
// Pulse replaces its session file whole (MoveFileEx over it), which a reader
// without FILE_SHARE_DELETE would make fail, its last save as it exits too.
SharedRead readShared(const std::wstring& file)
{
    SharedRead read;
    const win32::UniqueHandle handle(::CreateFileW(file.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size {};
    if (!handle.valid() || !::GetFileSizeEx(handle.get(), &size) || size.QuadPart <= 0
        || size.QuadPart > 64 * 1024 * 1024) // Pulse's own limit
        return read;
    QByteArray bytes(static_cast<qsizetype>(size.QuadPart), Qt::Uninitialized);
    DWORD got = 0;
    if (!::ReadFile(handle.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr)
        || !::GetFileTime(handle.get(), nullptr, nullptr, &read.written))
        return read;
    bytes.truncate(got);
    read.bytes = std::move(bytes);
    return read;
}

// The version of Pulse's session a wait was spent on in vain, and the title
// its window had then (pulseSession).
std::mutex g_pulseMutex;
FILETIME g_pulseWaited {};
std::wstring g_pulseWaitedTitle;

// What Pulse's windows show, as its session says (`windows`: its main
// windows, pulseMainWindows). Saved a few seconds late (kPulseSaveWait),
// while a window's title changes at once: with one window open, whose title
// names the folder in its front pane ("乙 — Pulse"), a session that names
// another is waited for, so that Ctrl+G right after going somewhere in Pulse
// goes there and not to the folder before. Once per version of the file and
// title: one the session never names costs one wait, not one each time,
// while going on to another folder is waited for again. More windows are
// called by names of their own ("t — Pulse", for the folder their panes
// have in common): the session goes as it is then, a few seconds old at
// worst.
QList<pathtext::PulseWindow> pulseSession(const std::vector<HWND>& windows)
{
    const std::wstring file = windows.empty() ? std::wstring() : pulseSessionFile();
    if (file.empty())
        return {};
    SharedRead read = readShared(file);
    QList<pathtext::PulseWindow> session = pathtext::pulseWindows(read.bytes);
    std::wstring title;
    const auto current = [&] {
        if (windows.size() != 1 || session.isEmpty() || session.front().frontName.isEmpty())
            return true;
        title = caption(windows.front());
        return title == pathtext::pulseTitle(session.front().frontName).toStdWString();
    };
    if (current())
        return session;
    {
        const std::lock_guard lock(g_pulseMutex);
        if (::CompareFileTime(&g_pulseWaited, &read.written) == 0 && g_pulseWaitedTitle == title)
            return session;
    }
    const FILETIME stale = read.written;
    if (waitFor([&] {
            const FILETIME now = writtenAt(file);
            return ::CompareFileTime(&now, &stale) != 0;
        }, kPulseSaveWait)) {
        read = readShared(file);
        session = pathtext::pulseWindows(read.bytes);
    }
    if (!current()) {
        const std::lock_guard lock(g_pulseMutex);
        g_pulseWaited = read.written;
        g_pulseWaitedTitle = title;
    }
    return session;
}

// Whether Pulse's `window` is where the session says `w` is when restored,
// as Pulse sees it: GetWindowPlacement scales it for a thread of another DPI
// awareness.
bool samePlace(HWND window, const pathtext::PulseWindow& w)
{
    const DPI_AWARENESS_CONTEXT ours = ::SetThreadDpiAwarenessContext(::GetWindowDpiAwarenessContext(window));
    WINDOWPLACEMENT placement {sizeof(WINDOWPLACEMENT)};
    const bool got = ::GetWindowPlacement(window, &placement);
    if (ours)
        ::SetThreadDpiAwarenessContext(ours);
    const RECT& r = placement.rcNormalPosition;
    return got && r.left == w.left && r.top == w.top && r.right == w.right && r.bottom == w.bottom;
}

struct PulseShown {
    HWND window;
    QList<pathtext::XYplorerTab> folders; // as pathtext::PulseWindow's, on disk
};

// Each main window of Pulse, front to back, with the folders on disk it
// shows: the only one is the session's first (the one used last); more are
// told apart by their places, each taken once. One moved or opened since the
// session was saved is not in it: it shows nothing until the next save.
std::vector<PulseShown> pulseShown()
{
    const std::vector<HWND> windows = pulseMainWindows();
    const QList<pathtext::PulseWindow> session = pulseSession(windows);
    std::vector<bool> taken(static_cast<std::size_t>(session.size()), false);
    std::vector<PulseShown> shown;
    for (const HWND window : windows) {
        qsizetype found = -1;
        for (qsizetype i = 0; i < session.size() && found < 0; ++i) {
            if (!taken[static_cast<std::size_t>(i)] && (windows.size() == 1 || samePlace(window, session[i])))
                found = i;
        }
        if (found < 0)
            continue;
        taken[static_cast<std::size_t>(found)] = true;
        PulseShown entry {window, session[found].folders};
        entry.folders.removeIf([](const pathtext::XYplorerTab& f) { return !isFolder(f.path.toStdWString()); }); // archives, gone
        shown.push_back(std::move(entry));
    }
    return shown;
}

} // namespace

std::optional<Kind> kindOf(HWND window)
{
    wchar_t name[512] {}; // WPF includes the assembly name and a GUID
    if (!window || ::GetClassNameW(window, name, static_cast<int>(std::size(name))) == 0)
        return std::nullopt;
    if (::wcscmp(name, L"CabinetWClass") == 0)
        return Kind::Explorer;
    if (::wcscmp(name, L"TTOTAL_CMD") == 0)
        return Kind::TotalCommander;
    if (::wcscmp(name, L"dopus.lister") == 0)
        return Kind::DirectoryOpus;
    if (::wcscmp(name, L"ThunderRT6FormDC") == 0 && isXYplorerMain(window))
        return Kind::XYplorer;
    if (::wcscmp(name, kPulseClass) == 0 && isPulseProgram(programOf(window)))
        return Kind::Pulse;
    if (std::wstring_view(name).starts_with(L"HwndWrapper[") && !::GetWindow(window, GW_OWNER)
        && win32::equalsIgnoreCase(baseName(programOf(window)), L"OneCommander.exe"))
        return Kind::OneCommander;
    if (::GetAncestor(window, GA_ROOT) == window && !::GetWindow(window, GW_OWNER)
        && win32::equalsIgnoreCase(baseName(programOf(window)), L"tessoa.exe"))
        return Kind::Tessoa;
    return std::nullopt;
}

std::vector<Folder> openFolders()
{
    struct Search {
        std::vector<ExplorerTab> explorer;
        QList<pathtext::OpusTab> opus;
        std::vector<PulseShown> pulse;
        std::vector<Folder> onTop; // each window's folder in front
        std::vector<Folder> behind; // the others
        std::vector<std::pair<std::size_t, HWND>> oneCommander;
    } search {explorerTabs(), opusTabs(), pulseShown(), {}, {}, {}};
    // Top-level windows come front to back, and so do a window's children.
    ::EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(param);
            // Explorer: Windows 11 has a child per tab (all of them visible), the tab on top first.
            bool first = true;
            for (HWND child = ::FindWindowExW(hwnd, nullptr, L"ShellTabWindowClass", nullptr); child;
                child = ::FindWindowExW(hwnd, child, L"ShellTabWindowClass", nullptr)) {
                for (const ExplorerTab& tab : search.explorer) {
                    if (tab.tab == child)
                        (first ? search.onTop : search.behind).push_back({tab.path, Kind::Explorer});
                }
                first = false;
            }
            for (const ExplorerTab& tab : search.explorer) { // a window without such children
                if (tab.window == hwnd && tab.tab == hwnd)
                    search.onTop.push_back({tab.path, Kind::Explorer});
            }
            const std::optional<Kind> kind = kindOf(hwnd);
            if (kind == Kind::TotalCommander && ::IsWindowVisible(hwnd)) {
                first = true;
                for (std::wstring& folder : totalCommanderFolders(hwnd))
                    (std::exchange(first, false) ? search.onTop : search.behind).push_back({std::move(folder), *kind});
            } else if (kind == Kind::DirectoryOpus) {
                // The tab in front on the side with the focus, the other side's, the tabs behind.
                for (const int state : {1, 2, 0}) {
                    for (const pathtext::OpusTab& tab : std::as_const(search.opus)) {
                        if (tab.lister == reinterpret_cast<quintptr>(hwnd) && tab.state == state)
                            (state == 1 ? search.onTop : search.behind).push_back({tab.path.toStdWString(), *kind});
                    }
                }
            } else if (kind == Kind::XYplorer && ::IsWindowVisible(hwnd)) {
                // Likewise; asked window by window (a window in the tray is not shown: not asked).
                const QList<pathtext::XYplorerTab> tabs = xyplorerTabs(hwnd);
                for (const int state : {1, 2, 0}) {
                    for (const pathtext::XYplorerTab& tab : tabs) {
                        if (tab.state == state)
                            (state == 1 ? search.onTop : search.behind).push_back({tab.path.toStdWString(), *kind});
                    }
                }
            } else if (kind == Kind::Pulse && ::IsWindowVisible(hwnd)) {
                // Likewise, as its session says (in that order already); one in the tray left out.
                for (const PulseShown& shown : search.pulse) {
                    if (shown.window != hwnd)
                        continue;
                    for (const pathtext::XYplorerTab& folder : shown.folders)
                        (folder.state == 1 ? search.onTop : search.behind).push_back({folder.path.toStdWString(), *kind});
                }
            } else if (kind == Kind::OneCommander && ::IsWindowVisible(hwnd)) {
                search.oneCommander.emplace_back(search.onTop.size(), hwnd);
                search.onTop.push_back({{}, *kind}); // reserve its place in Z order
            } else if (kind == Kind::Tessoa && ::IsWindowVisible(hwnd)) {
                const auto path = tessoa::currentFolder(hwnd).toStdWString();
                if (!path.empty()) search.onTop.push_back({path, *kind});
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    // Explorer windows held in another window's tabs (Clover: frameOf) are
    // no top-level windows: theirs after all the others (the list drops the
    // ones already in it).
    for (const ExplorerTab& tab : search.explorer)
        search.behind.push_back({tab.path, Kind::Explorer});
    // UI Automation can pump window messages. Ask after the User32
    // enumeration returns, retaining the captured windows' front-to-back order.
    for (const auto& [at, window] : search.oneCommander) {
        const OneCommanderAutomation automation(window);
        std::wstring folder = automation.folder();
        search.onTop[at].path = folder;
        if (folder.empty())
            continue;
        const auto tabs = automation.tabs(window, folder);
        for (const int state : {2, 0}) {
            for (const auto& tab : tabs) {
                if (tab.state == state && isFolder(tab.path.toStdWString()))
                    search.behind.push_back({tab.path.toStdWString(), Kind::OneCommander});
            }
        }
    }
    std::vector<Folder> folders;
    for (const auto* list : {&search.onTop, &search.behind}) {
        for (const Folder& folder : *list) {
            if (folder.path.empty())
                continue;
            if (std::ranges::none_of(folders, [&](const Folder& f) { return ::_wcsicmp(f.path.c_str(), folder.path.c_str()) == 0; }))
                folders.push_back(folder);
        }
    }
    return folders;
}

std::wstring explorerFolder(HWND tab)
{
    ComPtr<IWebBrowser2> web;
    ComPtr<IShellBrowser> browser;
    return findTab(tab, web, browser) ? folderPath(browser.Get()) : std::wstring();
}

std::optional<std::wstring> explorerPlace(HWND tab)
{
    ComPtr<IWebBrowser2> web;
    ComPtr<IShellBrowser> browser;
    if (!findTab(tab, web, browser))
        return std::nullopt;
    std::wstring folder = folderPath(browser.Get());
    if (!folder.empty())
        return folder;
    ComPtr<IShellView> view;
    ComPtr<IFolderView> folderView;
    ComPtr<IPersistFolder2> shown;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&folderView))
        || FAILED(folderView->GetFolder(IID_PPV_ARGS(&shown))) || FAILED(shown->GetCurFolder(&pidl)) || !pidl)
        return std::nullopt;
    PWSTR name = nullptr;
    const bool everywhere = SUCCEEDED(::SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &name)) && searchtyping::wholePcPlace(name);
    ::CoTaskMemFree(name);
    ::ILFree(pidl);
    return everywhere ? std::optional(std::wstring()) : std::nullopt;
}

bool goTo(HWND tab, const std::wstring& folder)
{
    ComPtr<IWebBrowser2> web;
    ComPtr<IShellBrowser> browser;
    return findTab(tab, web, browser) && navigate(web.Get(), folder);
}

bool selectInExplorer(HWND tab, const std::wstring& folder, const std::wstring& path,
    const std::function<bool()>& current)
{
    const auto active = [&] {
        if (!current() || !::IsWindow(tab))
            return false;
        const HWND root = folderview::frameOf(tab);
        const HWND first = ::FindWindowExW(root, nullptr, L"ShellTabWindowClass", nullptr);
        return !first || first == tab;
    };
    if (!active())
        return false;
    const auto identity = [](const std::wstring& value) {
        const auto mapped = unc::fromMappedDrive(value);
        return QString::fromStdWString(mapped.empty() ? value : mapped);
    };
    const QString expected = identity(folder);
    const QFileInfo chosen(identity(path));
    if (!pathtext::sameFolder(chosen.absolutePath(), expected) || chosen.fileName().isEmpty())
        return false;
    ComPtr<IWebBrowser2> web;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IFolderView> folderView;
    ComPtr<IShellFolder> shellFolder;
    if (!findTab(tab, web, browser) || !active()
        || !pathtext::sameFolder(identity(folderPath(browser.Get())), expected)
        || FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&folderView))
        || FAILED(folderView->GetFolder(IID_PPV_ARGS(&shellFolder))))
        return false;
    std::wstring name = chosen.fileName().toStdWString();
    PIDLIST_RELATIVE child = nullptr;
    if (FAILED(shellFolder->ParseDisplayName(nullptr, nullptr, name.data(), nullptr, &child, nullptr)) || !child)
        return false;
    // The item gets the view's cursor, but the search field keeps the keyboard.
    constexpr UINT flags = SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE | SVSI_FOCUSED | SVSI_NOTAKEFOCUS;
    const bool selected = active() && pathtext::sameFolder(identity(folderPath(browser.Get())), expected)
        && active() && SUCCEEDED(view->SelectItem(child, flags));
    ::CoTaskMemFree(child);
    return selected;
}

bool goToKeepingFocus(HWND tab, const std::wstring& folder, const std::wstring& select)
{
    const HWND root = folderview::frameOf(tab);
    const auto inFront = [root] { return ::GetForegroundWindow() == root && filesFocused(root); };
    // Just handed the front, the window has not given its files the keyboard
    // back yet (docs/pitfalls.md: the file dialogs' "刚交还前台就操作"); gone
    // on from there, Explorer gives the new folder's files the focus too.
    waitFor(inFront, std::chrono::milliseconds(500));
    ComPtr<IWebBrowser2> web;
    ComPtr<IShellBrowser> browser;
    if (!findTab(tab, web, browser))
        return false;
    const QString wanted = QString::fromStdWString(folder);
    const auto showsWanted = [&] { return pathtext::sameFolder(QString::fromStdWString(folderPath(browser.Get())), wanted); };
    // Already there (an item to select in the folder shown): no navigation,
    // which would make a new view that loses the selection.
    if (select.empty() || !showsWanted()) {
        if (!navigate(web.Get(), folder))
            return false;
        waitFor(showsWanted, std::chrono::milliseconds(2000));
    }
    if (!select.empty()) {
        // The view lists the folder after it is shown: asked again until it
        // has the item (two seconds at most). Not found: the folder is open
        // all the same, true.
        PIDLIST_ABSOLUTE pidl = ::ILCreateFromPathW(select.c_str());
        if (pidl) {
            constexpr UINT flags = SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE | SVSI_FOCUSED;
            waitFor([&] {
                ComPtr<IShellView> view;
                return showsWanted() && SUCCEEDED(browser->QueryActiveShellView(&view)) && view
                    && SUCCEEDED(view->SelectItem(::ILFindLastID(pidl), flags));
            }, std::chrono::milliseconds(2000));
            ::ILFree(pidl);
        }
    }
    // The new view takes the focus as it comes; should it not have it (the
    // window was not in front with its files focused), it is given to it.
    if (::GetForegroundWindow() == root && !waitFor(inFront, std::chrono::milliseconds(300))) {
        ComPtr<IShellView> view;
        if (SUCCEEDED(browser->QueryActiveShellView(&view)) && view)
            view->UIActivate(SVUIA_ACTIVATE_FOCUS);
    }
    return true;
}

std::wstring program(Kind kind)
{
    switch (kind) {
    case Kind::TotalCommander:
        return totalCommanderProgram();
    case Kind::DirectoryOpus:
        return opusProgram();
    case Kind::XYplorer:
        return xyplorerProgram();
    case Kind::Pulse:
        return pulseProgram();
    case Kind::OneCommander:
        return oneCommanderProgram();
    case Kind::Custom: {
        const std::lock_guard lock(g_customMutex);
        return QDir::toNativeSeparators(customProgram(g_customOpen)).toStdWString();
    }
    case Kind::Explorer:
    case Kind::Tessoa: // read-only current-folder integration
        break;
    }
    return {};
}

bool installed(Kind kind)
{
    return kind == Kind::Explorer || kind == Kind::Custom || !program(kind).empty();
}

bool openFolder(Kind kind, const std::wstring& folder)
{
    if (kind == Kind::Custom) {
        QString command;
        {
            const std::lock_guard lock(g_customMutex);
            command = g_customOpen;
        }
        return runCustom(command, folder, false);
    }
    const std::wstring exe = program(kind);
    if (exe.empty())
        return false;
    switch (kind) {
    case Kind::TotalCommander: // in the copy that runs (/O), the panel with the focus (/S: /L is that one)
        return run(exe, L"/O /T /S /L=" + quoted(folder), false);
    case Kind::DirectoryOpus:
        return run(exe, L"/acmd Go " + quoted(folder) + L" NEWTAB=findexisting,tofront", false);
    case Kind::XYplorer: {
        if (const HWND window = xyplorerWindow()) {
            const std::wstring script = xyplorerGoScript(QString::fromStdWString(folder), {});
            if (!askXYplorer(window, [&](const std::wstring&) { return script; }).delivered)
                return false;
            bringToFront(window);
            return true;
        }
        // Not running: started on it. "D:\" bare: a backslash before the closing quote would escape it.
        return run(exe, folder.ends_with(L'\\') ? folder : quoted(folder), false);
    }
    case Kind::Pulse:
        // Its single-instance hand-over (pulse.exe <folder>) to the Pulse that
        // runs, or started on it. Its window that shows it already comes to
        // it; else it goes where Pulse puts any folder from outside: by its
        // setting, by default a new tab in the window used last, or a new
        // window when that one is busy or was used in the last 8 s.
        letPulseToFront();
        return run(exe, pathtext::commandArgument(QString::fromStdWString(folder)).toStdWString(), false);
    case Kind::OneCommander: {
        DWORD pid = 0;
        if (const HWND window = oneCommanderWindow(false); window && ::GetWindowThreadProcessId(window, &pid))
            ::AllowSetForegroundWindow(pid);
        return run(exe, L"-o " + pathtext::commandArgument(QString::fromStdWString(folder)).toStdWString() + L" -newtab",
            true, folderOf(exe)); // hand-over finishes before another pane command is sent
    }
    case Kind::Custom:
    case Kind::Explorer:
    case Kind::Tessoa:
        break;
    }
    return false;
}

bool goToInOpus(const std::wstring& folder)
{
    const std::wstring exe = opusProgram();
    return !exe.empty() && run(exe, L"/acmd Go " + quoted(folder), false);
}

std::optional<std::wstring> opusPlace(HWND view)
{
    const HWND root = ::GetAncestor(view, GA_ROOT);
    if (!folderview::isOpusFileDisplay(root, view))
        return std::nullopt;
    // DOpusRT's `tab` is the file-display container, while keyboard focus
    // belongs to its file-display child (measured with Opus 13.25).
    const quintptr container = reinterpret_cast<quintptr>(::GetParent(view));
    const std::optional<QByteArray> answer = opusPathsAnswer();
    if (!answer || !pathtext::opusAnswered(*answer))
        return std::nullopt;
    const QList<pathtext::OpusTab> tabs = pathtext::opusTabs(*answer);
    const auto shown = std::ranges::find_if(tabs, [&](const pathtext::OpusTab& tab) {
        return tab.lister == reinterpret_cast<quintptr>(root) && tab.tab == container;
    });
    // Not among them: This PC, which Opus 13.25 leaves out of its answer (and
    // what pathtext::opusTabs drops: FTP and the like). A library or the
    // inside of an archive is named there, and is no folder.
    if (shown == tabs.end())
        return std::wstring();
    std::wstring path = shown->path.toStdWString();
    return isFolder(path) ? std::move(path) : std::wstring();
}

std::wstring opusFolder(HWND view)
{
    return opusPlace(view).value_or({});
}

bool goToInOpusView(HWND root, HWND tab, const std::wstring& folder, const std::wstring& select)
{
    // /acmd targets the last active lister. Only send it while this same
    // lister and file display still have the keyboard; never another pane.
    const auto inFront = [&] {
        GUITHREADINFO info {sizeof info};
        return ::GetForegroundWindow() == root && ::GetAncestor(tab, GA_ROOT) == root
            && kindOf(root) == Kind::DirectoryOpus
            && ::GetGUIThreadInfo(::GetWindowThreadProcessId(root, nullptr), &info) && info.hwndFocus == tab
            && !(info.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE));
    };
    if (!waitFor(inFront, std::chrono::milliseconds(500)))
        return false;
    const std::wstring exe = opusProgram();
    if (exe.empty()
        || !run(exe,
            L"/acmd Go " + quoted(select.empty() ? folder : select) + (select.empty() ? L"" : L" OPENCONTAINER"), true))
        return false;
    // Queries queued on the typing worker after this must see the new
    // folder. Opus can finish its Go after dopusrt has returned.
    const QString wanted = QString::fromStdWString(folder);
    return waitFor([&] { return pathtext::sameFolder(QString::fromStdWString(opusFolder(tab)), wanted); },
        std::chrono::milliseconds(2000));
}

std::wstring frontFolder(HWND window)
{
    const std::optional<Kind> kind = kindOf(window);
    if (!kind)
        return {};
    switch (*kind) {
    case Kind::Explorer: {
        const HWND tab = ::FindWindowExW(window, nullptr, L"ShellTabWindowClass", nullptr); // the one on top first
        return explorerFolder(tab ? tab : window);
    }
    case Kind::TotalCommander: {
        const std::vector<std::wstring> folders = totalCommanderFolders(window);
        return folders.empty() ? std::wstring() : folders.front();
    }
    case Kind::DirectoryOpus:
        for (const pathtext::OpusTab& tab : opusTabs()) {
            if (tab.lister == reinterpret_cast<quintptr>(window) && tab.state == 1)
                return tab.path.toStdWString();
        }
        return {};
    case Kind::XYplorer:
        for (const pathtext::XYplorerTab& tab : xyplorerTabs(window)) {
            if (tab.state == 1)
                return tab.path.toStdWString();
        }
        return {};
    case Kind::Pulse:
        for (const PulseShown& shown : pulseShown()) {
            if (shown.window != window)
                continue;
            for (const pathtext::XYplorerTab& folder : shown.folders) {
                if (folder.state == 1)
                    return folder.path.toStdWString();
            }
        }
        return {};
    case Kind::OneCommander:
        return OneCommanderAutomation(window).folder();
    case Kind::Tessoa:
        return tessoa::currentFolder(window).toStdWString();
    case Kind::Custom:
        break;
    }
    return {};
}

bool goToInFront(Kind kind, const std::wstring& folder)
{
    switch (kind) {
    case Kind::DirectoryOpus:
        return goToInOpus(folder);
    case Kind::TotalCommander: {
        // The copy that runs (/O), the panel with the focus (/S), its
        // current tab: without /T, which would open a new one.
        const std::wstring exe = totalCommanderProgram();
        return !exe.empty() && run(exe, L"/O /S /L=" + quoted(folder), false);
    }
    case Kind::XYplorer:
    case Kind::Pulse:
        return openFolder(kind, folder);
    case Kind::OneCommander:
        if (const HWND window = oneCommanderWindow(); window && OneCommanderAutomation(window).goTo(folder)) {
            bringToFront(window);
            return true;
        }
        return false;
    case Kind::Explorer:
    case Kind::Custom:
    case Kind::Tessoa:
        break;
    }
    return false;
}

bool openOpusPreferences()
{
    const std::wstring exe = opusProgram();
    return !exe.empty() && run(exe, L"/acmd Prefs", false);
}

std::wstring opusPrefsFile()
{
    const std::wstring file = qEnvironmentVariable("APPDATA").toStdWString()
        + L"\\GPSoftware\\Directory Opus\\ConfigFiles\\prefs.oxc";
    return isFile(file) ? file : std::wstring();
}

namespace {

// Directory Opus's process in this session, 0 if none.
DWORD opusProcess()
{
    DWORD session = 0;
    ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
    const win32::UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid())
        return 0;
    PROCESSENTRY32W entry {sizeof entry};
    for (BOOL more = ::Process32FirstW(snapshot.get(), &entry); more; more = ::Process32NextW(snapshot.get(), &entry)) {
        DWORD processSession = 0;
        if (::_wcsicmp(entry.szExeFile, L"dopus.exe") == 0 && ::ProcessIdToSessionId(entry.th32ProcessID, &processSession)
            && processSession == session)
            return entry.th32ProcessID;
    }
    return 0;
}

} // namespace

bool opusRunning()
{
    return opusProcess() != 0;
}

PrefsEdit editOpusPrefs(const std::function<std::optional<QByteArray>(const QByteArray&)>& edit)
{
    const std::wstring rt = opusProgram();
    const std::wstring file = opusPrefsFile();
    if (rt.empty() || file.empty())
        return PrefsEdit::Failed;
    // Closed as its own Exit would close it (it may ask about a copy still going).
    const DWORD pid = opusProcess();
    const bool hadWindow = ::FindWindowW(L"dopus.lister", nullptr) != nullptr;
    if (pid) {
        const win32::UniqueHandle process(::OpenProcess(SYNCHRONIZE, FALSE, pid));
        if (!process.valid() || !run(rt, L"/cmd Close PROGRAM", true)
            || ::WaitForSingleObject(process.get(), 15000) != WAIT_OBJECT_0) {
            qWarning() << "Directory Opus did not close";
            return PrefsEdit::OpusStayed;
        }
    }
    bool done = false;
    QFile in(QString::fromStdWString(file));
    if (in.open(QIODevice::ReadOnly)) {
        const QByteArray before = in.readAll();
        in.close();
        if (const std::optional<QByteArray> after = edit(before); !after) {
            done = true;
        } else {
            QSaveFile out(in.fileName());
            done = out.open(QIODevice::WriteOnly) && out.write(*after) == after->size() && out.commit();
        }
    }
    if (!done)
        qWarning() << "Could not change" << in.fileName();
    if (pid && run(folderOf(rt) + L"\\dopus.exe", {}, false)) {
        // Its windows come back and to the front: done once one is there.
        // One that ran with no window (in the tray) opens its default lister
        // as it starts, and no switch keeps that shut: closed again.
        for (int waited = 0; waited < 3000; waited += 100) {
            if (!hadWindow) {
                if (::FindWindowW(L"dopus.lister", nullptr)) {
                    for (HWND lister = nullptr; (lister = ::FindWindowExW(nullptr, lister, L"dopus.lister", nullptr));)
                        ::PostMessageW(lister, WM_CLOSE, 0, 0);
                    break;
                }
            } else {
                wchar_t name[32] {};
                const HWND front = ::GetAncestor(::GetForegroundWindow(), GA_ROOT);
                if (front && ::GetClassNameW(front, name, static_cast<int>(std::size(name)))
                    && ::wcscmp(name, L"dopus.lister") == 0)
                    break;
            }
            ::Sleep(100);
        }
    }
    return done ? PrefsEdit::Done : PrefsEdit::Failed;
}

namespace {

// Each item's folder in XYplorer with the items selected: a drive, nothing
// above it, is opened. Items grouped by folder, in the order the folders
// first come up (one script each).
bool revealInXYplorer(const std::wstring& exe, const std::vector<std::wstring>& paths)
{
    struct Group {
        QString folder;
        QStringList names;
    };
    std::vector<Group> groups;
    for (const std::wstring& path : paths) {
        const QFileInfo info(QString::fromStdWString(path));
        const QString folder = info.isRoot() ? QDir::toNativeSeparators(info.filePath()) : QDir::toNativeSeparators(info.path());
        auto it = std::ranges::find_if(groups, [&](const Group& g) { return g.folder.compare(folder, Qt::CaseInsensitive) == 0; });
        if (it == groups.end())
            it = groups.insert(groups.end(), {folder, {}});
        if (!info.isRoot())
            it->names.append(info.fileName());
    }
    const HWND window = xyplorerWindow();
    if (!window) {
        // Not running: started with the first item selected (/select), or on the drive.
        const Group& first = groups.front();
        const std::wstring folder = first.folder.toStdWString();
        return run(exe, first.names.isEmpty() ? folder : L"/select=" + quoted((first.folder + u'\\' + first.names.first()).toStdWString()), false);
    }
    bool shown = false;
    for (const Group& group : groups) {
        const std::wstring script = xyplorerGoScript(group.folder, group.names);
        if (!askXYplorer(window, [&](const std::wstring&) { return script; }).delivered)
            return shown;
        shown = true;
    }
    bringToFront(window);
    return shown;
}

// Each item's folder in Pulse, by its single-instance hand-over (openFolder):
// a file is handed over itself, and Pulse opens its folder with it selected
// (one each time: the first of each folder). Pulse selects files only: a
// folder's folder opens as it is; a drive, nothing above it, is opened.
bool revealInPulse(const std::wstring& exe, const std::vector<std::wstring>& paths)
{
    std::vector<std::wstring> folders; // shown already
    bool shown = false;
    for (const std::wstring& path : paths) {
        const QFileInfo info(QString::fromStdWString(path));
        const QString folder = QDir::toNativeSeparators(info.isRoot() ? info.filePath() : info.path());
        std::wstring native = folder.toStdWString();
        if (std::ranges::any_of(folders, [&](const std::wstring& f) { return ::_wcsicmp(f.c_str(), native.c_str()) == 0; }))
            continue;
        folders.push_back(std::move(native));
        const QString handed = info.isFile() ? QDir::toNativeSeparators(info.filePath()) : folder;
        letPulseToFront();
        if (!run(exe, pathtext::commandArgument(handed).toStdWString(), paths.size() > 1))
            return shown;
        shown = true;
    }
    return shown;
}

// The public setter selects a file, but treats a directory as navigation.
// Show a directory's parent instead; for multiple parents keep a tab for
// each, selecting the first file per parent as the single-item API allows.
bool revealInOneCommander(const std::vector<std::wstring>& paths)
{
    struct Group {
        QString folder;
        std::wstring file;
    };
    std::vector<Group> groups;
    for (const std::wstring& path : paths) {
        const QFileInfo info(QString::fromStdWString(path));
        const QString folder = QDir::toNativeSeparators(info.isRoot() ? info.filePath() : info.path());
        auto group
            = std::ranges::find_if(groups, [&](const Group& g) { return pathtext::sameFolder(g.folder, folder); });
        if (group == groups.end())
            group = groups.insert(groups.end(), {folder, {}});
        if (group->file.empty() && info.isFile())
            group->file = path;
    }
    bool shown = false;
    for (const Group& group : groups) {
        HWND window = oneCommanderWindow();
        if (!window || groups.size() > 1) {
            if (!openFolder(Kind::OneCommander, group.folder.toStdWString())
                || !waitFor(
                    [&] {
                        window = oneCommanderWindow();
                        return window
                            && pathtext::sameFolder(
                                QString::fromStdWString(OneCommanderAutomation(window).folder()), group.folder);
                    },
                    std::chrono::milliseconds(3000)))
                return shown;
        }
        if (!OneCommanderAutomation(window).goTo(group.file.empty() ? group.folder.toStdWString() : group.file))
            return shown;
        bringToFront(window);
        shown = true;
    }
    return shown;
}

// Each item's folder opened by the custom open command (no reveal command).
bool openFoldersOf(const QString& openCommand, const std::vector<std::wstring>& paths)
{
    std::vector<std::wstring> folders;
    for (const std::wstring& path : paths) {
        const QFileInfo info(QString::fromStdWString(path));
        std::wstring folder = QDir::toNativeSeparators(info.isRoot() ? info.filePath() : info.path()).toStdWString();
        if (std::ranges::none_of(folders, [&](const std::wstring& f) { return ::_wcsicmp(f.c_str(), folder.c_str()) == 0; }))
            folders.push_back(std::move(folder));
    }
    bool shown = false;
    for (const std::wstring& folder : folders) {
        if (!runCustom(openCommand, folder, folders.size() > 1))
            return shown;
        shown = true;
    }
    return shown;
}

} // namespace

bool reveal(Kind kind, const std::vector<std::wstring>& paths)
{
    if (paths.empty())
        return false;
    if (kind == Kind::Custom) {
        QString open;
        QString reveal;
        {
            const std::lock_guard lock(g_customMutex);
            open = g_customOpen;
            reveal = g_customReveal;
        }
        if (reveal.trimmed().isEmpty())
            return openFoldersOf(open, paths);
        bool shown = false;
        for (const std::wstring& path : paths) {
            if (!runCustom(reveal, path, paths.size() > 1))
                return shown;
            shown = true;
        }
        return shown;
    }
    if (kind == Kind::OneCommander)
        return revealInOneCommander(paths);
    const std::wstring exe = program(kind);
    if (exe.empty() || kind == Kind::Explorer)
        return false;
    if (kind == Kind::XYplorer)
        return revealInXYplorer(exe, paths);
    if (kind == Kind::Pulse)
        return revealInPulse(exe, paths);
    std::vector<std::wstring> folders; // shown already (Total Commander)
    bool shown = false;
    for (const std::wstring& path : paths) {
        const QFileInfo info(QString::fromStdWString(path));
        const bool root = info.isRoot(); // a drive: nothing above it
        std::wstring args;
        if (kind == Kind::TotalCommander) {
            const std::wstring folder = QDir::toNativeSeparators(info.path()).toStdWString();
            if (std::ranges::any_of(folders, [&](const std::wstring& f) { return ::_wcsicmp(f.c_str(), folder.c_str()) == 0; }))
                continue;
            folders.push_back(folder);
            // A file: its folder with the cursor on it. A folder: /P, the one above with the cursor on it.
            args = std::wstring(L"/O /T /S ") + (info.isDir() && !root ? L"/P " : L"") + L"/L=" + quoted(path);
        } else {
            args = L"/acmd Go " + quoted(path) + (root ? L"" : L" OPENCONTAINER") + L" NEWTAB=findexisting,tofront";
        }
        if (!run(exe, args, paths.size() > 1))
            return shown;
        shown = true;
    }
    return shown;
}

void setProgramMemory(const QString& stateFile)
{
    ProgramMemory& memory = programMemory();
    const QMutexLocker lock(&memory.mutex);
    memory.file = stateFile;
    memory.seen.clear();
    if (stateFile.isEmpty())
        return;
    QSettings state(stateFile, QSettings::IniFormat);
    state.beginGroup(u"FileManagers"_s);
    for (const QString& key : state.childKeys())
        memory.seen.insert(key, state.value(key).toString());
}

void setCustomCommands(const QString& open, const QString& reveal)
{
    const std::lock_guard lock(g_customMutex);
    g_customOpen = open;
    g_customReveal = reveal;
}

QString customProgram(const QString& commandLine)
{
    const QString program = pathtext::customCommand(commandLine, u"x"_s).program;
    if (program.isEmpty())
        return {};
    if (program.contains(u'\\') || program.contains(u'/')) {
        const QFileInfo info(program);
        return info.isFile() ? QDir::toNativeSeparators(info.absoluteFilePath()) : QString();
    }
    std::wstring found(32768, L'\0');
    const DWORD n = ::SearchPathW(nullptr, program.toStdWString().c_str(), L".exe", static_cast<DWORD>(found.size()),
        found.data(), nullptr);
    if (n == 0 || n >= found.size())
        return {};
    found.resize(n);
    return QString::fromStdWString(found);
}

} // namespace ws::filemanager
