#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <functional>
#include <optional>
#include <vector>

namespace ws {

class NameMatcher;

// A web page opened by a keyword typed in the launcher's 全部: "gh" opens
// GitHub, "gh WinShun" searches GitHub for WinShun. Typing its name
// ("github", or "baidu" for 百度) finds it among the results too.
struct WebShortcut {
    QString keyword; // "gh": one word, matched ignoring case
    QString name; // "GitHub"
    // "https://github.com/search?q=%s": %s is where the words go. Without
    // %s, the page itself, and the keyword takes no words.
    QString url;
    QString home; // what the keyword alone opens; empty: see homeUrl()
    // A picture its user chose ("3f2a….png", see isValidCustomIconName),
    // kept by Win顺; empty: its site's own icon.
    QString icon;
    // Where the site's search suggestions come from as one types after the
    // keyword ("https://api.bing.com/osjson.aspx?query=%s", %s as in `url`);
    // empty: none. See WebSuggest.h for what replies are read.
    QString suggest;

    bool searches() const { return url.contains(u"%s"); }
    // `suggest` with %s replaced by the words, as searchUrl does; empty when
    // it has none or the shortcut does not search.
    QString suggestUrl(const QString& words) const;
    // What its rows' icon is asked for by (FileIconProvider::webIconUrl):
    // "custom:<icon>" for a picture of the user's, else the home page.
    QString iconRef() const;
    // `home`, else for a search its site ("https://github.com/"), or for a
    // link of an app's own scheme ("obsidian://search?query=%s") the link
    // with no words; for a page, the page.
    QString homeUrl() const;
    // `url` with %s replaced by the words, percent-encoded as UTF-8.
    QString searchUrl(const QString& words) const;

    friend bool operator==(const WebShortcut&, const WebShortcut&) = default;
};

using WebShortcuts = QList<WebShortcut>;

// Presets of the optional web plugin: common Chinese sites ("baidu 天气"
// searches 百度), and "winshun" for its page on GitHub. The rest are the
// user's own.
WebShortcuts defaultWebShortcuts();
// Raised when defaults are added: an older settings file gets the new ones
// once (Settings, WebSearch/Defaults), those its user removed stay removed.
constexpr int kWebDefaultsVersion = 2;
// `shortcuts` with the defaults it lacks after them: those whose keyword is
// not taken, nor their site with the same search capability (a user's own
// "bd" for 百度 keeps 百度 once; a fixed repository page cannot replace search).
WebShortcuts withNewDefaults(WebShortcuts shortcuts, const WebShortcuts& defaults);

// The address a web search for `words` opens when no keyword was typed (the
// OCR's 网页搜索): the first shortcut of the list that searches, so the one at
// the top is the default and the list's order is the user's to change; Bing's
// when the list has none. Empty when `words` has none.
QString defaultWebSearchUrl(const WebShortcuts& shortcuts, const QString& words);
// That shortcut: the first of the list that searches; -1 when none does.
qsizetype defaultWebSearch(const WebShortcuts& shortcuts) noexcept;

// "winshun-web:<keyword>": stands for a shortcut's row (SearchResult::path).
QString webPath(const QString& keyword, const QString& words = {});
bool isWebPath(QStringView path) noexcept;

struct TypedKeyword {
    qsizetype index = -1; // into the shortcuts
    QString words; // what follows the keyword, trimmed; empty: the keyword alone
    bool atEnd = false; // the words came first, the keyword last ("WinShun gh")
};

// The shortcut whose keyword the text starts with, alone or followed by a
// space and the words to search for ("gh", "gh  WinShun"). A shortcut that
// does not search takes no words: "page x" is an ordinary query then. Else
// the one whose keyword the text ends with, after the words ("WinShun gh",
// GitHub #320): one that searches only; the keyword first wins.
std::optional<TypedKeyword> typedKeyword(const WebShortcuts& shortcuts, QStringView text);

struct WebHit {
    qsizetype index = 0; // into the shortcuts
    int score = 0; // as an app's (higher is better)
};

// The shortcuts whose name matches, best first (by pinyin too: "bd" for 百度).
std::vector<WebHit> searchWebShortcuts(const WebShortcuts& shortcuts, const NameMatcher& matcher);

// What a typed address becomes, or empty if it is not one Win顺 opens:
// trimmed; "github.com/search?q=%s" gets https://; a search for "%s" copied
// from the address bar ("q=%25s") gets its %s back. Any scheme of two
// letters or more but file: (a word typed into a file path could start a
// program); http and https need a host.
QString normalizeWebUrl(const QString& text);

// A suggestion address as typed in the settings, or empty if it is not one
// (an http(s) address with %s): tidied as normalizeWebUrl does, and
// Listary's {query} read as %s, so one copied from there works too.
QString normalizeSuggestUrl(const QString& text);

// The suggestion address Win顺 knows for a search address's site (百度,
// 必应, 哔哩哔哩, 淘宝, Google), each checked to answer without signing in;
// empty for any other site. For the defaults, for shortcuts written before
// there were suggestions (no Suggest= in the file), and to offer in the
// settings for a new one.
QString knownSuggestUrl(const QString& searchUrl);

// A keyword one can type: not empty, no spaces or quotes.
bool isValidWebKeyword(QStringView keyword) noexcept;

// Who has a keyword already, as the settings window and AI both ask before
// giving it away: another web shortcut (not the one at `except`, the one
// being edited; -1: none), a favorite, or the screenshot history (pass its
// keyword only while capture is on).
struct KeywordClaim {
    enum class By { Nobody, WebShortcut, Favorite, Capture };
    By by = By::Nobody;
    QString owner; // the shortcut's or favorite's name, or the capture keyword
};
KeywordClaim keywordClaim(const QString& keyword, const WebShortcuts& shortcuts, qsizetype except,
    const QString& captureKeyword, const std::function<QString(const QString& keyword)>& favoriteOwner);

// An address as a row shows it: no https:// and www., %-escapes decoded
// ("github.com/search?q=Win Shun"); another scheme stays.
QString displayWebUrl(const QString& url);

// The name a shortcut gets when its user gives none: its site ("github.com"),
// or for an app's link the keyword. `shortcut` has its keyword, url and home.
QString defaultWebShortcutName(const WebShortcut& shortcut);

// ---- import and export ---------------------------------------------------
// A file of web shortcuts, to take them to another computer or share them:
// JSON, {"format": "winshun-web-shortcuts", "version": 1, "shortcuts": [{
// "keyword", "name", "url", "home"?, "icon"?, "suggest"? }]}, an icon as a
// PNG in a data: address, so the file is all one needs. Without "suggest"
// (a file from before suggestions), the one Win顺 knows for the site.

// A custom icon's name as Win顺 keeps it: 16 hex digits of its PNG's
// SHA-1 and ".png". Anything else (a path, "..") is not one.
bool isValidCustomIconName(QStringView name) noexcept;

struct WebShortcutFileEntry {
    WebShortcut shortcut; // `icon` empty
    QByteArray iconData; // the picture the file holds for it, still to be decoded; empty: none
};

// `iconPng(name)` gives the PNG of a custom icon, empty when it is gone.
QByteArray writeWebShortcutsFile(const WebShortcuts& shortcuts,
    const std::function<QByteArray(const QString& icon)>& iconPng);

struct WebShortcutFile {
    QList<WebShortcutFileEntry> entries;
    int skipped = 0; // entries with no keyword one can type or no address Win顺 opens
};
// Read as the settings file is (readWebShortcuts): addresses tidied, a
// missing name the keyword's. The later of two with one keyword wins. Null
// for a file that is not one of these, `newer` set when it is one a newer
// Win顺 wrote.
std::optional<WebShortcutFile> readWebShortcutsFile(QByteArrayView json, bool* newer = nullptr);

struct WebMerge {
    int added = 0;
    int updated = 0; // a keyword already there, with another address, name or icon
    int unchanged = 0;
};
// `from` into `into`: a keyword already there (ignoring case) is replaced in
// its place, a new one goes at the end.
WebMerge mergeWebShortcuts(WebShortcuts& into, const WebShortcuts& from);

// ---- site icons ----------------------------------------------------------
// A shortcut's row shows its site's own icon, fetched from the site the first
// time (nothing of other sites ships with Win顺) and kept on disk.

// What a site's icon is kept under: the host of the shortcut's home page,
// lower case ("fanyi.baidu.com", not "baidu.com": 百度翻译 has an icon of its
// own); empty for an address that is not http(s).
QString siteIconKey(const QString& homeUrl);

// The icons a page names, as absolute addresses. `icons` are what a browser
// shows beside the address: <link rel="icon"> with sizes of 64 or more
// first (largest first), then the other icons as the page has them, and last
// /favicon.ico of the page's site, which nearly every site has. `touchIcons`
// (apple-touch-icon, made for phones' home screens: 120-512 px, largest
// first) are sharper but often another picture (bilibili's says "WEB"): only
// for a site whose browser icon is too small. An SVG icon (drawn at any
// size) comes after the large ones and before the other icons: a picture
// declared 64 px or more is the site's own drawing as it meant it, an SVG
// is drawn by QtSvg, which gets some (filters, CSS) wrong. `pageUrl` is
// where the page was read from, after redirects.
struct SiteIconCandidates {
    QStringList icons;
    QStringList touchIcons;
};
SiteIconCandidates siteIconCandidates(QByteArrayView html, const QString& pageUrl);

} // namespace ws
