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

struct OcrTable; // OcrTable.h

// The words in a picture, read by PP-OCRv6 tiny (PaddleOCR: a model that
// finds the lines of text, one that reads each line; Chinese, English and
// the Latin-script languages) run by ONNX Runtime on the processor. With
// the larger reading model installed (ocr::preciseModel) Japanese and
// traditional Chinese too. Much
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

// The pieces as a table's rows of cells (top to bottom, left to right), when
// they stand as one: columns where the lines with pieces side by side leave
// a gap from top to bottom, at least two of them, and two such lines; else
// none. Screenshots' "复制为表格".
QList<QStringList> ocrTable(const QList<OcrPiece>& pieces);
// The pieces as lines of words, top to bottom, those side by side joined:
// what PaddleOcr::read gives for the picture they were found in.
QStringList ocrLines(const QList<OcrPiece>& pieces);

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
    // Reads with the larger model (ocr::preciseModel), not the tiny one.
    bool precise() const;
    // Reads with the user's own model (ocr::ownModel).
    bool own() const;
    // A file the user picked as their reading model: loaded as one and a
    // line read with it. False with why (not a model, no characters in it and
    // no dictionary given, not a PP-OCR recognition model) in `error`.
    static bool tryReadingModel(const QString& programFolder, const QString& file, const QString& dictionary, QString* error);

    // One string per line of text, top to bottom: the pieces found side by
    // side put together, with a space where there is a gap or Latin writing.
    // False when the recognition itself failed (`error`); a picture without
    // words is true with no lines.
    // `table`: the same lines as a table's rows of cells, when they stand as
    // one (ocrTable); else empty.
    bool read(const QImage& image, QStringList* lines, QString* error = nullptr, QList<QStringList>* table = nullptr);
    // The model that reads a table's rows, columns and merged cells from the
    // picture (SLANet_plus, PaddleOCR's; 7 MB, comes with the screenshot
    // plugin): its file, for every reader made in this process. Unset or
    // missing, readTable() works tables out from where the words stand alone.
    static void setTableModel(const QString& file);
    static bool hasTableModel();
    // read()'s lines, and the picture's table when it is one: its rows,
    // columns and merged cells from the lines drawn in it (a ruled table);
    // else as the table model sees them, where it is there and sure; else
    // ocrTable's rows. A line read across two cells is cut between them. No
    // table: `table` is empty. `larger`: the words are read with the picture
    // made this many times larger (a screenshot taken at 100%: twice), the
    // table's lines and cells are looked for in the picture as it is.
    bool readTable(const QImage& image, double larger, QStringList* lines, OcrTable* table, QString* error = nullptr);    // The lines as found, each with its corners and its characters' places,
    // in the picture's own pixels; the same reading as read(), in no order.
    bool readPieces(
        const QImage& image, QList<OcrPiece>* pieces, QString* error = nullptr, const OcrOptions& options = {});

    // Each step of every reading as it starts, for the OCR test a user runs
    // (tools/ocrtest): "detect 2000 x 1316", "recognize 6 lines, 48 x 7412".
    // Any thread; unset in the program, where nothing is made for it.
    using Trace = void (*)(const QString& step);
    static void setTrace(Trace trace);

private:
    PaddleOcr();
    struct Trial;
    static std::unique_ptr<PaddleOcr> load(const QString& programFolder, int threads, QString* error, const Trial* trial);
    bool find(const QImage& image, QList<OcrPiece>* found, bool chars, QString* error, const OcrOptions& options);
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace ws
