#pragma once

#include "Recording.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <windows.h>

#include <functional>
#include <memory>
#include <thread>

class QQmlEngine;
class QQuickWindow;

namespace ws::capture::recording {
class KeyHook;
}

namespace ws::capture {

class Recorder : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Made by the screen capture")

    Q_PROPERTY(Stage stage READ stage NOTIFY changed FINAL)
    Q_PROPERTY(QString status READ status NOTIFY changed FINAL)
    Q_PROPERTY(QString timeText READ timeText NOTIFY changed FINAL)
    Q_PROPERTY(QString sizeText READ sizeText NOTIFY changed FINAL)
    Q_PROPERTY(QString error READ error NOTIFY changed FINAL)
    Q_PROPERTY(QString folder READ folder NOTIFY changed FINAL)
    Q_PROPERTY(QString fileName READ fileName NOTIFY changed FINAL)
    Q_PROPERTY(QString dimensions READ dimensions NOTIFY changed FINAL)
    Q_PROPERTY(bool fullScreen READ fullScreen WRITE setFullScreen NOTIFY changed FINAL)
    Q_PROPERTY(bool systemAudio READ systemAudio WRITE setSystemAudio NOTIFY changed FINAL)
    Q_PROPERTY(bool microphone READ microphone WRITE setMicrophone NOTIFY changed FINAL)
    Q_PROPERTY(Quality quality READ quality WRITE setQuality NOTIFY changed FINAL)
    Q_PROPERTY(int frameRate READ frameRate WRITE setFrameRate NOTIFY changed FINAL)
    Q_PROPERTY(int savedFrameRate READ savedFrameRate NOTIFY changed FINAL)
    Q_PROPERTY(QString advice READ advice NOTIFY changed FINAL)
    Q_PROPERTY(Format format READ format WRITE setFormat NOTIFY changed FINAL)
    Q_PROPERTY(bool cursor READ cursor WRITE setCursor NOTIFY changed FINAL)
    Q_PROPERTY(bool countdown READ countdown WRITE setCountdown NOTIFY changed FINAL)
    Q_PROPERTY(bool clickHighlights READ clickHighlights WRITE setClickHighlights NOTIFY changed FINAL)
    Q_PROPERTY(bool cursorTrail READ cursorTrail WRITE setCursorTrail NOTIFY changed FINAL)
    Q_PROPERTY(KeyDisplay keys READ keys WRITE setKeys NOTIFY changed FINAL)
    Q_PROPERTY(bool mouseButtons READ mouseButtons WRITE setMouseButtons NOTIFY changed FINAL)

public:
    enum Stage { Ready, Counting, Starting, Recording, Paused, Saving, Done, Failed };
    Q_ENUM(Stage)
    // Persisted as 0 to 3. UltraQuality also doubles the video's size where
    // the region allows (recording::sharpScale).
    enum Quality { SmallFile, Standard, HighQuality, UltraQuality };
    Q_ENUM(Quality)
    enum Format { Mp4, Gif }; // persisted as 0, 1
    Q_ENUM(Format)
    // GitHub #98's keycaps; keycaps::Mode's order, persisted by its names.
    enum KeyDisplay { KeysOff, KeysShortcuts, KeysAll };
    Q_ENUM(KeyDisplay)

    struct Preferences {
        QString folder;
        bool systemAudio = false;
        bool microphone = false;
        Quality quality = Standard;
        bool cursor = true;
        bool countdown = true;
        bool clickHighlights = false;
        Format format = Mp4;
        KeyDisplay keys = KeysOff;
        bool mouseButtons = false;
        int frameRate = 30; // an MP4's: 15, 30 or 60; a GIF's goes with its quality
        bool cursorTrail = false; // recorded only with the pointer
    };
    struct Hooks {
        std::function<Preferences()> preferences;
        std::function<void(const Preferences&)> remember;
        std::function<void()> starting;
        std::function<void()> finished;
        std::function<bool(std::shared_ptr<recording::ClickStream>)> clicks;
        // Win顺's hotkeys that end a recording: never shown in it.
        std::function<std::vector<keycaps::Hotkey>()> ownKeys;
    };

    Recorder(QQmlEngine* engine, Hooks hooks, QObject* parent = nullptr);
    ~Recorder() override;
    void prepare(const QRect& region, HMONITOR monitor, double dpr);
    bool active() const;
    bool running() const { return m_control != nullptr; }
    Stage stage() const { return m_stage; }
    QString status() const;
    QString timeText() const;
    QString sizeText() const;
    QString error() const { return m_error; }
    // Where recordings go while no folder is chosen: WinShun in the user's Videos.
    static QString defaultFolder();
    QString folder() const;
    QString fileName() const;
    QString dimensions() const;
    QRect region() const { return m_region; } // physical pixels on the virtual desktop
    bool fullScreen() const { return m_fullScreen; }
    bool systemAudio() const { return m_preferences.systemAudio; }
    bool microphone() const { return m_preferences.microphone; }
    Quality quality() const { return m_preferences.quality; }
    int frameRate() const { return m_preferences.frameRate; }
    int savedFrameRate() const { return encoding().fps; } // a GIF's goes with its quality
    // After a recording that did not come out as chosen (at the region's own
    // size where twice was asked for, or with fewer frames a second): what
    // happened, and what to change. Empty otherwise.
    QString advice() const { return m_advice; }
    Format format() const { return m_preferences.format; }
    bool cursor() const { return m_preferences.cursor; }
    bool countdown() const { return m_preferences.countdown; }
    bool clickHighlights() const { return m_preferences.clickHighlights; }
    bool cursorTrail() const { return m_preferences.cursorTrail; }
    KeyDisplay keys() const { return m_preferences.keys; }
    bool mouseButtons() const { return m_preferences.mouseButtons; }
    void setSystemAudio(bool on);
    void setMicrophone(bool on);
    void setQuality(Quality value);
    void setFrameRate(int value);
    void setFormat(Format value);
    void setCursor(bool on);
    void setCountdown(bool on);
    void setClickHighlights(bool on);
    void setCursorTrail(bool on);
    void setKeys(KeyDisplay value);
    void setMouseButtons(bool on);
    void setFullScreen(bool on);

    Q_INVOKABLE void start();
    Q_INVOKABLE void skipCountdown();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void finish(); // also the screenshot hotkey while recording
    Q_INVOKABLE void discard(); // cancel in-progress recording without saving
    Q_INVOKABLE void close();
    Q_INVOKABLE void chooseFolder();
    Q_INVOKABLE void play();
    Q_INVOKABLE void copy();
    Q_INVOKABLE void reveal();
    Q_INVOKABLE void again(); // completed clips remain on disk

signals:
    void changed();

private:
    class Border;
    // What the choices come to for the region as it is.
    struct Encoding {
        int fps;
        int quality; // the encoder's, 0–100
        int scale; // the video's size against the region's
        int gifEdge; // a GIF's longest side at most
        QSize size; // of what is saved
    };
    Encoding encoding() const;
    void begin();
    // What was asked of an MP4's recording, for advice(): the scale, and its
    // frames a second where they fell short of the rate (0 where they did not).
    struct Outcome {
        int askedScale = 1;
        int achievedFps = 0;
    };
    void completed(const recording::Result& result, Outcome outcome = {});
    void remember();
    void createPanel();
    void keepPanelVisible();
    void updatePanelExclusion();
    void releaseClicks();
    void releaseKeys();

    QQmlEngine* m_engine;
    Hooks m_hooks;
    QPointer<QQuickWindow> m_panel;
    std::unique_ptr<Border> m_border;
    QRect m_region;
    QRect m_selectedRegion; // restored when leaving full-screen mode
    bool m_fullScreen = false;
    HMONITOR m_monitor = nullptr;
    double m_dpr = 1;
    Preferences m_preferences;
    Stage m_stage = Ready;
    int m_countdown = 0;
    QTimer m_timer;
    recording::Progress m_progress;
    QString m_error;
    QString m_advice;
    QString m_path;
    std::unique_ptr<recording::Control> m_control;
    std::jthread m_worker;
    bool m_clicksActive = false;
    std::unique_ptr<recording::KeyHook> m_keyHook; // while recording with keycaps
    bool m_exporting = false;
    bool m_discarded = false;
    bool m_compatibilityFallback = false;
};

} // namespace ws::capture
