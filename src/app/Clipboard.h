#pragma once

#include "ClipModel.h"
#include "ClipStore.h"
#include "platform/ClipboardWatcher.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

#include <windows.h>

#include <optional>
#include <vector>

namespace ws {

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
    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged FINAL) // the history is on (settings)
    Q_PROPERTY(bool paused READ paused NOTIFY stateChanged FINAL) // ... but paused from the tray menu
    Q_PROPERTY(QString shortcut READ shortcut NOTIFY stateChanged FINAL) // what opens it: "Win+V"
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
        Remove,
        RenameGroup, // on a category tab
        RemoveGroup,
        MoveToGroup = 1000, // + the group's id
    };
    Q_ENUM(Action)

    Clipboard(ClipStore* store, ClipboardWatcher* watcher, QObject* parent = nullptr);

    bool active() const { return m_active; }
    void setActive(bool active);
    bool keysRouted() const { return m_keysRouted; }
    void setKeysRouted(bool routed);
    bool recording() const { return m_recording; }
    bool paused() const { return m_paused; }
    void setState(bool recording, bool paused, const QString& shortcut);
    QString shortcut() const { return m_shortcut; }
    QString query() const { return m_query; }
    void setQuery(const QString& query);
    int category() const { return m_category; }
    void setCategory(int category);
    QVariantList categories() const;
    ClipModel* items() { return &m_model; }
    int total() const { return static_cast<int>(m_store->clips().size()); }
    QString statusText() const { return m_flash.isEmpty() ? m_status : m_flash; }
    QString separatorName() const;
    bool canUndo() const { return !m_removed.empty() || m_removedGroup.has_value(); }

    void setWindow(QWindow* window) { m_window = window; }
    // The window to paste into: the one in front before the clipboard opened.
    void setTarget(HWND window) { m_target = window; }
    // The field of Win顺's own that had the keyboard as the window opened, if
    // any (see the class comment). Taken before the window takes the focus:
    // its selection is gone then.
    void setField(QQuickItem* field, const QString& name);
    QString fieldName() const { return m_field.item ? m_field.name : QString(); }
    QQuickItem* field() const { return m_field.item; }
    void handleShown();
    void handleHidden();
    void historyChanged(); // the store changed outside this class (copied, limits, cleared)
    void retranslate();

    // On a row; on all of them, when it is one of several picked.
    Q_INVOKABLE void paste(int row, bool plainText);
    Q_INVOKABLE void quickPaste(int number, bool plainText); // Alt+1 ... Alt+9: the nth row
    Q_INVOKABLE void copy(int row);
    // A colour's other notation, as a new entry unless it would not read back
    // as that colour (#AARRGGBB).
    Q_INVOKABLE void copyText(const QString& text, bool remember);
    Q_INVOKABLE void togglePin(int row);
    Q_INVOKABLE void remove(int row);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void moveToGroup(int row, qint64 group);
    Q_INVOKABLE qint64 addGroup(const QString& name); // 0 if empty or not saved
    Q_INVOKABLE void renameGroup(qint64 group, const QString& name);

    Q_INVOKABLE void toggleSelected(int row);
    Q_INVOKABLE void selectRange(int from, int to, bool add);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void cycleCategory(int delta);
    Q_INVOKABLE void cycleSeparator();
    Q_INVOKABLE void dismiss(); // Esc, Win+V again: back to the field it came from, or closed
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
    void activeChanged();
    void keysRoutedChanged();
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
    void dismissRequested();
    void turnOnRequested();
    void contextMenuKeyPressed(); // Menu key / Shift+F10, while the window is shown
    void groupNameRequested(int row, qint64 group); // a name for a new group (group 0) for the row, or a new one for `group`
    void fieldChanged();
    // The field the window goes back to: its own window in front again (App).
    void fieldRequested(QQuickItem* field);

private:
    struct Category {
        ClipFilter::Category kind;
        qint64 group = 0;
    };
    // Where a paste goes: only onto the clipboard (a copy), into the program
    // that was in front, or into a field of Win顺's own.
    enum class Into { Clipboard, Program, Field };
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
    std::vector<const Clip*> targets(int row) const; // the row, or the picked ones it is one of
    ClipWrite writeFor(const std::vector<const Clip*>& clips, bool plainText, QString* problem) const;
    void startPaste(ClipWrite write, Into into, std::vector<qint64> touched, QString done);
    void onWritten(bool ok);
    void returnToField(QString text); // `text` typed into it; none: Esc
    void fillField(); // it has the keyboard again (or waited long enough)
    void dropUndo();
    QString separator() const;

    ClipStore* m_store;
    ClipboardWatcher* m_watcher;
    ClipModel m_model;
    QPointer<QWindow> m_window;
    bool m_active = false;
    bool m_keysRouted = false;
    bool m_recording = true;
    bool m_paused = false;
    QString m_shortcut;
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
    QElapsedTimer m_pasteClock;
    QElapsedTimer m_frontClock; // since the target came to the front
    QTimer m_keysTimer; // waits for the modifier keys to be let go
    QTimer m_focusTimer; // waits for the target to be in front
    // Going back to a field: it, and the text that goes in.
    Field m_returning;
    QString m_returnText;
    QElapsedTimer m_returnClock;
    QTimer m_returnTimer; // waits for the field to have the keyboard
};

} // namespace ws
