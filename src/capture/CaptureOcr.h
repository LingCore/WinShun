#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include "SmartRedact.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace ws::capture {

// The words in a capture or a pin ("识别文字"), for the OCR window
// (OcrPanel.qml): read by PaddleOcr (PP-OCRv6 tiny, the clipboard's reader)
// on a thread of its own below normal priority, the window up at once
// saying so meanwhile. The reader stays loaded for a minute after the last
// picture (loading it takes ~0.1 s, keeping it ~40 MB), then goes.
//
// Text captured at 100% or 125% is made twice as large first (ocrUpscale);
// PaddleOcr itself pads thin strips and shrinks very wide pictures.
//
// One window, one picture at a time: a new one replaces what is shown and
// a result for an older one is dropped.
//
// The same thread and the same reader also do smart redaction's scans
// (scan()): the lines of a picture with each character's place, for the
// capture and the editing window. A scan waits for no window and a window
// for no scan, except that the one running finishes first.
class CaptureOcr : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")

    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    Q_PROPERTY(QString text READ text NOTIFY changed FINAL) // the lines read, one per line
    Q_PROPERTY(QString message READ message NOTIFY changed FINAL) // none found, failed: why
    Q_PROPERTY(int round READ round NOTIFY changed FINAL) // counts up with each picture: the field takes the new text
    // The window's title when the text is not read from a picture's words (二维码); empty: 识别文字.
    Q_PROPERTY(QString title READ title NOTIFY changed FINAL)
    // The text as a web address (http or https) to offer 打开链接 for; empty when it is not one.
    Q_PROPERTY(QString link READ link NOTIFY changed FINAL)

public:
    explicit CaptureOcr(QObject* parent = nullptr);
    ~CaptureOcr() override;

    // The models are next to the program (cheap; nothing is loaded).
    static bool available();

    // Reads `image`, taken on a monitor at `dpr` (its text's size).
    void read(const QImage& image, double dpr);

    // The lines of `image` with their corners and characters' places, in its
    // own pixels (SmartRedact.h); `done` is called on the GUI thread, once,
    // unless a newer scan() or cancelScan() came first. Nothing the picture
    // says is kept or logged.
    void scan(const QImage& image, double dpr, std::function<void(QList<RedactLine>)> done);
    // Crop and grayscale conversion run on the reader thread, too.
    void scan(const QImage& image, const QRect& region, double dpr, std::function<void(RedactScan)> done);
    void cancelScan();

    // The window shows `texts` (one per line) under `title`, no reading: the QR
    // codes found in a selection; none found: `none` says so. A link is offered
    // as 打开链接 and never opened by itself.
    void showTexts(const QStringList& texts, const QString& title, const QString& none);

    bool busy() const { return m_busy; }
    QString text() const { return m_text; }
    QString message() const { return m_message; }
    int round() const { return m_round; }
    QString title() const { return m_title; }
    QString link() const { return m_link; }

    Q_INVOKABLE void copyAll(const QString& text) { emit copyRequested(text); }
    // The text (the selection, else all of it) as one line, for the search box.
    Q_INVOKABLE void search(const QString& text);
    // The same one line, for the default web search.
    Q_INVOKABLE void webSearch(const QString& text);
    // Lines as words, trimmed, cut at what a search box takes; empty when no words.
    static QString oneLine(const QString& text);
    Q_INVOKABLE void close() { emit closeRequested(); }
    Q_INVOKABLE void openLink(const QString& url) { emit openLinkRequested(url); }

signals:
    void changed();
    void copyRequested(const QString& text);
    void searchRequested(const QString& query);
    void webSearchRequested(const QString& query);
    void closeRequested();
    void openLinkRequested(const QString& url);

private:
    struct Job {
        int round = 0;
        QImage image;
        double scale = 1.0;
    };
    struct ScanJob {
        int ticket = 0;
        QImage image;
        QRect region;
        double dpr = 1.0;
        bool grayscale = true;
    };
    void enqueueScan(const QImage& image, const QRect& region, double dpr, bool grayscale, std::function<void(RedactScan)> done);
    void work(const std::stop_token& stop);
    void done(int round, const QString& text, const QString& error);
    void scanDone(int ticket, RedactScan scan);

    bool m_busy = false;
    QString m_text;
    QString m_message;
    QString m_title;
    QString m_link;
    int m_round = 0;

    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::optional<Job> m_job;
    std::optional<ScanJob> m_scan;
    int m_scanTicket = 0; // the GUI thread's: the scan whose result is wanted
    std::function<void(RedactScan)> m_scanDone;
    std::jthread m_thread; // last: started once the rest is there
};

} // namespace ws::capture
