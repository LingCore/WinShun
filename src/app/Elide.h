#pragma once

#include <QColor>
#include <QFont>
#include <QList>
#include <QString>

#include <utility>

namespace ws::elide {

// Ranges of a text: start, length.
using Ranges = QList<std::pair<qsizetype, qsizetype>>;

// `text` with `ranges` wrapped in the match colour (Text.StyledText), bold
// where the style's match weight is (Theme.matchWeight: PuHuiTi has only 400
// and 700, so a match is bold or not), escaped; overlapping ranges keep the first.
QString highlighted(const QString& text, Ranges ranges, const QColor& color, bool bold = true);
// A style's match weight (Font.Bold …) as highlighted() takes it.
inline bool boldWeight(int weight) { return weight >= QFont::DemiBold; }

// `text` highlighted as above and shortened in the middle to `width` in
// `font` (the highlighted characters bold where `bold`), the way Text.ElideMiddle would
// but around the ranges: they stay in sight, with the start and the end of
// the text. Without ranges it is shortened in its middle, the end kept as
// far as the start (a file's name keeps its extension and what comes just
// before it). A folder's path is cut where a folder's name starts, if one is
// near. Fits already, or the ranges alone are too wide: highlighted(), for
// the Text to elide.
QString shortenedHtml(const QString& text, Ranges ranges, qreal width, const QFont& font, const QColor& color, bool bold = true);

} // namespace ws::elide
