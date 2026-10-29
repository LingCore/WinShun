#pragma once

#include "Annotation.h"

#include <QChar>
#include <QList>
#include <QRgb>
#include <QString>

#include <array>

// The annotation tools and their styles (docs/design-capture-roadmap.md,
// 3.5): the toolbar's order and keys, the defaults, and what each tool
// remembers between captures, in logical px so it fits every screen.
namespace ws::capture::annot {

// A tool: its key letter and digit (both plain keys: Ctrl+digit is PixPin's,
// C copies a colour, H hides the toolbar, I is the eyedropper, Z belongs to
// undo and zoom), its icon (a Segoe Fluent Icons glyph; empty for now, the
// toolbar picks them) and its name as written in the source: show it
// through toolName(), which translates it.
struct ToolInfo {
    Kind kind;
    QChar letter;
    int digit;
    QString glyph;
    QString name;
};
// The toolbar's order: R E A L P M T N B, 1 to 9.
const QList<ToolInfo>& tools();
QString toolName(Kind kind);
// The panel's eight colours.
const QList<QRgb>& palette();
// The widths of a text's outline, logical px (the panel adds "none").
QList<int> strokePresets();
// The panel's sizes for a tool, logical px: widths, font sizes or cells.
QList<int> sizePresets(Kind kind);
// The brush redaction's widths, logical px.
QList<int> brushWidths();
// Logical px to image px on a screen of scale `dpr` (3.4: 2 / 4 / 7 at 150 %
// are 3 / 6 / 11), never under 1; and back, as the panel shows a width.
int toImagePx(int logical, qreal dpr);
int toLogicalPx(int image, qreal dpr);

// What each tool remembers, kept in [Capture] AnnotateStyles as JSON:
// {"version": 1, "rect": {"color": "#E53935", "width": 4, "fill": "none"},
//  "text": {"color": "#E53935", "size": 18, "bold": false, "boxed": false, "stroke": 2, "strokeColor": "#FFFFFF"},
//  "redact": {"mode": "mosaic", "cell": 12, "shape": "brush", "width": 24, "color": "#212121"}, ...}
// Only tools changed from their defaults are written, so a later change of
// a default reaches everyone who never changed that tool.
class StyleMemory {
public:
    // Logical values; fields a kind does not use stay as defaults() has them.
    struct Tool {
        QRgb color = 0;
        int width = 4;
        Fill fill = Fill::None;
        int size = 18;
        bool bold = false;
        bool boxed = false;
        int stroke = 0; // Text: the outline's width, logical px (0: none)
        QRgb strokeColor = 0; // Text: the outline's colour; 0: automatic
        RedactMode mode = RedactMode::Mosaic;
        RedactShape shape = RedactShape::Box; // Redact: along a box or along a stroke
        int cell = 12;
        NumberFormat format = NumberFormat::Arabic; // Number: the last format chosen
        LineMode lineMode = LineMode::Straight;
        MarkerMode markerMode = MarkerMode::Freehand;
        bool operator==(const Tool&) const = default;
    };

    StyleMemory();
    // Unknown tools and fields, and unusable values, are ignored.
    static StyleMemory fromJson(const QString& json);
    QString toJson() const;
    // A new item's style on a screen of scale `dpr`; the fields its kind
    // does not use are Style's defaults, so doc.json leaves them out.
    Style forTool(Kind kind, qreal dpr) const;
    // The fields its kind uses, from an item's style.
    void remember(Kind kind, const Style& style, qreal dpr);
    static Tool defaults(Kind kind);
    // The number tool's last format: a new capture's first series starts with it.
    NumberFormat numberFormat() const;
    void setNumberFormat(NumberFormat format);
    // The tool's style back to the defaults.
    void reset(Kind kind);
    const Tool& tool(Kind kind) const;
    bool operator==(const StyleMemory&) const = default;

private:
    std::array<Tool, kKindCount> m_tools;
};
}
