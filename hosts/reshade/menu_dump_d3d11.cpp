#include "hosts/reshade/menu_dump.h"
#include "hosts/reshade/shell_host.h"
#include <windows.h>
#include <wrl/client.h>
#include <vector>

// Diagnostics (DebugMenuDump=1, D3D11 swap chains): the back buffer after the bridge's write-back, copied into a
// staging texture on the immediate context and written on a later present once a DO_NOT_WAIT map succeeds.
namespace ofps::reshade {
namespace {
using Microsoft::WRL::ComPtr;
constexpr unsigned kPoolSize = 7; // the 10th menu present, the five after an exit, the probe

struct Slot {
    ComPtr<ID3D11Texture2D> staging;
    unsigned long long presentIndex = 0;
    bool busy = false, probe = false;
};
struct Dumps {
    std::vector<Slot> slots;
    UINT width = 0, height = 0;
    bool swizzle = false, poolFullLogged = false;
};
Dumps &State() { static Dumps *dumps = new Dumps(); return *dumps; } // never destroyed from DllMain
} // namespace

bool MenuDump11Prepare(ID3D11Device *device, UINT width, UINT height, DXGI_FORMAT format) {
    auto &s = State();
    s.slots.clear();
    s.swizzle = format == DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!s.swizzle && format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        Host().Log(OFPS_LOG_WARN, "menu pipeline: back buffer format is not 8-bit RGBA/BGRA; no dumps");
        return false;
    }
    D3D11_TEXTURE2D_DESC d{}; d.Width = width; d.Height = height; d.MipLevels = 1; d.ArraySize = 1; d.Format = format;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    s.slots.resize(kPoolSize);
    for (auto &slot : s.slots)
        if (FAILED(device->CreateTexture2D(&d, nullptr, &slot.staging))) { s.slots.clear(); return false; }
    s.width = width; s.height = height;
    return true;
}

bool MenuDump11Record(ID3D11DeviceContext *context, ID3D11Resource *backBuffer, unsigned long long presentIndex, bool probe) {
    auto &s = State();
    Slot *free = nullptr;
    for (auto &slot : s.slots) if (!slot.busy) { free = &slot; break; }
    if (!free) {
        if (!s.poolFullLogged) Host().Log(OFPS_LOG_WARN, "menu pipeline: every dump texture is in flight; dump skipped (logged once)");
        s.poolFullLogged = true;
        return false;
    }
    context->CopyResource(free->staging.Get(), backBuffer);
    free->busy = true; free->presentIndex = presentIndex; free->probe = probe;
    return true;
}

bool MenuDump11Idle() {
    for (const auto &slot : State().slots) if (slot.busy) return false;
    return true;
}

double MenuDump11Poll(ID3D11DeviceContext *context) {
    LARGE_INTEGER t0{}, t1{}, f{};
    QueryPerformanceCounter(&t0);
    bool wrote = false;
    for (auto &slot : State().slots) {
        if (!slot.busy) continue;
        D3D11_MAPPED_SUBRESOURCE map{};
        if (context->Map(slot.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map) != S_OK) continue; // still drawing
        auto &s = State();
        const bool ok = MenuWriteBmp(static_cast<const BYTE *>(map.pData), map.RowPitch, s.width, s.height, s.swizzle, slot.presentIndex, slot.probe);
        context->Unmap(slot.staging.Get(), 0);
        wrote = true;
        if (!ok) Host().Log(OFPS_LOG_WARN, "menu pipeline: a dump could not be written");
        else if (slot.probe) MenuEvent("probe %llu", slot.presentIndex);
        else MenuEvent("dump %llu", slot.presentIndex);
        slot.busy = false;
    }
    if (!wrote) return 0.0;
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f);
    return double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
}

void MenuDump11Release() { State().slots.clear(); }

} // namespace ofps::reshade
