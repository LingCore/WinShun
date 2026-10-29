#pragma once

#include "JournalBudget.h"
#include "platform/WindowEffects.h"

#include <cstdint>
#include <optional>

namespace ws {

enum class TriggerSource : std::uint8_t { DoubleTap, Hotkey, WinV, ClipboardHotkey, Tray, Command };
enum class TriggerWindow : std::uint8_t { Launcher, Clipboard };

// What each press that opens or closes a window of Win顺's came to, in the
// diagnostics journal as "trigger" (GitHub #111: "double Ctrl sometimes does
// nothing, or comes seconds late"). From the press to the window seen: how
// long the press waited for the GUI thread (queue_ms), what it did, whether
// Windows let the window to the front and how (front, front_ms), when it was
// on the screen (seen_ms), whether the memory had been handed back
// (IdleTrim) and how many pages came back meanwhile (faults), whether the
// service's key helper is in use and the keyboard's Raw Input registration
// is in place. Nothing typed, no names. Written once the window is seen, or
// the press is done with: never between the press and the window coming.
// The slow, refused and ignored ones are all kept, up to a limit an hour;
// the ordinary ones a few (JournalBudget). On the GUI thread.
class TriggerJournal {
public:
    using Source = TriggerSource;
    using Window = TriggerWindow;

    // A double tap's listener: own (this process's Raw Input), helper (the
    // service's), own-backup (this process's, the helper's word late); and
    // from the press until that listener told of it.
    struct Told {
        const char* from = "own";
        std::int64_t listenerMs = 0;
        // Its first press seemed this long, held up by a program's slow
        // hook (DoubleTapDetector::queuedMs, GitHub #111); else 0.
        std::int64_t queuedMs = 0;
    };
    struct Context {
        bool helper = false; // the service's key helper tells of double taps
        bool trimmed = false; // IdleTrim handed the memory back since the last opening
        const char* rawInput = "off"; // ok, none, other, unknown; off without double tap
        std::optional<Told> told; // double taps only
    };

    // A press got to the GUI thread; `pressedAt` on the 64-bit tick clock.
    // One whose window is still coming waits on for it (a press queued
    // behind it, decided before that window is up); any other still open,
    // and one that waited since the press before, are written first.
    void begin(Source source, std::uint64_t pressedAt, const Context& context);
    // What it came to: show, hide, panel, raise, dialog-bar, clipboard-back,
    // clipboard-stays (ignored: below). With `coming`, written once that
    // window is seen (or after the next press but one, or end()).
    void decided(const char* action, std::optional<Window> coming = {});
    // GameGuard's reason, and the facts it went by (k... below): written now.
    static constexpr std::uint8_t kCovers = 1, kExclusive = 2, kHidden = 4, kConfined = 8, kCaptured = 16, kTyping = 32;
    void ignored(const char* reason, std::uint8_t facts);
    void front(win::Front how, double ms);
    void seen(Window window);
    void end(); // written now, if still open
    bool open() const noexcept { return m_entry.has_value(); }

private:
    struct Entry;
    void write(const Entry& entry);

    struct Entry {
        Source source = Source::DoubleTap;
        std::uint64_t pressedAt = 0;
        std::uint64_t receivedAt = 0;
        Context context;
        const char* action = nullptr;
        const char* ignored = nullptr;
        std::uint8_t facts = 0;
        std::optional<Window> coming;
        std::optional<win::Front> front;
        double frontMs = 0;
        std::optional<std::uint64_t> seenAt;
        std::uint32_t faults = 0; // the process's count at the press
    };
    std::optional<Entry> m_entry;
    std::optional<Entry> m_waiting; // decided, its window still coming, a later press begun
    diagnostics::JournalBudget m_budget;
};

} // namespace ws
