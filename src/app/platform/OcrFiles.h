#pragma once

#include <QString>

namespace ws::ocr {

inline constexpr auto kVersion = u"ppocr-v6-ort1.30.0-x64";

// Shared by the clipboard and screenshot plugin, including portable profiles.
// Set before starting readers. No network or DLL loading in these functions.
void setComponentsRoot(const QString& folder);
QString componentsRoot();
QString installedFolder();
QString folder(const QString& programFolder); // installed component, then a legacy bundled copy
bool hasFiles(const QString& folder);

// The larger reading model (PP-OCRv6 small, 21 MB), a component of its own
// that the user may add to the one above: it also reads Japanese (the tiny
// one has no kana at all), traditional Chinese and 15 565 Han characters to
// the tiny one's 6 174, at about twice the time a picture. Installed is in
// use: every reader (clipboard, screenshots, OCR search) loads it instead of
// the tiny reading model; the model that finds the lines stays.
inline constexpr auto kPreciseVersion = u"ppocr-v6-rec-small";
inline constexpr auto kPreciseModel = u"PP-OCRv6_rec_small.onnx";
QString preciseFolder(); // where it is installed, there or not
QString preciseModel(); // the model's file when it is installed, else empty

// A reading model of the user's own choosing (another PP-OCR recognition
// model as ONNX: a larger one, another language's), copied here by the
// settings after a trial reading. Before the larger and the tiny one for
// every reader. Its characters are in the model (RapidOCR's files), or in
// the dictionary copied with it (PaddleOCR's own exports: one a line).
inline constexpr auto kOwnVersion = u"ocr-own-rec";
struct OwnModel {
    QString file; // empty: none
    QString dictionary; // empty: in the model
    QString name; // the file's name where the user had it, to show
    QString id; // differs with the file: what read a picture (TextRecognizer::engine)
};
QString ownFolder();
OwnModel ownModel();

// The formula reader's model (FormulaReader: Pix2Text's MFR 1.5, ~120 MB), a
// component of its own run by the OCR package's ONNX Runtime: these files in
// formulaFolder(), with a receipt once they are all there and checked.
inline constexpr auto kFormulaVersion = u"formula-mfr-1.5";
inline constexpr const char16_t* kFormulaFiles[] = {u"encoder_model.onnx", u"decoder_model.onnx", u"tokenizer.json"};
QString formulaFolder(); // where it is installed, there or not
QString formulaModel(); // that folder when the model is installed, else empty

} // namespace ws::ocr
