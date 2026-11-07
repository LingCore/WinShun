#pragma once

// Replace only this test executable's imported known-folder lookup. Windows'
// real folder settings and other processes remain outside the test fixture.
#include <QDir>
#include <QFileInfo>
#include <QString>

#include <windows.h>
#include <shlobj.h>

#ifdef small
#undef small
#endif

#include <cstring>
#include <vector>

extern "C" decltype(&::SHGetKnownFolderPath) __imp_SHGetKnownFolderPath;

class KnownFolderOverride {
public:
    struct Folder {
        const KNOWNFOLDERID* id;
        QString path; // empty simulates a failed lookup
    };

    explicit KnownFolderOverride(std::vector<Folder> values)
        : folders(std::move(values))
        , m_original(__imp_SHGetKnownFolderPath)
    {
        Q_ASSERT(!s_active);
        s_active = this;
        m_installed = replace(lookup);
    }
    ~KnownFolderOverride()
    {
        if (m_installed)
            replace(m_original);
        s_active = nullptr;
    }
    KnownFolderOverride(const KnownFolderOverride&) = delete;
    KnownFolderOverride& operator=(const KnownFolderOverride&) = delete;

    bool installed() const { return m_installed; }
    std::vector<Folder> folders;

private:
    using Function = decltype(&::SHGetKnownFolderPath);
    static bool replace(Function function)
    {
        DWORD protection = 0;
        if (!::VirtualProtect(&__imp_SHGetKnownFolderPath, sizeof(Function), PAGE_READWRITE, &protection))
            return false;
        __imp_SHGetKnownFolderPath = function;
        DWORD ignored = 0;
        ::VirtualProtect(&__imp_SHGetKnownFolderPath, sizeof(Function), protection, &ignored);
        return true;
    }
    static HRESULT WINAPI lookup(REFKNOWNFOLDERID id, DWORD flags, HANDLE token, PWSTR* path)
    {
        for (const Folder& folder : s_active->folders) {
            if (!::IsEqualGUID(id, *folder.id))
                continue;
            *path = nullptr;
            if (folder.path.isEmpty())
                return E_FAIL;
            if (flags & KF_FLAG_CREATE) {
                if (!QDir().mkpath(folder.path))
                    return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
            } else if (!(flags & KF_FLAG_DONT_VERIFY) && !QFileInfo(folder.path).isDir()) {
                return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
            }
            const QString native = QDir::toNativeSeparators(folder.path);
            const auto bytes = static_cast<std::size_t>(native.size() + 1) * sizeof(wchar_t);
            *path = static_cast<PWSTR>(::CoTaskMemAlloc(bytes));
            if (!*path)
                return E_OUTOFMEMORY;
            std::memcpy(*path, native.utf16(), bytes);
            return S_OK;
        }
        return s_active->m_original(id, flags, token, path);
    }

    inline static KnownFolderOverride* s_active = nullptr;
    Function m_original;
    bool m_installed = false;
};
