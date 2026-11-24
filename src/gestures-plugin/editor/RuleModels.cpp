#include "RuleModels.h"

#include <QCoreApplication>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::gestureseditor {

namespace {

struct Text {
    Q_DECLARE_TR_FUNCTIONS(GesturesEditor)
};

QString strokeWords(std::string_view stroke)
{
    if (stroke == gestures::kClockwise)
        return Text::tr("顺时针画圈");
    if (stroke == gestures::kCounterClockwise)
        return Text::tr("逆时针画圈");
    QStringList parts;
    for (const char c : stroke) {
        switch (c) {
        case 'L': parts << Text::tr("向左"); break;
        case 'R': parts << Text::tr("向右"); break;
        case 'U': parts << Text::tr("向上"); break;
        case 'D': parts << Text::tr("向下"); break;
        default: break;
        }
    }
    return parts.join(Text::tr("，再"));
}

QString gestureWords(const std::string& on, bool chinese)
{
    const auto parsed = gestures::parseGestureTrigger(on);
    if (!parsed)
        return strokeWords(on);
    const QString stroke = strokeWords(parsed->stroke);
    const QString trigger = parsed->trigger == gestures::MouseButton::Any ? QString() : qstr(gestures::buttonTitle(parsed->trigger, chinese));
    const QString chord = parsed->chord == gestures::MouseButton::Any ? QString() : qstr(gestures::buttonTitle(parsed->chord, chinese));
    if (parsed->stroke.empty() && !chord.isEmpty()) {
        if (!trigger.isEmpty())
            return Text::tr("按住%1，再点%2").arg(trigger, chord);
        return Text::tr("按住手势键，再点%1").arg(chord);
    }
    QString body = stroke;
    if (!chord.isEmpty())
        body = Text::tr("%1，再点%2").arg(body, chord);
    if (!trigger.isEmpty())
        return Text::tr("按住%1，%2").arg(trigger, body);
    return body;
}

QString triggerScopeTitle(gestures::MouseButton button, bool chinese, bool disabled)
{
    if (button == gestures::MouseButton::Any)
        return Text::tr("任意按键");
    const QString label = Text::tr("仅%1").arg(qstr(gestures::buttonTitle(button, chinese)));
    return disabled ? Text::tr("%1（未启用）").arg(label) : label;
}

bool hasApp(const gestures::Rule& rule, const std::wstring& app)
{
    return std::ranges::find(rule.apps, app) != rule.apps.end();
}

} // namespace

QString qstr(const std::wstring& text)
{
    return QString::fromStdWString(text);
}

std::wstring wstr(const QString& text)
{
    return text.toStdWString();
}

gestures::Action makeAction(const QString& kind, const QString& value)
{
    if (kind == u"builtin") {
        if (const auto builtin = gestures::builtinById(value.toStdString()))
            return gestures::builtinAction(*builtin);
        return {};
    }
    if (kind == u"keys")
        return gestures::keysAction(wstr(value));
    if (kind == u"open")
        return gestures::openAction(wstr(value));
    if (kind == u"type")
        return gestures::typeAction(wstr(value));
    return {};
}

QString kindName(gestures::Action::Kind kind)
{
    switch (kind) {
    case gestures::Action::Kind::Nothing: return u"nothing"_s;
    case gestures::Action::Kind::Builtin: return u"builtin"_s;
    case gestures::Action::Kind::Keys: return u"keys"_s;
    case gestures::Action::Kind::Open: return u"open"_s;
    case gestures::Action::Kind::Type: return u"type"_s;
    case gestures::Action::Kind::Invalid: return u"invalid"_s;
    }
    return u"nothing"_s;
}

QString actionIcon(const gestures::Action& action)
{
    switch (action.kind) {
    case gestures::Action::Kind::Nothing: return u"ph:prohibit"_s;
    case gestures::Action::Kind::Keys: return u"ph:keyboard"_s;
    case gestures::Action::Kind::Open: return u"ph:arrow-square-out"_s;
    case gestures::Action::Kind::Type: return u"ph:text-t"_s;
    case gestures::Action::Kind::Invalid: return u"ph:warning"_s;
    case gestures::Action::Kind::Builtin:
        switch (gestures::info(action.builtin).category) {
        case gestures::Category::Page: return u"ph:globe"_s;
        case gestures::Category::Tabs: return u"ph:browsers"_s;
        case gestures::Category::Window: return u"ph:app-window"_s;
        case gestures::Category::Text: return u"ph:text-aa"_s;
        case gestures::Category::WinShun: return u"ph:magnifying-glass"_s;
        case gestures::Category::System: return u"ph:power"_s;
        }
    }
    return u"ph:prohibit"_s;
}

// ---- RulesModel ------------------------------------------------------------------------

RulesModel::RulesModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int RulesModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rules.size());
}

QHash<int, QByteArray> RulesModel::roleNames() const
{
    return {
        {SourceRowRole, "sourceRow"}, {OnRole, "on"}, {ArrowsRole, "arrows"}, {DescriptionRole, "description"},
        {WhereRole, "where"}, {WhereTitleRole, "whereTitle"},
        {TriggerButtonRole, "triggerButton"}, {TriggerButtonTitleRole, "triggerButtonTitle"}, {TriggerButtonDisabledRole, "triggerButtonDisabled"},
        {SelectionRole, "selection"}, {AppsRole, "apps"},
        {KindRole, "kind"}, {ActionTitleRole, "actionTitle"}, {ActionNoteRole, "actionNote"}, {ActionIconRole, "actionIcon"},
        {ActionIdRole, "actionId"}, {ProvisionalRole, "provisional"},
    };
}

QVariant RulesModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rules.size()))
        return {};
    const gestures::Rule& rule = m_rules[static_cast<std::size_t>(index.row())];
    const gestures::Action& action = rule.action;
    const bool key = gestures::triggerKind(rule.on) == gestures::TriggerKind::Key;
    const bool takesWords = action.kind == gestures::Action::Kind::Builtin && gestures::takesSelection(action.builtin);
    const gestures::MouseButton trigBtn = rule.triggerButton();
    const bool trigDisabled = trigBtn != gestures::MouseButton::Any && !gestures::hasTriggerButton(m_buttons, trigBtn);
    switch (role) {
    case SourceRowRole: return index.row();
    case OnRole: return QString::fromStdString(rule.on);
    case ArrowsRole:
        return key ? qstr(gestures::triggerTitle(rule.on, m_chinese)) : qstr(gestures::gestureBadge(rule.on, m_chinese));
    case DescriptionRole:
        // A shortcut's section says it is for selected text: only one that is not says so.
        if (key)
            return rule.selection || takesWords ? QString() : Text::tr("不用选中文字");
        return gestureWords(rule.on, m_chinese);
    case WhereRole: return static_cast<int>(rule.where);
    case WhereTitleRole:
        return rule.where == gestures::Zone::Anywhere ? Text::tr("任何位置")
                                                      : Text::tr("从%1开始画").arg(qstr(gestures::zoneTitle(rule.where, m_chinese)));
    case TriggerButtonRole: return QString::fromLatin1(gestures::buttonId(trigBtn));
    case TriggerButtonTitleRole: return triggerScopeTitle(trigBtn, m_chinese, trigDisabled);
    case TriggerButtonDisabledRole: return trigDisabled;
    case SelectionRole: return rule.selection;
    case AppsRole: {
        QStringList apps;
        for (const std::wstring& app : rule.apps)
            apps << qstr(app);
        return apps;
    }
    case KindRole: return kindName(action.kind);
    case ActionTitleRole: return qstr(gestures::actionTitle(action, m_chinese));
    case ActionNoteRole:
        if (action.kind == gestures::Action::Kind::Builtin) {
            if (takesWords)
                return key ? QString() : Text::tr("用选中的文字");
            if (gestures::asksFirst(action.builtin))
                return Text::tr("会先倒数 5 秒，可取消");
        }
        return QString();
    case ActionIconRole: return actionIcon(action);
    case ActionIdRole:
        return action.kind == gestures::Action::Kind::Builtin ? QString::fromLatin1(gestures::info(action.builtin).id)
            : action.kind == gestures::Action::Kind::Type     ? qstr(action.target)
                                                              : qstr(action.text);
    case ProvisionalRole: return index.row() == m_provisional;
    default: return {};
    }
}

std::vector<gestures::Rule> RulesModel::saved() const
{
    std::vector<gestures::Rule> out;
    for (std::size_t i = 0; i < m_rules.size(); ++i) {
        if (static_cast<int>(i) != m_provisional)
            out.push_back(m_rules[i]);
    }
    return out;
}

void RulesModel::setRules(std::vector<gestures::Rule> rules)
{
    beginResetModel();
    m_rules = std::move(rules);
    m_provisional = -1;
    endResetModel();
}

void RulesModel::setChinese(bool chinese)
{
    if (m_chinese == chinese)
        return;
    m_chinese = chinese;
    if (!m_rules.empty())
        emit dataChanged(index(0), index(static_cast<int>(m_rules.size()) - 1));
}

void RulesModel::setButtons(const std::vector<gestures::MouseButton>& buttons)
{
    if (m_buttons == buttons)
        return;
    m_buttons = buttons;
    if (!m_rules.empty())
        emit dataChanged(index(0), index(static_cast<int>(m_rules.size()) - 1));
}

int RulesModel::find(const gestures::Rule& probe) const
{
    for (std::size_t i = 0; i < m_rules.size(); ++i) {
        if (m_rules[i].sameCase(probe))
            return static_cast<int>(i);
    }
    return -1;
}

int RulesModel::add(gestures::Rule rule, bool provisional)
{
    discardProvisional();
    const int row = static_cast<int>(m_rules.size());
    beginInsertRows({}, row, row);
    m_rules.push_back(std::move(rule));
    if (provisional)
        m_provisional = row;
    endInsertRows();
    if (!provisional)
        emit edited();
    return row;
}

void RulesModel::changed(int row)
{
    emit dataChanged(index(row), index(row));
}

void RulesModel::setAction(int row, const gestures::Action& action)
{
    if (row < 0 || row >= static_cast<int>(m_rules.size()))
        return;
    gestures::Rule& rule = m_rules[static_cast<std::size_t>(row)];
    const bool wasProvisional = row == m_provisional;
    if (!wasProvisional && rule.action == action)
        return;
    rule.action = action;
    m_provisional = wasProvisional ? -1 : m_provisional;
    changed(row);
    emit edited();
}

bool RulesModel::setWhere(int row, gestures::Zone zone)
{
    if (row < 0 || row >= static_cast<int>(m_rules.size()))
        return false;
    gestures::Rule probe = m_rules[static_cast<std::size_t>(row)];
    if (probe.where == zone)
        return true;
    probe.where = zone;
    if (const int other = find(probe); other >= 0 && other != row)
        return false;
    m_rules[static_cast<std::size_t>(row)].where = zone;
    changed(row);
    if (row != m_provisional)
        emit edited();
    return true;
}

bool RulesModel::setTriggerButton(int row, gestures::MouseButton button)
{
    if (row < 0 || row >= static_cast<int>(m_rules.size()))
        return false;
    gestures::Rule probe = m_rules[static_cast<std::size_t>(row)];
    const auto parsed = gestures::parseGestureTrigger(probe.on);
    if (!parsed)
        return false;
    if (parsed->trigger == button)
        return true;
    const std::string nextOn = gestures::formatGestureTrigger(button, parsed->stroke, parsed->chord);
    if (nextOn.empty())
        return false;
    probe.on = nextOn;
    if (const int other = find(probe); other >= 0 && other != row)
        return false;
    m_rules[static_cast<std::size_t>(row)].on = nextOn;
    changed(row);
    if (row != m_provisional)
        emit edited();
    return true;
}

void RulesModel::setTrigger(int row, const std::string& on)
{
    if (row < 0 || row >= static_cast<int>(m_rules.size()) || m_rules[static_cast<std::size_t>(row)].on == on)
        return;
    m_rules[static_cast<std::size_t>(row)].on = on;
    changed(row);
    if (row != m_provisional)
        emit edited();
}

void RulesModel::removeAt(int row)
{
    if (row < 0 || row >= static_cast<int>(m_rules.size()))
        return;
    const bool wasProvisional = row == m_provisional;
    beginRemoveRows({}, row, row);
    m_rules.erase(m_rules.begin() + row);
    if (wasProvisional)
        m_provisional = -1;
    else if (m_provisional > row)
        --m_provisional;
    endRemoveRows();
    if (!wasProvisional)
        emit edited();
}

int RulesModel::isolate(int row, const std::wstring& app)
{
    if (row < 0 || row >= static_cast<int>(m_rules.size()))
        return row;
    gestures::Rule& rule = m_rules[static_cast<std::size_t>(row)];
    if (rule.apps.size() < 2 || !hasApp(rule, app))
        return row;
    gestures::Rule one = rule;
    one.apps = {app};
    std::erase(rule.apps, app);
    beginInsertRows({}, row + 1, row + 1);
    m_rules.insert(m_rules.begin() + row + 1, std::move(one));
    if (m_provisional > row)
        ++m_provisional;
    endInsertRows();
    changed(row);
    return row + 1;
}

void RulesModel::removeApp(int row, const std::wstring& app)
{
    if (row < 0 || row >= static_cast<int>(m_rules.size()))
        return;
    gestures::Rule& rule = m_rules[static_cast<std::size_t>(row)];
    if (rule.apps.size() > 1 && hasApp(rule, app)) {
        std::erase(rule.apps, app);
        changed(row);
        emit edited();
        return;
    }
    removeAt(row);
}

void RulesModel::discardProvisional()
{
    if (m_provisional >= 0)
        removeAt(m_provisional);
}

// ---- RulesFilter -----------------------------------------------------------------------

RulesFilter::RulesFilter(QObject* parent)
    : QSortFilterProxyModel(parent)
{
}

void RulesFilter::setMode(int mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    invalidateFilter();
    emit modeChanged();
}

void RulesFilter::setApp(const QString& app)
{
    if (m_app == app)
        return;
    m_app = app;
    invalidateFilter();
    emit appChanged();
}

int RulesFilter::proxyRow(int sourceRow) const
{
    if (!sourceModel())
        return -1;
    return mapFromSource(sourceModel()->index(sourceRow, 0)).row();
}

bool RulesFilter::filterAcceptsRow(int sourceRow, const QModelIndex&) const
{
    const auto* model = qobject_cast<const RulesModel*>(sourceModel());
    if (!model || sourceRow < 0 || sourceRow >= static_cast<int>(model->rules().size()))
        return false;
    const gestures::Rule& rule = model->rules()[static_cast<std::size_t>(sourceRow)];
    const gestures::TriggerKind kind = gestures::triggerKind(rule.on);
    if (m_mode == 3)
        return kind == gestures::TriggerKind::Key && rule.apps.empty();
    if (kind != gestures::TriggerKind::Gesture)
        return false;
    switch (m_mode) {
    case 0: return !rule.selection && rule.apps.empty();
    case 1: return rule.selection && rule.apps.empty();
    default: return !m_app.isEmpty() && hasApp(rule, wstr(m_app));
    }
}

} // namespace ws::gestureseditor
