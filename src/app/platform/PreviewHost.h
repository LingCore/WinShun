#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QRect>
#include <QUuid>

#include <memory>
#include <optional>

using HWND = struct HWND__*;

namespace ws {

// Shows a file through its shell preview handler (IPreviewHandler: what
// Explorer's preview pane uses for Word, Excel, PowerPoint, PDF, mail,
// fonts…), for the preview window (FilePreview).
//
// The handler runs out of process (the system's surrogate, prevhost.exe,
// as for Explorer) and draws into a window of ours: a borderless popup that
// never takes the focus, laid over the preview window's content area, on a
// thread of its own (an STA with its own message loop). Not a child of the
// preview window: a child of another thread joins that thread's input
// queue, and a handler that hangs would then freeze the launcher's keys
// too. A handler that does not answer in time (kTimeout) counts as failed:
// failed() and the thread is left behind (a new one serves the next file),
// never waited for, and that handler is not tried again while Win顺 runs.
// The class factories stay (locked) while previews come, so Word or Excel
// starts once, not for every file; a minute without a preview lets them go.
class PreviewHost : public QObject {
    Q_OBJECT

public:
    PreviewHost();
    ~PreviewHost() override;

    // The preview handler registered for the file's type, if any (the
    // registry; any thread).
    static std::optional<QUuid> handlerFor(const QString& path);

    // `ticket` names the request in failed(). The rectangle is in physical
    // pixels on the screen; `window`: the preview window, which the popup
    // goes just above. `dark`: the colours the handler is asked to use, for
    // those that take them (IPreviewHandlerVisuals).
    void show(quint64 ticket, const QString& path, const QUuid& handler, int left, int top, int right, int bottom,
        HWND window, bool dark);
    void move(int left, int top, int right, int bottom);
    void hide(); // and the handler unloaded
    // The popup, a window in it, or one of the handler's program (Word
    // taking the focus as it starts).
    bool owns(HWND window) const;
    std::optional<QRect> visibleRect() const;

signals:
    void failed(quint64 ticket);

private:
    struct Worker;
    std::shared_ptr<Worker> worker();

    std::shared_ptr<Worker> m_worker;
    QTimer m_watchdog;
    quint64 m_waiting = 0; // the ticket the watchdog waits on
    QElapsedTimer m_since; // since show(): the handler's program counts as ours for a few seconds
    QString m_server; // the program that serves it ("excel.exe"), see owns()
    QUuid m_handler; // its handler: one that hangs is not tried again (handlerFor)
};

} // namespace ws
