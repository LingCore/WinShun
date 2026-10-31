#include "Install.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>

#include <miniz.h>

#include <windows.h>

#include <cstring>
#include <memory>
#include <string>

using namespace Qt::StringLiterals;

namespace ws::plugin::install {

namespace {

bool isArchive(const QString& path)
{
    return path.endsWith(u".zip"_s, Qt::CaseInsensitive) || path.endsWith(u".wsplugin"_s, Qt::CaseInsensitive);
}

// The name of an entry, in Unicode: UTF-8 when its flag says so, else the
// OEM code page (Explorer's own "压缩" writes that).
QString entryName(const mz_zip_archive_file_stat& stat)
{
    const std::size_t length = std::strlen(stat.m_filename);
    if (stat.m_bit_flag & (1u << 11))
        return QString::fromUtf8(stat.m_filename, static_cast<qsizetype>(length));
    std::wstring wide(length + 1, L'\0');
    const int n = ::MultiByteToWideChar(CP_OEMCP, 0, stat.m_filename, static_cast<int>(length), wide.data(), static_cast<int>(wide.size()));
    return n > 0 ? QString::fromWCharArray(wide.data(), n) : QString::fromLatin1(stat.m_filename, static_cast<qsizetype>(length));
}

// Unpacks the archive at `path` into `dir` (made here). The plugin.json may
// sit at the root or in one folder (as a zipped folder has it): the files
// land in `dir` either way. Empty on success, else why.
QString unpack(const QString& path, const QString& dir)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return u"打不开 %1"_s.arg(QDir::toNativeSeparators(path));
    if (file.size() > kMaxArchiveBytes)
        return u"压缩包超过 %1 MB"_s.arg(kMaxArchiveBytes >> 20);
    const QByteArray bytes = file.readAll();
    file.close();

    mz_zip_archive zip {};
    if (!mz_zip_reader_init_mem(&zip, bytes.constData(), static_cast<size_t>(bytes.size()), 0))
        return u"不是有效的 zip 压缩包"_s;
    struct End {
        mz_zip_archive* zip;
        ~End() { mz_zip_reader_end(zip); }
    } end {&zip};

    // The entries, and the one folder they all sit in, if any.
    struct Found {
        mz_uint index;
        QString name; // '/' separated, no leading one
        bool directory;
        quint64 size;
    };
    std::vector<Found> entries;
    quint64 total = 0;
    QString root; // "folder/" when every entry is under it
    bool firstEntry = true;
    bool manifestAtRoot = false;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat))
            return u"压缩包的目录读不出来"_s;
        QString name = entryName(stat).replace(u'\\', u'/');
        while (name.startsWith(u'/'))
            name.remove(0, 1);
        if (name.isEmpty())
            continue;
        const bool directory = mz_zip_reader_is_file_a_directory(&zip, i) || name.endsWith(u'/');
        if (name.contains(u"../"_s) || name.contains(u"/.."_s) || name == u".."_s || name.contains(u':'))
            return u"压缩包里有越出文件夹的路径：%1"_s.arg(name);
        total += stat.m_uncomp_size;
        if (total > static_cast<quint64>(kMaxArchiveBytes))
            return u"解压后超过 %1 MB"_s.arg(kMaxArchiveBytes >> 20);
        const qsizetype slash = name.indexOf(u'/');
        const QString top = slash < 0 ? QString() : name.left(slash + 1);
        if (firstEntry) {
            root = top;
            firstEntry = false;
        } else if (root != top)
            root.clear();
        if (name.compare(u"plugin.json"_s, Qt::CaseInsensitive) == 0)
            manifestAtRoot = true;
        entries.push_back({i, name, directory, stat.m_uncomp_size});
    }
    if (entries.empty())
        return u"压缩包是空的"_s;
    if (manifestAtRoot)
        root.clear(); // the manifest at the root: the folders beside it are the plugin's own
    bool manifest = manifestAtRoot;
    for (const Found& e : entries)
        manifest = manifest || (!root.isEmpty() && e.name.compare(root + u"plugin.json"_s, Qt::CaseInsensitive) == 0);
    if (!manifest)
        return u"压缩包里没有 plugin.json（要在最外层，或者在唯一的一个文件夹里）"_s;

    if (!QDir().mkpath(dir))
        return u"建不了临时文件夹 %1"_s.arg(QDir::toNativeSeparators(dir));
    for (const Found& e : entries) {
        const QString relative = root.isEmpty() ? e.name : e.name.mid(root.size());
        if (relative.isEmpty())
            continue;
        const QString target = dir + u'/' + relative;
        if (e.directory) {
            QDir().mkpath(target);
            continue;
        }
        QDir().mkpath(QFileInfo(target).path());
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, e.index, &size, 0);
        if (!data)
            return u"解压 %1 失败"_s.arg(e.name);
        QFile out(target);
        const bool written = out.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && out.write(static_cast<const char*>(data), static_cast<qint64>(size)) == static_cast<qint64>(size);
        mz_free(data);
        if (!written)
            return u"写不了 %1"_s.arg(QDir::toNativeSeparators(target));
    }
    return {};
}

// Copies a folder's files into `to` (made here). Empty on success.
QString copyTree(const QString& from, const QString& to)
{
    if (!QDir().mkpath(to))
        return u"建不了 %1"_s.arg(QDir::toNativeSeparators(to));
    QDirIterator it(from, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo info(it.next());
        const QString relative = QDir(from).relativeFilePath(info.filePath());
        const QString target = to + u'/' + relative;
        if (info.isDir()) {
            if (!QDir().mkpath(target))
                return u"建不了 %1"_s.arg(QDir::toNativeSeparators(target));
            continue;
        }
        QDir().mkpath(QFileInfo(target).path());
        if (!QFile::copy(info.filePath(), target))
            return u"复制不了 %1"_s.arg(QDir::toNativeSeparators(info.filePath()));
        ::SetFileAttributesW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(target).utf16()), FILE_ATTRIBUTE_NORMAL);
    }
    return {};
}

QString tempDir()
{
    return QDir::temp().filePath(u"WinShun-plugin-"_s + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
}

} // namespace

std::optional<Candidate> inspect(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return std::nullopt;
    Candidate c;
    c.source = QDir::toNativeSeparators(info.absoluteFilePath());
    if (info.isDir()) {
        c.dir = info.absoluteFilePath();
    } else if (isArchive(path)) {
        c.archive = true;
        c.dir = tempDir();
        c.error = unpack(info.absoluteFilePath(), c.dir);
        if (!c.error.isEmpty()) {
            discard(c);
            return c;
        }
    } else {
        c.error = u"要的是插件文件夹，或 .zip / .wsplugin 压缩包"_s;
        return c;
    }
    const QString manifestPath = c.dir + u"/plugin.json"_s;
    if (!QFileInfo::exists(manifestPath)) {
        c.error = u"这个文件夹里没有 plugin.json"_s;
        discard(c);
        return c;
    }
    QString error;
    c.manifest = Manifest::load(manifestPath, &error);
    if (!c.manifest) {
        c.error = error;
        discard(c);
    }
    return c;
}

QString place(const Candidate& candidate, const QString& userDir, QString* error)
{
    const auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return QString();
    };
    if (!candidate.manifest)
        return fail(candidate.error.isEmpty() ? u"不是可用的插件"_s : candidate.error);
    if (!QDir().mkpath(userDir))
        return fail(u"建不了 %1"_s.arg(QDir::toNativeSeparators(userDir)));
    const QString target = QDir(userDir).filePath(candidate.manifest->id);
    const QString sourceDir = QDir::cleanPath(candidate.dir);
    if (QDir::cleanPath(target).compare(sourceDir, Qt::CaseInsensitive) == 0)
        return QDir::toNativeSeparators(target); // installing what is installed: nothing to move
    if (QFileInfo::exists(target) && !QFileInfo::exists(target + u"/plugin.json"_s))
        return fail(u"%1 不是已安装的插件文件夹，为保留其中的数据，没有覆盖"_s.arg(QDir::toNativeSeparators(target)));
    // What is there goes aside first, and comes back if the new one cannot
    // be put in place.
    QTemporaryDir backup(QDir(userDir).filePath(u".winshun-update-XXXXXX"_s));
    if (!backup.isValid())
        return fail(u"建不了插件更新的临时文件夹"_s);
    const QString aside = backup.filePath(u"plugin"_s);
    const bool had = QFileInfo::exists(target);
    if (had && !QDir().rename(target, aside))
        return fail(u"换不掉原来的 %1：它的程序可能还在运行"_s.arg(QDir::toNativeSeparators(target)));
    QString why;
    if (candidate.archive && QDir().rename(candidate.dir, target))
        why.clear(); // moved in one go
    else
        why = copyTree(candidate.dir, target);
    if (!why.isEmpty()) {
        QDir(target).removeRecursively();
        if (had && !QDir().rename(aside, target)) {
            backup.setAutoRemove(false); // retain the old version if rollback fails
            return fail(why + u"；原来的插件保留在 %1"_s.arg(QDir::toNativeSeparators(aside)));
        }
        return fail(why);
    }
    return QDir::toNativeSeparators(target);
}

void discard(Candidate& candidate)
{
    if (candidate.archive && !candidate.dir.isEmpty() && QFileInfo::exists(candidate.dir))
        QDir(candidate.dir).removeRecursively();
    candidate.dir.clear();
}

QString remove(const QString& userDir, const QString& dataRoot, const QString& id, bool data)
{
    if (!validPluginId(id))
        return u"不是插件 id"_s;
    const QString dataDir = QDir(dataRoot.isEmpty() ? userDir : dataRoot).filePath(id + u".data"_s);
    if (data && QFileInfo::exists(dataDir + u"/plugin.json"_s))
        return u"数据文件夹 %1 中有另一个插件，为保留它，没有删除"_s.arg(QDir::toNativeSeparators(dataDir));
    const QString folder = QDir(userDir).filePath(id);
    if (QFileInfo::exists(folder) && !QDir(folder).removeRecursively())
        return u"删不掉 %1：它的程序可能还在运行"_s.arg(QDir::toNativeSeparators(folder));
    if (data) {
        if (QFileInfo::exists(dataDir) && !QDir(dataDir).removeRecursively())
            return u"插件已删除，它的数据 %1 删不掉"_s.arg(QDir::toNativeSeparators(dataDir));
    }
    return {};
}

QStringList unconfirmed(const Manifest& manifest, const QStringList& confirmed)
{
    QStringList out;
    for (const QString& name : manifest.permissions.names()) {
        if (!confirmed.contains(name, Qt::CaseInsensitive))
            out.push_back(name);
    }
    return out;
}

} // namespace ws::plugin::install
