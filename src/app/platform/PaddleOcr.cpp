#include "PaddleOcr.h"
#include "OcrFiles.h"
#include "OcrTable.h"

// The API comes from the DLL loaded here (Ort::InitApi), not from linking it.
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QPointF>
#include <QTransform>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

// What PaddleOCR's own pipeline does (RapidOCR 3.10, its defaults for
// PP-OCRv6), except: the picture is not made larger to find text (a
// screenshot's text is at its size already), a tall picture is looked at in
// strips rather than shrunk, a long thin one in tiles across, lines are cut
// from the picture as it is, and a very long line is read in parts. Memory
// stays that of a 2000 x 512 strip whatever the picture's shape.
constexpr int kMaxWidth = 2000; // wider: made smaller to find the text in it
constexpr int kMinSide = 30; // narrower: made larger
// The height looked at in one go. Finding the lines takes ~80 bytes a pixel
// looked at, for as long as it lasts: a whole 1920 x 1080 screenshot at once
// was 184 MB on top of the program's own, each time a picture was copied
// (GitHub #467); in strips of 512 rows it is a third of that, for a tenth of
// a second more (1440 x 1440: +168 MB in 250 ms at once, +59 MB in 360 ms).
constexpr int kStrip = 512;
constexpr int kOverlap = 160; // … overlapping, so a line is whole in one of them
// Text higher than the overlap is not whole in any strip (a poster's title):
// a tile with such a line cut by a strip's edge is looked at again in strips
// this high, as every picture was before.
constexpr int kTallStrip = 2000;
constexpr double kCutEdge = 3; // a box within this of a strip's cut edge was cut by it
constexpr float kPixelThreshold = 0.3f; // a pixel is text
constexpr float kBoxThreshold = 0.5f; // a box is text: its pixels' mean
constexpr double kUnclip = 1.6; // how far a box grows around its text
constexpr int kMaxBoxes = 1000;
constexpr int kLineHeight = 48; // what the reading model takes
constexpr double kMinRatio = 320.0 / 48.0;
constexpr int kBatch = 6;
// What one reading of the model takes goes with its columns (~4.5 KB each):
// a line longer than this is read in parts (250 times as long as high)…
constexpr int kLineColumns = 12000;
// … and lines are read together, padded to the widest, up to this: six
// 6000-column lines of a wide table still go at once.
constexpr double kBatchColumns = 36000;
constexpr float kTextThreshold = 0.5f; // a line read with less confidence is dropped
// A reading that may be upside down (OcrOptions::retryUpsideDown): fewer than
// this share of its lines kept, or their mean score below this. Measured on
// stored readings: upright pages 0.89 and up, pages turned half round 0.57.
constexpr double kPoorlyKept = 0.6;
constexpr double kPoorScore = 0.75;
constexpr double kClearlyMore = 1.3; // what the turned reading must say, times the first

using Quad = std::array<QPointF, 4>; // top left, top right, bottom right, bottom left

const wchar_t* const kDetModel = L"PP-OCRv6_det_tiny.onnx";
const wchar_t* const kRecModel = L"PP-OCRv6_rec_tiny.onnx";

// The table model (SLANet_plus): the picture's longer side made this long,
// the rest of the square left empty; its cells' boxes are in that square.
constexpr int kTableSide = 488;
// Darker than this on average (0…255): a dark table is shown to the model as
// a light one, which is what it learned from.
constexpr int kTableDark = 110;
// The model is this sure of a table's rows and columns (0.994 and up on 34
// table screenshots); of the cells it makes of a paragraph, code or a chat
// 0.92 to 0.95. Below: the table is worked out without it.
constexpr double kTableSure = 0.98;

std::mutex g_tableModelMutex;
QString g_tableModel; // PaddleOcr::setTableModel

QString filePath(const QString& folder, const wchar_t* name, bool model)
{
    return QDir::toNativeSeparators(folder + (model ? u"/ocr/"_s : u"/"_s) + QString::fromWCharArray(name));
}

// ONNX Runtime from the resolved component/legacy folder, once.
std::atomic<PaddleOcr::Trace> g_trace {nullptr};

// A step for the OCR test (PaddleOcr::setTrace), made only when it listens.
template <typename Make>
void trace(Make&& make)
{
    if (const PaddleOcr::Trace to = g_trace.load(std::memory_order_relaxed))
        to(make());
}

const OrtApi* loadApi(const QString& folder, QString* error)
{
    static std::mutex mutex;
    static const OrtApi* api = nullptr;
    static QString failure;
    const std::lock_guard lock(mutex);
    if (api)
        return api;
    if (failure.isEmpty()) {
        const QString dll = filePath(folder, L"onnxruntime.dll", false);
        HMODULE module = ::LoadLibraryExW(reinterpret_cast<const wchar_t*>(dll.utf16()), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
        const auto getBase = module ? reinterpret_cast<GetApiBase>(::GetProcAddress(module, "OrtGetApiBase")) : nullptr;
        const OrtApiBase* base = getBase ? getBase() : nullptr;
        api = base ? base->GetApi(ORT_API_VERSION) : nullptr;
        if (api)
            Ort::InitApi(api);
        else
            failure = module ? u"onnxruntime.dll is not version %1 or later"_s.arg(ORT_API_VERSION)
                             : u"onnxruntime.dll did not load (error %1)"_s.arg(::GetLastError());
    }
    if (!api && error)
        *error = failure;
    return api;
}

// ONNX Runtime's threads, below normal priority as the recognizer's own.
struct ThreadStart {
    OrtThreadWorkerFn work;
    void* param;
};

DWORD WINAPI threadMain(void* p)
{
    const ThreadStart start = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    start.work(start.param);
    return 0;
}

OrtCustomThreadHandle createThread(void*, OrtThreadWorkerFn work, void* param)
{
    auto* start = new ThreadStart {work, param};
    HANDLE thread = ::CreateThread(nullptr, 0, threadMain, start, CREATE_SUSPENDED, nullptr);
    if (!thread) {
        delete start;
        return nullptr;
    }
    ::SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
    ::ResumeThread(thread);
    return reinterpret_cast<OrtCustomThreadHandle>(thread);
}

void joinThread(OrtCustomThreadHandle handle)
{
    HANDLE thread = const_cast<HANDLE>(reinterpret_cast<const void*>(handle));
    ::WaitForSingleObject(thread, INFINITE);
    ::CloseHandle(thread);
}

// How every model is run: on the readers' shared threads.
Ort::SessionOptions sessionOptions()
{
    Ort::SessionOptions options;
    options.DisablePerSessionThreads()
        .SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL)
        .DisableCpuMemArena() // memory back after each picture
        // A planned block per input size is ~135 MB more than the
        // detection's own peak when a tall picture's strips repeat a size
        // (measured on 2000x4000..3200x12500), and no faster.
        .DisableMemPattern();
    return options;
}

double distance(QPointF a, QPointF b)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

double cross(QPointF o, QPointF a, QPointF b)
{
    return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
}

// The pixels as the models take them: blue, green, red planes, -1…1.
void toPlanes(const QImage& rgb, float* out, int width)
{
    const int w = rgb.width();
    const int h = rgb.height();
    const qsizetype plane = static_cast<qsizetype>(h) * width;
    for (int y = 0; y < h; ++y) {
        const uchar* line = rgb.constScanLine(y);
        float* b = out + static_cast<qsizetype>(y) * width;
        for (int x = 0; x < w; ++x) {
            b[x] = line[3 * x + 2] / 127.5f - 1.f;
            b[plane + x] = line[3 * x + 1] / 127.5f - 1.f;
            b[2 * plane + x] = line[3 * x] / 127.5f - 1.f;
        }
    }
}

QImage scaledRgb(const QImage& image, int w, int h)
{
    if (image.width() == w && image.height() == h)
        return image.format() == QImage::Format_RGB888 ? image : image.convertToFormat(QImage::Format_RGB888);
    return image.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB888);
}

int round32(double v)
{
    return std::max(32, static_cast<int>(std::lround(v / 32)) * 32);
}

// The smallest rectangle around the points (rotating calipers over their
// hull), as an origin, its two directions and how far it goes along each.
struct Rect {
    QPointF u, v; // unit directions
    double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
    double width() const { return u1 - u0; }
    double height() const { return v1 - v0; }
    Quad corners() const
    {
        const auto at = [this](double a, double b) { return u * a + v * b; };
        return {at(u0, v0), at(u1, v0), at(u1, v1), at(u0, v1)};
    }
};

bool minAreaRect(std::vector<QPointF>& points, Rect* rect)
{
    std::ranges::sort(points, [](QPointF a, QPointF b) { return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y()); });
    std::vector<QPointF> hull(2 * points.size());
    std::size_t k = 0;
    for (const QPointF& p : points) {
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], p) <= 0)
            --k;
        hull[k++] = p;
    }
    for (std::size_t i = points.size() - 1, low = k + 1; i-- > 0;) {
        while (k >= low && cross(hull[k - 2], hull[k - 1], points[i]) <= 0)
            --k;
        hull[k++] = points[i];
    }
    hull.resize(k > 1 ? k - 1 : k);
    if (hull.size() < 3)
        return false;
    double best = -1;
    for (std::size_t i = 0; i < hull.size(); ++i) {
        const QPointF edge = hull[(i + 1) % hull.size()] - hull[i];
        const double length = std::hypot(edge.x(), edge.y());
        if (length == 0)
            continue;
        Rect r;
        r.u = edge / length;
        r.v = QPointF(-r.u.y(), r.u.x());
        r.u0 = r.v0 = std::numeric_limits<double>::max();
        r.u1 = r.v1 = std::numeric_limits<double>::lowest();
        for (const QPointF& p : hull) {
            const double a = QPointF::dotProduct(p, r.u);
            const double b = QPointF::dotProduct(p, r.v);
            r.u0 = std::min(r.u0, a), r.u1 = std::max(r.u1, a);
            r.v0 = std::min(r.v0, b), r.v1 = std::max(r.v1, b);
        }
        const double area = r.width() * r.height();
        if (best < 0 || area < best) {
            best = area;
            *rect = r;
        }
    }
    return best >= 0;
}

// The mean of the map inside the quad.
float meanInside(const float* map, int w, int h, const Quad& quad)
{
    double left = quad[0].x(), right = left, top = quad[0].y(), bottom = top;
    for (const QPointF& p : quad) {
        left = std::min(left, p.x()), right = std::max(right, p.x());
        top = std::min(top, p.y()), bottom = std::max(bottom, p.y());
    }
    const int x0 = std::clamp(static_cast<int>(std::floor(left)), 0, w - 1);
    const int x1 = std::clamp(static_cast<int>(std::ceil(right)), 0, w - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(top)), 0, h - 1);
    const int y1 = std::clamp(static_cast<int>(std::ceil(bottom)), 0, h - 1);
    // Inside every edge, the edges themselves counted (as OpenCV's fillPoly).
    std::array<double, 4> lengths;
    for (int i = 0; i < 4; ++i)
        lengths[i] = std::max(1e-9, distance(quad[i], quad[(i + 1) % 4]));
    const double sign = cross(quad[0], quad[1], quad[2]) >= 0 ? 1 : -1;
    double sum = 0;
    int count = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            bool in = true;
            for (int i = 0; i < 4 && in; ++i)
                in = sign * cross(quad[i], quad[(i + 1) % 4], QPointF(x, y)) >= -0.5 * lengths[i];
            if (in) {
                sum += map[static_cast<qsizetype>(y) * w + x];
                ++count;
            }
        }
    }
    return count ? static_cast<float>(sum / count) : 0.f;
}

// The boxes of text in the detection model's map (w × h), in the
// coordinates of a picture destW × destH (DBNet's post-processing).
std::vector<Quad> boxesFrom(const float* map, int w, int h, int destW, int destH)
{
    // Text pixels, grown by one to the right and down (a 2 × 2 dilation).
    std::vector<uchar> text(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto on = [&](int xx, int yy) { return xx >= 0 && yy >= 0 && map[static_cast<qsizetype>(yy) * w + xx] > kPixelThreshold; };
            text[static_cast<std::size_t>(y) * w + x] = on(x, y) || on(x - 1, y) || on(x, y - 1) || on(x - 1, y - 1);
        }
    }
    std::vector<Quad> boxes;
    std::vector<int> stack;
    std::vector<int> pixels;
    std::vector<QPointF> edgePoints;
    std::vector<int> left, right;
    for (int start = 0; start < w * h && static_cast<int>(boxes.size()) < kMaxBoxes; ++start) {
        if (text[start] != 1)
            continue;
        // One piece of text: its pixels, eight ways connected.
        pixels.clear();
        stack.assign(1, start);
        text[start] = 2;
        while (!stack.empty()) {
            const int at = stack.back();
            stack.pop_back();
            pixels.push_back(at);
            const int px = at % w, py = at / w;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = px + dx, ny = py + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                        continue;
                    const int n = ny * w + nx;
                    if (text[n] == 1) {
                        text[n] = 2;
                        stack.push_back(n);
                    }
                }
            }
        }
        // Its outline: the ends of each row (enough for the hull).
        int minY = h, maxY = -1;
        for (const int p : pixels)
            minY = std::min(minY, p / w), maxY = std::max(maxY, p / w);
        left.assign(maxY - minY + 1, w);
        right.assign(maxY - minY + 1, -1);
        for (const int p : pixels) {
            const int row = p / w - minY;
            left[row] = std::min(left[row], p % w);
            right[row] = std::max(right[row], p % w);
        }
        edgePoints.clear();
        for (int row = 0; row <= maxY - minY; ++row) {
            edgePoints.emplace_back(left[row], minY + row);
            if (right[row] != left[row])
                edgePoints.emplace_back(right[row], minY + row);
        }
        Rect rect;
        if (!minAreaRect(edgePoints, &rect) || std::min(rect.width(), rect.height()) < 3)
            continue;
        if (meanInside(map, w, h, rect.corners()) < kBoxThreshold)
            continue;
        // Grown around the text: a rectangle offset by area × ratio / perimeter.
        const double grow = rect.width() * rect.height() * kUnclip / (2 * (rect.width() + rect.height()));
        rect.u0 -= grow, rect.u1 += grow, rect.v0 -= grow, rect.v1 += grow;
        if (std::min(rect.width(), rect.height()) < 5)
            continue;
        Quad quad = rect.corners();
        for (QPointF& p : quad) {
            p.setX(std::clamp(std::round(p.x() / w * destW), 0.0, static_cast<double>(destW)));
            p.setY(std::clamp(std::round(p.y() / h * destH), 0.0, static_cast<double>(destH)));
        }
        // In order: the left two by height, then the right two.
        std::ranges::sort(quad, {}, &QPointF::x);
        if (quad[0].y() > quad[1].y())
            std::swap(quad[0], quad[1]);
        if (quad[2].y() > quad[3].y())
            std::swap(quad[2], quad[3]);
        quad = {quad[0], quad[2], quad[3], quad[1]};
        for (QPointF& p : quad) {
            p.setX(std::trunc(std::clamp(p.x(), 0.0, destW - 1.0)));
            p.setY(std::trunc(std::clamp(p.y(), 0.0, destH - 1.0)));
        }
        if (static_cast<int>(distance(quad[0], quad[1])) <= 3 || static_cast<int>(distance(quad[0], quad[3])) <= 3)
            continue;
        boxes.push_back(quad);
    }
    return boxes;
}

// The line in the quad, straightened out (bilinear, the edges repeated).
QImage cut(const QImage& rgb, const Quad& q)
{
    const int w = static_cast<int>(std::max(distance(q[0], q[1]), distance(q[2], q[3])));
    const int h = static_cast<int>(std::max(distance(q[0], q[3]), distance(q[1], q[2])));
    if (w < 1 || h < 1)
        return {};
    QImage out(w, h, QImage::Format_RGB888);
    const int sw = rgb.width(), sh = rgb.height();
    for (int y = 0; y < h; ++y) {
        uchar* line = out.scanLine(y);
        const double t = static_cast<double>(y) / h;
        const QPointF a = q[0] + (q[3] - q[0]) * t;
        const QPointF b = q[1] + (q[2] - q[1]) * t;
        for (int x = 0; x < w; ++x) {
            const QPointF p = a + (b - a) * (static_cast<double>(x) / w);
            const double fx = std::clamp(p.x(), 0.0, sw - 1.0), fy = std::clamp(p.y(), 0.0, sh - 1.0);
            const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
            const int x1 = std::min(x0 + 1, sw - 1), y1 = std::min(y0 + 1, sh - 1);
            const double ax = fx - x0, ay = fy - y0;
            const uchar* r0 = rgb.constScanLine(y0);
            const uchar* r1 = rgb.constScanLine(y1);
            for (int c = 0; c < 3; ++c) {
                const double top = r0[3 * x0 + c] * (1 - ax) + r0[3 * x1 + c] * ax;
                const double bottom = r1[3 * x0 + c] * (1 - ax) + r1[3 * x1 + c] * ax;
                line[3 * x + c] = static_cast<uchar>(std::lround(top * (1 - ay) + bottom * ay));
            }
        }
    }
    if (h >= 1.5 * w) // standing up: turned to read left to right
        out = out.transformed(QTransform().rotate(-90));
    return out;
}

// A long strip's boxes, looked at in tiles across, put back together: a line
// over a tile's edge was found in both, cut at the edges, and what stands
// where they overlap twice. Boxes of different tiles on one line (overlapping
// by half the height) that overlap side to side become one box around them.
// From one tile: as they are.
std::vector<Quad> joinedAcrossTiles(std::vector<std::pair<int, Quad>> found)
{
    struct Box {
        int tile;
        double left, top, right, bottom;
    };
    if (std::ranges::all_of(found, [](const auto& f) { return f.first == 0; })) {
        std::vector<Quad> quads;
        quads.reserve(found.size());
        for (const auto& [tile, q] : found)
            quads.push_back(q);
        return quads;
    }
    std::vector<Box> boxes;
    boxes.reserve(found.size());
    for (const auto& [tile, q] : found) {
        Box b {tile, q[0].x(), q[0].y(), q[0].x(), q[0].y()};
        for (const QPointF& p : q) {
            b.left = std::min(b.left, p.x());
            b.right = std::max(b.right, p.x());
            b.top = std::min(b.top, p.y());
            b.bottom = std::max(b.bottom, p.y());
        }
        boxes.push_back(b);
    }
    std::vector<std::size_t> order(boxes.size());
    std::iota(order.begin(), order.end(), 0);
    std::ranges::sort(order, {}, [&](std::size_t i) { return boxes[i].left; });
    std::vector<std::size_t> group(boxes.size());
    std::iota(group.begin(), group.end(), 0);
    const auto root = [&](std::size_t i) {
        while (group[i] != i)
            i = group[i] = group[group[i]];
        return i;
    };
    for (std::size_t a = 0; a < order.size(); ++a) {
        const Box& one = boxes[order[a]];
        for (std::size_t b = a + 1; b < order.size() && boxes[order[b]].left <= one.right; ++b) {
            const Box& other = boxes[order[b]];
            const double overlap = std::min(one.bottom, other.bottom) - std::max(one.top, other.top);
            if (one.tile != other.tile && overlap >= 0.5 * std::min(one.bottom - one.top, other.bottom - other.top))
                group[root(order[b])] = root(order[a]);
        }
    }
    // By group (its first box): what its boxes cover, and how many there are.
    std::vector<Box> around = boxes;
    std::vector<int> members(boxes.size(), 0);
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const std::size_t g = root(i);
        Box& a = around[g];
        a.left = std::min(a.left, boxes[i].left);
        a.top = std::min(a.top, boxes[i].top);
        a.right = std::max(a.right, boxes[i].right);
        a.bottom = std::max(a.bottom, boxes[i].bottom);
        ++members[g];
    }
    std::vector<Quad> joined;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        if (members[i] == 1) // alone: its own corners, turned as it is
            joined.push_back(found[i].second);
        else if (members[i] > 1)
            joined.push_back({QPointF(around[i].left, around[i].top), QPointF(around[i].right, around[i].top),
                QPointF(around[i].right, around[i].bottom), QPointF(around[i].left, around[i].bottom)});
    }
    return joined;
}

// Where a line too long to read in one go is cut into parts no wider than
// `most` pixels: at the emptiest column (fewest pixels away from the line's
// commonest grey) in the stretch before the limit, in the middle of the gap
// there; `space` when the gap before a part is a word's (a quarter high).
struct LinePart {
    int from, width;
    bool space;
};

std::vector<LinePart> partsOf(const QImage& line, int most)
{
    const QImage rgb = line.format() == QImage::Format_RGB888 ? line : line.convertToFormat(QImage::Format_RGB888);
    const int w = rgb.width(), h = rgb.height();
    std::vector<uchar> grey(static_cast<std::size_t>(w) * h);
    std::array<int, 256> counts {};
    for (int y = 0; y < h; ++y) {
        const uchar* row = rgb.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            const auto g = static_cast<uchar>((row[3 * x] * 5 + row[3 * x + 1] * 9 + row[3 * x + 2] * 2) / 16);
            grey[static_cast<std::size_t>(y) * w + x] = g;
            ++counts[g];
        }
    }
    const int background = static_cast<int>(std::ranges::max_element(counts) - counts.begin());
    std::vector<int> ink(w, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            ink[x] += std::abs(grey[static_cast<std::size_t>(y) * w + x] - background) > 48 ? 1 : 0;
    std::vector<LinePart> parts;
    int from = 0;
    bool space = false;
    while (w - from > most) {
        const int to = from + most; // the last column the part may end before
        int best = to;
        for (int x = to - 1; x > to - std::min(most / 2, 3 * h); --x)
            if (ink[x] < ink[best])
                best = x;
        int gapFrom = best, gapTo = best + 1; // the empty columns around it
        if (ink[best] == 0) {
            while (gapFrom > from + 1 && ink[gapFrom - 1] == 0)
                --gapFrom;
            while (gapTo < w && ink[gapTo] == 0)
                ++gapTo;
        }
        const int at = std::clamp((gapFrom + gapTo) / 2, from + 1, to);
        parts.push_back({from, at - from, space});
        space = ink[best] == 0 && gapTo - gapFrom >= h / 4;
        from = at;
    }
    parts.push_back({from, w - from, space});
    return parts;
}

// Written without spaces between words: Chinese, Japanese, their punctuation
// and full-width forms.
bool spaceless(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= 0x2E80 && u <= 0x9FFF) || (u >= 0xF900 && u <= 0xFAFF) || (u >= 0xFF00 && u <= 0xFFEF) || c.isSurrogate();
}

struct Piece {
    double left, top, right, bottom;
    QString text;
};

struct Line {
    double top, bottom;
    std::vector<const Piece*> pieces; // left to right
};

// The pieces' lines: those side by side (overlapping by half the height) on
// one line; top to bottom. They point into `pieces`.
std::vector<Line> linesOf(std::vector<Piece>& pieces)
{
    std::ranges::sort(pieces, {}, [](const Piece& p) { return p.top + p.bottom; });
    std::vector<Line> lines;
    for (const Piece& p : pieces) {
        const double height = p.bottom - p.top;
        Line* into = nullptr;
        for (auto it = lines.rbegin(); it != lines.rend() && !into; ++it) {
            const double overlap = std::min(it->bottom, p.bottom) - std::max(it->top, p.top);
            if (overlap >= 0.5 * std::min(height, it->bottom - it->top))
                into = &*it;
        }
        if (into) {
            into->top = std::min(into->top, p.top);
            into->bottom = std::max(into->bottom, p.bottom);
            into->pieces.push_back(&p);
        } else {
            lines.push_back({p.top, p.bottom, {&p}});
        }
    }
    std::ranges::sort(lines, {}, &Line::top);
    for (Line& line : lines)
        std::ranges::sort(line.pieces, {}, &Piece::left);
    return lines;
}

// Pieces side by side put together, left to right: a space between them
// where there is a gap or Latin writing.
QString joined(const std::vector<const Piece*>& pieces)
{
    QString text;
    const Piece* before = nullptr;
    for (const Piece* p : pieces) {
        if (before && !text.isEmpty() && !p->text.isEmpty()) {
            const double gap = p->left - before->right;
            const double height = std::min(p->bottom - p->top, before->bottom - before->top);
            if (!(spaceless(text.back()) && spaceless(p->text.front()) && gap < height))
                text += u' ';
        }
        text += p->text;
        before = p;
    }
    return text;
}

// The pieces as a table (GitHub feedback "截图 OCR 识别表格"): the lines
// are its rows; its columns are where the lines with pieces side by side
// leave a gap from top to bottom, wider than half the text's height (a
// word's space is narrower). A table has at least two columns and two such
// lines; else none. A piece across several columns (a title) goes in the
// first of them.
QList<QStringList> tableOf(std::vector<Piece> pieces)
{
    const std::vector<Line> lines = linesOf(pieces);
    std::vector<double> heights;
    std::vector<std::pair<double, double>> spans; // left, right of the pieces of lines with more than one
    int sideBySide = 0;
    for (const Line& line : lines) {
        for (const Piece* p : line.pieces)
            heights.push_back(p->bottom - p->top);
        if (line.pieces.size() < 2)
            continue;
        ++sideBySide;
        for (const Piece* p : line.pieces)
            spans.emplace_back(p->left, p->right);
    }
    if (sideBySide < 2)
        return {};
    std::ranges::nth_element(heights, heights.begin() + heights.size() / 2);
    const double gutter = 0.5 * heights[heights.size() / 2];
    std::ranges::sort(spans);
    std::vector<std::pair<double, double>> columns;
    for (const auto& [left, right] : spans) {
        if (!columns.empty() && left - columns.back().second < gutter)
            columns.back().second = std::max(columns.back().second, right);
        else
            columns.emplace_back(left, right);
    }
    if (columns.size() < 2)
        return {};
    const auto columnOf = [&](const Piece& p) {
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (p.right > columns[i].first && p.left < columns[i].second)
                return i; // the first it reaches into
        }
        // In none (a line by itself, outside them all): the nearest.
        std::size_t best = 0;
        double distance = std::numeric_limits<double>::max();
        for (std::size_t i = 0; i < columns.size(); ++i) {
            const double d = std::max(columns[i].first - p.right, p.left - columns[i].second);
            if (d < distance) {
                distance = d;
                best = i;
            }
        }
        return best;
    };
    QList<QStringList> table;
    for (const Line& line : lines) {
        std::vector<std::vector<const Piece*>> cells(columns.size());
        for (const Piece* p : line.pieces)
            cells[columnOf(*p)].push_back(p);
        QStringList row;
        for (const auto& cell : cells)
            row.append(joined(cell));
        table.append(std::move(row));
    }
    return table;
}

using Reading = std::vector<std::pair<QString, float>>;

// Most lines dropped, or those read unsurely: perhaps upside down.
bool readsPoorly(const Reading& read)
{
    int kept = 0;
    double sum = 0;
    for (const auto& [text, score] : read) {
        kept += score >= kTextThreshold && !text.trimmed().isEmpty() ? 1 : 0;
        sum += score;
    }
    return kept < kPoorlyKept * static_cast<double>(read.size())
        || sum < kPoorScore * static_cast<double>(read.size());
}

// What a reading says: the characters of the lines kept, by how sure each is.
double saidBy(const Reading& read)
{
    double said = 0;
    for (const auto& [text, score] : read)
        if (score >= kTextThreshold)
            said += static_cast<double>(text.trimmed().size()) * score;
    return said;
}

} // namespace

struct PaddleOcr::Impl {
    Ort::Env env {nullptr};
    Ort::Session det {nullptr};
    Ort::Session rec {nullptr};
    std::string detIn, detOut, recIn, recOut;
    QStringList characters; // by the reading model's classes: 0 is none
    bool precise = false; // rec is the larger reading model
    bool own = false; // rec is the user's own
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // The table model, loaded when the first table is read (never for the
    // clipboard and OCR search, which read none).
    Ort::Session tableSession {nullptr};
    std::string tableIn;
    std::vector<std::string> tableOut;
    QStringList tableWords; // by the model's classes: begin first, end last
    bool tableTried = false;
    bool loadTable();
    // The picture's cells as the table model sees them; none when it is not
    // there, did not come to an end (a table too long for it) or failed.
    table::Structure tableStructure(const QImage& rgb);

    std::vector<Quad> detect(const QImage& strip);
    // `places`, when given, gets for each line the (index in the text, middle
    // along the line as a fraction of its width) of every character.
    // `progress`, when set, is called after each batch of lines.
    void recognize(const std::vector<QImage>& lines, std::vector<std::pair<QString, float>>* read,
        std::vector<QList<QPair<int, float>>>* places, const std::function<void()>& progress);
};

PaddleOcr::PaddleOcr()
    : d(nullptr)
{
}

PaddleOcr::~PaddleOcr() = default;

void PaddleOcr::setTrace(Trace trace)
{
    g_trace.store(trace);
}

bool PaddleOcr::precise() const { return d && d->precise; }
bool PaddleOcr::own() const { return d && d->own; }

// A reading model to be tried (tryReadingModel): it or nothing.
struct PaddleOcr::Trial {
    QString file;
    QString dictionary;
};

bool PaddleOcr::tryReadingModel(const QString& programFolder, const QString& file, const QString& dictionary, QString* error)
{
    const Trial trial {file, dictionary};
    const std::unique_ptr<PaddleOcr> reader = load(programFolder, 2, error, &trial);
    if (!reader)
        return false;
    QImage line(360, 64, QImage::Format_RGB32);
    line.fill(Qt::white);
    {
        QPainter painter(&line);
        QFont font = painter.font();
        font.setPixelSize(30);
        painter.setFont(font);
        painter.setPen(Qt::black);
        painter.drawText(line.rect(), Qt::AlignCenter, u"Test 2026"_s);
    }
    QStringList lines;
    return reader->read(line, &lines, error); // what it reads is its own business; that it reads is ours
}

std::unique_ptr<PaddleOcr> PaddleOcr::load(const QString& programFolder, int threads, QString* error)
{
    return load(programFolder, threads, error, nullptr);
}

bool PaddleOcr::installed(const QString& programFolder)
{
    return !ocr::folder(programFolder).isEmpty();
}

std::unique_ptr<PaddleOcr> PaddleOcr::load(const QString& programFolder, int threads, QString* error, const Trial* trial)
{
    const QString resolvedFolder = ocr::folder(programFolder);
    if (resolvedFolder.isEmpty()) {
        if (error)
            *error = u"the text recognition files are missing"_s;
        return nullptr;
    }
    if (!loadApi(resolvedFolder, error))
        return nullptr;
    std::unique_ptr<PaddleOcr> ocr(new PaddleOcr);
    try {
        auto impl = std::make_unique<Impl>();
        // One set of threads for both models, waiting rather than spinning.
        Ort::ThreadingOptions threading;
        threading.SetGlobalIntraOpNumThreads(std::max(1, threads))
            .SetGlobalInterOpNumThreads(1)
            .SetGlobalSpinControl(0)
            .SetGlobalCustomCreateThreadFn(createThread)
            .SetGlobalCustomJoinThreadFn(joinThread);
        impl->env = Ort::Env(threading, ORT_LOGGING_LEVEL_ERROR, "WinShun");
        const Ort::SessionOptions options = sessionOptions();
        const auto open = [&](const wchar_t* name) {
            const QString file = filePath(resolvedFolder, name, true);
            return Ort::Session(impl->env, reinterpret_cast<const wchar_t*>(file.utf16()), options);
        };
        impl->det = open(kDetModel);
        Ort::AllocatorWithDefaultOptions allocator;
        // A reading model and the characters it reads, one per line: in the
        // model, else in the dictionary beside it; around them "none" first
        // and the space last. Throws when either is missing.
        const auto reading = [&](const QString& file, const QString& dictionary) {
            const QString native = QDir::toNativeSeparators(file);
            Ort::Session session(impl->env, reinterpret_cast<const wchar_t*>(native.utf16()), options);
            QStringList characters;
            if (const Ort::AllocatedStringPtr keys = session.GetModelMetadata().LookupCustomMetadataMapAllocated("character", allocator)) {
                characters = QString::fromUtf8(keys.get()).split(u'\n');
            } else {
                QFile words(dictionary);
                if (dictionary.isEmpty() || !words.open(QIODevice::ReadOnly))
                    throw std::runtime_error("no characters in the reading model");
                characters = QString::fromUtf8(words.readAll()).remove(u'\r').split(u'\n');
                if (!characters.isEmpty() && characters.constLast().isEmpty())
                    characters.removeLast();
            }
            characters.prepend(QString());
            characters.append(u" "_s);
            impl->rec = std::move(session);
            impl->characters = std::move(characters);
        };
        if (trial) {
            reading(trial->file, trial->dictionary); // it or nothing
            impl->own = true;
        } else {
            // The user's own reading model, else the larger one where it is
            // installed, else the tiny one; a file gone bad is passed over,
            // said in the log.
            if (const ocr::OwnModel own = ocr::ownModel(); !own.file.isEmpty()) {
                try {
                    reading(own.file, own.dictionary);
                    impl->own = true;
                } catch (const std::exception& e) {
                    qWarning().noquote() << "Text recognition: the user's own reading model did not load:" << e.what();
                }
            }
            if (const QString precise = ocr::preciseModel(); !impl->own && !precise.isEmpty()) {
                try {
                    reading(precise, {});
                    impl->precise = true;
                } catch (const std::exception& e) {
                    qWarning().noquote() << "Text recognition: the larger reading model did not load, the tiny one instead:" << e.what();
                }
            }
            if (!impl->own && !impl->precise)
                reading(filePath(resolvedFolder, kRecModel, true), {});
        }
        impl->detIn = impl->det.GetInputNameAllocated(0, allocator).get();
        impl->detOut = impl->det.GetOutputNameAllocated(0, allocator).get();
        impl->recIn = impl->rec.GetInputNameAllocated(0, allocator).get();
        impl->recOut = impl->rec.GetOutputNameAllocated(0, allocator).get();
        ocr->d = std::move(impl);
    } catch (const std::exception& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return nullptr;
    }
    return ocr;
}

void PaddleOcr::setTableModel(const QString& file)
{
    const std::lock_guard lock(g_tableModelMutex);
    g_tableModel = file;
}

bool PaddleOcr::hasTableModel()
{
    const std::lock_guard lock(g_tableModelMutex);
    return !g_tableModel.isEmpty() && QFile::exists(g_tableModel);
}

bool PaddleOcr::Impl::loadTable()
{
    if (tableTried)
        return static_cast<bool>(tableSession);
    tableTried = true;
    QString file;
    {
        const std::lock_guard lock(g_tableModelMutex);
        file = QDir::toNativeSeparators(g_tableModel);
    }
    if (file.isEmpty() || !QFile::exists(file))
        return false;
    try {
        Ort::Session session(env, reinterpret_cast<const wchar_t*>(file.utf16()), sessionOptions());
        Ort::AllocatorWithDefaultOptions allocator;
        const Ort::AllocatedStringPtr words = session.GetModelMetadata().LookupCustomMetadataMapAllocated("character", allocator);
        if (!words || session.GetInputCount() != 1 || session.GetOutputCount() != 2)
            throw std::runtime_error("not a table structure model with its words in it");
        // The model's words, as PaddleOCR decodes them: an empty cell is one
        // word, and around them "begin" and "end".
        QStringList list = QString::fromUtf8(words.get()).remove(u'\r').split(u'\n', Qt::SkipEmptyParts);
        for (QString& word : list) {
            if (!word.startsWith(u' ')) // " colspan=…" keeps its space
                word = word.trimmed();
        }
        if (!list.contains(u"<td></td>"_s))
            list.append(u"<td></td>"_s);
        list.removeAll(u"<td>"_s);
        list.prepend(QString());
        list.append(QString());
        tableWords = std::move(list);
        tableIn = session.GetInputNameAllocated(0, allocator).get();
        tableOut = {session.GetOutputNameAllocated(0, allocator).get(), session.GetOutputNameAllocated(1, allocator).get()};
        tableSession = std::move(session);
    } catch (const std::exception& e) {
        qWarning().noquote() << "Text recognition: the table model did not load, tables are worked out without it:" << e.what();
    }
    return static_cast<bool>(tableSession);
}

table::Structure PaddleOcr::Impl::tableStructure(const QImage& rgb)
{
    if (!loadTable() || rgb.isNull())
        return {};
    const double ratio = static_cast<double>(kTableSide) / std::max(rgb.width(), rgb.height());
    const int w = std::clamp(static_cast<int>(rgb.width() * ratio), 1, kTableSide);
    const int h = std::clamp(static_cast<int>(rgb.height() * ratio), 1, kTableSide);
    QImage small = scaledRgb(rgb, w, h);
    {
        qint64 sum = 0;
        for (int y = 0; y < h; ++y) {
            const uchar* line = small.constScanLine(y);
            for (int x = 0; x < 3 * w; ++x)
                sum += line[x];
        }
        if (sum / (3ll * w * h) < kTableDark)
            small.invertPixels();
    }
    // Blue, green, red planes, each by the mean and deviation PaddleOCR gives
    // its first, second and third plane; 0 where the picture is not.
    static constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
    static constexpr float kDeviation[3] = {0.229f, 0.224f, 0.225f};
    const std::size_t plane = static_cast<std::size_t>(kTableSide) * kTableSide;
    std::vector<float> planes(3 * plane, 0.f);
    for (int y = 0; y < h; ++y) {
        const uchar* line = small.constScanLine(y);
        float* out = planes.data() + static_cast<std::size_t>(y) * kTableSide;
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c)
                out[c * plane + x] = (line[3 * x + 2 - c] / 255.f - kMean[c]) / kDeviation[c];
        }
    }
    const std::array<int64_t, 4> shape {1, 3, kTableSide, kTableSide};
    const Ort::Value in = Ort::Value::CreateTensor<float>(memory, planes.data(), planes.size(), shape.data(), shape.size());
    const char* inName = tableIn.c_str();
    const char* outNames[2] = {tableOut[0].c_str(), tableOut[1].c_str()};
    const std::vector<Ort::Value> out = tableSession.Run(Ort::RunOptions {nullptr}, &inName, &in, 1, outNames, 2);
    // One answer says the words (a score for each of them at every step), the
    // other where each step's cell is (4 or 8 numbers, 0…1 of the square).
    const std::vector<int64_t> first = out[0].GetTensorTypeAndShapeInfo().GetShape();
    const std::vector<int64_t> second = out[1].GetTensorTypeAndShapeInfo().GetShape();
    if (first.size() != 3 || second.size() != 3 || first[1] != second[1])
        throw std::runtime_error("unexpected table model output");
    const bool wordsFirst = first[2] == tableWords.size();
    const std::vector<int64_t>& wordShape = wordsFirst ? first : second;
    const std::vector<int64_t>& boxShape = wordsFirst ? second : first;
    if (wordShape[2] != tableWords.size() || (boxShape[2] != 4 && boxShape[2] != 8))
        throw std::runtime_error("the table model's words do not match its classes");
    const float* scores = out[wordsFirst ? 0 : 1].GetTensorData<float>();
    const float* places = out[wordsFirst ? 1 : 0].GetTensorData<float>();
    const auto steps = static_cast<std::size_t>(wordShape[1]);
    const auto classes = static_cast<std::size_t>(wordShape[2]);
    const auto numbers = static_cast<std::size_t>(boxShape[2]);
    const std::size_t end = classes - 1;
    QStringList words;
    std::vector<QRectF> boxes;
    bool ended = false;
    double sure = 0; // the words' scores, added up
    for (std::size_t s = 0; s < steps; ++s) {
        const float* step = scores + s * classes;
        const std::size_t best = static_cast<std::size_t>(std::max_element(step, step + classes) - step);
        if (s > 0 && best == end) {
            ended = true;
            break;
        }
        if (best == 0 || best == end)
            continue;
        const QString& word = tableWords[static_cast<qsizetype>(best)];
        if (word == u"<td" || word == u"<td></td>") {
            const float* p = places + s * numbers;
            double left = 1, top = 1, right = 0, bottom = 0;
            for (std::size_t k = 0; k + 1 < numbers; k += 2) {
                left = std::min<double>(left, p[k]);
                right = std::max<double>(right, p[k]);
                top = std::min<double>(top, p[k + 1]);
                bottom = std::max<double>(bottom, p[k + 1]);
            }
            const double toPicture = kTableSide / ratio;
            boxes.emplace_back(QPointF(left * toPicture, top * toPicture), QPointF(right * toPicture, bottom * toPicture));
        }
        words.append(word);
        sure += step[best];
    }
    sure = words.isEmpty() ? 0 : sure / static_cast<double>(words.size());
    trace([&] { return u"table: %1 words, %2 cells, sure %3%4"_s.arg(words.size()).arg(boxes.size()).arg(sure, 0, 'f', 3).arg(ended ? QString() : u", not ended"_s); });
    if (!ended)
        return {}; // longer than the model can say: its rows would stop short
    table::Structure structure = table::structureOf(words, boxes);
    structure.sure = sure;
    return structure;
}

bool PaddleOcr::readTable(const QImage& image, double larger, QStringList* lines, OcrTable* table, QString* error)
{
    lines->clear();
    *table = {};
    QList<OcrPiece> found;
    {
        QImage enlarged;
        if (larger > 1.0)
            enlarged = image.scaled(std::max(1, static_cast<int>(std::lround(image.width() * larger))),
                std::max(1, static_cast<int>(std::lround(image.height() * larger))), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (!find(larger > 1.0 ? enlarged : image, &found, true, error, {}))
            return false;
    }
    std::vector<Piece> pieces;
    for (const OcrPiece& p : std::as_const(found)) {
        const QRectF box = p.quad.boundingRect();
        pieces.push_back({box.left(), box.top(), box.right(), box.bottom(), p.text});
    }
    for (const Line& line : linesOf(pieces))
        lines->append(joined(line.pieces));
    // Without lines or the model, or where neither is sure: from where the words stand.
    table->rows = tableOf(std::move(pieces));
    if (found.size() < 4 || static_cast<qint64>(image.width()) * image.height() * std::max(1.0, larger * larger) > kMaxPixels)
        return true; // (a picture that large was read smaller: its lines are not where its cells are)
    try {
        QImage picture = image;
        if (image.format() != QImage::Format_RGB888) {
            picture = QImage(image.size(), QImage::Format_RGB888);
            picture.fill(Qt::white);
            QPainter painter(&picture);
            painter.drawImage(0, 0, image);
        }
        // The lines read, in the picture as it is.
        std::vector<double> heights;
        if (larger > 1.0) {
            for (OcrPiece& p : found) {
                for (QPointF& corner : p.quad)
                    corner /= larger;
            }
        }
        for (const OcrPiece& p : std::as_const(found))
            heights.push_back(p.quad.boundingRect().height());
        std::ranges::nth_element(heights, heights.begin() + static_cast<std::ptrdiff_t>(heights.size() / 2));
        // What makes cells of a picture makes them of any picture: its table
        // is one when the words in it stand as a table's.
        // Nor is it taken when it tears a line of writing into two rows.
        const auto taken = [&](const table::Structure& structure, const char16_t* by) {
            if (structure.rows < 2 || structure.columns < 2)
                return false;
            int torn = 0;
            OcrTable seen = table::filled(structure, found, &torn);
            const bool good = torn == 0 && seen.standsAsTable();
            trace([&] { return u"table by %1: %2 x %3, %4 lines torn, %5"_s.arg(QStringView(by)).arg(structure.rows).arg(structure.columns)
                .arg(torn).arg(good ? u"taken"_s : u"not taken"_s); });
            if (!good)
                return false;
            *table = std::move(seen);
            return true;
        };
        if (!taken(table::ruledStructure(picture, heights[heights.size() / 2]), u"its lines")) {
            if (const table::Structure structure = d->tableStructure(picture); structure.sure >= kTableSure)
                taken(structure, u"the model");
        }
    } catch (const std::exception& e) {
        qWarning().noquote() << "Text recognition: the table model failed, the table is worked out without it:" << e.what();
    }
    return true;
}
std::vector<Quad> PaddleOcr::Impl::detect(const QImage& strip)
{
    const int w = round32(strip.width()), h = round32(strip.height());
    trace([&] { return u"detect %1 x %2"_s.arg(w).arg(h); });
    const QImage input = scaledRgb(strip, w, h);
    std::vector<float> planes(3 * static_cast<std::size_t>(w) * h);
    toPlanes(input, planes.data(), w);
    const std::array<int64_t, 4> shape {1, 3, h, w};
    const Ort::Value in = Ort::Value::CreateTensor<float>(memory, planes.data(), planes.size(), shape.data(), shape.size());
    const char* inName = detIn.c_str();
    const char* outName = detOut.c_str();
    const std::vector<Ort::Value> out = det.Run(Ort::RunOptions {nullptr}, &inName, &in, 1, &outName, 1);
    const std::vector<int64_t> dims = out[0].GetTensorTypeAndShapeInfo().GetShape();
    if (dims.size() != 4 || dims[2] != h || dims[3] != w)
        throw std::runtime_error("unexpected detection output");
    std::vector<Quad> boxes = boxesFrom(out[0].GetTensorData<float>(), w, h, strip.width(), strip.height());
    trace([&] { return u"found %1 boxes"_s.arg(boxes.size()); });
    return boxes;
}

void PaddleOcr::Impl::recognize(const std::vector<QImage>& lines, std::vector<std::pair<QString, float>>* read,
    std::vector<QList<QPair<int, float>>>* places, const std::function<void()>& progress)
{
    // A line longer than kLineColumns at the model's height is read in parts
    // (partsOf), each as a line of its own; their readings put back together
    // below. What one reading of the model takes goes with its columns.
    struct Part {
        std::size_t line;
        QImage image;
        int from, width; // in the line's pixels
        bool space; // a word's gap before it
    };
    std::vector<Part> parts;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const QImage& line = lines[i];
        const int most = static_cast<int>(static_cast<qint64>(kLineColumns) * line.height() / kLineHeight);
        if (line.width() <= most) {
            parts.push_back({i, line, 0, line.width(), false});
            continue;
        }
        for (const LinePart& p : partsOf(line, most))
            parts.push_back({i, line.copy(p.from, 0, p.width, line.height()), p.from, p.width, p.space});
    }
    std::vector<std::size_t> order(parts.size());
    std::iota(order.begin(), order.end(), 0);
    const auto ratio = [&](std::size_t i) { return static_cast<double>(parts[i].image.width()) / parts[i].image.height(); };
    const auto columns = [&](std::size_t i) { return kLineHeight * std::max(kMinRatio, ratio(i)); };
    std::ranges::sort(order, {}, ratio);
    // Up to kBatch together, padded to the widest, in no more than kBatchColumns.
    std::vector<std::pair<std::size_t, std::size_t>> batches; // first, count
    for (std::size_t first = 0; first < order.size();) {
        std::size_t count = 1;
        while (count < static_cast<std::size_t>(kBatch) && first + count < order.size()
            && static_cast<double>(count + 1) * columns(order[first + count]) <= kBatchColumns)
            ++count;
        batches.emplace_back(first, count);
        first += count;
    }
    struct PartRead {
        QString text;
        double sum = 0; // of the characters' scores
        int kept = 0;
        QList<QPair<int, float>> where;
    };
    std::vector<PartRead> partRead(parts.size());
    for (std::size_t b = 0; b < batches.size(); ++b) {
        const auto [first, count] = batches[b];
        double widest = kMinRatio;
        for (std::size_t i = 0; i < count; ++i)
            widest = std::max(widest, ratio(order[first + i]));
        const int width = static_cast<int>(kLineHeight * widest);
        trace([&] { return u"recognize %1 lines, %2 x %3 (batch %4 of %5)"_s.arg(count).arg(kLineHeight).arg(width)
            .arg(b + 1).arg(batches.size()); });
        // Each line 48 px high, left-aligned; the rest 0 (mid grey).
        std::vector<float> planes(count * 3 * kLineHeight * static_cast<std::size_t>(width), 0.f);
        std::vector<int> used(count); // each line's width in the batch's, the rest being padding
        for (std::size_t i = 0; i < count; ++i) {
            const QImage& line = parts[order[first + i]].image;
            const int w = std::min(width, static_cast<int>(std::ceil(kLineHeight * ratio(order[first + i]))));
            used[i] = std::max(1, w);
            toPlanes(scaledRgb(line, used[i], kLineHeight), planes.data() + i * 3 * kLineHeight * width, width);
        }
        const std::array<int64_t, 4> shape {static_cast<int64_t>(count), 3, kLineHeight, width};
        const Ort::Value in = Ort::Value::CreateTensor<float>(memory, planes.data(), planes.size(), shape.data(), shape.size());
        const char* inName = recIn.c_str();
        const char* outName = recOut.c_str();
        const std::vector<Ort::Value> out = rec.Run(Ort::RunOptions {nullptr}, &inName, &in, 1, &outName, 1);
        const std::vector<int64_t> dims = out[0].GetTensorTypeAndShapeInfo().GetShape();
        if (dims.size() != 3 || dims[0] != static_cast<int64_t>(count))
            throw std::runtime_error("unexpected recognition output");
        const float* scores = out[0].GetTensorData<float>();
        const auto steps = static_cast<std::size_t>(dims[1]);
        const auto classes = static_cast<std::size_t>(dims[2]);
        if (classes != static_cast<std::size_t>(characters.size()))
            throw std::runtime_error("the reading model's characters do not match its classes");
        for (std::size_t i = 0; i < count; ++i) {
            // CTC: the best class at each step, repeats and "none" left out.
            QString text;
            double sum = 0;
            int kept = 0;
            std::size_t before = 0;
            // A character is a run of steps with the same class; its middle
            // step is where it stands. The padded width is spread over all
            // the steps, the line itself being at the left.
            QList<QPair<int, float>> where;
            std::size_t runStart = 0;
            const auto close = [&](std::size_t end) {
                if (!places || where.isEmpty() || where.last().second >= 0)
                    return;
                const double middle = (runStart + end) / 2.0 + 0.5;
                where.last().second = static_cast<float>(
                    std::clamp(middle * (static_cast<double>(width) / static_cast<double>(steps)) / used[i], 0.0, 1.0));
            };
            for (std::size_t s = 0; s < steps; ++s) {
                const float* step = scores + (i * steps + s) * classes;
                const std::size_t best = static_cast<std::size_t>(std::max_element(step, step + classes) - step);
                if (best != before) {
                    if (before != 0)
                        close(s - 1);
                    if (best != 0) {
                        where.append({static_cast<int>(text.size()), -1.f}); // the place is set when the run ends
                        runStart = s;
                        text += characters[static_cast<qsizetype>(best)];
                        sum += step[best];
                        ++kept;
                    }
                }
                before = best;
            }
            if (before != 0)
                close(steps - 1);
            partRead[order[first + i]] = {text, sum, kept, where};
        }
        if (progress)
            progress();
    }
    // Each line from its parts, left to right as they were cut: a space where
    // a word's gap was cut between Latin writing; the places along the part
    // to along the line.
    read->assign(lines.size(), {});
    if (places)
        places->assign(lines.size(), {});
    std::vector<double> sums(lines.size(), 0);
    std::vector<int> kept(lines.size(), 0);
    for (std::size_t k = 0; k < parts.size(); ++k) {
        const Part& part = parts[k];
        const PartRead& r = partRead[k];
        QString& text = (*read)[part.line].first;
        if (part.space && !text.isEmpty() && !r.text.isEmpty() && !text.back().isSpace() && !r.text.front().isSpace()
            && !spaceless(text.back()) && !spaceless(r.text.front()))
            text += u' ';
        if (places) {
            const double whole = lines[part.line].width();
            for (const auto& [at, along] : r.where)
                (*places)[part.line].append({at + static_cast<int>(text.size()),
                    along < 0 ? along : static_cast<float>((part.from + along * part.width) / whole)});
        }
        text += r.text;
        sums[part.line] += r.sum;
        kept[part.line] += r.kept;
    }
    for (std::size_t i = 0; i < lines.size(); ++i)
        (*read)[i].second = kept[i] ? static_cast<float>(sums[i] / kept[i]) : 0.f;
}

QList<QStringList> ocrTable(const QList<OcrPiece>& found)
{
    std::vector<Piece> pieces;
    for (const OcrPiece& p : found) {
        const QRectF box = p.quad.boundingRect();
        pieces.push_back({box.left(), box.top(), box.right(), box.bottom(), p.text});
    }
    return tableOf(std::move(pieces));
}

QStringList ocrLines(const QList<OcrPiece>& found)
{
    std::vector<Piece> pieces;
    for (const OcrPiece& p : found) {
        const QRectF box = p.quad.boundingRect();
        pieces.push_back({box.left(), box.top(), box.right(), box.bottom(), p.text});
    }
    QStringList lines;
    for (const Line& line : linesOf(pieces))
        lines.append(joined(line.pieces));
    return lines;
}

bool PaddleOcr::read(const QImage& image, QStringList* lines, QString* error, QList<QStringList>* table)
{
    lines->clear();
    if (table)
        table->clear();
    QList<OcrPiece> found;
    if (!find(image, &found, false, error, {}))
        return false;
    *lines = ocrLines(found);
    if (table)
        *table = ocrTable(found);
    return true;
}

bool PaddleOcr::readPieces(const QImage& image, QList<OcrPiece>* pieces, QString* error, const OcrOptions& options)
{
    pieces->clear();
    return find(image, pieces, true, error, options);
}

bool PaddleOcr::find(
    const QImage& image, QList<OcrPiece>* found, bool chars, QString* error, const OcrOptions& options)
{
    if (image.isNull())
        return true;
    // See-through parts white: black text there would be read as black on black.
    // A 24-bit picture has none and is read as it is, not copied: OCR search
    // hands over its long pictures and big pages that way to save memory.
    QImage picture = image;
    if (image.format() != QImage::Format_RGB888) {
        picture = QImage(image.size(), QImage::Format_RGB888);
        picture.fill(Qt::white);
        QPainter painter(&picture);
        painter.drawImage(0, 0, image);
    }
    if (static_cast<qint64>(picture.width()) * picture.height() > kMaxPixels) {
        const double s = std::sqrt(static_cast<double>(kMaxPixels) / (static_cast<qint64>(picture.width()) * picture.height()));
        picture = scaledRgb(picture, std::max(1, static_cast<int>(picture.width() * s)), std::max(1, static_cast<int>(picture.height() * s)));
    }
    try {
        // Where to look for text: no wider than kMaxWidth, no side under kMinSide.
        double scale = 1;
        if (picture.width() > kMaxWidth)
            scale = static_cast<double>(kMaxWidth) / picture.width();
        if (std::min(picture.width(), picture.height()) * scale < kMinSide)
            scale = static_cast<double>(kMinSide) / std::min(picture.width(), picture.height());
        QImage look = scale == 1 ? picture
                                 : scaledRgb(picture, std::max(1, static_cast<int>(picture.width() * scale)),
                                       std::max(1, static_cast<int>(picture.height() * scale)));
        trace([&] { return u"picture %1 x %2, read at %3 x %4, lines looked for at %5 x %6"_s.arg(image.width())
            .arg(image.height()).arg(picture.width()).arg(picture.height()).arg(look.width()).arg(look.height()); });
        // Across in tiles no wider than kMaxWidth (a long strip, kept wider
        // for its text's height), each down in strips of kStrip: one look is
        // never more than a 2000 x 2000 screenshot's, whatever the shape.
        std::vector<std::pair<int, Quad>> tileBoxes; // the tile each is from
        int tiles = 0;
        for (int left = 0;;) {
            const int across = std::min(kMaxWidth, look.width() - left);
            const bool lastTile = left + across >= look.width();
            QImage tile = across == look.width() ? look : look.copy(left, 0, across, look.height());
            // A thin strip gets black above and below: the model finds its text then.
            int padding = 0;
            if (tile.height() <= kMinSide || tile.width() > 8 * tile.height()) {
                padding = std::abs(std::max(tile.width() / 8, kMinSide) * 2 - tile.height()) / 2;
                QImage padded(tile.width(), tile.height() + 2 * padding, QImage::Format_RGB888);
                padded.fill(Qt::black);
                {
                    QPainter painter(&padded);
                    painter.drawImage(0, padding, tile);
                }
                tile = padded;
            }
            // Strip by strip; a box belongs to the strip its middle is in, away
            // from where it overlaps the next. False, with nothing kept of the
            // tile, when a box that is this strip's reaches the edge the strip
            // was cut at: a line higher than the overlap, whole in no strip.
            const int height = tile.height();
            const auto lookDown = [&](int most) {
                const std::size_t before = tileBoxes.size();
                for (int top = 0;;) {
                    const int strip = std::min(most, height - top);
                    const bool last = top + strip >= height;
                    const double from = top == 0 ? -1 : top + kOverlap / 2.0;
                    const double to = last ? height + 1.0 : top + strip - kOverlap / 2.0;
                    for (Quad quad : d->detect(strip == height ? tile : tile.copy(0, top, tile.width(), strip))) {
                        const auto [highest, lowest] = std::ranges::minmax(quad, {}, &QPointF::y);
                        const double middle = top + (quad[0].y() + quad[2].y()) / 2;
                        if (middle < from || middle >= to)
                            continue;
                        if (most < kTallStrip && ((top > 0 && highest.y() <= kCutEdge) || (!last && lowest.y() >= strip - kCutEdge))) {
                            trace([&] { return u"a line cut by a strip of %1 rows: looked at again in higher ones"_s.arg(most); });
                            tileBoxes.resize(before);
                            return false;
                        }
                        for (QPointF& p : quad) // to the picture's own pixels
                            p = QPointF(std::clamp((p.x() + left) / scale, 0.0, static_cast<double>(picture.width())),
                                std::clamp((p.y() + top - padding) / scale, 0.0, static_cast<double>(picture.height())));
                        tileBoxes.emplace_back(tiles, quad);
                    }
                    if (options.progress)
                        options.progress();
                    if (last)
                        break;
                    top += strip - kOverlap;
                }
                return true;
            };
            if (!lookDown(kStrip))
                lookDown(kTallStrip);
            ++tiles;
            if (lastTile)
                break;
            left += across - kOverlap;
        }
        const std::vector<Quad> boxes = joinedAcrossTiles(std::move(tileBoxes));
        std::vector<QImage> cuts;
        std::vector<Quad> quads;
        std::vector<bool> upright; // false: standing up, turned to be read (no places along it)
        for (const Quad& quad : boxes) {
            QImage line = cut(picture, quad);
            if (line.isNull())
                continue;
            const int w = static_cast<int>(std::max(distance(quad[0], quad[1]), distance(quad[2], quad[3])));
            const int h = static_cast<int>(std::max(distance(quad[0], quad[3]), distance(quad[1], quad[2])));
            upright.push_back(h < 1.5 * w);
            cuts.push_back(std::move(line));
            quads.push_back(quad);
        }
        trace([&] {
            QSize widest;
            for (const QImage& line : cuts)
                if (line.height() > 0 && (widest.isEmpty() || line.width() * widest.height() > widest.width() * line.height()))
                    widest = line.size();
            return u"cut %1 lines, the longest %2 x %3"_s.arg(cuts.size()).arg(widest.width()).arg(widest.height());
        });
        Reading read;
        std::vector<QList<QPair<int, float>>> places;
        d->recognize(cuts, &read, chars ? &places : nullptr, options.progress);
        if (options.retryUpsideDown && cuts.size() >= 2 && readsPoorly(read)) {
            trace([] { return u"read again turned"_s; });
            std::vector<QImage> turned;
            turned.reserve(cuts.size());
            for (const QImage& line : cuts)
                turned.push_back(line.flipped(Qt::Horizontal | Qt::Vertical));
            Reading other;
            std::vector<QList<QPair<int, float>>> otherPlaces;
            d->recognize(turned, &other, chars ? &otherPlaces : nullptr, options.progress);
            if (saidBy(other) > kClearlyMore * saidBy(read)) {
                read = std::move(other);
                places = std::move(otherPlaces);
                for (auto& line : places) // along the line the other way round
                    for (auto& place : line)
                        place.second = 1.f - place.second;
            }
        }
        // Back to the picture that came in, if it was made smaller.
        const double back = static_cast<double>(image.width()) / picture.width();
        for (std::size_t i = 0; i < quads.size(); ++i) {
            const QString raw = read[i].first;
            const QString text = raw.trimmed();
            if (text.isEmpty() || read[i].second < kTextThreshold)
                continue;
            OcrPiece piece;
            for (const QPointF& p : quads[i])
                piece.quad.append(p * back);
            piece.text = text;
            piece.score = read[i].second;
            if (chars && upright[i]) {
                int lead = 0; // the blanks trimmed off the front
                while (lead < raw.size() && raw[lead].isSpace())
                    ++lead;
                for (const QPair<int, float>& c : std::as_const(places[i])) {
                    const int at = c.first - lead;
                    if (at >= 0 && at < text.size())
                        piece.chars.append({at, c.second});
                }
            }
            found->append(std::move(piece));
        }
    } catch (const std::exception& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return false;
    }
    return true;
}

} // namespace ws
