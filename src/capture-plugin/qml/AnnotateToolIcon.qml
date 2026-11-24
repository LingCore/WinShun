pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Shared Phosphor icon for every annotation tool (AnnotationStyles' Kind order:
// Rect, Ellipse, Arrow, Line, Pen, Marker, Text, Number, Redact, Spotlight),
// shared by AnnotateToolbar and PresenterToolbar.
Glyph {
    id: icon

    property int kind: 0
    readonly property var names: [
        "square",
        "circle",
        "arrow-up-right",
        "line-segment",
        "pen",
        "highlighter",
        "text-t",
        "number-circle-one",
        "checkerboard",
        "vignette",
    ]

    iconName: kind >= 0 && kind < names.length ? names[kind] : ""
    size: 16
}
