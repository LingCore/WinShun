#pragma once

// A plugin's password settings (the plugin design, §4.7): kept by Windows'
// Credential Manager, never in WinShun.ini, as generic credentials named
// "WinShun/plugin/<id>/<key>". Read when the plugin is told its settings,
// removed with the plugin.

#include "Manifest.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>

namespace ws::plugin::secrets {

QString target(const QString& pluginId, const QString& key); // "WinShun/plugin/<id>/<key>"
bool write(const QString& pluginId, const QString& key, const QString& value); // an empty value removes it
std::optional<QString> read(const QString& pluginId, const QString& key); // nothing when there is none
bool remove(const QString& pluginId, const QString& key);
// Every secret of the plugin's, removed; how many there were.
int removeAll(const QString& pluginId);
QStringList keysOf(const QString& pluginId); // the keys with a secret kept

// A declared setting's value, kept where it belongs: a password in the
// Credential Manager, anything else among `iniValues` (the [Plugin.<id>]
// Setting.<key> lines, as text); a number with a range or a step fitted to
// them (Setting::fit), its line removed when it is no number (its default
// stands). True when `iniValues` changed.
bool store(const QString& pluginId, const Manifest& manifest, const QString& key, const QString& value,
    QList<std::pair<QString, QString>>& iniValues);
// winshun/settings/update (GitHub #332): the values a plugin sent for its own
// settings, as the settings file keeps them (key, text), for those that may
// be set this way: declared in its manifest; text, number, bool or choice (a
// password stays the user's to type, a folder, a file and a hotkey the
// user's to pick); of the JSON type the setting has; a choice one of its
// options; text no longer than kSettingTextMost. Anything else is left out.
// A number outside its range is taken, store() fits it.
QList<std::pair<QString, QString>> acceptedUpdate(const Manifest& manifest, const QJsonObject& values);
inline constexpr qsizetype kSettingTextMost = 4096;

// The plugin's settings as it reads them: the INI's text by the manifest's
// types (bool, number, text; a number with a range fitted to it, left out
// when it is no number), the passwords from the Credential Manager.
QJsonObject settingsOf(const QString& pluginId, const Manifest& manifest, const QList<std::pair<QString, QString>>& iniValues);

} // namespace ws::plugin::secrets
