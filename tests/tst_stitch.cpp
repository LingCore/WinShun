// The long screenshot's stitching (src/capture/ScrollStitcher) on frames cut
// from a made-up long page (docs/design-capture-roadmap.md, 2.4 item 1): a
// sticky header and footer, a fixed "back to top" button, pictures that fade
// in, zebra rows that repeat themselves, scrolling back, a bounce at the end.
// The result must be the page within 1 a channel (but where the button
// stays), and each step's shift exactly what was scrolled. Then text drawn
// again a little differently (WinUI), lines closer than a step, a side bar
// that stays, frames that do not overlap, the limits; and the diagnostics'
// text (ScrollDiagnostics).
#include "ScrollDiagnostics.h"
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

void replay(Replay& out, const Page& p, const std::vector<int>& positions, std::optional<int> expectError = std::nullopt,
    const Distort& distort = {})
{
    ScrollStitcher stitcher(QSize(kWidth, kFrame), {}, {kIgnoreRight});
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
    out.result = stitcher.result();
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
};

QTEST_APPLESS_MAIN(TestStitch)
#include "tst_stitch.moc"
