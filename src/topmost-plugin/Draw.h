// What the two things winshun.topmost draws (Note, Border) share: a colour as
// Direct2D takes it, and letting go of a COM pointer.
#pragma once

#include <d2d1.h>

#include <cstdint>

namespace topmost {

template <class T>
void release(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// 0xAARRGGBB.
inline D2D1_COLOR_F colour(std::uint32_t argb)
{
    return D2D1::ColorF(static_cast<float>((argb >> 16) & 0xFF) / 255.f, static_cast<float>((argb >> 8) & 0xFF) / 255.f,
        static_cast<float>(argb & 0xFF) / 255.f, static_cast<float>(argb >> 24) / 255.f);
}

} // namespace topmost
