#pragma once

#include "ContentReport.h"
#include <functional>
#include <windows.h>

namespace ws::reportprint {
enum class Result { Printed, Cancelled, Failed };
// Worker thread only. The dialog is native; QML keeps its event loop.
Result print(const contentreport::Report& report, const std::function<bool()>& cancel, HWND owner = nullptr);
// A supplied printer DC also permits isolated Print-to-PDF verification.
Result render(HDC printer, const contentreport::Report& report, const wchar_t* output,
    const std::function<bool()>& cancel);
}
