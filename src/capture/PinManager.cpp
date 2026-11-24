#include "PinManager.h"

#include "PictureWrite.h"
#include "platform/Shell.h"

#include <QDateTime>
#include <QDebug>
#include <QFileInfo>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::capture {

namespace {

enum MenuCommand {
    CopyCommand = 1,
    SaveCommand,
    SaveSourceCommand,
    SaveAsCommand,
    EditCommand,
    OcrCommand,
    TopmostCommand,
    ClickThroughCommand,
    CloseCommand,
    CloseAllCommand,
    PickTextCommand,
    RotateRightCommand,
    RotateLeftCommand,
    FlipHorizontalCommand,
    FlipVerticalCommand,
    CollapseTileCommand,
    CollapseEdgeCommand,
    ExpandCommand,
    CloseOthersCommand,
    HideAllCommand,
    AllTopmostCommand,
    OtherSideCommand,
    CopyTranslationCommand,
    TranslateCommand,
    CopyPictureCommand,
    OcrCopyCommand,
    CloseGroupCommand,
    NewGroupMoveCommand,
    ShowAllGroupsCommand
};

constexpr int kMoveGroupBase = 1000;
constexpr int kSwitchGroupBase = 2000;

shell::MenuItem command(int id, QString text, char16_t glyph, bool checked = false, bool enabled = true)
{
    shell::MenuItem item {id, std::move(text), checked, enabled};
    item.glyph = QString(QChar(glyph));
    return item;
}

} // namespace

PinManager::PinManager(Callbacks callbacks, QObject* parent)
    : QObject(parent)
    , m_callbacks(std::move(callbacks))
{
    // Polled, not hooked: a keyboard hook would sit in front of every key in
    // the system (and the double-Ctrl launcher is KeyListener's); this reads
    // Ctrl without touching it, and runs only while a pin is click-through.
    m_watch.setInterval(40);
    m_watch.setTimerType(Qt::CoarseTimer);
    connect(&m_watch, &QTimer::timeout, this, &PinManager::watch);
    m_hintTimer.setSingleShot(true);
    connect(&m_hintTimer, &QTimer::timeout, this, &PinManager::syncHints);
    m_hintClock.start();
}

PinManager::~PinManager()
{
    m_card.reset();
    m_hintPin = nullptr;
    m_pins.clear(); // their windows destroyed without telling anyone
    m_closing.clear();
}

PinWindow* PinManager::pin(const QImage& image, QPoint origin, bool screenshot, QByteArray project,
    std::optional<QList<RedactLine>> recognizedLines)
{
    return add(image, origin, screenshot, std::move(project), std::nullopt, std::move(recognizedLines), true);
}

PinWindow* PinManager::pinFitted(const QImage& image, QPoint origin, QPoint anchor, bool screenshot, QString text,
    std::optional<QList<RedactLine>> recognizedLines)
{
    const bool hasWords = !text.isEmpty();
    PinWindow* pin = add(image, origin, screenshot, {}, anchor, std::move(recognizedLines), !hasWords);
    if (pin && hasWords)
        pin->setCopiedText(std::move(text));
    return pin;
}

PinWindow* PinManager::add(const QImage& image, QPoint origin, bool screenshot, QByteArray project, std::optional<QPoint> fitAbout,
    std::optional<QList<RedactLine>> recognizedLines, bool announceLines)
{
    if (image.isNull())
        return nullptr;
    PinWindow::Host host;
    host.closing = [this](PinWindow* pin) {
        if (m_callbacks.closing)
            m_callbacks.closing(pin);
    };
    host.closed = [this](PinWindow* pin) { closed(pin); };
    host.menu = [this](PinWindow* pin, POINT at) { showMenu(pin, at); };
    host.copy = [this](PinWindow* pin) {
        if (!pin->copiedText().isEmpty() && m_callbacks.copyText)
            m_callbacks.copyText(pin->copiedText()); // the words, as they were copied
        else if (m_callbacks.copy)
            m_callbacks.copy(pin->image(), pin->screenshot());
    };
    host.save = [this](PinWindow* pin) {
        if (!pin->sourcePath().isEmpty()) {
            if (m_callbacks.saveSource)
                m_callbacks.saveSource(pin, pin->image(), pin->sourcePath());
            else if (m_callbacks.save)
                m_callbacks.save(pin->image(), pin->sourcePath());
            return;
        }
        if (m_callbacks.save)
            m_callbacks.save(pin->image(), {});
    };
    host.edit = [this](PinWindow* pin) {
        hintStop(pin);
        if (m_callbacks.edit)
            m_callbacks.edit(pin);
    };
    host.ocr = [this](PinWindow* pin) {
        if (m_callbacks.ocr)
            m_callbacks.ocr(pin->image(), pin->dpr(), pin->pictureRect());
    };
    host.ocrCopy = [this](PinWindow* pin) { copyWords(pin); };
    host.dark = m_callbacks.dark;
    host.accent = m_callbacks.accent;
    host.edge = m_callbacks.edge;
    host.pillRadius = m_callbacks.pillRadius;
    host.copyText = m_callbacks.copyText;
    host.readLines = m_callbacks.readLines;
    host.ocrAvailable = m_callbacks.ocrAvailable;
    host.takenTabs = [this](const PinWindow* self) {
        std::vector<QRect> tabs;
        for (const auto& p : m_pins) {
            if (p.get() != self && !p->groupHidden()) {
                if (const std::optional<QRect> tab = p->edgeTab())
                    tabs.push_back(*tab);
            }
        }
        return tabs;
    };
    host.released = [this](PinWindow* pin) {
        hintStop(pin); // collapsed
        if (m_callbacks.closed)
            m_callbacks.closed(); // its pixels can go back to Windows
    };
    host.focus = [this](PinWindow* pin, bool focused, bool fromOwnWindow) { hintFocus(pin, focused, fromOwnWindow); };
    host.input = [this](PinWindow* pin, PinInput input) { hintInput(pin, input); };
    host.toggleHints = [this](PinWindow* pin) { hintToggle(pin); };
    if (m_hidden)
        setAllHidden(false); // a new pin: they are wanted back
    auto pin = std::make_unique<PinWindow>(image, origin, std::move(host), fitAbout);
    pin->setScreenshot(screenshot);
    pin->setProject(std::move(project));
    pin->setDoubleClick(m_doubleClick);
    pin->setKeys(m_keys);
    pin->setGroupId(m_groups.targetGroupForNewPin());
    if (!pin->hwnd())
        return nullptr;
    if (recognizedLines)
        pin->setRecognizedLines(std::move(*recognizedLines), announceLines && m_autoOcr);
    pin->setAutoOcr(m_autoOcr);
    m_pins.push_back(std::move(pin));
    return m_pins.back().get();
}

bool PinManager::contains(const PinWindow* pin) const
{
    return std::ranges::any_of(m_pins, [pin](const auto& p) { return p.get() == pin; });
}

bool PinManager::hasHistoryPin(quint64 historyId) const
{
    if (historyId == 0)
        return false;
    return std::ranges::any_of(m_pins, [historyId](const auto& p) { return p->historyId() == historyId; });
}

bool PinManager::anyClickThrough() const
{
    return std::ranges::any_of(m_pins, [](const auto& p) { return p->clickThrough() && !p->groupHidden(); });
}

void PinManager::clearClickThrough()
{
    for (const auto& p : m_pins)
        p->setClickThrough(false);
    setWatching();
}

void PinManager::setWatching()
{
    if (anyClickThrough()) {
        if (!m_watch.isActive()) {
            m_watch.start();
            qInfo() << "Screen capture: pin watcher on (a click-through pin)";
        }
    } else if (m_watch.isActive()) {
        m_watch.stop();
        qInfo() << "Screen capture: pin watcher off (no click-through pin)";
    }
}

void PinManager::watch()
{
    const bool ctrl = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    // The pins' pixels are physical whatever the thread's awareness.
    const DPI_AWARENESS_CONTEXT previous = ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    POINT cursor {};
    ::GetCursorPos(&cursor);
    if (previous)
        ::SetThreadDpiAwarenessContext(previous);
    for (const auto& p : m_pins) {
        if (!p->groupHidden())
            p->updateReachable(ctrl, QPoint(cursor.x, cursor.y), p.get() == m_menuPin);
    }
}

void PinManager::closeAll()
{
    // Each close takes it out of m_pins (closed()): from a copy of the list.
    std::vector<PinWindow*> all;
    for (const auto& p : m_pins)
        all.push_back(p.get());
    for (PinWindow* p : all)
        p->close();
}

void PinManager::closeOthers(const PinWindow* keep)
{
    std::vector<PinWindow*> others;
    for (const auto& p : m_pins) {
        if (p.get() != keep && !p->groupHidden())
            others.push_back(p.get());
    }
    for (PinWindow* p : others)
        p->close();
}

void PinManager::closeActiveGroup()
{
    if (m_groups.showingAll()) {
        closeAll();
        return;
    }
    std::vector<PinWindow*> targets;
    for (const auto& p : m_pins) {
        if (m_groups.normalizePinGroup(p->groupId()) == m_groups.activeGroupId())
            targets.push_back(p.get());
    }
    for (PinWindow* p : targets)
        p->close();
}

// ---- pin groups (#451) ------------------------------------------------------------------------

void PinManager::syncGroupVisibility()
{
    for (const auto& p : m_pins) {
        const QString normalized = m_groups.normalizePinGroup(p->groupId());
        if (normalized != p->groupId())
            p->setGroupId(normalized);
        p->setGroupHidden(!m_groups.isPinVisibleInActiveView(normalized));
    }
    if (m_hintPin && m_hintPin->groupHidden())
        m_hints.stop();
    setWatching();
    syncHints();
}

void PinManager::loadGroups(const QString& serialized, const QString& activeToken)
{
    m_groups.loadGroups(serialized);
    m_groups.setActiveStateToken(activeToken);
    syncGroupVisibility();
}

std::optional<PinGroup> PinManager::createGroup(const QString& name, PinWindow* movePin, QString* error)
{
    const std::optional<PinGroup> created = m_groups.createGroup(name, error);
    if (!created)
        return std::nullopt;
    if (movePin && contains(movePin)) {
        movePin->setGroupId(created->id);
        syncGroupVisibility();
        if (!m_groups.showingAll() && created->id != m_groups.activeGroupId()) {
            if (m_callbacks.groupNotice)
                m_callbacks.groupNotice(tr("已新建分组「%1」并移入贴图").arg(created->name));
        } else {
            movePin->note(tr("已移至「%1」").arg(created->name));
        }
    }
    if (m_callbacks.groupsChanged)
        m_callbacks.groupsChanged();
    return created;
}

bool PinManager::renameGroup(const QString& id, const QString& name, QString* error)
{
    if (!m_groups.renameGroup(id, name, error))
        return false;
    if (m_callbacks.groupsChanged)
        m_callbacks.groupsChanged();
    return true;
}

bool PinManager::deleteGroup(const QString& id)
{
    if (!m_groups.deleteGroup(id))
        return false;
    // Non-destructive: rescue pins in the deleted group back into the default group.
    for (const auto& p : m_pins) {
        if (p->groupId() == id)
            p->setGroupId({});
    }
    syncGroupVisibility();
    if (m_callbacks.groupsChanged)
        m_callbacks.groupsChanged();
    return true;
}

void PinManager::switchGroup(const QString& id)
{
    m_groups.setActiveGroup(id);
    if (m_hidden)
        setAllHidden(false);
    syncGroupVisibility();
    if (m_callbacks.groupsChanged)
        m_callbacks.groupsChanged();
}

void PinManager::showAllGroups()
{
    m_groups.setShowingAll(true);
    if (m_hidden)
        setAllHidden(false);
    syncGroupVisibility();
    if (m_callbacks.groupsChanged)
        m_callbacks.groupsChanged();
}

std::optional<QString> PinManager::cycleGroup()
{
    const std::optional<QString> next = m_groups.nextGroupId();
    if (!next)
        return std::nullopt;
    switchGroup(*next);
    return *next;
}

void PinManager::movePinToGroup(PinWindow* pin, const QString& groupId)
{
    if (!pin || !contains(pin))
        return;
    const QString target = m_groups.normalizePinGroup(groupId);
    if (pin->groupId() == target)
        return;
    pin->setGroupId(target);
    syncGroupVisibility();
    const QString label = m_groups.nameOf(target);
    if (pin->groupHidden()) {
        if (m_callbacks.groupNotice)
            m_callbacks.groupNotice(tr("已移至分组「%1」").arg(label));
    } else {
        pin->note(tr("已移至「%1」").arg(label));
    }
}

int PinManager::pinCountInGroup(const QString& groupId) const
{
    const QString target = m_groups.normalizePinGroup(groupId);
    return static_cast<int>(std::ranges::count_if(m_pins, [&](const auto& p) {
        return m_groups.normalizePinGroup(p->groupId()) == target;
    }));
}

int PinManager::visiblePinCount() const
{
    return static_cast<int>(std::ranges::count_if(m_pins, [](const auto& p) { return !p->groupHidden(); }));
}

void PinManager::setAllHidden(bool hidden)
{
    m_hidden = hidden;
    for (const auto& p : m_pins)
        p->setAllHidden(hidden);
    if (hidden)
        m_hints.stop();
    syncHints();
}

void PinManager::restyle()
{
    for (const auto& p : m_pins)
        p->restyle();
    syncHints(); // the card in the new colours
}

bool PinManager::allTopmost() const
{
    return !m_pins.empty() && std::ranges::all_of(m_pins, [](const auto& p) { return p->topmost(); });
}

void PinManager::setAllTopmost(bool on)
{
    for (const auto& p : m_pins)
        p->setTopmost(on);
}

void PinManager::setDoubleClick(PinDoubleClick action)
{
    m_doubleClick = action;
    for (const auto& p : m_pins)
        p->setDoubleClick(action);
}

void PinManager::setKeys(const CaptureKeys& keys)
{
    m_keys = keys;
    for (const auto& p : m_pins)
        p->setKeys(keys);
    syncHints();
}

void PinManager::setKeyHints(bool on)
{
    m_hints.setEnabled(on);
    syncHints();
}

void PinManager::setAutoOcr(bool on)
{
    m_autoOcr = on;
    for (const auto& p : m_pins)
        p->setAutoOcr(on);
}

// ---- the key card (GitHub #343) ---------------------------------------------------------------

void PinManager::hintFocus(PinWindow* pin, bool focused, bool fromOwnWindow)
{
    if (focused) {
        // The same round goes on (PinHintPolicy rule 3) for the pin back from its
        // own menu or another of the app's windows, and for the pin with the
        // focus told so again (a second WM_ACTIVATE, no WA_INACTIVE between).
        m_hints.focus(m_hintClock.elapsed(), pin == m_lastHintPin && (fromOwnWindow || pin == m_hintPin));
        m_hintPin = pin;
        m_lastHintPin = pin;
        if (pin->form() != PinForm::Full)
            m_hints.stop(); // a tile or a tab: the card's keys are not its keys
    } else if (pin == m_hintPin) {
        m_hints.blur();
        m_hintPin = nullptr;
    }
    syncHints();
}

void PinManager::hintInput(PinWindow* pin, PinInput input)
{
    if (pin != m_hintPin)
        return;
    const qint64 now = m_hintClock.elapsed();
    switch (input) {
    case PinInput::Press: m_hints.press(now); break;
    case PinInput::Release: m_hints.release(now); break;
    case PinInput::Key: m_hints.input(now); break;
    }
    syncHints();
    // Told before the pin acts on it: once it has (moved, zoomed, collapsed, closed), the card again.
    QMetaObject::invokeMethod(this, &PinManager::syncHints, Qt::QueuedConnection);
}

void PinManager::hintToggle(PinWindow* pin)
{
    if (pin->form() != PinForm::Full)
        return;
    if (pin != m_hintPin) { // keys go to the pin with the focus, which should have said so
        m_hints.focus(m_hintClock.elapsed(), false);
        m_hintPin = pin;
        m_lastHintPin = pin;
    }
    m_hints.toggle();
    syncHints();
}

void PinManager::hintStop(const PinWindow* pin)
{
    if (pin != m_lastHintPin) // the round is the last focused pin's, the focus there or away
        return;
    m_hints.stop();
    syncHints();
}

void PinManager::syncHints()
{
    const qint64 now = m_hintClock.elapsed();
    m_hints.tick(now);
    // The focus comes in a pin's constructor, before it is one of m_pins: shown later.
    PinWindow* pin = m_hintPin && contains(m_hintPin) ? m_hintPin : nullptr;
    bool shown = false;
    if (pin && m_hints.shown() && !m_hidden && pin->form() == PinForm::Full && pin->hwnd()) {
        const bool ocr = m_callbacks.readLines && m_callbacks.ocrAvailable && m_callbacks.ocrAvailable();
        const std::vector<PinKeyRow> rows = pinKeyRows(m_keys, ocr, pin->hasOtherSide(), pin->translationShown());
        const PinKeyCard::Look look = m_callbacks.keyCardLook ? m_callbacks.keyCardLook()
                                                              : PinKeyCard::Look::fallback(m_callbacks.dark && m_callbacks.dark());
        MONITORINFO monitor {};
        monitor.cbSize = sizeof monitor;
        if (::GetMonitorInfoW(::MonitorFromWindow(pin->hwnd(), MONITOR_DEFAULTTONEAREST), &monitor)) {
            const RECT& w = monitor.rcWork;
            if (!m_card)
                m_card = std::make_unique<PinKeyCard>();
            shown = m_card->show(rows, QRect(w.left, w.top, w.right - w.left, w.bottom - w.top), pin->pictureRect(),
                pin->dpr(), look);
        }
    }
    if (!shown)
        m_card.reset(); // its window and pixels go with it
    const qint64 next = m_hints.nextDeadline();
    if (next >= 0)
        m_hintTimer.start(static_cast<int>(std::clamp<qint64>(next - now, 0, 60'000)));
    else
        m_hintTimer.stop();
}

void PinManager::closed(PinWindow* pin)
{
    if (pin == m_lastHintPin) { // its round is over (rule 6)
        m_lastHintPin = nullptr;
        m_hints.stop();
        if (pin == m_hintPin) {
            m_hints.blur();
            m_hintPin = nullptr;
        }
        syncHints();
    }
    // Called from inside its window procedure: deleted once that returns.
    const auto found = std::ranges::find(m_pins, pin, &std::unique_ptr<PinWindow>::get);
    if (found == m_pins.end())
        return;
    m_closing.push_back(std::move(*found));
    m_pins.erase(found);
    setWatching();
    QMetaObject::invokeMethod(this, [this] {
        m_closing.clear();
        if (m_callbacks.closed)
            m_callbacks.closed();
    }, Qt::QueuedConnection);
}

void PinManager::copyWords(PinWindow* pin)
{
    if (!pin)
        return;
    // Copied text pinned (#388) or a translated pin currently showing its
    // translation: copy the exact text directly without re-running OCR.
    if (!pin->copiedText().isEmpty() && m_callbacks.copyText) {
        m_callbacks.copyText(pin->copiedText());
        pin->showCopiedAllPill();
    } else if (pin->hasOtherSide() && pin->translationShown() && !pin->translatedWords().isEmpty() && m_callbacks.copyText) {
        m_callbacks.copyText(pin->translatedWords());
        pin->showCopiedAllPill();
    } else if (pin->copyAllRecognizedText()) {
        return;
    } else if (m_callbacks.ocrCopy) {
        m_callbacks.ocrCopy(pin->image(), pin->dpr(), pin->pictureRect());
    }
}

void PinManager::showMenu(PinWindow* pin, POINT at)
{
    const bool ocrAvailable = m_callbacks.ocrAvailable && m_callbacks.ocrAvailable();
    const QString ocr = ocrAvailable ? tr("识别文字") : tr("识别文字（安装增强识别…）");
    const bool full = pin->form() == PinForm::Full;
    // The key after a tab; nothing when the action has none.
    const auto hint = [this](KeyAction action) { return m_keys.has(action) ? u"\t"_s + m_keys.text(action) : QString(); };
    auto turn = command(0, tr("旋转与翻转"), 0xE777);
    turn.children = {
        command(RotateRightCommand, tr("向右转 90°") + hint(KeyAction::PinRotateRight), 0xE72C),
        command(RotateLeftCommand, tr("向左转 90°") + hint(KeyAction::PinRotateLeft), 0xE777),
        command(FlipHorizontalCommand, tr("左右翻转") + hint(KeyAction::PinFlipHorizontal), 0xE8DE),
        command(FlipVerticalCommand, tr("上下翻转") + hint(KeyAction::PinFlipVertical), 0xE8CB),
    };
    auto collapse = command(0, tr("收起"), 0xE73F);
    collapse.children = {
        command(CollapseTileCommand, tr("收成小块") + hint(KeyAction::PinCollapse), 0xE73F),
        command(CollapseEdgeCommand, tr("收到屏幕顶边") + hint(KeyAction::PinCollapseEdge), 0xE74A),
    };
    const std::vector<PinGroup> groupList = m_groups.groups();
    const QString pinGroup = m_groups.normalizePinGroup(pin->groupId());
    auto groupMenu = command(0, tr("分组"), 0xE8B7); // Folder / Group
    for (int i = 0; i < static_cast<int>(groupList.size()); ++i) {
        const PinGroup& g = groupList[static_cast<std::size_t>(i)];
        groupMenu.children.push_back(command(kMoveGroupBase + i, g.name, 0xE8B7, pinGroup == g.id));
    }
    groupMenu.children.push_back(shell::MenuItem::separator());
    groupMenu.children.push_back(command(NewGroupMoveCommand, tr("新建分组并移入…"), 0xE710)); // Add
    if (m_groups.hasCustomGroups()) {
        groupMenu.children.push_back(shell::MenuItem::separator());
        auto switchMenu = command(0, tr("切换当前分组"), 0xE8AB); // Switch
        for (int i = 0; i < static_cast<int>(groupList.size()); ++i) {
            const PinGroup& g = groupList[static_cast<std::size_t>(i)];
            const int n = pinCountInGroup(g.id);
            const QString label = n > 0 ? tr("%1（%2）").arg(g.name).arg(n) : g.name;
            switchMenu.children.push_back(
                command(kSwitchGroupBase + i, label, 0xE8B7, !m_groups.showingAll() && m_groups.activeGroupId() == g.id));
        }
        switchMenu.children.push_back(shell::MenuItem::separator());
        switchMenu.children.push_back(command(ShowAllGroupsCommand,
            tr("显示全部分组（%1）").arg(count()), 0xE8A9, m_groups.showingAll()));
        groupMenu.children.push_back(std::move(switchMenu));
    }

    auto all = command(0, tr("全部贴图"), 0xE8A9);
    all.children = {
        command(HideAllCommand, m_hidden ? tr("显示全部贴图") : tr("隐藏全部贴图"), 0xE890),
        command(AllTopmostCommand, tr("全部置顶"), 0xE718, allTopmost()),
    };
    if (m_groups.hasCustomGroups() && !m_groups.showingAll())
        all.children.push_back(command(CloseGroupCommand, tr("关闭当前分组（%1）").arg(m_groups.nameOf(m_groups.activeGroupId())), 0xE8BB));
    all.children.push_back(command(CloseAllCommand, tr("关闭全部贴图"), 0xE8BB));
    // Copied words pinned (GitHub #388) or text currently highlighted on the pin:
    // 复制 takes the text, with 复制图片 offered right beside it.
    const bool words = !pin->copiedText().isEmpty();
    const bool picked = full && pin->hasPickedText();
    const QString copyLabel = picked ? tr("复制选中文字") : words ? tr("复制文字") : tr("复制");
    std::vector<shell::MenuItem> items {command(CopyCommand, copyLabel + hint(KeyAction::PinCopy), 0xE8C8)};
    if (picked && words)
        items.push_back(command(OcrCopyCommand, tr("复制全部文字") + hint(KeyAction::PinOcrCopy), 0xE8C8));
    if (picked || words)
        items.push_back(command(CopyPictureCommand, tr("复制图片"), 0xE8C8));
    const bool hasSource = !pin->sourcePath().isEmpty();
    if (hasSource) {
        items.push_back(command(SaveSourceCommand,
            tr("保存到原文件（%1）").arg(shortFileName(pin->sourcePath())) + hint(KeyAction::PinSave), 0xE74E));
        items.push_back(command(SaveCommand, tr("保存到“屏幕截图”"), 0xE74E));
    } else {
        items.push_back(command(SaveCommand, tr("保存到“屏幕截图”") + hint(KeyAction::PinSave), 0xE74E));
    }
    items.push_back(command(SaveAsCommand, tr("另存为…"), 0xE74E));
    items.push_back(shell::MenuItem::separator());
    if (full && pin->hasOtherSide()) { // 截图翻译 (#191): the translation is one side, the original the other
        items.push_back(command(OtherSideCommand, (pin->translationShown() ? tr("显示原文") : tr("显示译文")) + u"\tTab"_s, 0xE8C1));
        items.push_back(command(CopyTranslationCommand, tr("复制译文"), 0xE8C8));
        items.push_back(shell::MenuItem::separator());
    }
    if (full) {
        items.push_back(command(EditCommand, tr("编辑") + hint(KeyAction::PinEdit), 0xE70F));
        items.push_back(command(OcrCommand, ocr + hint(KeyAction::PinOcr), 0xE8A5));
        if (!words)
            items.push_back(command(OcrCopyCommand, tr("复制全部文字") + hint(KeyAction::PinOcrCopy), 0xE8C8));
        if (m_callbacks.translate && !pin->hasOtherSide())
            items.push_back(command(TranslateCommand, tr("翻译"), 0xE8C1, false, ocrAvailable));
        items.push_back(command(PickTextCommand, tr("选择文字") + hint(KeyAction::PinTextPick), 0xE8D2,
            pin->textMode(), (ocrAvailable && pin->canPickText()) || pin->hasRecognizedLines()));
        items.push_back(shell::MenuItem::separator());
        items.push_back(std::move(turn));
        items.push_back(std::move(collapse));
        items.push_back(shell::MenuItem::separator());
        items.push_back(command(TopmostCommand, tr("置顶"), 0xE718, pin->topmost()));
        items.push_back(command(ClickThroughCommand, tr("鼠标穿透（按住 Ctrl 可操作）"), 0xE7C9, pin->clickThrough()));
    } else {
        items.push_back(command(ExpandCommand, tr("展开"), 0xE740));
        items.push_back(shell::MenuItem::separator());
        items.push_back(command(TopmostCommand, tr("置顶"), 0xE718, pin->topmost()));
    }
    items.push_back(shell::MenuItem::separator());
    items.push_back(std::move(groupMenu));
    items.push_back(command(CloseCommand, tr("关闭") + hint(KeyAction::PinClose), 0xE8BB));
    items.push_back(command(CloseOthersCommand, tr("关闭其他"), 0xE8BB));
    items.push_back(std::move(all));
    const HWND hwnd = pin->hwnd();
    m_menuPin = pin; // the pin keeps the mouse while its menu is up
    const int chosen = m_callbacks.popupMenu ? m_callbacks.popupMenu(hwnd, items, at) : shell::popupMenu(hwnd, items, at);
    m_menuPin = nullptr;
    // The pin may have gone while the menu was up (the tray closed them all).
    if (std::ranges::find(m_pins, pin, &std::unique_ptr<PinWindow>::get) == m_pins.end())
        return;
    switch (chosen) {
    case CopyCommand:
        if (picked)
            pin->copyPickedText();
        else if (words && m_callbacks.copyText)
            m_callbacks.copyText(pin->copiedText());
        else if (m_callbacks.copy)
            m_callbacks.copy(pin->image(), pin->screenshot());
        break;
    case CopyPictureCommand:
        if (m_callbacks.copy)
            m_callbacks.copy(pin->image(), pin->screenshot());
        break;
    case SaveSourceCommand:
        if (!pin->sourcePath().isEmpty()) {
            if (m_callbacks.saveSource)
                m_callbacks.saveSource(pin, pin->image(), pin->sourcePath());
            else if (m_callbacks.save)
                m_callbacks.save(pin->image(), pin->sourcePath());
        }
        break;
    case SaveCommand:
        if (m_callbacks.save)
            m_callbacks.save(pin->image(), {});
        break;
    case SaveAsCommand: {
        const QString source = pin->sourcePath();
        const QString name = source.isEmpty() ? screenshotFileName(QDateTime::currentDateTime(), {}) : QFileInfo(source).fileName();
        const QString suffix = source.isEmpty() ? u"png"_s : QFileInfo(source).suffix().toLower();
        const QString defaultExt = isWritableImageExtension(u"x."_s + suffix) ? suffix : u"png"_s;
        const QString filterName = defaultExt == u"jpg" || defaultExt == u"jpeg" || defaultExt == u"jpe" || defaultExt == u"jfif"
            ? tr("JPEG 图片")
            : defaultExt == u"bmp" || defaultExt == u"dib" ? tr("BMP 图片")
            : defaultExt == u"tif" || defaultExt == u"tiff" ? tr("TIFF 图片")
                                                            : tr("PNG 图片");
        const QString path = shell::pickSaveFile(hwnd, tr("另存为"), name, filterName, defaultExt);
        if (!path.isEmpty() && m_callbacks.save && std::ranges::find(m_pins, pin, &std::unique_ptr<PinWindow>::get) != m_pins.end())
            m_callbacks.save(pin->image(), path);
        break;
    }
    case EditCommand:
        hintStop(pin);
        if (m_callbacks.edit)
            m_callbacks.edit(pin);
        break;
    case OcrCommand:
        if (m_callbacks.ocr)
            m_callbacks.ocr(pin->image(), pin->dpr(), pin->pictureRect());
        break;
    case OcrCopyCommand:
        copyWords(pin);
        break;
    case OtherSideCommand:
        pin->showOtherSide();
        break;
    case TranslateCommand:
        if (m_callbacks.translate)
            m_callbacks.translate(pin);
        break;
    case CopyTranslationCommand:
        if (m_callbacks.copyText)
            m_callbacks.copyText(pin->translatedWords());
        break;
    case PickTextCommand:
        if (pin->textMode())
            pin->endTextPick();
        else
            pin->beginTextPick();
        break;
    case RotateRightCommand:
        pin->rotate(true);
        break;
    case RotateLeftCommand:
        pin->rotate(false);
        break;
    case FlipHorizontalCommand:
        pin->flip(true);
        break;
    case FlipVerticalCommand:
        pin->flip(false);
        break;
    case CollapseTileCommand:
        pin->collapse(PinForm::Tile);
        break;
    case CollapseEdgeCommand:
        pin->collapse(PinForm::Edge);
        break;
    case ExpandCommand:
        pin->expand();
        break;
    case TopmostCommand:
        pin->setTopmost(!pin->topmost());
        break;
    case ClickThroughCommand:
        pin->setClickThrough(!pin->clickThrough());
        setWatching();
        break;
    case CloseCommand:
        pin->close();
        break;
    case CloseOthersCommand:
        closeOthers(pin);
        break;
    case CloseGroupCommand:
        closeActiveGroup();
        break;
    case CloseAllCommand:
        closeAll();
        break;
    case HideAllCommand:
        setAllHidden(!m_hidden);
        break;
    case AllTopmostCommand:
        setAllTopmost(!allTopmost());
        break;
    case NewGroupMoveCommand:
        if (m_callbacks.requestNewGroup)
            m_callbacks.requestNewGroup(pin);
        break;
    case ShowAllGroupsCommand:
        showAllGroups();
        break;
    default:
        if (chosen >= kMoveGroupBase && chosen < kMoveGroupBase + static_cast<int>(groupList.size())) {
            movePinToGroup(pin, groupList[static_cast<std::size_t>(chosen - kMoveGroupBase)].id);
        } else if (chosen >= kSwitchGroupBase && chosen < kSwitchGroupBase + static_cast<int>(groupList.size())) {
            switchGroup(groupList[static_cast<std::size_t>(chosen - kSwitchGroupBase)].id);
        }
        break;
    }
}

} // namespace ws::capture
