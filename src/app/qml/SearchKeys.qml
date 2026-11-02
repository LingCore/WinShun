pragma Singleton
import QtQuick

QtObject {
    // Normalize aliases before the existing list/grid navigation handles
    // selection, scrolling and boundaries. IME and AltGr keep their keys.
    function navigationKey(key: int, modifiers: int, composing: bool): int {
        if (composing || (modifiers & ~Qt.KeypadModifier) !== Qt.ControlModifier)
            return key
        switch (key) {
        case Qt.Key_J:
        case Qt.Key_N: return Qt.Key_Down
        case Qt.Key_K:
        case Qt.Key_P: return Qt.Key_Up
        case Qt.Key_U: return Qt.Key_PageUp
        case Qt.Key_D: return Qt.Key_PageDown
        default: return key
        }
    }
}
