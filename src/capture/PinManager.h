#pragma once

#include "PinGroups.h"
#include "PinKeyCard.h"
#include "PinWindow.h"
#include "platform/Shell.h"

#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ws::capture {

// The pictures pinned on the screen (PinWindow): the screen capture's, and
// pictures pinned from a file's row or the clipboard history (贴到屏幕),
// which need no capture turned on. The app makes it when first wanted;
// turning the capture off closes them all (the stage 2 rule). Each one's
// menu (the native popup menu, as the tray's): 复制, 保存, 另存为…, 编辑,
// 识别文字, 选择文字, 旋转与翻转, 收起, 置顶, 鼠标穿透, 关闭, 关闭其他, 关闭
// 全部贴图, 全部贴图 (隐藏 / 显示, 全部置顶). What leaves (the clipboard, a
// file, the OCR window) goes through the callbacks.
class PinManager : public QObject {
    Q_OBJECT

public:
    struct Callbacks {
        std::function<void(const QImage&, bool screenshot)> copy; // screenshot: the pin came from a capture
        std::function<void(const QImage&, const QString& path)> save; // empty path: the Screenshots folder
        // Direct save-back to the pin's original image file (#427); falls back to `save` if unset.
        std::function<void(PinWindow*, const QImage&, const QString& path)> saveSource;
        std::function<void(const QImage&, double dpr, const QRect& picture)> ocr;
        std::function<void(const QImage&, double dpr, const QRect& picture)> ocrCopy; // Shift+C / 复制文字 (#423)
        std::function<void(const QImage&, const QRect& picture)> formula; // Shift+F / 识别公式
        std::function<void(PinWindow*)> translate; // 翻译: its words translated in place (#191); empty: not offered
        std::function<void(PinWindow*)> edit; // 编辑: the editing window opens on it
        std::function<void(PinWindow*)> closing; // a pin is about to close (pixels still intact, #446)
        std::function<void()> closed; // a pin closed: its memory can go back to Windows
        std::function<bool()> dark;
        std::function<QColor()> accent; // a fill that carries white words
        std::function<QColor()> edge; // PinWindow::Host::edge
        std::function<qreal()> pillRadius; // PinWindow::Host::pillRadius
        std::function<void(const QString&)> copyText; // the text picked on a pin
        // The lines of a picture (CaptureOcr::scan), for picking text and auto-OCR.
        std::function<void(const QImage&, double dpr, std::function<void(QList<RedactLine>)> done,
            std::shared_ptr<const std::atomic<bool>> cancelled)>
            readLines;
        std::function<bool()> ocrAvailable; // checked again after an on-demand installation
        // The host supplies its themed native menu; standalone previews can use the system menu.
        std::function<int(HWND, const std::vector<shell::MenuItem>&, POINT)> popupMenu;
        // The key card's colours (the 界面风格's); empty: PinKeyCard::Look::fallback.
        std::function<PinKeyCard::Look()> keyCardLook;
        // Pin groups (#451): notified whenever groups or active group change (for persistence),
        // when a user-facing notice should be shown (e.g., pin moved to a hidden group), or
        // when the pin context menu asks to create a new group and move `pin` into it.
        std::function<void()> groupsChanged;
        std::function<void(const QString& text)> groupNotice;
        std::function<void(PinWindow* moveTarget)> requestNewGroup;
    };

    explicit PinManager(Callbacks callbacks, QObject* parent = nullptr);
    ~PinManager() override;

    // A pin of `image` with its top left at `origin` (physical, virtual
    // desktop); `screenshot`: taken with the capture (PinWindow::screenshot);
    // `project`: its annotations, to edit again (empty: none).
    // The pin made (the manager's, here until it is closed: contains()); none for a null picture.
    PinWindow* pin(const QImage& image, QPoint origin, bool screenshot = true, QByteArray project = {},
        std::optional<QList<RedactLine>> recognizedLines = std::nullopt);
    // A picture not shown where it was taken (a long screenshot, a file, the
    // clipboard's): its own pixels kept, it starts within the work area of
    // the monitor at `anchor`, shown fitted when larger than 90% of it, its
    // pixel at `anchor` kept in place (PinWindow). `text`: the words the
    // picture shows (copied text pinned, GitHub #388), copied by Ctrl+C.
    PinWindow* pinFitted(const QImage& image, QPoint origin, QPoint anchor, bool screenshot, QString text = {},
        std::optional<QList<RedactLine>> recognizedLines = std::nullopt);
    int count() const { return static_cast<int>(m_pins.size()); }
    // `pin` is one of the pins on the screen (not closed since): for what
    // keeps a pointer to one meanwhile (the editing window).
    bool contains(const PinWindow* pin) const;
    // Whether an open pin currently tracks PinHistory entry `historyId` (#446).
    bool hasHistoryPin(quint64 historyId) const;
    bool anyClickThrough() const;
    void clearClickThrough(); // the tray menu, the restore hotkey: every pin takes the mouse again
    void closeAll();
    void closeOthers(const PinWindow* keep); // 关闭其他: only the one kept stays
    // 隐藏全部贴图 / 显示全部贴图: out of sight without closing them. A pin made
    // while they are hidden shows them again.
    bool allHidden() const { return m_hidden; }
    void setAllHidden(bool hidden);
    // Light or dark, or the 界面风格, changed: the pins' borders again.
    void restyle();
    // 全部置顶: every pin on top of other windows, or none.
    bool allTopmost() const;
    void setAllTopmost(bool on);
    // [Capture] PinDoubleClick: for every pin, and those made later.
    void setDoubleClick(PinDoubleClick action);
    // The keys inside the pins (and in their menus), for every pin, and those made later.
    void setKeys(const CaptureKeys& keys);
    // [Capture] KeyHints: the key card (GitHub #343, PinKeyCard) shows by
    // itself when a pin has the focus and the user stops (PinHintPolicy);
    // off, only ? or F1 shows it.
    void setKeyHints(bool on);
    // [Capture] PinAutoOcr: quietly recognize text on pinned pictures for direct drag selection.
    void setAutoOcr(bool on);
    bool keyCardShown() const { return m_card && m_card->visible(); }
    const CaptureKeys& keys() const { return m_keys; }
    // True while the Ctrl watcher runs (some pin is click-through), else no timer at all.
    bool watching() const { return m_watch.isActive(); }

    // Pin groups (#451): organize pins by work context, switching visibility per group.
    const PinGroupStore& groups() const { return m_groups; }
    void loadGroups(const QString& serialized, const QString& activeToken);
    std::optional<PinGroup> createGroup(const QString& name, PinWindow* movePin = nullptr, QString* error = nullptr);
    bool renameGroup(const QString& id, const QString& name, QString* error = nullptr);
    bool deleteGroup(const QString& id);
    void switchGroup(const QString& id);
    void showAllGroups();
    std::optional<QString> cycleGroup();
    void movePinToGroup(PinWindow* pin, const QString& groupId);
    int pinCountInGroup(const QString& groupId) const;
    int visiblePinCount() const;
    void closeActiveGroup();

private:
    void syncGroupVisibility();
    void setWatching(); // starts or stops the watcher to match anyClickThrough()
    void watch(); // one tick: Ctrl and the cursor against every click-through pin
    PinWindow* add(const QImage& image, QPoint origin, bool screenshot, QByteArray project, std::optional<QPoint> fitAbout,
        std::optional<QList<RedactLine>> recognizedLines, bool announceLines);
    void showMenu(PinWindow* pin, POINT at);
    void copyWords(PinWindow* pin);
    void closed(PinWindow* pin);
    // The key card: what the pins report (PinWindow::Host), and the card shown or taken away to match.
    void hintFocus(PinWindow* pin, bool focused, bool fromOwnWindow);
    void hintInput(PinWindow* pin, PinInput input);
    void hintToggle(PinWindow* pin);
    void hintStop(const PinWindow* pin); // edited or collapsed: no card for the rest of the round
    void syncHints();

    Callbacks m_callbacks;
    PinGroupStore m_groups;
    std::vector<std::unique_ptr<PinWindow>> m_pins;
    QTimer m_watch; // a click-through pin under the cursor with Ctrl held takes the mouse (#59)
    bool m_hidden = false; // 隐藏全部贴图
    bool m_autoOcr = true;
    PinDoubleClick m_doubleClick = PinDoubleClick::Reset;
    CaptureKeys m_keys;
    const PinWindow* m_menuPin = nullptr; // the pin whose menu is up
    std::vector<std::unique_ptr<PinWindow>> m_closing; // windows gone, deleted once their procedure returned
    // The key card
    PinHintPolicy m_hints;
    QTimer m_hintTimer; // wakes the policy at its next deadline
    QElapsedTimer m_hintClock;
    PinWindow* m_hintPin = nullptr; // the pin with the focus
    const PinWindow* m_lastHintPin = nullptr; // the last one that had it: compared, never used
    std::unique_ptr<PinKeyCard> m_card; // only while it shows
};

} // namespace ws::capture
