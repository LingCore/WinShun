#include "UiFont.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonArray>

#include <algorithm>
#include <optional>

#include <windows.h>

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
        QFile data(folder + file);
        if (data.open(QIODevice::ReadOnly))
            id = QFontDatabase::addApplicationFontFromData(data.readAll());
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
    return listed.isEmpty() ? defaultFont->family() : listed;
}

void apply(const QString& family)
{
    initialize();
    QFont font = *defaultFont;
    const QFont current = QGuiApplication::font();
    font.setStyleStrategy(current.styleStrategy()); // retain the renderer's antialiasing policy
    // By the name Windows lists it under: the one Qt has it under with either
    // font engine, whichever of its names was saved.
    if (const QString listed = listedFamily(family); !listed.isEmpty())
        font.setFamily(listed);
    if (current != font)
        QGuiApplication::setFont(font);
}

} // namespace ws::uifont
