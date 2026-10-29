#include "Phosphor.h"

#include <QFile>
#include <QFontDatabase>

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace ws::phosphor {

namespace {

struct Mapping {
    char16_t segoe;
    char16_t phosphor;
    bool fill; // from Phosphor-Fill: an "on" state, as MDL2's filled glyphs
};

// Sorted by the Segoe code point; the Segoe name, then Phosphor's.
// Window caption buttons are not here: they keep Windows' own glyphs.
constexpr auto kMappings = std::to_array<Mapping>({
    {0xE70B, 0xE63E, false}, // QuickNote: notepad
    {0xE70D, 0xE136, false}, // ChevronDown: caret-down
    {0xE70E, 0xE13C, false}, // ChevronUp: caret-up
    {0xE70F, 0xE3B4, false}, // Edit: pencil-simple
    {0xE710, 0xE3D4, false}, // Add: plus
    {0xE711, 0xE4F6, false}, // Cancel: x
    {0xE712, 0xE1FE, false}, // More: dots-three
    {0xE713, 0xE270, false}, // Settings: gear
    {0xE714, 0xE4DA, false}, // Video: video-camera
    {0xE715, 0xE218, false}, // Mail: envelope-simple
    {0xE718, 0xE3E2, false}, // Pin: push-pin
    {0xE719, 0xE470, false}, // Shop: storefront
    {0xE71B, 0xE2E2, false}, // Link: link
    {0xE721, 0xE30C, false}, // Search: magnifying-glass
    {0xE722, 0xE10E, false}, // Camera: camera
    {0xE724, 0xE396, false}, // Send: paper-plane-right
    {0xE728, 0xEBDC, false}, // FavoriteList: list-star
    {0xE72B, 0xE058, false}, // Back: arrow-left
    {0xE72C, 0xE036, false}, // Refresh: arrow-clockwise
    {0xE734, 0xE46A, false}, // FavoriteStar: star
    {0xE736, 0xE0E6, false}, // ReadingMode: book-open
    {0xE738, 0xE32A, false}, // Remove: minus
    {0xE739, 0xE45E, false}, // Checkbox (the rectangle tool): square
    {0xE73E, 0xE182, false}, // CheckMark: check
    {0xE73F, 0xE09A, false}, // BackToWindow: arrows-in
    {0xE740, 0xE0A2, false}, // FullScreen: arrows-out
    {0xE74A, 0xE08E, false}, // Up: arrow-up
    {0xE74B, 0xE03E, false}, // Down: arrow-down
    {0xE74D, 0xE4A6, false}, // Delete: trash
    {0xE74E, 0xE248, false}, // Save: floppy-disk
    {0xE753, 0xE1AA, false}, // Cloud: cloud
    {0xE756, 0xEAE8, false}, // CommandPrompt: terminal-window
    {0xE765, 0xE2D8, false}, // Keyboard: keyboard
    {0xE768, 0xE3D0, false}, // Play: play
    {0xE769, 0xE39E, false}, // Pause: pause
    {0xE76C, 0xE13A, false}, // ChevronRight: caret-right
    {0xE76D, 0xE3AA, false}, // InkingTool: pen
    {0xE76F, 0xEAE2, false}, // GripperBarHorizontal: dots-six-vertical
    {0xE771, 0xE6F0, false}, // Personalize: paint-brush
    {0xE774, 0xE288, false}, // Globe: globe
    {0xE777, 0xE038, false}, // UpdateRestore: arrow-counter-clockwise
    {0xE77A, 0xE3E4, false}, // Unpin: push-pin-slash
    {0xE77F, 0xE198, false}, // Paste: clipboard-text
    {0xE790, 0xE6C8, false}, // Color: palette
    {0xE7A6, 0xE08C, false}, // Redo: arrow-u-up-right
    {0xE7A7, 0xE08A, false}, // Undo: arrow-u-up-left
    {0xE7BA, 0xE4E0, false}, // Warning: warning
    {0xE7E6, 0xEC76, false}, // Highlight: highlighter
    {0xE7E8, 0xE3DA, false}, // Power: power
    {0xE7EF, 0xE40C, false}, // Admin: shield-check
    {0xE7F4, 0xE32E, false}, // TVMonitor: monitor
    {0xE81C, 0xE1A0, false}, // History: clock-counter-clockwise
    {0xE823, 0xE19A, false}, // Recent: clock
    {0xE838, 0xE256, false}, // FolderOpen: folder-open
    {0xE841, 0xE3E2, true}, // PinnedFill: push-pin
    {0xE890, 0xE220, false}, // View: eye
    {0xE895, 0xE094, false}, // Sync: arrows-clockwise
    {0xE896, 0xE20C, false}, // Download: download-simple
    {0xE8A4, 0xE0EA, false}, // (a dotted list; browser bookmarks): bookmark-simple
    {0xE8A5, 0xE23A, false}, // Document: file-text
    {0xE8A7, 0xE5DE, false}, // OpenInNewWindow: arrow-square-out
    {0xE8A9, 0xE464, false}, // ViewAll: squares-four
    {0xE8AC, 0xEB0A, false}, // Rename: textbox
    {0xE8B5, 0xE010, false}, // Import: tray-arrow-down
    {0xE8B7, 0xE24A, false}, // Folder: folder
    {0xE8BB, 0xE4F6, false}, // ChromeClose: x
    {0xE8BD, 0xE17A, false}, // Message: chat-text
    {0xE8C1, 0xE4A2, false}, // Characters: translate
    {0xE8C8, 0xE1CA, false}, // Copy: copy
    {0xE8CB, 0xE098, false}, // Sort: arrows-down-up
    {0xE8CE, 0xEDDE, false}, // MapDrive: network
    {0xE8D2, 0xE6EE, false}, // Font: text-aa
    {0xE8D7, 0xE2D6, false}, // Permissions: key
    {0xE8D9, 0xE46A, true}, // Unfavorite: star
    {0xE8DA, 0xE256, false}, // OpenLocal: folder-open
    {0xE8DD, 0xE5BE, false}, // Bold: text-b
    {0xE8DE, 0xE83C, false}, // MoveToFolder: swap
    {0xE8E5, 0xE5DE, false}, // OpenFile: arrow-square-out
    {0xE8EA, 0xE1E0, false}, // CellPhone: device-mobile
    {0xE8EF, 0xE538, false}, // Calculator: calculator
    {0xE8F4, 0xE258, false}, // NewFolder: folder-plus
    {0xE8FC, 0xE560, false}, // (a screen; the Desktop folder): desktop
    {0xE8FD, 0xE2F2, false}, // BulletedList: list-bullets
    {0xE916, 0xE492, false}, // Stopwatch: timer
    {0xE91B, 0xE2CA, false}, // Photo2: image
    {0xE928, 0xE23E, false}, // Fingerprint: fingerprint
    {0xE943, 0xE1BC, false}, // Code: code
    {0xE946, 0xE2CE, false}, // Info: info
    {0xE968, 0xE2A0, false}, // Network: hard-drives
    {0xE99A, 0xE762, false}, // Robot: robot
    {0xE9D5, 0xEB7A, false}, // CheckList: scroll
    {0xEA3A, 0xE18A, false}, // CircleRing: circle
    {0xEA86, 0xE596, false}, // Puzzle: puzzle-piece
    {0xEB9F, 0xE2CA, false}, // Picture: image
    {0xEC8F, 0xE140, false}, // (up and down chevrons; long capture): caret-up-down
    {0xED14, 0xE3E6, false}, // QRCode: qr-code
    {0xED1A, 0xE224, false}, // Hide: eye-slash
    {0xEDE1, 0xEE52, false}, // Export: tray-arrow-up
    {0xEF3C, 0xE568, false}, // Eyedropper: eyedropper
    {0xF012, 0xE958, false}, // ZipFolder: file-zip
});
static_assert(std::ranges::is_sorted(kMappings, {}, &Mapping::segoe));

QString g_regular; // the families the fonts registered; empty if not loaded
QString g_fill;

QString load(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QFontDatabase::applicationFontFamilies(QFontDatabase::addApplicationFontFromData(file.readAll())).value(0);
}

} // namespace

void loadFonts()
{
    if (!g_regular.isEmpty())
        return;
    g_regular = load(u":/fonts/Phosphor.ttf"_s);
    g_fill = load(u":/fonts/Phosphor-Fill.ttf"_s);
    if (g_regular.isEmpty() || g_fill.isEmpty())
        qWarning("WinShun: the Phosphor icon fonts did not load; the system icon font draws the icons");
}

std::optional<Icon> forSegoe(const QString& glyph)
{
    if (glyph.size() != 1)
        return std::nullopt;
    const char16_t code = glyph.front().unicode();
    const auto found = std::ranges::lower_bound(kMappings, code, {}, &Mapping::segoe);
    if (found == kMappings.end() || found->segoe != code)
        return std::nullopt;
    const QString& family = found->fill ? g_fill : g_regular;
    if (family.isEmpty())
        return std::nullopt;
    return Icon {QString(QChar(found->phosphor)), family};
}

QList<QChar> segoeCodes()
{
    QList<QChar> codes;
    for (const Mapping& m : kMappings)
        codes.append(QChar(m.segoe));
    return codes;
}

} // namespace ws::phosphor
