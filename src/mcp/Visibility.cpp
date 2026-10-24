#include "Visibility.h"

#include "Win32Util.h"
#include "WorkerPool.h"

#include <QDir>
#include <QElapsedTimer>
#include <QSet>

#include <string>
#include <vector>

namespace ws::mcp {

Visibility::Visibility(HANDLE token)
    : m_token(token ? token : defaultToken())
{
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

QString Visibility::keyOf(const QString& folder)
{
    return QDir::toNativeSeparators(folder).toCaseFolded();
}

bool Visibility::check(const QString& path, DWORD desired)
{
    if (!m_token)
        return false;
    m_checks.fetch_add(1, std::memory_order_relaxed);
    // The label too: AccessCheck weighs the integrity policy when it is there.
    constexpr SECURITY_INFORMATION kParts = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION
        | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION;
    // GetFileSecurityW reads what GetNamedSecurityInfoW read (a junction's
    // own descriptor, not its target's: "Application Data" stays one that
    // cannot be listed; the same decisions for 41 000 folders), in half the
    // time, and a quarter of it with 16 threads at once (that one hardly
    // gains from more threads: 12 µs a check, 3 this way).
    const std::wstring name = win32::longPath(path.toStdWString());
    alignas(8) unsigned char onStack[1024];
    std::vector<unsigned char> onHeap;
    PSECURITY_DESCRIPTOR descriptor = onStack;
    DWORD needed = 0;
    BOOL read = ::GetFileSecurityW(name.c_str(), kParts, descriptor, sizeof onStack, &needed);
    if (!read && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        onHeap.resize(needed);
        descriptor = onHeap.data();
        read = ::GetFileSecurityW(name.c_str(), kParts, descriptor, needed, &needed);
    }
    bool allowed = false;
    if (read) {
        GENERIC_MAPPING mapping {FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
        DWORD access = desired;
        ::MapGenericMask(&access, &mapping);
        PRIVILEGE_SET privileges {};
        DWORD privilegesSize = sizeof privileges;
        DWORD granted = 0;
        BOOL status = FALSE;
        allowed = ::AccessCheck(descriptor, m_token, access, &mapping, &privileges, &privilegesSize, &granted, &status)
            && status;
    }
    // Else: not even its security descriptor can be read (a protected system
    // folder): not shown.
    return allowed;
}

bool Visibility::timedCheck(const QString& path, DWORD desired)
{
    QElapsedTimer timer;
    timer.start();
    const bool allowed = check(path, desired);
    m_us += timer.nsecsElapsed() / 1000;
    return allowed;
}

bool Visibility::canList(const QString& folder)
{
    const QString key = keyOf(folder);
    if (const auto it = m_folders.constFind(key); it != m_folders.cend())
        return it.value();
    const bool allowed = timedCheck(folder, FILE_LIST_DIRECTORY);
    m_folders.insert(key, allowed);
    return allowed;
}

void Visibility::checkAhead(
    const QStringList& folders, WorkerPool& pool, const std::stop_token& stop, qsizetype minimum)
{
    QStringList todo;
    std::vector<QString> keys;
    QSet<QString> asked; // of these, each once
    for (const QString& folder : folders) {
        QString key = keyOf(folder);
        if (m_folders.contains(key) || asked.contains(key))
            continue;
        asked.insert(key);
        todo.push_back(folder);
        keys.push_back(std::move(key));
    }
    if (todo.isEmpty() || todo.size() < minimum)
        return;
    QElapsedTimer timer;
    timer.start();
    enum : char { Unknown, Allowed, Denied };
    std::vector<char> found(keys.size(), Unknown);
    pool.parallelFor(found.size(), [&](std::size_t i) {
        if (!stop.stop_requested())
            found[i] = check(todo[static_cast<qsizetype>(i)], FILE_LIST_DIRECTORY) ? Allowed : Denied;
    });
    m_us += timer.nsecsElapsed() / 1000;
    for (std::size_t i = 0; i < found.size(); ++i) {
        if (found[i] != Unknown)
            m_folders.insert(keys[i], found[i] == Allowed);
    }
}

bool Visibility::canRead(const QString& file)
{
    return timedCheck(file, FILE_READ_DATA);
}

bool Visibility::canReadAttributes(const QString& path)
{
    return timedCheck(path, FILE_READ_ATTRIBUTES);
}

bool Visibility::sees(const QString& path)
{
    return canList(parentOf(path));
}

} // namespace ws::mcp
