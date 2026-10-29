#include "CaptureSettings.h"

#include "platform/Shell.h"

#include <QRegularExpression>
#include <QSettings>
#include <QWindow>

using namespace Qt::StringLiterals;

namespace ws::captureplugin {

namespace {

// A value that is not there is written, so the file shows what can be set.
template <typename T> T readOrDefault(QSettings& s, const QString& key, const T& fallback)
{
    if (!s.contains(key)) {
        s.setValue(key, fallback);
        return fallback;
    }
    // A number written by hand that is none ("abc", "1h"): the default, not 0.
    if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
        QVariant value = s.value(key);
        return value.convert(QMetaType::fromType<T>()) ? value.value<T>() : fallback;
    } else {
        return s.value(key).value<T>();
    }
}

const QStringList& mouseActions()
{
    static const QStringList known {u"none"_s, u"copy"_s, u"copypin"_s, u"save"_s, u"pin"_s, u"ocr"_s, u"ocrcopy"_s,
        u"ocrsearch"_s, u"ocrweb"_s, u"cancel"_s};
    return known;
}

// A hand-written "a,b" with no quotes reads back as a list (QSettings splits at commas).
QString listOrText(QSettings& s, const QString& key, const QString& fallback)
{
    const QVariant raw = s.value(key);
    if (raw.metaType().id() == QMetaType::QStringList)
        return raw.toStringList().join(u',');
    return raw.isValid() ? raw.toString() : fallback;
}

} // namespace

QStringList CaptureOptions::toolbarButtonIds()
{
    return {u"undo"_s, u"redo"_s, u"copy"_s, u"save"_s, u"pin"_s, u"ocr"_s, u"long"_s, u"record"_s, u"qr"_s, u"delay"_s, u"cancel"_s};
}

QString CaptureOptions::normalizedToolbarHidden(const QString& text)
{
    QStringList asked;
    for (const QString& part : text.split(u',', Qt::SkipEmptyParts))
        asked.append(part.trimmed().toLower());
    QStringList kept;
    for (const QString& id : toolbarButtonIds()) {
        if (asked.contains(id))
            kept.append(id);
    }
    return kept.join(u',');
}

// Words separated by commas (also the Chinese ones, semicolons, line breaks),
// each trimmed, none empty or twice.
QStringList CaptureOptions::normalizedSmartWords(const QString& text)
{
    static const QRegularExpression separators(u"[,;\\n\\r，、；]"_s);
    QStringList words;
    for (const QString& part : text.split(separators, Qt::SkipEmptyParts)) {
        const QString word = part.trimmed().left(64);
        if (!word.isEmpty() && !words.contains(word, Qt::CaseInsensitive))
            words.append(word);
    }
    return words;
}

CaptureOptions CaptureOptions::load(const QString& file)
{
    const CaptureOptions d;
    CaptureOptions o;
    QSettings s(file, QSettings::IniFormat);
    o.hotkey = readOrDefault(s, u"Capture/Hotkey"_s, d.hotkey).trimmed();
    o.printScreen = readOrDefault(s, u"Capture/PrintScreen"_s, d.printScreen);
    o.printScreenBefore = readOrDefault(s, u"Capture/PrintScreenBefore"_s, d.printScreenBefore).trimmed();
    o.copyOnSelect = readOrDefault(s, u"Capture/CopyOnSelect"_s, d.copyOnSelect);
    o.magnifier = readOrDefault(s, u"Capture/Magnifier"_s, d.magnifier);
    o.pinOnCopy = readOrDefault(s, u"Capture/PinOnCopy"_s, d.pinOnCopy);
    const auto mouse = [&](const QString& key, const QString& fallback, bool allowBack) {
        const QString v = readOrDefault(s, key, fallback).trimmed().toLower();
        return mouseActions().contains(v) || (allowBack && v == u"back") ? v : fallback;
    };
    o.mouseDoubleLeft = mouse(u"Capture/MouseDoubleLeft"_s, d.mouseDoubleLeft, false);
    o.mouseRight = mouse(u"Capture/MouseRight"_s, d.mouseRight, true);
    o.mouseMiddle = mouse(u"Capture/MouseMiddle"_s, d.mouseMiddle, false);
    o.mouseBack = mouse(u"Capture/MouseBack"_s, d.mouseBack, false);
    o.mouseForward = mouse(u"Capture/MouseForward"_s, d.mouseForward, false);
    o.toolbarHidden = normalizedToolbarHidden(listOrText(s, u"Capture/ToolbarHidden"_s, d.toolbarHidden));
    o.annotateStyles = readOrDefault(s, u"Capture/AnnotateStyles"_s, d.annotateStyles).trimmed();
    o.smartPrescan = readOrDefault(s, u"Capture/SmartPrescan"_s, d.smartPrescan);
    o.smartIpMac = readOrDefault(s, u"Capture/SmartIpMac"_s, d.smartIpMac);
    o.smartAllText = readOrDefault(s, u"Capture/SmartAllText"_s, d.smartAllText);
    o.smartWords = normalizedSmartWords(listOrText(s, u"Capture/SmartWords"_s, d.smartWords)).join(u',');
    o.cursor = readOrDefault(s, u"Capture/IncludeCursor"_s, d.cursor);
    o.hdrFile = readOrDefault(s, u"Capture/HdrFile"_s, d.hdrFile);
    o.controlsUia = readOrDefault(s, u"Capture/ControlsUia"_s, d.controlsUia);
    const int seconds = readOrDefault(s, u"Capture/DelaySeconds"_s, d.delay);
    o.delay = seconds == 3 || seconds == 5 || seconds == 10 ? seconds : d.delay;
    const QString format = readOrDefault(s, u"Capture/ColorFormat"_s, d.colorFormat).trimmed().toLower();
    o.colorFormat = format == u"rgb" || format == u"hsl" ? format : u"hex"_s;
    o.pinsHotkey = readOrDefault(s, u"Capture/PinsHotkey"_s, d.pinsHotkey).trimmed();
    o.pinsRestoreHotkey = readOrDefault(s, u"Capture/PinsRestoreHotkey"_s, d.pinsRestoreHotkey).trimmed();
    o.lastPinHotkey = readOrDefault(s, u"Capture/LastPinHotkey"_s, d.lastPinHotkey).trimmed();
    o.saveFolder = readOrDefault(s, u"Capture/SaveFolder"_s, d.saveFolder).trimmed();
    o.recordingFolder = readOrDefault(s, u"Capture/RecordingFolder"_s, d.recordingFolder).trimmed();
    o.recordingSystemAudio = readOrDefault(s, u"Capture/RecordingSystemAudio"_s, d.recordingSystemAudio);
    o.recordingMicrophone = readOrDefault(s, u"Capture/RecordingMicrophone"_s, d.recordingMicrophone);
    o.recordingQuality = readOrDefault(s, u"Capture/RecordingQuality"_s, d.recordingQuality);
    if (o.recordingQuality < 0 || o.recordingQuality > 2)
        o.recordingQuality = d.recordingQuality;
    o.recordingCursor = readOrDefault(s, u"Capture/RecordingCursor"_s, d.recordingCursor);
    o.recordingCountdown = readOrDefault(s, u"Capture/RecordingCountdown"_s, d.recordingCountdown);
    o.recordingClicks = readOrDefault(s, u"Capture/RecordingClicks"_s, d.recordingClicks);
    o.recordingFormat = readOrDefault(s, u"Capture/RecordingFormat"_s, d.recordingFormat);
    if (o.recordingFormat < 0 || o.recordingFormat > 1)
        o.recordingFormat = d.recordingFormat;
    o.recordingKeys = readOrDefault(s, u"Capture/RecordingKeys"_s, d.recordingKeys).trimmed().toLower();
    if (o.recordingKeys != u"shortcuts" && o.recordingKeys != u"all")
        o.recordingKeys = d.recordingKeys;
    o.recordingMouseButtons = readOrDefault(s, u"Capture/RecordingMouseButtons"_s, d.recordingMouseButtons);
    const QString action = readOrDefault(s, u"Capture/PinDoubleClick"_s, d.pinDoubleClick).trimmed().toLower();
    o.pinDoubleClick = action == u"close" || action == u"collapse" || action == u"none" ? action : u"reset"_s;
    return o;
}

void CaptureOptions::save(const QString& file) const
{
    QSettings s(file, QSettings::IniFormat);
    s.setValue(u"Capture/Hotkey"_s, hotkey);
    s.setValue(u"Capture/PrintScreen"_s, printScreen);
    s.setValue(u"Capture/PrintScreenBefore"_s, printScreenBefore);
    s.setValue(u"Capture/CopyOnSelect"_s, copyOnSelect);
    s.setValue(u"Capture/Magnifier"_s, magnifier);
    s.setValue(u"Capture/PinOnCopy"_s, pinOnCopy);
    s.setValue(u"Capture/MouseDoubleLeft"_s, mouseDoubleLeft);
    s.setValue(u"Capture/MouseRight"_s, mouseRight);
    s.setValue(u"Capture/MouseMiddle"_s, mouseMiddle);
    s.setValue(u"Capture/MouseBack"_s, mouseBack);
    s.setValue(u"Capture/MouseForward"_s, mouseForward);
    s.setValue(u"Capture/ToolbarHidden"_s, toolbarHidden);
    s.setValue(u"Capture/AnnotateStyles"_s, annotateStyles);
    s.setValue(u"Capture/SmartPrescan"_s, smartPrescan);
    s.setValue(u"Capture/SmartIpMac"_s, smartIpMac);
    s.setValue(u"Capture/SmartAllText"_s, smartAllText);
    s.setValue(u"Capture/SmartWords"_s, smartWords);
    s.setValue(u"Capture/IncludeCursor"_s, cursor);
    s.setValue(u"Capture/HdrFile"_s, hdrFile);
    s.setValue(u"Capture/ControlsUia"_s, controlsUia);
    s.setValue(u"Capture/DelaySeconds"_s, delay);
    s.setValue(u"Capture/ColorFormat"_s, colorFormat);
    s.setValue(u"Capture/PinsHotkey"_s, pinsHotkey);
    s.setValue(u"Capture/PinsRestoreHotkey"_s, pinsRestoreHotkey);
    s.setValue(u"Capture/LastPinHotkey"_s, lastPinHotkey);
    s.setValue(u"Capture/SaveFolder"_s, saveFolder);
    s.setValue(u"Capture/RecordingFolder"_s, recordingFolder);
    s.setValue(u"Capture/RecordingSystemAudio"_s, recordingSystemAudio);
    s.setValue(u"Capture/RecordingMicrophone"_s, recordingMicrophone);
    s.setValue(u"Capture/RecordingQuality"_s, recordingQuality);
    s.setValue(u"Capture/RecordingCursor"_s, recordingCursor);
    s.setValue(u"Capture/RecordingCountdown"_s, recordingCountdown);
    s.setValue(u"Capture/RecordingClicks"_s, recordingClicks);
    s.setValue(u"Capture/RecordingFormat"_s, recordingFormat);
    s.setValue(u"Capture/RecordingKeys"_s, recordingKeys);
    s.setValue(u"Capture/RecordingMouseButtons"_s, recordingMouseButtons);
    s.setValue(u"Capture/PinDoubleClick"_s, pinDoubleClick);
}

CaptureSettings::CaptureSettings(QString file, QObject* parent)
    : QObject(parent)
    , m_file(std::move(file))
    , m_options(CaptureOptions::load(m_file))
{
}

void CaptureSettings::update(const std::function<void(CaptureOptions&)>& change)
{
    CaptureOptions changed = m_options;
    change(changed);
    m_options = changed;
    commit();
}

void CaptureSettings::commit()
{
    m_options.save(m_file);
    emit changed();
}

void CaptureSettings::setMouse(QString& field, const QString& action, bool allowBack)
{
    if (field == action || !(mouseActions().contains(action) || (allowBack && action == u"back")))
        return;
    field = action;
    commit();
}

void CaptureSettings::setCaptureDelay(int seconds)
{
    if (seconds == 3 || seconds == 5 || seconds == 10)
        change(m_options.delay, seconds);
}

void CaptureSettings::setCapturePinDoubleClick(const QString& action)
{
    if (action == u"reset" || action == u"close" || action == u"collapse" || action == u"none")
        change(m_options.pinDoubleClick, action);
}

void CaptureSettings::setProblems(const Problems& p)
{
    if (p.hotkey == m_hotkeyError && p.pinsHotkey == m_pinsHotkeyError && p.pinsRestoreHotkey == m_pinsRestoreHotkeyError
        && p.lastPinHotkey == m_lastPinHotkeyError && p.note == m_note)
        return;
    m_hotkeyError = p.hotkey;
    m_pinsHotkeyError = p.pinsHotkey;
    m_pinsRestoreHotkeyError = p.pinsRestoreHotkey;
    m_lastPinHotkeyError = p.lastPinHotkey;
    m_note = p.note;
    emit problemsChanged();
}

void CaptureSettings::setRecordingHotkey(bool on)
{
    if (m_recordingHotkey == on)
        return;
    m_recordingHotkey = on;
    emit recordingHotkeyChanged();
}

QString CaptureSettings::pickFolder(const QString& title)
{
    auto* window = qobject_cast<QWindow*>(m_window);
    return shell::pickFolder(window ? reinterpret_cast<HWND>(window->winId()) : nullptr, title);
}

} // namespace ws::captureplugin
