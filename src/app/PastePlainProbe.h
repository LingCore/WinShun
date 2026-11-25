#pragma once

#include <QStringList>

class QDir;
namespace ws { class App; }

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
}
