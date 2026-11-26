#include "Query.h"

#include "FuzzyMatch.h"
#include "PathText.h"
#include "TextUtil.h"
#include "Wtf8.h"

#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws {

std::optional<QString> normalizedTypeKeyword(QStringView text)
{
    text = text.trimmed();
    if (text.startsWith(u':'))
        text = text.mid(1);
    else if (text.endsWith(u':'))
        text = text.chopped(1);
    if (text.size() > 32 || std::ranges::any_of(text, [](QChar c) {
            return !c.isLetterOrNumber() && c != u'_' && c != u'-';
        }))
        return std::nullopt;
    return text.toString().toLower();
}

FileTypeQuery splitFileTypeQuery(const QString& text, const FileTypes& types, QStringView folderKeyword)
{
    const QStringView trimmed = QStringView(text).trimmed();
    FileTypeQuery result{trimmed.toString(), std::nullopt};
    for (qsizetype start = 0; start < trimmed.size();) {
        while (start < trimmed.size() && trimmed[start].isSpace())
            ++start;
        qsizetype end = start;
        bool quoted = false;
        bool hasQuotes = false;
        while (end < trimmed.size() && (quoted || !trimmed[end].isSpace())) {
            if (trimmed[end] == u'"') {
                quoted = !quoted;
                hasQuotes = true;
            }
            ++end;
        }
        const QStringView token = trimmed.sliced(start, end - start);
        QStringView keyword;
        if (!hasQuotes && token.startsWith(u':'))
            keyword = token.sliced(1);
        else if (!hasQuotes && token.endsWith(u':')
            && !(token.size() == 2 && token.front().isLetter())) // a drive, "C:"
            keyword = token.chopped(1);
        QList<int> selected;
        if (!keyword.isEmpty()) {
            const auto parts = keyword.toString().split(u',', Qt::KeepEmptyParts);
            for (const QString& part : parts) {
                int type = -1;
                if (!folderKeyword.isEmpty() && part.compare(folderKeyword, Qt::CaseInsensitive) == 0)
                    type = -2;
                else {
                    for (qsizetype i = 0; i < types.size(); ++i) {
                        if (!types[i].keyword.isEmpty() && part.compare(types[i].keyword, Qt::CaseInsensitive) == 0) {
                            type = int(i);
                            break;
                        }
                    }
                }
                if (type == -1 || (type == -2 && parts.size() != 1)) {
                    selected.clear(); // an unknown or mixed group stays literal, as before
                    break;
                }
                if (!selected.contains(type))
                    selected.append(type);
            }
        }
        if (!selected.isEmpty()) {
            result.type = selected.first();
            if (keyword.contains(u','))
                result.types = selected;
            result.words = (trimmed.left(start).trimmed().toString() + u' '
                + trimmed.mid(end).trimmed()).trimmed();
            return result;
        }
        start = end;
    }
    return result;
}

QString nameCondition(const QString& starts, const QString& ends, const QString& lacks)
{
    const auto plain = [](QString text) { return text.remove(u'"').trimmed(); };
    QStringList parts;
    if (const QString text = plain(starts); !text.isEmpty())
        parts.append(u"regex:\"\\A%1\""_s.arg(QRegularExpression::escape(text)));
    if (const QString text = plain(ends); !text.isEmpty())
        parts.append(u"regex:\"%1(\\.[^.]*)?\\z\""_s.arg(QRegularExpression::escape(text)));
    for (QString word : plain(lacks).split(QChar::Space, Qt::SkipEmptyParts)) {
        while (word.startsWith(u'!'))
            word.remove(0, 1);
        if (!word.isEmpty())
            parts.append(u"!regex:\"%1\""_s.arg(QRegularExpression::escape(word)));
    }
    return parts.join(QChar::Space);
}

QuickFilterQuery splitQuickFilterQuery(const QString& text, const QuickFilters& filters)
{
    QuickFilterQuery result {text, {}};
    if (std::ranges::none_of(filters, [](const QuickFilter& f) { return !f.keyword.isEmpty(); }) || !text.contains(u':'))
        return result;
    for (qsizetype start = 0; start < result.words.size();) {
        const QStringView words(result.words);
        while (start < words.size() && words[start].isSpace())
            ++start;
        qsizetype end = start;
        bool quoted = false;
        bool hasQuotes = false;
        while (end < words.size() && (quoted || !words[end].isSpace())) {
            if (words[end] == u'"') {
                quoted = !quoted;
                hasQuotes = true;
            }
            ++end;
        }
        const QStringView token = words.sliced(start, end - start);
        QStringView keyword;
        if (!hasQuotes && token.startsWith(u':'))
            keyword = token.sliced(1);
        else if (!hasQuotes && token.endsWith(u':') && !(token.size() == 2 && token.front().isLetter())) // a drive, "C:"
            keyword = token.chopped(1);
        int found = -1;
        for (qsizetype i = 0; i < filters.size() && found < 0 && !keyword.isEmpty(); ++i) {
            if (!filters[i].keyword.isEmpty() && keyword.compare(filters[i].keyword, Qt::CaseInsensitive) == 0)
                found = int(i);
        }
        if (found < 0) {
            start = end;
            continue;
        }
        if (!result.filters.contains(found))
            result.filters.append(found);
        // The token goes, the words around it as they were typed.
        const QString before = words.left(start).trimmed().toString();
        const QString after = words.mid(end).trimmed().toString();
        result.words = before.isEmpty() || after.isEmpty() ? before + after : before + u' ' + after;
        start = before.size();
    }
    return result;
}

namespace {

std::string foldedUtf8(const QString& s)
{
    return text::foldAscii(wtf8::fromUtf16(wtf8::view(s)));
}

bool isPathSeparator(QChar c)
{
    return c == u'\\' || c == u'/';
}

const QRegularExpression& separatorRegex()
{
    static const QRegularExpression re(u"[\\\\/]"_s);
    return re;
}

// The longest run of plain characters that every match of a regular
// expression contains ("disk.*" → "disk", "^IMG_\d+" → "IMG_"), for the
// quick look the plain words get before the expression itself runs (GitHub
// #411: a regex alone over every name took 1 s for 3 million). Empty when
// nothing is sure: an alternative outside parentheses, inline options, \Q,
// a hex or property escape. Only what the ASCII-folded search finds as the
// case-insensitive expression would: ASCII and letters without case (汉字).
QString requiredLiteral(QStringView pattern)
{
    if (pattern.contains(u"(?") || pattern.contains(u"(*"))
        return {}; // (?x) and the like change what is plain
    QString best;
    QString run;
    const auto endRun = [&] {
        if (run.size() > best.size())
            best = run;
        run.clear();
    };
    const auto isAsciiAlnum = [](QChar c) { return c.unicode() < 0x80 && c.isLetterOrNumber(); };
    int depth = 0;
    qsizetype i = 0;
    while (i < pattern.size()) {
        const QChar c = pattern[i];
        std::optional<QChar> plain; // the atom's character, when it is one
        if (c == u'\\') {
            if (i + 1 >= pattern.size())
                return {};
            const QChar next = pattern[i + 1];
            if (isAsciiAlnum(next)) {
                // \d \w \b \A ... are no characters; \x41, \p{L}, \1 are, but not plainly.
                if (next.isDigit() || QStringView(u"xpPNuocgkQE").contains(next))
                    return {};
            } else {
                plain = next;
            }
            i += 2;
        } else if (c == u'[') {
            // A class: skipped to its end (a ] first in it is one of its characters).
            qsizetype j = i + 1;
            if (j < pattern.size() && pattern[j] == u'^')
                ++j;
            if (j < pattern.size() && pattern[j] == u']')
                ++j;
            for (; j < pattern.size() && pattern[j] != u']'; ++j) {
                if (pattern[j] == u'\\')
                    ++j;
                else if (pattern[j] == u'[' && j + 1 < pattern.size() && pattern[j + 1] == u':')
                    j = std::max(j, pattern.indexOf(u":]", j + 2) + 1);
            }
            if (j >= pattern.size())
                return {};
            i = j + 1;
        } else if (c == u'(') {
            ++depth; // what is inside may be optional or one of alternatives: not counted
            endRun();
            ++i;
            continue;
        } else if (c == u')') {
            depth = std::max(0, depth - 1);
            ++i;
        } else if (c == u'|') {
            if (depth == 0)
                return {}; // either side: neither is sure
            ++i;
            continue;
        } else if (c == u'.' || c == u'^' || c == u'$') {
            ++i;
        } else if (c == u'*' || c == u'+' || c == u'?' || c == u'{') {
            return {}; // a quantifier with nothing before it: not a pattern to guess at
        } else {
            plain = c;
            ++i;
        }
        // What follows the atom: none of it may be there, or more of it.
        bool optional = false;
        bool repeated = false;
        if (i < pattern.size()) {
            const QChar q = pattern[i];
            if (q == u'*' || q == u'?' || q == u'+') {
                optional = q != u'+';
                repeated = true;
                ++i;
            } else if (q == u'{') {
                const qsizetype close = pattern.indexOf(u'}', i);
                const QStringView inside = close < 0 ? QStringView() : pattern.sliced(i + 1, close - i - 1);
                const qsizetype comma = inside.indexOf(u',');
                bool ok = false;
                const int least = (comma < 0 ? inside : inside.first(comma)).toInt(&ok);
                if (!ok)
                    return {}; // a { PCRE reads as itself: not worth the guess
                optional = least == 0;
                repeated = true;
                i = close + 1;
            }
            if (repeated && i < pattern.size() && (pattern[i] == u'?' || pattern[i] == u'+'))
                ++i; // lazy or possessive
        }
        const bool sure = depth == 0 && plain && !optional
            && (plain->unicode() < 0x80 || (!plain->isLower() && !plain->isUpper() && !plain->isTitleCase())) && !plain->isSurrogate();
        if (sure)
            run.append(*plain);
        if (!sure || repeated)
            endRun();
    }
    endRun();
    return best;
}

// A regular expression on an index name, decoded into one buffer per
// thread rather than two new strings a name.
bool regexMatches(const QRegularExpression& expression, std::string_view name)
{
    thread_local std::u16string utf16;
    utf16.clear();
    wtf8::decodeAppend(utf16, name);
    return expression.matchView(QStringView(utf16.data(), static_cast<qsizetype>(utf16.size()))).hasMatch();
}

} // namespace

bool nameMatchesAny(std::string_view name, const std::vector<std::string>& foldedPatterns) noexcept
{
    return std::ranges::any_of(foldedPatterns, [&](const std::string& p) { return text::globMatch(name, p); });
}

namespace {

constexpr QChar kWideSlash(0xFF0F); // ／, as a Chinese keyboard layout types /
constexpr QChar kWideBackslash(0xFF3C); // ＼

bool isScopeSeparator(QChar c)
{
    return c == u'/' || c == u'\\' || c == kWideSlash || c == kWideBackslash;
}

// A name a folder can have (as Windows allows them, and no "." or "..").
bool isFolderName(QStringView name)
{
    if (name.isEmpty() || name == u"." || name == u".." || name.front().isSpace() || name.back().isSpace())
        return false;
    return std::ranges::all_of(name, [](QChar c) { return c.unicode() >= 32 && !QStringView(u"<>\"|?*:/\\").contains(c); });
}

// "工作/abc/" -> {"工作", "abc"}; empty when a part is no folder's name.
QStringList folderNames(QStringView text)
{
    QStringList names;
    qsizetype from = 0;
    for (qsizetype i = 0; i <= text.size(); ++i) {
        if (i < text.size() && !isScopeSeparator(text[i]))
            continue;
        const QStringView part = text.sliced(from, i - from);
        from = i + 1;
        if (part.isEmpty())
            continue;
        if (!isFolderName(part))
            return {};
        names.append(part.toString());
    }
    return names;
}

// A word as queryWords() takes it, and where it is in the text.
struct Piece {
    qsizetype start;
    qsizetype end;
    bool quoted;
};

std::vector<Piece> pieces(const QString& text)
{
    std::vector<Piece> found;
    bool quoted = false;
    qsizetype start = -1;
    bool hadQuote = false;
    for (qsizetype i = 0; i <= text.size(); ++i) {
        const bool atEnd = i == text.size();
        if (!atEnd && text[i] == u'"') {
            quoted = !quoted;
            hadQuote = true;
            if (start < 0)
                start = i;
            continue;
        }
        if (atEnd || (!quoted && text[i].isSpace())) {
            if (start >= 0)
                found.push_back({start, i, hadQuote});
            start = -1;
            hadQuote = false;
            continue;
        }
        if (start < 0)
            start = i;
    }
    return found;
}

// A path from a drive or share starting at `from` ("D:\My Files\ 报告",
// "\\nas\docs\", "%USERPROFILE%\Documents\", or quoted), up to the first
// separator followed by a space or the end: no name starts with a space, and
// the path is to a folder. How long it is, or 0.
qsizetype folderPathAt(const QString& text, qsizetype from, QString& path)
{
    const QStringView rest = QStringView(text).sliced(from);
    const auto separatorEnds = [&](qsizetype i) {
        return isScopeSeparator(rest[i]) && (i + 1 == rest.size() || rest[i + 1].isSpace());
    };
    if (rest.startsWith(u'"')) {
        const qsizetype close = rest.indexOf(u'"', 1);
        if (close < 2 || !isScopeSeparator(rest[close - 1]) || (close + 1 < rest.size() && !rest[close + 1].isSpace()))
            return 0;
        path = pathtext::pathFromText(rest.sliced(1, close - 1).toString());
        return path.isEmpty() ? 0 : close + 1;
    }
    const bool drive = rest.size() >= 3 && rest[0].isLetter() && rest[0].unicode() < 128 && rest[1] == u':'
        && isScopeSeparator(rest[2]);
    if (!drive && !rest.startsWith(u"\\\\") && !rest.startsWith(u'%'))
        return 0;
    for (qsizetype i = 2; i < rest.size(); ++i) {
        if (rest[i] == u'"')
            return 0;
        if (separatorEnds(i)) {
            QString typed = rest.first(i + 1).toString();
            typed.replace(kWideSlash, u'/').replace(kWideBackslash, u'\\');
            path = pathtext::pathFromText(typed);
            return path.isEmpty() ? 0 : i + 1;
        }
    }
    return 0;
}

// A word that says which files to read (ContentQuery::files): "ext:py",
// "*.md", "报告*.docx", "!*.min.js".
bool isFileWord(QStringView word)
{
    if (word.size() > 4 && word.startsWith(u"ext:", Qt::CaseInsensitive))
        return true;
    if (word.size() > 1 && word.startsWith(u'!'))
        word = word.sliced(1);
    static constexpr QStringView kNotInNames = u"<>:\"|/\\";
    return word.contains(u'*') && word.contains(u'.')
        && std::ranges::none_of(word, [](QChar c) { return kNotInNames.contains(c); })
        && std::ranges::any_of(word, [](QChar c) { return c != u'*' && c != u'?' && c != u'.'; }); // "*.*" is every file
}

ContentQuery splitContentFolders(const QString& text, bool regexMode);

} // namespace

ContentQuery splitContentQuery(const QString& text, bool regexMode)
{
    QStringList files;
    QString rest;
    if (!regexMode) {
        // Taken out with the space after each: what is left may be a line of
        // code, searched for as it is.
        qsizetype kept = 0;
        for (const Piece& p : pieces(text)) {
            const QStringView word = QStringView(text).sliced(p.start, p.end - p.start);
            if (p.quoted || !isFileWord(word))
                continue;
            files.append(word.toString());
            rest += QStringView(text).sliced(kept, p.start - kept);
            kept = p.end;
            while (kept < text.size() && text[kept].isSpace())
                ++kept;
        }
        rest += QStringView(text).sliced(kept);
    }
    if (files.isEmpty()) {
        ContentQuery query = splitContentFolders(text, regexMode);
        query.withoutFiles = text;
        return query;
    }
    ContentQuery query = splitContentFolders(rest, false);
    query.files = std::move(files);
    query.withoutFiles = rest.trimmed();
    return query;
}

namespace {

ContentQuery splitContentFolders(const QString& text, bool regexMode)
{
    ContentQuery query;
    if (text.trimmed().isEmpty()) {
        query.words = text;
        return query;
    }
    if (regexMode || looksLikeCode(text.trimmed())) {
        // A folder first is where to look, whatever its name has in it
        // ("D:\Program Files (x86)\ 词", as Explorer's "search here" types it);
        // only the rest may be code.
        qsizetype lead = 0;
        while (text[lead].isSpace())
            ++lead;
        QString path;
        const qsizetype length = folderPathAt(text, lead, path);
        if (length == 0) {
            query.words = text;
            return query;
        }
        const QString rest = text.sliced(lead + length);
        if (regexMode || looksLikeCode(rest.trimmed())) {
            query.folders.push_back({text.sliced(lead, length), path, {}});
            query.words = rest.trimmed();
            return query;
        }
        // Words after it: on below, as for any folder.
    }
    // The words, with what names a folder taken out: the stretches kept.
    QString words;
    qsizetype kept = 0;
    const auto takeOut = [&](qsizetype start, qsizetype end) {
        words += QStringView(text).sliced(kept, start - kept);
        words += u' ';
        kept = end;
    };

    // Paths first: theirs may have spaces in them.
    std::vector<std::pair<qsizetype, qsizetype>> paths;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (i > 0 && !text[i - 1].isSpace())
            continue;
        QString path;
        if (const qsizetype length = folderPathAt(text, i, path); length > 0) {
            query.folders.push_back({text.sliced(i, length), path, {}});
            paths.push_back({i, i + length});
            i += length - 1;
        }
    }
    const std::vector<Piece> found = pieces(text);
    // The last word that is not a path: the only place for "/abc".
    const Piece* last = nullptr;
    for (const Piece& p : found) {
        if (std::ranges::none_of(paths, [&](const auto& span) { return p.start >= span.first && p.start < span.second; }))
            last = &p;
    }
    qsizetype others = 0; // the words that stay words
    std::vector<std::pair<const Piece*, QStringList>> named;
    for (const Piece& p : found) {
        if (std::ranges::any_of(paths, [&](const auto& span) { return p.start >= span.first && p.start < span.second; }))
            continue;
        const QStringView word = QStringView(text).sliced(p.start, p.end - p.start);
        const bool plain = !p.quoted && !word.startsWith(u'!') && !word.contains(u'*') && !word.contains(u'?')
            && !word.startsWith(u"ext:", Qt::CaseInsensitive);
        QStringList names;
        if (plain && isScopeSeparator(word.back()))
            names = folderNames(word);
        else if (plain && &p == last && word.size() > 1 && (word.front() == u'/' || word.front() == kWideSlash)
            && std::ranges::none_of(word.sliced(1), isScopeSeparator))
            names = folderNames(word.sliced(1));
        if (names.isEmpty())
            ++others;
        else
            named.push_back({&p, std::move(names)});
    }
    // "/abc" alone is something to find ("/usr"), not where to look.
    if (others == 0 && !named.empty() && !isScopeSeparator(QStringView(text).sliced(named.back().first->end - 1).front()))
        named.pop_back();

    // Taken out in the order they are in the text.
    std::vector<std::pair<qsizetype, qsizetype>> spans = paths;
    for (const auto& [p, names] : named) {
        query.folders.push_back({text.sliced(p->start, p->end - p->start), {}, names});
        spans.push_back({p->start, p->end});
    }
    std::ranges::sort(spans);
    std::ranges::stable_sort(query.folders, {}, [&](const ContentFolderSpec& f) { return text.indexOf(f.typed); });
    for (const auto& [start, end] : spans)
        takeOut(start, end);
    if (query.folders.empty()) {
        query.words = text;
        return query;
    }
    words += QStringView(text).sliced(kept);
    query.words = words.trimmed();
    return query;
}

} // namespace

std::optional<std::uint64_t> parseSizeValue(QStringView s, std::uint64_t bareUnit)
{
    s = s.trimmed();
    qsizetype digits = 0;
    while (digits < s.size() && (s[digits].isDigit() || s[digits] == u'.'))
        ++digits;
    if (digits == 0)
        return std::nullopt;
    bool ok = false;
    const double value = s.first(digits).toDouble(&ok);
    if (!ok || value < 0)
        return std::nullopt;
    const QString unit = s.sliced(digits).trimmed().toString().toLower();
    double scale = 1;
    if (unit.isEmpty())
        scale = static_cast<double>(bareUnit);
    else if (unit == u"b")
        scale = 1;
    else if (unit == u"k" || unit == u"kb")
        scale = 1024.0;
    else if (unit == u"m" || unit == u"mb")
        scale = 1024.0 * 1024;
    else if (unit == u"g" || unit == u"gb")
        scale = 1024.0 * 1024 * 1024;
    else if (unit == u"t" || unit == u"tb")
        scale = 1024.0 * 1024 * 1024 * 1024;
    else
        return std::nullopt;
    const double bytes = value * scale;
    return bytes >= 1.8e19 ? std::numeric_limits<std::uint64_t>::max() : static_cast<std::uint64_t>(bytes + 0.5);
}

std::optional<std::pair<QDateTime, QDateTime>> parseDateValue(QStringView s)
{
    const QStringList parts = s.trimmed().toString().split(QRegularExpression(u"[-/.]"_s), Qt::SkipEmptyParts);
    if (parts.isEmpty() || parts.size() > 3)
        return std::nullopt;
    int numbers[3] = {0, 1, 1};
    for (qsizetype i = 0; i < parts.size(); ++i) {
        bool ok = false;
        numbers[i] = parts[i].toInt(&ok);
        if (!ok)
            return std::nullopt;
    }
    if (numbers[0] < 1980 || numbers[0] > 2200)
        return std::nullopt;
    const QDate date(numbers[0], numbers[1], numbers[2]);
    if (!date.isValid())
        return std::nullopt;
    const QDate end = parts.size() == 1 ? date.addYears(1) : parts.size() == 2 ? date.addMonths(1) : date.addDays(1);
    return std::pair {date.startOfDay(), end.startOfDay()};
}

namespace {

FileTime fileTimeAt(const QDateTime& when)
{
    const qint64 secs = when.toSecsSinceEpoch();
    return secs <= 0 ? 0 : secs >= 0xFFFF'FFFFll ? 0xFFFF'FFFFu : static_cast<FileTime>(secs);
}

} // namespace

bool parseSizeToken(QStringView token, EntryFilter& filter)
{
    QStringView s = token.sliced(5).trimmed(); // after "size:"
    if (s.isEmpty())
        return false;
    const QString word = s.toString().toLower();
    // Everything's names for the usual sizes.
    struct Bucket {
        const char16_t* name;
        std::uint64_t from;
        std::uint64_t to; // exclusive; 0: no bound
    };
    static constexpr Bucket kBuckets[] = {
        {u"empty", 0, 1},
        {u"tiny", 0, 10 << 10},
        {u"small", 10 << 10, 100 << 10},
        {u"medium", 100 << 10, 1 << 20},
        {u"large", 1 << 20, 16 << 20},
        {u"huge", 16 << 20, 128 << 20},
        {u"gigantic", 128 << 20, 0},
    };
    for (const Bucket& b : kBuckets) {
        if (word == b.name) {
            filter.minSize = std::max(filter.minSize, packSize(b.from));
            if (b.to != 0)
                filter.maxSize = std::min(filter.maxSize, packSize(b.to - 1));
            return true;
        }
    }
    // A range: "1mb..5mb", "1mb-5mb" (the dash not inside a number's unit).
    qsizetype dots = s.indexOf(u"..");
    qsizetype dash = dots >= 0 ? -1 : s.indexOf(u'-', 1);
    if (dots >= 0 || dash >= 0) {
        const qsizetype at = dots >= 0 ? dots : dash;
        const auto from = parseSizeValue(s.first(at));
        const auto to = parseSizeValue(s.sliced(at + (dots >= 0 ? 2 : 1)));
        if (!from || !to || *from > *to)
            return false;
        filter.minSize = std::max(filter.minSize, packSize(*from));
        filter.maxSize = std::min(filter.maxSize, packSize(*to));
        return true;
    }
    enum { AtLeast, Above, AtMost, Below } how = AtLeast;
    if (s.startsWith(u">=")) {
        s = s.sliced(2);
    } else if (s.startsWith(u"<=")) {
        s = s.sliced(2);
        how = AtMost;
    } else if (s.startsWith(u'>')) {
        s = s.sliced(1);
        how = Above;
    } else if (s.startsWith(u'<')) {
        s = s.sliced(1);
        how = Below;
    } else if (s.startsWith(u'=')) {
        s = s.sliced(1);
    }
    const auto value = parseSizeValue(s);
    if (!value)
        return false;
    switch (how) {
    case AtLeast:
        filter.minSize = std::max(filter.minSize, packSize(*value));
        break;
    case Above:
        filter.minSize = std::max(filter.minSize, packSize(*value + 1));
        break;
    case AtMost:
        filter.maxSize = std::min(filter.maxSize, packSize(*value));
        break;
    case Below:
        if (*value == 0)
            return false;
        filter.maxSize = std::min(filter.maxSize, packSize(*value - 1));
        break;
    }
    return true;
}

bool parseDateToken(QStringView token, const QDateTime& now, EntryFilter& filter)
{
    QStringView s = token.sliced(3).trimmed(); // after "dm:"
    if (s.isEmpty())
        return false;
    const QString word = s.toString().toLower();
    const QDate today = now.date();
    const auto since = [&](const QDateTime& from) {
        filter.after = std::max(filter.after, fileTimeAt(from));
        return true;
    };
    const auto between = [&](const QDateTime& from, const QDateTime& to) {
        filter.after = std::max(filter.after, fileTimeAt(from));
        const FileTime before = fileTimeAt(to);
        filter.before = filter.before == 0 ? before : std::min(filter.before, before);
        return true;
    };
    if (word == u"today")
        return since(today.startOfDay());
    if (word == u"yesterday")
        return between(today.addDays(-1).startOfDay(), today.startOfDay());
    if (word == u"thisweek")
        return since(today.addDays(1 - today.dayOfWeek()).startOfDay());
    if (word == u"thismonth")
        return since(QDate(today.year(), today.month(), 1).startOfDay());
    if (word == u"thisyear")
        return since(QDate(today.year(), 1, 1).startOfDay());
    // "7d", "2w", "3m", "1y": within the last so many.
    if (word.size() >= 2 && word.back().isLetter()) {
        bool ok = false;
        const int n = word.first(word.size() - 1).toInt(&ok);
        if (ok && n >= 0) {
            switch (word.back().unicode()) {
            case u'd':
                return since(now.addDays(-n));
            case u'w':
                return since(now.addDays(-7LL * n));
            case u'm':
                return since(now.addMonths(-n));
            case u'y':
                return since(now.addYears(-n));
            default:
                break;
            }
        }
    }
    if (const qsizetype dots = s.indexOf(u".."); dots >= 0) {
        const auto from = parseDateValue(s.first(dots));
        const auto to = parseDateValue(s.sliced(dots + 2));
        if (!from || !to || from->first > to->first)
            return false;
        return between(from->first, to->second);
    }
    if (s.startsWith(u">=") || s.startsWith(u'>')) {
        const bool inclusive = s.startsWith(u">=");
        const auto value = parseDateValue(s.sliced(inclusive ? 2 : 1));
        return value && since(inclusive ? value->first : value->second);
    }
    if (s.startsWith(u"<=") || s.startsWith(u'<')) {
        const bool inclusive = s.startsWith(u"<=");
        const auto value = parseDateValue(s.sliced(inclusive ? 2 : 1));
        if (!value)
            return false;
        const FileTime before = fileTimeAt(inclusive ? value->second : value->first);
        filter.before = filter.before == 0 ? before : std::min(filter.before, before);
        return true;
    }
    const auto value = parseDateValue(s);
    return value && between(value->first, value->second);
}

QString sizeText(std::uint64_t bytes)
{
    static constexpr const char16_t* kUnits[] = {u"B", u"KB", u"MB", u"GB", u"TB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1000 && unit + 1 < std::size(kUnits)) {
        value /= 1024;
        ++unit;
    }
    if (unit == 0)
        return QString::number(bytes) + u" B";
    // Three figures: 1.23 MB, 12.3 MB, 123 MB.
    const int decimals = value < 10 ? 2 : value < 100 ? 1 : 0;
    return QString::number(value, 'f', decimals) + u' ' + kUnits[unit];
}

ParsedQuery parseQuery(const QString& text, bool regexTerms, bool allowFuzzyPrefix)
{
    ParsedQuery q;
    qsizetype start = 0;
    const auto spans = pieces(text);
    for (const Piece& piece : spans) {
        if (piece.quoted || QStringView(text).sliced(piece.start, piece.end - piece.start) != u"|")
            continue;
        const QString branch = text.sliced(start, piece.start - start).trimmed();
        if (branch.isEmpty() || q.alternatives.size() >= 32) {
            q.error = branch.isEmpty() ? QObject::tr("“|”两侧都需要搜索条件") : QObject::tr("搜索条件过多");
            return q;
        }
        q.alternatives.push_back(parseQuery(branch, regexTerms, allowFuzzyPrefix));
        start = piece.end;
    }
    if (!q.alternatives.empty()) {
        const QString branch = text.sliced(start).trimmed();
        if (branch.isEmpty()) {
            q.error = QObject::tr("“|”两侧都需要搜索条件");
            return q;
        }
        q.alternatives.push_back(parseQuery(branch, regexTerms, allowFuzzyPrefix));
        for (const ParsedQuery& group : q.alternatives) {
            q.highlights.append(group.highlights);
            q.fuzzyHighlights.append(group.fuzzyHighlights);
            if (!group.error.isEmpty())
                q.error = group.error;
        }
        q.highlights.removeDuplicates();
        q.fuzzyHighlights.removeDuplicates();
        return q;
    }
    // A path typed or pasted whole ("C:\Users\me\Tencent Files\x") is one
    // term: its spaces are in the names, not between terms.
    const pathtext::TypedPath typed = regexTerms ? pathtext::TypedPath() : pathtext::splitTyped(text);
    struct Token { QString text; bool quotedLiteral; };
    std::vector<Token> tokens;
    if (typed.folder.isEmpty()) {
        for (const Piece& span : spans) {
            const QString raw = text.sliced(span.start, span.end - span.start);
            const QString word = queryWords(raw).value(0);
            if (!word.isEmpty())
                tokens.push_back({word, raw.startsWith(u'"')});
        }
    } else {
        tokens.push_back({(typed.folder.endsWith(u'\\') ? typed.folder : typed.folder + u'\\') + typed.name.trimmed(), false});
    }
    const QDateTime now = QDateTime::currentDateTime();
    for (auto [token, quotedLiteral] : tokens) {
        if (token.startsWith(u"size:", Qt::CaseInsensitive) && parseSizeToken(token, q.filter))
            continue;
        if (token.startsWith(u"dm:", Qt::CaseInsensitive) && parseDateToken(token, now, q.filter))
            continue;
        if (token.startsWith(u"ext:", Qt::CaseInsensitive)) {
            const QStringList exts = token.mid(4).split(QRegularExpression(u"[,;|]"_s), Qt::SkipEmptyParts);
            for (QString ext : exts) {
                while (ext.startsWith(u'*') || ext.startsWith(u'.'))
                    ext.remove(0, 1);
                if (!ext.isEmpty())
                    q.extensions.push_back(foldedUtf8(ext));
            }
            continue;
        }

        QueryTerm term;
        term.quoted = quotedLiteral;
        if (token.size() > 1 && token.startsWith(u'!')) {
            term.negated = true;
            token.remove(0, 1);
        }

        const qsizetype regexPrefix = quotedLiteral ? 0
            : token.startsWith(u"regex:", Qt::CaseInsensitive) ? 6
            : token.startsWith(u"re:", Qt::CaseInsensitive) ? 3 : 0;
        const bool prefixed = regexPrefix != 0;
        const bool regex = prefixed || regexTerms;
        const bool anchored = !quotedLiteral && (token.startsWith(u'^') || token.endsWith(u'$'));
        if (regex || anchored) {
            QString pattern;
            if (regex) {
                pattern = prefixed ? token.mid(regexPrefix) : token;
            } else {
                const bool prefix = token.startsWith(u'^');
                const bool suffix = token.endsWith(u'$');
                token = token.mid(prefix ? 1 : 0, token.size() - (prefix ? 1 : 0) - (suffix ? 1 : 0));
                pattern = (prefix ? u"\\A"_s : QString()) + QRegularExpression::escape(token)
                    + (suffix ? u"\\z"_s : QString());
            }
            term.regex = true;
            term.text = foldedUtf8(token);
            term.expression = QRegularExpression(u"(*LIMIT_MATCH=10000)(*LIMIT_DEPTH=100)"_s + pattern,
                QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
            if ((regex && pattern.isEmpty()) || !term.expression.isValid()) {
                q.error = QObject::tr("正则表达式无效：%1").arg(pattern.isEmpty() ? QObject::tr("请输入表达式")
                    : term.expression.errorString());
                return q;
            }
            term.expression.optimize();
            term.literal = foldedUtf8(regex ? requiredLiteral(pattern) : token);
            if (!regex && !term.negated && !token.isEmpty())
                q.highlights.append(token);
            q.terms.push_back(std::move(term));
            continue;
        }

        if (!quotedLiteral && allowFuzzyPrefix) {
            const qsizetype fuzzyPrefix = token.startsWith(u"fuzzy:", Qt::CaseInsensitive) ? 6
                : token.startsWith(u"fz:", Qt::CaseInsensitive) ? 3 : 0;
            if (fuzzyPrefix != 0) {
                token = token.mid(fuzzyPrefix);
                if (token.isEmpty())
                    continue;
                term.fuzzy = true;
            }
        }

        if (std::any_of(token.cbegin(), token.cend(), isPathSeparator)) {
            QStringList parts = token.split(separatorRegex());
            const QString last = parts.takeLast();
            for (const QString& part : parts) {
                if (!part.isEmpty())
                    term.ancestors.push_back(foldedUtf8(part));
            }
            token = last;
            if (token.isEmpty())
                term.fuzzy = false;
        }

        term.wildcard = !term.fuzzy && (token.contains(u'*') || token.contains(u'?'));
        term.text = foldedUtf8(token);
        if (term.wildcard) {
            std::size_t run = 0; // where the literal run between wildcards began
            for (std::size_t i = 0; i <= term.text.size(); ++i) {
                if (i == term.text.size() || term.text[i] == '*' || term.text[i] == '?') {
                    if (i - run > term.literal.size())
                        term.literal = term.text.substr(run, i - run);
                    run = i + 1;
                }
            }
        }
        if (term.text.empty() && term.ancestors.empty())
            continue;
        if (!term.negated && !term.wildcard && !token.isEmpty()) {
            q.highlights.append(token);
            if (term.fuzzy)
                q.fuzzyHighlights.append(token);
        }
        q.terms.push_back(std::move(term));
    }
    return q;
}

bool isFuzzyEligible(const ParsedQuery& query) noexcept
{
    if (!query.error.isEmpty())
        return false;
    if (!query.alternatives.empty())
        return std::ranges::any_of(query.alternatives, [](const ParsedQuery& b) { return isFuzzyEligible(b); });
    return std::ranges::any_of(query.terms, [](const QueryTerm& t) {
        return !t.negated && !t.wildcard && !t.regex && !t.quoted && !t.fuzzy
            && fuzzy::isEligibleAutoTerm(t.text);
    });
}

bool ParsedQuery::hasFilters() const noexcept
{
    return filter.any() || std::ranges::any_of(alternatives, [](const ParsedQuery& q) { return q.hasFilters(); });
}

std::vector<ParsedQuery> homophoneQueries(const ParsedQuery& query)
{
    if (!query.alternatives.empty() || !query.error.isEmpty()
        || std::ranges::any_of(query.terms, [](const QueryTerm& t) { return t.regex || t.fuzzy; }))
        return {};
    constexpr std::size_t kMostQueries = 8; // each is a search over the whole index
    // Each term to read, in pieces: a Chinese character's readings, or a run
    // of other characters as it is (one "reading").
    struct Spoken {
        std::size_t term;
        std::vector<QStringList> pieces;
    };
    std::vector<Spoken> spoken;
    for (std::size_t i = 0; i < query.terms.size(); ++i) {
        const QueryTerm& t = query.terms[i];
        if (t.negated || t.wildcard || t.text.empty())
            continue;
        std::vector<QStringList> pieces;
        QString run;
        bool han = false;
        for (const char32_t c : wtf8::toQString(t.text).toUcs4()) {
            const pinyin::Readings readings = pinyin::readings(c);
            if (readings.count == 0) {
                run.append(QString::fromUcs4(&c, 1));
                continue;
            }
            if (!run.isEmpty())
                pieces.push_back({std::exchange(run, QString())});
            QStringList options;
            for (const std::string_view reading : readings)
                options.append(QString::fromLatin1(reading));
            pieces.push_back(std::move(options));
            han = true;
        }
        if (!han)
            continue;
        if (!run.isEmpty())
            pieces.push_back({run});
        spoken.push_back({i, std::move(pieces)});
    }
    if (spoken.empty())
        return {};

    // The characters with more than one reading, and which reading each takes
    // in the query being made: counted up as an odometer (the first character
    // slowest), so the first query is every character's most common reading.
    std::vector<std::pair<std::size_t, std::size_t>> several; // in spoken, piece
    for (std::size_t s = 0; s < spoken.size(); ++s) {
        for (std::size_t p = 0; p < spoken[s].pieces.size(); ++p) {
            if (spoken[s].pieces[p].size() > 1)
                several.push_back({s, p});
        }
    }
    std::vector<qsizetype> chosen(several.size(), 0);
    std::vector<ParsedQuery> queries;
    for (;;) {
        ParsedQuery q = query;
        q.highlights.clear();
        std::vector<QString> texts(spoken.size());
        for (std::size_t s = 0; s < spoken.size(); ++s) {
            QString text;
            for (std::size_t p = 0; p < spoken[s].pieces.size(); ++p) {
                const auto it = std::find(several.begin(), several.end(), std::pair {s, p});
                const qsizetype which = it == several.end() ? 0 : chosen[static_cast<std::size_t>(it - several.begin())];
                text += spoken[s].pieces[p][which];
            }
            q.terms[spoken[s].term].text = foldedUtf8(text);
            texts[s] = text;
        }
        // The highlights as parseQuery gives them (one per term that is
        // neither an exclusion nor a wildcard, in order): the pinyin for the
        // terms read, the rest as they were.
        qsizetype k = 0;
        for (std::size_t i = 0; i < q.terms.size(); ++i) {
            const QueryTerm& t = q.terms[i];
            if (t.negated || t.wildcard || t.text.empty())
                continue;
            const auto it = std::find_if(spoken.begin(), spoken.end(), [&](const Spoken& s) { return s.term == i; });
            if (it != spoken.end())
                q.highlights.append(texts[static_cast<std::size_t>(it - spoken.begin())]);
            else if (k < query.highlights.size())
                q.highlights.append(query.highlights[k]);
            ++k;
        }
        queries.push_back(std::move(q));
        if (queries.size() >= kMostQueries)
            break;
        // The next combination, the last character's reading changing fastest.
        std::size_t d = several.size();
        while (d > 0) {
            --d;
            if (++chosen[d] < spoken[several[d].first].pieces[several[d].second].size())
                break;
            chosen[d] = 0;
            if (d == 0)
                return queries; // every combination made
        }
        if (several.empty())
            break;
    }
    return queries;
}

std::vector<ParsedQuery> parseConditions(const QStringList& texts)
{
    std::vector<ParsedQuery> conditions;
    for (const QString& text : texts) {
        ParsedQuery condition = parseQuery(text);
        if (condition.error.isEmpty() && !condition.isEmpty())
            conditions.push_back(std::move(condition));
    }
    return conditions;
}

NameMatcher::NameMatcher(const ParsedQuery& query, bool autoFuzzy)
    : m_filesOnly(query.filesOnly)
    , m_hidden(query.hiddenFolders)
    , m_extensions(query.extensions)
    , m_excluded(query.excludedExtensions)
    , m_excludedNames(query.excludedFileNames)
{
    m_invalid = !query.error.isEmpty();
    m_filter = query.filter;
    for (const ParsedQuery& branch : query.alternatives) {
        m_alternatives.emplace_back(branch, autoFuzzy);
        m_hasFuzzy = m_hasFuzzy || m_alternatives.back().hasFuzzyTerms();
    }
    for (const ParsedQuery& condition : query.conditions)
        m_conditions.emplace_back(condition, false); // saved filters stay deterministic
    for (const std::string& e : m_excluded)
        m_excludedLongest = std::max(m_excludedLongest, e.size());
    for (const auto& t : query.terms) {
        if (t.negated) {
            m_negative.push_back(t); // plain exclusions stay literal; only explicit !fz: is fuzzy
            if (t.fuzzy)
                m_hasFuzzy = true;
            continue;
        }
        const bool explicitFuzzy = t.fuzzy && !t.text.empty();
        const bool termAutoFuzzy = autoFuzzy && !explicitFuzzy && !t.wildcard && !t.regex && !t.quoted
            && fuzzy::isEligibleAutoTerm(t.text);
        const std::string_view text
            = t.wildcard || t.regex || explicitFuzzy ? std::string_view() : std::string_view(t.text);
        Positive p {t, pinyin::Matcher(text), pinyin::Matcher(text, true), {}, false, explicitFuzzy, termAutoFuzzy,
            (explicitFuzzy || termAutoFuzzy) ? fuzzy::codePointCount(t.text) : 0};
        p.initials = !explicitFuzzy && text.size() >= 2 && text.size() <= 12
            && std::ranges::all_of(text, [](unsigned char c) {
                   return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
               });
        if (explicitFuzzy || termAutoFuzzy)
            m_hasFuzzy = true;
        for (const std::string& segment : t.ancestors)
            p.folders.emplace_back(segment);
        m_positive.push_back(std::move(p));
    }
    // Longest term first: it rejects the most names before the others are tried.
    std::stable_sort(m_positive.begin(), m_positive.end(),
        [](const Positive& a, const Positive& b) { return a.term.text.size() > b.term.text.size(); });

    // With something else to find in the name (another term, an extension),
    // a plain word may be a folder's instead: "src app" finds src\app\.
    if (m_positive.size() + (m_extensions.empty() ? 0 : 1) >= 2) {
        for (std::size_t i = 0; i < m_positive.size() && i < 64; ++i) {
            const QueryTerm& t = m_positive[i].term;
            if (!t.wildcard && !t.regex && !t.fuzzy && !t.text.empty() && t.ancestors.empty())
                m_folderTerms |= std::uint64_t {1} << i;
        }
    }
    for (std::size_t i = 0; i < m_positive.size() && i < 64; ++i) {
        const QueryTerm& t = m_positive[i].term;
        if (!t.wildcard && !t.regex && !t.text.empty() && !(m_folderTerms >> i & 1)) {
            m_quick = static_cast<int>(i);
            break;
        }
    }
    m_quickDecides = m_quick >= 0 && m_positive.size() == 1 && m_negative.empty() && m_extensions.empty();
}

namespace {

NameMatcher::MatchResult betterMatch(NameMatcher::MatchResult a, NameMatcher::MatchResult b) noexcept
{
    if (!a.matched())
        return b;
    if (!b.matched())
        return a;
    if (a.fuzzy != b.fuzzy)
        return a.fuzzy ? b : a;
    return a.score >= b.score ? a : b;
}

bool programInitials(std::string_view name, std::size_t extLength, std::string_view initials)
{
    if (extLength != 3 || name.size() <= extLength + 1)
        return false;
    const std::string_view extension = name.substr(name.size() - extLength);
    constexpr std::string_view programs[] {"exe", "com", "lnk", "bat", "cmd", "ps1"};
    if (!std::ranges::any_of(programs, [&](std::string_view ext) { return text::equalsFolded(extension, ext); }))
        return false;
    return text::wordInitialsMatch(name.substr(0, name.size() - extLength - 1), initials);
}

// Index names are stored with padding, so they can use the SIMD search.
template <bool Padded> std::size_t find(std::string_view haystack, std::string_view folded)
{
    if constexpr (Padded)
        return text::findFoldedPadded(haystack, folded);
    else
        return text::findFolded(haystack, folded);
}

} // namespace

template <bool Padded, typename Han, typename AncestorsMatch, typename FolderTerms>
NameMatcher::MatchResult NameMatcher::score(std::string_view name, bool isDir, Han&& han, std::size_t extLength,
    AncestorsMatch&& ancestorsMatch, FolderTerms&& folderTerms, bool skipFuzzyScore) const
{
    if (m_invalid || (m_filesOnly && isDir))
        return {};
    int score = 0;
    bool usedFuzzy = false;
    int literalNameTerms = 0;
    std::size_t covered = 0;
    std::optional<pinyin::Span> pinyinMatch; // of a lone term, for the exact-name bonus
    std::uint64_t elsewhere = 0; // terms not in the name, to be found in its folders
    std::uint64_t deferredFuzzy = 0; // fuzzy-candidate terms deferred until cheap filters & folders pass
    bool nonDeferredInName = !m_extensions.empty(); // something non-deferred is in the name (checked below)
    std::size_t nameChars = 0;
    const auto countNameChars = [&] {
        if (nameChars == 0)
            nameChars = fuzzy::codePointCount(name);
        return nameChars;
    };

    for (std::size_t i = 0; i < m_positive.size(); ++i) {
        const Positive& p = m_positive[i];
        const QueryTerm& t = p.term;
        if (!t.text.empty() || t.regex) {
            if (t.regex) {
                if (!t.literal.empty() && find<Padded>(name, t.literal) == text::npos)
                    return {};
                if (!regexMatches(t.expression, name))
                    return {};
                score += 20;
                ++literalNameTerms;
                nonDeferredInName = true;
            } else if (t.wildcard) {
                if (!t.literal.empty() && find<Padded>(name, t.literal) == text::npos)
                    return {};
                if (!text::globMatch(name, t.text))
                    return {};
                score += 10;
                covered += static_cast<std::size_t>(
                    std::count_if(t.text.begin(), t.text.end(), [](char c) { return c != '*' && c != '?'; }));
                ++literalNameTerms;
                nonDeferredInName = true;
            } else if (p.explicitFuzzy) {
                if (!fuzzy::isSubsequence(name, t.text))
                    return {};
                if (i < 64) {
                    deferredFuzzy |= std::uint64_t {1} << i;
                } else {
                    const auto matched = fuzzy::match(name, t.text);
                    if (!matched)
                        return {};
                    score += fuzzy::normalizedScore(matched->score, p.patternChars, countNameChars());
                    usedFuzzy = true;
                    nonDeferredInName = true;
                }
            } else if (std::size_t pos = find<Padded>(name, t.text); pos == text::npos) {
                // Not spelled out: maybe as pinyin ("bg" for 报告), ranked a little lower,
                // and lower still when it takes a less common reading (解 as xie).
                auto span = p.pinyin.valid() && han() ? p.pinyin.findUtf8(name) : std::nullopt;
                if (!span && p.initials && !isDir && programInitials(name, extLength, t.text)) {
                    score += 12; // below literal word matches, above a word's middle
                    covered += t.text.size();
                    ++literalNameTerms;
                    nonDeferredInName = true;
                } else if (!span && p.autoFuzzy) {
                    if (fuzzy::isSubsequence(name, t.text)) {
                        if (i < 64) {
                            deferredFuzzy |= std::uint64_t {1} << i;
                        } else if (const auto matched = fuzzy::match(name, t.text);
                                   matched && fuzzy::passesAutoThreshold(matched->score, p.patternChars)) {
                            score += fuzzy::normalizedScore(matched->score, p.patternChars, countNameChars());
                            usedFuzzy = true;
                            nonDeferredInName = true;
                        } else {
                            return {};
                        }
                    } else if (i < 64 && (m_folderTerms >> i & 1)) {
                        elsewhere |= std::uint64_t {1} << i;
                        continue;
                    } else {
                        return {};
                    }
                } else if (!span) {
                    if (i < 64 && (m_folderTerms >> i & 1)) {
                        elsewhere |= std::uint64_t {1} << i;
                        continue;
                    }
                    return {};
                } else {
                    int bonus = -12;
                    if (const auto common = p.commonPinyin.findUtf8(name)) {
                        span = common;
                        bonus = 0;
                    }
                    bonus += span->start == 0 ? 26 : text::isWordStart(name, span->start) ? 16 : 4;
                    score += bonus;
                    covered += span->length;
                    pinyinMatch = span;
                    ++literalNameTerms;
                    nonDeferredInName = true;
                }
            } else {
                int bonus = 4;
                if (pos == 0) {
                    bonus = 30;
                } else {
                    // Prefer an occurrence that starts a word ("my_report" > "deportation").
                    for (int tries = 0; tries < 4 && pos != text::npos; ++tries) {
                        if (text::isWordStart(name, pos)) {
                            bonus = 18;
                            break;
                        }
                        pos = text::findFolded(name, t.text, pos + 1);
                    }
                }
                score += bonus;
                covered += t.text.size();
                ++literalNameTerms;
                nonDeferredInName = true;
            }
        }
        if (!t.ancestors.empty() && !ancestorsMatch(t.ancestors, p.folders.data()))
            return {};
    }

    // "!draft" leaves out names with "draft"; "!node_modules\" everything in
    // such a folder; "!tmp\*.log" only the .log files in one; "!fz:abc" fuzzy exclusion.
    for (const auto& t : m_negative) {
        const bool nameHit
            = t.regex ? (t.literal.empty() || find<Padded>(name, t.literal) != text::npos) && regexMatches(t.expression, name)
            : t.fuzzy ? fuzzy::isSubsequence(name, t.text)
            : t.text.empty() || (t.wildcard ? text::globMatch(name, t.text) : find<Padded>(name, t.text) != text::npos);
        if (nameHit && (t.ancestors.empty() || ancestorsMatch(t.ancestors, nullptr)))
            return {};
    }

    if (!m_extensions.empty()) {
        if (isDir || extLength == 0)
            return {};
        const std::string_view ext = name.substr(name.size() - extLength);
        const bool ok = std::any_of(
            m_extensions.begin(), m_extensions.end(), [&](const std::string& e) { return text::equalsFolded(ext, e); });
        if (!ok)
            return {};
    }

    // Check known folder terms before running deferred Smith-Waterman V2 DP so
    // multi-term queries (e.g. "core nsrch") reject entries with non-matching
    // ancestor folders without paying for fuzzy DP on every candidate name.
    bool foldersLoaded = false;
    std::uint64_t cachedFolders = 0;
    const auto loadFolders = [&] {
        if (!foldersLoaded) {
            cachedFolders = folderTerms();
            foldersLoaded = true;
        }
        return cachedFolders;
    };
    if (elsewhere != 0) {
        if ((!nonDeferredInName && deferredFuzzy == 0) || (loadFolders() & elsewhere) != elsewhere)
            return {};
    }

    int fuzzyInName = 0;
    for (std::uint64_t bits = deferredFuzzy; bits != 0; bits &= bits - 1) {
        const auto i = static_cast<std::size_t>(std::countr_zero(bits));
        const Positive& p = m_positive[i];
        const QueryTerm& t = p.term;
        if (skipFuzzyScore && m_quickDecides && t.ancestors.empty()) {
            score += 1;
            usedFuzzy = true;
            ++fuzzyInName;
        } else if (const auto matched = fuzzy::match(name, t.text);
                   matched && (p.explicitFuzzy || fuzzy::passesAutoThreshold(matched->score, p.patternChars))) {
            score += fuzzy::normalizedScore(matched->score, p.patternChars, countNameChars());
            usedFuzzy = true;
            ++fuzzyInName;
        } else if (!p.explicitFuzzy && (m_folderTerms >> i & 1) && (loadFolders() >> i & 1)) {
            elsewhere |= std::uint64_t {1} << i;
        } else {
            return {};
        }
    }

    // The rest in the folders it is in: a little, below any place in the name.
    if (elsewhere != 0) {
        if (!nonDeferredInName && fuzzyInName == 0)
            return {};
        score += 3 * std::popcount(elsewhere);
    }

    // In multi-term queries mixing literal/folder terms and fuzzy terms, keep
    // literal matches ahead of entries where all terms matched only fuzzily.
    if (usedFuzzy && m_positive.size() > 1)
        score += literalNameTerms * 100 + std::popcount(elsewhere) * 85;

    if (!usedFuzzy && m_positive.size() == 1 && !m_positive[0].term.wildcard && !m_positive[0].term.regex
        && !m_positive[0].term.text.empty()) {
        const std::string& t = m_positive[0].term.text;
        const std::size_t stem
            = extLength > 0 && name.size() > extLength + 1 ? name.size() - extLength - 1 : name.size();
        if (pinyinMatch) {
            if (pinyinMatch->start == 0 && pinyinMatch->length >= stem)
                score += 40; // the whole name (without extension) as pinyin
        } else if (text::equalsFolded(name, t)) {
            score += 60; // exact name
        } else if (stem < name.size() && text::equalsFolded(name.substr(0, stem), t)) {
            score += 45; // exact name without extension
        }
    }

    if (!name.empty() && covered > 0)
        score += static_cast<int>(25 * std::min(covered, name.size()) / name.size());
    return {score, usedFuzzy};
}

namespace {

// `pinyin` is null for exclusions, which stay literal.
bool folderMatches(
    std::string_view folder, bool han, const std::string& segment, const pinyin::Matcher* pinyin, bool padded)
{
    if ((padded ? text::findFoldedPadded(folder, segment) : text::findFolded(folder, segment)) != text::npos)
        return true;
    return han && pinyin && pinyin->valid() && pinyin->findUtf8(folder);
}

} // namespace

std::uint64_t NameMatcher::termsInFolder(std::string_view name, bool han, bool padded) const
{
    std::uint64_t terms = 0;
    for (std::uint64_t left = m_folderTerms; left != 0; left &= left - 1) {
        const auto i = static_cast<std::size_t>(std::countr_zero(left));
        if (folderMatches(name, han, m_positive[i].term.text, &m_positive[i].pinyin, padded))
            terms |= std::uint64_t {1} << i;
    }
    return terms;
}

std::uint64_t NameMatcher::termsInFolders(const FileIndex& index, EntryId folder, Folders* folders) const
{
    if (folders && folders->m_slots.empty())
        folders->m_slots.resize(Folders::kSlots);
    const auto slotOf = [&](EntryId id) -> Folders::Slot& { return folders->m_slots[id % Folders::kSlots]; };
    const auto own = [&](EntryId id) {
        const EntryScan& e = index.scan(id);
        return termsInFolder(index.name(e), e.flags & EntryFlag::Han, true);
    };

    // Up to a folder known already (or the top), then down again, noting each.
    std::array<EntryId, 64> chain;
    std::size_t length = 0;
    std::uint64_t terms = 0;
    EntryId cur = folder;
    for (; cur != kNoEntry && length < chain.size(); cur = index.parent(cur)) {
        if (folders && slotOf(cur).folder == cur) {
            terms = slotOf(cur).terms;
            break;
        }
        chain[length++] = cur;
    }
    if (length == chain.size()) { // that deep: the rest unnoted
        for (; cur != kNoEntry; cur = index.parent(cur))
            terms |= own(cur);
    }
    while (length > 0) {
        const EntryId id = chain[--length];
        terms |= own(id);
        if (folders)
            slotOf(id) = {id, terms};
    }
    return terms;
}

bool NameMatcher::hasExcludedFiles() const noexcept
{
    return !m_excluded.empty() || !m_excludedNames.empty()
        || std::ranges::any_of(m_alternatives, [](const NameMatcher& m) { return m.hasExcludedFiles(); });
}

int NameMatcher::match(const FileIndex& index, const EntryScan& entry, Folders& folders, EntryId id) const
{
    SkipReason reason = SkipReason::None;
    const MatchResult scored = matchDetailed(index, entry, folders, id, reason, false);
    return reason == SkipReason::None ? scored.score : -1;
}

NameMatcher::MatchResult NameMatcher::matchDetailed(const FileIndex& index, const EntryScan& entry, Folders& folders,
    EntryId id, SkipReason& reason, bool checkExcluded, bool skipFuzzyScore) const
{
    reason = SkipReason::None;
    const MatchResult scored = matchShownDetailed(index, entry, folders, id, reason, checkExcluded, skipFuzzyScore);
    if (!scored.matched())
        return {};
    // Last: only what matches is looked up in its folders.
    if (m_hidden && m_hidden->hides(index, entry, folders.m_hidden)) {
        if (reason == SkipReason::None) {
            reason = SkipReason::HiddenFolder;
            return scored;
        }
        // Skipped by both a hidden folder and a file rule: not shown under either alone.
        reason = SkipReason::None;
        return {};
    }
    return scored;
}

NameMatcher::MatchResult NameMatcher::matchShownDetailed(const FileIndex& index, const EntryScan& entry,
    Folders& folders, EntryId id, SkipReason& reason, bool checkExcluded, bool skipFuzzyScore) const
{
    reason = SkipReason::None;
    if (m_invalid)
        return {};
    if (!m_conditions.empty()) {
        folders.m_conditions.resize(m_conditions.size());
        for (std::size_t i = 0; i < m_conditions.size(); ++i) {
            if (m_conditions[i].match(index, entry, folders.m_conditions[i], id) < 0)
                return {};
        }
    }
    if (!m_alternatives.empty()) {
        folders.m_alternatives.resize(m_alternatives.size());
        MatchResult bestShown;
        MatchResult bestExcluded;
        MatchResult bestHidden;
        for (std::size_t i = 0; i < m_alternatives.size(); ++i) {
            SkipReason branchReason = SkipReason::None;
            const MatchResult s = m_alternatives[i].matchDetailed(
                index, entry, folders.m_alternatives[i], id, branchReason, checkExcluded, false);
            if (!s.matched())
                continue;
            if (branchReason == SkipReason::None)
                bestShown = betterMatch(bestShown, s);
            else if (branchReason == SkipReason::ExcludedFile)
                bestExcluded = betterMatch(bestExcluded, s);
            else if (branchReason == SkipReason::HiddenFolder)
                bestHidden = betterMatch(bestHidden, s);
        }
        if (bestShown.matched())
            return bestShown;
        if (bestExcluded.matched()) {
            reason = SkipReason::ExcludedFile;
            return bestExcluded;
        }
        if (bestHidden.matched()) {
            reason = SkipReason::HiddenFolder;
            return bestHidden;
        }
        return {};
    }
    if (m_filter.any()) {
        if (id == kNoEntry)
            id = index.findChild(entry.parent, index.name(entry), true);
        if (id == kNoEntry || !m_filter.fits(index.modified(id), index.size(id), entry.isDir()))
            return {};
    }
    // A skipped type: a file only (a folder named "x.bak" stays), and before
    // the rest when skipped counts are not asked for, so that the name is not
    // searched for nothing. Not in nameMayMatch: a name is shared by a file
    // and a folder.
    bool excludedExt = false;
    if (!m_excluded.empty() && !entry.isDir()) {
        const std::size_t extLength = index.extLength(entry);
        if (extLength > 0 && extLength <= m_excludedLongest) {
            const std::string_view name = index.name(entry);
            const std::string_view ext = name.substr(name.size() - extLength);
            if (std::any_of(m_excluded.begin(), m_excluded.end(),
                    [&](const std::string& e) { return text::equalsFolded(ext, e); })) {
                if (!checkExcluded)
                    return {};
                excludedExt = true;
            }
        }
    }
    const MatchResult scored = score<true>(
        index.name(entry), entry.isDir(), [&] { return (entry.flags & EntryFlag::Han) != 0; }, index.extLength(entry),
        [&](const std::vector<std::string>& segments, const pinyin::Matcher* folders) {
            // Walk up from the parent, consuming segments innermost-first.
            auto j = static_cast<std::ptrdiff_t>(segments.size()) - 1;
            for (EntryId cur = entry.parent; cur != kNoEntry && j >= 0; cur = index.parent(cur)) {
                const auto k = static_cast<std::size_t>(j);
                const EntryScan& folder = index.scan(cur);
                if (folderMatches(index.name(folder), folder.flags & EntryFlag::Han, segments[k],
                        folders ? &folders[k] : nullptr, true))
                    --j;
            }
            return j < 0;
        },
        [&] { return termsInFolders(index, entry.parent, &folders); }, skipFuzzyScore);
    if (!scored.matched())
        return {};
    // A skipped file name, files only as above; after the rest, as a pattern
    // costs more than an extension and few names that match are skipped.
    if (excludedExt || (!m_excludedNames.empty() && !entry.isDir() && nameMatchesAny(index.name(entry), m_excludedNames))) {
        if (!checkExcluded)
            return {};
        reason = SkipReason::ExcludedFile;
        return scored;
    }
    return scored;
}

NameMatcher::NameCandidate NameMatcher::classifyName(std::string_view name, std::size_t fileExtLength, bool han) const
{
    if (m_invalid)
        return NameCandidate::None;
    if (!m_alternatives.empty()) {
        NameCandidate best = NameCandidate::None;
        for (const NameMatcher& m : m_alternatives) {
            const NameCandidate c = m.classifyName(name, fileExtLength, han);
            if (c == NameCandidate::Normal)
                return NameCandidate::Normal;
            if (c == NameCandidate::FuzzyOnly)
                best = NameCandidate::FuzzyOnly;
        }
        return best;
    }
    if (m_quick >= 0) {
        const Positive& p = m_positive[static_cast<std::size_t>(m_quick)];
        if (p.explicitFuzzy) {
            if (!fuzzy::isSubsequence(name, p.term.text))
                return NameCandidate::None;
            if (m_quickDecides)
                return NameCandidate::FuzzyOnly;
        } else {
            const bool normal = text::findFoldedPadded(name, p.term.text) != text::npos
                || (han && p.pinyin.valid() && p.pinyin.findUtf8(name))
                || (p.initials && programInitials(name, fileExtLength, p.term.text));
            if (normal) {
                if (m_quickDecides)
                    return NameCandidate::Normal;
            } else {
                if (!p.autoFuzzy || !fuzzy::isSubsequence(name, p.term.text))
                    return NameCandidate::None;
                const auto matched = fuzzy::match(name, p.term.text);
                if (!matched || !fuzzy::passesAutoThreshold(matched->score, p.patternChars))
                    return NameCandidate::None;
                if (m_quickDecides)
                    return NameCandidate::FuzzyOnly;
            }
        }
    }
    // What depends on the entry, at its most lenient: a file, in folders
    // that hold every term. Exclusions by folder rule nothing out here (they
    // are the calls without pinyin).
    const MatchResult res = score<true>(
        name, false, [han] { return han; }, fileExtLength,
        [](const std::vector<std::string>&, const pinyin::Matcher* folders) { return folders != nullptr; },
        [] { return ~std::uint64_t {0}; });
    if (!res.matched())
        return NameCandidate::None;
    return res.fuzzy ? NameCandidate::FuzzyOnly : NameCandidate::Normal;
}

bool NameMatcher::nameMayMatch(std::string_view name, std::size_t fileExtLength, bool han) const
{
    return classifyName(name, fileExtLength, han) != NameCandidate::None;
}

bool NameMatcher::filtersNames() const noexcept
{
    if (!m_alternatives.empty())
        return std::ranges::all_of(m_alternatives, [](const NameMatcher& m) { return m.filtersNames(); });
    if (!m_extensions.empty())
        return true;
    const auto byName = [](const QueryTerm& t) { return t.regex || !t.text.empty(); };
    return std::any_of(m_positive.begin(), m_positive.end(), [&](const Positive& p) { return byName(p.term); })
        || std::any_of(m_negative.begin(), m_negative.end(),
            [&](const QueryTerm& t) { return byName(t) && t.ancestors.empty(); });
}

int NameMatcher::match(const FileIndex& index, const EntryScan& entry) const
{
    Folders folders;
    return match(index, entry, folders);
}

int NameMatcher::matchPath(const QString& fullPath, bool isDir, FileTime modified, FileSize size) const
{
    if (m_invalid)
        return -1;
    if (std::ranges::any_of(m_conditions, [&](const NameMatcher& m) { return m.matchPath(fullPath, isDir, modified, size) < 0; }))
        return -1;
    if (!m_alternatives.empty()) {
        int best = -1;
        for (const NameMatcher& m : m_alternatives)
            best = std::max(best, m.matchPath(fullPath, isDir, modified, size));
        return best;
    }
    if (!m_filter.fits(modified, size, isDir))
        return -1;
    QStringList parts = fullPath.split(separatorRegex(), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return -1;
    const std::string name = wtf8::fromUtf16(wtf8::view(parts.takeLast()));
    const std::size_t extLength = extensionLength(name, isDir);
    return score<false>(
        name, isDir, [&] { return pinyin::hasHan(name); }, extLength,
        [&](const std::vector<std::string>& segments, const pinyin::Matcher* folders) {
            auto j = static_cast<std::ptrdiff_t>(segments.size()) - 1;
            for (auto it = parts.crbegin(); it != parts.crend() && j >= 0; ++it) {
                const auto k = static_cast<std::size_t>(j);
                const std::string folder = wtf8::fromUtf16(wtf8::view(*it));
                if (folderMatches(folder, pinyin::hasHan(folder), segments[k], folders ? &folders[k] : nullptr, false))
                    --j;
            }
            return j < 0;
        },
        [&] {
            std::uint64_t terms = 0;
            for (const QString& part : parts) {
                const std::string folder = wtf8::fromUtf16(wtf8::view(part));
                terms |= termsInFolder(folder, pinyin::hasHan(folder), false);
            }
            return terms;
        })
        .score;
}

bool NameMatcher::isExactName(std::string_view name, bool isDir) const
{
    if (m_invalid || !m_alternatives.empty() || m_positive.size() != 1 || m_positive[0].term.wildcard || m_positive[0].term.regex || m_positive[0].term.text.empty())
        return false;
    const std::string& t = m_positive[0].term.text;
    if (text::equalsFolded(name, t))
        return true;
    const std::size_t extLength = extensionLength(name, isDir);
    return extLength > 0 && name.size() > extLength + 1
        && text::equalsFolded(name.substr(0, name.size() - extLength - 1), t);
}

bool NameMatcher::isExactName(QStringView name, bool isDir) const
{
    if (m_positive.size() != 1 || m_positive[0].term.wildcard || m_positive[0].term.text.empty())
        return false;
    // A UTF-16 unit is a byte of UTF-8 at least: a name longer than what
    // was typed is it only by its extension, the dot no further in than
    // that. The rest are not converted (ws's 100 000 rows: 14 ms of it).
    const auto typed = static_cast<qsizetype>(m_positive[0].term.text.size());
    if (name.size() > typed && !name.first(typed + 1).contains(u'.'))
        return false;
    return isExactName(
        wtf8::fromUtf16(std::u16string_view(name.utf16(), static_cast<std::size_t>(name.size()))), isDir);
}

int NameMatcher::matchName(std::string_view name) const
{
    return matchNameDetailed(name).score;
}

NameMatcher::MatchResult NameMatcher::matchNameDetailed(std::string_view name) const
{
    if (m_invalid)
        return {};
    // As if typed: an app or a place is in no folder and has no size, so
    // "!$RECYCLE.BIN\" leaves it be, and ext: or size: leaves it out.
    if (std::ranges::any_of(m_conditions, [&](const NameMatcher& m) { return m.matchName(name) < 0; }))
        return {};
    if (!m_alternatives.empty()) {
        MatchResult best;
        for (const NameMatcher& m : m_alternatives)
            best = betterMatch(best, m.matchNameDetailed(name));
        return best;
    }
    if (m_filter.any())
        return {};
    return score<false>(
        name, false, [&] { return pinyin::hasHan(name); }, 0,
        [](const std::vector<std::string>&, const pinyin::Matcher*) { return false; },
        [] { return std::uint64_t {0}; });
}

} // namespace ws
