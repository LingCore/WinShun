#pragma once

#include "Keycaps.h"

#include <QFont>
#include <QImage>
#include <QRect>
#include <QString>

#include <array>
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

// The encoder's quality (0–100) for each picture quality the panel offers,
// smallest file first: 小体积, 标准, 高清, 原画. Measured on small text
// (tst_recording clarityByQuality): each step is closer to the screen in
// brightness, 46 to 55 dB. Colour is another matter: see sharpScale().
inline constexpr std::array<int, 4> kQualities {70, 80, 90, 100};

struct Options {
    QRect region; // physical pixels, virtual desktop; wholly on one display
    QString path; // a new MP4 file; never overwrite an existing file
    QString temporary; // supervisor-owned UUID staging path; empty for a direct call
    int fps = 30; // 1 to 60
    int quality = kQualities[1];
    int scale = 1; // 2: at twice the region's size, where sharpScale() allows
    bool systemAudio = false;
    bool microphone = false;
    bool cursor = true;
    bool clickHighlights = false;
    // A line behind the pointer that draws back into it when it stops
    // (CursorTrail). The trail goes under the pointer, so the capture leaves
    // the pointer out and the frames get it drawn as Windows shows it
    // (CursorArt). Only with `cursor`.
    bool cursorTrail = false;
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
    bool noVideoDevice = false; // tests: as on a driver without D3D11 video (GitHub #214)
    bool bitrateOnly = false; // tests: as with an encoder that takes no quality
    bool screenByGdi = false; // tests: the screen through GDI, as on Windows 10 1809
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
    QString stage; // last media operation, for timeout diagnostics
    QString screen; // how the screen is read: Windows.Graphics.Capture, or GDI and why
    QString rate; // what the encoder was given to aim for: "quality 90", or "bitrate 12000 kbps" where it takes no quality
    int scale = 1; // the video's size against the region's: 2 only where Options::scale asked and the encoder took it
    QString refused; // the ways to encode that were tried before this one, and why each was refused; for the log
    QString timing; // at the end, for the log: what a frame took to read from the screen and to encode, and how frames are waited for
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

// The still part of sample()'s picture: small dark, coloured and light-on-dark
// text, as on a desktop. The moving block covers rows height / 2 to
// height / 2 + 34; everywhere else a decoded frame can be held against this.
QImage sampleBackdrop(QSize size);

// 2 where a region can be recorded at twice its size, every pixel a 2×2 block;
// 1 where that is more than H.264 takes. H.264 keeps one colour for each 2×2
// block (4:2:0), which smears coloured and ClearType text at the region's own
// size; doubled, each pixel of the screen keeps its colour.
int sharpScale(QSize region, int fps);

// The frames a second a recording came to where the computer did not keep
// up: under nine tenths of the rate asked for, over two seconds or more.
// 0 where it kept up.
int fallenShortFps(quint64 frames, qint64 elapsedMs, int fps);

// The H.264 quantizer (0–51, lower is closer to the screen) that goes with an
// encoder quality: 25, 22, 19 and 16 for kQualities. NVIDIA's encoder works
// out the same one from the quality; it is given outright for an encoder
// that does not (see Writer).
int quantizer(int quality);

// Bits per second for an encoder that takes no quality (a vendor's that only
// implements bitrate-based VBR): more for a higher quality, so that the
// panel's choices still differ there. As much as Cap records at: 9, 19, 37
// and 62 Mbps for 1920 × 1080 at 30 frames a second.
int videoBitrate(QSize size, int fps, int quality);

// Full-range BGRA to studio-range BT.709 NV12, as the GPU's video processor
// makes it, for a device without D3D11 video (GitHub #214). Even width and
// height; each 2×2 block shares the chroma of its average.
void bgraToNv12(const unsigned char* bgra, int pitch, int width, int height, unsigned char* luma, int lumaStride,
    unsigned char* chroma, int chromaStride);

} // namespace ws::capture::recording
