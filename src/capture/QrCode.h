#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

namespace ws::capture {

// The QR codes in a picture (zxing-cpp, Apache-2.0, third_party/zxing-cpp):
// the text of each one found, in the order they are read (QR and Micro QR
// only; a screenshot's lines of text are not barcodes). Empty when there is
// none. Nothing the picture says is logged.
QStringList readQrCodes(const QImage& image);
// What a picture says for the search: the words read in it (`words`, lines
// of OCR, maybe none) and, each on a line after them, what its QR codes
// hold, unless the words have that already. A screenshot of a code alone
// said nothing, and its row in the history read 没有识别出文字 (GitHub #305).
QString withQrCodes(QString words, const QImage& image);

// The web address a decoded text is, to offer 打开链接: the whole text, trimmed,
// as an http or https URL with a host; empty for anything else. Never opened
// without the user's click.
QString qrLink(const QString& text);

} // namespace ws::capture
