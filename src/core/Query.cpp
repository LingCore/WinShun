#include "Query.h"

#include "TextUtil.h"
#include "Wtf8.h"

#include <QRegularExpression>

#include <algorithm>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;

namespace qf {

namespace {

std::string foldedUtf8(const QString& s)
{
    return text::foldAscii(wtf8::fromUtf16(wtf8::view(s)));
}

QStringList tokenize(const QString& text)
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

ParsedQuery parseQuery(const QString& text)
{
    ParsedQuery q;
    for (QString token : tokenize(text)) {
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

NameMatcher::NameMatcher(const ParsedQuery& query)
    : m_extensions(query.extensions)
{
    for (const auto& t : query.terms) {
        if (t.negated) {
            m_negative.push_back(t); // exclusions stay literal: no surprises
            continue;
        }
        const std::string_view text = t.wildcard ? std::string_view() : std::string_view(t.text);
        Positive p {t, pinyin::Matcher(text), pinyin::Matcher(text, true), {}};
        for (const std::string& segment : t.ancestors)
            p.folders.emplace_back(segment);
        m_positive.push_back(std::move(p));
    }
    // Longest term first: it rejects the most names before the others are tried.
    std::stable_sort(m_positive.begin(), m_positive.end(),
        [](const Positive& a, const Positive& b) { return a.term.text.size() > b.term.text.size(); });
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

template <bool Padded, typename AncestorsMatch>
int NameMatcher::score(
    std::string_view name, bool isDir, bool han, std::size_t extLength, AncestorsMatch&& ancestorsMatch) const
{
    int score = 0;
    std::size_t covered = 0;
    std::optional<pinyin::Span> pinyinMatch; // of a lone term, for the exact-name bonus

    for (const Positive& p : m_positive) {
        const QueryTerm& t = p.term;
        if (!t.text.empty()) {
            if (t.wildcard) {
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
                auto span = han && p.pinyin.valid() ? p.pinyin.findUtf8(name) : std::nullopt;
                if (!span)
                    return -1;
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
        }
        if (!t.ancestors.empty() && !ancestorsMatch(p))
            return -1;
    }

    for (const auto& t : m_negative) {
        if (t.wildcard ? text::globMatch(name, t.text) : find<Padded>(name, t.text) != text::npos)
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

    if (m_positive.size() == 1 && !m_positive[0].term.wildcard && !m_positive[0].term.text.empty()) {
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

bool folderMatches(
    std::string_view folder, bool han, const std::string& segment, const pinyin::Matcher& pinyin, bool padded)
{
    if ((padded ? text::findFoldedPadded(folder, segment) : text::findFolded(folder, segment)) != text::npos)
        return true;
    return han && pinyin.valid() && pinyin.findUtf8(folder);
}

} // namespace

int NameMatcher::match(const FileIndex& index, const Entry& entry) const
{
    return score<true>(
        index.name(entry), entry.isDir(), entry.flags & EntryFlag::Han, entry.extLength, [&](const Positive& p) {
            // Walk up from the parent, consuming segments innermost-first.
            auto j = static_cast<std::ptrdiff_t>(p.term.ancestors.size()) - 1;
            for (EntryId cur = entry.parent; cur != kNoEntry && j >= 0; cur = index.entry(cur).parent) {
                const auto k = static_cast<std::size_t>(j);
                const Entry& folder = index.entry(cur);
                if (folderMatches(index.name(folder), folder.flags & EntryFlag::Han, p.term.ancestors[k], p.folders[k],
                        true))
                    --j;
            }
            return j < 0;
        });
}

int NameMatcher::matchPath(const QString& fullPath, bool isDir) const
{
    QStringList parts = fullPath.split(separatorRegex(), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return -1;
    const std::string name = wtf8::fromUtf16(wtf8::view(parts.takeLast()));
    const std::size_t extLength = extensionLength(name, isDir);
    return score<false>(name, isDir, pinyin::hasHan(name), extLength, [&](const Positive& p) {
        auto j = static_cast<std::ptrdiff_t>(p.term.ancestors.size()) - 1;
        for (auto it = parts.crbegin(); it != parts.crend() && j >= 0; ++it) {
            const auto k = static_cast<std::size_t>(j);
            const std::string folder = wtf8::fromUtf16(wtf8::view(*it));
            if (folderMatches(folder, pinyin::hasHan(folder), p.term.ancestors[k], p.folders[k], false))
                --j;
        }
        return j < 0;
    });
}

int NameMatcher::matchName(std::string_view name) const
{
    return score<false>(name, false, pinyin::hasHan(name), 0, [](const Positive&) { return false; });
}

} // namespace qf
