#include "UiFont.h"

#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonArray>

#include <optional>
#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::uifont {
namespace {
std::optional<QFont> defaultFont;
QJsonObject loaded;
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
    auto result = QFontDatabase::families();
    result.removeIf([](const QString& family) {
        return family.startsWith(u'@') || QFontDatabase::isPrivateFamily(family);
    });
    // Readable Windows CJK faces stay easy to find among large font collections.
    for (const QString& family : {u"Microsoft YaHei"_s, u"Microsoft YaHei UI"_s}) {
        if (result.removeOne(family))
            result.prepend(family);
    }
    return result;
}

bool available(const QString& family)
{
    return family.isEmpty() || families().contains(family, Qt::CaseInsensitive);
}

QString resolvedFamily(const QString& family)
{
    initialize();
    return !family.isEmpty() && available(family) ? family : defaultFont->family();
}

void apply(const QString& family)
{
    initialize();
    QFont font = *defaultFont;
    const QFont current = QGuiApplication::font();
    font.setStyleStrategy(current.styleStrategy()); // retain the renderer's antialiasing policy
    if (!family.isEmpty() && (family == current.family() || available(family)))
        font.setFamily(family);
    if (current != font)
        QGuiApplication::setFont(font);
}

} // namespace ws::uifont
