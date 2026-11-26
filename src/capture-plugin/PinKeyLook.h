#pragma once

#include "PinKeyCard.h"
#include "SystemTheme.h"

#include <QColor>
#include <QObject>
#include <QString>

namespace ws::captureplugin {

// A pin's key card (GitHub #343) in the colours and type of the capture's
// card (CaptureKeyHints.qml): the style's floating panel and its key hints,
// in a menu's type at the 界面大小.
// `theme`: the Theme singleton (its keyEdge is not in nativeLook);
// `rounded`: Windows' corners are round (the radii when there is no Theme).
// CaptureApp's pins and the previews.
inline capture::PinKeyCard::Look pinKeyCardLook(const QObject* theme, bool dark, bool rounded)
{
    capture::PinKeyCard::Look look = capture::PinKeyCard::Look::fallback(dark);
    look.background = SystemTheme::nativeColor("menuBackground", look.background);
    look.edge = SystemTheme::nativeColor("menuEdge", look.edge);
    look.text = SystemTheme::nativeColor("text", look.text);
    look.subtext = SystemTheme::nativeColor("subtext", look.subtext);
    look.faint = SystemTheme::nativeColor("faint", look.faint);
    look.divider = SystemTheme::nativeColor("divider", look.divider);
    if (theme) {
        const QColor keyEdge = theme->property("keyEdge").value<QColor>();
        if (keyEdge.isValid())
            look.keyEdge = keyEdge;
        // The type at the 界面大小, as the capture's card reads it.
        const auto size = [theme](const char* name, int fallback) {
            const int value = theme->property(name).toInt();
            return value > 0 ? value : fallback;
        };
        look.textSize = size("menuTextSize", look.textSize);
        look.captionSize = size("menuCaptionSize", look.captionSize);
        look.keyHeight = size("keycapHeight", look.keyHeight);
    }
    // A key hint's words, as the styles' HintCap: fluent's in the words'
    // colour, brand's in the second grey, tactile's in the faint one.
    const QString style = SystemTheme::nativeLook().value(QStringLiteral("style")).toString();
    look.keyText = style == QLatin1String("brand") ? look.subtext : style == QLatin1String("tactile") ? look.faint : look.text;
    look.radius = SystemTheme::nativeReal("menuRadius", rounded ? 8 : 0);
    look.keyRadius = SystemTheme::nativeReal("controlRadius", rounded ? 4 : 0);
    return look;
}

} // namespace ws::captureplugin
