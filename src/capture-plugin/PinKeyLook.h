#pragma once

#include "PinKeyCard.h"
#include "SystemTheme.h"

#include <QColor>
#include <QObject>
#include <QString>

namespace ws::captureplugin {

// A pin's key card (GitHub #343) in the colours of the capture's card
// (CaptureKeyHints.qml): the style's floating panel and its key hints.
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
