#pragma once

#include "Recording.h"
#include "GifExport.h"

#include <QStringList>

namespace ws::capture::recording {

// Driver and Media Foundation calls cannot safely be interrupted on a
// thread. Supervise an owned process instead; never terminate the UI.
struct WorkerProgram {
    QString executable;
    QStringList arguments;
    int timeoutMs = 10'000;
};

Result recordIsolated(const WorkerProgram& program, const Options& options, Control& control,
    const std::function<void(const Progress&)>& progress, int syntheticMs = -1);
Result exportGifIsolated(const WorkerProgram& program, const QString& mp4, const GifOptions& options,
    Control& control, const std::function<void(const Progress&)>& progress);

// Called only by the executable's --recording-worker entry, after making
// QGuiApplication. stdin/stdout carry bounded JSON lines, stderr diagnostics.
int recordingWorkerMain();

} // namespace ws::capture::recording
