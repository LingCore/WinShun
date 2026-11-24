#include "PictureWrite.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QPainter>
#include <QScopeGuard>
#include <QUuid>

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;
using Microsoft::WRL::ComPtr;

namespace ws::capture {

namespace {

enum class Format { Png, Jpeg, Bmp, Tiff };

constexpr std::array<std::pair<QStringView, Format>, 9> kWritableFormats {{
    {u"png", Format::Png},
    {u"jpg", Format::Jpeg},
    {u"jpeg", Format::Jpeg},
    {u"jpe", Format::Jpeg},
    {u"jfif", Format::Jpeg},
    {u"bmp", Format::Bmp},
    {u"dib", Format::Bmp},
    {u"tif", Format::Tiff},
    {u"tiff", Format::Tiff},
}};

std::optional<Format> formatForPath(QStringView path)
{
    const qsizetype slash = std::max(path.lastIndexOf(u'\\'), path.lastIndexOf(u'/'));
    const qsizetype dot = path.lastIndexOf(u'.');
    if (dot <= slash || dot + 1 >= path.size())
        return std::nullopt;
    const QStringView ext = path.sliced(dot + 1);
    for (const auto& [name, fmt] : kWritableFormats) {
        if (ext.compare(name, Qt::CaseInsensitive) == 0)
            return fmt;
    }
    return std::nullopt;
}

QImage flattenOpaque(const QImage& image)
{
    if (!image.hasAlphaChannel())
        return image.format() == QImage::Format_RGB32 ? image : image.convertToFormat(QImage::Format_RGB32);
    QImage flat(image.size(), QImage::Format_RGB32);
    flat.fill(Qt::white);
    QPainter painter(&flat);
    painter.drawImage(0, 0, image);
    painter.end();
    return flat;
}

bool encodeWithWic(const QImage& image, const QString& filePath, Format format)
{
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    const auto uninit = qScopeGuard([com] {
        if (SUCCEEDED(com))
            ::CoUninitialize();
    });
    if (FAILED(com) && com != RPC_E_CHANGED_MODE)
        return false;

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return false;

    const std::wstring native = QDir::toNativeSeparators(filePath).toStdWString();
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(native.c_str(), GENERIC_WRITE)))
        return false;

    const GUID container = format == Format::Jpeg ? GUID_ContainerFormatJpeg
        : format == Format::Bmp                   ? GUID_ContainerFormatBmp
        : format == Format::Tiff                  ? GUID_ContainerFormatTiff
                                                  : GUID_ContainerFormatPng;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(container, nullptr, &encoder))
        || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props)))
        return false;

    if (format == Format::Jpeg && props) {
        PROPBAG2 option {};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value;
        ::VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = 0.92f;
        props->Write(1, &option, &value);
        ::VariantClear(&value);
    }

    if (FAILED(frame->Initialize(props.Get())))
        return false;

    const bool keepAlpha = (format == Format::Png || format == Format::Tiff) && image.hasAlphaChannel();
    QImage pixels;
    WICPixelFormatGUID sourceFormat {};
    if (keepAlpha) {
        pixels = image.convertToFormat(QImage::Format_ARGB32);
        sourceFormat = GUID_WICPixelFormat32bppBGRA;
    } else {
        // WIC JPEG/BMP encoders expect 24-bit BGR; convert from flattened RGB32 to BGR888.
        pixels = flattenOpaque(image).convertToFormat(QImage::Format_BGR888);
        sourceFormat = GUID_WICPixelFormat24bppBGR;
    }
    if (pixels.isNull())
        return false;

    const UINT w = static_cast<UINT>(pixels.width());
    const UINT h = static_cast<UINT>(pixels.height());
    if (FAILED(frame->SetSize(w, h)))
        return false;

    WICPixelFormatGUID targetFormat = sourceFormat;
    if (FAILED(frame->SetPixelFormat(&targetFormat)))
        return false;

    if (targetFormat == sourceFormat) {
        if (FAILED(frame->WritePixels(h, static_cast<UINT>(pixels.bytesPerLine()),
                static_cast<UINT>(pixels.sizeInBytes()), const_cast<BYTE*>(pixels.constBits()))))
            return false;
    } else {
        ComPtr<IWICBitmap> bitmap;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wic->CreateBitmapFromMemory(w, h, sourceFormat, static_cast<UINT>(pixels.bytesPerLine()),
                static_cast<UINT>(pixels.sizeInBytes()), const_cast<BYTE*>(pixels.constBits()), &bitmap))
            || FAILED(wic->CreateFormatConverter(&converter))
            || FAILED(converter->Initialize(bitmap.Get(), targetFormat, WICBitmapDitherTypeNone, nullptr, 0,
                WICBitmapPaletteTypeMedianCut))
            || FAILED(frame->WriteSource(converter.Get(), nullptr)))
            return false;
    }

    return SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

bool isInsideDirectory(const QString& path, const QString& dir)
{
    if (dir.isEmpty())
        return false;
    const QString cleanPath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QString cleanDir = QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
    if (!cleanDir.endsWith(u'/'))
        cleanDir += u'/';
    return cleanPath.startsWith(cleanDir, Qt::CaseInsensitive);
}

QString winErrorMessage(const QString& targetPath, DWORD code)
{
    if (code == ERROR_ACCESS_DENIED || code == ERROR_WRITE_PROTECT)
        return QCoreApplication::translate("ws::capture::PictureWrite", "无法保存到 %1（文件只读或没有写入权限）").arg(targetPath);
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION)
        return QCoreApplication::translate("ws::capture::PictureWrite", "无法保存到 %1（文件正被其他程序占用）").arg(targetPath);
    if (code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL)
        return QCoreApplication::translate("ws::capture::PictureWrite", "无法保存到 %1（磁盘空间不足）").arg(targetPath);
    return QCoreApplication::translate("ws::capture::PictureWrite", "无法保存到 %1").arg(targetPath);
}

} // namespace

bool isWritableImageExtension(QStringView path)
{
    return formatForPath(path).has_value();
}

bool isWritableSourceFile(const QString& path, const QString& internalDataDir)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty() || !isWritableImageExtension(trimmed))
        return false;
    const QFileInfo info(trimmed);
    if (info.fileName().isEmpty() || info.isDir())
        return false;
    if (!internalDataDir.isEmpty()) {
        if (isInsideDirectory(trimmed, internalDataDir))
            return false;
        // Also exclude the host WinShun profile directory (two levels above plugins/<id>.data)
        // so clipboard cache images inside %APPDATA%\WinShun\images are never treated as user files.
        const QDir dataDir(internalDataDir);
        if (QFileInfo(internalDataDir).fileName().compare(u"winshun.capture.data"_s, Qt::CaseInsensitive) == 0) {
            const QString hostProfile = QDir::cleanPath(dataDir.absoluteFilePath(u"../.."_s));
            if (isInsideDirectory(trimmed, hostProfile))
                return false;
        }
    }
    return true;
}

QString writeImageFile(const QImage& image, const QString& targetPath)
{
    const QString nativeTarget = QDir::toNativeSeparators(targetPath.trimmed());
    if (image.isNull() || nativeTarget.isEmpty())
        return QCoreApplication::translate("ws::capture::PictureWrite", "无法保存到 %1").arg(nativeTarget);

    const QFileInfo info(nativeTarget);
    const QDir dir = info.dir();
    if (!dir.exists())
        return QCoreApplication::translate("ws::capture::PictureWrite", "无法保存到 %1（目录不存在）").arg(nativeTarget);

    const Format format = formatForPath(nativeTarget).value_or(Format::Png);
    const QString ext = info.suffix().isEmpty() ? u"png"_s : info.suffix();
    const QString tempPath = QDir::toNativeSeparators(
        dir.filePath(u".winshun-save-%1.%2"_s.arg(QUuid::createUuid().toString(QUuid::WithoutBraces), ext)));

    auto removeTemp = qScopeGuard([&] { QFile::remove(tempPath); });

    bool encoded = false;
    if (format == Format::Png) {
        QFile out(tempPath);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QImageWriter writer(&out, "png");
            encoded = writer.write(image) && out.flush();
            out.close();
        }
    } else {
        encoded = encodeWithWic(image, tempPath, format);
    }

    if (!encoded) {
        const DWORD err = ::GetLastError();
        return winErrorMessage(nativeTarget, err);
    }

    const std::wstring wTemp = tempPath.toStdWString();
    const std::wstring wTarget = nativeTarget.toStdWString();
    bool replaced = false;
    DWORD lastError = 0;
    if (::GetFileAttributesW(wTarget.c_str()) != INVALID_FILE_ATTRIBUTES) {
        replaced = ::ReplaceFileW(wTarget.c_str(), wTemp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != FALSE;
        if (!replaced)
            lastError = ::GetLastError();
    }
    if (!replaced) {
        replaced = ::MoveFileExW(wTemp.c_str(), wTarget.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!replaced)
            lastError = ::GetLastError();
    }
    if (!replaced)
        return winErrorMessage(nativeTarget, lastError);

    removeTemp.dismiss();
    return {};
}

QString shortFileName(const QString& path, qsizetype maxChars)
{
    const QString name = QFileInfo(path).fileName();
    if (name.size() <= maxChars || maxChars < 8)
        return name;
    const qsizetype dot = name.lastIndexOf(u'.');
    const QString ext = (dot > 0 && name.size() - dot <= 8) ? name.mid(dot) : QString();
    const QString stem = ext.isEmpty() ? name : name.left(dot);
    const qsizetype budget = maxChars - ext.size() - 1;
    if (budget <= 4)
        return name.left(maxChars - 1) + u'…';
    const qsizetype head = (budget + 1) / 2;
    const qsizetype tail = budget - head;
    return stem.left(head) + u'…' + stem.right(tail) + ext;
}

} // namespace ws::capture
