// WinShunOcrTest.exe: what reading the words in the clipboard history's
// pictures does on a user's PC (tools/ocrtest/README.md).
//
// The parent finds the history and the OCR component Win顺 uses there, then
// has a child of itself read the pictures as Win顺 does at a start (the same
// order, one model, the same threads), with that PC's own onnxruntime.dll and
// models. It watches the child from outside: its memory every 100 ms against
// the step it says it is in, a small dump of it (Windows' minidump, stacks as
// pointers only) at 1 GB, 2 GB, at the memory limit it is given and when it
// crashes. A picture that goes wrong is read again alone, and alone with one
// thread. Two pictures of its own first, as a measure of the PC itself.
// Everything ends in one zip on the desktop: numbers, steps, dumps; no
// picture, no word read, no path under the user's profile.

#include "platform/CrashRecord.h"
#include "platform/OcrFiles.h"
#include "platform/PaddleOcr.h"
#include "QrCode.h"

#include <onnxruntime_c_api.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <windows.h>

#include <dbghelp.h>
#include <immintrin.h>
#include <intrin.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <miniz.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;

namespace {

constexpr auto kToolVersion = "1.0";
constexpr auto kEngine = "ppocr-v6-tiny"; // TextRecognizer::engine() with PP-OCRv6
constexpr qint64 kMB = 1024 * 1024;
constexpr qint64 kProblemBytes = 1536 * kMB; // more than this for one picture: something to look into
constexpr int kSampleMs = 100;

// Exit codes of the child, besides a crash's.
enum ChildExit : DWORD { kOk = 0, kSomeFailed = 10, kThrew = 11, kNotLoaded = 13, kBadArguments = 14 };

QString hidden(QString text); // forward

// ------------------------------------------------------------------ console

HANDLE g_out = nullptr;
bool g_console = false;

void say(const QString& line = {})
{
    const QString text = line + u"\r\n"_s;
    DWORD written = 0;
    if (g_console) {
        ::WriteConsoleW(g_out, text.utf16(), static_cast<DWORD>(text.size()), &written, nullptr);
    } else if (g_out && g_out != INVALID_HANDLE_VALUE) {
        const QByteArray utf8 = text.toUtf8();
        ::WriteFile(g_out, utf8.constData(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

void waitForEnter()
{
    const HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (!::GetConsoleMode(in, &mode))
        return;
    ::FlushConsoleInputBuffer(in);
    wchar_t buffer[64];
    DWORD read = 0;
    ::ReadConsoleW(in, buffer, 64, &read, nullptr);
}

QString megabytes(qint64 bytes)
{
    if (bytes < kMB)
        return u"%1 KB"_s.arg(bytes / 1024);
    if (bytes >= 1024 * kMB)
        return u"%1 GB"_s.arg(static_cast<double>(bytes) / (1024.0 * kMB), 0, 'f', 1);
    return u"%1 MB"_s.arg(bytes / kMB);
}

QString seconds(qint64 ms)
{
    return u"%1 秒"_s.arg(static_cast<double>(ms) / 1000.0, 0, 'f', 1);
}

// ------------------------------------------------------------------ privacy

QString g_profile; // the user's folder, written as %USERPROFILE%

QString hidden(QString text)
{
    for (const QString& form : {g_profile, QDir::fromNativeSeparators(g_profile)}) {
        if (!form.isEmpty())
            text.replace(form, u"%USERPROFILE%"_s, Qt::CaseInsensitive);
    }
    return text;
}

// ------------------------------------------------------------------ the PC

QString registryText(const wchar_t* name)
{
    wchar_t value[256] = {};
    DWORD bytes = sizeof value;
    if (::RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name, RRF_RT_REG_SZ,
            nullptr, value, &bytes) != ERROR_SUCCESS)
        return {};
    return QString::fromWCharArray(value);
}

QJsonObject windowsInfo()
{
    QJsonObject o;
    using GetVersion = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW version {sizeof version};
    if (const auto get = reinterpret_cast<GetVersion>(
            reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))); get && get(&version) == 0)
        o.insert(u"version"_s, u"%1.%2.%3"_s.arg(version.dwMajorVersion).arg(version.dwMinorVersion).arg(version.dwBuildNumber));
    DWORD ubr = 0;
    DWORD bytes = sizeof ubr;
    if (::RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD,
            nullptr, &ubr, &bytes) == ERROR_SUCCESS)
        o.insert(u"ubr"_s, static_cast<qint64>(ubr));
    QString product = registryText(L"ProductName");
    if (version.dwBuildNumber >= 22000) // Windows 11 still says 10 there
        product.replace(u"Windows 10"_s, u"Windows 11"_s);
    o.insert(u"product"_s, product);
    o.insert(u"edition"_s, registryText(L"EditionID"));
    o.insert(u"release"_s, registryText(L"ReleaseId"));
    o.insert(u"display_version"_s, registryText(L"DisplayVersion"));
    o.insert(u"remote_session"_s, ::GetSystemMetrics(SM_REMOTESESSION) != 0);
    return o;
}

QJsonObject processorInfo()
{
    QJsonObject o;
    int r[4] = {};
    __cpuid(r, 0);
    const int maxLeaf = r[0];
    char vendor[13] = {};
    std::memcpy(vendor, &r[1], 4);
    std::memcpy(vendor + 4, &r[3], 4);
    std::memcpy(vendor + 8, &r[2], 4);
    o.insert(u"vendor"_s, QString::fromLatin1(vendor));
    __cpuid(r, static_cast<int>(0x80000000));
    char brand[49] = {};
    if (static_cast<unsigned>(r[0]) >= 0x80000004) {
        for (int i = 0; i < 3; ++i)
            __cpuid(reinterpret_cast<int*>(brand + 16 * i), static_cast<int>(0x80000002 + i));
    }
    o.insert(u"name"_s, QString::fromLatin1(brand).trimmed());
    __cpuid(r, 1);
    const auto eax1 = static_cast<unsigned>(r[0]), ecx1 = static_cast<unsigned>(r[2]);
    const unsigned family = ((eax1 >> 8) & 0xF) + (((eax1 >> 8) & 0xF) == 0xF ? ((eax1 >> 20) & 0xFF) : 0);
    const unsigned model = ((eax1 >> 4) & 0xF) | (((eax1 >> 8) & 0xF) >= 6 ? ((eax1 >> 12) & 0xF0) : 0);
    o.insert(u"family_model_stepping"_s, u"%1 %2 %3"_s.arg(family).arg(model).arg(eax1 & 0xF));
    const bool osxsave = (ecx1 >> 27) & 1;
    const unsigned long long xcr0 = osxsave ? _xgetbv(0) : 0;
    const bool avxState = (xcr0 & 0x6) == 0x6;
    const bool avx512State = (xcr0 & 0xE6) == 0xE6;
    QJsonObject f;
    f.insert(u"sse4_2"_s, static_cast<bool>((ecx1 >> 20) & 1));
    f.insert(u"avx"_s, ((ecx1 >> 28) & 1) && avxState);
    f.insert(u"fma"_s, ((ecx1 >> 12) & 1) && avxState);
    f.insert(u"f16c"_s, static_cast<bool>((ecx1 >> 29) & 1));
    if (maxLeaf >= 7) {
        __cpuidex(r, 7, 0);
        const auto ebx = static_cast<unsigned>(r[1]), ecx = static_cast<unsigned>(r[2]), edx = static_cast<unsigned>(r[3]);
        f.insert(u"avx2"_s, ((ebx >> 5) & 1) && avxState);
        f.insert(u"bmi2"_s, static_cast<bool>((ebx >> 8) & 1));
        f.insert(u"avx512f"_s, ((ebx >> 16) & 1) && avx512State);
        f.insert(u"avx512dq"_s, ((ebx >> 17) & 1) && avx512State);
        f.insert(u"avx512bw"_s, ((ebx >> 30) & 1) && avx512State);
        f.insert(u"avx512vl"_s, ((ebx >> 31) & 1) && avx512State);
        f.insert(u"avx512vnni"_s, ((ecx >> 11) & 1) && avx512State);
        f.insert(u"amx_tile"_s, static_cast<bool>((edx >> 24) & 1));
        __cpuidex(r, 7, 1);
        f.insert(u"avx_vnni"_s, ((static_cast<unsigned>(r[0]) >> 4) & 1) && avxState);
        f.insert(u"avx512bf16"_s, ((static_cast<unsigned>(r[0]) >> 5) & 1) && avx512State);
    }
    o.insert(u"features"_s, f);
    const bool hypervisor = (ecx1 >> 31) & 1;
    o.insert(u"virtual_machine"_s, hypervisor);
    if (hypervisor) {
        __cpuid(r, 0x40000000);
        char name[13] = {};
        std::memcpy(name, &r[1], 4);
        std::memcpy(name + 4, &r[2], 4);
        std::memcpy(name + 8, &r[3], 4);
        o.insert(u"hypervisor"_s, QString::fromLatin1(name));
    }
    o.insert(u"logical_processors"_s, static_cast<qint64>(::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    o.insert(u"hardware_concurrency"_s, static_cast<qint64>(std::thread::hardware_concurrency()));
    return o;
}

struct Memory {
    qint64 physical = 0, physicalFree = 0, commitLimit = 0, commitFree = 0, commitTotal = 0, commitPeak = 0;
    int load = 0;
};

Memory memoryNow()
{
    Memory m;
    MEMORYSTATUSEX status {sizeof status};
    if (::GlobalMemoryStatusEx(&status)) {
        m.physical = static_cast<qint64>(status.ullTotalPhys);
        m.physicalFree = static_cast<qint64>(status.ullAvailPhys);
        m.commitLimit = static_cast<qint64>(status.ullTotalPageFile);
        m.commitFree = static_cast<qint64>(status.ullAvailPageFile);
        m.load = static_cast<int>(status.dwMemoryLoad);
    }
    PERFORMANCE_INFORMATION performance {sizeof performance};
    if (::GetPerformanceInfo(&performance, sizeof performance)) {
        const auto page = static_cast<qint64>(performance.PageSize);
        m.commitTotal = static_cast<qint64>(performance.CommitTotal) * page;
        m.commitPeak = static_cast<qint64>(performance.CommitPeak) * page;
    }
    return m;
}

QJsonObject memoryJson(const Memory& m)
{
    return {{u"physical_mb"_s, m.physical / kMB}, {u"physical_free_mb"_s, m.physicalFree / kMB},
        {u"commit_limit_mb"_s, m.commitLimit / kMB}, {u"commit_free_mb"_s, m.commitFree / kMB},
        {u"commit_total_mb"_s, m.commitTotal / kMB}, {u"commit_peak_mb"_s, m.commitPeak / kMB}, {u"load_percent"_s, m.load}};
}

QString fileVersion(const QString& file)
{
    const std::wstring path = QDir::toNativeSeparators(file).toStdWString();
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0)
        return {};
    std::vector<BYTE> data(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!::GetFileVersionInfoW(path.c_str(), 0, size, data.data())
        || !::VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info)
        return {};
    return u"%1.%2.%3.%4"_s.arg(HIWORD(info->dwFileVersionMS)).arg(LOWORD(info->dwFileVersionMS))
        .arg(HIWORD(info->dwFileVersionLS)).arg(LOWORD(info->dwFileVersionLS));
}

// The running Win顺 copies (the program itself, not its helpers).
QStringList runningApps()
{
    QStringList paths;
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return paths;
    PROCESSENTRY32W entry {sizeof entry};
    for (BOOL more = ::Process32FirstW(snapshot, &entry); more; more = ::Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"WinShun.exe") != 0)
            continue;
        if (const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)) {
            wchar_t path[MAX_PATH * 2] = {};
            DWORD length = MAX_PATH * 2;
            if (::QueryFullProcessImageNameW(process, 0, path, &length))
                paths.append(QDir::fromNativeSeparators(QString::fromWCharArray(path, static_cast<qsizetype>(length))));
            ::CloseHandle(process);
        }
    }
    ::CloseHandle(snapshot);
    return paths;
}

QString knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    QString folder;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, 0, nullptr, &path)) && path)
        folder = QDir::fromNativeSeparators(QString::fromWCharArray(path));
    ::CoTaskMemFree(path);
    return folder;
}

// ------------------------------------------------------------------ the component

QString sha256(const QString& file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&f);
    return QString::fromLatin1(hash.result().toHex());
}

// Its files against what Win顺 shipped (resources/ocr/package.json).
QJsonObject componentInfo(const QString& folder)
{
    QJsonObject o;
    o.insert(u"folder"_s, hidden(QDir::toNativeSeparators(folder)));
    o.insert(u"onnxruntime_file_version"_s, fileVersion(folder + u"/onnxruntime.dll"_s));
    QFile manifest(u":/ocrtest/resources/ocr/package.json"_s);
    if (!manifest.open(QIODevice::ReadOnly))
        return o;
    const QJsonObject package = QJsonDocument::fromJson(manifest.readAll()).object();
    o.insert(u"expected_version"_s, package.value(u"version"_s));
    QJsonArray files;
    int matching = 0, checked = 0;
    for (const QJsonValue& value : package.value(u"files"_s).toArray()) {
        const QJsonObject item = value.toObject();
        const QString path = item.value(u"path"_s).toString();
        if (path.startsWith(u"licenses/"))
            continue;
        const QString file = folder + u'/' + path;
        const QFileInfo info(file);
        const QString hash = info.isFile() ? sha256(file) : QString();
        const bool same = info.size() == item.value(u"size"_s).toInteger() && hash == item.value(u"sha256"_s).toString();
        ++checked;
        matching += same ? 1 : 0;
        files.append(QJsonObject {{u"path"_s, path}, {u"size"_s, info.isFile() ? info.size() : -1}, {u"sha256"_s, hash},
            {u"as_shipped"_s, same}});
    }
    o.insert(u"files"_s, files);
    o.insert(u"files_as_shipped"_s, u"%1/%2"_s.arg(matching).arg(checked));
    return o;
}

// ------------------------------------------------------------------ the history

struct Picture {
    qint64 id = 0;
    QString file;
    qint64 created = 0; // ms since 1970
    int width = 0, height = 0;
    qint64 bytes = 0;
    bool unread = false;
    bool sample = false;
    QString name; // a sample's
};

QString describe(const Picture& p)
{
    const QString size = u"%1 × %2，%3"_s.arg(p.width).arg(p.height).arg(megabytes(QFileInfo(p.file).size()));
    if (p.sample)
        return u"示例图「%1」（%2）"_s.arg(p.name, size);
    return u"剪贴板里 %1 复制的图（%2%3）"_s.arg(QDateTime::fromMSecsSinceEpoch(p.created).toString(u"MM-dd HH:mm"_s), size,
        p.unread ? u"，Win顺 还没读出它的字"_s : QString());
}

QJsonObject pictureJson(const Picture& p)
{
    QJsonObject o {{u"width"_s, p.width}, {u"height"_s, p.height}, {u"file_kb"_s, QFileInfo(p.file).size() / 1024},
        {u"format"_s, QFileInfo(p.file).suffix().toLower()}};
    if (p.sample) {
        o.insert(u"sample"_s, p.name);
    } else {
        o.insert(u"id"_s, p.id);
        o.insert(u"copied"_s, QDateTime::fromMSecsSinceEpoch(p.created).toString(Qt::ISODate));
        o.insert(u"unread"_s, p.unread);
    }
    return o;
}

struct History {
    bool found = false;
    QString problem;
    int pictures = 0, unread = 0, missing = 0;
    QString recognizer;
    std::vector<Picture> queue; // as Win顺 reads them at a start
    QJsonObject stats;
};

// The history's pictures in the order Win顺's start reads them: the unread
// ones as the list shows them (last used first), and when the recognizer
// changed since they were read, the read ones after. From a copy of the
// database, taken away again at once (it holds what was copied).
History readHistory(const QString& clipboard, const QString& work, int most)
{
    History h;
    if (!QFileInfo::exists(clipboard + u"/clipboard.db"_s)) {
        h.problem = u"no clipboard.db"_s;
        return h;
    }
    h.found = true;
    const QString copy = work + u"/history-copy"_s;
    QDir().mkpath(copy);
    for (const QString& name : {u"clipboard.db"_s, u"clipboard.db-wal"_s, u"clipboard.db-shm"_s}) {
        if (QFileInfo::exists(clipboard + u'/' + name))
            QFile::copy(clipboard + u'/' + name, copy + u'/' + name);
    }
    struct Row {
        Picture picture;
        int format = 0;
    };
    std::vector<Row> rows;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, u"history"_s);
        db.setDatabaseName(copy + u"/clipboard.db"_s);
        if (db.open()) {
            QSet<QString> columns;
            QSqlQuery info(db);
            if (info.exec(u"PRAGMA table_info(clips)"_s)) {
                while (info.next())
                    columns.insert(info.value(1).toString());
            }
            const QString unread = columns.contains(u"ocr"_s) ? u"ocr IS NULL"_s : u"0"_s;
            const QString format = columns.contains(u"imageFormat"_s) ? u"imageFormat"_s : u"0"_s;
            QSqlQuery clips(db);
            if (clips.exec(u"SELECT id, created, width, height, bytes, %1, %2 FROM clips WHERE kind = 4 ORDER BY used DESC, id DESC"_s
                        .arg(unread, format))) {
                while (clips.next()) {
                    Row row;
                    row.picture.id = clips.value(0).toLongLong();
                    row.picture.created = clips.value(1).toLongLong();
                    row.picture.width = clips.value(2).toInt();
                    row.picture.height = clips.value(3).toInt();
                    row.picture.bytes = clips.value(4).toLongLong();
                    row.picture.unread = clips.value(5).toBool();
                    row.format = clips.value(6).toInt();
                    rows.push_back(row);
                }
            } else {
                h.problem = u"the pictures could not be listed"_s;
            }
            QSqlQuery state(db);
            if (state.exec(u"SELECT value FROM state WHERE key = 'recognizer'"_s) && state.next())
                h.recognizer = state.value(0).toString();
            db.close();
        } else {
            h.problem = u"clipboard.db did not open"_s;
        }
    }
    QSqlDatabase::removeDatabase(u"history"_s);
    QDir(copy).removeRecursively();
    const bool readAgain = h.recognizer != QLatin1StringView(kEngine);
    std::vector<Picture> unread, read;
    qint64 largest = 0;
    QString largestSize;
    for (Row& row : rows) {
        const QString extension = row.format == 1 ? u"gif"_s : row.format == 2 ? u"jpg"_s : u"png"_s;
        row.picture.file = clipboard + u"/images/%1.%2"_s.arg(row.picture.id).arg(extension);
        ++h.pictures;
        if (!QFileInfo::exists(row.picture.file)) {
            ++h.missing;
            continue; // Win顺 skips it too
        }
        const qint64 pixels = static_cast<qint64>(row.picture.width) * row.picture.height;
        if (pixels > largest) {
            largest = pixels;
            largestSize = u"%1 x %2"_s.arg(row.picture.width).arg(row.picture.height);
        }
        h.unread += row.picture.unread ? 1 : 0;
        // A changed reader has Win顺 read them all again, in this order.
        (row.picture.unread || readAgain ? unread : read).push_back(row.picture);
    }
    // Those Win顺 reads when it starts first, in its order; then the newest
    // read before, so a history with nothing left to read is still tried.
    h.queue = std::move(unread);
    h.queue.insert(h.queue.end(), read.begin(), read.end());
    if (static_cast<int>(h.queue.size()) > most)
        h.queue.resize(static_cast<std::size_t>(most));
    h.stats = {{u"pictures"_s, h.pictures}, {u"unread"_s, h.unread}, {u"files_missing"_s, h.missing},
        {u"recognizer_noted"_s, h.recognizer}, {u"largest"_s, largestSize},
        {u"queue"_s, static_cast<qint64>(h.queue.size())},
        {u"all_read_again"_s, readAgain}};
    return h;
}

// Two pictures of the tool's own: a page of interface text (a 1440 × 1440
// poster of Win顺's) and a wide table of long lines, the reading model's
// heaviest kind of work. The same numbers on the developer's PC are in the README.
std::vector<Picture> samples(const QString& work)
{
    std::vector<Picture> list;
    const QString poster = work + u"/sample-poster.jpg"_s;
    QFile::copy(u":/ocrtest/docs/images/clipboard.jpg"_s, poster);
    QFile::setPermissions(poster, QFile::ReadOwner | QFile::WriteOwner);
    const QSize posterSize = QImageReader(poster).size();
    list.push_back({.file = poster, .width = posterSize.width(), .height = posterSize.height(), .sample = true,
        .name = u"界面截图"_s});
    QImage table(6000, 360, QImage::Format_RGB32);
    table.fill(Qt::white);
    {
        QPainter painter(&table);
        for (int row = 0; row < 6; ++row) {
            const int y = 30 + row * 55;
            for (int x = 20; x < 5960; x += 26)
                painter.fillRect(x, y, 14 + (x * 7 + row * 13) % 9, 22, Qt::black);
        }
    }
    const QString wide = work + u"/sample-wide.png"_s;
    table.save(wide);
    list.push_back({.file = wide, .width = table.width(), .height = table.height(), .sample = true, .name = u"宽表格"_s});
    return list;
}

// ------------------------------------------------------------------ the child

enum Asked : DWORD { kCrash = 1, kThrown = 2 };

struct Shared {
    DWORD thread = 0;
    ULONG64 pointers = 0; // EXCEPTION_POINTERS* in the child; 0: abort()
    DWORD asked = 0; // Asked
};

HANDLE g_childOut = nullptr;
QElapsedTimer g_childClock;
std::mutex g_stepMutex;
HANDLE g_crashEvent = nullptr;
HANDLE g_dumpedEvent = nullptr;
Shared* g_shared = nullptr;
std::atomic<bool> g_handedOver {false};
std::atomic<int> g_thrown {0};
constexpr int kThrownDumps = 2;
void(__cdecl* g_previousAbort)(int) = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

// One line to the parent: milliseconds, tab, what it is doing now.
void step(const QString& text)
{
    ws::crashrecord::setActivity(text);
    const QByteArray line = QByteArray::number(g_childClock.elapsed()) + '\t' + text.toUtf8() + '\n';
    const std::lock_guard lock(g_stepMutex);
    DWORD written = 0;
    ::WriteFile(g_childOut, line.constData(), static_cast<DWORD>(line.size()), &written, nullptr);
}

// The parent writes a dump of this process from outside, while this thread
// waits: at a crash nothing more is safe here.
void askForDump(Asked asked, EXCEPTION_POINTERS* info) noexcept
{
    if (!g_shared)
        return;
    ::ResetEvent(g_dumpedEvent);
    g_shared->thread = ::GetCurrentThreadId();
    g_shared->pointers = reinterpret_cast<ULONG64>(info);
    g_shared->asked = asked;
    ::SetEvent(g_crashEvent);
    ::WaitForSingleObject(g_dumpedEvent, 20000);
}

void handOver(EXCEPTION_POINTERS* info) noexcept
{
    if (!g_handedOver.exchange(true))
        askForDump(kCrash, info);
}

// The decorated name of what an MSVC throw throws (".?AVbad_alloc@std@@"),
// from its ThrowInfo: x64 keeps image-relative offsets, the image base in
// the fourth parameter. Empty when the record is not one.
void thrownTypeName(const EXCEPTION_RECORD& record, char* name, size_t size) noexcept
{
    name[0] = 0;
    if (record.NumberParameters < 4 || record.ExceptionInformation[0] != 0x19930520 || !record.ExceptionInformation[2])
        return;
    __try {
        const auto base = static_cast<uintptr_t>(record.ExceptionInformation[3]);
        const auto* throwInfo = reinterpret_cast<const int*>(record.ExceptionInformation[2]);
        const auto* types = reinterpret_cast<const int*>(base + throwInfo[3]); // count, then offsets
        if (types[0] < 1)
            return;
        const auto* catchable = reinterpret_cast<const int*>(base + types[1]); // properties, type, ...
        const char* decorated = reinterpret_cast<const char*>(base + catchable[1] + 2 * sizeof(void*));
        strncpy_s(name, size, decorated, _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        name[0] = 0;
    }
}

// A C++ exception where it is thrown, before anything unwinds: out of memory
// (operator new, or ONNX Runtime's own allocator) shows who asked for it.
// The QR code reader (ZXing) throws and catches its own on most pictures;
// of the rest each is a step, the first two are kept. Nothing allocates
// here: the throw may be std::bad_alloc.
LONG WINAPI onThrown(EXCEPTION_POINTERS* info)
{
    if (!info || !info->ExceptionRecord || info->ExceptionRecord->ExceptionCode != 0xE06D7363)
        return EXCEPTION_CONTINUE_SEARCH;
    char type[160];
    thrownTypeName(*info->ExceptionRecord, type, sizeof type);
    if (std::strstr(type, "@ZXing@@"))
        return EXCEPTION_CONTINUE_SEARCH;
    const int count = ++g_thrown;
    if (count <= 20 && g_childOut) {
        char line[256];
        const int length = std::snprintf(line, sizeof line, "%lld\tthrown %s on thread %lu\n",
            static_cast<long long>(g_childClock.elapsed()), type[0] ? type : "(unknown type)", ::GetCurrentThreadId());
        const std::lock_guard lock(g_stepMutex);
        DWORD written = 0;
        ::WriteFile(g_childOut, line, static_cast<DWORD>(std::clamp(length, 0, static_cast<int>(sizeof line) - 1)), &written, nullptr);
    }
    if (count <= kThrownDumps)
        askForDump(kThrown, info);
    return EXCEPTION_CONTINUE_SEARCH;
}

void __cdecl onChildAbort(int signal)
{
    if (g_previousAbort && g_previousAbort != SIG_DFL && g_previousAbort != SIG_IGN && g_previousAbort != onChildAbort)
        g_previousAbort(signal); // the crash record
    handOver(nullptr);
}

LONG WINAPI onChildException(EXCEPTION_POINTERS* info)
{
    // MSVC's throw nobody catches goes on to std::terminate: abort() hands it over.
    if (info && info->ExceptionRecord && info->ExceptionRecord->ExceptionCode != 0xE06D7363)
        handOver(info);
    return g_previousFilter ? g_previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

QString onnxRuntimeVersion()
{
    const HMODULE module = ::GetModuleHandleW(L"onnxruntime.dll");
    using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
    const auto get = module ? reinterpret_cast<GetApiBase>(::GetProcAddress(module, "OrtGetApiBase")) : nullptr;
    return get ? QString::fromUtf8(get()->GetVersionString()) : QString();
}

// For trying the tool out (README): the child goes wrong on purpose.
[[noreturn]] void fail(const QString& kind)
{
    step(u"going wrong on purpose: "_s + kind);
    if (kind == u"abort")
        std::abort();
    if (kind == u"throw")
        std::thread([] { throw std::runtime_error("nobody catches this"); }).join();
    if (kind == u"oom") {
        std::vector<std::unique_ptr<char[]>> blocks;
        for (;;) {
            blocks.emplace_back(new char[64 * kMB]);
            std::memset(blocks.back().get(), 1, 64 * kMB);
            step(u"held %1 MB"_s.arg(static_cast<qint64>(blocks.size()) * 64));
        }
    }
    if (kind == u"hang")
        ::Sleep(INFINITE);
    volatile int* nowhere = nullptr;
    *nowhere = 1;
    std::abort();
}

// Reads the pictures in the list as TextRecognizer does: one after another on
// one thread below normal priority, the models loaded with the first one.
int child(const QStringList& args)
{
    const auto value = [&](const QString& name) {
        const qsizetype at = args.indexOf(name);
        return at >= 0 && at + 1 < args.size() ? args[at + 1] : QString();
    };
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // no Windows report: the parent has it
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    g_childOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    g_childClock.start();
    const QString names = value(u"--names"_s);
    if (!names.isEmpty()) {
        g_crashEvent = ::OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, (names + u"-crash"_s).toStdWString().c_str());
        g_dumpedEvent = ::OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, (names + u"-dumped"_s).toStdWString().c_str());
        if (const HANDLE mapping = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, (names + u"-shared"_s).toStdWString().c_str()))
            g_shared = static_cast<Shared*>(::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    }
    ws::crashrecord::install(value(u"--crash"_s));
    g_previousAbort = std::signal(SIGABRT, onChildAbort);
    g_previousFilter = ::SetUnhandledExceptionFilter(onChildException);
    ::AddVectoredExceptionHandler(1, onThrown);
    ws::ocr::setComponentsRoot(value(u"--components"_s));
    ws::PaddleOcr::setTrace(step);
    QFile list(value(u"--list"_s));
    if (!list.open(QIODevice::ReadOnly))
        return kBadArguments;
    std::vector<std::pair<QString, QString>> pictures; // label, file
    for (const QByteArray& line : list.readAll().split('\n')) {
        const QList<QByteArray> parts = line.trimmed().split('\t');
        if (parts.size() == 2)
            pictures.emplace_back(QString::fromUtf8(parts[0]), QString::fromUtf8(parts[1]));
    }
    const int threads = std::max(1, value(u"--threads"_s).toInt());
    const QString fault = value(u"--fault"_s);
    const QString faultAt = fault.section(u':', 1);
    step(u"start, %1 threads, %2 pictures"_s.arg(threads).arg(pictures.size()));
    std::unique_ptr<ws::PaddleOcr> paddle;
    bool someFailed = false;
    try {
        for (const auto& [label, file] : pictures) {
            step(u"reading picture "_s + label);
            if (!QFileInfo::exists(file)) {
                step(u"missing"_s);
                continue;
            }
            if (!paddle) {
                QString error;
                paddle = ws::PaddleOcr::load(value(u"--program"_s), threads, &error);
                if (!paddle) {
                    step(u"not loaded: "_s + hidden(error));
                    return kNotLoaded;
                }
                step(u"models loaded, onnxruntime "_s + onnxRuntimeVersion());
            }
            QImageReader reader(file);
            reader.setAutoTransform(true);
            step(u"decode %1, %2 x %3"_s.arg(QString::fromLatin1(reader.format())).arg(reader.size().width()).arg(reader.size().height()));
            const QImage image = reader.read();
            step(image.isNull() ? u"not decoded: "_s + reader.errorString()
                                : u"decoded %1 x %2, format %3, depth %4"_s.arg(image.width()).arg(image.height())
                                      .arg(static_cast<int>(image.format())).arg(image.depth()));
            if (!fault.isEmpty() && (faultAt.isEmpty() || faultAt == label))
                fail(fault.section(u':', 0, 0));
            QStringList lines;
            QString error;
            if (!paddle->read(image, &lines, &error)) {
                step(u"failed: "_s + hidden(error));
                someFailed = true;
                continue; // Win顺 goes on to the next one too
            }
            qsizetype characters = 0;
            for (const QString& line : std::as_const(lines))
                characters += line.size();
            step(u"read %1 lines, %2 characters"_s.arg(lines.size()).arg(characters));
            const QString words = ws::capture::withQrCodes(lines.join(u'\n'), image);
            step(u"qr codes looked for, %1 characters in all"_s.arg(words.size()));
            step(u"done "_s + label);
        }
    } catch (const std::exception& e) {
        // Win顺 up to 0.38 had nothing around this: the program would have ended.
        step(u"exception: "_s + hidden(QString::fromUtf8(e.what())));
        return kThrew;
    } catch (...) {
        step(u"exception: of no known kind"_s);
        return kThrew;
    }
    step(u"end"_s);
    return someFailed ? kSomeFailed : kOk;
}

// ------------------------------------------------------------------ watching the child

struct Context {
    QString exe, work, program, components, fault;
    qint64 limit = 0; // bytes the child may commit
    int timeoutMs = 240000;
};

struct Stage {
    qint64 ms = 0; // since the child started, as the parent saw it
    QString text;
    qint64 commitAt = 0, highest = 0; // when it began, the most until the next
};

struct Run {
    QString title;
    std::vector<Picture> pictures;
    int threads = 0;
    QString outcome; // ok, some-failed, threw, not-loaded, crashed, memory-limit, timeout, did-not-start
    DWORD exitCode = 0;
    qint64 ms = 0, peakCommit = 0, peakWorkingSet = 0, jobPeak = 0, cpuMs = 0;
    bool hitLimit = false;
    std::vector<Stage> stages;
    std::vector<std::array<qint64, 3>> samples; // ms, commit, working set
    QStringList dumps;
    QJsonObject crash;
    QString lastPicture; // the label of the one read when it ended
    QStringList failedPictures; // those whose reading failed, in order
    std::map<QString, qint64> picturePeak; // label → the most committed while it was read
    bool ok() const { return outcome.startsWith(u"ok"); } // also "ok (no memory limit)"
    bool problem() const { return !ok() || peakCommit > kProblemBytes; }
    // The picture that went wrong: the one being read when the child ended
    // early; when it read them all, the one that took the most (if too much),
    // else the first that failed.
    QString culprit() const
    {
        if (!ok() && !outcome.startsWith(u"some-failed"))
            return lastPicture;
        const auto most = std::ranges::max_element(picturePeak, {}, [](const auto& p) { return p.second; });
        if (most != picturePeak.end() && most->second > kProblemBytes)
            return most->first;
        return failedPictures.isEmpty() ? QString() : failedPictures.first();
    }
};

QString label(const Picture& p)
{
    return p.sample ? u"sample-"_s + p.name : QString::number(p.id);
}

bool writeDump(HANDLE process, DWORD pid, const QString& file, DWORD thread = 0, ULONG64 pointers = 0)
{
    const HANDLE out = ::CreateFileW(QDir::toNativeSeparators(file).toStdWString().c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE)
        return false;
    MINIDUMP_EXCEPTION_INFORMATION exception {thread, reinterpret_cast<PEXCEPTION_POINTERS>(pointers), TRUE};
    // The calls of every thread; of the stacks only what points into code.
    const auto type = static_cast<MINIDUMP_TYPE>(
        MiniDumpNormal | MiniDumpFilterMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    const bool ok = ::MiniDumpWriteDump(process, pid, out, type, pointers ? &exception : nullptr, nullptr, nullptr);
    ::CloseHandle(out);
    if (!ok)
        QFile::remove(file);
    return ok;
}

Run runChild(const QString& title, const std::vector<Picture>& pictures, int threads, const Context& c)
{
    static int count = 0;
    const int n = ++count;
    Run run;
    run.title = title;
    run.pictures = pictures;
    run.threads = threads;
    const QString list = c.work + u"/run-%1.txt"_s.arg(n);
    {
        QFile out(list);
        if (out.open(QIODevice::WriteOnly)) {
            for (const Picture& p : pictures)
                out.write((label(p) + u'\t' + QDir::toNativeSeparators(p.file) + u'\n').toUtf8());
        }
    }
    const QString crashFile = c.work + u"/crash-%1.txt"_s.arg(n);
    const QString names = u"Local\\WinShunOcrTest-%1-%2"_s.arg(::GetCurrentProcessId()).arg(n);
    const HANDLE crashEvent = ::CreateEventW(nullptr, TRUE, FALSE, (names + u"-crash"_s).toStdWString().c_str());
    const HANDLE dumpedEvent = ::CreateEventW(nullptr, TRUE, FALSE, (names + u"-dumped"_s).toStdWString().c_str());
    const HANDLE mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared),
        (names + u"-shared"_s).toStdWString().c_str());
    auto* shared = mapping ? static_cast<Shared*>(::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared))) : nullptr;
    const HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit {};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    limit.ProcessMemoryLimit = static_cast<SIZE_T>(c.limit);
    ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof limit);
    SECURITY_ATTRIBUTES inherit {sizeof inherit, nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    ::CreatePipe(&readEnd, &writeEnd, &inherit, 0);
    ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    const HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup {sizeof startup};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nul;
    startup.hStdOutput = writeEnd;
    startup.hStdError = writeEnd;
    const auto quoted = [](const QString& s) -> QString { return u'"' + QDir::toNativeSeparators(s) + u'"'; };
    QString command = quoted(c.exe) + u" --child --list "_s + quoted(list) + u" --threads %1 --names "_s.arg(threads) + names
        + u" --crash "_s + quoted(crashFile) + u" --program "_s + quoted(c.program) + u" --components "_s + quoted(c.components);
    if (!c.fault.isEmpty())
        command += u" --fault "_s + c.fault;
    std::wstring line = command.toStdWString();
    PROCESS_INFORMATION process {};
    const bool started = ::CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process);
    ::CloseHandle(writeEnd);
    if (nul != INVALID_HANDLE_VALUE)
        ::CloseHandle(nul);
    if (!started) {
        run.outcome = u"did-not-start"_s;
        ::CloseHandle(readEnd);
        ::CloseHandle(job);
        return run;
    }
    const bool limited = ::AssignProcessToJobObject(job, process.hProcess);
    ::ResumeThread(process.hThread);
    QByteArray pending;
    std::mutex pendingMutex;
    std::thread reader([&] {
        char buffer[4096];
        DWORD got = 0;
        while (::ReadFile(readEnd, buffer, sizeof buffer, &got, nullptr) && got) {
            const std::lock_guard lock(pendingMutex);
            pending.append(buffer, static_cast<qsizetype>(got));
        }
    });
    QElapsedTimer clock;
    clock.start();
    QByteArray carry;
    qint64 commit = 0;
    bool handedOver = false, dumped1 = false, dumped2 = false, dumpedLimit = false;
    int thrown = 0;
    const auto dump = [&](const QString& why, DWORD thread = 0, ULONG64 pointers = 0) {
        const QString name = u"dumps/run%1-%2.dmp"_s.arg(n).arg(why);
        if (writeDump(process.hProcess, process.dwProcessId, c.work + u'/' + name, thread, pointers))
            run.dumps.append(name);
    };
    const auto drain = [&] {
        {
            const std::lock_guard lock(pendingMutex);
            carry += pending;
            pending.clear();
        }
        for (qsizetype end; (end = carry.indexOf('\n')) >= 0;) {
            const QString text = QString::fromUtf8(carry.left(end)).section(u'\t', 1);
            carry.remove(0, end + 1);
            if (text.startsWith(u"reading picture "))
                run.lastPicture = text.mid(16);
            else if (text.startsWith(u"failed: ") && !run.lastPicture.isEmpty())
                run.failedPictures.append(run.lastPicture);
            run.stages.push_back({clock.elapsed(), text, commit, commit});
        }
    };
    for (;;) {
        // A dump asked for comes first: the child waits for it before it ends.
        const HANDLE waits[2] = {crashEvent, process.hProcess};
        const DWORD signalled = handedOver ? ::WaitForSingleObject(process.hProcess, kSampleMs)
                                           : ::WaitForMultipleObjects(2, waits, FALSE, kSampleMs);
        const bool asked = !handedOver && signalled == WAIT_OBJECT_0;
        const bool exited = handedOver ? signalled == WAIT_OBJECT_0 : signalled == WAIT_OBJECT_0 + 1;
        PROCESS_MEMORY_COUNTERS_EX counters {};
        if (!exited // gone: nothing left to count
            && ::GetProcessMemoryInfo(process.hProcess, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof counters)
            && counters.PrivateUsage > 0) {
            commit = static_cast<qint64>(counters.PrivateUsage);
            const auto workingSet = static_cast<qint64>(counters.WorkingSetSize);
            run.peakCommit = std::max(run.peakCommit, commit);
            run.peakWorkingSet = std::max(run.peakWorkingSet, workingSet);
            run.samples.push_back({clock.elapsed(), commit, workingSet});
            if (!run.lastPicture.isEmpty())
                run.picturePeak[run.lastPicture] = std::max(run.picturePeak[run.lastPicture], commit);
        }
        drain();
        if (!run.stages.empty())
            run.stages.back().highest = std::max(run.stages.back().highest, commit);
        if (!dumped1 && commit >= 1024 * kMB) {
            dumped1 = true;
            dump(u"1GB"_s);
        }
        if (!dumped2 && commit >= 2048 * kMB && 2048 * kMB < c.limit - 256 * kMB) {
            dumped2 = true;
            dump(u"2GB"_s);
        }
        if (!dumpedLimit && commit >= c.limit - 160 * kMB) {
            dumpedLimit = true;
            dump(u"limit"_s);
        }
        if (asked) {
            ::ResetEvent(crashEvent); // before it is answered: it may ask again
            const bool crashed = !shared || shared->asked == kCrash;
            handedOver = crashed;
            dump(crashed ? u"crash"_s : u"thrown%1"_s.arg(++thrown), shared ? shared->thread : 0, shared ? shared->pointers : 0);
            ::SetEvent(dumpedEvent);
        }
        if (exited)
            break;
        if (clock.elapsed() > c.timeoutMs) {
            dump(u"timeout"_s);
            ::TerminateJobObject(job, 0xDEAD);
            ::TerminateProcess(process.hProcess, 0xDEAD);
            ::WaitForSingleObject(process.hProcess, 10000);
            run.outcome = u"timeout"_s;
            break;
        }
    }
    reader.join();
    drain();
    run.ms = clock.elapsed();
    ::GetExitCodeProcess(process.hProcess, &run.exitCode);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION used {};
    if (limited && ::QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &used, sizeof used, nullptr))
        run.jobPeak = static_cast<qint64>(used.PeakProcessMemoryUsed);
    FILETIME created {}, exited {}, kernel {}, user {};
    if (::GetProcessTimes(process.hProcess, &created, &exited, &kernel, &user)) {
        const auto ticks = [](const FILETIME& t) { return (static_cast<qint64>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        run.cpuMs = (ticks(kernel) + ticks(user)) / 10000;
    }
    run.peakCommit = std::max(run.peakCommit, run.jobPeak);
    run.hitLimit = limited && run.jobPeak >= c.limit - 64 * kMB;
    if (run.outcome.isEmpty()) {
        switch (run.exitCode) {
        case kOk: run.outcome = u"ok"_s; break;
        case kSomeFailed: run.outcome = u"some-failed"_s; break;
        case kThrew: run.outcome = u"threw"_s; break;
        case kNotLoaded: run.outcome = u"not-loaded"_s; break;
        default: run.outcome = u"crashed"_s; break;
        }
        if (run.hitLimit && run.outcome != u"ok")
            run.outcome = u"memory-limit"_s;
    }
    run.crash = ws::crashrecord::takeLeft(crashFile);
    if (!limited)
        run.outcome += u" (no memory limit)"_s;
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    ::CloseHandle(readEnd);
    ::CloseHandle(job);
    if (shared)
        ::UnmapViewOfFile(shared);
    for (const HANDLE h : {mapping, crashEvent, dumpedEvent}) {
        if (h)
            ::CloseHandle(h);
    }
    QFile::remove(list);
    return run;
}

QJsonObject runJson(const Run& r)
{
    QJsonObject o {{u"title"_s, r.title}, {u"threads"_s, r.threads}, {u"outcome"_s, r.outcome},
        {u"exit_code"_s, u"0x%1"_s.arg(r.exitCode, 8, 16, u'0')}, {u"ms"_s, r.ms}, {u"cpu_ms"_s, r.cpuMs},
        {u"peak_commit_mb"_s, r.peakCommit / kMB}, {u"peak_working_set_mb"_s, r.peakWorkingSet / kMB},
        {u"hit_memory_limit"_s, r.hitLimit}, {u"last_picture"_s, r.lastPicture}};
    QJsonArray pictures;
    for (const Picture& p : r.pictures) {
        QJsonObject item = pictureJson(p);
        if (const auto at = r.picturePeak.find(label(p)); at != r.picturePeak.end())
            item.insert(u"peak_commit_mb"_s, at->second / kMB);
        pictures.append(item);
    }
    o.insert(u"pictures"_s, pictures);
    QJsonArray stages;
    for (const Stage& s : r.stages)
        stages.append(QJsonObject {{u"ms"_s, s.ms}, {u"step"_s, s.text}, {u"commit_mb"_s, s.commitAt / kMB},
            {u"highest_mb"_s, s.highest / kMB}});
    o.insert(u"steps"_s, stages);
    // At most 600 points: of each stretch, its highest.
    QJsonArray timeline;
    const std::size_t stride = std::max<std::size_t>(1, (r.samples.size() + 599) / 600);
    for (std::size_t i = 0; i < r.samples.size(); i += stride) {
        std::array<qint64, 3> top = r.samples[i];
        for (std::size_t j = i; j < std::min(r.samples.size(), i + stride); ++j) {
            if (r.samples[j][1] > top[1])
                top = r.samples[j];
        }
        timeline.append(QJsonArray {top[0], top[1] / kMB, top[2] / kMB});
    }
    o.insert(u"timeline_ms_commit_ws_mb"_s, timeline);
    o.insert(u"dumps"_s, QJsonArray::fromStringList(r.dumps));
    if (!r.crash.isEmpty())
        o.insert(u"crash"_s, r.crash);
    return o;
}

// The step it was in when its memory was highest, and how high.
const Stage* heaviestStep(const Run& r)
{
    const Stage* heaviest = nullptr;
    for (const Stage& s : r.stages) {
        if (!heaviest || s.highest > heaviest->highest)
            heaviest = &s;
    }
    return heaviest;
}

QString outcomeText(const Run& r)
{
    if (r.outcome.startsWith(u"ok"))
        return u"正常"_s;
    if (r.outcome.startsWith(u"some-failed"))
        return u"有的图没读出来（识别报错）"_s;
    if (r.outcome.startsWith(u"threw"))
        return u"出了异常（旧版 Win顺 在这里会闪退）"_s;
    if (r.outcome.startsWith(u"not-loaded"))
        return u"识别组件加载失败"_s;
    if (r.outcome.startsWith(u"memory-limit"))
        return u"内存涨到上限 %1 被停下"_s.arg(megabytes(r.jobPeak));
    if (r.outcome.startsWith(u"timeout"))
        return u"太久没读完，被停下"_s;
    if (r.outcome.startsWith(u"did-not-start"))
        return u"子进程没能启动"_s;
    return u"崩溃了（退出码 0x%1）"_s.arg(r.exitCode, 8, 16, u'0');
}

// ------------------------------------------------------------------ the report

struct Report {
    QJsonObject head;
    std::vector<Run> runs;
    QStringList summary; // what the console said, for report.txt
    QString zipPath;
};

bool writeZip(const Report& report, const QString& work)
{
    QJsonObject root = report.head;
    QJsonArray runs;
    for (const Run& r : report.runs)
        runs.append(runJson(r));
    root.insert(u"runs"_s, runs);
    std::vector<std::pair<std::string, QByteArray>> files;
    files.emplace_back("results.json", QJsonDocument(root).toJson(QJsonDocument::Indented));
    files.emplace_back("report.txt", (report.summary.join(u"\r\n"_s) + u"\r\n"_s).toUtf8());
    for (const Run& r : report.runs) {
        for (const QString& name : r.dumps) {
            QFile dump(work + u'/' + name);
            if (dump.open(QIODevice::ReadOnly))
                files.emplace_back(name.toStdString(), dump.readAll());
        }
    }
    mz_zip_archive zip {};
    if (!mz_zip_writer_init_heap(&zip, 0, 1 << 20))
        return false;
    bool ok = true;
    for (const auto& [name, data] : files) {
        if (ok && !mz_zip_writer_add_mem(&zip, name.c_str(), data.constData(), static_cast<size_t>(data.size()), MZ_BEST_COMPRESSION))
            ok = false;
    }
    void* buffer = nullptr;
    size_t bytes = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &buffer, &bytes);
    mz_zip_writer_end(&zip);
    if (ok) {
        QFile out(report.zipPath + u".partial"_s);
        ok = out.open(QIODevice::WriteOnly) && out.write(static_cast<const char*>(buffer), static_cast<qint64>(bytes)) == static_cast<qint64>(bytes);
        out.close();
        QFile::remove(report.zipPath);
        ok = ok && QFile::rename(report.zipPath + u".partial"_s, report.zipPath);
    }
    mz_free(buffer);
    return ok;
}

void note(Report& report, const QString& line = {})
{
    say(line);
    report.summary.append(line);
}

void noteRun(Report& report, const Run& r)
{
    QString line = u"  → %1，内存最高 %2，用时 %3"_s.arg(outcomeText(r), megabytes(r.peakCommit), seconds(r.ms));
    note(report, line);
    if (r.problem()) {
        if (const Stage* s = heaviestStep(r); s && s->highest > 0)
            note(report, u"    内存最高时在做：%1（%2）"_s.arg(s->text, megabytes(s->highest)));
        if (!r.stages.empty())
            note(report, u"    最后一步：%1"_s.arg(r.stages.back().text));
    }
}

struct Options {
    QString data, program, out, fault;
    qint64 limitMb = 0;
    int timeoutS = 240, threads = 0, most = 80, budgetMinutes = 20;
    bool wait = true, open = true;
};

Options options(const QStringList& args)
{
    Options o;
    const auto value = [&](const QString& name) {
        const qsizetype at = args.indexOf(name);
        return at >= 0 && at + 1 < args.size() ? args[at + 1] : QString();
    };
    o.data = value(u"--data"_s);
    o.program = value(u"--program"_s);
    o.out = value(u"--out"_s);
    o.fault = value(u"--fault"_s);
    o.limitMb = value(u"--limit-mb"_s).toLongLong();
    if (const int t = value(u"--timeout"_s).toInt(); t > 0)
        o.timeoutS = t;
    o.threads = value(u"--threads"_s).toInt();
    if (const int m = value(u"--most"_s).toInt(); m > 0)
        o.most = m;
    if (const int b = value(u"--minutes"_s).toInt(); b > 0)
        o.budgetMinutes = b;
    o.wait = !args.contains(u"--no-wait"_s);
    o.open = !args.contains(u"--no-open"_s);
    return o;
}

int parent(const QStringList& args)
{
    g_out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    g_console = ::GetConsoleMode(g_out, &mode);
    ::SetConsoleTitleW(L"Win顺 读图测试");
    const Options o = options(args);
    g_profile = QDir::toNativeSeparators(QDir::homePath());
    Report report;
    note(report, u"Win顺 读图测试 %1"_s.arg(QLatin1StringView(kToolVersion)));
    note(report, u"它用这台电脑上 Win顺 的文字识别组件，把剪贴板历史里的图片照 Win顺 启动时的样子读一遍，"_s
                 u"记下每一步用了多少内存。不会改动剪贴板历史，也不会把图片或里面的字写进报告。"_s);
    note(report);
    QStringList apps = runningApps();
    if (!apps.isEmpty() && o.wait) {
        say(u"Win顺 正在运行。请先在任务栏右下角 Win顺 的图标上点右键，选“退出”，然后回到这里按回车。"_s);
        say(u"（不退出也能测，只是 Win顺 自己也在读图，结果会不准。）"_s);
        waitForEnter();
        const QStringList still = runningApps();
        if (!still.isEmpty())
            apps = still;
    }
    QTemporaryDir work(QDir::tempPath() + u"/WinShunOcrTest-XXXXXX"_s);
    if (!work.isValid()) {
        note(report, u"没能建临时文件夹，测试做不了。"_s);
        if (o.wait)
            waitForEnter();
        return 1;
    }
    QDir().mkpath(work.filePath(u"dumps"_s));
    // Where Win顺 keeps its data: a portable copy's "data" beside it, else the user's.
    QString appFolder = apps.isEmpty() ? QString() : QFileInfo(apps.first()).absolutePath();
    QString data = o.data;
    if (data.isEmpty() && !appFolder.isEmpty() && QFileInfo(appFolder + u"/data"_s).isDir())
        data = appFolder + u"/data"_s;
    if (data.isEmpty())
        data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/WinShun"_s;
    QString program = o.program;
    if (program.isEmpty())
        program = !appFolder.isEmpty() ? appFolder : knownFolder(FOLDERID_ProgramFiles) + u"/WinShun"_s;
    const QString components = data + u"/components"_s;
    ws::ocr::setComponentsRoot(components);
    const QString component = ws::ocr::folder(program);
    const Memory before = memoryNow();
    const int threads = o.threads > 0 ? o.threads : std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, 4);
    qint64 limit = o.limitMb > 0 ? o.limitMb * kMB : std::min<qint64>(3072 * kMB, before.commitFree - 768 * kMB);
    limit = std::max<qint64>(limit, 1024 * kMB);
    const QString stamp = QDateTime::currentDateTime().toString(u"yyyyMMdd-HHmmss"_s);
    const QString outFolder = !o.out.isEmpty() ? o.out : knownFolder(FOLDERID_Desktop);
    report.zipPath = QDir(outFolder.isEmpty() ? QDir::homePath() : outFolder).filePath(u"WinShun-ocr-test-%1.zip"_s.arg(stamp));
    const QJsonObject processor = processorInfo();
    const QJsonObject windows = windowsInfo();
    report.head = {{u"tool"_s, QLatin1StringView(kToolVersion)}, {u"created"_s, QDateTime::currentDateTime().toString(Qt::ISODate)},
        {u"windows"_s, windows}, {u"processor"_s, processor}, {u"memory_before"_s, memoryJson(before)},
        {u"winshun"_s, QJsonObject {{u"running"_s, !apps.isEmpty()}, {u"program_folder"_s, hidden(QDir::toNativeSeparators(program))},
                            {u"version"_s, fileVersion(program + u"/WinShun.exe"_s)},
                            {u"data_folder"_s, hidden(QDir::toNativeSeparators(data))}, {u"portable"_s, data.startsWith(program)}}},
        {u"threads"_s, threads}, {u"memory_limit_mb"_s, limit / kMB}, {u"fault"_s, o.fault}};
    const QJsonObject features = processor.value(u"features"_s).toObject();
    note(report, u"电脑：%1 %2（%3.%4）· %5 · %6 个逻辑处理器%7"_s
            .arg(windows.value(u"product"_s).toString(), windows.value(u"display_version"_s).toString().isEmpty()
                    ? windows.value(u"release"_s).toString() : windows.value(u"display_version"_s).toString(),
                windows.value(u"version"_s).toString().section(u'.', 2), QString::number(windows.value(u"ubr"_s).toInteger()),
                processor.value(u"name"_s).toString(), QString::number(processor.value(u"logical_processors"_s).toInteger()),
                processor.value(u"virtual_machine"_s).toBool() ? u" · 虚拟化层 "_s + processor.value(u"hypervisor"_s).toString() : QString()));
    note(report, u"处理器指令：AVX2 %1，AVX-512 %2"_s.arg(features.value(u"avx2"_s).toBool() ? u"有"_s : u"无"_s,
                                                 features.value(u"avx512f"_s).toBool() ? u"有"_s : u"无"_s));
    note(report, u"内存：%1，可用 %2；可提交 %3，已提交 %4；这次给每次读图的上限 %5"_s.arg(megabytes(before.physical),
                     megabytes(before.physicalFree), megabytes(before.commitLimit), megabytes(before.commitTotal), megabytes(limit)));
    note(report, u"Win顺：%1%2"_s.arg(fileVersion(program + u"/WinShun.exe"_s).isEmpty() ? u"（没找到程序）"_s
                                                                                    : fileVersion(program + u"/WinShun.exe"_s),
                     apps.isEmpty() ? QString() : u"，测试时还在运行"_s));
    if (component.isEmpty()) {
        report.head.insert(u"component"_s, QJsonObject {{u"found"_s, false},
            {u"components_folder_entries"_s, QJsonArray::fromStringList(QDir(components).entryList(QDir::Dirs | QDir::NoDotAndDotDot))}});
        note(report, u"没找到 Win顺 的增强文字识别组件。这台电脑上 Win顺 用的是 Windows 自带的识别，这个测试测不了它。"_s);
    } else {
        const QJsonObject info = componentInfo(component);
        report.head.insert(u"component"_s, info);
        note(report, u"文字识别组件：ONNX Runtime %1，文件和发布时一致 %2"_s.arg(info.value(u"onnxruntime_file_version"_s).toString(),
                         info.value(u"files_as_shipped"_s).toString()));
    }
    const History history = readHistory(data + u"/clipboard"_s, work.path(), o.most);
    report.head.insert(u"history"_s, history.stats);
    if (!history.found)
        note(report, u"没找到剪贴板历史。"_s);
    else
        note(report, u"剪贴板历史：%1 张图，还没读过字的 %2 张，这次读 %3 张"_s.arg(history.pictures).arg(history.unread)
                         .arg(history.queue.size()));
    note(report);
    writeZip(report, work.path());
    Context context {.exe = QCoreApplication::applicationFilePath(), .work = work.path(), .program = program,
        .components = components, .fault = {}, .limit = limit, .timeoutMs = o.timeoutS * 1000};
    const QString fault = o.fault;
    bool samplesProblem = false;
    std::vector<QString> culprits;
    if (!component.isEmpty()) {
        // The PC itself, on pictures that are the same everywhere.
        note(report, u"先读两张测试程序自带的图，看这台电脑本身怎么样："_s);
        context.fault = fault.contains(u':') ? QString() : fault;
        Run sampleRun = runChild(u"samples"_s, samples(work.path()), threads, context);
        for (const Picture& p : sampleRun.pictures) {
            const auto at = sampleRun.picturePeak.find(label(p));
            note(report, u"  %1：内存最高 %2"_s.arg(describe(p), at == sampleRun.picturePeak.end() ? u"—"_s : megabytes(at->second)));
        }
        noteRun(report, sampleRun);
        samplesProblem = sampleRun.problem();
        report.runs.push_back(std::move(sampleRun));
        writeZip(report, work.path());
        note(report);
        // Then the history's, as Win顺 reads them; one that goes wrong alone
        // (and alone on one thread), then on with the rest.
        context.fault = fault.contains(u':') ? fault : QString();
        std::vector<Picture> left = history.queue;
        QElapsedTimer budget;
        budget.start();
        int round = 0;
        while (!left.empty() && budget.elapsed() < o.budgetMinutes * 60000LL) {
            ++round;
            note(report, round == 1 ? u"读剪贴板里的 %1 张图，Win顺 启动时要读的在前，顺序和它一样："_s.arg(left.size())
                                    : u"接着读剩下的 %1 张："_s.arg(left.size()));
            Run together = runChild(u"history in order"_s, left, threads, context);
            noteRun(report, together);
            const bool problem = together.problem();
            const QString culprit = together.culprit();
            report.runs.push_back(std::move(together));
            writeZip(report, work.path());
            if (!problem)
                break;
            const auto at = std::ranges::find_if(left, [&](const Picture& p) { return label(p) == culprit; });
            if (at == left.end())
                break;
            const Picture picture = *at;
            culprits.push_back(describe(picture));
            note(report, u"  问题出在%1。单独再读它一次："_s.arg(describe(picture)));
            Run alone = runChild(u"alone"_s, {picture}, threads, context);
            noteRun(report, alone);
            report.runs.push_back(std::move(alone));
            writeZip(report, work.path());
            if (threads > 1) {
                note(report, u"  只用 1 个线程再读一次："_s);
                Run single = runChild(u"alone, 1 thread"_s, {picture}, 1, context);
                noteRun(report, single);
                report.runs.push_back(std::move(single));
                writeZip(report, work.path());
            }
            left.erase(left.begin(), at + 1);
        }
        if (!left.empty() && budget.elapsed() >= o.budgetMinutes * 60000LL)
            note(report, u"时间到了，还有 %1 张没读。"_s.arg(left.size()));
    }
    report.head.insert(u"memory_after"_s, memoryJson(memoryNow()));
    note(report);
    if (component.isEmpty() || !history.found) {
        note(report, u"这次没能测读图。"_s);
    } else if (samplesProblem) {
        note(report, u"结果：测试程序自带的图也出了问题，多半是这台电脑的环境（处理器、内存）引起的，和具体哪张图无关。"_s);
    } else if (!culprits.empty()) {
        note(report, u"结果：找到了出问题的图——"_s);
        for (const QString& c : culprits)
            note(report, u"  · "_s + c);
        note(report, u"想让 Win顺 先不闪退，可以在剪贴板历史里找到这张图删掉。方便的话也可以把这张图发给开发者"_s
                     u"（图里有不想给人看的内容就不用发）。"_s);
    } else {
        note(report, u"结果：读图没有发现问题。"_s);
    }
    const bool saved = writeZip(report, work.path());
    note(report);
    if (saved) {
        say(u"测试做完了。请把这个文件发给开发者："_s);
        say(QDir::toNativeSeparators(report.zipPath));
        if (o.open) {
            const std::wstring select = (u"/select,\""_s + QDir::toNativeSeparators(report.zipPath) + u'"').toStdWString();
            ::ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
        }
    } else {
        say(u"报告没能保存到 "_s + QDir::toNativeSeparators(report.zipPath));
    }
    if (o.wait) {
        say(u"按回车关闭这个窗口。"_s);
        waitForEnter();
    }
    return saved ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv); // pictures need no window
    const QStringList args = QCoreApplication::arguments();
    if (args.contains(u"--child"_s))
        return child(args);
    return parent(args);
}
