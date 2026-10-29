#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QtGlobal>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

// The long screenshot's stitching (docs/design-capture-roadmap.md, 4.2):
// frames of one region of the screen, taken while what is in it scrolls,
// laid one under the other where they overlap. Plain C++ on pictures, no
// windows, so the tests feed it frames directly.
//
// Only two frames next to each other are compared: how many rows the
// content moved up (dy). Each row is cut into 16 segments; a segment with
// some edge in it is compared by its luminance, within a tolerance, since
// programs draw the same text again a little differently (WinUI never gives
// two equal rows after a scroll; stage 6 probe). Candidate shifts come from
// segments of about the same brightness and texture, each candidate is
// checked segment by segment, and the one where the most of the compared
// segments agree wins (score >= 0.6 and >= 24 agreeing segments; or, when a
// picture faded in where the frames overlap, a clear winner: >= 0.35, >= 160
// segments, twice any other shift). Rows at the top and the bottom that stay
// where they are (sticky bars) and segment columns that stay (side bars)
// take no part; a strip on the right (Options::ignoreRight, an overlay
// scroll bar) neither. Newer frames are drawn over older ones (a picture
// that loaded late is in the result) but for the top sixth of each frame,
// where a bar that sticks only once the page scrolls would be. Upward
// growth protects the bottom sixth of the scrolling content instead.
namespace ws::capture {

class ScrollStitcher {
public:
    enum class State {
        Waiting, // no frame yet, or the first
        Appended, // new rows were added at either end
        Unchanged, // nothing moved
        Behind, // moved inside the part already captured: nothing new
        Lost, // no overlap with the frame before
        Full, // at the limit: nothing more is added
    };
    struct Limits {
        int maxHeight = 30000;
        qint64 maxPixels = 60'000'000;
    };
    struct Options {
        int ignoreRight = 0; // px on the right left out of the comparing (still in the picture)
    };
    struct Step {
        State state = State::Waiting;
        int dy = 0; // rows the content moved up since the frame before (negative: down)
        double score = 0; // agreeing / compared segments at dy
        int votes = 0; // agreeing segments at dy
        bool predicted = false; // dy was the caller's guess: nothing fitted
        int top = 0; // the fixed bands known after this step
        int bottom = 0;
        int height = 0; // the result's height after this step
    };

    explicit ScrollStitcher(QSize frame, Limits limits = {}, Options options = {});
    ~ScrollStitcher();

    ScrollStitcher(const ScrollStitcher&) = delete;
    ScrollStitcher& operator=(const ScrollStitcher&) = delete;

    // A region-sized frame (opaque, 32 bits a pixel). `expectDy`: where the
    // caller thinks the content went (what it scrolled by); of shifts that
    // fit equally well (a page that repeats itself) the nearest wins, else
    // the nearest to the last shift. `guess`: when no shift fits, take
    // `expectDy` anyway; the result is then only partly right (partial()).
    Step add(const QImage& frame, std::optional<int> expectDy = std::nullopt, bool guess = false);

    QSize frameSize() const { return m_size; }
    int height() const; // the result's height now (0 before the first frame)
    int frames() const { return m_frames; }
    bool partial() const { return m_partial; }
    bool full() const { return m_full; }
    int fixedTop() const { return m_top; }
    int fixedBottom() const { return m_bottom; }
    // The topmost frame's header + the content + the bottommost frame's
    // footer, Format_RGB32. Either end can grow from the initial viewport.
    QImage result() const;
    // The end most recently extended, scaled to `box`'s width.
    QImage preview(QSize box) const;
    // Gives the memory back (the result was taken); add() starts over.
    void release();

    struct Analysis;

private:
    struct Match;

    uint32_t* row(int y); // the canvas row, its block made when first needed
    const uint32_t* row(int y) const;
    bool write(const QImage& frame, int offset, int from, int to); // false: at the limit
    Match match(const Analysis& a, const Analysis& b, std::optional<int> expectDy);
    bool bounced(const Analysis& end, const Analysis& now, int gone, bool top = false) const;
    int limitHeight() const;

    QSize m_size;
    Limits m_limits;
    Options m_options;
    // Signed content coordinates, so scrolling above the first viewport
    // allocates blocks at the front without moving any captured pixels.
    std::deque<uint32_t*> m_blocks;
    int m_firstBlock = 0;
    std::unique_ptr<Analysis> m_prev; // the last frame that matched
    int m_prevOffset = 0;
    int m_nearOffset = 0; // topmost row of the result, possibly negative
    bool m_previewTop = false;
    QImage m_far; // the frame that went farthest: the result's end comes from it
    int m_farOffset = 0;
    int m_farRows = 0; // of m_far's rows, those in the result (fewer at the limit)
    int m_farFrom = 0; // m_far's first row in the result; the canvas has the rows above
    int m_top = 0;
    int m_bottom = 0;
    bool m_bandsKnown = false;
    int m_velocity = 0; // the last dy that moved
    int m_lost = 0; // frames lost in a row
    int m_frames = 0;
    bool m_partial = false;
    bool m_full = false;
};

QString stateName(ScrollStitcher::State state);

} // namespace ws::capture
