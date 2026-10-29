#pragma once

#include "Keycaps.h"

#include <QFont>
#include <QRect>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

namespace ws::capture::recording {

class ClickStream;
class KeyStream;

// All times are 100 ns, as used by Media Foundation and WASAPI.
class Timeline {
public:
    explicit Timeline(qint64 start) : m_start(start) { }
    void pause(qint64 now);
    void resume(qint64 now);
    qint64 elapsed(qint64 now) const;
private:
    qint64 m_start;
    qint64 m_pause = -1;
    qint64 m_paused = 0;
};

struct Options {
    QRect region; // physical pixels, virtual desktop; wholly on one display
    QString path; // a new MP4 file; never overwrite an existing file
    int fps = 30;
    int quality = 75;
    bool systemAudio = false;
    bool microphone = false;
    bool cursor = true;
    bool clickHighlights = false;
    double pointerScale = 1;
    std::shared_ptr<ClickStream> clickInput;
    // GitHub #98: the keys and mouse buttons pressed, as keycap rows in a
    // corner (keys.mode Off and keys.mouse off: none). keyInput is their
    // hook's output (KeyStream.h).
    keycaps::Config keys;
    std::shared_ptr<KeyStream> keyInput;
    QFont keyFont; // Win顺's
    double outputScale = 1; // a GIF scaled down: its size against the region's
    bool software = false; // the probe's deterministic software-encoder path
};

struct Control {
    std::atomic<bool> paused {false};
    std::atomic<bool> stop {false};
    std::atomic<bool> discard {false};
};

struct Progress {
    qint64 elapsedMs = 0;
    qint64 bytes = 0;
    quint64 frames = 0;
    QString encoder;
    bool hardware = false;
    int percent = -1; // export progress; -1 while capturing or without a duration
};

struct Result : Progress {
    QString path;
    QString error;
};

// Runs on the caller's worker thread. It owns the capture session, D3D
// device, audio clients and encoder. The GUI only changes Control; neither
// the screenshot grabber nor the system clipboard is touched.
Result record(const Options& options, Control& control, const std::function<void(const Progress&)>& progress);

// Generated desktop-like pixels, through the same GPU conversion, MP4
// writer and timestamp handling. No desktop capture or audio devices. With
// keycaps on, "Ctrl + S" (or a click) at 100 ms. `control` pauses it.
Result sample(const Options& options, int milliseconds, const std::function<void(const Progress&)>& progress = {});
Result sample(const Options& options, int milliseconds, Control& control, const std::function<void(const Progress&)>& progress = {});

} // namespace ws::capture::recording
