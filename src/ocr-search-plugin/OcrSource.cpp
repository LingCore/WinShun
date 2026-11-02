#include "OcrSource.h"
#include "OcrWorker.h"
#include "PictureFile.h"

#include "Extract.h"
#include "Zip.h"
#include "platform/OcrFiles.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>

#include <fpdfview.h>
#include <objbase.h>

#include <cmath>
#include <filesystem>
#include <mutex>
#include <stdexcept>

using namespace Qt::StringLiterals;

namespace ws::ocrsearch {
namespace {
constexpr qint64 kMaxFile = 100ll << 20;
constexpr int kMaxImages = 500;
constexpr int kRenderSide = 2600;
std::mutex pdfMutex;

[[noreturn]] void fail(const QString& message)
{
    throw std::runtime_error(message.toUtf8().constData());
}
void stopped(const Cancel& cancel)
{
    if (cancel && cancel())
        fail(QObject::tr("已暂停，稍后可继续"));
}

QByteArray readFile(const QString& path)
{
    if (onlineOnly(path))
        fail(QObject::tr("文件仅在云端，下载到本机后再试"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail(file.errorString());
    if (file.size() > kMaxFile)
        fail(QObject::tr("超过单文件 100 MB 的识别上限"));
    const QByteArray bytes = file.read(kMaxFile + 1);
    if (bytes.size() != file.size() || file.error() != QFile::NoError)
        fail(QObject::tr("文件读取不完整"));
    return bytes;
}

struct ComScope {
    bool initialized = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    ~ComScope()
    {
        if (initialized)
            ::CoUninitialize();
    }
};

QImage opaque(QImage image)
{
    if (!image.hasAlphaChannel())
        return image.convertToFormat(QImage::Format_RGB32);
    QImage result(image.size(), QImage::Format_RGB32);
    result.fill(Qt::white);
    QPainter painter(&result);
    painter.drawImage(0, 0, image);
    return result;
}

QImage decode(QByteArray bytes, int frame = 0, int* frames = nullptr)
{
    ComScope com;
    QSize original;
    QImage native = decodePictureData(bytes, 4096, &original, frame, frames);
    if (!native.isNull()) {
        if (qint64(original.width()) * original.height() > 100'000'000)
            fail(QObject::tr("图片尺寸无效或超过 1 亿像素"));
        return opaque(std::move(native));
    }
    if (frame != 0)
        fail(QObject::tr("无法读取多页图片"));
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > 100'000'000)
        fail(QObject::tr("图片尺寸无效或超过 1 亿像素"));
    if (size.width() > 4096 || size.height() > 4096)
        reader.setScaledSize(size.scaled(4096, 4096, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (image.isNull())
        fail(reader.errorString());
    if (frames)
        *frames = std::max(1, reader.imageCount());
    return opaque(std::move(image));
}

struct Pdf {
    QByteArray bytes;
    FPDF_DOCUMENT document = nullptr;
    explicit Pdf(const QString& path)
        : bytes(readFile(path))
    {
        const std::lock_guard lock(pdfMutex);
        if (!extract::loadPdfium())
            fail(QObject::tr("缺少 PDF 读取组件，请更新 Win顺"));
        document = FPDF_LoadMemDocument64(bytes.constData(), bytes.size(), nullptr);
        if (!document) {
            const auto error = FPDF_GetLastError();
            fail(error == FPDF_ERR_PASSWORD || error == FPDF_ERR_SECURITY ? QObject::tr("PDF 已加密，解密后再试")
                                                                          : QObject::tr("PDF 已损坏或无法读取"));
        }
    }
    ~Pdf()
    {
        const std::lock_guard lock(pdfMutex);
        if (document)
            FPDF_CloseDocument(document);
    }
    int count() const
    {
        const std::lock_guard lock(pdfMutex);
        return FPDF_GetPageCount(document);
    }
    QImage render(int number) const
    {
        const std::lock_guard lock(pdfMutex);
        FPDF_PAGE page = FPDF_LoadPage(document, number - 1);
        if (!page)
            fail(QObject::tr("无法读取 PDF 第 %1 页").arg(number));
        const double w = FPDF_GetPageWidthF(page), h = FPDF_GetPageHeightF(page);
        if (!(w > 0 && h > 0 && std::isfinite(w) && std::isfinite(h))) {
            FPDF_ClosePage(page);
            fail(QObject::tr("PDF 页面尺寸无效"));
        }
        const double scale = std::min(200.0 / 72.0, kRenderSide / std::max(w, h));
        QImage image(
            std::max(1, int(std::ceil(w * scale))), std::max(1, int(std::ceil(h * scale))), QImage::Format_RGB32);
        if (image.isNull()) {
            FPDF_ClosePage(page);
            fail(QObject::tr("没有足够内存渲染 PDF"));
        }
        FPDF_BITMAP bitmap
            = FPDFBitmap_CreateEx(image.width(), image.height(), FPDFBitmap_BGRx, image.bits(), image.bytesPerLine());
        if (!bitmap) {
            FPDF_ClosePage(page);
            fail(QObject::tr("PDF 页面渲染失败"));
        }
        FPDFBitmap_FillRect(bitmap, 0, 0, image.width(), image.height(), 0xFFFFFFFF);
        FPDF_RenderPageBitmap(bitmap, page, 0, 0, image.width(), image.height(), 0, FPDF_ANNOT);
        FPDFBitmap_Destroy(bitmap);
        FPDF_ClosePage(page);
        return image;
    }
};

QByteArray partBytes(extract::Zip& zip, const std::string& name, std::size_t limit = 32u << 20)
{
    const auto index = zip.find(name);
    std::string bytes;
    if (!index || zip.encrypted(*index) || !zip.readAll(*index, bytes, limit))
        fail(QObject::tr("无法读取文档内的图片或关联信息"));
    return QByteArray(bytes.data(), qsizetype(bytes.size()));
}

struct Relationship {
    QString id;
    QString type;
    std::string target;
};
QList<Relationship> relationships(extract::Zip& zip, const std::string& part)
{
    const auto slash = part.find_last_of('/');
    const std::string rel = part.substr(0, slash + 1) + "_rels/" + part.substr(slash + 1) + ".rels";
    if (!zip.find(rel))
        return {};
    QXmlStreamReader xml(partBytes(zip, rel, 4u << 20));
    QList<Relationship> out;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != u"Relationship")
            continue;
        const auto attributes = xml.attributes();
        if (attributes.value(u"TargetMode").compare(u"External", Qt::CaseInsensitive) == 0)
            continue;
        const QString target = attributes.value(u"Target").toString();
        out.append({attributes.value(u"Id").toString(), attributes.value(u"Type").toString(),
            extract::resolvePartPath(part, target.toUtf8().toStdString())});
    }
    if (xml.hasError())
        fail(QObject::tr("文档的图片关联信息已损坏"));
    return out;
}

struct Embedded {
    std::string part;
    QString label;
    QJsonObject place;
};
QList<Embedded> embedded(extract::Zip& zip)
{
    QList<Embedded> out;
    QHash<QString, QString> drawingSheets;
    QHash<QString, QString> sheetNames;
    QHash<QString, int> slideOrder;
    const bool hasPresentation = zip.find("ppt/presentation.xml").has_value();
    if (hasPresentation) {
        const auto rels = relationships(zip, "ppt/presentation.xml");
        QXmlStreamReader xml(partBytes(zip, "ppt/presentation.xml"));
        int number = 0;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement() || xml.name() != u"sldId")
                continue;
            ++number;
            for (const auto& attr : xml.attributes())
                if (attr.name() == u"id")
                    for (const auto& rel : rels)
                        if (rel.id == attr.value() && rel.type.endsWith(u"/slide"))
                            slideOrder.insert(QString::fromStdString(rel.target), number);
        }
        if (xml.hasError())
            fail(QObject::tr("文档的图片引用已损坏"));
    }
    if (zip.find("xl/workbook.xml")) {
        const auto rels = relationships(zip, "xl/workbook.xml");
        QXmlStreamReader xml(partBytes(zip, "xl/workbook.xml"));
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == u"sheet") {
                QString id;
                for (const auto& attr : xml.attributes())
                    if (attr.name() == u"id")
                        id = attr.value().toString();
                for (const auto& rel : rels)
                    if (rel.id == id)
                        sheetNames.insert(
                            QString::fromStdString(rel.target), xml.attributes().value(u"name").toString());
            }
        }
        for (auto it = sheetNames.cbegin(); it != sheetNames.cend(); ++it)
            for (const auto& rel : relationships(zip, it.key().toStdString()))
                if (rel.type.endsWith(u"/drawing"))
                    drawingSheets.insert(QString::fromStdString(rel.target), it.value());
    }
    const QRegularExpression slideNumber(u"^ppt/slides/slide(\\d+)\\.xml$"_s);
    for (std::size_t i = 0; i < zip.count(); ++i) {
        const std::string& name = zip.name(i);
        const QString part = QString::fromStdString(name);
        if (!part.endsWith(u".xml")
            || !(part.startsWith(u"word/") || part.startsWith(u"ppt/slides/slide")
                || part.startsWith(u"xl/drawings/drawing")))
            continue;
        if (hasPresentation && part.startsWith(u"ppt/slides/") && !slideOrder.contains(part))
            continue;
        const auto rels = relationships(zip, name);
        QHash<QString, std::string> images;
        for (const auto& rel : rels)
            if (rel.type.endsWith(u"/image"))
                images.insert(rel.id, rel.target);
        if (images.isEmpty())
            continue;
        QXmlStreamReader xml(partBytes(zip, name));
        int paragraph = 0, imageNumber = 0;
        const auto slide = slideNumber.match(part);
        const int slidePosition = slideOrder.value(part, slide.captured(1).toInt());
        QSet<QString> used;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement())
                continue;
            if (xml.name() == u"p")
                ++paragraph;
            for (const auto& attr : xml.attributes()) {
                if (attr.name() != u"embed" && attr.name() != u"id")
                    continue;
                const QString id = attr.value().toString();
                if (!images.contains(id) || used.contains(id))
                    continue;
                used.insert(id);
                ++imageNumber;
                QString label;
                if (slide.hasMatch())
                    label = QObject::tr("第 %1 张幻灯片 · 图片 %2").arg(slidePosition).arg(imageNumber);
                else if (drawingSheets.contains(part))
                    label = QObject::tr("工作表「%1」· 图片 %2").arg(drawingSheets.value(part)).arg(imageNumber);
                else if (part == u"word/document.xml")
                    label = QObject::tr("正文第 %1 段 · 图片 %2").arg(paragraph).arg(imageNumber);
                else
                    label = QObject::tr("%1 · 图片 %2").arg(QFileInfo(part).completeBaseName()).arg(imageNumber);
                QJsonObject place {{u"imageNumber"_s, imageNumber}};
                if (slide.hasMatch())
                    place.insert(u"slide"_s, slidePosition);
                else if (drawingSheets.contains(part))
                    place.insert(u"sheet"_s, drawingSheets.value(part));
                else if (part == u"word/document.xml")
                    place.insert(u"paragraph"_s, paragraph);
                else
                    place.insert(u"section"_s, QFileInfo(part).completeBaseName());
                out.append({images.value(id), label, place});
                if (out.size() > kMaxImages)
                    return out;
            }
        }
        if (xml.hasError())
            fail(QObject::tr("文档的图片引用已损坏"));
    }
    return out;
}
} // namespace

bool supported(const QString& path)
{
    static const QStringList types {u"png"_s, u"jpg"_s, u"jpeg"_s, u"bmp"_s, u"tif"_s, u"tiff"_s, u"webp"_s, u"pdf"_s,
        u"docx"_s, u"docm"_s, u"pptx"_s, u"pptm"_s, u"xlsx"_s, u"xlsm"_s};
    return types.contains(QFileInfo(path).suffix().toLower());
}
bool legacyDocument(const QString& path)
{
    static const QStringList types {u"doc"_s, u"xls"_s, u"ppt"_s, u"wps"_s, u"et"_s, u"dps"_s};
    return types.contains(QFileInfo(path).suffix().toLower());
}
bool onlineOnly(const QString& path)
{
    const DWORD attributes = ::GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_OFFLINE | 0x00040000 | 0x00400000));
}

Extraction extractImages(const QString& path, const ImageSink& sink, const Cancel& cancel)
{
    Extraction result;
    QStringList issues;
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == u"pdf") {
        Pdf pdf(path);
        const int count = pdf.count();
        for (int page = 1; page <= std::min(count, kMaxImages); ++page) {
            stopped(cancel);
            QImage image;
            try {
                image = pdf.render(page);
            } catch (const std::exception& e) {
                issues.append(QString::fromUtf8(e.what()));
                continue;
            }
            sink(image, QObject::tr("第 %1 页").arg(page), {{u"kind"_s, u"pdf"_s}, {u"page"_s, page}});
            ++result.images;
        }
        result.partial = count > kMaxImages;
    } else if (suffix == u"docx" || suffix == u"docm" || suffix == u"pptx" || suffix == u"pptm" || suffix == u"xlsx"
        || suffix == u"xlsm") {
        const QByteArray bytes = readFile(path);
        extract::MemorySource source(std::string_view(bytes.constData(), bytes.size()));
        extract::Zip zip(source, 256ull << 20);
        if (!zip.ok())
            fail(QObject::tr("文档已加密、损坏或不是有效的 Office Open XML 文件"));
        const auto images = embedded(zip);
        for (const auto& item : images) {
            stopped(cancel);
            if (result.images >= kMaxImages) {
                result.partial = true;
                break;
            }
            QImage image;
            try {
                image = decode(partBytes(zip, item.part));
            } catch (const std::exception& e) {
                issues.append(item.label + u"："_s + QString::fromUtf8(e.what()));
                continue;
            }
            QJsonObject imageSource = item.place;
            imageSource.insert(u"kind"_s, u"office"_s);
            imageSource.insert(u"part"_s, QString::fromStdString(item.part));
            sink(image, item.label, imageSource);
            ++result.images;
        }
    } else {
        const QByteArray bytes = readFile(path);
        int count = 1;
        QImage first = decode(bytes, 0, &count);
        for (int frame = 0; frame < std::min(count, kMaxImages); ++frame) {
            stopped(cancel);
            const QImage image = frame == 0 ? first : decode(bytes, frame);
            sink(image, count > 1 ? QObject::tr("第 %1 页").arg(frame + 1) : QObject::tr("图片"),
                {{u"kind"_s, u"image"_s}, {u"frame"_s, frame}, {u"frames"_s, count}});
            ++result.images;
        }
        result.partial = count > kMaxImages;
    }
    if (result.partial)
        result.message = QObject::tr("达到单文件 500 页／张的上限，结果不完整");
    if (!issues.isEmpty()) {
        result.partial = true;
        result.message += (result.message.isEmpty() ? u""_s : u"\n"_s)
            + QObject::tr("部分图片未完成：%1").arg(issues.mid(0, 5).join(u"；"_s));
    }
    return result;
}

QImage previewImage(const QString& path, const QJsonObject& source)
{
    const QString kind = source.value(u"kind"_s).toString();
    if (kind == u"pdf")
        return Pdf(path).render(source.value(u"page"_s).toInt());
    if (kind == u"office") {
        const QByteArray bytes = readFile(path);
        extract::MemorySource input(std::string_view(bytes.constData(), bytes.size()));
        extract::Zip zip(input, 256ull << 20);
        return decode(partBytes(zip, source.value(u"part"_s).toString().toStdString()));
    }
    return decode(readFile(path), source.value(u"frame"_s).toInt());
}

QByteArray imageHash(const QImage& image, const QString& engine)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(engine.toUtf8());
    hash.addData(QByteArray::number(image.width()) + ':' + QByteArray::number(image.height()) + ':');
    for (int y = 0; y < image.height(); ++y)
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(image.constScanLine(y)), image.width() * 4));
    return hash.result();
}

void scan(const QString& appDir, const QString& dataDir, const ScanOptions& options, const Cancel& cancel,
    const std::function<void(const ScanProgress&)>& progress)
{
    Store store(dataDir, options.capacityMB);
    const QString engine = QString(ocr::kVersion) + u"/ocr-search-2"_s;
    const qint64 generation = QDateTime::currentMSecsSinceEpoch();
    std::unique_ptr<PaddleOcr> reader;
    std::unique_ptr<Worker> worker;
    if (!options.workerProgram.isEmpty())
        worker = std::make_unique<Worker>(options.workerProgram, appDir, dataDir, engine, options.timeoutMs);
    ScanProgress state;
    store.retainFolders(options.folders);
    QSet<QString> visited;
    for (const QString& root : options.folders) {
        stopped(cancel);
        if (!QDir(root).exists()) {
            state.message = QObject::tr("目录暂时不可用，保留已有索引：%1").arg(root);
            progress(state);
            continue;
        }
        std::error_code error;
        std::filesystem::recursive_directory_iterator it(std::filesystem::path(root.toStdWString()), error), end;
        bool complete = !error;
        for (; it != end; it.increment(error)) {
            stopped(cancel);
            if (error) {
                complete = false;
                error.clear();
                continue;
            }
            const QString path = QString::fromStdWString(it->path().wstring());
            const bool excluded = std::any_of(options.excluded.begin(), options.excluded.end(),
                [&](const QString& folder) { return inside(path, folder); });
            const DWORD attributes = ::GetFileAttributesW(it->path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                complete = false;
                continue;
            }
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (excluded || attributes & FILE_ATTRIBUTE_REPARSE_POINT || inside(path, dataDir))
                    it.disable_recursion_pending();
                continue;
            }
            if (excluded || (!supported(path) && !legacyDocument(path)) || visited.contains(pathKey(path)))
                continue;
            std::error_code typeError;
            if (std::filesystem::is_symlink(it->symlink_status(typeError)))
                continue;
            if (typeError) {
                complete = false;
                continue;
            }
            visited.insert(pathKey(path));
            const QFileInfo info(path);
            const qint64 size = info.size(), modified = info.lastModified().toMSecsSinceEpoch();
            state.current = QDir::toNativeSeparators(path);
            ++state.checked;
            state.message.clear();
            progress(state);
            store.touch(path, generation);
            if (!options.retry && store.current(path, size, modified, engine))
                continue;
            if (legacyDocument(path)) {
                store.status(path, size, modified, engine, generation, u"unsupported"_s,
                    QObject::tr("旧版格式暂不支持图片识别，请转换为 DOCX、PPTX 或 XLSX"));
                continue;
            }
            if (onlineOnly(path)) {
                store.status(path, size, modified, engine, generation, u"offline"_s,
                    QObject::tr("文件仅在云端，下载到本机后再试"));
                continue;
            }
            try {
                const QByteArray bytes = readFile(path);
                const QByteArray digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
                if (store.reuse(path, size, modified, digest, engine, generation))
                    continue;
                store.beginFile(path, size, modified, digest, engine, generation);
                Extraction result;
                if (worker) {
                    const QJsonObject reply = worker->read(path, cancel);
                    for (const auto& value : reply.value(u"pages"_s).toArray()) {
                        stopped(cancel);
                        const QJsonObject page = value.toObject();
                        const QByteArray hash = QByteArray::fromHex(page.value(u"hash"_s).toString().toLatin1());
                        qint64 id = store.asset(hash);
                        if (!id) {
                            id = store.putAsset(hash,
                                QSize(page.value(u"width"_s).toInt(), page.value(u"height"_s).toInt()),
                                piecesFromJson(page.value(u"pieces"_s).toArray()));
                            ++state.recognized;
                            progress(state);
                        }
                        store.addPage(id, page.value(u"label"_s).toString(), page.value(u"source"_s).toObject());
                    }
                    result.images = reply.value(u"images"_s).toInt();
                    result.partial = reply.value(u"partial"_s).toBool();
                    result.message = reply.value(u"message"_s).toString();
                } else
                    result = extractImages(
                        path,
                        [&](const QImage& image, const QString& label, const QJsonObject& source) {
                            stopped(cancel);
                            const QByteArray hash = imageHash(image, engine);
                            qint64 id = store.asset(hash);
                            if (!id) {
                                if (!reader) {
                                    QString message;
                                    reader = PaddleOcr::load(appDir, 2, &message);
                                    if (!reader)
                                        fail(QObject::tr("需要增强识别组件：%1").arg(message));
                                }
                                QList<OcrPiece> pieces;
                                QString message;
                                if (!reader->readPieces(image, &pieces, &message))
                                    fail(message);
                                stopped(cancel);
                                id = store.putAsset(hash, image.size(), pieces);
                                ++state.recognized;
                                progress(state);
                            }
                            store.addPage(id, label, source);
                        },
                        cancel);
                const QFileInfo after(path);
                if (after.size() != size || after.lastModified().toMSecsSinceEpoch() != modified)
                    fail(QObject::tr("识别期间文件发生变化，下次更新时重试"));
                store.finishFile(result.partial ? u"partial"_s
                        : result.images         ? u"ready"_s
                                                : u"empty"_s,
                    result.message);
            } catch (const std::exception& exception) {
                store.rollback();
                stopped(cancel);
                const QString message = QString::fromUtf8(exception.what());
                if (message.contains(u"full", Qt::CaseInsensitive))
                    fail(QObject::tr("索引空间已满，请提高容量上限或移除部分目录"));
                store.status(path, size, modified, engine, generation, u"failed"_s, message);
            }
        }
        if (error)
            complete = false;
        if (complete)
            store.prune(root, generation);
        else {
            state.message = QObject::tr("部分目录无法读取，保留未检查文件的索引：%1").arg(root);
            progress(state);
        }
    }
    store.compact();
}

} // namespace ws::ocrsearch
