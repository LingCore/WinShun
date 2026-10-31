#pragma once

#include "PieGeometry.h"
#include "platform/ComWorker.h"
#include "platform/PieDrop.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class QQuickWindow;
class QScreen;

namespace ws {

class Favorites;
class SystemCatalog;

// 拖放圆盘: while files are dragged (File Explorer, the desktop, any file
// manager that drags with OLE), or a file only held, the key under Esc
// (` and ·) brings up the favorite folders round the pointer, as small
// chips, the first eight. The direction the pointer goes from the middle
// chooses one (PieGeometry): let go and the files are moved there within a
// drive, copied to another, Ctrl and Shift changing that as in File
// Explorer. Let go in the middle, or press Esc, and nothing happens; leave
// the chips and they go away, the drag carrying on as before; the key again
// does the same.
//
// KeyListener sees the key (Raw Input, or its hotkey) and that an OLE drag is
// under way; this shows the window (DropPieWindow.qml, made once and kept)
// without taking the focus, and is its OLE drop target in place of Qt's
// (piedrop::DropTarget). It shows itself only once the drag enters it with
// files or shell items: a drag of text (Word, VS Code) passes the
// listener's test too. Lasting files are copied or moved by IFileOperation
// on threads of the application's (piedrop::FileOperations) and the source
// is told nothing was dropped, so it never deletes anything itself (one that
// drops asynchronously, File Explorer, is held until the copy is over);
// everything else (a phone's or a zip file's contents, the Recycle Bin's, an
// archiver's unpacked copies in the temporary folder) goes to the folder's
// own drop target while the source still holds it, as in File Explorer
// (piedrop::DragItems).
//
// Its window is see-through but still takes the pointer where nothing is
// drawn (a layered D3D11 window): it goes the moment the pointer leaves the
// chips' reach, so it never stands between a drag and what is under it.
class DropPie : public QObject, private piedrop::Handler {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    // The folders' chips: {name, label, hint, path, disabled, reason, x, y,
    // width}: label the name as shown (shortened in its middle), hint its
    // parent folder's name when another has the same name; x, y the chip's
    // top left from the centre.
    Q_PROPERTY(QVariantList sectors READ sectors NOTIFY sectorsChanged FINAL)
    Q_PROPERTY(bool noFolders READ noFolders NOTIFY sectorsChanged FINAL)
    Q_PROPERTY(QPointF centre READ centre NOTIFY layoutChanged FINAL) // in the window
    Q_PROPERTY(qreal sizeScale READ sizeScale NOTIFY layoutChanged FINAL) // 界面大小
    Q_PROPERTY(int labelPixelSize READ labelPixelSize NOTIFY layoutChanged FINAL)
    Q_PROPERTY(qreal hubRadius READ hubRadius NOTIFY layoutChanged FINAL)
    Q_PROPERTY(qreal chipHeight READ chipHeight NOTIFY layoutChanged FINAL)
    Q_PROPERTY(QRectF statusRect READ statusRect NOTIFY layoutChanged FINAL) // in the window
    Q_PROPERTY(bool revealed READ revealed NOTIFY revealedChanged FINAL)
    Q_PROPERTY(int hovered READ hovered NOTIFY stateChanged FINAL) // the sector the pointer is in, -1
    Q_PROPERTY(int picked READ picked NOTIFY stateChanged FINAL) // ... if it can be dropped on, else -1
    Q_PROPERTY(QString mode READ mode NOTIFY stateChanged FINAL) // the picked one's: "move", "copy" or ""
    Q_PROPERTY(bool confirming READ confirming NOTIFY stateChanged FINAL) // dropped: the chip flashes
    // The line under the chips: what letting go does ("移动到" and the
    // folder's name, its path, the key that changes it) or why it cannot.
    Q_PROPERTY(QString statusVerb READ statusVerb NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString statusName READ statusName NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString statusDetail READ statusDetail NOTIFY stateChanged FINAL) // the path, shortened
    Q_PROPERTY(QString statusHint READ statusHint NOTIFY stateChanged FINAL)

public:
    // A short notice to the user (the tray's), callable from any thread.
    using Notify = std::function<void(const QString& title, const QString& text)>;

    // `operations` is the application's (it outlives the pie and waits for
    // copies under way when the application goes); `notify` tells the user
    // when one fails.
    DropPie(Favorites* favorites, SystemCatalog* places, piedrop::FileOperations* operations, Notify notify,
        QObject* parent = nullptr);
    ~DropPie() override;

    // DropPieWindow: made into a see-through window and taken over as a
    // drop target here, then shown once out of sight so that its first
    // real showing is not slowed by the graphics' setting up.
    void setWindow(QQuickWindow* window);
    bool dropReady() const noexcept { return m_hwnd && m_target; }

    // From the listener thread (KeyListener::Callbacks): during an OLE drag
    // at `pt` (physical pixels), whose source has captured the mouse with
    // `captureWindow`, the pie's key pressed (again: it goes); the primary
    // button let go.
    void summoned(POINT pt, HWND captureWindow, DWORD sourcePid);
    void buttonReleased();
    // Inside a drop (the folder's drop target may run a dialog's loop): not
    // to be destroyed now.
    bool inDrop() const noexcept { return m_inDrop; }

    QVariantList sectors() const { return m_sectorItems; }
    bool noFolders() const noexcept { return m_folders.empty(); }
    QPointF centre() const noexcept { return m_centre; }
    qreal sizeScale() const noexcept { return m_scale; }
    int labelPixelSize() const noexcept { return m_labelPixelSize; }
    qreal hubRadius() const noexcept { return m_metrics.hub; }
    qreal chipHeight() const noexcept { return m_metrics.chipHeight; }
    QRectF statusRect() const noexcept { return m_statusRect; }
    bool revealed() const noexcept { return m_revealed; }
    int hovered() const noexcept { return m_hovered; }
    int picked() const noexcept { return m_picked; }
    QString mode() const;
    bool confirming() const noexcept { return m_confirming; }
    QString statusVerb() const { return m_statusVerb; }
    QString statusName() const { return m_statusName; }
    QString statusDetail() const { return m_statusDetail; }
    QString statusHint() const { return m_statusHint; }

    // `text` shortened in its middle with "…" to `width` logical pixels, in
    // the application's font at `pixelSize` (Bold if `bold`): the same
    // fitter as the chips' labels, for the line under them.
    Q_INVOKABLE QString elide(const QString& text, int pixelSize, bool bold, qreal width) const;

signals:
    void sectorsChanged();
    void layoutChanged();
    void revealedChanged();
    void stateChanged();

private:
    struct Folder {
        QString path; // file-system path, no trailing backslash but a drive's
        QString name; // as the favorite is named
        bool network = false;
    };
    enum class Block { None, Missing, Source, Inside, Network };

    // piedrop::Handler, on the GUI thread
    DWORD dragEnter(IDataObject* data, DWORD keys, POINT pt, DWORD allowed) override;
    DWORD dragOver(DWORD keys, POINT pt, DWORD allowed) override;
    void dragLeave() override;
    DWORD drop(IDataObject* data, DWORD keys, POINT pt, DWORD allowed) override;

    void onSummoned(POINT pt, HWND captureWindow, DWORD sourcePid);
    void open(POINT pt);
    QRect place(QScreen* screen, POINT pt); // the window's, the middle and the line in it, m_warp
    void follow(POINT pt); // the window placed for the pointer at `pt`
    DWORD reveal(DWORD keys, POINT pt);
    void hide(const char* why);
    void nudge() const;
    bool takeDropSite();
    void prewarm();
    void gatherFolders(); // favorites -> m_folders, places resolved on the worker
    void checkFolders(); // which local ones are missing, on the worker
    void updateSectors();
    Block blockOf(const Folder& folder) const;
    QString reasonOf(Block block) const;
    pie::Hit hitAt(POINT pt) const;
    POINT centreOnScreen() const; // physical pixels
    pie::Allowed allowed() const noexcept { return {bool(m_allowed & DROPEFFECT_MOVE), bool(m_allowed & DROPEFFECT_COPY)}; }
    pie::Mode modeOf(int sector) const noexcept;
    DWORD track(DWORD keys, POINT pt); // DragOver's work: the state and the effect
    void updateTexts();
    void setRevealed(bool revealed);

    Favorites* m_favorites;
    SystemCatalog* m_places;
    QPointer<QQuickWindow> m_window;
    HWND m_hwnd = nullptr; // the window's, as registered for drops
    Microsoft::WRL::ComPtr<piedrop::DropTarget> m_target;

    std::vector<Folder> m_folders; // the sectors, at most eight
    QSet<QString> m_missing; // case-folded paths of local folders found missing
    QVariantList m_sectorItems;
    pie::Metrics m_metrics;
    pie::Layout m_layout;
    qreal m_scale = 0; // none yet: the first updateSectors sets it
    int m_labelPixelSize = 13;
    QPointF m_centre; // in the window
    QRectF m_statusRect; // in the window

    // The drag over the pie
    bool m_open = false; // shown (revealed or not)
    bool m_entered = false; // DragEnter came
    bool m_revealed = false;
    bool m_confirming = false;
    bool m_inDrop = false;
    DWORD m_allowed = 0; // what the source allows
    std::vector<std::wstring> m_items; // the file-system paths of what is dragged (piedrop::DragItems)
    std::size_t m_count = 0; // ... and how many items there are
    bool m_plain = false; // ... all lasting files: copied by our own file operation
    std::vector<Block> m_blocks; // by sector, for this drag
    std::vector<bool> m_sameDrive; // by sector: every item already on its drive
    pie::Hit m_hit;
    pie::Keys m_keys;
    int m_hovered = -1;
    int m_picked = -1;
    QString m_statusVerb;
    QString m_statusName;
    QString m_statusDetail;
    QString m_statusHint;
    POINT m_shownAt {};
    qreal m_dpr = 1; // the scale of the screen it is on (the window's may lag behind on its first showing there)
    bool m_warp = false; // placed off the pointer by a screen's edge: the pointer goes to the middle at DragEnter
    QElapsedTimer m_warped; // since it went: moves OLE saw before it are still on their way
    QElapsedTimer m_shownClock;
    QTimer m_revealWait; // no DragEnter yet: the pointer still, or the drag elsewhere
    QTimer m_releaseWait; // the button let go without a Drop or a DragLeave
    QTimer m_confirmTimer; // the dropped-on chip shown a moment
    int m_checks = 0; // the latest existence check: older answers are dropped

    piedrop::FileOperations* m_operations;
    Notify m_notify;
    ComWorker m_worker; // last: its tasks use the members above
};

} // namespace ws
