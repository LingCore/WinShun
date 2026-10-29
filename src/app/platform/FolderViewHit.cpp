#include "FolderViewHit.h"

#include <ole2.h> // before oleacc.h and uiautomation.h: WIN32_LEAN_AND_MEAN keeps it out of windows.h

#include <oleacc.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <cwchar>
#include <format>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace ws::folderview {

namespace {

bool hasClass(HWND window, const wchar_t* name)
{
    wchar_t className[64] {};
    ::GetClassNameW(window, className, static_cast<int>(std::size(className)));
    return std::wcscmp(className, name) == 0;
}

std::wstring classOf(HWND window)
{
    wchar_t className[64] {};
    ::GetClassNameW(window, className, static_cast<int>(std::size(className)));
    return className;
}

long roleOf(IAccessible* object, LONG child)
{
    VARIANT id {};
    id.vt = VT_I4;
    id.lVal = child;
    VARIANT role {};
    if (FAILED(object->get_accRole(id, &role)))
        return 0;
    const long value = role.vt == VT_I4 ? role.lVal : 0;
    ::VariantClear(&role);
    return value;
}

// "LIST(33)": the roles the walk meets by name, any other by number.
std::wstring roleText(long role)
{
    const wchar_t* name = L"";
    switch (role) {
    case ROLE_SYSTEM_WINDOW: name = L"WINDOW"; break;
    case ROLE_SYSTEM_CLIENT: name = L"CLIENT"; break;
    case ROLE_SYSTEM_PANE: name = L"PANE"; break;
    case ROLE_SYSTEM_SCROLLBAR: name = L"SCROLLBAR"; break;
    case ROLE_SYSTEM_GROUPING: name = L"GROUPING"; break;
    case ROLE_SYSTEM_LIST: name = L"LIST"; break;
    case ROLE_SYSTEM_LISTITEM: name = L"LISTITEM"; break;
    case ROLE_SYSTEM_TEXT: name = L"TEXT"; break;
    case ROLE_SYSTEM_PUSHBUTTON: name = L"PUSHBUTTON"; break;
    case ROLE_SYSTEM_SPLITBUTTON: name = L"SPLITBUTTON"; break;
    default: break;
    }
    return std::format(L"{}({})", name, role);
}

std::wstring resultText(HRESULT hr)
{
    return std::format(L"0x{:08X}", static_cast<unsigned long>(hr));
}

void note(std::wstring* steps, const std::wstring& step)
{
    if (!steps)
        return;
    if (!steps->empty())
        *steps += L" | ";
    *steps += step;
}

// The first child of a group: a group again for the whole group (its
// header), a button (collapse) for the header itself. Both are groups to
// accessibility, and their class names (UIGroupItem, UIGroupHeader) cannot be
// had: UI Automation will not take these objects (docs/pitfalls.md).
long firstChildRole(IAccessible* object)
{
    VARIANT child {};
    LONG got = 0;
    if (FAILED(::AccessibleChildren(object, 0, 1, &child, &got)) || got != 1)
        return 0;
    long role = 0;
    if (child.vt == VT_DISPATCH && child.pdispVal) {
        ComPtr<IAccessible> accessible;
        if (SUCCEEDED(child.pdispVal->QueryInterface(IID_PPV_ARGS(&accessible))))
            role = roleOf(accessible.Get(), CHILDID_SELF);
    } else if (child.vt == VT_I4) {
        role = roleOf(object, child.lVal);
    }
    ::VariantClear(&child);
    return role;
}

// Through Microsoft Active Accessibility, down through what the point is on
// to the innermost part. The desktop's list (SysListView32, oleacc's own
// proxy): the list itself on empty space, an item (a simple child) on an
// icon. File Explorer's items view (DirectUI) measured on Windows 11
// (2026-10-13): the list itself in the gaps of a view without groups and
// below its last row; a group where a view with groups has gaps; an item, a
// cell (text), a header button or a scroll bar's parts elsewhere. Windows 10
// builds its tree differently (the list inside a "Shell Folder View" pane);
// UI Automation (below) decides there. `steps`: what it met, for the journal.
bool accessibleBlank(const Hit& hit, std::wstring* steps)
{
    ComPtr<IAccessible> object;
    const HRESULT got = ::AccessibleObjectFromWindow(hit.view, OBJID_CLIENT, IID_PPV_ARGS(&object));
    if (FAILED(got) || !object) {
        note(steps, L"no object " + resultText(got));
        return false;
    }
    if (steps)
        note(steps, roleText(roleOf(object.Get(), CHILDID_SELF)));
    for (int depth = 0; depth < 6; ++depth) {
        VARIANT at {};
        const HRESULT hr = object->accHitTest(hit.pt.x, hit.pt.y, &at);
        if (hr != S_OK) {
            note(steps, std::format(L"hit {} vt {}", resultText(hr), at.vt));
            ::VariantClear(&at);
            return false;
        }
        if (at.vt == VT_DISPATCH && at.pdispVal) {
            ComPtr<IAccessible> inner;
            const HRESULT asked = at.pdispVal->QueryInterface(IID_PPV_ARGS(&inner));
            ::VariantClear(&at);
            if (FAILED(asked) || !inner) {
                note(steps, L"DISP no IAccessible");
                return false;
            }
            const long role = roleOf(inner.Get(), CHILDID_SELF);
            if (steps) {
                HWND own = nullptr;
                ::WindowFromAccessibleObject(inner.Get(), &own);
                note(steps, L"DISP " + roleText(role) + (own == hit.view ? L"" : L" " + classOf(own)));
            }
            // A window of its own inside (the scroll bars' hosts): not blank.
            if (role == ROLE_SYSTEM_WINDOW || role == ROLE_SYSTEM_CLIENT || role == ROLE_SYSTEM_SCROLLBAR)
                return false;
            object = std::move(inner);
            continue;
        }
        if (at.vt != VT_I4) {
            note(steps, std::format(L"vt {}", at.vt));
            return false; // outside
        }
        if (at.lVal != CHILDID_SELF) {
            note(steps, std::format(L"child {} {}", at.lVal, roleText(roleOf(object.Get(), at.lVal))));
            return false; // a simple child: an icon on the desktop
        }
        const long role = roleOf(object.Get(), CHILDID_SELF);
        note(steps, L"SELF " + roleText(role));
        switch (role) {
        case ROLE_SYSTEM_LIST:
            return true;
        case ROLE_SYSTEM_GROUPING: {
            const long first = firstChildRole(object.Get());
            note(steps, L"first " + roleText(first));
            return first == ROLE_SYSTEM_GROUPING; // the group, not its header
        }
        default:
            return false;
        }
    }
    note(steps, L"too deep");
    return false;
}

enum class Verdict { Blank, NotBlank, Unknown };

// File Explorer's items view through UI Automation: the element at the
// point, by its class name. The view itself (UIItemsView: its gaps, below
// the last row) and a group's gaps (UIGroupItem) are blank; an item and its
// parts (UIItem, UIProperty, UIImage), a group's header (UIGroupHeader), the
// column headers (UIViewHeader, UIColumnHeader) and the scroll bars are not.
// QTTabBar has gone by the same two names since Windows 7
// (ExtendedItemsView::PointIsBackground); the accessibility tree around the
// list is not alike from one Windows to the next (GitHub #45). Made for this
// use, as the file dialogs' (FileDialog.cpp), and given up on soon should
// Explorer hang. Unknown: no answer; the walk above decides then.
Verdict automationVerdict(const Hit& hit, BlankTrace* trace)
{
    ComPtr<IUIAutomation> automation;
    if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))
        && FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))))
        return Verdict::Unknown;
    ComPtr<IUIAutomation2> timed;
    if (SUCCEEDED(automation.As(&timed))) {
        timed->put_ConnectionTimeout(1000);
        timed->put_TransactionTimeout(1000);
    }
    ComPtr<IUIAutomationCacheRequest> cache;
    if (FAILED(automation->CreateCacheRequest(&cache)) || !cache)
        return Verdict::Unknown;
    cache->AddProperty(UIA_ClassNamePropertyId); // never the Name: an item's is a file's
    cache->AddProperty(UIA_ControlTypePropertyId);
    cache->AddProperty(UIA_ProcessIdPropertyId);
    ComPtr<IUIAutomationElement> element;
    const HRESULT hr = automation->ElementFromPointBuildCache(hit.pt, cache.Get(), &element);
    if (trace)
        trace->uiaResult = hr;
    if (FAILED(hr) || !element)
        return Verdict::Unknown;
    BSTR name = nullptr;
    element->get_CachedClassName(&name);
    const std::wstring className = name ? name : L"";
    ::SysFreeString(name);
    int process = 0;
    element->get_CachedProcessId(&process);
    DWORD viewProcess = 0;
    ::GetWindowThreadProcessId(hit.view, &viewProcess);
    const bool other = static_cast<DWORD>(process) != viewProcess;
    if (trace) {
        CONTROLTYPEID type = 0;
        element->get_CachedControlType(&type);
        trace->uiaClass = className;
        trace->uiaControlType = type;
        trace->uiaOtherProcess = other;
    }
    if (other)
        return Verdict::NotBlank; // something else is there by now
    return className == L"UIItemsView" || className == L"UIGroupItem" ? Verdict::Blank : Verdict::NotBlank;
}

} // namespace

Hit classify(HWND window, POINT pt, bool classicList)
{
    Hit hit;
    if (!window)
        return hit;
    const HWND parent = ::GetParent(window);
    const HWND root = ::GetAncestor(window, GA_ROOT);
    if (!parent || !root || !hasClass(parent, L"SHELLDLL_DefView"))
        return hit;
    const bool list = hasClass(window, L"SysListView32");
    if ((hasClass(window, L"DirectUIHWND") || (list && classicList))
        && (hasClass(root, L"CabinetWClass") || hasClass(root, L"ExploreWClass"))) {
        // The items view; or the classic list view in its place, which
        // ExplorerPatcher ("Enable SysListView32 for Explorer views") and
        // Windhawk's syslistview32-enabler bring back (GitHub #45).
        hit.surface = Surface::Explorer;
        // Windows 11 has a window per tab; the one clicked is in front.
        hit.tab = root;
        for (HWND up = parent; up && up != root; up = ::GetParent(up)) {
            if (hasClass(up, L"ShellTabWindowClass")) {
                hit.tab = up;
                break;
            }
        }
    } else if (list && (hasClass(root, L"Progman") || hasClass(root, L"WorkerW"))) {
        hit.surface = Surface::Desktop;
    } else {
        return hit;
    }
    hit.view = window;
    hit.root = root;
    hit.pt = pt;
    return hit;
}

Hit classifyWindow(HWND root)
{
    Hit hit;
    if (!root || ::GetAncestor(root, GA_ROOT) != root)
        return hit;
    if (hasClass(root, L"CabinetWClass") || hasClass(root, L"ExploreWClass")) {
        hit.surface = Surface::Explorer;
        // A ShellTabWindowClass child per tab, the one on top first among
        // them (Windows 10 has one: the window's only tab).
        const HWND tab = ::FindWindowExW(root, nullptr, L"ShellTabWindowClass", nullptr);
        hit.tab = tab ? tab : root;
    } else if ((hasClass(root, L"Progman") || hasClass(root, L"WorkerW"))
        && ::FindWindowExW(root, nullptr, L"SHELLDLL_DefView", nullptr)) {
        hit.surface = Surface::Desktop;
    } else {
        return hit;
    }
    hit.root = root;
    return hit;
}

bool isOpusLister(HWND root)
{
    return root && ::GetAncestor(root, GA_ROOT) == root && hasClass(root, L"dopus.lister");
}

bool isOpusFileDisplay(HWND root, HWND focus)
{
    return isOpusLister(root) && focus && ::GetAncestor(focus, GA_ROOT) == root && hasClass(focus, L"dopus.filedisplay")
        && hasClass(::GetParent(focus), L"dopus.filedisplaycontainer");
}

bool isBlank(const Hit& hit, BlankTrace* trace)
{
    if (hit.surface == Surface::None || !::IsWindow(hit.view) || ::IsHungAppWindow(hit.root))
        return false;
    if (hasClass(hit.view, L"DirectUIHWND")) {
        const Verdict verdict = automationVerdict(hit, trace);
        if (verdict != Verdict::Unknown) {
            if (trace)
                trace->by = "uia";
            return verdict == Verdict::Blank;
        }
    }
    if (trace)
        trace->by = "msaa";
    return accessibleBlank(hit, trace ? &trace->msaa : nullptr);
}

std::wstring accessibleTrace(const Hit& hit)
{
    std::wstring steps;
    if (hit.view && ::IsWindow(hit.view) && !::IsHungAppWindow(hit.root))
        accessibleBlank(hit, &steps);
    return steps;
}

std::wstring classChain(HWND window)
{
    std::wstring chain;
    for (int level = 0; window && level < 12; ++level, window = ::GetParent(window)) {
        if (!chain.empty())
            chain += L'<';
        chain += classOf(window);
        if (::GetAncestor(window, GA_ROOT) == window)
            break;
    }
    return chain;
}

bool inShellView(HWND window)
{
    const HWND parent = window ? ::GetParent(window) : nullptr;
    return parent && hasClass(parent, L"SHELLDLL_DefView");
}

} // namespace ws::folderview
