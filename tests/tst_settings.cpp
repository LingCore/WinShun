#include "Settings.h"
#include "SettingsCommand.h"

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
            (QStringList {u"undo"_s, u"redo"_s, u"copy"_s, u"save"_s, u"pin"_s, u"ocr"_s, u"long"_s, u"qr"_s, u"delay"_s, u"cancel"_s}));

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

    // `WinShun.exe --settings --settings-at 9,0` to a copy that is running: the place travels
    // with the command (it was dropped, and the window opened on its first page).
    void settingsCommandCarriesThePlace()
    {
        namespace sc = ws::settingscommand;
        QCOMPARE(sc::make({}), u"settings"_s);
        QCOMPARE(sc::make(u"9,0"_s), u"settings:9,0"_s);
        QCOMPARE(sc::make(u"indexAccess"_s), u"settings:indexAccess"_s);
        QString place = u"left over"_s;
        QVERIFY(sc::parse(u"settings"_s, &place));
        QVERIFY(place.isEmpty());
        QVERIFY(sc::parse(u"settings:9,1"_s, &place));
        QCOMPARE(place, u"9,1"_s);
        QVERIFY(sc::parse(u"settings:indexAccess"_s, &place));
        QCOMPARE(place, u"indexAccess"_s);
        QVERIFY(sc::parse(u"settings:"_s, &place)); // no place: just open
        QVERIFY(place.isEmpty());
        QVERIFY(sc::parse(u"settings"_s, nullptr));
        // Other commands are not this one.
        for (const QString& other : {u"toggle"_s, u"quit"_s, u"show"_s, u"query:settings"_s, u"settingsx"_s, QString()})
            QVERIFY2(!sc::parse(other, &place), qPrintable(other));
        // What make() builds, parse() reads back.
        for (const QString& at : {u"0,0"_s, u"9,2"_s, u"indexAccess"_s}) {
            QVERIFY(sc::parse(sc::make(at), &place));
            QCOMPARE(place, at);
        }
    }
};

QTEST_GUILESS_MAIN(SettingsTest)
#include "tst_settings.moc"
