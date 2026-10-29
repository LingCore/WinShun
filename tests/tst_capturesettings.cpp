// The screenshot plugin's settings (src/capture-plugin/CaptureSettings,
// capture.ini): what tst_settings checked of [Capture] in WinShun.ini before
// the screenshot became a plugin (GitHub #138). The group keeps its name
// and its keys, so that the settings moved over read as they were.

#include "CaptureSettings.h"

#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include <windows.h>

using namespace Qt::StringLiterals;
using ws::captureplugin::CaptureOptions;

// The folder dialog of the settings window, not opened here.
namespace ws::shell {
QString pickFolder(HWND, const QString&)
{
    return {};
}
} // namespace ws::shell

namespace {

// A value of `key` written by hand, as a line of the file (no quotes).
void writeByHand(const QString& file, const QByteArray& key, const QByteArray& value)
{
    QFile f(file);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QByteArray text = f.readAll();
    f.close();
    const qsizetype at = text.indexOf(key + '=');
    QVERIFY(at >= 0);
    const qsizetype end = text.indexOf('\n', at);
    text.replace(at, end - at, key + '=' + value);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(text);
}

} // namespace

class CaptureSettingsTest : public QObject {
    Q_OBJECT

private slots:
    // GitHub #83: the recording's options and the folders, defaults and a round trip.
    void recordingAndFolders()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(u"capture.ini"_s);
        const CaptureOptions fresh = CaptureOptions::load(file);
        QVERIFY(fresh.lastPinHotkey.isEmpty());
        QVERIFY(fresh.saveFolder.isEmpty());
        QVERIFY(fresh.recordingFolder.isEmpty());
        QVERIFY(!fresh.recordingSystemAudio && !fresh.recordingMicrophone);
        QCOMPARE(fresh.recordingQuality, 1);
        QVERIFY(fresh.recordingCursor && fresh.recordingCountdown);
        QVERIFY(!fresh.recordingClicks);
        QCOMPARE(fresh.recordingFormat, 0);

        CaptureOptions o = fresh;
        o.lastPinHotkey = u"Ctrl+Alt+F8"_s;
        o.saveFolder = dir.filePath(u"截图 文件夹"_s);
        o.recordingFolder = dir.filePath(u"录屏"_s);
        o.recordingSystemAudio = true;
        o.recordingMicrophone = true;
        o.recordingQuality = 0;
        o.recordingCursor = false;
        o.recordingCountdown = false;
        o.recordingClicks = true;
        o.recordingFormat = 1;
        o.save(file);
        const CaptureOptions back = CaptureOptions::load(file);
        QCOMPARE(back.lastPinHotkey, o.lastPinHotkey);
        QCOMPARE(back.saveFolder, o.saveFolder);
        QCOMPARE(back.recordingFolder, o.recordingFolder);
        QVERIFY(back.recordingSystemAudio && back.recordingMicrophone);
        QCOMPARE(back.recordingQuality, 0);
        QVERIFY(!back.recordingCursor && !back.recordingCountdown);
        QVERIFY(back.recordingClicks);
        QCOMPARE(back.recordingFormat, 1);

        {
            QSettings ini(file, QSettings::IniFormat);
            ini.remove(u"Capture/RecordingQuality"_s);
            ini.setValue(u"Capture/RecordingHighQuality"_s, true); // the first version's preference
        }
        QCOMPARE(CaptureOptions::load(file).recordingQuality, 2);
        {
            QSettings ini(file, QSettings::IniFormat);
            ini.setValue(u"Capture/RecordingQuality"_s, 99);
            ini.setValue(u"Capture/RecordingFormat"_s, 99);
        }
        const CaptureOptions odd = CaptureOptions::load(file);
        QCOMPARE(odd.recordingQuality, 1);
        QCOMPARE(odd.recordingFormat, 0);
    }

    // The mouse buttons (#46): the old behaviour by default, kept across a
    // save, and a name not known (or "back" on a button that cannot step
    // back) falls to the default.
    void mouseActions()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(u"capture.ini"_s);
        const CaptureOptions fresh = CaptureOptions::load(file);
        QCOMPARE(fresh.mouseDoubleLeft, u"copy"_s);
        QCOMPARE(fresh.mouseRight, u"back"_s);
        QCOMPARE(fresh.mouseMiddle, u"none"_s);
        QCOMPARE(fresh.mouseBack, u"none"_s);
        QCOMPARE(fresh.mouseForward, u"none"_s);
        QVERIFY(!fresh.pinOnCopy);

        CaptureOptions o = fresh;
        o.mouseDoubleLeft = u"save"_s;
        o.mouseRight = u"cancel"_s;
        o.mouseMiddle = u"copypin"_s;
        o.pinOnCopy = true;
        o.mouseBack = u"ocr"_s;
        o.mouseForward = u"copy"_s;
        o.save(file);
        const CaptureOptions back = CaptureOptions::load(file);
        QCOMPARE(back.mouseDoubleLeft, u"save"_s);
        QCOMPARE(back.mouseRight, u"cancel"_s);
        QCOMPARE(back.mouseMiddle, u"copypin"_s);
        QVERIFY(back.pinOnCopy);
        QCOMPARE(back.mouseBack, u"ocr"_s);
        QCOMPARE(back.mouseForward, u"copy"_s);

        {
            QSettings ini(file, QSettings::IniFormat);
            ini.setValue(u"Capture/MouseDoubleLeft"_s, u"back"_s);
            ini.setValue(u"Capture/MouseRight"_s, u"Back"_s);
            ini.setValue(u"Capture/MouseMiddle"_s, u"nonsense"_s);
        }
        const CaptureOptions odd = CaptureOptions::load(file);
        QCOMPARE(odd.mouseDoubleLeft, u"copy"_s);
        QCOMPARE(odd.mouseRight, u"back"_s);
        QCOMPARE(odd.mouseMiddle, u"none"_s);
    }

    // The OCR choices of the mouse buttons and the toolbar's buttons left
    // off (#57), stage 7's options and smart redaction's.
    void toolbarStageSevenAndRedaction()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(u"capture.ini"_s);
        const CaptureOptions fresh = CaptureOptions::load(file);
        QVERIFY(fresh.toolbarHidden.isEmpty()); // all shown
        QCOMPARE(CaptureOptions::toolbarButtonIds(),
            (QStringList {u"undo"_s, u"redo"_s, u"copy"_s, u"save"_s, u"pin"_s, u"ocr"_s, u"long"_s, u"record"_s, u"qr"_s, u"delay"_s, u"cancel"_s}));

        CaptureOptions o = fresh;
        o.mouseDoubleLeft = u"ocrcopy"_s;
        o.mouseMiddle = u"ocrsearch"_s;
        o.mouseBack = u"ocrweb"_s;
        o.toolbarHidden = u"pin,redo"_s;
        o.save(file);
        QCOMPARE(QSettings(file, QSettings::IniFormat).value(u"Capture/ToolbarHidden"_s).toString(), u"pin,redo"_s);
        const CaptureOptions back = CaptureOptions::load(file);
        QCOMPARE(back.mouseDoubleLeft, u"ocrcopy"_s);
        QCOMPARE(back.mouseMiddle, u"ocrsearch"_s);
        QCOMPARE(back.mouseBack, u"ocrweb"_s);
        QCOMPARE(back.toolbarHidden, u"redo,pin"_s); // the toolbar's order

        // Unknown ids, repeats, case and blanks are cleaned on the way in.
        QCOMPARE(CaptureOptions::normalizedToolbarHidden(u" Cancel, OCR ,nonsense,cancel,,undo "_s), u"undo,ocr,cancel"_s);
        QCOMPARE(CaptureOptions::normalizedToolbarHidden(u"nothing"_s), QString());
        {
            QSettings ini(file, QSettings::IniFormat);
            ini.setValue(u"Capture/ToolbarHidden"_s, u"copy,save,bogus,copy"_s);
            ini.setValue(u"Capture/MouseRight"_s, u"ocrweb"_s);
        }
        const CaptureOptions odd = CaptureOptions::load(file);
        QCOMPARE(odd.toolbarHidden, u"copy,save"_s);
        QCOMPARE(odd.mouseRight, u"ocrweb"_s);
        // Written by hand with no quotes, "ocr,undo" is a list to QSettings: still read.
        writeByHand(file, "ToolbarHidden", "ocr,undo");
        CaptureOptions typed = CaptureOptions::load(file);
        QCOMPARE(typed.toolbarHidden, u"undo,ocr"_s);

        QVERIFY(!typed.cursor && typed.controlsUia && typed.delay == 5);
        QCOMPARE(typed.colorFormat, u"hex"_s);
        QVERIFY(typed.pinsHotkey.isEmpty() && typed.pinsRestoreHotkey.isEmpty());
        QCOMPARE(typed.pinDoubleClick, u"reset"_s);
        typed.cursor = true;
        typed.controlsUia = false;
        typed.delay = 10;
        typed.colorFormat = u"hsl"_s;
        typed.pinsHotkey = u"Ctrl+Alt+H"_s;
        typed.pinsRestoreHotkey = u"Ctrl+Alt+J"_s;
        typed.pinDoubleClick = u"close"_s;
        typed.save(file);
        const CaptureOptions seven = CaptureOptions::load(file);
        QVERIFY(seven.cursor && !seven.controlsUia);
        QCOMPARE(seven.delay, 10);
        QCOMPARE(seven.colorFormat, u"hsl"_s);
        QCOMPARE(seven.pinsHotkey, u"Ctrl+Alt+H"_s);
        QCOMPARE(seven.pinsRestoreHotkey, u"Ctrl+Alt+J"_s);
        QCOMPARE(seven.pinDoubleClick, u"close"_s);
        {
            QSettings ini(file, QSettings::IniFormat);
            ini.setValue(u"Capture/DelaySeconds"_s, 7);
            ini.setValue(u"Capture/ColorFormat"_s, u"cmyk"_s);
            ini.setValue(u"Capture/PinDoubleClick"_s, u"explode"_s);
        }
        const CaptureOptions odd7 = CaptureOptions::load(file);
        QCOMPARE(odd7.delay, 5);
        QCOMPARE(odd7.colorFormat, u"hex"_s);
        QCOMPARE(odd7.pinDoubleClick, u"reset"_s);

        QVERIFY(typed.smartPrescan && !typed.smartIpMac && !typed.smartAllText);
        QVERIFY(typed.smartWords.isEmpty());
        typed.smartPrescan = false;
        typed.smartWords = u"Atlas,客户甲"_s;
        typed.save(file);
        const CaptureOptions words = CaptureOptions::load(file);
        QVERIFY(!words.smartPrescan);
        QCOMPARE(words.smartWords, u"Atlas,客户甲"_s);
        writeByHand(file, "SmartWords", "Atlas, Beta;Atlas");
        QCOMPARE(CaptureOptions::load(file).smartWords, u"Atlas,Beta"_s);
        QCOMPARE(CaptureOptions::normalizedSmartWords(u"a，b、c；d\n e ,a"_s), (QStringList {u"a"_s, u"b"_s, u"c"_s, u"d"_s, u"e"_s}));
    }
};

QTEST_GUILESS_MAIN(CaptureSettingsTest)
#include "tst_capturesettings.moc"
