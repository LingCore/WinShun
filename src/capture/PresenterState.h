#pragma once

#include <Qt>

namespace ws::capture::presenter {

// The presenter's rules (GitHub #189, docs/design-presenter.md 2.2-2.6) with
// no window in them, so tst_presenter checks them on their own: what the
// hotkey does, the step back of Esc and the right button, which keys are the
// presenter's own. Presenter asks these; nothing else decides them.

enum class Mode {
    Closed,
    Draw, // the ink layer takes the mouse and the keys
    Through, // the ink stays on the screen; the mouse and keys reach the programs beneath
};

enum class Board {
    None, // the live desktop
    White,
    Black,
};

// The presenter's hotkey: what opens it closes it, and from Through it
// opens the drawing again (the drawing is what was closed there).
enum class HotkeyStep { Open, Close, Draw };
constexpr HotkeyStep hotkeyStep(Mode mode)
{
    switch (mode) {
    case Mode::Closed: return HotkeyStep::Open;
    case Mode::Draw: return HotkeyStep::Close;
    case Mode::Through: return HotkeyStep::Draw;
    }
    return HotkeyStep::Open;
}

// Esc and the right button, after the model has had its say (a path or a
// text finished, a gesture cancelled: `modelTook`): one step back, never
// the ink, never the end. In order: deselect, close the board, let go.
enum class BackStep { Done, Deselect, CloseBoard, Through };
constexpr BackStep backStep(bool modelTook, bool selected, Board board)
{
    if (modelTook)
        return BackStep::Done;
    if (selected)
        return BackStep::Deselect;
    if (board != Board::None)
        return BackStep::CloseBoard;
    return BackStep::Through;
}

// The presenter's own keys in the ink layer (fixed until #187's key map
// takes them: CaptureKeys with a Presenter context). The annotation tools'
// letters and digits, Ctrl+Z and the rest stay the model's.
inline constexpr int kLaserKey = Qt::Key_X;
inline constexpr int kWhiteboardKey = Qt::Key_W;
inline constexpr int kBlackboardKey = Qt::Key_K;
inline constexpr int kToolbarKey = Qt::Key_H;

enum class Command {
    None, // the model's, if anyone's
    Laser,
    Whiteboard,
    Blackboard,
    Toolbar,
    Clear, // Delete with nothing selected
    Blocked, // a capture key that means nothing on the live desktop: swallowed
};

constexpr Command command(int key, Qt::KeyboardModifiers modifiers, bool selected)
{
    const Qt::KeyboardModifiers mods = modifiers & ~Qt::KeypadModifier;
    const bool ctrl = mods & Qt::ControlModifier;
    const bool other = mods & (Qt::AltModifier | Qt::MetaModifier);
    const bool shift = mods & Qt::ShiftModifier;
    if (ctrl || other)
        return Command::None;
    // Redaction (B, 9, Shift+B smart redaction) samples the picture beneath,
    // and the eyedropper (I) reads it: there is none.
    if (key == Qt::Key_B || key == Qt::Key_9 || key == Qt::Key_I)
        return Command::Blocked;
    if (shift)
        return Command::None;
    switch (key) {
    case kLaserKey: return Command::Laser;
    case kWhiteboardKey: return Command::Whiteboard;
    case kBlackboardKey: return Command::Blackboard;
    case kToolbarKey: return Command::Toolbar;
    case Qt::Key_Delete: return selected ? Command::None : Command::Clear;
    default: return Command::None;
    }
}

// The tools offered: every annotation tool but redaction (annot::Kind::Redact,
// the last of AnnotationStyles' table).
inline constexpr int kRedactTool = 8;
constexpr bool toolOffered(int tool) { return tool >= -1 && tool < kRedactTool; }

// The board W or K leads to from `now`: the same key again closes it.
constexpr Board toggledBoard(Board now, Board wanted) { return now == wanted ? Board::None : wanted; }

} // namespace ws::capture::presenter
