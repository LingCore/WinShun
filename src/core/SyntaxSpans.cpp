#include "SyntaxSpans.h"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace ws::syntax {

namespace {

using namespace std::string_view_literals;
using Words = std::span<const std::u16string_view>;

// Every list sorted (binary search); checked by the tests through what they colour.
constexpr std::array kCKeywords {
    u"abstract"sv, u"alignas"sv, u"alignof"sv, u"as"sv, u"async"sv, u"await"sv, u"break"sv, u"case"sv, u"catch"sv,
    u"class"sv, u"co_await"sv, u"co_return"sv, u"co_yield"sv, u"concept"sv, u"const"sv, u"consteval"sv, u"constexpr"sv,
    u"constinit"sv, u"continue"sv, u"crate"sv, u"debugger"sv, u"default"sv, u"defer"sv, u"delegate"sv, u"delete"sv,
    u"do"sv, u"dyn"sv, u"else"sv, u"enum"sv, u"explicit"sv, u"export"sv, u"extends"sv, u"extern"sv, u"final"sv,
    u"finally"sv, u"fn"sv, u"for"sv, u"foreach"sv, u"friend"sv, u"from"sv, u"fun"sv, u"func"sv, u"function"sv, u"go"sv,
    u"goto"sv, u"if"sv, u"impl"sv, u"implements"sv, u"import"sv, u"in"sv, u"inline"sv, u"instanceof"sv, u"interface"sv,
    u"internal"sv, u"is"sv, u"let"sv, u"lock"sv, u"loop"sv, u"match"sv, u"mod"sv, u"move"sv, u"mut"sv, u"mutable"sv,
    u"namespace"sv, u"new"sv, u"noexcept"sv, u"of"sv, u"operator"sv, u"out"sv, u"override"sv, u"package"sv,
    u"private"sv, u"property"sv, u"protected"sv, u"pub"sv, u"public"sv, u"range"sv, u"readonly"sv, u"ref"sv,
    u"register"sv, u"required"sv, u"requires"sv, u"return"sv, u"sealed"sv, u"select"sv, u"signal"sv, u"sizeof"sv,
    u"static"sv, u"struct"sv, u"super"sv, u"switch"sv, u"template"sv, u"this"sv, u"throw"sv, u"throws"sv, u"trait"sv,
    u"try"sv, u"type"sv, u"typedef"sv, u"typename"sv, u"typeof"sv, u"union"sv, u"unsafe"sv, u"use"sv, u"using"sv,
    u"val"sv, u"var"sv, u"virtual"sv, u"volatile"sv, u"when"sv, u"where"sv, u"while"sv, u"yield"sv,
};
constexpr std::array kCTypes {
    u"False"sv, u"None"sv, u"Self"sv, u"True"sv, u"any"sv, u"auto"sv, u"bool"sv, u"boolean"sv, u"byte"sv, u"char"sv,
    u"char16_t"sv, u"char32_t"sv, u"char8_t"sv, u"decimal"sv, u"double"sv, u"f32"sv, u"f64"sv, u"false"sv, u"float"sv,
    u"float32"sv, u"float64"sv, u"i16"sv, u"i32"sv, u"i64"sv, u"i8"sv, u"int"sv, u"int16"sv, u"int32"sv, u"int64"sv,
    u"int8"sv, u"isize"sv, u"long"sv, u"never"sv, u"nil"sv, u"null"sv, u"nullptr"sv, u"number"sv, u"object"sv,
    u"real"sv, u"self"sv, u"short"sv, u"signed"sv, u"size_t"sv, u"str"sv, u"string"sv, u"true"sv, u"u16"sv, u"u32"sv,
    u"u64"sv, u"u8"sv, u"uint"sv, u"uint16"sv, u"uint32"sv, u"uint64"sv, u"uint8"sv, u"undefined"sv, u"unknown"sv,
    u"unsigned"sv, u"usize"sv, u"void"sv, u"wchar_t"sv,
};
constexpr std::array kPythonKeywords {
    u"and"sv, u"as"sv, u"assert"sv, u"async"sv, u"await"sv, u"break"sv, u"case"sv, u"class"sv, u"continue"sv, u"def"sv,
    u"del"sv, u"elif"sv, u"else"sv, u"except"sv, u"finally"sv, u"for"sv, u"from"sv, u"global"sv, u"if"sv, u"import"sv,
    u"in"sv, u"is"sv, u"lambda"sv, u"match"sv, u"nonlocal"sv, u"not"sv, u"or"sv, u"pass"sv, u"raise"sv, u"return"sv,
    u"try"sv, u"while"sv, u"with"sv, u"yield"sv,
};
constexpr std::array kPythonTypes {
    u"False"sv, u"None"sv, u"True"sv, u"bool"sv, u"bytes"sv, u"dict"sv, u"float"sv, u"int"sv, u"list"sv, u"object"sv,
    u"self"sv, u"set"sv, u"str"sv, u"tuple"sv,
};
constexpr std::array kShellKeywords {
    u"begin"sv, u"break"sv, u"case"sv, u"catch"sv, u"class"sv, u"continue"sv, u"def"sv, u"do"sv, u"done"sv, u"elif"sv,
    u"else"sv, u"elseif"sv, u"elsif"sv, u"end"sv, u"esac"sv, u"exit"sv, u"export"sv, u"fi"sv, u"finally"sv, u"for"sv,
    u"foreach"sv, u"function"sv, u"if"sv, u"in"sv, u"local"sv, u"module"sv, u"my"sv, u"param"sv, u"require"sv,
    u"return"sv, u"sub"sv, u"switch"sv, u"then"sv, u"throw"sv, u"try"sv, u"unless"sv, u"until"sv, u"use"sv, u"while"sv,
};
constexpr std::array kShellTypes {u"FALSE"sv, u"NULL"sv, u"TRUE"sv, u"false"sv, u"nil"sv, u"null"sv, u"true"sv};
constexpr std::array kBatchKeywords { // compared in lower case
    u"call"sv, u"cd"sv, u"copy"sv, u"del"sv, u"do"sv, u"echo"sv, u"else"sv, u"endlocal"sv, u"exist"sv, u"exit"sv,
    u"for"sv, u"goto"sv, u"if"sv, u"in"sv, u"not"sv, u"pause"sv, u"set"sv, u"setlocal"sv, u"shift"sv, u"start"sv,
};
constexpr std::array kLuaKeywords {
    u"and"sv, u"break"sv, u"do"sv, u"else"sv, u"elseif"sv, u"end"sv, u"for"sv, u"function"sv, u"goto"sv, u"if"sv,
    u"in"sv, u"local"sv, u"not"sv, u"or"sv, u"repeat"sv, u"return"sv, u"then"sv, u"until"sv, u"while"sv,
};
constexpr std::array kLuaTypes {u"false"sv, u"nil"sv, u"true"sv};
constexpr std::array kSqlKeywords { // compared in lower case
    u"add"sv, u"all"sv, u"alter"sv, u"and"sv, u"as"sv, u"asc"sv, u"begin"sv, u"between"sv, u"by"sv, u"case"sv,
    u"check"sv, u"column"sv, u"commit"sv, u"constraint"sv, u"create"sv, u"database"sv, u"default"sv, u"delete"sv,
    u"desc"sv, u"distinct"sv, u"drop"sv, u"else"sv, u"end"sv, u"exists"sv, u"foreign"sv, u"from"sv, u"group"sv,
    u"having"sv, u"if"sv, u"in"sv, u"index"sv, u"inner"sv, u"insert"sv, u"into"sv, u"is"sv, u"join"sv, u"key"sv,
    u"left"sv, u"like"sv, u"limit"sv, u"not"sv, u"on"sv, u"or"sv, u"order"sv, u"outer"sv, u"primary"sv,
    u"references"sv, u"right"sv, u"rollback"sv, u"select"sv, u"set"sv, u"table"sv, u"then"sv, u"transaction"sv,
    u"union"sv, u"unique"sv, u"update"sv, u"values"sv, u"view"sv, u"when"sv, u"where"sv, u"with"sv,
};
constexpr std::array kSqlTypes { // compared in lower case
    u"bigint"sv, u"blob"sv, u"boolean"sv, u"char"sv, u"date"sv, u"datetime"sv, u"decimal"sv, u"false"sv, u"float"sv,
    u"int"sv, u"integer"sv, u"null"sv, u"real"sv, u"text"sv, u"timestamp"sv, u"true"sv, u"varchar"sv,
};
constexpr std::array kDataTypes { // JSON, YAML, INI: the constants
    u"False"sv, u"No"sv, u"Null"sv, u"True"sv, u"Yes"sv, u"false"sv, u"no"sv, u"null"sv, u"off"sv, u"on"sv, u"true"sv,
    u"yes"sv,
};

static_assert(std::ranges::is_sorted(kCKeywords) && std::ranges::is_sorted(kCTypes) && std::ranges::is_sorted(kPythonKeywords)
    && std::ranges::is_sorted(kPythonTypes) && std::ranges::is_sorted(kShellKeywords) && std::ranges::is_sorted(kShellTypes)
    && std::ranges::is_sorted(kBatchKeywords) && std::ranges::is_sorted(kLuaKeywords) && std::ranges::is_sorted(kLuaTypes)
    && std::ranges::is_sorted(kSqlKeywords) && std::ranges::is_sorted(kSqlTypes) && std::ranges::is_sorted(kDataTypes));

struct Pair {
    std::u16string_view open;
    std::u16string_view close;
    Kind kind;
    bool lineOnly = false; // never carried to the next line
};

struct Rules {
    std::span<const std::u16string_view> lineComments;
    std::span<const Pair> pairs; // comments and strings with an end of their own
    std::u16string_view quotes; // one character each, ended by the same on the line ('\\' escapes)
    Words keywords;
    Words types;
    bool foldCase = false; // keywords in any case (SQL, batch)
    bool keys = false; // a name or a string before ':' is a key (JSON, CSS)
    bool commentAtStartOnly = false; // a comment mark counts only as the line's first thing (INI's ';')
};

constexpr std::array kSlashes {u"//"sv};
constexpr std::array kHash {u"#"sv};
constexpr std::array kDashes {u"--"sv};
constexpr std::array kIniComments {u";"sv, u"#"sv};
constexpr std::array kBatchComments {u"::"sv, u"rem "sv, u"REM "sv, u"Rem "sv};
constexpr std::array kCPairs {Pair {u"/*"sv, u"*/"sv, Kind::Comment}, Pair {u"`"sv, u"`"sv, Kind::String}};
constexpr std::array kCssPairs {Pair {u"/*"sv, u"*/"sv, Kind::Comment}};
constexpr std::array kPythonPairs {Pair {u"\"\"\""sv, u"\"\"\""sv, Kind::String}, Pair {u"'''"sv, u"'''"sv, Kind::String}};
constexpr std::array kLuaPairs {Pair {u"--[["sv, u"]]"sv, Kind::Comment}, Pair {u"[["sv, u"]]"sv, Kind::String}};
constexpr std::array kShellPairs {Pair {u"<#"sv, u"#>"sv, Kind::Comment}}; // PowerShell

Rules rulesFor(Language language)
{
    switch (language) {
    case Language::CFamily: return {kSlashes, kCPairs, u"\"'"sv, kCKeywords, kCTypes};
    case Language::Python: return {kHash, kPythonPairs, u"\"'"sv, kPythonKeywords, kPythonTypes};
    case Language::Shell: return {kHash, kShellPairs, u"\"'"sv, kShellKeywords, kShellTypes};
    case Language::Batch: return {kBatchComments, {}, u"\""sv, kBatchKeywords, {}, true, false, true};
    case Language::Lua: return {kDashes, kLuaPairs, u"\"'"sv, kLuaKeywords, kLuaTypes};
    case Language::Sql: return {kDashes, kCssPairs, u"'\""sv, kSqlKeywords, kSqlTypes, true};
    case Language::Css: return {{}, kCssPairs, u"\"'"sv, {}, {}, false, true};
    case Language::Json: return {kSlashes, kCssPairs, u"\""sv, {}, kDataTypes, false, true};
    case Language::Ini: return {kIniComments, {}, u"\"'"sv, {}, kDataTypes, false, false, true};
    default: return {};
    }
}

bool nameStart(QChar c)
{
    return c.isLetter() || c == u'_' || c == u'$' || c == u'@';
}

bool namePart(QChar c)
{
    return c.isLetterOrNumber() || c == u'_' || c == u'$';
}

bool has(Words words, QStringView word, bool foldCase)
{
    if (words.empty())
        return false;
    const QString folded = foldCase ? word.toString().toLower() : QString();
    const QStringView key = foldCase ? QStringView(folded) : word;
    const std::u16string_view view(key.utf16(), static_cast<std::size_t>(key.size()));
    return std::ranges::binary_search(words, view);
}

bool startsAt(QStringView line, qsizetype at, std::u16string_view what)
{
    return line.size() - at >= static_cast<qsizetype>(what.size())
        && line.mid(at, static_cast<qsizetype>(what.size())) == QStringView(what.data(), static_cast<qsizetype>(what.size()));
}

qsizetype findFrom(QStringView line, qsizetype from, std::u16string_view what)
{
    return line.indexOf(QStringView(what.data(), static_cast<qsizetype>(what.size())), from);
}

void add(std::vector<Span>& out, qsizetype start, qsizetype end, Kind kind)
{
    if (end > start)
        out.push_back({static_cast<int>(start), static_cast<int>(end - start), kind});
}

qsizetype skipSpaces(QStringView line, qsizetype at)
{
    while (at < line.size() && line[at].isSpace())
        ++at;
    return at;
}

// A number starting at `at` (the character before is no part of a name): its end.
qsizetype numberEnd(QStringView line, qsizetype at)
{
    qsizetype end = at;
    while (end < line.size() && (line[end].isLetterOrNumber() || line[end] == u'.' || line[end] == u'_'))
        ++end;
    return end;
}

// The languages told apart by Rules alone.
std::vector<Span> scan(const Rules& rules, QStringView line, State& state)
{
    std::vector<Span> out;
    qsizetype i = 0;
    // What the line before left open.
    if (rules.pairs.empty())
        state = {}; // nothing of this language's goes on over lines
    if (state.mode == State::BlockComment || state.mode == State::LongString) {
        const Pair& pair = rules.pairs[std::min<std::size_t>(state.detail, rules.pairs.size() - 1)];
        const qsizetype close = findFrom(line, 0, pair.close);
        if (close < 0) {
            add(out, 0, line.size(), pair.kind);
            return out;
        }
        i = close + static_cast<qsizetype>(pair.close.size());
        add(out, 0, i, pair.kind);
        state = {};
    }
    const qsizetype first = skipSpaces(line, 0);
    while (i < line.size()) {
        const QChar c = line[i];
        // A comment or a string with an end of its own.
        bool paired = false;
        for (std::size_t p = 0; p < rules.pairs.size() && !paired; ++p) {
            const Pair& pair = rules.pairs[p];
            if (!startsAt(line, i, pair.open))
                continue;
            const qsizetype close = findFrom(line, i + static_cast<qsizetype>(pair.open.size()), pair.close);
            if (close < 0) {
                add(out, i, line.size(), pair.kind);
                state.mode = pair.kind == Kind::Comment ? State::BlockComment : State::LongString;
                state.detail = static_cast<quint8>(p);
                return out;
            }
            const qsizetype end = close + static_cast<qsizetype>(pair.close.size());
            add(out, i, end, pair.kind);
            i = end;
            paired = true;
        }
        if (paired)
            continue;
        if (!rules.commentAtStartOnly || i == first) {
            const bool comment = std::ranges::any_of(rules.lineComments, [&](std::u16string_view mark) { return startsAt(line, i, mark); });
            if (comment) {
                add(out, i, line.size(), Kind::Comment);
                return out;
            }
        }
        if (rules.quotes.find(c.unicode()) != std::u16string_view::npos) {
            qsizetype end = i + 1;
            while (end < line.size() && line[end] != c)
                end += line[end] == u'\\' && end + 1 < line.size() ? 2 : 1;
            end = std::min(end + 1, line.size());
            const bool key = rules.keys && skipSpaces(line, end) < line.size() && line[skipSpaces(line, end)] == u':';
            add(out, i, end, key ? Kind::Key : Kind::String);
            i = end;
            continue;
        }
        if (c.isDigit()) {
            const qsizetype end = numberEnd(line, i);
            add(out, i, end, Kind::Number);
            i = end;
            continue;
        }
        if (nameStart(c)) {
            qsizetype end = i + 1;
            while (end < line.size() && (namePart(line[end]) || (rules.keys && line[end] == u'-')))
                ++end;
            const QStringView word = line.mid(i, end - i);
            if (has(rules.keywords, word, rules.foldCase))
                add(out, i, end, Kind::Keyword);
            else if (has(rules.types, word, rules.foldCase))
                add(out, i, end, Kind::Type);
            else if (rules.keys && end < line.size() && line[end] == u':')
                add(out, i, end, Kind::Key);
            i = end;
            continue;
        }
        ++i;
    }
    return out;
}

// name = value, [section]; the value's strings, numbers and constants.
std::vector<Span> scanIni(QStringView line, State& state)
{
    const qsizetype first = skipSpaces(line, 0);
    if (first < line.size() && line[first] == u'[') {
        std::vector<Span> out;
        const qsizetype close = line.indexOf(u']', first);
        add(out, first, close < 0 ? line.size() : close + 1, Kind::Tag);
        return out;
    }
    std::vector<Span> out;
    qsizetype value = 0;
    if (first < line.size() && line[first] != u';' && line[first] != u'#') {
        qsizetype equals = line.indexOf(u'=', first);
        if (equals < 0)
            equals = line.indexOf(u':', first);
        if (equals > first) {
            qsizetype end = equals;
            while (end > first && line[end - 1].isSpace())
                --end;
            add(out, first, end, Kind::Key);
            value = equals + 1;
        }
    }
    State none;
    for (Span span : scan(rulesFor(Language::Ini), line.mid(value), value > 0 ? none : state)) {
        span.start += static_cast<int>(value);
        out.push_back(span);
    }
    return out;
}

// key: value, "- " items, "#" comments; "---".
std::vector<Span> scanYaml(QStringView line)
{
    std::vector<Span> out;
    qsizetype i = skipSpaces(line, 0);
    if (line.mid(i).startsWith(u"---") || line.mid(i).startsWith(u"...")) {
        add(out, i, line.size(), Kind::Mark);
        return out;
    }
    while (i < line.size() && line[i] == u'-' && i + 1 < line.size() && line[i + 1] == u' ') {
        add(out, i, i + 1, Kind::Keyword);
        i = skipSpaces(line, i + 1);
    }
    // A key: up to a ':' followed by a space or the end, with no quote or '#' before it.
    if (i < line.size() && line[i] != u'#' && line[i] != u'"' && line[i] != u'\'') {
        for (qsizetype colon = i; colon < line.size(); ++colon) {
            const QChar c = line[colon];
            if (c == u'#' || c == u'"' || c == u'\'' || c == u'{' || c == u'[')
                break;
            if (c == u':' && (colon + 1 == line.size() || line[colon + 1].isSpace())) {
                add(out, i, colon, Kind::Key);
                i = colon + 1;
                break;
            }
        }
    }
    while (i < line.size()) {
        const QChar c = line[i];
        if (c == u'#' && (i == 0 || line[i - 1].isSpace())) {
            add(out, i, line.size(), Kind::Comment);
            break;
        }
        if (c == u'"' || c == u'\'') {
            qsizetype end = i + 1;
            while (end < line.size() && line[end] != c)
                end += line[end] == u'\\' && end + 1 < line.size() ? 2 : 1;
            end = std::min(end + 1, line.size());
            add(out, i, end, Kind::String);
            i = end;
        } else if (c.isDigit() && (i == 0 || !namePart(line[i - 1]))) {
            const qsizetype end = numberEnd(line, i);
            add(out, i, end, Kind::Number);
            i = end;
        } else if (nameStart(c)) {
            qsizetype end = i + 1;
            while (end < line.size() && namePart(line[end]))
                ++end;
            // A constant only when it is the whole value.
            const bool alone = (end == line.size() || line[end].isSpace()) && (i == 0 || line[i - 1].isSpace());
            if (alone && has(kDataTypes, line.mid(i, end - i), false))
                add(out, i, end, Kind::Type);
            i = end;
        } else {
            ++i;
        }
    }
    return out;
}

// <tag attribute="value">, <!-- comments -->; a tag may go on over lines.
std::vector<Span> scanXml(QStringView line, State& state)
{
    std::vector<Span> out;
    qsizetype i = 0;
    if (state.mode == State::BlockComment) {
        const qsizetype close = line.indexOf(u"-->");
        if (close < 0) {
            add(out, 0, line.size(), Kind::Comment);
            return out;
        }
        add(out, 0, close + 3, Kind::Comment);
        i = close + 3;
        state = {};
    }
    bool inTag = state.mode == State::InTag;
    state = {};
    while (i < line.size()) {
        const QChar c = line[i];
        if (!inTag) {
            if (line.mid(i).startsWith(u"<!--")) {
                const qsizetype close = line.indexOf(u"-->", i + 4);
                if (close < 0) {
                    add(out, i, line.size(), Kind::Comment);
                    state.mode = State::BlockComment;
                    return out;
                }
                add(out, i, close + 3, Kind::Comment);
                i = close + 3;
            } else if (c == u'<') {
                qsizetype name = i + 1;
                if (name < line.size() && (line[name] == u'/' || line[name] == u'?' || line[name] == u'!'))
                    ++name;
                qsizetype end = name;
                while (end < line.size() && (namePart(line[end]) || line[end] == u'-' || line[end] == u':' || line[end] == u'.'))
                    ++end;
                add(out, name, end, Kind::Tag);
                inTag = end > name;
                i = std::max(end, i + 1);
            } else {
                ++i;
            }
            continue;
        }
        if (c == u'>') {
            inTag = false;
            ++i;
        } else if (c == u'"' || c == u'\'') {
            qsizetype end = line.indexOf(c, i + 1);
            end = end < 0 ? line.size() : end + 1;
            add(out, i, end, Kind::String);
            i = end;
        } else if (nameStart(c)) {
            qsizetype end = i + 1;
            while (end < line.size() && (namePart(line[end]) || line[end] == u'-' || line[end] == u':' || line[end] == u'.'))
                ++end;
            add(out, i, end, Kind::Key);
            i = end;
        } else {
            ++i;
        }
    }
    if (inTag)
        state.mode = State::InTag;
    return out;
}

// The inline parts of a Markdown line from `from` on: `code`, **strong**,
// *emphasis*, [links](…), bare addresses.
void markdownInline(QStringView line, qsizetype from, std::vector<Span>& out)
{
    qsizetype i = from;
    const auto closing = [&](qsizetype at, QStringView mark) {
        const qsizetype close = line.indexOf(mark, at);
        // Something between the marks, and no space right before the closing one.
        return close > at && !line[close - 1].isSpace() ? close : qsizetype {-1};
    };
    while (i < line.size()) {
        const QChar c = line[i];
        if (c == u'\\') {
            i += 2;
        } else if (c == u'`') {
            const qsizetype close = line.indexOf(u'`', i + 1);
            if (close < 0)
                break;
            add(out, i, close + 1, Kind::Code);
            i = close + 1;
        } else if ((c == u'*' || c == u'_') && i + 1 < line.size() && line[i + 1] == c) {
            const QStringView mark = line.mid(i, 2);
            const qsizetype close = i + 2 < line.size() && !line[i + 2].isSpace() ? closing(i + 2, mark) : -1;
            if (close < 0) {
                i += 2;
                continue;
            }
            add(out, i, i + 2, Kind::Mark);
            add(out, i + 2, close, Kind::Strong);
            add(out, close, close + 2, Kind::Mark);
            i = close + 2;
        } else if ((c == u'*' || (c == u'_' && (i == 0 || !namePart(line[i - 1])))) && i + 1 < line.size() && !line[i + 1].isSpace()) {
            const qsizetype close = closing(i + 1, line.mid(i, 1));
            if (close < 0) {
                ++i;
                continue;
            }
            add(out, i, i + 1, Kind::Mark);
            add(out, i + 1, close, Kind::Emphasis);
            add(out, close, close + 1, Kind::Mark);
            i = close + 1;
        } else if (c == u'[' || (c == u'!' && i + 1 < line.size() && line[i + 1] == u'[')) {
            const qsizetype open = c == u'!' ? i + 1 : i;
            const qsizetype text = line.indexOf(u']', open + 1);
            if (text < 0 || text + 1 >= line.size() || line[text + 1] != u'(') {
                ++i;
                continue;
            }
            const qsizetype close = line.indexOf(u')', text + 2);
            if (close < 0) {
                ++i;
                continue;
            }
            add(out, i, open + 1, Kind::Mark);
            add(out, open + 1, text, Kind::Link);
            add(out, text, close + 1, Kind::Mark);
            i = close + 1;
        } else if (line.mid(i).startsWith(u"http://") || line.mid(i).startsWith(u"https://")) {
            qsizetype end = i;
            while (end < line.size() && !line[end].isSpace() && line[end] != u')' && line[end] != u'>')
                ++end;
            add(out, i, end, Kind::Link);
            i = end;
        } else {
            ++i;
        }
    }
}

std::vector<Span> scanMarkdown(QStringView line, State& state)
{
    std::vector<Span> out;
    const qsizetype first = skipSpaces(line, 0);
    const QStringView rest = line.mid(first);
    const bool fence = rest.startsWith(u"```") || rest.startsWith(u"~~~");
    if (state.mode == State::Fence) {
        add(out, 0, line.size(), fence ? Kind::Mark : Kind::Code);
        if (fence)
            state = {};
        return out;
    }
    if (fence) {
        add(out, 0, line.size(), Kind::Mark);
        state.mode = State::Fence;
        return out;
    }
    if (rest.startsWith(u'#')) {
        qsizetype level = 0;
        while (level < rest.size() && rest[level] == u'#')
            ++level;
        if (level <= 6 && level < rest.size() && rest[level] == u' ') {
            const qsizetype text = skipSpaces(line, first + level);
            add(out, first, first + level, Kind::Mark);
            add(out, text, line.size(), level == 1 ? Kind::Heading1 : level == 2 ? Kind::Heading2 : Kind::Heading3);
            return out;
        }
    }
    if (rest.startsWith(u'>')) {
        add(out, first, first + 1, Kind::Mark);
        add(out, first + 1, line.size(), Kind::Quote);
        return out;
    }
    // A rule, or a table's line of dashes.
    if (rest.size() >= 3 && std::ranges::all_of(rest, [](QChar c) { return c == u'-' || c == u'*' || c == u'_' || c == u'|' || c == u':' || c.isSpace(); })) {
        add(out, first, line.size(), Kind::Mark);
        return out;
    }
    qsizetype from = first;
    // A list's mark: "- ", "* ", "+ ", "12. ", and a task's box.
    if (rest.size() >= 2 && (rest[0] == u'-' || rest[0] == u'*' || rest[0] == u'+') && rest[1] == u' ') {
        add(out, first, first + 1, Kind::Keyword);
        from = first + 2;
    } else {
        qsizetype digits = 0;
        while (digits < rest.size() && rest[digits].isDigit())
            ++digits;
        if (digits > 0 && digits + 1 < rest.size() && (rest[digits] == u'.' || rest[digits] == u')') && rest[digits + 1] == u' ') {
            add(out, first, first + digits + 1, Kind::Keyword);
            from = first + digits + 2;
        }
    }
    if (from > first && line.mid(from).size() >= 3 && line[from] == u'[' && line[from + 2] == u']') {
        add(out, from, from + 3, Kind::Keyword);
        from += 3;
    }
    markdownInline(line, from, out);
    return out;
}

} // namespace

Language languageFor(QStringView fileName)
{
    const qsizetype slash = std::max(fileName.lastIndexOf(u'/'), fileName.lastIndexOf(u'\\'));
    const QString name = fileName.mid(slash + 1).toString().toLower();
    const qsizetype dot = name.lastIndexOf(u'.');
    const QStringView ext = dot < 0 ? QStringView() : QStringView(name).mid(dot + 1);
    struct Entry {
        Language language;
        std::u16string_view extensions; // each between spaces
    };
    static constexpr std::array table {
        Entry {Language::CFamily, u" c h cc cpp cxx hpp hxx hh inl ipp m mm cs java js jsx mjs cjs ts tsx go rs kt kts swift dart php qml scala groovy gradle glsl hlsl proto "sv},
        Entry {Language::Python, u" py pyw pyi "sv},
        Entry {Language::Shell, u" sh bash zsh fish ps1 psm1 psd1 rb pl pm r mk cmake tcl awk dockerfile gitignore "sv},
        Entry {Language::Batch, u" bat cmd "sv},
        Entry {Language::Lua, u" lua "sv},
        Entry {Language::Sql, u" sql "sv},
        Entry {Language::Css, u" css scss less "sv},
        Entry {Language::Json, u" json jsonc json5 jsonl ndjson geojson ipynb webmanifest "sv},
        Entry {Language::Yaml, u" yaml yml "sv},
        Entry {Language::Ini, u" ini cfg conf toml properties reg inf desktop editorconfig gitconfig env "sv},
        Entry {Language::Xml, u" xml html htm xhtml svg xaml xsl xslt xsd vue csproj vcxproj props targets plist resx qrc ui "sv},
        Entry {Language::Markdown, u" md markdown mdx "sv},
    };
    if (name == u"dockerfile" || name == u"makefile" || name == u"cmakelists.txt" || name == u"gemfile" || name == u"rakefile")
        return Language::Shell;
    if (ext.isEmpty())
        return Language::None;
    const QString key = u' ' + ext.toString() + u' ';
    for (const Entry& entry : table) {
        if (QStringView(entry.extensions.data(), static_cast<qsizetype>(entry.extensions.size())).contains(key))
            return entry.language;
    }
    return Language::None;
}

std::vector<Span> highlight(Language language, QStringView line, State& state)
{
    switch (language) {
    case Language::None: return {};
    case Language::Markdown: return scanMarkdown(line, state);
    case Language::Xml: return scanXml(line, state);
    case Language::Yaml: return scanYaml(line);
    case Language::Ini: return scanIni(line, state);
    default: return scan(rulesFor(language), line, state);
    }
}

std::vector<Found> findWords(QStringView line, const QStringList& words)
{
    std::vector<Found> found;
    for (const QString& word : words) {
        // One letter or digit says nothing; one Han character does.
        if (word.isEmpty() || (word.size() == 1 && word[0].unicode() < 0x2E80))
            continue;
        for (qsizetype at = line.indexOf(word, 0, Qt::CaseInsensitive); at >= 0; at = line.indexOf(word, at + word.size(), Qt::CaseInsensitive))
            found.push_back({static_cast<int>(at), static_cast<int>(word.size())});
    }
    std::ranges::sort(found, {}, &Found::start);
    std::vector<Found> merged;
    for (const Found& f : found) {
        if (!merged.empty() && f.start <= merged.back().start + merged.back().length)
            merged.back().length = std::max(merged.back().length, f.start + f.length - merged.back().start);
        else
            merged.push_back(f);
    }
    return merged;
}

} // namespace ws::syntax
