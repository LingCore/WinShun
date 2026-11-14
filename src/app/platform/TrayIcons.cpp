#include "TrayIcons.h"

#include <windows.h>
#include <objbase.h>
#include <UIAutomation.h>

#include <QFileInfo>
#include <QDir>
#include <QImage>
#include <QSet>
#include <QStandardPaths>

#include <climits>
#include <utility>
#include <vector>

#include "WindowSearch.h"

#pragma comment(lib, "uiautomationcore.lib")

using namespace Qt::StringLiterals;

namespace ws::traysearch {

QString trayKey(const QString& name)
{
    // "Filee Filee：按住 Ctrl 拖动文件" → "Filee"; "Argus Argus" → "Argus";
    // a tooltip whose first line is a long PATH (LocalSend) → "LocalSend".
    QString key = name;
    const qsizetype colon = key.indexOf(u'：');
    if (colon > 0)
        key = key.left(colon);
    if (key.length() > 60 || key.contains(u';')) {
        const qsizetype lastSpace = key.lastIndexOf(u' ');
        if (lastSpace >= 0)
            key = key.mid(lastSpace + 1);
    } else {
        const qsizetype space = key.indexOf(u' ');
        if (space > 0)
            key = key.left(space);
    }
    return key.trimmed();
}

namespace {

struct ComScope {
    HRESULT hr;
    ComScope() : hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope()
    {
        if (SUCCEEDED(hr))
            ::CoUninitialize();
    }
    bool usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

QString takeBstr(BSTR text)
{
    if (!text)
        return {};
    const QString value = QString::fromWCharArray(text, static_cast<int>(::SysStringLen(text)));
    ::SysFreeString(text);
    return value;
}

QString className(IUIAutomationElement* element)
{
    BSTR value = nullptr;
    element->get_CurrentClassName(&value);
    return takeBstr(value);
}

QString elementName(IUIAutomationElement* element)
{
    BSTR value = nullptr;
    element->get_CurrentName(&value);
    return takeBstr(value).trimmed();
}

bool sameProgram(const QString& a, const QString& b)
{
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
}

// Which window the icon belongs to. With the icon's own program known only
// that program's windows count: another program's window whose title merely
// holds the icon's name ("微信 · (myscoop) …") must not be taken for it.
bool windowMatches(const TrayItem& item, const SearchResult& window, const QString& program)
{
    const QString programName = QFileInfo(window.target).completeBaseName();
    if (!program.isEmpty() && !window.target.isEmpty()) {
        return sameProgram(window.target, program)
            || programName.compare(QFileInfo(program).completeBaseName(), Qt::CaseInsensitive) == 0;
    }
    return window.name.compare(item.name, Qt::CaseInsensitive) == 0
        || (!programName.isEmpty()
            && (item.key.compare(programName, Qt::CaseInsensitive) == 0
                || item.name.contains(programName, Qt::CaseInsensitive)));
}

// The icon exactly as the taskbar draws it: a small screen grab of the
// button's middle, saved as a PNG for the tile. The UI Automation element
// belongs to explorer and carries no picture of its own.
QString grabIconFile(const QString& name, const QRect& rect)
{
    if (rect.width() < 8 || rect.height() < 8)
        return {};
    const int side = qMax(8, qMin(rect.width(), rect.height()) * 2 / 3);
    const int x = rect.left() + (rect.width() - side) / 2;
    const int y = rect.top() + (rect.height() - side) / 2;
    HDC screen = ::GetDC(nullptr);
    if (!screen)
        return {};
    HDC memory = ::CreateCompatibleDC(screen);
    HBITMAP bitmap = ::CreateCompatibleBitmap(screen, side, side);
    HGDIOBJ old = ::SelectObject(memory, bitmap);
    QImage raw;
    if (::BitBlt(memory, 0, 0, side, side, screen, x, y, SRCCOPY)) {
        raw = QImage(side, side, QImage::Format_RGB32);
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = side;
        info.bmiHeader.biHeight = -side;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        if (!::GetDIBits(memory, bitmap, 0, side, raw.bits(), &info, DIB_RGB_COLORS))
            raw = QImage();
    }
    ::SelectObject(memory, old);
    ::DeleteObject(bitmap);
    ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
    if (raw.isNull())
        return {};
    // Key out the taskbar behind the icon: flood from the edges over the
    // pixels close to the edge colour. The icon's own pixels stay, even
    // where they share that colour in its middle.
    qint64 red = 0, green = 0, blue = 0;
    int samples = 0;
    const auto sample = [&](int sx, int sy) {
        const QRgb p = raw.pixel(sx, sy);
        red += qRed(p);
        green += qGreen(p);
        blue += qBlue(p);
        ++samples;
    };
    for (int i = 0; i < side; ++i) {
        sample(i, 0);
        sample(i, side - 1);
    }
    for (int j = 1; j + 1 < side; ++j) {
        sample(0, j);
        sample(side - 1, j);
    }
    const int backRed = static_cast<int>(red / samples);
    const int backGreen = static_cast<int>(green / samples);
    const int backBlue = static_cast<int>(blue / samples);
    constexpr int kKeyDistance = 40;
    std::vector<unsigned char> cleared(static_cast<std::size_t>(side) * side, 0);
    std::vector<std::pair<int, int>> stack;
    const auto tryKey = [&](int px, int py) {
        if (px < 0 || py < 0 || px >= side || py >= side)
            return;
        const std::size_t at = static_cast<std::size_t>(py) * side + px;
        if (cleared[at])
            return;
        const QRgb p = raw.pixel(px, py);
        const int dr = qRed(p) - backRed;
        const int dg = qGreen(p) - backGreen;
        const int db = qBlue(p) - backBlue;
        if (dr * dr + dg * dg + db * db <= kKeyDistance * kKeyDistance) {
            cleared[at] = 1;
            stack.emplace_back(px, py);
        }
    };
    for (int i = 0; i < side; ++i) {
        tryKey(i, 0);
        tryKey(i, side - 1);
    }
    for (int j = 0; j < side; ++j) {
        tryKey(0, j);
        tryKey(side - 1, j);
    }
    while (!stack.empty()) {
        const auto [px, py] = stack.back();
        stack.pop_back();
        tryKey(px - 1, py);
        tryKey(px + 1, py);
        tryKey(px, py - 1);
        tryKey(px, py + 1);
    }
    QImage image(side, side, QImage::Format_ARGB32);
    for (int py = 0; py < side; ++py) {
        for (int px = 0; px < side; ++px) {
            const QRgb p = raw.pixel(px, py);
            const int alpha = cleared[static_cast<std::size_t>(py) * side + px] ? 0 : 255;
            image.setPixel(px, py, qRgba(qRed(p), qGreen(p), qBlue(p), alpha));
        }
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + u"/winshun-tray-icons"_s;
    QDir().mkpath(dir);
    const QString key = name + u':' + QString::number(rect.left()) + u':' + QString::number(rect.top());
    const QString file = dir + u'/' + QString::number(static_cast<qulonglong>(qHash(key)), 16) + u".png";
    if (!image.save(file, "PNG"))
        return {};
    return file;
}

void collect(IUIAutomation* uia, IUIAutomationElement* root, QVector<TrayItem>* items)
{
    IUIAutomationCondition* all = nullptr;
    if (FAILED(uia->CreateTrueCondition(&all)) || !all)
        return;
    IUIAutomationElementArray* array = nullptr;
    const HRESULT found = root->FindAll(TreeScope_Descendants, all, &array);
    all->Release();
    if (FAILED(found) || !array)
        return;
    int length = 0;
    array->get_Length(&length);
    for (int i = 0; i < length; ++i) {
        IUIAutomationElement* element = nullptr;
        if (FAILED(array->GetElement(i, &element)) || !element)
            continue;
        if (className(element) == u"SystemTray.NormalButton"_s) {
            const QString text = elementName(element);
            if (!text.isEmpty()) {
                RECT rect {};
                element->get_CurrentBoundingRectangle(&rect);
                TrayItem item;
                const qsizetype newline = text.indexOf(u'\n');
                item.name = newline > 0 ? text.left(newline).trimmed() : text;
                item.subtitle = newline > 0 ? text.mid(newline + 1).trimmed() : QString();
                item.key = trayKey(item.name);
                item.rect = QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
                if (item.rect.width() > 0 && item.rect.height() > 0)
                    items->append(item);
            }
        }
        element->Release();
    }
    array->Release();
}

IUIAutomation* createUia()
{
    IUIAutomation* uia = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                                  reinterpret_cast<void**>(&uia))))
        return nullptr;
    return uia;
}

// The taskbar's tray, and the overflow panel while it is open.
void trayRoots(IUIAutomation* uia, QVector<IUIAutomationElement*>* roots)
{
    const HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) {
        IUIAutomationElement* root = nullptr;
        if (SUCCEEDED(uia->ElementFromHandle(tray, &root)) && root)
            roots->append(root);
    }
    for (const wchar_t* windowClass : {L"TopLevelWindowForOverflowXaml", L"NotifyIconOverflowWindow"}) {
        const HWND overflow = ::FindWindowW(windowClass, nullptr);
        if (!overflow)
            continue;
        IUIAutomationElement* root = nullptr;
        if (SUCCEEDED(uia->ElementFromHandle(overflow, &root)) && root)
            roots->append(root);
    }
}

IUIAutomationElement* findElement(IUIAutomation* uia, const TrayItem& item)
{
    QVector<IUIAutomationElement*> roots;
    trayRoots(uia, &roots);
    IUIAutomationElement* best = nullptr;
    int bestDistance = INT_MAX;
    for (IUIAutomationElement* root : roots) {
        IUIAutomationCondition* all = nullptr;
        if (SUCCEEDED(uia->CreateTrueCondition(&all)) && all) {
            IUIAutomationElementArray* array = nullptr;
            if (SUCCEEDED(root->FindAll(TreeScope_Descendants, all, &array)) && array) {
                int length = 0;
                array->get_Length(&length);
                for (int i = 0; i < length; ++i) {
                    IUIAutomationElement* element = nullptr;
                    if (FAILED(array->GetElement(i, &element)) || !element)
                        continue;
                    bool keep = false;
                    if (className(element) == u"SystemTray.NormalButton"_s && elementName(element) == item.name) {
                        RECT rect {};
                        element->get_CurrentBoundingRectangle(&rect);
                        const int distance = qAbs(static_cast<int>(rect.left) - item.rect.left())
                            + qAbs(static_cast<int>(rect.top) - item.rect.top());
                        if (!best || distance < bestDistance) {
                            if (best)
                                best->Release();
                            best = element;
                            bestDistance = distance;
                            keep = true;
                        }
                    }
                    if (!keep)
                        element->Release();
                }
                array->Release();
            }
            all->Release();
        }
        root->Release();
    }
    return best;
}

void clickAt(const QRect& rect)
{
    ::SetCursorPos(rect.left() + rect.width() / 2, rect.top() + rect.height() / 2);
    INPUT input[2] = {};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    ::SendInput(2, input, sizeof(INPUT));
}

void rightClickAt(const QRect& rect)
{
    ::SetCursorPos(rect.left() + rect.width() / 2, rect.top() + rect.height() / 2);
    INPUT input[2] = {};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    ::SendInput(2, input, sizeof(INPUT));
}

void escapeKey()
{
    INPUT input[2] = {};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_ESCAPE;
    input[1].type = INPUT_KEYBOARD;
    input[1].ki.wVk = VK_ESCAPE;
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(2, input, sizeof(INPUT));
}

bool windowAppeared(const TrayItem& item, const QString& program)
{
    const SearchResults windows = windowsearch::snapshot();
    for (const SearchResult& window : windows) {
        if (windowMatches(item, window, program))
            return true;
    }
    return false;
}

// The icon's own click through UI Automation: no pointer movement. False
// when the element has no invoke pattern.
bool invokeIcon(const TrayItem& item)
{
    ComScope com;
    if (!com.usable())
        return false;
    IUIAutomation* uia = createUia();
    if (!uia)
        return false;
    IUIAutomationElement* element = findElement(uia, item);
    bool done = false;
    if (element) {
        IUIAutomationInvokePattern* invoke = nullptr;
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_InvokePatternId, IID_IUIAutomationInvokePattern,
                                                   reinterpret_cast<void**>(&invoke)))
            && invoke) {
            done = SUCCEEDED(invoke->Invoke());
            invoke->Release();
        }
        element->Release();
    }
    uia->Release();
    return done;
}

// The visible top-level windows, to tell whether a click answered at all: a
// menu counts as an answer too, and must not be clicked away by a second one.
QSet<quintptr> visibleTopLevels()
{
    QSet<quintptr> windows;
    ::EnumWindows([](HWND window, LPARAM data) -> BOOL {
        if (::IsWindowVisible(window))
            reinterpret_cast<QSet<quintptr>*>(data)->insert(reinterpret_cast<quintptr>(window));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

bool anyNewWindow(const QSet<quintptr>& before)
{
    struct Context {
        const QSet<quintptr>* before;
        bool* found;
    };
    bool found = false;
    Context context {&before, &found};
    ::EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto& context = *reinterpret_cast<Context*>(data);
        if (::IsWindowVisible(window) && !context.before->contains(reinterpret_cast<quintptr>(window))) {
            *context.found = true;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return found;
}

struct MenuEntry {
    QString name;
    bool enabled;
    RECT rect;
};

struct MenuSearch {
    IUIAutomation* uia;
    QVector<MenuEntry>* entries;
    bool found;
};

// The popup menu an application opened: a window without a caption, owned or
// popup, whose UI Automation tree holds menu items. Windows order: the first
// one found is the topmost.
BOOL CALLBACK menuWindowProc(HWND window, LPARAM data)
{
    auto& search = *reinterpret_cast<MenuSearch*>(data);
    if (search.found)
        return FALSE;
    if (!::IsWindowVisible(window))
        return TRUE;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    if (!pid || pid == ::GetCurrentProcessId())
        return TRUE;
    wchar_t cls[128] {};
    ::GetClassNameW(window, cls, 128);
    const QString className = QString::fromWCharArray(cls);
    if (className == u"Shell_TrayWnd"_s || className == u"Shell_SecondaryTrayWnd"_s)
        return TRUE;
    const LONG_PTR style = ::GetWindowLongPtrW(window, GWL_STYLE);
    const bool caption = (style & WS_CAPTION) == WS_CAPTION;
    const bool popup = (style & WS_POPUP) != 0;
    const bool owned = ::GetWindow(window, GW_OWNER) != nullptr;
    if (caption || (!popup && !owned))
        return TRUE;
    IUIAutomationElement* root = nullptr;
    if (FAILED(search.uia->ElementFromHandle(window, &root)) || !root)
        return TRUE;
    IUIAutomationCondition* condition = nullptr;
    VARIANT value;
    ::VariantInit(&value);
    value.vt = VT_I4;
    value.lVal = UIA_MenuItemControlTypeId;
    if (SUCCEEDED(search.uia->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &condition)) && condition) {
        IUIAutomationElementArray* array = nullptr;
        if (SUCCEEDED(root->FindAll(TreeScope_Descendants, condition, &array)) && array) {
            int length = 0;
            array->get_Length(&length);
            for (int i = 0; i < length; ++i) {
                IUIAutomationElement* element = nullptr;
                if (FAILED(array->GetElement(i, &element)) || !element)
                    continue;
                BSTR name = nullptr;
                element->get_CurrentName(&name);
                const QString text = takeBstr(name).trimmed();
                BOOL enabled = TRUE;
                element->get_CurrentIsEnabled(&enabled);
                RECT rect {};
                element->get_CurrentBoundingRectangle(&rect);
                if (!text.isEmpty() && rect.right > rect.left)
                    search.entries->append(MenuEntry {text, enabled != FALSE, rect});
                element->Release();
            }
            array->Release();
        }
        condition->Release();
    }
    root->Release();
    if (!search.entries->isEmpty()) {
        search.found = true;
        return FALSE;
    }
    return TRUE;
}

bool readMenu(QVector<MenuEntry>* entries)
{
    ComScope com;
    if (!com.usable())
        return false;
    IUIAutomation* uia = createUia();
    if (!uia)
        return false;
    MenuSearch search {uia, entries, false};
    ::EnumWindows(menuWindowProc, reinterpret_cast<LPARAM>(&search));
    uia->Release();
    return !entries->isEmpty();
}

} // namespace

QVector<TrayItem> snapshot()
{
    ComScope com;
    QVector<TrayItem> items;
    if (!com.usable())
        return items;
    IUIAutomation* uia = createUia();
    if (!uia)
        return items;
    QVector<IUIAutomationElement*> roots;
    trayRoots(uia, &roots);
    for (IUIAutomationElement* root : roots) {
        collect(uia, root, &items);
        root->Release();
    }
    uia->Release();
    return items;
}

bool activate(const TrayItem& item, bool doubleClick, bool* usedDouble, const QString& program)
{
    if (usedDouble)
        *usedDouble = false;
    // The window it belongs to: its program's own window, or, the program
    // unknown, a window named exactly like the icon.
    const SearchResults windows = windowsearch::snapshot();
    for (const SearchResult& window : windows) {
        if (!windowMatches(item, window, program))
            continue;
        if (windowsearch::activate(window) == windowsearch::Activation::Switched)
            return true;
    }

    // No window of its own: click its tray icon through UI Automation, which
    // leaves the pointer where it is. A tray that answers with a window or a
    // menu took the single click; one that answers with nothing wants a
    // double click, and is remembered as such by the caller.
    if (!doubleClick) {
        const QSet<quintptr> before = visibleTopLevels();
        invokeIcon(item);
        ::Sleep(150);
        if (windowAppeared(item, program) || anyNewWindow(before))
            return true;
    }

    // A tray that takes only a real double click: the pointer is put back
    // exactly where it was. The icon may have moved or gone meanwhile: click
    // the place it is at now, and nothing when it is gone.
    QRect latest = item.rect;
    bool present = false;
    for (const TrayItem& current : snapshot()) {
        if (current.name == item.name) {
            latest = current.rect;
            present = true;
            break;
        }
    }
    if (!present)
        return false;
    POINT saved {};
    ::GetCursorPos(&saved);
    clickAt(latest);
    ::Sleep(60);
    clickAt(latest);
    ::Sleep(120);
    ::SetCursorPos(saved.x, saved.y);
    if (usedDouble)
        *usedDouble = true;
    return true;
}

QVector<TrayMenuItem> openMenu(const TrayItem& item)
{
    rightClickAt(item.rect);
    ::Sleep(200);
    QVector<MenuEntry> entries;
    if (!readMenu(&entries))
        return {}; // a menu no reader can enumerate: leave it open on the taskbar
    escapeKey();
    QVector<TrayMenuItem> result;
    result.reserve(entries.size());
    for (const MenuEntry& entry : entries)
        result.append(TrayMenuItem {entry.name, entry.enabled});
    return result;
}

bool chooseMenuItem(const TrayItem& item, const QStringList& names, int index)
{
    rightClickAt(item.rect);
    ::Sleep(200);
    QVector<MenuEntry> entries;
    if (!readMenu(&entries) || entries.isEmpty())
        return false;
    int at = -1;
    if (index >= 0 && index < names.size()) {
        for (int i = 0; i < entries.size(); ++i) {
            if (entries[i].enabled && entries[i].name == names[index]) {
                at = i;
                break;
            }
        }
    }
    if (at < 0 && index >= 0 && index < entries.size() && entries[index].enabled)
        at = index;
    if (at < 0) {
        escapeKey();
        return false;
    }
    const RECT rect = entries[at].rect;
    ::SetCursorPos((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
    INPUT input[2] = {};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    ::SendInput(2, input, sizeof(INPUT));
    return true;
}

bool showMenu(const TrayItem& item)
{
    ComScope com;
    if (!com.usable())
        return false;
    IUIAutomation* uia = createUia();
    if (!uia)
        return false;
    IUIAutomationElement* element = findElement(uia, item);
    QRect rect = item.rect;
    if (element) {
        RECT found {};
        element->get_CurrentBoundingRectangle(&found);
        rect = QRect(found.left, found.top, found.right - found.left, found.bottom - found.top);
        element->Release();
    }
    uia->Release();
    if (rect.isEmpty())
        return false;
    ::SetCursorPos(rect.left() + rect.width() / 2, rect.top() + rect.height() / 2);
    INPUT input[2] = {};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    ::SendInput(2, input, sizeof(INPUT));
    return true;
}

} // namespace ws::traysearch
