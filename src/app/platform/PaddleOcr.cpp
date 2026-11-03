#include "PaddleOcr.h"
#include "OcrFiles.h"

// The API comes from the DLL loaded here (Ort::InitApi), not from linking it.
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>

#include <QDir>
#include <QFile>
#include <QPainter>
#include <QPointF>
#include <QTransform>

#include <windows.h>

#include <algorithm>
#include <array>
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
// strips rather than shrunk, and lines are cut from the picture as it is.
constexpr int kMaxWidth = 2000; // wider: made smaller to find the text in it
constexpr int kMinSide = 30; // narrower: made larger
constexpr int kStrip = 2000; // the height looked at in one go…
constexpr int kOverlap = 160; // … overlapping, so a line is whole in one of them
constexpr float kPixelThreshold = 0.3f; // a pixel is text
constexpr float kBoxThreshold = 0.5f; // a box is text: its pixels' mean
constexpr double kUnclip = 1.6; // how far a box grows around its text
constexpr int kMaxBoxes = 1000;
constexpr int kLineHeight = 48; // what the reading model takes
constexpr double kMinRatio = 320.0 / 48.0;
constexpr int kBatch = 6;
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

QString filePath(const QString& folder, const wchar_t* name, bool model)
{
    return QDir::toNativeSeparators(folder + (model ? u"/ocr/"_s : u"/"_s) + QString::fromWCharArray(name));
}

// ONNX Runtime from the resolved component/legacy folder, once.
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

// The pieces as lines: those side by side (overlapping by half the height)
// on one line, left to right.
QStringList linesOf(std::vector<Piece> pieces)
{
    std::ranges::sort(pieces, {}, [](const Piece& p) { return p.top + p.bottom; });
    struct Line {
        double top, bottom;
        std::vector<const Piece*> pieces;
    };
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
    QStringList out;
    for (Line& line : lines) {
        std::ranges::sort(line.pieces, {}, &Piece::left);
        QString text;
        const Piece* before = nullptr;
        for (const Piece* p : line.pieces) {
            if (before) {
                const double gap = p->left - before->right;
                const double height = std::min(p->bottom - p->top, before->bottom - before->top);
                if (!(spaceless(text.back()) && spaceless(p->text.front()) && gap < height))
                    text += u' ';
            }
            text += p->text;
            before = p;
        }
        out.append(text);
    }
    return out;
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
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

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

bool PaddleOcr::installed(const QString& programFolder)
{
    return !ocr::folder(programFolder).isEmpty();
}

std::unique_ptr<PaddleOcr> PaddleOcr::load(const QString& programFolder, int threads, QString* error)
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
        Ort::SessionOptions options;
        options.DisablePerSessionThreads()
            .SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL)
            .DisableCpuMemArena() // memory back after each picture
            // A planned block per input size is ~135 MB more than the
            // detection's own peak when a tall picture's strips repeat a size
            // (measured on 2000x4000..3200x12500), and no faster.
            .DisableMemPattern();
        const auto open = [&](const wchar_t* name) {
            const QString file = filePath(resolvedFolder, name, true);
            return Ort::Session(impl->env, reinterpret_cast<const wchar_t*>(file.utf16()), options);
        };
        impl->det = open(kDetModel);
        impl->rec = open(kRecModel);
        Ort::AllocatorWithDefaultOptions allocator;
        impl->detIn = impl->det.GetInputNameAllocated(0, allocator).get();
        impl->detOut = impl->det.GetOutputNameAllocated(0, allocator).get();
        impl->recIn = impl->rec.GetInputNameAllocated(0, allocator).get();
        impl->recOut = impl->rec.GetOutputNameAllocated(0, allocator).get();
        // The characters it reads are in the model, one per line; around
        // them "none" first and the space last.
        const Ort::AllocatedStringPtr keys = impl->rec.GetModelMetadata().LookupCustomMetadataMapAllocated("character", allocator);
        if (!keys)
            throw std::runtime_error("no characters in the reading model");
        impl->characters = QString::fromUtf8(keys.get()).split(u'\n');
        impl->characters.prepend(QString());
        impl->characters.append(u" "_s);
        ocr->d = std::move(impl);
    } catch (const std::exception& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return nullptr;
    }
    return ocr;
}

std::vector<Quad> PaddleOcr::Impl::detect(const QImage& strip)
{
    const int w = round32(strip.width()), h = round32(strip.height());
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
    return boxesFrom(out[0].GetTensorData<float>(), w, h, strip.width(), strip.height());
}

void PaddleOcr::Impl::recognize(const std::vector<QImage>& lines, std::vector<std::pair<QString, float>>* read,
    std::vector<QList<QPair<int, float>>>* places, const std::function<void()>& progress)
{
    read->assign(lines.size(), {});
    if (places)
        places->assign(lines.size(), {});
    std::vector<std::size_t> order(lines.size());
    std::iota(order.begin(), order.end(), 0);
    const auto ratio = [&](std::size_t i) { return static_cast<double>(lines[i].width()) / lines[i].height(); };
    std::ranges::sort(order, {}, ratio);
    for (std::size_t first = 0; first < order.size(); first += kBatch) {
        const std::size_t count = std::min<std::size_t>(kBatch, order.size() - first);
        double widest = kMinRatio;
        for (std::size_t i = 0; i < count; ++i)
            widest = std::max(widest, ratio(order[first + i]));
        const int width = static_cast<int>(kLineHeight * widest);
        // Each line 48 px high, left-aligned; the rest 0 (mid grey).
        std::vector<float> planes(count * 3 * kLineHeight * static_cast<std::size_t>(width), 0.f);
        std::vector<int> used(count); // each line's width in the batch's, the rest being padding
        for (std::size_t i = 0; i < count; ++i) {
            const QImage& line = lines[order[first + i]];
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
            (*read)[order[first + i]] = {text, kept ? static_cast<float>(sum / kept) : 0.f};
            if (places)
                (*places)[order[first + i]] = where;
        }
        if (progress)
            progress();
    }
}

bool PaddleOcr::read(const QImage& image, QStringList* lines, QString* error)
{
    lines->clear();
    QList<OcrPiece> found;
    if (!find(image, &found, false, error, {}))
        return false;
    std::vector<Piece> pieces;
    for (const OcrPiece& p : std::as_const(found)) {
        const QRectF box = p.quad.boundingRect();
        pieces.push_back({box.left(), box.top(), box.right(), box.bottom(), p.text});
    }
    *lines = linesOf(std::move(pieces));
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
        // A thin strip gets black above and below: the model finds its text then.
        int padding = 0;
        if (look.height() <= kMinSide || look.width() > 8 * look.height()) {
            padding = std::abs(std::max(look.width() / 8, kMinSide) * 2 - look.height()) / 2;
            QImage padded(look.width(), look.height() + 2 * padding, QImage::Format_RGB888);
            padded.fill(Qt::black);
            {
                QPainter painter(&padded);
                painter.drawImage(0, padding, look);
            }
            look = padded;
        }
        // Strip by strip; a box belongs to the strip its middle is in, away
        // from where it overlaps the next.
        std::vector<Quad> boxes;
        const int height = look.height();
        for (int top = 0;;) {
            const int strip = std::min(kStrip, height - top);
            const bool last = top + strip >= height;
            const double from = top == 0 ? -1 : top + kOverlap / 2.0;
            const double to = last ? height + 1.0 : top + strip - kOverlap / 2.0;
            for (Quad quad : d->detect(strip == height ? look : look.copy(0, top, look.width(), strip))) {
                const double middle = top + (quad[0].y() + quad[2].y()) / 2;
                if (middle < from || middle >= to)
                    continue;
                for (QPointF& p : quad) // to the picture's own pixels
                    p = QPointF(std::clamp(p.x() / scale, 0.0, static_cast<double>(picture.width())),
                        std::clamp((p.y() + top - padding) / scale, 0.0, static_cast<double>(picture.height())));
                boxes.push_back(quad);
            }
            if (options.progress)
                options.progress();
            if (last)
                break;
            top += strip - kOverlap;
        }
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
        Reading read;
        std::vector<QList<QPair<int, float>>> places;
        d->recognize(cuts, &read, chars ? &places : nullptr, options.progress);
        if (options.retryUpsideDown && cuts.size() >= 2 && readsPoorly(read)) {
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
