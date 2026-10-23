#include "PathText.h"

#include "Query.h"
#include "Wtf8.h"

#include <QCollator>
#include <QDirListing>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimeZone>
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

// "%USERPROFILE%\Downloads" with its variables' values; an unknown name stays.
QString expandVariables(QString line)
{
    if (!line.contains(u'%'))
        return line;
    static const QRegularExpression variable(u"%([^%\\\\/]+)%"_s);
    QString expanded;
    qsizetype from = 0;
    for (const QRegularExpressionMatch& m : variable.globalMatch(line)) {
        const QString value = qEnvironmentVariable(m.captured(1).toLocal8Bit().constData());
        expanded += line.mid(from, m.capturedStart() - from);
        expanded += value.isEmpty() ? m.captured() : value;
        from = m.capturedEnd();
    }
    return expanded + line.mid(from);
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
    return expandVariables(line);
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

bool inChosenFolders(const QStringList& chosen, const QString& path)
{
    QStringView p = QStringView(path);
    while (p.size() > 2 && p.endsWith(u'\\'))
        p.chop(1);
    return std::ranges::any_of(chosen, [&](const QString& folder) {
        QStringView f = QStringView(folder);
        while (f.size() > 2 && f.endsWith(u'\\'))
            f.chop(1);
        return !f.isEmpty() && p.startsWith(f, Qt::CaseInsensitive)
            && (p.size() == f.size() || p[f.size()] == u'\\');
    });
}

TypedPath typedOutside(const QString& text, const QStringList& chosen)
{
    TypedPath typed = splitTyped(text);
    if (typed.folder.size() < 2 || !isDriveLetter(typed.folder[0]) || typed.folder[1] != u':'
        || inChosenFolders(chosen, typed.folder))
        return {};
    return typed;
}

ScopeNote scopeNote(bool foldersOnly, bool outside, bool dismissed, int rows, int fewRows, qsizetype typed) noexcept
{
    if (!foldersOnly)
        return ScopeNote::None;
    if (outside)
        return ScopeNote::Outside;
    return !dismissed && rows < fewRows && typed >= 2 ? ScopeNote::Chosen : ScopeNote::None;
}

RelativePath splitRelative(const QString& text)
{
    const QString line = text.trimmed();
    const auto separator = [](QChar c) { return c == u'\\' || c == u'/'; };
    if (line.contains(u'\n') || line.startsWith(u'!') || !std::ranges::any_of(line, separator)
        || (line.size() >= 2 && line[1] == u':') || line.startsWith(u"\\\\"_s) || line.startsWith(u"//"_s))
        return {};
    RelativePath path;
    path.folder = separator(line.back());
    for (const QStringView name : QStringView(line).tokenize(u'/')) {
        for (const QStringView part : name.tokenize(u'\\')) {
            if (part.isEmpty())
                continue;
            if (part == u"." || part == u".." || part.front().isSpace() || part.back().isSpace()
                || !std::ranges::all_of(part, isNameChar))
                return {};
            path.names.append(part.toString());
        }
    }
    if (path.names.isEmpty())
        return {};
    return path;
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
        row.modified = static_cast<std::uint32_t>(std::max<qint64>(0, entry.lastModified(QTimeZone::UTC).toSecsSinceEpoch()));
        if (!row.isDir)
            row.size = entry.size(); // for the sorts by size (#44)
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
    const QFileInfo folderInfo(typed.folder);
    const bool self = all && folderInfo.isDir();
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
        row.modified = static_cast<std::uint32_t>(std::max<qint64>(0, folderInfo.lastModified(QTimeZone::UTC).toSecsSinceEpoch()));
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

QList<XYplorerTab> xyplorerTabs(const QString& reply)
{
    QList<XYplorerTab> tabs;
    const QStringList lines = reply.split(u'\n');
    if (lines.size() < 2)
        return tabs;
    const bool dual = lines[0].trimmed() == u"1";
    for (int pane = 1; pane <= 2 && pane < lines.size(); ++pane) {
        if (pane == 2 && !dual)
            break; // the other pane is not shown
        bool first = true;
        for (const QStringView token : QStringView(lines[pane]).tokenize(u'|')) {
            QString path = token.trimmed().toString();
            if (const qsizetype search = path.indexOf(u'?'); search >= 0)
                path.truncate(search); // a quick search in that folder
            path = normalized(path);
            if (path.isEmpty())
                continue;
            if (path[1] == u':')
                path[0] = path[0].toUpper();
            tabs.append({std::move(path), first ? pane : 0});
            first = false;
        }
    }
    return tabs;
}

QString xyplorerQuoted(const QString& text)
{
    QString quoted = text;
    quoted.replace(u'\'', u"''"_s);
    return u'\'' + quoted + u'\'';
}

namespace {

// A path as Pulse keeps it, "\\?\F:\Work" or "\\?\UNC\nas\photos", as on a
// drive or share; empty for a place of its own ("", "pulse:recycle").
QString fromPulse(QString path)
{
    if (path.startsWith(u"\\\\?\\UNC\\"_s, Qt::CaseInsensitive))
        path = u"\\\\"_s + path.mid(8);
    else if (path.startsWith(u"\\\\?\\"_s))
        path = path.mid(4);
    return normalized(path);
}

} // namespace

QList<PulseWindow> pulseWindows(const QByteArray& session)
{
    QList<PulseWindow> windows;
    const QJsonObject root = QJsonDocument::fromJson(session).object();
    QJsonArray list = root.value(u"windows").toArray();
    if (list.isEmpty() && root.contains(u"layoutTabs"))
        list.append(root); // version 7 and before: the one window, at the top
    for (const QJsonValue& value : std::as_const(list)) {
        const QJsonObject object = value.toObject();
        PulseWindow window;
        window.left = object.value(u"left").toInt();
        window.top = object.value(u"top").toInt();
        window.right = object.value(u"right").toInt();
        window.bottom = object.value(u"bottom").toInt();
        const QJsonArray tabs = object.value(u"layoutTabs").toArray();
        const int active = object.value(u"activeTab").toInt();
        QList<XYplorerTab> behind; // the other tabs'
        for (int t = 0; t < tabs.size(); ++t) {
            const QJsonObject tab = tabs[t].toObject();
            const QJsonArray panes = tab.value(u"panes").toArray();
            const int focused = tab.value(u"focused").toInt();
            QList<int> order; // the pane with the focus first
            if (focused >= 0 && focused < panes.size())
                order.append(focused);
            for (int p = 0; p < panes.size(); ++p) {
                if (p != focused)
                    order.append(p);
            }
            for (const int p : std::as_const(order)) {
                QString path = fromPulse(panes[p].toObject().value(u"path").toString());
                if (t == active && p == focused && !path.isEmpty()) {
                    // As Pulse names a tab: the last name, "C:" for a drive.
                    const QStringView name = path.endsWith(u'\\') ? QStringView(path).chopped(1) : QStringView(path);
                    window.frontName = name.mid(name.lastIndexOf(u'\\') + 1).toString();
                }
                if (path.isEmpty())
                    continue;
                if (t != active)
                    behind.append({std::move(path), 0});
                else
                    window.folders.append({std::move(path), p == focused ? 1 : 2});
            }
        }
        window.folders += behind;
        windows.append(std::move(window));
    }
    return windows;
}

QString pulseTitle(const QString& name)
{
    return name + u' ' + QChar(0x2014) + u" Pulse"_s; // an em dash
}

QString commandArgument(const QString& path)
{
    if (!path.isEmpty() && !path.contains(u' ') && !path.contains(u'\t') && !path.contains(u'"'))
        return path;
    QString quoted = u"\""_s;
    qsizetype backslashes = 0;
    for (QChar c : path) {
        if (c == u'\\') {
            ++backslashes;
            continue;
        }
        quoted += QString(backslashes * (c == u'"' ? 2 : 1), u'\\');
        if (c == u'"')
            quoted += u'\\';
        quoted += c;
        backslashes = 0;
    }
    return quoted + QString(backslashes * 2, u'\\') + u'"';
}

CustomCommand customCommand(const QString& commandLine, const QString& path)
{
    const QString line = expandVariables(commandLine.trimmed());
    CustomCommand command;
    QString rest;
    if (line.startsWith(u'"')) {
        const qsizetype end = line.indexOf(u'"', 1);
        if (end < 0)
            return {};
        command.program = line.mid(1, end - 1).trimmed();
        rest = line.mid(end + 1);
    } else {
        const qsizetype space = line.indexOf(u' ');
        command.program = space < 0 ? line : line.left(space);
        rest = space < 0 ? QString() : line.mid(space + 1);
    }
    if (command.program.isEmpty())
        return {};
    static const QString placeholder = u"{path}"_s;
    const QString argument = commandArgument(path);
    rest = rest.trimmed();
    const bool quotedByUser = rest.contains(u'"' + placeholder + u'"');
    if (quotedByUser)
        rest.replace(u'"' + placeholder + u'"', argument); // not in quotes twice
    if (rest.contains(placeholder))
        rest.replace(placeholder, argument);
    else if (!quotedByUser)
        rest = rest.isEmpty() ? argument : rest + u' ' + argument;
    command.arguments = rest;
    return command;
}

namespace {

// An element in XML text, found by its name: where it begins, where its
// start tag ends, where its content ends (its end tag begins) and where it
// ends. For <name … />, the content ends where it ends.
struct Element {
    qsizetype begin = -1;
    qsizetype tagEnd = -1;
    qsizetype contentEnd = -1;
    qsizetype end = -1;

    bool found() const { return begin >= 0; }
    bool empty() const { return contentEnd == end; }
};

// Past the '>' of the tag at `begin`; quoted values may hold one. -1 if none.
qsizetype tagEnd(QByteArrayView xml, qsizetype begin)
{
    char quote = 0;
    for (qsizetype i = begin + 1; i < xml.size(); ++i) {
        const char c = xml[i];
        if (quote)
            quote = c == quote ? 0 : quote;
        else if (c == '"' || c == '\'')
            quote = c;
        else if (c == '>')
            return i + 1;
    }
    return -1;
}

// The first element called `name` from `from` that ends by `to`; none of
// the same name inside it (prefs.oxc has none).
Element findElement(QByteArrayView xml, QByteArrayView name, qsizetype from, qsizetype to)
{
    const QByteArray open = '<' + name.toByteArray();
    for (qsizetype at = xml.indexOf(open, from); at >= 0 && at < to; at = xml.indexOf(open, at + 1)) {
        const qsizetype next = at + open.size();
        if (next >= xml.size() || !QByteArrayView(" \t\r\n/>").contains(xml[next]))
            continue; // <dblclkfoo
        Element e {.begin = at, .tagEnd = tagEnd(xml, at)};
        if (e.tagEnd < 0 || e.tagEnd > to)
            return {};
        if (xml[e.tagEnd - 2] == '/') {
            e.contentEnd = e.end = e.tagEnd;
            return e;
        }
        e.contentEnd = xml.indexOf("</" + name.toByteArray() + '>', e.tagEnd);
        e.end = e.contentEnd + name.size() + 3;
        return e.contentEnd >= 0 && e.end <= to ? e : Element {};
    }
    return {};
}

struct BlankClickPlace {
    Element dblclk;
    Element bgCommands; // in dblclk; maybe not there
    Element event; // in bgCommands, id="0"; maybe not there
    QXmlStreamAttributes attributes; // the event's
};

BlankClickPlace findBlankClick(QByteArrayView prefs)
{
    BlankClickPlace place;
    place.dblclk = findElement(prefs, "dblclk", 0, prefs.size());
    if (!place.dblclk.found() || place.dblclk.empty())
        return place;
    place.bgCommands = findElement(prefs, "bg_commands", place.dblclk.tagEnd, place.dblclk.contentEnd);
    if (!place.bgCommands.found())
        return place;
    for (qsizetype from = place.bgCommands.tagEnd;;) {
        const Element e = findElement(prefs, "event", from, place.bgCommands.contentEnd);
        if (!e.found())
            break;
        QXmlStreamReader reader(prefs.sliced(e.begin, e.end - e.begin).toByteArray());
        if (reader.readNextStartElement() && reader.attributes().value(u"id") == u"0") {
            place.event = e;
            place.attributes = reader.attributes();
            break;
        }
        from = e.end;
    }
    return place;
}

} // namespace

OpusBlankClick opusBlankClick(const QByteArray& prefs)
{
    const BlankClickPlace place = findBlankClick(prefs);
    OpusBlankClick click;
    click.hasPlace = place.dblclk.found() && !place.dblclk.empty();
    if (place.event.found()) {
        click.element = prefs.sliced(place.event.begin, place.event.end - place.event.begin);
        click.command = place.attributes.value(u"command").toString();
        click.enabled = place.attributes.value(u"enable") == u"yes";
    }
    return click;
}

QByteArray opusBlankClickEvent(const QString& command)
{
    return "<event command=\"" + command.toHtmlEscaped().toUtf8() + "\" enable=\"yes\" id=\"0\" />";
}

QByteArray withOpusBlankClick(const QByteArray& prefs, const QByteArray& element)
{
    const BlankClickPlace place = findBlankClick(prefs);
    if (!place.dblclk.found() || place.dblclk.empty())
        return {};
    QByteArray result = prefs;
    if (place.event.found()) {
        qsizetype begin = place.event.begin;
        if (element.isEmpty()) { // its line goes with it
            while (begin > 0 && (prefs[begin - 1] == ' ' || prefs[begin - 1] == '\t'))
                --begin;
            if (begin > 0 && prefs[begin - 1] == '\n')
                --begin;
            if (begin > 0 && prefs[begin - 1] == '\r')
                --begin;
        }
        return result.replace(begin, place.event.end - begin, element);
    }
    if (element.isEmpty())
        return result;
    if (const Element& bg = place.bgCommands; bg.found()) {
        if (!bg.empty())
            return result.insert(bg.contentEnd, element);
        // <bg_commands … />: opened up to hold it.
        return result.replace(bg.end - 2, 2, '>' + element + "</bg_commands>");
    }
    // Where Opus writes it: before <mouse_hook_delay>, else last.
    const Element delay = findElement(prefs, "mouse_hook_delay", place.dblclk.tagEnd, place.dblclk.contentEnd);
    return result.insert(delay.found() ? delay.begin : place.dblclk.contentEnd, "<bg_commands>" + element + "</bg_commands>");
}

} // namespace ws::pathtext
