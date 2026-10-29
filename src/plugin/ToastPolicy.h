#pragma once

// A plugin's system notification as Win顺 shows it (GitHub #113). Its title
// begins with the plugin's name, which the plugin cannot leave out or
// disguise, so it never passes for one of Win顺's own. A click on it may do
// only what cannot surprise: open the plugin's own list, open a plain local
// file or folder (the open policy still holds when it is done), and copy,
// with the clipboard:write permission. Pure, for tst_plugin.

#include "Items.h"

#include <QString>

#include <functional>

namespace ws::plugin {

// A tray notification's title holds 63 characters (NOTIFYICONDATAW's
// szInfoTitle, 64 with its end): with the name and the "…".
inline constexpr qsizetype kToastTitleMost = 63;
inline constexpr qsizetype kToastNameMost = 24; // of those, the name (with 插件「」) at most
inline constexpr qsizetype kToastClickMost = 8; // of what a click asks for, the first ones are read

// Whose plugin: one that came with Win顺, or one the user put in.
enum class ToastFrom { Bundled, User };

// The name a notification shows for its plugin, cleaned (toastWords) and
// cut with "…": as it is for one that came with Win顺 ("局域网传输"); in
// 插件「…」 (Plugin “…”) for one the user put in, whatever it calls itself
// ("Win顺", letters that look like it), so it never passes for Win顺's
// own. Its id in 插件「…」 when nothing of its name can be seen (only
// spaces, fillers, marks).
QString toastName(ToastFrom from, const QString& pluginId, const QString& pluginName, const QString& locale);

// "<name>：<title>" ("<name>: <title>" in English), cut to kToastTitleMost
// with "…" (a pair of surrogates kept whole); the name alone when there is
// no title.
QString toastTitle(ToastFrom from, const QString& pluginId, const QString& pluginName, const QString& title, const QString& locale);

// The words of a notification without the characters that do not show:
// controls; the format ones (the marks, embeddings, overrides and isolates
// that turn the direction of the words, which could make the name read as
// something else; zero width spaces, tags) but for the joiners an emoji or
// a script needs; blank letters and marks (the Hangul fillers, the braille
// blank, variation selectors). On one line, or (`oneLine` false, its text)
// keeping its line breaks.
QString toastWords(const QString& text, bool oneLine = true);

// A drive letter's absolute path, no stream after it: "C:\…", "D:/…".
// Never a web link, a share (\\server\…, \\?\…), a shell: name or a
// relative name a search could turn into a program. (A network drive with
// a letter is a drive letter's path too.)
bool plainLocalPath(QStringView path);

// What a click on a plugin's notification may do, of what it asked, in its
// order, one of each kind at most: setQuery when `ownQuery` says the text
// opens one of this plugin's commands; open of a plainLocalPath() with no
// arguments or working folder; copy with `mayCopy` (clipboard:write).
// Everything else is dropped (web links, notify, hide, keep, replace).
Effects toastClick(const Effects& asked, const std::function<bool(const QString& query)>& ownQuery, bool mayCopy);

} // namespace ws::plugin
