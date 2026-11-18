#pragma once

#include <QString>

// winshun:// links (GitHub #358): a web page, a note (Obsidian), a desktop
// shortcut or a script opens Win顺 with something typed, or its settings.
//
//   winshun://search/<text>      the search box opens with <text> typed; everything after
//                                "search/" is the text, slashes included (a path), "+" stays a "+"
//   winshun://search?q=<text>    the same, as a form: here "+" is a space
//   winshun://search             just the search box
//   winshun://settings           the settings window
//   winshun://settings/<page>    ... at that page (one of the ids of AiSettings' sections)
//   winshun://settings/plugin/<id>  ... at that plugin's card (its id, as "winshun.capture"; GitHub #390)
//
// The scheme and the words search and settings are read in any case; the text
// is percent-decoded as UTF-8 (raw Unicode is read as it is). Anything else
// opens the search box. A link never opens, runs or deletes a file: it can
// only type words into the search box (which the user then reads) or show the
// settings, because a web page can offer these links to be clicked.
// WinShunSearch.exe (src/stub) forwards the link as it is (--uri "<link>"),
// and it is read here, in the running Win顺 (App's uri callback).
namespace ws::winshunuri {

enum class Kind {
    Open, // the search box, nothing typed
    Search, // the search box, `text` typed (never empty)
    Settings, // the settings window; `page` says where, or is empty
};

struct Request {
    Kind kind = Kind::Open;
    QString text; // Search: what to type
    // Settings: the page asked for as a plain id (lower case letters, digits
    // and underscores, "-" read as "_"), "" for none; whether it is a page
    // is for the caller to say.
    QString page;
    // Settings: the plugin whose card to show, as an id a plugin may have
    // (lower case letters, digits, ".", "_" and "-"), "" for none.
    QString plugin;
    friend bool operator==(const Request&, const Request&) = default;
};

// The most characters of a typed text (UTF-16 units); the rest is cut off, never in the middle of a pair.
inline constexpr int kMaxTextChars = 2000;
inline constexpr int kMaxPageChars = 64;

// What `link` asks for; a text that is no winshun:// link opens the search box.
Request parse(const QString& link);

} // namespace ws::winshunuri
