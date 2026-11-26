#include "FormulaReader.h"
#include "OcrFiles.h"
#include "OnnxRuntime.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr int kSide = 384; // what the encoder takes
constexpr int64_t kStart = 1; // <s>, the decoder's first token
constexpr int64_t kEnd = 2; // </s>
constexpr int64_t kLastSpecial = 4; // <pad> <s> </s> <unk> <mask>: no text
// Longer is not a formula one picture holds; a formula of a screenshot is
// 20 to 120 tokens.
constexpr int kMaxTokens = 400;
// The model's way of failing: the same few tokens over and over (a column
// of \\, "0 &" ...). This many times at least, period by period; once is kept.
constexpr int kLongestPeriod = 12;
constexpr int repeats(int period) { return std::max(6, (24 + period - 1) / period); }

// A pixel's grey as Pillow's convert("L") has it.
uchar grey(const uchar* rgb)
{
    return static_cast<uchar>((rgb[0] * 19595 + rgb[1] * 38470 + rgb[2] * 7471 + 0x8000) >> 16);
}

// The middle of the edges' greys (corners twice), as numpy's median: the
// ground the formula stands on.
double borderMedian(const std::vector<uchar>& greys, int width, int height)
{
    std::vector<uchar> edge;
    edge.reserve(2 * (static_cast<size_t>(width) + height));
    for (int x = 0; x < width; ++x) {
        edge.push_back(greys[x]);
        edge.push_back(greys[static_cast<size_t>(height - 1) * width + x]);
    }
    for (int y = 0; y < height; ++y) {
        edge.push_back(greys[static_cast<size_t>(y) * width]);
        edge.push_back(greys[static_cast<size_t>(y) * width + width - 1]);
    }
    const size_t middle = edge.size() / 2;
    std::ranges::nth_element(edge, edge.begin() + middle);
    if (edge.size() % 2)
        return edge[middle];
    const uchar above = edge[middle];
    const uchar below = *std::max_element(edge.begin(), edge.begin() + middle);
    return (above + below) / 2.0;
}

// Pillow's resampling (Resample.c) for 8-bit pixels, the bicubic filter:
// weights over the source's pixels for each pixel made, in fixed point.
constexpr int kPrecision = 32 - 8 - 2;

double bicubic(double x)
{
    constexpr double a = -0.5;
    x = std::abs(x);
    if (x < 1)
        return ((a + 2) * x - (a + 3)) * x * x + 1;
    if (x < 2)
        return (((x - 5) * x + 8) * x - 4) * a;
    return 0;
}

struct Weights {
    int size = 0; // per pixel made
    std::vector<int> first; // the source's first pixel for each
    std::vector<int> count;
    std::vector<int32_t> weights;
};

Weights weights(int from, int to)
{
    const double scale = static_cast<double>(from) / to;
    const double filterScale = std::max(1.0, scale);
    const double support = 2 * filterScale;
    Weights w;
    w.size = static_cast<int>(std::ceil(support)) * 2 + 1;
    w.first.resize(to);
    w.count.resize(to);
    w.weights.assign(static_cast<size_t>(to) * w.size, 0);
    std::vector<double> k(w.size);
    for (int i = 0; i < to; ++i) {
        const double center = (i + 0.5) * scale;
        const int lo = std::max(0, static_cast<int>(center - support + 0.5));
        const int hi = std::min(from, static_cast<int>(center + support + 0.5));
        const int n = hi - lo;
        double sum = 0;
        for (int x = 0; x < n; ++x) {
            k[x] = bicubic((x + lo - center + 0.5) / filterScale);
            sum += k[x];
        }
        for (int x = 0; x < n; ++x) {
            const double v = sum != 0 ? k[x] / sum : k[x];
            w.weights[static_cast<size_t>(i) * w.size + x] = static_cast<int32_t>(v < 0 ? -0.5 + v * (1 << kPrecision) : 0.5 + v * (1 << kPrecision));
        }
        w.first[i] = lo;
        w.count[i] = n;
    }
    return w;
}

uchar clip(int32_t v)
{
    return static_cast<uchar>(std::clamp(v >> kPrecision, 0, 255));
}

// Across, then down, a byte between, as Pillow does it.
QImage resize(const QImage& image, int width, int height)
{
    QImage across = image;
    if (image.width() != width) {
        const Weights w = weights(image.width(), width);
        across = QImage(width, image.height(), QImage::Format_RGB888);
        for (int y = 0; y < image.height(); ++y) {
            const uchar* in = image.constScanLine(y);
            uchar* out = across.scanLine(y);
            for (int x = 0; x < width; ++x) {
                const int32_t* k = &w.weights[static_cast<size_t>(x) * w.size];
                for (int c = 0; c < 3; ++c) {
                    int32_t sum = 1 << (kPrecision - 1);
                    for (int i = 0; i < w.count[x]; ++i)
                        sum += in[(w.first[x] + i) * 3 + c] * k[i];
                    out[x * 3 + c] = clip(sum);
                }
            }
        }
    }
    if (across.height() == height)
        return across;
    const Weights w = weights(across.height(), height);
    QImage down(width, height, QImage::Format_RGB888);
    for (int y = 0; y < height; ++y) {
        const int32_t* k = &w.weights[static_cast<size_t>(y) * w.size];
        uchar* out = down.scanLine(y);
        for (int x = 0; x < width * 3; ++x) {
            int32_t sum = 1 << (kPrecision - 1);
            for (int i = 0; i < w.count[y]; ++i)
                sum += across.constScanLine(w.first[y] + i)[x] * k[i];
            out[x] = clip(sum);
        }
    }
    return down;
}

// GPT-2's bytes as characters (the tokenizer's ByteLevel): the printable
// ones as themselves, the rest from U+0100 on, in order.
std::array<int, 0x144> byteOfCharacter()
{
    std::array<int, 0x144> bytes;
    bytes.fill(-1);
    int next = 0x100;
    for (int b = 0; b < 256; ++b) {
        const bool printable = (b >= 0x21 && b <= 0x7E) || (b >= 0xA1 && b <= 0xAC) || (b >= 0xAE && b <= 0xFF);
        bytes[printable ? b : next++] = b;
    }
    return bytes;
}

// The tokens' bytes by their numbers, from tokenizer.json's vocabulary.
std::vector<QByteArray> readTokens(const QString& file)
{
    QFile json(file);
    if (!json.open(QIODevice::ReadOnly))
        throw std::runtime_error("tokenizer.json did not open");
    const QJsonObject vocabulary = QJsonDocument::fromJson(json.readAll()).object()
        .value(u"model"_s).toObject().value(u"vocab"_s).toObject();
    if (vocabulary.size() < 100)
        throw std::runtime_error("tokenizer.json has no vocabulary");
    static const std::array<int, 0x144> bytes = byteOfCharacter();
    std::vector<QByteArray> tokens(vocabulary.size());
    for (auto it = vocabulary.begin(); it != vocabulary.end(); ++it) {
        const qint64 id = it.value().toInteger(-1);
        if (id < 0 || id >= static_cast<qint64>(tokens.size()))
            throw std::runtime_error("tokenizer.json has a token out of its range");
        if (id <= kLastSpecial)
            continue;
        QByteArray& token = tokens[static_cast<size_t>(id)];
        for (const char32_t c : it.key().toUcs4()) {
            if (c >= bytes.size() || bytes[c] < 0)
                throw std::runtime_error("tokenizer.json has a token that is not bytes");
            token.append(static_cast<char>(bytes[c]));
        }
    }
    return tokens;
}

// The end of `ids` the same `period` tokens repeats(period) times or more.
bool goesRound(const std::vector<int64_t>& ids, size_t start, int period)
{
    const size_t span = static_cast<size_t>(period) * repeats(period);
    if (ids.size() - start < span)
        return false;
    const auto end = ids.end();
    return std::equal(end - span + period, end, end - span);
}

} // namespace

struct FormulaReader::Impl {
    Ort::Env env {nullptr};
    Ort::Session encoder {nullptr};
    Ort::Session decoder {nullptr};
    std::string encoderIn;
    std::string encoderOut;
    std::string tokensIn; // input_ids
    std::string hiddenIn; // encoder_hidden_states
    std::string logitsOut;
    std::vector<QByteArray> tokens;
};

FormulaReader::FormulaReader() = default;
FormulaReader::~FormulaReader() = default;

std::unique_ptr<FormulaReader> FormulaReader::load(
    const QString& runtimeFolder, const QString& modelFolder, int threads, QString* error)
{
    if (runtimeFolder.isEmpty() || modelFolder.isEmpty()) {
        if (error)
            *error = runtimeFolder.isEmpty() ? u"the text recognition files are missing"_s : u"the formula model is missing"_s;
        return nullptr;
    }
    if (!onnx::load(runtimeFolder, error))
        return nullptr;
    std::unique_ptr<FormulaReader> reader(new FormulaReader);
    try {
        auto impl = std::make_unique<Impl>();
        const QDir folder(modelFolder);
        impl->tokens = readTokens(folder.filePath(QString(ocr::kFormulaFiles[2])));
        impl->env = onnx::environment(threads);
        const Ort::SessionOptions options = onnx::sessionOptions();
        impl->encoder = onnx::session(impl->env, folder.filePath(QString(ocr::kFormulaFiles[0])), options);
        impl->decoder = onnx::session(impl->env, folder.filePath(QString(ocr::kFormulaFiles[1])), options);
        Ort::AllocatorWithDefaultOptions allocator;
        impl->encoderIn = impl->encoder.GetInputNameAllocated(0, allocator).get();
        impl->encoderOut = impl->encoder.GetOutputNameAllocated(0, allocator).get();
        const size_t decoderInputs = impl->decoder.GetInputCount();
        for (size_t i = 0; i < decoderInputs; ++i) {
            std::string name = impl->decoder.GetInputNameAllocated(i, allocator).get();
            if (name == "input_ids")
                impl->tokensIn = std::move(name);
            else if (name == "encoder_hidden_states")
                impl->hiddenIn = std::move(name);
        }
        if (decoderInputs != 2 || impl->tokensIn.empty() || impl->hiddenIn.empty())
            throw std::runtime_error("the decoder does not take input_ids and encoder_hidden_states alone");
        impl->logitsOut = impl->decoder.GetOutputNameAllocated(0, allocator).get();
        reader->d = std::move(impl);
    } catch (const std::exception& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return nullptr;
    }
    return reader;
}

QImage FormulaReader::prepare(const QImage& picture)
{
    if (picture.isNull())
        return {};
    // Over white where it lets the ground through (a pin of a transparent PNG).
    QImage rgb(picture.size(), QImage::Format_RGB888);
    if (picture.hasAlphaChannel()) {
        rgb.fill(Qt::white);
        QPainter painter(&rgb);
        painter.drawImage(0, 0, picture);
    } else {
        rgb = picture.convertToFormat(QImage::Format_RGB888);
    }
    const int width = rgb.width();
    const int height = rgb.height();
    std::vector<uchar> greys(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        const uchar* line = rgb.constScanLine(y);
        for (int x = 0; x < width; ++x)
            greys[static_cast<size_t>(y) * width + x] = grey(line + x * 3);
    }
    // Light on dark: dark on light, as it learned.
    double ground = borderMedian(greys, width, height);
    if (ground < 128) {
        rgb.invertPixels();
        for (uchar& g : greys)
            g = 255 - g;
        ground = 255 - ground;
    }
    int left = width, top = height, right = -1, bottom = -1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (std::abs(greys[static_cast<size_t>(y) * width + x] - ground) > 40) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    if (right < 0)
        return {};
    // A third of its height around it: the margin the model is surest with
    // (405 of 480; an eighth 398, the picture as it was 391).
    const int cutWidth = right - left + 1;
    const int cutHeight = bottom - top + 1;
    const int margin = std::max(4, static_cast<int>(std::nearbyint(cutHeight * 0.3))); // half to even, as Python's round
    QImage framed(cutWidth + 2 * margin, cutHeight + 2 * margin, QImage::Format_RGB888);
    framed.fill(ground > 128 ? Qt::white : Qt::black);
    for (int y = 0; y < cutHeight; ++y)
        std::memcpy(framed.scanLine(margin + y) + margin * 3, rgb.constScanLine(top + y) + left * 3, static_cast<size_t>(cutWidth) * 3);
    return resize(framed, kSide, kSide);
}

std::optional<FormulaReader::Reading> FormulaReader::read(const QImage& picture, QString* error, const std::atomic_bool* cancelled)
{
    const auto stopped = [&] { return cancelled && cancelled->load(std::memory_order_relaxed); };
    const QImage seen = prepare(picture);
    if (seen.isNull())
        return Reading {};
    try {
        // -1…1 by plane, red, green, blue.
        std::vector<float> pixels(3 * kSide * kSide);
        for (int y = 0; y < kSide; ++y) {
            const uchar* line = seen.constScanLine(y);
            for (int x = 0; x < kSide; ++x)
                for (int c = 0; c < 3; ++c)
                    pixels[(static_cast<size_t>(c) * kSide + y) * kSide + x] = line[x * 3 + c] / 127.5f - 1.0f;
        }
        const Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<int64_t, 4> pictureShape {1, 3, kSide, kSide};
        const Ort::Value pictureIn = Ort::Value::CreateTensor<float>(memory, pixels.data(), pixels.size(), pictureShape.data(), pictureShape.size());
        const char* encoderIn = d->encoderIn.c_str();
        const char* encoderOut = d->encoderOut.c_str();
        std::vector<Ort::Value> encoded = d->encoder.Run(Ort::RunOptions {nullptr}, &encoderIn, &pictureIn, 1, &encoderOut, 1);

        // A token at a time, the likeliest, each from all before it (the
        // model has no cache of them).
        std::array<Ort::Value, 2> inputs {Ort::Value {nullptr}, std::move(encoded.front())};
        const std::array<const char*, 2> inputNames {d->tokensIn.c_str(), d->hiddenIn.c_str()};
        const char* logitsOut = d->logitsOut.c_str();
        std::vector<int64_t> ids {kStart};
        double logSum = 0;
        int steps = 0;
        bool cut = true;
        while (static_cast<int>(ids.size()) <= kMaxTokens) {
            if (stopped()) {
                if (error)
                    *error = u"cancelled"_s;
                return std::nullopt;
            }
            const std::array<int64_t, 2> shape {1, static_cast<int64_t>(ids.size())};
            inputs[0] = Ort::Value::CreateTensor<int64_t>(memory, ids.data(), ids.size(), shape.data(), shape.size());
            const std::vector<Ort::Value> out = d->decoder.Run(Ort::RunOptions {nullptr}, inputNames.data(), inputs.data(), inputs.size(), &logitsOut, 1);
            const std::vector<int64_t> outShape = out.front().GetTensorTypeAndShapeInfo().GetShape();
            const int64_t words = outShape.back();
            const float* logits = out.front().GetTensorData<float>() + (static_cast<int64_t>(ids.size()) - 1) * words;
            const float* best = std::max_element(logits, logits + words);
            double sum = 0;
            for (int64_t i = 0; i < words; ++i)
                sum += std::exp(static_cast<double>(logits[i] - *best));
            logSum -= std::log(sum); // log of the best one's probability
            ++steps;
            const int64_t next = best - logits;
            if (next == kEnd) {
                cut = false;
                break;
            }
            ids.push_back(next);
            int period = 0;
            for (int p = 1; p <= kLongestPeriod && !period; ++p)
                period = goesRound(ids, 1, p) ? p : 0;
            if (period) {
                ids.resize(ids.size() - static_cast<size_t>(period) * (repeats(period) - 1));
                break;
            }
        }
        QByteArray text;
        for (size_t i = 1; i < ids.size(); ++i)
            if (ids[i] > kLastSpecial && ids[i] < static_cast<int64_t>(d->tokens.size()))
                text += d->tokens[static_cast<size_t>(ids[i])];
        return Reading {QString::fromUtf8(text).trimmed(), std::exp(logSum / std::max(1, steps)), cut};
    } catch (const std::exception& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return std::nullopt;
    }
}

} // namespace ws
