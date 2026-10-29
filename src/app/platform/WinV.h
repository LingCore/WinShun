#pragma once

// Taking Win+V over from Windows' clipboard history, and Win+S from its
// search.
//
// Explorer registers these keys for itself (RegisterHotKey), so nobody else
// can, unless its "DisabledHotkeys" value (HKCU\Software\Microsoft\Windows\
// CurrentVersion\Explorer\Advanced, one character per key) holds the letter:
// Explorer reads it when it starts and leaves every Win combination with that
// letter alone (S frees Win+S and Win+Shift+S, the screen capture; Win+Ctrl+S
// is not Explorer's). So taking one over takes an Explorer restart, or the
// next sign-in. No keyboard hook is needed (see KeyListener for why that
// matters).
namespace ws::winv {

// `key` (an upper-case letter, matched exactly: Explorer reads each character as a virtual-key code) is in DisabledHotkeys: Explorer leaves its
// Win combinations alone once it (re)starts.
bool releasedByExplorer(wchar_t key);
// Adds `key` to DisabledHotkeys or takes it out again, keeping the other keys
// there. Returns false if the value could not be written.
bool setReleasedByExplorer(wchar_t key, bool released);

// Which letters of DisabledHotkeys are ours is kept per user next to it
// (HKCU\Software\WinShun, ExplorerHotkeys): a letter there that we did not put
// there is someone else's, or a leftover, and is not ours to remove.
//
// What to do about one letter, given the switch, whether the letter is in
// DisabledHotkeys, and whether the record has it.
struct Plan {
    bool addLetter = false;
    bool removeLetter = false;
    bool recorded = false; // the record has the letter afterwards
    bool blocked = false; // switch off, the letter there, not ours: left alone
    bool operator==(const Plan&) const = default;
};
// Who asks. Toggle: the user changed the switch (a letter already there is
// someone else's). Startup: the default profile starting with the switch as
// stored (a letter there with the switch on is an older version's, which kept
// no record: ours). StartupOtherProfile: a --profile copy starting; the
// letter and the record belong to the main copy, so it may add but never
// removes or adopts.
enum class Origin { Toggle, Startup, StartupOtherProfile };
Plan planFor(bool switchOn, bool present, bool recorded, Origin origin = Origin::Toggle);
// Brings DisabledHotkeys and the record in line with the switch (planFor).
// Returns false if a write failed.
bool reconcile(wchar_t key, bool switchOn, Origin origin = Origin::Toggle);
// Takes the letter out of DisabledHotkeys whoever put it there, and out of
// the record. Returns false if a write failed.
bool giveBack(wchar_t key);
// `key` is in the record.
bool recordedAsOurs(wchar_t key);

// Whether an Explorer runs the taskbar (one to restart).
bool canRestartExplorer();
// Ends the Explorer that runs the taskbar and starts it again with the
// user's rights, not ours (as administrator, everything started from the
// taskbar would be too). Windows starts it again by itself only when it had
// started that one at sign-in, so this does when Windows does not. Its folder
// windows close. Takes seconds: not for the GUI thread. Returns whether the
// taskbar is back.
bool restartExplorer();

} // namespace ws::winv
