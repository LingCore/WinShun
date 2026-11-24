// The long screenshot's stitching (src/capture/ScrollStitcher) on frames cut
// from a made-up long page (docs/design-capture-roadmap.md, 2.4 item 1): a
// sticky header and footer, a fixed "back to top" button, pictures that fade
// in, zebra rows that repeat themselves, scrolling back, a bounce at the end.
// The result must be the page within 1 a channel (but where the button
// stays), and each step's shift exactly what was scrolled. Then text drawn
// again a little differently (WinUI), lines closer than a step, a side bar
// that stays, frames that do not overlap, the limits; and the diagnostics'
// text (ScrollDiagnostics).
#include "ScrollAutoEngine.h"
#include "ScrollDiagnostics.h"
#include "ScrollGrabber.h"
#include "ScrollStitcher.h"

#include <QElapsedTimer>
#include <QTest>

#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <optional>
#include <random>

using namespace ws::capture;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWidth = 640;
constexpr int kFrame = 480;
constexpr int kHeader = 56;
constexpr int kFooter = 40;
constexpr int kViewport = kFrame - kHeader - kFooter; // 384
constexpr int kBody = 3000;
constexpr int kIgnoreRight = 24; // an overlay scroll bar's strip

constexpr QRgb kPaper = 0xFFFFFFFF;
constexpr QRgb kZebra = 0xFFF0F4F8;
constexpr QRgb kInk = 0xFF222222;

QRgb mix(QRgb from, QRgb to, int amount) // amount 0..255 of `to`
{
    const auto ch = [&](int a, int b) { return a + (b - a) * amount / 255; };
    return qRgb(ch(qRed(from), qRed(to)), ch(qGreen(from), qGreen(to)), ch(qBlue(from), qBlue(to)));
}

// 48 glyphs of 7 x 12 px: random ink with soft edges, like small text.
struct Font {
    std::vector<std::array<uint8_t, 7 * 12>> glyphs;

    Font()
    {
        std::mt19937 rng(7);
        for (int g = 0; g < 48; ++g) {
            std::array<int, 7 * 12> hard {};
            for (int y = 2; y < 11; ++y)
                for (int x = 0; x < 6; ++x)
                    hard[size_t(y * 7 + x)] = rng() % 100 < 42 ? 255 : 0;
            std::array<uint8_t, 7 * 12> soft {};
            for (int y = 0; y < 12; ++y)
                for (int x = 0; x < 7; ++x) {
                    int v = hard[size_t(y * 7 + x)] * 6;
                    if (x > 0)
                        v += hard[size_t(y * 7 + x - 1)];
                    if (x < 6)
                        v += hard[size_t(y * 7 + x + 1)];
                    if (y > 0)
                        v += hard[size_t((y - 1) * 7 + x)];
                    if (y < 11)
                        v += hard[size_t((y + 1) * 7 + x)];
                    soft[size_t(y * 7 + x)] = uint8_t(std::min(255, v / 7));
                }
            glyphs.push_back(soft);
        }
    }
};

const Font& font()
{
    static const Font f;
    return f;
}

// A line of words from x0 to x1 at `top`, on what is there.
void text(QImage& image, int top, int x0, int x1, uint32_t seed)
{
    std::mt19937 rng(seed);
    int x = x0;
    while (x < x1 - 8) {
        const int letters = 2 + int(rng() % 7);
        for (int i = 0; i < letters && x < x1 - 8; ++i, x += 8) {
            const auto& g = font().glyphs[rng() % 48];
            for (int y = 0; y < 12; ++y) {
                auto* row = reinterpret_cast<QRgb*>(image.scanLine(top + y));
                for (int gx = 0; gx < 7; ++gx)
                    if (const int ink = g[size_t(y * 7 + gx)])
                        row[x + gx] = mix(row[x + gx], kInk, ink);
            }
        }
        x += 6;
    }
}

void fill(QImage& image, int top, int rows, QRgb colour)
{
    for (int y = top; y < top + rows; ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        std::fill(row, row + image.width(), colour);
    }
}

// A picture: a soft gradient with shapes on it, different for each seed.
void picture(QImage& image, const QRect& r, uint32_t seed)
{
    std::mt19937 rng(seed);
    const int hue = int(rng() % 120);
    struct Shape {
        int x, y, size;
        QRgb colour;
    };
    std::vector<Shape> shapes;
    for (int i = 0; i < 6; ++i)
        shapes.push_back({int(rng() % uint32_t(r.width())), int(rng() % uint32_t(r.height())), 10 + int(rng() % 30),
            qRgb(int(rng() % 200), int(rng() % 200), int(rng() % 200))});
    for (int y = 0; y < r.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(r.top() + y));
        for (int x = 0; x < r.width(); ++x) {
            QRgb p = qRgb(40 + hue + x / 3, 90 + y / 2, 150 + (x + y) % 60);
            for (const Shape& s : shapes) {
                const int dx = x - s.x;
                const int dy = y - s.y;
                if (dx * dx + dy * dy < s.size * s.size)
                    p = s.colour;
            }
            row[r.left() + x] = p;
        }
    }
}

// The made-up page: its header, body and footer, and a frame of it.
struct Page {
    QImage header;
    QImage body;
    QImage footer;
    QImage truth; // header + body + footer
    std::vector<QRect> pictures; // in the body
    QRect button; // in a frame
    bool withButton = true;

    Page()
    {
        header = QImage(kWidth, kHeader, QImage::Format_RGB32);
        fill(header, 0, kHeader, qRgb(32, 64, 128));
        for (int x = 24; x < 120; ++x) // a logo
            for (int y = 18; y < 38; ++y)
                header.setPixel(x, y, (x / 6 + y / 5) % 2 ? qRgb(240, 240, 240) : qRgb(32, 64, 128));
        text(header, 22, 200, 560, 1);
        footer = QImage(kWidth, kFooter, QImage::Format_RGB32);
        fill(footer, 0, kFooter, qRgb(236, 236, 236));
        fill(footer, 0, 1, qRgb(200, 200, 200));
        text(footer, 14, 24, 400, 2);

        body = QImage(kWidth, kBody, QImage::Format_RGB32);
        fill(body, 0, kBody, kPaper);
        uint32_t seed = 100;
        int y = 16;
        const auto paragraph = [&](int lines) {
            for (int i = 0; i < lines && y < kBody - 60; ++i, y += 20)
                text(body, y + 4, 24, i == lines - 1 ? 300 : kWidth - 48, seed++);
            y += 16;
        };
        const auto image = [&](bool left) {
            const QRect r(left ? 40 : 360, y, 240, 120);
            picture(body, r, seed++);
            pictures.push_back(r);
            y += 120 + 16;
        };
        const auto table = [&](int rows, int repeatFrom, int repeatTo) {
            fill(body, y, 28, qRgb(225, 230, 235));
            text(body, y + 8, 24, 600, seed++);
            y += 28;
            for (int i = 0; i < rows; ++i, y += 24) {
                fill(body, y, 24, i % 2 ? kZebra : kPaper);
                // Rows repeatFrom..repeatTo say the same: alike two rows apart.
                const uint32_t s = i >= repeatFrom && i <= repeatTo ? 9000 : seed++;
                text(body, y + 6, 24, 180, s);
                text(body, y + 6, 220, 400, s + 1);
                text(body, y + 6, 440, 600, s + 2);
            }
            y += 16;
        };
        paragraph(8);
        image(true);
        paragraph(5);
        table(30, 8, 19);
        paragraph(10);
        image(false);
        paragraph(6);
        table(12, -1, -1);
        paragraph(9);
        image(true);
        paragraph(7);
        table(10, 3, 7);
        while (y < kBody - 120)
            paragraph(6);
        Q_ASSERT(y <= kBody - 40);
        text(body, kBody - 30, 24, 400, seed++); // the page's last line

        truth = QImage(kWidth, kHeader + kBody + kFooter, QImage::Format_RGB32);
        for (int r = 0; r < truth.height(); ++r) {
            const QImage& src = r < kHeader ? header : r < kHeader + kBody ? body : footer;
            const int sy = r < kHeader ? r : r < kHeader + kBody ? r - kHeader : r - kHeader - kBody;
            std::memcpy(truth.scanLine(r), src.constScanLine(sy), size_t(kWidth) * 4);
        }
        button = QRect(kWidth - kIgnoreRight - 16 - 44, kFrame - kFooter - 16 - 44, 44, 44);
    }

    // The screen at `scroll` (rows past the body's end are paper: a page
    // pulled past its end); picture i drawn at alpha[i] (0..1).
    QImage frame(int scroll, const std::vector<double>& alpha = {}) const
    {
        QImage f(kWidth, kFrame, QImage::Format_RGB32);
        for (int y = 0; y < kFrame; ++y) {
            auto* row = reinterpret_cast<QRgb*>(f.scanLine(y));
            if (y < kHeader) {
                std::memcpy(row, header.constScanLine(y), size_t(kWidth) * 4);
            } else if (y >= kFrame - kFooter) {
                std::memcpy(row, footer.constScanLine(y - (kFrame - kFooter)), size_t(kWidth) * 4);
            } else {
                const int r = scroll + y - kHeader;
                if (r >= 0 && r < kBody)
                    std::memcpy(row, body.constScanLine(r), size_t(kWidth) * 4);
                else
                    std::fill(row, row + kWidth, kPaper);
            }
        }
        for (size_t i = 0; i < pictures.size() && i < alpha.size(); ++i) {
            if (alpha[i] >= 1)
                continue;
            const QRect& p = pictures[i];
            for (int r = p.top(); r <= p.bottom(); ++r) {
                const int y = r - scroll + kHeader;
                if (y < kHeader || y >= kFrame - kFooter)
                    continue;
                auto* row = reinterpret_cast<QRgb*>(f.scanLine(y));
                for (int x = p.left(); x <= p.right(); ++x)
                    row[x] = mix(kPaper, row[x], int(alpha[i] * 255));
            }
        }
        if (withButton) {
            for (int y = button.top(); y <= button.bottom(); ++y) {
                auto* row = reinterpret_cast<QRgb*>(f.scanLine(y));
                for (int x = button.left(); x <= button.right(); ++x) {
                    const int cx = x - button.center().x();
                    const int cy = y - button.center().y();
                    const bool chevron = cy >= -6 && cy <= 4 && std::abs(std::abs(cx) - (cy + 6)) <= 1;
                    row[x] = chevron ? qRgb(255, 255, 255) : qRgb(0, 103, 192);
                }
            }
        }
        return f;
    }
};

const Page& page()
{
    static const Page p;
    return p;
}

// Plays `positions` (the page's scroll at each frame) through a stitcher;
// pictures fade in: at 0.4 the first frame they show in, whole after.
// Checks each step's state and shift; returns the stitcher's result.
struct Replay {
    std::vector<ScrollStitcher::Step> steps;
    QImage result;
    int fixedTop = 0;
    int fixedBottom = 0;
};

using Distort = std::function<QImage(QImage, int frame)>;

// `sideways`: the frames turned a quarter (transposed()) into a horizontal
// stitcher, its result turned back.
void replay(Replay& out, const Page& p, const std::vector<int>& positions, std::optional<int> expectError = std::nullopt,
    const Distort& distort = {}, bool sideways = false)
{
    ScrollStitcher stitcher(sideways ? QSize(kFrame, kWidth) : QSize(kWidth, kFrame), {}, {kIgnoreRight, sideways});
    std::vector<int> seen(p.pictures.size(), -1);
    int farthest = 0;
    for (size_t t = 0; t < positions.size(); ++t) {
        const int s = positions[t];
        std::vector<double> alpha(p.pictures.size(), 1.0);
        for (size_t i = 0; i < p.pictures.size(); ++i) {
            const QRect& r = p.pictures[i];
            const bool visible = r.bottom() >= s && r.top() < s + kViewport;
            if (visible && seen[i] < 0)
                seen[i] = int(t);
            if (seen[i] == int(t))
                alpha[i] = 0.4;
        }
        QImage f = p.frame(s, alpha);
        if (distort)
            f = distort(f, int(t));
        if (sideways)
            f = transposed(f);
        std::optional<int> expect;
        const int dy = t ? s - positions[t - 1] : 0;
        if (expectError && t && dy)
            expect = dy + *expectError;
        const ScrollStitcher::Step step = stitcher.add(f, expect);
        out.steps.push_back(step);
        const QByteArray where = QByteArray("frame ") + QByteArray::number(int(t)) + " at " + QByteArray::number(s);
        if (t == 0) {
            QVERIFY2(step.state == ScrollStitcher::State::Appended, where);
        } else if (dy == 0) {
            QVERIFY2(step.state == ScrollStitcher::State::Unchanged, where + " " + stateName(step.state).toUtf8());
        } else {
            QVERIFY2(step.state == (s > farthest ? ScrollStitcher::State::Appended : ScrollStitcher::State::Behind),
                where + " " + stateName(step.state).toUtf8() + " dy " + QByteArray::number(step.dy));
            QVERIFY2(step.dy == dy, where + " dy " + QByteArray::number(step.dy) + " not " + QByteArray::number(dy));
            QVERIFY(!step.predicted);
        }
        farthest = std::max(farthest, s);
    }
    out.result = sideways ? transposed(stitcher.result()) : stitcher.result();
    out.fixedTop = stitcher.fixedTop();
    out.fixedBottom = stitcher.fixedBottom();
    QVERIFY(!stitcher.partial());
}

// The largest difference of a channel between `a` and `b`, outside `skip`;
// `at` gets where.
int difference(const QImage& a, const QImage& b, const QRect& skip, QPoint* at = nullptr)
{
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto* ra = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const auto* rb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            if (skip.contains(x, y))
                continue;
            const int d = std::max({std::abs(qRed(ra[x]) - qRed(rb[x])), std::abs(qGreen(ra[x]) - qGreen(rb[x])),
                std::abs(qBlue(ra[x]) - qBlue(rb[x]))});
            if (d > worst) {
                worst = d;
                if (at)
                    *at = QPoint(x, y);
            }
        }
    }
    return worst;
}

// Down the page in steps of 160 to 180 (two steps never more than the
// viewport less a sixth, so a picture that faded in is drawn again whole;
// 500 to 670 overlaps only rows that repeat), still frames, back up and down
// again, past the end and back (a bounce).
std::vector<int> pagePositions()
{
    return {0, 0, 160, 330, 500, 670, 670, 840, 1005, 1170, 1330, 1490, 1320, 1180, 1350, 1530, 1700, 1870, 2040,
        2210, 2380, 2380, 2550, 2616, 2676, 2616, 2616};
}

} // namespace

class TestStitch : public QObject {
    Q_OBJECT

private slots:
    void pageReplayed()
    {
        const Page& p = page();
        const std::vector<int> positions = pagePositions();
        Replay r;
        replay(r, p, positions);
        if (QTest::currentTestFailed())
            return;
        const int last = 2616; // where it settled: the end
        QCOMPARE(r.result.size(), p.truth.size());
        QCOMPARE(r.fixedTop, kHeader);
        QCOMPARE(r.fixedBottom, kFooter);
        // The button stays only where the last frame had it.
        const QRect button = p.button.translated(0, last);
        QPoint at;
        const int d = difference(r.result, p.truth, button, &at);
        QVERIFY2(d <= 1, qPrintable(u"%1 at %2,%3"_s.arg(d).arg(at.x()).arg(at.y())));
        QCOMPARE(r.result.copy(button), p.frame(last).copy(p.button));
    }

    // Content scrolled sideways (Options::horizontal, feedback 2026-11-16):
    // the page turned a quarter, frames and all, comes out as the page
    // turned; the preview is the right end, as high as the box.
    void sideways()
    {
        const Page& p = page();
        QCOMPARE(transposed(transposed(p.frame(330))), p.frame(330));
        QCOMPARE(transposed(p.frame(0)).size(), QSize(kFrame, kWidth));
        Replay r;
        replay(r, p, pagePositions(), std::nullopt, {}, true);
        if (QTest::currentTestFailed())
            return;
        QCOMPARE(r.result.size(), p.truth.size());
        QCOMPARE(r.fixedTop, kHeader); // the left and the right that stay
        QCOMPARE(r.fixedBottom, kFooter);
        QPoint at;
        const int d = difference(r.result, p.truth, p.button.translated(0, 2616), &at);
        QVERIFY2(d <= 1, qPrintable(u"%1 at %2,%3"_s.arg(d).arg(at.x()).arg(at.y())));

        ScrollStitcher stitcher(QSize(kFrame, kWidth), {}, {kIgnoreRight, true});
        QCOMPARE(stitcher.frameSize(), QSize(kWidth, kFrame));
        QCOMPARE(stitcher.add(transposed(p.frame(0))).state, ScrollStitcher::State::Appended);
        QCOMPARE(stitcher.add(transposed(p.frame(170))).dy, 170);
        QCOMPARE(stitcher.result().size(), QSize(kFrame + 170, kWidth));
        const QImage preview = stitcher.preview(QSize(300, 200));
        QCOMPARE(preview.height(), 200);
        QVERIFY(preview.width() > 0 && preview.width() <= 300);
        // Frames the other way round (not turned) are not taken.
        QCOMPARE(stitcher.add(p.frame(340)).state, ScrollStitcher::State::Lost);
    }

    // Auto-scroll knows about how far it scrolled: off by a few px, it
    // still comes out exact.
    void pageWithExpectedShifts()
    {
        const Page& p = page();
        Replay r;
        replay(r, p, pagePositions(), 7);
        if (QTest::currentTestFailed())
            return;
        QCOMPARE(r.result.size(), p.truth.size());
        QVERIFY(difference(r.result, p.truth, p.button.translated(0, 2616)) <= 1);
    }

    // Scrolled by hand: many small steps, some frames twice.
    void pageInSmallSteps()
    {
        const Page& p = page();
        std::vector<int> positions {0};
        std::mt19937 rng(3);
        while (positions.back() < kBody - kViewport) {
            const int step = 6 + int(rng() % 40);
            positions.push_back(std::min(kBody - kViewport, positions.back() + step));
            if (rng() % 5 == 0)
                positions.push_back(positions.back());
        }
        Replay r;
        replay(r, p, positions);
        if (QTest::currentTestFailed())
            return;
        QCOMPARE(r.result.size(), p.truth.size());
        QPoint at;
        const int d = difference(r.result, p.truth, p.button.translated(0, kBody - kViewport), &at);
        QVERIFY2(d <= 1, qPrintable(u"%1 at %2,%3"_s.arg(d).arg(at.x()).arg(at.y())));
    }

    void upwardFromBottom()
    {
        Page p;
        p.withButton = false;
        ScrollStitcher stitcher(QSize(kWidth, kFrame), {}, {kIgnoreRight});
        int previous = kBody - kViewport;
        QCOMPARE(stitcher.add(p.frame(previous)).state, ScrollStitcher::State::Appended);
        while (previous > 0) {
            const int position = std::max(0, previous - 160);
            const auto step = stitcher.add(p.frame(position));
            QCOMPARE(step.state, ScrollStitcher::State::Appended);
            QCOMPARE(step.dy, position - previous);
            QCOMPARE(step.height, kBody - kViewport - position + kFrame);
            previous = position;
        }
        QCOMPARE(stitcher.fixedTop(), kHeader);
        QCOMPARE(stitcher.fixedBottom(), kFooter);
        QCOMPARE(stitcher.result(), p.truth);
        QVERIFY(!stitcher.partial());
        // While extending upwards, the preview follows the new top.
        QCOMPARE(stitcher.preview(QSize(kWidth, 120)), p.truth.copy(0, 0, kWidth, 120));
        // An elastic overscroll at the top is trimmed when it springs back.
        QCOMPARE(stitcher.add(p.frame(-60)).state, ScrollStitcher::State::Appended);
        QCOMPARE(stitcher.add(p.frame(0)).state, ScrollStitcher::State::Behind);
        QCOMPARE(stitcher.result(), p.truth);
    }

    void extendBothEnds()
    {
        Page p;
        p.withButton = false;
        ScrollStitcher stitcher(QSize(kWidth, kFrame), {}, {kIgnoreRight});
        const std::vector<int> positions {1280, 1120, 960, 800, 640, 800, 960, 1120, 1280, 1440,
            1600, 1760, 1920, 2080, 2240, 2400, 2560, 2616, 2456, 2296, 2136, 1976, 1816,
            1656, 1496, 1336, 1176, 1016, 856, 696, 536, 376, 216, 56, 0};
        int nearest = positions.front();
        int farthest = nearest;
        int previous = nearest;
        for (size_t i = 0; i < positions.size(); ++i) {
            const int position = positions[i];
            const auto step = stitcher.add(p.frame(position));
            const bool grows = !i || position < nearest || position > farthest;
            QCOMPARE(step.state, grows ? ScrollStitcher::State::Appended : ScrollStitcher::State::Behind);
            if (i)
                QCOMPARE(step.dy, position - previous);
            nearest = std::min(nearest, position);
            farthest = std::max(farthest, position);
            QCOMPARE(step.height, farthest - nearest + kFrame);
            previous = position;
        }
        QCOMPARE(stitcher.result(), p.truth);
        // Revisited content must not duplicate rows or shrink the result.
        QCOMPARE(stitcher.add(p.frame(160)).state, ScrollStitcher::State::Behind);
        QCOMPARE(stitcher.result(), p.truth);
    }

    void upwardWithFloatingButton()
    {
        const Page& p = page();
        ScrollStitcher stitcher(QSize(kWidth, kFrame), {}, {kIgnoreRight});
        int position = kBody - kViewport;
        stitcher.add(p.frame(position));
        while (position > 0) {
            position = std::max(0, position - 160);
            QCOMPARE(stitcher.add(p.frame(position)).state, ScrollStitcher::State::Appended);
        }
        const QRect button = p.button.translated(0, kBody - kViewport);
        QVERIFY(difference(stitcher.result(), p.truth, button) <= 1);
        QCOMPARE(stitcher.result().copy(button), p.frame(kBody - kViewport).copy(p.button));
    }

    void upwardLimitAndRelease()
    {
        Page p;
        p.withButton = false;
        ScrollStitcher stitcher(QSize(kWidth, kFrame), {1000, 60'000'000}, {kIgnoreRight});
        for (int position : {1600, 1440, 1280, 1120, 960}) {
            const auto step = stitcher.add(p.frame(position));
            if (step.state == ScrollStitcher::State::Full)
                break;
        }
        QVERIFY(stitcher.full());
        QCOMPARE(stitcher.height(), 1000);
        const QImage result = stitcher.result();
        QCOMPARE(result.size(), QSize(kWidth, 1000));
        QCOMPARE(result.copy(0, 1000 - kFooter, kWidth, kFooter), p.footer);
        stitcher.release();
        QCOMPARE(stitcher.height(), 0);
        QCOMPARE(stitcher.add(p.frame(0)).state, ScrollStitcher::State::Appended);
        QCOMPARE(stitcher.result(), p.frame(0));
    }

    void signedBlockEdgesAndLostRecovery()
    {
        Page p;
        p.withButton = false;
        ScrollStitcher stitcher(QSize(kWidth, kFrame), {}, {kIgnoreRight});
        stitcher.add(p.frame(900));
        for (int position : {899, 644, 388, 132, 0})
            QCOMPARE(stitcher.add(p.frame(position)).state, ScrollStitcher::State::Appended);
        QImage expected(kWidth, 900 + kFrame, QImage::Format_RGB32);
        for (int y = 0; y < expected.height(); ++y) {
            const QImage& source = y < kHeader ? p.header : y < expected.height() - kFooter ? p.body : p.footer;
            const int row = y < kHeader ? y : y < expected.height() - kFooter ? y - kHeader : y - (expected.height() - kFooter);
            std::memcpy(expected.scanLine(y), source.constScanLine(row), size_t(kWidth) * 4);
        }
        QCOMPARE(stitcher.result(), expected);
        const QImage before = stitcher.result();
        QCOMPARE(stitcher.add(p.frame(2200)).state, ScrollStitcher::State::Lost);
        QCOMPARE(stitcher.result(), before);
        QCOMPARE(stitcher.add(p.frame(0)).state, ScrollStitcher::State::Unchanged);
        QCOMPARE(stitcher.add(p.frame(160)).state, ScrollStitcher::State::Behind);
        QCOMPARE(stitcher.result(), before);
    }

    // Text drawn again a little differently each frame (WinUI: up to 8
    // levels on the edges of letters): the shifts are still exact.
    void redrawnText()
    {
        const Page& p = page();
        Replay r;
        replay(r, p, pagePositions(), std::nullopt, [](QImage f, int t) {
            std::mt19937 rng(uint32_t(1000 + t));
            for (int y = 0; y < f.height(); ++y) {
                auto* row = reinterpret_cast<QRgb*>(f.scanLine(y));
                for (int x = f.width() - 1; x > 0; --x) {
                    if (std::abs(qGray(row[x]) - qGray(row[x - 1])) <= 24)
                        continue;
                    const int d = int(rng() % 9) - 4; // two frames differ by up to 8
                    row[x] = qRgb(std::clamp(qRed(row[x]) + d, 0, 255), std::clamp(qGreen(row[x]) + d, 0, 255),
                        std::clamp(qBlue(row[x]) + d, 0, 255));
                }
            }
            return f;
        });
        if (QTest::currentTestFailed())
            return;
        QCOMPARE(r.result.height(), p.truth.height());
    }

    // An overlay scroll bar that moves in the strip left out of the comparing.
    void scrollBarIgnored()
    {
        const Page& p = page();
        Replay r;
        replay(r, p, pagePositions(), std::nullopt, [](QImage f, int t) {
            const int thumb = 56 + (t * 37) % 300;
            for (int y = 0; y < f.height(); ++y) {
                auto* row = reinterpret_cast<QRgb*>(f.scanLine(y));
                for (int x = kWidth - 10; x < kWidth - 4; ++x)
                    row[x] = y >= thumb && y < thumb + 60 ? qRgb(120, 120, 120) : qRgb(245, 245, 245);
            }
            return f;
        });
        if (QTest::currentTestFailed())
            return;
        QCOMPARE(r.result.height(), p.truth.height());
    }

    // Lines of text 18 px apart (Word), steps much longer than that.
    void linesCloserThanAStep()
    {
        QImage doc(kWidth, 4000, QImage::Format_RGB32);
        fill(doc, 0, doc.height(), kPaper);
        for (int y = 10, i = 0; y < doc.height() - 20; y += 18, ++i)
            text(doc, y, 60, i % 9 == 8 ? 260 : 580, uint32_t(5000 + i));
        const auto frameAt = [&](int s) { return doc.copy(0, s, kWidth, kFrame); };
        ScrollStitcher stitcher(QSize(kWidth, kFrame), {}, {kIgnoreRight});
        stitcher.add(frameAt(0));
        int s = 0;
        std::mt19937 rng(11);
        for (int i = 0; i < 14; ++i) {
            const int dy = 150 + int(rng() % 140);
            s += dy;
            const ScrollStitcher::Step step = stitcher.add(frameAt(s));
            QCOMPARE(step.state, ScrollStitcher::State::Appended);
            QCOMPARE(step.dy, dy);
        }
        const QImage result = stitcher.result();
        QCOMPARE(result.height(), s + kFrame);
        QCOMPARE(result, doc.copy(0, 0, kWidth, s + kFrame));
    }

    // A side bar that stays while the rest scrolls (a file manager's tree).
    void sideBarStays()
    {
        const Page& p = page();
        std::vector<int> positions = pagePositions();
        ScrollStitcher stitcher(QSize(kWidth, kFrame), {}, {kIgnoreRight});
        QImage bar(120, kFrame, QImage::Format_RGB32);
        fill(bar, 0, kFrame, qRgb(243, 243, 243));
        for (int y = 70; y < kFrame - 60; y += 22)
            text(bar, y, 12, 110, uint32_t(y));
        int prev = 0;
        for (size_t t = 0; t < positions.size(); ++t) {
            QImage f = p.frame(positions[t]);
            for (int y = 0; y < kFrame; ++y)
                std::memcpy(f.scanLine(y), bar.constScanLine(y), 120 * 4);
            const ScrollStitcher::Step step = stitcher.add(f);
            if (t && positions[t] != prev)
                QCOMPARE(step.dy, positions[t] - prev);
            prev = positions[t];
        }
        QCOMPARE(stitcher.height(), p.truth.height());
    }

    // A window that is mostly bars (words in the header, the footer and a
    // wide side bar): a jump past the overlap is lost, not taken for
    // nothing having moved, before the bars are known and after.
    void jumpWithBarsIsLost()
    {
        const Page& p = page();
        QImage bar(240, kFrame, QImage::Format_RGB32);
        fill(bar, 0, kFrame, qRgb(243, 243, 243));
        for (int y = 64; y < kFrame - 48; y += 20)
            text(bar, y, 12, 228, uint32_t(700 + y));
        const auto frameAt = [&](int s) {
            QImage f = p.frame(s);
            for (int y = 0; y < kFrame; ++y)
                std::memcpy(f.scanLine(y), bar.constScanLine(y), 240 * 4);
            return f;
        };
        ScrollStitcher first(QSize(kWidth, kFrame), {}, {kIgnoreRight});
        first.add(frameAt(0));
        QCOMPARE(first.add(frameAt(1200)).state, ScrollStitcher::State::Lost);
        QCOMPARE(first.height(), kFrame);

        ScrollStitcher stitcher(QSize(kWidth, kFrame), {}, {kIgnoreRight});
        for (int s : {0, 160, 330})
            stitcher.add(frameAt(s));
        QCOMPARE(stitcher.height(), 330 + kFrame);
        const QImage before = stitcher.result();
        const ScrollStitcher::Step step = stitcher.add(frameAt(1200));
        QCOMPARE(step.state, ScrollStitcher::State::Lost);
        QCOMPARE(stitcher.height(), 330 + kFrame);
        QCOMPARE(stitcher.result(), before);
        // The next frame that overlaps the last one taken still fits.
        QCOMPARE(stitcher.add(frameAt(490)).dy, 160);
    }

    // Rows alike but for a number (a file list) beside a tree that stays,
    // scrolled by most of a frame at a time: every multiple of a row's height
    // gets votes from what all rows have, the true shift from fewer rows
    // (Explorer, live test 2026-10-22: 774 of 1106 px was never tried).
    void alikeRowsFarApart()
    {
        constexpr int kRow = 43;
        QImage list(kWidth, 4000, QImage::Format_RGB32);
        fill(list, 0, list.height(), kPaper);
        for (int y = 8, i = 0; y < list.height() - kRow; y += kRow, ++i) {
            text(list, y + 15, 160, 220, 42); // the same word before the number
            text(list, y + 15, 228, 252, uint32_t(9100 + i));
            text(list, y + 15, 260, 300, 43);
            text(list, y + 15, 420, 560, 44); // the same date
        }
        QImage tree(140, kFrame, QImage::Format_RGB32);
        fill(tree, 0, kFrame, qRgb(243, 243, 243));
        for (int y = 30; y < kFrame - 30; y += 48)
            text(tree, y, 16, 120, uint32_t(800 + y));
        const auto frameAt = [&](int s) {
            QImage f = list.copy(0, s, kWidth, kFrame);
            for (int y = 0; y < kFrame; ++y)
                std::memcpy(f.scanLine(y), tree.constScanLine(y), 140 * 4);
            return f;
        };
        ScrollStitcher stitcher(QSize(kWidth, kFrame), {}, {kIgnoreRight});
        stitcher.add(frameAt(0));
        int s = 0;
        for (int dy : {95, 340, 333, 351, 300, 345}) {
            s += dy;
            const ScrollStitcher::Step step = stitcher.add(frameAt(s));
            QCOMPARE(step.state, ScrollStitcher::State::Appended);
            QCOMPARE(step.dy, dy);
        }
        QCOMPARE(stitcher.height(), s + kFrame);
    }

    // How long a frame takes at real sizes (manual scrolling stitches every
    // frame it gets: the slower, the farther apart they are).
    void timing_data()
    {
        QTest::addColumn<int>("w");
        QTest::addColumn<int>("h");
        QTest::addRow("1500x1300") << 1500 << 1300;
        QTest::addRow("2560x1440") << 2560 << 1440;
    }

    void timing()
    {
        QFETCH(int, w);
        QFETCH(int, h);
        QImage doc(w, h * 4, QImage::Format_RGB32);
        fill(doc, 0, doc.height(), kPaper);
        for (int y = 12, i = 0; y < doc.height() - 20; y += 22, ++i)
            text(doc, y, 40, i % 7 == 6 ? w / 3 : w - 60, uint32_t(20000 + i));
        for (int y = 200; y + 300 < doc.height(); y += 900)
            picture(doc, QRect(w / 2, y, 400, 300), uint32_t(y));
        std::vector<QImage> frames;
        for (int s = 0; s + h <= doc.height(); s += h / 3)
            frames.push_back(doc.copy(0, s, w, h));
        ScrollStitcher stitcher(QSize(w, h), {}, {24});
        QElapsedTimer clock;
        clock.start();
        stitcher.add(frames[0]);
        const qint64 firstMs = clock.elapsed();
        clock.restart();
        for (size_t i = 1; i < frames.size(); ++i) {
            const ScrollStitcher::Step step = stitcher.add(frames[i]);
            QCOMPARE(step.state, ScrollStitcher::State::Appended);
            QCOMPARE(step.dy, h / 3);
        }
        const double each = double(clock.elapsed()) / double(frames.size() - 1);
        clock.restart();
        const QImage result = stitcher.result();
        const qint64 resultMs = clock.elapsed();
        QCOMPARE(result, doc.copy(0, 0, w, int(frames.size() - 1) * (h / 3) + h));
        qInfo().noquote() << u"%1 x %2: first frame %3 ms, each next %4 ms, result %5 ms"_s.arg(w).arg(h).arg(firstMs)
                                 .arg(each, 0, 'f', 1).arg(resultMs);
    }

    // Frames that do not overlap: lost; with a guess, taken, partly right.
    void lostAndGuessed()
    {
        const Page& p = page();
        ScrollStitcher stitcher(QSize(kWidth, kFrame));
        stitcher.add(p.frame(0));
        QCOMPARE(stitcher.add(p.frame(150)).dy, 150);
        ScrollStitcher::Step step = stitcher.add(p.frame(900));
        QCOMPARE(step.state, ScrollStitcher::State::Lost);
        step = stitcher.add(p.frame(900), 750);
        QCOMPARE(step.state, ScrollStitcher::State::Lost); // not without `guess`
        QVERIFY(!stitcher.partial());
        step = stitcher.add(p.frame(900), 750, true);
        QCOMPARE(step.state, ScrollStitcher::State::Appended);
        QVERIFY(step.predicted);
        QCOMPARE(step.dy, 750);
        QVERIFY(stitcher.partial());
        QCOMPARE(stitcher.height(), 900 + kFrame);
        // Back on track from there.
        step = stitcher.add(p.frame(1060));
        QCOMPARE(step.dy, 160);
        QVERIFY(!step.predicted);
        // A frame of another size is not taken.
        QCOMPARE(stitcher.add(QImage(10, 10, QImage::Format_RGB32)).state, ScrollStitcher::State::Lost);
        // Nothing to compare by (plain paper): lost too.
        ScrollStitcher blank(QSize(kWidth, kFrame));
        QImage paper(kWidth, kFrame, QImage::Format_RGB32);
        paper.fill(kPaper);
        blank.add(paper);
        QImage grey(kWidth, kFrame, QImage::Format_RGB32);
        grey.fill(qRgb(200, 200, 200));
        QCOMPARE(blank.add(grey).state, ScrollStitcher::State::Lost);
    }

    // At the limit it stops, the result as high as the limit.
    void limits()
    {
        const Page& p = page();
        ScrollStitcher stitcher(QSize(kWidth, kFrame), {1200, 60'000'000});
        ScrollStitcher::State last = ScrollStitcher::State::Waiting;
        for (int s : pagePositions()) {
            last = stitcher.add(p.frame(s)).state;
            if (last == ScrollStitcher::State::Full)
                break;
        }
        QCOMPARE(last, ScrollStitcher::State::Full);
        QVERIFY(stitcher.full());
        QCOMPARE(stitcher.height(), 1200);
        QCOMPARE(stitcher.add(p.frame(2000)).state, ScrollStitcher::State::Full);
        const QImage result = stitcher.result();
        QCOMPARE(result.height(), 1200);
        QVERIFY(difference(result, p.truth.copy(0, 0, kWidth, 1200), p.button.translated(0, 840)) <= 1);
        // By pixels: 640 x 1000 = 640 000.
        ScrollStitcher byPixels(QSize(kWidth, kFrame), {30000, 640'000});
        for (int s : pagePositions())
            if (byPixels.add(p.frame(s)).state == ScrollStitcher::State::Full)
                break;
        QCOMPARE(byPixels.height(), 1000);
        // Given back: starts over.
        stitcher.release();
        QCOMPARE(stitcher.height(), 0);
        QVERIFY(!stitcher.full());
        QVERIFY(stitcher.result().isNull());
        QCOMPARE(stitcher.add(p.frame(0)).state, ScrollStitcher::State::Appended);
        QCOMPARE(stitcher.height(), kFrame);
    }

    // The panel's picture: the end of the result, the box's width.
    void preview()
    {
        const Page& p = page();
        ScrollStitcher stitcher(QSize(kWidth, kFrame));
        for (int s : {0, 150, 300, 470})
            stitcher.add(p.frame(s));
        const QImage small = stitcher.preview(QSize(160, 200));
        QCOMPARE(small.width(), 160);
        QCOMPARE(small.height(), 200);
        // Its last row is the footer's, scaled.
        const QRgb footer = p.footer.pixel(600, kFooter - 2);
        const QRgb shown = small.pixel(150, 199);
        QVERIFY(std::abs(qGray(footer) - qGray(shown)) <= 8);
        // A short result: all of it.
        ScrollStitcher one(QSize(kWidth, kFrame));
        one.add(p.frame(0));
        QCOMPARE(one.preview(QSize(160, 1000)).height(), kFrame / 4);
    }

    // The scroll ladder (ScrollInput.h) as the stage 6 probe found the programs.
    void ladderPostedWheelWorks() // Notepad, Word
    {
        ScrollLadder ladder;
        QCOMPARE(ladder.method(), ScrollMethod::PostWheel);
        QCOMPARE(ladder.amount(400), 1.0); // nothing measured: one notch
        QCOMPARE(ladder.expected(1), 0);
        ladder.moved(92, 1);
        QCOMPARE(ladder.method(), ScrollMethod::PostWheel);
        QCOMPARE(ladder.amount(400), 4.0); // 92 a notch
        QCOMPARE(ladder.expected(4), 368);
        ladder.moved(368, 4);
        // The end: it stops; the mouse is not over the region, UI Automation says the end.
        ladder.moved(0, 4);
        QCOMPARE(ladder.method(), ScrollMethod::SendWheel);
        ladder.refused(ScrollCursorOutside);
        QCOMPARE(ladder.method(), ScrollMethod::Uia);
        QVERIFY(!ladder.atEnd());
        ladder.refused(ScrollAtEnd);
        QCOMPARE(ladder.method(), ScrollMethod::PostWheel);
        QVERIFY(!ladder.atEnd());
        ladder.moved(0, 4);
        QVERIFY(ladder.atEnd());
        QCOMPARE(ladder.method(), ScrollMethod::Manual);
        QCOMPARE(ladder.hint(), ScrollLadder::Hint::None);
    }

    void ladderChromium() // posted wheels do nothing; UI Automation scrolls
    {
        ScrollLadder ladder;
        ladder.moved(0, 1);
        QCOMPARE(ladder.method(), ScrollMethod::SendWheel);
        ladder.refused(ScrollCursorOutside);
        QCOMPARE(ladder.method(), ScrollMethod::Uia);
        QCOMPARE(ladder.amount(600), 0.25);
        ladder.moved(602, 0.25);
        QVERIFY(ladder.worked(ScrollMethod::Uia));
        QCOMPARE(ladder.method(), ScrollMethod::Uia);
        QCOMPARE(qRound(ladder.amount(600) * 100), 25);
        for (int i = 0; i < 5; ++i)
            ladder.moved(600, 0.25);
        ladder.refused(ScrollAtEnd);
        QVERIFY(ladder.atEnd());
    }

    void ladderExplorer() // the first posted wheel only; UI Automation's first step shows nothing
    {
        ScrollLadder ladder;
        ladder.moved(95, 1);
        ladder.moved(0, 4);
        QCOMPARE(ladder.method(), ScrollMethod::SendWheel);
        ladder.refused(ScrollCursorOutside);
        QCOMPARE(ladder.method(), ScrollMethod::Uia);
        ladder.moved(0, 0.25);
        QCOMPARE(ladder.method(), ScrollMethod::PostWheel); // still alive: it moved once
        ladder.moved(0, 4);
        QVERIFY(!ladder.atEnd());
        QCOMPARE(ladder.method(), ScrollMethod::SendWheel); // asked again: the mouse may be there now
        ladder.refused(ScrollCursorOutside);
        QCOMPARE(ladder.method(), ScrollMethod::Uia);
        ladder.moved(505, 0.25);
        QCOMPARE(ladder.method(), ScrollMethod::Uia);
        ladder.moved(505, 0.25);
        QVERIFY(!ladder.atEnd());
    }

    void ladderNothingWorks()
    {
        ScrollLadder ladder;
        for (int i = 0; i < 8 && ladder.method() != ScrollMethod::Manual; ++i) {
            if (ladder.method() == ScrollMethod::Uia)
                ladder.refused(ScrollNoPattern);
            else
                ladder.moved(0, 1);
        }
        QCOMPARE(ladder.method(), ScrollMethod::Manual);
        QCOMPARE(ladder.hint(), ScrollLadder::Hint::NothingWorks);
        QVERIFY(!ladder.atEnd());
    }

    void ladderMouseOutside() // only SendInput's wheel is left: it waits for the mouse
    {
        ScrollLadder ladder;
        ladder.moved(0, 1);
        ladder.refused(ScrollCursorOutside);
        ladder.refused(ScrollNoPattern);
        ladder.moved(0, 1); // the posted wheel's second: gone
        QCOMPARE(ladder.method(), ScrollMethod::SendWheel);
        ladder.refused(ScrollCursorOutside);
        QCOMPARE(ladder.hint(), ScrollLadder::Hint::MouseOutside);
        QCOMPARE(ladder.method(), ScrollMethod::SendWheel);
        ladder.moved(300, 3);
        QCOMPARE(ladder.hint(), ScrollLadder::Hint::None);
        QVERIFY(ladder.worked(ScrollMethod::SendWheel));
        ladder.refused(ScrollKeysHeld); // Ctrl down: later
        QCOMPARE(ladder.method(), ScrollMethod::SendWheel);
    }

    void ladderElevated()
    {
        ScrollLadder known(true);
        QCOMPARE(known.method(), ScrollMethod::Manual);
        QCOMPARE(known.hint(), ScrollLadder::Hint::Elevated);
        ScrollLadder found;
        found.refused(ScrollAccessDenied);
        QCOMPARE(found.method(), ScrollMethod::Manual);
        QCOMPARE(found.hint(), ScrollLadder::Hint::Elevated);
    }

    void diagnosticsText()
    {
        ScrollDiagnostics d(5);
        ScrollDiagnostics::Facts facts;
        facts.windows = u"10.0.26200"_s;
        facts.version = u"0.12.0"_s;
        facts.program = u"msedge.exe"_s;
        facts.elevated = true;
        facts.region = QSize(1000, 1200);
        facts.scale = 1.5;
        facts.ignoreRight = 24;
        d.begin(facts);
        d.scrolled(10, {ScrollMethod::PostWheel, ScrollAccessDenied, 5}, 3);
        d.event(11, u"manual: runs as administrator"_s);
        ScrollStitcher::Step step;
        step.state = ScrollStitcher::State::Appended;
        step.dy = 602;
        step.score = 0.9871;
        step.votes = 1234;
        step.top = 56;
        step.bottom = 40;
        step.height = 1802;
        d.scrolled(20, {ScrollMethod::Uia, ScrollOk, 0}, 0.45);
        d.moved(150, ScrollMethod::Uia, 602);
        d.stitched(151, step, 7);
        step.state = ScrollStitcher::State::Lost;
        d.stitched(300, step, 6);
        d.scrolled(310, {ScrollMethod::SendWheel, ScrollCursorOutside, 0}, 3);
        // Copied while it runs: the picture so far.
        QVERIFY(d.text().contains(u"result so far: 1802 px high, fixed top 56 px, bottom 40 px"_s));
        d.ended(900, u"finished"_s, 1802, 56, 40, false);
        const QString text = d.text();
        QVERIFY2(text.contains(u"program: msedge.exe (runs as administrator)"_s), qPrintable(text));
        QVERIFY(text.contains(u"region: 1000 x 1200 px at 150 %, SDR; right 24 px"_s));
        QVERIFY(text.contains(u"post wheel access denied x1 (moved 0)"_s));
        QVERIFY(text.contains(u"ui automation ok x1 (moved 1)"_s));
        QVERIFY(text.contains(u"send wheel mouse outside x1 (moved 0)"_s));
        QVERIFY(text.contains(u"frames 2: appended 1, lost 1"_s));
        QVERIFY(text.contains(u"result: 1802 px high, fixed top 56 px, bottom 40 px"_s));
        QVERIFY(text.contains(u"ended: finished after 0.9 s"_s));
        // Each step: the method, the shift, the score, the state.
        QVERIFY(text.contains(u"frame 1: appended, dy 602, score 0.987, votes 1234, 1802 px high"_s));
        QVERIFY(text.contains(u"frame 2: lost"_s));
        // The last 5 of 8 events kept.
        QCOMPARE(d.events(), 8);
        QCOMPARE(d.lines().size(), 5);
        QVERIFY(text.contains(u"events (the last 5 of 8):"_s));
        QVERIFY(!text.contains(u"access denied (5)"_s)); // gone from the ring
        QVERIFY(d.lines().last().contains(u"end: finished, 1802 px high"_s));
        QVERIFY(text.contains(u"scroll: send wheel by 3.00: mouse outside"_s));
    }

    void samePixelsIgnoresExcludedStrip()
    {
        QImage a(200, 120, QImage::Format_RGB32);
        a.fill(kPaper);
        QImage b = a.copy();
        // Overlay scrollbar changes only in the right 20 px strip.
        for (int y = 10; y < 80; ++y)
            for (int x = 184; x < 196; ++x)
                b.setPixel(x, y, qRgb(110, 110, 110));
        QVERIFY(!samePixels(a, b));
        QVERIFY(samePixels(a, b, QRect(0, 0, 180, 120)));
        // Any 1 px change inside the compared area is still caught.
        b.setPixel(50, 50, qRgb(10, 20, 30));
        QVERIFY(!samePixels(a, b, QRect(0, 0, 180, 120)));
    }

    // Smooth scrolling at fractional DPI produces frames sampled at non-integer
    // vertical scroll positions. Anchor keyframes + subpixel vertical-envelope
    // matching must track continuously without losing lock or accumulating drift.
    void smoothScrollSubpixelFractional()
    {
        const Page& p = page();
        auto sampleFractional = [&](double pos) {
            const int y0 = int(std::floor(pos));
            const double frac = pos - y0;
            const QImage f0 = p.frame(y0);
            if (frac < 1e-4)
                return f0;
            const QImage f1 = p.frame(y0 + 1);
            QImage out(kWidth, kFrame, QImage::Format_RGB32);
            const int weight = int(std::lround(frac * 255.0));
            for (int y = 0; y < kFrame; ++y) {
                const auto* r0 = reinterpret_cast<const QRgb*>(f0.constScanLine(y));
                const auto* r1 = reinterpret_cast<const QRgb*>(f1.constScanLine(y));
                auto* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
                for (int x = 0; x < kWidth; ++x)
                    dst[x] = mix(r0[x], r1[x], weight);
            }
            return out;
        };
        ScrollStitcher stitcher(QSize(kWidth, kFrame), {}, {kIgnoreRight});
        double pos = 0.0;
        QCOMPARE(stitcher.add(sampleFractional(pos)).state, ScrollStitcher::State::Appended);
        for (int i = 0; i < 65; ++i) {
            pos += 17.37; // non-integer smooth scroll cadence across 1129 px
            const ScrollStitcher::Step step = stitcher.add(sampleFractional(pos));
            QVERIFY2(step.state != ScrollStitcher::State::Lost,
                qPrintable(u"lost on fractional frame %1 at pos %2"_s.arg(i).arg(pos)));
            QVERIFY(!step.predicted);
        }
        const int expectedHeight = int(std::lround(pos)) + kFrame;
        QVERIFY2(std::abs(stitcher.height() - expectedHeight) <= 3,
            qPrintable(u"height %1 vs expected %2"_s.arg(stitcher.height()).arg(expectedHeight)));
    }

    // Dark-mode apps with sparse short text in wide segments have low segment-wide
    // mean gradient, yet crisp local edges that must still stitch accurately.
    void darkModeSparseLines()
    {
        constexpr int w = 1280;
        constexpr int h = 520;
        const QRgb bg = qRgb(24, 24, 27);
        const QRgb fg = qRgb(92, 96, 108); // subtle dark-mode muted text (~68 luma contrast)
        QImage doc(w, 2200, QImage::Format_RGB32);
        fill(doc, 0, doc.height(), bg);
        std::mt19937 rng(77);
        for (int y = 18, row = 0; y < doc.height() - 24; y += 34, ++row) {
            const int x0 = 40 + (row % 3) * 16; // short indented bullet lines
            for (int c = 0; c < 6; ++c) {
                const auto& g = font().glyphs[rng() % 48];
                for (int gy = 0; gy < 12; ++gy) {
                    auto* line = reinterpret_cast<QRgb*>(doc.scanLine(y + gy));
                    for (int gx = 0; gx < 7; ++gx)
                        if (const int ink = g[size_t(gy * 7 + gx)])
                            line[x0 + c * 9 + gx] = mix(bg, fg, ink);
                }
            }
        }
        ScrollStitcher stitcher(QSize(w, h), {}, {24});
        QCOMPARE(stitcher.add(doc.copy(0, 0, w, h)).state, ScrollStitcher::State::Appended);
        int s = 0;
        for (int dy : {80, 95, 110, 88, 120, 105, 92, 130, 115, 90}) {
            s += dy;
            const auto step = stitcher.add(doc.copy(0, s, w, h));
            QCOMPARE(step.state, ScrollStitcher::State::Appended);
            QCOMPARE(step.dy, dy);
        }
        QCOMPARE(stitcher.result(), doc.copy(0, 0, w, s + h));
    }

    // A sticky bar that pins only after the page scrolls down (taller than h/6)
    // must be recognized once stable so it does not stamp duplicates.
    void lateAppearingStickyHeader()
    {
        QImage doc(kWidth, 2400, QImage::Format_RGB32);
        fill(doc, 0, doc.height(), kPaper);
        for (int y = 12, i = 0; y < doc.height() - 24; y += 22, ++i)
            text(doc, y, 24, kWidth - 48, uint32_t(31000 + i));
        constexpr int kLateBar = 96; // > kFrame / 6 (80)
        QImage lateBar(kWidth, kLateBar, QImage::Format_RGB32);
        fill(lateBar, 0, kLateBar, qRgb(36, 70, 124));
        text(lateBar, 38, 24, kWidth - 48, 4242);

        const auto frameAt = [&](int s, bool pinned) {
            QImage f = doc.copy(0, s, kWidth, kFrame);
            if (pinned) {
                for (int y = 0; y < kLateBar; ++y)
                    std::memcpy(f.scanLine(y), lateBar.constScanLine(y), size_t(kWidth) * 4);
            }
            return f;
        };
        ScrollStitcher stitcher(QSize(kWidth, kFrame), {}, {kIgnoreRight});
        QCOMPARE(stitcher.add(frameAt(0, false)).state, ScrollStitcher::State::Appended);
        int s = 0;
        for (int i = 0; i < 10; ++i) {
            s += 120;
            const auto step = stitcher.add(frameAt(s, i >= 1));
            QCOMPARE(step.state, ScrollStitcher::State::Appended);
            QCOMPARE(step.dy, 120);
        }
        QCOMPARE(stitcher.fixedTop(), kLateBar);
        QCOMPARE(stitcher.height(), s + kFrame);
        // Content below the initial top area must stay clean (no repeated dark bar bands).
        const QImage result = stitcher.result();
        for (int y = kLateBar + 20; y < result.height() - 20; ++y) {
            const QRgb px = result.pixel(8, y);
            QVERIFY2(qRed(px) > 200 && qGreen(px) > 200,
                qPrintable(u"late sticky bar leaked onto canvas row %1"_s.arg(y)));
        }
    }

    // Continuous closed-loop auto-scroll: smooth streaming ticks, multi-frame
    // ease-out calibration, backpressure, no cursor parking once PostWheel works,
    // and crisp end-of-page verification (< 1.2 s after last movement).
    void autoEngineContinuousStreaming()
    {
        ScrollAutoEngine::Config cfg;
        cfg.speed = ScrollSpeed::Normal;
        ScrollAutoEngine engine(cfg);
        constexpr int zone = 600;
        qint64 now = 0;
        engine.begin(now, 1);

        int sentTicks = 0;
        double targetPos = 0.0;
        double shownPos = 0.0;
        constexpr double maxScroll = 2400.0;
        qint64 lastMovedAt = 0;
        qint64 finishedAt = -1;
        bool everParkedCursor = false;

        for (int iter = 0; iter < 1500 && now < 15'000; ++iter) {
            const auto cmd = engine.next(now, zone, shownPos > 0);
            if (cmd.kind == ScrollAutoEngine::CommandKind::FinishAtEnd) {
                finishedAt = now;
                break;
            }
            if (cmd.kind == ScrollAutoEngine::CommandKind::SendScroll) {
                if (cmd.allowParkCursor)
                    everParkedCursor = true;
                if (cmd.method == ScrollMethod::PostWheel) {
                    ++sentTicks;
                    targetPos = std::min(maxScroll, targetPos + 120.0 * cmd.amount);
                    engine.onScrollSent(now, {ScrollMethod::PostWheel, ScrollOk, 0}, cmd.amount, zone);
                } else if (cmd.method == ScrollMethod::SendWheel) {
                    engine.onScrollSent(now, {ScrollMethod::SendWheel, ScrollCursorOutside, 0}, cmd.amount, zone);
                } else {
                    engine.onScrollSent(now, {ScrollMethod::Uia, ScrollNoPattern, 0}, cmd.amount, zone);
                }
            } else if (cmd.kind == ScrollAutoEngine::CommandKind::WaitFrame) {
                const int dt = std::min(cmd.timeoutMs, 16);
                now += dt;
                // Smooth-scroll easing: moves 35% of remaining distance each 16ms frame.
                const double remaining = targetPos - shownPos;
                if (remaining >= 1.0) {
                    const int dy = std::max(1, int(std::round(remaining * 0.35)));
                    shownPos = std::min(targetPos, shownPos + dy);
                    lastMovedAt = now;
                    ScrollStitcher::Step step;
                    step.state = ScrollStitcher::State::Appended;
                    step.dy = dy;
                    engine.onFrame(now, step, shownPos > 0, zone);
                }
            } else {
                QFAIL("unexpected manual fallback during smooth PostWheel stream");
            }
        }
        QVERIFY2(finishedAt > 0, "auto-scroll must detect page end");
        QCOMPARE(int(std::lround(shownPos)), int(maxScroll));
        QVERIFY2(sentTicks >= 18, qPrintable(u"expected smooth multi-tick stream, got %1"_s.arg(sentTicks)));
        QVERIFY2(!everParkedCursor, "cursor must not be parked when PostWheel works");
        QVERIFY2(finishedAt - lastMovedAt <= 1200,
            qPrintable(u"end-of-page verification took %1 ms (too slow)"_s.arg(finishedAt - lastMovedAt)));
    }

    // WinUI/InputSite targets start directly on SendWheel with one polite cursor
    // park, yield immediately if the user moves the mouse away, and resume once
    // the user places the mouse back in the region.
    void autoEngineWinUiPrefersSendWheelAndYields()
    {
        ScrollAutoEngine::Config cfg;
        cfg.prefersSendWheel = true;
        ScrollAutoEngine engine(cfg);
        constexpr int zone = 500;
        qint64 now = 0;
        engine.begin(now, 1);

        auto cmd = engine.next(now, zone, false);
        QCOMPARE(cmd.kind, ScrollAutoEngine::CommandKind::SendScroll);
        QCOMPARE(cmd.method, ScrollMethod::SendWheel);
        QVERIFY(cmd.allowParkCursor);
        engine.onScrollSent(now, {ScrollMethod::SendWheel, ScrollOk, 0}, cmd.amount, zone);

        now += 32;
        ScrollStitcher::Step moved;
        moved.state = ScrollStitcher::State::Appended;
        moved.dy = 110;
        engine.onFrame(now, moved, true, zone);
        QCOMPARE(engine.phase(), ScrollAutoEngine::Phase::Streaming);

        // User grabs the mouse and moves it outside the region.
        now += 20;
        engine.onUserMovedCursor(now, false);
        // Must NEVER ask to park the cursor again after the user took it.
        now += 200;
        cmd = engine.next(now, zone, true);
        if (cmd.kind == ScrollAutoEngine::CommandKind::SendScroll && cmd.method == ScrollMethod::SendWheel)
            QVERIFY(!cmd.allowParkCursor);

        // Fresh run where left mouse button was just clicked on the Auto-Scroll
        // toggle (ScrollKeysHeld): must wait briefly, then retry SendWheel with
        // allowParkCursor still enabled rather than falling into MouseOutside.
        ScrollAutoEngine afterClick(cfg);
        now = 0;
        afterClick.begin(now, 1);
        cmd = afterClick.next(now, zone, false);
        QCOMPARE(cmd.method, ScrollMethod::SendWheel);
        QVERIFY(cmd.allowParkCursor);
        afterClick.onScrollSent(now, {ScrollMethod::SendWheel, ScrollKeysHeld, 0}, cmd.amount, zone);
        QCOMPARE(afterClick.hint(), ScrollLadder::Hint::None);
        cmd = afterClick.next(now + 20, zone, false);
        QCOMPARE(cmd.kind, ScrollAutoEngine::CommandKind::WaitFrame);
        now += 150;
        cmd = afterClick.next(now, zone, false);
        QCOMPARE(cmd.kind, ScrollAutoEngine::CommandKind::SendScroll);
        QCOMPARE(cmd.method, ScrollMethod::SendWheel);
        QVERIFY(cmd.allowParkCursor);
    }

    // Speed modes change pacing cleanly; reverse direction supports upward
    // auto-scroll; user manually scrolling opposite yields gracefully.
    void autoEngineSpeedDirectionAndReverseYield()
    {
        auto measureTickInterval = [](ScrollSpeed speed) {
            ScrollAutoEngine::Config cfg;
            cfg.speed = speed;
            ScrollAutoEngine engine(cfg);
            qint64 now = 0;
            engine.begin(now, 1);
            auto cmd = engine.next(now, 800, false);
            engine.onScrollSent(now, {ScrollMethod::PostWheel, ScrollOk, 0}, cmd.amount, 800);
            now += 16;
            ScrollStitcher::Step step;
            step.state = ScrollStitcher::State::Appended;
            step.dy = 120;
            engine.onFrame(now, step, true, 800);
            // Advance through several steady-state ticks and measure average interval.
            qint64 firstTick = -1;
            qint64 lastTick = -1;
            int ticks = 0;
            for (int i = 0; i < 400 && ticks < 8; ++i) {
                cmd = engine.next(now, 800, true);
                if (cmd.kind == ScrollAutoEngine::CommandKind::SendScroll) {
                    if (firstTick < 0)
                        firstTick = now;
                    lastTick = now;
                    ++ticks;
                    engine.onScrollSent(now, {ScrollMethod::PostWheel, ScrollOk, 0}, cmd.amount, 800);
                    now += 16;
                    step.dy = int(std::lround(120.0 * cmd.amount));
                    engine.onFrame(now, step, true, 800);
                } else {
                    now += cmd.timeoutMs;
                }
            }
            return ticks > 1 ? double(lastTick - firstTick) / (ticks - 1) : 0.0;
        };
        const double slowMs = measureTickInterval(ScrollSpeed::Slow);
        const double normalMs = measureTickInterval(ScrollSpeed::Normal);
        const double fastMs = measureTickInterval(ScrollSpeed::Fast);
        QVERIFY2(slowMs > normalMs && normalMs > fastMs,
            qPrintable(u"expected slow > normal > fast intervals, got %1 / %2 / %3"_s.arg(slowMs).arg(normalMs).arg(fastMs)));

        // Upward auto-scroll (direction = -1) sends direction = -1 ticks and yields if user scrolls down.
        ScrollAutoEngine up;
        qint64 now = 0;
        up.begin(now, -1);
        auto cmd = up.next(now, 600, false);
        QCOMPARE(cmd.kind, ScrollAutoEngine::CommandKind::SendScroll);
        QCOMPARE(cmd.direction, -1);
        up.onScrollSent(now, {ScrollMethod::PostWheel, ScrollOk, 0}, cmd.amount, 600);
        now += 20;
        ScrollStitcher::Step step;
        step.state = ScrollStitcher::State::Appended;
        step.dy = -90;
        up.onFrame(now, step, true, 600);
        QCOMPARE(up.phase(), ScrollAutoEngine::Phase::Streaming);
        // Two consecutive user reverse scrolls (+60 px) gracefully yield auto-scroll.
        step.dy = 60;
        up.onFrame(now + 30, step, true, 600);
        QCOMPARE(up.phase(), ScrollAutoEngine::Phase::Streaming);
        up.onFrame(now + 60, step, true, 600);
        QCOMPARE(up.phase(), ScrollAutoEngine::Phase::Idle);
    }

    // Transient Lost frame pauses, tries a gentle reverse reconnect tick, and
    // resumes smoothly instead of immediately abandoning auto-scroll.
    void autoEngineLostRecovery()
    {
        ScrollAutoEngine engine;
        qint64 now = 0;
        engine.begin(now, 1);
        auto cmd = engine.next(now, 600, false);
        engine.onScrollSent(now, {ScrollMethod::PostWheel, ScrollOk, 0}, cmd.amount, 600);
        now += 20;
        ScrollStitcher::Step ok;
        ok.state = ScrollStitcher::State::Appended;
        ok.dy = 100;
        engine.onFrame(now, ok, true, 600);
        QCOMPARE(engine.phase(), ScrollAutoEngine::Phase::Streaming);

        // One Lost frame enters Recovering (does NOT abort immediately).
        ScrollStitcher::Step lost;
        lost.state = ScrollStitcher::State::Lost;
        engine.onFrame(now + 30, lost, true, 600);
        QCOMPARE(engine.phase(), ScrollAutoEngine::Phase::Recovering);

        // Wait past the settle pause: engine emits one gentle reverse tick (-1).
        now += 400;
        cmd = engine.next(now, 600, true);
        QCOMPARE(cmd.kind, ScrollAutoEngine::CommandKind::SendScroll);
        QCOMPARE(cmd.direction, -1);
        engine.onScrollSent(now, {ScrollMethod::PostWheel, ScrollOk, 0}, cmd.amount, 600);

        // Matching frame arrives -> resumes Streaming cleanly!
        ScrollStitcher::Step reconnected;
        reconnected.state = ScrollStitcher::State::Behind;
        reconnected.dy = -80;
        engine.onFrame(now + 40, reconnected, true, 600);
        QCOMPARE(engine.phase(), ScrollAutoEngine::Phase::Streaming);

        // If recovery fails completely, it yields with lost = true.
        engine.onFrame(now + 80, lost, true, 600);
        now += 450;
        cmd = engine.next(now, 600, true);
        QCOMPARE(cmd.kind, ScrollAutoEngine::CommandKind::SendScroll);
        engine.onScrollSent(now, {ScrollMethod::PostWheel, ScrollOk, 0}, cmd.amount, 600);
        now += 400;
        cmd = engine.next(now, 600, true);
        QCOMPARE(cmd.kind, ScrollAutoEngine::CommandKind::YieldToManual);
        QVERIFY(cmd.lost);
    }

    // Explorer-like target (PostWheel moves once then stops) escalates smoothly
    // to SendWheel without ending early; vertical dead-end switches sideways.
    void autoEngineExplorerAndSideways()
    {
        ScrollAutoEngine explorer;
        qint64 now = 0;
        explorer.begin(now, 1);
        auto cmd = explorer.next(now, 600, false);
        QCOMPARE(cmd.method, ScrollMethod::PostWheel);
        explorer.onScrollSent(now, {ScrollMethod::PostWheel, ScrollOk, 0}, cmd.amount, 600);
        now += 20;
        ScrollStitcher::Step step;
        step.state = ScrollStitcher::State::Appended;
        step.dy = 95;
        explorer.onFrame(now, step, true, 600);

        // PostWheel stops responding; SendWheel takes over and rescues it!
        bool sendWheelRescued = false;
        for (int i = 0; i < 120 && !sendWheelRescued; ++i) {
            cmd = explorer.next(now, 600, true);
            if (cmd.kind == ScrollAutoEngine::CommandKind::SendScroll) {
                explorer.onScrollSent(now, {cmd.method, ScrollOk, 0}, cmd.amount, 600);
                if (cmd.method == ScrollMethod::SendWheel) {
                    QVERIFY(cmd.allowParkCursor); // PostWheel only ran 1 tick, so rescue may park
                    now += 20;
                    step.dy = 120;
                    explorer.onFrame(now, step, true, 600);
                    sendWheelRescued = explorer.phase() == ScrollAutoEngine::Phase::Streaming;
                }
            } else if (cmd.kind == ScrollAutoEngine::CommandKind::WaitFrame) {
                now += cmd.timeoutMs;
            } else {
                break;
            }
        }
        QVERIFY2(sendWheelRescued, "SendWheel must rescue when PostWheel stops after one tick");

        // Nothing moves vertically before any growth -> SwitchSideways once.
        ScrollAutoEngine sideways;
        now = 0;
        sideways.begin(now, 1);
        bool switched = false;
        for (int i = 0; i < 80; ++i) {
            cmd = sideways.next(now, 600, false);
            if (cmd.kind == ScrollAutoEngine::CommandKind::SwitchSideways) {
                switched = true;
                break;
            }
            if (cmd.kind == ScrollAutoEngine::CommandKind::SendScroll) {
                const int code = cmd.method == ScrollMethod::Uia ? ScrollNoPattern : ScrollOk;
                sideways.onScrollSent(now, {cmd.method, code, 0}, cmd.amount, 600);
            } else if (cmd.kind == ScrollAutoEngine::CommandKind::WaitFrame) {
                now += cmd.timeoutMs;
            }
        }
        QVERIFY2(switched, "vertical dead-end with no growth must try sideways once");
    }

    // Large animated GIF/sticker in a chat window (reproduces real-world QQ
    // logs): must stay Unchanged while stationary (no Lost, no +/-1 subpixel
    // oscillation, no false sticky footer) and keep stitching while scrolling.
    void animatedGifInChatStaysUnchangedAndStitches()
    {
        constexpr int w = 600;
        constexpr int h = 560;
        QImage chat(w, 2600, QImage::Format_RGB32);
        fill(chat, 0, chat.height(), qRgb(245, 245, 247));
        for (int y = 18, i = 0; y < chat.height() - 28; y += 34, ++i) {
            const bool rightMsg = (i % 3 == 1);
            const int x0 = rightMsg ? 320 : 28;
            const int x1 = rightMsg ? w - 40 : 280;
            text(chat, y, x0, x1, uint32_t(41000 + i));
        }

        const QRect gif1(240, 140, 280, 240); // dominates texture on initial viewport
        const QRect gif2(200, 1280, 290, 250); // dominates texture after scrolling down
        const auto frameAt = [&](int scroll, uint32_t animTick) {
            QImage f = chat.copy(0, scroll, w, h);
            for (const QRect& g : {gif1, gif2}) {
                const QRect vis = g.translated(0, -scroll).intersected(QRect(0, 0, w, h));
                if (!vis.isEmpty())
                    picture(f, vis, 70000 + animTick * 13);
            }
            return f;
        };

        ScrollStitcher stitcher(QSize(w, h), {}, {24});
        QCOMPARE(stitcher.add(frameAt(0, 0)).state, ScrollStitcher::State::Appended);

        // Phase 1: Stationary before any scroll while GIF1 animates every frame.
        for (uint32_t t = 1; t <= 12; ++t) {
            const auto step = stitcher.add(frameAt(0, t));
            QVERIFY2(step.state == ScrollStitcher::State::Unchanged,
                qPrintable(u"phase 1 tick %1 expected Unchanged, got %2 dy %3"_s
                               .arg(t).arg(stateName(step.state)).arg(step.dy)));
        }
        QCOMPARE(stitcher.height(), h);

        // Phase 2: Scroll smoothly down past GIF1 and into GIF2 while animating.
        int pos = 0;
        uint32_t tick = 20;
        while (pos < 1200) {
            const int dy = 60;
            pos += dy;
            const auto step = stitcher.add(frameAt(pos, ++tick));
            QVERIFY2(step.state == ScrollStitcher::State::Appended && step.dy == dy,
                qPrintable(u"phase 2 pos %1 got %2 dy %3"_s
                               .arg(pos).arg(stateName(step.state)).arg(step.dy)));
        }
        QCOMPARE(stitcher.fixedTop(), 0);
        QCOMPARE(stitcher.fixedBottom(), 0);

        // Phase 3: Stationary at pos=1200 while GIF2 animates at 60fps.
        const int heightBefore = stitcher.height();
        for (uint32_t t = 0; t < 18; ++t) {
            const auto step = stitcher.add(frameAt(pos, ++tick));
            QVERIFY2(step.state == ScrollStitcher::State::Unchanged,
                qPrintable(u"phase 3 tick %1 expected Unchanged, got %2 dy %3"_s
                               .arg(t).arg(stateName(step.state)).arg(step.dy)));
        }
        QCOMPARE(stitcher.height(), heightBefore);
        QCOMPARE(stitcher.fixedTop(), 0);
        QCOMPARE(stitcher.fixedBottom(), 0);
    }

    // Fast manual fling back into already-captured content must relocalize
    // directly against the stitched canvas instead of getting stuck in Lost.
    void wholeCanvasRelocalizeAfterFastFling()
    {
        const Page& p = page();
        ScrollStitcher stitcher(QSize(kWidth, kFrame), {}, {kIgnoreRight});
        stitcher.add(p.frame(0));
        int pos = 0;
        for (int s : {160, 320, 480, 640, 800, 960, 1120, 1280, 1440, 1600, 1760, 1920, 2080}) {
            pos = s;
            QCOMPARE(stitcher.add(p.frame(pos)).state, ScrollStitcher::State::Appended);
        }
        const int heightBefore = stitcher.height();
        QCOMPARE(heightBefore, 2080 + kFrame);

        // Jump 1180 px back (zero overlap with m_prev at 2080!) inside canvas.
        auto step = stitcher.add(p.frame(900));
        QCOMPARE(step.state, ScrollStitcher::State::Behind);
        QCOMPARE(stitcher.height(), heightBefore);

        // Continue scrolling smoothly from the relocalized position.
        step = stitcher.add(p.frame(740));
        QCOMPARE(step.state, ScrollStitcher::State::Behind);
        QCOMPARE(step.dy, -160);

        // Jump straight to past m_far (2080 + 160 = 2240): m_farAnalysis reconnects and appends!
        step = stitcher.add(p.frame(2240));
        QCOMPARE(step.state, ScrollStitcher::State::Appended);
        QCOMPARE(stitcher.height(), 2240 + kFrame);

        // True jump outside captured canvas still reports Lost, then scrolling
        // back into ANY captured part of the canvas reconnects immediately.
        QImage outside(kWidth, kFrame, QImage::Format_RGB32);
        fill(outside, 0, kFrame, qRgb(245, 240, 235));
        for (int y = 60, i = 0; y < kFrame - 50; y += 22, ++i)
            text(outside, y, 40, kWidth - 40, uint32_t(88000 + i));
        QCOMPARE(stitcher.add(outside).state, ScrollStitcher::State::Lost);
        step = stitcher.add(p.frame(1300));
        QCOMPARE(step.state, ScrollStitcher::State::Behind);
        QCOMPARE(stitcher.height(), 2240 + kFrame);
    }

    // Smooth fractional wheel avoids cold-start surge on slow-ramping smooth
    // scrollers and automatically falls back to integer notches on legacy apps.
    void smoothFractionalWheelAndLegacyFallback()
    {
        // 1. Slow-ramping smooth scroller (first frame moves only 2 px for 190 px notch).
        ScrollAutoEngine smooth;
        constexpr int zone = 600;
        qint64 now = 0;
        smooth.begin(now, 1);
        auto cmd = smooth.next(now, zone, false);
        QCOMPARE(cmd.kind, ScrollAutoEngine::CommandKind::SendScroll);
        QCOMPARE(cmd.amount, 1.0);
        smooth.onScrollSent(now, {ScrollMethod::PostWheel, ScrollOk, 0}, cmd.amount, zone);
        now += 16;
        ScrollStitcher::Step firstStep;
        firstStep.state = ScrollStitcher::State::Appended;
        firstStep.dy = 2;
        smooth.onFrame(now, firstStep, true, zone);
        QCOMPARE(smooth.phase(), ScrollAutoEngine::Phase::Streaming);
        // Must wait for the smooth cadence instead of immediately spamming another tick.
        cmd = smooth.next(now + 20, zone, true);
        QCOMPARE(cmd.kind, ScrollAutoEngine::CommandKind::WaitFrame);

        // 2. Legacy Win32 app that truncates fractional deltas (< 1.0) to 0 px.
        ScrollAutoEngine legacy;
        now = 0;
        legacy.begin(now, 1);
        cmd = legacy.next(now, zone, false);
        legacy.onScrollSent(now, {ScrollMethod::PostWheel, ScrollOk, 0}, cmd.amount, zone);
        now += 16;
        ScrollStitcher::Step step;
        step.state = ScrollStitcher::State::Appended;
        step.dy = 190;
        legacy.onFrame(now, step, true, zone);
        bool fellBackToInteger = false;
        for (int i = 0; i < 80; ++i) {
            cmd = legacy.next(now, zone, true);
            if (cmd.kind == ScrollAutoEngine::CommandKind::SendScroll) {
                legacy.onScrollSent(now, {ScrollMethod::PostWheel, ScrollOk, 0}, cmd.amount, zone);
                if (cmd.amount >= 0.99) {
                    fellBackToInteger = true;
                    now += 16;
                    step.dy = 190;
                    legacy.onFrame(now, step, true, zone);
                    break;
                }
            } else if (cmd.kind == ScrollAutoEngine::CommandKind::WaitFrame) {
                now += cmd.timeoutMs;
            } else {
                break;
            }
        }
        QVERIFY2(fellBackToInteger, "engine must fall back to integer wheel notches when fractional deltas do not scroll");
        QCOMPARE(legacy.phase(), ScrollAutoEngine::Phase::Streaming);
    }
};

QTEST_APPLESS_MAIN(TestStitch)
#include "tst_stitch.moc"
