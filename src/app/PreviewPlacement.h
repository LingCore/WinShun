#pragma once

#include <QRect>
#include <QSize>

#include <algorithm>
#include <array>
#include <concepts>

namespace ws {

// Where the preview window goes by the launcher (FilePreview), all in
// logical pixels on the launcher's screen: no Qt objects, so that the rules
// are tested on their own (tst_previewplacement).
enum class PreviewSide { None = -1, Right, Left, Below, Above, Over, OverLeft };

struct PreviewPlacement {
    QRect rect;
    PreviewSide side = PreviewSide::None;
};

namespace previewplacement {

inline constexpr int kGap = 8; // from the launcher and the work area's edges
inline constexpr QSize kSmallest {360, 280}; // a room smaller than this is no room
inline constexpr int kOverWidth = 480; // no room beside it: at least this wide over the launcher
inline constexpr int kNamesShare = 40; // % of the launcher's width (from its left) never covered
inline constexpr int kSideTolerance = 48; // keep a side through small moves at its boundary

// The rooms beside the launcher: right, left, below. The sides take the work
// area's whole height (the window slides up when the launcher is low on the
// screen), so a launcher in the middle of the screen has room on both sides.
inline std::array<QRect, 4> rooms(const QRect& area, const QRect& launcher)
{
    const int top = area.top() + kGap, bottom = area.bottom() - kGap;
    return {
        QRect(QPoint(launcher.right() + 1 + kGap, top), QPoint(area.right() - kGap, bottom)),
        QRect(QPoint(area.left() + kGap, top), QPoint(launcher.left() - 1 - kGap, bottom)),
        QRect(QPoint(std::max(area.left() + kGap, launcher.left()), launcher.bottom() + 1 + kGap),
            QPoint(area.right() - kGap, bottom)),
        QRect(QPoint(std::max(area.left() + kGap, launcher.left()), top),
            QPoint(area.right() - kGap, launcher.top() - 1 - kGap)),
    };
}

inline bool usable(const QRect& room)
{
    return room.width() >= kSmallest.width() && room.height() >= kSmallest.height();
}

// The side for a window of `document`'s size: right or left (whichever has
// more of it; below only without either: the rows grow downwards as one
// types), and over the launcher only when there is no room beside it at
// all. A narrower window beside the list is better than one on top of it:
// that one hides the names, and the pointer can no longer go over the rows.
inline PreviewSide chooseSide(const QRect& area, const QRect& launcher, QSize document)
{
    const auto beside = rooms(area, launcher);
    PreviewSide best = PreviewSide::None;
    qint64 bestArea = 0;
    for (const PreviewSide side : {PreviewSide::Right, PreviewSide::Left}) {
        const QRect& room = beside[static_cast<int>(side)];
        if (!usable(room))
            continue;
        const QSize fits = document.boundedTo(room.size());
        if (const qint64 a = qint64(fits.width()) * fits.height(); a > bestArea) {
            bestArea = a;
            best = side;
        }
    }
    if (best != PreviewSide::None)
        return best;
    if (usable(beside[2])) return PreviewSide::Below;
    if (usable(beside[3])) return PreviewSide::Above;
    return beside[0].width() >= beside[1].width() ? PreviewSide::Over : PreviewSide::OverLeft;
}

} // namespace previewplacement

// The window's place: on `side` when one was chosen and it still has room
// (it does not jump from side to side as the arrows go through small and
// large files), else on the side chosen for `document`. `fit` gives the
// window's size within a room (as large as it would like to be, a picture
// in its proportions). The window's top is level with the launcher's,
// moved up as far as it must to stay on the screen.
template <typename Fit>
    requires std::invocable<Fit, QSize> && std::convertible_to<std::invoke_result_t<Fit, QSize>, QSize>
PreviewPlacement placePreview(const QRect& area, const QRect& launcher, PreviewSide side, QSize document, Fit&& fit)
{
    using namespace previewplacement;
    const QRect inner = area.adjusted(kGap, kGap, -kGap, -kGap);
    if (!launcher.isValid()) {
        QRect rect(QPoint(), fit(inner.size()));
        rect.moveCenter(inner.center());
        return {rect, PreviewSide::Over};
    }
    auto beside = rooms(area, launcher);
    // A small overlap is preferable to flipping across the list when the
    // launcher moves a pixel across the minimum readable width.
    if (side == PreviewSide::Right && beside[0].width() < kSmallest.width()
        && beside[0].width() >= kSmallest.width() - kSideTolerance)
        beside[0].setLeft(std::max(inner.left(), beside[0].right() + 1 - kSmallest.width()));
    if (side == PreviewSide::Left && beside[1].width() < kSmallest.width()
        && beside[1].width() >= kSmallest.width() - kSideTolerance)
        beside[1].setRight(std::min(inner.right(), beside[1].left() + kSmallest.width() - 1));
    const bool overlay = side == PreviewSide::Over || side == PreviewSide::OverLeft;
    if (side == PreviewSide::None || (!overlay && !usable(beside[static_cast<int>(side)])))
        side = chooseSide(area, launcher, document);
    else if (overlay) {
        const auto roomy = [](const QRect& room) { return usable(room) && room.width() >= kSmallest.width() + kSideTolerance; };
        if (roomy(beside[0]) || roomy(beside[1]))
            side = chooseSide(area, launcher, document);
    }

    const auto level = [&](QRect rect, const QRect& room) {
        rect.moveTop(std::clamp(launcher.top(), room.top(), std::max(room.top(), room.bottom() + 1 - rect.height())));
        return rect;
    };
    switch (side) {
    case PreviewSide::Right: {
        const QRect& room = beside[0];
        return {level(QRect(room.topLeft(), fit(room.size())), room), side};
    }
    case PreviewSide::Left: {
        const QRect& room = beside[1];
        QRect rect(room.topLeft(), fit(room.size()));
        rect.moveRight(room.right()); // up against the launcher
        return {level(rect, room), side};
    }
    case PreviewSide::Below: {
        const QRect& room = beside[2];
        return {QRect(room.topLeft(), fit(room.size())), side};
    }
    case PreviewSide::Above: {
        const QRect& room = beside[3];
        QRect rect(room.topLeft(), fit(room.size()));
        rect.moveBottom(room.bottom());
        return {rect, side};
    }
    default:
        break;
    }
    // No room beside it (a wide launcher on a small screen): in front of it,
    // from the screen's edge on the roomier side, over as little of the
    // launcher as a readable window needs, never over its names.
    const bool toRight = side != PreviewSide::OverLeft;
    const int names = launcher.width() * kNamesShare / 100;
    const int roomWidth = std::max(0, toRight ? beside[0].width() : beside[1].width());
    const int most = toRight ? inner.right() + 1 - std::max(inner.left(), launcher.left() + names)
                             : std::min(inner.right() + 1, launcher.right() + 1 - names) - inner.left();
    const int width = std::clamp(std::max(roomWidth, kOverWidth), std::min(kSmallest.width(), inner.width()),
        std::max(std::min(kSmallest.width(), inner.width()), most));
    QRect rect(QPoint(), fit(QSize(width, inner.height())));
    if (toRight)
        rect.moveRight(inner.right());
    else
        rect.moveLeft(inner.left());
    return {level(rect, inner), side};
}

} // namespace ws
