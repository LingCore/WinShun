#include "ContentReportPrint.h"

#include <QCoreApplication>
#include <objbase.h>
#include <commdlg.h>
#include <algorithm>

namespace ws::reportprint {

Result render(HDC dc, const contentreport::Report& report, const wchar_t* output,
    const std::function<bool()>& cancel)
{
    if (!dc) return Result::Failed;
    const std::wstring title = report.title.toStdWString();
    DOCINFOW info {sizeof(info), title.c_str(), output, nullptr, 0};
    if (::StartDocW(dc, &info) <= 0) return Result::Failed;
    const int dpiX = ::GetDeviceCaps(dc, LOGPIXELSX);
    const int dpiY = ::GetDeviceCaps(dc, LOGPIXELSY);
    const int marginX = dpiX / 3;
    const int marginY = dpiY / 3;
    const int width = ::GetDeviceCaps(dc, HORZRES) - 2 * marginX;
    const int bottom = ::GetDeviceCaps(dc, VERTRES) - marginY;
    const HFONT font = ::CreateFontW(-::MulDiv(10, dpiY, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    const auto previous = ::SelectObject(dc, font);
    ::SetBkMode(dc, TRANSPARENT);
    TEXTMETRICW metrics {};
    ::GetTextMetricsW(dc, &metrics);
    const int lineHeight = metrics.tmHeight + metrics.tmExternalLeading + dpiY / 40;
    bool failed = width <= 0 || lineHeight <= 0 || bottom <= marginY + lineHeight * 3;
    int y = bottom;
    bool pageStarted = false;
    const QString text = contentreport::text(report);
    for (const QStringView source : QStringView(text).split(u'\n')) {
        QString value = source.toString();
        value.replace(u'\t', QStringLiteral("    "));
        if (value.isEmpty()) value = QStringLiteral(" ");
        qsizetype at = 0;
        while (at < value.size() && !failed) {
            if (cancel && cancel()) {
                ::AbortDoc(dc);
                ::SelectObject(dc, previous);
                ::DeleteObject(font);
                return Result::Cancelled;
            }
            if (y + lineHeight > bottom) {
                if (pageStarted && ::EndPage(dc) <= 0) { failed = true; break; }
                if (::StartPage(dc) <= 0) { failed = true; break; }
                pageStarted = true;
                y = marginY;
                ::TextOutW(dc, marginX, y, title.c_str(), static_cast<int>(title.size()));
                y += 2 * lineHeight;
            }
            const int available = static_cast<int>(std::min<qsizetype>(value.size() - at, INT_MAX));
            int fits = 0;
            SIZE extent {};
            const auto* chars = reinterpret_cast<const wchar_t*>(value.utf16() + at);
            if (!::GetTextExtentExPointW(dc, chars, available, width, &fits, nullptr, &extent) || fits <= 0) {
                failed = true;
                break;
            }
            if (at + fits < value.size() && value[at + fits - 1].isHighSurrogate()) --fits;
            if (fits <= 0) { failed = true; break; }
            if (!::TextOutW(dc, marginX, y, chars, fits)) { failed = true; break; }
            at += fits;
            y += lineHeight;
        }
        if (failed) break;
    }
    if (!failed && pageStarted) failed = ::EndPage(dc) <= 0;
    if (failed) ::AbortDoc(dc); else failed = ::EndDoc(dc) <= 0;
    ::SelectObject(dc, previous);
    ::DeleteObject(font);
    return failed ? Result::Failed : Result::Printed;
}

Result print(const contentreport::Report& report, const std::function<bool()>& cancel, HWND owner)
{
    PRINTDLGEXW dialog {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner ? owner : ::GetDesktopWindow();
    dialog.Flags = PD_RETURNDC | PD_NOSELECTION | PD_NOPAGENUMS | PD_NOCURRENTPAGE | PD_USEDEVMODECOPIESANDCOLLATE;
    dialog.nStartPage = START_PAGE_GENERAL;
    const HRESULT result = ::PrintDlgExW(&dialog);
    Result printed = Result::Cancelled;
    if (FAILED(result)) printed = Result::Failed;
    else if (dialog.dwResultAction == PD_RESULT_PRINT)
        printed = render(dialog.hDC, report, nullptr, cancel);
    if (dialog.hDC) ::DeleteDC(dialog.hDC);
    if (dialog.hDevMode) ::GlobalFree(dialog.hDevMode);
    if (dialog.hDevNames) ::GlobalFree(dialog.hDevNames);
    return printed;
}
} // namespace ws::reportprint
