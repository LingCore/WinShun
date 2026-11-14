#pragma once

#include <QCoreApplication>
#include <QKeyCombination>
#include <QKeySequence>
#include <QString>
#include <Qt>

#include <array>
#include <cstddef>
#include <optional>

namespace ws::capture {

// The keys of the screen capture that the user may change (#187): the pin
// action while capturing and in the editing window, and the keys inside a pin
// window. Pure logic with no window in it, so the rules are tested on their
// own (tst_capturekeys). The windows ask this map; nothing else knows a key.
enum class KeyAction {
    Pin, // 贴到屏幕, in the capture and the editing window
    Save,
    PinClose, // from here on: inside the pin window
    PinCopy,
    PinSave,
    PinCollapse,
    PinTextPick,
    PinRotateRight,
    PinRotateLeft,
    PinFlipHorizontal,
    PinFlipVertical,
    PinEdit, // the editing window, as the menu's 编辑 (#124)
    PinOcr, // the menu's 识别文字 (#212)
    PinCollapseEdge, // the menu's 收到屏幕顶边; again: back in full (#212)
};

enum class KeyContext { Capture, PinWindow };

namespace keys {

struct ActionInfo {
    KeyAction action;
    const char* id; // the settings name: Capture/Keys/<id>
    KeyContext context;
    QKeyCombination key; // the default
    const char* name; // what a message calls it
};

// In the order of KeyAction.
inline constexpr std::array kActions {
    ActionInfo {KeyAction::Pin, "Pin", KeyContext::Capture, QKeyCombination(Qt::Key_F3),
        QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "贴到屏幕")},
    ActionInfo {KeyAction::Save, "Save", KeyContext::Capture, QKeyCombination(Qt::ControlModifier, Qt::Key_S),
        QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "保存")},
    ActionInfo {KeyAction::PinClose, "PinClose", KeyContext::PinWindow, QKeyCombination(Qt::Key_Escape),
        QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "关闭贴图")},
    ActionInfo {KeyAction::PinCopy, "PinCopy", KeyContext::PinWindow, QKeyCombination(Qt::ControlModifier, Qt::Key_C),
        QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "复制")},
    ActionInfo {KeyAction::PinSave, "PinSave", KeyContext::PinWindow, QKeyCombination(Qt::ControlModifier, Qt::Key_S),
        QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "保存")},
    ActionInfo {KeyAction::PinCollapse, "PinCollapse", KeyContext::PinWindow, QKeyCombination(Qt::Key_Space),
        QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "收起/展开")},
    ActionInfo {KeyAction::PinTextPick, "PinTextPick", KeyContext::PinWindow, QKeyCombination(Qt::Key_T),
        QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "选取文字")},
    ActionInfo {KeyAction::PinRotateRight, "PinRotateRight", KeyContext::PinWindow, QKeyCombination(Qt::Key_R),
        QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "向右转")},
    ActionInfo {KeyAction::PinRotateLeft, "PinRotateLeft", KeyContext::PinWindow,
        QKeyCombination(Qt::ShiftModifier, Qt::Key_R), QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "向左转")},
    ActionInfo {KeyAction::PinFlipHorizontal, "PinFlipHorizontal", KeyContext::PinWindow, QKeyCombination(Qt::Key_H),
        QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "左右翻转")},
    ActionInfo {KeyAction::PinFlipVertical, "PinFlipVertical", KeyContext::PinWindow, QKeyCombination(Qt::Key_V),
        QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "上下翻转")},
    ActionInfo {KeyAction::PinEdit, "PinEdit", KeyContext::PinWindow, QKeyCombination(Qt::Key_E),
        QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "编辑")},
    ActionInfo {KeyAction::PinOcr, "PinOcr", KeyContext::PinWindow, QKeyCombination(Qt::Key_O),
        QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "识别文字")},
    ActionInfo {KeyAction::PinCollapseEdge, "PinCollapseEdge", KeyContext::PinWindow, QKeyCombination(Qt::Key_U),
        QT_TRANSLATE_NOOP("ws::capture::CaptureKeys", "收到屏幕顶边")},
};

inline constexpr std::size_t kCount = kActions.size();

constexpr const ActionInfo& info(KeyAction action) { return kActions[static_cast<std::size_t>(action)]; }

// Enter on the keypad and the main one are one key.
constexpr int normalKey(int key) { return key == Qt::Key_Enter ? int(Qt::Key_Return) : key; }

constexpr bool isModifierKey(int key)
{
    return key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta
        || key == Qt::Key_AltGr || key == Qt::Key_CapsLock || key == Qt::Key_NumLock || key == Qt::Key_ScrollLock;
}

// A key that types a character (the specials start at 0x01000000).
constexpr bool isPrintable(int key) { return key >= 0x20 && key < 0x01000000; }

constexpr bool isArrow(int key)
{
    return key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up || key == Qt::Key_Down;
}

// Win32 virtual-key codes, written out so that no <windows.h> is needed here.
inline constexpr unsigned kVkBack = 0x08, kVkTab = 0x09, kVkReturn = 0x0D, kVkEscape = 0x1B, kVkSpace = 0x20,
                          kVkPrior = 0x21, kVkNext = 0x22, kVkEnd = 0x23, kVkHome = 0x24, kVkLeft = 0x25,
                          kVkUp = 0x26, kVkRight = 0x27, kVkDown = 0x28, kVkInsert = 0x2D, kVkDelete = 0x2E,
                          kVkNumpad0 = 0x60, kVkNumpad9 = 0x69, kVkAdd = 0x6B, kVkSubtract = 0x6D, kVkF1 = 0x70,
                          kVkF24 = 0x87, kVkOemPlus = 0xBB, kVkOemMinus = 0xBD, kVkSnapshot = 0x2C;

// The keys a pin window can be told about (fromVirtualKey makes exactly these).
constexpr bool pinCanHear(int key)
{
    key = normalKey(key);
    return (key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9)
        || (key >= Qt::Key_F1 && key <= Qt::Key_F24) || isArrow(key)
        || key == Qt::Key_Space || key == Qt::Key_Escape || key == Qt::Key_Return || key == Qt::Key_Tab
        || key == Qt::Key_Backspace || key == Qt::Key_Delete || key == Qt::Key_Insert || key == Qt::Key_Home
        || key == Qt::Key_End || key == Qt::Key_PageUp || key == Qt::Key_PageDown || key == Qt::Key_Plus
        || key == Qt::Key_Minus;
}

} // namespace keys

class CaptureKeys {
public:
    CaptureKeys()
    {
        for (const keys::ActionInfo& a : keys::kActions)
            m_keys[static_cast<std::size_t>(a.action)] = a.key;
    }
    static CaptureKeys defaults() { return {}; }

    // Qt::Key_unknown: no key (the user cleared it).
    QKeyCombination key(KeyAction action) const { return m_keys[static_cast<std::size_t>(action)]; }
    void set(KeyAction action, QKeyCombination key) { m_keys[static_cast<std::size_t>(action)] = normalized(key); }
    bool has(KeyAction action) const { return key(action).key() != Qt::Key_unknown; }
    bool operator==(const CaptureKeys&) const = default;

    // Exactly this key with exactly these modifiers (the keypad's does not count).
    bool matches(KeyAction action, int qtKey, Qt::KeyboardModifiers modifiers) const
    {
        const QKeyCombination k = key(action);
        return k.key() != Qt::Key_unknown && k.keyboardModifiers() == (modifiers & ~Qt::KeypadModifier)
            && keys::normalKey(k.key()) == keys::normalKey(qtKey);
    }
    // Which action of `context` this key is.
    std::optional<KeyAction> triggered(KeyContext context, int qtKey, Qt::KeyboardModifiers modifiers) const
    {
        for (const keys::ActionInfo& a : keys::kActions) {
            if (a.context == context && matches(a.action, qtKey, modifiers))
                return a.action;
        }
        return std::nullopt;
    }

    // What a menu or a tip shows ("Ctrl+S"); empty: no key.
    QString text(KeyAction action) const
    {
        return has(action) ? QKeySequence(key(action)).toString(QKeySequence::NativeText) : QString();
    }
    // What the settings keep ("Ctrl+S", language-free); empty: no key.
    QString portable(KeyAction action) const
    {
        return has(action) ? QKeySequence(key(action)).toString(QKeySequence::PortableText) : QString();
    }
    // Empty text: no key (a valid answer). Garbage, or more than one key: nothing.
    static std::optional<QKeyCombination> parse(const QString& text)
    {
        if (text.trimmed().isEmpty())
            return QKeyCombination();
        const QKeySequence sequence = QKeySequence::fromString(text.trimmed(), QKeySequence::PortableText);
        if (sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown)
            return std::nullopt;
        return normalized(sequence[0]);
    }

    // Why `candidate` may not be the key of `action`, in words for the user;
    // nothing when it may. Clearing is always fine.
    std::optional<QString> problem(KeyAction action, QKeyCombination candidate) const
    {
        candidate = normalized(candidate);
        const int k = candidate.key();
        if (k == Qt::Key_unknown)
            return std::nullopt;
        const KeyContext context = keys::info(action).context;
        for (const keys::ActionInfo& other : keys::kActions) {
            if (other.action != action && other.context == context && key(other.action) == candidate) {
                return tr("已被“%1”使用").arg(QCoreApplication::translate("ws::capture::CaptureKeys", other.name));
            }
        }
        const Qt::KeyboardModifiers mods = candidate.keyboardModifiers();
        const bool ctrl = mods & Qt::ControlModifier;
        const bool alt = mods & Qt::AltModifier;
        const bool meta = mods & Qt::MetaModifier;
        if (context == KeyContext::Capture) {
            // Win chords are the system's; PrtScn is the Snipping Tool's or the capture hotkey's.
            if (meta || k == Qt::Key_Print)
                return tr("截图时不支持这个键");
            // Letters and digits choose a tool or type into a text; Ctrl or Alt keeps them clear.
            if (keys::isPrintable(k) && !ctrl && !alt)
                return tr("截图时字母、数字会用来选工具或打字，请带上 Ctrl 或 Alt，或用功能键");
            if (captureReserved(k, ctrl, alt))
                return tr("截图时这个键已经有用处");
            return std::nullopt;
        }
        // The pin window: a key the window procedure cannot hear, or keeps for itself.
        if (!keys::pinCanHear(k) || meta)
            return tr("贴图窗口不支持这个键");
        if (keys::isArrow(k) || k == Qt::Key_Plus || k == Qt::Key_Minus || k == Qt::Key_F10
            || (alt && (k == Qt::Key_F4 || k == Qt::Key_Space)))
            return tr("贴图窗口里这个键已经有用处");
        return std::nullopt;
    }

    // A Win32 key press as a Qt key, whatever the keyboard layout (the pin
    // window is native and has no Qt key event). Nothing for a key we do not know.
    static QKeyCombination fromVirtualKey(unsigned vk, bool ctrl, bool shift, bool alt)
    {
        int key = Qt::Key_unknown;
        if (vk >= 'A' && vk <= 'Z')
            key = Qt::Key_A + int(vk - 'A');
        else if (vk >= '0' && vk <= '9')
            key = Qt::Key_0 + int(vk - '0');
        else if (vk >= keys::kVkNumpad0 && vk <= keys::kVkNumpad9)
            key = Qt::Key_0 + int(vk - keys::kVkNumpad0);
        else if (vk >= keys::kVkF1 && vk <= keys::kVkF24)
            key = Qt::Key_F1 + int(vk - keys::kVkF1);
        else {
            switch (vk) {
            case keys::kVkSpace: key = Qt::Key_Space; break;
            case keys::kVkEscape: key = Qt::Key_Escape; break;
            case keys::kVkReturn: key = Qt::Key_Return; break;
            case keys::kVkTab: key = Qt::Key_Tab; break;
            case keys::kVkBack: key = Qt::Key_Backspace; break;
            case keys::kVkDelete: key = Qt::Key_Delete; break;
            case keys::kVkInsert: key = Qt::Key_Insert; break;
            case keys::kVkHome: key = Qt::Key_Home; break;
            case keys::kVkEnd: key = Qt::Key_End; break;
            case keys::kVkPrior: key = Qt::Key_PageUp; break;
            case keys::kVkNext: key = Qt::Key_PageDown; break;
            case keys::kVkLeft: key = Qt::Key_Left; break;
            case keys::kVkRight: key = Qt::Key_Right; break;
            case keys::kVkUp: key = Qt::Key_Up; break;
            case keys::kVkDown: key = Qt::Key_Down; break;
            case keys::kVkOemPlus:
            case keys::kVkAdd: key = Qt::Key_Plus; break;
            case keys::kVkOemMinus:
            case keys::kVkSubtract: key = Qt::Key_Minus; break;
            default: break;
            }
        }
        if (key == Qt::Key_unknown)
            return {};
        Qt::KeyboardModifiers mods;
        if (ctrl)
            mods |= Qt::ControlModifier;
        if (shift)
            mods |= Qt::ShiftModifier;
        if (alt)
            mods |= Qt::AltModifier;
        return QKeyCombination(mods, Qt::Key(key));
    }

    // The other way, as RegisterHotKey takes it (MOD_ALT 1, MOD_CONTROL 2,
    // MOD_SHIFT 4, MOD_WIN 8): to ask whether a global hotkey holds the key.
    // Nothing for a key with no virtual-key code of its own here.
    struct VirtualKey {
        unsigned modifiers = 0;
        unsigned vk = 0;
        bool operator==(const VirtualKey&) const = default;
    };
    static std::optional<VirtualKey> toVirtualKey(QKeyCombination combination)
    {
        const int key = keys::normalKey(combination.key());
        unsigned vk = 0;
        if (key >= Qt::Key_A && key <= Qt::Key_Z)
            vk = 'A' + unsigned(key - Qt::Key_A);
        else if (key >= Qt::Key_0 && key <= Qt::Key_9)
            vk = '0' + unsigned(key - Qt::Key_0);
        else if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
            vk = keys::kVkF1 + unsigned(key - Qt::Key_F1);
        else {
            switch (key) {
            case Qt::Key_Space: vk = keys::kVkSpace; break;
            case Qt::Key_Escape: vk = keys::kVkEscape; break;
            case Qt::Key_Return: vk = keys::kVkReturn; break;
            case Qt::Key_Tab: vk = keys::kVkTab; break;
            case Qt::Key_Backspace: vk = keys::kVkBack; break;
            case Qt::Key_Delete: vk = keys::kVkDelete; break;
            case Qt::Key_Insert: vk = keys::kVkInsert; break;
            case Qt::Key_Home: vk = keys::kVkHome; break;
            case Qt::Key_End: vk = keys::kVkEnd; break;
            case Qt::Key_PageUp: vk = keys::kVkPrior; break;
            case Qt::Key_PageDown: vk = keys::kVkNext; break;
            case Qt::Key_Left: vk = keys::kVkLeft; break;
            case Qt::Key_Right: vk = keys::kVkRight; break;
            case Qt::Key_Up: vk = keys::kVkUp; break;
            case Qt::Key_Down: vk = keys::kVkDown; break;
            case Qt::Key_Plus: vk = keys::kVkOemPlus; break;
            case Qt::Key_Minus: vk = keys::kVkOemMinus; break;
            case Qt::Key_Print: vk = keys::kVkSnapshot; break;
            default: return std::nullopt;
            }
        }
        const Qt::KeyboardModifiers mods = combination.keyboardModifiers();
        VirtualKey result {0, vk};
        if (mods & Qt::AltModifier)
            result.modifiers |= 1;
        if (mods & Qt::ControlModifier)
            result.modifiers |= 2;
        if (mods & Qt::ShiftModifier)
            result.modifiers |= 4;
        if (mods & Qt::MetaModifier)
            result.modifiers |= 8;
        return result;
    }

private:
    static QString tr(const char* text) { return QCoreApplication::translate("ws::capture::CaptureKeys", text); }

    static QKeyCombination normalized(QKeyCombination c)
    {
        if (c.key() == Qt::Key_unknown)
            return {};
        return QKeyCombination(c.keyboardModifiers() & ~Qt::KeypadModifier, Qt::Key(keys::normalKey(c.key())));
    }

    // What the overlay and the editing window keep for themselves (AnnotationModel::key,
    // CaptureEditor::keyPressed, CaptureSession::key): the fixed keys and Ctrl chords.
    static bool captureReserved(int k, bool ctrl, bool alt)
    {
        if (keys::isModifierKey(k) || keys::isArrow(k))
            return true;
        switch (k) {
        case Qt::Key_Escape:
        case Qt::Key_Return:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
        case Qt::Key_Backspace:
        case Qt::Key_Delete:
            return true;
        case Qt::Key_F1:
            return !ctrl && !alt; // the hints, with Shift too (CaptureSession::key)
        case Qt::Key_F4:
            return alt; // closes the editing window
        default:
            break;
        }
        if (!ctrl || alt)
            return false;
        switch (k) {
        case Qt::Key_C: // copy
        case Qt::Key_V: // paste, cut: the text box's own (it hands the user's keys over, #187)
        case Qt::Key_X:
        case Qt::Key_Z: // undo, redo
        case Qt::Key_Y:
        case Qt::Key_A: // select all
        case Qt::Key_D: // duplicate, Ctrl+Shift+D delay
        case Qt::Key_L: // long capture
        case Qt::Key_R: // record
        case Qt::Key_Plus: // the editor's zoom
        case Qt::Key_Equal:
        case Qt::Key_Minus:
        case Qt::Key_Underscore:
        case Qt::Key_0:
        case Qt::Key_BracketLeft: // lower, raise
        case Qt::Key_BracketRight:
        case Qt::Key_BraceLeft:
        case Qt::Key_BraceRight:
            return true;
        default:
            return false;
        }
    }

    std::array<QKeyCombination, keys::kCount> m_keys;
};

} // namespace ws::capture
