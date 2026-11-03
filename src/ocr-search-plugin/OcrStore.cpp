#include "OcrStore.h"

#include "ContentIndex.h"
#include "Query.h"
#include "platform/OcrFiles.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::ocrsearch {
namespace {
// 3: the pipeline below reads files with what 2 left out (recheckOlderPipeline()).
constexpr int kSchema = 3;
constexpr auto kPipeline = u"/ocr-search-3";
constexpr auto kOlderPipeline = u"/ocr-search-2";
// Unchanged while one picture is read the same: the recognition stored for
// it before is found again by the same imageHash().
constexpr auto kRecognizer = u"/ocr-search-2";
// A picture whose lines read at less than this on average was maybe upside
// down (measured: 11 to 18 lines kept at 0.57; upright text reads above 0.9).
constexpr double kPoorScore = 0.75;
// The files (as f) where nothing was found to read: no pictures in them, or
// none with words. An empty text is stored as qCompress's 4-byte header.
// Only files this pipeline settled (bind pipelineEngine()): one an upgrade
// marked waits to be read again, and may well have words then.
constexpr auto kTextless = "(f.engine=? AND (f.status='empty' OR (f.status='ready' AND NOT EXISTS(SELECT 1 FROM "
                           "pages JOIN assets ON assets.id=pages.asset WHERE pages.file=f.path "
                           "AND length(assets.text)>4))))";

void checked(QSqlQuery& query)
{
    if (!query.exec())
        throw std::runtime_error(query.lastError().text().toStdString());
}
QByteArray packed(const QList<OcrPiece>& pieces)
{
    return qCompress(QJsonDocument(piecesToJson(pieces)).toJson(QJsonDocument::Compact), 6);
}

// Traditional Chinese characters and the simplified form OpenCC lists first
// for each (third_party/opencc/TSCharacters.txt). What is stored and what is
// searched for are both folded, so either way of writing finds the other.
const QHash<char32_t, char32_t>& simplifiedForms()
{
    static const QHash<char32_t, char32_t> forms = [] {
        QFile file(u":/ocr-search/TSCharacters.txt"_s);
        if (!file.open(QIODevice::ReadOnly))
            throw std::logic_error("The traditional Chinese table is not built in");
        QHash<char32_t, char32_t> table;
        for (const QByteArray& line : file.readAll().split('\n')) {
            const qsizetype tab = line.indexOf('\t');
            if (line.startsWith('#') || tab < 0)
                continue;
            const QList<uint> key = QString::fromUtf8(line.first(tab)).toUcs4();
            const QList<uint> form = QString::fromUtf8(line.sliced(tab + 1).split(' ').constFirst()).toUcs4();
            if (key.size() == 1 && form.size() == 1 && key[0] != form[0])
                table.insert(key[0], form[0]);
        }
        // A form that is listed itself (薴 → 苧 → 苎) is followed to the end,
        // so that all of them fold to one character.
        for (auto it = table.begin(); it != table.end(); ++it) {
            for (int hops = 0; hops < 8; ++hops) {
                const auto next = table.constFind(it.value());
                if (next == table.cend())
                    break;
                it.value() = next.value();
            }
        }
        return table;
    }();
    return forms;
}

// The code point that ends just before `i`, and the one that starts at `i`.
char32_t pointBefore(QStringView text, qsizetype i)
{
    if (i >= 2 && text[i - 1].isLowSurrogate() && text[i - 2].isHighSurrogate())
        return QChar::surrogateToUcs4(text[i - 2], text[i - 1]);
    return text[i - 1].unicode();
}
char32_t pointAt(QStringView text, qsizetype i)
{
    if (i + 1 < text.size() && text[i].isHighSurrogate() && text[i + 1].isLowSurrogate())
        return QChar::surrogateToUcs4(text[i], text[i + 1]);
    return text[i].unicode();
}

// Chinese and Japanese are written without spaces between words: next to one
// of their characters a space is OCR's guess, or the typist's habit around
// Latin words and numbers ("第 3 页", "Win顺 插件"), not a break a search
// should depend on. Korean (spaced like English) is not among them.
bool spaceless(char32_t c)
{
    return grams::isIndexed(c) && !(c >= 0xAC00 && c <= 0xD7AF) && !(c >= 0x3130 && c <= 0x318F);
}

bool wordCharacter(QChar c)
{
    return c.unicode() < 128 && c.isLetterOrNumber();
}
// One or two Latin letters or digits ("a4", "q3", "12"): too short for an
// ASCII triple, so they are indexed and matched as whole words instead.
bool shortWord(QStringView word)
{
    return (word.size() == 1 || word.size() == 2) && std::ranges::all_of(word, wordCharacter);
}

// Keep the existing CJK/ASCII candidate semantics. Hex tokens let FTS5
// compress the postings without storing word positions or a second text copy.
QString gramToken(grams::Key key)
{
    return u"g"_s + QString::number(key, 16);
}
// Never like a gram's token: 'w' is no hex digit.
QString wordToken(QStringView word)
{
    return u"w"_s + word.toString();
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
    QStringList words;
    for (qsizetype from = 0, end = 0; from < text.size(); from = end + 1) {
        for (end = from; end < text.size() && wordCharacter(text[end]);)
            ++end;
        if (const QStringView word = QStringView(text).sliced(from, end - from); shortWord(word))
            words.append(wordToken(word));
    }
    words.sort();
    words.removeDuplicates();
    return (tokens + words).join(u' ');
}

// What an asset is searched by: the lines it was read as, normalized.
QString assetText(const QList<OcrPiece>& pieces)
{
    QStringList lines;
    for (const OcrPiece& piece : pieces)
        lines.append(piece.text);
    return normalized(lines.join(u'\n'));
}

// Contentless FTS5 deletes must name exactly the tokens once inserted, and the
// stored text must be the one searches compare with: the index records which
// rules made both (normalized(), the gram rules of src/core grams, the short
// words). Any change to their output for this probe -- CJK pairs and singles,
// ASCII triples and short words, digits, punctuation, spaces OCR puts among
// Chinese characters, traditional characters, scripts that are not indexed --
// or to the table of traditional characters rebuilds the text and the tokens
// from the stored lines.
QString termsRules()
{
    static const QString rules = [] {
        const QString probe = normalized(u"报销规定 去年的 申 invoice 731905 abc-12.5 ab x A4 紙 第 3 頁 Q3 報銷 薴 "
                                         u"документ λόγος カタカナ テスト 한국어 문서 ①② 𠀀 𠀁 ＡＢＣ"_s);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData((probe + u'\n' + indexTokens(probe)).toUtf8());
        const auto& forms = simplifiedForms();
        std::vector<std::pair<char32_t, char32_t>> folds;
        folds.reserve(forms.size());
        for (auto it = forms.cbegin(); it != forms.cend(); ++it)
            folds.emplace_back(it.key(), it.value());
        std::ranges::sort(folds); // QHash's order differs from run to run
        for (const auto& [from, to] : folds) {
            const char32_t fold[] {from, to};
            hash.addData(QByteArrayView(reinterpret_cast<const char*>(fold), sizeof fold));
        }
        return u"2:"_s + QString::fromLatin1(hash.result().toHex().left(16));
    }();
    return rules;
}
QString storedTermsRules(const QSqlDatabase& db)
{
    QSqlQuery q(db);
    return q.exec(u"SELECT value FROM meta WHERE key='terms'"_s) && q.next() ? q.value(0).toString() : QString();
}

// A key under a folder's key (both pathKey()s).
bool under(const QString& key, const QString& folder)
{
    return key == folder || key.startsWith(folder.endsWith(u'/') ? folder : folder + u'/');
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
    text = text.normalized(QString::NormalizationForm_KC).toCaseFolded();
    const auto& forms = simplifiedForms();
    QString folded;
    folded.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char32_t c = pointAt(text, i);
        if (QChar::requiresSurrogates(c))
            ++i;
        folded += QChar::fromUcs4(forms.value(c, c));
    }
    folded = folded.simplified();
    // Spaces next to Chinese or Japanese characters go (spaceless()), and
    // between two Korean ones as before; spaces between other words stay.
    QString out;
    out.reserve(folded.size());
    for (qsizetype i = 0; i < folded.size(); ++i) {
        if (folded[i] == u' ' && i > 0 && i + 1 < folded.size()) {
            const char32_t before = pointBefore(folded, i), after = pointAt(folded, i + 1);
            if (spaceless(before) || spaceless(after) || (grams::isIndexed(before) && grams::isIndexed(after)))
                continue;
        }
        out += folded[i];
    }
    return out;
}

QStringList searchTerms(const QString& words)
{
    QStringList terms = contentTerms(words);
    for (QString& term : terms)
        term = normalized(term);
    terms.removeAll(QString());
    return terms;
}

bool containsTerm(QStringView text, QStringView term)
{
    if (!shortWord(term))
        return text.contains(term);
    for (qsizetype at = text.indexOf(term); at >= 0; at = text.indexOf(term, at + 1)) {
        const qsizetype end = at + term.size();
        if ((at == 0 || !wordCharacter(text[at - 1])) && (end == text.size() || !wordCharacter(text[end])))
            return true;
    }
    return false;
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
    return under(pathKey(path), pathKey(folder));
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
        if (schemaVersion > kSchema)
            throw std::runtime_error("This OCR index needs a newer plugin");
        if (capacityMB == 0) {
            if (schemaVersion != kSchema || storedTermsRules(m_db) != termsRules())
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
        m_maxPages = qint64(std::clamp(capacityMB, 16, 4096)) * 1024 * 1024 / bytesPerPage;
        exec(u"PRAGMA max_page_count=%1"_s.arg(m_maxPages));
        // Version 1: all schema/data changes roll back together on failure.
        if (schemaVersion < 2)
            begin();
        exec(
            u"CREATE TABLE IF NOT EXISTS files(path TEXT PRIMARY KEY,display TEXT NOT NULL,size INTEGER,modified INTEGER,hash BLOB,engine TEXT,generation INTEGER,status TEXT,error TEXT)"_s);
        exec(u"CREATE INDEX IF NOT EXISTS file_hash ON files(hash,engine,status)"_s);
        exec(
            u"CREATE TABLE IF NOT EXISTS assets(id INTEGER PRIMARY KEY,hash BLOB UNIQUE NOT NULL,width INTEGER,height INTEGER,text BLOB NOT NULL,pieces BLOB NOT NULL)"_s);
        exec(
            u"CREATE TABLE IF NOT EXISTS pages(file TEXT REFERENCES files(path) ON DELETE CASCADE,number INTEGER,asset INTEGER REFERENCES assets(id),label TEXT,source TEXT,PRIMARY KEY(file,number)) WITHOUT ROWID"_s);
        exec(u"CREATE INDEX IF NOT EXISTS page_asset ON pages(asset)"_s);
        exec(u"CREATE VIRTUAL TABLE IF NOT EXISTS asset_terms USING fts5(tokens,content='',detail=none,columnsize=0)"_s);
        exec(u"CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value TEXT)"_s);
        const bool rules = schemaVersion < 2 || storedTermsRules(m_db) != termsRules();
        if (rules || schemaVersion < kSchema) {
            // Reuse stored recognition: rebuilding never decodes source images
            // or calls OCR, and asset IDs and highlight boxes stay the same.
            if (!m_transaction)
                begin();
            if (rules) {
                rebuildTerms();
                exec(u"DROP TABLE IF EXISTS grams"_s);
            }
            if (schemaVersion < kSchema)
                recheckOlderPipeline();
            exec(u"PRAGMA user_version=%1"_s.arg(kSchema));
            commit();
            if (rules)
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

void Store::begin()
{
    if (!m_db.transaction())
        throw std::runtime_error(m_db.lastError().text().toStdString());
    m_transaction = true;
}

void Store::commit()
{
    if (!m_db.commit()) {
        const QString error = m_db.lastError().text();
        rollback();
        throw std::runtime_error(error.toStdString());
    }
    m_transaction = false;
}

// The pipeline before (ocr-search-2) left out PDF pages whose words are drawn
// rather than in the text layer, made long pictures and big pages smaller,
// read upside-down pages as they came, and gave up on dense pages it heard
// nothing from for a minute; installed, it found no pdfium.dll, so every PDF
// failed (extract::loadPdfium). Only the files where that can matter are walked
// again: their rows keep the older engine, so that the next scan visits them;
// every other row is marked as made by this pipeline. Nothing is decoded or
// read here, and a picture whose pixels come out the same finds its
// recognition again (imageHash()).
void Store::recheckOlderPipeline()
{
    const QString older = QString(ocr::kVersion) + QStringView(kOlderPipeline);
    {
        // Read poorly, maybe upside down: one more byte on the key makes the
        // next look-up miss, so the picture is read again (turned round, if
        // that reads clearly better). Until then the row serves searches.
        QSqlQuery poor(m_db), mark(m_db);
        poor.setForwardOnly(true);
        if (!poor.exec(u"SELECT id,hash,pieces FROM assets WHERE length(hash)=32"_s))
            throw std::runtime_error(poor.lastError().text().toStdString());
        mark.prepare(u"UPDATE assets SET hash=? WHERE id=?"_s);
        while (poor.next()) {
            const QJsonArray pieces = QJsonDocument::fromJson(qUncompress(poor.value(2).toByteArray())).array();
            double scores = 0;
            for (const auto& piece : pieces)
                scores += piece.toObject().value(u"score"_s).toDouble();
            if (pieces.size() < 2 || scores >= kPoorScore * pieces.size())
                continue;
            mark.bindValue(0, poor.value(1).toByteArray() + '\0');
            mark.bindValue(1, poor.value(0));
            checked(mark);
        }
    }
    QSqlQuery q(m_db);
    q.prepare(uR"(UPDATE files SET engine=? WHERE engine=? AND NOT (
            (status='empty' AND lower(path) LIKE '%.pdf')
            OR (status='failed' AND (error LIKE ? OR error LIKE ? OR error LIKE ? OR error LIKE ?))
            OR EXISTS(SELECT 1 FROM pages JOIN assets ON assets.id=pages.asset WHERE pages.file=files.path AND (
                length(assets.hash)<>32
                OR (json_extract(pages.source,'$.kind')='pdf' AND max(assets.width,assets.height)>=2600)
                OR (json_extract(pages.source,'$.kind') IN ('image','office') AND max(assets.width,assets.height)=4096
                    AND max(assets.width,assets.height)>3*min(assets.width,assets.height))))))"_s);
    q.addBindValue(pipelineEngine());
    q.addBindValue(older);
    // The worker's deadline and the missing PDF reader, as worded in either
    // language when they were stored.
    q.addBindValue(u"%秒没有进展%"_s);
    q.addBindValue(u"No progress on the file for %"_s);
    q.addBindValue(u"缺少 PDF 读取组件%"_s);
    q.addBindValue(u"PDF component is missing%"_s);
    checked(q);
    QSqlQuery note(m_db);
    note.prepare(u"INSERT OR REPLACE INTO meta VALUES('recheck',?)"_s);
    note.addBindValue(older);
    checked(note);
}

int Store::recheckRemaining() const
{
    QSqlQuery q(m_db);
    if (!q.exec(u"SELECT count(*) FROM files WHERE engine=(SELECT value FROM meta WHERE key='recheck')"_s) || !q.next())
        return 0;
    return q.value(0).toInt();
}

void Store::recheckDone()
{
    exec(u"DELETE FROM meta WHERE key='recheck'"_s);
}

void Store::rebuildTerms()
{
    exec(u"INSERT INTO asset_terms(asset_terms) VALUES('delete-all')"_s);
    // The text too is made again from the stored lines, by today's rules:
    // nothing is decoded or read with OCR again. In batches by ID, so that no
    // row changes under an open read of its table.
    QSqlQuery assets(m_db), update(m_db), insert(m_db);
    assets.prepare(u"SELECT id,text,pieces FROM assets WHERE id>? ORDER BY id LIMIT 256"_s);
    update.prepare(u"UPDATE assets SET text=? WHERE id=?"_s);
    insert.prepare(u"INSERT INTO asset_terms(rowid,tokens) VALUES(?,?)"_s);
    for (qint64 last = 0;;) {
        assets.bindValue(0, last);
        checked(assets);
        struct Row {
            qint64 id;
            QString text;
            bool changed;
        };
        QList<Row> rows;
        while (assets.next()) {
            const QString text = assetText(
                piecesFromJson(QJsonDocument::fromJson(qUncompress(assets.value(2).toByteArray())).array()));
            rows.append({assets.value(0).toLongLong(), text,
                text != QString::fromUtf8(qUncompress(assets.value(1).toByteArray()))});
        }
        assets.finish();
        if (rows.isEmpty())
            break;
        for (const Row& row : std::as_const(rows)) {
            if (row.changed) {
                update.bindValue(0, qCompress(row.text.toUtf8(), 6));
                update.bindValue(1, row.id);
                checked(update);
            }
            insert.bindValue(0, row.id);
            insert.bindValue(1, indexTokens(row.text));
            checked(insert);
        }
        last = rows.constLast().id;
    }
    insert.prepare(u"INSERT OR REPLACE INTO meta VALUES('terms',?)"_s);
    insert.addBindValue(termsRules());
    checked(insert);
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

QString Store::state(const QString& path, qint64 size, qint64 modified, const QString& engine) const
{
    QSqlQuery q(m_db);
    q.prepare(u"SELECT status FROM files WHERE path=? AND size=? AND modified=? AND engine=?"_s);
    q.addBindValue(pathKey(path));
    q.addBindValue(size);
    q.addBindValue(modified);
    q.addBindValue(engine);
    checked(q);
    return q.next() ? q.value(0).toString() : QString();
}

QString pipelineEngine()
{
    return QString(ocr::kVersion) + QStringView(kPipeline);
}

QString recognizerEngine()
{
    return QString(ocr::kVersion) + QStringView(kRecognizer);
}

bool settled(const QString& status)
{
    static const QStringList done {u"ready"_s, u"empty"_s, u"partial"_s, u"unsupported"_s, u"failed"_s};
    return done.contains(status);
}

bool Store::current(const QString& path, qint64 size, qint64 modified, const QString& engine) const
{
    return settled(state(path, size, modified, engine));
}

void Store::beginFile(
    const QString& path, qint64 size, qint64 modified, const QByteArray& hash, const QString& engine, qint64 generation)
{
    rollback();
    begin();
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
    const QString text = assetText(pieces);
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
    commit();
    ++m_changes;
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

void Store::prune(const QString& root, const QSet<QString>& seen, const QStringList& unreadable)
{
    const QString rootKey = pathKey(root);
    QStringList kept;
    for (const QString& folder : unreadable)
        kept.append(pathKey(folder));
    QSqlQuery q(m_db);
    if (!q.exec(u"SELECT path FROM files"_s))
        throw std::runtime_error(q.lastError().text().toStdString());
    QStringList stale;
    while (q.next()) {
        const QString path = q.value(0).toString();
        if (under(path, rootKey) && !seen.contains(path)
            && std::none_of(kept.begin(), kept.end(), [&](const QString& folder) { return under(path, folder); }))
            stale.append(path);
    }
    q.finish();
    remove(stale);
}

void Store::retainFolders(const QStringList& roots)
{
    QStringList keys;
    for (const QString& root : roots)
        keys.append(pathKey(root));
    QSqlQuery q(m_db);
    if (!q.exec(u"SELECT path FROM files"_s))
        throw std::runtime_error(q.lastError().text().toStdString());
    QStringList stale;
    while (q.next()) {
        const QString path = q.value(0).toString();
        if (std::none_of(keys.begin(), keys.end(), [&](const QString& root) { return under(path, root); }))
            stale.append(path);
    }
    q.finish();
    remove(stale);
}

void Store::uncap()
{
    exec(u"PRAGMA max_page_count=1073741823"_s);
}

void Store::recap() noexcept
{
    QSqlQuery(m_db).exec(u"PRAGMA max_page_count=%1"_s.arg(m_maxPages));
}

// Freeing space can itself need new pages (FTS5 records deletions as new
// segment data, b-trees rebalance), so maintenance runs above the user's cap
// and the cap applies again afterwards: a full index can still be emptied.
void Store::remove(const QStringList& paths)
{
    if (paths.isEmpty())
        return;
    uncap();
    const auto capped = qScopeGuard([this] { recap(); });
    begin();
    try {
        QSqlQuery q(m_db);
        q.prepare(u"DELETE FROM files WHERE path=?"_s);
        for (const QString& path : paths) {
            q.bindValue(0, path);
            checked(q);
        }
        commit();
    } catch (...) {
        rollback();
        throw;
    }
    m_changes += paths.size();
}

void Store::compact()
{
    uncap();
    const auto capped = qScopeGuard([this] { recap(); });
    QSqlQuery count(m_db);
    if (!count.exec(u"SELECT count(*),EXISTS(SELECT 1 FROM pages) FROM assets"_s) || !count.next())
        throw std::runtime_error(count.lastError().text().toStdString());
    const qint64 total = count.value(0).toLongLong();
    const bool referenced = count.value(1).toBool();
    count.finish();
    qint64 removed = 0;
    begin();
    try {
        if (!referenced) {
            // Nothing refers to any recognition (the index was cleared): one
            // command empties the inverted index instead of one per asset.
            removed = total;
            if (removed) {
                exec(u"INSERT INTO asset_terms(asset_terms) VALUES('delete-all')"_s);
                exec(u"DELETE FROM assets"_s);
            }
        } else {
            QSqlQuery stale(m_db), remove(m_db);
            if (!stale.exec(
                    u"SELECT id,text FROM assets WHERE NOT EXISTS(SELECT 1 FROM pages WHERE pages.asset=assets.id)"_s))
                throw std::runtime_error(stale.lastError().text().toStdString());
            remove.prepare(u"INSERT INTO asset_terms(asset_terms,rowid,tokens) VALUES('delete',?,?)"_s);
            while (stale.next()) {
                remove.bindValue(0, stale.value(0));
                remove.bindValue(1, indexTokens(QString::fromUtf8(qUncompress(stale.value(1).toByteArray()))));
                checked(remove);
                ++removed;
            }
            stale.finish();
            if (removed)
                exec(u"DELETE FROM assets WHERE NOT EXISTS(SELECT 1 FROM pages WHERE pages.asset=assets.id)"_s);
        }
        commit();
    } catch (...) {
        rollback();
        throw;
    }
    if (removed || m_changes) {
        // Rewriting the whole inverted index pays off only after big deletes;
        // otherwise a bounded merge. In its own transaction: when it finds no
        // room, the deletions above stay done and a later round merges again.
        try {
            begin();
            exec(removed && removed * 10 >= total ? u"INSERT INTO asset_terms(asset_terms) VALUES('optimize')"_s
                                                  : u"INSERT INTO asset_terms(asset_terms,rank) VALUES('merge',500)"_s);
            commit();
        } catch (const std::exception&) {
            rollback();
        }
    }
    m_changes = 0;
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
    q.prepare(u"SELECT count(*) FROM files WHERE status NOT IN ('ready','empty') AND engine=?"_s); // as errorText()
    q.addBindValue(pipelineEngine());
    if (q.exec() && q.next())
        out.insert(u"unfinished"_s, q.value(0).toInt());
    q.prepare(u"SELECT count(*) FROM files f WHERE "_s + QLatin1StringView(kTextless));
    q.addBindValue(pipelineEngine());
    if (q.exec() && q.next())
        out.insert(u"textless"_s, q.value(0).toInt());
    out.insert(u"recheck"_s, recheckRemaining());
    out.insert(u"bytes"_s, QFileInfo(m_path).size() + QFileInfo(m_path + u"-wal"_s).size()
        + QFileInfo(m_path + u"-shm"_s).size());
    return out;
}

QString Store::errorText() const
{
    // Only files this pipeline settled: one an upgrade marked to read again
    // keeps its old reason until then (1.0.0's missing PDF reader, a timeout),
    // and listing it would say the update had not helped.
    QSqlQuery q(m_db);
    q.prepare(u"SELECT display,error FROM files WHERE status NOT IN ('ready','empty') AND engine=? "
              u"ORDER BY display LIMIT 200"_s);
    q.addBindValue(pipelineEngine());
    q.exec();
    QStringList lines;
    while (q.next())
        lines.append(q.value(0).toString() + u"\n"_s + q.value(1).toString());
    return lines.join(u"\n\n"_s);
}

QString Store::textlessText() const
{
    // Documents by name; pictures without words (photos, mostly) only
    // counted after them, or they would bury the documents.
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    q.prepare(u"SELECT display,status,lower(path) LIKE '%.pdf',"
              u"(SELECT json_extract(source,'$.kind') FROM pages WHERE pages.file=f.path LIMIT 1) "
              u"FROM files f WHERE "_s
        + QLatin1StringView(kTextless) + u" ORDER BY display"_s);
    q.addBindValue(pipelineEngine());
    q.exec();
    QStringList lines;
    int more = 0;
    int pictures = 0;
    while (q.next()) {
        QString why;
        if (q.value(1).toString() == u"empty")
            why = q.value(2).toBool() ? QObject::tr("没有要识别的页面：字都在 PDF 的文字层里")
                                      : QObject::tr("文档里没有图片");
        else if (q.value(3).toString() == u"image")
            ++pictures;
        else
            why = QObject::tr("识别过了，没有认出文字");
        if (why.isEmpty())
            continue;
        if (lines.size() < 200)
            lines.append(q.value(0).toString() + u"\n"_s + why);
        else
            ++more;
    }
    if (more)
        lines.append(QObject::tr("另有 %n 个文件", nullptr, more));
    if (pictures)
        lines.append(QObject::tr("另有 %n 张图片识别过了，没有认出文字", nullptr, pictures));
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

QList<OcrPiece> Store::pieces(qint64 id, QSize* size) const
{
    QSqlQuery q(m_db);
    q.prepare(u"SELECT pieces,width,height FROM assets WHERE id=?"_s);
    q.addBindValue(id);
    checked(q);
    if (!q.next())
        return {};
    if (size)
        *size = QSize(q.value(1).toInt(), q.value(2).toInt());
    return piecesFromJson(QJsonDocument::fromJson(qUncompress(q.value(0).toByteArray())).array());
}

QList<Hit> Store::search(const QString& words, int limit, const QJsonObject& filters, const Cancel& cancel) const
{
    const ContentQuery scope = splitContentQuery(words.left(512));
    const QStringList terms = searchTerms(scope.words);
    if (terms.isEmpty() || limit <= 0)
        return {};
    QStringList candidates;
    for (const QString& term : terms) {
        QStringList tokens;
        for (auto key : grams::ofPhrase(term))
            tokens.append(gramToken(key));
        if (shortWord(term))
            tokens.append(wordToken(term));
        if (!tokens.isEmpty())
            candidates.append(tokens.join(u" AND "_s));
    }
    // Words with neither (Cyrillic, Greek, two signs, ...) cannot be looked
    // up: no scan of every file.
    if (candidates.isEmpty())
        return {};
    const QDateTime from = QDateTime::fromString(filters.value(u"modifiedFrom"_s).toString(), Qt::ISODate);
    const QDateTime to = QDateTime::fromString(filters.value(u"modifiedTo"_s).toString(), Qt::ISODate);
    QList<QRegularExpression> excludedNames;
    for (const auto& value : filters.value(u"excludedFileNames"_s).toArray())
        excludedNames.append(QRegularExpression(QRegularExpression::wildcardToRegularExpression(value.toString()),
            QRegularExpression::CaseInsensitiveOption));
    const QString folder = filters.value(u"folder"_s).toString();
    const QString picked = filters.value(u"contentPick"_s).toString();
    const auto extensions = filters.value(u"extensions"_s).toArray();
    const auto excludedExtensions = filters.value(u"excludedExtensions"_s).toArray();
    // From the path alone, before any text is read or the file is looked at.
    const auto acceptedPath = [&](const QFileInfo& info) {
        const QString path = info.absoluteFilePath();
        if (scope.folders.empty() && !folder.isEmpty() && !inside(path, folder))
            return false;
        if (!scope.folders.empty()) {
            bool matches = false;
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
        if (!extensions.isEmpty() && std::none_of(extensions.begin(), extensions.end(), [&](const auto& ext) {
                return ext.toString().compare(info.suffix(), Qt::CaseInsensitive) == 0;
            }))
            return false;
        for (const auto& ext : excludedExtensions)
            if (ext.toString().compare(info.suffix(), Qt::CaseInsensitive) == 0)
                return false;
        for (const auto& pattern : excludedNames)
            if (pattern.match(info.fileName()).hasMatch())
                return false;
        return true;
    };
    // The file itself: only for rows whose text matched, at most `limit` of them.
    const auto acceptedFile = [&](const QFileInfo& info) {
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
    QSqlQuery pages(m_db);
    pages.prepare(
        u"SELECT a.id,a.text,p.label,p.source FROM pages p JOIN assets a ON a.id=p.asset WHERE p.file=? ORDER BY p.number"_s);
    QList<Hit> out;
    QHash<qint64, QString> cache;
    while (files.next()) {
        if (cancel && cancel())
            break;
        const QString path = files.value(0).toString();
        QFileInfo info(path);
        if (!acceptedPath(info))
            continue;
        pages.bindValue(0, files.value(3));
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
                if (!containsTerm(text, terms[term]))
                    continue;
                ++count;
                if (!found[term]) {
                    found[term] = true;
                    --remaining;
                }
            }
            if (count > bestCount) {
                bestCount = count;
                best = {path, {}, {}, asset, 0, 0, QJsonDocument::fromJson(pages.value(3).toString().toUtf8()).object()};
                best.location = locationText(best.source, pages.value(2).toString());
                bestText = text;
            }
            if (!remaining)
                break;
        }
        pages.finish();
        if (remaining || !best.asset)
            continue;
        // Still the file that was recognised: same size and modified time.
        if (!info.exists() || info.size() != files.value(1).toLongLong()
            || info.lastModified().toMSecsSinceEpoch() != files.value(2).toLongLong() || !acceptedFile(info))
            continue;
        best.size = info.size();
        best.modified = info.lastModified().toSecsSinceEpoch();
        best.source.insert(u"fileModifiedMs"_s, info.lastModified().toMSecsSinceEpoch());
        for (const auto& piece : pieces(best.asset)) {
            const QString text = normalized(piece.text);
            if (std::any_of(
                    terms.begin(), terms.end(), [&](const QString& term) { return containsTerm(text, term); })) {
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
