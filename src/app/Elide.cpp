#include "Elide.h"

#include <QFontMetricsF>

#include <algorithm>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::elide {

QString highlighted(const QString& text, Ranges ranges, const QColor& color, bool bold)
{
    std::sort(ranges.begin(), ranges.end());
    QString html;
    html.reserve(text.size() + 64);
    const QString open = u"<font color=\"%1\">%2"_s.arg(color.name(), bold ? u"<b>"_s : QString());
    const QString close = bold ? u"</b></font>"_s : u"</font>"_s;
    qsizetype pos = 0;
    for (const auto& [start, length] : ranges) {
        if (start < pos || length <= 0)
            continue; // overlapping ranges: keep the first
        html += text.mid(pos, start - pos).toHtmlEscaped();
        html += open + text.mid(start, length).toHtmlEscaped() + close;
        pos = start + length;
    }
    html += text.mid(pos).toHtmlEscaped();
    return html;
}

QString shortenedHtml(const QString& text, Ranges ranges, qreal width, const QFont& font, const QColor& color, bool bold)
{
    std::sort(ranges.begin(), ranges.end());

    // Each character's advance, the highlighted ones bold; a surrogate pair's on its first half.
    QFont matched = font;
    matched.setBold(bold);
    const QFontMetricsF plain(font);
    const QFontMetricsF strong(matched);
    std::vector<qreal> before(static_cast<std::size_t>(text.size()) + 1); // of the text up to each place
    for (qsizetype i = 0, r = 0; i < text.size(); ++i) {
        while (r < ranges.size() && ranges[r].first + ranges[r].second <= i)
            ++r;
        const bool strongHere = r < ranges.size() && ranges[r].first <= i;
        qreal advance = 0;
        if (text[i].isHighSurrogate() && i + 1 < text.size())
            advance = (strongHere ? strong : plain).horizontalAdvance(text.mid(i, 2));
        else if (!text[i].isLowSurrogate())
            advance = (strongHere ? strong : plain).horizontalAdvance(text[i]);
        before[i + 1] = before[i] + advance;
    }
    const auto widthOf = [&](qsizetype from, qsizetype to) { return before[to] - before[from]; };
    const qreal room = width - 2; // what the advances leave out (kerning, rounding)
    if (widthOf(0, text.size()) <= room)
        return highlighted(text, ranges, color, bold);

    // Shortened in its middle, keeping as much of its end as of its start
    // (the end of a file's name says which one: "…第三版.docx", "…(2).pdf").
    // In a path, the end starts where a folder's name does, if one is near.
    const qreal ellipsis = plain.horizontalAdvance(u"…"_s);
    const auto starts = [&](qsizetype i) { return text[i] == u'\\'; };
    const auto shortened = [&](qsizetype from, qsizetype to, qreal fit) {
        if (widthOf(from, to) <= fit)
            return text.mid(from, to - from);
        const qreal left = fit - ellipsis;
        // The end: about half, from the start of a folder's name a little
        // before it, or else just after it.
        qsizetype tail = to;
        while (tail > from && widthOf(tail - 1, to) <= left / 2)
            --tail;
        qsizetype back = tail;
        while (back > from && !starts(back) && widthOf(back - 1, to) <= left * 2 / 3)
            --back;
        if (back < tail && starts(back)) {
            tail = back;
        } else {
            qsizetype on = tail;
            while (on < to && !starts(on))
                ++on;
            if (on < to)
                tail = on;
        }
        // The start: the rest, to the end of a folder's name in it.
        qsizetype head = from;
        while (head < tail && widthOf(from, head + 1) + widthOf(tail, to) <= left)
            ++head;
        if (head < tail) {
            qsizetype cut = head;
            while (cut > from && text[cut - 1] != u'\\')
                --cut;
            if (cut > from)
                head = cut;
        }
        if (head > from && text[head - 1].isHighSurrogate())
            --head;
        if (tail < to && text[tail].isLowSurrogate())
            ++tail;
        return text.mid(from, head - from) + u"…"_s + text.mid(tail, to - tail);
    };
    if (ranges.isEmpty())
        return shortened(0, text.size(), room).toHtmlEscaped();

    // Each side of the ranges shortened in its middle: the start of the
    // text and what is just before them; just after them and the end.
    const qsizetype coreFrom = ranges.front().first;
    qsizetype coreTo = 0;
    for (const auto& [start, length] : ranges)
        coreTo = std::max(coreTo, start + length);
    const qreal spare = room - widthOf(coreFrom, coreTo);
    if (spare <= 0)
        return highlighted(text, ranges, color, bold); // the Text elides it
    const qreal leftWidth = widthOf(0, coreFrom);
    const qreal rightWidth = widthOf(coreTo, text.size());
    qreal leftRoom = spare / 2;
    qreal rightRoom = spare / 2;
    if (leftWidth < leftRoom) {
        rightRoom += leftRoom - leftWidth;
        leftRoom = leftWidth;
    } else if (rightWidth < rightRoom) {
        leftRoom += rightRoom - rightWidth;
        rightRoom = rightWidth;
    }
    const QString leftPart = shortened(0, coreFrom, leftRoom);
    const QString shown = leftPart + text.mid(coreFrom, coreTo - coreFrom) + shortened(coreTo, text.size(), rightRoom);
    for (auto& range : ranges)
        range.first += leftPart.size() - coreFrom;
    return highlighted(shown, ranges, color, bold);
}

} // namespace ws::elide
