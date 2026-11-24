#pragma once

#include <QByteArrayView>
#include <QImage>
#include <QPoint>

#include <memory>
#include <vector>

namespace ws::capture::recording {

// The pointer as Windows draws it, for a recording that draws the pointer
// itself (the trail goes under it): `image` is laid over the screen
// (premultiplied ARGB32), `invert` (Alpha8) marks the pixels that invert
// the screen instead, as the default I-beam and other monochrome pointers
// do; null when none does.
struct CursorPicture {
    QImage image;
    QImage invert;
    QPoint hotSpot;
    quint64 id = 0; // a new one for each picture made: the GPU's copy follows it
};

// From a pointer's bitmaps as GetDIBits gives them at 32 bits, top row
// first. `color` is null for a monochrome pointer; `mask` is the AND mask
// (black 0, white 1), for a monochrome pointer twice as high with the XOR
// mask below it. A colour pointer with alpha is drawn by its alpha; one
// without, by its mask: colour where the mask is 0, and where it is 1 the
// screen as it is (black) or inverted (any other colour; white in practice).
CursorPicture cursorPicture(const QImage& color, const QImage& mask, QPoint hotSpot);

// Windows' pointer size on a display at this scale: [HKCU\Control Panel\
// Cursors] CursorBaseSize (32 unless made larger in Settings) times the
// display's scale. The standard pointers show at this size on that display
// (a 120 px arrow for 80 at 150%), whatever size their handle has.
int systemCursorSize(double scale);

// A pointer file's pictures at a size: of a .cur (or .ico), the picture it
// holds for that size as it is, which is what Windows shows (LoadImageW
// draws it again from a larger one, a pixel off at the edges), else the
// next larger one made smaller; of an .ani, its frames in the order and for
// the time its steps give. `ends` says when each picture ends, in 100 ns
// from the first one's start. Both empty when the file cannot be read.
struct CursorFrames {
    std::vector<CursorPicture> pictures;
    std::vector<qint64> ends;
};
CursorFrames cursorFileFrames(QByteArrayView file, int size);

// The pointers' pictures for one recording, on the media worker's thread.
// A standard pointer (arrow, I-beam, hand, ...) is read again from the
// file of the user's pointer scheme at systemCursorSize (cursorFileFrames),
// as on screen; failing that, and for the programs' own pointers, from the handle.
// An animated pointer's frames step with the time.
class CursorShapes {
public:
    explicit CursorShapes(double scale);
    ~CursorShapes();
    CursorShapes(const CursorShapes&) = delete;
    CursorShapes& operator=(const CursorShapes&) = delete;

    // The picture of the pointer whose handle (HCURSOR) this is, at a time
    // in 100 ns; null when it cannot be read.
    const CursorPicture* picture(void* cursor, qint64 time);
    // The standard arrow's picture: the trail takes its colours.
    const CursorPicture* arrow();
    int systemSize() const { return m_size; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    int m_size;
};

} // namespace ws::capture::recording
