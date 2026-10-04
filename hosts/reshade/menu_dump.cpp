#include "hosts/reshade/menu_dump.h"
#include "hosts/reshade/shell_host.h"
#include <windows.h>
#include <wrl/client.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace ofps::reshade {
namespace {
using Microsoft::WRL::ComPtr;
// The 10th menu present plus the first five after an exit can be in flight at once, and a probe.
constexpr unsigned kPoolSize = 7;

struct Slot {
    ComPtr<ID3D12Resource> buffer;
    unsigned long long presentIndex = 0;
    UINT64 fence = 0; // f3 value; 0 = recorded, not yet submitted
    bool busy = false, probe = false;
};
struct Dumps {
    std::vector<Slot> slots;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT width = 0, height = 0;
    bool swizzle = false, poolFullLogged = false;
    FILE *events = nullptr;
};
Dumps &State() { static Dumps *dumps = new Dumps(); return *dumps; } // never destroyed from DllMain

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t *slash = wcsrchr(path, L'\\')) slash[1] = 0;
    return path;
}

bool WriteBmp(Slot &slot) {
    auto &s = State();
    void *p = nullptr;
    D3D12_RANGE range{0, SIZE_T(slot.buffer->GetDesc().Width)};
    if (FAILED(slot.buffer->Map(0, &range, &p))) return false;
    const bool ok = MenuWriteBmp(static_cast<const BYTE *>(p) + s.layout.Offset, s.layout.Footprint.RowPitch, s.width, s.height, s.swizzle,
                                 slot.presentIndex, slot.probe);
    D3D12_RANGE empty{0, 0}; slot.buffer->Unmap(0, &empty);
    return ok;
}
} // namespace

// Same layout as the bench's own dump (device_dump.cpp): 32-bit BGRA, top-down, alpha forced to 255.
bool MenuWriteBmp(const BYTE *rows, UINT rowPitch, UINT width, UINT height, bool swizzle, unsigned long long presentIndex, bool probe) {
    std::vector<BYTE> pixels(std::size_t(width) * height * 4);
    for (UINT y = 0; y < height; ++y) {
        const BYTE *src = rows + std::size_t(y) * rowPitch;
        BYTE *dst = pixels.data() + std::size_t(y) * width * 4;
        for (UINT x = 0; x < width; ++x) {
            dst[x * 4] = src[x * 4 + (swizzle ? 2 : 0)]; dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + (swizzle ? 0 : 2)]; dst[x * 4 + 3] = 255;
        }
    }
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + DWORD(pixels.size());
    BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = LONG(width); info.biHeight = -LONG(height);
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB; info.biSizeImage = DWORD(pixels.size());
    const std::wstring name = ExeDirectory() + (probe ? L"menu_dump_probe_" : L"menu_dump_") + std::to_wstring(presentIndex) + L".bmp";
    FILE *f = nullptr;
    if (_wfopen_s(&f, name.c_str(), L"wb") != 0 || !f) return false;
    const bool ok = std::fwrite(&file, sizeof(file), 1, f) == 1 && std::fwrite(&info, sizeof(info), 1, f) == 1 &&
                    std::fwrite(pixels.data(), pixels.size(), 1, f) == 1;
    return std::fclose(f) == 0 && ok;
}

void MenuEvent(const char *fmt, ...) {
    auto &s = State();
    if (!s.events && _wfopen_s(&s.events, (ExeDirectory() + L"menu_events.log").c_str(), L"w") != 0) s.events = nullptr;
    if (!s.events) return;
    va_list args; va_start(args, fmt); std::vfprintf(s.events, fmt, args); va_end(args);
    std::fputc('\n', s.events);
    std::fflush(s.events); // the bench leaves through TerminateProcess
}

bool MenuDumpPrepare(ID3D12Device *device, const D3D12_RESOURCE_DESC &backBuffer) {
    auto &s = State();
    s.slots.clear();
    s.swizzle = backBuffer.Format == DXGI_FORMAT_R8G8B8A8_UNORM || backBuffer.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (!s.swizzle && backBuffer.Format != DXGI_FORMAT_B8G8R8A8_UNORM && backBuffer.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        Host().Log(OFPS_LOG_WARN, "menu pipeline: back buffer format is not 8-bit RGBA/BGRA; no dumps");
        return false;
    }
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&backBuffer, 0, 1, 0, &s.layout, nullptr, nullptr, &bytes);
    s.width = UINT(backBuffer.Width); s.height = backBuffer.Height;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = bytes; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    s.slots.resize(kPoolSize);
    for (auto &slot : s.slots)
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&slot.buffer)))) { s.slots.clear(); return false; }
    return true;
}

bool MenuDumpIdle() {
    for (const auto &slot : State().slots) if (slot.busy) return false;
    return true;
}

bool MenuDumpRecord(ID3D12GraphicsCommandList *list, ID3D12Resource *backBuffer, unsigned long long presentIndex, bool probe) {
    auto &s = State();
    Slot *free = nullptr;
    for (auto &slot : s.slots) if (!slot.busy) { free = &slot; break; }
    if (!free) {
        if (!s.poolFullLogged) Host().Log(OFPS_LOG_WARN, "menu pipeline: every dump buffer is in flight; dump skipped (logged once)");
        s.poolFullLogged = true;
        return false;
    }
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = backBuffer;
    b.Transition.Subresource = 0;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    src.pResource = backBuffer; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = free->buffer.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = s.layout;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    free->busy = true; free->fence = 0; free->presentIndex = presentIndex; free->probe = probe;
    return true;
}

void MenuDumpSubmitted(UINT64 fenceValue) {
    for (auto &slot : State().slots) if (slot.busy && slot.fence == 0) slot.fence = fenceValue;
}

void MenuDumpDiscard() {
    for (auto &slot : State().slots) if (slot.busy && slot.fence == 0) slot.busy = false;
}

double MenuDumpPoll(UINT64 completed) {
    LARGE_INTEGER t0{}, t1{}, f{};
    QueryPerformanceCounter(&t0);
    bool wrote = false;
    for (auto &slot : State().slots) {
        if (!slot.busy || slot.fence == 0 || completed < slot.fence) continue;
        const bool ok = WriteBmp(slot);
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

void MenuDumpRelease() {
    auto &s = State();
    s.slots.clear();
}

} // namespace ofps::reshade
