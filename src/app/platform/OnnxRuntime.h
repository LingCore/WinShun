#pragma once

// ONNX Runtime for Win顺's models: the words in pictures (PaddleOcr) and
// formulas (FormulaReader). onnxruntime.dll comes with the OCR package
// (OcrFiles: ocr::folder) and is loaded by its full path when a model is
// first wanted, as Windows has an older one in System32; its API is taken
// from it rather than linked, for every file that includes this one.
#ifndef ORT_API_MANUAL_INIT
#define ORT_API_MANUAL_INIT
#endif
#include <onnxruntime_cxx_api.h>

#include <QString>

namespace ws::onnx {

// onnxruntime.dll from `folder`, once a process: true when it is loaded,
// whichever folder it came from; else false and why in `error`, as long as
// the process runs.
bool load(const QString& folder, QString* error = nullptr);

// The environment every model is run in: one set of `threads` threads for
// them all, below normal priority and waiting rather than spinning. ONNX
// Runtime has one a process: while one is there, another is the same one,
// on the threads it was made with.
Ort::Env environment(int threads);

// How every model is run: on the environment's threads, its memory given
// back after each run.
Ort::SessionOptions sessionOptions();

// The model in `file` (a full path); throws Ort::Exception when it is not one.
Ort::Session session(const Ort::Env& env, const QString& file, const Ort::SessionOptions& options);

} // namespace ws::onnx
