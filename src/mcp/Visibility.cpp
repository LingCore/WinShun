#include "Visibility.h"

#include "Unc.h"
#include "WebDav.h"
#include "Win32Util.h"

#include <QDir>
#include <QElapsedTimer>

#include <aclapi.h>

#include <cstddef>

namespace ws::mcp {

namespace {

// The TokenUser of `token`; empty when it cannot be had.
std::vector<std::byte> userOf(HANDLE token)
{
    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> user(size);
    if (size == 0 || !::GetTokenInformation(token, TokenUser, user.data(), size, &size))
        return {};
    return user;
}

} // namespace

Visibility::Visibility(HANDLE token, std::vector<std::wstring> remoteRoots)
    : m_token(token ? token : defaultToken())
    , m_roots(std::move(remoteRoots))
{
}

Visibility::Place Visibility::placeOf(const QString& path, const std::vector<std::wstring>& remoteRoots)
{
    const QString p = QDir::toNativeSeparators(path);
    if (p.size() >= 2 && p[0].unicode() < 128 && p[0].isLetter() && p[1] == u':' && (p.size() == 2 || p[2] == u'\\'))
        return Place::Local;
    const std::wstring wide = p.toStdWString();
    if (!unc::isUnc(wide))
        return Place::Elsewhere; // "\\?\…", "\\.\…", "dir\file", "\file"
    const std::wstring canonical = unc::canonical(wide);
    if (canonical.empty())
        return Place::Elsewhere; // a server alone
    for (const std::wstring& root : remoteRoots) {
        if (unc::contains(root, canonical) || webdav::rootHolds(root, canonical))
            return Place::Remote;
    }
    return Place::Elsewhere;
}

bool Visibility::sameUser(HANDLE token)
{
    // Never destroyed: a call's own thread may still ask while the process ends.
    static const std::vector<std::byte>* const own = [] {
        auto* user = new std::vector<std::byte>;
        HANDLE process = nullptr;
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &process)) {
            *user = userOf(process);
            ::CloseHandle(process);
        }
        return user;
    }();
    if (!token || own->empty())
        return false;
    const std::vector<std::byte> viewer = userOf(token);
    return !viewer.empty()
        && ::EqualSid(reinterpret_cast<const TOKEN_USER*>(viewer.data())->User.Sid,
            reinterpret_cast<const TOKEN_USER*>(own->data())->User.Sid);
}

HANDLE Visibility::defaultToken()
{
    static const HANDLE token = [] {
        HANDLE process = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process))
            return static_cast<HANDLE>(nullptr);
        HANDLE result = nullptr;
        DWORD size = 0;
        TOKEN_ELEVATION_TYPE elevation = TokenElevationTypeDefault;
        if (::GetTokenInformation(process, TokenElevationType, &elevation, sizeof elevation, &size)
            && elevation == TokenElevationTypeFull) {
            // Elevated: the linked token is the same user without the
            // administrator group and privileges (identification level,
            // which AccessCheck is content with).
            TOKEN_LINKED_TOKEN linked {};
            if (::GetTokenInformation(process, TokenLinkedToken, &linked, sizeof linked, &size))
                result = linked.LinkedToken;
        }
        if (!result) {
            ::DuplicateTokenEx(process, TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE, nullptr, SecurityImpersonation,
                TokenImpersonation, &result);
        }
        ::CloseHandle(process);
        return result;
    }();
    return token;
}

QString Visibility::parentOf(const QString& path)
{
    QString p = QDir::toNativeSeparators(path);
    while (p.size() > 3 && p.endsWith(u'\\'))
        p.chop(1);
    if (p.size() <= 3) // "C:\", "C:"
        return p.size() == 2 ? p + u'\\' : p;
    const qsizetype at = p.lastIndexOf(u'\\');
    if (at < 0)
        return p;
    QString parent = p.left(at);
    if (parent.size() == 2 && parent[1] == u':')
        parent.append(u'\\');
    return parent;
}

bool Visibility::check(const QString& path, DWORD desired)
{
    if (!m_token)
        return false;
    switch (placeOf(path, m_roots)) {
    case Place::Local:
        break;
    case Place::Remote: // see the header: no server is asked
        if (!m_sameUser)
            m_sameUser = sameUser(m_token);
        return *m_sameUser;
    case Place::Elsewhere:
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    ++m_checks;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    // The label too: AccessCheck weighs the integrity policy when it is there.
    const DWORD error = ::GetNamedSecurityInfoW(win32::longPath(path.toStdWString()).c_str(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &descriptor);
    bool allowed = false;
    if (error == ERROR_SUCCESS) {
        GENERIC_MAPPING mapping {FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
        DWORD access = desired;
        ::MapGenericMask(&access, &mapping);
        PRIVILEGE_SET privileges {};
        DWORD privilegesSize = sizeof privileges;
        DWORD granted = 0;
        BOOL status = FALSE;
        allowed = ::AccessCheck(descriptor, m_token, access, &mapping, &privileges, &privilegesSize, &granted, &status) && status;
        ::LocalFree(descriptor);
    }
    // Else: not even its security descriptor can be read (a protected system
    // folder): not shown.
    m_us += timer.nsecsElapsed() / 1000;
    return allowed;
}

bool Visibility::canList(const QString& folder)
{
    const QString key = QDir::toNativeSeparators(folder).toCaseFolded();
    if (const auto it = m_folders.constFind(key); it != m_folders.cend())
        return it.value();
    const bool allowed = check(folder, FILE_LIST_DIRECTORY);
    m_folders.insert(key, allowed);
    return allowed;
}

bool Visibility::canRead(const QString& file)
{
    return check(file, FILE_READ_DATA);
}

bool Visibility::canReadAttributes(const QString& path)
{
    return check(path, FILE_READ_ATTRIBUTES);
}

bool Visibility::sees(const QString& path)
{
    // Not on a drive: as the place has it, a root of the index there too
    // (its parent is not under one).
    if (placeOf(path, m_roots) != Place::Local)
        return check(path, FILE_LIST_DIRECTORY);
    return canList(parentOf(path));
}

} // namespace ws::mcp
