#pragma once

#include <QDir>
#include <QStringList>

namespace ws::preview {

// WinShun.exe --preview-results <folder> --clipboard-scroll [--clips 1000]
//   [--large-pictures] [--size compact|standard|roomy] [--label name] [--qml-debug]
// Scrolls the production clipboard list (ClipboardWindow.qml, on the screen,
// without focus) over an isolated history like a long real one, with wheel
// events sent to the window, and writes <folder>\clipscroll-<label>.json:
// frame times while moving, missed vsyncs, picture rows shown without their
// thumbnail, and the model's data() cost per row. --qml-debug lets
// qmlprofiler attach. Nothing touches the user's profile or clipboard.
int runClipScrollBench(const QDir& output, const QStringList& args);

} // namespace ws::preview
