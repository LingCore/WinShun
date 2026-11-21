#pragma once

#include "DoubleTapAction.h"

#include <QString>
#include <QStringView>

namespace ws::trayaction {

// Actions a left or middle click on the notification-area icon can trigger.
// Built-ins cover the main Win顺 entry points; plugin actions reuse the
// authenticated `plugin:<id>/<registered key>` encoding from ws::doubletap.
inline constexpr QStringView kLauncher = u"launcher";
inline constexpr QStringView kPanel = u"panel";
inline constexpr QStringView kClipboard = u"clipboard";
inline constexpr QStringView kSettings = u"settings";
inline constexpr QStringView kScreenshots = u"screenshots";
inline constexpr QStringView kPauseClipboard = u"pause_clipboard";
inline constexpr QStringView kNone = u"none";

inline bool isBuiltIn(QStringView value)
{
    return value == kLauncher || value == kPanel || value == kClipboard || value == kSettings
        || value == kScreenshots || value == kPauseClipboard || value == kNone;
}

inline bool valid(QStringView value)
{
    return isBuiltIn(value) || !doubletap::decode(value).plugin.isEmpty();
}

} // namespace ws::trayaction
