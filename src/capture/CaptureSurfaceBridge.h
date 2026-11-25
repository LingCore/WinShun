#pragma once

#include "CaptureSurfaceProtocol.h"

#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QTimer>

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

class QQuickWindow;

namespace ws::capture {

// Maps a Win32 key from the UIAccess Band-2 capture surface to a Qt::Key,
// covering every shortcut and punctuation key CaptureOverlay uses.
int qtKeyFromSurface(std::uint32_t vk, bool extended, bool shift, const QString& text);

// Plugin-side bridge to WinShunService.exe --capture-surface (the protected
// High-IL UIAccess Band-2 mirror window). Runs inside the ordinary Medium-IL
// winshun.capture plugin:
//   - creates the private per-session named pipe and verifies the helper PID;
//   - sends Init once both overlays have rendered and the pipe is verified;
//   - translates relayed Mouse/Key/Ime events into QQuickWindow events;
//   - syncs QQuickWindow cursor (standard & custom RGBA) and IME state back.
class CaptureSurfaceBridge : public QObject {
    Q_OBJECT

public:
    struct Target {
        QPointer<QQuickWindow> window;
        QRect physicalRect;
        double dpr = 1.0;
    };

    struct OpenReply {
        bool ok = false;
        DWORD childPid = 0;
        QString reason;
        DWORD error = 0;
    };
    using OpenFn = std::function<void(HWND fullScreenHwnd, const QString& pipeName, std::function<void(OpenReply)> done)>;
    using CloseFn = std::function<void()>;

    explicit CaptureSurfaceBridge(QObject* parent = nullptr);
    ~CaptureSurfaceBridge() override;

    // Starts the private pipe server and asks Win顺's host to lease the
    // UIAccess surface from WinShunService.exe.
    bool start(HWND fullScreenTarget, std::vector<Target> targets, int activeIndex, OpenFn openSurface, CloseFn closeSurface);
    // Called once the Band-1 CaptureOverlay windows have rendered their first
    // frame and uncloaked (ready for DwmRegisterThumbnail).
    void setOverlaysReady();
    // Tears down the surface immediately (sends Close, stops the pipe thread,
    // and releases the host lease). Safe to call repeatedly.
    void stop();

    bool active() const noexcept { return m_running; }
    bool isReady() const noexcept { return m_ready; }
    const std::vector<HWND>& surfaceHwnds() const noexcept { return m_surfaceHwnds; }
    bool isSurfaceHwnd(HWND hwnd) const noexcept;

signals:
    void ready(const std::vector<HWND>& surfaceHwnds);
    void failed(const QString& reason, unsigned long error);
    void closedExternally(ws::capsurface::CloseReason reason);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void onHostOpenReply(OpenReply reply);
    void onPipeConnected(DWORD clientPid);
    void trySendInit();
    void onFrameReceived(ws::capsurface::Type type, std::vector<std::byte> payload);
    void onPipeBroken();
    void deliverMouse(const ws::capsurface::MouseMsg& msg);
    void deliverKey(const ws::capsurface::KeyMsg& msg);
    void deliverIme(const ws::capsurface::ImeEventMsg& msg);
    void syncCursor(QQuickWindow* window);
    void syncIme();
    bool sendFrame(std::vector<std::byte> frame);
    void pipeLoop(HANDLE pipe, HANDLE stopEvent);
    int targetIndexAt(int globalX, int globalY) const;
    void setQuickFocus(int index);
    void clearQuickFocus();
    void installWatchers();
    void removeWatchers();

    std::vector<Target> m_targets;
    int m_activeIndex = 0;
    int m_focusedIndex = -1;
    int m_hoverIndex = -1;
    int m_captureIndex = -1;
    CloseFn m_closeSurface;

    bool m_running = false;
    bool m_overlaysReady = false;
    bool m_initSent = false;
    bool m_ready = false;
    bool m_hostReplied = false;
    bool m_pipeConnected = false;
    DWORD m_expectedPeerPid = 0;
    DWORD m_connectedPeerPid = 0;
    std::vector<HWND> m_surfaceHwnds;

    // Cursor / IME dedup state (GUI thread only).
    bool m_hasLastCursor = false;
    capsurface::CursorKind m_lastCursorKind = capsurface::CursorKind::Standard;
    std::uint32_t m_lastStandardShape = 0;
    std::uint64_t m_lastRgbaKey = 0;
    std::set<std::uint64_t> m_sentRgbaCursors;
    bool m_lastImeEnabled = false;
    int m_lastImeMonitor = -1;
    QRect m_lastImeCaret;
    bool m_watchersInstalled = false;

    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    HANDLE m_stopEvent = nullptr;
    HANDLE m_writeEvent = nullptr;
    std::mutex m_writeMutex;
    std::thread m_reader;
    std::atomic<bool> m_stopping {false};
    QTimer m_timeout;
};

} // namespace ws::capture
