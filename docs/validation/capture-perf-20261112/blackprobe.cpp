// Watches the primary monitor frame by frame (DXGI desktop duplication) and
// prints every composed frame in which most of a 24x16 sample grid is pure
// black, or in which the count of black samples jumps: a black flash of the
// capture overlay shows as one or more such lines.
//   blackprobe.exe <seconds>
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    const double seconds = argc > 1 ? atof(argv[1]) : 10;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
        return 1;
    IDXGIDevice* dxgi = nullptr;
    device->QueryInterface(&dxgi);
    IDXGIAdapter* adapter = nullptr;
    dxgi->GetAdapter(&adapter);
    IDXGIOutput* output = nullptr;
    if (FAILED(adapter->EnumOutputs(0, &output)))
        return 2;
    IDXGIOutput1* output1 = nullptr;
    output->QueryInterface(&output1);
    IDXGIOutputDuplication* dup = nullptr;
    if (FAILED(output1->DuplicateOutput(device, &dup)))
        return 3;
    DXGI_OUTDUPL_DESC dd {};
    dup->GetDesc(&dd);
    printf("mode %ux%u @ %.1f Hz\n", dd.ModeDesc.Width, dd.ModeDesc.Height,
        dd.ModeDesc.RefreshRate.Denominator ? double(dd.ModeDesc.RefreshRate.Numerator) / dd.ModeDesc.RefreshRate.Denominator : 0.0);
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = dd.ModeDesc.Width;
    desc.Height = dd.ModeDesc.Height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = dd.ModeDesc.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
        return 4;
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    int last = -1;
    long frames = 0;
    for (;;) {
        QueryPerformanceCounter(&now);
        const double t = double(now.QuadPart - start.QuadPart) / freq.QuadPart;
        if (t > seconds)
            break;
        DXGI_OUTDUPL_FRAME_INFO info {};
        IDXGIResource* resource = nullptr;
        const HRESULT hr = dup->AcquireNextFrame(100, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            continue;
        if (FAILED(hr)) {
            printf("%.4f acquire failed %08lx\n", t, hr);
            break;
        }
        if (info.LastPresentTime.QuadPart != 0) { // the picture changed, not just the pointer
            ID3D11Texture2D* texture = nullptr;
            resource->QueryInterface(&texture);
            context->CopyResource(staging, texture);
            texture->Release();
            D3D11_MAPPED_SUBRESOURCE mapped {};
            if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
                int black = 0;
                const int cols = 24, rows = 16;
                for (int r = 0; r < rows; ++r) {
                    const unsigned y = (2 * r + 1) * desc.Height / (2 * rows);
                    for (int c = 0; c < cols; ++c) {
                        const unsigned x = (2 * c + 1) * desc.Width / (2 * cols);
                        const unsigned char* p = static_cast<const unsigned char*>(mapped.pData) + size_t(mapped.RowPitch) * y + size_t(x) * 4;
                        if (p[0] == 0 && p[1] == 0 && p[2] == 0)
                            ++black;
                    }
                }
                context->Unmap(staging, 0);
                ++frames;
                const double at = double(info.LastPresentTime.QuadPart - start.QuadPart) / freq.QuadPart;
                if (black != last && (black > cols * rows / 2 || last > cols * rows / 2 || abs(black - last) > 40))
                    printf("%.4f black %d/%d (was %d)\n", at, black, cols * rows, last);
                last = black;
            }
        }
        resource->Release();
        dup->ReleaseFrame();
    }
    printf("%ld frames watched, last black %d\n", frames, last);
    return 0;
}
