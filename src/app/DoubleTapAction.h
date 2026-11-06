#pragma once

#include <QString>
#include <QStringView>

namespace ws::doubletap {

// One gesture has one destination; never simulate a global hotkey to invoke
// a plugin (another application may own it). The original spelling is sent
// through the same authenticated notification as PluginHotkeys.
struct PluginAction {
    QString plugin;
    QString key;
};

inline QString encode(const QString& plugin, const QString& key)
{
    return QStringLiteral("plugin:") + plugin + u'/' + key;
}

inline PluginAction decode(QStringView value)
{
    if (!value.startsWith(u"plugin:"))
        return {};
    const qsizetype slash = value.indexOf(u'/', 7);
    if (slash <= 7 || slash + 1 == value.size() || value.size() > 256)
        return {};
    const QStringView id = value.mid(7, slash - 7);
    if (!id.front().isLetterOrNumber() || !id.back().isLetterOrNumber() || id.contains(u".."))
        return {};
    for (QChar ch : id) {
        if (!ch.isLetterOrNumber() && ch != u'.' && ch != u'-' && ch != u'_')
            return {};
    }
    const QStringView key = value.mid(slash + 1);
    if (key.size() > 64)
        return {};
    for (QChar ch : key) {
        if (!ch.isPrint())
            return {};
    }
    return {id.toString(), key.toString()};
}

inline bool valid(QStringView value)
{
    return value == u"launcher" || value == u"clipboard" || !decode(value).plugin.isEmpty();
}

} // namespace ws::doubletap
