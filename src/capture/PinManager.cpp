#include "PinManager.h"

#include "platform/Shell.h"

#include <QDateTime>
#include <QDebug>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::capture {

namespace {

enum MenuCommand {
    CopyCommand = 1,
    SaveCommand,
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
    AllTopmostCommand
};

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
}

PinManager::~PinManager()
{
    m_pins.clear(); // their windows destroyed without telling anyone
    m_closing.clear();
}

void PinManager::pin(const QImage& image, QPoint origin, bool screenshot, QByteArray project)
{
    add(image, origin, screenshot, std::move(project), std::nullopt);
}

void PinManager::pinFitted(const QImage& image, QPoint origin, QPoint anchor, bool screenshot)
{
    add(image, origin, screenshot, {}, anchor);
}

void PinManager::add(const QImage& image, QPoint origin, bool screenshot, QByteArray project, std::optional<QPoint> fitAbout)
{
    if (image.isNull())
        return;
    PinWindow::Host host;
    host.closed = [this](PinWindow* pin) { closed(pin); };
    host.menu = [this](PinWindow* pin, POINT at) { showMenu(pin, at); };
    host.copy = [this](PinWindow* pin) {
        if (m_callbacks.copy)
            m_callbacks.copy(pin->image(), pin->screenshot());
    };
    host.save = [this](PinWindow* pin) {
        if (m_callbacks.save)
            m_callbacks.save(pin->image(), {});
    };
    host.edit = [this](PinWindow* pin) {
        if (m_callbacks.edit)
            m_callbacks.edit(pin);
    };
    host.dark = m_callbacks.dark;
    host.accent = m_callbacks.accent;
    host.edge = m_callbacks.edge;
    host.pillRadius = m_callbacks.pillRadius;
    host.copyText = m_callbacks.copyText;
    host.readLines = m_callbacks.readLines;
    host.takenTabs = [this](const PinWindow* self) {
        std::vector<QRect> tabs;
        for (const auto& p : m_pins) {
            if (p.get() != self) {
                if (const std::optional<QRect> tab = p->edgeTab())
                    tabs.push_back(*tab);
            }
        }
        return tabs;
    };
    host.released = [this](PinWindow*) {
        if (m_callbacks.closed)
            m_callbacks.closed(); // its pixels can go back to Windows
    };
    if (m_hidden)
        setAllHidden(false); // a new pin: they are wanted back
    auto pin = std::make_unique<PinWindow>(image, origin, std::move(host), fitAbout);
    pin->setScreenshot(screenshot);
    pin->setProject(std::move(project));
    pin->setDoubleClick(m_doubleClick);
    pin->setKeys(m_keys);
    if (pin->hwnd())
        m_pins.push_back(std::move(pin));
}

bool PinManager::contains(const PinWindow* pin) const
{
    return std::ranges::any_of(m_pins, [pin](const auto& p) { return p.get() == pin; });
}

bool PinManager::anyClickThrough() const
{
    return std::ranges::any_of(m_pins, [](const auto& p) { return p->clickThrough(); });
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
    for (const auto& p : m_pins)
        p->updateReachable(ctrl, QPoint(cursor.x, cursor.y), p.get() == m_menuPin);
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
        if (p.get() != keep)
            others.push_back(p.get());
    }
    for (PinWindow* p : others)
        p->close();
}

void PinManager::setAllHidden(bool hidden)
{
    m_hidden = hidden;
    for (const auto& p : m_pins)
        p->setAllHidden(hidden);
}

void PinManager::restyle()
{
    for (const auto& p : m_pins)
        p->restyle();
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
}

void PinManager::closed(PinWindow* pin)
{
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
        command(CollapseEdgeCommand, tr("收到屏幕顶边"), 0xE74A),
    };
    auto all = command(0, tr("全部贴图"), 0xE8A9);
    all.children = {
        command(HideAllCommand, m_hidden ? tr("显示全部贴图") : tr("隐藏全部贴图"), 0xE890),
        command(AllTopmostCommand, tr("全部置顶"), 0xE718, allTopmost()),
        command(CloseAllCommand, tr("关闭全部贴图"), 0xE8BB),
    };
    std::vector<shell::MenuItem> items {
        command(CopyCommand, tr("复制") + hint(KeyAction::PinCopy), 0xE8C8),
        command(SaveCommand, tr("保存到“屏幕截图”") + hint(KeyAction::PinSave), 0xE74E),
        command(SaveAsCommand, tr("另存为…"), 0xE74E),
        shell::MenuItem::separator(),
    };
    if (full) {
        items.push_back(command(EditCommand, tr("编辑") + hint(KeyAction::PinEdit), 0xE70F));
        items.push_back(command(OcrCommand, ocr, 0xE8A5));
        items.push_back(command(PickTextCommand, tr("选择文字") + hint(KeyAction::PinTextPick), 0xE8D2,
            pin->textMode(), ocrAvailable && pin->canPickText()));
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
        if (m_callbacks.copy)
            m_callbacks.copy(pin->image(), pin->screenshot());
        break;
    case SaveCommand:
        if (m_callbacks.save)
            m_callbacks.save(pin->image(), {});
        break;
    case SaveAsCommand: {
        const QString name = screenshotFileName(QDateTime::currentDateTime(), {});
        const QString path = shell::pickSaveFile(hwnd, tr("另存为"), name, tr("PNG 图片"), u"png"_s);
        if (!path.isEmpty() && m_callbacks.save && std::ranges::find(m_pins, pin, &std::unique_ptr<PinWindow>::get) != m_pins.end())
            m_callbacks.save(pin->image(), path);
        break;
    }
    case EditCommand:
        if (m_callbacks.edit)
            m_callbacks.edit(pin);
        break;
    case OcrCommand:
        if (m_callbacks.ocr)
            m_callbacks.ocr(pin->image(), pin->dpr(), pin->pictureRect());
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
    case CloseAllCommand:
        closeAll();
        break;
    case HideAllCommand:
        setAllHidden(!m_hidden);
        break;
    case AllTopmostCommand:
        setAllTopmost(!allTopmost());
        break;
    default:
        break;
    }
}

} // namespace ws::capture
