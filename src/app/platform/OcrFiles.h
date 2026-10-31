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

} // namespace ws::ocr
