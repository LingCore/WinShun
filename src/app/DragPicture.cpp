#include "DragPicture.h"

#include "FileIconProvider.h"
#include "SystemTheme.h"

#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <vector>

namespace ws {

namespace {

struct Item {
    QImage image;
    bool photo = false; // opaque, edge to edge: rounded and edged, as a print
};

bool opaque(const QImage& image)
{
    if (!image.hasAlphaChannel())
        return true;
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) < 255)
                return false;
        }
    }
    return true;
}

// The item at most `px` on its longer side, never enlarged (a small picture
// stays crisp, as it is).
Item picture(const QString& path, int px)
{
    const QString native = QDir::toNativeSeparators(path);
    // A picture file of a sensible size, read at the size it is shown (a
    // JPEG decodes small, fast): Windows' cache often holds only the list's
    // small thumbnail, which would show a stamp.
    QImage image;
    QImageReader reader(native);
    const QSize size = reader.canRead() ? reader.size() : QSize();
    if (size.isValid() && QFileInfo(native).size() < 24 * 1024 * 1024) {
        if (size.width() > px || size.height() > px)
            reader.setScaledSize(size.scaled(px, px, Qt::KeepAspectRatio));
        image = reader.read();
    }
    if (image.isNull())
        image = FileIconProvider::thumbnail(native, px, true); // a video's, a PDF's: the cache only, instant
    if (!image.isNull())
        return {image.convertToFormat(QImage::Format_ARGB32_Premultiplied), opaque(image)};
    return {FileIconProvider::fileIcon(native, px).convertToFormat(QImage::Format_ARGB32_Premultiplied), false};
}

// Where the picture has ink (an icon's own margin left out).
QRect inkOf(const QImage& image)
{
    int left = image.width(), top = image.height(), right = -1, bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) > 24) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    return right < 0 ? image.rect() : QRect(QPoint(left, top), QPoint(right, bottom));
}

// One pass of a box blur along rows (or columns, `across` false) of `a`.
void boxBlur(std::vector<int>& a, int w, int h, int r, bool across)
{
    const int lines = across ? h : w, length = across ? w : h;
    std::vector<int> line(length);
    for (int l = 0; l < lines; ++l) {
        const auto at = [&](int i) -> int& { return across ? a[l * w + i] : a[i * w + l]; };
        for (int i = 0; i < length; ++i)
            line[i] = at(i);
        int sum = 0;
        for (int i = -r; i <= r; ++i)
            sum += i >= 0 && i < length ? line[i] : 0;
        for (int i = 0; i < length; ++i) {
            at(i) = sum / (2 * r + 1);
            const int out = i - r, in = i + r + 1;
            sum += (in < length ? line[in] : 0) - (out >= 0 ? line[out] : 0);
        }
    }
}

// The soft shadow of `image`'s ink: its alpha, blurred (three box passes come
// close to a Gaussian), in `color`.
QImage shadowOf(const QImage& image, int radius, QColor color)
{
    const int w = image.width(), h = image.height();
    std::vector<int> alpha(std::size_t(w) * h);
    for (int y = 0; y < h; ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < w; ++x)
            alpha[std::size_t(y) * w + x] = qAlpha(line[x]);
    }
    const int r = std::max(1, radius / 3);
    for (int pass = 0; pass < 3; ++pass) {
        boxBlur(alpha, w, h, r, true);
        boxBlur(alpha, w, h, r, false);
    }
    QImage shadow(w, h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) {
        auto* line = reinterpret_cast<QRgb*>(shadow.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int a = alpha[std::size_t(y) * w + x] * color.alpha() / 255;
            line[x] = qRgba(color.red() * a / 255, color.green() * a / 255, color.blue() * a / 255, a);
        }
    }
    return shadow;
}

} // namespace

DragPicture dragPicture(const QStringList& paths, qreal dpr)
{
    if (paths.isEmpty())
        return {};
    const auto px = [dpr](qreal logical) { return std::max(1, qRound(logical * dpr)); };
    const int box = px(72); // the front picture's longer side
    const int step = px(7); // each one behind: up and to the right
    const int pad = px(18); // room for the shadow and the badge

    // Photos stack, as prints in a pile; icons do not (the same outline a few
    // pixels apart reads as a blur): the first one alone, the count says the rest.
    std::vector<Item> items {picture(paths.front(), box)};
    if (items.front().image.isNull())
        return {};
    for (int i = 1; i < std::min<int>(paths.size(), 3) && items.front().photo; ++i) {
        Item behind = picture(paths.at(i), box);
        if (!behind.photo)
            break;
        items.push_back(std::move(behind));
    }
    const int layers = static_cast<int>(items.size());

    const int spread = step * (layers - 1);
    QImage canvas(pad * 2 + box + spread, pad * 2 + box + spread, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // The front item's box at the bottom left, each behind it a step further
    // up and right; every picture centred in its box on whole pixels, as it
    // came (never resampled).
    QRect frontInk;
    for (int i = layers - 1; i >= 0; --i) {
        const QImage& image = items[i].image;
        if (image.isNull())
            continue;
        const QPoint boxAt(pad + step * i, pad + spread - step * i);
        const QPoint at = boxAt + QPoint((box - image.width()) / 2, (box - image.height()) / 2);
        if (items[i].photo) {
            const qreal radius = px(4);
            QPainterPath round;
            round.addRoundedRect(QRectF(at, image.size()), radius, radius);
            painter.save();
            painter.setClipPath(round);
            painter.drawImage(at, image);
            painter.restore();
            // A hairline, so a pale photo keeps its edge on a pale window.
            const qreal hair = 1;
            painter.setPen(QPen(QColor(0, 0, 0, 40), hair));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(at, image.size()).adjusted(hair / 2, hair / 2, -hair / 2, -hair / 2),
                                    radius - hair / 2, radius - hair / 2);
        } else {
            painter.drawImage(at, image);
        }
        if (i == 0)
            frontInk = items[i].photo ? QRect(at, image.size()) : inkOf(image).translated(at);
    }

    // Several: their count on the accent, in white, at the front picture's
    // upper right corner (by its ink, not its box).
    if (paths.size() > 1) {
        const QString count = paths.size() > 99 ? QStringLiteral("99+") : QString::number(paths.size());
        QFont font = QGuiApplication::font();
        font.setPixelSize(px(12));
        font.setWeight(QFont::DemiBold);
        QPainterPath digits;
        digits.addText(0, 0, font, count);
        const QRectF ink = digits.boundingRect();
        const qreal height = px(20);
        const qreal width = std::max(height, ink.width() + px(12));
        const QPointF centre(frontInk.right() + 1 - px(2), frontInk.top() + px(2));
        const QRectF badge(centre.x() - width / 2, centre.y() - height / 2, width, height);
        painter.setPen(Qt::NoPen);
        painter.setBrush(SystemTheme::accentColor());
        painter.drawRoundedRect(badge, height / 2, height / 2);
        painter.setBrush(Qt::white);
        painter.drawPath(digits.translated(badge.center() - ink.center())); // centred by the digits' ink
    }
    painter.end();

    // Lifted off whatever it passes over: a soft shadow a little below.
    QImage picture(canvas.size(), QImage::Format_ARGB32_Premultiplied);
    picture.fill(Qt::transparent);
    QPainter layered(&picture);
    layered.drawImage(QPoint(0, px(2)), shadowOf(canvas, px(8), QColor(0, 0, 0, 80)));
    layered.drawImage(QPoint(0, 0), canvas);
    layered.end();

    // The pointer's tip just under the front picture, at its middle.
    const QPoint hotspot(frontInk.center().x(), std::min(picture.height() - 1, frontInk.bottom() + px(6)));
    return {picture, hotspot};
}

} // namespace ws
