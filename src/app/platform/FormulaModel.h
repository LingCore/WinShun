#pragma once

#include "Http.h"

#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace ws {

class OcrPackage;

namespace formula {

// The model of 公式识别 (FormulaReader, ocr::formulaModel): its files as
// Pix2Text publishes them at one commit, each taken only with this size and hash.
struct File {
    QString name;
    QByteArray sha256;
    qint64 size = 0;
};
struct Spec {
    QString version; // the folder's name under the components, and its receipt
    QStringList sources; // where the files are, in order: a file's name after one is its address
    QList<File> files;
    qint64 size() const;
};
Spec spec();

// What http::download does: the body to `sink` as it comes.
using Fetcher = std::function<http::Response(const QString& url, qint64 maxBytes, const http::Sink& sink)>;
using Progress = std::function<void(qint64 received)>;

// Every file from the first of `sources` that has it whole (a web address,
// else a folder), the next one when a source fails, is wrong or crawls;
// staged beside the model's place and moved in whole once all are there.
// Empty, or why not in words for the user. Installed before is kept until
// the new copy is complete.
QString install(const Spec& spec, const QStringList& sources, const Fetcher& fetch, const Progress& progress,
    const std::atomic_bool& cancelled);
// Empty, or why it is still there (a reader has it open).
QString remove(const Spec& spec);

} // namespace formula

// The model in the settings (增强文字识别 › 公式识别模型) and for the
// screenshot plugin (App's formula_model tool): fetched, installed and
// removed. It runs on the OCR package's ONNX Runtime: download() fetches
// that first when it is not there.
class FormulaModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(bool installed READ installed NOTIFY changed FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY changed FINAL)
    Q_PROPERTY(QString status READ status NOTIFY changed FINAL)
    Q_PROPERTY(QString downloadSize READ downloadSize CONSTANT FINAL)

public:
    // `runtime`: the OCR package, which this one is a child of.
    explicit FormulaModel(OcrPackage* runtime, formula::Fetcher fetcher = {}, formula::Spec spec = formula::spec());
    ~FormulaModel() override;
    bool installed() const { return m_installed; }
    bool busy() const { return m_busy; }
    double progress() const { return m_progress; }
    QString status() const { return m_status; } // of the last download or removal; empty before any
    QString downloadSize() const;
    Q_INVOKABLE void download();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void installLocal(); // asks for a file of the model (localRequested)
    void installFolder(const QString& folder); // the files fetched by hand, in one folder
    Q_INVOKABLE void remove();
    void refresh(); // another window or process installed or removed it

    // installed, busy, progress, status and size, for the screenshot plugin.
    QJsonObject state() const;

signals:
    void changed();
    void localRequested();

private:
    void fetch(double done); // `done`: the share of the progress already made (the OCR package)
    void start(const QStringList& sources, double done);
    void runtimeChanged();
    double runtimeShare() const; // the OCR package's share of fetching both
    OcrPackage* m_runtime;
    formula::Fetcher m_fetcher;
    formula::Spec m_spec;
    bool m_installed = false;
    bool m_busy = false;
    bool m_awaitingRuntime = false; // the OCR package is being fetched first
    double m_progress = 0;
    QString m_status;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QFileSystemWatcher m_watcher;
    std::jthread m_worker; // joined before the QObject and cancellation state are destroyed
};

} // namespace ws
