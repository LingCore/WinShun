#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <windows.h>

namespace ws::hotkeys {

using namespace Qt::StringLiterals;

// "Alt+Space", "Ctrl+Shift+F" as RegisterHotKey takes it (MOD_NOREPEAT
// always): Win顺's own hotkeys (MessageWindow) and the screen capture
// plugin's (src/capture-plugin), which records them the same way.
inline bool parse(const QString& shortcut, UINT* modifiers, UINT* vk)
{
    *modifiers = MOD_NOREPEAT;
    *vk = 0;
    const QStringList parts = shortcut.split(u'+', Qt::SkipEmptyParts);
    for (QString part : parts) {
        part = part.trimmed();
        const QString key = part.toLower();
        if (key == u"ctrl" || key == u"control")
            *modifiers |= MOD_CONTROL;
        else if (key == u"alt")
            *modifiers |= MOD_ALT;
        else if (key == u"shift")
            *modifiers |= MOD_SHIFT;
        else if (key == u"win" || key == u"meta")
            *modifiers |= MOD_WIN;
        else if (key == u"space")
            *vk = VK_SPACE;
        else if (key == u"tab")
            *vk = VK_TAB;
        else if (key == u"enter" || key == u"return")
            *vk = VK_RETURN;
        else if (key == u"`" || key == u"backquote")
            *vk = VK_OEM_3;
        else if (key == u"printscreen" || key == u"print" || key == u"prtsc")
            *vk = VK_SNAPSHOT; // the screen capture's, once taken over from the Snipping Tool (printscreen::)
        else if (key.size() == 1 && key[0].isLetterOrNumber() && key[0].unicode() < 128)
            *vk = part.toUpper()[0].unicode();
        else if (key.startsWith(u'f') && key.size() <= 3) {
            bool ok = false;
            const int n = key.mid(1).toInt(&ok);
            if (!ok || n < 1 || n > 24)
                return false;
            *vk = VK_F1 + n - 1;
        } else {
            return false;
        }
    }
    return *vk != 0;
}

// The same in one spelling ("win+ctrl+alt+shift+key", lower case), or empty
// when it names no key parse() knows or two keys.
inline QString normalized(const QString& shortcut)
{
    bool win = false;
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    QString key;
    for (const QString& part : shortcut.split(u'+', Qt::SkipEmptyParts)) {
        const QString name = part.trimmed().toLower();
        if (name == u"ctrl" || name == u"control")
            ctrl = true;
        else if (name == u"alt")
            alt = true;
        else if (name == u"shift")
            shift = true;
        else if (name == u"win" || name == u"meta")
            win = true;
        else if (!key.isEmpty() || name.isEmpty())
            return {}; // two keys
        else if (name == u"return")
            key = u"enter"_s;
        else if (name == u"print" || name == u"prtsc")
            key = u"printscreen"_s;
        else if (name == u"backquote")
            key = u"`"_s;
        else
            key = name;
    }
    // The keys parse() knows.
    static const QRegularExpression knownKey(u"^([a-z0-9]|f([1-9]|1\\d|2[0-4])|space|tab|enter|printscreen|`)$"_s);
    if (!knownKey.match(key).hasMatch())
        return {};
    QStringList parts;
    if (win)
        parts.push_back(u"win"_s);
    if (ctrl)
        parts.push_back(u"ctrl"_s);
    if (alt)
        parts.push_back(u"alt"_s);
    if (shift)
        parts.push_back(u"shift"_s);
    parts.push_back(key);
    return parts.join(u'+');
}

// What may be a hotkey (Settings::hotkeyAllowed: the settings window's
// recorder and AI): a function key alone, or a key with Ctrl, Alt or Win.
inline bool allowed(const QString& shortcut)
{
    const QString normal = normalized(shortcut);
    if (normal.isEmpty())
        return false;
    const QStringList parts = normal.split(u'+');
    static const QRegularExpression functionKey(u"^f([1-9]|1\\d|2[0-4])$"_s);
    if (functionKey.match(parts.last()).hasMatch())
        return true;
    return parts.contains(u"ctrl") || parts.contains(u"alt") || parts.contains(u"win");
}

} // namespace ws::hotkeys
