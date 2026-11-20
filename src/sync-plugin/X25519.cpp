// X25519 (RFC 7748): the Montgomery ladder over GF(2^255 - 19) in sixteen
// 16-bit limbs, after TweetNaCl's crypto_scalarmult (public domain; Bernstein,
// van Gastel, Janssen, Lange, Schwabe, Smetsers). Constant time: the ladder
// swaps by masks, never by branches on the scalar's bits.

#include "Crypto.h"

#include <array>
#include <cstdint>
#include <stdexcept>

namespace shunsync {

namespace {

using Limbs = std::array<std::int64_t, 16>;

void carry(Limbs& o) noexcept
{
    for (int i = 0; i < 16; ++i) {
        o[i] += std::int64_t(1) << 16;
        const std::int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * (std::int64_t(1) << 16);
    }
}

// p and q swapped when b is 1, as they are when 0.
void swapIf(Limbs& p, Limbs& q, std::int64_t b) noexcept
{
    const std::int64_t mask = ~(b - 1);
    for (int i = 0; i < 16; ++i) {
        const std::int64_t t = mask & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void pack(std::uint8_t* out, const Limbs& n) noexcept
{
    Limbs t = n;
    Limbs m {};
    carry(t);
    carry(t);
    carry(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const std::int64_t b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        swapIf(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        out[2 * i] = static_cast<std::uint8_t>(t[i] & 0xff);
        out[2 * i + 1] = static_cast<std::uint8_t>((t[i] >> 8) & 0xff);
    }
}

void unpack(Limbs& o, const std::uint8_t* n) noexcept
{
    for (int i = 0; i < 16; ++i)
        o[i] = n[2 * i] + (std::int64_t(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff; // the u-coordinate's top bit is ignored (RFC 7748, 5)
}

void add(Limbs& o, const Limbs& a, const Limbs& b) noexcept
{
    for (int i = 0; i < 16; ++i)
        o[i] = a[i] + b[i];
}

void sub(Limbs& o, const Limbs& a, const Limbs& b) noexcept
{
    for (int i = 0; i < 16; ++i)
        o[i] = a[i] - b[i];
}

void mul(Limbs& o, const Limbs& a, const Limbs& b) noexcept
{
    std::array<std::int64_t, 31> t {};
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j)
            t[i + j] += a[i] * b[j];
    }
    for (int i = 0; i < 15; ++i)
        t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i)
        o[i] = t[i];
    carry(o);
    carry(o);
}

void square(Limbs& o, const Limbs& a) noexcept { mul(o, a, a); }

void invert(Limbs& o, const Limbs& in) noexcept
{
    Limbs c = in;
    for (int a = 253; a >= 0; --a) {
        square(c, c);
        if (a != 2 && a != 4)
            mul(c, c, in);
    }
    o = c;
}

} // namespace

Bytes x25519(std::string_view scalar, std::string_view point)
{
    if (scalar.size() != 32 || point.size() != 32)
        throw std::invalid_argument("x25519: 32 bytes each");
    std::array<std::uint8_t, 32> z {};
    for (int i = 0; i < 32; ++i)
        z[i] = static_cast<std::uint8_t>(scalar[i]);
    z[31] = static_cast<std::uint8_t>((z[31] & 127) | 64);
    z[0] &= 248;
    Limbs x {};
    unpack(x, reinterpret_cast<const std::uint8_t*>(point.data()));
    Limbs a {}, b = x, c {}, d {}, e {}, f {};
    a[0] = d[0] = 1;
    static const Limbs k121665 {0xDB41, 1};
    for (int i = 254; i >= 0; --i) {
        const std::int64_t r = (z[i >> 3] >> (i & 7)) & 1;
        swapIf(a, b, r);
        swapIf(c, d, r);
        add(e, a, c);
        sub(a, a, c);
        add(c, b, d);
        sub(b, b, d);
        square(d, e);
        square(f, a);
        mul(a, c, a);
        mul(c, b, e);
        add(e, a, c);
        sub(a, a, c);
        square(b, a);
        sub(c, d, f);
        mul(a, c, k121665);
        add(a, a, d);
        mul(c, c, a);
        mul(a, d, f);
        mul(d, b, x);
        square(b, e);
        swapIf(a, b, r);
        swapIf(c, d, r);
    }
    invert(c, c);
    mul(a, a, c);
    Bytes out(32, '\0');
    pack(reinterpret_cast<std::uint8_t*>(out.data()), a);
    return out;
}

Bytes x25519Public(std::string_view scalar)
{
    Bytes base(32, '\0');
    base[0] = 9;
    return x25519(scalar, base);
}

} // namespace shunsync
