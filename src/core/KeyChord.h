#pragma once

#include "PasteKeys.h"

#include <QString>
#include <QStringView>
#include <Qt>

#include <cstdint>
#include <optional>
#include <vector>

// A favorite that presses a shortcut where its user is working (收藏夹 ›
// 添加快捷键): one key, with the modifiers held for it, as a hand would press
// it. Its keyword puts it on top, Enter presses it in the window that was in
// front. Kept in the favorites as a command of Win顺's own,
// "winshun:keys/Ctrl+Shift+T" (FavoritesSettings, Launcher, FolderMenu;
// KeyPresser presses it): an older Win顺 keeps it as it is, and running it
// there opens nothing but the search box (WinShunUri).
//
// Pure logic, no window and no <windows.h> (tst_favorites): virtual-key
// codes are written out, by their US keyboard names. A key that types a
// sign is the key, not the sign: "Shift+=" is the key right of 0 with
// Shift, whatever the layout puts on it.
namespace ws::keychord {

// The modifiers, as RegisterHotKey's bits (MOD_ALT 1, MOD_CONTROL 2,
// MOD_SHIFT 4, MOD_WIN 8).
inline constexpr unsigned kAlt = 0x1;
inline constexpr unsigned kCtrl = 0x2;
inline constexpr unsigned kShift = 0x4;
inline constexpr unsigned kWin = 0x8;

struct Chord {
    unsigned modifiers = 0;
    std::uint16_t vk = 0; // the key's virtual-key code; never a modifier's
    bool extended = false; // the number pad's Enter (VK_RETURN, as the main one)
    friend constexpr bool operator==(const Chord&, const Chord&) = default;
};

// "Ctrl+Shift+T", "win + shift + s", "F5", "Num+", "VolumeUp": the modifiers
// (Ctrl, Alt, Shift, Win; Control, Meta, Windows too) and one key, any case.
// Nothing for no key, two keys, a modifier twice or a name not known.
std::optional<Chord> parse(QStringView text);

// As the settings and the rows show it and the favorites keep it: Win, Ctrl,
// Alt, Shift in that order, then the key ("Win+Shift+S", "Ctrl+Alt+Left").
QString text(const Chord& chord);

// The settings' recorder (HotkeyRecorder.qml, keyMode): a key event as Qt
// has it, the number pad's keys with Qt::KeypadModifier. Nothing for a
// modifier alone or a key it cannot press.
std::optional<Chord> fromQt(int key, Qt::KeyboardModifiers modifiers);

// Whether Windows takes it from a program: not Ctrl+Alt+Delete (the secure
// attention sequence, only a real keyboard's), nor a chord with no key.
bool sendable(const Chord& chord) noexcept;

// The keys to send: the modifiers pressed (left sides; Win, Ctrl, Alt,
// Shift), the key pressed and let go, the modifiers let go in reverse.
std::vector<pastekeys::Key> presses(const Chord& chord);

// ---- as a favorite's command ---------------------------------------------
inline constexpr QStringView kCommandPrefix = u"winshun:keys/";

bool isCommand(QStringView command) noexcept;
QString command(const Chord& chord); // "winshun:keys/Ctrl+Shift+T"
std::optional<Chord> ofCommand(QStringView command); // nothing for any other command

} // namespace ws::keychord
