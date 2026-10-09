#include "ClipStore.h"

#include "Pinyin.h"
#include "Wtf8.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>
#include <limits>
#include <optional>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr qint64 kDayMs = 24ll * 60 * 60 * 1000;
constexpr qsizetype kPinyinChars = 4096; // pinyin is for words near the start: searching it further costs too much

QString unquoted(QString line)
{
    line = line.trimmed();
    if (line.size() >= 2 && line.startsWith(u'"') && line.endsWith(u'"'))
        line = line.mid(1, line.size() - 2);
    return line;
}

QByteArray hashOf(ClipKind kind, const QString& text)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    const char tag = static_cast<char>(kind == ClipKind::Files ? 'f' : 't'); // a path as text and as a file differ
    hash.addData(QByteArrayView(&tag, 1));
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(text.utf16()), text.size() * 2));
    return hash.result();
}

// NULL in the database for nothing (the formats an entry does not have).
QVariant blob(const QByteArray& bytes)
{
    return bytes.isEmpty() ? QVariant(QMetaType::fromType<QByteArray>()) : QVariant(bytes);
}

// Text never NULL: an empty QString may be a null one, which binds as NULL.
QString text(const QString& s)
{
    return s.isNull() ? QString(u""_s) : s;
}

bool run(QSqlQuery& query, const char* what)
{
    if (query.exec())
        return true;
    qWarning().noquote() << "Clipboard history:" << what << "failed:" << query.lastError().text();
    return false;
}

bool run(QSqlDatabase& db, const QString& sql)
{
    QSqlQuery query(db);
    if (query.exec(sql))
        return true;
    qWarning().noquote() << "Clipboard history:" << sql.left(60) << "failed:" << query.lastError().text();
    return false;
}

} // namespace

ClipKind kindOfText(const QString& text)
{
    const QString t = text.trimmed();
    if (t.isEmpty() || t.size() > 8192)
        return ClipKind::Text;
    static const QRegularExpression link(uR"(^(?:(?:https?|ftp)://|www\.[^\s./]+\.)\S+$)"_s,
        QRegularExpression::CaseInsensitiveOption);
    if (!t.contains(u'\n') && link.match(t).hasMatch())
        return ClipKind::Link;
    // "C:\dir\file", "\\server\share\x", also quoted as Explorer's "Copy as path" puts it.
    static const QRegularExpression path(uR"(^(?:[A-Za-z]:[\\/]|\\\\[^\\/\s]+[\\/])[^"<>|?*\t]*$)"_s);
    const QStringList lines = t.split(u'\n', Qt::SkipEmptyParts);
    if (lines.size() > 100)
        return ClipKind::Text;
    for (const QString& line : lines) {
        const QString p = unquoted(line);
        if (p.size() > 1024 || !path.match(p).hasMatch())
            return ClipKind::Text;
    }
    return ClipKind::Path;
}

QStringList Clip::files() const
{
    if (kind != ClipKind::Files && kind != ClipKind::Path)
        return {};
    QStringList paths;
    for (const QString& line : text.split(u'\n', Qt::SkipEmptyParts)) {
        const QString p = unquoted(line);
        if (!p.isEmpty())
            paths.append(QDir::toNativeSeparators(p));
    }
    return paths;
}

// --- ClipMatcher ------------------------------------------------------------

ClipMatcher::ClipMatcher(const QString& query)
{
    QString spaced = query;
    spaced.replace(QChar(0x3000), u' '); // the full-width space of Chinese input
    for (const QString& word : spaced.split(u' ', Qt::SkipEmptyParts)) {
        const bool source = word.size() > 1 && word.startsWith(u'@');
        Term term {source ? word.mid(1) : word, std::nullopt};
        const std::string folded = term.text.toLower().toStdString();
        if (pinyin::isPinyinTerm(folded))
            term.pinyin.emplace(folded);
        (source ? m_sourceTerms : m_terms).append(std::move(term));
    }
}

bool ClipMatcher::find(const Term& term, QStringView text, qsizetype* start, qsizetype* length)
{
    const qsizetype i = text.indexOf(term.text, 0, Qt::CaseInsensitive);
    if (i >= 0) {
        *start = i;
        *length = term.text.size();
        return true;
    }
    if (!term.pinyin || !term.pinyin->valid())
        return false;
    const QStringView head = text.left(kPinyinChars);
    const auto span = term.pinyin->findUtf16(
        std::u16string_view(reinterpret_cast<const char16_t*>(head.utf16()), static_cast<std::size_t>(head.size())));
    if (!span)
        return false;
    *start = static_cast<qsizetype>(span->start);
    *length = static_cast<qsizetype>(span->length);
    return true;
}

bool ClipMatcher::matches(const Clip& clip) const
{
    qsizetype start = 0;
    qsizetype length = 0;
    for (const Term& term : m_sourceTerms) {
        if (!find(term, clip.source, &start, &length))
            return false;
    }
    for (const Term& term : m_terms) {
        if (!find(term, clip.text, &start, &length))
            return false;
    }
    return true;
}

QList<std::pair<qsizetype, qsizetype>> ClipMatcher::spans(QStringView text) const
{
    QList<std::pair<qsizetype, qsizetype>> spans;
    for (const Term& term : m_terms) {
        qsizetype start = 0;
        qsizetype length = 0;
        if (find(term, text, &start, &length))
            spans.append({start, length});
    }
    return spans;
}

ClipBundle bundle(std::span<const Clip* const> clips, const std::function<QString(const Clip&)>& wholeText,
    QStringView separator)
{
    ClipBundle result;
    bool allFiles = true;
    int joined = 0;
    for (const Clip* clip : clips) {
        if (clip->kind == ClipKind::Image) {
            ++result.skippedImages;
            continue;
        }
        ++joined;
        allFiles = allFiles && clip->kind == ClipKind::Files;
    }
    if (joined == 0)
        return result;
    if (allFiles) {
        for (const Clip* clip : clips) {
            if (clip->kind != ClipKind::Files)
                continue;
            for (const QString& file : clip->files()) {
                if (!result.files.contains(file, Qt::CaseInsensitive))
                    result.files.append(file);
            }
        }
        return result;
    }
    QStringList parts;
    for (const Clip* clip : clips) {
        if (clip->kind == ClipKind::Image)
            continue;
        parts.append(clip->kind == ClipKind::Files ? clip->files().join(u"\r\n"_s) : wholeText(*clip));
    }
    result.text = parts.join(separator);
    return result;
}

// --- ClipStore --------------------------------------------------------------

ClipStore::ClipStore(QString folder)
    : m_folder(std::move(folder))
    , m_connection(u"clipboard-%1"_s.arg(reinterpret_cast<quintptr>(this), 0, 16))
{
}

ClipStore::~ClipStore()
{
    {
        QSqlDatabase db = QSqlDatabase::database(m_connection, false);
        if (db.isOpen())
            db.close();
    } // no handle left when the connection goes
    QSqlDatabase::removeDatabase(m_connection);
}

bool ClipStore::open(qint64 now)
{
    QDir().mkpath(m_folder + u"/images"_s);
    QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, m_connection);
    db.setDatabaseName(m_folder + u"/clipboard.db"_s);
    if (!db.open()) {
        qWarning().noquote() << "Clipboard history: cannot open the database:" << db.lastError().text();
        return false;
    }
    // WAL: a write is an append, and without a sync on every commit (the
    // history is not worth slowing a copy down for).
    run(db, u"PRAGMA journal_mode=WAL"_s);
    run(db, u"PRAGMA synchronous=NORMAL"_s);
    // AUTOINCREMENT: an id is never used twice, so a removed entry's picture
    // (kept for undo) is never taken for a new one's.
    const bool created = run(db,
                             u"CREATE TABLE IF NOT EXISTS clips ("
                             "id INTEGER PRIMARY KEY AUTOINCREMENT, kind INTEGER NOT NULL, "
                             "text TEXT NOT NULL DEFAULT '', length INTEGER NOT NULL DEFAULT 0, html BLOB, rtf BLOB, "
                             "hash BLOB NOT NULL, created INTEGER NOT NULL, used INTEGER NOT NULL, "
                             "source TEXT NOT NULL DEFAULT '', sourcePath TEXT NOT NULL DEFAULT '', "
                             "groupId INTEGER NOT NULL DEFAULT 0, width INTEGER NOT NULL DEFAULT 0, "
                             "height INTEGER NOT NULL DEFAULT 0, bytes INTEGER NOT NULL DEFAULT 0)"_s)
        && run(db, u"CREATE TABLE IF NOT EXISTS groups (id INTEGER PRIMARY KEY, name TEXT NOT NULL, position INTEGER NOT NULL)"_s)
        && run(db, u"CREATE TABLE IF NOT EXISTS state (key TEXT PRIMARY KEY, value TEXT NOT NULL)"_s)
        && run(db, u"INSERT OR IGNORE INTO groups (id, name, position) VALUES (1, '', 0)"_s);
    if (!created)
        return false;

    QSqlQuery groups(db);
    groups.prepare(u"SELECT id, name FROM groups ORDER BY position, id"_s);
    if (!run(groups, "Reading the groups"))
        return false;
    while (groups.next())
        m_groups.push_back({groups.value(0).toLongLong(), groups.value(1).toString()});

    QSqlQuery clips(db);
    clips.setForwardOnly(true);
    clips.prepare(u"SELECT id, kind, substr(text, 1, %1), length, hash, created, used, source, sourcePath, groupId, "
                  "width, height, bytes, html IS NOT NULL, rtf IS NOT NULL FROM clips ORDER BY used DESC, id DESC"_s
            .arg(kTextInMemory));
    if (!run(clips, "Reading the history"))
        return false;
    QSet<qint64> ids;
    while (clips.next()) {
        Clip clip;
        clip.id = clips.value(0).toLongLong();
        clip.kind = static_cast<ClipKind>(std::clamp(clips.value(1).toInt(), 0, static_cast<int>(ClipKind::Image)));
        clip.text = clips.value(2).toString();
        clip.textLength = clips.value(3).toLongLong();
        clip.hash = clips.value(4).toByteArray();
        clip.created = clips.value(5).toLongLong();
        clip.used = clips.value(6).toLongLong();
        clip.source = clips.value(7).toString();
        clip.sourcePath = clips.value(8).toString();
        clip.group = clips.value(9).toLongLong();
        clip.width = clips.value(10).toInt();
        clip.height = clips.value(11).toInt();
        clip.bytes = clips.value(12).toLongLong();
        clip.html = clips.value(13).toBool();
        clip.rtf = clips.value(14).toBool();
        if (clip.group != 0 && !group(clip.group))
            clip.group = 0;
        ids.insert(clip.id);
        m_clips.push_back(std::move(clip));
    }
    m_open = true;

    // Pictures nothing refers to any more (removed, then the app quit before
    // they were discarded).
    const QDir images(m_folder + u"/images"_s);
    for (const QString& name : images.entryList({u"*.png"_s}, QDir::Files)) {
        bool ok = false;
        const qint64 id = QStringView(name).chopped(4).toLongLong(&ok);
        if (!ok || !ids.contains(id))
            QFile::remove(images.filePath(name));
    }
    prune(now);
    return true;
}

void ClipStore::setLimits(Limits limits, qint64 now)
{
    m_limits = limits;
    if (m_open)
        prune(now);
}

const Clip* ClipStore::clip(qint64 id) const
{
    const auto it = std::find_if(m_clips.begin(), m_clips.end(), [id](const Clip& c) { return c.id == id; });
    return it == m_clips.end() ? nullptr : &*it;
}

const ClipGroup* ClipStore::group(qint64 id) const
{
    const auto it = std::find_if(m_groups.begin(), m_groups.end(), [id](const ClipGroup& g) { return g.id == id; });
    return it == m_groups.end() ? nullptr : &*it;
}

int ClipStore::groupSize(qint64 id) const
{
    return static_cast<int>(std::count_if(m_clips.begin(), m_clips.end(), [id](const Clip& c) { return c.group == id; }));
}

QString ClipStore::imagePath(qint64 id) const
{
    return QDir::toNativeSeparators(m_folder + u"/images/%1.png"_s.arg(id));
}

void ClipStore::moveToFront(std::size_t index)
{
    if (index > 0 && index < m_clips.size())
        std::rotate(m_clips.begin(), m_clips.begin() + static_cast<std::ptrdiff_t>(index),
            m_clips.begin() + static_cast<std::ptrdiff_t>(index) + 1);
}

qint64 ClipStore::add(const ClipCapture& capture, qint64 now)
{
    if (!m_open)
        return 0;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    const QByteArray hash = capture.hash.isEmpty() ? hashOf(capture.kind, capture.text) : capture.hash;
    const auto same = std::find_if(m_clips.begin(), m_clips.end(), [&](const Clip& c) { return c.hash == hash; });
    if (same != m_clips.end() && capture.initial)
        return same->id; // listed already, and not copied just now
    if (same != m_clips.end()) {
        // Copied again: to the top, from where it was copied this time, with
        // the formats it came with this time if it has them.
        QSqlQuery query(db);
        query.prepare(u"UPDATE clips SET used = ?, source = ?, sourcePath = ?, html = COALESCE(?, html), "
                      "rtf = COALESCE(?, rtf) WHERE id = ?"_s);
        query.addBindValue(now);
        query.addBindValue(text(capture.source));
        query.addBindValue(text(capture.sourcePath));
        query.addBindValue(blob(capture.html));
        query.addBindValue(blob(capture.rtf));
        query.addBindValue(same->id);
        if (!run(query, "Updating an entry"))
            return 0;
        same->used = now;
        same->source = capture.source;
        same->sourcePath = capture.sourcePath;
        same->html = same->html || !capture.html.isEmpty();
        same->rtf = same->rtf || !capture.rtf.isEmpty();
        const qint64 id = same->id;
        moveToFront(static_cast<std::size_t>(same - m_clips.begin()));
        return id;
    }

    QSqlQuery query(db);
    query.prepare(u"INSERT INTO clips (kind, text, length, html, rtf, hash, created, used, source, sourcePath, "
                  "width, height, bytes) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"_s);
    query.addBindValue(static_cast<int>(capture.kind));
    query.addBindValue(text(capture.text));
    query.addBindValue(capture.text.size());
    query.addBindValue(blob(capture.html));
    query.addBindValue(blob(capture.rtf));
    query.addBindValue(hash);
    query.addBindValue(now);
    query.addBindValue(now);
    query.addBindValue(text(capture.source));
    query.addBindValue(text(capture.sourcePath));
    query.addBindValue(capture.width);
    query.addBindValue(capture.height);
    query.addBindValue(static_cast<qint64>(capture.png.size()));
    if (!run(query, "Saving an entry"))
        return 0;
    const qint64 id = query.lastInsertId().toLongLong();
    if (capture.kind == ClipKind::Image) {
        QSaveFile file(imagePath(id));
        if (!file.open(QIODevice::WriteOnly) || file.write(capture.png) != capture.png.size() || !file.commit()) {
            qWarning().noquote() << "Clipboard history: cannot save a picture:" << file.errorString();
            removeRows({id}, false);
            return 0;
        }
    }

    Clip clip;
    clip.id = id;
    clip.kind = capture.kind;
    clip.text = capture.text.left(kTextInMemory);
    clip.textLength = capture.text.size();
    clip.hash = hash;
    clip.created = now;
    clip.used = now;
    clip.source = capture.source;
    clip.sourcePath = capture.sourcePath;
    clip.width = capture.width;
    clip.height = capture.height;
    clip.bytes = capture.png.size();
    clip.html = !capture.html.isEmpty();
    clip.rtf = !capture.rtf.isEmpty();
    m_clips.insert(m_clips.begin(), std::move(clip));
    prune(now);
    return id;
}

void ClipStore::touch(qint64 id, qint64 now)
{
    const auto it = std::find_if(m_clips.begin(), m_clips.end(), [id](const Clip& c) { return c.id == id; });
    if (it == m_clips.end())
        return;
    QSqlQuery query(QSqlDatabase::database(m_connection, false));
    query.prepare(u"UPDATE clips SET used = ? WHERE id = ?"_s);
    query.addBindValue(now);
    query.addBindValue(id);
    if (!run(query, "Updating an entry"))
        return;
    it->used = now;
    moveToFront(static_cast<std::size_t>(it - m_clips.begin()));
}

std::vector<ClipRecord> ClipStore::remove(std::span<const qint64> ids)
{
    std::vector<ClipRecord> records;
    if (!m_open)
        return records;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery read(db);
    read.prepare(u"SELECT text, html, rtf FROM clips WHERE id = ?"_s);
    std::vector<qint64> gone;
    for (const qint64 id : ids) {
        const Clip* c = clip(id);
        if (!c)
            continue;
        ClipRecord record {*c, {}, {}, {}};
        read.addBindValue(id);
        if (run(read, "Reading an entry") && read.next()) {
            record.text = read.value(0).toString();
            record.html = read.value(1).toByteArray();
            record.rtf = read.value(2).toByteArray();
        }
        read.finish();
        records.push_back(std::move(record));
        gone.push_back(id);
    }
    removeRows(gone, false);
    return records;
}

void ClipStore::restore(std::vector<ClipRecord> records)
{
    if (!m_open)
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    db.transaction();
    QSqlQuery query(db);
    query.prepare(u"INSERT OR REPLACE INTO clips (id, kind, text, length, html, rtf, hash, created, used, source, "
                  "sourcePath, groupId, width, height, bytes) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"_s);
    for (ClipRecord& record : records) {
        Clip& c = record.clip;
        if (clip(c.id))
            continue;
        if (c.group != 0 && !group(c.group))
            c.group = 0;
        query.addBindValue(c.id);
        query.addBindValue(static_cast<int>(c.kind));
        query.addBindValue(text(record.text));
        query.addBindValue(c.textLength);
        query.addBindValue(blob(record.html));
        query.addBindValue(blob(record.rtf));
        query.addBindValue(c.hash);
        query.addBindValue(c.created);
        query.addBindValue(c.used);
        query.addBindValue(text(c.source));
        query.addBindValue(text(c.sourcePath));
        query.addBindValue(c.group);
        query.addBindValue(c.width);
        query.addBindValue(c.height);
        query.addBindValue(c.bytes);
        if (!run(query, "Restoring an entry"))
            continue;
        // Back where it was: newest first by when it was used.
        const auto at = std::find_if(m_clips.begin(), m_clips.end(),
            [&](const Clip& other) { return other.used < c.used || (other.used == c.used && other.id < c.id); });
        m_clips.insert(at, std::move(c));
    }
    db.commit();
}

void ClipStore::discardImages(std::span<const qint64> ids)
{
    for (const qint64 id : ids) {
        if (!clip(id))
            QFile::remove(imagePath(id));
    }
}

void ClipStore::removeRows(const std::vector<qint64>& ids, bool deleteImages)
{
    if (ids.empty())
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    db.transaction();
    QSqlQuery query(db);
    query.prepare(u"DELETE FROM clips WHERE id = ?"_s);
    for (const qint64 id : ids) {
        query.addBindValue(id);
        run(query, "Removing an entry");
    }
    db.commit();
    const QSet<qint64> gone(ids.begin(), ids.end());
    std::erase_if(m_clips, [&](const Clip& c) { return gone.contains(c.id); });
    if (deleteImages) {
        for (const qint64 id : ids)
            QFile::remove(imagePath(id));
    }
}

void ClipStore::prune(qint64 now)
{
    const qint64 cutoff = m_limits.maxDays > 0 ? now - m_limits.maxDays * kDayMs : std::numeric_limits<qint64>::min();
    std::vector<qint64> drop;
    int kept = 0;
    for (const Clip& c : m_clips) {
        if (c.group != 0)
            continue;
        if (c.used < cutoff || ++kept > m_limits.maxItems)
            drop.push_back(c.id);
    }
    removeRows(drop, true);
}

void ClipStore::setGroup(std::span<const qint64> ids, qint64 groupId)
{
    if (!m_open || (groupId != 0 && !group(groupId)))
        return;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    db.transaction();
    QSqlQuery query(db);
    query.prepare(u"UPDATE clips SET groupId = ? WHERE id = ?"_s);
    for (const qint64 id : ids) {
        auto it = std::find_if(m_clips.begin(), m_clips.end(), [id](const Clip& c) { return c.id == id; });
        if (it == m_clips.end())
            continue;
        query.addBindValue(groupId);
        query.addBindValue(id);
        if (run(query, "Moving an entry"))
            it->group = groupId;
    }
    db.commit();
}

qint64 ClipStore::addGroup(const QString& name)
{
    if (!m_open)
        return 0;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery query(db);
    query.prepare(u"INSERT INTO groups (name, position) VALUES (?, (SELECT COALESCE(MAX(position), 0) + 1 FROM groups))"_s);
    query.addBindValue(text(name));
    if (!run(query, "Adding a group"))
        return 0;
    const qint64 id = query.lastInsertId().toLongLong();
    m_groups.push_back({id, name});
    return id;
}

void ClipStore::renameGroup(qint64 id, const QString& name)
{
    const auto it = std::find_if(m_groups.begin(), m_groups.end(), [id](const ClipGroup& g) { return g.id == id; });
    if (it == m_groups.end() || id == kPinned)
        return;
    QSqlQuery query(QSqlDatabase::database(m_connection, false));
    query.prepare(u"UPDATE groups SET name = ? WHERE id = ?"_s);
    query.addBindValue(text(name));
    query.addBindValue(id);
    if (run(query, "Renaming a group"))
        it->name = name;
}

ClipGroupRecord ClipStore::removeGroup(qint64 id)
{
    ClipGroupRecord record;
    const auto it = std::find_if(m_groups.begin(), m_groups.end(), [id](const ClipGroup& g) { return g.id == id; });
    if (id == kPinned || it == m_groups.end())
        return record;
    QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    QSqlQuery position(db);
    position.prepare(u"SELECT position FROM groups WHERE id = ?"_s);
    position.addBindValue(id);
    if (run(position, "Reading a group") && position.next())
        record.position = position.value(0).toInt();
    QSqlQuery query(db);
    query.prepare(u"DELETE FROM groups WHERE id = ?"_s);
    query.addBindValue(id);
    if (!run(query, "Removing a group"))
        return record;
    std::vector<qint64> members;
    for (const Clip& c : m_clips) {
        if (c.group == id)
            members.push_back(c.id);
    }
    record.group = *it;
    record.index = static_cast<int>(it - m_groups.begin());
    m_groups.erase(it);
    record.clips = remove(members);
    return record;
}

void ClipStore::restoreGroup(ClipGroupRecord record)
{
    if (!m_open || record.group.id == 0 || group(record.group.id))
        return;
    QSqlQuery query(QSqlDatabase::database(m_connection, false));
    query.prepare(u"INSERT INTO groups (id, name, position) VALUES (?, ?, ?)"_s);
    query.addBindValue(record.group.id);
    query.addBindValue(text(record.group.name));
    query.addBindValue(record.position);
    if (!run(query, "Restoring a group"))
        return;
    const auto at = m_groups.begin() + std::clamp<std::ptrdiff_t>(record.index, 0, static_cast<std::ptrdiff_t>(m_groups.size()));
    m_groups.insert(at, std::move(record.group));
    restore(std::move(record.clips));
}

int ClipStore::clearHistory()
{
    std::vector<qint64> drop;
    for (const Clip& c : m_clips) {
        if (c.group == 0)
            drop.push_back(c.id);
    }
    removeRows(drop, true);
    return static_cast<int>(drop.size());
}

ClipPayload ClipStore::payload(qint64 id) const
{
    ClipPayload payload;
    const Clip* c = clip(id);
    if (!c)
        return payload;
    if (c->kind == ClipKind::Image)
        payload.imagePath = imagePath(id);
    if (!c->truncated() && !c->html && !c->rtf) {
        payload.text = c->text; // all of it is in memory
        return payload;
    }
    QSqlQuery query(QSqlDatabase::database(m_connection, false));
    query.prepare(u"SELECT text, html, rtf FROM clips WHERE id = ?"_s);
    query.addBindValue(id);
    if (run(query, "Reading an entry") && query.next()) {
        payload.text = query.value(0).toString();
        payload.html = query.value(1).toByteArray();
        payload.rtf = query.value(2).toByteArray();
    }
    return payload;
}

QString ClipStore::value(const QString& key) const
{
    if (!m_open)
        return {};
    QSqlQuery query(QSqlDatabase::database(m_connection, false));
    query.prepare(u"SELECT value FROM state WHERE key = ?"_s);
    query.addBindValue(key);
    return run(query, "Reading a value") && query.next() ? query.value(0).toString() : QString();
}

void ClipStore::setValue(const QString& key, const QString& value)
{
    if (!m_open)
        return;
    QSqlQuery query(QSqlDatabase::database(m_connection, false));
    query.prepare(u"INSERT OR REPLACE INTO state (key, value) VALUES (?, ?)"_s);
    query.addBindValue(key);
    query.addBindValue(text(value));
    run(query, "Saving a value");
}

std::vector<int> ClipStore::find(const ClipFilter& filter) const
{
    using Category = ClipFilter::Category;
    const ClipMatcher matcher(filter.query);
    std::vector<int> rows;
    rows.reserve(m_clips.size());
    for (std::size_t i = 0; i < m_clips.size(); ++i) {
        const Clip& c = m_clips[i];
        bool shown = true;
        switch (filter.category) {
        case Category::All:
            break;
        case Category::Text:
            shown = c.kind == ClipKind::Text;
            break;
        case Category::Links:
            shown = c.kind == ClipKind::Link;
            break;
        case Category::Images:
            shown = c.kind == ClipKind::Image;
            break;
        case Category::Files:
            shown = c.kind == ClipKind::Files || c.kind == ClipKind::Path;
            break;
        case Category::Group:
            shown = c.group == filter.group;
            break;
        }
        if (shown && (matcher.empty() || matcher.matches(c)))
            rows.push_back(static_cast<int>(i));
    }
    return rows;
}

} // namespace ws
