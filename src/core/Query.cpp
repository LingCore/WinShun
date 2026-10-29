#include "Query.h"

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
    if (!trimmed.startsWith(u':'))
        return result;
    qsizetype end = 1;
    while (end < trimmed.size() && !trimmed[end].isSpace())
        ++end;
    const QStringView keyword = trimmed.mid(1, end - 1);
    if (keyword.isEmpty())
        return result;
    if (keyword.contains(u',')) {
        const auto keywords = keyword.toString().split(u',', Qt::KeepEmptyParts);
        for (const QString& part : keywords) {
            const auto found = splitFileTypeQuery(u":"_s + part, types, folderKeyword);
            if (!found.type || *found.type < 0)
                return result;
            if (!result.types.contains(*found.type))
                result.types.append(*found.type);
        }
        result.type = result.types.first();
        result.words = trimmed.mid(end).trimmed().toString();
        return result;
    }
    if (!folderKeyword.isEmpty() && keyword.compare(folderKeyword, Qt::CaseInsensitive) == 0)
        result.type = -2;
    else {
        for (qsizetype i = 0; i < types.size(); ++i) {
            if (!types[i].keyword.isEmpty() && keyword.compare(types[i].keyword, Qt::CaseInsensitive) == 0) {
                result.type = int(i);
                break;
            }
        }
    }
    if (result.type)
        result.words = trimmed.mid(end).trimmed().toString();
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

} // namespace

bool nameMatchesAny(std::string_view name, const std::vector<std::string>& foldedPatterns) noexcept
{
    return std::ranges::any_of(foldedPatterns, [&](const std::string& p) { return text::globMatch(name, p); });
}

QStringList queryWords(const QString& text)
{
    QStringList tokens;
    QString current;
    bool quoted = false;
    for (const QChar c : text) {
        if (c == u'"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && c.isSpace()) {
            if (!current.isEmpty())
                tokens.append(std::exchange(current, QString()));
            continue;
        }
        current.append(c);
    }
    if (!current.isEmpty())
        tokens.append(current);
    return tokens;
}

bool looksLikeCode(QStringView text)
{
    static constexpr QStringView kSigns[] {u"=", u";", u"(", u")", u"{", u"}", u"<", u">", u"[", u"]", u"->", u"::", u"&&", u"||"};
    return std::ranges::any_of(kSigns, [&](QStringView s) { return text.contains(s); });
}

QStringList contentTerms(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return {};
    if (!looksLikeCode(trimmed))
        return queryWords(trimmed); // an unclosed quote: to the end
    // Quotes around the whole line only make it one phrase, as for words.
    if (trimmed.count(u'"') == 2 && trimmed.size() > 2 && trimmed.startsWith(u'"') && trimmed.endsWith(u'"'))
        return {trimmed.mid(1, trimmed.size() - 2).trimmed()};
    return {trimmed};
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

} // namespace

ContentQuery splitContentQuery(const QString& text)
{
    ContentQuery query;
    if (text.trimmed().isEmpty()) {
        query.words = text;
        return query;
    }
    if (looksLikeCode(text.trimmed())) {
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
        if (looksLikeCode(rest.trimmed())) {
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

namespace {

// "10mb", "1.5 g", "200": bytes, or nothing.
std::optional<std::uint64_t> parseSizeValue(QStringView s)
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
    if (unit.isEmpty() || unit == u"b")
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

// "2026", "2026-1", "2026-01-15" (or with / or .): the first moment of that
// year, month or day, and the first moment after it.
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

ParsedQuery parseQuery(const QString& text)
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
        q.alternatives.push_back(parseQuery(branch));
        start = piece.end;
    }
    if (!q.alternatives.empty()) {
        const QString branch = text.sliced(start).trimmed();
        if (branch.isEmpty()) {
            q.error = QObject::tr("“|”两侧都需要搜索条件");
            return q;
        }
        q.alternatives.push_back(parseQuery(branch));
        for (const ParsedQuery& group : q.alternatives) {
            q.highlights.append(group.highlights);
            if (!group.error.isEmpty())
                q.error = group.error;
        }
        q.highlights.removeDuplicates();
        return q;
    }
    // A path typed or pasted whole ("C:\Users\me\Tencent Files\x") is one
    // term: its spaces are in the names, not between terms.
    const pathtext::TypedPath typed = pathtext::splitTyped(text);
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
        if (token.size() > 1 && token.startsWith(u'!')) {
            term.negated = true;
            token.remove(0, 1);
        }

        const bool regex = !quotedLiteral && token.startsWith(u"regex:", Qt::CaseInsensitive);
        const bool anchored = !quotedLiteral && (token.startsWith(u'^') || token.endsWith(u'$'));
        if (regex || anchored) {
            QString pattern;
            if (regex) {
                pattern = token.mid(6);
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
            if (!regex && !term.negated && !token.isEmpty())
                q.highlights.append(token);
            q.terms.push_back(std::move(term));
            continue;
        }

        if (std::any_of(token.cbegin(), token.cend(), isPathSeparator)) {
            QStringList parts = token.split(separatorRegex());
            const QString last = parts.takeLast();
            for (const QString& part : parts) {
                if (!part.isEmpty())
                    term.ancestors.push_back(foldedUtf8(part));
            }
            token = last;
        }

        term.wildcard = token.contains(u'*') || token.contains(u'?');
        term.text = foldedUtf8(token);
        if (term.wildcard) {
            std::size_t start = 0;
            for (std::size_t i = 0; i <= term.text.size(); ++i) {
                if (i == term.text.size() || term.text[i] == '*' || term.text[i] == '?') {
                    if (i - start > term.literal.size())
                        term.literal = term.text.substr(start, i - start);
                    start = i + 1;
                }
            }
        }
        if (term.text.empty() && term.ancestors.empty())
            continue;
        if (!term.negated && !term.wildcard && !token.isEmpty())
            q.highlights.append(token);
        q.terms.push_back(std::move(term));
    }
    return q;
}

bool ParsedQuery::hasFilters() const noexcept
{
    return filter.any() || std::ranges::any_of(alternatives, [](const ParsedQuery& q) { return q.hasFilters(); });
}

std::vector<ParsedQuery> homophoneQueries(const ParsedQuery& query)
{
    if (!query.alternatives.empty() || !query.error.isEmpty()
        || std::ranges::any_of(query.terms, [](const QueryTerm& t) { return t.regex; }))
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

NameMatcher::NameMatcher(const ParsedQuery& query)
    : m_extensions(query.extensions)
    , m_excluded(query.excludedExtensions)
    , m_excludedNames(query.excludedFileNames)
{
    m_invalid = !query.error.isEmpty();
    m_filter = query.filter;
    for (const ParsedQuery& branch : query.alternatives)
        m_alternatives.emplace_back(branch);
    for (const std::string& e : m_excluded)
        m_excludedLongest = std::max(m_excludedLongest, e.size());
    for (const auto& t : query.terms) {
        if (t.negated) {
            m_negative.push_back(t); // exclusions stay literal: no surprises
            continue;
        }
        const std::string_view text = t.wildcard || t.regex ? std::string_view() : std::string_view(t.text);
        Positive p {t, pinyin::Matcher(text), pinyin::Matcher(text, true), {}};
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
            if (!t.wildcard && !t.regex && !t.text.empty() && t.ancestors.empty())
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
int NameMatcher::score(std::string_view name, bool isDir, Han&& han, std::size_t extLength,
    AncestorsMatch&& ancestorsMatch, FolderTerms&& folderTerms) const
{
    if (m_invalid)
        return -1;
    int score = 0;
    std::size_t covered = 0;
    std::optional<pinyin::Span> pinyinMatch; // of a lone term, for the exact-name bonus
    std::uint64_t elsewhere = 0; // terms not in the name, to be found in its folders
    bool inName = !m_extensions.empty(); // something is in the name (checked below)

    for (std::size_t i = 0; i < m_positive.size(); ++i) {
        const Positive& p = m_positive[i];
        const QueryTerm& t = p.term;
        if (!t.text.empty() || t.regex) {
            if (t.regex) {
                if (!t.expression.match(wtf8::toQString(name)).hasMatch())
                    return -1;
                score += 20;
            } else if (t.wildcard) {
                if (!t.literal.empty() && find<Padded>(name, t.literal) == text::npos)
                    return -1;
                if (!text::globMatch(name, t.text))
                    return -1;
                score += 10;
                covered += static_cast<std::size_t>(
                    std::count_if(t.text.begin(), t.text.end(), [](char c) { return c != '*' && c != '?'; }));
            } else if (std::size_t pos = find<Padded>(name, t.text); pos == text::npos) {
                // Not spelled out: maybe as pinyin ("bg" for 报告), ranked a little lower,
                // and lower still when it takes a less common reading (解 as xie).
                auto span = p.pinyin.valid() && han() ? p.pinyin.findUtf8(name) : std::nullopt;
                if (!span) {
                    if (i < 64 && (m_folderTerms >> i & 1)) {
                        elsewhere |= std::uint64_t {1} << i;
                        continue;
                    }
                    return -1;
                }
                int bonus = -12;
                if (const auto common = p.commonPinyin.findUtf8(name)) {
                    span = common;
                    bonus = 0;
                }
                bonus += span->start == 0 ? 26 : text::isWordStart(name, span->start) ? 16 : 4;
                score += bonus;
                covered += span->length;
                pinyinMatch = span;
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
            }
            inName = true;
        }
        if (!t.ancestors.empty() && !ancestorsMatch(t.ancestors, p.folders.data()))
            return -1;
    }

    // "!draft" leaves out names with "draft"; "!node_modules\" everything in
    // such a folder; "!tmp\*.log" only the .log files in one.
    for (const auto& t : m_negative) {
        const bool nameHit
            = t.regex ? t.expression.match(wtf8::toQString(name)).hasMatch()
            : t.text.empty() || (t.wildcard ? text::globMatch(name, t.text) : find<Padded>(name, t.text) != text::npos);
        if (nameHit && (t.ancestors.empty() || ancestorsMatch(t.ancestors, nullptr)))
            return -1;
    }

    if (!m_extensions.empty()) {
        if (isDir || extLength == 0)
            return -1;
        const std::string_view ext = name.substr(name.size() - extLength);
        const bool ok = std::any_of(
            m_extensions.begin(), m_extensions.end(), [&](const std::string& e) { return text::equalsFolded(ext, e); });
        if (!ok)
            return -1;
    }

    // The rest in the folders it is in: a little, below any place in the name.
    if (elsewhere != 0) {
        if (!inName || (folderTerms() & elsewhere) != elsewhere)
            return -1;
        score += 3 * std::popcount(elsewhere);
    }

    if (m_positive.size() == 1 && !m_positive[0].term.wildcard && !m_positive[0].term.regex && !m_positive[0].term.text.empty()) {
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

    if (!name.empty())
        score += static_cast<int>(25 * std::min(covered, name.size()) / name.size());
    return score;
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
        const Entry& e = index.entry(id);
        return termsInFolder(index.name(e), e.flags & EntryFlag::Han, true);
    };

    // Up to a folder known already (or the top), then down again, noting each.
    std::array<EntryId, 64> chain;
    std::size_t length = 0;
    std::uint64_t terms = 0;
    EntryId cur = folder;
    for (; cur != kNoEntry && length < chain.size(); cur = index.entry(cur).parent) {
        if (folders && slotOf(cur).folder == cur) {
            terms = slotOf(cur).terms;
            break;
        }
        chain[length++] = cur;
    }
    if (length == chain.size()) { // that deep: the rest unnoted
        for (; cur != kNoEntry; cur = index.entry(cur).parent)
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

int NameMatcher::match(const FileIndex& index, const Entry& entry, Folders& folders, EntryId id) const
{
    if (m_invalid)
        return -1;
    if (!m_alternatives.empty()) {
        folders.m_alternatives.resize(m_alternatives.size());
        int best = -1;
        for (std::size_t i = 0; i < m_alternatives.size(); ++i)
            best = std::max(best, m_alternatives[i].match(index, entry, folders.m_alternatives[i], id));
        return best;
    }
    if (m_filter.any()) {
        if (id == kNoEntry)
            id = index.findChild(entry.parent, index.name(entry), true);
        if (id == kNoEntry || !m_filter.fits(index.modified(id), index.size(id), entry.isDir()))
            return -1;
    }
    // A skipped type: a file only (a folder named "x.bak" stays), and before
    // the rest, so that the name is not searched for nothing. Not in
    // nameMayMatch: a name is shared by a file and a folder.
    if (!m_excluded.empty() && !entry.isDir()) {
        const std::size_t extLength = index.extLength(entry);
        if (extLength > 0 && extLength <= m_excludedLongest) {
            const std::string_view name = index.name(entry);
            const std::string_view ext = name.substr(name.size() - extLength);
            if (std::any_of(m_excluded.begin(), m_excluded.end(), [&](const std::string& e) { return text::equalsFolded(ext, e); }))
                return -1;
        }
    }
    const int scored = score<true>(
        index.name(entry), entry.isDir(), [&] { return (entry.flags & EntryFlag::Han) != 0; }, index.extLength(entry),
        [&](const std::vector<std::string>& segments, const pinyin::Matcher* folders) {
            // Walk up from the parent, consuming segments innermost-first.
            auto j = static_cast<std::ptrdiff_t>(segments.size()) - 1;
            for (EntryId cur = entry.parent; cur != kNoEntry && j >= 0; cur = index.entry(cur).parent) {
                const auto k = static_cast<std::size_t>(j);
                const Entry& folder = index.entry(cur);
                if (folderMatches(index.name(folder), folder.flags & EntryFlag::Han, segments[k],
                        folders ? &folders[k] : nullptr, true))
                    --j;
            }
            return j < 0;
        },
        [&] { return termsInFolders(index, entry.parent, &folders); });
    // A skipped file name, files only as above; after the rest, as a pattern
    // costs more than an extension and few names that match are skipped.
    if (scored >= 0 && !m_excludedNames.empty() && !entry.isDir() && nameMatchesAny(index.name(entry), m_excludedNames))
        return -1;
    return scored;
}

bool NameMatcher::nameMayMatch(std::string_view name, std::size_t fileExtLength, bool han) const
{
    if (m_invalid)
        return false;
    if (!m_alternatives.empty())
        return std::ranges::any_of(m_alternatives, [&](const NameMatcher& m) { return m.nameMayMatch(name, fileExtLength, han); });
    if (m_quick >= 0) {
        const Positive& p = m_positive[static_cast<std::size_t>(m_quick)];
        if (text::findFoldedPadded(name, p.term.text) == text::npos && !(han && p.pinyin.valid() && p.pinyin.findUtf8(name)))
            return false;
        if (m_quickDecides)
            return true; // the term in the name; its folders, if any, are the entry's
    }
    // What depends on the entry, at its most lenient: a file, in folders
    // that hold every term. Exclusions by folder rule nothing out here (they
    // are the calls without pinyin).
    return score<true>(
               name, false, [han] { return han; }, fileExtLength,
               [](const std::vector<std::string>&, const pinyin::Matcher* folders) { return folders != nullptr; },
               [] { return ~std::uint64_t {0}; })
        >= 0;
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

int NameMatcher::match(const FileIndex& index, const Entry& entry) const
{
    Folders folders;
    return match(index, entry, folders);
}

int NameMatcher::matchPath(const QString& fullPath, bool isDir, FileTime modified, FileSize size) const
{
    if (m_invalid)
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
        });
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

int NameMatcher::matchName(std::string_view name) const
{
    if (m_invalid)
        return -1;
    if (!m_alternatives.empty()) {
        int best = -1;
        for (const NameMatcher& m : m_alternatives)
            best = std::max(best, m.matchName(name));
        return best;
    }
    if (m_filter.any())
        return -1;
    return score<false>(
        name, false, [&] { return pinyin::hasHan(name); }, 0,
        [](const std::vector<std::string>&, const pinyin::Matcher*) { return false; },
        [] { return std::uint64_t {0}; });
}

} // namespace ws
