#include "Elevation.h"

#include "Install.h"
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
#include <shlobj.h>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::plugin::elevation {

namespace {

constexpr int kMaxFiles = 20000;
constexpr int kKeptTrees = 2; // the confirmed copy, and the one before (an update waiting to be put in place)
const QString kRecord = u"record.json"_s;

QMutex g_mutex; // the store's writes and the hash cache

struct Cached {
    qint64 size = 0;
    qint64 time = 0;
    QByteArray sha256;
};
QHash<QString, Cached> g_hashes; // lower-cased native path -> what it hashed to at that size and time

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
        if (::GetLastError() == ERROR_FILE_NOT_FOUND)
            return true;
        if (error)
            *error = u"读不了 %1（错误 %2）"_s.arg(QDir::toNativeSeparators(dir)).arg(::GetLastError());
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

QList<Found> found(const QString& dir, QString* error)
{
    QList<Found> files;
    qint64 total = 0;
    if (!QFileInfo(dir).isDir()) {
        if (error)
            *error = u"没有 %1"_s.arg(QDir::toNativeSeparators(dir));
        return {};
    }
    if (!walk(dir, {}, files, total, error))
        return {};
    std::sort(files.begin(), files.end(), [](const Found& a, const Found& b) { return a.path.compare(b.path, Qt::CaseInsensitive) < 0; });
    return files;
}

// The whole file, read once: no write while it is read, and never through a
// link put in its place since it was listed.
bool readFile(const QString& path, QByteArray& bytes, QString* error)
{
    const HANDLE file = ::CreateFileW(wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error)
            *error = u"读不了 %1（错误 %2）"_s.arg(QDir::toNativeSeparators(path)).arg(::GetLastError());
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info {};
    LARGE_INTEGER size {};
    bool ok = ::GetFileInformationByHandle(file, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        && ::GetFileSizeEx(file, &size) && size.QuadPart <= install::kMaxArchiveBytes;
    if (ok) {
        bytes.resize(static_cast<qsizetype>(size.QuadPart));
        qint64 done = 0;
        while (ok && done < size.QuadPart) {
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>(std::min<qint64>(size.QuadPart - done, 1 << 20));
            ok = ::ReadFile(file, bytes.data() + done, want, &got, nullptr) && got > 0;
            done += got;
        }
    }
    ::CloseHandle(file);
    if (!ok && error)
        *error = u"读不了 %1"_s.arg(QDir::toNativeSeparators(path));
    return ok;
}

QByteArray sha256Of(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

// A file's SHA-256, from the cache while its size and time are what they
// were when it was hashed.
bool hashOf(const Found& f, QByteArray* sha, QString* error)
{
    const QString key = f.full.toLower();
    {
        const QMutexLocker lock(&g_mutex);
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
    const QMutexLocker lock(&g_mutex);
    g_hashes.insert(key, Cached {f.size, f.time, *sha});
    return true;
}

QList<File> hashed(const QList<Found>& files, QString* error)
{
    QList<File> out;
    out.reserve(files.size());
    for (const Found& f : files) {
        File file {f.path, f.size, {}};
        if (!hashOf(f, &file.sha256, error))
            return {};
        out.push_back(file);
    }
    return out;
}

// The store's root, made (when `create`) and checked. Missing and not to be
// made: false with `missing` set.
bool rootReady(const Store& store, bool create, QString* error, bool* missing = nullptr)
{
    if (missing)
        *missing = false;
    if (store.root.isEmpty()) {
        if (error)
            *error = u"没有放确认副本的地方"_s;
        return false;
    }
    if (!store.requireProtected) {
        if (create)
            QDir().mkpath(store.root);
        if (!QFileInfo(store.root).isDir()) {
            if (missing)
                *missing = true;
            if (error)
                *error = u"没有 %1"_s.arg(QDir::toNativeSeparators(store.root));
            return false;
        }
        return true;
    }
    const QString root = native(store.root);
    if (create) {
        if (!processElevated()) {
            if (error)
                *error = u"Win顺 不是以管理员身份运行"_s;
            return false;
        }
        // %ProgramData%\WinShun, elevated-plugins, <SID>: each made with an
        // administrators-only DACL, or made so when it was there and weak
        // (ProgramData lets ordinary users add folders; someone may have
        // been first). Checked as a whole below.
        const QString parent = QFileInfo(root).path();
        for (const QString& level : {QFileInfo(parent).path(), parent, root})
            svcloc::secureLogFolder(wide(level));
    } else if (!QFileInfo(root).isDir()) {
        if (missing)
            *missing = true;
        if (error)
            *error = u"没有 %1"_s.arg(root);
        return false;
    }
    std::wstring why;
    if (!svcloc::folderProtected(wide(root), &why)) {
        if (error)
            *error = u"放确认副本的文件夹不安全：%1 普通权限也能改"_s.arg(QString::fromStdWString(why));
        return false;
    }
    return true;
}

struct Tree {
    QString name;
    QString version;
    QList<File> files;
};

QList<Tree> readRecord(const QString& keyDir)
{
    QFile file(keyDir + u'/' + kRecord);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QList<Tree> trees;
    for (const QJsonValue& t : QJsonDocument::fromJson(file.readAll()).object().value(u"trees"_s).toArray()) {
        const QJsonObject o = t.toObject();
        Tree tree {o.value(u"name"_s).toString(), o.value(u"version"_s).toString(), {}};
        if (tree.name.isEmpty() || tree.name.contains(u'/') || tree.name.contains(u'\\') || tree.name.startsWith(u'.'))
            continue;
        for (const QJsonValue& f : o.value(u"files"_s).toArray()) {
            const QJsonObject fo = f.toObject();
            tree.files.push_back(File {fo.value(u"path"_s).toString(), static_cast<qint64>(fo.value(u"size"_s).toDouble()),
                fo.value(u"sha256"_s).toString().toLatin1()});
        }
        trees.push_back(tree);
    }
    return trees;
}

bool writeRecord(const QString& keyDir, const QString& id, const QList<Tree>& trees, QString* error)
{
    QJsonArray list;
    for (const Tree& tree : trees) {
        QJsonArray files;
        for (const File& f : tree.files)
            files.push_back(QJsonObject {{u"path"_s, f.path}, {u"size"_s, static_cast<double>(f.size)}, {u"sha256"_s, QString::fromLatin1(f.sha256)}});
        list.push_back(QJsonObject {{u"name"_s, tree.name}, {u"version"_s, tree.version}, {u"files"_s, files}});
    }
    const QByteArray text = QJsonDocument(QJsonObject {{u"id"_s, id}, {u"trees"_s, list}}).toJson(QJsonDocument::Indented);
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

// `staged` (a folder in `keyDir`, which only administrators can change)
// becomes a confirmed copy: hashed where it lies, kept under its tree's
// name, and the record says so. g_mutex is not held (hashing takes it).
bool finish(const QString& keyDir, const QString& id, const QString& staged, QString* error)
{
    QString why;
    const QList<File> files = hashed(found(staged, &why), &why);
    if (files.isEmpty()) {
        QDir(staged).removeRecursively();
        if (error)
            *error = why.isEmpty() ? u"插件里没有文件"_s : why;
        return false;
    }
    QString version; // for the card's words only
    if (QFile manifest(staged + u"/plugin.json"_s); manifest.open(QIODevice::ReadOnly))
        version = QJsonDocument::fromJson(manifest.read(1 << 20)).object().value(u"version"_s).toString();
    const QString name = QString::fromLatin1(treeName(files));
    const QString target = keyDir + u'/' + name;
    const QMutexLocker lock(&g_mutex);
    if (QFileInfo(target).isDir())
        QDir(staged).removeRecursively(); // the same files confirmed before
    else if (!::MoveFileExW(wide(staged).c_str(), wide(target).c_str(), 0)) {
        const DWORD e = ::GetLastError();
        QDir(staged).removeRecursively();
        if (error)
            *error = u"确认副本放不进去（错误 %1）"_s.arg(e);
        return false;
    }
    QList<Tree> trees {Tree {name, version, files}};
    for (const Tree& t : readRecord(keyDir)) {
        if (t.name != name && trees.size() < kKeptTrees)
            trees.push_back(t);
    }
    if (!writeRecord(keyDir, id, trees, error))
        return false;
    // What no longer counts goes (one still running from it stays until next time).
    for (const QFileInfo& info : QDir(keyDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (std::none_of(trees.cbegin(), trees.cend(), [&](const Tree& t) { return t.name == info.fileName(); }))
            QDir(info.filePath()).removeRecursively();
    }
    return true;
}

QString newStaging(const QString& keyDir)
{
    return keyDir + u"/.new-"_s + QUuid::createUuid().toString(QUuid::Id128);
}

} // namespace

Store defaultStore()
{
    Store store;
    PWSTR data = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &data)) && data) {
        const std::wstring sid = pipe::userSid();
        if (!sid.empty())
            store.root = QString::fromWCharArray(data) + u"\\WinShun\\elevated-plugins\\"_s + QString::fromStdWString(sid);
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
    TOKEN_ELEVATION elevation {};
    DWORD size = 0;
    const bool ok = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size);
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

QList<File> listFiles(const QString& dir, QString* error)
{
    QString why;
    const QList<Found> files = found(dir, &why);
    if (!why.isEmpty()) {
        if (error)
            *error = why;
        return {};
    }
    return hashed(files, error);
}

Result check(const Store& store, const QString& id, const QString& pluginDir)
{
    Result r;
    QString why;
    bool missing = false;
    if (!rootReady(store, false, &why, &missing)) {
        r.check = missing ? Check::NoRecord : Check::Unavailable;
        r.detail = why;
        return r;
    }
    const QString keyDir = store.root + u'/' + keyOf(id, pluginDir);
    const QList<Tree> trees = readRecord(keyDir);
    if (trees.isEmpty()) {
        r.check = Check::NoRecord;
        return r;
    }
    r.version = trees.first().version;
    const QList<File> now = listFiles(pluginDir, &why);
    if (now.isEmpty()) {
        r.check = Check::Changed;
        r.detail = why;
        return r;
    }
    for (const Tree& tree : trees) {
        if (!sameFiles(now, tree.files))
            continue;
        const QString dir = QDir::toNativeSeparators(keyDir + u'/' + tree.name);
        std::wstring unsafe;
        if (!QFileInfo(dir).isDir()) {
            r.check = Check::NoRecord;
            return r;
        }
        if (store.requireProtected && !svcloc::folderProtected(wide(dir), &unsafe)) {
            r.check = Check::Unavailable;
            r.detail = u"确认副本的文件夹不安全：%1 普通权限也能改"_s.arg(QString::fromStdWString(unsafe));
            return r;
        }
        r.check = Check::Match;
        r.dir = dir;
        r.version = tree.version;
        return r;
    }
    r.check = Check::Changed;
    return r;
}

bool confirmFolder(const Store& store, const QString& id, const QString& pluginDir, QString* error)
{
    if (!rootReady(store, true, error))
        return false;
    QString why;
    const QList<Found> files = found(pluginDir, &why);
    if (files.isEmpty()) {
        if (error)
            *error = why.isEmpty() ? u"插件里没有文件"_s : why;
        return false;
    }
    const QString keyDir = store.root + u'/' + keyOf(id, pluginDir);
    QDir().mkpath(keyDir);
    const QString staged = newStaging(keyDir);
    for (const Found& f : files) {
        QByteArray bytes;
        if (!readFile(f.full, bytes, error)) {
            QDir(staged).removeRecursively();
            return false;
        }
        const QString target = staged + u'/' + f.path;
        QDir().mkpath(QFileInfo(target).path());
        QFile out(target);
        if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly) || out.write(bytes) != bytes.size()) {
            if (error)
                *error = u"复制不了 %1：%2"_s.arg(f.path, out.errorString());
            QDir(staged).removeRecursively();
            return false;
        }
    }
    return finish(keyDir, id, staged, error);
}

bool keepConfirmed(const Store& store, const QString& id, const QString& pluginDir, const QByteArray& package, QString* error)
{
    if (!hasRecord(store, id, pluginDir))
        return true;
    if (!rootReady(store, true, error))
        return false;
    const QString keyDir = store.root + u'/' + keyOf(id, pluginDir);
    const QString staged = newStaging(keyDir);
    const QString problem = install::unpackArchive(package, staged);
    if (!problem.isEmpty()) {
        QDir(staged).removeRecursively();
        if (error)
            *error = problem;
        return false;
    }
    return finish(keyDir, id, staged, error);
}

bool hasRecord(const Store& store, const QString& id, const QString& pluginDir)
{
    if (store.root.isEmpty())
        return false;
    return QFileInfo::exists(store.root + u'/' + keyOf(id, pluginDir) + u'/' + kRecord);
}

void forget(const Store& store, const QString& id, const QString& pluginDir)
{
    if (store.root.isEmpty() || (store.requireProtected && !processElevated()))
        return;
    const QMutexLocker lock(&g_mutex);
    QDir(store.root + u'/' + keyOf(id, pluginDir)).removeRecursively();
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
