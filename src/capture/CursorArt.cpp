#include "CursorArt.h"

#include <QFile>
#include <QString>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace ws::capture::recording {

namespace {

constexpr qint64 kSecond = 10'000'000;

std::atomic<quint64> g_pictures {0};

// The standard pointers and their names under [HKCU\Control Panel\Cursors],
// where the pointer scheme keeps each one's file.
struct Standard {
    WORD id;
    const wchar_t* name;
};
constexpr Standard kStandard[] = {
    {32512, L"Arrow"}, {32513, L"IBeam"}, {32514, L"Wait"}, {32515, L"Crosshair"}, {32516, L"UpArrow"},
    {32642, L"SizeNWSE"}, {32643, L"SizeNESW"}, {32644, L"SizeWE"}, {32645, L"SizeNS"}, {32646, L"SizeAll"},
    {32648, L"No"}, {32649, L"Hand"}, {32650, L"AppStarting"}, {32651, L"Help"}, {32671, L"Pin"}, {32672, L"Person"},
};

// user32's, undocumented but exported since Windows XP: an animated
// pointer's frame and its time in jiffies (1/60 s).
using FrameInfo = HCURSOR(WINAPI*)(HCURSOR, DWORD, DWORD, DWORD*, DWORD*);

FrameInfo frameInfo()
{
    static const auto function = reinterpret_cast<FrameInfo>(
        reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetCursorFrameInfo")));
    return function;
}

QImage bitmapPixels(HBITMAP bitmap)
{
    BITMAP info {};
    if (!bitmap || !::GetObjectW(bitmap, sizeof info, &info) || info.bmWidth <= 0 || info.bmHeight <= 0)
        return {};
    QImage image(info.bmWidth, info.bmHeight, QImage::Format_ARGB32);
    BITMAPINFO header {};
    header.bmiHeader.biSize = sizeof header.bmiHeader;
    header.bmiHeader.biWidth = info.bmWidth;
    header.bmiHeader.biHeight = -info.bmHeight; // top row first
    header.bmiHeader.biPlanes = 1;
    header.bmiHeader.biBitCount = 32;
    header.bmiHeader.biCompression = BI_RGB;
    HDC screen = ::GetDC(nullptr);
    const int lines = ::GetDIBits(screen, bitmap, 0, UINT(info.bmHeight), image.bits(), &header, DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, screen);
    return lines == info.bmHeight ? image : QImage();
}

CursorPicture pictureOf(HCURSOR cursor)
{
    ICONINFO info {};
    if (!cursor || !::GetIconInfo(cursor, &info))
        return {};
    const QImage color = bitmapPixels(info.hbmColor);
    const QImage mask = bitmapPixels(info.hbmMask);
    if (info.hbmColor) ::DeleteObject(info.hbmColor);
    if (info.hbmMask) ::DeleteObject(info.hbmMask);
    return cursorPicture(color, mask, QPoint(int(info.xHotspot), int(info.yHotspot)));
}

// A picture read at another size than Windows shows it: made larger or
// smaller smoothly, the hot spot with it.
CursorPicture resized(const CursorPicture& picture, int size)
{
    if (picture.image.isNull() || picture.image.width() == size || size <= 0)
        return picture;
    const double factor = double(size) / picture.image.width();
    const QSize target(std::max(1, int(std::lround(picture.image.width() * factor))),
        std::max(1, int(std::lround(picture.image.height() * factor))));
    CursorPicture out;
    out.image = picture.image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (!picture.invert.isNull())
        out.invert = picture.invert.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    out.hotSpot = QPoint(int(std::lround(picture.hotSpot.x() * factor)), int(std::lround(picture.hotSpot.y() * factor)));
    out.id = ++g_pictures;
    return out;
}

QString schemeFile(const wchar_t* name)
{
    wchar_t raw[MAX_PATH] {};
    DWORD bytes = sizeof raw;
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
            nullptr, raw, &bytes) != ERROR_SUCCESS || !raw[0])
        return {};
    wchar_t path[MAX_PATH] {};
    const DWORD length = ::ExpandEnvironmentStringsW(raw, path, MAX_PATH);
    return length && length <= MAX_PATH ? QString::fromWCharArray(path) : QString();
}

// Little-endian numbers in a file read whole; 0 past its end.
quint32 word(QByteArrayView bytes, qsizetype at)
{
    return at >= 0 && at + 2 <= bytes.size() ? quint32(uchar(bytes[at])) | quint32(uchar(bytes[at + 1])) << 8 : 0;
}

quint32 dword(QByteArrayView bytes, qsizetype at)
{
    return at >= 0 && at + 4 <= bytes.size() ? word(bytes, at) | word(bytes, at + 2) << 16 : 0;
}

bool tagged(QByteArrayView bytes, qsizetype at, const char (&tag)[5])
{
    return at >= 0 && at + 4 <= bytes.size() && std::memcmp(bytes.data() + at, tag, 4) == 0;
}

// A .cur (or .ico) file's picture for a size: the one made for it, else the
// smallest larger one, else the largest, made that size.
CursorPicture filePicture(QByteArrayView file, int size)
{
    const quint32 type = word(file, 2), count = word(file, 4);
    if (word(file, 0) != 0 || (type != 1 && type != 2) || !count || file.size() < 6 + 16 * qsizetype(count))
        return {};
    qsizetype best = -1;
    int bestWidth = 0;
    for (quint32 i = 0; i < count; ++i) {
        const qsizetype entry = 6 + 16 * qsizetype(i);
        const int width = uchar(file[entry]) ? uchar(file[entry]) : 256;
        const bool better = best < 0
            || (width >= size ? bestWidth < size || width < bestWidth : bestWidth < size && width > bestWidth);
        if (better) {
            best = entry;
            bestWidth = width;
        }
    }
    const quint32 bytes = dword(file, best + 8), offset = dword(file, best + 12);
    if (!bytes || offset > quint32(file.size()) || bytes > quint32(file.size()) - offset)
        return {};
    // As a cursor resource: the hot spot (an icon's is its middle), then
    // the picture as the file has it, DIB or PNG, read at its own size.
    const quint16 hot[2] = {quint16(type == 2 ? word(file, best + 4) : bestWidth / 2),
        quint16(type == 2 ? word(file, best + 6) : bestWidth / 2)};
    QByteArray resource(sizeof hot + qsizetype(bytes), Qt::Uninitialized);
    std::memcpy(resource.data(), hot, sizeof hot);
    std::memcpy(resource.data() + sizeof hot, file.data() + offset, bytes);
    const HICON made = ::CreateIconFromResourceEx(reinterpret_cast<PBYTE>(resource.data()), DWORD(resource.size()), FALSE,
        0x00030000, 0, 0, LR_DEFAULTCOLOR);
    if (!made)
        return {};
    const CursorPicture picture = pictureOf(made);
    ::DestroyCursor(made);
    return resized(picture, size);
}

} // namespace

CursorFrames cursorFileFrames(QByteArrayView file, int size)
{
    CursorFrames out;
    if (!tagged(file, 0, "RIFF")) {
        CursorPicture picture = filePicture(file, size);
        if (!picture.image.isNull()) {
            out.pictures.push_back(std::move(picture));
            out.ends.push_back(kSecond);
        }
        return out;
    }
    // RIFF "ACON": "anih" (frames, steps, jiffies a step, flags), "rate" and
    // "seq " (each step's jiffies and frame) and the frames, one "icon"
    // chunk each, inside a LIST "fram".
    if (!tagged(file, 8, "ACON"))
        return out;
    quint32 frames = 0, steps = 0, rate = 0, flags = 0;
    QByteArrayView rates, order;
    std::vector<QByteArrayView> icons;
    const auto chunks = [&file](qsizetype at, qsizetype end, const auto& visit) {
        while (at + 8 <= end) {
            const qsizetype body = at + 8, length = dword(file, at + 4);
            if (length > end - body)
                return;
            visit(at, body, length);
            at = body + length + (length & 1);
        }
    };
    chunks(12, file.size(), [&](qsizetype at, qsizetype body, qsizetype length) {
        if (tagged(file, at, "anih") && length >= 36) {
            frames = dword(file, body + 4);
            steps = dword(file, body + 8);
            rate = dword(file, body + 28);
            flags = dword(file, body + 32);
        } else if (tagged(file, at, "rate")) {
            rates = file.sliced(body, length);
        } else if (tagged(file, at, "seq ")) {
            order = file.sliced(body, length);
        } else if (tagged(file, at, "LIST") && tagged(file, body, "fram")) {
            chunks(body + 4, body + length, [&](qsizetype in, qsizetype data, qsizetype bytes) {
                if (tagged(file, in, "icon"))
                    icons.push_back(file.sliced(data, bytes));
            });
        }
    });
    const quint32 count = steps ? steps : frames;
    if (!(flags & 1) || icons.empty() || !count || count > 256) // without AF_ICON, raw bitmaps: not read here
        return out;
    std::vector<CursorPicture> made(icons.size());
    qint64 jiffiesSoFar = 0;
    for (quint32 step = 0; step < count; ++step) {
        const quint32 frame = order.size() >= 4 * qsizetype(step + 1) ? dword(order, 4 * qsizetype(step)) : step;
        if (frame >= icons.size())
            return {};
        if (made[frame].image.isNull() && (made[frame] = filePicture(icons[frame], size)).image.isNull())
            return {};
        const quint32 jiffies = rates.size() >= 4 * qsizetype(step + 1) ? dword(rates, 4 * qsizetype(step)) : rate;
        jiffiesSoFar += std::clamp<quint32>(jiffies, 1, 600);
        out.pictures.push_back(made[frame]); // the same frame keeps its id: one GPU copy
        out.ends.push_back(jiffiesSoFar * kSecond / 60);
    }
    return out;
}

namespace {

// The scheme's file for a standard pointer, read at a size.
CursorFrames schemeFrames(const QString& path, int size)
{
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024)
        return {};
    return cursorFileFrames(file.readAll(), size);
}

} // namespace

CursorPicture cursorPicture(const QImage& color, const QImage& mask, QPoint hotSpot)
{
    CursorPicture picture;
    picture.hotSpot = hotSpot;
    picture.id = ++g_pictures;
    const bool monochrome = color.isNull();
    const QSize size = monochrome ? QSize(mask.width(), mask.height() / 2) : color.size();
    if (size.isEmpty())
        return picture;
    if (!monochrome) {
        const QImage straight = color.convertToFormat(QImage::Format_ARGB32);
        bool alpha = false;
        for (int y = 0; y < straight.height() && !alpha; ++y) {
            const auto* row = reinterpret_cast<const QRgb*>(straight.constScanLine(y));
            alpha = std::any_of(row, row + straight.width(), [](QRgb pixel) { return qAlpha(pixel) != 0; });
        }
        if (alpha) { // Windows draws by the alpha and leaves the mask aside
            picture.image = straight.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            return picture;
        }
    }
    const QImage bits = mask.convertToFormat(QImage::Format_ARGB32);
    const QImage colors = monochrome ? QImage() : color.convertToFormat(QImage::Format_ARGB32);
    const bool masked = bits.width() >= size.width() && bits.height() >= size.height() * (monochrome ? 2 : 1);
    const auto on = [&bits](int x, int y) { return (reinterpret_cast<const QRgb*>(bits.constScanLine(y))[x] & 0xffffffu) != 0; };
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(0);
    QImage invert(size, QImage::Format_Alpha8);
    invert.fill(0);
    bool inverts = false;
    for (int y = 0; y < size.height(); ++y) {
        auto* out = reinterpret_cast<QRgb*>(image.scanLine(y));
        uchar* flip = invert.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const bool keep = masked && on(x, y); // AND 1: the screen stays, then the XOR
            const QRgb ink = monochrome ? (masked && on(x, y + size.height()) ? 0xffffffu : 0u)
                                        : reinterpret_cast<const QRgb*>(colors.constScanLine(y))[x] & 0xffffffu;
            if (!keep) {
                out[x] = 0xff000000u | ink;
            } else if (ink) {
                flip[x] = 255;
                inverts = true;
            }
        }
    }
    picture.image = image;
    if (inverts)
        picture.invert = invert;
    return picture;
}

int systemCursorSize(double scale)
{
    DWORD base = 0;
    DWORD bytes = sizeof base;
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"CursorBaseSize", RRF_RT_REG_DWORD, nullptr, &base, &bytes)
            != ERROR_SUCCESS || base < 16 || base > 512)
        base = 32;
    return std::max(1, int(std::lround(base * std::clamp(scale, 0.5, 8.0))));
}

struct CursorShapes::Impl {
    struct Entry {
        std::vector<CursorPicture> frames;
        std::vector<qint64> ends; // when each frame ends, from the first one's start
        qint64 since = -1;
    };
    std::unordered_map<HCURSOR, Entry> entries;

    Entry make(HCURSOR cursor, int size) const
    {
        Entry entry;
        const wchar_t* standard = nullptr;
        for (const auto& known : kStandard) {
            if (::LoadCursorW(nullptr, MAKEINTRESOURCEW(known.id)) == cursor) {
                standard = known.name;
                break;
            }
        }
        HCURSOR source = cursor;
        HCURSOR loaded = nullptr;
        if (standard) {
            const QString file = schemeFile(standard);
            CursorFrames read = schemeFrames(file, size);
            if (!read.pictures.empty()) {
                entry.frames = std::move(read.pictures);
                entry.ends = std::move(read.ends);
                return entry;
            }
            if (!file.isEmpty()) // a file only Windows reads: at the size, drawn again
                loaded = static_cast<HCURSOR>(::LoadImageW(nullptr, reinterpret_cast<const wchar_t*>(file.utf16()), IMAGE_CURSOR,
                    size, size, LR_LOADFROMFILE));
            if (loaded)
                source = loaded;
        }
        DWORD rate = 0, steps = 0;
        const FrameInfo frames = frameInfo();
        if (frames && frames(source, 0, 0, &rate, &steps) && steps > 1 && steps <= 256) {
            qint64 jiffiesSoFar = 0;
            for (DWORD step = 0; step < steps; ++step) {
                DWORD jiffies = 0, count = 0;
                CursorPicture picture = pictureOf(frames(source, 0, step, &jiffies, &count));
                if (picture.image.isNull())
                    break;
                jiffiesSoFar += std::clamp<DWORD>(jiffies, 1, 600);
                entry.frames.push_back(std::move(picture));
                entry.ends.push_back(jiffiesSoFar * kSecond / 60);
            }
        }
        if (entry.frames.size() < 2) {
            entry.frames.clear();
            entry.ends.clear();
            CursorPicture picture = pictureOf(source);
            if (!picture.image.isNull()) {
                entry.frames.push_back(std::move(picture));
                entry.ends.push_back(kSecond);
            }
        }
        // A standard pointer without its file (the scheme leaves it to
        // Windows' own) at the size it shows; the programs' own, as made.
        if (standard && !loaded)
            for (auto& picture : entry.frames)
                picture = resized(picture, size);
        if (loaded)
            ::DestroyCursor(loaded);
        return entry;
    }
};

CursorShapes::CursorShapes(double scale) : m_impl(std::make_unique<Impl>()), m_size(systemCursorSize(scale)) { }
CursorShapes::~CursorShapes() = default;

const CursorPicture* CursorShapes::picture(void* cursor, qint64 time)
{
    const auto handle = static_cast<HCURSOR>(cursor);
    if (!handle)
        return nullptr;
    auto found = m_impl->entries.find(handle);
    if (found == m_impl->entries.end()) {
        if (m_impl->entries.size() >= 64)
            m_impl->entries.clear(); // programs that make a new pointer each time
        found = m_impl->entries.emplace(handle, m_impl->make(handle, m_size)).first;
    }
    auto& entry = found->second;
    if (entry.frames.empty())
        return nullptr;
    if (entry.frames.size() == 1)
        return &entry.frames.front();
    if (entry.since < 0)
        entry.since = time;
    const qint64 at = std::max<qint64>(0, time - entry.since) % entry.ends.back();
    const size_t frame = size_t(std::upper_bound(entry.ends.begin(), entry.ends.end(), at) - entry.ends.begin());
    return &entry.frames[std::min(frame, entry.frames.size() - 1)];
}

const CursorPicture* CursorShapes::arrow()
{
    return picture(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)), 0);
}

} // namespace ws::capture::recording
