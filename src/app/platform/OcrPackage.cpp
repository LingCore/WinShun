#include "OcrPackage.h"

#include "OcrFiles.h"
#include "PaddleOcr.h"
#include "Http.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QPointer>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>

#include <miniz.h>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws {
namespace {
constexpr qint64 kMaxArchive = 64ll << 20;
// lupdate files a tr() in ws::ocr under "ws::ocr": the same here, or English never shows.
QString tr(const char* text) { return QCoreApplication::translate("ws::ocr", text); }
}

namespace ocr {

Package package()
{
    QFile file(u":/components/ocr.json"_s);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    Package result {json.value(u"version"_s).toString(), json.value(u"name"_s).toString(),
        json.value(u"downloadBytes"_s).toInteger(), {}};
    for (const QJsonValue& value : json.value(u"files"_s).toArray()) {
        const QJsonObject item = value.toObject();
        result.files.append({item.value(u"path"_s).toString(), item.value(u"sha256"_s).toString().toLatin1(), item.value(u"size"_s).toInteger()});
    }
    return result;
}

namespace {
QString readArchive(const QByteArray& bytes, const Package& spec, const std::atomic_bool& cancelled, const QString& folder = {})
{
    if (bytes.isEmpty() || bytes.size() > kMaxArchive || spec.files.isEmpty() || spec.version != QString(kVersion))
        return tr("增强识别包无效，请重新下载。");
    mz_zip_archive archive {};
    if (!mz_zip_reader_init_mem(&archive, bytes.constData(), static_cast<size_t>(bytes.size()), 0))
        return tr("增强识别包已损坏，请重新下载。");
    const auto close = qScopeGuard([&] { mz_zip_reader_end(&archive); });
    QSet<QString> seen;
    const auto count = mz_zip_reader_get_num_files(&archive);
    if (count > 32)
        return tr("增强识别包包含不需要的文件。");
    for (mz_uint index = 0; index < count; ++index) {
        if (cancelled.load())
            return tr("已取消，稍后可以重新安装。");
        mz_zip_archive_file_stat info {};
        if (!mz_zip_reader_file_stat(&archive, index, &info) || info.m_is_encrypted)
            return tr("增强识别包已损坏，请重新下载。");
        if (info.m_is_directory)
            continue;
        const QString path = QString::fromUtf8(info.m_filename);
        const auto found = std::ranges::find(spec.files, path, &File::path);
        if (found == spec.files.end() || seen.contains(path) || path.contains(u'\\') || path.startsWith(u'/') || path.split(u'/').contains(u".."_s))
            return tr("增强识别包包含不需要的文件。");
        if (found->size <= 0 || found->size > (32ll << 20) || info.m_uncomp_size != static_cast<mz_uint64>(found->size))
            return tr("增强识别包的文件大小不符，请重新下载。");
        QByteArray content(found->size, Qt::Uninitialized);
        if (!mz_zip_reader_extract_to_mem(&archive, index, content.data(), static_cast<size_t>(content.size()), 0)
            || QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex() != found->sha256)
            return tr("增强识别包未通过校验，请重新下载。");
        if (!folder.isEmpty()) {
            const QString target = QDir(folder).filePath(path);
            if (!QDir().mkpath(QFileInfo(target).absolutePath()))
                return tr("无法保存增强识别文件。");
            QSaveFile file(target);
            if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit())
                return tr("无法保存增强识别文件。");
        }
        seen.insert(path);
    }
    if (seen.size() != spec.files.size())
        return tr("增强识别包缺少文件，请重新下载。");
    return {};
}
}

QString verify(const QByteArray& bytes, const Package& spec, const std::atomic_bool& cancelled)
{
    return readArchive(bytes, spec, cancelled);
}

QString install(const QByteArray& bytes, const QString& root, const Package& spec, const std::atomic_bool& cancelled)
{
    if (bytes.isEmpty() || bytes.size() > kMaxArchive || spec.files.isEmpty() || spec.version != QString(kVersion))
        return tr("增强识别包无效，请重新下载。");
    if (!QDir().mkpath(root))
        return tr("无法创建增强识别的安装目录。");
    QLockFile lock(root + u"/ocr-install.lock"_s);
    if (!lock.tryLock(0))
        return tr("另一个窗口正在安装增强识别，完成后会自动可用。");
    QTemporaryDir staged(root + u"/ocr-install-XXXXXX"_s);
    if (!staged.isValid())
        return tr("无法创建增强识别的安装目录。");
    if (const QString error = readArchive(bytes, spec, cancelled, staged.path()); !error.isEmpty())
        return error;
    QSaveFile receipt(staged.path() + u"/.ready"_s);
    const QByteArray version = spec.version.toUtf8();
    if (!receipt.open(QIODevice::WriteOnly) || receipt.write(version) != version.size() || !receipt.commit())
        return tr("无法保存增强识别文件。");
    if (cancelled.load())
        return tr("已取消，稍后可以重新安装。");
    const QString destination = QDir(root).filePath(spec.version);
    QString previous;
    if (QFileInfo::exists(destination)) {
        previous = destination + u".previous-"_s + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!QDir().rename(destination, previous))
            return tr("增强识别文件正在使用，请关闭识别窗口后重试。");
    }
    if (!QDir().rename(staged.path(), destination)) {
        if (!previous.isEmpty())
            QDir().rename(previous, destination);
        return tr("无法完成增强识别安装，原来的文件已保留。");
    }
    staged.setAutoRemove(false);
    return {};
}

Model preciseSpec()
{
    return {u"https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv6/rec/PP-OCRv6_rec_small.onnx"_s,
        "6f327246b50388f3c176ae304bd95767ea6dc0c9ae92153ef8cbe210b3c14884", 21234383};
}

QString installPrecise(const QByteArray& model, const Model& spec, const std::atomic_bool& cancelled)
{
    if (spec.size <= 0 || model.size() != spec.size || QCryptographicHash::hash(model, QCryptographicHash::Sha256).toHex() != spec.sha256)
        return tr("高精度模型未通过校验，请重新下载。");
    const QString root = componentsRoot();
    if (!QDir().mkpath(root))
        return tr("无法创建高精度模型的安装目录。");
    QLockFile lock(root + u"/ocr-model-install.lock"_s);
    if (!lock.tryLock(0))
        return tr("另一个窗口正在安装高精度模型，完成后会自动可用。");
    QTemporaryDir staged(root + u"/ocr-model-XXXXXX"_s);
    if (!staged.isValid())
        return tr("无法创建高精度模型的安装目录。");
    const auto save = [&](const QString& name, const QByteArray& content) {
        QSaveFile file(staged.filePath(name));
        return file.open(QIODevice::WriteOnly) && file.write(content) == content.size() && file.commit();
    };
    if (!save(QString(kPreciseModel), model) || !save(u".ready"_s, QString(kPreciseVersion).toUtf8()))
        return tr("无法保存高精度模型，请检查磁盘空间。");
    if (cancelled.load())
        return tr("已取消，稍后可以重新安装。");
    // What is left of an earlier copy (no receipt, or one being read) out of the way first.
    const QString destination = preciseFolder();
    if (QFileInfo::exists(destination) && !QDir(destination).removeRecursively())
        return tr("高精度模型正在使用，请关闭识别窗口后重试。");
    if (!QDir().rename(staged.path(), destination))
        return tr("无法完成高精度模型的安装。");
    staged.setAutoRemove(false);
    return {};
}

QString removePrecise()
{
    const QString folder = preciseFolder();
    if (!QFileInfo::exists(folder))
        return {};
    // The receipt first: no reader takes the model from here on, whatever
    // of the rest a reader still holds open goes with the next removal.
    QFile::remove(folder + u"/.ready"_s);
    if (!preciseModel().isEmpty())
        return tr("高精度模型正在使用，请关闭识别窗口后重试。");
    QDir(folder).removeRecursively();
    return {};
}

QString installOwn(const QString& file, const QString& programFolder, const std::atomic_bool& cancelled)
{
    const QFileInfo picked(file);
    if (!picked.isFile() || picked.size() <= 0 || picked.size() > (512ll << 20))
        return tr("这个文件不能用作识别模型。");
    if (ocr::folder(programFolder).isEmpty())
        return tr("请先安装增强文字识别。");
    // Its characters: in the model, else the .txt of its name beside it.
    QString dictionary;
    QString why;
    if (!PaddleOcr::tryReadingModel(programFolder, file, {}, &why)) {
        const QString beside = picked.absolutePath() + u'/' + picked.completeBaseName() + u".txt"_s;
        if (why != u"no characters in the reading model")
            return tr("这个文件不能用作识别模型：它不是 PP-OCR 的文字识别（rec）模型。");
        if (!QFileInfo(beside).isFile())
            return tr("这个模型里没有字典。把它的字典文件改成同名的 %1，和模型放在同一个文件夹里再选。").arg(QFileInfo(beside).fileName());
        if (!PaddleOcr::tryReadingModel(programFolder, file, beside, &why))
            return tr("这个模型和旁边的字典 %1 对不上（字数不符），或不是 PP-OCR 的文字识别（rec）模型。").arg(QFileInfo(beside).fileName());
        dictionary = beside;
    }
    if (cancelled.load())
        return tr("已取消。");
    const QString root = componentsRoot();
    if (!QDir().mkpath(root))
        return tr("无法保存这个模型，请检查磁盘空间。");
    QLockFile lock(root + u"/ocr-model-install.lock"_s);
    if (!lock.tryLock(0))
        return tr("另一个窗口正在安装模型，请稍后再试。");
    QTemporaryDir staged(root + u"/ocr-own-XXXXXX"_s);
    QSaveFile receipt(staged.filePath(u".ready"_s));
    const QByteArray name = picked.fileName().toUtf8();
    if (!staged.isValid() || !QFile::copy(file, staged.filePath(u"model.onnx"_s))
        || (!dictionary.isEmpty() && !QFile::copy(dictionary, staged.filePath(u"dictionary.txt"_s)))
        || !receipt.open(QIODevice::WriteOnly) || receipt.write(name) != name.size() || !receipt.commit())
        return tr("无法保存这个模型，请检查磁盘空间。");
    const QString destination = ownFolder();
    if (QFileInfo::exists(destination) && !QDir(destination).removeRecursively())
        return tr("原来的模型正在使用，请关闭识别窗口后重试。");
    if (!QDir().rename(staged.path(), destination))
        return tr("无法保存这个模型，请检查磁盘空间。");
    staged.setAutoRemove(false);
    return {};
}

QString removeOwn()
{
    const QString folder = ownFolder();
    if (!QFileInfo::exists(folder))
        return {};
    QFile::remove(folder + u"/.ready"_s); // no reader takes it from here on
    if (!ownModel().file.isEmpty())
        return tr("原来的模型正在使用，请关闭识别窗口后重试。");
    QDir(folder).removeRecursively();
    return {};
}

} // namespace ocr

OcrModel::OcrModel(QObject* parent, download::Fetcher fetcher, ocr::Model spec, const QString& programFolder)
    : QObject(parent)
    , m_fetcher(std::move(fetcher))
    , m_spec(std::move(spec))
    , m_programFolder(programFolder.isEmpty() ? QCoreApplication::applicationDirPath() : programFolder)
{
    QDir().mkpath(ocr::componentsRoot());
    m_watcher.addPath(ocr::componentsRoot());
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &OcrModel::refresh);
    refresh();
}

OcrModel::~OcrModel()
{
    if (m_cancel)
        m_cancel->store(true);
}

QString OcrModel::downloadSize() const
{
    return tr("约 %1 MB").arg((m_spec.size + 999999) / 1000000);
}

void OcrModel::refresh()
{
    // The components folder alone is watched: the model's own folder comes
    // and goes whole, and a watch on it would hold it open against that.
    const bool installed = !ocr::preciseModel().isEmpty();
    const QString ownName = ocr::ownModel().name;
    if (m_installed != installed || m_ownName != ownName) {
        m_installed = installed;
        m_ownName = ownName;
        if (!m_busy)
            m_status.clear();
        emit changed();
    }
}

void OcrModel::chooseOwn()
{
    emit ownRequested();
}

void OcrModel::removeOwn()
{
    if (m_busy || m_ownName.isEmpty())
        return;
    const QString error = ocr::removeOwn();
    refresh();
    m_status = error;
    emit changed();
}

void OcrModel::useOwnFile(const QString& file)
{
    if (m_busy)
        return;
    m_busy = true;
    m_progress = 0;
    m_status = tr("正在检查这个模型…");
    m_cancel = std::make_shared<std::atomic_bool>(false);
    emit changed();
    m_worker = std::jthread([self = QPointer(this), file, folder = m_programFolder, cancel = m_cancel] {
        const QString error = ocr::installOwn(file, folder, *cancel);
        if (self)
            QMetaObject::invokeMethod(self, [self, error] {
                if (!self)
                    return;
                self->m_busy = false;
                self->refresh();
                self->m_status = error;
                emit self->changed();
            }, Qt::QueuedConnection);
    });
}

void OcrModel::download()
{
    const QString given = qEnvironmentVariable("WINSHUN_OCR_MODEL");
    start(given.isEmpty() ? m_spec.url : given);
}

void OcrModel::cancel()
{
    if (m_cancel && m_busy) {
        m_cancel->store(true);
        m_status = tr("正在取消…");
        emit changed();
    }
}

void OcrModel::installLocal()
{
    emit localRequested();
}

void OcrModel::installFile(const QString& file) { start(file); }

void OcrModel::remove()
{
    if (m_busy || !m_installed)
        return;
    const QString error = ocr::removePrecise();
    refresh();
    m_status = error;
    m_progress = 0;
    emit changed();
}

void OcrModel::start(const QString& source)
{
    if (m_busy || m_installed)
        return;
    m_busy = true;
    m_progress = 0;
    m_status = tr("正在下载高精度模型…");
    m_cancel = std::make_shared<std::atomic_bool>(false);
    emit changed();
    m_worker = std::jthread([self = QPointer(this), source, spec = m_spec, cancel = m_cancel, fetcher = m_fetcher] {
        const auto progress = [self, cancel, spec](qint64 received) {
            if (cancel->load())
                return false;
            if (self)
                QMetaObject::invokeMethod(self, [self, received, spec] {
                    if (self) {
                        self->m_progress = std::min(0.99, static_cast<double>(received) / std::max<qint64>(1, spec.size));
                        emit self->changed();
                    }
                }, Qt::QueuedConnection);
            return true;
        };
        const download::Fetcher web = fetcher ? fetcher : [](const QString& url, qint64 maxBytes, const download::Progress& onProgress) {
            http::Response response = http::get(url, {}, 15000, maxBytes, http::Session::Own, onProgress);
            return download::Fetched {response.status, std::move(response.body), response.error};
        };
        const auto genuine = [&](const QByteArray& content) {
            return content.size() == spec.size && QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex() == spec.sha256;
        };
        const download::Fetched fetched = download::fetch(source, spec.size, web, progress, genuine);
        const QString scheme = QUrl(source).scheme();
        QString error;
        if (scheme != u"http" && scheme != u"https" && (fetched.status != 200 || !genuine(fetched.body)))
            error = tr("这不是高精度模型文件 %1。").arg(QString(ocr::kPreciseModel)); // a file picked by hand
        else if (fetched.status != 200)
            error = tr("下载没有完成，请检查网络后重试，或从本地安装模型文件。");
        else
            error = ocr::installPrecise(fetched.body, spec, *cancel);
        if (cancel->load())
            error = tr("已取消，稍后可以重新安装。");
        if (self)
            QMetaObject::invokeMethod(self, [self, error] {
                if (!self)
                    return;
                self->m_busy = false;
                self->refresh();
                self->m_status = error;
                self->m_progress = error.isEmpty() ? 1 : 0;
                emit self->changed();
            }, Qt::QueuedConnection);
    });
}

OcrPackage::OcrPackage(QObject* parent, const QString& programFolder, download::Fetcher fetcher)
    : QObject(parent)
    , m_programFolder(programFolder.isEmpty() ? QCoreApplication::applicationDirPath() : programFolder)
    , m_fetcher(std::move(fetcher))
    , m_precise(new OcrModel(this, m_fetcher, ocr::preciseSpec(), m_programFolder))
{
    QDir().mkpath(ocr::componentsRoot());
    m_watcher.addPath(ocr::componentsRoot());
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &OcrPackage::refresh);
    refresh();
    m_formula = new FormulaModel(this);
}

OcrPackage::~OcrPackage()
{
    if (m_cancel)
        m_cancel->store(true);
}

QString OcrPackage::downloadSize() const
{
    return tr("约 %1 MB").arg((ocr::package().downloadBytes + 999999) / 1000000);
}

void OcrPackage::refresh()
{
    const QString installed = QDir(ocr::componentsRoot()).filePath(QString(ocr::kVersion));
    for (const QString& path : {installed, installed + u"/ocr"_s, m_programFolder + u"/ocr"_s}) {
        if (QFileInfo(path).isDir() && !m_watcher.directories().contains(path))
            m_watcher.addPath(path);
    }
    const bool available = !ocr::folder(m_programFolder).isEmpty();
    if (m_available != available) {
        m_available = available;
        if (!m_busy)
            m_status.clear();
        emit changed();
    }
}

void OcrPackage::download()
{
    const QString given = qEnvironmentVariable("WINSHUN_OCR_PACKAGE");
    const QString source = given.isEmpty()
        ? u"https://github.com/LingCore/WinShun/releases/download/v"_s + QString::fromLatin1(WINSHUN_VERSION) + u'/' + ocr::package().name
        : given;
    start(source);
}

void OcrPackage::cancel()
{
    if (m_cancel && m_busy) {
        m_cancel->store(true);
        m_status = tr("正在取消…");
        emit changed();
    }
}

void OcrPackage::installLocal()
{
    emit localRequested();
}

void OcrPackage::installFile(const QString& file) { start(file); }

void OcrPackage::start(const QString& source)
{
    if (m_busy || m_available)
        return;
    const ocr::Package spec = ocr::package();
    m_busy = true;
    m_progress = 0;
    m_status = tr("正在下载安装增强识别…");
    m_cancel = std::make_shared<std::atomic_bool>(false);
    emit changed();
    m_worker = std::jthread([self = QPointer(this), source, spec, root = ocr::componentsRoot(), cancel = m_cancel, fetcher = m_fetcher] {
        QByteArray bytes;
        QString error;
        const auto progress = [self, cancel, spec](qint64 received) {
            if (cancel->load())
                return false;
            if (self)
                QMetaObject::invokeMethod(self, [self, received, spec] {
                    if (self) {
                        self->m_progress = std::min(0.99, static_cast<double>(received) / std::max<qint64>(1, spec.downloadBytes));
                        emit self->changed();
                    }
                }, Qt::QueuedConnection);
            return true;
        };
        const download::Fetcher web = fetcher ? fetcher : [](const QString& url, qint64 maxBytes, const download::Progress& onProgress) {
            http::Response response = http::get(url, {}, 15000, maxBytes, http::Session::Own, onProgress);
            return download::Fetched {response.status, std::move(response.body), response.error};
        };
        const download::Fetched fetched = download::fetch(source, kMaxArchive, web, progress,
            [&](const QByteArray& content) { return ocr::verify(content, spec, *cancel).isEmpty(); });
        if (fetched.status == 200)
            bytes = fetched.body;
        else {
            const QString scheme = QUrl(source).scheme();
            error = scheme == u"http" || scheme == u"https"
                ? tr("下载没有完成，请检查网络后重试，或从本地安装增强识别包。")
                : tr("无法读取增强识别包。");
        }
        if (error.isEmpty())
            error = ocr::install(bytes, root, spec, *cancel);
        if (cancel->load())
            error = tr("已取消，稍后可以重新安装。");
        if (self)
            QMetaObject::invokeMethod(self, [self, error] {
                if (!self)
                    return;
                self->m_busy = false;
                self->refresh();
                self->m_status = error.isEmpty() ? tr("增强识别已安装，可以离线使用。") : error;
                self->m_progress = error.isEmpty() ? 1 : 0;
                emit self->changed();
            }, Qt::QueuedConnection);
    });
}

} // namespace ws
