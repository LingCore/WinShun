#pragma once

// What a plugin answers with (the plugin design, appendix A): items, read
// out of JSON (an object with `items`, a bare array, or plain text lines),
// and the effects of an action. Plain values; the launcher turns them into
// rows (stage 2).

#include "Manifest.h"

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <cstdint>

namespace ws::plugin {

inline constexpr int kMaxItems = 200; // per response; the rest is dropped

// "file:C:\…", "ext:pdf", "app:<AUMID>", "glyph:U+E8E5", "color:#ff8800",
// "icon.png" (in the plugin folder), "data:image/png;base64,…",
// "site:https://example.com/page" (the site's own icon, as a browser shows it).
struct IconSpec {
    enum class Kind { None, File, Ext, App, Glyph, Color, PluginFile, Data, Site };
    Kind kind = Kind::None;
    QString value; // the part after the prefix; PluginFile: the relative path; Site: "https://host/"
    friend bool operator==(const IconSpec&, const IconSpec&) = default;
};
IconSpec parseIcon(const QString& text); // Kind::None when it is none of these (or a path that leaves the folder)

struct Open {
    enum class Kind { None, Path, Url };
    Kind kind = Kind::None;
    QString target;
    QString args; // Path: a program's arguments
    QString cwd;
    friend bool operator==(const Open&, const Open&) = default;
};

struct Item {
    QString id;
    QString title;
    QString subtitle;
    // The second line while the keyboard is on another row ("按 ↑ 选中，再
    // Enter 接收" on a row that came in above it); empty: `subtitle` always.
    QString subtitleAway;
    std::uint32_t modified = 0; // Unix seconds (0: not supplied)
    std::uint32_t created = 0;
    qint64 size = -1; // bytes (-1: not supplied)
    IconSpec icon;
    int score = 50; // 0–100
    bool hasScore = false;
    QString match; // what to filter and highlight on instead of the title
    QString autocomplete; // Tab
    Open open; // the default action, done by Win顺 without asking the plugin
    QString copy; // Ctrl+C; empty: the title
    QString previewText;
    QList<Action> actions; // overriding the command's
    bool valid = true;
    QJsonValue data; // opaque; goes back with the action
    // Zero-code: what the row does instead of opening something.
    QString searchQuery; // run a search in the launcher
    QString searchScope;
    QString setQuery; // put this in the box
    friend bool operator==(const Item&, const Item&) = default;
};
using Items = QList<Item>;

struct QueryResult {
    Items items;
    double rerun = 0; // seconds; 0: no
    int cacheSeconds = 0;
    bool truncated = false; // more than kMaxItems came
};

Item parseItem(const QJsonObject& object);
// The `result` of a query response, or what a once plugin printed as JSON:
// {items, rerun, cache} or a bare array.
QueryResult parseResult(const QJsonValue& value);
// What a once plugin printed: JSON as above, else one item per non-empty
// line (plain text: Enter copies it).
QueryResult parseOutput(const QByteArray& text);

struct Effect {
    enum class Kind { Hide, Keep, SetQuery, Notify, Copy, Open, Replace };
    Kind kind = Kind::Hide;
    QString text; // SetQuery, Notify, Copy
    Open open;
    Items items; // Replace
    friend bool operator==(const Effect&, const Effect&) = default;
};
using Effects = QList<Effect>;

// {effects: [...]} or a bare array; nothing means Hide.
Effects parseEffects(const QJsonValue& value);
QJsonObject itemToJson(const Item& item); // for the action request: as it came, in effect

} // namespace ws::plugin

Q_DECLARE_METATYPE(ws::plugin::Items)
Q_DECLARE_METATYPE(ws::plugin::Effects)
