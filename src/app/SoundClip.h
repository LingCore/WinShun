#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <windows.h>
// mmsystem.h after windows.h
#include <mmsystem.h>

#include <memory>
#include <thread>
#include <vector>

namespace ws {

// A short sound played from the UI, with how loud it is at each moment, so
// that something can move along with it (the 拾穗计划 avatar's easter egg).
// Decoded once with Media Foundation, in the background (prepare()), the
// silence around it cut off, then played through a wave-out stream that is
// open only while it plays. play() while it plays does nothing: presses in
// quick succession neither pile up sounds nor keep starting it over (which
// would only ever repeat its first moment). It writes nothing to the log,
// not even when it fails: it is a surprise, to be found, not read about.
class SoundClip : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged FINAL)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged FINAL)
    Q_PROPERTY(bool available READ available NOTIFY availableChanged FINAL)

public:
    explicit SoundClip(QObject* parent = nullptr);
    ~SoundClip() override;

    QString source() const { return m_source; }
    void setSource(const QString& source); // a resource (":/...") or a file
    bool playing() const { return m_device != nullptr; }
    // False once the sound could not be read or decoded (Windows N without
    // the Media Feature Pack has no Media Foundation).
    bool available() const { return !m_failed; }

    Q_INVOKABLE void prepare(); // decode now, in the background, so that play() starts at once
    Q_INVOKABLE void play(); // from the start, unless it is playing
    Q_INVOKABLE void stop();
    // How loud the sound is where playback is now: 0 to 1, its loudest
    // moment being 1. 0 when it is not playing.
    Q_INVOKABLE double level() const;

    struct Decoded {
        WAVEFORMATEX format {};
        QByteArray pcm; // 16-bit
        std::vector<float> levels; // loudness of every `step` frames, 0 to 1
        DWORD step = 1;
    };

signals:
    void sourceChanged();
    void playingChanged();
    void availableChanged();

private:
    void decoded(std::shared_ptr<const Decoded> clip);
    void start();

    QString m_source;
    std::shared_ptr<const Decoded> m_clip; // null until decoded
    bool m_failed = false;
    bool m_decoding = false;
    bool m_playWhenReady = false; // play() came while decoding

    HWAVEOUT m_device = nullptr; // open while it plays
    WAVEHDR m_header {};
    QTimer m_watch; // notices the end of the sound

    std::jthread m_decoder; // last: joined before the members above go
};

} // namespace ws
