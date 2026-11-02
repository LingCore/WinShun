#pragma once

#include "ClipboardDisplayProtocol.h"
#include "platform/KeyRouter.h"

#include <QJsonArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QThreadPool>

#include <atomic>
#include <memory>
#include <thread>

class QQuickWindow;

namespace ws {
class Clipboard;
class Placement;

// A display-only peer. The store, native clipboard, plugins and paste actions
// remain in the ordinary GUI process. Only a service-launched peer may connect.
class ClipboardDisplay : public QObject {
    Q_OBJECT
public:
    ClipboardDisplay(Clipboard* clipboard, QQuickWindow* window, Placement* placement, bool probe = false, QObject* parent = nullptr);
    ~ClipboardDisplay() override;

    bool ready() const { return m_ready; }
    bool active() const { return m_active; }
    bool uiAccess() const { return m_ready && !m_probe; }
    HWND handle() const { return m_handle; }
    void start();
    bool show();
    void hide();
    void activate();
    void passive();
    void key(const KeyRouter::Key& key);
    void event(const QString& name, const QJsonArray& arguments = {});
    QString probeOutput() { return m_probe ? QString::fromUtf8(m_probeChild.readAll()) : QString(); }

signals:
    void available();
    void lost();
    void geometryEdited(const QRect& geometry);
    void focusLost();

private slots:
    void schedule();
    void synchronize();

private:
    void accept();
    void receive();
    void dispatch(const QJsonObject& message);
    void send(const QJsonObject& message);
    void disconnectPeer();
    void lease(const std::wstring& pipe);

    Clipboard* m_clipboard;
    QQuickWindow* m_window;
    const HWND m_sourceWindow;
    Placement* m_placement;
    QLocalServer m_server;
    QPointer<QLocalSocket> m_socket;
    clipdisplay::Decoder m_decoder;
    QTimer m_update;
    std::vector<QJsonObject> m_rows;
    bool m_ready = false;
    bool m_active = false;
    HWND m_handle = nullptr;
    QString m_pipe;
    std::atomic<DWORD> m_peerPid {0};
    HANDLE m_stop = nullptr;
    std::thread m_lease;
    const bool m_probe;
    QProcess m_probeChild;
    QThreadPool m_icons;
    int m_imageRequests = 0;
};

// Runs before the normal App / instance mutex / plugin runtime. Production
// requires a UIAccess token and an authenticated parent pipe. A medium-token
// probe can exercise the UI transport, but is never reported as UIAccess.
bool clipboardDisplayRequested(const QStringList& arguments);
int runClipboardDisplay(const QStringList& arguments);

} // namespace ws
