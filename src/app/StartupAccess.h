#pragma once

#include "PathText.h"
#include <QStringList>
#include <QStringView>
#include <QSettings>

namespace ws::startupaccess {
// A cancelled UAC prompt falls back once, without changing the saved mode.
inline const QString unelevatedOnce = QStringLiteral("--startup-unelevated");

inline QString storedMode(const QString& fileName)
{
    // Read only the startup decision; normal initialization owns migrations
    // and the full settings load after the permission level is settled.
    return QSettings(fileName, QSettings::IniFormat).value(QStringLiteral("Index/Access"), QStringLiteral("auto"))
        .toString().trimmed().toLower();
}

inline bool needsElevation(QStringView mode, bool administrator, bool elevated, bool declined)
{
    return mode == u"direct" && administrator && !elevated && !declined;
}

inline QStringList fallbackArguments(QStringList arguments)
{
    arguments.append(unelevatedOnce);
    return arguments;
}

inline QString commandLine(const QStringList& arguments)
{
    QStringList quoted;
    quoted.reserve(arguments.size());
    for (const auto& argument : arguments)
        quoted.append(pathtext::commandArgument(argument));
    return quoted.join(u' ');
}
}
