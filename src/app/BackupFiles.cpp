#include "BackupFiles.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>

#include <algorithm>

#include <miniz.h>

using namespace Qt::StringLiterals;

namespace ws::backupfiles {

namespace {

constexpr qint64 kMaxZipBytes = 512ll << 20;
const QString kPrefix = u"WinShun-"_s;
const QString kTimeFormat = u"yyyy-MM-dd-HHmmss"_s;
const QString kManifest = u"backup.json"_s;
const QString kSettingsName = u"WinShun.ini"_s;
constexpr const char16_t* kLocalSettings[] {
    u"Backup/Folder", u"Index/Folder", u"Index/Drives", u"Launcher/FolderMenuOpusBefore",
    u"Launcher/FolderMenuOpusMiddleBefore", u"Launcher/FolderMenuOpusSingleMiddleBefore", u"Capture/PrintScreenBefore"};

QString tr(const char* text)
{
    return QCoreApplication::translate("ws::backupfiles", text);
}

// The files of the data folder a backup holds, by their names in it.
constexpr const char16_t* kDataFiles[] {u"favorites.json", u"dialog-pins.txt"};
// Folders of it whose files are all held (but logs, big ones, and the
// plugins' "<id>.data" folders: their caches, such as the semantic index,
// are of this computer's files and rebuilt).
constexpr const char16_t* kDataFolders[] {u"icons/web/custom", u"plugins"};

struct Zip {
    mz_zip_archive archive {};
    bool reading = false;
    ~Zip()
    {
        if (reading)
            mz_zip_reader_end(&archive);
    }
};

// An entry's name, '/' separated; empty when it could leave the folder.
QString safeName(Zip& zip, mz_uint index)
{
    const mz_uint length = mz_zip_reader_get_filename(&zip.archive, index, nullptr, 0);
    if (length < 2 || length > 32768)
        return {};
    QByteArray raw(static_cast<qsizetype>(length), Qt::Uninitialized);
    if (mz_zip_reader_get_filename(&zip.archive, index, raw.data(), length) != length)
        return {};
    raw.chop(1);
    const QString name = QString::fromUtf8(raw);
    if (name.toUtf8() != raw || name.contains(QChar::Null) || name.contains(u'\\'))
        return {};
    static const QRegularExpression invalid(u"[<>:\"|?*\\x00-\\x1f]"_s);
    static const QRegularExpression device(
        u"^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\\.|$)"_s, QRegularExpression::CaseInsensitiveOption);
    const QStringList parts = name.split(u'/');
    if (std::any_of(parts.begin(), parts.end(), [](const QString& p) {
            return p.isEmpty() || p.endsWith(u'.') || p.endsWith(u' ') || invalid.match(p).hasMatch()
                || device.match(p).hasMatch();
        }))
        return {};
    return name;
}

QJsonObject manifestOf(Zip& zip)
{
    const int i = mz_zip_reader_locate_file(&zip.archive, "backup.json", nullptr, 0);
    if (i < 0)
        return {};
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip.archive, static_cast<mz_uint>(i), &stat) || stat.m_uncomp_size > 64 * 1024)
        return {};
    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(&zip.archive, static_cast<mz_uint>(i), &size, 0);
    if (!data)
        return {};
    const QJsonObject o
        = QJsonDocument::fromJson(QByteArray(static_cast<const char*>(data), static_cast<qsizetype>(size))).object();
    mz_free(data);
    return o.value(u"app"_s).toString() == u"WinShun" ? o : QJsonObject();
}

bool payloadName(const QString& name)
{
    if (name == kSettingsName)
        return true;
    if (!name.startsWith(u"data/"_s))
        return false;
    const QString relative = name.mid(5);
    for (const char16_t* file : kDataFiles) {
        if (relative == QString::fromUtf16(file))
            return true;
    }
    for (const char16_t* folder : kDataFolders) {
        if (relative.startsWith(QString::fromUtf16(folder) + u'/'))
            return !relative.section(u'/', 1, 1).endsWith(u".data"_s, Qt::CaseInsensitive)
                && !relative.endsWith(u".log"_s, Qt::CaseInsensitive);
    }
    return false;
}

void addHash(QCryptographicHash& hash, const QString& name, const QByteArray& bytes)
{
    hash.addData(name.toUtf8());
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(bytes);
    hash.addData(QByteArrayView("\0", 1));
}

// Only the selected INI is edited; credentials never leave Credential Manager.
bool localSettings(const QString& path, const QString& current = {})
{
    QVariantMap local;
    if (!current.isEmpty()) {
        QSettings existing(current, QSettings::IniFormat);
        existing.setFallbacksEnabled(false);
        for (const char16_t* name : kLocalSettings) {
            const QString key = QString::fromUtf16(name);
            if (existing.contains(key))
                local.insert(key, existing.value(key));
        }
        if (existing.status() != QSettings::NoError)
            return false;
    }
    QSettings settings(path, QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    if (settings.allKeys().isEmpty() || settings.status() != QSettings::NoError)
        return false;
    for (const char16_t* name : kLocalSettings) {
        const QString key = QString::fromUtf16(name);
        settings.remove(key);
        if (local.contains(key))
            settings.setValue(key, local.value(key));
    }
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool copyAtomic(const QString& from, const QString& to)
{
    QFile input(from);
    QSaveFile output(to);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
        return false;
    while (!input.atEnd()) {
        const QByteArray bytes = input.read(64 * 1024);
        if (input.error() != QFileDevice::NoError || output.write(bytes) != bytes.size())
            return false;
    }
    return output.commit();
}

// A pre-existing link inside the destination must not redirect an import.
bool hasLinkedParent(const QString& root, const QString& relative)
{
    QString path = root;
    for (const QString& part : relative.split(u'/')) {
        path = QDir(path).filePath(part);
        const QFileInfo info(path);
        if (info.isSymbolicLink() || info.isJunction())
            return true;
    }
    return false;
}

} // namespace

QList<File> collect(const QString& settingsFile, const QString& dataFolder)
{
    QList<File> files;
    if (QFileInfo(settingsFile).isFile())
        files.append({kSettingsName, settingsFile});
    const QDir data(dataFolder);
    for (const char16_t* name : kDataFiles) {
        const QString path = data.filePath(QString::fromUtf16(name));
        if (QFileInfo(path).isFile())
            files.append({u"data/"_s + QString::fromUtf16(name), path});
    }
    for (const char16_t* folder : kDataFolders) {
        const QString root = data.filePath(QString::fromUtf16(folder));
        QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QFileInfo info = it.nextFileInfo();
            if (info.isSymLink() || info.size() > kMaxFileBytes
                || info.suffix().compare(u"log"_s, Qt::CaseInsensitive) == 0)
                continue;
            const QString name = data.relativeFilePath(info.filePath());
            if (name.section(u'/', 1, 1).endsWith(u".data"_s, Qt::CaseInsensitive) && name.startsWith(u"plugins/"_s))
                continue;
            files.append({u"data/"_s + name, info.filePath()});
        }
    }
    std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.name < b.name; });
    return files;
}

Packed pack(const QList<File>& files, const QString& version, const QDateTime& when, bool forTransfer)
{
    if (std::none_of(files.begin(), files.end(), [](const File& file) { return file.name == kSettingsName; }))
        return {};
    Zip zip;
    if (!mz_zip_writer_init_heap(&zip.archive, 0, 64 * 1024))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    bool ok = true;
    qint64 total = 0;
    for (const File& f : files) {
        QFile file(f.path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxZipBytes - total) {
            ok = false;
            break;
        }
        QByteArray bytes = file.readAll();
        if (file.error() != QFileDevice::NoError) {
            ok = false;
            break;
        }
        total += bytes.size();
        if (forTransfer && f.name == kSettingsName) {
            QTemporaryDir temporary;
            QFile ini(temporary.filePath(kSettingsName));
            if (!temporary.isValid() || !ini.open(QIODevice::WriteOnly) || ini.write(bytes) != bytes.size()
                || !ini.flush()) {
                ok = false;
                break;
            }
            ini.close();
            if (!localSettings(ini.fileName()) || !ini.open(QIODevice::ReadOnly)) {
                ok = false;
                break;
            }
            bytes = ini.readAll();
        }
        const QByteArray name = f.name.toUtf8();
        addHash(hash, f.name, bytes);
        if (!mz_zip_writer_add_mem(&zip.archive, name.constData(), bytes.constData(), static_cast<size_t>(bytes.size()),
                MZ_DEFAULT_COMPRESSION)) {
            ok = false;
            break;
        }
    }
    Packed packed;
    packed.hash = QString::fromLatin1(hash.result().toHex());
    const QByteArray manifest = QJsonDocument(QJsonObject {
                                                  {u"app"_s, u"WinShun"_s},
                                                  {u"format"_s, 1},
                                                  {u"version"_s, version},
                                                  {u"created"_s, when.toString(Qt::ISODate)},
                                                  {u"hash"_s, packed.hash},
                                                  {u"portable"_s, forTransfer},
                                              })
                                    .toJson();
    ok = ok
        && mz_zip_writer_add_mem(&zip.archive, "backup.json", manifest.constData(),
            static_cast<size_t>(manifest.size()), MZ_DEFAULT_COMPRESSION);
    void* buffer = nullptr;
    size_t size = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&zip.archive, &buffer, &size);
    mz_zip_writer_end(&zip.archive);
    if (!ok) {
        if (buffer)
            mz_free(buffer);
        return {};
    }
    packed.zip = QByteArray(static_cast<const char*>(buffer), static_cast<qsizetype>(size));
    mz_free(buffer);
    return packed;
}

QString fileName(const QDateTime& when)
{
    return kPrefix + when.toString(kTimeFormat) + u".zip"_s;
}

QList<Found> list(const QString& folder)
{
    QList<Found> found;
    const QDir dir(folder);
    for (const QFileInfo& info : dir.entryInfoList({kPrefix + u"*.zip"_s}, QDir::Files)) {
        const QString stamp = info.completeBaseName().mid(kPrefix.size());
        const QDateTime when = QDateTime::fromString(stamp, kTimeFormat);
        if (when.isValid())
            found.append({info.filePath(), when});
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.when > b.when; });
    return found;
}

QString hashOf(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxZipBytes)
        return {};
    const QByteArray bytes = file.readAll();
    Zip zip;
    zip.reading = mz_zip_reader_init_mem(&zip.archive, bytes.constData(), static_cast<size_t>(bytes.size()), 0);
    return zip.reading ? manifestOf(zip).value(u"hash"_s).toString() : QString();
}

QString write(const QString& folder, const Packed& packed, const QDateTime& when)
{
    if (packed.zip.isEmpty())
        return tr("有文件读不出来");
    if (!QDir().mkpath(folder))
        return tr("建不了文件夹 %1").arg(QDir::toNativeSeparators(folder));
    const QDir dir(folder);
    QDateTime stamp = when;
    while (QFileInfo::exists(dir.filePath(fileName(stamp))))
        stamp = stamp.addSecs(1);
    const QString target = dir.filePath(fileName(stamp));
    const QString problem = exportTo(target, packed);
    if (!problem.isEmpty())
        return problem;
    // Only the newest kept; and no ".part" left over by one cut short.
    const QList<Found> all = list(folder);
    for (qsizetype i = kKept; i < all.size(); ++i)
        QFile::remove(all[i].path);
    for (const QFileInfo& info : dir.entryInfoList({kPrefix + u"*.zip.part"_s}, QDir::Files))
        QFile::remove(info.filePath());
    return {};
}

QString exportTo(const QString& path, const Packed& packed)
{
    if (packed.zip.isEmpty())
        return tr("有文件读不出来");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(packed.zip) != packed.zip.size() || !file.commit())
        return tr("写不进 %1").arg(QDir::toNativeSeparators(path));
    return {};
}

QString unpack(const QString& path, const QString& folder)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return tr("打不开 %1").arg(QDir::toNativeSeparators(path));
    if (file.size() > kMaxZipBytes)
        return tr("这不是 Win顺 的备份");
    const QByteArray bytes = file.readAll();
    file.close();
    Zip zip;
    zip.reading = mz_zip_reader_init_mem(&zip.archive, bytes.constData(), static_cast<size_t>(bytes.size()), 0);
    const QJsonObject manifest = zip.reading ? manifestOf(zip) : QJsonObject();
    if (manifest.isEmpty())
        return tr("这不是 Win顺 的备份");
    if (manifest.value(u"format"_s) != QJsonValue(1))
        return tr("不支持这个备份格式，请更新 Win顺 后再试");
    static const QRegularExpression digest(u"^[0-9a-f]{64}$"_s);
    const QString expected = manifest.value(u"hash"_s).toString();
    if (!digest.match(expected).hasMatch())
        return tr("备份文件已损坏");

    struct Entry {
        mz_uint index;
        QString name;
    };
    QList<Entry> entries;
    QSet<QString> names;
    quint64 total = 0;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip.archive); ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip.archive, i, &stat))
            return tr("备份文件已损坏");
        if (mz_zip_reader_is_file_a_directory(&zip.archive, i))
            continue;
        const QString name = safeName(zip, i);
        if (name.isEmpty() || names.contains(name.toCaseFolded()))
            return tr("备份文件已损坏");
        names.insert(name.toCaseFolded());
        if (name == kManifest)
            continue;
        if (!payloadName(name) || stat.m_uncomp_size > static_cast<quint64>(kMaxZipBytes) - total
            || (name.startsWith(u"data/plugins/"_s) && stat.m_uncomp_size > static_cast<quint64>(kMaxFileBytes)))
            return tr("备份文件已损坏");
        total += stat.m_uncomp_size;
        entries.append({i, name});
    }
    if (!names.contains(kSettingsName.toCaseFolded()))
        return tr("备份文件已损坏");
    QDir(folder).removeRecursively();
    if (!QDir().mkpath(folder))
        return tr("建不了文件夹 %1").arg(QDir::toNativeSeparators(folder));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const Entry& e : entries) {
        const QString target = folder + u'/' + e.name;
        if (!QDir().mkpath(QFileInfo(target).path()))
            return tr("写不进 %1").arg(QDir::toNativeSeparators(folder));
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip.archive, e.index, &size, 0);
        if (!data)
            return tr("备份文件已损坏");
        const QByteArray content(static_cast<const char*>(data), static_cast<qsizetype>(size));
        mz_free(data);
        addHash(hash, e.name, content);
        QFile out(target);
        const bool written = out.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && out.write(content) == content.size() && out.flush();
        if (!written)
            return tr("写不进 %1").arg(QDir::toNativeSeparators(folder));
    }
    if (QString::fromLatin1(hash.result().toHex()) != expected)
        return tr("备份文件已损坏");
    QSettings settings(QDir(folder).filePath(kSettingsName), QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    if (settings.allKeys().isEmpty() || settings.status() != QSettings::NoError)
        return tr("备份文件已损坏");
    QFile metadata(QDir(folder).filePath(kManifest));
    const QByteArray json = QJsonDocument(manifest).toJson();
    if (!metadata.open(QIODevice::WriteOnly) || metadata.write(json) != json.size() || !metadata.flush())
        return tr("写不进 %1").arg(QDir::toNativeSeparators(folder));
    return {};
}

QString apply(const QString& staged, const QString& settingsFile, const QString& dataFolder)
{
    const QDir dir(staged);
    const QString ini = dir.filePath(kSettingsName);
    if (!QFileInfo(ini).isFile())
        return tr("备份文件已损坏");
    QFile metadata(dir.filePath(kManifest));
    if (metadata.open(QIODevice::ReadOnly)
        && QJsonDocument::fromJson(metadata.readAll()).object().value(u"portable"_s).toBool()
        && !localSettings(ini, settingsFile))
        return tr("写不进 %1").arg(QDir::toNativeSeparators(staged));

    struct Change {
        QString from;
        QString to;
        QString before;
        bool existed;
    };
    QTemporaryDir previous(dir.filePath(u"previous-XXXXXX"_s));
    if (!previous.isValid())
        return tr("写不进 %1").arg(QDir::toNativeSeparators(staged));
    QList<Change> changes;
    const QList<File> files = collect(ini, dir.filePath(u"data"_s));
    for (const File& file : files) {
        const bool isSettings = file.name == kSettingsName;
        const QString relative = isSettings ? QFileInfo(settingsFile).fileName() : file.name.mid(5);
        const QString root = isSettings ? QFileInfo(settingsFile).absolutePath() : dataFolder;
        const QString target = QDir(root).filePath(relative);
        const QString before = previous.filePath(file.name);
        const bool existed = QFileInfo::exists(target);
        if (hasLinkedParent(root, relative) || QFileInfo(target).isDir() || !QDir().mkpath(QFileInfo(target).path())
            || (existed && (!QDir().mkpath(QFileInfo(before).path()) || !QFile::copy(target, before))))
            return tr("写不进 %1").arg(QDir::toNativeSeparators(target));
        changes.append({file.path, target, before, existed});
    }
    for (qsizetype i = 0; i < changes.size(); ++i) {
        if (copyAtomic(changes[i].from, changes[i].to))
            continue;
        bool rolledBack = true;
        for (qsizetype j = i; j-- > 0;) {
            const Change& change = changes[j];
            const bool restored = change.existed ? copyAtomic(change.before, change.to) : QFile::remove(change.to);
            rolledBack = restored && rolledBack;
        }
        if (!rolledBack) {
            previous.setAutoRemove(false);
            return tr("恢复未完成，原配置副本保留在 %1").arg(QDir::toNativeSeparators(previous.path()));
        }
        return tr("恢复失败，已保留原来的设置：%1").arg(QDir::toNativeSeparators(changes[i].to));
    }
    metadata.close();
    QDir(staged).removeRecursively();
    return {};
}

} // namespace ws::backupfiles
