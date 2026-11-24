#include "GestureEditor.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QWindow>

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::shell {
// src/app/platform/ShellDialogs.cpp: the standard "open file" dialog (modal); empty when cancelled.
QString pickFile(HWND owner, const QString& title, const std::vector<std::pair<QString, QString>>& filters);
}

namespace ws::gestureseditor {

namespace {

struct Text {
    Q_DECLARE_TR_FUNCTIONS(GesturesEditor)
};

constexpr int kPendingMost = 16; // saved lines waiting for their answer, at most (one may never get it)

QString onOff(bool on)
{
    return on ? u"1"_s : u"0"_s;
}

QStringList toList(const std::vector<std::wstring>& programs)
{
    QStringList out;
    for (const std::wstring& program : programs)
        out << qstr(program);
    return out;
}

bool hasApp(const gestures::Rule& rule, const std::wstring& app)
{
    return std::ranges::find(rule.apps, app) != rule.apps.end();
}

void remember(QStringList& pending, const QString& value)
{
    pending << value;
    while (pending.size() > kPendingMost)
        pending.removeFirst();
}

// The name of a key as an action's keys write it ("F5", "Left", "="); empty for one that has none.
QString keyName(int key, bool shift)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return QString(QChar(key));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return QString(QChar(key));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return u"F%1"_s.arg(key - Qt::Key_F1 + 1);
    switch (key) {
    case Qt::Key_Left: return u"Left"_s;
    case Qt::Key_Right: return u"Right"_s;
    case Qt::Key_Up: return u"Up"_s;
    case Qt::Key_Down: return u"Down"_s;
    case Qt::Key_Home: return u"Home"_s;
    case Qt::Key_End: return u"End"_s;
    case Qt::Key_PageUp: return u"PgUp"_s;
    case Qt::Key_PageDown: return u"PgDn"_s;
    case Qt::Key_Return:
    case Qt::Key_Enter: return u"Enter"_s;
    case Qt::Key_Escape: return u"Esc"_s;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return u"Tab"_s;
    case Qt::Key_Space: return u"Space"_s;
    case Qt::Key_Backspace: return u"Backspace"_s;
    case Qt::Key_Delete: return u"Delete"_s;
    case Qt::Key_Insert: return u"Insert"_s;
    case Qt::Key_Menu: return u"Menu"_s;
    case Qt::Key_Print: return u"PrintScreen"_s;
    case Qt::Key_VolumeUp: return u"VolumeUp"_s;
    case Qt::Key_VolumeDown: return u"VolumeDown"_s;
    case Qt::Key_VolumeMute: return u"Mute"_s;
    case Qt::Key_MediaTogglePlayPause:
    case Qt::Key_MediaPlay: return u"PlayPause"_s;
    case Qt::Key_MediaNext: return u"NextTrack"_s;
    case Qt::Key_MediaPrevious: return u"PrevTrack"_s;
    case Qt::Key_Back: return u"BrowserBack"_s;
    case Qt::Key_Forward: return u"BrowserForward"_s;
    case Qt::Key_Minus: case Qt::Key_Equal: case Qt::Key_Comma: case Qt::Key_Period: case Qt::Key_Slash:
    case Qt::Key_Semicolon: case Qt::Key_BracketLeft: case Qt::Key_BracketRight: case Qt::Key_QuoteLeft:
    case Qt::Key_Apostrophe: case Qt::Key_Backslash:
        return QString(QChar(key));
    default: break;
    }
    // With Shift held Qt names the key by what it types: the key as it is on a US keyboard.
    if (key > 0 && key < 128) {
        const int shiftedDigit = QStringLiteral(")!@#$%^&*(").indexOf(QChar(key));
        if (shiftedDigit >= 0)
            return QString(QChar(u'0' + shiftedDigit));
        static const QString from = QStringLiteral("_+{}|:\"<>?~");
        static const QString to = QStringLiteral("-=[]\\;',./`");
        if (const int at = from.indexOf(QChar(key)); at >= 0)
            return QString(to[at]);
        if (key == Qt::Key_Plus && shift)
            return u"="_s;
    }
    return {};
}

struct WindowPrograms {
    QStringList names;
    DWORD own = 0;
};

BOOL CALLBACK collectWindow(HWND window, LPARAM parameter)
{
    auto* found = reinterpret_cast<WindowPrograms*>(parameter);
    if (!::IsWindowVisible(window) || ::GetWindowTextLengthW(window) == 0)
        return TRUE;
    if (::GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
        return TRUE;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(::DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked)
        return TRUE;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    if (!pid || pid == found->own)
        return TRUE;
    if (HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH * 2];
        DWORD length = sizeof path / sizeof path[0];
        if (::QueryFullProcessImageNameW(process, 0, path, &length)) {
            const QString full = QString::fromWCharArray(path, static_cast<qsizetype>(length));
            const QString name = full.mid(full.lastIndexOf(u'\\') + 1).toLower();
            // The shell's own hosts have windows no one chooses a gesture for.
            static const QStringList hosts {u"applicationframehost.exe"_s, u"textinputhost.exe"_s, u"searchhost.exe"_s,
                u"startmenuexperiencehost.exe"_s, u"shellexperiencehost.exe"_s, u"lockapp.exe"_s};
            if (!name.isEmpty() && !hosts.contains(name) && !found->names.contains(name))
                found->names << name;
        }
        ::CloseHandle(process);
    }
    return TRUE;
}

} // namespace

GestureEditor::GestureEditor(QObject* parent)
    : QObject(parent)
    , m_rules(this)
{
    m_saveLater.setSingleShot(true);
    m_saveLater.setInterval(0);
    connect(&m_saveLater, &QTimer::timeout, this, &GestureEditor::saveRules);
    connect(&m_rules, &RulesModel::edited, this, &GestureEditor::rulesEdited);
    m_rules.setRules(gestures::defaultRules());
    m_rules.setButtons(m_buttons);
    rebuildPrograms();
}

QStringList GestureEditor::buttons() const
{
    QStringList out;
    for (const gestures::MouseButton b : m_buttons)
        out << QString::fromLatin1(gestures::buttonId(b));
    return out;
}

bool GestureEditor::hasButton(const QString& id) const
{
    const auto btn = gestures::buttonById(id.toStdString());
    return btn && gestures::hasTriggerButton(m_buttons, *btn);
}

void GestureEditor::toggleButton(const QString& id)
{
    const auto btn = gestures::buttonById(id.toStdString());
    if (!btn || !gestures::isTriggerButton(*btn))
        return;
    std::vector<gestures::MouseButton> next = m_buttons;
    if (gestures::hasTriggerButton(next, *btn)) {
        if (next.size() <= 1)
            return; // always keep at least one trigger button enabled
        std::erase(next, *btn);
    } else {
        next.push_back(*btn);
        next = gestures::parseTriggerButtons(gestures::formatTriggerButtons(next));
    }
    if (next == m_buttons)
        return;
    m_buttons = std::move(next);
    m_rules.setButtons(m_buttons);
    emit buttonsChanged();
    saveButtons();
}

QVariantList GestureEditor::categories() const
{
    QVariantList out;
    for (const gestures::Category category : {gestures::Category::Page, gestures::Category::Tabs, gestures::Category::Window,
             gestures::Category::Text, gestures::Category::WinShun, gestures::Category::System}) {
        QVariantList items;
        QString icon;
        for (const gestures::BuiltinInfo& b : gestures::builtins()) {
            if (b.category != category)
                continue;
            const gestures::Action action = gestures::builtinAction(b.builtin);
            if (icon.isEmpty())
                icon = actionIcon(action);
            QString note;
            if (gestures::takesSelection(b.builtin))
                note = Text::tr("用选中的文字");
            else if (gestures::asksFirst(b.builtin))
                note = Text::tr("会先倒数 5 秒，可取消");
            items.append(QVariantMap {{u"id"_s, QString::fromLatin1(b.id)}, {u"title"_s, qstr(m_chinese ? b.zh : b.en)}, {u"note"_s, note}});
        }
        out.append(QVariantMap {{u"title"_s, qstr(gestures::categoryTitle(category, m_chinese))}, {u"icon"_s, icon}, {u"items"_s, items}});
    }
    return out;
}

void GestureEditor::setChinese(bool chinese)
{
    if (m_chinese == chinese)
        return;
    m_chinese = chinese;
    m_rules.setChinese(chinese);
    emit chineseChanged();
}

void GestureEditor::setTrail(bool on)
{
    if (m_trail == on)
        return;
    m_trail = on;
    remember(m_pendingTrail, onOff(on));
    emit trailChanged();
    emit saveRequested({{u"trail"_s, on}});
}

void GestureEditor::setFullScreen(bool on)
{
    if (m_fullScreen == on)
        return;
    m_fullScreen = on;
    remember(m_pendingFullScreen, onOff(on));
    emit fullScreenChanged();
    emit saveRequested({{u"fullScreen"_s, on}});
}

void GestureEditor::setPaused(bool on)
{
    if (m_paused == on)
        return;
    m_paused = on;
    remember(m_pendingPaused, onOff(on));
    emit pausedChanged();
    emit pauseRequested(on);
}

// An answer to what was saved, or a change from elsewhere? `incoming` that is what this window
// has now, an older value it saved, or what the plugin said before, is not a change; anything else is.
bool GestureEditor::accept(QStringList& pending, QString& heard, const QString& incoming, const QString& current)
{
    if (incoming == heard)
        return false;
    heard = incoming;
    if (incoming == current) {
        if (const qsizetype at = pending.lastIndexOf(incoming); at >= 0)
            pending = pending.mid(at + 1);
        return false;
    }
    if (const qsizetype at = pending.indexOf(incoming); at >= 0) {
        pending = pending.mid(at + 1);
        return false;
    }
    pending.clear();
    return true;
}

void GestureEditor::applyState(const QJsonObject& state, bool first)
{
    if (state.contains(u"rules"_s)) {
        std::optional<std::vector<gestures::Rule>> parsed = gestures::parseRules(state.value(u"rules"_s).toString().toStdString());
        std::vector<gestures::Rule> rules = parsed ? std::move(*parsed) : gestures::defaultRules();
        const QString incoming = QString::fromStdString(gestures::rulesJson(rules));
        const QString current = QString::fromStdString(gestures::rulesJson(m_rules.saved()));
        if (first) {
            m_pendingRules.clear();
            m_heardRules = incoming;
        }
        if (first || accept(m_pendingRules, m_heardRules, incoming, current)) {
            m_rules.setRules(std::move(rules));
            ++m_revision;
            emit revisionChanged();
            rebuildPrograms();
        }
    }
    if (state.contains(u"buttons"_s)) {
        const std::vector<gestures::MouseButton> parsed = gestures::parseTriggerButtons(state.value(u"buttons"_s).toString().toStdString());
        const QString incoming = QString::fromStdString(gestures::formatTriggerButtons(parsed));
        const QString current = QString::fromStdString(gestures::formatTriggerButtons(m_buttons));
        if (first) {
            m_pendingButtons.clear();
            m_heardButtons = incoming;
        }
        if (first || accept(m_pendingButtons, m_heardButtons, incoming, current)) {
            m_buttons = parsed;
            m_rules.setButtons(m_buttons);
            emit buttonsChanged();
        }
    }
    if (state.contains(u"exclude"_s)) {
        const QStringList incoming = toList(gestures::programList(wstr(state.value(u"exclude"_s).toString())));
        if (first) {
            m_pendingExclude.clear();
            m_heardExclude = incoming.join(u';');
        }
        if (first || accept(m_pendingExclude, m_heardExclude, incoming.join(u';'), m_excluded.join(u';'))) {
            m_excluded = incoming;
            emit excludedChanged();
        }
    }
    const auto flag = [&](const char* key, bool& value, QStringList& pending, QString& heard, void (GestureEditor::*changed)()) {
        const QString name = QString::fromLatin1(key);
        if (!state.value(name).isBool())
            return;
        const bool incoming = state.value(name).toBool();
        if (first) {
            pending.clear();
            heard = onOff(incoming);
        }
        if ((first || accept(pending, heard, onOff(incoming), onOff(value))) && value != incoming) {
            value = incoming;
            emit(this->*changed)();
        }
    };
    flag("trail", m_trail, m_pendingTrail, m_heardTrail, &GestureEditor::trailChanged);
    flag("fullScreen", m_fullScreen, m_pendingFullScreen, m_heardFullScreen, &GestureEditor::fullScreenChanged);
    flag("paused", m_paused, m_pendingPaused, m_heardPaused, &GestureEditor::pausedChanged);
    // Only the plugin knows this one (nothing here saves it): taken as it comes.
    if (state.value(u"taken"_s).isArray()) {
        QStringList taken;
        for (const QJsonValue& on : state.value(u"taken"_s).toArray())
            taken << on.toString();
        if (taken != m_taken) {
            m_taken = taken;
            emit takenChanged();
        }
    }
}

void GestureEditor::useSample()
{
    std::vector<gestures::Rule> rules = gestures::defaultRules();
    const auto add = [&](const char* on, gestures::Action action, std::vector<std::wstring> apps = {}, gestures::Zone where = gestures::Zone::Anywhere) {
        gestures::Rule rule;
        rule.on = on;
        rule.action = std::move(action);
        rule.apps = std::move(apps);
        rule.where = where;
        rules.push_back(std::move(rule));
    };
    add("L", gestures::keysAction(L"Ctrl+Z"), {L"photoshop.exe"});
    add("R", gestures::keysAction(L"Ctrl+Shift+Z"), {L"photoshop.exe"});
    add("DR", gestures::Action {}, {L"photoshop.exe"});
    add(gestures::kWheelUp.data(), gestures::keysAction(L"Ctrl+="), {L"photoshop.exe"});
    add("D", gestures::builtinAction(gestures::Builtin::Refresh), {L"chrome.exe"});
    add("D+l", gestures::builtinAction(gestures::Builtin::ReopenTab));
    add("r:+l", gestures::builtinAction(gestures::Builtin::CloseTab));
    add("x1:U", gestures::builtinAction(gestures::Builtin::NewTab));
    add("cw", gestures::builtinAction(gestures::Builtin::Shutdown), {}, gestures::Zone::BottomRight);
    add("corner:tl", gestures::builtinAction(gestures::Builtin::TaskView));
    add("corner:br", gestures::openAction(L"C:\\Windows\\System32\\notepad.exe"));
    add("edge:top", gestures::builtinAction(gestures::Builtin::Mute));
    add("edge:right", gestures::typeAction(L"Hello, world"));
    for (const auto& [keys, builtin] : {std::pair {L"Ctrl+Alt+T", gestures::Builtin::Translate}, std::pair {L"Ctrl+Alt+G", gestures::Builtin::WebSearch}}) {
        gestures::Rule rule;
        rule.on = gestures::keyTrigger(keys);
        rule.selection = true;
        rule.action = gestures::builtinAction(builtin);
        rules.push_back(std::move(rule));
    }
    m_taken = {u"key:Ctrl+Alt+G"_s}; // as if another program held it
    emit takenChanged();
    m_buttons = {gestures::MouseButton::Right, gestures::MouseButton::X1};
    m_rules.setButtons(m_buttons);
    emit buttonsChanged();
    m_rules.setRules(std::move(rules));
    m_excluded = {u"mstsc.exe"_s, u"vmware.exe"_s};
    ++m_revision;
    emit excludedChanged();
    emit revisionChanged();
    rebuildPrograms();
}

void GestureEditor::rulesEdited()
{
    ++m_revision;
    emit revisionChanged();
    rebuildPrograms();
    m_saveLater.start();
}

void GestureEditor::saveRules()
{
    const QString json = QString::fromStdString(gestures::rulesJson(m_rules.saved()));
    remember(m_pendingRules, json);
    emit saveRequested({{u"rules"_s, json}});
}

void GestureEditor::saveButtons()
{
    const QString text = QString::fromStdString(gestures::formatTriggerButtons(m_buttons));
    remember(m_pendingButtons, text);
    emit saveRequested({{u"buttons"_s, text}});
}

void GestureEditor::saveExcluded()
{
    const QString text = m_excluded.join(u';');
    remember(m_pendingExclude, text);
    emit saveRequested({{u"exclude"_s, text}});
}

void GestureEditor::rebuildPrograms()
{
    QStringList names;
    QList<int> counts;
    const std::vector<gestures::Rule> saved = m_rules.saved();
    const auto note = [&](const QString& name, int more) {
        const qsizetype at = names.indexOf(name);
        if (at < 0) {
            names << name;
            counts << more;
        } else {
            counts[at] += more;
        }
    };
    for (const gestures::Rule& rule : saved) {
        const gestures::TriggerKind kind = gestures::triggerKind(rule.on);
        if (kind != gestures::TriggerKind::Gesture && kind != gestures::TriggerKind::Wheel)
            continue;
        for (const std::wstring& app : rule.apps)
            note(qstr(app), 1);
    }
    for (const QString& name : std::as_const(m_added))
        note(name, 0);
    QVariantList list;
    for (qsizetype i = 0; i < names.size(); ++i)
        list.append(QVariantMap {{u"name"_s, names[i]}, {u"count"_s, counts[i]}});
    if (list != m_programs) {
        m_programs = list;
        emit programsChanged();
    }
}

int GestureEditor::addOrFind(const QString& on, int where, bool selection, const QString& app)
{
    const std::wstring program = wstr(app);
    const auto& rules = m_rules.rules();
    int row = -1;
    for (std::size_t i = 0; i < rules.size(); ++i) {
        const gestures::Rule& r = rules[i];
        if (r.on == on.toStdString() && static_cast<int>(r.where) == where && r.selection == selection
            && (program.empty() ? r.apps.empty() : hasApp(r, program))) {
            row = static_cast<int>(i);
            break;
        }
    }
    const bool created = row < 0;
    if (created) {
        gestures::Rule rule;
        rule.on = on.toStdString();
        rule.where = static_cast<gestures::Zone>(where);
        rule.selection = selection;
        if (!program.empty())
            rule.apps = {program};
        row = m_rules.add(std::move(rule), true);
        rebuildPrograms();
    } else {
        m_rules.discardProvisional();
        // The one that was provisional may have been above: its row is looked up again.
        for (std::size_t i = 0; i < m_rules.rules().size(); ++i) {
            const gestures::Rule& r = m_rules.rules()[i];
            if (r.on == on.toStdString() && static_cast<int>(r.where) == where && r.selection == selection
                && (program.empty() ? r.apps.empty() : hasApp(r, program))) {
                row = static_cast<int>(i);
                break;
            }
        }
    }
    emit ruleFocused(row, created);
    return row;
}

void GestureEditor::discardProvisional()
{
    m_rules.discardProvisional();
    rebuildPrograms();
}

void GestureEditor::setAction(int row, const QString& kind, const QString& value, const QString& app)
{
    if (!app.isEmpty())
        row = m_rules.isolate(row, wstr(app));
    m_rules.setAction(row, makeAction(kind, value));
}

void GestureEditor::setWhere(int row, int zone, const QString& app)
{
    if (!app.isEmpty())
        row = m_rules.isolate(row, wstr(app));
    m_rules.setWhere(row, static_cast<gestures::Zone>(zone));
}

QVariantList GestureEditor::zoneChoices(int row) const
{
    QVariantList out;
    const auto& rules = m_rules.rules();
    if (row < 0 || row >= static_cast<int>(rules.size()))
        return out;
    static const char* const icons[] = {"ph:monitor", "ph:arrow-up-left", "ph:arrow-up-right", "ph:arrow-down-left", "ph:arrow-down-right"};
    for (const gestures::Zone zone : {gestures::Zone::Anywhere, gestures::Zone::TopLeft, gestures::Zone::TopRight,
             gestures::Zone::BottomLeft, gestures::Zone::BottomRight}) {
        gestures::Rule probe = rules[static_cast<std::size_t>(row)];
        if (probe.where == zone)
            continue;
        probe.where = zone;
        if (const int other = m_rules.find(probe); other >= 0 && other != row)
            continue;
        const QString text = zone == gestures::Zone::Anywhere ? Text::tr("任何位置")
                                                               : Text::tr("从%1开始画").arg(qstr(gestures::zoneTitle(zone, m_chinese)));
        out.append(QVariantMap {{u"text"_s, text}, {u"glyph"_s, QString::fromLatin1(icons[static_cast<int>(zone)])}, {u"zone"_s, static_cast<int>(zone)}});
    }
    return out;
}

void GestureEditor::setTriggerButton(int row, const QString& buttonId, const QString& app)
{
    const gestures::MouseButton btn = buttonId.isEmpty()
        ? gestures::MouseButton::Any
        : gestures::buttonById(buttonId.toStdString()).value_or(gestures::MouseButton::Any);
    if (!app.isEmpty())
        row = m_rules.isolate(row, wstr(app));
    m_rules.setTriggerButton(row, btn);
}

QVariantList GestureEditor::buttonChoices(int row) const
{
    QVariantList out;
    const auto& rules = m_rules.rules();
    if (row < 0 || row >= static_cast<int>(rules.size()))
        return out;
    const auto parsed = gestures::parseGestureTrigger(rules[static_cast<std::size_t>(row)].on);
    if (!parsed)
        return out;
    const auto iconOf = [](gestures::MouseButton b) -> QString {
        switch (b) {
        case gestures::MouseButton::Right: return u"ph:mouse-right-click"_s;
        case gestures::MouseButton::Middle: return u"ph:mouse-middle-click"_s;
        case gestures::MouseButton::X1:
        case gestures::MouseButton::X2: return u"ph:mouse"_s;
        default: return u"ph:mouse-simple"_s;
        }
    };
    for (const gestures::MouseButton cand : {gestures::MouseButton::Any, gestures::MouseButton::Right,
             gestures::MouseButton::Middle, gestures::MouseButton::X1, gestures::MouseButton::X2}) {
        if (parsed->trigger == cand)
            continue;
        const std::string nextOn = gestures::formatGestureTrigger(cand, parsed->stroke, parsed->chord);
        if (nextOn.empty())
            continue;
        gestures::Rule probe = rules[static_cast<std::size_t>(row)];
        probe.on = nextOn;
        if (const int other = m_rules.find(probe); other >= 0 && other != row)
            continue;
        const QString text = cand == gestures::MouseButton::Any
            ? Text::tr("任意按键")
            : Text::tr("仅%1").arg(qstr(gestures::buttonTitle(cand, m_chinese)));
        out.append(QVariantMap {{u"text"_s, text}, {u"glyph"_s, iconOf(cand)}, {u"button"_s, QString::fromLatin1(gestures::buttonId(cand))}});
    }
    return out;
}

void GestureEditor::remove(int row, const QString& app)
{
    if (app.isEmpty())
        m_rules.removeAt(row);
    else
        m_rules.removeApp(row, wstr(app));
}

int GestureEditor::slotRow(const QString& on, const QString& app) const
{
    const auto& rules = m_rules.rules();
    const std::wstring program = wstr(app);
    for (std::size_t i = 0; i < rules.size(); ++i) {
        const gestures::Rule& r = rules[i];
        if (r.on == on.toStdString() && r.where == gestures::Zone::Anywhere && !r.selection
            && (program.empty() ? r.apps.empty() : hasApp(r, program)))
            return static_cast<int>(i);
    }
    return -1;
}

QVariantMap GestureEditor::slot(const QString& on, const QString& app) const
{
    const int row = slotRow(on, app);
    if (row < 0)
        return {{u"has"_s, false}, {u"kind"_s, u"none"_s}, {u"title"_s, QString()}, {u"icon"_s, u"ph:plus"_s}, {u"note"_s, QString()}, {u"id"_s, QString()}};
    const QModelIndex index = m_rules.index(row);
    return {{u"has"_s, true}, {u"kind"_s, m_rules.data(index, RulesModel::KindRole)},
        {u"title"_s, m_rules.data(index, RulesModel::ActionTitleRole)}, {u"icon"_s, m_rules.data(index, RulesModel::ActionIconRole)},
        {u"note"_s, m_rules.data(index, RulesModel::ActionNoteRole)}, {u"id"_s, m_rules.data(index, RulesModel::ActionIdRole)}};
}

void GestureEditor::setSlot(const QString& on, const QString& app, const QString& kind, const QString& value)
{
    const gestures::Action action = makeAction(kind, value);
    const bool wheel = gestures::triggerKind(on.toStdString()) == gestures::TriggerKind::Wheel;
    int row = slotRow(on, app);
    // A corner or an edge with nothing to do is one that is not set.
    if (action.kind == gestures::Action::Kind::Nothing && app.isEmpty() && !wheel) {
        if (row >= 0)
            m_rules.removeAt(row);
        return;
    }
    if (row >= 0) {
        if (!app.isEmpty())
            row = m_rules.isolate(row, wstr(app));
        m_rules.setAction(row, action);
        return;
    }
    gestures::Rule rule;
    rule.on = on.toStdString();
    if (!app.isEmpty())
        rule.apps = {wstr(app)};
    rule.action = action;
    m_rules.add(std::move(rule), false);
}

void GestureEditor::clearSlot(const QString& on, const QString& app)
{
    const int row = slotRow(on, app);
    if (row < 0)
        return;
    if (!app.isEmpty())
        m_rules.removeApp(row, wstr(app));
    else
        m_rules.removeAt(row);
}

QString GestureEditor::normalizeProgram(const QString& text) const
{
    const std::vector<std::wstring> list = gestures::programList(wstr(text));
    return list.empty() ? QString() : qstr(list.front());
}

QString GestureEditor::addProgram(const QString& name)
{
    const QString program = normalizeProgram(name);
    if (program.isEmpty())
        return {};
    bool known = m_added.contains(program);
    for (const QVariant& entry : std::as_const(m_programs))
        known = known || entry.toMap().value(u"name"_s).toString() == program;
    if (!known)
        m_added << program;
    rebuildPrograms();
    return program;
}

void GestureEditor::removeProgram(const QString& app)
{
    const std::wstring program = wstr(app);
    for (int row = static_cast<int>(m_rules.rules().size()) - 1; row >= 0; --row) {
        if (hasApp(m_rules.rules()[static_cast<std::size_t>(row)], program))
            m_rules.removeApp(row, program);
    }
    m_added.removeAll(app);
    rebuildPrograms();
}

void GestureEditor::addExcluded(const QString& program)
{
    bool changed = false;
    for (const std::wstring& name : gestures::programList(wstr(program))) {
        const QString text = qstr(name);
        if (!m_excluded.contains(text)) {
            m_excluded << text;
            changed = true;
        }
    }
    if (!changed)
        return;
    emit excludedChanged();
    saveExcluded();
}

void GestureEditor::removeExcluded(const QString& program)
{
    if (m_excluded.removeAll(program) == 0)
        return;
    emit excludedChanged();
    saveExcluded();
}

void GestureEditor::restoreDefaults()
{
    m_rules.setRules(gestures::defaultRules());
    m_added.clear();
    ++m_revision;
    emit revisionChanged();
    rebuildPrograms();
    m_saveLater.stop();
    saveRules();
}

QStringList GestureEditor::visiblePrograms() const
{
    WindowPrograms found;
    found.own = ::GetCurrentProcessId();
    ::EnumWindows(&collectWindow, reinterpret_cast<LPARAM>(&found));
    found.names.sort();
    return found.names;
}

QString GestureEditor::browseFile(const QString& title)
{
    const HWND owner = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    return ws::shell::pickFile(owner, title, {{Text::tr("所有文件"), u"*.*"_s}});
}

QString GestureEditor::keyText(int key, int modifiers, quint32 scanCode) const
{
    Q_UNUSED(scanCode)
    const QString name = keyName(key, modifiers & Qt::ShiftModifier);
    if (name.isEmpty())
        return {};
    QStringList parts;
    if (modifiers & Qt::MetaModifier)
        parts << u"Win"_s;
    if (modifiers & Qt::ControlModifier)
        parts << u"Ctrl"_s;
    if (modifiers & Qt::AltModifier)
        parts << u"Alt"_s;
    if (modifiers & Qt::ShiftModifier)
        parts << u"Shift"_s;
    parts << name;
    const QString text = parts.join(u'+');
    return keysValid(text) ? text : QString();
}

bool GestureEditor::keysValid(const QString& text) const
{
    return gestures::keysAction(wstr(text)).kind == gestures::Action::Kind::Keys;
}

QString GestureEditor::addShortcut(const QString& keys)
{
    gestures::Rule rule;
    rule.on = gestures::keyTrigger(wstr(keys));
    if (rule.on.empty())
        return Text::tr("这个键不能用：同时按住 Ctrl、Alt 或 Win，或者用 F1–F24");
    rule.selection = true;
    if (const int row = m_rules.find(rule); row >= 0) {
        emit shortcutFocused(row);
        return {};
    }
    rule.action = gestures::builtinAction(gestures::Builtin::Translate);
    emit shortcutFocused(m_rules.add(std::move(rule), false));
    return {};
}

QString GestureEditor::setShortcut(int row, const QString& keys)
{
    const auto& rules = m_rules.rules();
    if (row < 0 || row >= static_cast<int>(rules.size()))
        return {};
    gestures::Rule probe = rules[static_cast<std::size_t>(row)];
    probe.on = gestures::keyTrigger(wstr(keys));
    if (probe.on.empty())
        return Text::tr("这个键不能用：同时按住 Ctrl、Alt 或 Win，或者用 F1–F24");
    if (const int other = m_rules.find(probe); other >= 0 && other != row)
        return Text::tr("%1 已经设过了").arg(keys);
    m_rules.setTrigger(row, probe.on);
    return {};
}

void GestureEditor::setRecording(bool recording)
{
    if (m_recording == recording)
        return;
    m_recording = recording;
    emit recordingRequested(recording);
}

} // namespace ws::gestureseditor
