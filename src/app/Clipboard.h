#pragma once

#include "ClipModel.h"
#include "ClipStore.h"
#include "platform/ClipboardWatcher.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
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

// The view-model behind the launcher's clipboard page (Win+V): the history
// listed by category or group and searched, a preview of the current entry,
// and pasting into the program that was in front before the launcher.
//
// A paste waits until Shift, Ctrl, Alt and the Windows keys are let go (so
// they do not join the paste keys), puts the entry on the clipboard with all
// the formats it was copied with (or only its text), brings that program
// back and presses its paste keys there.
class Clipboard : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(bool active READ active NOTIFY activeChanged FINAL) // the launcher shows the clipboard page
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
    // The window to paste into: the one in front before the launcher opened.
    void setTarget(HWND window) { m_target = window; }
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
    Q_INVOKABLE void dismiss();
    Q_INVOKABLE void turnOn(); // the history, from the page's button

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
    void contextMenuKeyPressed(); // Menu key / Shift+F10, while the page is shown
    void groupNameRequested(int row, qint64 group); // a name for a new group (group 0) for the row, or a new one for `group`

private:
    struct Category {
        ClipFilter::Category kind;
        qint64 group = 0;
    };
    std::vector<Category> categoryList() const;
    void refresh();
    void refreshStatus();
    void flash(const QString& message);
    std::vector<const Clip*> targets(int row) const; // the row, or the picked ones it is one of
    ClipWrite writeFor(const std::vector<const Clip*>& clips, bool plainText, QString* problem) const;
    void startPaste(ClipWrite write, bool intoTarget, std::vector<qint64> touched, QString done);
    void onWritten(bool ok);
    void dropUndo();
    QString separator() const;

    ClipStore* m_store;
    ClipboardWatcher* m_watcher;
    ClipModel m_model;
    QPointer<QWindow> m_window;
    bool m_active = false;
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
    bool m_pasting = false;
    ClipWrite m_write;
    bool m_intoTarget = false;
    std::vector<qint64> m_touched; // moved up once pasted (joined pastes; a single one moves up by itself)
    QString m_doneMessage; // a copy without pasting says so
    HWND m_pasteWindow = nullptr;
    QElapsedTimer m_pasteClock;
    QElapsedTimer m_frontClock; // since the target came to the front
    QTimer m_keysTimer; // waits for the modifier keys to be let go
    QTimer m_focusTimer; // waits for the target to be in front
};

} // namespace ws
