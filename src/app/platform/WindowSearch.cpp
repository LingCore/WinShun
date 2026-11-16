#include "WindowSearch.h"
#include "Query.h"
#include "Win32Util.h"

#include <QFileInfo>
#include <dwmapi.h>
#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace ws::windowsearch {
namespace {

bool taskWindow(HWND window)
{
    if (!::IsWindow(window) || !::IsWindowVisible(window) || !::IsWindowEnabled(window)
        || window == ::GetShellWindow())
        return false;
    const auto style = ::GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))
        return false;
    if (::GetWindow(window, GW_OWNER) && !(style & WS_EX_APPWINDOW))
        return false;
    DWORD cloaked = 0;
    return FAILED(::DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || !cloaked;
}

// GetWindowText sends WM_GETTEXT to a window of this process, which waits
// for the thread that owns it; InternalGetWindowText reads the caption kept
// by the system, as GetWindowText does for other processes' windows.
QString caption(HWND window, DWORD process)
{
    std::array<wchar_t, 2048> text {};
    const int size = static_cast<int>(text.size());
    const int length = process == ::GetCurrentProcessId() ? ::InternalGetWindowText(window, text.data(), size)
                                                          : ::GetWindowTextW(window, text.data(), size);
    return QString::fromWCharArray(text.data(), length).trimmed();
}

} // namespace

SearchResults snapshot(Own own)
{
    struct Context { DWORD excluded; SearchResults rows; } context {own == Own::Excluded ? ::GetCurrentProcessId() : 0, {}};
    ::EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto& context = *reinterpret_cast<Context*>(data);
        DWORD pid = 0;
        const DWORD tid = ::GetWindowThreadProcessId(window, &pid);
        if (!pid || pid == context.excluded || !taskWindow(window))
            return TRUE;
        const QString title = caption(window, pid);
        if (title.isEmpty())
            return TRUE;
        SearchResult row;
        row.name = title;
        row.app = AppKind::Window;
        row.windowHandle = reinterpret_cast<quintptr>(window);
        row.windowProcess = pid;
        row.windowThread = tid;
        row.path = u"winshun-window:%1/%2/%3"_s.arg(pid).arg(tid).arg(row.windowHandle);
        if (win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)); process.valid()) {
            std::array<wchar_t, 32768> path {};
            DWORD length = static_cast<DWORD>(path.size());
            if (::QueryFullProcessImageNameW(process.get(), 0, path.data(), &length))
                row.target = QString::fromWCharArray(path.data(), static_cast<int>(length));
        }
        context.rows.append(std::move(row));
        return context.rows.size() < 256; // no unbounded caption/process scan per key
    }, reinterpret_cast<LPARAM>(&context));
    return context.rows;
}

SearchResults matching(const SearchResults& windows, const QString& text, bool regex)
{
    const auto query = parseQuery(text, regex);
    if (query.isEmpty() || !query.error.isEmpty() || query.hasFilters())
        return {};
    const NameMatcher matcher(query);
    SearchResults rows;
    for (const auto& window : windows) {
        const QString name = QFileInfo(window.target).completeBaseName();
        const int score = std::max(matcher.matchName(window.name.toStdString()),
            matcher.matchName((window.name + u' ' + name).toStdString()));
        if (score < 0)
            continue;
        auto row = window;
        row.score = score;
        rows.append(std::move(row));
    }
    // Equal matches retain front-to-back order; multiple documents/windows
    // from one application remain distinct and selectable.
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    if (rows.size() > 8)
        rows.resize(8);
    return rows;
}

bool current(const SearchResult& row)
{
    if (!row.isWindow() || !row.windowHandle || !row.windowProcess)
        return false;
    const HWND window = reinterpret_cast<HWND>(row.windowHandle);
    DWORD pid = 0;
    const DWORD tid = ::GetWindowThreadProcessId(window, &pid);
    return pid == row.windowProcess && tid == row.windowThread && taskWindow(window)
        && caption(window, pid) == row.name;
}

Activation activate(const SearchResult& row)
{
    if (!current(row))
        return Activation::Gone;
    const HWND window = reinterpret_cast<HWND>(row.windowHandle);
    if (::IsIconic(window))
        ::ShowWindowAsync(window, SW_RESTORE);
    return ::SetForegroundWindow(window) ? Activation::Switched : Activation::Refused;
}

} // namespace ws::windowsearch
