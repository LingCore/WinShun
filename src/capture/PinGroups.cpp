#include "PinGroups.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::capture {

namespace {

bool matchesIdFormat(const QString& id, int* number)
{
    if (!id.startsWith(u'g') || id.size() < 2)
        return false;
    bool ok = false;
    const int value = id.mid(1).toInt(&ok);
    if (!ok || value <= 0)
        return false;
    if (number)
        *number = value;
    return true;
}

} // namespace

QString PinGroupStore::defaultGroupName()
{
    return QCoreApplication::translate("ws::capture::PinGroups", "默认");
}

QString PinGroupStore::allGroupsName()
{
    return QCoreApplication::translate("ws::capture::PinGroups", "全部分组");
}

std::vector<PinGroup> PinGroupStore::groups() const
{
    std::vector<PinGroup> result;
    result.reserve(m_custom.size() + 1);
    result.push_back({QString(), defaultGroupName()});
    for (const PinGroup& g : m_custom)
        result.push_back(g);
    return result;
}

bool PinGroupStore::contains(const QString& id) const
{
    if (id.isEmpty())
        return true;
    return std::ranges::any_of(m_custom, [&](const PinGroup& g) { return g.id == id; });
}

QString PinGroupStore::nameOf(const QString& id) const
{
    if (id.isEmpty())
        return defaultGroupName();
    const auto it = std::ranges::find(m_custom, id, &PinGroup::id);
    return it != m_custom.end() ? it->name : defaultGroupName();
}

QString PinGroupStore::normalizePinGroup(const QString& id) const
{
    return contains(id) ? id : QString();
}

QString PinGroupStore::activeStateToken() const
{
    return m_showingAll ? kAllGroupsToken.toString() : m_activeId;
}

bool PinGroupStore::isPinVisibleInActiveView(const QString& pinGroupId) const
{
    return m_showingAll || normalizePinGroup(pinGroupId) == m_activeId;
}

void PinGroupStore::setActiveGroup(const QString& id)
{
    m_activeId = normalizePinGroup(id);
    m_showingAll = false;
}

void PinGroupStore::setShowingAll(bool on)
{
    m_showingAll = on;
}

void PinGroupStore::setActiveStateToken(const QString& token)
{
    const QString trimmed = token.trimmed();
    if (trimmed == kAllGroupsToken) {
        m_showingAll = true;
    } else {
        m_activeId = normalizePinGroup(trimmed);
        m_showingAll = false;
    }
}

QString PinGroupStore::validateName(const QString& rawName, const QString& ignoreId, QString* error) const
{
    const QString name = rawName.simplified();
    if (name.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("ws::capture::PinGroups", "请输入分组名称");
        return {};
    }
    if (name.size() > kMaxNameLength) {
        if (error)
            *error = QCoreApplication::translate("ws::capture::PinGroups", "分组名称最多 %1 个字").arg(kMaxNameLength);
        return {};
    }
    if (name.compare(defaultGroupName(), Qt::CaseInsensitive) == 0
        || name.compare(u"默认"_s, Qt::CaseInsensitive) == 0
        || name.compare(u"Default"_s, Qt::CaseInsensitive) == 0
        || name.compare(allGroupsName(), Qt::CaseInsensitive) == 0) {
        if (error)
            *error = QCoreApplication::translate("ws::capture::PinGroups", "不能使用内置分组名称");
        return {};
    }
    const bool dup = std::ranges::any_of(m_custom, [&](const PinGroup& g) {
        return g.id != ignoreId && g.name.compare(name, Qt::CaseInsensitive) == 0;
    });
    if (dup) {
        if (error)
            *error = QCoreApplication::translate("ws::capture::PinGroups", "已有同名分组");
        return {};
    }
    return name;
}

QString PinGroupStore::allocateId()
{
    for (;;) {
        const QString candidate = u"g%1"_s.arg(m_nextId++);
        if (!contains(candidate))
            return candidate;
    }
}

std::optional<PinGroup> PinGroupStore::createGroup(const QString& rawName, QString* error)
{
    if (static_cast<int>(m_custom.size()) >= kMaxCustomGroups) {
        if (error)
            *error = QCoreApplication::translate("ws::capture::PinGroups", "最多可创建 %1 个自定义分组").arg(kMaxCustomGroups);
        return std::nullopt;
    }
    const QString name = validateName(rawName, {}, error);
    if (name.isEmpty())
        return std::nullopt;
    PinGroup group {allocateId(), name};
    m_custom.push_back(group);
    return group;
}

bool PinGroupStore::renameGroup(const QString& id, const QString& rawName, QString* error)
{
    if (id.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("ws::capture::PinGroups", "默认分组不能重命名");
        return false;
    }
    const auto it = std::ranges::find(m_custom, id, &PinGroup::id);
    if (it == m_custom.end()) {
        if (error)
            *error = QCoreApplication::translate("ws::capture::PinGroups", "未找到该分组");
        return false;
    }
    const QString name = validateName(rawName, id, error);
    if (name.isEmpty())
        return false;
    it->name = name;
    return true;
}

bool PinGroupStore::deleteGroup(const QString& id)
{
    if (id.isEmpty())
        return false;
    const auto it = std::ranges::find(m_custom, id, &PinGroup::id);
    if (it == m_custom.end())
        return false;
    m_custom.erase(it);
    if (m_activeId == id)
        m_activeId.clear();
    return true;
}

std::optional<QString> PinGroupStore::nextGroupId() const
{
    if (m_custom.empty())
        return std::nullopt;
    if (m_showingAll)
        return m_custom.front().id;
    if (m_activeId.isEmpty())
        return m_custom.front().id;
    const auto it = std::ranges::find(m_custom, m_activeId, &PinGroup::id);
    if (it == m_custom.end() || std::next(it) == m_custom.end())
        return QString(); // wrap back to built-in default group
    return std::next(it)->id;
}

QString PinGroupStore::suggestedNewName() const
{
    for (int i = 1; i <= kMaxCustomGroups + 10; ++i) {
        const QString candidate = QCoreApplication::translate("ws::capture::PinGroups", "分组 %1").arg(i);
        const bool taken = std::ranges::any_of(m_custom, [&](const PinGroup& g) {
            return g.name.compare(candidate, Qt::CaseInsensitive) == 0;
        });
        if (!taken)
            return candidate;
    }
    return QCoreApplication::translate("ws::capture::PinGroups", "新分组");
}

QString PinGroupStore::serializeGroups() const
{
    if (m_custom.empty())
        return {};
    QJsonArray array;
    for (const PinGroup& g : m_custom)
        array.append(QJsonObject {{u"id"_s, g.id}, {u"name"_s, g.name}});
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

void PinGroupStore::loadGroups(const QString& serialized)
{
    m_custom.clear();
    m_nextId = 1;
    const QJsonDocument doc = QJsonDocument::fromJson(serialized.toUtf8());
    if (doc.isArray()) {
        for (const QJsonValue& value : doc.array()) {
            if (static_cast<int>(m_custom.size()) >= kMaxCustomGroups)
                break;
            if (!value.isObject())
                continue;
            const QJsonObject obj = value.toObject();
            const QString id = obj.value(u"id"_s).toString().trimmed();
            const QString name = validateName(obj.value(u"name"_s).toString(), {}, nullptr);
            if (id.isEmpty() || id == kAllGroupsToken || name.isEmpty() || contains(id))
                continue;
            m_custom.push_back({id, name});
            int number = 0;
            if (matchesIdFormat(id, &number) && number >= m_nextId)
                m_nextId = number + 1;
        }
    }
    if (!contains(m_activeId))
        m_activeId.clear();
}

} // namespace ws::capture
