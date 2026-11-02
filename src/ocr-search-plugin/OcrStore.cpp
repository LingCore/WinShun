#include "OcrStore.h"

#include "ContentIndex.h"
#include "Query.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>
#include <limits>
#include <stdexcept>

using namespace Qt::StringLiterals;

namespace ws::ocrsearch {
namespace {
void checked(QSqlQuery& query)
{
    if (!query.exec())
        throw std::runtime_error(query.lastError().text().toStdString());
}
QByteArray packed(const QList<OcrPiece>& pieces)
{
    return qCompress(QJsonDocument(piecesToJson(pieces)).toJson(QJsonDocument::Compact), 6);
}

// Keep the existing CJK/ASCII candidate semantics. Hex tokens let FTS5
// compress the postings without storing word positions or a second text copy.
QString gramToken(grams::Key key)
{
    return u"g"_s + QString::number(key, 16);
}

QString indexTokens(const QString& text)
{
    auto keys = grams::ofPhrase(text);
    for (char32_t codepoint : text.toUcs4())
        if (grams::isIndexed(codepoint))
            keys.push_back(grams::Key(codepoint) << grams::kCharBits);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    QStringList tokens;
    for (auto key : keys)
        tokens.append(gramToken(key));
    return tokens.join(u' ');
}
} // namespace

QJsonArray piecesToJson(const QList<OcrPiece>& pieces)
{
    QJsonArray array;
    for (const OcrPiece& piece : pieces) {
        QJsonArray points;
        for (const QPointF& point : piece.quad)
            points.append(QJsonArray {point.x(), point.y()});
        array.append(QJsonObject {{u"text"_s, piece.text}, {u"score"_s, piece.score}, {u"quad"_s, points}});
    }
    return array;
}

QString normalized(QString text)
{
    text = text.normalized(QString::NormalizationForm_KC).toCaseFolded().simplified();
    // OCR may put spaces between Chinese characters that are adjacent on paper.
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] == u' ' && i > 0 && i + 1 < text.size() && grams::isIndexed(text[i - 1].unicode())
            && grams::isIndexed(text[i + 1].unicode()))
            continue;
        out += text[i];
    }
    return out;
}

QString locationText(const QJsonObject& source, const QString& fallback)
{
    const QString kind = source.value(u"kind"_s).toString();
    if (kind == u"pdf")
        return QObject::tr("第 %1 页").arg(source.value(u"page"_s).toInt());
    if (kind == u"image")
        return source.value(u"frames"_s).toInt(1) > 1
            ? QObject::tr("第 %1 页").arg(source.value(u"frame"_s).toInt() + 1)
            : QObject::tr("图片");
    const int image = source.value(u"imageNumber"_s).toInt();
    if (!image)
        return fallback;
    if (source.contains(u"slide"_s))
        return QObject::tr("第 %1 张幻灯片 · 图片 %2").arg(source.value(u"slide"_s).toInt()).arg(image);
    if (source.contains(u"sheet"_s))
        return QObject::tr("工作表「%1」· 图片 %2").arg(source.value(u"sheet"_s).toString()).arg(image);
    if (source.contains(u"paragraph"_s))
        return QObject::tr("正文第 %1 段 · 图片 %2").arg(source.value(u"paragraph"_s).toInt()).arg(image);
    return QObject::tr("%1 · 图片 %2").arg(source.value(u"section"_s).toString()).arg(image);
}

QString pathKey(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath())).toCaseFolded();
}

bool inside(const QString& path, const QString& folder)
{
    const QString p = pathKey(path);
    const QString f = pathKey(folder);
    return p == f || p.startsWith(f.endsWith(u'/') ? f : f + u'/');
}

Store::Store(const QString& folder, int capacityMB)
    : m_connection(u"ocr-%1"_s.arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
    , m_path(QDir(folder).filePath(u"ocr-index.sqlite"_s))
{
    if (!QDir().mkpath(folder))
        throw std::runtime_error("Cannot create the OCR index directory");
    m_db = QSqlDatabase::addDatabase(u"QSQLITE"_s, m_connection);
    m_db.setDatabaseName(m_path);
    m_db.setConnectOptions(
        capacityMB == 0 ? u"QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=1000"_s : u"QSQLITE_BUSY_TIMEOUT=3000"_s);
    if (!m_db.open()) {
        const QString error = m_db.lastError().text();
        m_db = {};
        QSqlDatabase::removeDatabase(m_connection);
        throw std::runtime_error(error.toStdString());
    }
    try {
        QSqlQuery version(m_db);
        if (!version.exec(u"PRAGMA user_version"_s) || !version.next())
            throw std::runtime_error("Cannot read the OCR index version");
        const int schemaVersion = version.value(0).toInt();
        version.finish();
        if (schemaVersion > 2)
            throw std::runtime_error("This OCR index needs a newer plugin");
        if (capacityMB == 0) {
            if (schemaVersion != 2)
                throw std::runtime_error("The OCR index must be upgraded before searching");
            return;
        }
        exec(u"PRAGMA foreign_keys=ON"_s);
        exec(u"PRAGMA auto_vacuum=INCREMENTAL"_s);
        exec(u"PRAGMA journal_mode=WAL"_s);
        exec(u"PRAGMA synchronous=NORMAL"_s);
        exec(u"PRAGMA wal_autocheckpoint=256"_s);
        exec(u"PRAGMA journal_size_limit=1048576"_s);
        QSqlQuery pageSize(m_db);
        if (!pageSize.exec(u"PRAGMA page_size"_s) || !pageSize.next() || pageSize.value(0).toInt() <= 0)
            throw std::runtime_error("Cannot read the OCR index page size");
        const int bytesPerPage = pageSize.value(0).toInt();
        pageSize.finish();
        exec(u"PRAGMA max_page_count=%1"_s.arg(qint64(std::clamp(capacityMB, 16, 4096)) * 1024 * 1024 / bytesPerPage));
        if (schemaVersion < 2) {
            if (!m_db.transaction())
                throw std::runtime_error(m_db.lastError().text().toStdString());
            m_transaction = true;
        }
        exec(
            u"CREATE TABLE IF NOT EXISTS files(path TEXT PRIMARY KEY,display TEXT NOT NULL,size INTEGER,modified INTEGER,hash BLOB,engine TEXT,generation INTEGER,status TEXT,error TEXT)"_s);
        exec(u"CREATE INDEX IF NOT EXISTS file_hash ON files(hash,engine,status)"_s);
        exec(
            u"CREATE TABLE IF NOT EXISTS assets(id INTEGER PRIMARY KEY,hash BLOB UNIQUE NOT NULL,width INTEGER,height INTEGER,text BLOB NOT NULL,pieces BLOB NOT NULL)"_s);
        exec(
            u"CREATE TABLE IF NOT EXISTS pages(file TEXT REFERENCES files(path) ON DELETE CASCADE,number INTEGER,asset INTEGER REFERENCES assets(id),label TEXT,source TEXT,PRIMARY KEY(file,number)) WITHOUT ROWID"_s);
        exec(u"CREATE INDEX IF NOT EXISTS page_asset ON pages(asset)"_s);
        exec(u"CREATE VIRTUAL TABLE IF NOT EXISTS asset_terms USING fts5(tokens,content='',detail=none,columnsize=0)"_s);
        if (schemaVersion < 2) {
            // Reuse stored recognition: migration never decodes source images
            // or calls OCR. All schema/data changes roll back together on failure.
            QSqlQuery assets(m_db), insert(m_db);
            if (!assets.exec(u"SELECT id,text FROM assets ORDER BY id"_s))
                throw std::runtime_error(assets.lastError().text().toStdString());
            insert.prepare(u"INSERT INTO asset_terms(rowid,tokens) VALUES(?,?)"_s);
            while (assets.next()) {
                insert.bindValue(0, assets.value(0));
                insert.bindValue(1, indexTokens(QString::fromUtf8(qUncompress(assets.value(1).toByteArray()))));
                checked(insert);
            }
            assets.finish();
            exec(u"DROP TABLE IF EXISTS grams"_s);
            exec(u"PRAGMA user_version=2"_s);
            if (!m_db.commit())
                throw std::runtime_error(m_db.lastError().text().toStdString());
            m_transaction = false;
            reclaim();
        }
    } catch (...) {
        rollback();
        m_db.close();
        m_db = {};
        QSqlDatabase::removeDatabase(m_connection);
        throw;
    }
}

Store::~Store()
{
    rollback();
    m_db.close();
    m_db = {};
    QSqlDatabase::removeDatabase(m_connection);
}

void Store::exec(const QString& sql) const
{
    QSqlQuery query(m_db);
    if (!query.exec(sql))
        throw std::runtime_error(query.lastError().text().toStdString());
    while (query.next()) { }
    if (query.lastError().isValid())
        throw std::runtime_error(query.lastError().text().toStdString());
}

void Store::reclaim()
{
    // QSQLITE treats incremental_vacuum as a non-select (zero columns), so
    // next() cannot step its SQLITE_ROW results. Each exec frees just one
    // page on that driver. Repeat while the freelist makes progress, keeping
    // reclamation incremental instead of rewriting the whole index with VACUUM.
    qint64 previous = std::numeric_limits<qint64>::max();
    QSqlQuery free(m_db);
    for (;;) {
        if (!free.exec(u"PRAGMA freelist_count"_s) || !free.next())
            throw std::runtime_error(free.lastError().text().toStdString());
        const qint64 remaining = free.value(0).toLongLong();
        free.finish();
        if (remaining == 0 || remaining >= previous)
            break;
        previous = remaining;
        exec(u"PRAGMA incremental_vacuum"_s);
    }
    exec(u"PRAGMA wal_checkpoint(TRUNCATE)"_s);
}

bool Store::current(const QString& path, qint64 size, qint64 modified, const QString& engine) const
{
    QSqlQuery q(m_db);
    q.prepare(
        u"SELECT 1 FROM files WHERE path=? AND size=? AND modified=? AND engine=? AND status IN ('ready','empty','partial','unsupported')"_s);
    q.addBindValue(pathKey(path));
    q.addBindValue(size);
    q.addBindValue(modified);
    q.addBindValue(engine);
    checked(q);
    return q.next();
}

void Store::touch(const QString& path, qint64 generation)
{
    QSqlQuery q(m_db);
    q.prepare(u"UPDATE files SET generation=? WHERE path=?"_s);
    q.addBindValue(generation);
    q.addBindValue(pathKey(path));
    checked(q);
}

void Store::beginFile(
    const QString& path, qint64 size, qint64 modified, const QByteArray& hash, const QString& engine, qint64 generation)
{
    rollback();
    if (!m_db.transaction())
        throw std::runtime_error(m_db.lastError().text().toStdString());
    m_transaction = true;
    m_file = pathKey(path);
    m_page = 0;
    QSqlQuery q(m_db);
    q.prepare(
        u"INSERT INTO files VALUES(?,?,?,?,?,?,?,'ready','') ON CONFLICT(path) DO UPDATE SET display=excluded.display,size=excluded.size,modified=excluded.modified,hash=excluded.hash,engine=excluded.engine,generation=excluded.generation,status='ready',error=''"_s);
    q.addBindValue(m_file);
    q.addBindValue(QDir::toNativeSeparators(path));
    q.addBindValue(size);
    q.addBindValue(modified);
    q.addBindValue(hash);
    q.addBindValue(engine);
    q.addBindValue(generation);
    checked(q);
    q.prepare(u"DELETE FROM pages WHERE file=?"_s);
    q.addBindValue(m_file);
    checked(q);
}

bool Store::reuse(
    const QString& path, qint64 size, qint64 modified, const QByteArray& hash, const QString& engine, qint64 generation)
{
    QSqlQuery q(m_db);
    q.prepare(
        u"SELECT path FROM files WHERE hash=? AND engine=? AND status IN ('ready','empty') AND path<>? LIMIT 1"_s);
    q.addBindValue(hash);
    q.addBindValue(engine);
    q.addBindValue(pathKey(path));
    checked(q);
    if (!q.next())
        return false;
    const QString original = q.value(0).toString();
    q.finish();
    beginFile(path, size, modified, hash, engine, generation);
    q.prepare(u"INSERT INTO pages SELECT ?,number,asset,label,source FROM pages WHERE file=?"_s);
    q.addBindValue(m_file);
    q.addBindValue(original);
    checked(q);
    m_page = q.numRowsAffected();
    finishFile(m_page ? u"ready"_s : u"empty"_s);
    return true;
}

qint64 Store::asset(const QByteArray& hash) const
{
    QSqlQuery q(m_db);
    q.prepare(u"SELECT id FROM assets WHERE hash=?"_s);
    q.addBindValue(hash);
    checked(q);
    return q.next() ? q.value(0).toLongLong() : 0;
}

qint64 Store::putAsset(const QByteArray& hash, const QSize& size, const QList<OcrPiece>& pieces)
{
    if (const qint64 found = asset(hash))
        return found;
    QStringList lines;
    for (const OcrPiece& piece : pieces)
        lines.append(piece.text);
    const QString text = normalized(lines.join(u'\n'));
    QSqlQuery q(m_db);
    q.prepare(u"INSERT INTO assets(hash,width,height,text,pieces) VALUES(?,?,?,?,?)"_s);
    q.addBindValue(hash);
    q.addBindValue(size.width());
    q.addBindValue(size.height());
    q.addBindValue(qCompress(text.toUtf8(), 6));
    q.addBindValue(packed(pieces));
    checked(q);
    const qint64 id = q.lastInsertId().toLongLong();
    q.prepare(u"INSERT INTO asset_terms(rowid,tokens) VALUES(?,?)"_s);
    q.addBindValue(id);
    q.addBindValue(indexTokens(text));
    checked(q);
    return id;
}

void Store::addPage(qint64 id, const QString& label, const QJsonObject& source)
{
    QSqlQuery q(m_db);
    q.prepare(u"INSERT INTO pages VALUES(?,?,?,?,?)"_s);
    q.addBindValue(m_file);
    q.addBindValue(++m_page);
    q.addBindValue(id);
    q.addBindValue(label);
    q.addBindValue(QString::fromUtf8(QJsonDocument(source).toJson(QJsonDocument::Compact)));
    checked(q);
}

void Store::finishFile(const QString& status, const QString& error)
{
    QSqlQuery q(m_db);
    q.prepare(u"UPDATE files SET status=?,error=? WHERE path=?"_s);
    q.addBindValue(status);
    q.addBindValue(error.isNull() ? u""_s : error);
    q.addBindValue(m_file);
    checked(q);
    if (!m_db.commit()) {
        rollback();
        throw std::runtime_error(m_db.lastError().text().toStdString());
    }
    m_transaction = false;
}

void Store::rollback()
{
    if (m_transaction)
        m_db.rollback();
    m_transaction = false;
}

void Store::status(const QString& path, qint64 size, qint64 modified, const QString& engine, qint64 generation,
    const QString& state, const QString& error)
{
    beginFile(path, size, modified, {}, engine, generation);
    finishFile(state, error);
}

void Store::prune(const QString& root, qint64 generation)
{
    QSqlQuery q(m_db);
    q.exec(u"SELECT path,generation FROM files"_s);
    QStringList stale;
    while (q.next()) {
        if (inside(q.value(0).toString(), root) && q.value(1).toLongLong() != generation)
            stale.append(q.value(0).toString());
    }
    q.prepare(u"DELETE FROM files WHERE path=?"_s);
    for (const QString& path : stale) {
        q.bindValue(0, path);
        checked(q);
    }
}

void Store::retainFolders(const QStringList& roots)
{
    QSqlQuery q(m_db);
    q.exec(u"SELECT path FROM files"_s);
    QStringList stale;
    while (q.next()) {
        const QString path = q.value(0).toString();
        if (std::none_of(roots.begin(), roots.end(), [&](const QString& root) { return inside(path, root); }))
            stale.append(path);
    }
    q.prepare(u"DELETE FROM files WHERE path=?"_s);
    for (const QString& path : stale) {
        q.bindValue(0, path);
        checked(q);
    }
}

void Store::compact()
{
    if (!m_db.transaction())
        throw std::runtime_error(m_db.lastError().text().toStdString());
    m_transaction = true;
    QSqlQuery stale(m_db), remove(m_db);
    if (!stale.exec(u"SELECT id,text FROM assets WHERE NOT EXISTS(SELECT 1 FROM pages WHERE pages.asset=assets.id)"_s))
        throw std::runtime_error(stale.lastError().text().toStdString());
    remove.prepare(u"INSERT INTO asset_terms(asset_terms,rowid,tokens) VALUES('delete',?,?)"_s);
    while (stale.next()) {
        remove.bindValue(0, stale.value(0));
        remove.bindValue(1, indexTokens(QString::fromUtf8(qUncompress(stale.value(1).toByteArray()))));
        checked(remove);
    }
    stale.finish();
    exec(u"DELETE FROM assets WHERE NOT EXISTS(SELECT 1 FROM pages WHERE pages.asset=assets.id)"_s);
    exec(u"INSERT INTO asset_terms(asset_terms) VALUES('optimize')"_s);
    if (!m_db.commit())
        throw std::runtime_error(m_db.lastError().text().toStdString());
    m_transaction = false;
    reclaim();
}

QJsonObject Store::statistics() const
{
    QJsonObject out;
    QSqlQuery q(m_db);
    q.exec(u"SELECT status,count(*) FROM files GROUP BY status"_s);
    int files = 0;
    while (q.next()) {
        out.insert(q.value(0).toString(), q.value(1).toInt());
        files += q.value(1).toInt();
    }
    out.insert(u"files"_s, files);
    q.exec(u"SELECT count(*) FROM pages"_s);
    if (q.next())
        out.insert(u"pages"_s, q.value(0).toInt());
    q.exec(u"SELECT count(*) FROM assets"_s);
    if (q.next())
        out.insert(u"assets"_s, q.value(0).toInt());
    out.insert(u"bytes"_s, QFileInfo(m_path).size() + QFileInfo(m_path + u"-wal"_s).size()
        + QFileInfo(m_path + u"-shm"_s).size());
    return out;
}

QString Store::errorText() const
{
    QSqlQuery q(m_db);
    q.exec(u"SELECT display,error FROM files WHERE status NOT IN ('ready','empty') ORDER BY display LIMIT 200"_s);
    QStringList lines;
    while (q.next())
        lines.append(q.value(0).toString() + u"\n"_s + q.value(1).toString());
    return lines.join(u"\n\n"_s);
}

QList<OcrPiece> piecesFromJson(const QJsonArray& array)
{
    QList<OcrPiece> out;
    for (const auto& value : array) {
        const auto o = value.toObject();
        OcrPiece piece;
        piece.text = o.value(u"text"_s).toString();
        piece.score = float(o.value(u"score"_s).toDouble());
        for (const auto& point : o.value(u"quad"_s).toArray()) {
            const auto xy = point.toArray();
            if (xy.size() == 2)
                piece.quad.append(QPointF(xy[0].toDouble(), xy[1].toDouble()));
        }
        out.append(std::move(piece));
    }
    return out;
}

QList<OcrPiece> Store::pieces(qint64 id) const
{
    QSqlQuery q(m_db);
    q.prepare(u"SELECT pieces FROM assets WHERE id=?"_s);
    q.addBindValue(id);
    checked(q);
    return q.next() ? piecesFromJson(QJsonDocument::fromJson(qUncompress(q.value(0).toByteArray())).array())
                    : QList<OcrPiece> {};
}

QList<Hit> Store::search(const QString& words, int limit, const QJsonObject& filters, const Cancel& cancel) const
{
    const ContentQuery scope = splitContentQuery(words.left(512));
    QStringList terms = contentTerms(scope.words);
    for (QString& term : terms)
        term = normalized(term);
    terms.removeAll(QString());
    if (terms.isEmpty() || limit <= 0)
        return {};
    QStringList candidates;
    for (const QString& term : terms) {
        QStringList tokens;
        for (auto key : grams::ofPhrase(term))
            tokens.append(gramToken(key));
        if (!tokens.isEmpty())
            candidates.append(tokens.join(u" AND "_s));
    }
    const QDateTime from = QDateTime::fromString(filters.value(u"modifiedFrom"_s).toString(), Qt::ISODate);
    const QDateTime to = QDateTime::fromString(filters.value(u"modifiedTo"_s).toString(), Qt::ISODate);
    QList<QRegularExpression> excludedNames;
    for (const auto& value : filters.value(u"excludedFileNames"_s).toArray())
        excludedNames.append(QRegularExpression(QRegularExpression::wildcardToRegularExpression(value.toString()),
            QRegularExpression::CaseInsensitiveOption));
    const auto accepted = [&](const QFileInfo& info) {
        const QString path = info.absoluteFilePath();
        const QString folder = filters.value(u"folder"_s).toString();
        if (scope.folders.empty() && !folder.isEmpty() && !inside(path, folder))
            return false;
        if (!scope.folders.empty()) {
            bool matches = false;
            const QString picked = filters.value(u"contentPick"_s).toString();
            for (const ContentFolderSpec& spec : scope.folders) {
                if (!picked.isEmpty()) {
                    matches |= inside(path, picked);
                    continue;
                }
                if (!spec.path.isEmpty()) {
                    matches |= inside(path, spec.path);
                    continue;
                }
                const QStringList ancestors
                    = QDir::fromNativeSeparators(info.absolutePath()).split(u'/', Qt::SkipEmptyParts);
                int wanted = 0;
                for (const QString& ancestor : ancestors)
                    if (wanted < spec.names.size() && ancestor.contains(spec.names[wanted], Qt::CaseInsensitive))
                        ++wanted;
                matches |= wanted == spec.names.size();
            }
            if (!matches)
                return false;
        }
        const auto extensions = filters.value(u"extensions"_s).toArray();
        if (!extensions.isEmpty() && std::none_of(extensions.begin(), extensions.end(), [&](const auto& ext) {
                return ext.toString().compare(info.suffix(), Qt::CaseInsensitive) == 0;
            }))
            return false;
        for (const auto& ext : filters.value(u"excludedExtensions"_s).toArray())
            if (ext.toString().compare(info.suffix(), Qt::CaseInsensitive) == 0)
                return false;
        for (const auto& pattern : excludedNames)
            if (pattern.match(info.fileName()).hasMatch())
                return false;
        const auto minimum = filters.value(u"minSize"_s), maximum = filters.value(u"maxSize"_s);
        if ((minimum.isDouble() && info.size() < minimum.toInteger())
            || (maximum.isDouble() && info.size() > maximum.toInteger()))
            return false;
        return (!from.isValid() || info.lastModified() >= from) && (!to.isValid() || info.lastModified() < to);
    };
    QSqlQuery files(m_db);
    QString sql = u"SELECT display,size,modified,path FROM files WHERE status IN ('ready','partial')"_s;
    // Intersect at file level: separate search words may match different pages.
    for (qsizetype i = 0; i < candidates.size(); ++i)
        sql += u" AND path IN (SELECT p.file FROM asset_terms JOIN pages p ON p.asset=asset_terms.rowid WHERE asset_terms MATCH ?)"_s;
    sql += u" ORDER BY path"_s;
    files.prepare(sql);
    for (const QString& candidate : candidates)
        files.addBindValue(candidate);
    checked(files);
    QList<Hit> out;
    QHash<qint64, QString> cache;
    while (files.next()) {
        if (cancel && cancel())
            break;
        const QString path = files.value(0).toString();
        const QFileInfo info(path);
        if (!info.exists() || info.size() != files.value(1).toLongLong()
            || info.lastModified().toMSecsSinceEpoch() != files.value(2).toLongLong() || !accepted(info))
            continue;
        QSqlQuery pages(m_db);
        pages.prepare(
            u"SELECT a.id,a.text,p.label,p.source FROM pages p JOIN assets a ON a.id=p.asset WHERE p.file=? ORDER BY p.number"_s);
        pages.addBindValue(files.value(3));
        checked(pages);
        QList<bool> found(terms.size(), false);
        int remaining = int(terms.size()), bestCount = 0;
        Hit best;
        QString bestText;
        while (pages.next()) {
            if (cancel && cancel())
                return out;
            const qint64 asset = pages.value(0).toLongLong();
            QString text;
            const auto cached = cache.constFind(asset);
            if (cached != cache.cend())
                text = *cached;
            else {
                text = QString::fromUtf8(qUncompress(pages.value(1).toByteArray()));
                if (cache.size() >= 64)
                    cache.clear();
                cache.insert(asset, text);
            }
            int count = 0;
            for (qsizetype term = 0; term < terms.size(); ++term) {
                if (!text.contains(terms[term]))
                    continue;
                ++count;
                if (!found[term]) {
                    found[term] = true;
                    --remaining;
                }
            }
            if (count > bestCount) {
                bestCount = count;
                best = {path, {}, {}, asset, info.size(), info.lastModified().toSecsSinceEpoch(),
                    QJsonDocument::fromJson(pages.value(3).toString().toUtf8()).object()};
                best.location = locationText(best.source, pages.value(2).toString());
                best.source.insert(u"fileModifiedMs"_s, info.lastModified().toMSecsSinceEpoch());
                bestText = text;
            }
            if (!remaining)
                break;
        }
        if (remaining || !best.asset)
            continue;
        for (const auto& piece : pieces(best.asset)) {
            const QString text = normalized(piece.text);
            if (std::any_of(terms.begin(), terms.end(), [&](const QString& term) { return text.contains(term); })) {
                best.snippet = piece.text.left(140);
                break;
            }
        }
        if (best.snippet.isEmpty())
            best.snippet = bestText.left(140);
        out.append(std::move(best));
        if (out.size() >= std::min(limit, 200))
            break;
    }
    return out;
}

} // namespace ws::ocrsearch
