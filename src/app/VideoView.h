#pragma once

#include <QImage>
#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QSize>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <windows.h>
#include <wrl/client.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IMFDXGIDeviceManager;
struct IMFMediaEngine;

namespace ws {

class VideoEngineNotify;

// A video played by Windows itself, for the preview (FilePreview's Video):
// Media Foundation's media engine with the decoders this PC has (hardware
// ones where there are), nothing of our own bundled. Each new frame is
// scaled by the GPU to the size it is shown at (a device of our own), read
// back and drawn as a picture, so that QML's controls can go over it.
//
// Plays as soon as it can. Hidden, its window hidden, or another source:
// the engine goes (and so the sound, and the file is let go); shown again,
// it starts over. The volume and muting are kept for the session.
class VideoView : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged FINAL) // a file's path
    Q_PROPERTY(Status status READ status NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged FINAL)
    Q_PROPERTY(bool ended READ ended NOTIFY playingChanged FINAL)
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY hasFrameChanged FINAL) // a picture of it is up (else the poster)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged FINAL) // seconds; 0: not known
    Q_PROPERTY(double position READ position NOTIFY positionChanged FINAL) // seconds
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged FINAL) // 0 to 1
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged FINAL)
    Q_PROPERTY(QSizeF videoSize READ videoSize NOTIFY videoSizeChanged FINAL) // as shown (its pixel shape applied)
    Q_PROPERTY(QRectF videoRect READ videoRect NOTIFY videoRectChanged FINAL) // where the picture is, in the item

public:
    enum Status { Empty, Loading, Ready, Failed };
    Q_ENUM(Status)

    explicit VideoView(QQuickItem* parent = nullptr);
    ~VideoView() override;

    QString source() const { return m_source; }
    void setSource(const QString& source);
    Status status() const { return m_status; }
    bool playing() const { return m_playing; }
    bool ended() const { return m_ended; }
    bool hasFrame() const { return !m_frame.isNull(); }
    double duration() const { return m_duration; }
    double position() const { return m_position; }
    double volume() const;
    void setVolume(double volume);
    bool muted() const;
    void setMuted(bool muted);
    QSizeF videoSize() const { return m_videoSize; }
    QRectF videoRect() const;

    // Paused or at the end: play (from the start, at the end); playing: pause.
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void seek(double seconds);

    // Whether Windows has a media engine at all (not on N editions without the Media Feature Pack).
    static bool available();

signals:
    void sourceChanged();
    void statusChanged();
    void playingChanged();
    void hasFrameChanged();
    void durationChanged();
    void positionChanged();
    void volumeChanged();
    void mutedChanged();
    void videoSizeChanged();
    void videoRectChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void geometryChange(const QRectF& now, const QRectF& before) override;

private:
    friend class VideoEngineNotify;
    void onEngineEvent(quint64 generation, DWORD event);
    void updateActive();
    bool open();
    void close();
    void tick();
    QSize frameSize() const; // the frame's in device pixels: as shown (videoRect)
    bool transferFrame();
    void readState();
    void setStatus(Status status);

    QString m_source;
    Status m_status = Empty;
    bool m_playing = false;
    bool m_ended = false;
    double m_duration = 0;
    double m_position = 0;
    QSizeF m_videoSize;
    QImage m_frame;
    bool m_frameNew = false;
    bool m_seeking = false;
    quint64 m_generation = 0;
    QPointer<QQuickWindow> m_window;
    QTimer m_tick;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_manager;
    Microsoft::WRL::ComPtr<IMFMediaEngine> m_engine;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_target; // the frame, at the size shown
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging; // ...read back from here
    QSize m_targetSize;
    VideoEngineNotify* m_notify = nullptr;
};

} // namespace ws
