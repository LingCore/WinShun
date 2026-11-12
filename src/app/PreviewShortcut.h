#pragma once

#include "platform/HotkeyText.h"

#include <QKeySequence>
#include <algorithm>
#include <initializer_list>

namespace ws::previewkeys {

// Local to the search window. Space is opt-in; Shift+Space stays available
// for entering a separator. Navigation and existing editing commands stay usable.
inline QString normalized(const QString& shortcut)
{
    const QString normal = hotkeys::normalized(shortcut);
    if (normal.isEmpty())
        return {};
    const QStringList parts = normal.split(u'+');
    const QString& key = parts.last();
    const bool ctrl = parts.contains(u"ctrl");
    const bool alt = parts.contains(u"alt");
    if (parts.contains(u"win") || key == u"tab" || key == u"enter" || key == u"printscreen")
        return {};
    if (key == u"space" && normal != u"space")
        return {};
    if (key != u"space" && !hotkeys::allowed(normal))
        return {};
    if (alt && key == u"f4")
        return {};
    if (normal == u"shift+f10")
        return {};
    if (ctrl && !alt && key.size() == 1 && QStringView(u"acdefrstvxyz").contains(key))
        return {};
    if (normal == u"ctrl+j" || normal == u"ctrl+k" || normal == u"ctrl+n" || normal == u"ctrl+p"
        || normal == u"ctrl+u" || normal == u"ctrl+,")
        return {};
    if ((ctrl || alt) && key.size() == 1 && key.front().isDigit())
        return {};
    const QKeySequence sequence = QKeySequence::fromString(normal, QKeySequence::PortableText);
    return sequence.count() == 1 ? sequence.toString(QKeySequence::PortableText) : QString();
}

inline bool allowed(const QString& shortcut)
{
    return !normalized(shortcut).isEmpty();
}

// A second key for Enter on a result (GitHub #293: one the left hand has
// without leaving the letters). As the preview's, but never Space: that
// separates the words of every search.
inline QString normalizedOpen(const QString& shortcut)
{
    const QString normal = normalized(shortcut);
    return normal == u"Space" ? QString() : normal;
}

inline bool matches(const QKeySequence& sequence, int key, int modifiers) noexcept
{
    const auto mods = Qt::KeyboardModifiers(modifiers) & ~Qt::KeypadModifier;
    return !sequence.isEmpty() && sequence[0] == QKeyCombination(mods, Qt::Key(key));
}

inline bool usedGlobally(const QString& shortcut, std::initializer_list<QString> shortcuts)
{
    const QString normal = hotkeys::normalized(shortcut);
    return !normal.isEmpty() && std::ranges::any_of(shortcuts, [&](const QString& other) {
        return normal == hotkeys::normalized(other);
    });
}

} // namespace ws::previewkeys
