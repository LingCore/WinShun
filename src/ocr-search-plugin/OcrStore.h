#pragma once

#include "platform/PaddleOcr.h"

#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <QSqlDatabase>
#include <QStringList>

#include <functional>

namespace ws::ocrsearch {

using Cancel = std::function<bool()>;
QString normalized(QString text);
QString pathKey(const QString& path);
bool inside(const QString& path, const QString& folder);
QString locationText(const QJsonObject& source, const QString& fallback);
QJsonArray piecesToJson(const QList<OcrPiece>& pieces);
QList<OcrPiece> piecesFromJson(const QJsonArray& array);
// A stored file status with nothing left to do while the file is unchanged:
// done, or failed (only a manual "recheck all" redoes those).
bool settled(const QString& status);

struct Hit {
    QString path;
    QString location;
    QString snippet;
    qint64 asset = 0;
    qint64 size = 0;
    qint64 modified = 0;
    QJsonObject source;
};

// One connection per calling thread. Images never go into the database:
// compressed recognition results are shared by their image-content hash.
class Store {
public:
    explicit Store(const QString& folder, int capacityMB = 512);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // The stored status while the row still describes this file (same size,
    // modified time and engine); empty otherwise.
    QString state(const QString& path, qint64 size, qint64 modified, const QString& engine) const;
    // settled(state()).
    bool current(const QString& path, qint64 size, qint64 modified, const QString& engine) const;
    bool reuse(const QString& path, qint64 size, qint64 modified, const QByteArray& hash, const QString& engine,
        qint64 generation);
    void beginFile(const QString& path, qint64 size, qint64 modified, const QByteArray& hash, const QString& engine,
        qint64 generation);
    qint64 asset(const QByteArray& hash) const;
    qint64 putAsset(const QByteArray& hash, const QSize& size, const QList<OcrPiece>& pieces);
    void addPage(qint64 asset, const QString& label, const QJsonObject& source);
    void finishFile(const QString& status = QStringLiteral("ready"), const QString& error = {});
    void rollback();
    void status(const QString& path, qint64 size, qint64 modified, const QString& engine, qint64 generation,
        const QString& state, const QString& error);
    // Drops the rows under `root` that a complete walk did not see (`seen`:
    // pathKey()s), except under folders that could not be listed.
    void prune(const QString& root, const QSet<QString>& seen, const QStringList& unreadable = {});
    void retainFolders(const QStringList& roots);
    void compact();
    QJsonObject statistics() const;
    QList<Hit> search(
        const QString& words, int limit, const QJsonObject& filters = {}, const Cancel& cancel = {}) const;
    // The recognised pieces; `size`: the recognised image's size, their coordinate space.
    QList<OcrPiece> pieces(qint64 asset, QSize* size = nullptr) const;
    QString errorText() const;

private:
    void exec(const QString& sql) const;
    void begin();
    void commit();
    void remove(const QStringList& paths);
    void uncap();
    void recap() noexcept;
    void rebuildTerms();
    void reclaim();
    QSqlDatabase m_db;
    QString m_connection;
    QString m_path;
    QString m_file;
    bool m_transaction = false;
    int m_page = 0;
    qint64 m_maxPages = 0;
    qint64 m_changes = 0; // rows written since the last compact()
};

} // namespace ws::ocrsearch
