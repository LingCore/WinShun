#pragma once

#include <windows.h>

#include <string>
#include <vector>

// A protocol fixture, not AutoCAD: native combo/edit controls, deliberately
// nonstandard IDs and no SHELLDLL_DefView. Like MFC, its file-name model is
// updated by CBN_EDITCHANGE, not by reading the edit when OK is pressed.
namespace fakedialog {

class CustomFileDialog {
public:
    CustomFileDialog()
    {
        WNDCLASSW wc {};
        wc.lpfnWndProc = ::DefWindowProcW;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SysListView32";
        ::RegisterClassW(&wc);
        m_window = make(L"#32770", nullptr, 0, 0, 0, 0, 620, 430);
        m_list = make(L"SysListView32", m_window, 0, 0x220, 20, 20, 400, 220);
        m_name = make(L"ComboBox", m_window, CBS_DROPDOWN, kNameId, 20, 260, 400, 200);
        m_edit = ::FindWindowExW(m_name, nullptr, L"Edit", nullptr);
        m_types = make(L"ComboBox", m_window, CBS_DROPDOWNLIST, 0x633, 20, 310, 400, 200);
        ::SendMessageW(m_types, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Drawing (*.dwg)"));
        ::SendMessageW(m_types, CB_SETCURSEL, 0, 0);
        make(L"Button", m_window, 0, IDOK, 450, 260, 80, 30);
        m_cancel = make(L"Button", m_window, 0, IDCANCEL, 450, 310, 80, 30);
        ::SetWindowTextW(m_edit, L"keep-name.dwg");
        ::SetWindowLongPtrW(m_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        m_previous = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(m_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&proc)));
    }
    ~CustomFileDialog() { ::DestroyWindow(m_window); }
    CustomFileDialog(const CustomFileDialog&) = delete;
    CustomFileDialog& operator=(const CustomFileDialog&) = delete;

    HWND handle() const { return m_window; }
    HWND edit() const { return m_edit; }
    bool valid() const { return m_window && m_list && m_name && m_edit && m_types && m_cancel; }
    std::wstring fileName() const { return text(m_edit); }
    const std::vector<std::wstring>& entered() const { return m_entered; }
    bool accepted() const { return m_accepted; }
    int notifications() const { return m_notifications; }
    void withoutType() { ::DestroyWindow(m_types); }
    void withoutList() { ::DestroyWindow(m_list); }
    void withoutCancel() { ::DestroyWindow(m_cancel); }
    void ambiguousName() { make(L"ComboBox", m_window, CBS_DROPDOWN, 0x885, 20, 280, 400, 200); }

private:
    static constexpr int kNameId = 0x553;
    static HWND make(const wchar_t* type, HWND parent, DWORD style, int id, int x, int y, int width, int height)
    {
        return ::CreateWindowExW(0, type, L"", (parent ? WS_CHILD : WS_POPUP) | WS_VISIBLE | style,
            parent ? x : -20000, parent ? y : -20000, width, height, parent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ::GetModuleHandleW(nullptr), nullptr);
    }
    static std::wstring text(HWND window)
    {
        std::wstring value(static_cast<std::size_t>(::GetWindowTextLengthW(window)) + 1, L'\0');
        value.resize(static_cast<std::size_t>(::GetWindowTextW(window, value.data(), static_cast<int>(value.size()))));
        return value;
    }
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<CustomFileDialog*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_COMMAND && LOWORD(wParam) == kNameId && HIWORD(wParam) == CBN_EDITCHANGE) {
            self->m_boundName = text(self->m_edit);
            ++self->m_notifications;
            return 0;
        }
        if (message == WM_COMMAND && LOWORD(wParam) == IDOK) {
            const DWORD attributes = ::GetFileAttributesW(self->m_boundName.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                self->m_entered.push_back(self->m_boundName);
                self->m_boundName.clear();
                ::SetWindowTextW(self->m_edit, L"");
            } else {
                self->m_accepted = true;
            }
            return 0;
        }
        return ::CallWindowProcW(self->m_previous, window, message, wParam, lParam);
    }

    HWND m_window = nullptr;
    HWND m_list = nullptr;
    HWND m_name = nullptr;
    HWND m_edit = nullptr;
    HWND m_types = nullptr;
    HWND m_cancel = nullptr;
    WNDPROC m_previous = nullptr;
    std::wstring m_boundName = L"keep-name.dwg";
    std::vector<std::wstring> m_entered;
    int m_notifications = 0;
    bool m_accepted = false;
};

} // namespace fakedialog
