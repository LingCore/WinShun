#include "Settings.h"
#include "Query.h"

#include "ServiceVolumeAccess.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using namespace Qt::StringLiterals;

namespace {

const QString kSizeKeys[] {u"Content/MaxFileSizeMB"_s, u"Content/MaxCodeFileSizeMB"_s,
    u"Content/MaxDataFileSizeMB"_s, u"Content/MaxDocumentFileSizeMB"_s};

} // namespace

class SettingsTest : public QObject {
    Q_OBJECT

private slots:
    void oneCommander()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.fileManager = u"onecommander"_s;
        settings.save();
        ws::Settings restored;
        restored.load();
        QCOMPARE(restored.fileManager, settings.fileManager);

        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        ini.setValue(u"Launcher/FileManager"_s, u" OneCommander "_s);
        ini.sync();
        restored.load();
        QCOMPARE(restored.fileManager, u"onecommander"_s);
    }

    void captureIssue83()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.load();
        QVERIFY(settings.captureLastPinHotkey.isEmpty());
        QVERIFY(settings.captureSaveFolder.isEmpty());
        QVERIFY(settings.captureRecordingFolder.isEmpty());
        QVERIFY(!settings.captureRecordingSystemAudio && !settings.captureRecordingMicrophone);
        QCOMPARE(settings.captureRecordingQuality, 1);
        QVERIFY(settings.captureRecordingCursor && settings.captureRecordingCountdown);
        QVERIFY(!settings.captureRecordingClicks);
        QCOMPARE(settings.captureRecordingFormat, 0);
        settings.captureLastPinHotkey = u"Ctrl+Alt+F8"_s;
        settings.captureSaveFolder = profile.filePath(u"截图 文件夹"_s);
        settings.captureRecordingFolder = profile.filePath(u"录屏"_s);
        settings.captureRecordingSystemAudio = true;
        settings.captureRecordingMicrophone = true;
        settings.captureRecordingQuality = 0;
        settings.captureRecordingCursor = false;
        settings.captureRecordingCountdown = false;
        settings.captureRecordingClicks = true;
        settings.captureRecordingFormat = 1;
        settings.save();
        ws::Settings restored;
        restored.load();
        QCOMPARE(restored.captureLastPinHotkey, settings.captureLastPinHotkey);
        QCOMPARE(restored.captureSaveFolder, settings.captureSaveFolder);
        QCOMPARE(restored.captureRecordingFolder, settings.captureRecordingFolder);
        QVERIFY(restored.captureRecordingSystemAudio && restored.captureRecordingMicrophone);
        QCOMPARE(restored.captureRecordingQuality, 0);
        QVERIFY(!restored.captureRecordingCursor && !restored.captureRecordingCountdown);
        QVERIFY(restored.captureRecordingClicks);
        QCOMPARE(restored.captureRecordingFormat, 1);

        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        ini.remove(u"Capture/RecordingQuality"_s);
        ini.setValue(u"Capture/RecordingHighQuality"_s, true); // the first version's preference
        ini.sync();
        restored.load();
        QCOMPARE(restored.captureRecordingQuality, 2);
        ini.setValue(u"Capture/RecordingQuality"_s, 99);
        ini.setValue(u"Capture/RecordingFormat"_s, 99);
        ini.sync();
        restored.load();
        QCOMPARE(restored.captureRecordingQuality, 1);
        QCOMPARE(restored.captureRecordingFormat, 0);
    }

    void opusTyping()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.load();
        QVERIFY(!settings.opusTyping);
        for (const bool explorer : {false, true}) {
            for (const bool opus : {true, false}) {
                settings.explorerTyping = explorer;
                settings.opusTyping = opus;
                settings.save();
                ws::Settings read;
                read.load();
                QCOMPARE(read.explorerTyping, explorer);
                QCOMPARE(read.opusTyping, opus);
            }
        }
        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        ini.remove(u"Launcher/OpusTyping"_s); // an older profile keeps Opus's own typing
        ini.sync();
        settings.load();
        QVERIFY(!settings.opusTyping);
        QVERIFY(settings.explorerTyping);
    }

    // GitHub #110: typed in a folder's window, its rows first (the new
    // default, for a profile from before too) or that folder alone.
    void folderTyping()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.load();
        QCOMPARE(settings.folderTyping, u"first"_s);
        settings.folderTyping = u"only"_s;
        settings.save();
        ws::Settings read;
        read.load();
        QCOMPARE(read.folderTyping, u"only"_s);

        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        ini.setValue(u"Launcher/FolderTyping"_s, u" Only "_s); // by hand
        ini.sync();
        read.load();
        QCOMPARE(read.folderTyping, u"only"_s);
        for (const QString& value : {u"everywhere"_s, QString()}) {
            ini.setValue(u"Launcher/FolderTyping"_s, value);
            ini.sync();
            read.load();
            QCOMPARE(read.folderTyping, u"first"_s);
        }
        ini.remove(u"Launcher/FolderTyping"_s); // a profile from before: the folder first
        ini.sync();
        read.load();
        QCOMPARE(read.folderTyping, u"first"_s);
    }

    void selectedDrivesRoundTrip()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        ws::Settings::useProfile(temporary.path());
        ws::Settings settings;
        settings.load();
        QVERIFY(!settings.selectedDrives);
        settings.selectedDrives = QStringList{u"c:"_s, u"D:\\"_s, u"C:"_s, u"invalid"_s};
        settings.save();
        settings.load();
        QCOMPARE(*settings.selectedDrives, (QStringList{u"C:"_s, u"D:"_s}));
        settings.selectedDrives = QStringList{};
        settings.save();
        settings.load();
        QVERIFY(settings.selectedDrives && settings.selectedDrives->isEmpty());
        settings.selectedDrives.reset();
        settings.save();
        settings.load();
        QVERIFY(!settings.selectedDrives);
    }

    // GitHub #132: skipped file names, Office's and WPS's lock files by
    // default; an emptied list stays empty, and one written by hand is read
    // as the settings window keeps it.
    void skippedFileNames()
    {
        QCOMPARE(ws::Settings::fileNamePatterns(u" ~$* ; Thumbs.db，*.TMP、新建 文本文档*\n~$*;THUMBS.DB"_s),
            (QStringList {u"~$*"_s, u"Thumbs.db"_s, u"*.TMP"_s, u"新建 文本文档*"_s}));
        QVERIFY(ws::Settings::fileNamePatterns(u"D:\\x\\~$*, a/b, c:d, ,"_s).isEmpty());
        QVERIFY(ws::Settings::fileNamePatterns(u"*; *.*; ?; **"_s).isEmpty()); // every file

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        ws::Settings::useProfile(temporary.path());
        ws::Settings settings;
        settings.load();
        QCOMPARE(settings.excludedFileNames, QStringList {u"~$*"_s});
        settings.excludedFileNames.clear();
        settings.save();
        settings.load();
        QVERIFY(settings.excludedFileNames.isEmpty());
        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Index/ExcludedFileNames"_s, u"~$*; *.tmp~"_s);
        }
        settings.load();
        QCOMPARE(settings.excludedFileNames, (QStringList {u"~$*"_s, u"*.tmp~"_s}));
    }

    void wslFolders()
    {
        const QString path = u"\\\\wsl.localhost\\Ubuntu\\home\\Dev"_s;
        QCOMPARE(ws::Settings::sharePath(u"\"\\\\wsl$\\Ubuntu\\home\\Dev\\\""_s), path);
        QCOMPARE(ws::Settings::sharePath(u"file://wsl.localhost/Ubuntu/home/Dev"_s), path);
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.shares = {{path, u"项目"_s}, {u"\\\\wsl.localhost\\Ubuntu\\home\\dev"_s, {}}};
        settings.save();
        ws::Settings again;
        again.load();
        QCOMPARE(again.shares.size(), 2);
        QCOMPARE(again.shares[0].path, path);
        QCOMPARE(again.shareOptions().shares.size(), std::size_t {2});
    }

    // GitHub #114: a WebDAV folder, as its web address or as Windows writes
    // it, is kept in one form: one added twice is seen (addShare), and two
    // spellings of one in the settings file become one.
    void webDavFolders()
    {
        const QString small = u"\\\\127.0.0.1@8714\\small"_s;
        QCOMPARE(ws::Settings::sharePath(u"http://127.0.0.1:8714/small/"_s), small);
        QCOMPARE(ws::Settings::sharePath(u"\\\\127.0.0.1@8714\\DavWWWRoot\\small"_s), small);
        QCOMPARE(ws::Settings::sharePath(u"\"\\\\127.0.0.1@8714\\davwwwroot\\small\\\""_s), small);
        QCOMPARE(ws::Settings::sharePath(u"//127.0.0.1@8714/small"_s), small);
        QCOMPARE(ws::Settings::sharePath(u"https://NAS/dav"_s), u"\\\\nas@SSL\\dav"_s);
        QCOMPARE(ws::Settings::sharePath(u"\\\\nas@SSL@443\\DavWWWRoot\\dav"_s), u"\\\\nas@SSL\\dav"_s);
        QCOMPARE(ws::Settings::sharePath(u"\\\\nas@SSL@5006"_s), u"\\\\nas@SSL@5006\\DavWWWRoot"_s);
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings settings;
        settings.shares = {{u"\\\\127.0.0.1@8714\\DavWWWRoot\\small"_s, {}}, {small, {}},
            {u"http://127.0.0.1:8714/many"_s, {}}};
        settings.save();
        ws::Settings again;
        again.load();
        QCOMPARE(again.shares.size(), 2);
        QCOMPARE(again.shares[0].path, small);
        QCOMPARE(again.shares[1].path, u"\\\\127.0.0.1@8714\\many"_s);
    }

    void fileTypePrefixes()
    {
        auto types = ws::Settings::defaultFileTypes();
        QCOMPARE(types[0].keyword, u"documents"_s);
        const auto folder = ws::splitFileTypeQuery(u"  :FoLdEr 报告 2026 "_s, types, u"folder");
        QCOMPARE(folder.type, std::optional<int>(-2));
        QCOMPARE(folder.words, u"报告 2026"_s);
        const auto pictures = ws::splitFileTypeQuery(u":pictures"_s, types, u"folder");
        QCOMPARE(pictures.type, std::optional<int>(1));
        QVERIFY(pictures.words.isEmpty());
        types.append({u"设计图"_s, {u"dwg"_s, u"dxf"_s}, {}, u"cad"_s});
        QCOMPARE(ws::splitFileTypeQuery(u":CAD 泵 size:>1mb"_s, types, u"folder").words, u"泵 size:>1mb"_s);
        for (const QString text : {u":unknown 报告"_s, u"\":folder\" 报告"_s, u"C:\\folder\\报告"_s, u"报告 :folder"_s}) {
            const auto literal = ws::splitFileTypeQuery(text, types, u"folder");
            QVERIFY(!literal.type);
            QCOMPARE(literal.words, text);
        }
        QCOMPARE(ws::normalizedTypeKeyword(u" :CAD "), std::optional<QString>(u"cad"_s));
        QVERIFY(!ws::normalizedTypeKeyword(u"bad prefix"));
        QVERIFY(!ws::normalizedTypeKeyword(u"a:b"));
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        ws::Settings::useProfile(temporary.path());
        ws::Settings settings;
        settings.fileTypes = types;
        settings.folderKeyword = u"dir"_s;
        settings.showModifiedTime = true;
        settings.save();
        ws::Settings loaded;
        loaded.load();
        QCOMPARE(loaded.fileTypes, types);
        QCOMPARE(loaded.folderKeyword, u"dir"_s);
        QVERIFY(loaded.showModifiedTime);
    }

    // Limits still at the default of an older version take the new default;
    // ones the user set, and a list of extensions set by hand, stay.
    void contentDefaults_data()
    {
        QTest::addColumn<int>("version"); // 0: no DefaultsVersion
        QTest::addColumn<QList<double>>("written");
        QTest::addColumn<QString>("extensions");
        QTest::addColumn<QList<double>>("expected"); // -1: the new default
        QTest::addColumn<bool>("defaultExtensions");

        QTest::newRow("before 3, untouched") << 0 << QList<double> {64, 8, 16, 512} << u"txt"_s
                                             << QList<double> {-1, -1, -1, -1} << true;
        QTest::newRow("before 3, one set") << 2 << QList<double> {64, 3, 16, 512} << u"txt, md"_s
                                           << QList<double> {-1, 3, -1, -1} << false;
        QTest::newRow("3, untouched") << 3 << QList<double> {16, 2, 4, 100} << QString()
                                      << QList<double> {-1, -1, -1, -1} << true;
        QTest::newRow("3, text set to 1 KB") << 3 << QList<double> {1.0 / 1024, 2, 4, 100} << QString()
                                             << QList<double> {1.0 / 1024, -1, -1, -1} << true;
        QTest::newRow("3, txt set by hand") << 3 << QList<double> {16, 8, 4, 512} << u"txt"_s
                                            << QList<double> {-1, 8, -1, 512} << false;
        // The new defaults are what the user now sets, the old ones too.
        QTest::newRow("4, old values set") << 4 << QList<double> {16, 2, 4, 100} << QString()
                                           << QList<double> {16, 2, 4, 100} << true;
    }

    void contentDefaults()
    {
        QFETCH(int, version);
        QFETCH(QList<double>, written);
        QFETCH(QString, extensions);
        QFETCH(QList<double>, expected);
        QFETCH(bool, defaultExtensions);

        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            if (version)
                ini.setValue(u"Content/DefaultsVersion"_s, version);
            for (int k = 0; k < 4; ++k)
                ini.setValue(kSizeKeys[k], written[k]);
            if (!extensions.isEmpty())
                ini.setValue(u"Content/Extensions"_s, extensions.split(u", "_s));
        }
        const ws::Settings d = ws::Settings::defaults();
        ws::Settings settings;
        settings.load();
        for (std::size_t k = 0; k < 4; ++k) {
            const double want = expected[k] < 0 ? d.contentMaxSizeMB[k] : expected[k];
            QCOMPARE(settings.contentMaxSizeMB[k], want);
        }
        if (defaultExtensions)
            QCOMPARE(settings.contentExtensions, d.contentExtensions);
        else
            QCOMPARE(settings.contentExtensions, extensions.split(u", "_s));

        // Migrated once: loading again changes nothing, and the file holds it.
        ws::Settings again;
        again.load();
        QCOMPARE(again.contentMaxSizeMB, settings.contentMaxSizeMB);
        QCOMPARE(again.contentExtensions, settings.contentExtensions);
        QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
        QCOMPARE(ini.value(u"Content/DefaultsVersion"_s).toInt(), 4);
    }

    // The folder menu's groups (GitHub #51): all on by default, kept when
    // saved, and a file with none on reads back as all on.
    void folderMenuGroups()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings fresh;
        fresh.load();
        QVERIFY(fresh.folderMenuFavorites && fresh.folderMenuRecent && fresh.folderMenuWindows);

        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Launcher/FolderMenuFavorites"_s, false);
            ini.setValue(u"Launcher/FolderMenuRecent"_s, true);
            ini.setValue(u"Launcher/FolderMenuWindows"_s, false);
        }
        ws::Settings some;
        some.load();
        QVERIFY(!some.folderMenuFavorites && some.folderMenuRecent && !some.folderMenuWindows);

        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Launcher/FolderMenuRecent"_s, false);
        }
        ws::Settings none;
        none.load();
        QVERIFY(none.folderMenuFavorites && none.folderMenuRecent && none.folderMenuWindows);
    }

    // What a new install leaves off until wanted: the folder menu, the dialog
    // bar, 粘贴为文件. A file from before without their keys keeps the first
    // two on, as they were; a choice written stays.
    void offUntilWanted()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings fresh;
        fresh.load();
        QVERIFY(!fresh.dialogBar);
        QCOMPARE(fresh.folderMenu, u"off"_s);
        QVERIFY(!fresh.pasteFileEnabled);
        QVERIFY(fresh.pasteFileCtrlV && fresh.pasteFileMenu); // its ways, once it is on
        fresh.save();
        ws::Settings again;
        again.load();
        QVERIFY(!again.dialogBar);
        QCOMPARE(again.folderMenu, u"off"_s);

        QTemporaryDir older;
        QVERIFY(older.isValid());
        ws::Settings::useProfile(older.path());
        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Launcher/Hotkey"_s, u"Alt+Space"_s); // a file from before the keys
        }
        ws::Settings upgraded;
        upgraded.load();
        QVERIFY(upgraded.dialogBar);
        QCOMPARE(upgraded.folderMenu, u"auto"_s);
        QVERIFY(!upgraded.pasteFileEnabled); // new: off, as 截图 is

        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Launcher/FolderMenu"_s, u"sideways"_s);
            ini.setValue(u"PasteFile/Enabled"_s, true);
            ini.setValue(u"PasteFile/CtrlV"_s, false);
        }
        ws::Settings written;
        written.load();
        QCOMPARE(written.folderMenu, u"auto"_s); // a stranger: as before
        QVERIFY(written.pasteFileEnabled && !written.pasteFileCtrlV && written.pasteFileMenu);
    }

    // GitHub #44: the other sorts and the double-click setting. A click opens
    // by default; every sort name is kept, and a stranger reads as the best match.
    // The capture's mouse buttons (#46): the old behaviour by default, kept
    // across a save, and a name not known (or "back" on a button that cannot
    // step back) falls to the default.
    void captureMouseActions()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings fresh;
        fresh.load();
        QCOMPARE(fresh.captureMouseDoubleLeft, u"copy"_s);
        QCOMPARE(fresh.captureMouseRight, u"back"_s);
        QCOMPARE(fresh.captureMouseMiddle, u"none"_s);
        QCOMPARE(fresh.captureMouseBack, u"none"_s);
        QCOMPARE(fresh.captureMouseForward, u"none"_s);
        QVERIFY(!fresh.capturePinOnCopy);

        ws::Settings s;
        s.load();
        s.captureMouseDoubleLeft = u"save"_s;
        s.captureMouseRight = u"cancel"_s;
        s.captureMouseMiddle = u"copypin"_s;
        s.capturePinOnCopy = true;
        s.captureMouseBack = u"ocr"_s;
        s.captureMouseForward = u"copy"_s;
        s.save();
        ws::Settings back;
        back.load();
        QCOMPARE(back.captureMouseDoubleLeft, u"save"_s);
        QCOMPARE(back.captureMouseRight, u"cancel"_s);
        QCOMPARE(back.captureMouseMiddle, u"copypin"_s);
        QVERIFY(back.capturePinOnCopy);
        QCOMPARE(back.captureMouseBack, u"ocr"_s);
        QCOMPARE(back.captureMouseForward, u"copy"_s);

        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Capture/MouseDoubleLeft"_s, u"back"_s);
            ini.setValue(u"Capture/MouseRight"_s, u"Back"_s);
            ini.setValue(u"Capture/MouseMiddle"_s, u"nonsense"_s);
        }
        ws::Settings odd;
        odd.load();
        QCOMPARE(odd.captureMouseDoubleLeft, u"copy"_s);
        QCOMPARE(odd.captureMouseRight, u"back"_s);
        QCOMPARE(odd.captureMouseMiddle, u"none"_s);
    }

    // A number written by hand that is none ("abc", "", "1h") reads as the
    // default, not as the 0 a failed conversion gives ("每 0 小时", GitHub
    // #114); a 0 written as one stays 0 (the WebDAV folders: only at start).
    void numbersWrittenByHand()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        const ws::Settings d;
        const auto read = [](const QString& key, const QVariant& value) {
            {
                QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
                ini.setValue(key, value);
            }
            ws::Settings s;
            s.load();
            return s;
        };
        for (const QString& text : {u"abc"_s, QString(), u"1h"_s}) {
            QCOMPARE(read(u"Network/VerifyHoursUnwatched"_s, text).networkVerifyHoursUnwatched, d.networkVerifyHoursUnwatched);
            QCOMPARE(read(u"Network/Threads"_s, text).networkThreads, d.networkThreads);
        }
        QCOMPARE(read(u"Network/VerifyHoursUnwatched"_s, u"0"_s).networkVerifyHoursUnwatched, 0);
        QCOMPARE(read(u"Network/VerifyHoursUnwatched"_s, u"6"_s).networkVerifyHoursUnwatched, 6);
        QCOMPARE(read(u"Network/Threads"_s, u"8"_s).networkThreads, 8);
        QCOMPARE(read(u"Content/MaxFileSizeMB"_s, u"lots"_s).contentMaxSizeMB[0], d.contentMaxSizeMB[0]);
    }

    // The OCR choices of the mouse buttons and the toolbar's buttons left off (#57).
    void captureOcrActionsAndToolbarButtons()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings fresh;
        fresh.load();
        QVERIFY(fresh.captureToolbarHidden.isEmpty()); // all shown
        QCOMPARE(ws::Settings::captureToolbarButtonIds(),
            (QStringList {u"undo"_s, u"redo"_s, u"copy"_s, u"save"_s, u"pin"_s, u"ocr"_s, u"long"_s, u"record"_s, u"qr"_s, u"delay"_s, u"cancel"_s}));

        ws::Settings s;
        s.load();
        s.captureMouseDoubleLeft = u"ocrcopy"_s;
        s.captureMouseMiddle = u"ocrsearch"_s;
        s.captureMouseBack = u"ocrweb"_s;
        s.captureToolbarHidden = u"pin,redo"_s;
        s.save();
        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            // Written once, in a stable form.
            QCOMPARE(ini.value(u"Capture/ToolbarHidden"_s).toString(), u"pin,redo"_s);
        }
        ws::Settings back;
        back.load();
        QCOMPARE(back.captureMouseDoubleLeft, u"ocrcopy"_s);
        QCOMPARE(back.captureMouseMiddle, u"ocrsearch"_s);
        QCOMPARE(back.captureMouseBack, u"ocrweb"_s);
        QCOMPARE(back.captureToolbarHidden, u"redo,pin"_s); // the toolbar's order

        // Unknown ids, repeats, case and blanks are cleaned on the way in.
        QCOMPARE(ws::Settings::normalizedToolbarHidden(u" Cancel, OCR ,nonsense,cancel,,undo "_s), u"undo,ocr,cancel"_s);
        QCOMPARE(ws::Settings::normalizedToolbarHidden(u"nothing"_s), QString());
        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Capture/ToolbarHidden"_s, u"copy,save,bogus,copy"_s);
            ini.setValue(u"Capture/MouseRight"_s, u"ocrweb"_s);
        }
        ws::Settings odd;
        odd.load();
        QCOMPARE(odd.captureToolbarHidden, u"copy,save"_s);
        // Written by hand with no quotes, "ocr,undo" is a list to QSettings: still read (found in the live test).
        {
            QFile file(ws::Settings::filePath());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QByteArray text = file.readAll();
            file.close();
            const qsizetype at = text.indexOf("ToolbarHidden=");
            QVERIFY(at >= 0);
            const qsizetype end = text.indexOf('\n', at);
            text.replace(at, end - at, "ToolbarHidden=ocr,undo");
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(text);
            file.close();
        }
        ws::Settings typed;
        typed.load();
        QCOMPARE(typed.captureToolbarHidden, u"undo,ocr"_s);
        QCOMPARE(odd.captureMouseRight, u"ocrweb"_s);

        // Stage 7's options: defaults, a round trip, and values it does not know fall back.
        QVERIFY(!typed.captureCursor && typed.captureControlsUia && typed.captureDelay == 5);
        QCOMPARE(typed.captureColorFormat, u"hex"_s);
        QVERIFY(typed.capturePinsHotkey.isEmpty() && typed.capturePinsRestoreHotkey.isEmpty());
        QCOMPARE(typed.capturePinDoubleClick, u"reset"_s);
        typed.captureCursor = true;
        typed.captureControlsUia = false;
        typed.captureDelay = 10;
        typed.captureColorFormat = u"hsl"_s;
        typed.capturePinsHotkey = u"Ctrl+Alt+H"_s;
        typed.capturePinsRestoreHotkey = u"Ctrl+Alt+J"_s;
        typed.capturePinDoubleClick = u"close"_s;
        typed.save();
        ws::Settings seven;
        seven.load();
        QVERIFY(seven.captureCursor && !seven.captureControlsUia);
        QCOMPARE(seven.captureDelay, 10);
        QCOMPARE(seven.captureColorFormat, u"hsl"_s);
        QCOMPARE(seven.capturePinsHotkey, u"Ctrl+Alt+H"_s);
        QCOMPARE(seven.capturePinsRestoreHotkey, u"Ctrl+Alt+J"_s);
        QCOMPARE(seven.capturePinDoubleClick, u"close"_s);
        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Capture/DelaySeconds"_s, 7);
            ini.setValue(u"Capture/ColorFormat"_s, u"cmyk"_s);
            ini.setValue(u"Capture/PinDoubleClick"_s, u"explode"_s);
        }
        ws::Settings odd7;
        odd7.load();
        QCOMPARE(odd7.captureDelay, 5);
        QCOMPARE(odd7.captureColorFormat, u"hex"_s);
        QCOMPARE(odd7.capturePinDoubleClick, u"reset"_s);
        typed.captureCursor = false;
        typed.captureControlsUia = true;
        typed.captureDelay = 5;
        typed.captureColorFormat = u"hex"_s;
        typed.capturePinsHotkey.clear();
        typed.capturePinsRestoreHotkey.clear();
        typed.capturePinDoubleClick = u"reset"_s;
        typed.save();

        // Smart redaction's options: defaults, a round trip, and the words typed by hand with no quotes.
        QVERIFY(typed.captureSmartPrescan && !typed.captureSmartIpMac && !typed.captureSmartAllText);
        QVERIFY(typed.captureSmartWords.isEmpty());
        typed.captureSmartPrescan = false;
        typed.captureSmartWords = u"Atlas,客户甲"_s;
        typed.save();
        ws::Settings words;
        words.load();
        QVERIFY(!words.captureSmartPrescan);
        QCOMPARE(words.captureSmartWords, u"Atlas,客户甲"_s);
        {
            QFile file(ws::Settings::filePath());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QByteArray text = file.readAll();
            file.close();
            const qsizetype at = text.indexOf("SmartWords=");
            QVERIFY(at >= 0);
            text.replace(at, text.indexOf('\n', at) - at, "SmartWords=Atlas, Beta;Atlas");
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(text);
            file.close();
        }
        ws::Settings hand;
        hand.load();
        QCOMPARE(hand.captureSmartWords, u"Atlas,Beta"_s);
        QCOMPARE(ws::Settings::normalizedSmartWords(u"a，b、c；d\n e ,a"_s), (QStringList {u"a"_s, u"b"_s, u"c"_s, u"d"_s, u"e"_s}));
    }

    void sortAndDoubleClick()
    {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        ws::Settings fresh;
        fresh.load();
        QCOMPARE(fresh.sort, u"match"_s);
        QVERIFY(!fresh.doubleClickToOpen);

        for (const QStringView name : ws::kSortNames) {
            ws::Settings s;
            s.load();
            s.sort = name.toString();
            s.doubleClickToOpen = true;
            s.save();
            ws::Settings back;
            back.load();
            QCOMPARE(back.sort, name.toString());
            QVERIFY(back.doubleClickToOpen);
        }
        QCOMPARE(ws::sortIndex(u"newest"), 1);
        QCOMPARE(ws::sortIndex(u"smallest"), 7);
        QCOMPARE(ws::sortIndex(u"nonsense"), 0);

        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Results/Sort"_s, u"alphabetical"_s);
        }
        ws::Settings odd;
        odd.load();
        QCOMPARE(odd.sort, u"match"_s);
    }

    // GitHub #52: a new install starts with auto and the known folders; an
    // older file, which indexed whole disks, goes on so (and says so in the
    // file). The folders stay as written: an emptied list is not filled again.
    void indexScope()
    {
        QTemporaryDir fresh;
        QVERIFY(fresh.isValid());
        ws::Settings::useProfile(fresh.path());
        ws::Settings installed;
        installed.load();
        QCOMPARE(installed.indexScope, u"auto"_s);
        QCOMPARE(installed.indexFolders, ws::Settings::defaultIndexFolders());
        QCOMPARE(installed.folderVerifyHours, 24);
        QCOMPARE(QSettings(ws::Settings::filePath(), QSettings::IniFormat).value(u"Index/Scope"_s).toString(), u"auto"_s);

        QTemporaryDir old;
        QVERIFY(old.isValid());
        ws::Settings::useProfile(old.path());
        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Index/RescanOnStartup"_s, true);
        }
        ws::Settings upgraded;
        upgraded.load();
        QCOMPARE(upgraded.indexScope, u"drives"_s);
        QCOMPARE(QSettings(ws::Settings::filePath(), QSettings::IniFormat).value(u"Index/Scope"_s).toString(), u"drives"_s);
        ws::Settings again;
        again.load();
        QCOMPARE(again.indexScope, u"drives"_s);

        // Kept as written, nested ones and variables too; an empty list stays empty.
        const QStringList written {u"D:\\Work"_s, u"D:\\Work\\Sub"_s, u"%USERPROFILE%\\Desktop"_s, u"E:\\"_s};
        again.indexScope = u"folders"_s;
        again.indexFolders = written;
        again.folderVerifyHours = 4;
        again.save();
        ws::Settings back;
        back.load();
        QCOMPARE(back.indexScope, u"folders"_s);
        QCOMPARE(back.indexFolders, written);
        QCOMPARE(back.folderVerifyHours, 4);
        back.indexFolders.clear();
        back.save();
        ws::Settings emptied;
        emptied.load();
        QVERIFY(emptied.indexFolders.isEmpty());

        {
            QSettings ini(ws::Settings::filePath(), QSettings::IniFormat);
            ini.setValue(u"Index/Scope"_s, u"Everything"_s);
            ini.setValue(u"Index/FolderVerifyHours"_s, 1000);
        }
        ws::Settings odd;
        odd.load();
        QCOMPARE(odd.indexScope, u"auto"_s);
        QCOMPARE(odd.folderVerifyHours, 24 * 7);
    }

    // GitHub #52 review: the known folders are not written into the file
    // when it is read. A list never set follows them (OneDrive taking the
    // documents, say) until only the chosen folders are first indexed, which
    // takes them as they are then, for good; one changed in any way (by the
    // AI too, which knows no flag) is written. An emptied list stays empty.
    void indexFoldersUnset()
    {
        if (ws::Settings::defaultIndexFolders().isEmpty())
            QSKIP("no known folders on a local disk here");
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        ws::Settings::useProfile(profile.path());
        const auto written = [] { return QSettings(ws::Settings::filePath(), QSettings::IniFormat).contains(u"Index/Folders"_s); };
        ws::Settings fresh;
        fresh.load();
        QVERIFY(!fresh.indexFoldersChosen);
        QCOMPARE(fresh.indexFolders, ws::Settings::defaultIndexFolders());
        QVERIFY(!written());
        fresh.save();
        QVERIFY(!written());
        ws::Settings same;
        same.load();
        QVERIFY(!same.indexFoldersChosen);

        // The first time the chosen folders are in effect: taken, once.
        QVERIFY(fresh.chooseDefaultIndexFolders());
        QVERIFY(fresh.indexFoldersChosen);
        QCOMPARE(fresh.indexFolders, ws::Settings::defaultIndexFolders());
        QVERIFY(!fresh.chooseDefaultIndexFolders());
        fresh.save();
        QVERIFY(written());
        ws::Settings taken;
        taken.load();
        QVERIFY(taken.indexFoldersChosen);
        QCOMPARE(taken.indexFolders, ws::Settings::defaultIndexFolders());
        QVERIFY(!taken.chooseDefaultIndexFolders());

        // Emptied: stays so, and is not filled again.
        taken.indexFolders.clear();
        taken.save();
        ws::Settings emptied;
        emptied.load();
        QVERIFY(emptied.indexFoldersChosen);
        QVERIFY(!emptied.chooseDefaultIndexFolders());
        QVERIFY(emptied.indexFolders.isEmpty());

        // Changed without the flag (an edit that knows nothing of it): written all the same.
        QTemporaryDir other;
        QVERIFY(other.isValid());
        ws::Settings::useProfile(other.path());
        ws::Settings edited;
        edited.load();
        edited.indexFolders.removeLast();
        ws::Settings takenAsIs = edited; // the chosen folders come into effect after that edit
        QCOMPARE(takenAsIs.chooseDefaultIndexFolders(), !edited.indexFolders.isEmpty());
        QCOMPARE(takenAsIs.indexFolders, edited.indexFolders); // not put back to the known ones
        edited.save();
        QVERIFY(written());
        ws::Settings back;
        back.load();
        QVERIFY(back.indexFoldersChosen);
        QCOMPARE(back.indexFolders, edited.indexFolders);
    }

    // GitHub #52 review: whether only the chosen folders are indexed. Never
    // with the disks read whole, nor with drives; while the index service is
    // wanted and only on its way (stopped, starting, not answering), whole
    // disks for folders and auto alike: else every login that comes before
    // the service would drop the rest of each disk and read it again.
    void foldersInEffect()
    {
        using State = ws::ntfs::ServiceState;
        ws::Settings s;
        const auto in = [&](const char* scope, const char* access, bool walked, bool elevated, bool wanted, State state) {
            s.indexScope = QString::fromLatin1(scope);
            s.indexAccess = QString::fromLatin1(access);
            return s.foldersInEffect(walked, elevated, wanted, state);
        };
        // Read whole (through the service, or as administrator), or drives: never.
        QVERIFY(!in("folders", "auto", false, false, true, State::Running));
        QVERIFY(!in("folders", "direct", false, true, false, State::Unknown));
        QVERIFY(!in("drives", "walk", true, false, false, State::Unknown));
        QVERIFY(!in("drives", "auto", true, false, true, State::NotInstalled));
        // Walked for good: told to walk, or no service (not installed, disabled).
        QVERIFY(in("folders", "walk", true, false, false, State::Unknown));
        QVERIFY(in("auto", "walk", true, false, false, State::Unknown));
        QVERIFY(in("folders", "auto", true, false, true, State::NotInstalled));
        QVERIFY(in("folders", "auto", true, false, true, State::Disabled));
        QVERIFY(in("auto", "auto", true, false, true, State::NotInstalled));
        QVERIFY(in("auto", "service", true, false, true, State::Disabled));
        // As administrator, told to use the service that is not there: folders only when told so.
        QVERIFY(in("folders", "service", true, true, true, State::NotInstalled));
        QVERIFY(!in("auto", "service", true, true, true, State::NotInstalled));
        // The service on its way: whole disks, folders or auto alike.
        for (const State state : {State::Stopped, State::Starting, State::Running, State::Unknown}) {
            QVERIFY2(!in("folders", "auto", true, false, true, state), ws::ntfs::ServiceVolumeAccess::stateName(state));
            QVERIFY2(!in("folders", "service", true, true, true, state), ws::ntfs::ServiceVolumeAccess::stateName(state));
            QVERIFY2(!in("auto", "auto", true, false, true, state), ws::ntfs::ServiceVolumeAccess::stateName(state));
        }
    }

    // As IndexService takes them: full, the letter upper case, "D:" for a
    // drive; network places and what is no full path left out.
    void indexFolderPaths()
    {
        ws::Settings s;
        s.indexFolders = {u"d:\\Work\\"_s, u"E:\\"_s, u"e:"_s, u"\\\\nas\\docs"_s, u"Documents"_s, u"F:Work"_s, u"  "_s};
        const std::vector<std::wstring> expected {L"D:\\Work", L"E:", L"E:"};
        QCOMPARE(s.indexFolderPaths(), expected);
        QVERIFY(ws::Settings::isNetworkPath(u"\\\\nas\\docs"_s));
        QVERIFY(!ws::Settings::isNetworkPath(u"C:\\Windows"_s));

        // None inside another, none on a network, every one there.
        const QStringList defaults = ws::Settings::defaultIndexFolders();
        for (const QString& f : defaults) {
            QVERIFY2(QFileInfo(f).isDir(), qPrintable(f));
            QVERIFY(!ws::Settings::isNetworkPath(f));
            for (const QString& o : defaults)
                QVERIFY2(f == o || !f.startsWith(o + u'\\', Qt::CaseInsensitive), qPrintable(f + u" in "_s + o));
        }
    }
};

QTEST_GUILESS_MAIN(SettingsTest)
#include "tst_settings.moc"
