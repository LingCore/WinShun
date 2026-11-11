#pragma once

// Whether a name typed for a file or folder (renaming a result in place,
// GitHub #285) is one Windows takes, before the shell is asked: said at once,
// in the launcher's words, instead of in a dialog of Explorer's.

#include <QString>
#include <QStringView>

#include <algorithm>
#include <array>

namespace ws::renamecheck {

enum class Problem {
    None,
    Empty,
    Same, // the name it has: nothing to do
    Character, // one of \ / : * ? " < > | or a control character
    Reserved, // CON, NUL, COM1 … (with or without an extension)
    Ending, // ends with a dot: Windows drops it
    Long, // more than 255 characters
};

// The name as it will be: without the spaces around it.
inline QString cleaned(const QString& typed)
{
    return typed.trimmed();
}

inline Problem problem(const QString& current, const QString& typed)
{
    using namespace Qt::StringLiterals;
    const QString name = cleaned(typed);
    if (name.isEmpty())
        return Problem::Empty;
    if (name == current)
        return Problem::Same;
    if (name.endsWith(u'.'))
        return Problem::Ending;
    if (name.size() > 255)
        return Problem::Long;
    const bool bad = std::ranges::any_of(name, [](QChar c) {
        return c.unicode() < 0x20 || QStringView(u"\\/:*?\"<>|").contains(c);
    });
    if (bad)
        return Problem::Character;
    static constexpr std::array reserved {u"CON", u"PRN", u"AUX", u"NUL", u"COM1", u"COM2", u"COM3", u"COM4", u"COM5", u"COM6",
        u"COM7", u"COM8", u"COM9", u"LPT1", u"LPT2", u"LPT3", u"LPT4", u"LPT5", u"LPT6", u"LPT7", u"LPT8", u"LPT9"};
    const qsizetype dot = name.indexOf(u'.');
    const QStringView stem = QStringView(name).left(dot < 0 ? name.size() : dot).trimmed();
    if (std::ranges::any_of(reserved, [&](const char16_t* word) { return stem.compare(QStringView(word), Qt::CaseInsensitive) == 0; }))
        return Problem::Reserved;
    return Problem::None;
}

// How much of a name is picked when its box opens: a file's name up to its
// last dot (the extension stays as it is unless the user goes for it), a
// folder's whole name, and a name that starts with its only dot
// (".gitignore") whole.
inline qsizetype stemLength(const QString& name, bool folder)
{
    const qsizetype dot = name.lastIndexOf(u'.');
    return folder || dot <= 0 ? name.size() : dot;
}

} // namespace ws::renamecheck
