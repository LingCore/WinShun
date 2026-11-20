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
}
