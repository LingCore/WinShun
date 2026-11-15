// What Windows, Qt and DirectWrite each call an installed font (GitHub #325).
// fontprobe.exe <out.txt> [name ...]; run next to Qt's DLLs.
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QRawFont>
#include <QSettings>
#include <QTextStream>

#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

static QTextStream* out = nullptr;

static bool ours(const QString& name)
{
    return name.contains(QStringLiteral("LXGW"), Qt::CaseInsensitive) || name.contains(QChar(0x971E));
}

static QStringList filesOf(const QString& face, int weight)
{
    QStringList files;
    ComPtr<IDWriteFactory> factory;
    if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &factory)))
        return files;
    ComPtr<IDWriteGdiInterop> gdi;
    if (FAILED(factory->GetGdiInterop(&gdi)))
        return files;
    LOGFONTW logfont {};
    logfont.lfCharSet = DEFAULT_CHARSET;
    logfont.lfWeight = weight;
    face.toWCharArray(logfont.lfFaceName);
    ComPtr<IDWriteFont> font;
    if (FAILED(gdi->CreateFontFromLOGFONT(&logfont, &font)) || !font)
        return {QStringLiteral("<no font>")};
    ComPtr<IDWriteFontFace> fontFace;
    if (FAILED(font->CreateFontFace(&fontFace)))
        return files;
    UINT32 count = 0;
    fontFace->GetFiles(&count, nullptr);
    std::vector<ComPtr<IDWriteFontFile>> list(count);
    fontFace->GetFiles(&count, reinterpret_cast<IDWriteFontFile**>(list.data()));
    for (const auto& file : list) {
        const void* key = nullptr;
        UINT32 size = 0;
        ComPtr<IDWriteFontFileLoader> loader;
        ComPtr<IDWriteLocalFontFileLoader> local;
        if (FAILED(file->GetReferenceKey(&key, &size)) || FAILED(file->GetLoader(&loader)) || FAILED(loader.As(&local)))
            continue;
        wchar_t path[MAX_PATH] {};
        if (SUCCEEDED(local->GetFilePathFromKey(key, size, path, MAX_PATH)))
            files.append(QString::fromWCharArray(path) + QStringLiteral(" #%1 w%2").arg(fontFace->GetIndex()).arg(int(font->GetWeight())));
    }
    return files;
}

static void qtSays(const QString& name)
{
    QFont font(name);
    font.setPixelSize(16);
    const QFontInfo info(font);
    const QRawFont raw = QRawFont::fromFont(font);
    *out << "  QFont(" << name << "): info.family=" << info.family() << " exact=" << info.exactMatch()
         << " raw.family=" << raw.familyName() << " raw.style=" << raw.styleName() << " hasFamily=" << QFontDatabase::hasFamily(name)
         << " styles=" << QFontDatabase::styles(name).join(u'|') << "\n";
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QFile file(app.arguments().value(1, QStringLiteral("fontprobe.txt")));
    file.open(QIODevice::WriteOnly);
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    out = &stream;
    QStringList names = app.arguments().mid(2);
    if (names.isEmpty())
        names = {QStringLiteral("LXGW WenKai"), QString::fromUtf8("\xE9\x9C\x9E\xE9\xB9\x9C\xE6\x96\x87\xE6\xA5\xB7"),
            QStringLiteral("LXGW WenKai Light"), QString::fromUtf8("\xE9\x9C\x9E\xE9\xB9\x9C\xE6\x96\x87\xE6\xA5\xB7 Light")};

    stream << "platform " << qEnvironmentVariable("QT_QPA_PLATFORM") << " locale " << QLocale::system().name() << " ui "
           << QLocale::system().uiLanguages().join(u',') << " acp " << ::GetACP() << "\n";

    stream << "GDI:\n";
    LOGFONTW ask {};
    ask.lfCharSet = DEFAULT_CHARSET;
    const HDC screen = ::GetDC(nullptr);
    ::EnumFontFamiliesExW(
        screen, &ask,
        [](const LOGFONTW* font, const TEXTMETRICW*, DWORD type, LPARAM) -> int {
            const auto* full = reinterpret_cast<const ENUMLOGFONTEXW*>(font);
            const QString name = QString::fromWCharArray(font->lfFaceName);
            if (ours(name) || ours(QString::fromWCharArray(full->elfFullName)))
                *out << "  face=" << name << " full=" << QString::fromWCharArray(full->elfFullName) << " style="
                     << QString::fromWCharArray(full->elfStyle) << " weight=" << font->lfWeight << " charset=" << font->lfCharSet
                     << " type=" << type << "\n";
            return 1;
        },
        0, 0);
    ::ReleaseDC(nullptr, screen);

    stream << "Registry:\n";
    for (const QString& root : {QStringLiteral("HKEY_LOCAL_MACHINE"), QStringLiteral("HKEY_CURRENT_USER")}) {
        QSettings fonts(root + QStringLiteral("\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts"), QSettings::NativeFormat);
        for (const QString& key : fonts.allKeys()) {
            if (ours(key) || ours(fonts.value(key).toString()))
                stream << "  " << root.mid(5) << " " << key << " = " << fonts.value(key).toString() << "\n";
        }
    }

    stream << "DirectWrite files:\n";
    for (const QString& name : names)
        stream << "  " << name << ": 400 " << filesOf(name, 400).join(u';') << " | 700 " << filesOf(name, 700).join(u';') << "\n";

    stream << "Qt before anything is drawn:\n";
    for (const QString& name : names)
        qtSays(name);
    stream << "QFontDatabase::families():";
    for (const QString& family : QFontDatabase::families()) {
        if (ours(family))
            stream << " [" << family << "]";
    }
    stream << "\nQt after families():\n";
    for (const QString& name : names)
        qtSays(name);

    stream << "As an application font:\n";
    for (const QString& name : names) {
        const QStringList files = filesOf(name, 400);
        if (files.isEmpty() || files.first().startsWith(u'<'))
            continue;
        const QString path = files.first().section(QStringLiteral(" #"), 0, 0);
        const int id = QFontDatabase::addApplicationFont(path);
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        stream << "  " << name << " <- " << path << " id " << id << " families " << families.join(u'|') << "\n";
        for (const QString& family : families)
            qtSays(family);
        qtSays(name);
    }
    return 0;
}
