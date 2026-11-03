// A Scoop install (core/ScoopInstall): how it is told, which path is registered
// for it ("current", not the version folder `scoop cleanup` deletes), and the
// bucket manifest (bucket/winshun.json) that Scoop reads.

#include "ScoopInstall.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

using namespace Qt::StringLiterals;
using namespace ws;

namespace {

void touch(const QString& path, const QByteArray& text = "{}")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(text);
}

// A version folder as Scoop leaves it.
void makeVersion(const QString& appDir, const QString& version, bool newNames = true)
{
    touch(appDir + u'/' + version + u"/WinShun.exe"_s);
    touch(appDir + u'/' + version + (newNames ? u"/scoop-manifest.json"_s : u"/manifest.json"_s));
    touch(appDir + u'/' + version + (newNames ? u"/scoop-install.json"_s : u"/install.json"_s));
}

// `current` as Scoop makes it: a directory junction (mklink /J).
bool junction(const QString& link, const QString& target)
{
    QDir(link).removeRecursively();
    QFile::remove(link);
    return QProcess::execute(u"cmd.exe"_s, {u"/c"_s, u"mklink"_s, u"/J"_s, QDir::toNativeSeparators(link),
               QDir::toNativeSeparators(target)})
        == 0;
}

QString native(const QString& path)
{
    return QDir::toNativeSeparators(path);
}

} // namespace

class TstScoopInstall : public QObject {
    Q_OBJECT

private slots:
    void detectsBothRecordNames()
    {
        QTemporaryDir root;
        const QString apps = root.path() + u"/scoop/apps/winshun"_s;
        makeVersion(apps, u"0.18.0"_s);
        makeVersion(apps, u"0.17.0"_s, false);
        const auto install = scoop::detect(apps + u"/0.18.0"_s, u"C:/nowhere"_s);
        QVERIFY(install);
        QCOMPARE(install->name, u"winshun"_s);
        QCOMPARE(install->version, u"0.18.0"_s);
        QVERIFY(!install->global);
        QCOMPARE(install->root, native(root.path() + u"/scoop"_s));
        QCOMPARE(install->appDir(), native(apps));
        QCOMPARE(scoop::updateCommand(*install), u"scoop update winshun"_s);
        QVERIFY(scoop::detect(apps + u"/0.17.0"_s)); // manifest.json + install.json of older Scoop
    }

    void notScoop()
    {
        QTemporaryDir root;
        const QString apps = root.path() + u"/scoop/apps/winshun"_s;
        touch(apps + u"/0.18.0/WinShun.exe"_s);
        QVERIFY(!scoop::detect(apps + u"/0.18.0"_s)); // no records
        touch(apps + u"/0.18.0/scoop-install.json"_s);
        QVERIFY(!scoop::detect(apps + u"/0.18.0"_s)); // one of the two
        makeVersion(root.path() + u"/other/winshun"_s, u"0.18.0"_s);
        QVERIFY(!scoop::detect(root.path() + u"/other/winshun/0.18.0"_s)); // not under apps\
        makeVersion(apps, u"0.18.1"_s);
        QVERIFY(!scoop::detect(apps + u"/0.18.1/sub"_s)); // not the program's own folder
        QVERIFY(!scoop::detect(u"C:/Program Files/WinShun"_s));
        QVERIFY(!scoop::detect({}));
    }

    void globalInstall()
    {
        QTemporaryDir root;
        const QString apps = root.path() + u"/ProgramData/scoop/apps/winshun"_s;
        makeVersion(apps, u"0.18.0"_s);
        const auto global = scoop::detect(apps + u"/0.18.0"_s, root.path() + u"/ProgramData/scoop"_s);
        QVERIFY(global);
        QVERIFY(global->global);
        QCOMPARE(scoop::updateCommand(*global), u"scoop update winshun -g"_s);
        const auto user = scoop::detect(apps + u"/0.18.0"_s, root.path() + u"/elsewhere"_s);
        QVERIFY(user && !user->global);
    }

    void currentIsStable()
    {
        QTemporaryDir root;
        const QString apps = root.path() + u"/scoop/apps/winshun"_s;
        makeVersion(apps, u"0.18.0"_s);
        makeVersion(apps, u"0.18.1"_s);
        QVERIFY(junction(apps + u"/current"_s, apps + u"/0.18.0"_s));

        // The version folder "current" points at maps to it, files and folders alike.
        QCOMPARE(scoop::stablePath(apps + u"/0.18.0/WinShun.exe"_s), native(apps + u"/current/WinShun.exe"_s));
        QCOMPARE(scoop::stablePath(apps + u"/0.18.0"_s), native(apps + u"/current"_s));
        QCOMPARE(scoop::stablePath(apps + u"/0.18.0/bin/ws.exe"_s), native(apps + u"/current/bin/ws.exe"_s));
        QCOMPARE(scoop::stablePath(native(apps + u"/0.18.0/WinShun.exe"_s)), native(apps + u"/current/WinShun.exe"_s));
        // "current" stays; another version does not map to it.
        QCOMPARE(scoop::stablePath(apps + u"/current/WinShun.exe"_s), native(apps + u"/current/WinShun.exe"_s));
        QCOMPARE(scoop::stablePath(apps + u"/0.18.1/WinShun.exe"_s), native(apps + u"/0.18.1/WinShun.exe"_s));

        QVERIFY(scoop::sameCopy(apps + u"/0.18.0/WinShun.exe"_s, apps + u"/current/WinShun.exe"_s));
        QVERIFY(scoop::sameCopy(apps + u"/CURRENT/winshun.EXE"_s, apps + u"/0.18.0/WinShun.exe"_s));
        QVERIFY(!scoop::sameCopy(apps + u"/0.18.1/WinShun.exe"_s, apps + u"/current/WinShun.exe"_s));
        QVERIFY(!scoop::sameCopy({}, {}));

        const auto install = scoop::detect(apps + u"/current"_s);
        QVERIFY(install);
        QCOMPARE(install->version, u"0.18.0"_s); // which version "current" is

        // After `scoop update`: "current" moves on, the old version folder is another copy's no more.
        QVERIFY(junction(apps + u"/current"_s, apps + u"/0.18.1"_s));
        QCOMPARE(scoop::stablePath(apps + u"/0.18.0/WinShun.exe"_s), native(apps + u"/0.18.0/WinShun.exe"_s));
        QCOMPARE(scoop::stablePath(apps + u"/0.18.1/WinShun.exe"_s), native(apps + u"/current/WinShun.exe"_s));
        QCOMPARE(scoop::detect(apps + u"/current"_s)->version, u"0.18.1"_s);
    }

    void noCurrentOrNotScoop()
    {
        QTemporaryDir root;
        const QString apps = root.path() + u"/scoop/apps/winshun"_s;
        makeVersion(apps, u"0.18.0"_s);
        // No "current" yet (the install is under way): the path as it is.
        QCOMPARE(scoop::stablePath(apps + u"/0.18.0/WinShun.exe"_s), native(apps + u"/0.18.0/WinShun.exe"_s));
        // A "current" that is a plain folder, not a junction to this one.
        touch(apps + u"/current/WinShun.exe"_s);
        QCOMPARE(scoop::stablePath(apps + u"/0.18.0/WinShun.exe"_s), native(apps + u"/0.18.0/WinShun.exe"_s));
        // A folder named like Scoop's, without Scoop's records.
        const QString plain = root.path() + u"/tools/apps/thing"_s;
        touch(plain + u"/1.0/thing.exe"_s);
        QVERIFY(junction(plain + u"/current"_s, plain + u"/1.0"_s));
        QCOMPARE(scoop::stablePath(plain + u"/1.0/thing.exe"_s), native(plain + u"/1.0/thing.exe"_s));
        // Not under apps at all, and empty.
        QCOMPARE(scoop::stablePath(u"C:/Program Files/WinShun/WinShun.exe"_s), native(u"C:/Program Files/WinShun/WinShun.exe"_s));
        QCOMPARE(scoop::stablePath({}), QString());
    }

    void bucketManifest()
    {
        QFile file(QStringLiteral(WINSHUN_BUCKET_MANIFEST));
        QVERIFY2(file.open(QIODevice::ReadOnly), "bucket/winshun.json");
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        const QJsonObject manifest = document.object();

        const QString version = manifest[u"version"_s].toString();
        QVERIFY(QRegularExpression(uR"(^\d+\.\d+\.\d+$)"_s).match(version).hasMatch());
        const QString zip = u"WinShun-%1-x64-portable.zip"_s.arg(version);
        QCOMPARE(manifest[u"url"_s].toString(), u"https://github.com/LingCore/WinShun/releases/download/v%1/%2"_s.arg(version, zip));
        QVERIFY(QRegularExpression(u"^[0-9a-f]{64}$"_s).match(manifest[u"hash"_s].toString()).hasMatch());
        QCOMPARE(manifest[u"license"_s].toString(), u"MIT"_s);
        QCOMPARE(manifest[u"extract_dir"_s].toString(), u"WinShun"_s);
        QCOMPARE(manifest[u"persist"_s].toString(), u"data"_s); // portable mode keeps everything in data\
        QCOMPARE(manifest[u"bin"_s].toString(), u"bin\\ws.exe"_s);
        QCOMPARE(manifest[u"shortcuts"_s].toArray().first().toArray().first().toString(), u"WinShun.exe"_s);
        QVERIFY(!manifest[u"homepage"_s].toString().isEmpty());
        QVERIFY(!manifest[u"description"_s].toString().isEmpty());

        // The uninstall hook runs on `scoop update` too: it must act on an uninstall alone.
        const QJsonArray hook = manifest[u"pre_uninstall"_s].toArray();
        QStringList lines;
        for (const QJsonValue& line : hook)
            lines << line.toString();
        const QString script = lines.join(u'\n');
        QVERIFY(script.contains(u"$cmd -eq 'uninstall'"_s));
        QVERIFY(script.contains(u"--uninstall-cleanup"_s));
        QVERIFY(script.contains(u"-Wait"_s)); // WinShun.exe is a GUI program: it is not waited for otherwise
        QVERIFY(script.indexOf(u"$cmd -eq"_s) < script.indexOf(u"--uninstall-cleanup"_s));

        // checkver / autoupdate: the hash comes from the release's SHA256SUMS.txt,
        // which lists four files; only the portable zip's line may match.
        QCOMPARE(manifest[u"checkver"_s].toString(), u"github"_s);
        const QJsonObject autoupdate = manifest[u"autoupdate"_s].toObject();
        QCOMPARE(autoupdate[u"url"_s].toString(),
            u"https://github.com/LingCore/WinShun/releases/download/v$version/WinShun-$version-x64-portable.zip"_s);
        const QJsonObject hash = autoupdate[u"hash"_s].toObject();
        QCOMPARE(hash[u"url"_s].toString(), u"$baseurl/SHA256SUMS.txt"_s);
        // Scoop turns $sha256 into a 64-hex capture and $basename into the file's name.
        QString pattern = hash[u"regex"_s].toString();
        pattern.replace(u"$sha256"_s, u"([a-fA-F0-9]{64})"_s).replace(u"$basename"_s, QRegularExpression::escape(u"WinShun-0.19.0-x64-portable.zip"_s));
        const QString sums = u"97b3ca6fc875831c200be87ea7bdebbc0f5c3f16ae46b68f428daf28c33bb879  WinShun-0.19.0-x64-setup.exe\n"
                             "0d78597abc35a2da0a28003b2493421220200cb300e62f3f5474b179d47a5768  WinShun-0.19.0-x64-portable.zip\n"
                             "a426cc9571e0f5e0ce445f187648f80343ef4dbac48330c5d5c29de26251f3cc  WinShun-0.19.0-x64-offline-setup.exe\n"
                             "ccb82d77a545f183504edc51e6353370742d0423b71fa1cf7800db919494277e  WinShun-ocr-ppocr-v6-ort1.30.0-x64.zip"_s;
        const QRegularExpressionMatch match = QRegularExpression(pattern).match(sums);
        QVERIFY(match.hasMatch());
        QCOMPARE(match.captured(1), u"0d78597abc35a2da0a28003b2493421220200cb300e62f3f5474b179d47a5768"_s);
    }
};

QTEST_GUILESS_MAIN(TstScoopInstall)
#include "tst_scoopinstall.moc"
