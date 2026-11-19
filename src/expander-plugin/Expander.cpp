#include "Expander.h"

#include <windows.h>

#include <algorithm>

namespace expander {

namespace {

bool space(wchar_t c)
{
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000;
}

std::wstring_view trimmedRight(std::wstring_view text)
{
    while (!text.empty() && space(text.back()))
        text.remove_suffix(1);
    return text;
}

std::wstring_view trimmed(std::wstring_view text)
{
    text = trimmedRight(text);
    while (!text.empty() && space(text.front()))
        text.remove_prefix(1);
    return text;
}

std::wstring mapped(std::wstring_view text, DWORD how)
{
    if (text.empty())
        return {};
    std::wstring out(text.size(), L'\0');
    const int n = ::LCMapStringEx(LOCALE_NAME_INVARIANT, how, text.data(), static_cast<int>(text.size()), out.data(),
                                  static_cast<int>(out.size()), nullptr, nullptr, 0);
    if (n <= 0)
        return std::wstring(text);
    out.resize(static_cast<std::size_t>(n));
    return out;
}

std::wstring upper(std::wstring_view text) { return mapped(text, LCMAP_UPPERCASE); }
std::wstring lower(std::wstring_view text) { return mapped(text, LCMAP_LOWERCASE); }

// A letter that has a capital and a small form, and the one it is.
bool cased(wchar_t c) { return upper({&c, 1}) != lower({&c, 1}); }
bool lowerCase(wchar_t c) { return cased(c) && upper({&c, 1}) != std::wstring_view(&c, 1); }

// The first letter that has a case, as a capital ("by the way": "By the way").
std::wstring capitalized(std::wstring_view text)
{
    std::wstring out(text);
    for (wchar_t& c : out) {
        if (cased(c)) {
            c = upper({&c, 1}).front();
            break;
        }
    }
    return out;
}

// Typed with an input method only, which this never sees (and a surrogate:
// an emoji, a rarer ideograph).
bool notTyped(wchar_t c)
{
    return (c >= 0x1100 && c <= 0x11FF) // Hangul Jamo
        || (c >= 0x2E80 && c <= 0x9FFF) // CJK radicals and punctuation, kana, ideographs
        || (c >= 0xA960 && c <= 0xA97F) || (c >= 0xAC00 && c <= 0xDFFF) // Hangul, surrogates
        || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) // compatibility ideographs and forms
        || (c >= 0xFF00 && c <= 0xFFEF); // full-width forms
}

} // namespace

bool isWordChar(wchar_t c)
{
    return c == L'_' || ::IsCharAlphaNumericW(c);
}

Phrases parse(std::wstring_view setting)
{
    Phrases out;
    struct Reading {
        Phrase phrase;
        bool kept = true; // no problem with its abbreviation
        bool lines = false; // it has a line besides the first's text
    };
    std::optional<Reading> current;
    int blanks = 0; // blank lines since its last line
    const auto finish = [&] {
        if (!current)
            return;
        if (current->kept && current->phrase.text.empty())
            out.problems.push_back({Problem::Kind::NoText, current->phrase.line, current->phrase.abbreviation});
        else if (current->kept)
            out.list.push_back(std::move(current->phrase));
        current.reset();
    };

    int number = 0;
    std::size_t start = 0;
    while (start <= setting.size()) {
        std::size_t end = setting.find(L'\n', start);
        if (end == std::wstring_view::npos)
            end = setting.size();
        std::wstring_view line = setting.substr(start, end - start);
        start = end + 1;
        ++number;
        if (!line.empty() && line.back() == L'\r')
            line.remove_suffix(1);

        const std::wstring_view bare = trimmed(line);
        if (bare.empty()) {
            ++blanks;
            continue;
        }
        if (bare.front() == L'#')
            continue;
        const std::size_t equals = line.find_first_of(L"=＝");
        if (equals == std::wstring_view::npos) { // the next line of the phrase above
            if (!current)
                out.problems.push_back({Problem::Kind::NoEquals, number, std::wstring(bare)});
            else if (current->phrase.text.empty() && !current->lines) // "abbreviation =": its first line
                current->phrase.text = trimmedRight(line);
            else
                current->phrase.text.append(static_cast<std::size_t>(blanks) + 1, L'\n').append(trimmedRight(line));
            if (current)
                current->lines = true;
            blanks = 0;
            continue;
        }

        finish();
        blanks = 0;
        Reading r;
        r.phrase.abbreviation = trimmed(line.substr(0, equals));
        r.phrase.text = trimmed(line.substr(equals + 1));
        r.phrase.line = number;
        const std::wstring& a = r.phrase.abbreviation;
        std::optional<Problem::Kind> problem;
        if (a.empty())
            problem = Problem::Kind::NoAbbreviation;
        else if (std::ranges::any_of(a, space))
            problem = Problem::Kind::Spaces;
        else if (a.size() > kLongest)
            problem = Problem::Kind::TooLong;
        else if (std::ranges::any_of(a, notTyped))
            problem = Problem::Kind::NotTyped;
        else if (std::ranges::any_of(out.list, [&](const Phrase& p) { return p.abbreviation == a; }))
            problem = Problem::Kind::Repeated;
        if (problem) {
            out.problems.push_back({*problem, number, a});
            r.kept = false;
        }
        current = std::move(r);
    }
    finish();
    return out;
}

std::optional<Match> find(const std::vector<Phrase>& phrases, std::wstring_view typed)
{
    enum Form { None, Capitals, Capitalized, Written }; // the better one later
    const Phrase* best = nullptr;
    Form bestForm = None;
    for (const Phrase& p : phrases) {
        const std::wstring& a = p.abbreviation;
        if (a.empty() || a.size() > typed.size() || (best && a.size() < best->abbreviation.size()))
            continue;
        const std::size_t at = typed.size() - a.size();
        if (at > 0 && isWordChar(a.front()) && isWordChar(typed[at - 1]))
            continue;
        const std::wstring_view tail = typed.substr(at);
        Form form = None;
        if (tail == a)
            form = Written;
        else if (lowerCase(a.front()) && tail == capitalized(a))
            form = Capitalized;
        else if (std::ranges::count_if(a, cased) >= 2 && std::ranges::any_of(a, lowerCase) && tail == upper(a))
            form = Capitals;
        if (form == None || (best && a.size() == best->abbreviation.size() && form <= bestForm))
            continue;
        best = &p;
        bestForm = form;
    }
    if (!best)
        return std::nullopt;
    Match m;
    m.phrase = best;
    m.typedLength = best->abbreviation.size();
    m.text = bestForm == Capitals ? upper(best->text) : bestForm == Capitalized ? capitalized(best->text) : best->text;
    return m;
}

std::size_t codePoints(std::wstring_view text)
{
    std::size_t n = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (IS_HIGH_SURROGATE(text[i]) && i + 1 < text.size() && IS_LOW_SURROGATE(text[i + 1]))
            ++i;
        ++n;
    }
    return n;
}

std::vector<std::wstring> programList(std::wstring_view text)
{
    std::vector<std::wstring> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find_first_of(L";；,，\r\n", start);
        if (end == std::wstring_view::npos)
            end = text.size();
        std::wstring_view name = trimmed(text.substr(start, end - start));
        start = end + 1;
        while (!name.empty() && name.front() == L'"')
            name.remove_prefix(1);
        while (!name.empty() && name.back() == L'"')
            name.remove_suffix(1);
        if (const std::size_t slash = name.find_last_of(L"\\/"); slash != std::wstring_view::npos)
            name.remove_prefix(slash + 1);
        std::wstring program = lower(trimmed(name));
        if (program.empty())
            continue;
        if (program.find(L'.') == std::wstring::npos)
            program += L".exe";
        if (std::ranges::find(out, program) == out.end())
            out.push_back(std::move(program));
    }
    return out;
}

bool listed(const std::vector<std::wstring>& list, std::wstring_view path)
{
    if (const std::size_t slash = path.find_last_of(L"\\/"); slash != std::wstring_view::npos)
        path.remove_prefix(slash + 1);
    return !path.empty() && std::ranges::find(list, lower(path)) != list.end();
}

} // namespace expander
