#pragma once

#include "OcrSource.h"
#include <QProcess>

namespace ws::ocrsearch {

// One parser process per pass, reset after a crash, timeout or cancellation.
// Index replies contain text and coordinates; preview replies contain a
// bounded PNG. Neither is persisted as an image cache.
class Worker {
public:
    Worker(QString program, QString appDir, QString dataDir, QString engine, int timeoutMs = 60000);
    ~Worker();
    QJsonObject read(const QString& path, const Cancel& cancel);
    QImage preview(const QString& path, const QJsonObject& source, const Cancel& cancel);

private:
    QJsonObject request(const QJsonObject& parameters, const Cancel& cancel);
    void reset();
    QProcess m_process;
    QString m_program, m_appDir, m_dataDir, m_engine;
    int m_timeoutMs;
};
int workerMain(const QString& appDir);

} // namespace ws::ocrsearch
