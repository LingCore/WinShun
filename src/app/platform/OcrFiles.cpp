#include "OcrFiles.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace ws::ocr {
namespace {
QMutex rootMutex;
QString explicitRoot;
}

void setComponentsRoot(const QString& folder)
{
    const QMutexLocker lock(&rootMutex);
    explicitRoot = folder.isEmpty() ? QString() : QDir::cleanPath(folder);
}

QString componentsRoot()
{
    const QMutexLocker lock(&rootMutex);
    if (!explicitRoot.isEmpty())
        return explicitRoot;
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/WinShun/components"_s;
}

bool hasFiles(const QString& folder)
{
    for (const QString& file : {u"onnxruntime.dll"_s, u"ocr/PP-OCRv6_det_tiny.onnx"_s, u"ocr/PP-OCRv6_rec_tiny.onnx"_s}) {
        const QFileInfo info(QDir(folder).filePath(file));
        if (!info.isFile() || info.size() == 0)
            return false;
    }
    return true;
}

QString installedFolder()
{
    const QString path = QDir(componentsRoot()).filePath(QString(kVersion));
    QFile receipt(path + u"/.ready"_s);
    return receipt.open(QIODevice::ReadOnly) && receipt.read(128) == QString(kVersion).toUtf8() && hasFiles(path) ? path : QString();
}

QString folder(const QString& programFolder)
{
    if (const QString installed = installedFolder(); !installed.isEmpty())
        return installed;
    return !programFolder.isEmpty() && hasFiles(programFolder) ? programFolder : QString();
}

} // namespace ws::ocr
