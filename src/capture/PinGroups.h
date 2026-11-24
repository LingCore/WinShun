#pragma once

#include <QString>
#include <QStringView>

#include <optional>
#include <vector>

namespace ws::capture {

// One pin group (#451). The built-in default group always has `id` empty;
// custom groups have short opaque ids ("g1", "g2", ...).
struct PinGroup {
    QString id;
    QString name;
};

// Pure-logic pin group model (#451): manages custom groups, active view
// (a single group or "all groups"), cycling order, validation, and JSON
// serialization without any HWND or thread dependencies.
class PinGroupStore {
public:
    static constexpr QStringView kAllGroupsToken = u"*";
    static constexpr int kMaxCustomGroups = 20;
    static constexpr int kMaxNameLength = 32;

    static QString defaultGroupName();
    static QString allGroupsName();

    // All groups in display order: default group first, followed by custom groups.
    std::vector<PinGroup> groups() const;
    const std::vector<PinGroup>& customGroups() const { return m_custom; }
    bool hasCustomGroups() const { return !m_custom.empty(); }

    bool contains(const QString& id) const;
    QString nameOf(const QString& id) const;
    QString normalizePinGroup(const QString& id) const;

    const QString& activeGroupId() const { return m_activeId; }
    bool showingAll() const { return m_showingAll; }
    QString activeStateToken() const;
    QString targetGroupForNewPin() const { return m_showingAll ? QString() : m_activeId; }
    bool isPinVisibleInActiveView(const QString& pinGroupId) const;

    void setActiveGroup(const QString& id);
    void setShowingAll(bool on);
    void setActiveStateToken(const QString& token);

    // Creates a custom group with trimmed name; returns nullopt and sets `error` on invalid/duplicate/full.
    std::optional<PinGroup> createGroup(const QString& rawName, QString* error = nullptr);
    bool renameGroup(const QString& id, const QString& rawName, QString* error = nullptr);
    bool deleteGroup(const QString& id);

    // Cycles across concrete groups (default -> g1 -> g2 -> default).
    // Returns nullopt when no custom groups exist.
    std::optional<QString> nextGroupId() const;
    // Generates an unused default name ("分组 1", "分组 2", ...) for the creation prompt.
    QString suggestedNewName() const;

    QString serializeGroups() const;
    void loadGroups(const QString& serialized);

private:
    QString validateName(const QString& rawName, const QString& ignoreId, QString* error) const;
    QString allocateId();

    std::vector<PinGroup> m_custom;
    QString m_activeId; // "" = built-in default group
    bool m_showingAll = false;
    int m_nextId = 1;
};

} // namespace ws::capture
