#include "ContentDetails.h"

#include <QRegularExpression>
#include <algorithm>
#include <numeric>

using namespace Qt::StringLiterals;

namespace ws::contentdetails {

std::shared_ptr<const Document> document(const doctext::DocText& source, const Cancel& cancel)
{
    auto out = std::make_shared<Document>();
    out->text = QString::fromUtf8(source.text);
    out->text.replace(u"\r\n"_s, u"\n"_s);
    out->text.replace(u'\r', u'\n');
    out->truncated = source.truncated;
    for (const auto& sheet : source.sheets)
        out->sheets.append(QString::fromUtf8(sheet));
    std::size_t place = 0;
    doctext::Place current;
    int section = 0;
    for (qsizetype at = 0; at < out->text.size();) {
        if (cancel && cancel())
            return {};
        if (out->lines.size() == kMaxLines) {
            out->truncated = true;
            out->text.truncate(at);
            break;
        }
        const auto number = static_cast<std::uint32_t>(out->lines.size() + 1);
        while (place < source.places.size() && source.places[place].line <= number) {
            const auto& next = source.places[place++];
            if (next.kind != current.kind || next.sheet != current.sheet
                || (next.kind != doctext::PlaceKind::Row && next.number != current.number))
                ++section;
            current = next;
        }
        const qsizetype end = out->text.indexOf(u'\n', at);
        const qsizetype length = (end < 0 ? out->text.size() : end) - at;
        const std::uint32_t where = current.kind == doctext::PlaceKind::Row
            ? current.number + number - current.line : current.number;
        out->lines.push_back({at, length, current.kind,
            current.kind == doctext::PlaceKind::None ? number : where, current.sheet, section});
        at += length + 1;
    }
    return out;
}

QString lineText(const Document& doc, int line)
{
    if (line < 0 || line >= std::ssize(doc.lines))
        return {};
    const auto& at = doc.lines[line];
    return doc.text.mid(at.offset, at.length);
}

Matches find(std::shared_ptr<const Document> text, const QStringList& terms, const Cancel& cancel)
{
    Matches out;
    out.document = std::move(text);
    if (!out.document)
        return out;
    out.truncated = out.document->truncated;
    for (const QString& term : terms) {
        if (!term.isEmpty() && !out.terms.contains(term, Qt::CaseInsensitive))
            out.terms.append(term);
    }
    out.counts.resize(out.terms.size());
    out.sheetCounts.resize(out.document->sheets.size());
    for (int line = 0; line < std::ssize(out.document->lines); ++line) {
        const auto& row = out.document->lines[line];
        const QStringView value = QStringView(out.document->text).mid(row.offset, row.length);
        for (int term = 0; term < out.terms.size(); ++term) {
            qsizetype at = 0;
            while ((at = value.indexOf(out.terms[term], at, Qt::CaseInsensitive)) >= 0) {
                if (cancel && cancel()) {
                    out.cancelled = true;
                    return out;
                }
                if (out.items.size() == kMaxMatches) {
                    out.truncated = true;
                    return out;
                }
                out.items.push_back({line, at, out.terms[term].size(), term});
                ++out.counts[term];
                if (row.sheet < out.sheetCounts.size()) ++out.sheetCounts[row.sheet];
                at += out.terms[term].size(); // occurrences of one term do not overlap
            }
        }
        if (cancel && cancel()) {
            out.cancelled = true;
            return out;
        }
    }
    std::ranges::stable_sort(out.items, [](const Match& a, const Match& b) {
        return a.line != b.line ? a.line < b.line : a.start < b.start;
    });
    return out;
}

Rows filter(const Matches& all, const std::vector<bool>& enabled, const Filter& settings, int context,
    int sheet, const Cancel& cancel)
{
    Rows out;
    if (!all.document)
        return out;
    QRegularExpression re;
    if (!settings.text.isEmpty()) {
        out.refinements = std::make_shared<std::vector<Match>>();
        const QString pattern = settings.regex ? settings.text : QRegularExpression::escape(settings.text);
        const QString boundary = settings.wholeWord ? u"(?<![\\p{L}\\p{N}_])(?:%1)(?![\\p{L}\\p{N}_])"_s.arg(pattern) : pattern;
        re = QRegularExpression(u"(*LIMIT_MATCH=10000)(*LIMIT_DEPTH=100)"_s + boundary,
            QRegularExpression::UseUnicodePropertiesOption
                | (settings.matchCase ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption));
        if (!re.isValid()) {
            out.error = re.errorString();
            return out;
        }
    }
    const auto& doc = *all.document;
    std::vector<bool> included(doc.lines.size(), false);
    int previousLine = -1;
    bool accepted = false;
    for (int i = 0; i < std::ssize(all.items); ++i) {
        if (cancel && cancel()) {
            out.cancelled = true;
            return out;
        }
        const Match& match = all.items[i];
        if (match.term >= std::ssize(enabled) || !enabled[match.term]
            || (sheet >= 0 && doc.lines[match.line].sheet != sheet))
            continue;
        if (match.line != previousLine) {
            previousLine = match.line;
            const QString value = lineText(doc, match.line);
            accepted = true;
            if (!settings.text.isEmpty()) {
                const auto evaluated = re.matchView(value);
                if (!evaluated.isValid()) {
                    out.error = u"Regular expression exceeded the matching limit"_s;
                    out.matches.clear();
                    return out;
                }
                accepted = !settings.enabled || evaluated.hasMatch();
                auto occurrences = re.globalMatchView(value);
                while (occurrences.hasNext() && out.filterCount < static_cast<qint64>(kMaxMatches)) {
                    const auto at = occurrences.next();
                    ++out.filterCount;
                    if (at.capturedLength() > 0)
                        out.refinements->push_back({match.line, at.capturedStart(), at.capturedLength(), -1});
                    if (cancel && cancel()) { out.cancelled = true; return out; }
                }
                if (occurrences.hasNext()) out.truncated = true;
            }
        }
        if (!accepted)
            continue;
        out.matches.push_back(i);
        const int radius = std::clamp(context, 0, 3);
        for (int line = std::max(0, match.line - radius);
            line <= std::min(static_cast<int>(doc.lines.size()) - 1, match.line + radius); ++line) {
            // Context never crosses a page, slide or sheet, nor bridges
            // missing spreadsheet rows as if they were adjacent.
            const auto& target = doc.lines[line];
            const auto& origin = doc.lines[match.line];
            if (target.section == origin.section
                && (origin.kind != doctext::PlaceKind::Row
                    || std::abs(static_cast<qint64>(target.number) - origin.number) <= radius))
                included[line] = true;
        }
    }
    for (int i = 0; i < std::ssize(included); ++i) {
        if (included[i])
            out.lines.push_back(i);
    }
    return out;
}

} // namespace ws::contentdetails
