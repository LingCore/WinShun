#include "PathText.h"

#include "Query.h"
#include "Wtf8.h"

#include <QCollator>
#include <QDirListing>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::pathtext {

namespace {

bool isDriveLetter(QChar c)
{
    return (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z');
}

bool isNameChar(QChar c)
{
    return c.unicode() >= 32 && !u"<>\"|?*:/\\"_s.contains(c);
}

// Backslashes only, one between names (a path copied from source code has
// them doubled), none at the end but after a drive. Empty for what is no
// absolute path or has characters a path cannot have.
QString normalized(QString path)
{
    path.replace(u'/', u'\\');
    if (path.size() == 2 && isDriveLetter(path[0]) && path[1] == u':')
        path += u'\\';
    const bool share = path.startsWith(u"\\\\"_s);
    if (!share && !(path.size() >= 3 && isDriveLetter(path[0]) && path[1] == u':' && path[2] == u'\\'))
        return {};
    if (path.size() > 32767)
        return {};
    QString out;
    out.reserve(path.size());
    for (qsizetype i = 0; i < path.size(); ++i) {
        const QChar c = path[i];
        if (c.unicode() < 32 || u"<>\"|?*"_s.contains(c) || (c == u':' && i != 1))
            return {};
        if (c == u'\\' && i >= 2 && out.endsWith(u'\\'))
            continue;
        out += c;
    }
    if (share) { // "\\server\share" at least
        const qsizetype slash = out.indexOf(u'\\', 2);
        if (slash <= 2 || slash + 1 >= out.size())
            return {};
    }
    while (out.endsWith(u'\\') && !(out.size() == 3 && out[1] == u':'))
        out.chop(1);
    return out;
}

// The first line, out of its quotes, a file: URL as its path, %VARIABLES%
// expanded; separators as they were. Empty for a URL of no local file.
QString unwrapped(const QString& text)
{
    QString line;
    for (const QStringView part : QStringView(text).tokenize(u'\n')) {
        line = part.trimmed().toString();
        if (!line.isEmpty())
            break;
    }
    if (line.size() >= 2 && line.startsWith(u'"') && line.endsWith(u'"'))
        line = line.mid(1, line.size() - 2).trimmed();
    if (line.startsWith(u"file:"_s, Qt::CaseInsensitive)) {
        const QUrl url(line);
        if (!url.isLocalFile())
            return {};
        line = url.toLocalFile();
    }
    if (line.contains(u'%')) { // "%USERPROFILE%\Downloads"; an unknown name stays
        static const QRegularExpression variable(u"%([^%\\\\/]+)%"_s);
        QString expanded;
        qsizetype from = 0;
        for (const QRegularExpressionMatch& m : variable.globalMatch(line)) {
            const QString value = qEnvironmentVariable(m.captured(1).toLocal8Bit().constData());
            expanded += line.mid(from, m.capturedStart() - from);
            expanded += value.isEmpty() ? m.captured() : value;
            from = m.capturedEnd();
        }
        line = expanded + line.mid(from);
    }
    return line;
}

} // namespace

QString pathFromText(const QString& text)
{
    return normalized(unwrapped(text));
}

TypedPath splitTyped(const QString& text)
{
    QString line = unwrapped(text);
    line.replace(u'/', u'\\');
    if (line.size() == 2 && isDriveLetter(line[0]) && line[1] == u':')
        line += u'\\';
    const qsizetype slash = line.lastIndexOf(u'\\');
    if (slash < 0)
        return {};
    TypedPath typed {normalized(line.left(slash + 1)), line.mid(slash + 1)};
    if (typed.folder.isEmpty() || !std::ranges::all_of(typed.name, isNameChar))
        return {};
    return typed;
}

SearchResults listTyped(const TypedPath& typed, bool foldersOnly, int max)
{
    if (typed.folder.isEmpty() || max <= 0)
        return {};
    const QString prefix = typed.folder.endsWith(u'\\') ? typed.folder : typed.folder + u'\\';
    const NameMatcher matcher(parseQuery(typed.name));
    const bool all = typed.name.trimmed().isEmpty();
    struct Found {
        SearchResult row;
        int score;
    };
    std::vector<Found> found;
    using Flag = QDirListing::IteratorFlag;
    for (const QDirListing::DirEntry& entry : QDirListing(typed.folder, foldersOnly ? Flag::DirsOnly : Flag::ExcludeOther)) {
        SearchResult row;
        row.name = entry.fileName();
        row.path = prefix + row.name;
        row.isDir = entry.isDir();
        const int score = all ? 0 : matcher.matchName(wtf8::fromUtf16(wtf8::view(row.name)));
        if (score >= 0)
            found.push_back({std::move(row), score});
    }
    QCollator collator; // Explorer's order: "2" before "10", case aside
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    const auto before = [&](const Found& a, const Found& b) {
        if (a.row.isDir != b.row.isDir)
            return a.row.isDir;
        if (a.score != b.score)
            return a.score > b.score;
        return collator.compare(a.row.name, b.row.name) < 0;
    };
    const bool self = all && QFileInfo(typed.folder).isDir();
    const auto keep = std::min<std::size_t>(found.size(), static_cast<std::size_t>(max - (self ? 1 : 0)));
    std::ranges::partial_sort(found, found.begin() + static_cast<std::ptrdiff_t>(keep), before);

    SearchResults rows;
    rows.reserve(static_cast<qsizetype>(keep) + 1);
    if (self) {
        SearchResult row;
        row.path = typed.folder;
        const qsizetype slash = typed.folder.lastIndexOf(u'\\');
        row.name = slash < 0 || slash == typed.folder.size() - 1 ? typed.folder : typed.folder.mid(slash + 1);
        row.isDir = true;
        rows.append(std::move(row));
    }
    for (std::size_t i = 0; i < keep; ++i)
        rows.append(std::move(found[i].row));
    return rows;
}

QStringList filterExtensions(const QString& fileType)
{
    // "*" alone: every file, as "*.*" is.
    static const QRegularExpression everything(u"(?:^|[\\s;,(])\\*(?:$|[\\s;,)])"_s);
    if (everything.match(fileType).hasMatch())
        return {};
    static const QRegularExpression pattern(u"\\*\\.([^\\s;,()]+)"_s);
    QStringList extensions;
    for (const QRegularExpressionMatch& m : pattern.globalMatch(fileType)) {
        const QString extension = m.captured(1).toLower();
        if (extension.contains(u'*') || extension.contains(u'?'))
            return {}; // "*.*", "*.htm*": no plain extension to look for
        if (!extensions.contains(extension))
            extensions.append(extension);
    }
    return extensions;
}

QString folderFromAddress(const QString& caption)
{
    // After the label ("Address: ", "地址: "), which is in the language of Windows.
    static const QRegularExpression start(u"(?<![A-Za-z])[A-Za-z]:\\\\|\\\\\\\\"_s);
    const QRegularExpressionMatch m = start.match(caption);
    return m.hasMatch() ? normalized(caption.mid(m.capturedStart()).trimmed()) : QString();
}

bool sameFolder(const QString& a, const QString& b)
{
    const QString x = normalized(a);
    return !x.isEmpty() && x.compare(normalized(b), Qt::CaseInsensitive) == 0;
}

QString folderFromTotalCommander(const QString& text)
{
    QString path = text.trimmed();
    const qsizetype slash = path.lastIndexOf(u'\\');
    if (slash >= 0 && (path.indexOf(u'*', slash) >= 0 || path.indexOf(u'?', slash) >= 0))
        path.truncate(slash + 1);
    path = normalized(path);
    if (path.size() >= 2 && path[1] == u':')
        path[0] = path[0].toUpper(); // "c:\Windows", as Total Commander writes it
    return path;
}

QList<OpusTab> opusTabs(const QByteArray& xml)
{
    QList<OpusTab> tabs;
    QXmlStreamReader reader(xml);
    while (reader.readNextStartElement()) {
        if (reader.name() == u"results")
            continue; // into it
        if (reader.name() != u"path") {
            reader.skipCurrentElement();
            continue;
        }
        OpusTab tab;
        const QXmlStreamAttributes attributes = reader.attributes();
        tab.lister = attributes.value(u"lister").toULongLong(nullptr, 16); // "0x6185e"
        tab.state = attributes.value(u"tab_state").toInt();
        tab.path = normalized(reader.readElementText().trimmed()); // not display_path: "C:\用户" for C:\Users
        if (!tab.path.isEmpty())
            tabs.append(std::move(tab));
    }
    return tabs;
}

} // namespace ws::pathtext
