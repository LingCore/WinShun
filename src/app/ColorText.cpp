#include "ColorText.h"

#include <QRegularExpression>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace ws::colortext {

std::optional<QColor> parse(QStringView text)
{
    QStringView t = text.trimmed();
    if (t.endsWith(u';'))
        t = t.chopped(1).trimmed();
    if (t.size() > kMaxLength)
        return std::nullopt;
    const QString s = t.toString();
    // Alpha after a comma or a slash: 0.5 or 50%.
    const auto alphaOf = [](const QRegularExpressionMatch& m, int number) {
        if (!m.hasCaptured(number))
            return 1.0;
        const double value = m.captured(number).toDouble();
        return std::clamp(m.captured(number + 1).isEmpty() ? value : value / 100, 0.0, 1.0);
    };
    static const QRegularExpression hex(u"^#([0-9a-fA-F]{3,4}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$"_s);
    if (const QRegularExpressionMatch m = hex.match(s); m.hasMatch()) {
        QString digits = m.captured(1);
        if (digits.size() <= 4) { // #rgb, #rgba: each digit twice
            QString longer;
            for (const QChar c : digits)
                longer += QString(2, c);
            digits = longer;
        }
        // CSS order, alpha last (Qt's own #aarrggbb has it first).
        const auto channel = [&](int i) { return digits.mid(i * 2, 2).toInt(nullptr, 16); };
        return QColor(channel(0), channel(1), channel(2), digits.size() == 8 ? channel(3) : 255);
    }
    // rgb(255, 136, 0), rgba(255, 136, 0, 0.5), rgb(255 136 0 / 50%), rgb(100% 53% 0%)
    static const QRegularExpression rgb(
        uR"(^rgba?\(\s*(\d*\.?\d+)(%?)(?:\s*,\s*|\s+)(\d*\.?\d+)(%?)(?:\s*,\s*|\s+)(\d*\.?\d+)(%?)\s*(?:[,/]\s*(\d*\.?\d+)(%?)\s*)?\)$)"_s,
        QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch m = rgb.match(s); m.hasMatch()) {
        int channels[3];
        for (int i = 0; i < 3; ++i) {
            const double value = m.captured(1 + i * 2).toDouble();
            const double scaled = m.captured(2 + i * 2).isEmpty() ? value : value * 255 / 100;
            if (scaled > 255)
                return std::nullopt;
            channels[i] = qRound(scaled);
        }
        QColor color(channels[0], channels[1], channels[2]);
        color.setAlphaF(static_cast<float>(alphaOf(m, 7)));
        return color;
    }
    // hsl(32, 100%, 50%), hsla(32deg 100% 50% / 0.5)
    static const QRegularExpression hsl(
        uR"(^hsla?\(\s*(-?\d*\.?\d+)(?:deg)?(?:\s*,\s*|\s+)(\d*\.?\d+)%(?:\s*,\s*|\s+)(\d*\.?\d+)%\s*(?:[,/]\s*(\d*\.?\d+)(%?)\s*)?\)$)"_s,
        QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch m = hsl.match(s); m.hasMatch()) {
        const double hue = std::fmod(std::fmod(m.captured(1).toDouble(), 360.0) + 360.0, 360.0);
        const double saturation = m.captured(2).toDouble() / 100;
        const double lightness = m.captured(3).toDouble() / 100;
        if (saturation > 1 || lightness > 1)
            return std::nullopt;
        return QColor::fromHslF(static_cast<float>(hue / 360), static_cast<float>(saturation),
            static_cast<float>(lightness), static_cast<float>(alphaOf(m, 4)));
    }
    return std::nullopt;
}

std::vector<Notation> notations(const QColor& color)
{
    const bool alpha = color.alpha() < 255;
    const QString a = QString::number(color.alphaF(), 'g', 2);
    const QColor rgb = color.toRgb();
    QString hex = rgb.name().toUpper();
    if (alpha)
        hex += u"%1"_s.arg(rgb.alpha(), 2, 16, u'0').toUpper();
    const QString channels = u"%1, %2, %3"_s.arg(rgb.red()).arg(rgb.green()).arg(rgb.blue());
    const QColor hsl = color.toHsl();
    const QString hslChannels = u"%1, %2%, %3%"_s.arg(std::max(0, hsl.hslHue()))
                                    .arg(qRound(hsl.hslSaturationF() * 100))
                                    .arg(qRound(hsl.lightnessF() * 100));
    std::vector<Notation> list {{u"HEX"_s, hex}};
    if (alpha)
        list.push_back({u"ARGB"_s, rgb.name(QColor::HexArgb).toUpper(), false});
    list.push_back({u"RGB"_s, alpha ? u"rgba(%1, %2)"_s.arg(channels, a) : u"rgb(%1)"_s.arg(channels)});
    list.push_back({u"HSL"_s, alpha ? u"hsla(%1, %2)"_s.arg(hslChannels, a) : u"hsl(%1)"_s.arg(hslChannels)});
    return list;
}

} // namespace ws::colortext
