#include "KeyChord.h"

#include <array>

using namespace Qt::StringLiterals;

namespace ws::keychord {

namespace {

// Virtual-key codes (WinUser.h), written out.
constexpr std::uint16_t kVkBack = 0x08, kVkTab = 0x09, kVkReturn = 0x0D, kVkPause = 0x13, kVkCapital = 0x14,
                        kVkEscape = 0x1B, kVkSpace = 0x20, kVkPrior = 0x21, kVkNext = 0x22, kVkEnd = 0x23,
                        kVkHome = 0x24, kVkLeft = 0x25, kVkUp = 0x26, kVkRight = 0x27, kVkDown = 0x28,
                        kVkSnapshot = 0x2C, kVkInsert = 0x2D, kVkDelete = 0x2E, kVkApps = 0x5D, kVkNumpad0 = 0x60,
                        kVkMultiply = 0x6A, kVkAdd = 0x6B, kVkSubtract = 0x6D, kVkDecimal = 0x6E, kVkDivide = 0x6F,
                        kVkF1 = 0x70, kVkNumLock = 0x90, kVkScroll = 0x91, kVkBrowserBack = 0xA6,
                        kVkBrowserForward = 0xA7, kVkBrowserRefresh = 0xA8, kVkBrowserSearch = 0xAA,
                        kVkBrowserHome = 0xAC, kVkVolumeMute = 0xAD, kVkVolumeDown = 0xAE, kVkVolumeUp = 0xAF,
                        kVkMediaNext = 0xB0, kVkMediaPrev = 0xB1, kVkMediaStop = 0xB2, kVkMediaPlayPause = 0xB3,
                        kVkOem1 = 0xBA, kVkOemPlus = 0xBB, kVkOemComma = 0xBC, kVkOemMinus = 0xBD,
                        kVkOemPeriod = 0xBE, kVkOem2 = 0xBF, kVkOem3 = 0xC0, kVkOem4 = 0xDB, kVkOem5 = 0xDC,
                        kVkOem6 = 0xDD, kVkOem7 = 0xDE;
constexpr std::uint16_t kVkLShift = 0xA0, kVkLControl = 0xA2, kVkLMenu = 0xA4, kVkLWin = 0x5B;

// The keys other than letters, digits and F1–F24: the name written first is
// the one shown, the others are read too.
struct Named {
    std::uint16_t vk;
    bool extended;
    std::array<QStringView, 3> names;
};

constexpr std::array kNamed {
    Named {kVkSpace, false, {u"Space"}},
    Named {kVkTab, false, {u"Tab"}},
    Named {kVkReturn, false, {u"Enter", u"Return"}},
    Named {kVkEscape, false, {u"Esc", u"Escape"}},
    Named {kVkBack, false, {u"Backspace"}},
    Named {kVkDelete, false, {u"Delete", u"Del"}},
    Named {kVkInsert, false, {u"Insert", u"Ins"}},
    Named {kVkHome, false, {u"Home"}},
    Named {kVkEnd, false, {u"End"}},
    Named {kVkPrior, false, {u"PgUp", u"PageUp"}},
    Named {kVkNext, false, {u"PgDn", u"PageDown"}},
    Named {kVkLeft, false, {u"Left", u"←"}},
    Named {kVkRight, false, {u"Right", u"→"}},
    Named {kVkUp, false, {u"Up", u"↑"}},
    Named {kVkDown, false, {u"Down", u"↓"}},
    Named {kVkSnapshot, false, {u"PrintScreen", u"PrtSc", u"Print"}},
    Named {kVkPause, false, {u"Pause", u"Break"}},
    Named {kVkCapital, false, {u"CapsLock"}},
    Named {kVkNumLock, false, {u"NumLock"}},
    Named {kVkScroll, false, {u"ScrollLock"}},
    Named {kVkApps, false, {u"Menu", u"Apps"}},
    Named {kVkOem3, false, {u"`", u"Backquote"}},
    Named {kVkOemMinus, false, {u"-"}},
    Named {kVkOemPlus, false, {u"="}},
    Named {kVkOem4, false, {u"["}},
    Named {kVkOem6, false, {u"]"}},
    Named {kVkOem5, false, {u"\\"}},
    Named {kVkOem1, false, {u";"}},
    Named {kVkOem7, false, {u"'"}},
    Named {kVkOemComma, false, {u","}},
    Named {kVkOemPeriod, false, {u"."}},
    Named {kVkOem2, false, {u"/"}},
    Named {kVkMultiply, false, {u"Num*"}},
    Named {kVkAdd, false, {u"Num+"}},
    Named {kVkSubtract, false, {u"Num-"}},
    Named {kVkDecimal, false, {u"Num."}},
    Named {kVkDivide, false, {u"Num/"}},
    Named {kVkReturn, true, {u"NumEnter"}},
    Named {kVkVolumeUp, false, {u"VolumeUp"}},
    Named {kVkVolumeDown, false, {u"VolumeDown"}},
    Named {kVkVolumeMute, false, {u"VolumeMute", u"Mute"}},
    Named {kVkMediaPlayPause, false, {u"MediaPlayPause", u"PlayPause"}},
    Named {kVkMediaStop, false, {u"MediaStop"}},
    Named {kVkMediaNext, false, {u"MediaNext", u"NextTrack"}},
    Named {kVkMediaPrev, false, {u"MediaPrev", u"PrevTrack"}},
    Named {kVkBrowserBack, false, {u"BrowserBack"}},
    Named {kVkBrowserForward, false, {u"BrowserForward"}},
    Named {kVkBrowserRefresh, false, {u"BrowserRefresh"}},
    Named {kVkBrowserSearch, false, {u"BrowserSearch"}},
    Named {kVkBrowserHome, false, {u"BrowserHome"}},
};

struct Modifier {
    unsigned bit;
    std::uint16_t vk; // its left side, as sent
    QStringView name;
};
constexpr std::array kModifiers {
    Modifier {kWin, kVkLWin, u"Win"},
    Modifier {kCtrl, kVkLControl, u"Ctrl"},
    Modifier {kAlt, kVkLMenu, u"Alt"},
    Modifier {kShift, kVkLShift, u"Shift"},
};

unsigned modifierNamed(QStringView name)
{
    const auto is = [&](QStringView word) { return name.compare(word, Qt::CaseInsensitive) == 0; };
    if (is(u"ctrl") || is(u"control") || is(u"ctl"))
        return kCtrl;
    if (is(u"alt"))
        return kAlt;
    if (is(u"shift"))
        return kShift;
    if (is(u"win") || is(u"windows") || is(u"meta"))
        return kWin;
    return 0;
}

std::optional<Chord> keyNamed(QStringView name)
{
    if (name.size() == 1) {
        const char16_t c = QChar::toUpper(name[0].unicode());
        if ((c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9'))
            return Chord {0, static_cast<std::uint16_t>(c)};
    }
    if (name.size() >= 2 && (name[0] == u'F' || name[0] == u'f')) {
        bool ok = false;
        const int n = name.sliced(1).toInt(&ok);
        if (ok && n >= 1 && n <= 24 && name[1] != u'0' && name[1] != u'+')
            return Chord {0, static_cast<std::uint16_t>(kVkF1 + n - 1)};
    }
    if (name.size() == 4 && name.startsWith(u"num", Qt::CaseInsensitive) && name[3] >= u'0' && name[3] <= u'9')
        return Chord {0, static_cast<std::uint16_t>(kVkNumpad0 + (name[3].unicode() - u'0'))};
    for (const Named& key : kNamed) {
        for (const QStringView known : key.names) {
            if (!known.isEmpty() && name.compare(known, Qt::CaseInsensitive) == 0)
                return Chord {0, key.vk, key.extended};
        }
    }
    return std::nullopt;
}

QString keyName(const Chord& chord)
{
    const std::uint16_t vk = chord.vk;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return QString(QChar(vk));
    if (vk >= kVkF1 && vk < kVkF1 + 24)
        return u"F"_s + QString::number(vk - kVkF1 + 1);
    if (vk >= kVkNumpad0 && vk <= kVkNumpad0 + 9)
        return u"Num"_s + QString::number(vk - kVkNumpad0);
    for (const Named& key : kNamed) {
        if (key.vk == vk && key.extended == chord.extended)
            return key.names[0].toString();
    }
    return {};
}

bool isModifierVk(std::uint16_t vk)
{
    return (vk >= 0x10 && vk <= 0x12) || (vk >= 0xA0 && vk <= 0xA5) || vk == 0x5B || vk == 0x5C;
}

} // namespace

std::optional<Chord> parse(QStringView text)
{
    QStringView rest = text.trimmed();
    // The key last; "Num+" is one, its + no separator. Before it: nothing,
    // or the modifiers, each followed by a +.
    QStringView key;
    if (rest.endsWith(u"num+", Qt::CaseInsensitive)) {
        key = rest.last(4);
        rest.chop(4);
    } else {
        const qsizetype plus = rest.lastIndexOf(u'+');
        key = rest.sliced(plus + 1);
        rest = rest.first(plus + 1);
    }
    std::optional<Chord> chord = keyNamed(key.trimmed());
    rest = rest.trimmed();
    if (!chord || (!rest.isEmpty() && !rest.endsWith(u'+')))
        return std::nullopt;
    if (rest.isEmpty())
        return chord;
    rest.chop(1);
    for (const QStringView part : rest.tokenize(u'+')) { // "+T" and "Ctrl++T": an empty part, no modifier
        const unsigned bit = modifierNamed(part.trimmed());
        if (!bit || (chord->modifiers & bit))
            return std::nullopt;
        chord->modifiers |= bit;
    }
    return chord;
}

QString text(const Chord& chord)
{
    const QString key = keyName(chord);
    if (key.isEmpty())
        return {};
    QString out;
    for (const Modifier& m : kModifiers) {
        if (chord.modifiers & m.bit) {
            out += m.name;
            out += u'+';
        }
    }
    return out + key;
}

std::optional<Chord> fromQt(int key, Qt::KeyboardModifiers modifiers)
{
    Chord chord;
    if (modifiers & Qt::ControlModifier)
        chord.modifiers |= kCtrl;
    if (modifiers & Qt::AltModifier)
        chord.modifiers |= kAlt;
    if (modifiers & Qt::ShiftModifier)
        chord.modifiers |= kShift;
    if (modifiers & Qt::MetaModifier)
        chord.modifiers |= kWin;
    const bool pad = modifiers & Qt::KeypadModifier;
    const auto is = [&](std::uint16_t vk, bool extended = false) {
        chord.vk = vk;
        chord.extended = extended;
        return chord;
    };
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return is(static_cast<std::uint16_t>('A' + (key - Qt::Key_A)));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return is(static_cast<std::uint16_t>((pad ? kVkNumpad0 : '0') + (key - Qt::Key_0)));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return is(static_cast<std::uint16_t>(kVkF1 + (key - Qt::Key_F1)));
    switch (key) {
    // Without the settings' own capture, Qt names the sign typed with Shift
    // ("!" for Shift+1): the key under it.
    case Qt::Key_Exclam: return is('1');
    case Qt::Key_At: return is('2');
    case Qt::Key_NumberSign: return is('3');
    case Qt::Key_Dollar: return is('4');
    case Qt::Key_Percent: return is('5');
    case Qt::Key_AsciiCircum: return is('6');
    case Qt::Key_Ampersand: return is('7');
    case Qt::Key_Asterisk: return is(pad ? kVkMultiply : std::uint16_t('8'));
    case Qt::Key_ParenLeft: return is('9');
    case Qt::Key_ParenRight: return is('0');
    case Qt::Key_Space: return is(kVkSpace);
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return is(kVkTab); // Shift+Tab: Qt's Backtab, with Shift
    case Qt::Key_Return: return is(kVkReturn);
    case Qt::Key_Enter: return is(kVkReturn, pad);
    case Qt::Key_Escape: return is(kVkEscape);
    case Qt::Key_Backspace: return is(kVkBack);
    case Qt::Key_Delete: return is(kVkDelete);
    case Qt::Key_Insert: return is(kVkInsert);
    case Qt::Key_Home: return is(kVkHome);
    case Qt::Key_End: return is(kVkEnd);
    case Qt::Key_PageUp: return is(kVkPrior);
    case Qt::Key_PageDown: return is(kVkNext);
    case Qt::Key_Left: return is(kVkLeft);
    case Qt::Key_Right: return is(kVkRight);
    case Qt::Key_Up: return is(kVkUp);
    case Qt::Key_Down: return is(kVkDown);
    case Qt::Key_Print: return is(kVkSnapshot);
    case Qt::Key_Pause: return is(kVkPause);
    case Qt::Key_CapsLock: return is(kVkCapital);
    case Qt::Key_NumLock: return is(kVkNumLock);
    case Qt::Key_ScrollLock: return is(kVkScroll);
    case Qt::Key_Menu: return is(kVkApps);
    case Qt::Key_QuoteLeft:
    case Qt::Key_AsciiTilde: return is(kVkOem3);
    case Qt::Key_Minus: return is(pad ? kVkSubtract : kVkOemMinus);
    case Qt::Key_Underscore: return is(kVkOemMinus);
    case Qt::Key_Plus: return is(pad ? kVkAdd : kVkOemPlus);
    case Qt::Key_Equal: return is(kVkOemPlus);
    case Qt::Key_BracketLeft:
    case Qt::Key_BraceLeft: return is(kVkOem4);
    case Qt::Key_BracketRight:
    case Qt::Key_BraceRight: return is(kVkOem6);
    case Qt::Key_Backslash:
    case Qt::Key_Bar: return is(kVkOem5);
    case Qt::Key_Semicolon:
    case Qt::Key_Colon: return is(kVkOem1);
    case Qt::Key_Apostrophe:
    case Qt::Key_QuoteDbl: return is(kVkOem7);
    case Qt::Key_Comma:
    case Qt::Key_Less: return is(kVkOemComma);
    case Qt::Key_Period: return is(pad ? kVkDecimal : kVkOemPeriod);
    case Qt::Key_Greater: return is(kVkOemPeriod);
    case Qt::Key_Slash: return is(pad ? kVkDivide : kVkOem2);
    case Qt::Key_Question: return is(kVkOem2);
    case Qt::Key_VolumeUp: return is(kVkVolumeUp);
    case Qt::Key_VolumeDown: return is(kVkVolumeDown);
    case Qt::Key_VolumeMute: return is(kVkVolumeMute);
    case Qt::Key_MediaPlay:
    case Qt::Key_MediaPause:
    case Qt::Key_MediaTogglePlayPause: return is(kVkMediaPlayPause);
    case Qt::Key_MediaStop: return is(kVkMediaStop);
    case Qt::Key_MediaNext: return is(kVkMediaNext);
    case Qt::Key_MediaPrevious: return is(kVkMediaPrev);
    case Qt::Key_Back: return is(kVkBrowserBack);
    case Qt::Key_Forward: return is(kVkBrowserForward);
    case Qt::Key_Refresh: return is(kVkBrowserRefresh);
    case Qt::Key_Search: return is(kVkBrowserSearch);
    case Qt::Key_HomePage: return is(kVkBrowserHome);
    default: return std::nullopt; // a modifier alone, or a key there is no name for
    }
}

bool sendable(const Chord& chord) noexcept
{
    const bool ctrlAlt = (chord.modifiers & (kCtrl | kAlt)) == (kCtrl | kAlt);
    return chord.vk != 0 && !isModifierVk(chord.vk) && !(ctrlAlt && (chord.vk == kVkDelete || chord.vk == kVkDecimal));
}

std::vector<pastekeys::Key> presses(const Chord& chord)
{
    std::vector<pastekeys::Key> out;
    if (chord.vk == 0 || isModifierVk(chord.vk))
        return out;
    for (const Modifier& m : kModifiers) {
        if (chord.modifiers & m.bit)
            out.push_back({.vk = m.vk, .extended = m.vk == kVkLWin});
    }
    out.push_back({.vk = chord.vk, .extended = chord.extended});
    out.push_back({.vk = chord.vk, .up = true, .extended = chord.extended});
    for (auto m = kModifiers.rbegin(); m != kModifiers.rend(); ++m) {
        if (chord.modifiers & m->bit)
            out.push_back({.vk = m->vk, .up = true, .extended = m->vk == kVkLWin});
    }
    return out;
}

bool isCommand(QStringView command) noexcept
{
    return command.startsWith(kCommandPrefix, Qt::CaseInsensitive);
}

QString command(const Chord& chord)
{
    const QString keys = text(chord);
    return keys.isEmpty() ? QString() : kCommandPrefix.toString() + keys;
}

std::optional<Chord> ofCommand(QStringView command)
{
    return isCommand(command) ? parse(command.sliced(kCommandPrefix.size())) : std::nullopt;
}

} // namespace ws::keychord
