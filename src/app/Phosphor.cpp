#include "Phosphor.h"

#include "PhosphorAliases.h"
#include "PhosphorIcons.generated.h"

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

// The generated table is sorted by name, which is plain Latin-1. Compare
// without allocating: a settings window resolves hundreds of icons.
int compareName(std::string_view a, std::string_view b)
{
    return a.compare(b);
}

int compareName(std::string_view a, const QString& b)
{
    const qsizetype asize = static_cast<qsizetype>(a.size());
    const qsizetype bsize = b.size();
    const qsizetype common = std::min(asize, bsize);
    for (qsizetype i = 0; i < common; ++i) {
        const int left = static_cast<unsigned char>(a[i]);
        const int right = b.at(i).unicode();
        if (left != right)
            return left < right ? -1 : 1;
    }
    return asize == bsize ? 0 : (asize < bsize ? -1 : 1);
}

template <typename Key>
const generated::Entry* find(const Key& name)
{
    std::size_t low = 0;
    std::size_t high = generated::kNames.size();
    while (low < high) {
        const std::size_t mid = low + (high - low) / 2;
        if (compareName(generated::kNames[mid].name, name) < 0)
            low = mid + 1;
        else
            high = mid;
    }
    if (low == generated::kNames.size() || compareName(generated::kNames[low].name, name) != 0)
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
    return make(find(name), weight);
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
