#pragma once

// Wire protocol between WinShunService.exe --capture-surface (the protected
// High-IL UIAccess Band-2 mirror process) and the ordinary Medium-IL
// winshun.capture plugin. Pure C++20 with no Qt dependency so WinShunService
// and the capture plugin share one exact parser/serializer.
//
// Every frame on the private named pipe (WinShun.CaptureSurface.<pid>.<32hex>)
// is little-endian:
//   [u16 type][u16 reserved=0][u32 payloadLength][payload bytes...]
// Lengths are checked strictly before allocating or reading.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ws::capsurface {

inline constexpr std::size_t kHeaderBytes = 8;
inline constexpr std::size_t kMaxFrameBytes = 256u * 1024u;
inline constexpr std::uint32_t kMaxMonitors = 16;
inline constexpr std::uint16_t kMaxCursorDim = 256;
inline constexpr std::uint16_t kMaxTextChars = 4096;

enum class Type : std::uint16_t {
    // Plugin -> UIAccess surface helper
    Init = 1,
    SetCursor = 2,
    SetIme = 3,
    Close = 4,

    // UIAccess surface helper -> Plugin
    Ready = 101,
    Mouse = 102,
    Key = 103,
    Ime = 104,
    Closed = 105,
};

enum class MouseKind : std::uint8_t {
    Move = 0,
    Press = 1,
    Release = 2,
    DblClick = 3,
    Wheel = 4,
    HWheel = 5,
    Leave = 6,
};

enum class MouseButton : std::uint8_t {
    None = 0,
    Left = 1,
    Right = 2,
    Middle = 3,
    X1 = 4,
    X2 = 5,
};

inline constexpr std::uint8_t kBtnLeft = 1u << 0;
inline constexpr std::uint8_t kBtnRight = 1u << 1;
inline constexpr std::uint8_t kBtnMiddle = 1u << 2;
inline constexpr std::uint8_t kBtnX1 = 1u << 3;
inline constexpr std::uint8_t kBtnX2 = 1u << 4;

inline constexpr std::uint8_t kModShift = 1u << 0;
inline constexpr std::uint8_t kModCtrl = 1u << 1;
inline constexpr std::uint8_t kModAlt = 1u << 2;

enum class CursorKind : std::uint8_t {
    Standard = 0,
    Rgba = 1,
};

enum class ImeKind : std::uint8_t {
    Preedit = 0,
    Commit = 1,
    End = 2,
};

enum class CloseReason : std::uint32_t {
    Requested = 0,
    Deactivated = 1,
    FullScreenGone = 2,
    Error = 3,
};

// Strict validation for the private pipe name so neither the service nor the
// session helper can be steered to an arbitrary pipe path.
inline bool isValidPipeName(std::wstring_view pipe, std::uint32_t pluginPid)
{
    if (!pluginPid)
        return false;
    const std::wstring prefix = L"WinShun.CaptureSurface." + std::to_wstring(pluginPid) + L".";
    if (pipe.size() != prefix.size() + 32 || !pipe.starts_with(prefix))
        return false;
    return std::all_of(pipe.begin() + prefix.size(), pipe.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
    });
}

struct MonitorSpec {
    std::uint64_t overlayHwnd = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;
    bool operator==(const MonitorSpec&) const = default;
};

struct InitMsg {
    std::uint32_t activeIndex = 0;
    std::vector<MonitorSpec> monitors;
    bool operator==(const InitMsg&) const = default;
};

struct ReadyMsg {
    std::uint32_t ok = 0;
    std::uint32_t error = 0;
    std::vector<std::uint64_t> surfaceHwnds;
    bool operator==(const ReadyMsg&) const = default;
};

struct CursorMsg {
    CursorKind kind = CursorKind::Standard;
    std::uint32_t qtShape = 0; // when Standard
    std::uint64_t cacheKey = 0; // when Rgba
    std::uint16_t width = 0; // 0 when selecting an already-cached RGBA cursor
    std::uint16_t height = 0;
    std::int16_t hotX = 0;
    std::int16_t hotY = 0;
    std::vector<std::uint8_t> bgra; // width * height * 4 premultiplied BGRA
    bool operator==(const CursorMsg&) const = default;
};

struct ImeStateMsg {
    bool enabled = false;
    std::uint32_t monitorIndex = 0;
    std::int32_t caretX = 0;
    std::int32_t caretY = 0;
    std::int32_t caretW = 0;
    std::int32_t caretH = 0;
    bool operator==(const ImeStateMsg&) const = default;
};

struct MouseMsg {
    MouseKind kind = MouseKind::Move;
    MouseButton button = MouseButton::None;
    std::uint8_t buttons = 0;
    std::uint8_t modifiers = 0;
    std::int32_t globalX = 0;
    std::int32_t globalY = 0;
    std::int16_t wheelDelta = 0;
    bool operator==(const MouseMsg&) const = default;
};

struct KeyMsg {
    bool press = false;
    bool repeat = false;
    bool extended = false;
    std::uint8_t modifiers = 0;
    std::uint32_t vk = 0;
    std::uint32_t scan = 0;
    std::u16string text;
    bool operator==(const KeyMsg&) const = default;
};

struct ImeEventMsg {
    ImeKind kind = ImeKind::End;
    std::int32_t cursorPos = 0;
    std::u16string text;
    bool operator==(const ImeEventMsg&) const = default;
};

struct ClosedMsg {
    CloseReason reason = CloseReason::Requested;
    bool operator==(const ClosedMsg&) const = default;
};

class Writer {
public:
    Writer() { reset(); }
    void reset() { m_buf.assign(kHeaderBytes, std::byte {}); }
    void u8(std::uint8_t v) { append(&v, sizeof v); }
    void u16(std::uint16_t v) { append(&v, sizeof v); }
    void i16(std::int16_t v) { append(&v, sizeof v); }
    void u32(std::uint32_t v) { append(&v, sizeof v); }
    void i32(std::int32_t v) { append(&v, sizeof v); }
    void u64(std::uint64_t v) { append(&v, sizeof v); }
    void bytes(std::span<const std::uint8_t> b)
    {
        if (!b.empty())
            append(b.data(), b.size());
    }
    void text16(std::u16string_view s)
    {
        const auto n = static_cast<std::uint16_t>((std::min)(s.size(), std::size_t {kMaxTextChars}));
        u16(n);
        if (n)
            append(s.data(), std::size_t {n} * sizeof(char16_t));
    }
    std::vector<std::byte> finish(Type type)
    {
        const auto len = static_cast<std::uint32_t>(m_buf.size() - kHeaderBytes);
        const auto t = static_cast<std::uint16_t>(type);
        const std::uint16_t reserved = 0;
        std::memcpy(m_buf.data(), &t, sizeof t);
        std::memcpy(m_buf.data() + 2, &reserved, sizeof reserved);
        std::memcpy(m_buf.data() + 4, &len, sizeof len);
        return std::move(m_buf);
    }

private:
    void append(const void* data, std::size_t n)
    {
        const auto* p = static_cast<const std::byte*>(data);
        m_buf.insert(m_buf.end(), p, p + n);
    }
    std::vector<std::byte> m_buf;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> data) noexcept
        : m_data(data)
    {
    }
    bool u8(std::uint8_t& v) noexcept { return take(&v, sizeof v); }
    bool u16(std::uint16_t& v) noexcept { return take(&v, sizeof v); }
    bool i16(std::int16_t& v) noexcept { return take(&v, sizeof v); }
    bool u32(std::uint32_t& v) noexcept { return take(&v, sizeof v); }
    bool i32(std::int32_t& v) noexcept { return take(&v, sizeof v); }
    bool u64(std::uint64_t& v) noexcept { return take(&v, sizeof v); }
    bool bytes(std::vector<std::uint8_t>& out, std::size_t n)
    {
        if (n > m_data.size() - m_pos)
            return false;
        out.resize(n);
        if (n)
            std::memcpy(out.data(), m_data.data() + m_pos, n);
        m_pos += n;
        return true;
    }
    bool text16(std::u16string& out)
    {
        std::uint16_t n = 0;
        if (!u16(n) || n > kMaxTextChars)
            return false;
        const std::size_t byteCount = std::size_t {n} * sizeof(char16_t);
        if (byteCount > m_data.size() - m_pos)
            return false;
        out.resize(n);
        if (n)
            std::memcpy(out.data(), m_data.data() + m_pos, byteCount);
        m_pos += byteCount;
        return true;
    }
    bool done() const noexcept { return m_pos == m_data.size(); }

private:
    bool take(void* dst, std::size_t n) noexcept
    {
        if (n > m_data.size() - m_pos)
            return false;
        std::memcpy(dst, m_data.data() + m_pos, n);
        m_pos += n;
        return true;
    }
    std::span<const std::byte> m_data;
    std::size_t m_pos = 0;
};

inline bool parseHeader(std::span<const std::byte, kHeaderBytes> header, Type& type, std::uint32_t& length) noexcept
{
    std::uint16_t rawType = 0;
    std::uint16_t reserved = 0;
    std::memcpy(&rawType, header.data(), sizeof rawType);
    std::memcpy(&reserved, header.data() + 2, sizeof reserved);
    std::memcpy(&length, header.data() + 4, sizeof length);
    if (reserved != 0 || length > kMaxFrameBytes)
        return false;
    type = static_cast<Type>(rawType);
    return true;
}

inline std::vector<std::byte> encode(const InitMsg& m)
{
    Writer w;
    w.u32(m.activeIndex);
    w.u32(static_cast<std::uint32_t>(m.monitors.size()));
    for (const auto& mon : m.monitors) {
        w.u64(mon.overlayHwnd);
        w.i32(mon.x);
        w.i32(mon.y);
        w.i32(mon.w);
        w.i32(mon.h);
    }
    return w.finish(Type::Init);
}

inline bool decode(std::span<const std::byte> payload, InitMsg& m)
{
    Reader r(payload);
    std::uint32_t count = 0;
    if (!r.u32(m.activeIndex) || !r.u32(count) || count == 0 || count > kMaxMonitors || m.activeIndex >= count)
        return false;
    m.monitors.resize(count);
    for (auto& mon : m.monitors) {
        if (!r.u64(mon.overlayHwnd) || !r.i32(mon.x) || !r.i32(mon.y) || !r.i32(mon.w) || !r.i32(mon.h)
            || !mon.overlayHwnd || mon.w <= 0 || mon.h <= 0 || mon.w > 32768 || mon.h > 32768)
            return false;
    }
    return r.done();
}

inline std::vector<std::byte> encode(const ReadyMsg& m)
{
    Writer w;
    w.u32(m.ok);
    w.u32(m.error);
    w.u32(static_cast<std::uint32_t>(m.surfaceHwnds.size()));
    for (std::uint64_t h : m.surfaceHwnds)
        w.u64(h);
    return w.finish(Type::Ready);
}

inline bool decode(std::span<const std::byte> payload, ReadyMsg& m)
{
    Reader r(payload);
    std::uint32_t count = 0;
    if (!r.u32(m.ok) || !r.u32(m.error) || !r.u32(count) || count > kMaxMonitors)
        return false;
    m.surfaceHwnds.resize(count);
    for (std::uint64_t& h : m.surfaceHwnds) {
        if (!r.u64(h))
            return false;
    }
    return r.done();
}

inline std::vector<std::byte> encode(const CursorMsg& m)
{
    Writer w;
    w.u8(static_cast<std::uint8_t>(m.kind));
    w.u32(m.qtShape);
    w.u64(m.cacheKey);
    w.u16(m.width);
    w.u16(m.height);
    w.i16(m.hotX);
    w.i16(m.hotY);
    w.bytes(m.bgra);
    return w.finish(Type::SetCursor);
}

inline bool decode(std::span<const std::byte> payload, CursorMsg& m)
{
    Reader r(payload);
    std::uint8_t rawKind = 0;
    if (!r.u8(rawKind) || rawKind > static_cast<std::uint8_t>(CursorKind::Rgba) || !r.u32(m.qtShape)
        || !r.u64(m.cacheKey) || !r.u16(m.width) || !r.u16(m.height) || !r.i16(m.hotX) || !r.i16(m.hotY))
        return false;
    m.kind = static_cast<CursorKind>(rawKind);
    if (m.width > kMaxCursorDim || m.height > kMaxCursorDim)
        return false;
    const std::size_t expected = m.kind == CursorKind::Rgba ? std::size_t {m.width} * std::size_t {m.height} * 4u : 0u;
    if (expected > kMaxFrameBytes - 32u || !r.bytes(m.bgra, expected))
        return false;
    return r.done();
}

inline std::vector<std::byte> encode(const ImeStateMsg& m)
{
    Writer w;
    w.u8(m.enabled ? 1 : 0);
    w.u32(m.monitorIndex);
    w.i32(m.caretX);
    w.i32(m.caretY);
    w.i32(m.caretW);
    w.i32(m.caretH);
    return w.finish(Type::SetIme);
}

inline bool decode(std::span<const std::byte> payload, ImeStateMsg& m)
{
    Reader r(payload);
    std::uint8_t en = 0;
    if (!r.u8(en) || !r.u32(m.monitorIndex) || !r.i32(m.caretX) || !r.i32(m.caretY) || !r.i32(m.caretW)
        || !r.i32(m.caretH) || !r.done())
        return false;
    m.enabled = en != 0;
    return true;
}

inline std::vector<std::byte> encodeClose()
{
    Writer w;
    return w.finish(Type::Close);
}

inline std::vector<std::byte> encode(const MouseMsg& m)
{
    Writer w;
    w.u8(static_cast<std::uint8_t>(m.kind));
    w.u8(static_cast<std::uint8_t>(m.button));
    w.u8(m.buttons);
    w.u8(m.modifiers);
    w.i32(m.globalX);
    w.i32(m.globalY);
    w.i16(m.wheelDelta);
    return w.finish(Type::Mouse);
}

inline bool decode(std::span<const std::byte> payload, MouseMsg& m)
{
    Reader r(payload);
    std::uint8_t k = 0, b = 0;
    if (!r.u8(k) || k > static_cast<std::uint8_t>(MouseKind::Leave)
        || !r.u8(b) || b > static_cast<std::uint8_t>(MouseButton::X2)
        || !r.u8(m.buttons) || !r.u8(m.modifiers) || !r.i32(m.globalX) || !r.i32(m.globalY)
        || !r.i16(m.wheelDelta) || !r.done())
        return false;
    m.kind = static_cast<MouseKind>(k);
    m.button = static_cast<MouseButton>(b);
    return true;
}

inline std::vector<std::byte> encode(const KeyMsg& m)
{
    Writer w;
    w.u8(m.press ? 1 : 0);
    w.u8(m.repeat ? 1 : 0);
    w.u8(m.extended ? 1 : 0);
    w.u8(m.modifiers);
    w.u32(m.vk);
    w.u32(m.scan);
    w.text16(m.text);
    return w.finish(Type::Key);
}

inline bool decode(std::span<const std::byte> payload, KeyMsg& m)
{
    Reader r(payload);
    std::uint8_t p = 0, rep = 0, ext = 0;
    if (!r.u8(p) || !r.u8(rep) || !r.u8(ext) || !r.u8(m.modifiers) || !r.u32(m.vk) || !r.u32(m.scan)
        || !r.text16(m.text) || !r.done())
        return false;
    m.press = p != 0;
    m.repeat = rep != 0;
    m.extended = ext != 0;
    return true;
}

inline std::vector<std::byte> encode(const ImeEventMsg& m)
{
    Writer w;
    w.u8(static_cast<std::uint8_t>(m.kind));
    w.i32(m.cursorPos);
    w.text16(m.text);
    return w.finish(Type::Ime);
}

inline bool decode(std::span<const std::byte> payload, ImeEventMsg& m)
{
    Reader r(payload);
    std::uint8_t k = 0;
    if (!r.u8(k) || k > static_cast<std::uint8_t>(ImeKind::End) || !r.i32(m.cursorPos) || !r.text16(m.text) || !r.done())
        return false;
    m.kind = static_cast<ImeKind>(k);
    return true;
}

inline std::vector<std::byte> encode(const ClosedMsg& m)
{
    Writer w;
    w.u32(static_cast<std::uint32_t>(m.reason));
    return w.finish(Type::Closed);
}

inline bool decode(std::span<const std::byte> payload, ClosedMsg& m)
{
    Reader r(payload);
    std::uint32_t reason = 0;
    if (!r.u32(reason) || reason > static_cast<std::uint32_t>(CloseReason::Error) || !r.done())
        return false;
    m.reason = static_cast<CloseReason>(reason);
    return true;
}

} // namespace ws::capsurface
