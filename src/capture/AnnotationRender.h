#pragma once

#include "Annotation.h"
#include "AnnotationCommands.h"

#include <QImage>
#include <QRect>

#include <memory>
#include <optional>

// Drawing a document over its base picture (docs/design-capture-roadmap.md,
// 3.3, 3.10, 3.11). The canvas's 256 x 256 tiles and the exported picture go
// through the same render(), so the screen and the export are the same
// pixels. Two things make any area come out exactly as the same pixels of
// the whole picture:
// - every shape is filled as polygons built once per item, relative to a
//   whole pixel of the item's own, with corners on a 1/64 px grid (what the
//   raster engine resolves), so drawing them at another whole pixel offset
//   moves the same coverage; the polygons are split where they cross every
//   256th pixel line, so the parts far outside an area can be pulled in to
//   its edge without changing anything inside, before Qt would clip them
//   itself (it cuts paths reaching 1024 px past the device, and those cut
//   edges depend on the device);
// - level and upright edges of boxes are whole pixels, filled without
//   antialiasing; text is laid out at whole-pixel baselines and drawn once
//   into a layer that areas blend at whole pixels (3.3, 10).
// Moving the document by whole pixels (the overlay's selection becoming the
// project's canvas) moves the drawing by exactly as much, for the same
// reason.
namespace ws::capture::annot {

class Compositor {
public:
    // The caches (drawing recipes, redaction patches, the redactions' mask)
    // share one budget, least recently used first out. clear() them when a
    // capture or the editor ends.
    explicit Compositor(qint64 cacheBytes = 32ll << 20);
    ~Compositor();
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;
    // Draws `area` (image px) of the document into *target, whose top left
    // is the area's top left; a target of another size or format is made
    // again as an opaque Format_RGB32 picture of the area's size. Pixels the
    // base does not cover are black. sampleBounds is S of 3.10: what
    // redactions take their colours from (the selection in the overlay, the
    // whole base in the editor). `skip` is an item not drawn (the text being
    // edited); `unworded` is a Number drawn without its words (those being typed).
    // `draft` is a brush redaction being drawn: shown as a mosaic, which is
    // cheap, whatever its mode (the exported picture never has a draft).
    void render(const Document& doc, const QImage& base, const QRect& sampleBounds, const QRect& area, QImage* target,
        std::optional<quint32> skip = {}, std::optional<quint32> unworded = {}, std::optional<quint32> draft = {});
    // The document's crop (within the canvas; all of it when the crop is
    // empty) drawn as an opaque Format_RGB32 picture.
    QImage exportImage(const Document& doc, const QImage& base, const QRect& sampleBounds);
    // Drops the caches of the changed items; all redaction patches when the
    // redactions' union changed.
    void invalidate(const Change& change);
    void clear();
    // The presenter's (docs/design-presenter.md, 4.3): no picture beneath,
    // the live desktop instead. render() then makes premultiplied ARGB areas
    // cleared to transparent, never reads the base, leaves out redactions
    // (they would sample pixels that are not there) and lays a marker over
    // with its own alpha (multiplying needs pixels beneath). exportImage()
    // follows it; nothing in the presenter exports.
    void setTransparent(bool transparent) { m_transparent = transparent; }
    bool transparent() const { return m_transparent; }
    // The bytes the caches hold now.
    qint64 cacheBytes() const;
    // Redaction patches made so far (the cache missed), for the log.
    qint64 patchesMade() const;

private:
    struct Private;
    std::unique_ptr<Private> d;
    bool m_transparent = false;
};

// U of 3.10 over `sampleBounds`: 255 where any redaction's box is, 0
// elsewhere; Format_Grayscale8 whose top left is sampleBounds' top left.
QImage coverageMask(const Document& doc, const QRect& sampleBounds);
// A redaction's pixels (3.10): an opaque Format_RGB32 picture of its whole
// box, offset() at the box's top left. It reads only base pixels that are in
// sampleBounds, outside `covered` (U, as coverageMask() gives it) and
// outside its own box, so nothing under any redaction can be recovered from
// it. Mosaic: cells from the box's top left, each the inverse-distance mean
// of the nearest readable pixels left, right, above and below its centre
// (none: the mean of all readable pixels, else #808080), plus a brightness
// noise of +-16 per cell from the item's seed. Blur: the same on a finer
// grid (max(4, cell / 2)), bilinear in between, +-4 noise per pixel. Solid:
// the style's colour. Smooth: every pixel from the nearest readable ones, no
// noise. A Brush redaction gives an ARGB32_Premultiplied picture of its box,
// clear outside the stroke; the pixels of its box that the stroke does not
// cover may be read.
QImage redactionPatch(const QImage& base, const QRect& sampleBounds, const QImage& covered, const Item& item);
}
