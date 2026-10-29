#pragma once

#include "ClipModel.h"
#include "ClipStore.h"
#include "platform/ClipboardWatcher.h"
#include "platform/Paster.h"
#include "platform/TextRecognizer.h"

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

#include <windows.h>

#include <optional>
#include <memory>
#include <thread>
#include <vector>

namespace ws {

class ComWorker;

// The view-model of the clipboard window (Win+V, ClipboardWindow.qml): the
// history listed by category or group and searched, a preview of an entry,
// and pasting into the program that was in front before the window.
//
// A paste waits until Shift, Ctrl, Alt and the Windows keys are let go (so
// they do not join the paste keys), puts the entry on the clipboard with all
// the formats it was copied with (or only its text), brings that program
// back and presses its paste keys there.
//
// Over another program the window does not take the focus (keysRouted, see
// KeyRouter): that program stays in front with its text field, and a paste
// only waits for the modifier keys, puts the window away and presses the
// paste keys.
//
// Opened from a text field of Win顺's own (the launcher's search box, the one
// by file dialogs, one in the settings window), the window drops down under
// that field (App) and goes back to it instead, Esc included: the field gets
// its caret and selection back, and a paste types the entry's text in there,
// over the selection.
class Clipboard : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    // The clipboard window is shown (App).
    Q_PROPERTY(bool active READ active NOTIFY activeChanged FINAL)
    // Shown over another program that keeps the focus: the keys come from
    // KeyRouter (App), the search field shows its caret without the focus.
    Q_PROPERTY(bool keysRouted READ keysRouted NOTIFY keysRoutedChanged FINAL)
    // Pinned (the button in its header, issue #8): it stays up, on top of the
    // others, through pastes, clicks elsewhere and other programs coming to
    // the front, and takes no focus: a click on an entry pastes into the
    // program in front, again and again. The keys stay that program's until
    // the hotkey is pressed again or the search field clicked (keysWanted),
    // up to the next paste or Esc. Not while it pastes into a field of Win顺's own.
    Q_PROPERTY(bool pinned READ pinned WRITE setPinned NOTIFY pinnedChanged FINAL)
    // The preview beside the list (Alt+P, →): as it was left, next time too.
    Q_PROPERTY(bool previewOpen READ previewOpen WRITE setPreviewOpen NOTIFY previewOpenChanged FINAL)
    // Its width as the user dragged it (logical pixels); 0: the default.
    Q_PROPERTY(int previewWidth READ previewWidth WRITE setPreviewWidth NOTIFY previewWidthChanged FINAL)
    Q_PROPERTY(int listWidth READ listWidth WRITE setListWidth NOTIFY listWidthChanged FINAL)
    // The entries in "固定" listed before the rest, in their own order (the
    // switch at the end of the tabs): what is kept at hand stays at the top,
    // on the same Alt+number. Off: newest first, as Windows lists them.
    Q_PROPERTY(bool pinnedFirst READ pinnedFirst WRITE setPinnedFirst NOTIFY pinnedFirstChanged FINAL)
    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged FINAL) // the history is on (settings)
    Q_PROPERTY(bool paused READ paused NOTIFY stateChanged FINAL) // ... but paused from the tray menu
    Q_PROPERTY(QString shortcut READ shortcut NOTIFY stateChanged FINAL) // what opens it: "Win+V"
    // The modifier that pastes the nth row with a digit: "alt", "ctrl", or ""
    // for none (Settings::clipboardNumberKeys).
    Q_PROPERTY(QString numberKeys READ numberKeys NOTIFY numberKeysChanged FINAL)
    Q_PROPERTY(QString numberHints READ numberHints NOTIFY numberKeysChanged FINAL)
    // Bits 1 << n of the digits n left to another program's global hotkey
    // (numberkeys::heldMask): their rows show the chord struck through.
    Q_PROPERTY(int numberKeysLeft READ numberKeysLeft NOTIFY numberKeysChanged FINAL)
    // A click pastes the row, as Windows' own history does (forum #137: the
    // PC driven from a phone, where a double click is hard to hit); off, it
    // picks the row and a double click pastes (Settings::clipboardClickToPaste).
    Q_PROPERTY(bool clickToPaste READ clickToPaste NOTIFY rowOptionsChanged FINAL)
    // Pictures on taller rows, their thumbnails large enough to make out
    // (Settings::clipboardLargePictures).
    Q_PROPERTY(bool largePictures READ largePictures NOTIFY rowOptionsChanged FINAL)
    // Opened over the caret or a search box (Placement.above), the window
    // upside down: the newest entry by it, older ones upwards
    // (Settings::clipboardFlipAbove).
    Q_PROPERTY(bool flipAbove READ flipAbove NOTIFY layoutOptionsChanged FINAL)
    // The search box at the end away from the caret, the entries by it
    // (Settings::clipboardSearchBoxSide "far"); else the search box by it.
    Q_PROPERTY(bool searchFar READ searchFar NOTIFY layoutOptionsChanged FINAL)
    // Its hotkey held after it opened the window (GitHub #107, App,
    // Settings::clipboardHoldToStep): the key pressed again moves to the next
    // older row (stepped), letting go of the hotkey pastes that row as Enter
    // would. holdHint: letting go pastes now (a step made, or held a second),
    // and the footer says so; holdKeys: what to let go of ("Ctrl").
    Q_PROPERTY(bool holdToStep READ holdToStep NOTIFY holdChanged FINAL)
    Q_PROPERTY(bool holdHint READ holdHint NOTIFY holdChanged FINAL)
    Q_PROPERTY(QString holdKeys READ holdKeys NOTIFY holdChanged FINAL)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged FINAL)
    Q_PROPERTY(int category READ category WRITE setCategory NOTIFY categoryChanged FINAL)
    // [{title, group (id, 0 for the kinds), size (entries in a group)}]
    Q_PROPERTY(QVariantList categories READ categories NOTIFY categoriesChanged FINAL)
    Q_PROPERTY(ws::ClipModel* items READ items CONSTANT FINAL)
    Q_PROPERTY(int total READ total NOTIFY listChanged FINAL) // entries in the history
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString separatorName READ separatorName NOTIFY separatorChanged FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoChanged FINAL)
    // Where Enter pastes, when it is a field of Win顺's own ("搜索框"); empty
    // for the program that was in front.
    Q_PROPERTY(QString fieldName READ fieldName NOTIFY fieldChanged FINAL)
    // The field itself (Main.qml: the launcher folds its rows away while the
    // window is under its search box).
    Q_PROPERTY(QQuickItem* field READ field NOTIFY fieldChanged FINAL)

public:
    enum Action {
        Paste,
        PastePlain,
        Copy,
        Pin, // in or out of "固定"
        NewGroup, // asks for the name (groupNameRequested), then moves the row there
        LeaveGroup,
        OpenLink,
        Reveal,
        OpenImage,
        PinImage, // a picture on the screen (贴到屏幕): pinRequested
        EditImage, // the editing window (编辑): editRequested
        Remove,
        MoveUp, // in its group's own order (ClipModel::movable)
        MoveDown,
        RenameGroup, // on a category tab
        RemoveGroup,
        RemoveAll, // everything listed (not what is in a group, unless a group's tab)
        MoveToGroup = 1000, // + the group's id
    };
    Q_ENUM(Action)

    Clipboard(ClipStore* store, ClipboardWatcher* watcher, QObject* parent = nullptr);
    ~Clipboard() override;

    bool active() const { return m_active; }
    void setActive(bool active);
    bool keysRouted() const { return m_keysRouted; }
    void setKeysRouted(bool routed);
    bool recording() const { return m_recording; }
    bool paused() const { return m_paused; }
    void setState(bool recording, bool paused, const QString& shortcut);
    QString shortcut() const { return m_shortcut; }
    QString numberKeys() const { return m_numberKeys; }
    QString numberHints() const { return m_numberHints; }
    void setNumberHints(const QString& hints);
    void setNumberKeys(const QString& keys); // "alt", "ctrl" or "off"
    int numberKeysLeft() const { return static_cast<int>(m_numberKeysLeft); }
    void setNumberKeysLeft(unsigned digits);
    bool clickToPaste() const { return m_clickToPaste; }
    bool largePictures() const { return m_largePictures; }
    void setRowOptions(bool clickToPaste, bool largePictures);
    bool flipAbove() const { return m_flipAbove; }
    bool searchFar() const { return m_searchFar; }
    void setLayoutOptions(bool flipAbove, bool searchFar);
    bool holdToStep() const { return m_holdToStep; }
    void setHoldToStep(bool on);
    bool holdHint() const { return m_holdHint; }
    QString holdKeys() const { return m_holdKeys; }
    // The hold (App), on an entry rather than a row: a copy coming in moves
    // it down, it stays the one. Begun as the window opens, on the first
    // entry once it shows (handleShown); a step; held a second without one
    // (that entry, the hint); let go (pasted as Enter, its keys past `guard`,
    // the hold's hook; false: the entry is gone, nothing pasted); over
    // without a paste (Esc, a click, another key).
    void beginSteps(const QString& keys);
    void step();
    void holdFirst();
    bool finishSteps(std::unique_ptr<paste::Guard> guard = nullptr);
    void endSteps();
    // The row of the entry the hold is on; -1 without one (the list empty as it opened).
    Q_INVOKABLE int heldRow() const;
    QString query() const { return m_query; }
    void setQuery(const QString& query);
    int category() const { return m_category; }
    void setCategory(int category);
    QVariantList categories() const;
    ClipModel* items() { return &m_model; }
    int total() const { return static_cast<int>(m_store->clips().size()) - m_store->hiddenCount(); }
    QString statusText() const { return m_flash.isEmpty() ? m_status : m_flash; }
    QString separatorName() const;
    bool canUndo() const { return !m_removed.empty() || m_removedGroup.has_value(); }

    bool pinned() const { return m_pinned; }
    void setPinned(bool pinned);
    bool previewOpen() const { return m_previewOpen; }
    void setPreviewOpen(bool open);
    int previewWidth() const { return m_previewWidth; }
    void setPreviewWidth(int width);
    int listWidth() const { return m_listWidth; }
    void setListWidth(int width);
    bool pinnedFirst() const { return m_pinnedFirst; }
    void setPinnedFirst(bool on);

    void setWindow(QWindow* window) { m_window = window; }
    // The window to paste into: the one in front before the clipboard opened
    // (pinned: before it took the keys; else the one in front as it pastes).
    void setTarget(HWND window) { m_target = window; }
    HWND target() const { return m_target; }
    // The field of Win顺's own that had the keyboard as the window opened, if
    // any (see the class comment). Taken before the window takes the focus:
    // its selection is gone then.
    void setField(QQuickItem* field, const QString& name);
    QString fieldName() const { return m_field.item ? m_field.name : QString(); }
    QQuickItem* field() const { return m_field.item; }
    void handleShown();
    void handlePlaced() { emit placed(); } // after handleShown, put where it opens: the preview may widen it now
    void handleHidden();
    void historyChanged(); // the store changed outside this class (copied, limits, cleared)
    void resumeHistory(); // the store was reopened: restore preferences and schedule unread pictures
    void suspendHistory(); // stop OCR work and discard transient rows before closing the store
    void retranslate();

    // On a row; on all of them, when it is one of several picked.
    Q_INVOKABLE void paste(int row, bool plainText);
    Q_INVOKABLE void quickPaste(int number, bool plainText); // Alt+1 ... Alt+9: the nth row
    Q_INVOKABLE void copy(int row);
    // An image exported as a PNG file; its history entry remains unchanged.
    Q_INVOKABLE void dragOut(int row);
    bool dragging() const { return m_dragging; } // do not hide the window during OLE's modal loop
    // A colour's other notation, as a new entry unless it would not read back
    // as that colour (#AARRGGBB).
    Q_INVOKABLE void copyText(const QString& text, bool remember);
    // A picture (a screen capture, a pin) onto the clipboard, the way copyText
    // puts text there: with `remember`, as a new history entry on top, whose
    // words are then read like any copied picture's; `screenshot`: one Win顺's
    // capture took (Clip::screenshot, the screenshot history). The PNG is made
    // off the GUI thread. `project`: the annotated picture's editable form
    // (opaque bytes, empty: none), kept with the history entry.
    void copyImage(const QImage& image, bool remember, bool screenshot = false, QByteArray project = {});
    // A picture (saved, pinned) as a screenshot entry in the history only:
    // the system clipboard is left alone and nothing is announced. Subject to
    // the same switch and pause as copyImage's `remember`; the same pixels
    // again move that entry to the top.
    void recordImage(const QImage& image, QByteArray project);
    // An entry by its id, from outside the window (the launcher's screenshot
    // history, jt): onto the clipboard and pasted into `target`, the window
    // in front before the launcher, as the window pastes; only copied when
    // that is not a window to paste into. False if there is no such entry.
    bool pasteEntry(qint64 id, HWND target);
    // A global digit: keep the current program in front throughout. Never
    // activates it again if the user changes windows while letting go.
    void pasteDirect(qint64 id, HWND target);
    // Entries by their ids out of the history, as remove() takes rows: undo()
    // (Ctrl+Z, here or in the launcher) puts them back.
    void removeEntries(const std::vector<qint64>& ids);
    Q_INVOKABLE void togglePin(int row);
    Q_INVOKABLE void remove(int row);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void moveToGroup(int row, qint64 group);
    // A row of a group, in the group's own order, to where row `to` is (a
    // drag, Ctrl+↑/↓); both must be movable and of the same group.
    Q_INVOKABLE bool canMove(int from, int to) const;
    Q_INVOKABLE void moveRow(int from, int to);
    Q_INVOKABLE qint64 addGroup(const QString& name); // 0 if empty or not saved
    Q_INVOKABLE void renameGroup(qint64 group, const QString& name);

    Q_INVOKABLE void toggleSelected(int row);
    Q_INVOKABLE void selectRange(int from, int to, bool add);
    Q_INVOKABLE void clearSelection();
    // Ctrl+A and 全部删除: the listed entries (this tab, this search). What is
    // in a group is left out, unless the tab is a group's. selectAll() returns
    // how many it picked; removeListed() can be undone as any delete.
    Q_INVOKABLE int selectAll();
    void removeListed();
    Q_INVOKABLE void cycleCategory(int delta);
    Q_INVOKABLE void cycleSeparator();
    Q_INVOKABLE void dismiss(); // Esc, Win+V again: back to the field it came from, or closed (pinned: the keys given back)
    Q_INVOKABLE void wantKeys() { emit keysWanted(); } // the search field clicked: acquire or refresh routing
    Q_INVOKABLE void turnOn(); // the history, from the window's button

    // The preview of a row: {kind, text, more, files, thumbnail, image,
    // width, height, swatch, colors, source, sourceIcon, time, size,
    // formatted, group}.
    Q_INVOKABLE QVariantMap preview(int row) const;

    // Context menus (ContextMenu.qml): of a row, of a category tab.
    Q_INVOKABLE QVariantList menuItems(int row) const;
    Q_INVOKABLE QVariantList categoryMenuItems(int category) const;
    Q_INVOKABLE void trigger(int row, int action);
    Q_INVOKABLE void triggerCategory(int category, int action);
    Q_INVOKABLE QRectF screenArea(QPointF globalPos) const;
    Q_INVOKABLE void prepareMenuWindow(QWindow* menu) const;

signals:
    void quickPasteFailed(const QString& message); // no panel to show a failure in: App's notification
    void numberKeysChanged();
    void rowOptionsChanged();
    void layoutOptionsChanged();
    void holdChanged();
    void stepped(int row); // the hold moved on to this row: the current one now
    void activeChanged();
    void keysRoutedChanged();
    void pinnedChanged();
    void previewOpenChanged();
    void previewWidthChanged();
    void listWidthChanged();
    void pinnedFirstChanged();
    void keysWanted(); // a search-field click: acquire or refresh routing (App)
    void keysReleased(); // pinned, Esc (or the hotkey again): the keys and the focus back to the program (App)
    // The window takes the focus after all (App), from the program that kept
    // it: before handing over to another program (a link opened), which may
    // come to the front only from the one in front.
    void focusNeeded();
    void stateChanged();
    void queryChanged();
    void categoryChanged();
    void categoriesChanged();
    void listChanged(); // rows replaced: select the first
    void statusChanged();
    void separatorChanged();
    void undoChanged();
    void shown();
    void placed();
    void dismissRequested();
    void turnOnRequested();
    void contextMenuKeyPressed(); // Menu key / Shift+F10, while the window is shown
    // A Shift alone under a Chinese layout, the keys routed here (App): meant
    // to switch to Chinese, which takes the focus; pinyin finds Chinese anyway.
    void pinyinHintRequested();
    void groupNameRequested(int row, qint64 group); // a name for a new group (group 0) for the row, or a new one for `group`
    void fieldChanged();
    // The field the window goes back to: its own window in front again (App).
    void fieldRequested(QQuickItem* field);
    // 贴到屏幕 on a picture's row: its file, for the app to pin (PinManager).
    void pinRequested(const QString& imagePath);
    // 编辑 on a picture: the entry's id, for the editing window (its project
    // if it has one, else the picture as the base of an empty document).
    void editRequested(qint64 id);

private:
    struct Category {
        ClipFilter::Category kind;
        qint64 group = 0;
    };
    // Where a paste goes: only onto the clipboard (a copy), into the program
    // that was in front, or into a field of Win顺's own.
    enum class Into { Clipboard, Program, Field, CurrentProgram };
    // A field of Win顺's own, as it was when the window opened.
    struct Field {
        QPointer<QQuickItem> item;
        QString name;
        int cursor = 0;
        int anchor = 0;
        bool oneLine = true; // a TextInput: line breaks go in as spaces
    };
    std::vector<Category> categoryList() const;
    void refresh();
    void refreshStatus();
    void flash(const QString& message);
    void failPaste(const QString& message);
    std::vector<const Clip*> targets(int row) const; // the row, or the picked ones it is one of
    ClipWrite writeFor(
        const std::vector<const Clip*>& clips, bool plainText, QString* problem, HWND target = nullptr) const;
    void startPaste(ClipWrite write, Into into, std::vector<qint64> touched, QString done);
    void encodeImage(const QImage& image, bool remember, bool screenshot, QByteArray project, bool toClipboard);
    // copyImage's (and recordImage's), encoded
    void storeImage(const QByteArray& png, const QByteArray& hash, QSize size, bool remember, bool screenshot,
        const QByteArray& project, bool toClipboard);
    void onWritten(bool ok);
    void returnToField(QString text); // `text` typed into it; none: Esc
    void fillField(); // it has the keyboard again (or waited long enough)
    void dropUndo();
    QString separator() const;
    void loadHistoryState();

    ClipStore* m_store;
    ClipboardWatcher* m_watcher;
    ClipModel m_model;
    TextRecognizer m_recognizer; // the words in pictures, for the search
    QTimer m_backlogTimer;
    QSet<qint64> m_rereading; // read by another recognizer before: once all are back, it is noted
    QPointer<QWindow> m_window;
    // Pictures dragged out, copied here first (a folder each, by id): kept
    // until the app ends, as the program dropped on may copy them later.
    // Made on the first drag; what a crash left is removed at the start.
    QString m_dragFolder;
    bool m_dragging = false;
    bool m_active = false;
    bool m_keysRouted = false;
    bool m_pinned = false;
    bool m_previewOpen = false;
    int m_previewWidth = 0;
    int m_listWidth = 480;
    bool m_pinnedFirst = false;
    bool m_recording = true;
    bool m_paused = false;
    QString m_shortcut;
    QString m_numberKeys = QStringLiteral("alt");
    QString m_numberHints = QStringLiteral("held");
    unsigned m_numberKeysLeft = 0;
    bool m_clickToPaste = false;
    bool m_largePictures = false;
    bool m_flipAbove = true;
    bool m_searchFar = false;
    bool m_holdToStep = false;
    bool m_holding = false; // beginSteps .. endSteps
    bool m_holdHint = false;
    QString m_holdKeys;
    qint64 m_heldId = 0; // the entry the hold is on; 0: none (the list empty as it opened)
    QString m_query;
    int m_category = 0;
    qint64 m_categoryGroup = 0; // the group the category tab is, kept when groups come and go
    QString m_status;
    QString m_flash;
    QTimer m_flashTimer;
    QString m_separator; // newline | space | comma | tab | none
    // For undo: the entries removed last, or the group.
    std::vector<ClipRecord> m_removed;
    std::optional<ClipGroupRecord> m_removedGroup;

    // A paste in progress (see the class comment).
    HWND m_target = nullptr;
    Field m_field; // the window's, while it shows
    bool m_pasting = false;
    ClipWrite m_write;
    Into m_into = Into::Clipboard;
    QString m_fieldText; // typed into the field once the clipboard has it
    std::vector<qint64> m_touched; // moved up once pasted (joined pastes; a single one moves up by itself)
    QString m_doneMessage; // a copy without pasting says so
    HWND m_pasteWindow = nullptr;
    std::unique_ptr<paste::Guard> m_nextGuard; // finishSteps', for startPaste
    std::unique_ptr<paste::Guard> m_pasteGuard; // the hook its keys go past (a hold's paste)
    QElapsedTimer m_pasteClock;
    QElapsedTimer m_frontClock; // since the target came to the front
    QTimer m_keysTimer; // waits for the modifier keys to be let go
    QTimer m_focusTimer; // waits for the target to be in front
    // Going back to a field: it, and the text that goes in.
    Field m_returning;
    QString m_returnText;
    QElapsedTimer m_returnClock;
    QTimer m_returnTimer; // waits for the field to have the keyboard
    std::unique_ptr<ComWorker> m_filePasteWorker;
    std::jthread m_imageEncoder; // copyImage's PNG; last, so joined first
};

} // namespace ws
