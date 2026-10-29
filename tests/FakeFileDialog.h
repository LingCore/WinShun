#pragma once

// A window tree shaped like an Open dialog of Windows Vista and later, as far
// as filedialog::kind / dialognav::classify and the address-bar route look at
// it: the view host, the address bar's crumbs ("Address: <folder>" in their
// caption) with the text box made by an earlier edit, and the file name box
// (cmb13). The text box does what the real one does on Enter: the crumbs show
// the folder typed, and it closes. All windows of this thread, so the
// messages the route sends (SendMessageTimeout) arrive at once.
//
// Or with no text box yet: a click on the crumbs (posted, so handed over only
// while this thread takes messages) makes it, shown as by most dialogs, or
// left hidden as by one that is not DPI aware (docs/pitfalls.md).
//
// The containers are classes of this module named as the real ones (for the
// windows a module makes, its own class comes before another of the same
// name); the boxes are real combo boxes with their edit fields, as there.

#include <windows.h>

#include <string>
#include <vector>

namespace fakedialog {

constexpr int kFileNameCombo = 0x47C; // cmb13

// The address bar's text box when the dialog comes up.
enum class AddressBox {
    Hidden, // made by an earlier edit, closed since
    ShownByClick, // none yet: a click on the crumbs makes it and shows it
    HiddenByClick, // none yet: the click makes it but never shows it
};

class OpenDialog {
public:
    explicit OpenDialog(const std::wstring& folder, AddressBox box = AddressBox::Hidden)
        : m_boxMode(box)
    {
        for (const wchar_t* name : {L"DUIViewWndClassName", L"WorkerW", L"ReBarWindow32", L"Address Band Root",
                 L"msctls_progress32", L"Breadcrumb Parent", L"ToolbarWindow32", L"ComboBoxEx32"}) {
            WNDCLASSW wc {};
            wc.lpfnWndProc = ::DefWindowProcW;
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.lpszClassName = name;
            ::RegisterClassW(&wc); // fails harmlessly once registered
        }
        m_dialog = make(L"#32770", nullptr);
        make(L"DUIViewWndClassName", m_dialog);
        m_bar = make(L"msctls_progress32",
            make(L"Address Band Root", make(L"ReBarWindow32", make(L"WorkerW", m_dialog))));
        m_crumbs = make(L"ToolbarWindow32", make(L"Breadcrumb Parent", m_bar));
        ::SetWindowTextW(m_crumbs, (L"Address: " + folder).c_str());
        if (m_crumbs) {
            ::SetPropW(m_crumbs, kSelf, this);
            m_crumbsProc = reinterpret_cast<WNDPROC>(
                ::SetWindowLongPtrW(m_crumbs, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&crumbsProc)));
        }
        if (box == AddressBox::Hidden)
            makeAddressBox(false);
        m_nameBox = editOf(make(L"ComboBox", make(L"ComboBoxEx32", m_dialog, 0, kFileNameCombo), CBS_DROPDOWN));
    }
    ~OpenDialog()
    {
        if (m_addressBox) {
            ::SetWindowLongPtrW(m_addressBox, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_boxProc));
            ::RemovePropW(m_addressBox, kSelf);
        }
        if (m_crumbs) {
            ::SetWindowLongPtrW(m_crumbs, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_crumbsProc));
            ::RemovePropW(m_crumbs, kSelf);
        }
        if (m_dialog)
            ::DestroyWindow(m_dialog);
    }
    OpenDialog(const OpenDialog&) = delete;
    OpenDialog& operator=(const OpenDialog&) = delete;

    bool valid() const
    {
        return m_dialog && m_crumbs && m_nameBox && (m_addressBox || m_boxMode != AddressBox::Hidden);
    }
    HWND handle() const { return m_dialog; }
    std::wstring address() const { return text(m_crumbs); } // "Address: <folder>"
    std::wstring fileName() const { return text(m_nameBox); }
    // What the address box held each time Enter was pressed in it.
    const std::vector<std::wstring>& entered() const { return m_entered; }
    int clicks() const { return m_clicks; } // on the crumbs

private:
    static constexpr const wchar_t* kSelf = L"WinShunFakeDialog";

    static HWND make(const wchar_t* type, HWND parent, DWORD style = 0, int id = 0, bool visible = true)
    {
        return ::CreateWindowExW(0, type, L"", (parent ? WS_CHILD : WS_POPUP) | (visible ? WS_VISIBLE : 0) | style,
            -20000, -20000, 300, 200, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            ::GetModuleHandleW(nullptr), nullptr);
    }
    static HWND editOf(HWND combo) { return combo ? ::FindWindowExW(combo, nullptr, L"Edit", nullptr) : nullptr; }
    static std::wstring text(HWND hwnd)
    {
        std::wstring value(static_cast<std::size_t>(::GetWindowTextLengthW(hwnd)) + 1, L'\0');
        value.resize(static_cast<std::size_t>(::GetWindowTextW(hwnd, value.data(), static_cast<int>(value.size()))));
        return value;
    }

    void makeAddressBox(bool shown)
    {
        m_addressCombo = make(L"ComboBoxEx32", m_bar, 0, 0, shown);
        m_addressBox = editOf(make(L"ComboBox", m_addressCombo, CBS_DROPDOWN));
        if (m_addressBox) {
            ::SetPropW(m_addressBox, kSelf, this);
            m_boxProc = reinterpret_cast<WNDPROC>(
                ::SetWindowLongPtrW(m_addressBox, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&addressBoxProc)));
        }
    }

    static LRESULT CALLBACK crumbsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto* self = static_cast<OpenDialog*>(::GetPropW(hwnd, kSelf));
        if (self && msg == WM_LBUTTONUP) {
            ++self->m_clicks;
            if (!self->m_addressBox)
                self->makeAddressBox(self->m_boxMode == AddressBox::ShownByClick);
            return 0;
        }
        return ::CallWindowProcW(self ? self->m_crumbsProc : ::DefWindowProcW, hwnd, msg, wParam, lParam);
    }

    static LRESULT CALLBACK addressBoxProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto* self = static_cast<OpenDialog*>(::GetPropW(hwnd, kSelf));
        if (self && (msg == WM_KEYDOWN || msg == WM_KEYUP) && wParam == VK_RETURN) {
            if (msg == WM_KEYDOWN) {
                const std::wstring typed = text(hwnd);
                self->m_entered.push_back(typed);
                std::wstring folder = typed;
                if (folder.size() > 3 && folder.ends_with(L'\\'))
                    folder.pop_back(); // the crumbs name "C:\", but "C:\a" for "C:\a\"
                ::SetWindowTextW(self->m_crumbs, (L"Address: " + folder).c_str());
                ::ShowWindow(self->m_addressCombo, SW_HIDE);
            }
            return 0;
        }
        return ::CallWindowProcW(self ? self->m_boxProc : ::DefWindowProcW, hwnd, msg, wParam, lParam);
    }

    AddressBox m_boxMode = AddressBox::Hidden;
    HWND m_dialog = nullptr;
    HWND m_bar = nullptr;
    HWND m_crumbs = nullptr;
    HWND m_addressCombo = nullptr;
    HWND m_addressBox = nullptr;
    HWND m_nameBox = nullptr;
    WNDPROC m_boxProc = nullptr;
    WNDPROC m_crumbsProc = nullptr;
    std::vector<std::wstring> m_entered;
    int m_clicks = 0;
};

} // namespace fakedialog
