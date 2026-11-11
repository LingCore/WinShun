#pragma once

#include <array>
#include <string_view>

namespace ws::phosphor {

// A legacy Segoe Fluent / MDL2 code point resolved by name. The names are
// official Phosphor Icons 2.1.2 names (PhosphorIcons.generated.h); the
// code points in that table must never be written here. Sorted by Segoe.
// tools/gen_phosphor_icons.py --check verifies every entry.
struct Alias {
    char16_t segoe;
    std::string_view name;
    bool fill; // the Fill font, for an 'on' state
};

inline constexpr auto kAliases = std::to_array<Alias>({
    {0xE16E, "funnel", true}, // Filter (MDL2's older one; here the filters shown): funnel
    {0xE70B, "notepad", false}, // QuickNote: notepad
    {0xE70D, "caret-down", false}, // ChevronDown: caret-down
    {0xE70E, "caret-up", false}, // ChevronUp: caret-up
    {0xE70F, "pencil-simple", false}, // Edit: pencil-simple
    {0xE710, "plus", false}, // Add: plus
    {0xE711, "x", false}, // Cancel: x
    {0xE712, "dots-three", false}, // More: dots-three
    {0xE713, "gear", false}, // Settings: gear
    {0xE714, "video-camera", false}, // Video: video-camera
    {0xE715, "envelope-simple", false}, // Mail: envelope-simple
    {0xE718, "push-pin", false}, // Pin: push-pin
    {0xE719, "storefront", false}, // Shop: storefront
    {0xE71A, "square", false}, // Stop: square
    {0xE71B, "link", false}, // Link: link
    {0xE71C, "funnel", false}, // Filter: funnel
    {0xE720, "microphone", false}, // Microphone: microphone
    {0xE721, "magnifying-glass", false}, // Search: magnifying-glass
    {0xE722, "camera", false}, // Camera: camera
    {0xE724, "paper-plane-right", false}, // Send: paper-plane-right
    {0xE728, "list-star", false}, // FavoriteList: list-star
    {0xE72B, "arrow-left", false}, // Back: arrow-left
    {0xE72C, "arrow-clockwise", false}, // Refresh: arrow-clockwise
    {0xE734, "star", false}, // FavoriteStar: star
    {0xE736, "book-open", false}, // ReadingMode: book-open
    {0xE738, "minus", false}, // Remove: minus
    {0xE739, "square", false}, // Checkbox (the rectangle tool): square
    {0xE73A, "check-square", false}, // CheckboxComposite: check-square
    {0xE73E, "check", false}, // CheckMark: check
    {0xE73F, "corners-in", false}, // BackToWindow: corners-in
    {0xE740, "corners-out", false}, // FullScreen: corners-out
    {0xE74A, "arrow-up", false}, // Up: arrow-up
    {0xE74B, "arrow-down", false}, // Down: arrow-down
    {0xE74D, "trash", false}, // Delete: trash
    {0xE74E, "floppy-disk", false}, // Save: floppy-disk
    {0xE74F, "speaker-slash", false}, // Mute: speaker-slash
    {0xE753, "cloud", false}, // Cloud: cloud
    {0xE756, "terminal-window", false}, // CommandPrompt: terminal-window
    {0xE765, "keyboard", false}, // Keyboard: keyboard
    {0xE767, "speaker-high", false}, // Volume: speaker-high
    {0xE768, "play", false}, // Play: play
    {0xE769, "pause", false}, // Pause: pause
    {0xE76C, "caret-right", false}, // ChevronRight: caret-right
    {0xE76D, "pen", false}, // InkingTool: pen
    {0xE76F, "dots-six-vertical", false}, // GripperBarHorizontal: dots-six-vertical
    {0xE771, "paint-brush", false}, // Personalize: paint-brush
    {0xE774, "globe", false}, // Globe: globe
    {0xE777, "arrow-counter-clockwise", false}, // UpdateRestore: arrow-counter-clockwise
    {0xE77A, "push-pin-slash", false}, // Unpin: push-pin-slash
    {0xE77F, "clipboard-text", false}, // Paste: clipboard-text
    {0xE790, "palette", false}, // Color: palette
    {0xE7A6, "arrow-u-up-right", false}, // Redo: arrow-u-up-right
    {0xE7A7, "arrow-u-up-left", false}, // Undo: arrow-u-up-left
    {0xE7BA, "warning", false}, // Warning: warning
    {0xE7C9, "cursor", false}, // TouchPointer: cursor
    {0xE7E6, "highlighter", false}, // Highlight: highlighter
    {0xE7E8, "power", false}, // Power: power
    {0xE7EF, "shield-check", false}, // Admin: shield-check
    {0xE7F4, "monitor", false}, // TVMonitor: monitor
    {0xE81C, "clock-counter-clockwise", false}, // History: clock-counter-clockwise
    {0xE823, "clock", false}, // Recent: clock
    {0xE838, "folder-open", false}, // FolderOpen: folder-open
    {0xE841, "push-pin", true}, // PinnedFill: push-pin
    {0xE890, "eye", false}, // View: eye
    {0xE895, "arrows-clockwise", false}, // Sync: arrows-clockwise
    {0xE896, "download-simple", false}, // Download: download-simple
    {0xE8A4, "bookmark-simple", false}, // (a dotted list; browser bookmarks): bookmark-simple
    {0xE8A5, "file-text", false}, // Document: file-text
    {0xE8A7, "arrow-square-out", false}, // OpenInNewWindow: arrow-square-out
    {0xE8A9, "squares-four", false}, // ViewAll: squares-four
    {0xE8AC, "textbox", false}, // Rename: textbox
    {0xE8B0, "cursor-click", false}, // Click: cursor-click
    {0xE8B5, "tray-arrow-down", false}, // Import: tray-arrow-down
    {0xE8B7, "folder", false}, // Folder: folder
    {0xE8BB, "x", false}, // ChromeClose: x
    {0xE8BD, "chat-text", false}, // Message: chat-text
    {0xE8C1, "translate", false}, // Characters: translate
    {0xE8C8, "copy", false}, // Copy: copy
    {0xE8CB, "arrows-down-up", false}, // Sort: arrows-down-up
    {0xE8CE, "network", false}, // MapDrive: network
    {0xE8D2, "text-aa", false}, // Font: text-aa
    {0xE8D7, "key", false}, // Permissions: key
    {0xE8D9, "star", true}, // Unfavorite: star
    {0xE8DA, "folder-open", false}, // OpenLocal: folder-open
    {0xE8DD, "text-b", false}, // Bold: text-b
    {0xE8DE, "swap", false}, // MoveToFolder: swap
    {0xE8E5, "arrow-square-out", false}, // OpenFile: arrow-square-out
    {0xE8EA, "device-mobile", false}, // CellPhone: device-mobile
    {0xE8EF, "calculator", false}, // Calculator: calculator
    {0xE8F4, "folder-plus", false}, // NewFolder: folder-plus
    {0xE8FC, "desktop", false}, // (a screen; the Desktop folder): desktop
    {0xE8FD, "list-bullets", false}, // BulletedList: list-bullets
    {0xE916, "timer", false}, // Stopwatch: timer
    {0xE91B, "image", false}, // Photo2: image
    {0xE928, "fingerprint", false}, // Fingerprint: fingerprint
    {0xE943, "code", false}, // Code: code
    {0xE946, "info", false}, // Info: info
    {0xE962, "mouse", false}, // Mouse: mouse
    {0xE968, "hard-drives", false}, // Network: hard-drives
    {0xE99A, "robot", false}, // Robot: robot
    {0xE9D5, "scroll", false}, // CheckList: scroll
    {0xE9D9, "pulse", false}, // Diagnostic: pulse
    {0xEA3A, "circle", false}, // CircleRing: circle
    {0xEA86, "puzzle-piece", false}, // Puzzle: puzzle-piece
    {0xEB9F, "image", false}, // Picture: image
    {0xEC54, "microphone-slash", false}, // MicOff: microphone-slash
    {0xEC8F, "caret-up-down", false}, // (up and down chevrons; long capture): caret-up-down
    {0xED14, "qr-code", false}, // QRCode: qr-code
    {0xED1A, "eye-slash", false}, // Hide: eye-slash
    {0xEDE1, "tray-arrow-up", false}, // Export: tray-arrow-up
    {0xEF3C, "eyedropper", false}, // Eyedropper: eyedropper
    {0xF012, "file-zip", false}, // ZipFolder: file-zip
});

} // namespace ws::phosphor
