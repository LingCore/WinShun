#pragma once

#include "AnnotationModel.h"
#include "AnnotationStyles.h"
#include "PresenterState.h"

#include <QCursor>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QSize>
#include <QString>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>
#include <vector>

namespace ws::capture {

class Presenter;

// One monitor of the presenter (GitHub #189, docs/design-presenter.md 4.2):
// its ink on the live desktop (`desk`) and, made the first time W or K is
// pressed there, the ink on its board (`boardModel`, the white board and the
// black one share it). Both are annotation models on a picture the size of
// the monitor in its physical pixels, at its scale, as the capture overlay's;
// the compositor draws them on nothing (Compositor::setTransparent). The ink
// window (PresenterOverlay.qml) shows them and hands its mouse and keys here.
class PresenterScreen : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Made by the presenter")

    Q_PROPERTY(int index READ index CONSTANT FINAL)
    Q_PROPERTY(qreal dpr READ dpr CONSTANT FINAL)
    Q_PROPERTY(ws::capture::AnnotationModel* desk READ desk CONSTANT FINAL)
    Q_PROPERTY(ws::capture::AnnotationModel* boardModel READ boardModel NOTIFY boardChanged FINAL)
    // The model the mouse and keys reach now: the board's while one is open.
    Q_PROPERTY(ws::capture::AnnotationModel* model READ model NOTIFY boardChanged FINAL)
    // presenter::Board: 0 none, 1 white, 2 black.
    Q_PROPERTY(int board READ boardInt NOTIFY boardChanged FINAL)
    // The item whose cursor the presenter sets (the ink window's root).
    Q_PROPERTY(QQuickItem* cursorItem READ cursorItem WRITE setCursorItem NOTIFY cursorItemChanged FINAL)

public:
    PresenterScreen(Presenter* presenter, int index, QSize pixels, qreal dpr);
    ~PresenterScreen() override;

    int index() const { return m_index; }
    qreal dpr() const { return m_dpr; }
    QSize pixels() const { return m_pixels; }
    AnnotationModel* desk() { return &m_desk; }
    AnnotationModel* boardModel() { return m_board.get(); }
    AnnotationModel* model() { return m_boardKind != presenter::Board::None && m_board ? m_board.get() : &m_desk; }
    presenter::Board board() const { return m_boardKind; }
    int boardInt() const { return int(m_boardKind); }
    QQuickItem* cursorItem() const { return m_cursorItem; }
    void setCursorItem(QQuickItem* item);

    // ---- input, window logical coordinates ----
    Q_INVOKABLE void mousePressed(qreal x, qreal y, int button, int modifiers);
    Q_INVOKABLE void mouseMoved(qreal x, qreal y, int modifiers);
    Q_INVOKABLE void mouseReleased(qreal x, qreal y, int button);
    Q_INVOKABLE void mouseDoubleClicked(qreal x, qreal y);
    Q_INVOKABLE void wheeled(int angleDelta);
    Q_INVOKABLE bool keyPressed(int key, int modifiers);

signals:
    void boardChanged();
    void cursorItemChanged();

private:
    friend class Presenter;

    QPoint imageAt(qreal x, qreal y) const;
    // The models' picture: `pixels` of zeros that takes no memory until written (never).
    void begin(const QImage& base, const annot::StyleMemory& styles, int tool);
    void end();
    void setBoard(presenter::Board board, const annot::StyleMemory& styles, int tool);
    void showCursor(QPoint image);

    Presenter* m_presenter;
    const int m_index;
    const QSize m_pixels;
    const qreal m_dpr;
    QImage m_base;
    AnnotationModel m_desk;
    std::unique_ptr<AnnotationModel> m_board;
    presenter::Board m_boardKind = presenter::Board::None;
    QPointer<QQuickItem> m_cursorItem;
    bool m_pressed = false;
    QString m_cursorKey; // what the cursor shows now, so it is made again only when that changes
};

// The presenter (GitHub #189, docs/design-presenter.md): drawing on the live
// desktop with the annotation tools, a laser pointer, white and black
// boards, and the ink left on the screen while the mouse and keys go to the
// programs beneath ("through"). This holds its state and its rules; the
// windows (a transparent ink window per monitor, the toolbar) and what is
// native about them are the owner's (PresenterHost, in the plugin), which
// follows modeChanged and is asked the rest through Hooks, so the tests
// drive this with no window at all.
class Presenter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Made by the app")

    // presenter::Mode: 0 closed, 1 drawing, 2 through.
    Q_PROPERTY(int mode READ modeInt NOTIFY modeChanged FINAL)
    Q_PROPERTY(bool through READ isThrough NOTIFY modeChanged FINAL)
    // Through is offered while no board is open (one cannot see what a click would reach).
    Q_PROPERTY(bool canThrough READ canThrough NOTIFY boardsChanged FINAL)
    Q_PROPERTY(int tool READ tool WRITE setTool NOTIFY toolChanged FINAL)
    Q_PROPERTY(bool laser READ laser WRITE setLaser NOTIFY toolChanged FINAL)
    // The screen last drawn on (the toolbar's undo, clear and boards act there) and its model.
    Q_PROPERTY(ws::capture::PresenterScreen* current READ current NOTIFY currentChanged FINAL)
    Q_PROPERTY(ws::capture::AnnotationModel* model READ model NOTIFY currentChanged FINAL)
    Q_PROPERTY(int board READ board NOTIFY boardsChanged FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged FINAL)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged FINAL)
    Q_PROPERTY(bool canClear READ canClear NOTIFY historyChanged FINAL)
    Q_PROPERTY(bool toolbarShown READ toolbarShown NOTIFY toolbarShownChanged FINAL)
    // A line once at the toolbar's edge (the recording hint, the through hint); empty: none.
    Q_PROPERTY(QString hint READ hint NOTIFY hintChanged FINAL)
    // The pen's colour and the laser's, for the toolbar's colour dot.
    Q_PROPERTY(QColor laserColor READ laserColor CONSTANT FINAL)

public:
    struct Monitor {
        QSize pixels; // physical
        qreal dpr = 1.0;
    };

    struct Hooks {
        // The monitors, in QGuiApplication::screens()' order.
        std::function<QList<Monitor>()> monitors;
        // The monitor the pointer is on (an index of monitors()); -1: none known.
        std::function<int()> screenAtPointer;
        // The foreground window (0: one of ours or none), and giving it back.
        std::function<quintptr()> foreground;
        std::function<void(quintptr)> setForeground;
        // Small remembered things ([Presenter] in presenter.ini): the tools'
        // styles (JSON), the tool, which hints were seen.
        std::function<QVariant(const QString& key)> load;
        std::function<void(const QString& key, const QVariant& value)> save;
    };

    // The keys of what is remembered.
    static constexpr const char* kStylesKey = "Styles";
    static constexpr const char* kToolKey = "Tool";
    static constexpr const char* kRecordHintKey = "RecordHintSeen";
    static constexpr const char* kThroughHintKey = "ThroughHintSeen";
    // The pen's width the first time, logical px: thicker than a screenshot's (seen from afar).
    static constexpr int kFirstPenWidth = 6;
    static constexpr int kFirstTool = int(annot::Kind::Pen);

    explicit Presenter(QObject* parent = nullptr);
    ~Presenter() override;

    void setHooks(Hooks hooks);
    // The presenter's hotkey's text, for the through hint ("" when it has none).
    void setHotkeyText(const QString& text) { m_hotkeyText = text; }

    // ---- the ways in and out ----
    // The hotkey: open, close, or from Through back to drawing (hotkeyStep).
    void hotkey();
    Q_INVOKABLE void open();
    Q_INVOKABLE void close();
    // Drawing again (the ink window takes the mouse and keys), or Through.
    Q_INVOKABLE void draw();
    Q_INVOKABLE void through();
    Q_INVOKABLE void toggleThrough();
    // Esc and the right button: one step back (presenter::backStep).
    Q_INVOKABLE void back();
    // The monitors changed: the ink goes (the windows are made again).
    void resetScreens();

    // ---- the toolbar ----
    int tool() const { return m_tool; }
    void setTool(int tool);
    bool laser() const { return m_laser; }
    void setLaser(bool on);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void clear();
    // presenter::Board, on the current screen; the same one again closes it.
    Q_INVOKABLE void toggleBoard(int board);
    Q_INVOKABLE void toggleToolbar();
    Q_INVOKABLE void dismissHint();

    // ---- what QML and the host read ----
    presenter::Mode mode() const { return m_mode; }
    int modeInt() const { return int(m_mode); }
    bool isOpen() const { return m_mode != presenter::Mode::Closed; }
    bool isThrough() const { return m_mode == presenter::Mode::Through; }
    bool canThrough() const;
    PresenterScreen* current() const;
    AnnotationModel* model() const;
    int board() const;
    bool canUndo() const;
    bool canRedo() const;
    bool canClear() const;
    bool toolbarShown() const { return m_toolbarShown; }
    QString hint() const { return m_hint; }
    QColor laserColor() const { return QColor(0xFF, 0x2D, 0x2D); }
    const std::vector<std::unique_ptr<PresenterScreen>>& screens() const { return m_screens; }
    PresenterScreen* screen(int index) const;
    // The window that had the foreground before drawing: Through gives it back.
    quintptr previousForeground() const { return m_previous; }
    const annot::StyleMemory& styles() const { return m_styles; }

    // The cursor for a screen at an image point: a dot of the pen's colour
    // and width (pen, marker, laser), else the model's shape.
    QCursor cursorFor(PresenterScreen* screen, QPoint image, QString* key) const;

signals:
    void modeChanged();
    void toolChanged();
    void currentChanged();
    void boardsChanged();
    void historyChanged();
    void toolbarShownChanged();
    void hintChanged();
    // The screens were made (open after none, or after the monitors changed): the host makes their windows.
    void screensChanged();

private:
    friend class PresenterScreen;

    void ensureScreens();
    void setMode(presenter::Mode mode);
    // The screen the mouse or keys came from becomes the current one: it
    // takes the remembered styles and the tool, the others let go of their selection.
    void activate(PresenterScreen* screen);
    // The styles a model changed (its panel, [ and ], the wheel) become everyone's.
    void collectStyles(AnnotationModel* model);
    void adoptStyles(AnnotationModel* model);
    void applyTool(AnnotationModel* model);
    void deselect(AnnotationModel* model);
    void watch(AnnotationModel* model);
    void touched(AnnotationModel* model);
    void showHint(const char* key, const QString& text);
    void saveState();
    bool commandKey(PresenterScreen* screen, int key, Qt::KeyboardModifiers modifiers);

    Hooks m_hooks;
    presenter::Mode m_mode = presenter::Mode::Closed;
    std::vector<std::unique_ptr<PresenterScreen>> m_screens;
    int m_current = 0;
    annot::StyleMemory m_styles;
    bool m_stylesLoaded = false;
    int m_tool = kFirstTool;
    bool m_laser = false;
    bool m_toolbarShown = true;
    quintptr m_previous = 0;
    QString m_hint;
    QString m_hotkeyText;
    bool m_syncing = false; // the presenter's own calls on a model: not the user's changes
    // The models in the order they were last changed (undo goes back along it) and undone (redo).
    std::vector<QPointer<AnnotationModel>> m_changed;
    std::vector<QPointer<AnnotationModel>> m_undone;
    QElapsedTimer m_clock;
};

} // namespace ws::capture
