#pragma once

#include <QStringList>

class QDir;
namespace ws {
class App;
struct Settings;
}

namespace ws::preview {
// --probe-runtime <folder> --paste-plain-check: 粘贴为纯文本's global hotkey
// pressed for real over a rich text box of another process (GitHub #408). See the .cpp.
inline constexpr auto kPastePlainProbeChord = u"Ctrl+Alt+Shift+F22";
// Before the App starts: the chord is nobody's yet (else the check would prove nothing).
bool pastePlainChordFree();
bool checkPastePlain(App& app, const QStringList& args, const QDir& output);

// --probe-runtime <folder> --activation-paste-check <window>: the clipboard
// opened by its own hotkey over <window> (a program that reads the clipboard
// only as it comes to the front, Photoshop, in front and holding a picture it
// read as it came) and Enter pressed on the picture in its history (GitHub
// #432). What went in is for the harness to read off the program
// (docs/validation/issue432-photoshop-paste). See the .cpp.
inline constexpr auto kActivationPasteProbeChord = u"Ctrl+Alt+Shift+F21";
bool activationPasteChordFree();
// Before the App starts: the one picture in this probe's history, `width` wide
// (--picture-width, 240 by default), 2/3 as high.
bool seedActivationPastePicture(const QString& clipboardFolder, int width);
bool checkActivationPaste(App& app, const QStringList& args, const QDir& output);

// --probe-runtime <folder> --entry-hotkey-check: an entry's own shortcut
// (Clip::hotkey) pressed for real over a text box of another process, with
// 保留重复项 on. See the .cpp.
inline constexpr auto kEntryHotkeyProbeChord = u"Ctrl+Alt+Shift+F20";
bool entryHotkeyChordFree();
// Before the App starts: the entry with the chord, and a newer one after it
// (what Ctrl+V or the first quick paste key would paste instead).
bool seedEntryHotkey(const QString& clipboardFolder);
bool checkEntryHotkey(App& app, const QStringList& args, const QDir& output);

// --probe-runtime <folder> --command-hotkey-check: a plugin command's own
// shortcut (PluginConfig::hotkeys, GitHub #466) pressed for real: the
// launcher with the command's keyword typed, and away again. See the .cpp.
inline constexpr auto kCommandHotkeyProbeChord = u"Ctrl+Alt+Shift+F19";
bool commandHotkeyChordFree();
// Before the App starts: a plugin that starts nothing in the probe's
// profile, its commands' shortcuts in `settings`.
bool seedCommandHotkey(Settings& settings);
bool checkCommandHotkey(App& app, const QStringList& args, const QDir& output);

// --probe-runtime <folder> --field-focus-check: the clipboard opened by its
// own hotkey over a text box of another process (no focus taken, the keys
// routed), its search field clicked for real: the window takes the focus,
// Chinese typed as an input method gives it lands in the search, and Enter
// still pastes into the box. The clipboard's chord as for
// --activation-paste-check. See the .cpp.
bool seedFieldFocus(const QString& clipboardFolder); // before the App starts: the two entries of its history
bool checkFieldFocus(App& app, const QStringList& args, const QDir& output);

// --probe-runtime <folder> --full-screen-game-check <window>: the clipboard
// opened by its own hotkey over <window>, a game in Direct3D's exclusive full
// screen (GitHub #452): it takes the focus, stays as the game leaves full
// screen, and Esc or Enter give the game the front back. The clipboard's
// chord and history as for --field-focus-check. See the .cpp.
bool checkFullScreenGame(App& app, const QStringList& args, const QDir& output);

// --probe-runtime <folder> --duplicates-check [--keep-place]: 保留重复项 on
// the real clipboard: copies as a program makes them, one after another,
// twice at once, and after a paste from the history (with 粘贴后保持位置 on
// for --keep-place). The history seeded and the chord as for
// --entry-hotkey-check. See the .cpp.
bool checkDuplicates(App& app, const QStringList& args, const QDir& output);
}
