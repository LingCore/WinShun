#include "TrayMenu.h"

#include "Phosphor.h"
#include "SystemTheme.h"
#include "platform/WindowEffects.h"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QObject>
#include <QPainter>
#include <QPalette>
#include <QScopeGuard>

#include <commctrl.h>
#include <oleacc.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws::traymenu {
namespace {

// Shared density for native tray and capture menus; QML menus have their own layout.
constexpr int kRowHeight = 28;
constexpr int kInfoHeight = 26;
constexpr int kSeparatorHeight = 7;
constexpr int kIconSize = 14;
constexpr int kIconBox = 18;
constexpr int kLabelLeft = 36;
constexpr int kTextRight = 12;
constexpr int kShortcutGap = 20;
constexpr int kVerticalPadding = 6;

// WCAG contrast of two opaque colours, and `over` laid on `under`.
double luminance(const QColor& c)
{
    const auto channel = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
}

double contrast(const QColor& a, const QColor& b)
{
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QColor composite(const QColor& over, const QColor& under)
{
    const double t = over.alphaF();
    return QColor::fromRgbF(float(over.redF() * t + under.redF() * (1 - t)), float(over.greenF() * t + under.greenF() * (1 - t)),
        float(over.blueF() * t + under.blueF() * (1 - t)));
}

struct Appearance {
    QFont body = QGuiApplication::font();
    QFont caption = body;
    QColor background, hover, text, selectedText, secondary, disabled, divider, accent;
    QColor hoverEdge; // invalid: none
    bool highContrast = false;
    qreal scale = 1;
    qreal radius = 0;
    qreal inset = 4; // the hover from the menu's sides
    int maxWidth = 360;

    Appearance(QObject* theme, POINT at)
    {
        const auto color = [theme](const char* name, QColor fallback) {
            return theme ? theme->property(name).value<QColor>() : fallback;
        };
        const QPalette palette = QGuiApplication::palette();
        background = color("menuBackground", palette.color(QPalette::Window));
        hover = color("menuHover", palette.color(QPalette::Highlight));
        text = color("text", palette.color(QPalette::WindowText));
        secondary = color("subtext", text);
        disabled = color("faint", palette.color(QPalette::Disabled, QPalette::WindowText));
        divider = color("divider", palette.color(QPalette::Mid));
        accent = color("accent", palette.color(QPalette::Highlight));
        selectedText = text;
        body.setPixelSize(theme ? theme->property("fontCaption").toInt() : 14);
        caption.setPixelSize(std::max(11, body.pixelSize() - 2));
        // The 界面风格's menu items (Theme reads its Look): their radius and
        // inset; the frame round them is Windows' (DWM's corners and edge).
        radius = theme ? theme->property("menuItemRadius").toReal() : 0;
        inset = theme ? std::clamp(theme->property("menuPadding").toReal(), 0.0, 12.0) : 4;
        // A hover that hardly differs from the menu (a style whose item rises
        // as a key face on a near-white menu): outlined, as the key's rim.
        if (contrast(composite(hover, background), background) < 1.06)
            hoverEdge = color("controlBorder", divider);
        UINT dpiX = 96, dpiY = 96;
        const HMONITOR monitor = ::MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST);
        if (SUCCEEDED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
            scale = dpiX / 96.0;
        MONITORINFO info {sizeof info};
        if (::GetMonitorInfoW(monitor, &info))
            maxWidth = std::max(120, std::min(maxWidth, int((info.rcWork.right - info.rcWork.left) / scale) - 32));
        HIGHCONTRASTW contrast {sizeof contrast};
        if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0)
            && (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
            highContrast = true;
            const auto system = [](int index) {
                const COLORREF c = ::GetSysColor(index);
                return QColor(GetRValue(c), GetGValue(c), GetBValue(c));
            };
            background = system(COLOR_MENU);
            hover = system(COLOR_HIGHLIGHT);
            text = secondary = system(COLOR_MENUTEXT);
            selectedText = system(COLOR_HIGHLIGHTTEXT);
            disabled = divider = system(COLOR_GRAYTEXT);
            accent = text;
            hoverEdge = QColor();
        }
    }
};

struct Row;
// Keep the accessibility structure first in the actual itemData object, as
// required by MSAA; the nontrivial Qt data lives behind its pointer.
struct ItemData {
    MSAAMENUINFO accessibility {};
    Row* row = nullptr;
};

struct Row {
    ItemData data;
    const shell::MenuItem* item = nullptr;
    QString label, shortcut;
    std::wstring accessibleName;
    bool separator = false;
    bool padding = false;
};

constexpr UINT kDrawSubmenuGlyphs = WM_APP + 0x152;
LRESULT CALLBACK popupProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

QString plainLabel(QStringView text)
{
    QString result;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] == u'&' && i + 1 < text.size()) {
            if (text[i + 1] != u'&')
                continue;
            ++i;
        }
        result += text[i];
    }
    return result;
}

QChar mnemonic(QStringView label)
{
    for (qsizetype i = 0; i + 1 < label.size(); ++i) {
        if (label[i] == u'&') {
            if (label[i + 1] != u'&')
                return label[i + 1].toCaseFolded();
            ++i;
        }
    }
    const QString text = plainLabel(label);
    return text.isEmpty() ? QChar() : text.front().toCaseFolded();
}

struct Menu {
    Appearance appearance;
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<HMENU> handles;
    struct Popup {
        HWND window;
        bool pending = false;
    };
    std::vector<Popup> popups;
    HBRUSH brush = nullptr;
    HMENU root = nullptr;
    std::function<void(HWND, HMENU)> shown;

    Menu(QObject* theme, POINT at)
        : appearance(theme, at)
    {
        const QColor c = appearance.background;
        brush = ::CreateSolidBrush(RGB(c.red(), c.green(), c.blue()));
    }
    ~Menu()
    {
        for (const auto& popup : popups)
            ::RemoveWindowSubclass(popup.window, popupProc, reinterpret_cast<UINT_PTR>(this));
        if (root)
            ::DestroyMenu(root);
        if (brush)
            ::DeleteObject(brush);
    }

    const Row* findRow(ULONG_PTR data) const
    {
        for (const auto& row : rows) {
            if (reinterpret_cast<ULONG_PTR>(&row->data) == data)
                return row.get();
        }
        return nullptr;
    }

    HMENU build(const std::vector<shell::MenuItem>& items)
    {
        HMENU handle = ::CreatePopupMenu();
        if (!handle)
            return nullptr;
        handles.push_back(handle);
        MENUINFO menuInfo {sizeof menuInfo};
        menuInfo.fMask = MIM_BACKGROUND | MIM_STYLE;
        menuInfo.dwStyle = MNS_NOCHECK;
        menuInfo.hbrBack = brush;
        ::SetMenuInfo(handle, &menuInfo);
        // Inert owner-drawn separators provide real menu padding: clicks in
        // the margins cannot execute the first or last command.
        static const shell::MenuItem spacer = shell::MenuItem::separator();
        for (std::size_t i = 0; i < items.size() + 2; ++i) {
            const bool padding = i == 0 || i == items.size() + 1;
            const auto& item = padding ? spacer : items[i - 1];
            auto row = std::make_unique<Row>();
            row->item = &item;
            row->padding = padding;
            row->separator = item.id == 0 && item.text.isEmpty() && item.children.empty();
            row->label = item.text.section(u'\t', 0, 0);
            row->shortcut = item.text.section(u'\t', 1);
            row->accessibleName = plainLabel(item.text).toStdWString();
            row->data.row = row.get();
            row->data.accessibility
                = {MSAA_MENU_SIG, static_cast<DWORD>(row->accessibleName.size()), row->accessibleName.data()};
            MENUITEMINFOW entry {sizeof entry};
            entry.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_STRING | MIIM_DATA;
            entry.fType = MFT_OWNERDRAW | (row->separator ? MFT_SEPARATOR : 0);
            entry.fState = (item.enabled && !row->separator ? MFS_ENABLED : MFS_DISABLED)
                | (item.checked ? MFS_CHECKED : 0) | (item.isDefault ? MFS_DEFAULT : 0);
            entry.wID = static_cast<UINT>(item.id);
            entry.dwTypeData = const_cast<wchar_t*>(reinterpret_cast<const wchar_t*>(item.text.utf16()));
            entry.dwItemData = reinterpret_cast<ULONG_PTR>(&row->data);
            if (!item.children.empty()) {
                entry.fMask |= MIIM_SUBMENU;
                entry.hSubMenu = build(item.children);
            }
            if (!::InsertMenuItemW(handle, static_cast<UINT>(::GetMenuItemCount(handle)), TRUE, &entry)
                && entry.hSubMenu)
                ::DestroyMenu(entry.hSubMenu);
            rows.push_back(std::move(row));
        }
        return handle;
    }

    void measure(MEASUREITEMSTRUCT& request) const
    {
        const auto& row = *reinterpret_cast<ItemData*>(request.itemData)->row;
        QFont font = row.item->secondary ? appearance.caption : appearance.body;
        font.setBold(row.item->isDefault);
        const qreal suffix = row.shortcut.isEmpty()
            ? 0
            : kShortcutGap + QFontMetricsF(appearance.caption).horizontalAdvance(row.shortcut);
        const qreal state = row.item->checked || !row.item->children.empty() ? kIconBox : 0;
        const qreal width = kLabelLeft + QFontMetricsF(font).horizontalAdvance(row.label) + suffix + state + kTextRight;
        request.itemWidth
            = qRound(std::clamp(width, qreal(std::min(208, appearance.maxWidth)), qreal(appearance.maxWidth))
                * appearance.scale);
        request.itemHeight = qRound((row.padding                  ? kVerticalPadding
                                            : row.separator       ? kSeparatorHeight
                                            : row.item->secondary ? kInfoHeight
                                                                  : kRowHeight)
            * appearance.scale);
    }

    void glyph(QPainter& painter, const QString& value, QRectF box, QColor color) const
    {
        if (value.isEmpty())
            return;
        const auto icon = phosphor::resolve(value);
        if (!icon)
            return;
        QFont font(icon->family);
        font.setPixelSize(qRound(kIconSize * phosphor::kScale));
        painter.setFont(font);
        painter.setPen(color);
        painter.drawText(box, Qt::AlignCenter, icon->text);
    }

    void draw(const DRAWITEMSTRUCT& request) const
    {
        const auto& row = *reinterpret_cast<ItemData*>(request.itemData)->row;
        const auto& a = appearance;
        QImage image(request.rcItem.right - request.rcItem.left, request.rcItem.bottom - request.rcItem.top,
            QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(a.scale);
        image.fill(a.background);
        QPainter painter(&image);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
        const qreal width = image.width() / a.scale, height = image.height() / a.scale;
        if (row.separator) {
            if (!row.padding)
                painter.fillRect(QRectF(8, std::floor(height / 2), width - 16, 1 / a.scale), a.divider);
        } else {
            const bool selected = (request.itemState & ODS_SELECTED) && row.item->enabled;
            if (selected) {
                const QRectF face(a.inset, 0, width - 2 * a.inset, height);
                painter.setPen(Qt::NoPen);
                painter.setBrush(a.hover);
                painter.drawRoundedRect(face, a.radius, a.radius);
                if (a.hoverEdge.isValid()) { // one device pixel, inside
                    const qreal hair = 1 / a.scale;
                    painter.setPen(QPen(a.hoverEdge, hair));
                    painter.setBrush(Qt::NoBrush);
                    const QRectF rim = face.adjusted(hair / 2, hair / 2, -hair / 2, -hair / 2);
                    painter.drawRoundedRect(rim, std::max(0.0, a.radius - hair / 2), std::max(0.0, a.radius - hair / 2));
                }
            }
            const QColor ink = selected ? a.selectedText
                : row.item->secondary   ? a.secondary
                : row.item->enabled     ? a.text
                                        : a.disabled;
            const QColor mark = selected && a.highContrast ? a.selectedText : a.accent;
            glyph(painter, row.item->glyph, QRectF(10, 0, kIconBox, height), row.item->isDefault ? mark : ink);
            const qreal suffix = row.shortcut.isEmpty() ? 0 : QFontMetricsF(a.caption).horizontalAdvance(row.shortcut);
            const qreal state = row.item->checked || !row.item->children.empty() ? kIconBox : 0;
            QFont font = row.item->secondary ? a.caption : a.body;
            font.setBold(row.item->isDefault);
            painter.setFont(font);
            painter.setPen(ink);
            const qreal available
                = std::max(qreal(0), width - kLabelLeft - kTextRight - state - (suffix ? suffix + kShortcutGap : 0));
            const QString label = QFontMetricsF(font).elidedText(row.label, Qt::ElideRight, available);
            painter.drawText(QRectF(kLabelLeft, 0, available, height),
                Qt::AlignVCenter | Qt::AlignLeft
                    | (request.itemState & ODS_NOACCEL ? Qt::TextHideMnemonic : Qt::TextShowMnemonic),
                label);
            painter.setFont(a.caption);
            painter.setPen(selected && a.highContrast ? a.selectedText : row.item->enabled ? a.secondary : a.disabled);
            painter.drawText(QRectF(width - kTextRight - state - suffix, 0, suffix, height),
                Qt::AlignVCenter | Qt::AlignRight, row.shortcut);
            if (row.item->checked)
                glyph(
                    painter, QString(QChar(0xE73E)), QRectF(width - kTextRight - kIconBox, 0, kIconBox, height), mark);
            if (!row.item->children.empty())
                glyph(painter, QString(QChar(0xE76C)), QRectF(width - 8 - kIconBox, 0, kIconBox, height), ink);
        }
        painter.end();
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = image.width();
        info.bmiHeader.biHeight = -image.height();
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        ::SetDIBitsToDevice(request.hDC, request.rcItem.left, request.rcItem.top, image.width(), image.height(), 0, 0,
            0, image.height(), image.constBits(), &info, DIB_RGB_COLORS);
    }

    void repaintSubmenuRows(HWND window, HDC dc = nullptr) const
    {
        MENUBARINFO bar {sizeof bar};
        if (!::GetMenuBarInfo(window, OBJID_CLIENT, 0, &bar))
            return;
        const bool release = !dc;
        if (!dc)
            dc = ::GetDC(window);
        if (!dc)
            return;
        const auto cleanup = qScopeGuard([&] {
            if (release)
                ::ReleaseDC(window, dc);
        });
        for (int i = 0; i < ::GetMenuItemCount(bar.hMenu); ++i) {
            MENUITEMINFOW entry {sizeof entry};
            entry.fMask = MIIM_DATA | MIIM_STATE | MIIM_SUBMENU;
            if (!::GetMenuItemInfoW(bar.hMenu, i, TRUE, &entry) || !entry.hSubMenu || !findRow(entry.dwItemData))
                continue;
            DRAWITEMSTRUCT request {};
            if (!::GetMenuItemRect(nullptr, bar.hMenu, i, &request.rcItem))
                continue;
            ::MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&request.rcItem), 2);
            request.hDC = dc;
            request.itemData = entry.dwItemData;
            request.itemState
                = (entry.fState & MFS_HILITE ? ODS_SELECTED : 0) | (entry.fState & MFS_DISABLED ? ODS_DISABLED : 0);
            draw(request);
        }
    }

    void scheduleSubmenuGlyphs()
    {
        for (auto& popup : popups) {
            if (!popup.pending) {
                popup.pending = true;
                ::PostMessageW(popup.window, kDrawSubmenuGlyphs, 0, 0);
            }
        }
    }
};

// Windows appends its system-coloured arrow after owner drawing. Repaint
// submenu rows after native painting and selection, keeping our Phosphor
// chevron readable in both themes without replacing native menu navigation.
LRESULT CALLBACK popupProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    auto& menu = *reinterpret_cast<Menu*>(data);
    if (message == kDrawSubmenuGlyphs) {
        for (auto& popup : menu.popups) {
            if (popup.window == window)
                popup.pending = false;
        }
        menu.repaintSubmenuRows(window);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(window, popupProc, id);
        std::erase_if(menu.popups, [window](const Menu::Popup& popup) { return popup.window == window; });
    }
    const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_PAINT || message == WM_PRINTCLIENT)
        menu.repaintSubmenuRows(window, message == WM_PRINTCLIENT ? reinterpret_cast<HDC>(wParam) : nullptr);
    return result;
}

LRESULT CALLBACK ownerProc(HWND owner, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
{
    auto& menu = *reinterpret_cast<Menu*>(data);
    if (message == WM_MEASUREITEM) {
        auto* request = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (request && request->CtlType == ODT_MENU && menu.findRow(request->itemData)) {
            menu.measure(*request);
            return TRUE;
        }
    } else if (message == WM_DRAWITEM) {
        auto* request = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (request && request->CtlType == ODT_MENU && menu.findRow(request->itemData)) {
            menu.draw(*request);
            menu.scheduleSubmenuGlyphs();
            return TRUE;
        }
    } else if (message == WM_MENUCHAR) {
        const HMENU handle = reinterpret_cast<HMENU>(lParam);
        const QChar key = QChar(LOWORD(wParam)).toCaseFolded();
        std::vector<int> matches;
        int highlighted = -1;
        for (int i = 0; i < ::GetMenuItemCount(handle); ++i) {
            MENUITEMINFOW entry {sizeof entry};
            entry.fMask = MIIM_DATA | MIIM_STATE;
            if (!::GetMenuItemInfoW(handle, i, TRUE, &entry) || !entry.dwItemData || (entry.fState & MFS_DISABLED))
                continue;
            const auto* row = menu.findRow(entry.dwItemData);
            if (!row)
                continue;
            if (entry.fState & MFS_HILITE)
                highlighted = i;
            if (!row->separator && mnemonic(row->label) == key)
                matches.push_back(i);
        }
        if (matches.size() == 1)
            return MAKELRESULT(matches.front(), MNC_EXECUTE);
        if (!matches.empty()) {
            const auto next = std::ranges::find_if(matches, [highlighted](int i) { return i > highlighted; });
            return MAKELRESULT(next == matches.end() ? matches.front() : *next, MNC_SELECT);
        }
        return MAKELRESULT(0, MNC_IGNORE);
    }
    return ::DefSubclassProc(owner, message, wParam, lParam);
}

thread_local Menu* currentMenu = nullptr;

void CALLBACK menuShown(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD, DWORD)
{
    if (!currentMenu || !window)
        return;
    MENUBARINFO bar {sizeof bar};
    if (!::GetMenuBarInfo(window, OBJID_CLIENT, 0, &bar)
        || std::ranges::find(currentMenu->handles, bar.hMenu) == currentMenu->handles.end())
        return;
    const auto id = reinterpret_cast<UINT_PTR>(currentMenu);
    if (std::ranges::none_of(currentMenu->popups, [window](const Menu::Popup& popup) { return popup.window == window; })
        && ::SetWindowSubclass(window, popupProc, id, reinterpret_cast<DWORD_PTR>(currentMenu))) {
        currentMenu->popups.push_back({window});
        currentMenu->scheduleSubmenuGlyphs();
    }
    win::setWindowCorners(window);
    if (auto callback = std::exchange(currentMenu->shown, {}))
        callback(window, currentMenu->root);
}

} // namespace

int popup(HWND owner, const std::vector<shell::MenuItem>& items, POINT screenPos, QObject* theme,
    std::function<void(HWND, HMENU)> shown)
{
    Menu menu(theme, screenPos);
    menu.root = menu.build(items);
    if (!menu.root)
        return 0;
    menu.shown = std::move(shown);
    const auto id = reinterpret_cast<UINT_PTR>(&menu);
    if (!::SetWindowSubclass(owner, ownerProc, id, reinterpret_cast<DWORD_PTR>(&menu)))
        return 0;
    Menu* previous = std::exchange(currentMenu, &menu);
    const HWINEVENTHOOK hook = ::SetWinEventHook(EVENT_SYSTEM_MENUPOPUPSTART, EVENT_SYSTEM_MENUPOPUPSTART, nullptr,
        menuShown, ::GetCurrentProcessId(), ::GetCurrentThreadId(), WINEVENT_OUTOFCONTEXT);
    const auto cleanup = qScopeGuard([&] {
        if (hook)
            ::UnhookWinEvent(hook);
        currentMenu = previous;
        ::RemoveWindowSubclass(owner, ownerProc, id);
    });
    ::SetForegroundWindow(owner);
    const int chosen = static_cast<int>(::TrackPopupMenuEx(
        menu.root, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screenPos.x, screenPos.y, owner, nullptr));
    ::PostMessageW(owner, WM_NULL, 0, 0);
    return chosen;
}

QImage render(const std::vector<shell::MenuItem>& items, QObject* theme, qreal scale, int highlighted)
{
    Menu menu(theme, POINT {0, 0});
    menu.appearance.scale = scale;
    menu.root = menu.build(items);
    if (!menu.root)
        return {};
    // The root's rows in order (build() adds a submenu's rows among them).
    std::vector<std::pair<ULONG_PTR, SIZE>> rows;
    LONG width = 0, height = 0;
    for (int i = 0; i < ::GetMenuItemCount(menu.root); ++i) {
        MENUITEMINFOW entry {sizeof entry};
        entry.fMask = MIIM_DATA;
        if (!::GetMenuItemInfoW(menu.root, i, TRUE, &entry) || !menu.findRow(entry.dwItemData))
            continue;
        MEASUREITEMSTRUCT request {ODT_MENU};
        request.itemData = entry.dwItemData;
        menu.measure(request);
        rows.push_back({entry.dwItemData, {static_cast<LONG>(request.itemWidth), static_cast<LONG>(request.itemHeight)}});
        width = std::max(width, static_cast<LONG>(request.itemWidth));
        height += static_cast<LONG>(request.itemHeight);
    }
    if (width <= 0 || height <= 0)
        return {};
    BITMAPINFO info {};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void* bits = nullptr;
    const HDC dc = ::CreateCompatibleDC(nullptr);
    const HBITMAP bitmap = dc ? ::CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    const auto cleanup = qScopeGuard([&] {
        if (bitmap)
            ::DeleteObject(bitmap);
        if (dc)
            ::DeleteDC(dc);
    });
    if (!bitmap)
        return {};
    const HGDIOBJ old = ::SelectObject(dc, bitmap);
    LONG top = 0;
    int item = -1; // the index into `items`: the first and last rows are padding
    for (const auto& [data, size] : rows) {
        const Row* row = menu.findRow(data);
        if (!row->padding)
            ++item;
        DRAWITEMSTRUCT request {};
        request.CtlType = ODT_MENU;
        request.itemState = ODS_NOACCEL | (item == highlighted && !row->padding ? ODS_SELECTED : 0);
        request.hDC = dc;
        request.rcItem = {0, top, width, top + size.cy};
        request.itemData = data;
        menu.draw(request);
        top += size.cy;
    }
    ::GdiFlush();
    QImage image = QImage(static_cast<const uchar*>(bits), width, height, width * 4, QImage::Format_RGB32).copy();
    ::SelectObject(dc, old);
    return image;
}

} // namespace ws::traymenu
