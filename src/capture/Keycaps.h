#pragma once

#include <QSize>
#include <QPoint>
#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>
#include <vector>

namespace ws::capture::keycaps {

// The keys and mouse buttons pressed while recording, drawn into the frames
// as keycap rows in a corner (GitHub #98), so the MP4 and the GIF made from
// it both have them. Pure logic, unit-tested (tst_keycaps): which keys a mode
// shows, what they are called, the rows, their counts and timing, the size.
// KeyHook (KeyStream.h) feeds the keys; the media worker asks the Model what
// is on screen at each frame's time and KeycapRender draws it.
//
// Nothing typed is kept beyond what is on screen: a typed row holds only the
// tail it shows and goes with the row, Shortcuts mode never hands typing over
// (passes()), and clear() forgets everything (pause, stop).

enum class Mode : std::uint8_t {
    Off,
    Shortcuts, // Ctrl/Alt/Win combinations and the command keys (Esc, Enter, F1…), never typing
    All, // typing too, as a row of text: passwords included
};
Mode modeFromName(QStringView name); // "off", "shortcuts", "all"; anything else is Off
QString modeName(Mode mode);

// The modifiers held as a key goes down.
enum Modifier : std::uint8_t {
    kCtrl = 1,
    kShift = 2,
    kAlt = 4,
    kWin = 8,
    kAltGr = 16, // the right Alt of a layout that has it (the left Ctrl Windows adds to it is no Ctrl)
};
inline constexpr std::uint8_t kCommandMods = kCtrl | kAlt | kWin; // a key with one of these is a shortcut

// Keys as Windows' virtual keys; the mouse buttons as theirs (VK_LBUTTON 1,
// VK_RBUTTON 2, VK_MBUTTON 4, VK_XBUTTON1 5, VK_XBUTTON2 6), and two of ours:
inline constexpr std::uint16_t kHiddenKey = 0; // a press this mode does not show: only that it came (it spoils a modifier's tap)
inline constexpr std::uint16_t kAltGrKey = 0x07; // the right Alt as AltGr (virtual key 7 is unassigned)

// Never shown: Win顺's own input (isOwnInput: its paste keys, its masking
// key), the masking key itself, VK_PACKET (characters sent as such, not keys
// pressed) and no key at all. Keys another program sends (AutoHotkey, a
// remapper's replacement) are shown: the hook's `flags` (LLKHF_INJECTED…)
// make no difference.
bool hidden(std::uint32_t vk, std::uintptr_t extraInfo, std::uint32_t flags = 0) noexcept;
bool isMouse(std::uint16_t vk) noexcept;
std::uint8_t modifierOf(std::uint16_t vk) noexcept; // 0 for a key that is none
bool isCommandKey(std::uint16_t vk) noexcept; // shown alone in Shortcuts mode
// Whether the hook hands the key over as itself: in Shortcuts mode only
// modifiers, mouse buttons, command keys and keys with Ctrl, Alt or Win;
// typing goes on as kHiddenKey (and no character is looked up).
bool passes(Mode mode, std::uint16_t vk, std::uint8_t mods) noexcept;

// The few words translated (Recorder passes them in).
struct Words {
    QString space = QStringLiteral("Space");
    QString left = QStringLiteral("Left click");
    QString right = QStringLiteral("Right click");
    QString middle = QStringLiteral("Middle click");
    QString back = QStringLiteral("Back button");
    QString forward = QStringLiteral("Forward button");
};

// A key's label: its legend for letters, digits and punctuation (`legend`,
// from the keyboard layout), a short name for the others; empty for a key
// not shown (browser keys, IME keys…).
QString keyName(std::uint16_t vk, char16_t legend, const Words& words);
QStringList modifierNames(std::uint8_t mods); // Win, Ctrl, Alt, AltGr, Shift: the settings' order

// A hotkey of Win顺's that ends the recording (RegisterHotKey's MOD_* and
// key): never shown, so the last frames do not show how it was stopped.
struct Hotkey {
    std::uint8_t mods = 0;
    std::uint16_t vk = 0;
    bool operator==(const Hotkey&) const = default;
};
Hotkey fromRegistered(unsigned modifiers, unsigned vk) noexcept;

// One key or mouse button from the hook.
struct Key {
    qint64 time = 0; // 100 ns, the media clock's
    std::uint16_t vk = kHiddenKey;
    std::uint8_t mods = 0; // held before it
    bool down = true;
    bool repeat = false; // the key held down: keeps its row on screen
    bool inside = true; // a mouse button: inside the recorded region (not on the panel)
    char16_t legend = 0; // punctuation keys: what is printed on the key
    std::array<char16_t, 2> text {}; // All mode, no Ctrl/Alt/Win: the character typed
};

struct Config {
    Mode mode = Mode::Off;
    bool mouse = false; // mouse buttons shown
    Words words;
    std::vector<Hotkey> own;
    qint64 doubleTap = 5'000'000; // between the taps of a modifier's double tap (100 ns)
};

// One row on screen: keycaps ("Ctrl", "Shift", "S"), or typed text.
struct Row {
    QStringList caps; // a text row: its one string
    bool text = false;
    bool cut = false; // a text row: its start dropped ("…")
    int count = 1; // "×3": the same keys again while the row was on screen
    qint64 shown = 0; // fades in from here
    qint64 last = 0; // lingers from here: its last press or repeat
};

inline constexpr qint64 kFadeIn = 900'000; // 90 ms
inline constexpr qint64 kLinger = 25'000'000; // 2.5 s after the last press
inline constexpr qint64 kFadeOut = 2'000'000; // 200 ms
inline constexpr int kRows = 3; // the newest at the bottom
inline constexpr int kTextTail = 16; // characters of a typed row
inline constexpr qint64 kTapMost = 10'000'000; // a modifier held longer alone is no tap

class Model {
public:
    explicit Model(Config config);
    void feed(const Key& key);
    void advance(qint64 time); // drops the rows gone by then
    void clear(); // pause, stop: rows, held modifiers, taps, all forgotten
    const std::vector<Row>& rows() const { return m_rows; } // the oldest first
    quint64 revision() const { return m_revision; } // changes with what the rows show
    const Config& config() const { return m_config; }
    static float opacity(const Row& row, qint64 time);

private:
    void mouse(const Key& key);
    void modifier(const Key& key);
    void tapped(std::uint8_t pressed, qint64 time);
    void spoil(); // something besides modifiers came
    void add(QStringList caps, qint64 time, int count = 1);
    void type(QStringView text, qint64 time);
    bool typing(qint64 time) const;
    bool own(const Key& key) const;

    Config m_config;
    std::vector<Row> m_rows;
    quint64 m_revision = 0;
    struct Hold {
        std::uint8_t pressed = 0; // modifiers pressed since the first went down
        bool spoiled = false; // with something else, or with one held from before
        qint64 since = 0;
    } m_hold;
    struct Tap {
        std::uint8_t pressed = 0;
        qint64 time = 0;
    } m_tap; // Shortcuts mode: a lone Ctrl, Shift or AltGr waiting for its second tap
};

// The rows' size in the frame's pixels: a row is `unit` high. At least 36
// logical pixels on the recorded display, and 5% of the region's shorter
// side; a GIF scaled down (`outputScale` < 1) keeps 34 pixels of its own.
// Never more than 16% of the shorter side.
int unitFor(QSize region, double dpr, double outputScale);
int marginFor(int unit);
int rowGapFor(int unit);
int rowsThatFit(QSize region, int unit);
// The rows go to the bottom right corner, or to the bottom left while the
// pointer is in the bottom right as they appear (they would cover what it
// points at). `cursor` relative to the region.
bool preferLeft(QPoint cursor, QSize region, int unit);

} // namespace ws::capture::keycaps
