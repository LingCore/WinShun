#include "PinImage.h"

#include "ClipImage.h"
#include "ImageOrigin.h"
#include "PictureFile.h"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainter>
#include <QScopeGuard>
#include <QStringList>
#include <QTextLayout>

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::captureplugin::pinimage {

namespace {

// The pixels a pin's picture keeps: the long capture's own limit
// (ScrollStitcher::Limits), so what it stitches pins at its own resolution.
constexpr qint64 kPinSourcePixels = 60'000'000;

// The first frame's size as stored, from its header only. Windows Imaging
// Component, as decodePictureFile: dist has no Qt image plugins, so
// QImageReader knows PNG and BMP sizes but not a JPEG's.
QSize storedSize(const QString& path)
{
    QString native = path;
    native.replace(u'/', u'\\');
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    UINT width = 0;
    UINT height = 0;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))
        || FAILED(wic->CreateDecoderFromFilename(
            native.toStdWString().c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))
        || FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height))
        || width > INT_MAX || height > INT_MAX)
        return {};
    return QSize(static_cast<int>(width), static_cast<int>(height));
}

QByteArray clipboardBytes(UINT format, SIZE_T limit)
{
    const HANDLE handle = ::GetClipboardData(format);
    if (!handle)
        return {};
    const SIZE_T size = ::GlobalSize(handle);
    if (!size || size > limit)
        return {};
    const void* data = ::GlobalLock(handle);
    if (!data)
        return {};
    const QByteArray bytes(static_cast<const char*>(data), static_cast<qsizetype>(size));
    ::GlobalUnlock(handle);
    return bytes;
}

// CF_UNICODETEXT up to its end or `limit` units, never half a pair.
QString clipboardText(qsizetype limit)
{
    const HANDLE handle = ::GetClipboardData(CF_UNICODETEXT);
    if (!handle)
        return {};
    const SIZE_T units = ::GlobalSize(handle) / sizeof(wchar_t);
    const auto* data = static_cast<const wchar_t*>(::GlobalLock(handle));
    if (!data)
        return {};
    qsizetype length = 0;
    while (static_cast<SIZE_T>(length) < units && length < limit && data[length])
        ++length;
    QString text = QString::fromWCharArray(data, length);
    ::GlobalUnlock(handle);
    if (!text.isEmpty() && text.back().isHighSurrogate())
        text.chop(1);
    return text;
}

// The card of renderText, in logical pixels.
constexpr qsizetype kCardChars = 2000;
constexpr int kCardLines = 50;
constexpr qreal kCardWidth = 560; // where a line wraps
constexpr qreal kCardPadX = 14;
constexpr qreal kCardPadY = 10;
constexpr qreal kCardFontPx = 14;
constexpr qreal kCardLineGap = 5; // between lines, for reading

// Two or more lines indented with a tab or spaces: code (or a table), whose
// columns line up only in a monospaced font.
bool indentedLines(const QStringList& lines)
{
    return std::ranges::count_if(lines, [](const QString& line) {
        return line.startsWith(u'\t') || (line.startsWith(u"  ") && !line.trimmed().isEmpty());
    }) >= 2;
}

QColor over(const QColor& color, const QColor& base)
{
    const qreal a = color.alphaF();
    return QColor::fromRgbF(color.redF() * a + base.redF() * (1 - a), color.greenF() * a + base.greenF() * (1 - a),
        color.blueF() * a + base.blueF() * (1 - a));
}

} // namespace

std::optional<Placement> cursorPlacement()
{
    POINT cursor {};
    if (!::GetCursorPos(&cursor))
        return std::nullopt;
    const HMONITOR monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info {sizeof info};
    if (!::GetMonitorInfoW(monitor, &info))
        return std::nullopt;
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (FAILED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        dpiX = 96;
    return Placement {{cursor.x, cursor.y},
        QRect(info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left, info.rcWork.bottom - info.rcWork.top),
        static_cast<int>(dpiX)};
}

QImage renderText(const QString& text, int dpi, const TextLook& look)
{
    return renderTextWithLines(text, dpi, look).image;
}

RenderedText renderTextWithLines(const QString& text, int dpi, const TextLook& look)
{
    QString shown = text.left(kCardChars + 1);
    shown.replace(u"\r\n"_s, u"\n"_s);
    shown.replace(u'\r', u'\n');
    bool more = shown.size() > kCardChars || text.size() > kCardChars + 1;
    if (shown.size() > kCardChars) {
        shown.truncate(kCardChars);
        if (shown.back().isHighSurrogate())
            shown.chop(1);
    }
    while (shown.startsWith(u'\n')) // blank lines above and below show nothing
        shown.remove(0, 1);
    while (!shown.isEmpty() && shown.back().isSpace())
        shown.chop(1);
    if (shown.trimmed().isEmpty())
        return {};
    QStringList lines = shown.split(u'\n');
    if (lines.size() > kCardLines) {
        lines.resize(kCardLines);
        more = true;
    }
    const bool code = indentedLines(lines);
    for (QString& line : lines)
        line.replace(u'\t', u"    "_s);

    const qreal scale = std::max(dpi, 96) / 96.0;
    QFont font;
    const QString family = look.font.isEmpty() ? QGuiApplication::font().family() : look.font;
    font.setFamilies(code ? QStringList {u"Cascadia Mono"_s, u"Consolas"_s, family} : QStringList {family});
    font.setPixelSize(static_cast<int>(std::lround(kCardFontPx * scale)));
    const QFontMetricsF metrics(font);
    const qreal lineHeight = std::ceil(metrics.height() + kCardLineGap * scale);
    const qreal width = std::round(kCardWidth * scale);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    std::vector<std::unique_ptr<QTextLayout>> layouts;
    const qreal most = kCardLines * lineHeight; // of the rows, wrapped ones too
    qreal y = 0;
    qreal widest = 0;
    for (const QString& line : lines) {
        if (y >= most) {
            more = true;
            break;
        }
        auto layout = std::make_unique<QTextLayout>(line, font);
        layout->setTextOption(option);
        layout->beginLayout();
        for (QTextLine row = layout->createLine(); row.isValid(); row = layout->createLine()) {
            row.setLineWidth(width);
            row.setPosition(QPointF(0, y + (lineHeight - row.height()) / 2));
            if (y < most)
                widest = std::max(widest, row.naturalTextWidth());
            y += lineHeight;
        }
        layout->endLayout();
        if (layout->lineCount() == 0) // a blank line
            y += lineHeight;
        layouts.push_back(std::move(layout));
    }
    if (y > most) { // the rest is cut off below (the clip)
        y = most;
        more = true;
    }
    const qreal ellipsis = more ? lineHeight : 0;
    const int padX = static_cast<int>(std::lround(kCardPadX * scale));
    const int padY = static_cast<int>(std::lround(kCardPadY * scale));
    const QSize size(static_cast<int>(std::ceil(std::max(widest, metrics.horizontalAdvance(u"…"_s)))) + 2 * padX,
        static_cast<int>(std::ceil(y + ellipsis)) + 2 * padY);
    const QColor base = look.dark ? QColor(0x2b, 0x2b, 0x2b) : QColor(0xfb, 0xfb, 0xfb);
    QImage image(size, QImage::Format_RGB32);
    image.fill(look.background.isValid() ? over(look.background, base) : base);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setPen(look.text.isValid() ? look.text : (look.dark ? QColor(0xf0, 0xf0, 0xf0) : QColor(0x1a, 0x1a, 0x1a)));
    painter.setClipRect(QRectF(0, padY, size.width(), y));
    QList<capture::RedactLine> textLines;
    for (const auto& layout : layouts) {
        layout->draw(&painter, QPointF(padX, padY));
        const QString lineText = layout->text();
        for (int r = 0; r < layout->lineCount(); ++r) {
            const QTextLine row = layout->lineAt(r);
            if (row.y() >= y)
                break;
            const int rawStart = row.textStart();
            int first = 0;
            int end = row.textLength();
            while (first < end && lineText[rawStart + first].isSpace())
                ++first;
            while (end > first && lineText[rawStart + end - 1].isSpace())
                --end;
            if (first >= end)
                continue;
            const QString visible = lineText.mid(rawStart + first, end - first);
            const qreal x0 = row.cursorToX(rawStart + first);
            const qreal x1 = row.cursorToX(rawStart + end);
            const qreal left = std::min(x0, x1);
            const qreal right = std::max(x0, x1);
            if (right <= left || row.height() <= 0)
                continue;
            const QRectF box(padX + left, padY + row.y(), right - left, row.height());
            capture::RedactLine rl;
            rl.text = visible;
            rl.quad = {box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft()};
            for (int c = 0; c < visible.size(); ++c) {
                if (visible[c].isLowSurrogate())
                    continue;
                const int next = (c + 1 < visible.size() && visible[c].isHighSurrogate() && visible[c + 1].isLowSurrogate()) ? c + 2 : c + 1;
                const qreal c0 = row.cursorToX(rawStart + first + c);
                const qreal c1 = row.cursorToX(rawStart + first + next);
                const float t = static_cast<float>(std::clamp(((c0 + c1) * 0.5 - left) / (right - left), 0.0, 1.0));
                rl.chars.append({c, t});
            }
            textLines.append(std::move(rl));
        }
    }
    painter.setClipping(false);
    if (more) {
        painter.setPen(look.subtext.isValid() ? look.subtext : QColor(0x80, 0x80, 0x80));
        painter.setFont(font);
        painter.drawText(QRectF(padX, padY + y, size.width() - 2 * padX, lineHeight), Qt::AlignLeft | Qt::AlignVCenter, u"…"_s);
    }
    painter.end();
    return {std::move(image), std::move(textLines)};
}

Prepared prepare(QImage image, const Placement& placement)
{
    if (image.isNull() || placement.work.isEmpty())
        return {};
    image = image.convertToFormat(QImage::Format_RGB32);
    if (image.isNull())
        return {};
    // Its own pixels stay: the pin shows a picture larger than the screen
    // fitted (PinManager::pinFitted), so a zoom still has the detail. This
    // is where the whole picture would start, its middle at the cursor; the
    // pin fits the shown picture about that middle and keeps it in the area.
    const QPoint origin = placement.cursor - QPoint(image.width() / 2, image.height() / 2);
    return {std::move(image), origin, placement.cursor};
}

QImage readFile(const QString& path)
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    const auto uninit = qScopeGuard([com] {
        if (SUCCEEDED(com))
            ::CoUninitialize();
    });
    // Read at its own size, not the screen's: a long picture shown fitted and
    // zoomed in had nothing left to show. Only one past the pixels a pin is
    // worth keeping is read smaller, so it can still be read at all.
    int largest = 0;
    if (const QSize stored = storedSize(path);
        !stored.isEmpty() && qint64(stored.width()) * stored.height() > kPinSourcePixels) {
        const double scale = std::sqrt(static_cast<double>(kPinSourcePixels) / (qint64(stored.width()) * stored.height()));
        largest = static_cast<int>(std::lround((std::max)(stored.width(), stored.height()) * scale));
    }
    return decodePictureFile(path, largest);
}

ClipboardPicture readClipboard(const LastCapture& last)
{
    bool opened = false;
    for (int attempt = 0; attempt != 8 && !opened; ++attempt) {
        opened = ::OpenClipboard(nullptr) != FALSE;
        if (!opened)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!opened)
        return {ClipboardStatus::Busy, {}};
    auto close = qScopeGuard([] { ::CloseClipboard(); });
    const DWORD sequence = ::GetClipboardSequenceNumber();
    QByteArray origin = clipboardBytes(::RegisterClipboardFormatW(kImageOriginFormat), 128);
    if (const qsizetype end = origin.indexOf('\0'); end >= 0)
        origin.truncate(end);
    if (!last.id.isEmpty()
        && ((sequence != 0 && sequence == last.clipboardSequence) || origin == last.id.toUtf8()))
        return {ClipboardStatus::LastCapture, {}};

    // Keep the supported representations so a damaged rich offer can still
    // fall back to its bitmap. No decoding or disk access while holding the lock.
    QList<QByteArray> encoded;
    qsizetype totalBytes = 0;
    bool offeredImage = false;
    for (const ClipImageOffer& offer : clipImageOffers()) {
        const QString name = offer.name.toString();
        const UINT format = ::RegisterClipboardFormatW(qUtf16Printable(name));
        offeredImage = offeredImage || ::IsClipboardFormatAvailable(format);
        if (QByteArray bytes = clipboardBytes(format, static_cast<SIZE_T>(kClipMaxImageBytes - totalBytes)); !bytes.isEmpty()) {
            totalBytes += bytes.size();
            encoded.append(std::move(bytes));
        }
    }
    offeredImage = offeredImage || ::IsClipboardFormatAvailable(CF_DIBV5) || ::IsClipboardFormatAvailable(CF_DIB);
    const QByteArray dibV5 = clipboardBytes(CF_DIBV5, kClipMaxImageBytes);
    const QByteArray dib = clipboardBytes(CF_DIB, kClipMaxImageBytes);
    const QString text = clipboardText(kPinTextChars); // words, pinned when there is no picture (GitHub #388)
    QStringList files;
    if (const HDROP drop = static_cast<HDROP>(::GetClipboardData(CF_HDROP))) {
        const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
            if (!length || length > 32767)
                continue;
            std::wstring path(length + 1, L'\0');
            if (::DragQueryFileW(drop, i, path.data(), length + 1))
                files.append(QString::fromWCharArray(path.data(), static_cast<int>(length)));
        }
    }
    ::CloseClipboard();
    close.dismiss();

    for (QByteArray& bytes : encoded) {
        if (QImage image = decodeClipImage(std::move(bytes)).firstFrame; !image.isNull())
            return {ClipboardStatus::Image, std::move(image)};
    }
    if (QImage image = imageFromDib(dibV5); !image.isNull())
        return {ClipboardStatus::Image, std::move(image)};
    if (QImage image = imageFromDib(dib); !image.isNull())
        return {ClipboardStatus::Image, std::move(image)};
    for (const QString& file : files) {
        // Decoding, rather than a different codec's canRead(), chooses the
        // first readable file. The caller fits it to the saved work area.
        if (QImage image = readFile(file); !image.isNull())
            return {ClipboardStatus::Image, std::move(image)};
    }
    if (offeredImage)
        return {ClipboardStatus::Unreadable, {}};
    if (files.isEmpty() && !text.trimmed().isEmpty())
        return {ClipboardStatus::Text, {}, text};
    return {ClipboardStatus::Empty, {}};
}

} // namespace ws::captureplugin::pinimage
