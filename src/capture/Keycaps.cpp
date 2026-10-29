#include "Keycaps.h"

#include "../core/OwnInput.h" // header only: the marks on Win顺's own input

#include <QRect>

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cmath>

using namespace Qt::StringLiterals;

namespace ws::capture::keycaps {

namespace {

bool printable(char16_t c)
{
    return c >= 0x20 && c != 0x7F && (c < 0x80 || c >= 0xA0);
}

} // namespace

Mode modeFromName(QStringView name)
{
    if (name == u"shortcuts")
        return Mode::Shortcuts;
    if (name == u"all")
        return Mode::All;
    return Mode::Off;
}

QString modeName(Mode mode)
{
    switch (mode) {
    case Mode::Shortcuts:
        return u"shortcuts"_s;
    case Mode::All:
        return u"all"_s;
    case Mode::Off:
        break;
    }
    return u"off"_s;
}

bool hidden(std::uint32_t vk, std::uintptr_t extraInfo, std::uint32_t /*flags*/) noexcept
{
    // The hook's flags make no difference: a key another program sends is
    // marked injected (LLKHF_INJECTED) and is shown all the same.
    return isOwnInput(extraInfo) || vk == kMaskKey || vk == VK_PACKET || vk == kHiddenKey || vk == kAltGrKey || vk > 0xFE;
}

bool isMouse(std::uint16_t vk) noexcept
{
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

std::uint8_t modifierOf(std::uint16_t vk) noexcept
{
    switch (vk) {
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return kCtrl;
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return kShift;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return kAlt;
    case VK_LWIN:
    case VK_RWIN:
        return kWin;
    case kAltGrKey:
        return kAltGr;
    default:
        return 0;
    }
}

bool isCommandKey(std::uint16_t vk) noexcept
{
    if (vk >= VK_F1 && vk <= VK_F24)
        return true;
    switch (vk) {
    case VK_ESCAPE:
    case VK_RETURN:
    case VK_TAB:
    case VK_BACK:
    case VK_DELETE:
    case VK_INSERT:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_UP:
    case VK_RIGHT:
    case VK_DOWN:
    case VK_SNAPSHOT:
    case VK_PAUSE:
    case VK_APPS:
    case VK_VOLUME_MUTE:
    case VK_VOLUME_DOWN:
    case VK_VOLUME_UP:
    case VK_MEDIA_NEXT_TRACK:
    case VK_MEDIA_PREV_TRACK:
    case VK_MEDIA_STOP:
    case VK_MEDIA_PLAY_PAUSE:
        return true;
    default:
        return false;
    }
}

bool passes(Mode mode, std::uint16_t vk, std::uint8_t mods) noexcept
{
    switch (mode) {
    case Mode::All:
        return true;
    case Mode::Shortcuts:
        return isMouse(vk) || modifierOf(vk) || isCommandKey(vk) || (mods & kCommandMods);
    case Mode::Off:
        break;
    }
    return isMouse(vk);
}

QString keyName(std::uint16_t vk, char16_t legend, const Words& words)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return QString(QChar(vk));
    if (vk >= VK_F1 && vk <= VK_F24)
        return u"F%1"_s.arg(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return u"Num %1"_s.arg(vk - VK_NUMPAD0);
    switch (vk) {
    case VK_LBUTTON:
        return words.left;
    case VK_RBUTTON:
        return words.right;
    case VK_MBUTTON:
        return words.middle;
    case VK_XBUTTON1:
        return words.back;
    case VK_XBUTTON2:
        return words.forward;
    case VK_SPACE:
        return words.space;
    case VK_BACK:
        return u"Backspace"_s;
    case VK_TAB:
        return u"Tab"_s;
    case VK_RETURN:
        return u"Enter"_s;
    case VK_ESCAPE:
        return u"Esc"_s;
    case VK_PAUSE:
        return u"Pause"_s;
    case VK_CAPITAL:
        return u"CapsLock"_s;
    case VK_PRIOR:
        return u"PgUp"_s;
    case VK_NEXT:
        return u"PgDn"_s;
    case VK_END:
        return u"End"_s;
    case VK_HOME:
        return u"Home"_s;
    case VK_LEFT:
        return u"←"_s;
    case VK_UP:
        return u"↑"_s;
    case VK_RIGHT:
        return u"→"_s;
    case VK_DOWN:
        return u"↓"_s;
    case VK_SNAPSHOT:
        return u"PrtSc"_s;
    case VK_INSERT:
        return u"Insert"_s;
    case VK_DELETE:
        return u"Delete"_s;
    case VK_APPS:
        return u"Menu"_s;
    case VK_MULTIPLY:
        return u"Num *"_s;
    case VK_ADD:
        return u"Num +"_s;
    case VK_SUBTRACT:
        return u"Num -"_s;
    case VK_DECIMAL:
        return u"Num ."_s;
    case VK_DIVIDE:
        return u"Num /"_s;
    case VK_NUMLOCK:
        return u"NumLock"_s;
    case VK_SCROLL:
        return u"ScrollLock"_s;
    case VK_VOLUME_MUTE:
        return u"Mute"_s;
    case VK_VOLUME_DOWN:
        return u"Volume −"_s;
    case VK_VOLUME_UP:
        return u"Volume +"_s;
    case VK_MEDIA_NEXT_TRACK:
        return u"Next track"_s;
    case VK_MEDIA_PREV_TRACK:
        return u"Previous track"_s;
    case VK_MEDIA_STOP:
        return u"Stop"_s;
    case VK_MEDIA_PLAY_PAUSE:
        return u"Play/Pause"_s;
    default:
        break;
    }
    // Punctuation (VK_OEM_*): what the layout prints on it.
    if (vk >= VK_OEM_1 && vk <= VK_OEM_102 && printable(legend))
        return QString(QChar(legend)).toUpper();
    return {};
}

QStringList modifierNames(std::uint8_t mods)
{
    QStringList names;
    if (mods & kWin)
        names << u"Win"_s;
    if (mods & kCtrl)
        names << u"Ctrl"_s;
    if (mods & kAlt)
        names << u"Alt"_s;
    if (mods & kAltGr)
        names << u"AltGr"_s;
    if (mods & kShift)
        names << u"Shift"_s;
    return names;
}

Hotkey fromRegistered(unsigned modifiers, unsigned vk) noexcept
{
    std::uint8_t mods = 0;
    if (modifiers & MOD_ALT)
        mods |= kAlt;
    if (modifiers & MOD_CONTROL)
        mods |= kCtrl;
    if (modifiers & MOD_SHIFT)
        mods |= kShift;
    if (modifiers & MOD_WIN)
        mods |= kWin;
    return {mods, static_cast<std::uint16_t>(vk)};
}

Model::Model(Config config)
    : m_config(std::move(config))
{
}

void Model::feed(const Key& key)
{
    if (isMouse(key.vk)) {
        mouse(key);
        return;
    }
    if (modifierOf(key.vk)) {
        modifier(key);
        return;
    }
    if (!key.down)
        return;
    spoil(); // a modifier held over it is no tap
    if (key.vk == kHiddenKey || m_config.mode == Mode::Off || own(key) || !passes(m_config.mode, key.vk, key.mods))
        return;
    const bool command = (key.mods & kCommandMods) != 0;
    // All mode: typing as text. A space starts no text row of its own: it
    // would show as an empty one.
    if (m_config.mode == Mode::All && !command && printable(key.text[0]) && (key.vk != VK_SPACE || typing(key.time))) {
        const qsizetype length = printable(key.text[1]) ? 2 : 1;
        type(QStringView(key.text.data(), length), key.time);
        return;
    }
    const QString name = keyName(key.vk, key.legend, m_config.words);
    if (name.isEmpty())
        return;
    QStringList caps = modifierNames(key.mods);
    caps << name;
    if (key.repeat) {
        // Held: its row stays while it is, counted once.
        if (!m_rows.empty() && !m_rows.back().text && m_rows.back().caps == caps && key.time < m_rows.back().last + kLinger)
            m_rows.back().last = std::max(m_rows.back().last, key.time);
        return;
    }
    add(std::move(caps), key.time);
}

void Model::mouse(const Key& key)
{
    if (!key.down)
        return;
    spoil(); // Ctrl+click, Alt+drag: no tap of the modifier
    if (!m_config.mouse || !key.inside || key.repeat)
        return;
    QStringList caps = modifierNames(key.mods);
    caps << keyName(key.vk, 0, m_config.words);
    add(std::move(caps), key.time);
}

void Model::modifier(const Key& key)
{
    if (key.repeat)
        return;
    const std::uint8_t bit = modifierOf(key.vk);
    const std::uint8_t others = key.mods & ~bit;
    if (key.down) {
        if (!others) {
            m_hold = {bit, false, key.time};
        } else if (!m_hold.pressed || (others & ~m_hold.pressed)) {
            // Held from before the hook saw it (a window of Win顺's was in
            // front): not alone, whatever comes.
            m_hold.pressed |= bit | others;
            m_hold.spoiled = true;
        } else {
            m_hold.pressed |= bit;
        }
        return;
    }
    if (others)
        return; // the others still held
    if (m_hold.pressed && !m_hold.spoiled && key.time - m_hold.since <= kTapMost)
        tapped(m_hold.pressed, key.time);
    m_hold = {};
}

void Model::tapped(std::uint8_t pressed, qint64 time)
{
    if (m_config.mode == Mode::Off)
        return;
    QStringList names = modifierNames(pressed);
    // Two or more together (Alt+Shift, Ctrl+Shift: the input language), Win
    // (Start) and Alt (the menu bar) are shortcuts of their own.
    if (m_config.mode == Mode::All || std::popcount(pressed) > 1 || pressed == kWin || pressed == kAlt) {
        m_tap = {};
        add(std::move(names), time);
        return;
    }
    // Shortcuts mode: a lone Ctrl, Shift or AltGr is nothing (Shift switches
    // an input method between Chinese and English); its double tap is
    // ("Ctrl ×2": Win顺's own launcher), and further taps count on.
    if (!m_rows.empty() && !m_rows.back().text && m_rows.back().caps == names && time < m_rows.back().last + kLinger) {
        add(std::move(names), time);
        return;
    }
    if (m_tap.pressed == pressed && time - m_tap.time <= m_config.doubleTap) {
        m_tap = {};
        add(std::move(names), time, 2);
        return;
    }
    m_tap = {pressed, time};
}

void Model::spoil()
{
    if (m_hold.pressed)
        m_hold.spoiled = true;
    m_tap = {};
}

bool Model::own(const Key& key) const
{
    const std::uint8_t mods = key.mods & (kCtrl | kShift | kAlt | kWin);
    return std::any_of(m_config.own.begin(), m_config.own.end(),
        [&](const Hotkey& hotkey) { return hotkey.vk == key.vk && hotkey.mods == mods; });
}

void Model::add(QStringList caps, qint64 time, int count)
{
    if (!m_rows.empty()) {
        Row& last = m_rows.back();
        if (!last.text && last.caps == caps && time < last.last + kLinger) {
            last.count += count;
            last.last = std::max(last.last, time);
            ++m_revision;
            return;
        }
    }
    Row row;
    row.caps = std::move(caps);
    row.count = count;
    row.shown = row.last = time;
    m_rows.push_back(std::move(row));
    if (m_rows.size() > size_t(kRows))
        m_rows.erase(m_rows.begin());
    ++m_revision;
}

bool Model::typing(qint64 time) const
{
    return !m_rows.empty() && m_rows.back().text && time < m_rows.back().last + kLinger;
}

void Model::type(QStringView text, qint64 time)
{
    if (!typing(time)) {
        Row row;
        row.caps = QStringList {QString()};
        row.text = true;
        row.shown = time;
        m_rows.push_back(std::move(row));
        if (m_rows.size() > size_t(kRows))
            m_rows.erase(m_rows.begin());
    }
    Row& row = m_rows.back();
    QString& shown = row.caps.front();
    shown += text;
    if (shown.size() > kTextTail) {
        // Only the tail on screen is kept.
        shown = shown.right(kTextTail);
        if (shown.front().isLowSurrogate())
            shown.remove(0, 1);
        row.cut = true;
    }
    row.last = std::max(row.last, time);
    ++m_revision;
}

void Model::advance(qint64 time)
{
    const auto gone = std::remove_if(m_rows.begin(), m_rows.end(),
        [time](const Row& row) { return time >= row.last + kLinger + kFadeOut; });
    if (gone != m_rows.end()) {
        m_rows.erase(gone, m_rows.end());
        ++m_revision;
    }
}

void Model::clear()
{
    m_rows.clear();
    m_hold = {};
    m_tap = {};
    ++m_revision;
}

float Model::opacity(const Row& row, qint64 time)
{
    const float in = std::clamp(float(time - row.shown) / float(kFadeIn), 0.0f, 1.0f);
    const qint64 fading = time - row.last - kLinger;
    const float out = fading <= 0 ? 1.0f : std::clamp(1.0f - float(fading) / float(kFadeOut), 0.0f, 1.0f);
    return std::min(in, out);
}

int unitFor(QSize region, double dpr, double outputScale)
{
    const double shorter = std::min(region.width(), region.height());
    double unit = std::max(36.0 * std::clamp(dpr, 0.75, 4.0), 0.05 * shorter);
    if (outputScale > 0 && outputScale < 1)
        unit = std::max(unit, 34.0 / outputScale);
    unit = std::min(unit, 0.16 * shorter);
    return std::max(20, int(std::lround(unit)));
}

int marginFor(int unit)
{
    return std::max(8, int(std::lround(unit * 0.5)));
}

int rowGapFor(int unit)
{
    return std::max(2, int(std::lround(unit * 0.16)));
}

int rowsThatFit(QSize region, int unit)
{
    const int room = region.height() - 2 * marginFor(unit) + rowGapFor(unit);
    return std::clamp(room / (unit + rowGapFor(unit)), 1, kRows);
}

bool preferLeft(QPoint cursor, QSize region, int unit)
{
    const int width = std::min(region.width() / 2, 9 * unit);
    const int height = std::min(region.height() / 2, unit * 9 / 2);
    return QRect(region.width() - width, region.height() - height, width, height).contains(cursor);
}

} // namespace ws::capture::keycaps
