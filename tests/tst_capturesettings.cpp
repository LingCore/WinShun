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
QString pickFolder(HWND, const QString&, const QString&)
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
    void pauseHotkeysInFullScreen()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(u"capture.ini"_s);
        auto options = CaptureOptions::load(file);
        QVERIFY(!options.pauseInFullScreen);
        for (const bool paused : {true, false}) {
            options.pauseInFullScreen = paused;
            options.hotkey = u"Ctrl+Alt+F8"_s;
            options.save(file);
            const auto back = CaptureOptions::load(file);
            QCOMPARE(back.pauseInFullScreen, paused);
            QCOMPARE(back.hotkey, options.hotkey);
        }
    }

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

    // The key hints in a corner of the capture: on by default, the switch
    // (and the panel's 不再显示, through the same setter) saved at once.
    void keyHints()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(u"capture.ini"_s);
        QVERIFY(CaptureOptions::load(file).keyHints);
        ws::captureplugin::CaptureSettings settings(file);
        QVERIFY(settings.captureKeyHints());
        QSignalSpy changed(&settings, &ws::captureplugin::CaptureSettings::changed);
        settings.setCaptureKeyHints(false);
        QCOMPARE(changed.count(), 1);
        QVERIFY(!CaptureOptions::load(file).keyHints);
        settings.setCaptureKeyHints(false);
        QCOMPARE(changed.count(), 1); // the same again: nothing
        settings.setCaptureKeyHints(true);
        QVERIFY(CaptureOptions::load(file).keyHints);
        writeByHand(file, "KeyHints", "false");
        QVERIFY(!CaptureOptions::load(file).keyHints);
    }

    // The capture's and the pins' keys (#187), Capture/Keys/<id>: missing is
    // the default, empty is none, garbage (or a key the rules refuse) is the
    // default; only the keys changed are written, and a reset removes one.
    void keysStorage()
    {
        using ws::capture::KeyAction;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(u"capture.ini"_s);
        const auto keyIn = [&](const QString& id) { return QSettings(file, QSettings::IniFormat).value(u"Capture/Keys/"_s + id); };
        const auto has = [&](const QString& id) { return QSettings(file, QSettings::IniFormat).contains(u"Capture/Keys/"_s + id); };

        CaptureOptions options = CaptureOptions::load(file);
        QCOMPARE(options.keys, ws::capture::CaptureKeys::defaults());
        options.save(file);
        QVERIFY(!has(u"Pin"_s)); // defaults are not written

        options.keys.set(KeyAction::Pin, QKeyCombination(Qt::Key_F4));
        options.keys.set(KeyAction::Save, QKeyCombination(Qt::ControlModifier, Qt::Key_Comma)); // a comma in the INI
        options.keys.set(KeyAction::PinRotateLeft, QKeyCombination());
        options.keys.set(KeyAction::PinClose, QKeyCombination(Qt::ShiftModifier, Qt::Key_R)); // freed from 向左转 above
        options.save(file);
        QCOMPARE(keyIn(u"Pin"_s).toString(), u"F4"_s);
        QCOMPARE(keyIn(u"PinRotateLeft"_s).toString(), QString());
        QVERIFY(has(u"PinRotateLeft"_s));
        QVERIFY(!has(u"PinCopy"_s));
        const CaptureOptions back = CaptureOptions::load(file);
        QCOMPARE(back.keys.key(KeyAction::Pin), QKeyCombination(Qt::Key_F4));
        QCOMPARE(back.keys.key(KeyAction::Save), QKeyCombination(Qt::ControlModifier, Qt::Key_Comma));
        QCOMPARE(back.keys.key(KeyAction::PinClose), QKeyCombination(Qt::ShiftModifier, Qt::Key_R));
        QVERIFY(!back.keys.has(KeyAction::PinRotateLeft));
        QCOMPARE(back.keys, options.keys);

        // By hand: unquoted Ctrl+, (a list to QSettings), garbage, a key the capture uses, two on one key.
        {
            QSettings s(file, QSettings::IniFormat);
            s.setValue(u"Capture/Keys/PinCopy"_s, u"Ctrl+Shift+Alt+Nonsense"_s);
            s.setValue(u"Capture/Keys/PinSave"_s, u"Up"_s); // the pin's own (moves it)
            s.setValue(u"Capture/Keys/PinTextPick"_s, u"F4"_s);
            s.setValue(u"Capture/Keys/PinFlipVertical"_s, u"F4"_s); // twice: the first keeps it
        }
        writeByHand(file, "Keys\\Save", "Ctrl+,");
        const CaptureOptions hand = CaptureOptions::load(file);
        QCOMPARE(hand.keys.key(KeyAction::Save), QKeyCombination(Qt::ControlModifier, Qt::Key_Comma));
        QCOMPARE(hand.keys.key(KeyAction::PinCopy), QKeyCombination(Qt::ControlModifier, Qt::Key_C));
        QCOMPARE(hand.keys.key(KeyAction::PinSave), QKeyCombination(Qt::ControlModifier, Qt::Key_S));
        QCOMPARE(hand.keys.key(KeyAction::PinTextPick), QKeyCombination(Qt::Key_F4));
        QCOMPARE(hand.keys.key(KeyAction::PinFlipVertical), QKeyCombination(Qt::Key_V));
    }

    // The settings window's side: a key box's key taken or refused (and then
    // not stored), cleared, reset; each change told once. The plugin's own
    // hotkeys and (through the probe) other programs' are refused too.
    void keysFromTheSettingsWindow()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(u"capture.ini"_s);
        ws::captureplugin::CaptureSettings settings(file);
        QSignalSpy changed(&settings, &ws::captureplugin::CaptureSettings::changed);
        const auto text = [&](const char* id) { return settings.captureKeys().value(QLatin1StringView(id)).toMap().value(u"text"_s).toString(); };
        const auto entry = [&](const char* id) { return settings.captureKeys().value(QLatin1StringView(id)).toMap(); };
        QCOMPARE(settings.captureKeys().size(), qsizetype(ws::capture::keys::kCount));
        QCOMPARE(text("Pin"), u"F3"_s);
        QVERIFY(!entry("Pin").value(u"changed"_s).toBool());

        // Refused: a bare letter while capturing, an arrow in a pin, a key another action has.
        QVERIFY(!settings.setCaptureKey(u"Save"_s, Qt::Key_P, 0).isEmpty());
        QVERIFY(!settings.setCaptureKey(u"PinCopy"_s, Qt::Key_Left, 0).isEmpty());
        QVERIFY(settings.setCaptureKey(u"PinCopy"_s, Qt::Key_T, 0).contains(u"选取文字"_s));
        QCOMPARE(changed.count(), 0);
        QCOMPARE(text("Save"), u"Ctrl+S"_s);

        // Taken: stored at once, told once.
        QVERIFY(settings.setCaptureKey(u"Save"_s, Qt::Key_S, int(Qt::ControlModifier | Qt::ShiftModifier)).isEmpty());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(text("Save"), u"Ctrl+Shift+S"_s);
        QVERIFY(entry("Save").value(u"changed"_s).toBool());
        QCOMPARE(entry("Save").value(u"defaultText"_s).toString(), u"Ctrl+S"_s);
        QCOMPARE(CaptureOptions::load(file).keys.key(ws::capture::KeyAction::Save),
            QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_S));

        // A modifier alone is no key; an unknown id nothing.
        QVERIFY(settings.setCaptureKey(u"Save"_s, Qt::Key_Control, int(Qt::ControlModifier)).isEmpty());
        QVERIFY(settings.setCaptureKey(u"Nothing"_s, Qt::Key_F5, 0).isEmpty());
        QCOMPARE(changed.count(), 1);

        // Cleared, then reset: none, then the default again (removed from the file).
        settings.clearCaptureKey(u"Pin"_s);
        QCOMPARE(changed.count(), 2);
        QCOMPARE(text("Pin"), QString());
        QVERIFY(!CaptureOptions::load(file).keys.has(ws::capture::KeyAction::Pin));
        settings.clearCaptureKey(u"Pin"_s);
        QCOMPARE(changed.count(), 2); // the same again: nothing
        settings.resetCaptureKey(u"Pin"_s);
        QCOMPARE(text("Pin"), u"F3"_s);
        QVERIFY(!QSettings(file, QSettings::IniFormat).contains(u"Capture/Keys/Pin"_s));
        // A default another action took meanwhile: reset to none.
        QVERIFY(settings.setCaptureKey(u"PinTextPick"_s, Qt::Key_C, int(Qt::ControlModifier)).contains(u"复制"_s));
        settings.clearCaptureKey(u"PinCopy"_s);
        QVERIFY(settings.setCaptureKey(u"PinTextPick"_s, Qt::Key_C, int(Qt::ControlModifier)).isEmpty());
        settings.resetCaptureKey(u"PinCopy"_s);
        QCOMPARE(text("PinCopy"), QString());

        // The plugin's own hotkeys: refused by name, and shown as a conflict once one is set later.
        settings.setCaptureHotkey(u"Ctrl+Alt+A"_s);
        QVERIFY(settings.setCaptureKey(u"Pin"_s, Qt::Key_A, int(Qt::ControlModifier | Qt::AltModifier)).contains(u"开始截图"_s));
        QVERIFY(settings.setCaptureKey(u"Pin"_s, Qt::Key_F6, 0).isEmpty());
        QVERIFY(entry("Pin").value(u"conflict"_s).toString().isEmpty());
        settings.setCapturePresentHotkey(u"F6"_s);
        QVERIFY(entry("Pin").value(u"conflict"_s).toString().contains(u"演示"_s));

        // Other programs' (the probe): asked with RegisterHotKey's modifiers and key.
        unsigned askedModifiers = 0;
        unsigned askedKey = 0;
        settings.setHotkeyProbe([&](unsigned modifiers, unsigned vk) {
            askedModifiers = modifiers;
            askedKey = vk;
            return vk != 0x77; // F8 is held
        });
        QVERIFY(!settings.setCaptureKey(u"PinCollapse"_s, Qt::Key_F8, 0).isEmpty());
        QCOMPARE(askedKey, 0x77u);
        QVERIFY(settings.setCaptureKey(u"PinCollapse"_s, Qt::Key_K, int(Qt::ControlModifier | Qt::ShiftModifier)).isEmpty());
        QCOMPARE(askedModifiers, 2u | 4u); // MOD_CONTROL | MOD_SHIFT
        QCOMPARE(askedKey, unsigned('K'));
        QCOMPARE(text("PinCollapse"), u"Ctrl+Shift+K"_s);

        // A pin hears keys, not characters: Shift+1 typed as "!" is Shift+1 there (the scan code says which key).
        QVERIFY(settings.setCaptureKey(u"PinFlipVertical"_s, Qt::Key_Exclam, int(Qt::ShiftModifier), 0x02).isEmpty());
        QCOMPARE(text("PinFlipVertical"), u"Shift+1"_s);
    }

    // 识别屏幕文字's hotkey (#198), Capture/OcrHotkey: none by default, kept
    // trimmed across a save; the same key as another of the plugin's hotkeys
    // is said (that one keeps it), and a capture or pin key on it is refused
    // or shown as a conflict, as for the others.
    void ocrHotkey()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(u"capture.ini"_s);
        QVERIFY(CaptureOptions::load(file).ocrHotkey.isEmpty());
        QVERIFY(QSettings(file, QSettings::IniFormat).contains(u"Capture/OcrHotkey"_s)); // written, so the file shows it

        ws::captureplugin::CaptureSettings settings(file);
        QSignalSpy changed(&settings, &ws::captureplugin::CaptureSettings::changed);
        QVERIFY(settings.ocrHotkeyClash().isEmpty()); // none set
        settings.setCaptureOcrHotkey(u" Ctrl+Alt+T "_s);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(settings.captureOcrHotkey(), u"Ctrl+Alt+T"_s);
        QCOMPARE(CaptureOptions::load(file).ocrHotkey, u"Ctrl+Alt+T"_s);
        settings.setCaptureOcrHotkey(u"Ctrl+Alt+T"_s);
        QCOMPARE(changed.count(), 1); // the same again: nothing
        QVERIFY(settings.ocrHotkeyClash().isEmpty());

        // The same key as the screenshot's (written another way), as PrtScn once it is taken.
        settings.setCaptureHotkey(u"Alt+Ctrl+T"_s);
        QVERIFY(settings.ocrHotkeyClash().contains(u"开始截图"_s));
        settings.setCaptureHotkey(u"F1"_s);
        QVERIFY(settings.ocrHotkeyClash().isEmpty());
        settings.setCaptureOcrHotkey(u"PrintScreen"_s);
        QVERIFY(settings.ocrHotkeyClash().isEmpty());
        settings.setCapturePrintScreen(true);
        QVERIFY(settings.ocrHotkeyClash().contains(u"PrtScn"_s));
        settings.setCapturePrintScreen(false);
        settings.setCapturePresentHotkey(u"F7"_s);
        settings.setCaptureOcrHotkey(u"F7"_s);
        QVERIFY(settings.ocrHotkeyClash().contains(u"演示"_s));
        settings.setCaptureOcrHotkey(u"F8"_s);

        // The capture's and the pins' keys: refused on it, a conflict once it is set later.
        QVERIFY(settings.setCaptureKey(u"Pin"_s, Qt::Key_F8, 0).contains(u"识别屏幕文字"_s));
        QVERIFY(settings.setCaptureKey(u"Pin"_s, Qt::Key_F9, 0).isEmpty());
        const auto conflict = [&] { return settings.captureKeys().value(u"Pin"_s).toMap().value(u"conflict"_s).toString(); };
        QVERIFY(conflict().isEmpty());
        settings.setCaptureOcrHotkey(u"F9"_s);
        QVERIFY(conflict().contains(u"识别屏幕文字"_s));
        settings.setCaptureOcrHotkey({});
        QVERIFY(conflict().isEmpty());
        QVERIFY(CaptureOptions::load(file).ocrHotkey.isEmpty());
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
            (QStringList {u"undo"_s, u"redo"_s, u"copy"_s, u"save"_s, u"pin"_s, u"ocr"_s, u"long"_s, u"record"_s, u"qr"_s, u"delay"_s, u"cancel"_s, u"help"_s}));

        CaptureOptions o = fresh;
        o.mouseDoubleLeft = u"ocrcopy"_s;
        o.mouseMiddle = u"ocrsearch"_s;
        o.mouseBack = u"ocrweb"_s;
        o.toolbarHidden = u"pin,redo,help"_s;
        o.save(file);
        QCOMPARE(QSettings(file, QSettings::IniFormat).value(u"Capture/ToolbarHidden"_s).toString(), u"pin,redo,help"_s);
        const CaptureOptions back = CaptureOptions::load(file);
        QCOMPARE(back.mouseDoubleLeft, u"ocrcopy"_s);
        QCOMPARE(back.mouseMiddle, u"ocrsearch"_s);
        QCOMPARE(back.mouseBack, u"ocrweb"_s);
        QCOMPARE(back.toolbarHidden, u"redo,pin,help"_s); // the toolbar's order

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
