#include "ScoopInstall.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <windows.h>

using namespace Qt::StringLiterals;

namespace ws::scoop {

namespace {

QString clean(const QString& path)
{
    return QDir::toNativeSeparators(QDir::cleanPath(QDir::fromNativeSeparators(path)));
}

// The records Scoop leaves in a version folder: scoop-manifest.json and
// scoop-install.json, or manifest.json and install.json before Scoop 0.5.
bool hasRecords(const QString& folder)
{
    const auto has = [&](const QString& name) { return QFile::exists(folder + u'\\' + name); };
    return (has(u"scoop-manifest.json"_s) && has(u"scoop-install.json"_s))
        || (has(u"manifest.json"_s) && has(u"install.json"_s));
}

// The folder a (junction or plain) folder really is, lower case; empty when
// it is not there.
QString realFolder(const QString& folder)
{
    const HANDLE handle = ::CreateFileW(reinterpret_cast<const wchar_t*>(folder.utf16()), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return {};
    wchar_t buffer[MAX_PATH * 4] = {};
    const DWORD n = ::GetFinalPathNameByHandleW(handle, buffer, static_cast<DWORD>(std::size(buffer)), FILE_NAME_NORMALIZED);
    ::CloseHandle(handle);
    if (n == 0 || n >= std::size(buffer))
        return {};
    QString real = QString::fromWCharArray(buffer, static_cast<qsizetype>(n));
    if (real.startsWith(u"\\\\?\\UNC\\"_s))
        real = u"\\\\"_s + real.mid(8);
    else if (real.startsWith(u"\\\\?\\"_s))
        real = real.mid(4);
    return real.toLower();
}

// Where `parts` run through apps\<name>\<x>: the index of "apps" (never the first part).
std::optional<int> appsIndex(const QStringList& parts)
{
    for (int i = 1; i + 2 < parts.size(); ++i) {
        if (parts[i].compare(u"apps"_s, Qt::CaseInsensitive) == 0)
            return i;
    }
    return std::nullopt;
}

} // namespace

QString Install::appDir() const
{
    return root + u"\\apps\\"_s + name;
}

QString Install::current() const
{
    return appDir() + u"\\current"_s;
}

QString defaultGlobalRoot()
{
    const QString env = qEnvironmentVariable("SCOOP_GLOBAL");
    if (!env.isEmpty())
        return clean(env);
    return clean(qEnvironmentVariable("ProgramData", u"C:\\ProgramData"_s) + u"\\scoop"_s);
}

std::optional<Install> detect(const QString& folder, const QString& globalRoot)
{
    const QString path = clean(folder);
    const QStringList parts = path.split(u'\\', Qt::SkipEmptyParts);
    const std::optional<int> at = appsIndex(parts);
    if (!at || *at + 3 != parts.size())
        return std::nullopt; // the program's own folder is apps\<name>\<version or current>
    if (!hasRecords(path))
        return std::nullopt;
    Install install;
    install.name = parts[*at + 1];
    install.root = parts.mid(0, *at).join(u'\\');
    const QString version = parts[*at + 2];
    if (version.compare(u"current"_s, Qt::CaseInsensitive) != 0) {
        install.version = version;
    } else {
        const QString real = realFolder(path);
        install.version = real.isEmpty() ? QString() : QFileInfo(real).fileName();
    }
    install.global = !globalRoot.isEmpty() && install.root.compare(clean(globalRoot), Qt::CaseInsensitive) == 0;
    return install;
}

QString stablePath(const QString& path)
{
    if (path.isEmpty())
        return path;
    const QString text = clean(path);
    const QStringList parts = text.split(u'\\', Qt::SkipEmptyParts);
    const std::optional<int> at = appsIndex(parts);
    if (!at)
        return text;
    const QString version = parts[*at + 2];
    if (version.compare(u"current"_s, Qt::CaseInsensitive) == 0)
        return text;
    const QString appDir = parts.mid(0, *at + 2).join(u'\\');
    const QString versionDir = appDir + u'\\' + version;
    const QString current = appDir + u"\\current"_s;
    if (!hasRecords(versionDir))
        return text;
    const QString target = realFolder(current);
    if (target.isEmpty() || target != realFolder(versionDir))
        return text;
    const QStringList rest = parts.mid(*at + 3);
    return rest.isEmpty() ? current : current + u'\\' + rest.join(u'\\');
}

bool sameCopy(const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty())
        return false;
    return stablePath(a).compare(stablePath(b), Qt::CaseInsensitive) == 0;
}

QString shimsFolder(const QString& path)
{
    const QStringList parts = clean(path).split(u'\\', Qt::SkipEmptyParts);
    const std::optional<int> at = appsIndex(parts);
    return at ? parts.mid(0, *at).join(u'\\') + u"\\shims"_s : QString();
}

QString updateCommand(const Install& install)
{
    return u"scoop update "_s + install.name + (install.global ? u" -g"_s : QString());
}

} // namespace ws::scoop
