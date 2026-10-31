#pragma once

#include <QImage>
#include <QList>
#include <QPair>
#include <QPolygonF>
#include <QString>
#include <QStringList>

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

class PaddleOcr {
public:
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
    bool readPieces(const QImage& image, QList<OcrPiece>* pieces, QString* error = nullptr);

private:
    PaddleOcr();
    bool find(const QImage& image, QList<OcrPiece>* found, bool chars, QString* error);
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace ws
