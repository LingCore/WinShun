#pragma once

#include <QString>
#include <QStringList>

#include <algorithm>
#include <optional>
#include <vector>

// Which folder a window shows, told by what it shows of it: for a file
// manager that says nothing else (360文件夹, hostedexplorer::place). Its
// title is the folder's name, which a few folders on the PC may have; the
// names it lists tell which one it is.
namespace ws::shownfolder {

struct Candidate {
    QString path;
    QStringList children; // the names in it, as on disk
};

// Whether a name listed is that child's: the same, or without its
// extension (File Explorer hides known ones by default). Case aside.
inline bool listedAs(const QString& child, const QString& listed)
{
    if (listed.isEmpty())
        return false;
    if (child.compare(listed, Qt::CaseInsensitive) == 0)
        return true;
    const qsizetype dot = child.lastIndexOf(u'.');
    return dot > 0 && dot == listed.size() && child.startsWith(listed, Qt::CaseInsensitive);
}

// How many of the names listed are among the candidate's children.
inline int matches(const Candidate& candidate, const QStringList& listed)
{
    int found = 0;
    for (const QString& name : listed) {
        if (std::ranges::any_of(candidate.children, [&](const QString& child) { return listedAs(child, name); }))
            ++found;
    }
    return found;
}

// The candidate the names listed are of: the one most of them are in, if
// that is clearly one. With up to two names listed, all of them; with more,
// three in five (names shown in another language, as Windows shows its own
// folders in a profile: 文档 for Documents; the index a moment behind), and
// more than any other candidate has. Nothing listed (an empty folder): the
// only candidate. nullopt: none, or not one alone.
inline std::optional<QString> pick(const std::vector<Candidate>& candidates, const QStringList& listed)
{
    if (candidates.empty())
        return std::nullopt;
    if (listed.isEmpty())
        return candidates.size() == 1 ? std::optional(candidates.front().path) : std::nullopt;
    const int wanted = listed.size() <= 2 ? static_cast<int>(listed.size()) : static_cast<int>((listed.size() * 3 + 4) / 5);
    int best = -1;
    int second = -1;
    const Candidate* chosen = nullptr;
    for (const Candidate& candidate : candidates) {
        const int found = matches(candidate, listed);
        if (found > best) {
            second = best;
            best = found;
            chosen = &candidate;
        } else if (found > second) {
            second = found;
        }
    }
    if (!chosen || best < wanted || best == second)
        return std::nullopt;
    return chosen->path;
}

} // namespace ws::shownfolder
