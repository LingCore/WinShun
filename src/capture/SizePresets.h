#pragma once

#include <QSize>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace ws::capture {

// The sizes and shapes the capture's size panel keeps to pick from (GitHub
// #308): "1920x1080" is a size in screen pixels, "16:9" a shape the selection
// is held to. Pure text and numbers, so the settings read them without the
// capture (tst_capturesettings) and the rules are tested on their own
// (tst_capture).
struct SizePreset {
    static constexpr int kMost = 99999; // either number, from 1

    QSize size;
    bool ratio = false;

    bool valid() const { return size.width() > 0 && size.height() > 0; }
    // As the settings keep it, and as parseSizePreset reads it back.
    QString text() const
    {
        return QString::number(size.width()) + (ratio ? u':' : u'x') + QString::number(size.height());
    }
    friend bool operator==(const SizePreset&, const SizePreset&) = default;
};

// "1920x1080", "1920×1080", "1920*1080" and "16:9" (the full-width colon
// too), with spaces anywhere; not valid() otherwise.
inline SizePreset parseSizePreset(QStringView text)
{
    QString compact;
    for (const QChar c : text) {
        if (!c.isSpace())
            compact.append(c.toLower());
    }
    const auto mark = std::ranges::find_if(compact, [](QChar c) { return QStringView(u"x×*:：").contains(c); });
    if (mark == compact.begin() || mark == compact.end())
        return {};
    const qsizetype at = mark - compact.begin();
    bool okWidth = false;
    bool okHeight = false;
    const int width = QStringView(compact).left(at).toInt(&okWidth);
    const int height = QStringView(compact).mid(at + 1).toInt(&okHeight);
    if (!okWidth || !okHeight || width < 1 || height < 1 || width > SizePreset::kMost || height > SizePreset::kMost)
        return {};
    return {QSize(width, height), *mark == u':' || *mark == u'：'};
}

// How many the panel's list holds: it shows them all, without scrolling.
inline constexpr std::size_t kSizePresetLimit = 12;

// A comma separated list of them: those that read, once each, the first `limit`.
inline std::vector<SizePreset> parseSizePresets(QStringView text, std::size_t limit = kSizePresetLimit)
{
    std::vector<SizePreset> presets;
    for (const QStringView part : text.split(u',', Qt::SkipEmptyParts)) {
        const SizePreset preset = parseSizePreset(part);
        if (preset.valid() && presets.size() < limit && std::ranges::find(presets, preset) == presets.end())
            presets.push_back(preset);
    }
    return presets;
}

inline QString sizePresetsText(const std::vector<SizePreset>& presets)
{
    QStringList parts;
    for (const SizePreset& preset : presets)
        parts.append(preset.text());
    return parts.join(u',');
}

// What a new user starts with.
inline constexpr QStringView kDefaultSizePresets = u"1920x1080,1280x720,800x600,16:9,4:3,1:1";

} // namespace ws::capture
