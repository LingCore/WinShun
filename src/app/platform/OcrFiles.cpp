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

QString preciseFolder()
{
    return QDir(componentsRoot()).filePath(QString(kPreciseVersion));
}

QString preciseModel()
{
    const QString folder = preciseFolder();
    QFile receipt(folder + u"/.ready"_s);
    if (!receipt.open(QIODevice::ReadOnly) || receipt.read(128) != QString(kPreciseVersion).toUtf8())
        return {};
    const QFileInfo model(QDir(folder).filePath(QString(kPreciseModel)));
    return model.isFile() && model.size() > 0 ? model.absoluteFilePath() : QString();
}

QString ownFolder()
{
    return QDir(componentsRoot()).filePath(QString(kOwnVersion));
}

OwnModel ownModel()
{
    const QString folder = ownFolder();
    QFile receipt(folder + u"/.ready"_s); // the name where the user had it
    if (!receipt.open(QIODevice::ReadOnly))
        return {};
    const QString name = QString::fromUtf8(receipt.read(1024)).trimmed();
    const QFileInfo model(folder + u"/model.onnx"_s);
    if (name.isEmpty() || !model.isFile() || model.size() == 0)
        return {};
    const QFileInfo dictionary(folder + u"/dictionary.txt"_s);
    return {model.absoluteFilePath(), dictionary.isFile() ? dictionary.absoluteFilePath() : QString(), name,
        u"own-%1-%2"_s.arg(name).arg(model.size())};
}

QString formulaFolder()
{
    return QDir(componentsRoot()).filePath(QString(kFormulaVersion));
}

QString formulaModel()
{
    const QString path = formulaFolder();
    QFile receipt(path + u"/.ready"_s);
    if (!receipt.open(QIODevice::ReadOnly) || receipt.read(128) != QString(kFormulaVersion).toUtf8())
        return {};
    for (const char16_t* name : kFormulaFiles) {
        const QFileInfo file(QDir(path).filePath(QString(name)));
        if (!file.isFile() || file.size() == 0)
            return {};
    }
    return QDir(path).absolutePath();
}

QString folder(const QString& programFolder)
{
    if (const QString installed = installedFolder(); !installed.isEmpty())
        return installed;
    return !programFolder.isEmpty() && hasFiles(programFolder) ? programFolder : QString();
}

} // namespace ws::ocr
