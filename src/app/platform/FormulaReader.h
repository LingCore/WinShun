#pragma once

#include <QImage>
#include <QString>

#include <atomic>
#include <memory>
#include <optional>

namespace ws {

// 公式识别: the LaTeX of the formula in a picture, read by Pix2Text's MFR 1.5
// (breezedeus/pix2text-mfr-1.5, MIT: a TrOCR, whose encoder looks at the
// picture as 384 x 384 and whose decoder writes LaTeX a token at a time) on
// the OCR package's ONNX Runtime (onnx::load). The model is a component of
// its own (ocr::formulaModel).
//
// The picture is first made what the model learned from: light on dark
// turned dark on light, cut to what is in it with a third of its height
// around, then made 384 x 384 as Pillow's bicubic resampling does it. Read
// so, 405 of 480 pictures of formulas (40 formulas in 3 fonts, 2 sizes,
// light and dark, as on a screen) came out as written, 391 as they were.
//
// One object reads one picture at a time; its threads run below normal
// priority. Letting it go gives back its memory.
class FormulaReader {
public:
    struct Reading {
        QString latex; // as the model writes it (Latex::Source::Model); empty: nothing in the picture
        double score = 0; // 0…1, how sure it is: the geometric mean of each token's probability
        bool cut = false; // it went round in circles or on too long and was stopped
    };

    // A reading this sure (score) was right 402 times in 472, one less sure 3
    // times in 8 (the same 480 pictures): that one is to be checked.
    static constexpr double kSure = 0.9;

    ~FormulaReader();
    // The model in `modelFolder` run by onnxruntime.dll in `runtimeFolder`.
    // Null when something is missing or will not load; why in `error`.
    static std::unique_ptr<FormulaReader> load(
        const QString& runtimeFolder, const QString& modelFolder, int threads, QString* error = nullptr);

    // Nothing when the reading failed or `cancelled` turned true; why in `error`.
    std::optional<Reading> read(const QImage& picture, QString* error = nullptr, const std::atomic_bool* cancelled = nullptr);

    // The picture as the model sees it, 384 x 384 RGB888; null when nothing stands out from its ground.
    static QImage prepare(const QImage& picture);

private:
    FormulaReader();
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace ws
