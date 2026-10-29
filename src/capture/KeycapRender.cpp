#include "KeycapRender.h"

#include <QFontMetricsF>
#include <QPainter>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace ws::capture::keycaps {

namespace {

int scaled(double value, int least)
{
    return std::max(least, int(std::lround(value)));
}

int advance(const QFontMetricsF& metrics, const QString& text)
{
    return int(std::ceil(metrics.horizontalAdvance(text)));
}

// The baseline that centres the text on `middle`: Latin letters and digits
// on their capitals (a "g" or "p" hangs below, as on a key), CJK words and
// symbols (左键, ←) on their ink.
qreal baseline(const QString& text, const QFontMetricsF& metrics, qreal middle)
{
    const bool latin = std::all_of(text.cbegin(), text.cend(), [](QChar c) { return c.unicode() < 0x2000; });
    if (latin)
        return std::round(middle + metrics.capHeight() / 2);
    const QRectF ink = metrics.tightBoundingRect(text);
    return std::round(middle - (ink.top() + ink.height() / 2));
}

struct Piece {
    enum Kind { Cap, Plus, Count, Text } kind = Cap;
    QString text;
    int width = 0;
};

} // namespace

Picture render(std::span<const Row> rows, const QFont& font, int unit)
{
    Picture picture;
    if (rows.empty())
        return picture;
    const int pad = scaled(unit * 0.14, 2);
    const int capHeight = unit - 2 * pad;
    const int edge = scaled(capHeight * 0.07, 1); // the keycap's lower side, darker
    const int pixel = scaled(capHeight * 0.5, 9);
    QFont capFont(font);
    capFont.setPixelSize(pixel);
    capFont.setWeight(QFont::Bold);
    capFont.setStyleStrategy(QFont::PreferAntialias);
    QFont textFont(capFont);
    textFont.setWeight(QFont::Normal);
    QFont plusFont(textFont);
    plusFont.setPixelSize(scaled(pixel * 0.85, 8));
    const QFontMetricsF capMetrics(capFont);
    const QFontMetricsF textMetrics(textFont);
    const QFontMetricsF plusMetrics(plusFont);
    const int capPadding = scaled(capHeight * 0.3, 3);
    const int spacing = scaled(capHeight * 0.16, 2);

    std::vector<std::vector<Piece>> pieces(rows.size());
    std::vector<int> widths(rows.size());
    int widest = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        auto& list = pieces[i];
        int x = pad;
        if (row.text) {
            const QString text = (row.cut ? u"…"_s : QString()) + row.caps.value(0);
            list.push_back({Piece::Text, text, advance(textMetrics, text) + 2 * capPadding});
            x += list.back().width;
        } else {
            for (qsizetype c = 0; c < row.caps.size(); ++c) {
                if (c) {
                    list.push_back({Piece::Plus, u"+"_s, advance(plusMetrics, u"+"_s) + 2 * spacing});
                    x += list.back().width;
                }
                list.push_back({Piece::Cap, row.caps[c], std::max(capHeight, advance(capMetrics, row.caps[c]) + 2 * capPadding)});
                x += list.back().width;
            }
            if (row.count > 1) {
                const QString count = u"×%1"_s.arg(row.count);
                list.push_back({Piece::Count, count, advance(capMetrics, count) + 2 * spacing});
                x += list.back().width;
            }
        }
        widths[i] = x + pad;
        widest = std::max(widest, widths[i]);
    }

    constexpr int kApart = 2; // rows apart in the picture
    picture.image = QImage(widest, int(rows.size()) * (unit + kApart) - kApart, QImage::Format_ARGB32_Premultiplied);
    picture.image.fill(Qt::transparent);
    QPainter painter(&picture.image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const qreal radius = std::round(unit * 0.22);
    const qreal capRadius = std::max(2.0, std::round(capHeight * 0.18));
    for (size_t i = 0; i < rows.size(); ++i) {
        const int top = int(i) * (unit + kApart);
        const QRect pill(0, top, widths[i], unit);
        picture.rows.push_back(pill);
        painter.setPen(QPen(QColor(255, 255, 255, 52), 1));
        painter.setBrush(QColor(22, 22, 25, 226));
        painter.drawRoundedRect(QRectF(pill).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        const qreal face = top + pad + (capHeight - edge) / 2.0; // the keycaps' top's middle
        int x = pad;
        for (const Piece& piece : pieces[i]) {
            switch (piece.kind) {
            case Piece::Cap: {
                const QRectF cap(x, top + pad, piece.width, capHeight);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(46, 46, 52));
                painter.drawRoundedRect(cap, capRadius, capRadius);
                painter.setBrush(QColor(76, 76, 84));
                painter.drawRoundedRect(cap.adjusted(0, 0, 0, -edge), capRadius, capRadius);
                painter.setFont(capFont);
                painter.setPen(Qt::white);
                painter.drawText(QPointF(std::round(x + (piece.width - capMetrics.horizontalAdvance(piece.text)) / 2),
                                     baseline(piece.text, capMetrics, face)),
                    piece.text);
                break;
            }
            case Piece::Plus:
                painter.setFont(plusFont);
                painter.setPen(QColor(176, 176, 184));
                painter.drawText(QPointF(std::round(x + (piece.width - plusMetrics.horizontalAdvance(piece.text)) / 2),
                                     baseline(piece.text, plusMetrics, face)),
                    piece.text);
                break;
            case Piece::Count:
                painter.setFont(capFont);
                painter.setPen(Qt::white);
                painter.drawText(QPointF(x + spacing, baseline(piece.text, capMetrics, face)), piece.text);
                break;
            case Piece::Text:
                painter.setFont(textFont);
                painter.setPen(Qt::white);
                painter.drawText(QPointF(x + capPadding, baseline(piece.text, textMetrics, top + unit / 2.0)), piece.text);
                break;
            }
            x += piece.width;
        }
    }
    return picture;
}

std::vector<QPoint> place(const Picture& picture, QSize region, int unit, bool left)
{
    const int margin = marginFor(unit);
    const int gap = rowGapFor(unit);
    std::vector<QPoint> at(picture.rows.size());
    int bottom = region.height() - margin;
    for (size_t i = picture.rows.size(); i-- > 0;) {
        const QRect& row = picture.rows[i];
        at[i] = QPoint(left ? margin : region.width() - margin - row.width(), bottom - row.height());
        bottom -= row.height() + gap;
    }
    return at;
}

} // namespace ws::capture::keycaps
