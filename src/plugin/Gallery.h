#pragma once

// The plugin library (插件库): the list the WinShun-plugins repository
// publishes (index.json, an asset of its "index" release), what each entry
// is here -- not installed, installed, older than the list's, come with
// Win顺, or asking for a newer Win顺 -- and a package fetched from it,
// checked against the list (its size and SHA-256) before the usual install
// (Install.h) looks at it. The network is the caller's (a Fetcher: the
// app's and wsbench's WinHTTP); a file, a path or a file: URL, is read here
// (a list made for a test, and the packages next to it).

#include "Manifest.h"
#include "Download.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <vector>

namespace ws::plugin {
class Catalog;
}

namespace ws::plugin::gallery {

inline constexpr auto kIndexUrl = u"https://github.com/LingCore/WinShun-plugins/releases/download/index/index.json";
inline constexpr auto kRepositoryUrl = u"https://github.com/LingCore/WinShun-plugins";
inline constexpr qint64 kMaxIndexBytes = 4LL << 20;
inline constexpr qint64 kMaxPackageBytes = 64LL << 20;

// One plugin of the list.
enum class Review { Unspecified, Official, Reviewed, UnreviewedNative };
struct Item {
    QString id;
    Text name;
    Text description;
    QString version;
    QString author;
    QString homepage;
    QString repo; // "owner/name" on GitHub: where its packages come from, and its issues (issuesAddress)
    QStringList keywords;
    QStringList permissions; // as manifests name them: "network", "files:read" …
    std::optional<Program> program; // what it runs on; nothing: zero-code
    QString minWinShun; // empty: any
    QString url; // the package: an https address (a file when the list is one)
    QString sha256; // lower-case hex
    // The package's README, published beside the list (GitHub #304): an https
    // address and the SHA-256 of what is there; both empty when the list names none.
    QString readme;
    QString readmeSha256;
    qint64 size = 0;
    bool bundled = false; // comes with Win顺 and is updated with it
    Review review = Review::Unspecified;
    bool needsReviewConsent() const noexcept
    {
        return review == Review::UnreviewedNative || (review == Review::Unspecified && program == Program::Exe && !bundled);
    }
};

struct Index {
    QString generated; // when the list was made (ISO 8601, as written)
    std::vector<Item> items;
    int skipped = 0; // entries left out: no id, version, address, size or SHA-256 that makes sense
    const Item* find(QStringView id) const;
};

// The list as published, read from `source` (its address or file, which a
// package's relative address is taken from); nothing with `error` when it
// is no list. An entry that makes no sense is left out, not the list.
std::optional<Index> parse(const QByteArray& json, const QString& source, QString* error);

// Where a problem with a plugin is reported: to its author, not to Win顺.
// The issues of `repo` (its library entry's "owner/name"); else of the
// GitHub repository `homepage` (its manifest's or entry's) is in; else
// `homepage` itself when it is an address of the web. The library's own
// repository holds Win顺's plugins (their homepage is Win顺's own) and a
// few older plugins of others: its issues only when nothing else says
// where. Empty: nowhere known.
QString issuesAddress(const QString& repo, const QString& homepage);
// Whether a plugin fits what was typed to filter a list of them (the
// library's, the installed ones'): every word typed, parted by spaces, case
// aside, is somewhere in `words`, the lower-cased text the plugin is found
// by. Nothing typed fits them all.
bool fitsFilter(const QString& words, const QString& filter);
// The page of `repo` on GitHub, for a 主页 when the plugin gives none;
// empty when it is no repository, or the library's own (which holds
// packages, not their code).
QString repositoryAddress(const QString& repo);

enum class State {
    Install, // not here
    Update, // the user's copy is older than the list's (or cannot be read)
    Installed, // the user's copy is as new as the list's, or newer
    Bundled, // came with Win顺; the user has no copy of their own
    NeedsNewerApp, // asks for a newer Win顺 than this one
};

// What this Win顺 has of a plugin.
struct Local {
    bool userCopy = false; // in the user's plugins folder
    QString userVersion; // its manifest's; empty when that cannot be read
    bool bundled = false; // in the folder of the plugins that came with Win顺
};
Local localOf(const Catalog& catalog, const QString& bundledDir, const QString& id);
// A newer Win顺 is asked for only where a button would be: a plugin that
// came with Win顺 is updated with Win顺.
State stateOf(const Item& item, const Local& local, QStringView appVersion);

// What a fetch brought.
using Fetched = download::Fetched;
// Hears how much has come, after each piece; false stops the fetch.
using Progress = download::Progress;
// A web address, fetched; the body cut at `maxBytes` (0: no limit).
using Fetcher = download::Fetcher;
// `source` an http(s) address (through `web`) or a file (a path or a
// file: URL), read up to `maxBytes` (0: no limit). Blocking: run it off
// the GUI thread.
using download::fetch;

enum class Problem { None, TooBig, SizeMismatch, HashMismatch };
// The package against the list: not installed unless None.
Problem check(const Item& item, const QByteArray& package);

// The package in a new temporary file the install can read
// (<temp>\WinShun-<id>-XXXXXX.wsplugin); its path, or empty with `error`.
QString saveTemporary(const Item& item, const QByteArray& package, QString* error);

} // namespace ws::plugin::gallery
