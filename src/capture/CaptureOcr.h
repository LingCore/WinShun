#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include "SmartRedact.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

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
    // The words read as a table (ocrTable), tab between cells, a line a row,
    // as Excel and WPS paste them into cells; empty when they are not one.
    Q_PROPERTY(QString table READ table NOTIFY changed FINAL)
    Q_PROPERTY(bool needsPackage READ needsPackage NOTIFY changed FINAL)
    // 翻译 (GitHub #191): the words' translation, under them in the same
    // window. With a signal of its own: changed() puts the words read back
    // into the field, over what the user edited there.
    Q_PROPERTY(bool translating READ translating NOTIFY translationChanged FINAL)
    Q_PROPERTY(QString translation READ translation NOTIFY translationChanged FINAL)
    Q_PROPERTY(QString translationNote READ translationNote NOTIFY translationChanged FINAL) // none: why
    // What is to be set up before there can be one: "service" (the translate
    // plugin has no service or key), "plugin" (it is not installed or off); else empty.
    Q_PROPERTY(QString translationNeeds READ translationNeeds NOTIFY translationChanged FINAL)
    Q_PROPERTY(bool translationShown READ translationShown NOTIFY translationChanged FINAL)

public:
    explicit CaptureOcr(QObject* parent = nullptr);
    ~CaptureOcr() override;

    // Checks the shared component first, then a legacy/offline copy next to
    // WinShun.exe (setProgramFolder). Nothing is loaded by this check.
    static bool available();
    static void setProgramFolder(const QString& folder) { s_programFolder = folder; }
    static QString programFolder();

    // Reads `image`, taken on a monitor at `dpr` (its text's size);
    // `thenTranslate`: and asks for the words' translation once they are read.
    void read(const QImage& image, double dpr, bool thenTranslate = false);
    void retry(); // continue the original picture after an OCR package is installed
    void discardPending() { m_pendingImage = {}; }
    bool needsPackage() const { return !m_pendingImage.isNull(); }
    Q_INVOKABLE void setupRecognition() { emit setupRequested(); }

    // The lines of `image` with their corners and characters' places, in its
    // own pixels (SmartRedact.h); queued independently of smart redaction so
    // multiple pins can scan without dropping one another. `done` is called on
    // the GUI thread unless `cancelled` turns true first. Nothing the picture
    // says is kept or logged.
    void scan(const QImage& image, double dpr, std::function<void(QList<RedactLine>)> done,
        std::shared_ptr<const std::atomic<bool>> cancelled = {});
    // Crop and grayscale conversion run on the reader thread, too (single-slot
    // smart redaction: a newer scan() or cancelScan() drops the previous one).
    void scan(const QImage& image, const QRect& region, double dpr, std::function<void(RedactScan)> done);
    void cancelScan();

    // The window shows `texts` (one per line) under `title`, no reading: the QR
    // codes found in a selection; none found: `none` says so. A link is offered
    // as 打开链接 and never opened by itself.
    void showTexts(const QStringList& texts, const QString& title, const QString& none);
    // The window shows words read before (识别屏幕文字's 编辑, #198): as a reading's, no link offered.
    void showText(const QString& text);

    bool busy() const { return m_busy; }
    QString text() const { return m_text; }
    QString message() const { return m_message; }
    int round() const { return m_round; }
    QString title() const { return m_title; }
    QString link() const { return m_link; }
    QString table() const { return m_table; }
    // Cells with tabs, rows with line breaks; a tab or line break in a cell becomes a space.
    static QString tabbed(const QList<QStringList>& table);

    Q_INVOKABLE void copyAll(const QString& text) { emit copyRequested(text); }
    Q_INVOKABLE void copyTable() { if (!m_table.isEmpty()) emit copyRequested(m_table); }
    // The text (the selection, else all of it) as one line, for the search box.
    Q_INVOKABLE void search(const QString& text);
    // The same one line, for the default web search.
    Q_INVOKABLE void webSearch(const QString& text);
    // Asks for the translation of the text (the selection, else all of it),
    // its lines put together as paragraphs (ocrParagraphs), up to
    // kMaxTranslate characters: translateRequested() with a ticket, answered
    // by setTranslation(). A newer request, or a new picture, drops the answer
    // to an older one.
    Q_INVOKABLE void translate(const QString& text);
    static constexpr int kMaxTranslate = 3000;
    enum class Translated { Done, Failed, NoService, NoPlugin, PluginOff };
    // `text`: the translation (Done; empty: nothing to show, the part closes), or why not (Failed).
    void setTranslation(int ticket, Translated outcome, const QString& text);
    Q_INVOKABLE void setupTranslation() { emit translationSetupRequested(); }

    bool translating() const { return m_translating; }
    QString translation() const { return m_translation; }
    QString translationNote() const { return m_translationNote; }
    QString translationNeeds() const { return m_translationNeeds; }
    bool translationShown() const { return m_translating || !m_translation.isEmpty() || !m_translationNote.isEmpty(); }
    // Lines as words, trimmed, cut at what a search box takes; empty when no words.
    static QString oneLine(const QString& text);
    // Joins soft-wrapped OCR lines into readable paragraphs while preserving
    // sentence-ending breaks, blank-line paragraph gaps, and list items.
    Q_INVOKABLE static QString mergeLines(const QString& text);
    Q_INVOKABLE void close() { emit closeRequested(); }
    Q_INVOKABLE void openLink(const QString& url) { emit openLinkRequested(url); }

signals:
    void changed();
    void copyRequested(const QString& text);
    void searchRequested(const QString& query);
    void webSearchRequested(const QString& query);
    void translationChanged();
    void translateRequested(const QString& text, int ticket);
    void translationSetupRequested();
    void closeRequested();
    void openLinkRequested(const QString& url);
    void setupRequested();

private:
    static inline QString s_programFolder;
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
    struct LineJob {
        int ticket = 0;
        QImage image;
        double dpr = 1.0;
        std::shared_ptr<const std::atomic<bool>> cancelled;
    };
    struct LineCallback {
        std::function<void(QList<RedactLine>)> done;
        std::shared_ptr<const std::atomic<bool>> cancelled;
    };
    void enqueueScan(const QImage& image, const QRect& region, double dpr, bool grayscale, std::function<void(RedactScan)> done);
    void work(const std::stop_token& stop);
    void done(int round, const QString& text, const QString& table, const QString& error);
    void clearTranslation();
    void scanDone(int ticket, RedactScan scan);
    void lineScanDone(int ticket, QList<RedactLine> lines);

    bool m_busy = false;
    QString m_text;
    QString m_message;
    QString m_title;
    QString m_link;
    QString m_table;
    int m_round = 0;
    QImage m_pendingImage;
    double m_pendingDpr = 1;
    bool m_thenTranslate = false;
    bool m_translating = false;
    int m_translateTicket = 0;
    QString m_translation;
    QString m_translationNote;
    QString m_translationNeeds;

    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::optional<Job> m_job;
    std::optional<ScanJob> m_scan;
    std::deque<LineJob> m_lineQueue;
    int m_scanTicket = 0; // the GUI thread's: the scan whose result is wanted
    std::function<void(RedactScan)> m_scanDone;
    int m_lineTicket = 0;
    std::unordered_map<int, LineCallback> m_lineDone;
    std::jthread m_thread; // last: started once the rest is there
};

} // namespace ws::capture
