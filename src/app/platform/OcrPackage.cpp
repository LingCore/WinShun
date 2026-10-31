#include "OcrPackage.h"

#include "OcrFiles.h"
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
QString tr(const char* text) { return QCoreApplication::translate("ws::OcrPackage", text); }
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

} // namespace ocr

OcrPackage::OcrPackage(QObject* parent, const QString& programFolder, download::Fetcher fetcher)
    : QObject(parent)
    , m_programFolder(programFolder.isEmpty() ? QCoreApplication::applicationDirPath() : programFolder)
    , m_fetcher(std::move(fetcher))
{
    QDir().mkpath(ocr::componentsRoot());
    m_watcher.addPath(ocr::componentsRoot());
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &OcrPackage::refresh);
    refresh();
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
