#pragma once

// The JSON of winshun.devtools ("开发工具"): read as it is written -- keys in
// their order, numbers as their digits (a 19-digit id stays exact) -- and,
// for what logs hold, leniently: comments, single quotes, bare keys,
// trailing commas, Python's True / False / None. A string that is itself
// JSON (a log's "body": "{\"a\":1}") carries that document under it.

#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>
#include <vector>

namespace ws::devtools {

enum class Kind : quint8 { Object, Array, String, Number, Bool, Null };

struct Node {
    QString key; // a member's name, decoded (in an object)
    QString text; // a string: decoded; a number: as written; true, false, null
    int parent = -1;
    int first = -1; // the first child; a string's own document (embedded) when it is JSON
    int last = -1;
    int next = -1; // the next sibling
    int count = 0; // children
    int index = 0; // its place among its siblings
    Kind kind = Kind::Null;
    bool embedded = false; // a string that is JSON: `first` is the root of that document
};

// What a lenient read let pass, for the note under the tree.
enum Relaxation : unsigned {
    Comments = 1u << 0,
    SingleQuotes = 1u << 1,
    BareKeys = 1u << 2,
    TrailingCommas = 1u << 3,
    PythonWords = 1u << 4, // True, False, None, undefined
    NonFinite = 1u << 5, // NaN, Infinity
};

struct Error {
    QString message;
    qsizetype offset = 0; // in the text read
    int line = 1; // 1-based, as an editor counts
    int column = 1;
};

struct Document {
    std::vector<Node> nodes; // nodes[0] is the root; none when nothing was read
    unsigned relaxed = 0;

    bool isEmpty() const { return nodes.empty(); }
    // The node whose children stand under `node`: a string's own document, else itself.
    int container(int node) const { return nodes[node].embedded ? nodes[node].first : node; }
    // What `node` is to the eye: an embedded string shows as its document's object or array.
    Kind shape(int node) const { return nodes[container(node)].kind; }
    int childCount(int node) const { return nodes[container(node)].count; }
    int firstChild(int node) const { return nodes[container(node)].first; }
    bool expandable(int node) const;
};

// One value at `from` (whitespace and comments before it skipped); `end`
// gets where it stopped. nullopt, and `error`, when it is not JSON there.
std::optional<Document> parse(QStringView text, qsizetype from = 0, qsizetype* end = nullptr, Error* error = nullptr);

// What the tool makes of a paste, a log line or a file: the JSON itself, or
// JSON in quotes, escaped (\"), after a log line's time and level, or one a
// line; `notes` say which. `error` when none of that is JSON: where the
// plain reading failed.
struct Reading {
    Document document;
    std::optional<Error> error;
    QStringList notes;
    bool empty = false; // only whitespace
};
Reading read(const QString& text);
inline constexpr qsizetype kMostCharacters = 32 * 1024 * 1024;

enum class Layout { Pretty, Compact };
// `node` and what is under it as JSON. An embedded string stays a string.
QString write(const Document& document, int node, Layout layout, const QString& indent = QStringLiteral("  "));
QString quoted(QStringView text); // a JSON string literal, quotes and all
QString escaped(QStringView json); // `json` as it goes inside a string literal: no quotes round it
// "对象 · 12 个键", "数组 · 3 项", "字符串", ...
QString describe(const Document& document, int node);
QString relaxations(unsigned relaxed); // "注释、单引号"

} // namespace ws::devtools
