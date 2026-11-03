#include "CaptureSettings.h"

#include "platform/HotkeyText.h"
#include "platform/Shell.h"

#include <QCoreApplication>
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

QString keySetting(const capture::keys::ActionInfo& a)
{
    return u"Capture/Keys/"_s + QLatin1StringView(a.id);
}

const capture::keys::ActionInfo* actionById(const QString& id)
{
    for (const capture::keys::ActionInfo& a : capture::keys::kActions) {
        if (id == QLatin1StringView(a.id))
            return &a;
    }
    return nullptr;
}

} // namespace

QStringList CaptureOptions::toolbarButtonIds()
{
    return {u"undo"_s, u"redo"_s, u"copy"_s, u"save"_s, u"pin"_s, u"ocr"_s, u"long"_s, u"record"_s, u"qr"_s, u"delay"_s, u"cancel"_s, u"help"_s};
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
    o.pauseInFullScreen = readOrDefault(s, u"Capture/PauseInFullScreen"_s, d.pauseInFullScreen);
    o.printScreenBefore = readOrDefault(s, u"Capture/PrintScreenBefore"_s, d.printScreenBefore).trimmed();
    o.copyOnSelect = readOrDefault(s, u"Capture/CopyOnSelect"_s, d.copyOnSelect);
    o.magnifier = readOrDefault(s, u"Capture/Magnifier"_s, d.magnifier);
    o.keyHints = readOrDefault(s, u"Capture/KeyHints"_s, d.keyHints);
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
    o.historyCount = std::clamp(readOrDefault(s, u"Capture/HistoryCount"_s, d.historyCount), 0, 100);
    const QString format = readOrDefault(s, u"Capture/ColorFormat"_s, d.colorFormat).trimmed().toLower();
    o.colorFormat = format == u"rgb" || format == u"hsl" ? format : u"hex"_s;
    o.pinsHotkey = readOrDefault(s, u"Capture/PinsHotkey"_s, d.pinsHotkey).trimmed();
    o.pinsRestoreHotkey = readOrDefault(s, u"Capture/PinsRestoreHotkey"_s, d.pinsRestoreHotkey).trimmed();
    o.lastPinHotkey = readOrDefault(s, u"Capture/LastPinHotkey"_s, d.lastPinHotkey).trimmed();
    o.presentHotkey = readOrDefault(s, u"Capture/PresentHotkey"_s, d.presentHotkey).trimmed();
    o.saveFolder = readOrDefault(s, u"Capture/SaveFolder"_s, d.saveFolder).trimmed();
    o.recordingFolder = readOrDefault(s, u"Capture/RecordingFolder"_s, d.recordingFolder).trimmed();
    o.recordingSystemAudio = readOrDefault(s, u"Capture/RecordingSystemAudio"_s, d.recordingSystemAudio);
    o.recordingMicrophone = readOrDefault(s, u"Capture/RecordingMicrophone"_s, d.recordingMicrophone);
    o.recordingQuality = readOrDefault(s, u"Capture/RecordingQuality"_s,
        readOrDefault(s, u"Capture/RecordingHighQuality"_s, false) ? 2 : d.recordingQuality); // the first version's
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
    // The keys (#187): missing or unreadable, the default; empty, none. One
    // written by hand that the settings window would refuse (a key the capture
    // uses, one an action before it has): the default, or none when that is
    // taken too. Each is checked against those before it only.
    for (const capture::keys::ActionInfo& a : capture::keys::kActions)
        o.keys.set(a.action, {});
    for (const capture::keys::ActionInfo& a : capture::keys::kActions) {
        const QString name = keySetting(a);
        QKeyCombination key = a.key;
        if (s.contains(name))
            key = capture::CaptureKeys::parse(listOrText(s, name, {})).value_or(a.key);
        if (o.keys.problem(a.action, key))
            key = o.keys.problem(a.action, a.key) ? QKeyCombination() : a.key;
        o.keys.set(a.action, key);
    }
    return o;
}

void CaptureOptions::save(const QString& file) const
{
    QSettings s(file, QSettings::IniFormat);
    s.setValue(u"Capture/Hotkey"_s, hotkey);
    s.setValue(u"Capture/PrintScreen"_s, printScreen);
    s.setValue(u"Capture/PauseInFullScreen"_s, pauseInFullScreen);
    s.setValue(u"Capture/PrintScreenBefore"_s, printScreenBefore);
    s.setValue(u"Capture/CopyOnSelect"_s, copyOnSelect);
    s.setValue(u"Capture/Magnifier"_s, magnifier);
    s.setValue(u"Capture/KeyHints"_s, keyHints);
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
    s.setValue(u"Capture/HistoryCount"_s, historyCount);
    s.setValue(u"Capture/ColorFormat"_s, colorFormat);
    s.setValue(u"Capture/PinsHotkey"_s, pinsHotkey);
    s.setValue(u"Capture/PinsRestoreHotkey"_s, pinsRestoreHotkey);
    s.setValue(u"Capture/LastPinHotkey"_s, lastPinHotkey);
    s.setValue(u"Capture/PresentHotkey"_s, presentHotkey);
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
    // Only the keys changed: a default changed later reaches whoever never set one.
    for (const capture::keys::ActionInfo& a : capture::keys::kActions) {
        if (keys.key(a.action) == a.key)
            s.remove(keySetting(a));
        else
            s.setValue(keySetting(a), keys.portable(a.action));
    }
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
        && p.lastPinHotkey == m_lastPinHotkeyError && p.presentHotkey == m_presentHotkeyError && p.note == m_note)
        return;
    m_hotkeyError = p.hotkey;
    m_pinsHotkeyError = p.pinsHotkey;
    m_pinsRestoreHotkeyError = p.pinsRestoreHotkey;
    m_lastPinHotkeyError = p.lastPinHotkey;
    m_presentHotkeyError = p.presentHotkey;
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

QVariantMap CaptureSettings::captureKeys() const
{
    const capture::CaptureKeys& keys = m_options.keys;
    const capture::CaptureKeys defaults;
    QVariantMap map;
    for (const capture::keys::ActionInfo& a : capture::keys::kActions) {
        map.insert(QString::fromLatin1(a.id), QVariantMap {
            {u"text"_s, keys.text(a.action)},
            {u"changed"_s, keys.key(a.action) != a.key},
            {u"defaultText"_s, defaults.text(a.action)},
            {u"conflict"_s, keys.has(a.action) ? hotkeyConflict(keys.key(a.action), false) : QString()},
        });
    }
    return map;
}

QString CaptureSettings::setCaptureKey(const QString& id, int key, int modifiers, int scanCode)
{
    const capture::keys::ActionInfo* a = actionById(id);
    if (!a || key == 0 || key == Qt::Key_unknown || capture::keys::isModifierKey(key))
        return {};
    const auto mods = Qt::KeyboardModifiers(modifiers)
        & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier | Qt::KeypadModifier);
    QKeyCombination candidate(mods, Qt::Key(key));
    // The pin window hears keys (PinWindow: virtual keys), so Shift+1 is Shift
    // and 1 there, not "!": the key under the finger, found by its scan code.
    if (a->context == capture::KeyContext::PinWindow && !capture::keys::pinCanHear(key) && scanCode > 0) {
        const UINT vk = ::MapVirtualKeyW(UINT(scanCode) & 0xFF, MAPVK_VSC_TO_VK);
        const QKeyCombination heard = capture::CaptureKeys::fromVirtualKey(vk, mods.testFlag(Qt::ControlModifier),
            mods.testFlag(Qt::ShiftModifier), mods.testFlag(Qt::AltModifier));
        if (heard.key() != Qt::Key_unknown)
            candidate = QKeyCombination(heard.keyboardModifiers() | (mods & Qt::MetaModifier), heard.key());
    }
    if (const auto why = m_options.keys.problem(a->action, candidate))
        return *why;
    if (const QString why = hotkeyConflict(candidate, true); !why.isEmpty())
        return why;
    setKey(id, [&](capture::KeyAction) { return candidate; });
    return {};
}

void CaptureSettings::clearCaptureKey(const QString& id)
{
    setKey(id, [](capture::KeyAction) { return QKeyCombination(); });
}

void CaptureSettings::resetCaptureKey(const QString& id)
{
    // The default, unless another action has taken it meanwhile: then none.
    setKey(id, [this](capture::KeyAction action) {
        const QKeyCombination fallback = capture::keys::info(action).key;
        return m_options.keys.problem(action, fallback) ? QKeyCombination() : fallback;
    });
}

void CaptureSettings::setKey(const QString& id, const std::function<QKeyCombination(capture::KeyAction)>& key)
{
    const capture::keys::ActionInfo* a = actionById(id);
    if (!a)
        return;
    capture::CaptureKeys keys = m_options.keys;
    keys.set(a->action, key(a->action));
    change(m_options.keys, keys);
}

QString CaptureSettings::hotkeyConflict(QKeyCombination key, bool askSystem) const
{
    const auto tr = [](const char* text) { return QCoreApplication::translate("ws::captureplugin::CaptureSettings", text); };
    const QString normal = hotkeys::normalized(QKeySequence(key).toString(QKeySequence::PortableText));
    if (!normal.isEmpty()) {
        const CaptureOptions& o = m_options;
        const std::pair<QString, const char*> own[] {
            {o.hotkey, QT_TRANSLATE_NOOP("ws::captureplugin::CaptureSettings", "开始截图")},
            {o.printScreen ? u"PrintScreen"_s : QString(), QT_TRANSLATE_NOOP("ws::captureplugin::CaptureSettings", "用 PrtScn 键截图")},
            {o.lastPinHotkey, QT_TRANSLATE_NOOP("ws::captureplugin::CaptureSettings", "贴图")},
            {o.pinsHotkey, QT_TRANSLATE_NOOP("ws::captureplugin::CaptureSettings", "隐藏 / 显示全部贴图")},
            {o.pinsRestoreHotkey, QT_TRANSLATE_NOOP("ws::captureplugin::CaptureSettings", "找回穿透贴图")},
            {o.presentHotkey, QT_TRANSLATE_NOOP("ws::captureplugin::CaptureSettings", "演示 / 白板")},
        };
        for (const auto& [hotkey, name] : own) {
            if (!hotkey.isEmpty() && hotkeys::normalized(hotkey) == normal)
                return tr("和“%1”的快捷键相同，按下时会先被它用掉").arg(tr(name));
        }
    }
    if (askSystem && m_probe) {
        if (const auto vk = capture::CaptureKeys::toVirtualKey(key); vk && !m_probe(vk->modifiers, vk->vk))
            return tr("其他程序或 Win顺 已把它设为全局快捷键，按下时这里收不到");
    }
    return {};
}

QString CaptureSettings::pickFolder(const QString& title)
{
    auto* window = qobject_cast<QWindow*>(m_window);
    return shell::pickFolder(window ? reinterpret_cast<HWND>(window->winId()) : nullptr, title);
}

} // namespace ws::captureplugin
