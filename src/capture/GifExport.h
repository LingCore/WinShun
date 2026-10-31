#pragma once

#include "Recording.h"

namespace ws::capture::recording {

struct GifOptions {
    QString path; // new output file; committed only after every frame is written
    int fps = 15;
    int maximumEdge = 1280;
    QString temporaryDirectory; // an isolated supervisor's owned staging directory
};

QSize gifSize(QSize source, int maximumEdge);

// Streams the captured video through the Windows decoder and GIF encoder.
// Keeps at most two frames, preserves pause-adjusted timing, and never
// modifies the source. Runs on the recording worker, without extra programs.
Result exportGif(const QString& video, const GifOptions& options, Control& control,
    const std::function<void(const Progress&)>& progress = {});

} // namespace ws::capture::recording
