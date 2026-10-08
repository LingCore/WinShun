#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace ws::release {

// Version numbers: dot-separated numbers such as 0.2.0 or v0.2.1. A suffix
// after '-' ("-beta") is ignored.
QString normalized(const QString& tag); // without the leading v
bool isNewer(const QString& candidate, const QString& base);

// A version published on GitHub (/repos/{owner}/{repo}/releases/latest).
struct Info {
    QString version; // without the v
    QString pageUrl; // the release page
    QString notes; // Markdown, in the format of docs/release-notes
    bool operator==(const Info&) const = default;
};

// The answer of the releases API; nothing if it is not one.
std::optional<Info> parse(const QByteArray& json);

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
