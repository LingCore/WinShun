#pragma once

// Stand-ins for the windows ws::pathbox knows (PathBoxDialog.h), made of real
// controls and behaving as the programs were measured or as their sources
// say. None contains anything of Bandizip, 7-Zip or WinRAR.
//
//  - TreeBoxDialog: Bandizip's "选择解压路径" (7.36, measured 2026-11-13): a
//    combo box with the path (ID 1021), a folder tree (1024), OK and Cancel.
//    Its own idea of the folder (where the tree is) follows the box only on
//    CBN_EDITCHANGE; OK answers with the box's text.
//  - SevenZipDialog: 7-Zip's "Extract" by ExtractDialogRes.h. OK answers with
//    the list's selected entry if there is one, else with the text
//    (ExtractDialog.cpp, OnOK).
//  - WinRarDialog: "Extraction path and options" as Listary's plugin finds
//    it: tabs (0x3020), the first page with the path box (0x65) and the
//    "Display" button (0x66), which takes the tree to the box's folder.

#include <windows.h>
#include <commctrl.h>

#include <string>
#include <vector>

namespace fakepathbox {

inline HWND make(const wchar_t* type, HWND parent, DWORD style, int id, const wchar_t* text = L"")
{
    static const bool controls = [] {
        const INITCOMMONCONTROLSEX classes {sizeof(INITCOMMONCONTROLSEX), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES};
        return ::InitCommonControlsEx(&classes) != FALSE;
    }();
    (void)controls;
    return ::CreateWindowExW(0, type, text, (parent ? WS_CHILD : WS_POPUP) | WS_VISIBLE | style, parent ? 10 : -20000,
        parent ? 10 : -20000, 400, 200, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        ::GetModuleHandleW(nullptr), nullptr);
}

inline std::wstring textOf(HWND window)
{
    wchar_t text[1100] {};
    ::GetWindowTextW(window, text, static_cast<int>(std::size(text)));
    return text;
}

// A window whose WM_COMMAND goes to `onCommand` of the class deriving from it.
class Host {
public:
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    HWND handle() const { return m_window; }

protected:
    explicit Host(HWND parent = nullptr)
        : m_window(make(L"#32770", parent, 0, 0))
    {
        ::SetWindowLongPtrW(m_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        m_previous = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(m_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&proc)));
    }
    virtual ~Host()
    {
        if (::IsWindow(m_window)) {
            ::SetWindowLongPtrW(m_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_previous));
            ::DestroyWindow(m_window);
        }
    }
    virtual void onCommand(int id, int code) = 0;

private:
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<Host*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_COMMAND) {
            self->onCommand(LOWORD(wParam), HIWORD(wParam));
            return 0;
        }
        return ::CallWindowProcW(self->m_previous, window, message, wParam, lParam);
    }

    HWND m_window = nullptr;
    WNDPROC m_previous = nullptr;
};

class TreeBoxDialog : public Host {
public:
    static constexpr int kPath = 1021;
    static constexpr int kTree = 1024;

    // `plainEdit`: the path in an edit field of its own, not in a combo box.
    explicit TreeBoxDialog(const wchar_t* path = L"C:\\Users", bool plainEdit = false)
    {
        if (plainEdit) {
            m_edit = make(L"Edit", handle(), ES_AUTOHSCROLL, kPath, path);
        } else {
            m_combo = make(L"ComboBox", handle(), CBS_DROPDOWN | CBS_AUTOHSCROLL, kPath);
            for (const wchar_t* earlier : {L"C:\\earlier one", L"D:\\earlier two"})
                ::SendMessageW(m_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(earlier));
            m_edit = ::FindWindowExW(m_combo, nullptr, L"Edit", nullptr);
            ::SetWindowTextW(m_edit, path);
        }
        m_tree = make(WC_TREEVIEWW, handle(), TVS_HASLINES, kTree);
        m_ok = make(L"Button", handle(), BS_DEFPUSHBUTTON, IDOK, L"OK");
        m_cancel = make(L"Button", handle(), 0, IDCANCEL, L"Cancel");
        m_shown = path;
    }

    HWND combo() const { return m_combo; }
    HWND edit() const { return m_edit; }
    HWND tree() const { return m_tree; }
    HWND ok() const { return m_ok; }
    HWND cancel() const { return m_cancel; }
    bool valid() const { return handle() && m_edit && m_tree && m_ok && m_cancel; }
    std::wstring path() const { return textOf(m_edit); }
    const std::wstring& shown() const { return m_shown; } // where its tree is
    int edits() const { return m_edits; }
    const std::vector<std::wstring>& answers() const { return m_answers; }

private:
    void onCommand(int id, int code) override
    {
        if (id == kPath && code == (m_combo ? CBN_EDITCHANGE : EN_CHANGE)) {
            ++m_edits;
            m_shown = path();
        } else if (id == IDOK && code == BN_CLICKED) {
            m_answers.push_back(path());
        }
    }

    HWND m_combo = nullptr;
    HWND m_edit = nullptr;
    HWND m_tree = nullptr;
    HWND m_ok = nullptr;
    HWND m_cancel = nullptr;
    std::wstring m_shown;
    int m_edits = 0;
    std::vector<std::wstring> m_answers;
};

class SevenZipDialog : public Host {
public:
    SevenZipDialog()
    {
        m_path = make(L"ComboBox", handle(), CBS_DROPDOWN | CBS_AUTOHSCROLL, 100);
        for (const wchar_t* earlier : {L"C:\\extracted before\\", L"D:\\another\\"})
            ::SendMessageW(m_path, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(earlier));
        ::SendMessageW(m_path, CB_SETCURSEL, 0, 0); // as 7-Zip opens: the folder used last, selected
        make(L"Button", handle(), 0, 101, L"...");
        make(L"ComboBox", handle(), CBS_DROPDOWNLIST, 102);
        make(L"ComboBox", handle(), CBS_DROPDOWNLIST, 103);
        make(L"Edit", handle(), ES_PASSWORD | ES_AUTOHSCROLL, 120);
        make(L"Edit", handle(), ES_AUTOHSCROLL, 130);
        make(L"Button", handle(), BS_DEFPUSHBUTTON, IDOK, L"OK");
        make(L"Button", handle(), 0, IDCANCEL, L"Cancel");
    }

    HWND path() const { return m_path; }
    const std::vector<std::wstring>& answers() const { return m_answers; }
    // What OK would answer with now.
    std::wstring answer() const
    {
        const LRESULT chosen = ::SendMessageW(m_path, CB_GETCURSEL, 0, 0);
        if (chosen == CB_ERR)
            return textOf(m_path);
        wchar_t entry[1100] {};
        ::SendMessageW(m_path, CB_GETLBTEXT, static_cast<WPARAM>(chosen), reinterpret_cast<LPARAM>(entry));
        return entry;
    }

private:
    void onCommand(int id, int code) override
    {
        if (id == IDOK && code == BN_CLICKED)
            m_answers.push_back(answer());
    }

    HWND m_path = nullptr;
    std::vector<std::wstring> m_answers;
};

class WinRarDialog : public Host {
public:
    WinRarDialog()
        : m_general(handle())
    {
        make(WC_TABCONTROLW, handle(), 0, 0x3020);
        make(L"Button", handle(), BS_DEFPUSHBUTTON, IDOK, L"OK");
        make(L"Button", handle(), 0, IDCANCEL, L"Cancel");
    }

    HWND page() const { return m_general.handle(); }
    HWND path() const { return m_general.path; }
    std::wstring text() const { return textOf(m_general.path); }
    const std::vector<std::wstring>& displayed() const { return m_general.displayed; } // the folders "Display" was pressed for
    int answers() const { return m_answers; }

private:
    struct General : Host {
        explicit General(HWND dialog)
            : Host(dialog)
        {
            path = make(L"ComboBox", handle(), CBS_DROPDOWN | CBS_AUTOHSCROLL, 0x65);
            ::SetWindowTextW(::FindWindowExW(path, nullptr, L"Edit", nullptr), L"C:\\Users\\someone\\Desktop\\archive");
            make(L"Button", handle(), 0, 0x66, L"Display");
            make(WC_TREEVIEWW, handle(), TVS_HASLINES, 0x67);
        }
        void onCommand(int id, int code) override
        {
            if (id == 0x66 && code == BN_CLICKED)
                displayed.push_back(textOf(path));
        }
        HWND path = nullptr;
        std::vector<std::wstring> displayed;
    };

    void onCommand(int id, int code) override
    {
        if (id == IDOK && code == BN_CLICKED)
            ++m_answers;
    }

    General m_general;
    int m_answers = 0;
};

} // namespace fakepathbox
