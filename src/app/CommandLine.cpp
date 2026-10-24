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

// How often a PATH that changes under our hands is read again before giving up.
constexpr int kAttempts = 4;

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

// The note, put back as it was when the PATH could not be written after it.
void restoreNote(const Locations& where, const RawValue& before)
{
    if (before.present)
        writeValue(where.recordRoot, where.recordKey, kRecordName, before.text, REG_SZ);
    else
        deleteValue(where.recordRoot, where.recordKey, kRecordName);
}

// The note gone; the key it lived in too, when nothing else is in it.
void dropNote(const Locations& where)
{
    deleteValue(where.recordRoot, where.recordKey, kRecordName);
    deleteKeyIfEmpty(where.recordRoot, where.recordKey);
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

QStringList splitPath(const QString& path)
{
    QStringList entries;
    qsizetype start = 0;
    bool quoted = false;
    for (qsizetype i = 0; i < path.size(); ++i) {
        if (path[i] == u'"') {
            quoted = !quoted;
        } else if (path[i] == u';' && !quoted) {
            entries.push_back(path.mid(start, i - start));
            start = i + 1;
        }
    }
    entries.push_back(path.mid(start));
    return entries;
}

QString entryOf(const QString& folder)
{
    const QString native = QDir::toNativeSeparators(folder);
    return native.contains(u';') ? u'"' + native + u'"' : native;
}

QString normalized(const QString& entry)
{
    QString text = entry.trimmed();
    if (text.size() >= 2 && text.startsWith(u'"') && text.endsWith(u'"'))
        text = text.mid(1, text.size() - 2).trimmed();
    if (text.isEmpty())
        return text;
    text = QDir::toNativeSeparators(QDir::cleanPath(expandVariables(text)));
    // The long form of a short name (PROGRA~1); a folder that is not there stays
    // as written. Only a name that has a '~' can be one: asking about the others
    // reaches out to the disk, and a PATH entry may be a share that is not there.
    if (text.contains(u'~')) {
        std::wstring wide = text.toStdWString();
        std::wstring longForm(32768, L'\0');
        const DWORD n = ::GetLongPathNameW(wide.c_str(), longForm.data(), static_cast<DWORD>(longForm.size()));
        if (n > 0 && n < longForm.size())
            text = QString::fromWCharArray(longForm.c_str(), static_cast<qsizetype>(n));
    }
    return text;
}

bool sameFolder(const QString& a, const QString& b)
{
    const QString left = normalized(a);
    return !left.isEmpty() && left.compare(normalized(b), Qt::CaseInsensitive) == 0;
}

bool hasEntry(const QString& path, const QString& folder)
{
    const QStringList entries = splitPath(path);
    return std::ranges::any_of(entries, [&](const QString& entry) { return sameFolder(entry, folder); });
}

QString withEntry(const QString& path, const QString& folder)
{
    if (hasEntry(path, folder))
        return path;
    const QString entry = entryOf(folder);
    if (path.isEmpty())
        return entry;
    return path.endsWith(u';') ? path + entry : path + u';' + entry;
}

bool hasExactEntry(const QString& path, const QString& entry)
{
    return !entry.isEmpty() && splitPath(path).contains(entry);
}

QString withoutExactEntry(const QString& path, const QString& entry)
{
    if (entry.isEmpty())
        return path;
    QStringList entries = splitPath(path);
    const qsizetype at = entries.lastIndexOf(entry);
    if (at < 0)
        return path;
    entries.removeAt(at);
    return entries.join(u';');
}

RawValue readValue(HKEY root, const std::wstring& key, const wchar_t* name, const std::function<void()>& betweenReads)
{
    RawValue out;
    constexpr DWORD kTypes = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
    for (int attempt = 0; attempt < 8; ++attempt) {
        DWORD type = 0;
        DWORD size = 0;
        LSTATUS status = ::RegGetValueW(root, key.c_str(), name, kTypes, &type, nullptr, &size);
        if (status == ERROR_FILE_NOT_FOUND)
            return out; // the only way to be absent
        if (status != ERROR_SUCCESS) {
            out.error = static_cast<DWORD>(status); // another type (ERROR_UNSUPPORTED_TYPE), no access…
            return out;
        }
        if (betweenReads)
            betweenReads();
        std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
        size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
        status = ::RegGetValueW(root, key.c_str(), name, kTypes, &type, buffer.data(), &size);
        if (status == ERROR_MORE_DATA)
            continue; // it grew between the two calls: from the top
        if (status == ERROR_FILE_NOT_FOUND)
            return out; // gone between them
        if (status != ERROR_SUCCESS) {
            out.error = static_cast<DWORD>(status);
            return out;
        }
        out.text = QString::fromWCharArray(buffer.c_str()); // up to the closing null
        out.type = type;
        out.present = true;
        return out;
    }
    out.error = ERROR_MORE_DATA; // it never stopped growing
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

bool deleteValue(HKEY root, const std::wstring& key, const wchar_t* name)
{
    const LSTATUS status = ::RegDeleteKeyValueW(root, key.c_str(), name);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

bool deleteKeyIfEmpty(HKEY root, const std::wstring& key)
{
    HKEY handle = nullptr;
    if (::RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE, &handle) != ERROR_SUCCESS)
        return false;
    DWORD subKeys = 0;
    DWORD values = 0;
    const bool empty = ::RegQueryInfoKeyW(handle, nullptr, nullptr, nullptr, &subKeys, nullptr, nullptr, &values, nullptr, nullptr,
                           nullptr, nullptr) == ERROR_SUCCESS
        && subKeys == 0 && values == 0;
    ::RegCloseKey(handle);
    return empty && ::RegDeleteKeyW(root, key.c_str()) == ERROR_SUCCESS;
}

Sources readSources(const Locations& where)
{
    Sources sources;
    const RawValue system = readValue(where.systemRoot, where.systemKey, L"Path");
    const RawValue user = readValue(where.userRoot, where.userKey, L"Path");
    sources.systemPath = system.text;
    sources.userPath = user.text;
    sources.pathError = system.error ? system.error : user.error;
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
    status.pathError = sources.pathError;
    if (exists(bin + u'\\' + kExeName))
        status.exe = bin + u'\\' + kExeName;
    // Ours to take out: the entry WinShun wrote is on the user's list as it wrote it.
    status.removable = hasExactEntry(sources.userPath, sources.recorded) && sameFolder(sources.recorded, bin);

    // The order Windows searches in: the system list, then the user's, each
    // entry in turn, each name completed by every extension in turn. Up to
    // our own folder: what comes after it cannot come first. Without ours on
    // the PATH yet, any other ws is the one that wins over it.
    struct Entry {
        QString folder;
        Status::Where list;
    };
    std::vector<Entry> entries;
    for (const QString& entry : splitPath(sources.systemPath)) {
        if (!entry.isEmpty())
            entries.push_back({entry, Status::Where::System});
    }
    for (const QString& entry : splitPath(sources.userPath)) {
        if (!entry.isEmpty())
            entries.push_back({entry, Status::Where::User});
    }
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
        {u"shadow"_s, status.shadow}, {u"pathError"_s, static_cast<uint>(status.pathError)}};
}

DWORD addToUserPath(const QString& bin, const Locations& where)
{
    // On the system's list already (an all-users install put it there): a second
    // entry on the user's would be noted as WinShun's and left behind by an uninstall.
    // A system list that cannot be read changes nothing here: the worst is a duplicate.
    const RawValue system = readValue(where.systemRoot, where.systemKey, L"Path");
    if (system.present && hasEntry(system.text, bin))
        return 0;

    const QString entry = entryOf(bin);
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        const RawValue current = readValue(where.userRoot, where.userKey, L"Path");
        if (current.error != 0)
            return current.error; // not an empty PATH: left as it is
        if (current.present && hasEntry(current.text, bin))
            return 0; // there already: someone else's entry, or ours from before
        const RawValue noted = readValue(where.recordRoot, where.recordKey, kRecordName);

        // A copy of WinShun that was somewhere else leaves its entry on the PATH
        // (the note holds one): it moves here, so that it can be taken out.
        QString updated = current.text;
        if (noted.present && !noted.text.isEmpty() && !sameFolder(noted.text, bin))
            updated = withoutExactEntry(updated, noted.text);
        updated = updated.isEmpty() ? entry : updated.endsWith(u';') ? updated + entry : updated + u';' + entry;

        // The note first: if it cannot be kept, the PATH stays as it is, and a
        // failure after it puts it back. (The other way round, an entry written
        // and not noted could never be taken out.)
        DWORD error = writeValue(where.recordRoot, where.recordKey, kRecordName, entry, REG_SZ);
        if (error != 0)
            return error;
        // Right before the write: the work above (short names, a share) takes
        // time, and a program that changed the PATH meanwhile is not ours to overwrite.
        const RawValue again = readValue(where.userRoot, where.userKey, L"Path");
        if (again.error != 0) {
            restoreNote(where, noted);
            return again.error;
        }
        if (again.present != current.present || again.text != current.text || again.type != current.type) {
            restoreNote(where, noted);
            continue;
        }
        error = writeValue(where.userRoot, where.userKey, L"Path", updated, current.present ? current.type : REG_EXPAND_SZ);
        if (error != 0) {
            restoreNote(where, noted);
            return error;
        }
        return 0;
    }
    return ERROR_BUSY; // the PATH kept changing
}

DWORD removeFromUserPath(const QString& bin, const Locations& where)
{
    const RawValue noted = readValue(where.recordRoot, where.recordKey, kRecordName);
    if (!noted.present || !sameFolder(noted.text, bin))
        return ERROR_NOT_FOUND;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        const RawValue current = readValue(where.userRoot, where.userKey, L"Path");
        if (current.error != 0)
            return current.error; // unreadable: the PATH and the note stay
        const QString updated = withoutExactEntry(current.text, noted.text);
        if (!current.present || updated == current.text) {
            // Not there as we wrote it: gone already, or the user's own spelling of the folder.
            const bool otherSpelling = current.present && hasEntry(current.text, bin);
            dropNote(where);
            return otherSpelling ? ERROR_NOT_FOUND : 0;
        }
        const RawValue again = readValue(where.userRoot, where.userKey, L"Path");
        if (again.error != 0)
            return again.error;
        if (again.present != current.present || again.text != current.text || again.type != current.type)
            continue; // changed meanwhile: look again
        const DWORD error = writeValue(where.userRoot, where.userKey, L"Path", updated, current.type);
        if (error != 0)
            return error;
        dropNote(where);
        return 0;
    }
    return ERROR_BUSY;
}

} // namespace ws::cli
