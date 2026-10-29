#pragma once

#include "Annotation.h"
#include "AnnotationCommands.h"
#include "AnnotationFormat.h"
#include "AnnotationRender.h"
#include "AnnotationStyles.h"
#include "SmartRedact.h"

#include <QImage>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace ws::capture {

// The annotations of one capture, as the overlay shows and edits them
// (docs/design-capture-roadmap.md, 3.2, 3.5-3.9): the document, its undo
// stack, the tool, the selected object, the text being typed, the style
// panel. CaptureSession owns it and routes the mouse and keys to it (3.6);
// the canvas (AnnotationCanvas) draws it; AnnotateLayer.qml and the toolbar
// show its state and call back into the Q_INVOKABLE members.
//
// Every position here is in image pixels: the pixels of the frozen screen,
// which are the physical pixels of that monitor. The overlay's own unit
// (logical) never comes in. Logical sizes (a width of "4") appear only in
// panel map, as the user thinks of them; the document keeps image
// pixels (3.4).
//
// The editing window (stage 4b, CaptureEditor) uses the same model for a
// project: begin(project) makes it an editor, where the picture is the
// project's whole base, drawing is allowed anywhere on it, redactions read
// all of it, and `crop` is what is exported, dragged at its edges. Several
// objects can be selected there (and in the overlay): Shift+click, a box
// dragged on the empty picture, Ctrl+A.
class AnnotationModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Made by the capture")

    // The tool's index in annot::tools() (the toolbar's order); -1: none.
    Q_PROPERTY(int tool READ tool WRITE setTool NOTIFY toolChanged FINAL)
    // [{kind, name (translated), letter, digit, glyph}] in the toolbar's order.
    Q_PROPERTY(QVariantList tools READ toolList CONSTANT FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged FINAL)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged FINAL)
    Q_PROPERTY(QString undoLabel READ undoLabel NOTIFY historyChanged FINAL)
    Q_PROPERTY(QString redoLabel READ redoLabel NOTIFY historyChanged FINAL)
    Q_PROPERTY(bool hasItems READ hasItems NOTIFY itemsChanged FINAL)
    Q_PROPERTY(int itemCount READ itemCount NOTIFY itemsChanged FINAL)
    // The style panel is shown: a tool is on or an object is selected.
    Q_PROPERTY(bool panelVisible READ panelVisible NOTIFY panelChanged FINAL)
    // What the panel shows, logical units (see panel()).
    Q_PROPERTY(QVariantMap panel READ panel NOTIFY panelChanged FINAL)
    // The selected object (none or one): its outline and handles, to draw
    // over the canvas (see selection()).
    Q_PROPERTY(QVariantList selection READ selection NOTIFY selectionChanged FINAL)
    // The text being typed; empty map when none (see editing()).
    Q_PROPERTY(QVariantMap editing READ editing NOTIFY editingChanged FINAL)
    // A Number being renumbered by double click: {id, x, y (its centre,
    // image px), value (-1: counted), shown (what it shows now)}; empty when none.
    Q_PROPERTY(QVariantMap numberEdit READ numberEdit NOTIFY numberEditChanged FINAL)
    // A drawing, moving or reshaping gesture is under way.
    // The family the compositor draws text in (annotationFont); the text box uses the same.
    Q_PROPERTY(QString fontFamily READ fontFamily CONSTANT FINAL)
    // The editing window's model (begin(project)); the overlay's is not.
    Q_PROPERTY(bool editor READ editor NOTIFY editorChanged FINAL)
    // The editor's export range (image px); the selection in the overlay.
    Q_PROPERTY(QRect crop READ cropRect NOTIFY cropChanged FINAL)
    // The box being dragged to select objects (image px); null when none.
    Q_PROPERTY(QRectF marquee READ marquee NOTIFY marqueeChanged FINAL)
    // The eyedropper (I) is on: the next press takes the colour under it.
    Q_PROPERTY(bool picking READ picking NOTIFY pickingChanged FINAL)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged FINAL)
    // Smart redaction (4.1): how many places the last scan of the selection's
    // text found that are not hidden yet; -1 while there is nothing to offer
    // (no scan yet, one under way, nothing found, all hidden).
    Q_PROPERTY(int smartFound READ smartFound NOTIFY smartChanged FINAL)
    // A scan asked for by the button is under way.
    Q_PROPERTY(bool smartBusy READ smartBusy NOTIFY smartChanged FINAL)
    // The crop's edge is being dragged.
    Q_PROPERTY(bool cropping READ cropping NOTIFY gestureChanged FINAL)
    Q_PROPERTY(bool gestureActive READ gestureActive NOTIFY gestureChanged FINAL)
    Q_PROPERTY(bool pathActive READ pathActive NOTIFY gestureChanged FINAL)
    // What the gesture is changing (image px, set 16 logical px wider): the
    // toolbar steps aside when it reaches it (3.5). Null when no gesture.
    Q_PROPERTY(QRectF gestureBounds READ gestureBounds NOTIFY gestureChanged FINAL)

public:
    explicit AnnotationModel(QObject* parent = nullptr);
    ~AnnotationModel() override;

    // A new capture on one monitor's frozen picture: an empty document the
    // size of `base`, no tool, nothing selected, an empty undo stack; every
    // tool's style as `styles` remembers it. `dpr` is the monitor's scale.
    void begin(const QImage& base, qreal dpr, const annot::StyleMemory& styles);
    // The editing window's: a project opened (its base whole, its crop as it
    // was left) with every tool's style as `styles` remembers it.
    // `deviceDpr` is the window's scale, for a project that has none (a
    // picture that never was a screenshot). What the model is asked to keep
    // from here on is measured against this document (modified()).
    void begin(const annot::Project& project, qreal deviceDpr, const annot::StyleMemory& styles);
    // Everything let go of (the picture, the caches, the stack).
    void end();
    bool editor() const { return m_editor; }
    // The editor's picture shown at `zoom` on a window whose device pixel is
    // `devicePerLogical` logical px (its real buffer scale): hit tolerances,
    // handle reach and the drag distance are screen sizes, so in image px
    // they follow the zoom. The overlay is 1 at its monitor's scale (default).
    void setView(qreal devicePerLogical, qreal zoom);
    // The document differs from the one opened (a text under way counts).
    bool modified() const;
    // Esc in the editor: all changes given up in one step Ctrl+Z takes back.
    // Nothing when there is none.
    Q_INVOKABLE void discardChanges();
    bool active() const { return !m_base.isNull(); }
    // The selection rectangle (image px): the document's crop and the range
    // redactions take their colours from. Moving it redraws the redactions.
    void setSelection(const QRect& selection);
    QRect selectionRect() const { return m_document.crop; }
    QRect cropRect() const { return m_document.crop; }
    // What redactions take their colours from (3.10, S): the selection in
    // the overlay (all of the picture before there is one). The canvas and
    // composed() render with it.
    QRect sampleBounds() const;

    // ---- what the canvas and the capture read ----
    const annot::Document& document() const { return m_document; }
    const QImage& base() const { return m_base; }
    qreal dpr() const { return m_document.dpr; }
    annot::Compositor& compositor() { return *m_compositor; }
    // The text being edited is not drawn by the canvas (the TextEdit shows it).
    std::optional<quint32> skipped() const;
    // The Number whose words are being typed: drawn without them (the text box shows them).
    std::optional<quint32> unworded() const;
    // The brush redaction under the mouse (not yet let go): drawn as a mosaic meanwhile.
    std::optional<quint32> draft() const;
    // The styles as they are now (the tools' remembered ones, including this
    // capture's changes); `stylesChanged()` tells whether any changed.
    const annot::StyleMemory& styles() const { return m_styles; }
    bool stylesChanged() const { return m_stylesChanged; }
    // Smart redaction: how the text of the selection is read, the rules to
    // find, and whether to scan on its own (SmartRedact.h). The scan starts
    // 300 ms after the selection (the crop, in the editor) stops changing.
    void setSmartHooks(SmartHooks hooks);
    int smartFound() const;
    bool smartBusy() const;
    // 智能打码: every place found becomes a redaction, in the tool's remembered
    // style, as one undo step. Reads the text first when that has not been done.
    Q_INVOKABLE void smartRedact();
    // The composed picture of the selection (annot::Compositor::exportImage)
    // and the project that makes it editable again; both commit a text under
    // way first. Null / empty when there are no items. The editor's: the
    // crop of the picture, with or without items; the project is the whole
    // base with the crop kept in the document.
    QImage composed();
    annot::Project project() const;

    int tool() const { return m_tool; }
    void setTool(int tool); // index into annot::tools(); -1: none; ends a text under way
    bool pathActive() const;
    Q_INVOKABLE bool finishPath(bool closed = false);
    QVariantList toolList() const;
    QString fontFamily() const;
    bool canUndo() const;
    bool canRedo() const;
    QString undoLabel() const;
    QString redoLabel() const;
    bool hasItems() const { return !m_document.items.isEmpty(); }
    int itemCount() const { return static_cast<int>(m_document.items.size()); }
    bool panelVisible() const;
    // Keys, all in logical units except `colors` (RGB strings):
    //   kind        annot::Kind of the tool or selected object, -1 for none
    //   name        its translated name
    //   color       "#RRGGBB" (for a redaction: the solid colour)
    //   colors      the palette
    //   width, widths   line width and its presets (Rect, Ellipse, Arrow, Line, Pen, Marker)
    //   fill        "none" | "soft" | "solid" (Rect, Ellipse)
    //   size, sizes     font size and presets (Text, Number)
    //   bold, boxed     Text
    //   stroke, strokes     Text: the outline's width (0: none) and its presets, logical px
    //   strokeColor, strokeAuto     Text: the outline's "#RRGGBB" as drawn, and whether it follows the text's
    //   mode        "mosaic" | "blur" | "solid" (Redact)
    //   cell, cells     mosaic cell and presets (Redact)
    //   start       the current series' first number (Number)
    //   format      how the series writes its numbers: annot::numberFormatName
    //               (Number; the selected number's series, else the current one)
    //   selected    an object is selected (the panel shows its style, and
    //               a change goes to it as well as to the tool's memory)
    //   canRaise, canLower   the selected object can move up / down a layer
    QVariantMap panel() const;
    // [] or [{id, kind, x, y, width, height (the outline: its bounds, image
    // px), box (true: drawn with eight handles; false: a line's two end
    // handles), handles: [{x, y, index}] (image px; index is the value
    // reshaped() takes)}].
    QVariantList selection() const;
    // {id (0: a new text), x, y (the text's top left, image px), fontPx,
    // bold, boxed, color ("#RRGGBB" of the text), fill ("#RRGGBB" of the box
    // when boxed), text, pad and radius (the box's padding and corner radius
    // round the text, image px; 0 when not boxed), pitch (baseline to
    // baseline) and ascent (top to the first baseline), image px, as the
    // compositor lays the text out, clickX, clickY (where the press landed
    // inside it, relative to x, y; -1 when it began with a new text),
    // stroke (the outline's width, image px, 0: none) and strokeColor ("#RRGGBB")}.
    // The words of a Number are the same map (number: true, x, y the words'
    // top left to the right of the disc, not bold, never boxed).
    QVariantMap editing() const;
    QVariantMap numberEdit() const;
    bool gestureActive() const { return m_gesture != Gesture::None; }
    QRectF gestureBounds() const;
    QRectF marquee() const;
    bool picking() const { return m_picking; }
    bool cropping() const { return m_gesture == Gesture::Crop; }
    int selectedCount() const { return static_cast<int>(m_picked.size()); }

    // ---- the mouse, from CaptureSession (3.6); positions are image px ----
    // The first questions of a press, before the selection's own handles:
    // commit a text when the press is outside it; take a colour when the
    // eyedropper is on; a handle of the selected object; in the editor a
    // handle of the crop. Returns true when the press is taken.
    bool pressFirst(QPoint p, Qt::KeyboardModifiers modifiers);
    // The rest, after the selection's handles: with a tool, the selected
    // objects' bodies (move), a text or number under it (edit / select), or
    // a new object inside the selection (outside it: taken, nothing done,
    // notice()); without one, an object under the press (select, drag to
    // move; with Shift it joins or leaves the selected ones), and in the
    // editor the empty picture starts a box that selects what it touches. A text under the text tool is picked up like a number: a drag
    // moves it, a click (released without a drag) opens it for typing, the
    // caret where the press was. Returns true when taken; a press that does
    // nothing (outside the selection with a tool) is taken too, but
    // gestureActive() stays false.
    bool pressSecond(QPoint p, Qt::KeyboardModifiers modifiers);
    void move(QPoint p, Qt::KeyboardModifiers modifiers);
    // Ends the gesture: the object is kept (one undo step) or, when nothing
    // was drawn (a click that did not move), dropped.
    void release(QPoint p);
    // The cursor over `p` when the model has a say (Qt::CursorShape): the
    // selected object's handles, a movable object, the tool's cursor inside
    // the selection. -1 when the session's own cursors apply.
    int cursorAt(QPoint p) const;
    // `p` is on a handle of the selected object: pressFirst() takes it, so
    // its cursor comes before the selection's own handles (3.6, step 2
    // before 3); everything else cursorAt() says comes after them.
    bool onHandle(QPoint p) const;
    // A double click: a Text under it opens for editing, a Number for its
    // number. True as well on any other object and whenever a tool is on
    // (3.6: those never copy); false only on the bare picture without a
    // tool, where the session copies.
    bool doubleClick(QPoint p);
    // Wheel with a tool or a selected object: size steps; `angleDelta` is
    // the wheel's, summed until 120 makes a notch, one notch per 200 ms at
    // most (touchpads). True when the wheel is the model's (tool or selection).
    bool wheel(int angleDelta, qint64 nowMs);
    // The tool keys, undo and redo, Delete, arrows on a selected object,
    // [ and ], Ctrl+[ and Ctrl+], Ctrl+A (all objects), Ctrl+D (a copy of
    // the selected ones, a little aside), I (the eyedropper). True when taken.
    bool key(int key, Qt::KeyboardModifiers modifiers);
    // Esc, steps 2 and 3 of 3.6: commit the text under way, or cancel the
    // gesture under way; before either, close a number's edit box. True
    // when it did one of them.
    bool escape();
    bool editingText() const { return m_editing.has_value(); }
    bool editingNumber() const { return m_numberEdit.has_value(); }
    void cancelGesture();
    // Commits a text under way (for Ctrl+S / F3 / Enter, and the hotkey).
    void commitPending();

    // ---- from QML ----
    // The text box's end: the text typed. A new text with words becomes an
    // object, a new empty one is dropped, an old one with no words is
    // deleted, a changed one is retyped; the result is selected.
    Q_INVOKABLE void commitText(const QString& text);
    // The text box's words as they are now (TextEdit.onTextChanged): what
    // commitPending(), escape() and a press outside the box commit, since
    // those come from the session, not from the box. No signal: the box
    // has the words already (editing.text follows them).
    Q_INVOKABLE void setEditingText(const QString& text);
    // A change on the panel: any of color ("#RRGGBB"), width, fill, size,
    // bold, boxed, stroke (0: none; logical px), strokeColor ("#RRGGBB", or "auto"), mode, cell (logical units). Goes to the selected object
    // when there is one, and is remembered as the tool's style either way.
    Q_INVOKABLE void setPanel(const QVariantMap& change);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void deleteSelection();
    Q_INVOKABLE void raise(bool toTop);
    Q_INVOKABLE void lower(bool toBottom);
    // The selected objects' styles, and the tool's, back to the defaults
    // (恢复默认样式).
    Q_INVOKABLE void resetStyle();
    // The eyedropper on (or off): the next press on the picture sets the
    // colour of what the panel is for to the pixel under it.
    Q_INVOKABLE void pickColor(bool on = true);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void duplicateSelection();
    // "另起一组": the next numbers start a new group counting from 1.
    Q_INVOKABLE void newSeries();
    // "起始值": where the current group's numbers start.
    Q_INVOKABLE void setStart(int start);
    // 双击序号: a number set by hand (an empty value: counted again).
    Q_INVOKABLE void setNumber(int id, const QVariant& value);
    Q_INVOKABLE void closeNumberEdit();

signals:
    // Image px: what the canvas redraws. An empty rectangle asks for all.
    void changed(const QRectF& dirty);
    void toolChanged();
    void historyChanged();
    void itemsChanged();
    void panelChanged();
    void selectionChanged();
    void editingChanged();
    void numberEditChanged();
    void gestureChanged();
    void editorChanged();
    void cropChanged();
    void marqueeChanged();
    void pickingChanged();
    void smartChanged();
    // A word for the user (the overlay shows it by the selection).
    void notice(const QString& text);

private:
    enum class Gesture { None, Draw, Move, Reshape, Crop, Marquee };
    // What a style change goes to (3.5): the text being typed, else the
    // selected object, else the tool.
    struct Target {
        annot::Kind kind = annot::Kind::Rect; // the first object's, when several
        QList<quint32> ids; // objects in the document
        bool typing = false; // a new text, not in the document yet
        bool mixed = false; // several objects of different kinds
        std::optional<quint32> id() const { return ids.isEmpty() ? std::nullopt : std::optional<quint32>(ids.first()); }
    };
    struct Editing {
        quint32 id = 0; // 0: a new text, in the document only once committed
        QPoint at; // the text's top left
        QString text; // the words as typed so far
        QPoint click {-1, -1};
        annot::Style style; // a new text's (an old one keeps its own in the document)
        bool words = false; // the words of the Number `id` (in the document), not a Text
    };
    // The gesture's command and where it began, the wheel's sum, the clock
    // the undo merges go by, what the change signals were last sent for.
    struct Private;

    qint64 now() const;
    // Screen pixels as image pixels at the zoom (the monitor's scale in the overlay).
    qreal factor() const;
    // Where objects may go: the whole picture in the editor, the selection in the overlay.
    QRect room() const;
    std::optional<quint32> single() const; // the selected object, when it is one
    qreal tolerance() const;
    int reach() const;
    int dragStart() const;
    QPoint clampToSelection(QPoint p) const;
    std::optional<annot::Kind> toolKind() const;
    const annot::Item* find(std::optional<quint32> id) const;
    std::optional<quint32> itemAt(QPoint p) const;
    bool onSelected(QPoint p) const;
    int handleAt(QPoint p) const;
    int cropHandleAt(QPoint p) const; // a Handle (Geometry.h) of the crop under `p`, or 0 (None)
    QRgb colorAt(QPoint p) const;
    std::optional<Target> styleTarget() const;
    annot::Style targetStyle(const Target& target) const;
    annot::Style editingStyle() const;
    QRect editingBox() const;
    annot::Document committed() const;
    std::unique_ptr<annot::Command> textCommand(const annot::Document& doc, const QString& text) const;
    void startDraw(annot::Kind kind, QPoint p, Qt::KeyboardModifiers modifiers);
    void startMove(const QList<quint32>& ids, QPoint p, Qt::KeyboardModifiers modifiers);
    void startCrop(int handle, QPoint p);
    void startMarquee(QPoint p, Qt::KeyboardModifiers modifiers);
    void startReshape(int handle, QPoint p, Qt::KeyboardModifiers modifiers);
    void updateGesture();
    void finishGesture();
    annot::Item shapeTo(QPoint p, bool constrain) const;
    annot::Item strokeTo(const annot::Item& stroke, QPoint p, bool constrain) const;
    void beginNewText(QPoint p);
    void beginEdit(const annot::Item& item, QPoint p);
    // The words of a Number: after it was put down (click -1,-1: the caret at the end) or on a click.
    void beginWordsEdit(const annot::Item& item, QPoint click);
    // `p` is on the Number's words (not its disc).
    bool onWords(const annot::Item& item, QPoint p) const;
    annot::NumberGeometry geometryOf(const annot::Item& item) const;
    void commitWords(const QString& text);
    void stepSize(int direction);
    void nudge(QPoint delta);
    void reorder(int direction, bool toEnd);
    bool canReorder(int direction) const;
    void push(std::unique_ptr<annot::Command> command);
    void touched(const annot::Change& change);
    void keepValid();
    void smartSchedule();
    void smartInvalidate();
    void smartStart(bool apply);
    void smartScanned(int ticket, RedactScan scan);
    const QList<RedactHit>& smartHits() const;
    void reset();
    void sync();

    annot::Document m_document;
    annot::Document m_loaded; // the editor's: as opened
    QByteArray m_basePng; // the editor's: the base as stored (annot::Project::basePng)
    bool m_editor = false;
    qreal m_devicePerLogical = 0; // 0: the document's scale
    qreal m_zoom = 1;
    bool m_picking = false;
    QImage m_base;
    annot::StyleMemory m_styles;
    bool m_stylesChanged = false;
    annot::CommandStack m_stack;
    std::unique_ptr<annot::Compositor> m_compositor;
    int m_tool = -1;
    Gesture m_gesture = Gesture::None;
    QList<quint32> m_picked; // the selected objects, in the order they were picked
    std::optional<Editing> m_editing;
    std::optional<quint32> m_numberEdit;
    std::unique_ptr<Private> d;
};

} // namespace ws::capture
