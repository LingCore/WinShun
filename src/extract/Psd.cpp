// Photoshop's .psd and .psb: the flattened picture every such file ends with
// (what Photoshop itself shows before the layers are read; white where a
// file saved without "maximize compatibility" has none). Planes one after
// another, raw or PackBits by row; scaled down as it is read, a box of
// pixels to each one of the result, so a poster of 20 000 pixels takes the
// memory of what is shown.

#include "Render.h"

#include <algorithm>
#include <vector>

namespace ws::extract {

namespace {

constexpr std::uint32_t kMaxSide = 300000; // Photoshop's own limit (PSB)
constexpr std::size_t kMaxRowBytes = 2u * kMaxSide + 1024; // a packed row at its worst

struct Reader {
    Source& source;
    std::uint64_t at = 0;
    bool ok = true;

    bool bytes(void* to, std::size_t n)
    {
        if (!ok || !source.readExact(at, to, n))
            return ok = false;
        at += n;
        return true;
    }
    std::uint64_t big(int size)
    {
        unsigned char b[8] {};
        if (!bytes(b, static_cast<std::size_t>(size)))
            return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < size; ++i)
            v = (v << 8) | b[i];
        return v;
    }
    void skip(std::uint64_t n)
    {
        if (n > source.size() || at + n > source.size())
            ok = false;
        else
            at += n;
    }
};

// One PackBits row into `to` (exactly its size, or false).
bool unpack(const unsigned char* from, std::size_t n, std::vector<unsigned char>& to)
{
    std::size_t out = 0;
    std::size_t i = 0;
    while (i < n && out < to.size()) {
        const auto count = static_cast<signed char>(from[i++]);
        if (count >= 0) {
            const std::size_t run = static_cast<std::size_t>(count) + 1;
            if (i + run > n || out + run > to.size())
                return false;
            std::copy_n(from + i, run, to.data() + out);
            i += run;
            out += run;
        } else if (count != -128) {
            const std::size_t run = static_cast<std::size_t>(-count) + 1;
            if (i >= n || out + run > to.size())
                return false;
            std::fill_n(to.data() + out, run, from[i++]);
            out += run;
        }
    }
    return out == to.size();
}

} // namespace

Status renderPsd(Source& source, std::uint32_t largest, Picture& out)
{
    Reader in {source};
    char magic[4];
    if (!in.bytes(magic, 4) || std::string_view(magic, 4) != "8BPS")
        return Status::Unsupported;
    const auto version = static_cast<int>(in.big(2));
    in.skip(6);
    const auto channels = static_cast<std::uint32_t>(in.big(2));
    const auto height = static_cast<std::uint32_t>(in.big(4));
    const auto width = static_cast<std::uint32_t>(in.big(4));
    const auto depth = static_cast<int>(in.big(2));
    const auto mode = static_cast<int>(in.big(2));
    if (!in.ok || (version != 1 && version != 2) || channels < 1 || channels > 56 || width < 1 || height < 1
        || width > kMaxSide || height > kMaxSide)
        return Status::Broken;
    // Bitmap 0, grey 1, indexed 2, RGB 3, CMYK 4, multichannel 7, duotone 8
    // (its grey plate), Lab 9.
    enum { Grey = 1, Indexed = 2, Rgb = 3, Cmyk = 4, Duotone = 8 };
    const std::uint32_t planes = mode == Rgb ? 3 : mode == Cmyk ? 4 : 1;
    if ((depth != 8 && depth != 16) || (mode != Grey && mode != Indexed && mode != Rgb && mode != Cmyk && mode != Duotone)
        || channels < planes)
        return Status::Unsupported;

    std::vector<unsigned char> palette; // indexed: 256 reds, greens, blues
    const std::uint64_t colorBytes = in.big(4);
    if (mode == Indexed && colorBytes >= 768) {
        palette.resize(768);
        const std::uint64_t start = in.at;
        in.bytes(palette.data(), 768);
        in.at = start;
    }
    in.skip(colorBytes);
    in.skip(in.big(4)); // image resources
    in.skip(in.big(version == 2 ? 8 : 4)); // layers and masks
    const auto compression = static_cast<int>(in.big(2));
    if (!in.ok)
        return Status::Broken;
    if (compression != 0 && compression != 1)
        return Status::Unsupported; // zip: not written for the flattened picture
    if (mode == Indexed && palette.empty())
        return Status::Broken;

    const std::uint32_t sample = depth / 8;
    const std::size_t rowBytes = static_cast<std::size_t>(width) * sample;
    // Where each row of each plane is (PackBits: by the table of their sizes).
    std::vector<std::uint32_t> packed;
    std::uint64_t data = in.at;
    if (compression == 1) {
        const int entry = version == 2 ? 4 : 2;
        const std::size_t rows = static_cast<std::size_t>(channels) * height;
        std::vector<unsigned char> table(rows * static_cast<std::size_t>(entry));
        if (!in.bytes(table.data(), table.size()))
            return Status::Broken;
        packed.resize(static_cast<std::size_t>(planes) * height);
        for (std::size_t i = 0; i < packed.size(); ++i) {
            std::uint32_t v = 0;
            for (int b = 0; b < entry; ++b)
                v = (v << 8) | table[i * static_cast<std::size_t>(entry) + static_cast<std::size_t>(b)];
            if (v > kMaxRowBytes)
                return Status::Broken;
            packed[i] = v;
        }
        data = in.at;
    }

    // Each `step` × `step` pixels to one of the result.
    largest = std::clamp<std::uint32_t>(largest, 64, 8192);
    const std::uint32_t step = (std::max(width, height) + largest - 1) / largest;
    const std::uint32_t outWidth = std::max<std::uint32_t>(1, width / step);
    const std::uint32_t outHeight = std::max<std::uint32_t>(1, height / step);
    // Sums by plane, then by pixel of the result.
    std::vector<std::uint32_t> sums(static_cast<std::size_t>(planes) * outWidth * outHeight, 0);
    std::vector<unsigned char> row(rowBytes);
    std::vector<unsigned char> raw;
    std::uint64_t at = data;
    for (std::uint32_t plane = 0; plane < planes; ++plane) {
        std::uint32_t* planeSums = sums.data() + static_cast<std::size_t>(plane) * outWidth * outHeight;
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint64_t rowAt = at;
            const std::size_t stored = compression == 1 ? packed[static_cast<std::size_t>(plane) * height + y] : rowBytes;
            at += stored;
            const std::uint32_t outY = y / step;
            if (outY >= outHeight)
                continue; // the rows left over below the last whole box
            if (compression == 1) {
                raw.resize(stored);
                if (!source.readExact(rowAt, raw.data(), stored) || !unpack(raw.data(), stored, row))
                    return Status::Broken;
            } else if (!source.readExact(rowAt, row.data(), rowBytes)) {
                return Status::Broken;
            }
            std::uint32_t* to = planeSums + static_cast<std::size_t>(outY) * outWidth;
            const unsigned char* from = row.data(); // 16 bits: the high byte comes first
            if (mode == Indexed) { // an average of indexes means nothing: the box's first pixel
                if (y % step == 0) {
                    for (std::uint32_t x = 0; x < outWidth; ++x)
                        to[x] = from[static_cast<std::size_t>(x) * step];
                }
                continue;
            }
            for (std::uint32_t x = 0, limit = outWidth * step; x < limit; ++x)
                to[x / step] += from[static_cast<std::size_t>(x) * sample];
        }
    }

    out.width = outWidth;
    out.height = outHeight;
    out.fullWidth = width;
    out.fullHeight = height;
    out.bgra.resize(static_cast<std::size_t>(outWidth) * outHeight * 4);
    const std::uint32_t box = mode == Indexed ? 1 : step * step;
    const std::size_t pixels = static_cast<std::size_t>(outWidth) * outHeight;
    auto* to = reinterpret_cast<unsigned char*>(out.bgra.data());
    for (std::size_t i = 0; i < pixels; ++i, to += 4) {
        const std::uint32_t a = sums[i] / box;
        std::uint32_t r = a;
        std::uint32_t g = a;
        std::uint32_t b = a;
        if (mode == Rgb) {
            g = sums[pixels + i] / box;
            b = sums[2 * pixels + i] / box;
        } else if (mode == Cmyk) { // stored as 255 - ink
            const std::uint32_t k = sums[3 * pixels + i] / box;
            r = a * k / 255;
            g = sums[pixels + i] / box * k / 255;
            b = sums[2 * pixels + i] / box * k / 255;
        } else if (mode == Indexed) {
            r = palette[a];
            g = palette[256 + a];
            b = palette[512 + a];
        }
        to[0] = static_cast<unsigned char>(b);
        to[1] = static_cast<unsigned char>(g);
        to[2] = static_cast<unsigned char>(r);
        to[3] = 255;
    }
    return Status::Ok;
}

Status render(Source& source, std::uint32_t largest, Picture& out)
{
    out = {};
    char buffer[1024];
    const std::string_view head(buffer, source.read(0, buffer, sizeof buffer));
    if (head.starts_with("8BPS"))
        return renderPsd(source, largest, out);
    if (head.find("%PDF-") != std::string_view::npos)
        return renderPdf(source, largest, out);
    return Status::Unsupported;
}

} // namespace ws::extract
