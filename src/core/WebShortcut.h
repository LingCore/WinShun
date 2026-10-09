#pragma once

#include <QList>
#include <QString>
#include <QStringView>

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

    bool searches() const { return url.contains(u"%s"); }
    // `home`, else for a search its site ("https://github.com/"), or for a
    // link of an app's own scheme ("obsidian://search?query=%s") the link
    // with no words; for a page, the page.
    QString homeUrl() const;
    // `url` with %s replaced by the words, percent-encoded as UTF-8.
    QString searchUrl(const QString& words) const;

    friend bool operator==(const WebShortcut&, const WebShortcut&) = default;
};

using WebShortcuts = QList<WebShortcut>;

// What Win顺 comes with: one, to show how it goes ("winshun" opens its page
// on GitHub). The rest are the user's own.
WebShortcuts defaultWebShortcuts();

// "winshun-web:<keyword>": stands for a shortcut's row (SearchResult::path).
QString webPath(const QString& keyword, const QString& words = {});
bool isWebPath(QStringView path) noexcept;

struct TypedKeyword {
    qsizetype index = -1; // into the shortcuts
    QString words; // what follows the keyword, trimmed; empty: the keyword alone
};

// The shortcut whose keyword the text starts with, alone or followed by a
// space and the words to search for ("gh", "gh  WinShun"). A shortcut that
// does not search takes no words: "page x" is an ordinary query then.
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

// A keyword one can type: not empty, no spaces or quotes.
bool isValidWebKeyword(QStringView keyword) noexcept;

// An address as a row shows it: no https:// and www., %-escapes decoded
// ("github.com/search?q=Win Shun"); another scheme stays.
QString displayWebUrl(const QString& url);

} // namespace ws
