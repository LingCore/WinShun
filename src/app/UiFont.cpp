#include "UiFont.h"

#include "FontFile.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QDebug>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGuiApplication>
#include <QHash>
#include <QJsonArray>
#include <QRawFont>

#include <algorithm>
#include <array>
#include <optional>

#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>

using namespace Qt::StringLiterals;

namespace ws::uifont {
namespace {
std::optional<QFont> defaultFont;
QJsonObject loaded;

// The installed outline fonts Windows has under `face`, or all of them for an
// empty one, by the names it lists them under for this user: "霞鹜文楷" on a
// Chinese system, also when asked for "LXGW WenKai". Windows is asked, not
// QFontDatabase::families(): that list has a family under one of its names or
// the other, or not at all, by the font engine and by what was drawn so far
// (GitHub #325: "LXGW WenKai", chosen from it, was not in it at the next
// start, and the default font came back). No vertical ("@") faces, no bitmap
// fonts (the FreeType engine draws outlines).
QStringList installed(const QString& face = {})
{
    LOGFONTW ask {};
    ask.lfCharSet = DEFAULT_CHARSET;
    if (face.size() >= LF_FACESIZE)
        return {}; // longer than a face name can be
    face.toWCharArray(ask.lfFaceName);
    QStringList names;
    const HDC screen = ::GetDC(nullptr);
    ::EnumFontFamiliesExW(
        screen, &ask,
        [](const LOGFONTW* font, const TEXTMETRICW*, DWORD type, LPARAM out) -> int {
            const QString name = QString::fromWCharArray(font->lfFaceName);
            auto& names = *reinterpret_cast<QStringList*>(out);
            if ((type & TRUETYPE_FONTTYPE) && !name.startsWith(u'@') && !names.contains(name))
                names.append(name); // once, not for each of its scripts
            return 1;
        },
        reinterpret_cast<LPARAM>(&names), 0);
    ::ReleaseDC(nullptr, screen);
    return names;
}

// A font file among the application's fonts: by its path, or by its bytes
// where FreeType could not open that path (FontFile.h).
int addFile(const QString& path)
{
    const bool ascii = std::all_of(path.cbegin(), path.cend(), [](QChar c) { return c.unicode() < 128; });
    return ascii ? QFontDatabase::addApplicationFont(path) : addFontFromFile(path);
}

// The files Windows draws the face `listed` from, upright: its regular and
// its bold. DirectWrite knows the installed fonts by the names GDI has for
// them and where each one's file is, installed for all users or for this one.
QStringList filesOf(const QString& listed)
{
    using Microsoft::WRL::ComPtr;
    QStringList files;
    ComPtr<IDWriteFactory> factory;
    ComPtr<IDWriteGdiInterop> gdi;
    if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &factory))
        || FAILED(factory->GetGdiInterop(&gdi)))
        return files;
    for (const LONG weight : {FW_NORMAL, FW_BOLD}) {
        LOGFONTW ask {};
        ask.lfCharSet = DEFAULT_CHARSET;
        ask.lfWeight = weight;
        listed.toWCharArray(ask.lfFaceName);
        ComPtr<IDWriteFont> font;
        ComPtr<IDWriteFontFace> face;
        ComPtr<IDWriteFontFile> file;
        UINT32 count = 1; // more than one file: no TrueType font
        const void* key = nullptr;
        UINT32 keySize = 0;
        ComPtr<IDWriteFontFileLoader> loader;
        ComPtr<IDWriteLocalFontFileLoader> local;
        std::array<wchar_t, MAX_PATH> path {};
        if (FAILED(gdi->CreateFontFromLOGFONT(&ask, &font)) || !font || FAILED(font->CreateFontFace(&face))
            || FAILED(face->GetFiles(&count, &file)) || !file || FAILED(file->GetReferenceKey(&key, &keySize))
            || FAILED(file->GetLoader(&loader)) || FAILED(loader.As(&local))
            || FAILED(local->GetFilePathFromKey(key, keySize, path.data(), static_cast<UINT32>(path.size()))))
            continue;
        if (const QString name = QString::fromWCharArray(path.data()); !files.contains(name, Qt::CaseInsensitive))
            files.append(name);
    }
    return files;
}

// The face `listed` at its regular weight, selected for GDI to draw with.
class GdiFace {
public:
    explicit GdiFace(const QString& listed)
        : m_dc(::CreateCompatibleDC(nullptr))
    {
        LOGFONTW ask {};
        ask.lfCharSet = DEFAULT_CHARSET;
        ask.lfWeight = FW_NORMAL;
        listed.toWCharArray(ask.lfFaceName);
        m_font = ::CreateFontIndirectW(&ask);
        if (!m_dc || !m_font)
            return;
        m_before = ::SelectObject(m_dc, m_font);
        // The mapper gives some font for any name: only the one asked for.
        std::array<wchar_t, LF_FACESIZE> drawn {};
        ::GetTextFaceW(m_dc, static_cast<int>(drawn.size()), drawn.data());
        m_found = listed.compare(QString::fromWCharArray(drawn.data()), Qt::CaseInsensitive) == 0;
    }
    ~GdiFace()
    {
        if (m_before)
            ::SelectObject(m_dc, m_before);
        if (m_font)
            ::DeleteObject(m_font);
        if (m_dc)
            ::DeleteDC(m_dc);
    }
    GdiFace(const GdiFace&) = delete;
    GdiFace& operator=(const GdiFace&) = delete;

    // How heavy it is: 300 for a "Light" that Windows lists as a family of its own; 0 when not told.
    int weight() const
    {
        TEXTMETRICW metrics {};
        return m_found && ::GetTextMetricsW(m_dc, &metrics) ? static_cast<int>(metrics.tmWeight) : 0;
    }
    // Its font file as GDI has it, a collection whole: for a font DirectWrite
    // has no file of (one a font manager switched on for the session without
    // installing it).
    QByteArray bytes() const
    {
        QByteArray bytes;
        if (!m_found)
            return bytes;
        constexpr DWORD kCollection = 0x66637474; // 'ttcf'
        DWORD table = kCollection;
        DWORD size = ::GetFontData(m_dc, table, 0, nullptr, 0);
        if (size == GDI_ERROR) {
            table = 0; // a single font: the file from its start
            size = ::GetFontData(m_dc, table, 0, nullptr, 0);
        }
        if (size == GDI_ERROR || size == 0)
            return bytes;
        bytes.resize(size);
        if (::GetFontData(m_dc, table, 0, bytes.data(), size) != size)
            bytes.clear();
        return bytes;
    }

private:
    HDC m_dc = nullptr;
    HFONT m_font = nullptr;
    HGDIOBJ m_before = nullptr;
    bool m_found = false;
};

// Whether Qt, asked for the face by the name Windows lists it under, draws
// that font and not a fallback. A GDI face name ends after 31 characters.
bool drawsIt(const QString& listed)
{
    QFont font(listed);
    font.setPixelSize(16);
    return QFontInfo(font).exactMatch() || QRawFont::fromFont(font).familyName().startsWith(listed, Qt::CaseInsensitive);
}

// What to ask Qt for to have the installed face `listed`.
struct Drawn {
    QString family;
    int weight = 0; // 0: as the interface's font has it
};

// The same name, as a rule. Qt's own list of the installed fonts goes by
// their names in the registry, though, and leaves out a font whose name there
// it cannot match to what Windows calls it. 霞鹜文楷's regular has a Chinese
// family name and only an English full name: Qt has no "霞鹜文楷" at all, and
// its "LXGW WenKai" is the light weight (GitHub #325: chosen, the interface
// came up in a fallback font). Such a font is added from its own file; Qt
// then has it under the family its file names, with the other weights of
// that family it knew before, so its own weight is asked for with it.
Drawn drawnAs(const QString& listed)
{
    static QHash<QString, Drawn> known; // the interface's thread only
    if (const auto found = known.constFind(listed); found != known.cend())
        return *found;
    Drawn drawn {listed, 0};
    if (!drawsIt(listed)) {
        const GdiFace face(listed);
        const int weight = face.weight();
        QStringList families;
        const QStringList files = filesOf(listed);
        for (const QString& file : files)
            families += QFontDatabase::applicationFontFamilies(addFile(file));
        if (families.isEmpty()) {
            if (const QByteArray bytes = face.bytes(); !bytes.isEmpty())
                families = QFontDatabase::applicationFontFamilies(QFontDatabase::addApplicationFontFromData(bytes));
        }
        if (families.isEmpty()) {
            qWarning().noquote() << u"UI font: %1 is installed, but Qt does not draw it, and its file could not be read"_s.arg(listed);
        } else {
            // A collection has several families: the one of that name, else its first.
            const auto same
                = std::ranges::find_if(families, [&](const QString& f) { return f.compare(listed, Qt::CaseInsensitive) == 0; });
            drawn.family = same != families.cend() ? *same : families.first();
            if (weight >= QFont::Thin && weight <= QFont::Black && weight != QFont::Normal)
                drawn.weight = weight;
            qInfo().noquote() << u"UI font: Qt does not draw %1 by that name: added from %2 as %3, weight %4"_s
                                     .arg(listed, files.isEmpty() ? u"its bytes"_s : u"%1 file(s)"_s.arg(files.size()), drawn.family)
                                     .arg(weight);
        }
    }
    known.insert(listed, drawn);
    return drawn;
}
}

QJsonObject initialize()
{
    if (defaultFont)
        return loaded;
    const QString folder = QCoreApplication::applicationDirPath() + u"/fonts/"_s;
    const QString file = u"AlibabaPuHuiTi-3-55-Regular.ttf"_s;
    const bool ascii = std::all_of(folder.cbegin(), folder.cend(), [](QChar c) { return c.unicode() < 128; });
    int id = -1;
    if (ascii) {
        id = QFontDatabase::addApplicationFont(folder + file);
    } else {
        // FreeType opens paths through the ANSI C runtime. Data also works
        // when the portable program lives in a Chinese-named directory.
        id = addFontFromFile(folder + file);
    }
    const auto bundled = QFontDatabase::applicationFontFamilies(id);
    defaultFont = bundled.isEmpty() ? QGuiApplication::font() : QFont(bundled.first());
    defaultFont->setHintingPreference(QFont::PreferVerticalHinting);
    if (bundled.isEmpty())
        qWarning("UI font: nothing loaded from %s, using the system font", qUtf8Printable(QDir::toNativeSeparators(folder)));
    QGuiApplication::setFont(*defaultFont);
    loaded = {{u"by"_s, ascii ? u"file"_s : u"data"_s},
        {u"files"_s, QJsonArray {QJsonObject {{u"file"_s, file}, {u"loaded"_s, id >= 0}}}},
        {u"family"_s, defaultFont->family()}};
    return loaded;
}

QStringList families()
{
    QStringList result = installed();
    QCollator order; // the user's own: Chinese names by pinyin on a Chinese system
    order.setCaseSensitivity(Qt::CaseInsensitive);
    order.setNumericMode(true);
    std::ranges::sort(result, [&order](const QString& a, const QString& b) { return order.compare(a, b) < 0; });
    // Readable Windows CJK faces stay easy to find among large font collections.
    for (const QString& family : {u"Microsoft YaHei"_s, u"微软雅黑"_s, u"Microsoft YaHei UI"_s}) {
        if (result.removeOne(family))
            result.prepend(family);
    }
    return result;
}

QString listedFamily(const QString& family)
{
    if (family.isEmpty())
        return {};
    const QStringList names = installed(family);
    // Its own spelling when Windows lists it so too (a font named in two
    // languages at once), else the name the list has it under.
    for (const QString& name : names) {
        if (name.compare(family, Qt::CaseInsensitive) == 0)
            return name;
    }
    return names.value(0);
}

bool available(const QString& family)
{
    return family.isEmpty() || !listedFamily(family).isEmpty();
}

QString resolvedFamily(const QString& family)
{
    initialize();
    const QString listed = listedFamily(family);
    return listed.isEmpty() ? defaultFont->family() : drawnAs(listed).family;
}

void apply(const QString& family)
{
    initialize();
    QFont font = *defaultFont;
    const QFont current = QGuiApplication::font();
    font.setStyleStrategy(current.styleStrategy()); // retain the renderer's antialiasing policy
    // Whichever of its names was saved: the font Windows lists under it, by
    // the name Qt draws that font under.
    if (const QString listed = listedFamily(family); !listed.isEmpty()) {
        const Drawn drawn = drawnAs(listed);
        font.setFamily(drawn.family);
        if (drawn.weight > 0)
            font.setWeight(static_cast<QFont::Weight>(drawn.weight));
    }
    if (current != font)
        QGuiApplication::setFont(font);
}

} // namespace ws::uifont
