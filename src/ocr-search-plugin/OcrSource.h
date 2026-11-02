#pragma once

#include "OcrStore.h"

#include <QImage>

#include <stdexcept>

namespace ws::ocrsearch {

// Not about the file at hand (the recognition parts or the worker do not
// load): the round stops and nothing is recorded as failed for the file.
struct ScanStopped : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Extraction {
    int images = 0;
    bool partial = false;
    QString message;
};

bool supported(const QString& path);
bool legacyDocument(const QString& path);
bool onlineOnly(const QString& path);
using ImageSink = std::function<void(const QImage&, const QString&, const QJsonObject&)>;
// `advanced`: after each page, picture or embedded image is done with (also
// the ones skipped), for a deadline that measures progress.
Extraction extractImages(const QString& path, const ImageSink& sink, const Cancel& cancel = {},
    const std::function<void()>& advanced = {});
QImage previewImage(const QString& path, const QJsonObject& source);
QByteArray imageHash(const QImage& image, const QString& engine);

struct ScanOptions {
    QStringList folders;
    QStringList excluded;
    int capacityMB = 512;
    QString workerProgram;
    int timeoutMs = 60000;
    bool retry = false;
};
struct ScanProgress {
    int checked = 0;
    int recognized = 0;
    QString current;
    QString message;
};
void scan(const QString& appDir, const QString& dataDir, const ScanOptions& options, const Cancel& cancel,
    const std::function<void(const ScanProgress&)>& progress);

} // namespace ws::ocrsearch
