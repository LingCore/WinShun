// Making delta packages (UpdateDelta.h): for tools/delta (WinShunDelta.exe,
// run by scripts/release.ps1) and the tests. Win顺 itself never links this:
// it only applies packages, with zstd's decompressor alone.

#include "UpdateDelta.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <miniz.h>
#include <zstd.h>

#include <algorithm>
#include <bit>

using namespace Qt::StringLiterals;

namespace ws::update::delta {

namespace {

QByteArray sha256(QByteArrayView bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

// The files of a program folder, '/' separated and relative, without the
// user's "data" (a portable copy's).
QStringList filesOf(const QString& folder)
{
    QStringList list;
    const QDir root(folder);
    QDirIterator it(folder, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString relative = root.relativeFilePath(it.next());
        if (relative.section(u'/', 0, 0).compare(u"data"_s, Qt::CaseInsensitive) != 0)
            list.append(relative);
    }
    std::sort(list.begin(), list.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    return list;
}

bool readFile(const QString& path, QByteArray* bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    *bytes = file.readAll();
    return bytes->size() == file.size();
}

// One zstd frame of `data`, against `prefix` when given (zstd's
// --patch-from): a window over both, long-distance matching, level 19.
QByteArray zstd(const QByteArray& data, const QByteArray* prefix, QString* why)
{
    ZSTD_CCtx* context = ZSTD_createCCtx();
    if (!context) {
        *why = u"out of memory"_s;
        return {};
    }
    const quint64 span = static_cast<quint64>(data.size()) + (prefix ? static_cast<quint64>(prefix->size()) : 0);
    const int windowLog = std::clamp(static_cast<int>(std::bit_width(std::max<quint64>(span, 1))), 20, 30);
    ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, 19);
    ZSTD_CCtx_setParameter(context, ZSTD_c_windowLog, windowLog);
    ZSTD_CCtx_setParameter(context, ZSTD_c_enableLongDistanceMatching, 1);
    ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, 1);
    ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 1);
    size_t result = 0;
    if (prefix)
        result = ZSTD_CCtx_refPrefix(context, prefix->constData(), static_cast<size_t>(prefix->size()));
    QByteArray out;
    if (!ZSTD_isError(result)) {
        out.resize(static_cast<qsizetype>(ZSTD_compressBound(static_cast<size_t>(data.size()))));
        result = ZSTD_compress2(context, out.data(), static_cast<size_t>(out.size()), data.constData(), static_cast<size_t>(data.size()));
    }
    ZSTD_freeCCtx(context);
    if (ZSTD_isError(result)) {
        *why = QString::fromLatin1(ZSTD_getErrorName(result));
        return {};
    }
    out.resize(static_cast<qsizetype>(result));
    return out;
}

} // namespace

QString make(const QString& oldDir, const QString& newDir, const QString& from, const QString& to, const QString& setup,
    const QString& outZip, MadeStats* stats)
{
    MadeStats made;
    mz_zip_archive archive {};
    if (!mz_zip_writer_init_heap(&archive, 0, 64 << 20))
        return u"cannot start the zip"_s;
    struct End {
        mz_zip_archive* archive;
        bool done = false;
        ~End()
        {
            if (!done)
                mz_zip_writer_end(archive);
        }
    } end {&archive};
    // Already compressed (zstd frames, the LZMA Setup): stored as they are.
    const auto add = [&archive](const QByteArray& name, const QByteArray& bytes, mz_uint level) {
        return mz_zip_writer_add_mem(&archive, name.constData(), bytes.constData(), static_cast<size_t>(bytes.size()), level);
    };

    QJsonArray files;
    int dataIndex = 0;
    const QDir oldRoot(oldDir);
    const QDir newRoot(newDir);
    const QStringList names = filesOf(newDir);
    if (names.isEmpty())
        return u"no files in %1"_s.arg(newDir);
    for (const QString& name : names) {
        QByteArray bytes;
        if (!readFile(newRoot.filePath(name), &bytes))
            return u"cannot read %1"_s.arg(name);
        QJsonObject entry {{u"path"_s, name}, {u"size"_s, bytes.size()}, {u"sha256"_s, QString::fromLatin1(sha256(bytes))}};
        QByteArray old;
        const bool hasOld = QFileInfo(oldRoot.filePath(name)).isFile() && readFile(oldRoot.filePath(name), &old);
        if (hasOld && old == bytes) {
            entry.insert(u"how"_s, u"same"_s);
            ++made.same;
        } else {
            QString why;
            QByteArray frame = zstd(bytes, nullptr, &why);
            if (!why.isEmpty())
                return u"%1: %2"_s.arg(name, why);
            entry.insert(u"how"_s, u"new"_s);
            if (hasOld) {
                const QByteArray patch = zstd(bytes, &old, &why);
                if (!why.isEmpty())
                    return u"%1: %2"_s.arg(name, why);
                if (patch.size() < frame.size()) {
                    frame = patch;
                    entry.insert(u"how"_s, u"patch"_s);
                    entry.insert(u"base"_s, QString::fromLatin1(sha256(old)));
                }
            }
            const QString data = u"d/%1"_s.arg(dataIndex++);
            entry.insert(u"data"_s, data);
            if (!add(data.toLatin1(), frame, MZ_NO_COMPRESSION))
                return u"cannot add %1"_s.arg(name);
            if (entry.value(u"how"_s).toString() == u"patch"_s)
                ++made.patched;
            else
                ++made.added;
        }
        files.append(entry);
    }

    QJsonObject manifest {{u"schema"_s, kSchema}, {u"from"_s, from}, {u"to"_s, to}, {u"files"_s, files}};
    if (!setup.isEmpty()) {
        QByteArray bytes;
        if (!readFile(setup, &bytes))
            return u"cannot read %1"_s.arg(setup);
        manifest.insert(u"setup"_s, QJsonObject {{u"size"_s, bytes.size()}, {u"sha256"_s, QString::fromLatin1(sha256(bytes))}});
        if (!add(QByteArray(kSetupEntry), bytes, MZ_NO_COMPRESSION))
            return u"cannot add the Setup"_s;
    }
    if (!add(QByteArray(kManifestEntry), QJsonDocument(manifest).toJson(QJsonDocument::Compact), MZ_BEST_COMPRESSION))
        return u"cannot add the manifest"_s;

    void* buffer = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&archive, &buffer, &size))
        return u"cannot finish the zip"_s;
    const QByteArray zip(static_cast<const char*>(buffer), static_cast<qsizetype>(size));
    mz_free(buffer); // the archive gave it up
    mz_zip_writer_end(&archive);
    end.done = true;
    QFile out(outZip);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(zip) != zip.size())
        return u"cannot write %1"_s.arg(outZip);
    made.bytes = zip.size();
    if (stats)
        *stats = made;
    return {};
}

} // namespace ws::update::delta
