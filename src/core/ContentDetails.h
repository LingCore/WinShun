#pragma once

#include "ContentPattern.h"
#include "DocText.h"

#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <vector>

namespace ws::contentdetails {

// Offsets are UTF-16, as Qt's text controls expect. Source line numbers and
// document locations are kept separately: an Excel row is not a text row.
struct Line {
    qsizetype offset = 0;
    qsizetype length = 0;
    doctext::PlaceKind kind = doctext::PlaceKind::None;
    std::uint32_t number = 0;
    std::uint16_t sheet = doctext::Place::kNoSheet;
    int section = 0;
};
struct Document {
    QString text;
    QStringList sheets;
    std::vector<Line> lines;
    bool truncated = false;
};
struct Match {
    int line = 0; // index in Document::lines
    qsizetype start = 0; // relative to that line
    qsizetype length = 0;
    int term = 0;
};
struct Matches {
    std::shared_ptr<const Document> document;
    QStringList terms;
    std::vector<Match> items;
    std::vector<qint64> counts;
    std::vector<qint64> sheetCounts;
    bool truncated = false;
    bool cancelled = false;
};
struct Filter {
    QString text;
    bool matchCase = false;
    bool wholeWord = false;
    bool regex = false;
    bool enabled = true;
};
struct Rows {
    // Original occurrence indexes, and merged context lines in document order.
    std::vector<int> matches;
    std::vector<int> lines;
    std::shared_ptr<std::vector<Match>> refinements;
    qint64 filterCount = 0;
    bool truncated = false;
    QString error;
    bool cancelled = false;
};
using Cancel = std::function<bool()>;

// Limits bound a selected-file view, not ordinary searches. A cut view must
// always be labelled as partial, including in exports.
inline constexpr std::size_t kMaxLines = 250'000;
inline constexpr std::size_t kMaxMatches = 250'000;
std::shared_ptr<const Document> document(const doctext::DocText& text, const Cancel& cancel = {});
// Where each needle of a content search is (ContentPattern::needles): words
// in any case, an expression's matches within a line. Matches::terms: their
// texts, each once.
Matches find(std::shared_ptr<const Document> text, const std::vector<ContentNeedle>& needles, const Cancel& cancel = {});
// Words, as they are.
Matches find(std::shared_ptr<const Document> text, const QStringList& terms, const Cancel& cancel = {});
Rows filter(const Matches& all, const std::vector<bool>& enabled, const Filter& filter, int context,
    int sheet = -1, const Cancel& cancel = {});
QString lineText(const Document& doc, int line);

} // namespace ws::contentdetails
