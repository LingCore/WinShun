#include "ContentScanner.h"

#include "TextUtil.h"
#include "Win32Util.h"

#include <QStringDecoder>

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws {

// ---- ByteFinder ---------------------------------------------------------

ByteFinder::ByteFinder(std::string needle)
    : m_needle(text::foldAscii(needle))
{
    const std::size_t m = m_needle.size();
    m_skip.fill(m == 0 ? 1 : m);
    for (std::size_t j = 0; j + 1 < m; ++j)
        m_skip[static_cast<unsigned char>(m_needle[j])] = m - 1 - j;
}

std::size_t ByteFinder::find(const char* data, std::size_t length, std::size_t from) const noexcept
{
    const std::size_t m = m_needle.size();
    if (m == 0 || length < m || from > length - m)
        return text::npos;
    const auto last = static_cast<unsigned char>(m_needle[m - 1]);
    std::size_t i = from;
    while (i <= length - m) {
        const unsigned char c = text::fold(data[i + m - 1]);
        if (c == last) {
            std::size_t j = m - 1;
            while (j > 0 && text::fold(data[i + j - 1]) == static_cast<unsigned char>(m_needle[j - 1]))
                --j;
            if (j == 0)
                return i;
        }
        i += m_skip[c];
    }
    return text::npos;
}

// ---- helpers --------------------------------------------------------------

namespace {

constexpr std::size_t kChunkBytes = ContentScanner::kChunkBytes;
constexpr std::size_t kContextBytes = 600; // decoded around a candidate match
constexpr std::size_t kLongLineBytes = 8 * 1024; // a line longer is packed code or data (ContentMatch::longLine)
// The same for a regular expression's match, found in the text decoded.
constexpr qsizetype kContextChars = 300;
constexpr qsizetype kLongLineChars = 4096;
constexpr std::size_t kRegexTailBytes = 64 * 1024; // of a line not yet whole, kept for the next chunk

unsigned systemLegacyCodePage()
{
    const UINT acp = ::GetACP();
    if (acp != CP_UTF8)
        return acp;
    // "Use Unicode UTF-8 for worldwide language support" is on, yet legacy
    // .txt files are still in the regional code page. Guess it from the locale.
    const LANGID lang = ::GetUserDefaultUILanguage();
    switch (PRIMARYLANGID(lang)) {
    case LANG_CHINESE:
        return SUBLANGID(lang) == SUBLANG_CHINESE_TRADITIONAL || SUBLANGID(lang) == SUBLANG_CHINESE_HONGKONG ? 950
                                                                                                             : 936;
    case LANG_JAPANESE:
        return 932;
    case LANG_KOREAN:
        return 949;
    default:
        return 1252;
    }
}

std::size_t unitOf(TextEncoding e) noexcept
{
    return e == TextEncoding::Utf16LE || e == TextEncoding::Utf16BE ? 2 : 1;
}

// `i + unit <= length` must hold.
bool isNewline(const char* d, std::size_t i, TextEncoding e) noexcept
{
    switch (e) {
    case TextEncoding::Utf16LE:
        return d[i] == '\n' && d[i + 1] == 0;
    case TextEncoding::Utf16BE:
        return d[i] == 0 && d[i + 1] == '\n';
    default:
        return d[i] == '\n';
    }
}

std::uint64_t countNewlines(const char* d, std::size_t n, TextEncoding e) noexcept
{
    if (unitOf(e) == 1)
        return static_cast<std::uint64_t>(std::count(d, d + n, '\n'));
    std::uint64_t lines = 0;
    for (std::size_t i = 0; i + 1 < n; i += 2)
        lines += isNewline(d, i, e) ? 1 : 0;
    return lines;
}

// Reads until `capacity` bytes or end of file, so only the last chunk is short.
std::size_t fill(const ContentScanner::ReadFn& read, char* buffer, std::size_t capacity)
{
    std::size_t total = 0;
    while (total < capacity) {
        const std::size_t got = read(buffer + total, capacity - total);
        if (got == 0)
            break;
        total += got;
    }
    return total;
}

QString cleanLine(QString s)
{
    s.replace(u'\t', u' ');
    s.remove(u'\r');
    s.remove(u'\n');
    s.remove(QChar(0xFEFF));
    return s;
}

} // namespace

// ---- ContentScanner -------------------------------------------------------

unsigned ContentScanner::legacyCodePage()
{
    static const unsigned codePage = systemLegacyCodePage();
    return codePage;
}

ContentScanner::ContentScanner(const ContentPattern& pattern, unsigned ansiCodePage)
    : m_ways(pattern.ways)
    , m_codePage(ansiCodePage != 0 ? ansiCodePage : legacyCodePage())
{
    for (const ContentNeedle& needle : pattern.needles) {
        const QString& text = needle.text;
        Term& term = m_terms.emplace_back();
        term.text = text;
        if (needle.regex) {
            term.regex = true;
            term.expression = needle.expression;
            m_regexTerms |= std::uint32_t {1} << (m_terms.size() - 1);
            continue;
        }
        term.utf8 = ByteFinder(text.toUtf8().toStdString());

        std::string le(static_cast<std::size_t>(text.size()) * 2, '\0');
        std::string be(le.size(), '\0');
        for (qsizetype i = 0; i < text.size(); ++i) {
            const char16_t u = text[i].unicode();
            le[2 * i] = static_cast<char>(u & 0xFF);
            le[2 * i + 1] = static_cast<char>(u >> 8);
            be[2 * i] = static_cast<char>(u >> 8);
            be[2 * i + 1] = static_cast<char>(u & 0xFF);
        }
        term.utf16le = ByteFinder(std::move(le));
        term.utf16be = ByteFinder(std::move(be));

        const auto* wide = reinterpret_cast<const wchar_t*>(text.utf16());
        const int wideLen = static_cast<int>(text.size());
        BOOL usedDefault = FALSE;
        const int n
            = ::WideCharToMultiByte(m_codePage, WC_NO_BEST_FIT_CHARS, wide, wideLen, nullptr, 0, nullptr, &usedDefault);
        if (n > 0 && !usedDefault) {
            std::string ansi(static_cast<std::size_t>(n), '\0');
            ::WideCharToMultiByte(m_codePage, WC_NO_BEST_FIT_CHARS, wide, wideLen, ansi.data(), n, nullptr, &usedDefault);
            if (!usedDefault)
                term.ansi.emplace(std::move(ansi));
        }
    }
}

template <typename F> void ContentScanner::forEachHit(const Term& term, QStringView text, qsizetype from, F&& f) const
{
    if (!term.regex) {
        for (qsizetype at = text.indexOf(term.text, from, Qt::CaseInsensitive); at >= 0;
            at = text.indexOf(term.text, at + term.text.size(), Qt::CaseInsensitive)) {
            if (!f(at, term.text.size()))
                return;
        }
        return;
    }
    while (from <= text.size()) {
        const QRegularExpressionMatch match = term.expression.matchView(text, from);
        if (!match.hasMatch())
            return;
        const qsizetype at = match.capturedStart();
        const qsizetype length = match.capturedLength();
        if (length == 0) { // "x*" matches nothing everywhere: no place to show
            from = at + 1;
            continue;
        }
        if (!f(at, length))
            return;
        from = at + length;
    }
}

std::uint32_t ContentScanner::termsIn(const QString& line) const
{
    std::uint32_t found = 0;
    for (std::size_t i = 0; i < m_terms.size(); ++i) {
        forEachHit(m_terms[i], line, 0, [&](qsizetype, qsizetype) {
            found |= std::uint32_t {1} << i;
            return false;
        });
    }
    return found;
}

std::uint32_t ContentScanner::wayIn(std::uint32_t seen, std::uint32_t dead) const noexcept
{
    for (const std::uint32_t way : m_ways) {
        if ((way & seen) == way && !(way & dead))
            return way;
    }
    return 0;
}

std::uint32_t ContentScanner::wordStartsIn(const QString& line, std::uint32_t terms) const
{
    // A CJK character starts a word wherever it is; "Lock" does in "readLock".
    const auto wordChar = [](QChar c) { return c.unicode() < 0x80 && (c.isLetterOrNumber() || c == u'_'); };
    const auto startsWord = [&](qsizetype i) {
        return i == 0 || !wordChar(line[i - 1]) || (line[i - 1].isLower() && line[i].isUpper());
    };
    std::uint32_t starts = 0;
    for (std::size_t t = 0; t < m_terms.size(); ++t) {
        const std::uint32_t bit = std::uint32_t {1} << t;
        if (!(terms & bit))
            continue;
        forEachHit(m_terms[t], line, 0, [&](qsizetype at, qsizetype) {
            if (wordChar(line[at]) && !startsWord(at))
                return true;
            starts |= bit;
            return false;
        });
    }
    return starts;
}

int contentRelevance(const ContentMatch& match, const QStringList& words, QStringView fileName, int priority,
    std::int64_t ageSeconds)
{
    const int n = match.termCount > 0 ? match.termCount : static_cast<int>(words.size());
    if (n == 0)
        return 0;
    int score = 0;
    // Together on one line far more than apart; a packed file is all one line.
    if (n > 1 && !match.longLine)
        score += match.termsOnLine >= n ? 40 : 15 * std::max(match.termsOnLine - 1, 0) / (n - 1);
    score += 20 * std::popcount(match.wordStarts) / n;
    int named = 0;
    for (const QString& t : words)
        named += fileName.contains(t, Qt::CaseInsensitive) ? 1 : 0;
    score += 30 * std::min(named, n) / n; // of "a|b", one in the name is all of it
    constexpr int kPlace[] = {15, 0, -5, -10};
    score += kPlace[std::clamp(priority, 0, 3)];
    constexpr std::int64_t kDay = 24 * 60 * 60;
    if (ageSeconds >= 0)
        score += ageSeconds < 7 * kDay ? 12 : ageSeconds < 30 * kDay ? 8 : ageSeconds < 365 * kDay ? 4 : 0;
    if (match.longLine)
        score -= 35;
    return score;
}

namespace {

// How wide a character shows, in halves of an ideograph: CJK and full-width ones are 2.
int widthOf(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= 0x1100 && u <= 0x115F) || (u >= 0x2E80 && u <= 0xA4CF) || (u >= 0xAC00 && u <= 0xD7A3)
            || (u >= 0xF900 && u <= 0xFAFF) || (u >= 0xFE30 && u <= 0xFE4F) || (u >= 0xFF00 && u <= 0xFF60)
            || (u >= 0xFFE0 && u <= 0xFFE6)
        ? 2
        : 1;
}

} // namespace

// The snippet shows every term, within the width a result's second line has:
// the line shown from a little before its terms, and the terms it lacks after
// it, each with a few words from where it first is (`others`): "弯管机用户手
// 册…进行优化，如 … 折弯角度设定…". Terms on one line too far apart to fit
// are pieces of their own too.
ContentMatch ContentScanner::matchIn(
    const QString& line, int lineNumber, std::uint32_t has, const std::vector<QString>& others) const
{
    constexpr int kExpressionWidth = 8; // what an expression's match is taken to be wide, before it is found
    constexpr int kWidth = 56; // what a result shows, in halves of an ideograph (28 of them)
    constexpr int kLeadMax = 24; // at most before the first term of a piece
    constexpr int kGap = 4; // " … " between pieces
    constexpr std::size_t kMaxPieces = 4;
    constexpr qsizetype kMaxChars = 160; // the last piece runs on, and the row elides it
    const auto bit = [](std::size_t t) { return std::uint32_t {1} << t; };

    struct Hit {
        qsizetype at;
        qsizetype end;
        std::size_t term;
    };
    struct Source {
        const QString* text;
        std::vector<Hit> hits;
        std::vector<int> widths; // of the text before each place
        int width(qsizetype from, qsizetype to) const { return widths[to] - widths[from]; }
    };
    std::vector<Source> sources;
    const auto addSource = [&](const QString& text) {
        Source& s = sources.emplace_back();
        s.text = &text;
        for (std::size_t t = 0; t < m_terms.size(); ++t) {
            forEachHit(m_terms[t], text, 0, [&](qsizetype at, qsizetype length) {
                s.hits.push_back({at, at + length, t});
                return s.hits.size() < 4096; // a line of nothing but it
            });
        }
        std::ranges::sort(s.hits, {}, &Hit::at);
        s.widths.resize(static_cast<std::size_t>(text.size()) + 1);
        for (qsizetype i = 0; i < text.size(); ++i)
            s.widths[i + 1] = s.widths[i] + widthOf(text[i]);
    };
    addSource(line);
    for (const QString& other : others)
        addSource(other);
    std::vector<int> termWidths;
    for (const Term& t : m_terms) {
        int w = t.regex ? kExpressionWidth : 0;
        for (qsizetype i = 0; !t.regex && i < t.text.size(); ++i)
            w += widthOf(t.text[i]);
        termWidths.push_back(w);
    }

    // The pieces: each time the run of a text's terms that shows the most of
    // those not shown yet, leaving room for the rest; the line shown first.
    struct Piece {
        std::size_t source = 0;
        qsizetype from = 0; // its terms, from the first to the end of the last
        qsizetype to = 0;
    };
    std::vector<Piece> pieces;
    std::uint32_t unshown = 0;
    for (const Source& s : sources) {
        for (const Hit& h : s.hits)
            unshown |= bit(h.term);
    }
    int used = 0;
    while (unshown != 0 && pieces.size() < kMaxPieces) {
        int most = 0;
        Piece pick;
        std::uint32_t pickTerms = 0;
        for (std::size_t si = 0; si < sources.size(); ++si) {
            const Source& s = sources[si];
            for (std::size_t a = 0; a < s.hits.size(); ++a) {
                if (!(unshown & bit(s.hits[a].term)))
                    continue;
                std::uint32_t fit = 0;
                qsizetype end = 0;
                for (std::size_t b = a; b < s.hits.size(); ++b) {
                    end = std::max(end, s.hits[b].end);
                    fit |= bit(s.hits[b].term);
                    const int span = s.width(s.hits[a].at, end);
                    if (b > a && span > kWidth)
                        break;
                    int room = kWidth - used - (pieces.empty() ? 0 : kGap);
                    for (std::size_t t = 0; t < m_terms.size(); ++t) {
                        if (unshown & ~fit & bit(t))
                            room -= termWidths[t] + kGap;
                    }
                    if (b > a && span > room)
                        continue;
                    if (const int count = std::popcount(fit & unshown); count > most) {
                        most = count;
                        pick = {si, s.hits[a].at, end};
                        pickTerms = fit;
                    }
                }
            }
        }
        if (most == 0)
            break;
        used += sources[pick.source].width(pick.from, pick.to) + (pieces.empty() ? 0 : kGap);
        pieces.push_back(pick);
        unshown &= ~pickTerms;
    }
    if (pieces.empty())
        pieces.push_back({});
    std::ranges::sort(pieces, [](const Piece& a, const Piece& b) {
        return a.source != b.source ? a.source < b.source : a.from < b.from;
    });

    // Around each, an equal share of the width left before its terms, and
    // after them but for the last, which runs on (the row elides it). One
    // piece alone has half of it before.
    const auto count = static_cast<int>(pieces.size());
    int spans = kGap * (count - 1);
    for (const Piece& p : pieces)
        spans += sources[p.source].width(p.from, p.to);
    const int spare = std::max(kWidth - spans, 0);
    const int share = count == 1 ? spare / 2 : spare / (2 * count - 1);
    struct Shown {
        std::size_t source;
        qsizetype start;
        qsizetype end;
        qsizetype lead; // where the text starts, past spaces
    };
    std::vector<Shown> shown;
    for (int i = 0; i < count; ++i) {
        const Piece& p = pieces[i];
        const Source& s = sources[p.source];
        const QString& text = *s.text;
        const int before = std::min(kLeadMax, share);
        qsizetype lead = 0;
        while (lead < p.from && text[lead].isSpace())
            ++lead;
        qsizetype start = p.from;
        if (s.width(lead, p.from) <= before) {
            start = lead;
        } else {
            while (start > lead && s.width(start - 1, p.from) <= before)
                --start;
            if (start < p.from && text[start].isLowSurrogate())
                ++start;
        }
        qsizetype end = p.to;
        if (i == count - 1) {
            end = std::min(text.size(), std::max(start + kMaxChars, p.to));
        } else {
            const int after = 2 * share - s.width(start, p.from); // what it left before too
            while (end < text.size() && s.width(p.to, end + 1) <= after)
                ++end;
        }
        if (end > p.to && end < text.size() && text[end].isLowSurrogate())
            --end;
        while (end > p.to && text[end - 1].isSpace())
            --end;
        if (!shown.empty() && shown.back().source == p.source && start <= shown.back().end)
            shown.back().end = std::max(shown.back().end, end); // one run after all
        else
            shown.push_back({p.source, start, end, lead});
    }

    ContentMatch match;
    match.line = lineNumber;
    for (std::size_t i = 0; i < shown.size(); ++i) {
        const Shown& piece = shown[i];
        const QString& text = *sources[piece.source].text;
        if (i > 0)
            match.snippet += u" … "_s;
        else if (piece.start > piece.lead)
            match.snippet += u"…"_s;
        const qsizetype offset = match.snippet.size();
        match.snippet += QStringView(text).mid(piece.start, piece.end - piece.start);
        for (const Term& t : m_terms) {
            forEachHit(t, text, piece.start, [&](qsizetype at, qsizetype length) {
                if (at + length > piece.end)
                    return false;
                match.matches.append({offset + at - piece.start, length});
                return true;
            });
        }
        if (i + 1 == shown.size() && piece.end < text.size())
            match.snippet += u"…"_s;
    }
    std::ranges::sort(match.matches);
    match.termsOnLine = std::popcount(has);
    match.wordStarts = wordStartsIn(line, has);
    return match;
}

bool ContentScanner::isUtf8(std::string_view b, bool allowTruncatedTail)
{
    const std::size_t n = b.size();
    std::size_t i = 0;
    while (i < n) {
        // Eight ASCII bytes at a time: most text is.
        for (std::uint64_t eight = 0; i + 8 <= n; i += 8) {
            std::memcpy(&eight, b.data() + i, 8);
            if (eight & 0x8080'8080'8080'8080ull)
                break;
        }
        if (i >= n)
            break;
        const auto c = static_cast<unsigned char>(b[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t len = 0;
        if (c >= 0xC2 && c <= 0xDF)
            len = 2;
        else if (c >= 0xE0 && c <= 0xEF)
            len = 3;
        else if (c >= 0xF0 && c <= 0xF4)
            len = 4;
        else
            return false;
        const std::size_t avail = std::min(len, n - i);
        for (std::size_t k = 1; k < avail; ++k) {
            if ((static_cast<unsigned char>(b[i + k]) & 0xC0) != 0x80)
                return false;
        }
        if (avail < len)
            return allowTruncatedTail;
        i += len;
    }
    return true;
}

TextEncoding ContentScanner::detect(std::string_view head, std::size_t* bomLength)
{
    const auto at = [&](std::size_t i) { return static_cast<unsigned char>(head[i]); };
    std::size_t bom = 0;
    TextEncoding encoding = TextEncoding::Utf8;
    if (head.size() >= 3 && at(0) == 0xEF && at(1) == 0xBB && at(2) == 0xBF) {
        bom = 3;
    } else if (head.size() >= 2 && at(0) == 0xFF && at(1) == 0xFE) {
        bom = 2;
        encoding = TextEncoding::Utf16LE;
    } else if (head.size() >= 2 && at(0) == 0xFE && at(1) == 0xFF) {
        bom = 2;
        encoding = TextEncoding::Utf16BE;
    } else if (!isUtf8(head, true)) {
        encoding = TextEncoding::Ansi;
    }
    if (bomLength)
        *bomLength = bom;
    return encoding;
}

QString ContentScanner::decode(const char* data, std::size_t length, TextEncoding encoding) const
{
    const auto len = static_cast<qsizetype>(length);
    switch (encoding) {
    case TextEncoding::Utf8:
        return QString::fromUtf8(data, len);
    case TextEncoding::Utf16LE: {
        QStringDecoder decoder(QStringConverter::Utf16LE);
        return decoder(QByteArrayView(data, len));
    }
    case TextEncoding::Utf16BE: {
        QStringDecoder decoder(QStringConverter::Utf16BE);
        return decoder(QByteArrayView(data, len));
    }
    case TextEncoding::Ansi: {
        const int n = ::MultiByteToWideChar(m_codePage, 0, data, static_cast<int>(length), nullptr, 0);
        if (n <= 0)
            return {};
        QString out(n, Qt::Uninitialized);
        ::MultiByteToWideChar(m_codePage, 0, data, static_cast<int>(length), reinterpret_cast<wchar_t*>(out.data()), n);
        return out;
    }
    }
    return {};
}

std::optional<ContentMatch> ContentScanner::scan(
    const ReadFn& read, std::size_t chunkSize, const CancelFn& cancelled) const
{
    if (!isValid())
        return std::nullopt;
    chunkSize = std::max<std::size_t>(chunkSize, 16) & ~std::size_t {1}; // even: keeps UTF-16 aligned
    std::size_t longestNeedle = 0;
    for (const Term& t : m_terms)
        longestNeedle = std::max({longestNeedle, t.utf8.size(), t.utf16le.size(), t.ansi ? t.ansi->size() : 0});
    // An expression is run on whole lines: what a chunk ends with of a line
    // not yet whole waits for the next one, up to this much of it.
    const std::size_t tailMax = m_regexTerms != 0 ? kRegexTailBytes : 0;

    thread_local std::vector<char> buffer;
    buffer.resize(chunkSize + std::max(longestNeedle, tailMax) + 2);
    char* data = buffer.data();

    std::size_t windowLength = fill(read, data, chunkSize);
    if (windowLength == 0)
        return std::nullopt;
    bool last = windowLength < chunkSize; // the window ends the file

    std::size_t bom = 0;
    const TextEncoding encoding = detect({data, windowLength}, &bom);
    const auto bit = [](std::size_t t) { return std::uint32_t {1} << t; };
    std::vector<const ByteFinder*> finders(m_terms.size(), nullptr); // none: an expression, or a term that cannot be
    std::uint32_t dead = 0; // the terms that cannot be in this file
    for (std::size_t t = 0; t < m_terms.size(); ++t) {
        const Term& term = m_terms[t];
        if (term.regex)
            continue;
        switch (encoding) {
        case TextEncoding::Utf8:
            finders[t] = &term.utf8;
            break;
        case TextEncoding::Utf16LE:
            finders[t] = &term.utf16le;
            break;
        case TextEncoding::Utf16BE:
            finders[t] = &term.utf16be;
            break;
        case TextEncoding::Ansi:
            if (term.ansi)
                finders[t] = &*term.ansi;
            else
                dead |= bit(t); // the code page cannot write it
            break;
        }
    }
    // The terms of the ways that can still be: the only ones looked for.
    std::uint32_t wanted = 0;
    for (const std::uint32_t way : m_ways) {
        if (!(way & dead))
            wanted |= way;
    }
    if (wanted == 0)
        return std::nullopt;
    const std::size_t unit = unitOf(encoding);
    std::size_t needleBytes = 0;
    for (std::size_t t = 0; t < finders.size(); ++t) {
        if (finders[t] && (wanted & bit(t)))
            needleBytes = std::max(needleBytes, finders[t]->size());
    }

    std::uint32_t seen = 0; // the terms found so far
    struct Best {
        QString line;
        int number = 0;
        std::uint32_t has = 0; // the terms it has
        int terms = 0; // how many of them
        int starts = 0; // how many of those start a word
        bool longLine = false;
    } best;
    struct First {
        QString line;
        int number = 0;
        std::uint32_t terms = 0; // first seen on it
    };
    std::vector<First> firsts; // for the terms the line shown lacks

    std::uint64_t linesBeforeWindow = 0;
    std::size_t windowFloor = bom; // first byte that belongs to the text
    std::size_t regexFloor = bom; // first byte no expression was run on yet (the start of a line)
    for (;;) {
        if (cancelled && cancelled())
            return std::nullopt;

        // Newlines counted up to a place in the window, onward from the last one asked.
        std::size_t countedTo = 0;
        std::uint64_t countedLines = 0;
        const auto lineNumberAt = [&](std::size_t pos) {
            if (pos < countedTo)
                countedTo = countedLines = 0;
            countedLines += countNewlines(data + countedTo, pos - countedTo, encoding);
            countedTo = pos;
            return static_cast<int>(linesBeforeWindow + countedLines + 1);
        };
        // A line that has terms `has`, its number told by `number()` when asked for.
        const auto note = [&](QString line, std::uint32_t has, bool longLine, const auto& number) {
            if (const std::uint32_t fresh = has & ~seen)
                firsts.push_back({line, number(), fresh});
            seen |= has;
            // The most terms, then the most at the start of a word, then the first.
            if (const int count = std::popcount(has); count >= best.terms) {
                const int starts = std::popcount(wordStartsIn(line, has));
                if (count > best.terms || starts > best.starts) {
                    best = {std::move(line), number(), has, count, starts, longLine};
                } else if (starts == best.starts) {
                    if (const int n = number(); n < best.number)
                        best = {std::move(line), n, has, count, starts, longLine};
                }
            }
        };
        // The line around a candidate of term `t` at `pos`: whether the term is
        // really there, and if so which terms the line has. `next`: where to
        // look on (past the line).
        const auto look = [&](std::size_t t, std::size_t pos, std::size_t& next) {
            const std::size_t bytes = finders[t]->size();
            next = pos + 1;
            if (unit == 2 && (pos & 1))
                return false; // straddles two UTF-16 code units

            // Line bounds around the candidate, capped so huge lines stay cheap.
            std::size_t from = pos;
            bool atLineStart = false;
            while (from > windowFloor && pos - from < kContextBytes) {
                if (isNewline(data, from - unit, encoding)) {
                    atLineStart = true;
                    break;
                }
                from -= unit;
            }
            if (!atLineStart && encoding == TextEncoding::Utf8) {
                while (from < pos && (static_cast<unsigned char>(data[from]) & 0xC0) == 0x80)
                    ++from;
            }
            std::size_t to = pos + bytes;
            while (to + unit <= windowLength && to - pos < kContextBytes + bytes && !isNewline(data, to, encoding))
                to += unit;

            // Cut short, it may still be a paragraph (600 bytes are 200
            // Chinese characters): long only with no line break for 8 KB.
            std::size_t lineFrom = from;
            if (!atLineStart && pos - from >= kContextBytes) {
                while (lineFrom > windowFloor && pos - lineFrom < kLongLineBytes
                    && !isNewline(data, lineFrom - unit, encoding))
                    lineFrom -= unit;
            }
            std::size_t lineTo = to;
            if (to - pos >= kContextBytes + bytes) {
                while (lineTo + unit <= windowLength && lineTo - pos < kLongLineBytes && !isNewline(data, lineTo, encoding))
                    lineTo += unit;
            }
            const bool longLine = lineTo - lineFrom >= kLongLineBytes;

            QString line = cleanLine(decode(data + from, to - from, encoding));
            const std::uint32_t has = termsIn(line);
            if (!(has & bit(t)))
                return false; // byte-level false positive (e.g. a GBK trail byte)
            next = to;
            note(std::move(line), has, longLine, [&] { return lineNumberAt(pos); });
            return true;
        };

        // Where each word not seen yet first is.
        for (std::size_t t = 0; t < finders.size(); ++t) {
            if (!finders[t] || !(wanted & ~seen & bit(t)))
                continue;
            for (std::size_t pos = finders[t]->find(data, windowLength, windowFloor), next = 0; pos != text::npos;
                pos = finders[t]->find(data, windowLength, next)) {
                if (look(t, pos, next))
                    break;
            }
        }

        // The expressions, on the whole lines of the window, decoded: up to
        // its last line break (the rest is seen with the next chunk), or all
        // of it at the end of the file and of a line longer than is waited for.
        std::size_t regexEnd = windowLength;
        if (m_regexTerms != 0 && !last) {
            std::size_t at = windowLength - (windowLength - windowFloor) % unit;
            const std::size_t stop = at > regexFloor + unit && at - regexFloor > tailMax ? at - tailMax : regexFloor;
            while (at >= stop + unit && !isNewline(data, at - unit, encoding))
                at -= unit;
            if (at >= stop + unit)
                regexEnd = at;
            else if (windowLength - regexFloor <= tailMax)
                regexEnd = regexFloor; // one line so far: none of it yet
        }
        // The lines of the window read as text, and the line breaks counted in
        // it up to a place, onward as above.
        QString decoded;
        bool haveDecoded = false;
        qsizetype countedChars = 0;
        std::uint64_t countedCharLines = 0;
        const auto regexLineNumberAt = [&](qsizetype at) {
            if (at < countedChars)
                countedChars = countedCharLines = 0;
            countedCharLines += static_cast<std::uint64_t>(QStringView(decoded).sliced(countedChars, at - countedChars).count(u'\n'));
            countedChars = at;
            return static_cast<int>(linesBeforeWindow + countNewlines(data, regexFloor, encoding) + countedCharLines + 1);
        };
        // A match of expression `t` in the text decoded: its line noted, as
        // look() does. Where to look on: past the line.
        const auto lookText = [&](std::size_t t, qsizetype at) {
            const QStringView all(decoded);
            qsizetype from = at;
            while (from > 0 && at - from < kContextChars && all[from - 1] != u'\n')
                --from;
            const bool atLineStart = from == 0 || all[from - 1] == u'\n';
            if (from < at && all[from].isLowSurrogate())
                ++from;
            qsizetype to = at;
            while (to < all.size() && to - at < 2 * kContextChars && all[to] != u'\n')
                ++to;
            if (to > at && to < all.size() && all[to].isLowSurrogate())
                --to;
            qsizetype lineFrom = from;
            if (!atLineStart) {
                while (lineFrom > 0 && at - lineFrom < kLongLineChars && all[lineFrom - 1] != u'\n')
                    --lineFrom;
            }
            qsizetype lineTo = to;
            while (lineTo < all.size() && lineTo - at < kLongLineChars && all[lineTo] != u'\n')
                ++lineTo;
            const bool longLine = lineTo - lineFrom >= kLongLineChars;
            QString line = cleanLine(all.sliced(from, to - from).toString());
            // The expression matched there, whatever it makes of the piece
            // cut out of a long line (a ^ or a lookbehind may not match it).
            const std::uint32_t has = termsIn(line) | bit(t);
            note(std::move(line), has, longLine, [&] { return regexLineNumberAt(at); });
            return lineTo;
        };
        const auto decodeLines = [&] {
            if (!haveDecoded && regexEnd > regexFloor)
                decoded = decode(data + regexFloor, regexEnd - regexFloor, encoding);
            haveDecoded = true;
        };
        for (std::size_t t = 0; t < m_terms.size(); ++t) {
            if (!(m_regexTerms & wanted & ~seen & bit(t)))
                continue;
            decodeLines();
            forEachHit(m_terms[t], decoded, 0, [&](qsizetype at, qsizetype) {
                lookText(t, at);
                return false; // the first is enough
            });
        }

        if (const std::uint32_t way = wayIn(seen, dead)) {
            // A match. A line here that has more of the terms, or more of
            // them at the start of a word ("fix", not in "prefix"), shows it
            // better; a few dozen looked at (packed code repeats them).
            const int count = std::popcount(way);
            const auto better = [&] { return best.terms < count || best.starts < count; };
            int looks = 64;
            for (std::size_t t = 0; t < m_terms.size() && better() && looks > 0; ++t) {
                if (!(way & bit(t)))
                    continue;
                if (finders[t]) {
                    for (std::size_t pos = finders[t]->find(data, windowLength, windowFloor), next = 0;
                        pos != text::npos && better() && looks > 0; pos = finders[t]->find(data, windowLength, next)) {
                        look(t, pos, next);
                        --looks;
                    }
                } else if (m_terms[t].regex) {
                    decodeLines();
                    qsizetype from = 0;
                    while (better() && looks > 0 && from <= decoded.size()) {
                        qsizetype next = -1;
                        forEachHit(m_terms[t], decoded, from, [&](qsizetype at, qsizetype) {
                            next = lookText(t, at);
                            return false;
                        });
                        if (next < 0)
                            break;
                        from = next + 1;
                        --looks;
                    }
                }
            }
            // Of the ways the file has, the one the line shown has the most
            // of ("马上|立即": the one that is on it).
            std::uint32_t shown = way;
            for (const std::uint32_t other : m_ways) {
                if ((other & seen) == other && !(other & dead)
                    && std::popcount(other & best.has) > std::popcount(shown & best.has))
                    shown = other;
            }
            const std::uint32_t lacking = shown & ~best.has;
            std::ranges::sort(firsts, {}, &First::number);
            std::vector<QString> others;
            for (First& first : firsts) {
                if (first.terms & lacking)
                    others.push_back(std::move(first.line));
            }
            ContentMatch match = matchIn(best.line, best.number, best.has, others);
            match.longLine = best.longLine;
            match.termCount = std::popcount(shown);
            return match;
        }
        if (last)
            return std::nullopt;

        // Slide the window, keeping a tail so matches across chunks are
        // found, and the lines no expression was run on yet.
        std::size_t keep = needleBytes > unit ? needleBytes - unit : 0;
        keep = std::min(keep, windowLength - windowFloor);
        if (unit == 2)
            keep &= ~std::size_t {1};
        const std::size_t tail = windowLength - regexEnd;
        keep = std::max(keep, tail);
        linesBeforeWindow += countNewlines(data, windowLength - keep, encoding);
        std::memmove(data, data + windowLength - keep, keep);
        const std::size_t got = fill(read, data + keep, chunkSize);
        if (got == 0 && tail == 0)
            return std::nullopt;
        last = got < chunkSize;
        windowLength = keep + got;
        windowFloor = 0;
        regexFloor = keep - tail;
    }
}

std::optional<ContentMatch> ContentScanner::scanFile(
    std::wstring_view path, std::int64_t maxBytes, const CancelFn& cancelled, bool* large, bool* unreadable) const
{
    if (large)
        *large = false;
    if (unreadable)
        *unreadable = false;
    win32::UniqueHandle file(::CreateFileW(win32::longPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!file.valid()) {
        if (unreadable) *unreadable = true;
        return std::nullopt;
    }

    FILE_BASIC_INFO basic {};
    constexpr DWORD kSkip = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN
        | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
    if (::GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic, sizeof basic)
        && (basic.FileAttributes & kSkip))
        return std::nullopt; // never trigger a cloud download

    LARGE_INTEGER size {};
    if (!::GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0)
        return std::nullopt;
    if (maxBytes > 0 && size.QuadPart > maxBytes) {
        if (large)
            *large = true;
        return std::nullopt;
    }
    FILE_IO_PRIORITY_HINT_INFO hint {};
    hint.PriorityHint = IoPriorityHintLow;
    ::SetFileInformationByHandle(file.get(), FileIoPriorityHintInfo, &hint, sizeof hint);

    const HANDLE h = file.get();
    const ReadFn read = [h, unreadable](char* buffer, std::size_t capacity) -> std::size_t {
        DWORD got = 0;
        const auto request = static_cast<DWORD>(std::min<std::size_t>(capacity, 1u << 30));
        if (!::ReadFile(h, buffer, request, &got, nullptr)) {
            if (unreadable) *unreadable = true;
            return 0;
        }
        return got;
    };
    return scan(read, kChunkBytes, cancelled);
}

} // namespace ws
