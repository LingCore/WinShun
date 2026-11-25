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

// The larger reading model (OcrFiles: ocr::preciseModel), one file fetched
// from where RapidOCR publishes it and taken only with this size and hash.
struct Model {
    QString url;
    QByteArray sha256;
    qint64 size = 0;
};
Model preciseSpec();
// The file as ocr::preciseModel finds it, or nothing: staged beside its
// place and moved in whole. Empty, or why not in words for the user.
QString installPrecise(const QByteArray& model, const Model& spec, const std::atomic_bool& cancelled);
// Empty, or why it is still there (a reader has it open).
QString removePrecise();

// A reading model of the user's own (OcrFiles: ocr::ownModel): the file
// picked is tried (PaddleOcr::tryReadingModel, with the .txt of its name
// beside it as its dictionary when it has no characters of its own), then
// copied in whole. Empty, or why not in words for the user.
QString installOwn(const QString& file, const QString& programFolder, const std::atomic_bool& cancelled);
QString removeOwn();
}

// The larger reading model in the settings: fetched, installed and removed.
// Installed is in use (PaddleOcr::load), by the next reader that is made.
class OcrModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(bool installed READ installed NOTIFY changed FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY changed FINAL)
    Q_PROPERTY(QString status READ status NOTIFY changed FINAL)
    Q_PROPERTY(QString downloadSize READ downloadSize CONSTANT FINAL)
    // The user's own reading model in use, by its file's name; else empty.
    Q_PROPERTY(QString ownName READ ownName NOTIFY changed FINAL)

public:
    explicit OcrModel(QObject* parent = nullptr, download::Fetcher fetcher = {}, ocr::Model spec = ocr::preciseSpec(),
        const QString& programFolder = {});
    ~OcrModel() override;
    bool installed() const { return m_installed; }
    bool busy() const { return m_busy; }
    double progress() const { return m_progress; }
    QString status() const { return m_status; } // of the last download or removal; empty before any
    QString downloadSize() const;
    Q_INVOKABLE void download();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void installLocal();
    Q_INVOKABLE void remove();
    QString ownName() const { return m_ownName; }
    Q_INVOKABLE void chooseOwn(); // asks for the file (ownRequested)
    Q_INVOKABLE void removeOwn();
    void useOwnFile(const QString& file); // tried, then copied in and used
    void installFile(const QString& file); // the .onnx file itself, fetched by hand
    void refresh(); // another window, or the screenshot plugin, installed or removed it

signals:
    void changed();
    void localRequested();
    void ownRequested();

private:
    void start(const QString& source);
    download::Fetcher m_fetcher;
    ocr::Model m_spec;
    QString m_programFolder;
    QString m_ownName;
    bool m_installed = false;
    bool m_busy = false;
    double m_progress = 0;
    QString m_status;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QFileSystemWatcher m_watcher;
    std::jthread m_worker; // joined before the QObject and cancellation state are destroyed
};

class OcrPackage : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(bool available READ available NOTIFY changed FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY changed FINAL)
    Q_PROPERTY(QString status READ status NOTIFY changed FINAL)
    Q_PROPERTY(QString downloadSize READ downloadSize CONSTANT FINAL)
    Q_PROPERTY(ws::OcrModel* preciseModel READ preciseModel CONSTANT FINAL)

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
    OcrModel* preciseModel() const { return m_precise; } // offered once this package is there

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
    OcrModel* m_precise; // a child
    std::jthread m_worker; // joined before the QObject and cancellation state are destroyed
};

} // namespace ws
