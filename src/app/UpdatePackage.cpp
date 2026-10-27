#include "UpdatePackage.h"

#include "platform/Http.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringList>
#include <QUuid>

#include <miniz.h>
#include <windows.h>

#include <cstring>

using namespace Qt::StringLiterals;

namespace ws::update {

namespace {

constexpr auto kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
                               L"{C582F95C-EAF2-4AF2-961D-E35C4506652A}_is1"; // installer/WinShun.iss AppId
constexpr int kMaxEntries = 5000;
constexpr qint64 kMaxEntryBytes = 512LL << 20;
constexpr qint64 kMaxUnpackedBytes = 1LL << 30;

// The folder Setup installed into, from the uninstall key in that hive;
// empty when there is none.
QString installLocation(HKEY root, DWORD view)
{
    wchar_t value[MAX_PATH * 2] = {};
    DWORD bytes = sizeof(value);
    if (::RegGetValueW(root, kUninstallKey, L"InstallLocation", RRF_RT_REG_SZ | view, nullptr, value, &bytes)
        != ERROR_SUCCESS)
        return {};
    return QString::fromWCharArray(value);
}

bool sameFolder(const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty())
        return false;
    const auto clean = [](const QString& path) { return QDir::cleanPath(QDir::fromNativeSeparators(path)).toLower(); };
    return clean(a) == clean(b);
}

// UTF-8 when its flag says so, else the OEM code page (as plugin::unpack).
QString entryName(const mz_zip_archive_file_stat& stat)
{
    const std::size_t length = std::strlen(stat.m_filename);
    if (stat.m_bit_flag & (1u << 11))
        return QString::fromUtf8(stat.m_filename, static_cast<qsizetype>(length));
    std::wstring wide(length + 1, L'\0');
    const int n = ::MultiByteToWideChar(CP_OEMCP, 0, stat.m_filename, static_cast<int>(length), wide.data(),
        static_cast<int>(wide.size()));
    return n > 0 ? QString::fromWCharArray(wide.data(), n) : QString::fromLatin1(stat.m_filename, static_cast<qsizetype>(length));
}

bool reservedName(const QString& part)
{
    static const QStringList devices {u"CON"_s, u"PRN"_s, u"AUX"_s, u"NUL"_s, u"CONIN$"_s, u"CONOUT$"_s};
    const QString base = part.section(u'.', 0, 0).trimmed().toUpper();
    if (devices.contains(base))
        return true;
    return base.size() == 4 && (base.startsWith(u"COM"_s) || base.startsWith(u"LPT"_s)) && base[3].isDigit();
}

} // namespace

Kind installKind(const QString& appDir, bool portable)
{
    if (portable)
        return Kind::Portable;
    if (sameFolder(installLocation(HKEY_LOCAL_MACHINE, RRF_SUBKEY_WOW6464KEY), appDir)
        || sameFolder(installLocation(HKEY_LOCAL_MACHINE, RRF_SUBKEY_WOW6432KEY), appDir))
        return Kind::AllUsers;
    if (sameFolder(installLocation(HKEY_CURRENT_USER, 0), appDir))
        return Kind::CurrentUser;
    return Kind::None;
}

bool folderWritable(const QString& folder)
{
    const QString probe = QDir(folder).filePath(u".winshun-write-test-"_s + QUuid::createUuid().toString(QUuid::Id128));
    const HANDLE file = ::CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(probe).utf16()),
        GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    ::CloseHandle(file);
    return true;
}

QString fileVersion(const QString& path)
{
    const std::wstring file = QDir::toNativeSeparators(path).toStdWString();
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (size == 0)
        return {};
    QByteArray data(static_cast<qsizetype>(size), Qt::Uninitialized);
    if (!::GetFileVersionInfoW(file.c_str(), 0, size, data.data()))
        return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!::VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info
        || length < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != VS_FFI_SIGNATURE)
        return {};
    return u"%1.%2.%3.%4"_s.arg(HIWORD(info->dwFileVersionMS))
        .arg(LOWORD(info->dwFileVersionMS))
        .arg(HIWORD(info->dwFileVersionLS))
        .arg(LOWORD(info->dwFileVersionLS));
}

bool HeldFile::open(const QString& path)
{
    close();
    const HANDLE file = ::CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()),
        GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    m_handle = file;
    return true;
}

void HeldFile::close()
{
    if (m_handle)
        ::CloseHandle(m_handle);
    m_handle = nullptr;
}

qint64 HeldFile::size() const
{
    LARGE_INTEGER size {};
    return m_handle && ::GetFileSizeEx(m_handle, &size) ? size.QuadPart : -1;
}

QByteArray HeldFile::sha256(QByteArray* bytes, qint64 maxBytes) const
{
    if (!m_handle)
        return {};
    const qint64 total = size();
    if (total < 0 || (bytes && maxBytes > 0 && total > maxBytes))
        return {};
    LARGE_INTEGER start {};
    if (!::SetFilePointerEx(m_handle, start, nullptr, FILE_BEGIN))
        return {};
    if (bytes) {
        bytes->clear();
        bytes->reserve(total);
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(1 << 20, Qt::Uninitialized);
    qint64 read = 0;
    for (;;) {
        DWORD got = 0;
        if (!::ReadFile(m_handle, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr))
            return {};
        if (got == 0)
            break;
        hash.addData(QByteArrayView(buffer.constData(), got));
        if (bytes)
            bytes->append(buffer.constData(), got);
        read += got;
    }
    if (read != total)
        return {};
    return hash.result().toHex();
}

DownloadResult download(const release::Asset& asset, const QStringList& urls, const QString& target,
    const DownloadProgress& progress, int timeoutMs)
{
    DownloadResult result;
    const QString partial = target + u".partial"_s;
    if (asset.sha256.size() != 64 || asset.size <= 0) {
        result.failures.append(u"no size or digest"_s);
        return result;
    }
    for (qsizetype i = 0; i < urls.size(); ++i) {
        const QString& url = urls[i];
        const bool last = i == urls.size() - 1;
        QFile::remove(partial);
        QFile file(partial);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            result.failures.append(u"%1: cannot write %2"_s.arg(url, QDir::toNativeSeparators(partial)));
            return result; // the same for every source
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 received = 0;
        bool cancelled = false;
        bool slow = false;
        bool writeFailed = false;
        QElapsedTimer clock;
        clock.start();
        const http::Response response = http::download(url, {}, timeoutMs, asset.size, [&](const char* data, qsizetype size) {
            if (file.write(data, size) != size) {
                writeFailed = true;
                return false;
            }
            hash.addData(QByteArrayView(data, size));
            received += size;
            // A mirror that crawls (they may throttle): the next one, while
            // there is one; GitHub, last, is waited for.
            const qint64 ms = clock.elapsed();
            if (!last && ms > 30000 && received * 1000 / ms < 16 * 1024) {
                slow = true;
                return false;
            }
            if (progress && !progress(received, asset.size)) {
                cancelled = true;
                return false;
            }
            return true;
        });
        file.close();
        QString why;
        if (cancelled) {
            QFile::remove(partial);
            result.cancelled = true;
            return result;
        }
        if (writeFailed)
            why = u"cannot write the file"_s;
        else if (slow)
            why = u"too slow"_s;
        else if (response.status == 0 || response.error != 0)
            why = u"no answer (error %1)"_s.arg(response.error);
        else if (response.status < 200 || response.status >= 300)
            why = u"HTTP %1"_s.arg(response.status);
        else if (!response.finalUrl.startsWith(u"https://"_s) && !release::loopbackHttpUrl(response.finalUrl))
            why = u"redirected to %1"_s.arg(response.finalUrl);
        else if (received != asset.size)
            why = u"%1 bytes instead of %2"_s.arg(received).arg(asset.size);
        else if (QString::fromLatin1(hash.result().toHex()) != asset.sha256)
            why = u"SHA-256 differs"_s;
        if (!why.isEmpty()) {
            QFile::remove(partial);
            result.failures.append(url + u": "_s + why);
            continue;
        }
        QFile::remove(target);
        if (!QFile::rename(partial, target)) {
            QFile::remove(partial);
            result.failures.append(u"%1: cannot rename to %2"_s.arg(url, QDir::toNativeSeparators(target)));
            return result;
        }
        result.ok = true;
        return result;
    }
    if (urls.isEmpty())
        result.failures.append(u"no https:// source"_s);
    return result;
}

bool safeEntryName(const QString& name)
{
    QString path = name;
    path.replace(u'\\', u'/');
    if (path.isEmpty() || path.startsWith(u'/') || path.contains(u':'))
        return false;
    if (path.endsWith(u'/'))
        path.chop(1);
    for (const QString& part : path.split(u'/')) {
        if (part.isEmpty() || part == u"."_s || part == u".."_s || part.endsWith(u'.') || part.endsWith(u' ')
            || reservedName(part))
            return false;
        for (const QChar c : part)
            if (c.unicode() < 0x20 || QStringView(u"<>\"|?*").contains(c))
                return false;
    }
    return true;
}

QString unpackPortable(const QByteArray& zip, const QString& stage)
{
    if (QFileInfo::exists(stage))
        return u"the stage folder already exists"_s;
    mz_zip_archive archive {};
    if (!mz_zip_reader_init_mem(&archive, zip.constData(), static_cast<size_t>(zip.size()), 0))
        return u"not a zip archive"_s;
    struct End {
        mz_zip_archive* archive;
        ~End() { mz_zip_reader_end(archive); }
    } end {&archive};

    struct Entry {
        mz_uint index;
        QStringList parts;
        bool directory;
        qint64 size;
    };
    QList<Entry> entries;
    const mz_uint count = mz_zip_reader_get_num_files(&archive);
    if (count == 0 || count > kMaxEntries)
        return u"%1 entries"_s.arg(count);
    qint64 total = 0;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&archive, i, &stat))
            return u"entry %1 unreadable"_s.arg(i);
        const QString name = entryName(stat);
        if (!safeEntryName(name))
            return u"unsafe entry name: %1"_s.arg(name);
        if (stat.m_is_encrypted || !stat.m_is_supported)
            return u"encrypted or unsupported entry: %1"_s.arg(name);
        // A symbolic link: the Unix mode in the high word (S_IFLNK), or the
        // Windows attribute of a reparse point in the low one.
        const mz_uint32 unixMode = stat.m_external_attr >> 16;
        if ((unixMode & 0xF000) == 0xA000 || (stat.m_external_attr & FILE_ATTRIBUTE_REPARSE_POINT))
            return u"link entry: %1"_s.arg(name);
        const qint64 size = static_cast<qint64>(stat.m_uncomp_size);
        if (size > kMaxEntryBytes || (total += size) > kMaxUnpackedBytes)
            return u"too large: %1"_s.arg(name);
        QString path = name;
        path.replace(u'\\', u'/');
        const bool directory = stat.m_is_directory || path.endsWith(u'/');
        if (path.endsWith(u'/'))
            path.chop(1);
        entries.append({i, path.split(u'/'), directory, size});
    }

    // One folder around everything (release.ps1's "WinShun\"): dropped.
    const QString top = entries.first().parts.first();
    bool wrapped = true;
    for (const Entry& e : std::as_const(entries))
        wrapped = wrapped && e.parts.first().compare(top, Qt::CaseInsensitive) == 0 && (e.parts.size() > 1 || e.directory);
    QSet<QString> seen;
    bool hasProgram = false;
    QList<Entry> files;
    for (Entry e : std::as_const(entries)) {
        if (wrapped)
            e.parts.removeFirst();
        if (e.parts.isEmpty())
            continue; // the folder itself
        if (e.parts.first().compare(u"data"_s, Qt::CaseInsensitive) == 0)
            continue; // the user's own stays
        const QString key = e.parts.join(u'/').toLower();
        if (seen.contains(key))
            return u"duplicate entry: %1"_s.arg(e.parts.join(u'/'));
        seen.insert(key);
        if (!e.directory && e.parts.size() == 1 && e.parts.first().compare(u"WinShun.exe"_s, Qt::CaseInsensitive) == 0)
            hasProgram = true;
        files.append(e);
    }
    if (!hasProgram)
        return u"no WinShun.exe in the package"_s;

    QDir root(stage);
    if (!QDir().mkpath(stage))
        return u"cannot create %1"_s.arg(QDir::toNativeSeparators(stage));
    const QString rootPath = QFileInfo(stage).absoluteFilePath() + u'/';
    const auto fail = [&](const QString& why) {
        QDir(stage).removeRecursively();
        return why;
    };
    for (const Entry& e : std::as_const(files)) {
        const QString relative = e.parts.join(u'/');
        const QString target = QFileInfo(root.filePath(relative)).absoluteFilePath();
        if (!target.startsWith(rootPath, Qt::CaseInsensitive))
            return fail(u"entry outside the folder: %1"_s.arg(relative));
        if (e.directory) {
            if (!QDir().mkpath(target))
                return fail(u"cannot create %1"_s.arg(relative));
            continue;
        }
        if (!QDir().mkpath(QFileInfo(target).absolutePath()))
            return fail(u"cannot create the folder of %1"_s.arg(relative));
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&archive, e.index, &size, 0); // checks the CRC
        if (!data)
            return fail(u"cannot unpack %1"_s.arg(relative));
        QFile out(target);
        const bool written = static_cast<qint64>(size) == e.size && out.open(QIODevice::NewOnly)
            && out.write(static_cast<const char*>(data), static_cast<qint64>(size)) == static_cast<qint64>(size);
        mz_free(data);
        if (!written || !out.flush())
            return fail(u"cannot write %1"_s.arg(relative));
    }
    return {};
}

} // namespace ws::update
