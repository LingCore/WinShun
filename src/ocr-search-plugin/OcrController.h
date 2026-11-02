#pragma once

#include "OcrSource.h"

#include <QObject>
#include <QSettings>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <stop_token>
#include <thread>

class QQuickImageProvider;

namespace ws::ocrsearch {

class PreviewImages;

class Controller : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the plugin")
    Q_PROPERTY(QStringList folders READ folders NOTIFY changed FINAL)
    Q_PROPERTY(QStringList excluded READ excluded NOTIFY changed FINAL)
    Q_PROPERTY(int capacityMB READ capacityMB WRITE setCapacityMB NOTIFY changed FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    Q_PROPERTY(bool autoUpdate READ autoUpdate NOTIFY changed FINAL)
    Q_PROPERTY(bool setupNeeded READ setupNeeded NOTIFY changed FINAL)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed FINAL)
    Q_PROPERTY(QString progressText READ progressText NOTIFY changed FINAL)
    Q_PROPERTY(QVariantMap statistics READ statistics NOTIFY changed FINAL)
    Q_PROPERTY(QString errors READ errors NOTIFY changed FINAL)
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged FINAL)
    Q_PROPERTY(QVariantMap preview READ preview NOTIFY previewChanged FINAL)
    Q_PROPERTY(bool searching READ searching NOTIFY resultsChanged FINAL)
public:
    Controller(QString appDir, QString dataDir, QObject* parent = nullptr);
    ~Controller() override;
    QStringList folders() const { return m_options.folders; }
    QStringList excluded() const { return m_options.excluded; }
    int capacityMB() const { return m_options.capacityMB; }
    bool busy() const { return m_busy; }
    bool autoUpdate() const { return m_enabled; }
    bool setupNeeded() const;
    QString statusText() const { return m_status; }
    QString progressText() const { return m_progress; }
    QVariantMap statistics() const { return m_statistics; }
    QString errors() const { return m_errors; }
    QVariantList results() const { return m_results; }
    QVariantMap preview() const { return m_preview; }
    bool searching() const { return m_searching; }
    const QString& dataDir() const { return m_dataDir; }
    void setCapacityMB(int value);
    void addPath(const QString& path, bool exclusion = false);
    QQuickImageProvider* imageProvider();
    Q_INVOKABLE void addFolder(bool exclusion = false);
    Q_INVOKABLE void removeFolder(int index, bool exclusion = false);
    Q_INVOKABLE void start(bool retry = false);
    Q_INVOKABLE void pause();
    Q_INVOKABLE void setAutoUpdate(bool enabled);
    Q_INVOKABLE void clearIndex();
    Q_INVOKABLE void search(const QString& words);
    Q_INVOKABLE void select(int index);
    Q_INVOKABLE void openFile(const QString& path);
    Q_INVOKABLE void showFolder(const QString& path);
    Q_INVOKABLE void copyText();
    void showHit(const Hit& hit, const QString& words);
    void externalQuery(const QJsonObject& params, std::function<void(const QJsonObject&)> reply);
    void cancelQuery();
signals:
    void changed();
    void resultsChanged();
    void previewChanged();
    void setupRequested();
    void refreshRequested();

private:
    void save();
    void refresh();
    QString m_appDir;
    QString m_dataDir;
    QSettings m_settings;
    ScanOptions m_options;
    QTimer m_timer;
    std::jthread m_scan;
    QThreadPool m_searches;
    QThreadPool m_previews;
    std::stop_source m_searchStop;
    std::stop_source m_previewStop;
    QThreadPool m_queries;
    std::stop_source m_externalStop;
    PreviewImages* m_images = nullptr; // owned by QQmlEngine
    bool m_busy = false;
    bool m_searching = false;
    bool m_enabled = false;
    QString m_status;
    QString m_progress;
    QString m_errors;
    QString m_words;
    QVariantMap m_statistics;
    QVariantList m_results;
    QVariantMap m_preview;
    QList<Hit> m_hits;
    quint64 m_previewRevision = 0;
};

QJsonObject hitJson(const Hit& hit);
Hit hitFromJson(const QJsonObject& object);

} // namespace ws::ocrsearch
