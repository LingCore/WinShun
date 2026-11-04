#pragma once

#include <QImage>
#include <QList>
#include <QPair>
#include <QPolygonF>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace ws {

// The words in a picture, read by PP-OCRv6 tiny (PaddleOCR: a model that
// finds the lines of text, one that reads each line; Chinese, English and
// the Latin-script languages) run by ONNX Runtime on the processor. Much
// better on screenshots than Windows' own recognizer: small UI text, digits
// with decimal points, mixed Chinese and English.
//
// The files: the shared, verified OCR component, or the legacy/offline
// onnxruntime.dll next to the program with its two models in ocr\.
// Loaded by their full paths (an older onnxruntime.dll is in System32).
// One object reads one picture at a time; its threads run below normal
// priority. Letting it go gives back its memory (~40 MB while reading).
// One line of text the reader found, where it stands, and where each
// character is along it (smart redaction cuts a match out of a line by them).
struct OcrPiece {
    QPolygonF quad; // the line's four corners in the picture: top left, top right, bottom right, bottom left
    QString text;
    float score = 0;
    // Each character read: its index in `text` (UTF-16) and where its middle
    // is along the line, 0 (left end) to 1. Empty for a line standing up,
    // and for a character the reading model gave no place.
    QList<QPair<int, float>> chars;
};

// What a read can be asked for beyond the plain reading. The clipboard and
// screenshots read with the defaults; OCR search indexes whole folders.
struct OcrOptions {
    // Called on the reading thread after each strip of the picture searched
    // for lines and after each batch of lines read: proof that a long read is
    // still moving, for a watchdog that gives up on reads that stopped. It
    // may throw to abandon the read; the read then fails with its message.
    std::function<void()> progress;
    // Lines that read poorly are read once more turned half round, and that
    // reading is kept when it says clearly more: pages scanned or photographed
    // upside down, and those turned a quarter the other way than lines that
    // stand up are turned to be read. Costs a second reading of poor pages.
    bool retryUpsideDown = false;
};

class PaddleOcr {
public:
    // A larger picture is made smaller all over before it is read; OCR search
    // decodes long pictures and renders PDF pages up to this, not beyond.
    static constexpr qint64 kMaxPixels = 40'000'000;

    // The files are there (cheap: what to use, decided before loading).
    static bool installed(const QString& programFolder);
    // Null when something is missing or will not load; why in `error`.
    static std::unique_ptr<PaddleOcr> load(const QString& programFolder, int threads, QString* error = nullptr);
    ~PaddleOcr();

    // One string per line of text, top to bottom: the pieces found side by
    // side put together, with a space where there is a gap or Latin writing.
    // False when the recognition itself failed (`error`); a picture without
    // words is true with no lines.
    bool read(const QImage& image, QStringList* lines, QString* error = nullptr);
    // The lines as found, each with its corners and its characters' places,
    // in the picture's own pixels; the same reading as read(), in no order.
    bool readPieces(
        const QImage& image, QList<OcrPiece>* pieces, QString* error = nullptr, const OcrOptions& options = {});

private:
    PaddleOcr();
    bool find(const QImage& image, QList<OcrPiece>* found, bool chars, QString* error, const OcrOptions& options);
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace ws
