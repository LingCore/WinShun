// Delta updates (src/app/UpdateDelta.h): packages made by the release tool's
// code and applied by Win顺's, the line in the release notes, and every way a
// package or the files here may not fit (each must fail cleanly, so that the
// full package is downloaded instead).

#include "UpdateDelta.h"
#include "UpdatePackage.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

#include <miniz.h>

using namespace Qt::StringLiterals;
namespace delta = ws::update::delta;

namespace {

QByteArray randomBytes(qsizetype size, quint32 seed)
{
    QRandomGenerator generator(seed);
    QByteArray bytes(size, Qt::Uninitialized);
    for (qsizetype i = 0; i < size; ++i)
        bytes[i] = static_cast<char>(generator.bounded(256));
    return bytes;
}

void write(const QString& path, const QByteArray& bytes)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(bytes), bytes.size());
}

QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Every file under `folder` without "data": relative path -> bytes.
QMap<QString, QByteArray> tree(const QString& folder)
{
    QMap<QString, QByteArray> files;
    const QDir root(folder);
    QDirIterator it(folder, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString relative = root.relativeFilePath(it.next());
        if (!relative.startsWith(u"data/"_s))
            files.insert(relative, read(it.filePath()));
    }
    return files;
}

// A zip's entries, changed by `edit`, written again.
QByteArray rezip(const QByteArray& zip, const std::function<void(QMap<QByteArray, QByteArray>&)>& edit)
{
    QMap<QByteArray, QByteArray> entries;
    mz_zip_archive reader {};
    mz_zip_reader_init_mem(&reader, zip.constData(), static_cast<size_t>(zip.size()), 0);
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&reader); ++i) {
        mz_zip_archive_file_stat stat;
        mz_zip_reader_file_stat(&reader, i, &stat);
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&reader, i, &size, 0);
        entries.insert(QByteArray(stat.m_filename), QByteArray(static_cast<const char*>(data), static_cast<qsizetype>(size)));
        mz_free(data);
    }
    mz_zip_reader_end(&reader);
    edit(entries);
    mz_zip_archive writer {};
    mz_zip_writer_init_heap(&writer, 0, 0);
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
        mz_zip_writer_add_mem(&writer, it.key().constData(), it.value().constData(), static_cast<size_t>(it.value().size()), 0);
    void* buffer = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&writer, &buffer, &size);
    QByteArray out(static_cast<const char*>(buffer), static_cast<qsizetype>(size));
    mz_free(buffer);
    mz_zip_writer_end(&writer);
    return out;
}

QByteArray editManifest(const QByteArray& zip, const std::function<void(QJsonObject&)>& edit)
{
    return rezip(zip, [&](QMap<QByteArray, QByteArray>& entries) {
        QJsonObject manifest = QJsonDocument::fromJson(entries.value(delta::kManifestEntry)).object();
        edit(manifest);
        entries.insert(delta::kManifestEntry, QJsonDocument(manifest).toJson());
    });
}

} // namespace

class TestUpdateDelta : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_old, m_new, m_setup, m_zip;
    QByteArray m_package;

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_old = m_dir.filePath(u"old"_s);
        m_new = m_dir.filePath(u"new"_s);
        // A program that changes a little, a library that does not, a file
        // that changes wholly, one that comes and one that goes; the user's
        // data, which is never part of a package.
        const QByteArray program = randomBytes(3 << 20, 1);
        QByteArray newProgram = program;
        newProgram.replace(1000, 64, randomBytes(64, 2));
        newProgram.insert(2 << 20, randomBytes(4096, 3));
        write(m_old + u"/WinShun.exe"_s, program);
        write(m_new + u"/WinShun.exe"_s, newProgram);
        write(m_old + u"/Qt6Core.dll"_s, randomBytes(200000, 4));
        write(m_new + u"/Qt6Core.dll"_s, read(m_old + u"/Qt6Core.dll"_s));
        write(m_old + u"/WinShunService.exe"_s, randomBytes(50000, 5));
        write(m_new + u"/WinShunService.exe"_s, read(m_old + u"/WinShunService.exe"_s));
        const QByteArray qml = randomBytes(20000, 7);
        write(m_old + u"/qml/Main.qml"_s, qml);
        write(m_new + u"/qml/Main.qml"_s, QByteArray(qml).replace(500, 10, "visible: 1"));
        write(m_new + u"/plugins/new/plugin.json"_s, "{\"id\": \"new\"}");
        write(m_new + u"/empty.txt"_s, {});
        write(m_old + u"/gone.txt"_s, "old only");
        write(m_new + u"/data/settings.ini"_s, "user's");
        m_setup = m_dir.filePath(u"delta-setup.exe"_s);
        write(m_setup, randomBytes(100000, 6));
        m_zip = m_dir.filePath(u"WinShun-0.35.0-x64-delta-from-0.34.2.zip"_s);
        delta::MadeStats stats;
        QCOMPARE(delta::make(m_old, m_new, u"0.34.2"_s, u"0.35.0"_s, m_setup, m_zip, &stats), QString());
        QCOMPARE(stats.same, 2);
        QCOMPARE(stats.patched, 2); // WinShun.exe, Main.qml
        QCOMPARE(stats.added, 2); // plugin.json, empty.txt
        m_package = read(m_zip);
        QCOMPARE(m_package.size(), stats.bytes);
        // The program's patch is a small part of it.
        QVERIFY2(m_package.size() < 100000 + 200000, qPrintable(QString::number(m_package.size())));
    }

    void appliesToTheSameFiles()
    {
        const QString out = m_dir.filePath(u"out-portable"_s);
        QCOMPARE(delta::apply(m_package, m_old, out), QString());
        const auto built = tree(out);
        QCOMPARE(built, tree(m_new));
        QVERIFY(!built.contains(u"gone.txt"_s));
        QVERIFY(!QFileInfo::exists(out + u"/data"_s));
    }

    void setupHeldWithItsFiles()
    {
        const QString out = m_dir.filePath(u"out-setup/payload"_s);
        const QString setup = m_dir.filePath(u"out-setup/WinShun-setup.exe"_s);
        std::vector<std::unique_ptr<ws::update::HeldFile>> hold;
        delta::ApplyOptions options;
        options.setupTarget = setup;
        options.hold = &hold;
        QCOMPARE(delta::apply(m_package, m_old, out, options), QString());
        QCOMPARE(read(setup), read(m_setup));
        QCOMPARE(tree(out), tree(m_new));
        QCOMPARE(hold.size(), size_t(7)); // six files and the Setup
        // Nobody changes them while Setup runs.
        QFile program(out + u"/WinShun.exe"_s);
        QVERIFY(!program.open(QIODevice::ReadWrite));
        QVERIFY(!QFile::remove(setup));
        QVERIFY(!QFile::rename(out + u"/Qt6Core.dll"_s, out + u"/moved.dll"_s));
        QVERIFY(program.open(QIODevice::ReadOnly)); // Setup reads them
        program.close();
        hold.clear();
        QVERIFY(QFile::remove(setup));
    }

    void skipsWhatThisCopyHasNot()
    {
        const QString base = m_dir.filePath(u"base-user"_s);
        QVERIFY(QDir().mkpath(base));
        for (const QString& name : tree(m_old).keys())
            write(base + u'/' + name, read(m_old + u'/' + name));
        QVERIFY(QFile::remove(base + u"/WinShunService.exe"_s));
        const QString out = m_dir.filePath(u"out-user"_s);
        QVERIFY(!delta::apply(m_package, base, out).isEmpty()); // needed, not there
        QVERIFY(!QFileInfo::exists(out));
        delta::ApplyOptions options;
        options.skip = {u"WinShunService.exe"_s};
        QCOMPARE(delta::apply(m_package, base, out, options), QString());
        QVERIFY(!QFileInfo::exists(out + u"/WinShunService.exe"_s));
    }

    void failsCleanlyWhenFilesHereDiffer()
    {
        const QString base = m_dir.filePath(u"base-changed"_s);
        for (const QString& name : tree(m_old).keys())
            write(base + u'/' + name, read(m_old + u'/' + name));
        // Unchanged in the release, changed here (the antivirus, a hand).
        write(base + u"/Qt6Core.dll"_s, "patched by someone");
        const QString out = m_dir.filePath(u"out-changed"_s);
        const QString setup = m_dir.filePath(u"setup-changed.exe"_s);
        std::vector<std::unique_ptr<ws::update::HeldFile>> hold;
        delta::ApplyOptions options;
        options.setupTarget = setup;
        options.hold = &hold;
        QVERIFY(delta::apply(m_package, base, out, options).contains(u"Qt6Core.dll"_s));
        QVERIFY(!QFileInfo::exists(out));
        QVERIFY(!QFileInfo::exists(setup));
        QVERIFY(hold.empty());
        // The program patched against another program.
        write(base + u"/Qt6Core.dll"_s, read(m_old + u"/Qt6Core.dll"_s));
        write(base + u"/WinShun.exe"_s, randomBytes(3 << 20, 99));
        QVERIFY(delta::apply(m_package, base, out).contains(u"WinShun.exe"_s));
        QVERIFY(!QFileInfo::exists(out));
        // A file gone.
        write(base + u"/WinShun.exe"_s, read(m_old + u"/WinShun.exe"_s));
        QVERIFY(QFile::remove(base + u"/qml/Main.qml"_s));
        QVERIFY(!delta::apply(m_package, base, out).isEmpty());
        QVERIFY(!QFileInfo::exists(out));
    }

    void refusesBadPackages_data()
    {
        QTest::addColumn<QByteArray>("package");
        QTest::addColumn<QString>("why");
        const auto file = [](QJsonObject& m, const QString& path, const std::function<void(QJsonObject&)>& edit) {
            QJsonArray files = m.value(u"files"_s).toArray();
            for (qsizetype i = 0; i < files.size(); ++i) {
                QJsonObject f = files[i].toObject();
                if (f.value(u"path"_s).toString() == path) {
                    edit(f);
                    files[i] = f;
                }
            }
            m.insert(u"files"_s, files);
        };
        QTest::newRow("not a zip") << QByteArray("PK nonsense") << u"not a zip"_s;
        QTest::newRow("schema") << editManifest(m_package, [](QJsonObject& m) { m.insert(u"schema"_s, 2); }) << u"schema"_s;
        QTest::newRow("up and out") << editManifest(m_package, [&](QJsonObject& m) {
            file(m, u"empty.txt"_s, [](QJsonObject& f) { f.insert(u"path"_s, u"../evil.exe"_s); });
        }) << u"unsafe"_s;
        QTest::newRow("absolute") << editManifest(m_package, [&](QJsonObject& m) {
            file(m, u"empty.txt"_s, [](QJsonObject& f) { f.insert(u"path"_s, u"C:/Windows/evil.exe"_s); });
        }) << u"unsafe"_s;
        QTest::newRow("user data") << editManifest(m_package, [&](QJsonObject& m) {
            file(m, u"empty.txt"_s, [](QJsonObject& f) { f.insert(u"path"_s, u"Data/settings.ini"_s); });
        }) << u"data"_s;
        QTest::newRow("twice") << editManifest(m_package, [&](QJsonObject& m) {
            file(m, u"empty.txt"_s, [](QJsonObject& f) { f.insert(u"path"_s, u"qt6core.DLL"_s); });
        }) << u"duplicate"_s;
        QTest::newRow("wrong digest") << editManifest(m_package, [&](QJsonObject& m) {
            file(m, u"plugins/new/plugin.json"_s, [](QJsonObject& f) { f.insert(u"sha256"_s, QString(64, u'0')); });
        }) << u"built wrong"_s;
        QTest::newRow("wrong size") << editManifest(m_package, [&](QJsonObject& m) {
            file(m, u"WinShun.exe"_s, [](QJsonObject& f) { f.insert(u"size"_s, f.value(u"size"_s).toInteger() + 1); });
        }) << u"frame size"_s;
        QTest::newRow("no program") << editManifest(m_package, [&](QJsonObject& m) {
            file(m, u"WinShun.exe"_s, [](QJsonObject& f) { f.insert(u"path"_s, u"Other.exe"_s); });
        }) << u"no WinShun.exe"_s;
        QTest::newRow("data missing") << rezip(m_package, [](QMap<QByteArray, QByteArray>& e) { e.remove("d/0"); })
                                      << u"no entry"_s;
        QTest::newRow("patch corrupted") << rezip(m_package, [](QMap<QByteArray, QByteArray>& e) {
            for (auto it = e.begin(); it != e.end(); ++it)
                if (it.key().startsWith("d/") && it.value().size() > 100)
                    it.value()[it.value().size() / 2] = static_cast<char>(it.value()[it.value().size() / 2] ^ 0x5a);
        }) << u"WinShun.exe"_s;
    }

    void refusesBadPackages()
    {
        QFETCH(QByteArray, package);
        QFETCH(QString, why);
        const QString out = m_dir.filePath(u"out-bad"_s);
        const QString result = delta::apply(package, m_old, out);
        QVERIFY2(result.contains(why, Qt::CaseInsensitive), qPrintable(result));
        QVERIFY(!QFileInfo::exists(out));
        QVERIFY(!QFileInfo::exists(m_dir.filePath(u"evil.exe"_s)));
    }

    void refusesAnExistingFolder()
    {
        const QString out = m_dir.filePath(u"exists"_s);
        QVERIFY(QDir().mkpath(out));
        QVERIFY(!delta::apply(m_package, m_old, out).isEmpty());
        QVERIFY(QFileInfo::exists(out));
    }

    void cancels()
    {
        const std::atomic<bool> cancel {true};
        delta::ApplyOptions options;
        options.cancel = &cancel;
        const QString out = m_dir.filePath(u"out-cancel"_s);
        QCOMPARE(delta::apply(m_package, m_old, out, options), u"cancelled"_s);
        QVERIFY(!QFileInfo::exists(out));
    }

    void notesLine()
    {
        const QString sha = QString(64, u'a');
        const QString line = delta::notesLine({{u"0.34.2"_s, u"WinShun-0.35.0-x64-delta-from-0.34.2.zip"_s, 5400000, sha},
            {u"0.34.1"_s, u"WinShun-0.35.0-x64-delta-from-0.34.1.zip"_s, 6100000, QString(64, u'b')}});
        QCOMPARE(line, u"<!-- winshun-delta 1 0.34.2:WinShun-0.35.0-x64-delta-from-0.34.2.zip:5400000:%1 "
                       "0.34.1:WinShun-0.35.0-x64-delta-from-0.34.1.zip:6100000:%2 -->"_s.arg(sha, QString(64, u'b')));
        const auto parsed = delta::parseNotesLine(u"Win顺 0.35.0 …\n\n## 新功能 · What's new\n\n- x\n\n"_s + line + u"\n"_s);
        QCOMPARE(parsed.size(), 2);
        QCOMPARE(parsed[1].from, u"0.34.1"_s);
        QCOMPARE(parsed[1].size, qint64(6100000));
        // Unknown schema, bad fields: nothing, or the bad ones left out.
        QVERIFY(delta::parseNotesLine(QString(line).replace(u"delta 1 "_s, u"delta 2 "_s)).isEmpty());
        QCOMPARE(delta::parseNotesLine(QString(line).replace(u":5400000:"_s, u":-1:"_s)).size(), 1);
        QCOMPARE(delta::parseNotesLine(QString(line).replace(u"0.34.2:"_s, u"0.34:"_s)).size(), 1);
        QCOMPARE(delta::parseNotesLine(QString(line).replace(sha, u"x"_s)).size(), 1);
        // The update dialog shows none of it.
        QVERIFY(ws::release::highlights(u"## 新功能 · What's new\n\n- 🧪 **测试** x\n\n  **Test** y\n\n"_s + line, true).size() == 1);
    }

    void offer()
    {
        ws::release::Info info;
        info.version = u"0.35.0"_s;
        info.notes = u"Win顺 0.35.0.\n\n"_s
            + delta::notesLine({{u"0.34.2"_s, u"WinShun-0.35.0-x64-delta-from-0.34.2.zip"_s, 5400000, QString(64, u'c')},
                {u"0.34.1"_s, u"WinShun-0.34.0-x64-delta-from-0.34.1.zip"_s, 100, QString(64, u'd')}, // another release's
                {u"0.33.0"_s, u"../../evil.zip"_s, 100, QString(64, u'e')}});
        ws::release::Asset full;
        full.name = u"WinShun-0.35.0-x64-setup.exe"_s;
        full.url = u"https://github.com/LingCore/WinShun/releases/download/v0.35.0/WinShun-0.35.0-x64-setup.exe"_s;
        full.mirrors = {u"https://gh-proxy.com/"_s + full.url, u"https://example.com/other-file.exe"_s};
        full.size = 32000000;
        full.sha256 = QString(64, u'f');

        const auto asset = delta::offer(info, full, u"0.34.2"_s);
        QVERIFY(asset);
        QCOMPARE(asset->name, u"WinShun-0.35.0-x64-delta-from-0.34.2.zip"_s);
        QCOMPARE(asset->url, u"https://github.com/LingCore/WinShun/releases/download/v0.35.0/WinShun-0.35.0-x64-delta-from-0.34.2.zip"_s);
        QCOMPARE(asset->mirrors, QStringList {u"https://gh-proxy.com/"_s + asset->url});
        QCOMPARE(asset->size, qint64(5400000));
        QCOMPARE(asset->sha256, QString(64, u'c'));
        QCOMPARE(ws::release::downloadUrls(*asset).last(), asset->url);

        QVERIFY(!delta::offer(info, full, u"0.34.1"_s)); // its name is for another version
        QVERIFY(!delta::offer(info, full, u"0.33.0"_s)); // not a package name
        QVERIFY(!delta::offer(info, full, u"0.30.0"_s)); // none for that one
        ws::release::Info plain = info;
        plain.notes = u"Win顺 0.35.0."_s;
        QVERIFY(!delta::offer(plain, full, u"0.34.2"_s));
        ws::release::Asset elsewhere = full;
        elsewhere.url = u"https://github.com/LingCore/WinShun/releases/download/v0.35.0/setup.exe"_s;
        QVERIFY(!delta::offer(info, elsewhere, u"0.34.2"_s)); // the URL is not the package's
    }
};

QTEST_GUILESS_MAIN(TestUpdateDelta)
#include "tst_updatedelta.moc"
