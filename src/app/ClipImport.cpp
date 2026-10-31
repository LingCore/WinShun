#include "ClipImport.h"

#include "ClipImage.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringDecoder>
#include <QTemporaryDir>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>

using namespace Qt::StringLiterals;

namespace ws::clipimport {

namespace {

// The same limits the clipboard watcher keeps for what is copied.
constexpr qsizetype kMaxText = 2 * 1024 * 1024; // characters
constexpr qsizetype kFormattedUpTo = 1024 * 1024; // longer text keeps no HTML / RTF
constexpr qint64 kMaxFormatBytes = 16 * 1024 * 1024;
constexpr qint64 kMaxImageBytes = 256ll * 1024 * 1024;
constexpr qint64 kMaxFile = 1024ll * 1024 * 1024; // text and CSV, read whole
constexpr int kBatchItems = 200;
constexpr qint64 kBatchBytes = 32ll * 1024 * 1024;

QString fromCodePage(UINT codePage, const QByteArray& bytes)
{
    if (bytes.isEmpty())
        return {};
    const int size = ::MultiByteToWideChar(codePage, 0, bytes.constData(), static_cast<int>(bytes.size()), nullptr, 0);
    if (size <= 0)
        return {};
    QString text(size, QChar());
    ::MultiByteToWideChar(codePage, 0, bytes.constData(), static_cast<int>(bytes.size()),
        reinterpret_cast<wchar_t*>(text.data()), size);
    return text;
}

QByteArray withoutTrailingNuls(QByteArray bytes)
{
    while (bytes.endsWith('\0'))
        bytes.chop(1);
    return bytes;
}

// Blank lines at both ends off; nothing else touched (indentation counts).
QString trimBlankLines(const QString& text)
{
    qsizetype begin = 0;
    qsizetype end = text.size();
    for (qsizetype i = 0; i < end; ++i) {
        if (text[i] == u'\n') {
            begin = i + 1;
        } else if (!text[i].isSpace()) {
            break;
        }
    }
    while (end > begin && text[end - 1].isSpace())
        --end;
    return text.mid(begin, end - begin);
}

struct Pending {
    std::vector<ClipImportItem> items;
    qint64 bytes = 0;
};

// Collects entries and hands them over in batches, counting what happened.
class Feeder {
public:
    Feeder(const Hooks& hooks, Report& report, int total)
        : m_hooks(hooks)
        , m_report(report)
        , m_total(total)
    {
    }

    bool cancelled()
    {
        if (!m_report.cancelled && m_hooks.cancelled && m_hooks.cancelled())
            m_report.cancelled = true;
        return m_report.cancelled;
    }

    void add(ClipImportItem item)
    {
        m_pending.bytes += item.text.size() * 2 + item.html.size() + item.rtf.size() + item.imageData.size();
        m_pending.items.push_back(std::move(item));
        if (m_pending.items.size() >= kBatchItems || m_pending.bytes >= kBatchBytes)
            flush();
    }

    void looked(int count = 1)
    {
        m_done += count;
        if (m_hooks.progress && (m_done % 25 == 0 || m_done == m_total))
            m_hooks.progress(m_done, m_total);
    }

    void flush()
    {
        if (m_pending.items.empty())
            return;
        const ClipImportResult result = m_hooks.sink(std::move(m_pending.items));
        m_pending = {};
        m_report.added += result.added;
        m_report.duplicates += result.duplicates;
        m_report.failed += result.failed;
        m_report.ownGroupUsed = m_report.ownGroupUsed || result.ownGroupUsed;
    }

private:
    const Hooks& m_hooks;
    Report& m_report;
    int m_total;
    int m_done = 0;
    Pending m_pending;
};

ClipImportItem textItem(const QString& text, qint64 created, qint64 used)
{
    ClipImportItem item;
    item.kind = kindOfText(text);
    item.text = text;
    item.hash = clipHash(item.kind, text);
    item.created = created;
    item.used = used;
    return item;
}

// --- text and CSV -----------------------------------------------------------

std::optional<qint64> parseTime(const QString& field)
{
    const QString t = field.trimmed();
    if (t.isEmpty())
        return std::nullopt;
    static const QRegularExpression digits(uR"(^\d{9,13}$)"_s);
    if (digits.match(t).hasMatch()) {
        const qint64 n = t.toLongLong();
        return t.size() >= 12 ? n : n * 1000; // milliseconds or seconds since the epoch
    }
    QDateTime when = QDateTime::fromString(t, Qt::ISODateWithMs);
    if (!when.isValid()) {
        for (const QString& format : {u"yyyy-MM-dd HH:mm:ss"_s, u"yyyy/M/d H:mm:ss"_s, u"yyyy-M-d H:mm:ss"_s,
                 u"yyyy-MM-dd HH:mm"_s, u"yyyy/M/d H:mm"_s, u"yyyy.M.d H:mm:ss"_s, u"yyyy年M月d日 H:mm:ss"_s,
                 u"yyyy年M月d日 H:mm"_s, u"yyyy-MM-dd"_s, u"yyyy/M/d"_s, u"yyyy年M月d日"_s}) {
            when = QDateTime::fromString(t, format);
            if (when.isValid())
                break;
        }
    }
    if (!when.isValid())
        return std::nullopt;
    return when.toMSecsSinceEpoch();
}

Report importPlainText(const QString& path, const Options& options, const Hooks& hooks, bool csv)
{
    Report report;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        report.error = Error::Unreadable;
        return report;
    }
    if (file.size() > kMaxFile) {
        report.error = Error::TooLarge;
        return report;
    }
    const QString text = decodeText(file.readAll());
    const qint64 now = options.now > 0 ? options.now : QDateTime::currentMSecsSinceEpoch();

    // (text, time or 0), in the order of the file.
    std::vector<std::pair<QString, qint64>> entries;
    if (!csv) {
        for (const QString& part : splitText(text, options.split))
            entries.emplace_back(part, 0);
    } else {
        const std::vector<QStringList> rows = parseCsv(text);
        qsizetype textColumn = 0;
        qsizetype timeColumn = -1;
        std::size_t first = 0;
        if (!rows.empty()) {
            static const QStringList textNames {u"text"_s, u"content"_s, u"内容"_s, u"文本"_s, u"clip"_s, u"clipboard"_s,
                u"剪贴板"_s, u"剪贴板内容"_s, u"复制内容"_s};
            static const QStringList timeNames {u"time"_s, u"date"_s, u"datetime"_s, u"created"_s, u"timestamp"_s,
                u"时间"_s, u"日期"_s, u"创建时间"_s, u"复制时间"_s};
            const QStringList& head = rows.front();
            qsizetype named = -1;
            for (qsizetype i = 0; i < head.size(); ++i) {
                const QString name = head[i].trimmed().toLower();
                if (named < 0 && textNames.contains(name))
                    named = i;
                else if (timeColumn < 0 && timeNames.contains(name))
                    timeColumn = i;
            }
            if (named >= 0 || timeColumn >= 0) { // a header row
                first = 1;
                if (named >= 0) {
                    textColumn = named;
                } else { // only a time column is named: the text is the first of the others
                    textColumn = timeColumn == 0 ? 1 : 0;
                }
            }
        }
        for (std::size_t r = first; r < rows.size(); ++r) {
            const QStringList& row = rows[r];
            const QString value = textColumn < row.size() ? row[textColumn] : QString();
            const qint64 when = timeColumn >= 0 && timeColumn < row.size() ? parseTime(row[timeColumn]).value_or(0) : 0;
            entries.emplace_back(value, when);
        }
    }

    Feeder feeder(hooks, report, static_cast<int>(entries.size()));
    qint64 stamp = now;
    for (const auto& [value, when] : entries) {
        if (feeder.cancelled())
            break;
        feeder.looked();
        const qint64 at = when > 0 ? when : stamp;
        stamp -= 1000;
        if (value.trimmed().isEmpty()) {
            ++report.skipped;
            continue;
        }
        if (value.size() > kMaxText) {
            ++report.skipped;
            continue;
        }
        feeder.add(textItem(value, at, at));
    }
    feeder.flush();
    return report;
}

// --- Ditto ------------------------------------------------------------------

// Ditto keeps what was copied in Main (one row per entry; a group is a row
// with bIsGroup, an entry in it has the group's lID as lParentID, -1 for
// none) and Data (one row per clipboard format: strClipBoardFormat, ooData
// the bytes as Windows had them). Dates are Unix seconds. Sources: Ditto's
// src/DatabaseUtilities.cpp (the tables), src/Clip.cpp (the rows).
struct DittoGroup {
    QString name;
    qint64 parent = -1;
};

QString dittoGroupPath(const QHash<qint64, DittoGroup>& groups, qint64 id)
{
    QStringList names;
    for (int depth = 0; depth < 16 && groups.contains(id); ++depth) {
        const DittoGroup& g = groups[id];
        names.prepend(g.name.isEmpty() ? QCoreApplication::translate("ClipImport", "未命名") : g.name);
        id = g.parent;
    }
    return names.join(u'/');
}

struct DittoFormat {
    qint64 id = 0;
    qint64 size = 0;
};

QStringList dittoFiles(const QByteArray& drop)
{
    // A DROPFILES: where the list starts, a POINT, two BOOLs (the second:
    // wide characters), then the names one after another, an empty one last.
    if (drop.size() < 20)
        return {};
    DWORD offset = 0;
    DWORD wide = 0;
    std::memcpy(&offset, drop.constData(), 4);
    std::memcpy(&wide, drop.constData() + 16, 4);
    if (offset < 20 || offset >= static_cast<DWORD>(drop.size()))
        return {};
    QStringList files;
    if (wide) {
        const auto* p = reinterpret_cast<const char16_t*>(drop.constData() + offset);
        const auto* end = reinterpret_cast<const char16_t*>(drop.constData() + drop.size() - drop.size() % 2);
        while (p < end && *p) {
            const char16_t* q = p;
            while (q < end && *q)
                ++q;
            files.append(QString::fromUtf16(p, q - p));
            p = q + 1;
        }
    } else {
        for (const QByteArray& name : drop.mid(offset).split('\0')) {
            if (name.isEmpty())
                break;
            files.append(fromCodePage(CP_ACP, name));
        }
    }
    return files;
}

Report importDitto(const QString& path, const Hooks& hooks)
{
    Report report;
    static std::atomic<int> counter {0};
    QTemporaryDir temp;
    if (!temp.isValid()) {
        report.error = Error::CannotCopy;
        return report;
    }
    // Ditto may be running: read a copy (with the log of a WAL database).
    const QString copy = temp.filePath(u"Ditto.db"_s);
    for (const QString& suffix : {QString(), u"-wal"_s, u"-shm"_s}) {
        if (!QFile::exists(path + suffix))
            continue;
        if (!QFile::copy(path + suffix, copy + suffix)) {
            report.error = Error::CannotCopy;
            return report;
        }
    }
    const QString connection = u"ditto-import-%1"_s.arg(++counter);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, connection);
        db.setDatabaseName(copy);
        if (!db.open()) {
            report.error = Error::NotDitto;
        } else {
            const auto scalar = [&db](const QString& sql) -> QVariant {
                QSqlQuery q(db);
                return q.exec(sql) && q.next() ? q.value(0) : QVariant();
            };
            QSet<QString> columns;
            {
                QSqlQuery info(db);
                if (info.exec(u"PRAGMA table_info(Main)"_s)) {
                    while (info.next())
                        columns.insert(info.value(1).toString().toLower());
                }
            }
            QSet<QString> tables;
            {
                QSqlQuery q(db);
                if (q.exec(u"SELECT name FROM sqlite_master WHERE type = 'table'"_s)) {
                    while (q.next())
                        tables.insert(q.value(0).toString());
                }
            }
            if (!tables.contains(u"Main"_s) || !tables.contains(u"Data"_s) || !columns.contains(u"lid"_s)) {
                report.error = Error::NotDitto;
            } else {
                const auto col = [&columns](const char* name, const char* fallback) {
                    return columns.contains(QString::fromLatin1(name).toLower()) ? QString::fromLatin1(name)
                                                                                  : QString::fromLatin1(fallback);
                };
                QHash<qint64, DittoGroup> groups;
                {
                    QSqlQuery q(db);
                    if (q.exec(u"SELECT lID, mText, %1 FROM Main WHERE %2 <> 0"_s
                                   .arg(col("lParentID", "-1"), col("bIsGroup", "0")))) {
                        while (q.next())
                            groups.insert(q.value(0).toLongLong(), {q.value(1).toString(), q.value(2).toLongLong()});
                    }
                }
                // Top of Ditto's list first: pinned ("sticky") entries, then the
                // newest; inside a group by the group's own order.
                const QString inGroup = u"(%1 > 0)"_s.arg(col("lParentID", "-1"));
                const QString order = u"CASE WHEN %1 THEN %2 ELSE %3 END DESC, CASE WHEN %1 THEN %4 ELSE %5 END DESC, lID DESC"_s
                                          .arg(inGroup, col("stickyClipGroupOrder", "0"), col("stickyClipOrder", "0"),
                                              col("clipGroupOrder", "0"), col("clipOrder", "0"));
                const int total = scalar(u"SELECT count(*) FROM Main WHERE %1 = 0"_s.arg(col("bIsGroup", "0"))).toInt();
                QSqlQuery rows(db);
                rows.setForwardOnly(true);
                const bool ok = rows.exec(u"SELECT lID, %1, mText, %2, %3, %6 FROM Main WHERE %4 = 0 ORDER BY %5"_s
                                              .arg(col("lDate", "0"), col("lParentID", "-1"), col("lastPasteDate", "0"),
                                                  col("bIsGroup", "0"), order, col("lDontAutoDelete", "0")));
                if (!ok) {
                    report.error = Error::NotDitto;
                } else {
                    QSqlQuery formats(db);
                    formats.prepare(u"SELECT lID, strClipBoardFormat, length(ooData) FROM Data WHERE lParentID = ?"_s);
                    QSqlQuery blob(db);
                    blob.prepare(u"SELECT ooData FROM Data WHERE lID = ?"_s);
                    Feeder feeder(hooks, report, total);
                    while (rows.next()) {
                        if (feeder.cancelled())
                            break;
                        feeder.looked();
                        const qint64 id = rows.value(0).toLongLong();
                        qint64 created = rows.value(1).toLongLong();
                        created = created > 100'000'000'000 ? created : created * 1000; // seconds, but milliseconds would do
                        qint64 used = rows.value(4).toLongLong();
                        used = used > 100'000'000'000 ? used : used * 1000;
                        const QString description = rows.value(2).toString();
                        const qint64 parent = rows.value(3).toLongLong();

                        QHash<QString, DittoFormat> have; // by lower-case format name
                        formats.addBindValue(id);
                        if (formats.exec()) {
                            while (formats.next()) {
                                // The newest row of a format wins (Ditto reads them newest first).
                                const QString name = formats.value(1).toString().toLower();
                                const DittoFormat f {formats.value(0).toLongLong(), formats.value(2).toLongLong()};
                                if (!have.contains(name) || have[name].id < f.id)
                                    have.insert(name, f);
                            }
                        }
                        const auto read = [&](const char* name, qint64 limit) -> QByteArray {
                            const auto it = have.constFind(QString::fromLatin1(name).toLower());
                            if (it == have.constEnd() || it->size <= 0 || it->size > limit)
                                return {};
                            blob.addBindValue(it->id);
                            return blob.exec() && blob.next() ? blob.value(0).toByteArray() : QByteArray();
                        };

                        ClipImportItem item;
                        item.keepForever = rows.value(5).toLongLong() != 0;
                        item.created = created > 0 ? created : used;
                        item.used = used > 0 ? used : created;
                        if (parent > 0 && groups.contains(parent))
                            item.group = dittoGroupPath(groups, parent);

                        // The same order the watcher reads in: files, text, a picture.
                        QString text;
                        ClipKind kind = ClipKind::Text;
                        if (have.contains(u"cf_hdrop"_s)) {
                            const QStringList files = dittoFiles(read("CF_HDROP", 16 * 1024 * 1024));
                            if (!files.isEmpty()) {
                                kind = ClipKind::Files;
                                text = files.join(u'\n');
                            }
                        }
                        if (text.isEmpty() && have.contains(u"cf_unicodetext"_s)) {
                            const QByteArray raw = read("CF_UNICODETEXT", kMaxText * 2 + 2);
                            text = QString::fromUtf16(reinterpret_cast<const char16_t*>(raw.constData()), raw.size() / 2);
                            text.truncate(text.indexOf(QChar(0)) >= 0 ? text.indexOf(QChar(0)) : text.size());
                        }
                        if (text.trimmed().isEmpty() && have.contains(u"cf_text"_s)) {
                            const QByteArray raw = read("CF_TEXT", kMaxText);
                            text = fromCodePage(CP_ACP, raw.left(std::max<qsizetype>(0, raw.indexOf('\0') >= 0 ? raw.indexOf('\0') : raw.size())));
                        }
                        const bool bigText = have.contains(u"cf_unicodetext"_s) && have[u"cf_unicodetext"_s].size > kMaxText * 2 + 2;
                        if (bigText && text.isEmpty()) {
                            ++report.skipped;
                            continue;
                        }
                        if (kind == ClipKind::Text && !text.trimmed().isEmpty()) {
                            kind = kindOfText(text);
                            if (text.size() <= kFormattedUpTo) {
                                item.html = withoutTrailingNuls(read("HTML Format", kMaxFormatBytes));
                                item.rtf = withoutTrailingNuls(read("Rich Text Format", kMaxFormatBytes));
                            }
                        }
                        if (text.trimmed().isEmpty()) {
                            DecodedClipImage picture;
                            for (const ClipImageOffer& offer : clipImageOffers()) {
                                const QByteArray name = offer.name.toString().toLatin1();
                                picture = decodeClipImage(read(name.constData(), kMaxImageBytes));
                                if (!picture.firstFrame.isNull())
                                    break;
                            }
                            if (picture.firstFrame.isNull()) {
                                QByteArray dib = read("CF_DIBV5", kMaxImageBytes);
                                if (dib.isEmpty())
                                    dib = read("CF_DIB", kMaxImageBytes);
                                if (!dib.isEmpty())
                                    picture = encodeClipImage(imageFromDib(dib));
                            }
                            if (picture.firstFrame.isNull()) {
                                // Only HTML or RTF, or nothing usable: Ditto's own description is the text.
                                if (!description.trimmed().isEmpty() && (have.contains(u"html format"_s) || have.contains(u"rich text format"_s))) {
                                    text = description;
                                    kind = kindOfText(text);
                                    item.html = withoutTrailingNuls(read("HTML Format", kMaxFormatBytes));
                                    item.rtf = withoutTrailingNuls(read("Rich Text Format", kMaxFormatBytes));
                                } else {
                                    ++report.skipped;
                                    continue;
                                }
                            } else {
                                item.kind = ClipKind::Image;
                                item.imageData = std::move(picture.data);
                                item.imageFormat = picture.format;
                                item.animated = picture.animated;
                                item.width = picture.firstFrame.width();
                                item.height = picture.firstFrame.height();
                                item.hash = std::move(picture.hash);
                                feeder.add(std::move(item));
                                continue;
                            }
                        }
                        if (text.size() > kMaxText) {
                            ++report.skipped;
                            continue;
                        }
                        item.kind = kind;
                        item.text = std::move(text);
                        item.hash = clipHash(item.kind, item.text);
                        feeder.add(std::move(item));
                    }
                    feeder.flush();
                }
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return report;
}

} // namespace

// --- public -----------------------------------------------------------------

Source sourceOf(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == u"csv"_s)
        return Source::Csv;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) && file.read(15) == QByteArrayView("SQLite format 3"))
        return Source::Ditto;
    return Source::Text;
}

QString groupName(const QString& path)
{
    if (sourceOf(path) == Source::Ditto)
        return QCoreApplication::translate("ClipImport", "Ditto 导入");
    return QCoreApplication::translate("ClipImport", "%1 导入").arg(QFileInfo(path).completeBaseName());
}

QString decodeText(const QByteArray& bytes)
{
    if (bytes.startsWith("\xEF\xBB\xBF"))
        return QString::fromUtf8(bytes.mid(3));
    if (bytes.startsWith("\xFF\xFE"))
        return QStringDecoder(QStringConverter::Utf16LE)(bytes.mid(2));
    if (bytes.startsWith("\xFE\xFF"))
        return QStringDecoder(QStringConverter::Utf16BE)(bytes.mid(2));
    QStringDecoder utf8(QStringConverter::Utf8);
    const QString text = utf8(bytes);
    if (!utf8.hasError())
        return text;
    return fromCodePage(936, bytes); // GBK: what Chinese Excel saves as "CSV"
}

QStringList splitText(const QString& text, Split split)
{
    QStringList parts;
    switch (split) {
    case Split::Lines:
        for (QString line : text.split(u'\n')) {
            if (line.endsWith(u'\r'))
                line.chop(1);
            if (!line.trimmed().isEmpty())
                parts.append(line);
        }
        break;
    case Split::Paragraphs: {
        static const QRegularExpression blank(uR"(\r?\n(?:[ \t]*\r?\n)+)"_s);
        for (const QString& part : text.split(blank)) {
            const QString trimmed = trimBlankLines(part);
            if (!trimmed.isEmpty())
                parts.append(trimmed);
        }
        break;
    }
    case Split::Whole: {
        const QString trimmed = trimBlankLines(text);
        if (!trimmed.isEmpty())
            parts.append(trimmed);
        break;
    }
    }
    return parts;
}

std::vector<QStringList> parseCsv(const QString& text, QChar delimiter)
{
    if (delimiter.isNull()) {
        // Comma, unless the first row has more semicolons or tabs (outside quotes).
        int commas = 0, semicolons = 0, tabs = 0;
        bool quoted = false;
        for (const QChar c : text) {
            if (c == u'"') {
                quoted = !quoted;
            } else if (!quoted) {
                if (c == u'\n')
                    break;
                commas += c == u',';
                semicolons += c == u';';
                tabs += c == u'\t';
            }
        }
        delimiter = tabs > commas && tabs >= semicolons ? u'\t' : semicolons > commas ? u';' : u',';
    }
    std::vector<QStringList> rows;
    QStringList row;
    QString field;
    bool quoted = false;
    bool any = false; // something in this record
    const auto endField = [&] {
        row.append(field);
        field.clear();
    };
    const auto endRow = [&] {
        if (any || !row.isEmpty() || !field.isEmpty()) {
            endField();
            rows.push_back(std::move(row));
        }
        row = {};
        any = false;
    };
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (quoted) {
            if (c == u'"') {
                if (i + 1 < text.size() && text[i + 1] == u'"') {
                    field += u'"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field += c;
            }
        } else if (c == u'"' && field.isEmpty()) {
            quoted = true;
            any = true;
        } else if (c == delimiter) {
            any = true;
            endField();
        } else if (c == u'\n' || c == u'\r') {
            if (c == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n')
                ++i;
            endRow();
        } else {
            field += c;
            any = true;
        }
    }
    endRow();
    return rows;
}

Report importFile(const QString& path, const Options& options, const Hooks& hooks)
{
    if (!QFileInfo::exists(path)) {
        Report report;
        report.error = Error::Missing;
        return report;
    }
    switch (sourceOf(path)) {
    case Source::Ditto:
        return importDitto(path, hooks);
    case Source::Csv:
        return importPlainText(path, options, hooks, true);
    case Source::Text:
        break;
    }
    return importPlainText(path, options, hooks, false);
}

} // namespace ws::clipimport
