#include "UpdateDelta.h"

#include "UpdatePackage.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <miniz.h>
#include <zstd.h>

#include <cstring>

using namespace Qt::StringLiterals;

namespace ws::update::delta {

namespace {

constexpr int kMaxFiles = 5000;
constexpr qint64 kMaxFileBytes = 512LL << 20;
constexpr qint64 kMaxTotalBytes = 1LL << 30;
constexpr qint64 kMaxManifestBytes = 4 << 20;
constexpr qint64 kMaxPackageBytes = 400LL << 20; // as Updater's kMaxPackageBytes

const QRegularExpression& versionPattern()
{
    static const QRegularExpression re(u"^\\d{1,4}\\.\\d{1,4}\\.\\d{1,4}$"_s);
    return re;
}

const QRegularExpression& digestPattern()
{
    static const QRegularExpression re(u"^[0-9a-f]{64}$"_s);
    return re;
}

QString packageName(const QString& to, const QString& from)
{
    return u"WinShun-%1-x64-delta-from-%2.zip"_s.arg(to, from);
}

// `url` with its last path segment `from` swapped for `to`; empty when it
// does not end so.
QString swapName(const QString& url, const QString& from, const QString& to)
{
    const QString tail = u'/' + from;
    if (!url.endsWith(tail))
        return {};
    return url.left(url.size() - from.size()) + to;
}

QByteArray sha256(QByteArrayView bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

QByteArray readAll(const QString& path, qint64 limit, QString* why)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *why = u"cannot read %1"_s.arg(QDir::toNativeSeparators(path));
        return {};
    }
    if (file.size() > limit) {
        *why = u"too large: %1"_s.arg(QDir::toNativeSeparators(path));
        return {};
    }
    QByteArray bytes = file.readAll();
    if (bytes.size() != file.size()) {
        *why = u"cannot read %1"_s.arg(QDir::toNativeSeparators(path));
        return {};
    }
    return bytes;
}

// One zstd frame, of exactly `expected` bytes, patched against `prefix`
// when there is one.
QByteArray unzstd(QByteArrayView frame, const QByteArray* prefix, qint64 expected, QString* why)
{
    if (ZSTD_findFrameCompressedSize(frame.data(), static_cast<size_t>(frame.size())) != static_cast<size_t>(frame.size())) {
        *why = u"not one zstd frame"_s;
        return {};
    }
    const unsigned long long size = ZSTD_getFrameContentSize(frame.data(), static_cast<size_t>(frame.size()));
    if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR || size != static_cast<unsigned long long>(expected)) {
        *why = u"frame size differs"_s;
        return {};
    }
    QByteArray out(static_cast<qsizetype>(expected), Qt::Uninitialized);
    ZSTD_DCtx* context = ZSTD_createDCtx();
    if (!context) {
        *why = u"out of memory"_s;
        return {};
    }
    // Patches against the largest files need a window that covers the old
    // file and the new one (UpdateDeltaMake.cpp).
    ZSTD_DCtx_setParameter(context, ZSTD_d_windowLogMax, 31);
    size_t result = 0;
    if (prefix)
        result = ZSTD_DCtx_refPrefix(context, prefix->constData(), static_cast<size_t>(prefix->size()));
    if (!ZSTD_isError(result))
        result = ZSTD_decompressDCtx(context, out.data(), static_cast<size_t>(out.size()), frame.data(), static_cast<size_t>(frame.size()));
    ZSTD_freeDCtx(context);
    if (ZSTD_isError(result) || result != static_cast<size_t>(expected)) {
        *why = ZSTD_isError(result) ? QString::fromLatin1(ZSTD_getErrorName(result)) : u"short output"_s;
        return {};
    }
    return out;
}

struct File {
    QString path; // '/' separated, relative
    qint64 size = 0;
    QByteArray sha256;
    QString how;
    QByteArray base;
    QString data;
};

bool writeFile(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return false;
    const bool ok = file.write(bytes) == bytes.size();
    file.close();
    return ok && file.error() == QFileDevice::NoError;
}

// Held from now on (when asked), and its bytes as they are on disk checked.
bool holdAndCheck(const QString& path, const QByteArray& digest, std::vector<std::unique_ptr<HeldFile>>* hold)
{
    if (!hold)
        return true;
    auto held = std::make_unique<HeldFile>();
    if (!held->open(path) || held->sha256() != digest)
        return false;
    hold->push_back(std::move(held));
    return true;
}

} // namespace

QString notesLine(const QList<Offered>& offered)
{
    QStringList parts;
    for (const Offered& o : offered)
        parts.append(u"%1:%2:%3:%4"_s.arg(o.from, o.name).arg(o.size).arg(o.sha256));
    return u"<!-- winshun-delta %1 %2 -->"_s.arg(kSchema).arg(parts.join(u' '));
}

QList<Offered> parseNotesLine(const QString& notes)
{
    static const QRegularExpression line(u"<!-- winshun-delta (\\d+) ([^\\r\\n]*?) -->"_s);
    const QRegularExpressionMatch match = line.match(notes);
    if (!match.hasMatch() || match.captured(1).toInt() != kSchema)
        return {};
    QList<Offered> list;
    for (const QString& part : match.captured(2).split(u' ', Qt::SkipEmptyParts)) {
        const QStringList fields = part.split(u':');
        if (fields.size() != 4)
            continue;
        Offered o;
        o.from = fields[0];
        o.name = fields[1];
        bool ok = false;
        o.size = fields[2].toLongLong(&ok);
        o.sha256 = fields[3];
        if (!ok || o.size <= 0 || o.size > kMaxPackageBytes || !versionPattern().match(o.from).hasMatch()
            || !digestPattern().match(o.sha256).hasMatch())
            continue;
        list.append(o);
    }
    return list;
}

std::optional<release::Asset> offer(const release::Info& info, const release::Asset& full, const QString& current)
{
    const QString from = release::normalized(current);
    if (!versionPattern().match(from).hasMatch() || full.name.isEmpty())
        return std::nullopt;
    for (const Offered& o : parseNotesLine(info.notes)) {
        // Only the name a release gives it: nothing in the notes becomes a
        // path or another URL.
        if (!release::sameVersion(o.from, from) || o.name != packageName(info.version, o.from))
            continue;
        release::Asset asset;
        asset.name = o.name;
        asset.size = o.size;
        asset.sha256 = o.sha256;
        asset.url = swapName(full.url, full.name, o.name);
        if (asset.url.isEmpty())
            return std::nullopt;
        for (const QString& mirror : full.mirrors)
            if (const QString url = swapName(mirror, full.name, o.name); !url.isEmpty())
                asset.mirrors.append(url);
        return asset;
    }
    return std::nullopt;
}

QString apply(const QByteArray& package, const QString& base, const QString& out, const ApplyOptions& options)
{
    if (QFileInfo::exists(out))
        return u"the output folder already exists"_s;
    mz_zip_archive archive {};
    if (!mz_zip_reader_init_mem(&archive, package.constData(), static_cast<size_t>(package.size()), 0))
        return u"not a zip archive"_s;
    struct End {
        mz_zip_archive* archive;
        ~End() { mz_zip_reader_end(archive); }
    } end {&archive};

    // An entry's bytes, at most `limit` of them.
    const auto entry = [&archive](const char* name, qint64 limit, QString* why) -> QByteArray {
        const int index = mz_zip_reader_locate_file(&archive, name, nullptr, 0);
        mz_zip_archive_file_stat stat;
        if (index < 0 || !mz_zip_reader_file_stat(&archive, static_cast<mz_uint>(index), &stat)) {
            *why = u"no entry %1"_s.arg(QLatin1StringView(name));
            return {};
        }
        if (stat.m_is_encrypted || !stat.m_is_supported || static_cast<qint64>(stat.m_uncomp_size) > limit) {
            *why = u"entry %1 unusable"_s.arg(QLatin1StringView(name));
            return {};
        }
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&archive, static_cast<mz_uint>(index), &size, 0); // checks the CRC
        if (!data) {
            *why = u"entry %1 unreadable"_s.arg(QLatin1StringView(name));
            return {};
        }
        QByteArray bytes(static_cast<const char*>(data), static_cast<qsizetype>(size));
        mz_free(data);
        return bytes;
    };

    QString why;
    const QByteArray manifestBytes = entry(kManifestEntry, kMaxManifestBytes, &why);
    if (!why.isEmpty())
        return why;
    const QJsonObject manifest = QJsonDocument::fromJson(manifestBytes).object();
    if (manifest.value(u"schema"_s).toInt() != kSchema)
        return u"unknown manifest schema"_s;

    // Everything checked before anything is written.
    QList<File> files;
    QSet<QString> seen;
    qint64 total = 0;
    bool hasProgram = false;
    const QJsonArray list = manifest.value(u"files"_s).toArray();
    if (list.isEmpty() || list.size() > kMaxFiles)
        return u"%1 files"_s.arg(list.size());
    static const QRegularExpression dataName(u"^d/\\d{1,5}$"_s);
    for (const QJsonValue value : list) {
        const QJsonObject o = value.toObject();
        File f;
        f.path = o.value(u"path"_s).toString();
        f.size = o.value(u"size"_s).toInteger(-1);
        f.sha256 = o.value(u"sha256"_s).toString().toLatin1();
        f.how = o.value(u"how"_s).toString();
        f.base = o.value(u"base"_s).toString().toLatin1();
        f.data = o.value(u"data"_s).toString();
        if (!safeEntryName(f.path) || f.path.contains(u'\\') || f.path.endsWith(u'/'))
            return u"unsafe path: %1"_s.arg(f.path);
        if (f.path.section(u'/', 0, 0).compare(u"data"_s, Qt::CaseInsensitive) == 0)
            return u"the user's data folder is never written: %1"_s.arg(f.path);
        if (f.size < 0 || f.size > kMaxFileBytes || (total += f.size) > kMaxTotalBytes)
            return u"too large: %1"_s.arg(f.path);
        if (!digestPattern().match(QString::fromLatin1(f.sha256)).hasMatch())
            return u"no digest: %1"_s.arg(f.path);
        if (f.how == u"same"_s) {
            f.base = f.sha256;
        } else if (f.how == u"patch"_s || f.how == u"new"_s) {
            if (!dataName.match(f.data).hasMatch())
                return u"no data: %1"_s.arg(f.path);
            if (f.how == u"patch"_s && !digestPattern().match(QString::fromLatin1(f.base)).hasMatch())
                return u"no base digest: %1"_s.arg(f.path);
        } else {
            return u"unknown kind %1: %2"_s.arg(f.how, f.path);
        }
        const QString key = f.path.toLower();
        if (seen.contains(key))
            return u"duplicate path: %1"_s.arg(f.path);
        seen.insert(key);
        if (key == u"winshun.exe"_s)
            hasProgram = true;
        files.append(f);
    }
    if (!hasProgram)
        return u"no WinShun.exe in the package"_s;
    QSet<QString> skip;
    for (const QString& path : options.skip)
        skip.insert(QString(path).replace(u'\\', u'/').toLower());

    if (!QDir().mkpath(out))
        return u"cannot create %1"_s.arg(QDir::toNativeSeparators(out));
    const qsizetype held = options.hold ? static_cast<qsizetype>(options.hold->size()) : 0;
    const auto fail = [&](const QString& reason) {
        if (options.hold)
            options.hold->resize(static_cast<size_t>(held)); // closed before they are deleted
        QDir(out).removeRecursively();
        if (!options.setupTarget.isEmpty())
            QFile::remove(options.setupTarget);
        return reason;
    };
    const QDir baseDir(base);
    const QDir outDir(out);
    for (const File& f : std::as_const(files)) {
        if (options.cancel && *options.cancel)
            return fail(u"cancelled"_s);
        if (skip.contains(f.path.toLower()))
            continue;
        QByteArray old;
        if (f.how != u"new"_s) {
            old = readAll(baseDir.filePath(f.path), kMaxFileBytes, &why);
            if (!why.isEmpty())
                return fail(why);
            if (sha256(old) != f.base)
                return fail(u"changed here: %1"_s.arg(f.path));
        }
        QByteArray bytes;
        if (f.how == u"same"_s) {
            bytes = std::move(old);
        } else {
            const QByteArray frame = entry(f.data.toLatin1().constData(), kMaxFileBytes, &why);
            if (!why.isEmpty())
                return fail(why);
            bytes = unzstd(frame, f.how == u"patch"_s ? &old : nullptr, f.size, &why);
            if (!why.isEmpty())
                return fail(u"%1: %2"_s.arg(f.path, why));
        }
        if (bytes.size() != f.size || sha256(bytes) != f.sha256)
            return fail(u"built wrong: %1"_s.arg(f.path));
        const QString target = outDir.filePath(f.path);
        if (!writeFile(target, bytes))
            return fail(u"cannot write %1"_s.arg(f.path));
        if (!holdAndCheck(target, f.sha256, options.hold))
            return fail(u"changed after writing: %1"_s.arg(f.path));
    }

    if (!options.setupTarget.isEmpty()) {
        const QJsonObject setup = manifest.value(u"setup"_s).toObject();
        const qint64 size = setup.value(u"size"_s).toInteger(-1);
        const QByteArray digest = setup.value(u"sha256"_s).toString().toLatin1();
        if (size <= 0 || !digestPattern().match(QString::fromLatin1(digest)).hasMatch())
            return fail(u"no Setup in the package"_s);
        const QByteArray bytes = entry(kSetupEntry, kMaxFileBytes, &why);
        if (!why.isEmpty())
            return fail(why);
        if (bytes.size() != size || sha256(bytes) != digest)
            return fail(u"the Setup differs"_s);
        QFile::remove(options.setupTarget);
        if (!writeFile(options.setupTarget, bytes))
            return fail(u"cannot write the Setup"_s);
        if (!holdAndCheck(options.setupTarget, digest, options.hold))
            return fail(u"the Setup changed after writing"_s);
    }
    return {};
}

} // namespace ws::update::delta
