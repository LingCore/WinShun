#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace ws::release {

// Version numbers: dot-separated numbers such as 0.2.0 or v0.2.1. A suffix
// after '-' ("-beta") is ignored.
QString normalized(const QString& tag); // without the leading v
bool isNewer(const QString& candidate, const QString& base);

// A file of a release, as the update service lists it: GitHub's asset
// with the service's own additions (mirrors, the digest read from GitHub).
struct Asset {
    QString name;
    QString url; // browser_download_url: GitHub's
    QStringList mirrors; // in the order to try them; third parties
    qint64 size = 0;
    QString sha256; // lowercase hex; empty when there was no valid "sha256:..." digest
    bool operator==(const Asset&) const = default;
};

// A version published: the answer of the update service (lcstp.com, in the
// format of GitHub's /repos/{owner}/{repo}/releases/latest).
struct Info {
    QString version; // without the v
    QString pageUrl; // the release page
    QString notes; // Markdown, in the format of docs/release-notes
    QList<Asset> assets;
    bool operator==(const Info&) const = default;
};

// The answer of the releases API; nothing if it is not one.
std::optional<Info> parse(const QByteArray& json);

// What an answer of the update service says to a copy of version
// `current`: a newer version (in `newer`), none (also 204, no content: no
// version for this Windows and package), or nothing understood.
enum class Answer { Newer, UpToDate, Invalid };
Answer judge(int status, const QByteArray& body, const QString& current, std::optional<Info>* newer);

// The same version: 0.15.0 and 0.15.0.0 (a file version) are.
bool sameVersion(const QString& a, const QString& b);

// What an update downloads. The service sends one asset, the package asked
// for: the Setup ("-x64-setup.exe") or the portable zip ("-x64-portable.zip").
// Nothing when it sends another number of them, a name that is not that
// package, no size, or no valid digest (never downloaded unchecked).
enum class Package { Setup, Portable };
std::optional<Asset> packageAsset(const Info& info, Package package);

// Where to download it from, in order: the mirrors, then GitHub. Only
// https:// addresses; `loopbackHttp` (tests with a fake service on this
// computer, WINSHUN_UPDATE_FEED) lets http://127.0.0.1 and localhost in too.
QStringList downloadUrls(const Asset& asset, bool loopbackHttp = false);
bool loopbackHttpUrl(const QString& url);

// The page "more" opens: the service's html_url when it is on lcstp.com,
// else that site's own page for Win顺.
QString releasePage(const QString& pageUrl);
inline constexpr auto kProjectPage = "https://lcstp.com/projects/winshun";

// What the update dialog shows from the release notes. Their format: an
// opening paragraph, Chinese first, then English; under "## 新功能 · What's
// new" one line "- emoji 中文" per item, then an indented line of English.
struct Highlight {
    QString symbol; // the leading emoji, or empty
    QString text;
    bool operator==(const Highlight&) const = default;
};
QString summary(const QString& notes, bool chinese); // the opening paragraph's half in that language
QList<Highlight> highlights(const QString& notes, bool chinese);

} // namespace ws::release
