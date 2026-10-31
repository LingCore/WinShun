#pragma once

#include "PinWindow.h"

#include <QColor>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QTimer>

#include <functional>
#include <memory>
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
        std::function<void(const QImage&, double dpr, const QRect& picture)> ocr;
        std::function<void(PinWindow*)> edit; // 编辑: the editing window opens on it
        std::function<void()> closed; // a pin closed: its memory can go back to Windows
        std::function<bool()> dark;
        std::function<QColor()> accent; // a fill that carries white words
        std::function<void(const QString&)> copyText; // the text picked on a pin
        // The lines of a picture (CaptureOcr::scan), for picking text.
        std::function<void(const QImage&, double dpr, std::function<void(QList<RedactLine>)> done)> readLines;
        std::function<bool()> ocrAvailable; // checked again after an on-demand installation
    };

    explicit PinManager(Callbacks callbacks, QObject* parent = nullptr);
    ~PinManager() override;

    // A pin of `image` with its top left at `origin` (physical, virtual
    // desktop); `screenshot`: taken with the capture (PinWindow::screenshot);
    // `project`: its annotations, to edit again (empty: none).
    void pin(const QImage& image, QPoint origin, bool screenshot = true, QByteArray project = {});
    int count() const { return static_cast<int>(m_pins.size()); }
    // `pin` is one of the pins on the screen (not closed since): for what
    // keeps a pointer to one meanwhile (the editing window).
    bool contains(const PinWindow* pin) const;
    bool anyClickThrough() const;
    void clearClickThrough(); // the tray menu, the restore hotkey: every pin takes the mouse again
    void closeAll();
    void closeOthers(const PinWindow* keep); // 关闭其他: only the one kept stays
    // 隐藏全部贴图 / 显示全部贴图: out of sight without closing them. A pin made
    // while they are hidden shows them again.
    bool allHidden() const { return m_hidden; }
    void setAllHidden(bool hidden);
    // 全部置顶: every pin on top of other windows, or none.
    bool allTopmost() const;
    void setAllTopmost(bool on);
    // [Capture] PinDoubleClick: for every pin, and those made later.
    void setDoubleClick(PinDoubleClick action);
    // True while the Ctrl watcher runs (some pin is click-through), else no timer at all.
    bool watching() const { return m_watch.isActive(); }

private:
    void setWatching(); // starts or stops the watcher to match anyClickThrough()
    void watch(); // one tick: Ctrl and the cursor against every click-through pin
    void showMenu(PinWindow* pin, POINT at);
    void closed(PinWindow* pin);

    Callbacks m_callbacks;
    std::vector<std::unique_ptr<PinWindow>> m_pins;
    QTimer m_watch; // a click-through pin under the cursor with Ctrl held takes the mouse (#59)
    bool m_hidden = false; // 隐藏全部贴图
    PinDoubleClick m_doubleClick = PinDoubleClick::Reset;
    const PinWindow* m_menuPin = nullptr; // the pin whose menu is up
    std::vector<std::unique_ptr<PinWindow>> m_closing; // windows gone, deleted once their procedure returned
};

} // namespace ws::capture
