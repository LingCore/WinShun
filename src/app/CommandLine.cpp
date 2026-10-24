#include "CommandLine.h"

#include "Win32Util.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::cli {

namespace {

constexpr const wchar_t* kRecordName = L"CliPathEntry";
const QString kExeName = u"ws.exe"_s;

// What a name without an extension is completed with when PATHEXT says
// nothing (the Windows default), and .ps1, which PowerShell runs from the
// PATH without PATHEXT naming it.
const QStringList kDefaultExtensions {u".COM"_s, u".EXE"_s, u".BAT"_s, u".CMD"_s, u".VBS"_s, u".VBE"_s, u".JS"_s,
    u".JSE"_s, u".WSF"_s, u".WSH"_s, u".MSC"_s};

QStringList extensionsOf(const QString& text)
{
    QStringList out;
    for (const QString& part : text.split(u';', Qt::SkipEmptyParts)) {
        const QString extension = part.trimmed();
        if (!extension.isEmpty() && !out.contains(extension, Qt::CaseInsensitive))
            out.push_back(extension);
    }
    return out;
}

QString expandVariables(const QString& text)
{
    return text.contains(u'%') ? QString::fromStdWString(win32::expandEnvironment(text.toStdWString())) : text;
}

} // namespace

QString binFolder()
{
    return QDir::toNativeSeparators(QCoreApplication::applicationDirPath()) + u"\\bin"_s;
}

QString wsExe(const QString& bin)
{
    const QString exe = bin + u'\\' + kExeName;
    return QFileInfo::exists(exe) ? exe : QString();
}

QString normalized(const QString& entry)
{
    QString text = entry.trimmed();
    if (text.size() >= 2 && text.startsWith(u'"') && text.endsWith(u'"'))
        text = text.mid(1, text.size() - 2).trimmed();
    if (text.isEmpty())
        return text;
    text = QDir::toNativeSeparators(QDir::cleanPath(expandVariables(text)));
    // The long form of a short name; a folder that is not there stays as written.
    std::wstring wide = text.toStdWString();
    std::wstring longForm(32768, L'\0');
    const DWORD n = ::GetLongPathNameW(wide.c_str(), longForm.data(), static_cast<DWORD>(longForm.size()));
    if (n > 0 && n < longForm.size())
        text = QString::fromWCharArray(longForm.c_str(), static_cast<qsizetype>(n));
    return text;
}

bool sameFolder(const QString& a, const QString& b)
{
    const QString left = normalized(a);
    return !left.isEmpty() && left.compare(normalized(b), Qt::CaseInsensitive) == 0;
}

bool hasEntry(const QString& path, const QString& folder)
{
    return std::ranges::any_of(path.split(u';'), [&](const QString& entry) { return sameFolder(entry, folder); });
}

QString withEntry(const QString& path, const QString& folder)
{
    if (hasEntry(path, folder))
        return path;
    const QString entry = QDir::toNativeSeparators(folder);
    if (path.isEmpty())
        return entry;
    return path.endsWith(u';') ? path + entry : path + u';' + entry;
}

QString withoutEntry(const QString& path, const QString& folder)
{
    QStringList kept;
    bool removed = false;
    for (const QString& entry : path.split(u';')) {
        if (sameFolder(entry, folder))
            removed = true;
        else
            kept.push_back(entry);
    }
    return removed ? kept.join(u';') : path;
}

RawValue readValue(HKEY root, const std::wstring& key, const wchar_t* name)
{
    RawValue out;
    constexpr DWORD kTypes = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
    DWORD type = 0;
    DWORD size = 0;
    if (::RegGetValueW(root, key.c_str(), name, kTypes, &type, nullptr, &size) != ERROR_SUCCESS)
        return out;
    std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
    size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (::RegGetValueW(root, key.c_str(), name, kTypes, &type, buffer.data(), &size) != ERROR_SUCCESS)
        return out;
    out.text = QString::fromWCharArray(buffer.c_str()); // up to the closing null
    out.type = type;
    out.present = true;
    return out;
}

DWORD writeValue(HKEY root, const std::wstring& key, const wchar_t* name, const QString& text, DWORD type)
{
    HKEY handle = nullptr;
    LSTATUS status = ::RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr);
    if (status != ERROR_SUCCESS)
        return static_cast<DWORD>(status);
    const std::wstring data = text.toStdWString();
    status = ::RegSetValueExW(handle, name, 0, type, reinterpret_cast<const BYTE*>(data.c_str()),
        static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(handle);
    return static_cast<DWORD>(status);
}

Sources readSources(const Locations& where)
{
    Sources sources;
    sources.systemPath = readValue(where.systemRoot, where.systemKey, L"Path").text;
    sources.userPath = readValue(where.userRoot, where.userKey, L"Path").text;
    // The user's own PATHEXT replaces the system's (only PATH is joined).
    QString extensions = readValue(where.userRoot, where.userKey, L"PATHEXT").text;
    if (extensions.isEmpty())
        extensions = readValue(where.systemRoot, where.systemKey, L"PATHEXT").text;
    sources.pathExt = extensionsOf(expandVariables(extensions));
    if (sources.pathExt.isEmpty())
        sources.pathExt = kDefaultExtensions;
    if (!sources.pathExt.contains(u".PS1"_s, Qt::CaseInsensitive))
        sources.pathExt.push_back(u".PS1"_s);
    sources.recorded = readValue(where.recordRoot, where.recordKey, kRecordName).text;
    return sources;
}

Status analyse(const QString& bin, const Sources& sources, const std::function<bool(const QString&)>& exists)
{
    Status status;
    status.bin = bin;
    if (exists(bin + u'\\' + kExeName))
        status.exe = bin + u'\\' + kExeName;
    status.removable = hasEntry(sources.userPath, bin) && sameFolder(sources.recorded, bin);

    // The order Windows searches in: the system list, then the user's, each
    // entry in turn, each name completed by every extension in turn. Up to
    // our own folder: what comes after it cannot come first. Without ours on
    // the PATH yet, any other ws is the one that wins over it.
    struct Entry {
        QString folder;
        Status::Where list;
    };
    std::vector<Entry> entries;
    for (const QString& entry : sources.systemPath.split(u';', Qt::SkipEmptyParts))
        entries.push_back({entry, Status::Where::System});
    for (const QString& entry : sources.userPath.split(u';', Qt::SkipEmptyParts))
        entries.push_back({entry, Status::Where::User});
    for (const Entry& entry : entries) {
        const QString folder = normalized(entry.folder);
        if (folder.isEmpty())
            continue;
        if (sameFolder(folder, bin)) {
            status.where = entry.list;
            break;
        }
        if (!status.shadow.isEmpty())
            continue; // the first one is the one to name; the rest only matter up to ours
        for (const QString& extension : sources.pathExt) {
            // Lower case as files are mostly named (PATHEXT is upper case): this is shown to the user.
            const QString candidate = folder + u"\\ws"_s + extension.toLower();
            if (exists(candidate)) {
                status.shadow = candidate;
                break;
            }
        }
    }
    return status;
}

Status probe(const Locations& where)
{
    return analyse(binFolder(), readSources(where), [](const QString& file) { return QFileInfo::exists(file); });
}

QVariantMap toVariant(const Status& status)
{
    const char* where = status.where == Status::Where::System ? "system" : status.where == Status::Where::User ? "user" : "";
    return {{u"bin"_s, status.bin}, {u"exe"_s, status.exe}, {u"exeFound"_s, !status.exe.isEmpty()},
        {u"onPath"_s, status.onPath()}, {u"where"_s, QString::fromLatin1(where)}, {u"removable"_s, status.removable},
        {u"shadow"_s, status.shadow}};
}

DWORD addToUserPath(const QString& bin, const Locations& where)
{
    const RawValue current = readValue(where.userRoot, where.userKey, L"Path");
    const QString updated = withEntry(current.text, bin);
    if (current.present && updated == current.text)
        return 0; // there already: someone else's entry, not ours to take out later
    const DWORD error = writeValue(where.userRoot, where.userKey, L"Path", updated, current.present ? current.type : REG_EXPAND_SZ);
    if (error == 0)
        writeValue(where.recordRoot, where.recordKey, kRecordName, QDir::toNativeSeparators(bin), REG_SZ);
    return error;
}

DWORD removeFromUserPath(const QString& bin, const Locations& where)
{
    if (!sameFolder(readValue(where.recordRoot, where.recordKey, kRecordName).text, bin))
        return ERROR_NOT_FOUND;
    const RawValue current = readValue(where.userRoot, where.userKey, L"Path");
    if (current.present) {
        const QString updated = withoutEntry(current.text, bin);
        if (updated != current.text) {
            const DWORD error = writeValue(where.userRoot, where.userKey, L"Path", updated, current.type);
            if (error != 0)
                return error;
        }
    }
    ::RegDeleteKeyValueW(where.recordRoot, where.recordKey.c_str(), kRecordName);
    return 0;
}

} // namespace ws::cli
