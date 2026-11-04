#include "Elevation.h"

#include "Install.h"
#include "Manifest.h"
#include "PipeClient.h"
#include "ServiceLocation.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QUuid>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#include <algorithm>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::plugin::elevation {

namespace {

constexpr int kMaxFiles = 20000;
constexpr int kKeptTrees = 2; // the confirmed copy, and the one before (an update waiting to be put in place)
constexpr int kMaxCached = 50000; // hashes kept; past it the cache starts again
const QString kRecord = u"record.json"_s;
const QString kTemp = u"temp"_s; // a plugin's TEMP, beside its copies
// The store's folders: administrators own them, administrators and SYSTEM
// alone may open them (a copy may hold what only an administrator could
// read: nothing of it is for ordinary users), nothing inherited from above.
constexpr wchar_t kStoreSddl[] = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";

QMutex g_store; // the store's writes, one at a time (confirm, keep, forget, sweep)
QMutex g_cache; // the hash cache

struct Cached {
    qint64 size = 0;
    qint64 time = 0;
    QByteArray sha256;
};
QHash<QString, Cached> g_hashes; // lower-cased native path -> what it hashed to at that size and time

// A HANDLE closed when it goes.
class Owned {
public:
    Owned() = default;
    explicit Owned(HANDLE h)
        : m_h(h)
    {
    }
    Owned(Owned&& o) noexcept
        : m_h(std::exchange(o.m_h, INVALID_HANDLE_VALUE))
    {
    }
    Owned& operator=(Owned&& o) noexcept
    {
        if (this != &o) {
            reset();
            m_h = std::exchange(o.m_h, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    ~Owned() { reset(); }
    HANDLE get() const { return m_h; }
    bool valid() const { return m_h && m_h != INVALID_HANDLE_VALUE; }
    void reset()
    {
        if (valid())
            ::CloseHandle(m_h);
        m_h = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE m_h = INVALID_HANDLE_VALUE;
};

std::wstring wide(const QString& s)
{
    return QDir::toNativeSeparators(s).toStdWString();
}

QString native(const QString& s)
{
    return QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(s).absoluteFilePath()));
}

// The plugin's folder in the store: its id and which folder it is (a
// profile of its own, a developer's folder), so two never share a record.
QString keyOf(const QString& id, const QString& pluginDir)
{
    const QByteArray where = native(pluginDir).toLower().toUtf8();
    return id + u'-' + QString::fromLatin1(QCryptographicHash::hash(where, QCryptographicHash::Sha256).toHex().left(8));
}

QString longPath(const QString& path)
{
    const std::wstring w = wide(path);
    std::wstring out(32768, L'\0');
    const DWORD n = ::GetLongPathNameW(w.c_str(), out.data(), static_cast<DWORD>(out.size()));
    if (n == 0 || n >= out.size())
        return QDir::toNativeSeparators(path);
    out.resize(n);
    return QString::fromStdWString(out);
}

// Where an open handle really is ("C:\…", "\\server\share\…").
QString finalPath(HANDLE h)
{
    std::wstring out(32768, L'\0');
    const DWORD n = ::GetFinalPathNameByHandleW(h, out.data(), static_cast<DWORD>(out.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0 || n >= out.size())
        return {};
    out.resize(n);
    QString path = QString::fromStdWString(out);
    if (path.startsWith(u"\\\\?\\UNC\\"_s))
        path = u"\\\\"_s + path.mid(8);
    else if (path.startsWith(u"\\\\?\\"_s))
        path = path.mid(4);
    return path;
}

enum class Opened { Ok, Missing, Unsafe };

// A folder of the store, opened without following a link and kept from
// being deleted or renamed while open (no FILE_SHARE_DELETE; the access
// asked for includes listing it, which is what makes the sharing count).
// It must be a folder, not a link, really where it was asked for, and
// (`requireProtected`) owned by administrators or SYSTEM with nobody else
// allowed to change it as `role` says.
Opened openLevel(const QString& path, bool requireProtected, svcloc::Role role, Owned& out, QString* why)
{
    const QString shown = QDir::toNativeSeparators(path);
    Owned h(::CreateFileW(wide(path).c_str(), FILE_LIST_DIRECTORY | READ_CONTROL | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!h.valid()) {
        const DWORD e = ::GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            if (why)
                *why = u"没有 %1"_s.arg(shown);
            return Opened::Missing;
        }
        if (why)
            *why = u"打不开 %1（错误 %2）"_s.arg(shown).arg(e);
        return Opened::Unsafe;
    }
    FILE_ATTRIBUTE_TAG_INFO tag {};
    if (!::GetFileInformationByHandleEx(h.get(), FileAttributeTagInfo, &tag, sizeof tag)) {
        if (why)
            *why = u"读不了 %1 的属性"_s.arg(shown);
        return Opened::Unsafe;
    }
    if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (why)
            *why = u"%1 是一个链接"_s.arg(shown);
        return Opened::Unsafe;
    }
    if (!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (why)
            *why = u"%1 不是文件夹"_s.arg(shown);
        return Opened::Unsafe;
    }
    // Nothing above it was a link either: it is where it was asked for.
    if (finalPath(h.get()).compare(longPath(path), Qt::CaseInsensitive) != 0) {
        if (why)
            *why = u"%1 实际在 %2"_s.arg(shown, finalPath(h.get()));
        return Opened::Unsafe;
    }
    if (requireProtected) {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        PSID owner = nullptr;
        PACL dacl = nullptr;
        const DWORD e = ::GetSecurityInfo(h.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
            nullptr, &dacl, nullptr, &descriptor);
        const bool unsafe = e != ERROR_SUCCESS || svcloc::writableByNonAdmin(owner, dacl, role);
        if (descriptor)
            ::LocalFree(descriptor);
        if (unsafe) {
            if (why)
                *why = u"%1 普通权限也能改（或者不是管理员建的）"_s.arg(shown);
            return Opened::Unsafe;
        }
    }
    out = std::move(h);
    return Opened::Ok;
}

// A new folder of the store: administrators' from the start (never one
// that was there already: the caller opens and checks it after).
bool makeLevel(const QString& path, bool requireProtected, QString* why)
{
    if (!requireProtected) {
        QDir().mkpath(path);
        return true;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(kStoreSddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        if (why)
            *why = u"建不了受保护的文件夹（错误 %1）"_s.arg(::GetLastError());
        return false;
    }
    SECURITY_ATTRIBUTES attributes {sizeof attributes, descriptor, FALSE};
    const bool made = ::CreateDirectoryW(wide(path).c_str(), &attributes);
    const DWORD e = ::GetLastError();
    ::LocalFree(descriptor);
    if (!made && e != ERROR_ALREADY_EXISTS) {
        if (why)
            *why = u"建不了 %1（错误 %2）"_s.arg(QDir::toNativeSeparators(path)).arg(e);
        return false;
    }
    return true;
}

// `path`, opened as openLevel does; missing and `create`: made first.
Opened openOrMake(const QString& path, bool requireProtected, svcloc::Role role, bool create, Owned& out, QString* why)
{
    Opened opened = openLevel(path, requireProtected, role, out, why);
    if (opened == Opened::Missing && create) {
        if (!makeLevel(path, requireProtected, why))
            return Opened::Unsafe;
        opened = openLevel(path, requireProtected, role, out, why);
    }
    return opened;
}

} // namespace

class Hold {
public:
    std::vector<Owned> handles;
};

namespace {

// The store's root and the folder above it, opened and held; what is above
// that (Program Files) must be a folder only administrators can change.
Opened openRoot(const Store& store, bool create, Hold& held, QString* why)
{
    if (store.root.isEmpty()) {
        if (why)
            *why = u"没有放确认副本的地方"_s;
        return Opened::Unsafe;
    }
    if (create && store.requireProtected && !processElevated()) {
        if (why)
            *why = u"Win顺 不是以管理员身份运行"_s;
        return Opened::Unsafe;
    }
    const QString root = native(store.root);
    QStringList levels {root};
    if (store.requireProtected) {
        const QString base = QFileInfo(root).path();
        std::wstring unsafe;
        if (!svcloc::folderProtected(wide(QFileInfo(base).path()), &unsafe)) {
            if (why)
                *why = u"%1 普通权限也能改"_s.arg(QString::fromStdWString(unsafe));
            return Opened::Unsafe;
        }
        levels.prepend(QDir::toNativeSeparators(base));
    }
    for (const QString& level : std::as_const(levels)) {
        Owned h;
        const Opened opened = openOrMake(level, store.requireProtected, svcloc::Role::Ancestor, create, h, why);
        if (opened != Opened::Ok)
            return opened;
        held.handles.push_back(std::move(h));
    }
    return Opened::Ok;
}

struct Found {
    QString path; // relative, '/'
    QString full; // native
    qint64 size = 0;
    qint64 time = 0;
};

// Every file under `dir`, without following a link: one inside is refused
// (a junction to C:\ would be copied whole; a link swapped in later would
// point the copy elsewhere).
bool walk(const QString& dir, const QString& relative, QList<Found>& out, qint64& total, QString* error)
{
    WIN32_FIND_DATAW data;
    const std::wstring pattern = wide(dir) + L"\\*";
    const HANDLE find = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD e = ::GetLastError();
        if (e == ERROR_FILE_NOT_FOUND)
            return true;
        if (error)
            *error = u"读不了 %1（错误 %2）"_s.arg(QDir::toNativeSeparators(dir)).arg(e);
        return false;
    }
    bool ok = true;
    do {
        const QString name = QString::fromWCharArray(data.cFileName);
        if (name == u"." || name == u"..")
            continue;
        const QString rel = relative.isEmpty() ? name : relative + u'/' + name;
        const QString full = QDir::toNativeSeparators(dir) + u'\\' + name;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            if (error)
                *error = u"插件文件夹里有链接：%1"_s.arg(rel);
            ok = false;
            break;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!walk(full, rel, out, total, error)) {
                ok = false;
                break;
            }
            continue;
        }
        Found f;
        f.path = rel;
        f.full = full;
        f.size = (static_cast<qint64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        f.time = (static_cast<qint64>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
        total += f.size;
        out.push_back(f);
        if (out.size() > kMaxFiles || total > install::kMaxArchiveBytes) {
            if (error)
                *error = u"插件文件夹太大（超过 %1 个文件或 %2 MB）"_s.arg(kMaxFiles).arg(install::kMaxArchiveBytes >> 20);
            ok = false;
            break;
        }
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);
    return ok;
}

// The files of a plugin's folder, which must not be a link itself (an
// elevated Win顺 copying what a link points at could copy what only an
// administrator may read).
QList<Found> found(const QString& dir, QString* error)
{
    Owned root(::CreateFileW(wide(dir).c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    FILE_ATTRIBUTE_TAG_INFO tag {};
    if (!root.valid() || !::GetFileInformationByHandleEx(root.get(), FileAttributeTagInfo, &tag, sizeof tag)
        || !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (error)
            *error = u"没有 %1"_s.arg(QDir::toNativeSeparators(dir));
        return {};
    }
    if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (error)
            *error = u"插件文件夹是一个链接（%1），不能以管理员身份运行"_s.arg(QDir::toNativeSeparators(dir));
        return {};
    }
    QList<Found> files;
    qint64 total = 0;
    if (!walk(dir, {}, files, total, error))
        return {};
    std::sort(files.begin(), files.end(), [](const Found& a, const Found& b) { return a.path.compare(b.path, Qt::CaseInsensitive) < 0; });
    return files;
}

// The whole file, read once: no write while it is read, never through a
// link put in its place since it was listed, and not a hard link (another
// name of a file elsewhere, which may be one only an administrator reads).
bool readFile(const QString& path, QByteArray& bytes, QString* error)
{
    Owned file(::CreateFileW(wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid()) {
        const DWORD e = ::GetLastError();
        if (error)
            *error = u"读不了 %1（错误 %2）"_s.arg(QDir::toNativeSeparators(path)).arg(e);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info {};
    LARGE_INTEGER size {};
    if (!::GetFileInformationByHandle(file.get(), &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (error)
            *error = u"%1 是一个链接"_s.arg(QDir::toNativeSeparators(path));
        return false;
    }
    if (info.nNumberOfLinks > 1) {
        if (error)
            *error = u"%1 有硬链接（同一个文件的另一个名字）"_s.arg(QDir::toNativeSeparators(path));
        return false;
    }
    bool ok = ::GetFileSizeEx(file.get(), &size) && size.QuadPart <= install::kMaxArchiveBytes;
    if (ok) {
        bytes.resize(static_cast<qsizetype>(size.QuadPart));
        qint64 done = 0;
        while (ok && done < size.QuadPart) {
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>(std::min<qint64>(size.QuadPart - done, 1 << 20));
            ok = ::ReadFile(file.get(), bytes.data() + done, want, &got, nullptr) && got > 0;
            done += got;
        }
    }
    if (!ok && error)
        *error = u"读不了 %1"_s.arg(QDir::toNativeSeparators(path));
    return ok;
}

QByteArray sha256Of(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

// A file's SHA-256, from the cache while its size and time are what they
// were when it was hashed (`fresh`: read again whatever the cache says).
bool hashOf(const Found& f, bool fresh, QByteArray* sha, QString* error)
{
    const QString key = f.full.toLower();
    if (!fresh) {
        const QMutexLocker lock(&g_cache);
        const auto it = g_hashes.constFind(key);
        if (it != g_hashes.cend() && it->size == f.size && it->time == f.time) {
            *sha = it->sha256;
            return true;
        }
    }
    QByteArray bytes;
    if (!readFile(f.full, bytes, error))
        return false;
    *sha = sha256Of(bytes);
    const QMutexLocker lock(&g_cache);
    if (g_hashes.size() >= kMaxCached)
        g_hashes.clear();
    g_hashes.insert(key, Cached {f.size, f.time, *sha});
    return true;
}

QList<File> hashed(const QList<Found>& files, bool fresh, QString* error)
{
    QList<File> out;
    out.reserve(files.size());
    for (const Found& f : files) {
        File file {f.path, f.size, {}};
        if (!hashOf(f, fresh, &file.sha256, error))
            return {};
        out.push_back(file);
    }
    return out;
}

QList<File> filesOf(const QString& dir, bool fresh, QString* error)
{
    QString why;
    const QList<Found> files = found(dir, &why);
    if (!why.isEmpty()) {
        if (error)
            *error = why;
        return {};
    }
    return hashed(files, fresh, error);
}

struct Tree {
    QString name;
    QString version;
    QList<File> files;
};

struct Record {
    QString id;
    QString dir; // the plugin's folder
    QList<Tree> trees;
};

Record readRecord(const QString& keyDir)
{
    QByteArray bytes;
    if (!readFile(keyDir + u'/' + kRecord, bytes, nullptr))
        return {};
    const QJsonObject root = QJsonDocument::fromJson(bytes).object();
    Record record {root.value(u"id"_s).toString(), root.value(u"dir"_s).toString(), {}};
    for (const QJsonValue& t : root.value(u"trees"_s).toArray()) {
        const QJsonObject o = t.toObject();
        Tree tree {o.value(u"name"_s).toString(), o.value(u"version"_s).toString(), {}};
        if (tree.name.isEmpty() || tree.name.contains(u'/') || tree.name.contains(u'\\') || tree.name.startsWith(u'.') || tree.name == kTemp)
            continue;
        for (const QJsonValue& f : o.value(u"files"_s).toArray()) {
            const QJsonObject fo = f.toObject();
            tree.files.push_back(File {fo.value(u"path"_s).toString(), static_cast<qint64>(fo.value(u"size"_s).toDouble()),
                fo.value(u"sha256"_s).toString().toLatin1()});
        }
        record.trees.push_back(tree);
    }
    return record;
}

bool writeRecord(const QString& keyDir, const Record& record, QString* error)
{
    QJsonArray list;
    for (const Tree& tree : record.trees) {
        QJsonArray files;
        for (const File& f : tree.files)
            files.push_back(QJsonObject {{u"path"_s, f.path}, {u"size"_s, static_cast<double>(f.size)}, {u"sha256"_s, QString::fromLatin1(f.sha256)}});
        list.push_back(QJsonObject {{u"name"_s, tree.name}, {u"version"_s, tree.version}, {u"files"_s, files}});
    }
    const QByteArray text = QJsonDocument(QJsonObject {{u"id"_s, record.id}, {u"dir"_s, record.dir}, {u"trees"_s, list}})
                                .toJson(QJsonDocument::Indented);
    const QString target = keyDir + u'/' + kRecord;
    const QString temporary = target + u".new"_s;
    QFile::remove(temporary);
    QFile file(temporary);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(text) != text.size() || !file.flush()) {
        if (error)
            *error = u"记录写不了：%1"_s.arg(file.errorString());
        return false;
    }
    file.close();
    if (!::MoveFileExW(wide(temporary).c_str(), wide(target).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error)
            *error = u"记录换不了（错误 %1）"_s.arg(::GetLastError());
        return false;
    }
    return true;
}

bool sameFiles(const QList<File>& a, const QList<File>& b)
{
    if (a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (a[i].path.compare(b[i].path, Qt::CaseInsensitive) != 0 || a[i].size != b[i].size || a[i].sha256 != b[i].sha256)
            return false;
    }
    return true;
}

QByteArray treeName(const QList<File>& files)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const File& f : files) {
        hash.addData(f.path.toLower().toUtf8());
        hash.addData(QByteArrayView("\n"));
        hash.addData(f.sha256);
        hash.addData(QByteArrayView("\n"));
    }
    return hash.result().toHex().left(16);
}

QString versionIn(const QString& dir)
{
    QByteArray bytes;
    if (!readFile(dir + u"/plugin.json"_s, bytes, nullptr))
        return {};
    return QJsonDocument::fromJson(bytes).object().value(u"version"_s).toString();
}

// A folder of the store, gone: renamed aside first (a folder a running
// plugin uses cannot be, and is left for next time), then deleted.
void removeLevel(const QString& path)
{
    const QString aside = QFileInfo(path).path() + u"/.old-"_s + QUuid::createUuid().toString(QUuid::Id128);
    if (::MoveFileExW(wide(path).c_str(), wide(aside).c_str(), 0))
        QDir(aside).removeRecursively();
}

// The plugin's folder in the store, opened (made when `create`), with the
// root above it: everything a write needs held.
Opened openKey(const Store& store, const QString& keyDir, bool create, Hold& held, QString* why)
{
    const Opened root = openRoot(store, create, held, why);
    if (root != Opened::Ok)
        return root;
    Owned h;
    const Opened key = openOrMake(keyDir, store.requireProtected, svcloc::Role::Ancestor, create, h, why);
    if (key == Opened::Ok)
        held.handles.push_back(std::move(h));
    return key;
}

// A new folder to stage a copy in, inside `keyDir`.
QString newStaging(const QString& keyDir, bool requireProtected, QString* why)
{
    const QString staged = keyDir + u"/.new-"_s + QUuid::createUuid().toString(QUuid::Id128);
    if (!makeLevel(staged, requireProtected, why))
        return {};
    return staged;
}

// `staged` (a folder in `keyDir`, which only administrators can change)
// becomes a confirmed copy: hashed where it lies, kept under its tree's
// name, and the record says so. g_store is held by the caller.
bool finish(const QString& keyDir, const QString& id, const QString& pluginDir, const QString& staged, bool requireProtected, QString* error)
{
    QString why;
    const QList<File> files = filesOf(staged, true, &why);
    if (files.isEmpty()) {
        QDir(staged).removeRecursively();
        if (error)
            *error = why.isEmpty() ? u"插件里没有文件"_s : why;
        return false;
    }
    const QString version = versionIn(staged); // for the card's words only
    const QString name = QString::fromLatin1(treeName(files));
    const QString target = keyDir + u'\\' + name;
    const bool there = QFileInfo::exists(target) || QFileInfo(target).isSymLink() || QFileInfo(target).isJunction();
    if (there) {
        // The same files confirmed before: that copy, if it is still what it must be.
        Owned h;
        if (openLevel(target, requireProtected, svcloc::Role::Parent, h, &why) != Opened::Ok) {
            QDir(staged).removeRecursively();
            if (error)
                *error = u"确认副本的文件夹不安全：%1"_s.arg(why);
            return false;
        }
        QDir(staged).removeRecursively();
    } else if (!::MoveFileExW(wide(staged).c_str(), wide(target).c_str(), 0)) {
        const DWORD e = ::GetLastError();
        QDir(staged).removeRecursively();
        if (error)
            *error = u"确认副本放不进去（错误 %1）"_s.arg(e);
        return false;
    }
    Record record {id, native(pluginDir), {Tree {name, version, files}}};
    for (const Tree& t : readRecord(keyDir).trees) {
        if (t.name != name && record.trees.size() < kKeptTrees)
            record.trees.push_back(t);
    }
    if (!writeRecord(keyDir, record, error))
        return false;
    // What no longer counts goes; a copy a plugin still runs from cannot be
    // renamed and stays (not in the record: never started again) until a
    // later write. Staging folders are this operation's or a broken one's:
    // the writes are one at a time.
    for (const QFileInfo& info : QDir(keyDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        const QString n = info.fileName();
        if (n != kTemp && std::none_of(record.trees.cbegin(), record.trees.cend(), [&](const Tree& t) { return t.name == n; }))
            removeLevel(info.filePath());
    }
    return true;
}

// The files of a folder, read once each and written into `staged`.
bool copyInto(const QString& pluginDir, const QString& staged, QString* error)
{
    QString why;
    const QList<Found> files = found(pluginDir, &why);
    if (files.isEmpty()) {
        if (error)
            *error = why.isEmpty() ? u"插件里没有文件"_s : why;
        return false;
    }
    for (const Found& f : files) {
        QByteArray bytes;
        if (!readFile(f.full, bytes, error))
            return false;
        {
            // What was read is what the folder holds now (the cache may have
            // been fooled by a time put back).
            const QMutexLocker cache(&g_cache);
            g_hashes.insert(f.full.toLower(), Cached {f.size, f.time, sha256Of(bytes)});
        }
        const QString target = staged + u'/' + f.path;
        QDir().mkpath(QFileInfo(target).path());
        QFile out(target);
        if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly) || out.write(bytes) != bytes.size()) {
            if (error)
                *error = u"复制不了 %1：%2"_s.arg(f.path, out.errorString());
            return false;
        }
    }
    return true;
}

// A package unpacked into a new staging folder of `keyDir`; empty with `error`.
QString unpacked(const QString& keyDir, bool requireProtected, const QByteArray& package, QString* error)
{
    const QString staged = newStaging(keyDir, requireProtected, error);
    if (staged.isEmpty())
        return {};
    const QString problem = install::unpackArchive(package, staged);
    if (!problem.isEmpty()) {
        QDir(staged).removeRecursively();
        if (error)
            *error = problem;
        return {};
    }
    return staged;
}

} // namespace

Store defaultStore()
{
    Store store;
    PWSTR data = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &data)) && data) {
        const std::wstring sid = pipe::userSid();
        if (!sid.empty())
            store.root = QString::fromWCharArray(data) + u"\\WinShun Elevated Plugins\\"_s + QString::fromStdWString(sid);
    }
    if (data)
        ::CoTaskMemFree(data);
    return store;
}

bool processElevated()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    const Owned owned(token);
    TOKEN_ELEVATION elevation {};
    DWORD size = 0;
    return ::GetTokenInformation(owned.get(), TokenElevation, &elevation, sizeof elevation, &size) && elevation.TokenIsElevated;
}

QList<File> listFiles(const QString& dir, QString* error)
{
    return filesOf(dir, false, error);
}

Result check(const Store& store, const QString& id, const QString& pluginDir, bool hold)
{
    Result r;
    QString why;
    auto held = std::make_shared<Hold>();
    const QString keyDir = native(store.root) + u'\\' + keyOf(id, pluginDir);
    switch (openKey(store, keyDir, false, *held, &why)) {
    case Opened::Ok: break;
    case Opened::Missing: r.check = Check::NoRecord; return r;
    case Opened::Unsafe:
        r.check = Check::Unavailable;
        r.detail = u"放确认副本的文件夹不安全：%1"_s.arg(why);
        return r;
    }
    const Record record = readRecord(keyDir);
    if (record.trees.isEmpty()) {
        r.check = Check::NoRecord;
        return r;
    }
    r.version = record.trees.first().version;
    const QList<File> now = listFiles(pluginDir, &why);
    if (now.isEmpty()) {
        r.check = Check::Changed;
        r.detail = why;
        return r;
    }
    for (const Tree& tree : record.trees) {
        if (!sameFiles(now, tree.files))
            continue;
        const QString dir = keyDir + u'\\' + tree.name;
        Owned h;
        switch (openLevel(dir, store.requireProtected, svcloc::Role::Parent, h, &why)) {
        case Opened::Ok: break;
        case Opened::Missing: r.check = Check::NoRecord; return r;
        case Opened::Unsafe:
            r.check = Check::Unavailable;
            r.detail = u"确认副本的文件夹不安全：%1"_s.arg(why);
            return r;
        }
        held->handles.push_back(std::move(h));
        r.check = Check::Match;
        r.dir = dir;
        r.version = tree.version;
        if (hold)
            r.hold = held;
        return r;
    }
    r.check = Check::Changed;
    return r;
}

bool confirmFolder(const Store& store, const QString& id, const QString& pluginDir, QString* error)
{
    const QMutexLocker lock(&g_store);
    Hold held;
    const QString keyDir = native(store.root) + u'\\' + keyOf(id, pluginDir);
    if (openKey(store, keyDir, true, held, error) != Opened::Ok)
        return false;
    const QString staged = newStaging(keyDir, store.requireProtected, error);
    if (staged.isEmpty())
        return false;
    if (!copyInto(pluginDir, staged, error)) {
        QDir(staged).removeRecursively();
        return false;
    }
    return finish(keyDir, id, pluginDir, staged, store.requireProtected, error);
}

bool confirmPackage(const Store& store, const QString& id, const QString& pluginDir, const QByteArray& package, QString* error)
{
    const QMutexLocker lock(&g_store);
    Hold held;
    const QString keyDir = native(store.root) + u'\\' + keyOf(id, pluginDir);
    if (openKey(store, keyDir, true, held, error) != Opened::Ok)
        return false;
    const QString staged = unpacked(keyDir, store.requireProtected, package, error);
    if (staged.isEmpty())
        return false;
    QString why;
    const QList<File> fromPackage = filesOf(staged, true, &why);
    const QList<File> inFolder = filesOf(pluginDir, true, &why);
    if (fromPackage.isEmpty() || !sameFiles(fromPackage, inFolder)) {
        QDir(staged).removeRecursively();
        if (error)
            *error = why.isEmpty() ? u"插件的文件和插件库里的不一样，可能被改过。从插件库重新安装它，再确认"_s : why;
        return false;
    }
    return finish(keyDir, id, pluginDir, staged, store.requireProtected, error);
}

bool keepConfirmed(const Store& store, const QString& id, const QString& pluginDir, const QByteArray& package, QString* error)
{
    if (!hasRecord(store, id, pluginDir))
        return true;
    const QMutexLocker lock(&g_store);
    Hold held;
    const QString keyDir = native(store.root) + u'\\' + keyOf(id, pluginDir);
    if (openKey(store, keyDir, false, held, error) != Opened::Ok)
        return false;
    const QString staged = unpacked(keyDir, store.requireProtected, package, error);
    if (staged.isEmpty())
        return false;
    // Never back to an older version than the one confirmed (an old list
    // served again cannot bring back what an update fixed).
    const Record record = readRecord(keyDir);
    const QString version = versionIn(staged);
    if (!record.trees.isEmpty() && !record.trees.first().version.isEmpty()
        && (version.isEmpty() || compareVersions(version, record.trees.first().version) < 0)) {
        QDir(staged).removeRecursively();
        if (error)
            *error = u"包的版本（%1）比确认过的（%2）旧"_s.arg(version, record.trees.first().version);
        return false;
    }
    return finish(keyDir, id, pluginDir, staged, store.requireProtected, error);
}

bool hasRecord(const Store& store, const QString& id, const QString& pluginDir)
{
    if (store.root.isEmpty())
        return false;
    return QFileInfo::exists(native(store.root) + u'\\' + keyOf(id, pluginDir) + u'\\' + kRecord);
}

void forget(const Store& store, const QString& id, const QString& pluginDir)
{
    if (store.root.isEmpty() || (store.requireProtected && !processElevated()))
        return;
    const QMutexLocker lock(&g_store);
    Hold held;
    if (openRoot(store, false, held, nullptr) != Opened::Ok)
        return;
    const QString keyDir = native(store.root) + u'\\' + keyOf(id, pluginDir);
    if (QFileInfo(keyDir).isDir())
        removeLevel(keyDir);
}

int sweep(const Store& store, const std::function<bool(const QString& id, const QString& pluginDir)>& installed)
{
    if (store.root.isEmpty() || (store.requireProtected && !processElevated()))
        return 0;
    const QMutexLocker lock(&g_store);
    Hold held;
    if (openRoot(store, false, held, nullptr) != Opened::Ok)
        return 0;
    int removed = 0;
    for (const QFileInfo& info : QDir(native(store.root)).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        const QString name = info.fileName();
        bool gone = name.startsWith(u".old-"_s);
        if (!gone) {
            const Record record = readRecord(info.filePath());
            gone = record.id.isEmpty() || record.dir.isEmpty() || keyOf(record.id, record.dir) != name || !installed(record.id, record.dir);
        }
        if (gone) {
            removeLevel(info.filePath());
            ++removed;
        }
    }
    return removed;
}

QString tempDir(const Store& store, const QString& id, const QString& pluginDir)
{
    if (store.root.isEmpty())
        return {};
    const QString dir = native(store.root) + u'\\' + keyOf(id, pluginDir) + u'\\' + kTemp;
    Owned h;
    const bool create = !store.requireProtected || processElevated();
    if (openOrMake(dir, store.requireProtected, svcloc::Role::Parent, create, h, nullptr) != Opened::Ok)
        return {};
    return dir;
}

Verdict decide(bool declares, bool hostElevated, Check check)
{
    if (!declares)
        return Verdict::NotAsked;
    if (!hostElevated)
        return Verdict::HostNotElevated;
    switch (check) {
    case Check::Match: return Verdict::Elevated;
    case Check::NoRecord: return Verdict::Unconfirmed;
    case Check::Changed: return Verdict::Changed;
    case Check::Unavailable: return Verdict::Unavailable;
    }
    return Verdict::Unavailable;
}

QString verdictName(Verdict verdict)
{
    switch (verdict) {
    case Verdict::NotAsked:
    case Verdict::HostNotElevated: return {};
    case Verdict::Elevated: return u"elevated"_s;
    case Verdict::Unconfirmed: return u"unconfirmed"_s;
    case Verdict::Changed: return u"changed"_s;
    case Verdict::Unavailable: return u"unavailable"_s;
    }
    return {};
}

} // namespace ws::plugin::elevation
