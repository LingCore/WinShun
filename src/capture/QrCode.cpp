#include "QrCode.h"

#include <QUrl>

#include <ReadBarcode.h>

namespace ws::capture {

namespace {

QStringList readOnce(const QImage& grey, const ZXing::ReaderOptions& options)
{
    QStringList found;
    // Grayscale8 rows are padded to 4 bytes.
    const ZXing::ImageView view(grey.constBits(), grey.width(), grey.height(), ZXing::ImageFormat::Lum,
        static_cast<int>(grey.bytesPerLine()));
    for (const ZXing::Barcode& code : ZXing::ReadBarcodes(view, options)) {
        if (!code.isValid())
            continue;
        const std::string utf8 = code.text();
        const QString text = QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
        if (!text.isEmpty() && !found.contains(text))
            found.append(text);
    }
    return found;
}

} // namespace

QStringList readQrCodes(const QImage& image)
{
    if (image.isNull())
        return {};
    // Grey is all the reader looks at.
    const QImage grey = image.convertToFormat(QImage::Format_Grayscale8);
    ZXing::ReaderOptions options;
    options.setFormats(ZXing::BarcodeFormat::QRCode | ZXing::BarcodeFormat::MicroQRCode);
    options.setTryHarder(true);
    options.setTryRotate(true);
    options.setTryInvert(true);
    options.setMaxNumberOfSymbols(8);
    QStringList found = readOnce(grey, options);
    // A small code (QQ's round, dotted ones above all: GitHub #305) is often
    // read once it is drawn larger: of 142 real ones at a quarter of their
    // size, 111 were read as they were and 121 with this. Only for a picture
    // with nothing found, and never one that would grow past 12 megapixels.
    for (int factor = 2; found.isEmpty() && factor <= 3; ++factor) {
        const QSize size = grey.size() * factor;
        if (qint64(size.width()) * size.height() > 12'000'000)
            break;
        found = readOnce(grey.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), options);
    }
    return found;
}

QString withQrCodes(QString words, const QImage& image)
{
    for (const QString& found : readQrCodes(image)) {
        const QString code = found.trimmed();
        if (code.isEmpty() || words.contains(code))
            continue;
        if (!words.isEmpty())
            words += u'\n';
        words += code;
    }
    return words;
}

QString qrLink(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 2048)
        return {};
    for (const QChar c : trimmed) {
        if (c.isSpace() || c.unicode() < 0x20)
            return {};
    }
    const QUrl url(trimmed, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != u"http" && scheme != u"https") || url.host().isEmpty())
        return {};
    return trimmed;
}

} // namespace ws::capture
