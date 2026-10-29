// The index service (src/service) end to end, through its pipe client
// (ServiceVolumeAccess): `WinShunService.exe --console --pipe <random>` is
// started as a child (this test runs as administrator, as ctest does here)
// and asked for a real volume's MFT and change journal; what it gives is
// compared with reading the volume directly. The volume is the one holding
// the temporary folder (%TEMP%), or WINSHUN_TEST_VOLUME ("D:").
#include "Crawler.h"
#include "FileIndex.h"
#include "IndexFolder.h"
#include "IndexService.h"
#include "Ntfs.h"
#include "ServiceLocation.h"
#include "ServiceVolumeAccess.h"
#include "Snapshot.h"
#include "VolumeProtocol.h"
#include "Win32Util.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <winioctl.h>

#include <atomic>
#include <map>
#include <random>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace ws;
using namespace Qt::StringLiterals;

namespace {

bool elevated()
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

// Every record of an MFT, each reduced to a hash of what the index uses.
struct Digest {
    std::uint64_t recordCount = 0;
    std::uint64_t bytes = 0;
    std::uint64_t inUse = 0;
    std::uint64_t invalid = 0;
    std::uint64_t names = 0;
    std::uint64_t blocks = 0;
    std::vector<std::uint64_t> hashes; // by record number; 0: not handed out
    double ms = 0;
};

constexpr std::uint64_t kFnvBasis = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void mix(std::uint64_t& h, const void* data, std::size_t n)
{
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
}

template <typename T> void mix(std::uint64_t& h, const T& v)
{
    mix(h, &v, sizeof v);
}

Digest digest(ntfs::RecordSource& source)
{
    Digest d;
    QElapsedTimer t;
    t.start();
    d.recordCount = source.recordCount();
    d.bytes = source.bytes();
    d.hashes.assign(static_cast<std::size_t>(d.recordCount), 0);
    ntfs::FileRecord record;
    while (source.next()) {
        ++d.blocks;
        for (std::size_t i = 0; i < source.blockRecords(); ++i) {
            const std::uint64_t number = std::uint64_t {source.firstRecord()} + i;
            std::uint64_t h = kFnvBasis;
            mix(h, number);
            if (!source.parse(i, record)) {
                ++d.invalid;
                mix(h, std::uint8_t {0xFF});
            } else if (!record.inUse) {
                mix(h, std::uint8_t {0xFE});
            } else {
                ++d.inUse;
                mix(h, record.baseRecord);
                mix(h, record.attributes);
                mix(h, record.modified);
                mix(h, record.created);
                mix(h, record.directory);
                mix(h, record.hasAttributeList);
                for (const auto& n : record.names) {
                    ++d.names;
                    mix(h, n.parent);
                    mix(h, n.name.data(), n.name.size() * 2);
                }
            }
            if (number < d.hashes.size())
                d.hashes[static_cast<std::size_t>(number)] = h;
        }
    }
    d.ms = static_cast<double>(t.nsecsElapsed()) / 1e6;
    return d;
}

struct Change {
    std::int64_t usn;
    std::uint32_t reason;
    std::u16string name;
    bool operator==(const Change&) const = default;
};

std::vector<Change> changesOf(const std::vector<ntfs::UsnRecord>& records)
{
    std::vector<Change> out;
    for (const auto& r : records)
        out.push_back({r.usn, r.reason, std::u16string(r.name)});
    return out;
}

bool hasName(const std::vector<Change>& changes, std::u16string_view name, std::uint32_t reason)
{
    for (const auto& c : changes) {
        if (c.name == name && (c.reason & reason))
            return true;
    }
    return false;
}

// Reads on (waiting for something new each time) until a record names
// `name` with `reason`, or `limitMs` pass; `usn` moves on. The volume may
// be a busy one (C:), where every read completes with other programs'
// changes, so what we did may take a few batches to show up.
bool readUntil(ntfs::JournalSource& follow, std::int64_t& usn, std::u16string_view name, std::uint32_t reason,
    int limitMs = 5000)
{
    QElapsedTimer t;
    t.start();
    std::vector<ntfs::UsnRecord> records;
    std::int64_t next = 0;
    while (t.elapsed() < limitMs) {
        follow.start(usn, true);
        if (::WaitForSingleObject(follow.event(), static_cast<DWORD>(limitMs)) != WAIT_OBJECT_0)
            return false;
        if (!follow.finish(records, next))
            return false;
        usn = next;
        if (hasName(changesOf(records), name, reason))
            return true;
    }
    return false;
}

void touch(const QString& path)
{
    const HANDLE h = ::CreateFileW(path.toStdWString().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    QVERIFY(h != INVALID_HANDLE_VALUE);
    ::CloseHandle(h);
}

// The service's access, counting the MFT reads asked of it: whether the
// index caught up through the journal or read the volume again.
struct CountingAccess final : ntfs::VolumeAccess {
    ntfs::ServiceVolumeAccess inner;
    std::atomic<int> mftOpens {0};

    explicit CountingAccess(std::wstring pipe) : inner(std::move(pipe)) {}
    Kind kind() const noexcept override { return Kind::Service; }
    std::unique_ptr<ntfs::RecordSource> openMft(std::wstring_view root, std::uint32_t serial, bool lowPriority) override
    {
        ++mftOpens;
        return inner.openMft(root, serial, lowPriority);
    }
    std::optional<ntfs::JournalInfo> queryJournal(std::wstring_view root, std::uint32_t serial) override
    {
        return inner.queryJournal(root, serial);
    }
    std::unique_ptr<ntfs::JournalSource> openJournal(std::wstring_view root, std::uint32_t serial,
        std::uint64_t journalId) override
    {
        return inner.openJournal(root, serial, journalId);
    }
    void closeVolume(std::wstring_view root) override { inner.closeVolume(root); }
    std::unique_ptr<ntfs::BulkSource> openBulkRead(std::wstring_view root, std::uint32_t serial, bool quiet) override
    {
        return inner.openBulkRead(root, serial, quiet);
    }
};

// Whether `path` is in the index (with its read lock, as the app looks).
bool indexed(const IndexService& service, const QString& path)
{
    const std::shared_ptr<FileIndex> index = service.index();
    const auto lock = index->readLock();
    return index->findPath(path.toStdWString()) != kNoEntry;
}

// A connection of our own, for frames the client would never send.
struct RawClient {
    volproto::Channel channel;

    bool connect(const QString& pipe)
    {
        const std::wstring path = volproto::fullPipePath(pipe.toStdWString());
        for (int attempt = 0; attempt < 3; ++attempt) {
            const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                channel = volproto::Channel(h);
                return true;
            }
            // Between one client accepted and the next instance made: wait for it.
            if (::GetLastError() != ERROR_PIPE_BUSY || !::WaitNamedPipeW(path.c_str(), 2000))
                return false;
        }
        return false;
    }
    bool hello(std::uint16_t major, volproto::Type& reply, std::vector<std::byte>& payload)
    {
        volproto::Hello hello;
        hello.major = major;
        volproto::Writer w;
        encode(w, hello);
        return channel.write(w.finish(volproto::Type::Hello), 5000) && channel.read(reply, payload, 5000);
    }
};

} // namespace

class ServiceTest : public QObject {
    Q_OBJECT

    QString m_exe = QString::fromUtf8(WINSHUN_SERVICE_EXE);
    QString m_pipe;
    bool m_external = false; // WINSHUN_TEST_PIPE: a server someone else started (--no-privileges, say)
    std::unique_ptr<QProcess> m_service;
    std::wstring m_root; // "C:"
    std::uint32_t m_serial = 0;
    QString m_scratch; // a folder on that volume

    void startService()
    {
        if (m_external)
            return;
        m_service = std::make_unique<QProcess>();
        m_service->setProgram(m_exe);
        m_service->setArguments({u"--console"_s, u"--pipe"_s, m_pipe});
        m_service->setProcessChannelMode(QProcess::SeparateChannels);
        m_service->start();
        QVERIFY2(m_service->waitForStarted(5000), qPrintable(m_service->errorString()));
        QByteArray out;
        QElapsedTimer t;
        t.start();
        while (!out.contains("ready") && t.elapsed() < 10000 && m_service->state() == QProcess::Running) {
            m_service->waitForReadyRead(200);
            out += m_service->readAllStandardOutput();
        }
        QVERIFY2(out.contains("ready"), qPrintable(u"service did not come up: "_s + QString::fromUtf8(out)
            + QString::fromUtf8(m_service->readAllStandardError())));
    }

    void stopService()
    {
        if (!m_service)
            return;
        m_service->kill();
        m_service->waitForFinished(5000);
        const QByteArray log = m_service->readAllStandardError();
        if (!log.isEmpty())
            qInfo().noquote() << "service log:\n" << QString::fromUtf8(log).remove(u'\r').trimmed();
        m_service.reset();
    }

private slots:
    void initTestCase()
    {
        if (!elevated())
            QSKIP("runs as administrator only (the service reads the volumes)");
        QVERIFY2(QFile::exists(m_exe), qPrintable(m_exe));
        m_pipe = qEnvironmentVariable("WINSHUN_TEST_PIPE");
        m_external = !m_pipe.isEmpty();
        if (!m_external)
            m_pipe = u"WinShun.Volumes.test."_s + QUuid::createUuid().toString(QUuid::Id128).left(12);

        // The volume: WINSHUN_TEST_VOLUME, else the temporary folder's.
        QString root = qEnvironmentVariable("WINSHUN_TEST_VOLUME").left(2).toUpper();
        if (root.size() != 2)
            root = QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).left(2).toUpper();
        m_root = root.toStdWString();
        bool ntfs = false;
        for (const VolumeInfo& v : listLocalVolumes(true)) {
            if (win32::equalsIgnoreCase(v.root, m_root)) {
                m_serial = v.serial;
                ntfs = v.ntfs;
            }
        }
        QVERIFY2(ntfs && m_serial != 0, qPrintable(root + u" is not a local NTFS volume"_s));
        // A scratch folder on that volume: under TEMP when it is TEMP's, else at its root.
        const QString temp = QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::TempLocation));
        m_scratch = (temp.left(2).compare(root, Qt::CaseInsensitive) == 0 ? temp : root)
            + u"\\wsservice-test-"_s + QString::number(::GetCurrentProcessId());
        QVERIFY(QDir().mkpath(m_scratch));
        qInfo("volume %ls (serial %08X), scratch %ls", m_root.c_str(), m_serial, qUtf16Printable(m_scratch));
        startService();
    }

    void cleanupTestCase()
    {
        stopService();
        // Skipped (not elevated) before it was set: QDir("") is the current
        // folder, and the build folder the test ran in went with it.
        if (!m_scratch.isEmpty())
            QDir(m_scratch).removeRecursively();
    }

    void handshake()
    {
        ntfs::ServiceVolumeAccess access(m_pipe.toStdWString());
        QVERIFY2(access.probe(), qPrintable(QString::fromStdWString(access.lastError())));
        QCOMPARE(QString::fromStdWString(access.serviceVersion()), QString::fromUtf8(WINSHUN_VERSION));
        QVERIFY(access.lastError().empty());
        // Not there at all: told so, no hang.
        ntfs::ServiceVolumeAccess nothing(L"WinShun.Volumes.nowhere");
        QVERIFY(!nothing.probe());
        QVERIFY(QString::fromStdWString(nothing.lastError()).contains(u"not running"));
    }

    void dialogNavRefusedForOthers()
    {
        // GitHub #104: dialog navigation is for Win顺's own UI only (WinShun.exe
        // beside the service). This test process is not, so it is neither told
        // of it (whether or not the service could launch the helper: an admin
        // --console service cannot, one running as SYSTEM can) nor given it.
        RawClient raw;
        QVERIFY(raw.connect(m_pipe));
        volproto::Type reply {};
        std::vector<std::byte> payload;
        QVERIFY(raw.hello(volproto::kMajor, reply, payload));
        QCOMPARE(reply, volproto::Type::HelloOk);
        volproto::HelloOk ok;
        QVERIFY(decode(payload, ok));
        QVERIFY(!(ok.capabilities & volproto::kCapabilityDialogNav));
        volproto::Writer w;
        w.u64(0x1234);
        w.string(std::wstring(L"C:\\"));
        QVERIFY(raw.channel.write(w.finish(volproto::Type::OpenDialogNav), 5000));
        QVERIFY(raw.channel.read(reply, payload, 5000));
        QCOMPARE(reply, volproto::Type::Error);
        volproto::Error error;
        QVERIFY(decode(payload, error));
        QCOMPARE(error.code, static_cast<std::uint32_t>(ERROR_ACCESS_DENIED));
    }

    void keyListenerForAnyClient()
    {
        // The UIAccess double-tap listener, unlike dialog navigation, is for
        // any client the pipe lets in (a portable copy keeps double taps over
        // admin windows): advertised wherever the service can launch it (as
        // SYSTEM, WINSHUN_TEST_PIPE), never refused for who asks. Such a
        // service tells this client, not beside it, that it keeps the
        // navigator from it.
        RawClient raw;
        QVERIFY(raw.connect(m_pipe));
        volproto::Type reply {};
        std::vector<std::byte> payload;
        QVERIFY(raw.hello(volproto::kMajor, reply, payload));
        QCOMPARE(reply, volproto::Type::HelloOk);
        volproto::HelloOk ok;
        QVERIFY(decode(payload, ok));
        const bool listener = ok.capabilities & volproto::kCapabilityKeyListener;
        QCOMPARE(bool(ok.capabilities & volproto::kCapabilityDialogNavBesideOnly), listener);
        QVERIFY(!(ok.capabilities & volproto::kCapabilityDialogNav));
        if (listener) {
            qInfo("the service launches the listener: tst_keylistener's serviceWorker runs it");
            return;
        }
        volproto::Writer w;
        w.u64(0x1234);
        QVERIFY(raw.channel.write(w.finish(volproto::Type::OpenKeyListener), 5000));
        QVERIFY(raw.channel.read(reply, payload, 5000));
        QCOMPARE(reply, volproto::Type::Error);
        volproto::Error error;
        QVERIFY(decode(payload, error));
        QCOMPARE(error.code, static_cast<std::uint32_t>(ERROR_NOT_SUPPORTED)); // cannot, not "not for you"
    }

    void dialogNavRejectsBadFolder()
    {
        // The folder is checked as a string before anything else, the caller
        // included: no helper command line is ever built from a relative,
        // quoted, control-character, device or overlong path.
        const std::wstring bad[] = {L"relative\\x", L"C:", L"C:\\a\"b", L"C:\\a\tb", L"\\\\.\\PhysicalDrive0",
            L"C:\\a\\..\\b", std::wstring(L"C:\\") + std::wstring(300, L'a')};
        for (const std::wstring& folder : bad) {
            RawClient raw;
            QVERIFY(raw.connect(m_pipe));
            volproto::Type reply {};
            std::vector<std::byte> payload;
            QVERIFY(raw.hello(volproto::kMajor, reply, payload));
            QCOMPARE(reply, volproto::Type::HelloOk);
            volproto::Writer w;
            w.u64(0x1234);
            w.string(folder);
            QVERIFY(raw.channel.write(w.finish(volproto::Type::OpenDialogNav), 5000));
            QVERIFY(raw.channel.read(reply, payload, 5000));
            QCOMPARE(reply, volproto::Type::Error);
            volproto::Error error;
            QVERIFY(decode(payload, error));
            QCOMPARE(error.code, static_cast<std::uint32_t>(ERROR_INVALID_PARAMETER));
        }
    }

    void wrongProtocolRefused()
    {
        RawClient raw;
        QVERIFY(raw.connect(m_pipe));
        volproto::Type reply {};
        std::vector<std::byte> payload;
        QVERIFY(raw.hello(volproto::kMajor + 1, reply, payload));
        QCOMPARE(reply, volproto::Type::HelloRefused);
        volproto::HelloRefused refused;
        QVERIFY(decode(payload, refused));
        QCOMPARE(refused.reason, static_cast<std::uint32_t>(volproto::Refusal::Protocol));
        // The connection is over: nothing more comes.
        QVERIFY(!raw.channel.read(reply, payload, 2000));
    }

    void malformedFramesDropped()
    {
        // A frame longer than any the service reads: dropped unread.
        RawClient raw;
        QVERIFY(raw.connect(m_pipe));
        volproto::Type reply {};
        std::vector<std::byte> payload;
        QVERIFY(raw.hello(volproto::kMajor, reply, payload));
        QCOMPARE(reply, volproto::Type::HelloOk);
        std::byte header[8] {};
        const std::uint32_t length = volproto::kMaxPayloadBytes + 1;
        const std::uint16_t type = static_cast<std::uint16_t>(volproto::Type::OpenMft);
        std::memcpy(header, &length, 4);
        std::memcpy(header + 4, &type, 2);
        QVERIFY(raw.channel.write(header, 2000));
        QVERIFY(!raw.channel.read(reply, payload, 3000)); // gone
        QVERIFY(raw.channel.lastError() == ERROR_BROKEN_PIPE || raw.channel.lastError() == ERROR_PIPE_NOT_CONNECTED
            || raw.channel.lastError() == ERROR_NO_DATA);

        // A request whose root is not a drive: refused with Error.
        RawClient bad;
        QVERIFY(bad.connect(m_pipe));
        QVERIFY(bad.hello(volproto::kMajor, reply, payload));
        volproto::Writer w;
        encode(w, volproto::OpenMft {L"C:\\Users", 0, false});
        QVERIFY(bad.channel.write(w.finish(volproto::Type::OpenMft), 2000));
        QVERIFY(bad.channel.read(reply, payload, 5000));
        QCOMPARE(reply, volproto::Type::Error);
        volproto::Error error;
        QVERIFY(decode(payload, error));
        QCOMPARE(error.code, std::uint32_t {ERROR_INVALID_DATA});

        // An OpenMft cut short.
        RawClient cut;
        QVERIFY(cut.connect(m_pipe));
        QVERIFY(cut.hello(volproto::kMajor, reply, payload));
        volproto::Writer w2;
        w2.u16(2);
        w2.u16(u'C'); // the name says two units, one follows
        QVERIFY(cut.channel.write(w2.finish(volproto::Type::OpenMft), 2000));
        QVERIFY(cut.channel.read(reply, payload, 5000));
        QCOMPARE(reply, volproto::Type::Error);
    }

    void mftMatchesDirect()
    {
        ntfs::ServiceVolumeAccess access(m_pipe.toStdWString());
        ntfs::DirectVolumeAccess direct;
        // The service's read first, the direct one right after: the volume
        // changes meanwhile (other programs write), so a few records may differ.
        const std::unique_ptr<ntfs::RecordSource> viaService = access.openMft(m_root, m_serial, false);
        QVERIFY(viaService);
        QVERIFY2(viaService->valid(), qPrintable(QString::fromStdWString(viaService->error())));
        const Digest s = digest(*viaService);
        QVERIFY2(viaService->valid(), qPrintable(QString::fromStdWString(viaService->error())));
        QCOMPARE(viaService->bytesDone(), viaService->bytes());
        const std::unique_ptr<ntfs::RecordSource> viaVolume = direct.openMft(m_root, m_serial, false);
        QVERIFY(viaVolume && viaVolume->valid());
        const Digest d = digest(*viaVolume);
        QVERIFY(viaVolume->valid());

        QCOMPARE(s.recordCount, d.recordCount);
        QCOMPARE(s.bytes, d.bytes);
        std::uint64_t same = 0, differ = 0, handedOut = 0;
        for (std::size_t i = 0; i < d.hashes.size(); ++i) {
            if (d.hashes[i] == 0 && s.hashes[i] == 0)
                continue; // left out as free by both
            ++handedOut;
            if (d.hashes[i] == s.hashes[i])
                ++same;
            else
                ++differ;
        }
        qInfo("MFT %ls: %llu records, %llu handed out; service %llu in use, %llu names, %llu blocks, %.0f ms; "
              "direct %llu in use, %llu names, %llu blocks, %.0f ms; %llu records differ (changed meanwhile)",
            m_root.c_str(), static_cast<unsigned long long>(d.recordCount), static_cast<unsigned long long>(handedOut),
            static_cast<unsigned long long>(s.inUse), static_cast<unsigned long long>(s.names),
            static_cast<unsigned long long>(s.blocks), s.ms, static_cast<unsigned long long>(d.inUse),
            static_cast<unsigned long long>(d.names), static_cast<unsigned long long>(d.blocks), d.ms,
            static_cast<unsigned long long>(differ));
        QVERIFY(handedOut > 1000);
        QVERIFY2(differ * 10'000 <= handedOut, "more than 0.01% of the records differ");
        QVERIFY(same + differ == handedOut);
        QCOMPARE(s.invalid, d.invalid);
    }

    void mftRefusals()
    {
        ntfs::ServiceVolumeAccess access(m_pipe.toStdWString());
        // Another serial: the drive letter holds another disk, as far as the service knows.
        const std::unique_ptr<ntfs::RecordSource> wrong = access.openMft(m_root, m_serial ^ 0x5A5A, false);
        QVERIFY(wrong && !wrong->valid());
        QVERIFY2(QString::fromStdWString(wrong->error()).contains(u"another volume"),
            qPrintable(QString::fromStdWString(wrong->error())));
        // One MFT read per process and volume at a time.
        const std::unique_ptr<ntfs::RecordSource> first = access.openMft(m_root, m_serial, false);
        QVERIFY(first && first->valid());
        const std::unique_ptr<ntfs::RecordSource> second = access.openMft(m_root, m_serial, false);
        QVERIFY(second && !second->valid());
        QVERIFY2(QString::fromStdWString(second->error()).contains(u"already"),
            qPrintable(QString::fromStdWString(second->error())));
    }

    void journalMatchesDirect()
    {
        ntfs::ServiceVolumeAccess access(m_pipe.toStdWString());
        const std::optional<ntfs::JournalInfo> viaService = access.queryJournal(m_root, m_serial);
        const std::optional<ntfs::JournalInfo> viaVolume = ntfs::queryJournal(m_root);
        QVERIFY(viaService && viaVolume);
        QCOMPARE(viaService->id, viaVolume->id);
        QVERIFY(viaService->nextUsn <= viaVolume->nextUsn);
        QVERIFY(!access.queryJournal(m_root, m_serial ^ 1));

        // Changes in the scratch folder: the service's records are those the volume gives.
        const std::int64_t from = viaVolume->nextUsn;
        touch(m_scratch + u"\\one.txt"_s);
        QVERIFY(QFile::rename(m_scratch + u"\\one.txt"_s, m_scratch + u"\\two.txt"_s));
        QVERIFY(QFile::remove(m_scratch + u"\\two.txt"_s));
        const std::unique_ptr<ntfs::JournalSource> follow = access.openJournal(m_root, m_serial, viaService->id);
        QVERIFY(follow && follow->valid());
        std::vector<ntfs::UsnRecord> records;
        std::int64_t next = 0;
        follow->start(from, false);
        QCOMPARE(::WaitForSingleObject(follow->event(), 5000), DWORD {WAIT_OBJECT_0});
        QVERIFY(follow->finish(records, next));
        QVERIFY(next > from);
        const std::vector<Change> got = changesOf(records);
        QVERIFY(hasName(got, u"one.txt", USN_REASON_FILE_CREATE));
        QVERIFY(hasName(got, u"one.txt", USN_REASON_RENAME_OLD_NAME));
        QVERIFY(hasName(got, u"two.txt", USN_REASON_RENAME_NEW_NAME));
        QVERIFY(hasName(got, u"two.txt", USN_REASON_FILE_DELETE));
        ntfs::JournalReader reader(m_root, viaVolume->id);
        QVERIFY(reader.valid());
        reader.start(from, false);
        std::vector<ntfs::UsnRecord> direct;
        std::int64_t directNext = 0;
        QVERIFY(reader.finish(direct, directNext));
        const std::vector<Change> want = changesOf(direct);
        QVERIFY(want.size() >= got.size()); // more may have come since
        QVERIFY(std::equal(got.begin(), got.end(), want.begin()));
        if (want.size() == got.size())
            QCOMPARE(next, directNext);

        // Waiting for something new, then a cancel, then a read that still lines up.
        std::int64_t usn = next;
        follow->start(usn, true);
        touch(m_scratch + u"\\three.txt"_s);
        QCOMPARE(::WaitForSingleObject(follow->event(), 5000), DWORD {WAIT_OBJECT_0});
        QVERIFY(follow->finish(records, next));
        usn = next;
        if (!hasName(changesOf(records), u"three.txt", USN_REASON_FILE_CREATE))
            QVERIFY(readUntil(*follow, usn, u"three.txt", USN_REASON_FILE_CREATE));
        follow->start(usn, true);
        follow->cancel();
        touch(m_scratch + u"\\four.txt"_s);
        QVERIFY(readUntil(*follow, usn, u"four.txt", USN_REASON_FILE_CREATE));
        // A cancel that races the read: the batch is dropped, the next read
        // starts from the same place and finds the file anyway.
        follow->start(usn, true);
        touch(m_scratch + u"\\five.txt"_s);
        ::WaitForSingleObject(follow->event(), 2000);
        follow->cancel();
        QVERIFY(readUntil(*follow, usn, u"five.txt", USN_REASON_FILE_CREATE));
    }

    void closeVolumeAcknowledged()
    {
        ntfs::ServiceVolumeAccess access(m_pipe.toStdWString());
        const std::optional<ntfs::JournalInfo> journal = access.queryJournal(m_root, m_serial);
        QVERIFY(journal);
        const std::unique_ptr<ntfs::JournalSource> follow = access.openJournal(m_root, m_serial, journal->id);
        QVERIFY(follow && follow->valid());
        std::int64_t usn = journal->nextUsn;
        follow->start(usn, true); // the service waits on the volume (or a busy volume answers at once)
        QElapsedTimer t;
        t.start();
        access.closeVolume(m_root);
        const qint64 ms = t.elapsed();
        qInfo("CloseVolume acknowledged in %lld ms", static_cast<long long>(ms));
        QVERIFY(ms < 2000);
        // The follower was told (after any batch that completed first): its
        // journal is gone to it.
        std::vector<ntfs::UsnRecord> records;
        std::int64_t next = 0;
        bool lost = false;
        for (int i = 0; i < 20 && !lost; ++i) {
            QCOMPARE(::WaitForSingleObject(follow->event(), 2000), DWORD {WAIT_OBJECT_0});
            lost = !follow->finish(records, next);
            if (!lost) {
                usn = next;
                follow->start(usn, true);
            }
        }
        QVERIFY(lost);
        // Nothing held: a CloseVolume now is a no-op, and quick.
        t.restart();
        access.closeVolume(m_root);
        QVERIFY(t.elapsed() < 1000);
    }

    void serviceGoneAndBack()
    {
        if (m_external)
            QSKIP("the server is not ours to stop");
        ntfs::ServiceVolumeAccess::setReconnectWindow(std::chrono::seconds(6));
        ntfs::ServiceVolumeAccess access(m_pipe.toStdWString());
        const std::optional<ntfs::JournalInfo> journal = access.queryJournal(m_root, m_serial);
        QVERIFY(journal);
        const std::unique_ptr<ntfs::JournalSource> follow = access.openJournal(m_root, m_serial, journal->id);
        QVERIFY(follow && follow->valid());
        std::int64_t usn = journal->nextUsn;
        std::vector<ntfs::UsnRecord> records;
        std::int64_t next = 0;

        // The service dies while a read waits: batches that completed before
        // still come; then no "journal lost", just nothing new, and the next
        // reads try again.
        follow->start(usn, true);
        stopService();
        bool quiet = false;
        for (int i = 0; i < 20 && !quiet; ++i) {
            QCOMPARE(::WaitForSingleObject(follow->event(), 5000), DWORD {WAIT_OBJECT_0});
            QVERIFY(follow->finish(records, next));
            quiet = records.empty() && next == usn;
            if (!quiet) {
                usn = next;
                follow->start(usn, true);
            }
        }
        QVERIFY(quiet);
        follow->start(usn, true); // cannot connect: completes at once, with nothing
        QCOMPARE(::WaitForSingleObject(follow->event(), 1000), DWORD {WAIT_OBJECT_0});
        QVERIFY(follow->finish(records, next));
        QVERIFY(records.empty());
        QCOMPARE(next, usn);

        // Back: the next read connects again and goes on from the same USN.
        touch(m_scratch + u"\\while-away.txt"_s);
        startService();
        const std::int64_t before = usn;
        QVERIFY(readUntil(*follow, usn, u"while-away.txt", USN_REASON_FILE_CREATE));
        QVERIFY(usn > before);

        // Away for longer than the window: given up as lost.
        follow->start(usn, true);
        stopService();
        QElapsedTimer t;
        t.start();
        bool lost = false;
        while (!lost && t.elapsed() < 20000) {
            QCOMPARE(::WaitForSingleObject(follow->event(), 5000), DWORD {WAIT_OBJECT_0});
            lost = !follow->finish(records, next);
            if (!lost) {
                usn = next;
                ::Sleep(500); // as followJournals pauses between reads
                follow->start(usn, true);
            }
        }
        qInfo("given up after %lld ms", static_cast<long long>(t.elapsed()));
        QVERIFY(lost);
        QVERIFY(t.elapsed() >= 6000 && t.elapsed() < 12000);
        ntfs::ServiceVolumeAccess::setReconnectWindow(std::chrono::seconds(30));
        startService(); // for the tests after this one
    }

    // ---- IndexService with the service coming up after Win顺 (GitHub #21) ----------

    // Rules that leave out every other volume and, on ours, every folder but
    // the scratch folder's branch: an index small enough to read or walk in a
    // moment, but a real one, from the real MFT and journal.
    CrawlRules scratchRules() const
    {
        CrawlRules rules;
        for (const VolumeInfo& v : listLocalVolumes(true)) {
            if (!win32::equalsIgnoreCase(v.root, m_root))
                rules.excludedPaths.push_back(v.root); // "E:": the whole drive
        }
        QString ancestor = QString::fromStdWString(m_root);
        for (const QString& next : m_scratch.mid(3).split(u'\\', Qt::SkipEmptyParts)) {
            const QFileInfoList siblings
                = QDir(ancestor + u'\\').entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
            for (const QFileInfo& sub : siblings) {
                if (sub.fileName().compare(next, Qt::CaseInsensitive) != 0)
                    rules.excludedPaths.push_back(QDir::toNativeSeparators(sub.absoluteFilePath()).toStdWString());
            }
            ancestor += u'\\' + next;
        }
        return rules;
    }

    IndexService::Options scratchOptions(std::shared_ptr<ntfs::VolumeAccess> access) const
    {
        IndexService::Options options;
        options.rules = scratchRules();
        options.includeRemovable = true; // whatever kind of disk the scratch folder is on
        options.content.enabled = false;
        options.volumeAccess = std::move(access);
        return options;
    }

    // Writes `folder`'s snapshot: the scratch branch read through the service
    // and followed from the journal's position of now, plus a folder that is
    // not on disk, which only a walk or a read of the MFT would drop.
    QString fakeFolder() const { return m_scratch + u"\\not-on-disk"_s; }
    void makeSnapshot(const QString& folder)
    {
        {
            IndexService first(folder, scratchOptions(std::make_shared<ntfs::ServiceVolumeAccess>(m_pipe.toStdWString())));
            first.start();
            QTRY_COMPARE_WITH_TIMEOUT(first.state(), IndexService::State::Ready, 180'000);
            QVERIFY(indexed(first, m_scratch));
            first.shutdown(); // saves the snapshot with the volume's journal position
        }
        const QString path = indexfolder::snapshotPath(folder);
        std::optional<snapshot::Contents> contents = snapshot::load(path);
        QVERIFY(contents && contents->rules);
        QCOMPARE(contents->volumes.size(), std::size_t {1});
        QVERIFY(contents->journals.at(0).journalId != 0);
        const EntryId scratch = contents->index->findPath(m_scratch.toStdWString());
        QVERIFY(scratch != kNoEntry);
        {
            const auto lock = contents->index->writeLock();
            contents->index->add(scratch, "not-on-disk", EntryFlag::Directory);
        }
        const auto lock = contents->index->readLock();
        QVERIFY(snapshot::save(*contents->index, contents->volumes, contents->journals, *contents->rules, path));
    }

    void indexServiceComesLate()
    {
        QTemporaryDir dir;
        makeSnapshot(dir.path());
        // Started with no way to the journals, as at login before the service
        // is up: searchable from the snapshot, and waiting for the service.
        IndexService::Options options = scratchOptions(std::make_shared<ntfs::NoVolumeAccess>());
        options.accessWaitMs = 20'000;
        IndexService late(dir.path(), options);
        late.start();
        QTRY_COMPARE_WITH_TIMEOUT(late.state(), IndexService::State::Ready, 10'000);
        QTest::qWait(500);
        QCOMPARE(late.volumeAccessKind(), ntfs::VolumeAccess::Kind::None);
        QVERIFY(indexed(late, fakeFolder()));
        // The service: caught up from the snapshot's position (a file made
        // now comes through the journal), no MFT read, no walk.
        const auto service = std::make_shared<CountingAccess>(m_pipe.toStdWString());
        late.setVolumeAccess(service);
        QCOMPARE(late.volumeAccessKind(), ntfs::VolumeAccess::Kind::Service);
        const QString made = m_scratch + u"\\came-late.txt"_s;
        touch(made);
        QTRY_VERIFY_WITH_TIMEOUT(indexed(late, made), 15'000);
        QCOMPARE(service->mftOpens.load(), 0);
        QVERIFY(indexed(late, fakeFolder()));
        late.shutdown();
    }

    void indexServiceNeverComes()
    {
        QTemporaryDir dir;
        makeSnapshot(dir.path());
        // Waited for in vain: the volume is walked (the journal position is
        // lost, so whatever the rescan setting says), a little after start.
        IndexService::Options options = scratchOptions(std::make_shared<ntfs::NoVolumeAccess>());
        options.accessWaitMs = 300;
        options.rescanOnStartup = false;
        options.rescanDelayMs = 300;
        const QString made = m_scratch + u"\\walked-in.txt"_s;
        touch(made); // after the snapshot: only the walk can find it
        IndexService walked(dir.path(), options);
        walked.start();
        QTRY_COMPARE_WITH_TIMEOUT(walked.state(), IndexService::State::Ready, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(!indexed(walked, fakeFolder()), 60'000);
        QVERIFY(indexed(walked, made));
        QCOMPARE(walked.volumeAccessKind(), ntfs::VolumeAccess::Kind::None);
        // The service after all: the position is gone, so the MFT is read
        // once, and the volume followed from there.
        const auto service = std::make_shared<CountingAccess>(m_pipe.toStdWString());
        walked.setVolumeAccess(service);
        QTRY_COMPARE_WITH_TIMEOUT(service->mftOpens.load(), 1, 180'000);
        QTRY_VERIFY_WITH_TIMEOUT(walked.readingVolumes().empty() && walked.state() == IndexService::State::Ready, 180'000);
        QVERIFY(indexed(walked, made));
        const QString after = m_scratch + u"\\after-the-read.txt"_s;
        touch(after);
        QTRY_VERIFY_WITH_TIMEOUT(indexed(walked, after), 15'000);
        QCOMPARE(service->mftOpens.load(), 1);
        walked.shutdown();
    }

    void connectionLimit()
    {
        // 16 connections per process; the 17th is refused.
        std::vector<std::unique_ptr<RawClient>> held;
        volproto::Type reply {};
        std::vector<std::byte> payload;
        for (int i = 0; i < 16; ++i) {
            auto raw = std::make_unique<RawClient>();
            QVERIFY(raw->connect(m_pipe));
            QVERIFY(raw->hello(volproto::kMajor, reply, payload));
            QCOMPARE(reply, volproto::Type::HelloOk);
            held.push_back(std::move(raw));
        }
        RawClient extra;
        QVERIFY(extra.connect(m_pipe));
        QVERIFY(extra.hello(volproto::kMajor, reply, payload));
        QCOMPARE(reply, volproto::Type::HelloRefused);
        volproto::HelloRefused refused;
        QVERIFY(decode(payload, refused));
        QCOMPARE(refused.reason, static_cast<std::uint32_t>(volproto::Refusal::TooManyConnections));
        held.clear();
        // Let go: room again (the service notices the closed ones).
        QTest::qWait(300);
        ntfs::ServiceVolumeAccess access(m_pipe.toStdWString());
        QVERIFY2(access.probe(), qPrintable(QString::fromStdWString(access.lastError())));
    }

    // ---- bulk reads (the content index's first build) --------------------------

    // Files of several shapes in a folder of the scratch, written and
    // flushed, read off the volume through the service by their records:
    // the same bytes as ReadFile gives; what the service will not read
    // (compressed, not this user's to read, not there) said so.
    void bulkReadMatchesFiles()
    {
        const QString folder = m_scratch + u"\\bulk"_s;
        QVERIFY(QDir().mkpath(folder));
        std::mt19937_64 rng(20261015);
        const auto randomText = [&](int n) {
            QByteArray out;
            out.reserve(n);
            const char* words[] = {"alpha ", "beta ", "gamma\n", "报告 ", "合同\n", "delta "};
            while (out.size() < n)
                out.append(words[rng() % std::size(words)]);
            out.truncate(n);
            return out;
        };
        const auto write = [&](const QString& name, const QByteArray& bytes, DWORD fsctl = 0) {
            const QString path = folder + u"\\"_s + name;
            const HANDLE h = ::CreateFileW(path.toStdWString().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            QVERIFY(h != INVALID_HANDLE_VALUE);
            DWORD returned = 0;
            if (fsctl == FSCTL_SET_COMPRESSION) {
                USHORT format = COMPRESSION_FORMAT_DEFAULT;
                QVERIFY(::DeviceIoControl(h, FSCTL_SET_COMPRESSION, &format, sizeof format, nullptr, 0, &returned, nullptr));
            } else if (fsctl == FSCTL_SET_SPARSE) {
                QVERIFY(::DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr));
            }
            DWORD written = 0;
            QVERIFY(::WriteFile(h, bytes.constData(), static_cast<DWORD>(bytes.size()), &written, nullptr));
            QCOMPARE(written, static_cast<DWORD>(bytes.size()));
            QVERIFY(::FlushFileBuffers(h));
            ::CloseHandle(h);
        };
        const QByteArray tiny = QByteArray("a resident file\n"); // not `small`: rpcndr.h defines that
        const QByteArray medium = randomText(100'000);
        const QByteArray large = randomText(3'000'000);
        write(u"small.txt"_s, tiny);
        write(u"medium.txt"_s, medium);
        write(u"large.txt"_s, large);
        write(u"empty.txt"_s, QByteArray());
        write(u"packed.txt"_s, randomText(300'000), FSCTL_SET_COMPRESSION);
        // Sparse: 64 KB, a hole up to 1 MB, 64 KB.
        {
            const QString path = folder + u"\\holes.bin"_s;
            const HANDLE h = ::CreateFileW(path.toStdWString().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            QVERIFY(h != INVALID_HANDLE_VALUE);
            DWORD returned = 0;
            QVERIFY(::DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr));
            const QByteArray piece = randomText(64 << 10);
            DWORD written = 0;
            QVERIFY(::WriteFile(h, piece.constData(), static_cast<DWORD>(piece.size()), &written, nullptr));
            LARGE_INTEGER at {};
            at.QuadPart = 1 << 20;
            QVERIFY(::SetFilePointerEx(h, at, nullptr, FILE_BEGIN));
            QVERIFY(::WriteFile(h, piece.constData(), static_cast<DWORD>(piece.size()), &written, nullptr));
            QVERIFY(::FlushFileBuffers(h));
            ::CloseHandle(h);
        }
        // Not this user's to read: a deny ACE for our own SID (admins too).
        const QString deniedPath = folder + u"\\denied.txt"_s;
        write(u"denied.txt"_s, randomText(1000));
        {
            HANDLE token = nullptr;
            QVERIFY(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token));
            BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
            DWORD size = 0;
            QVERIFY(::GetTokenInformation(token, TokenUser, buffer, sizeof buffer, &size));
            ::CloseHandle(token);
            wchar_t* sid = nullptr;
            QVERIFY(::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &sid));
            const std::wstring sddl = L"D:P(D;;FR;;;" + std::wstring(sid) + L")(A;;FA;;;" + std::wstring(sid) + L")";
            ::LocalFree(sid);
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            QVERIFY(::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr));
            BOOL present = FALSE, defaulted = FALSE;
            PACL dacl = nullptr;
            QVERIFY(::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present);
            QCOMPARE(::SetNamedSecurityInfoW(const_cast<wchar_t*>(deniedPath.toStdWString().c_str()), SE_FILE_OBJECT,
                         DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr),
                DWORD {ERROR_SUCCESS});
            ::LocalFree(descriptor);
            // Sanity: we cannot read it ourselves now.
            const HANDLE h = ::CreateFileW(deniedPath.toStdWString().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            QVERIFY(h == INVALID_HANDLE_VALUE);
            QCOMPARE(::GetLastError(), DWORD {ERROR_ACCESS_DENIED});
        }
        // The records reach the disk a moment after the flush (--dirty-window: ~200 ms).
        QTest::qWait(1500);

        // The folder's record number: what the index keeps of folders.
        std::uint32_t parentRecord = 0;
        {
            const HANDLE h = ::CreateFileW(folder.toStdWString().c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            QVERIFY(h != INVALID_HANDLE_VALUE);
            BY_HANDLE_FILE_INFORMATION info {};
            QVERIFY(::GetFileInformationByHandle(h, &info));
            ::CloseHandle(h);
            parentRecord = ntfs::recordOf((std::uint64_t {info.nFileIndexHigh} << 32) | info.nFileIndexLow);
        }
        const std::vector<ntfs::BulkWant> wants {
            {parentRecord, u"small.txt", 16u << 20}, // 0
            {parentRecord, u"medium.txt", 16u << 20}, // 1
            {parentRecord, u"large.txt", 16u << 20}, // 2
            {parentRecord, u"empty.txt", 16u << 20}, // 3
            {parentRecord, u"packed.txt", 16u << 20}, // 4: compressed
            {parentRecord, u"holes.bin", 16u << 20}, // 5: sparse
            {parentRecord, u"denied.txt", 16u << 20}, // 6
            {parentRecord, u"missing.txt", 16u << 20}, // 7: no such file
            {parentRecord, u"medium.txt", 1000}, // 8: over its limit
        };

        ntfs::ServiceVolumeAccess access(m_pipe.toStdWString());
        const std::unique_ptr<ntfs::BulkSource> session = access.openBulkRead(m_root, m_serial, false);
        QVERIFY(session);
        QVERIFY2(session->valid(), qPrintable(QString::fromStdWString(session->error())));
        QVERIFY(session->want(wants));
        std::vector<ntfs::BulkFile> files;
        QVERIFY2(session->scan(files), qPrintable(QString::fromStdWString(session->error())));
        QCOMPARE(files.size(), wants.size());
        std::vector<const ntfs::BulkFile*> byTicket(wants.size(), nullptr);
        for (const ntfs::BulkFile& f : files) {
            QVERIFY(f.ticket < wants.size());
            byTicket[f.ticket] = &f;
        }
        for (const std::uint32_t t : {0u, 1u, 2u, 3u, 5u}) {
            QVERIFY2(byTicket[t]->status == ntfs::BulkStatus::Data, qPrintable(u"ticket %1: status %2 reason %3"_s.arg(t)
                .arg(static_cast<int>(byTicket[t]->status)).arg(ntfs::bulkSkipName(byTicket[t]->reason))));
        }
        QCOMPARE(byTicket[0]->size, static_cast<std::uint64_t>(tiny.size()));
        QCOMPARE(byTicket[2]->size, static_cast<std::uint64_t>(large.size()));
        QCOMPARE(byTicket[4]->status, ntfs::BulkStatus::Fallback);
        QCOMPARE(byTicket[4]->reason, ntfs::BulkSkip::Compressed);
        QCOMPARE(byTicket[6]->status, ntfs::BulkStatus::Denied);
        QCOMPARE(byTicket[7]->status, ntfs::BulkStatus::NotFound);
        QCOMPARE(byTicket[8]->status, ntfs::BulkStatus::TooLarge);
        QCOMPARE(byTicket[8]->size, static_cast<std::uint64_t>(medium.size()));
        // The Data ones come last, in the order to fetch them.
        std::size_t firstData = 0;
        while (firstData < files.size() && files[firstData].status != ntfs::BulkStatus::Data)
            ++firstData;
        for (std::size_t i = firstData; i < files.size(); ++i)
            QCOMPARE(files[i].status, ntfs::BulkStatus::Data);

        // The bytes, in two batches; a denied or spent ticket gives nothing.
        std::map<std::uint32_t, QByteArray> got;
        const auto sink = [&](std::uint32_t ticket, std::vector<std::byte>&& bytes) {
            QVERIFY(!got.contains(ticket));
            got[ticket] = QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
        };
        const std::vector<std::uint32_t> first {0, 1, 6};
        QVERIFY2(session->fetch(first, sink), qPrintable(QString::fromStdWString(session->error())));
        QCOMPARE(got.size(), std::size_t {2});
        const std::vector<std::uint32_t> second {2, 3, 5, 0, 7, 8, 99};
        QVERIFY2(session->fetch(second, sink), qPrintable(QString::fromStdWString(session->error())));
        QCOMPARE(got.size(), std::size_t {5});
        const auto fileBytes = [&](const QString& name) {
            QFile f(folder + u"\\"_s + name);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("could not open");
        };
        QCOMPARE(got[0], tiny);
        QCOMPARE(got[1], medium);
        QCOMPARE(got[2], large);
        QCOMPARE(got[3], QByteArray());
        QCOMPARE(got[5], fileBytes(u"holes.bin"_s)); // the hole read as zeros
        QVERIFY(!got.contains(6));
        qInfo("bulk read: %zu files matched, %d bytes fetched", files.size(), got[0].size() + got[1].size() + got[2].size() + got[5].size());
    }

    void bulkReadRefusals()
    {
        ntfs::ServiceVolumeAccess access(m_pipe.toStdWString());
        // Another serial: the drive letter holds another disk, as far as the service knows.
        const std::unique_ptr<ntfs::BulkSource> wrong = access.openBulkRead(m_root, m_serial ^ 0x5A5A, false);
        QVERIFY(wrong && !wrong->valid());
        QVERIFY2(QString::fromStdWString(wrong->error()).contains(u"another volume"),
            qPrintable(QString::fromStdWString(wrong->error())));
        // One bulk read per process and volume at a time.
        const std::unique_ptr<ntfs::BulkSource> first = access.openBulkRead(m_root, m_serial, false);
        QVERIFY(first && first->valid());
        const std::unique_ptr<ntfs::BulkSource> second = access.openBulkRead(m_root, m_serial, false);
        QVERIFY(second && !second->valid());
        QVERIFY2(QString::fromStdWString(second->error()).contains(u"already"),
            qPrintable(QString::fromStdWString(second->error())));
    }

    // A service without bulk reads (an older one, or --no-bulk): the client
    // sees no capability and takes the usual way; a request anyway is refused.
    void bulkReadWithoutCapability()
    {
        if (m_external)
            QSKIP("needs a server of our own");
        const QString pipe = m_pipe + u".nobulk"_s;
        QProcess old;
        old.setProgram(m_exe);
        old.setArguments({u"--console"_s, u"--pipe"_s, pipe, u"--no-bulk"_s});
        old.start();
        QVERIFY2(old.waitForStarted(5000), qPrintable(old.errorString()));
        QByteArray out;
        QElapsedTimer t;
        t.start();
        while (!out.contains("ready") && t.elapsed() < 10000 && old.state() == QProcess::Running) {
            old.waitForReadyRead(200);
            out += old.readAllStandardOutput();
        }
        QVERIFY2(out.contains("ready"), qPrintable(QString::fromUtf8(out)));
        ntfs::ServiceVolumeAccess access(pipe.toStdWString());
        QVERIFY(access.probe());
        QVERIFY(!access.openBulkRead(m_root, m_serial, false)); // null: the usual way
        RawClient raw;
        QVERIFY(raw.connect(pipe));
        volproto::Type reply {};
        std::vector<std::byte> payload;
        QVERIFY(raw.hello(volproto::kMajor, reply, payload));
        QCOMPARE(reply, volproto::Type::HelloOk);
        volproto::HelloOk ok;
        QVERIFY(decode(payload, ok));
        QCOMPARE(ok.capabilities & volproto::kCapabilityBulkRead, 0u);
        volproto::Writer w;
        encode(w, volproto::OpenBulkRead {m_root, m_serial, false});
        QVERIFY(raw.channel.write(w.finish(volproto::Type::OpenBulkRead), 5000));
        QVERIFY(raw.channel.read(reply, payload, 5000));
        QCOMPARE(reply, volproto::Type::Error);
        old.kill();
        old.waitForFinished(5000);
    }

    // The volume let go of (CloseVolume, as before an eject) ends a bulk
    // read under way: the next fetch says so.
    void bulkReadAbortedByCloseVolume()
    {
        ntfs::ServiceVolumeAccess access(m_pipe.toStdWString());
        const std::unique_ptr<ntfs::BulkSource> session = access.openBulkRead(m_root, m_serial, false);
        QVERIFY(session && session->valid());
        const std::vector<ntfs::BulkWant> wants {{ntfs::kRootRecord, u"nothing-here.txt", 1 << 20}};
        QVERIFY(session->want(wants));
        std::vector<ntfs::BulkFile> files;
        QVERIFY(session->scan(files));
        QCOMPARE(files.size(), std::size_t {1});
        QCOMPARE(files[0].status, ntfs::BulkStatus::NotFound);
        QElapsedTimer t;
        t.start();
        access.closeVolume(m_root);
        QVERIFY(t.elapsed() < 2000);
        const std::vector<std::uint32_t> tickets {0};
        QVERIFY(!session->fetch(tickets, [](std::uint32_t, std::vector<std::byte>&&) {}));
        QVERIFY2(QString::fromStdWString(session->error()).contains(u"let go"),
            qPrintable(QString::fromStdWString(session->error())));
    }

    // Registering the service from a user-writable folder is a local
    // privilege escalation (a portable copy could be replaced and run as
    // SYSTEM), so --install first copies itself into a protected folder. This
    // checks that copy machinery on a real protected folder (a throwaway one
    // under Program Files, never the real "WinShun Service", never the SCM).
    void placingInAProtectedFolder()
    {
        // The built service exe sits in the build tree (user-writable): the
        // very location --install must refuse to register from.
        QVERIFY(!svcloc::pathProtected(m_exe.toStdWString()));

        // A protected folder cannot live under a user-writable parent: refused.
        QTemporaryDir under; // in %TEMP%, which the user can write
        QVERIFY(under.isValid());
        std::wstring error;
        QVERIFY(!svcloc::ensureProtectedDir(QDir::toNativeSeparators(under.filePath(u"svc"_s)).toStdWString(), &error));
        QVERIFY(!error.empty());

        // A throwaway folder under Program Files (protected ancestors).
        std::wstring programFiles = svcloc::protectedServiceDir();
        QVERIFY(!programFiles.empty());
        programFiles.resize(programFiles.size() - std::wstring(L"\\WinShun Service").size());
        const std::wstring dir = programFiles + L"\\WinShunServiceTest-" + std::to_wstring(::GetCurrentProcessId());
        const auto cleanup = qScopeGuard([&] {
            ::DeleteFileW((dir + L"\\copy.exe").c_str());
            ::DeleteFileW((dir + L"\\copy.exe.new").c_str());
            ::RemoveDirectoryW(dir.c_str());
        });
        QVERIFY2(svcloc::ensureProtectedDir(dir, &error), qPrintable(QString::fromStdWString(error)));
        QVERIFY(svcloc::folderProtected(dir)); // the folder and all its ancestors
        // It already exists now: ensureProtectedDir verifies and accepts it.
        QVERIFY(svcloc::ensureProtectedDir(dir, &error));

        std::wstring dest;
        QVERIFY2(svcloc::copyExeInto(dir, L"copy.exe", m_exe.toStdWString(), &dest, &error),
            qPrintable(QString::fromStdWString(error)));
        QCOMPARE(dest, dir + L"\\copy.exe");
        QVERIFY(svcloc::pathProtected(dest)); // a non-admin cannot replace the copy
        {
            QFile source(m_exe), copied(QString::fromStdWString(dest));
            QVERIFY(source.open(QIODevice::ReadOnly) && copied.open(QIODevice::ReadOnly));
            QCOMPARE(copied.readAll(), source.readAll()); // byte for byte
        } // closed: an open handle (no FILE_SHARE_DELETE) would block the replace below
        // Replacing an existing copy (an upgrade, the service stopped) works too.
        QVERIFY2(svcloc::copyExeInto(dir, L"copy.exe", m_exe.toStdWString(), &dest, &error),
            qPrintable(QString::fromStdWString(error)));
        QVERIFY(svcloc::pathProtected(dest));

        // Let users add files to the folder, and nothing more: the copy itself
        // keeps its safe DACL, but a DLL planted beside it would be loaded by
        // a SYSTEM service before System32's, so the copy is no longer
        // protected (because of its folder) and the folder is refused.
        PSECURITY_DESCRIPTOR weak = nullptr;
        QVERIFY(::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;;0x2;;;BU)", SDDL_REVISION_1, &weak, nullptr));
        BOOL present = FALSE, defaulted = FALSE;
        PACL weakDacl = nullptr;
        ::GetSecurityDescriptorDacl(weak, &present, &weakDacl, &defaulted);
        const DWORD set = ::SetNamedSecurityInfoW(const_cast<wchar_t*>(dir.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, weakDacl, nullptr);
        ::LocalFree(weak);
        QCOMPARE(set, DWORD(ERROR_SUCCESS));
        std::wstring why;
        QVERIFY(!svcloc::pathProtected(dest, &why));
        QCOMPARE(QString::fromStdWString(why), QString::fromStdWString(dir)); // the folder, not the file
        QVERIFY(!svcloc::folderProtected(dir));
        QVERIFY(!svcloc::ensureProtectedDir(dir, &error));
    }
};

QTEST_GUILESS_MAIN(ServiceTest)
#include "tst_service.moc"
