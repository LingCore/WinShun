#pragma once

#include "Download.h"

#include <QObject>
#include <QFileSystemWatcher>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>
#include <thread>

namespace ws {

namespace ocr {
struct File {
    QString path;
    QByteArray sha256;
    qint64 size = 0;
};
struct Package {
    QString version;
    QString name;
    qint64 downloadBytes = 0;
    QList<File> files;
};
Package package();
QString verify(const QByteArray& archive, const Package& package, const std::atomic_bool& cancelled);
// Extracts only pinned files; verifies every size and hash before publishing.
// A failed or cancelled operation leaves the previous installed copy intact.
QString install(const QByteArray& archive, const QString& root, const Package& package, const std::atomic_bool& cancelled);
}

class OcrPackage : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(bool available READ available NOTIFY changed FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY changed FINAL)
    Q_PROPERTY(QString status READ status NOTIFY changed FINAL)
    Q_PROPERTY(QString downloadSize READ downloadSize CONSTANT FINAL)

public:
    explicit OcrPackage(QObject* parent = nullptr, const QString& programFolder = {}, download::Fetcher fetcher = {});
    ~OcrPackage() override;
    bool available() const { return m_available; }
    bool busy() const { return m_busy; }
    double progress() const { return m_progress; }
    QString status() const { return m_available && !m_busy ? tr("增强识别已安装，可以离线使用。") : m_status; }
    QString downloadSize() const;
    Q_INVOKABLE void download();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void installLocal();
    void installFile(const QString& file); // local archives, including the full offline distribution
    void refresh();

signals:
    void changed();
    void localRequested();

private:
    void start(const QString& source);
    QString m_programFolder;
    download::Fetcher m_fetcher;
    bool m_available = false;
    bool m_busy = false;
    double m_progress = 0;
    QString m_status;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QFileSystemWatcher m_watcher;
    std::jthread m_worker; // joined before the QObject and cancellation state are destroyed
};

} // namespace ws
