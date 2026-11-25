#include "ClipBackupFiles.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>

#include <miniz.h>

using namespace Qt::StringLiterals;

namespace ws::clipbackup {

namespace {

const QString kDatabase = u"clipboard.db"_s;
const QString kManifest = u"backup.json"_s;
constexpr const char16_t* kFolders[] {u"images", u"projects"};
constexpr qint64 kSpareBytes = 64ll << 20; // free space left after unpacking
constexpr int kRenameTries = 10; // the old Win顺's last files closing as it ends
constexpr int kRenameWaitMs = 300;

Started g_started;

QString tr(const char* text)
{
    return QCoreApplication::translate("ws::clipbackup", text);
}

QString cannotWrite(const QString& path)
{
    return tr("写不进 %1").arg(QDir::toNativeSeparators(path));
}

// A file of images\ or projects\ by its name there: the history's own are
// "<id>.png", "<id>.wsedit"; nothing with a path in it.
bool plainName(const QString& name)
{
    static const QRegularExpression plain(u"^[A-Za-z0-9_-]+\\.[A-Za-z0-9]+$"_s);
    return plain.match(name).hasMatch();
}

// An entry's place in the history folder ("clipboard.db", "images/12.png");
// empty for a name a backup does not have.
QString payloadName(const QString& name)
{
    if (name == kDatabase)
        return name;
    for (const char16_t* folder : kFolders) {
        const QString prefix = QString::fromUtf16(folder) + u'/';
        if (name.startsWith(prefix) && plainName(name.mid(prefix.size())))
            return name;
    }
    return {};
}

// Stored as they are: a picture is compressed already.
mz_uint levelFor(const QString& name)
{
    static const QRegularExpression packed(u"\\.(png|jpg|jpeg|gif|webp)$"_s, QRegularExpression::CaseInsensitiveOption);
    return packed.match(name).hasMatch() ? MZ_NO_COMPRESSION : MZ_DEFAULT_LEVEL;
}

size_t readDevice(void* opaque, mz_uint64 offset, void* buffer, size_t n)
{
    auto* device = static_cast<QIODevice*>(opaque);
    if (device->pos() != static_cast<qint64>(offset) && !device->seek(static_cast<qint64>(offset)))
        return 0;
    const qint64 read = device->read(static_cast<char*>(buffer), static_cast<qint64>(n));
    return read < 0 ? 0 : static_cast<size_t>(read);
}

size_t writeDevice(void* opaque, mz_uint64 offset, const void* buffer, size_t n)
{
    auto* device = static_cast<QIODevice*>(opaque);
    if (device->pos() != static_cast<qint64>(offset) && !device->seek(static_cast<qint64>(offset)))
        return 0;
    const qint64 written = device->write(static_cast<const char*>(buffer), static_cast<qint64>(n));
    return written < 0 ? 0 : static_cast<size_t>(written);
}

// A database connection of this call's own, closed and forgotten with it.
class Database {
public:
    explicit Database(const QString& path)
    {
        static std::atomic<int> counter {0};
        m_name = u"clip-backup-%1"_s.arg(++counter);
        QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, m_name);
        db.setDatabaseName(path);
        db.setConnectOptions(u"QSQLITE_BUSY_TIMEOUT=10000"_s);
        m_open = db.open();
    }
    ~Database()
    {
        {
            QSqlDatabase db = QSqlDatabase::database(m_name, false);
            if (db.isValid())
                db.close();
        }
        QSqlDatabase::removeDatabase(m_name);
    }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool isOpen() const { return m_open; }
    bool run(const QString& sql) const
    {
        QSqlQuery query(QSqlDatabase::database(m_name, false));
        return query.exec(sql);
    }
    // A count; -1 when the query fails.
    int count(const QString& sql) const
    {
        QSqlQuery query(QSqlDatabase::database(m_name, false));
        return query.exec(sql) && query.next() ? query.value(0).toInt() : -1;
    }

private:
    QString m_name;
    bool m_open = false;
};

// The history's entries in the database at `path`; -1 if it is not one.
int entriesIn(const QString& path)
{
    const Database db(path);
    if (!db.isOpen() || db.count(u"SELECT count(*) FROM groups"_s) < 0)
        return -1;
    return db.count(u"SELECT count(*) FROM clips"_s);
}

bool renamePatiently(const QString& from, const QString& to)
{
    for (int i = 0; i < kRenameTries; ++i) {
        if (QDir().rename(from, to))
            return true;
        QThread::msleep(kRenameWaitMs);
    }
    return false;
}

} // namespace

QString fileName(const QDateTime& when)
{
    return u"WinShun-clipboard-"_s + when.toString(u"yyyy-MM-dd-HHmmss"_s) + u".zip"_s;
}

Summary pack(const QString& clipFolder, const QString& zipPath, const QString& version, const QDateTime& when)
{
    Summary summary;
    const QDir folder(clipFolder);
    if (!folder.exists(kDatabase)) {
        summary.problem = tr("还没有剪贴板历史");
        return summary;
    }
    // The database as of now, in one file: SQLite's own copy, consistent
    // whatever is being written to the live one (and its log).
    QTemporaryDir temporary(clipFolder + u".backup-XXXXXX"_s);
    if (!temporary.isValid()) {
        summary.problem = cannotWrite(QFileInfo(clipFolder).path());
        return summary;
    }
    const QString snapshot = temporary.filePath(kDatabase);
    {
        const Database live(folder.filePath(kDatabase));
        QString quoted = QDir::toNativeSeparators(snapshot);
        quoted.replace(u'\'', u"''"_s);
        if (!live.isOpen() || !live.run(u"VACUUM INTO '%1'"_s.arg(quoted))) {
            summary.problem = tr("读不出剪贴板历史");
            return summary;
        }
    }
    summary.entries = entriesIn(snapshot);
    if (summary.entries < 0) {
        summary.problem = tr("读不出剪贴板历史");
        return summary;
    }

    QSaveFile out(zipPath);
    if (!out.open(QIODevice::WriteOnly)) {
        summary.problem = cannotWrite(zipPath);
        return summary;
    }
    mz_zip_archive zip {};
    zip.m_pWrite = writeDevice;
    zip.m_pIO_opaque = &out;
    if (!mz_zip_writer_init_v2(&zip, 0, 0)) {
        summary.problem = cannotWrite(zipPath);
        return summary;
    }
    // False only when the zip itself could not be written; a file that
    // cannot be opened is `missing`.
    const auto add = [&zip](const QString& name, const QString& path, bool* missing) {
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly)) {
            *missing = true;
            return true;
        }
        *missing = false;
        return mz_zip_writer_add_read_buf_callback(&zip, name.toUtf8().constData(), readDevice, &in,
                   static_cast<mz_uint64>(in.size()), nullptr, nullptr, 0, levelFor(name), nullptr, 0, nullptr, 0)
            == MZ_TRUE;
    };
    bool missing = false;
    bool ok = add(kDatabase, snapshot, &missing) && !missing;
    for (const char16_t* name : kFolders) {
        const QString sub = QString::fromUtf16(name);
        const QDir files(folder.filePath(sub));
        for (const QString& file : files.entryList(QDir::Files, QDir::Name)) {
            if (!ok)
                break;
            if (!plainName(file))
                continue; // not the history's
            ok = add(sub + u'/' + file, files.filePath(file), &missing);
            if (ok && !missing && sub == u"images")
                ++summary.pictures;
        }
    }
    const QByteArray manifest = QJsonDocument(QJsonObject {
                                                  {u"app"_s, u"WinShun"_s},
                                                  {u"kind"_s, u"clipboard"_s},
                                                  {u"format"_s, 1},
                                                  {u"version"_s, version},
                                                  {u"created"_s, when.toUTC().toString(Qt::ISODate)},
                                                  {u"entries"_s, summary.entries},
                                                  {u"pictures"_s, summary.pictures},
                                              })
                                    .toJson(QJsonDocument::Compact);
    ok = ok
        && mz_zip_writer_add_mem(&zip, "backup.json", manifest.constData(), static_cast<size_t>(manifest.size()),
            MZ_DEFAULT_LEVEL);
    ok = ok && mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    if (!ok || !out.commit()) {
        out.cancelWriting();
        summary.problem = cannotWrite(zipPath);
    }
    return summary;
}

Summary unpack(const QString& zipPath, const QString& folder)
{
    Summary summary;
    const auto fail = [&summary, &folder](const QString& why) {
        QDir(folder).removeRecursively();
        summary.problem = why;
        return summary;
    };
    QFile in(zipPath);
    if (!in.open(QIODevice::ReadOnly)) {
        summary.problem = tr("打不开 %1").arg(QDir::toNativeSeparators(zipPath));
        return summary;
    }
    mz_zip_archive zip {};
    zip.m_pRead = readDevice;
    zip.m_pIO_opaque = &in;
    if (!mz_zip_reader_init(&zip, static_cast<mz_uint64>(in.size()), 0)) {
        summary.problem = tr("这不是 Win顺 的剪贴板备份");
        return summary;
    }
    const struct Closer {
        mz_zip_archive* zip;
        ~Closer() { mz_zip_reader_end(zip); }
    } closer {&zip};

    QJsonObject manifest;
    if (const int at = mz_zip_reader_locate_file(&zip, "backup.json", nullptr, 0); at >= 0) {
        mz_zip_archive_file_stat stat;
        size_t size = 0;
        if (mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(at), &stat) && stat.m_uncomp_size <= 64 * 1024) {
            if (void* data = mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(at), &size, 0)) {
                manifest = QJsonDocument::fromJson(QByteArray(static_cast<const char*>(data), static_cast<qsizetype>(size)))
                               .object();
                mz_free(data);
            }
        }
    }
    if (manifest.value(u"app"_s).toString() != u"WinShun")
        return fail(tr("这不是 Win顺 的剪贴板备份"));
    if (manifest.value(u"kind"_s).toString() != u"clipboard")
        return fail(tr("这是设置的备份，不是剪贴板历史的；请到 设置 › 常规 里恢复"));
    if (manifest.value(u"format"_s) != QJsonValue(1))
        return fail(tr("不支持这个备份格式，请更新 Win顺 后再试"));

    struct Entry {
        mz_uint index;
        QString name;
    };
    QList<Entry> entries;
    QSet<QString> names;
    quint64 total = 0;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat))
            return fail(tr("备份文件已损坏"));
        if (mz_zip_reader_is_file_a_directory(&zip, i))
            continue;
        const QString name = QString::fromUtf8(stat.m_filename);
        if (name == kManifest)
            continue;
        if (payloadName(name).isEmpty() || names.contains(name.toCaseFolded()))
            return fail(tr("备份文件已损坏"));
        names.insert(name.toCaseFolded());
        total += stat.m_uncomp_size;
        entries.append({i, name});
    }
    if (!names.contains(kDatabase))
        return fail(tr("备份文件已损坏"));

    QDir(folder).removeRecursively();
    bool made = QDir().mkpath(folder);
    for (const char16_t* name : kFolders)
        made = made && QDir(folder).mkpath(QString::fromUtf16(name));
    if (!made)
        return fail(cannotWrite(folder));
    if (const QStorageInfo storage(folder);
        storage.isValid() && static_cast<quint64>(storage.bytesAvailable()) < total + static_cast<quint64>(kSpareBytes))
        return fail(tr("磁盘空间不够，还原需要 %1 MB").arg((total + static_cast<quint64>(kSpareBytes)) >> 20));
    for (const Entry& e : entries) {
        QFile out(folder + u'/' + e.name);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail(cannotWrite(folder));
        // Checked against the entry's checksum as it is read.
        const bool extracted = mz_zip_reader_extract_to_callback(&zip, e.index, writeDevice, &out, 0);
        const bool written = out.flush();
        out.close(); // before fail(): an open file keeps its folder
        if (!extracted)
            return fail(tr("备份文件已损坏"));
        if (!written)
            return fail(cannotWrite(folder));
        if (e.name.startsWith(u"images/"_s))
            ++summary.pictures;
    }
    summary.entries = entriesIn(folder + u'/' + kDatabase);
    if (summary.entries < 0)
        return fail(tr("备份文件已损坏"));
    return summary;
}

QString apply(const QString& staged, const QString& clipFolder)
{
    const QString old = clipFolder + u".replaced"_s;
    if (QFileInfo::exists(old) && !QDir(old).removeRecursively())
        return cannotWrite(old);
    const bool had = QFileInfo::exists(clipFolder);
    if (had && !renamePatiently(clipFolder, old))
        return tr("剪贴板历史正被别的程序占用，没有还原；重启 Win顺 会再试一次");
    if (!QDir().rename(staged, clipFolder)) {
        if (had)
            QDir().rename(old, clipFolder);
        return cannotWrite(clipFolder);
    }
    QDir(old).removeRecursively();
    return {};
}

QString stagedFolder(const QString& dataFolder)
{
    return QDir(dataFolder).filePath(u"clipboard.restore"_s);
}

QString historyFolder(const QString& dataFolder)
{
    return QDir(dataFolder).filePath(u"clipboard"_s);
}

Started applyStaged(const QString& dataFolder)
{
    const QString staged = stagedFolder(dataFolder);
    QDir(staged + u".part"_s).removeRecursively(); // one cut short
    if (QFileInfo(staged).isDir()) {
        g_started.problem = apply(staged, historyFolder(dataFolder));
        g_started.what = g_started.problem.isEmpty() ? Start::Restored : Start::Failed;
    }
    return g_started;
}

Started started()
{
    return g_started;
}

} // namespace ws::clipbackup
