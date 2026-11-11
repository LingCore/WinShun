#include "Phosphor.h"

#include "PhosphorAliases.h"
#include "PhosphorIcons.generated.h"

#include <QByteArray>
#include <QFile>
#include <QFontDatabase>

#include <algorithm>
#include <array>
#include <string_view>

using namespace Qt::StringLiterals;

namespace ws::phosphor {

static_assert(std::ranges::is_sorted(kAliases, {}, &Alias::segoe));

namespace {

QString g_regular; // the families the fonts registered; empty if not loaded
QString g_fill;

QString load(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QFontDatabase::applicationFontFamilies(QFontDatabase::addApplicationFontFromData(file.readAll())).value(0);
}

// The generated table is sorted by name, which is plain Latin-1. One lookup
// path for both callers: a name from C++ and an alias' string_view.
const generated::Entry* find(std::string_view name)
{
    std::size_t low = 0;
    std::size_t high = generated::kNames.size();
    while (low < high) {
        const std::size_t mid = low + (high - low) / 2;
        if (generated::kNames[mid].name.compare(name) < 0)
            low = mid + 1;
        else
            high = mid;
    }
    if (low == generated::kNames.size() || generated::kNames[low].name != name)
        return nullptr;
    return &generated::kNames[low];
}

std::optional<Icon> make(const generated::Entry* entry, Weight weight)
{
    if (!entry)
        return std::nullopt;
    const QString& family = weight == Weight::Fill ? g_fill : g_regular;
    if (family.isEmpty())
        return std::nullopt;
    return Icon {QString(QChar(entry->code)), family};
}

} // namespace

void loadFonts()
{
    if (!g_regular.isEmpty())
        return;
    g_regular = load(u":/fonts/Phosphor.ttf"_s);
    g_fill = load(u":/fonts/Phosphor-Fill.ttf"_s);
    if (g_regular.isEmpty() || g_fill.isEmpty())
        qWarning("WinShun: the Phosphor icon fonts did not load; the system icon font draws the icons");
}

std::optional<Icon> forName(const QString& name, Weight weight)
{
    const QByteArray latin = name.toLatin1();
    return make(find(std::string_view(latin.constData(), static_cast<std::size_t>(latin.size()))), weight);
}

std::optional<Icon> resolve(const QString& spec)
{
    if (spec.startsWith(u"ph-fill:"_s))
        return forName(spec.mid(8), Weight::Fill);
    if (spec.startsWith(u"ph:"_s))
        return forName(spec.mid(3));
    if (spec.size() == 1)
        return forSegoe(spec);
    return std::nullopt;
}

std::optional<Icon> forSegoe(const QString& glyph)
{
    if (glyph.size() != 1)
        return std::nullopt;
    const char16_t code = glyph.front().unicode();
    const auto found = std::ranges::lower_bound(kAliases, code, {}, &Alias::segoe);
    if (found == kAliases.end() || found->segoe != code)
        return std::nullopt;
    return make(find(found->name), found->fill ? Weight::Fill : Weight::Regular);
}

QList<QChar> segoeCodes()
{
    QList<QChar> codes;
    for (const Alias& alias : kAliases)
        codes.append(QChar(alias.segoe));
    return codes;
}

} // namespace ws::phosphor
