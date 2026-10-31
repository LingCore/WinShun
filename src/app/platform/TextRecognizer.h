#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

namespace ws {

// The words in pictures, for searching the clipboard history by them
// (GitHub #9). On a thread of its own below normal priority, one picture
// after another; the recognizer is let go when nothing is left to read.
//
// By PP-OCRv6 (PaddleOcr: Chinese, English, Latin-script languages), which
// comes with Win顺. Windows' own recognizer (Windows.Media.Ocr, part of a
// language's optional features) when the first East Asian language in the
// user's list is Japanese or Korean, or when PaddleOcr will not load: in
// that language if it has a recognizer, else in the user's languages as
// Windows picks. Its Chinese lines come back one word per character; the
// words are put together again without the spaces between them.
class TextRecognizer : public QObject {
    Q_OBJECT

public:
    explicit TextRecognizer(QObject* parent = nullptr);
    ~TextRecognizer() override; // what is queued is dropped

    // `done` later; `next`: before what is queued (just copied, the older
    // ones after).
    void read(qint64 id, const QString& imageFile, bool next = false);
    void cancel(); // forget queued work; the current image finishes, then the worker releases its models and exits
    bool running() const;
    bool refreshEngine(); // false: finish the current batch before switching engines

    // What reads them ("ppocr-v6-tiny", "windows"): pictures read by another
    // are read again. Can change once, when PaddleOcr does not load.
    QString engine() const;

signals:
    // From the thread: queued to the receiver. `ok` false: no recognizer
    // (no language with one installed) or recognition failed, to be tried
    // again another time. A file that cannot be loaded has no words.
    void done(qint64 id, const QString& words, bool ok);
    void idle();

private:
    struct Job {
        qint64 id = 0;
        QString file;
    };
    void work(const std::stop_token& stop);

    mutable std::mutex m_mutex;
    std::deque<Job> m_jobs;
    bool m_running = false; // guarded by m_mutex; no OCR thread until the first image
    std::atomic<bool> m_paddle; // else Windows'
    std::jthread m_thread; // last: started once the rest is there
};

} // namespace ws
