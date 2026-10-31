#pragma once

#include "PathText.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <array>
#include <optional>
#include <span>

namespace ws::onecommander {

using namespace Qt::StringLiterals;

struct TabState {
    int count = 0;
    int selected = -1;
    bool visible = true;
};

// A saved window has one Tabs<name>1/2.json per pane. Never treat the
// application's RecentFolders API as tabs: it also retains closed tabs.
// Accept a session only when its counts and selected path agree with the
// live UI Automation controls; an ambiguous window is handled by the caller.
inline std::optional<QList<pathtext::XYplorerTab>> sessionTabs(
    const std::array<QByteArray, 2>& panes, std::span<const TabState> state, const QString& active)
{
    if (state.size() != 2 || active.isEmpty())
        return std::nullopt;
    std::array<QStringList, 2> paths;
    int activePane = -1;
    for (std::size_t i = 0; i < panes.size(); ++i) {
        if (panes[i].isEmpty())
            continue; // a pane never changed may not have a session file yet
        const QJsonDocument document = QJsonDocument::fromJson(panes[i]);
        if (!document.isArray())
            return std::nullopt;
        const QJsonArray tabs = document.array();
        if (tabs.size() != state[i].count || state[i].selected < 0 || state[i].selected >= tabs.size())
            return std::nullopt;
        for (const QJsonValue& tab : tabs) {
            const QJsonObject item = tab.toObject();
            paths[i].append(item.value(u"IsFolder"_s).toBool()
                ? pathtext::pathFromText(item.value(u"FullPath"_s).toString()) : QString());
        }
        if (pathtext::sameFolder(paths[i][state[i].selected], active))
            activePane = static_cast<int>(i);
    }
    if (activePane < 0)
        return std::nullopt;
    QList<pathtext::XYplorerTab> out;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (!state[i].visible)
            continue;
        for (qsizetype j = 0; j < paths[i].size(); ++j) {
            if (!paths[i][j].isEmpty())
                out.append({paths[i][j], j == state[i].selected ? (int(i) == activePane ? 1 : 2) : 0});
        }
    }
    return out;
}

} // namespace ws::onecommander
