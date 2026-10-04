#include "hosts/reshade/menu_guides.h"
#include "hosts/reshade/ngx_params.h"
#include "hosts/reshade/readable_guides.h"
#include "hosts/reshade/shell_host.h"
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

namespace ofps::reshade {
namespace {
using Microsoft::WRL::ComPtr;
constexpr D3D12_RESOURCE_STATES kInput = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; // NGX inputs at evaluate
constexpr UINT64 kReserved = ~0ull;

struct Set {
    ComPtr<ID3D12Resource> depth;
    UINT64 readUntil = 0; // f2 value of the last pass that read this set; kReserved while a pass records
};
struct Guides {
    std::mutex mutex;
    Set sets[2];
    ComPtr<ID3D12Resource> zeroMotion; // committed resources are created zeroed
    D3D12_RESOURCE_DESC depthDesc{}, motionDesc{};
    ComPtr<ID3D12Device> device;                      // the sets' device (the host list's)
    int epoch = 0, current = -1, reserved = -1, reservedEpoch = -1;
    UINT64 reservedBefore = 0, lastRead = 0;
    ComPtr<ID3D12Fence> f2;                           // the fence the read values belong to
    std::vector<ComPtr<ID3D12Resource>> deferred;     // retired while a pass recorded with them
};
Guides &G() { static Guides *guides = new Guides(); return *guides; } // never destroyed from DllMain

void Log(const char *text) { Host().Log(OFPS_LOG_INFO, text); }

bool SameDesc(const D3D12_RESOURCE_DESC &a, const D3D12_RESOURCE_DESC &b) {
    return a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height && a.Format == b.Format &&
           a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels && a.SampleDesc.Count == b.SampleDesc.Count;
}

bool Make(ID3D12Device *device, const D3D12_RESOURCE_DESC &source, ComPtr<ID3D12Resource> &out) {
    D3D12_RESOURCE_DESC d = source; d.Flags = D3D12_RESOURCE_FLAG_NONE; d.Alignment = 0;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, kInput, nullptr, IID_PPV_ARGS(&out)));
}

// A planar depth (the readable twin of a D24S8 / D32S8 depth, readable_guides.cpp) is transitioned and copied on its
// depth plane only, as the twin itself is: its stencil plane rests in whatever state it was created in.
void Copy(ID3D12GraphicsCommandList *list, ID3D12Resource *source, ID3D12Resource *own) {
    const UINT sub = DepthPlaneSubresource(source->GetDesc().Format); // the own set has the source's format
    D3D12_RESOURCE_BARRIER b[2]{};
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition = {source, sub, kInput, D3D12_RESOURCE_STATE_COPY_SOURCE};
    b[1].Transition = {own, sub, kInput, D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(2, b);
    if (sub == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        list->CopyResource(own, source);
    } else {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = own; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = sub;
        src.pResource = source; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = sub;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
    list->ResourceBarrier(2, b);
}

// The core releases it once its registered queues (the host's, where the copies ran) and `f2` >= value passed.
void ToCore(ID3D12Resource *texture, ID3D12Fence *f2, UINT64 value) {
    IOfpsCore *core = Core();
    if (!texture || !core) return; // no core: no host copy or pass ever used it
    const OfpsFencePoint point{sizeof(OfpsFencePoint), f2, value};
    core->RetireResource(texture, f2 && value && value != kReserved ? &point : nullptr);
}

// Every texture to the graveyard; the ones a pass is recording with wait for MenuGuidesUsed/Unused. A set's copy on a
// host list keeps its own reference (readable_guides.h hold) until that list was submitted and passed its fence.
void RetireAll(Guides &g) {
    const bool recording = g.reserved >= 0 && g.reservedEpoch == g.epoch;
    for (int i = 0; i < 2; ++i) {
        Set &s = g.sets[i];
        if (s.depth) ReadableGuidesHoldDrop(s.depth.Get());
        if (recording && i == g.reserved) g.deferred.push_back(s.depth);
        else ToCore(s.depth.Get(), g.f2.Get(), s.readUntil);
        s = Set{};
    }
    if (recording) g.deferred.push_back(g.zeroMotion);
    else ToCore(g.zeroMotion.Get(), g.f2.Get(), g.lastRead);
    g.zeroMotion.Reset();
    g.device.Reset();
    g.current = -1;
    ++g.epoch;
}

void RetireDeferred(Guides &g, UINT64 value) {
    for (auto &texture : g.deferred) ToCore(texture.Get(), g.f2.Get(), value);
    g.deferred.clear();
}
} // namespace

int MenuGuidesCopy(ID3D12GraphicsCommandList *hostList, void *params, const char **problem) {
    *problem = nullptr;
    ID3D12Resource *depth = GetResource(params, "DLSSNR.Depth"), *motion = GetResource(params, "DLSSNR.MVec");
    if (!hostList || !depth || !motion) { *problem = "the game's NR block has no depth or motion"; return kMenuGuidesRefused; }
    const D3D12_RESOURCE_DESC dd = depth->GetDesc(), md = motion->GetDesc();
    if ((dd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) {
        *problem = "the game's depth is a depth-stencil texture (not readable in menus)";
        return kMenuGuidesRefused;
    }
    ComPtr<ID3D12Device> device; // the sets live on the device of the list that copies into them
    if (FAILED(hostList->GetDevice(IID_PPV_ARGS(&device)))) { *problem = "the game's NR command list has no device"; return kMenuGuidesRefused; }
    auto &g = G();
    std::unique_lock lock(g.mutex);
    if (!g.sets[0].depth || g.device != device || !SameDesc(dd, g.depthDesc) || !SameDesc(md, g.motionDesc)) {
        const int epoch = g.epoch; // any install, retirement or release from here on bumps it
        lock.unlock(); // a present reserving a set does not wait for the creation (reviewer M7)
        ComPtr<ID3D12Resource> made[3]; // two depth sets, the zero motion
        const bool ok = Make(device.Get(), dd, made[0]) && Make(device.Get(), dd, made[1]) && Make(device.Get(), md, made[2]);
        lock.lock();
        // Another evaluate or a release changed the sets meanwhile: those are newer than this allocation. `made` was
        // never recorded or handed out, so it is released right here; the next evaluate checks again.
        if (g.epoch != epoch) return kMenuGuidesBusy;
        RetireAll(g);
        if (!ok) { // what was made is released with `made`: nothing was ever recorded with it
            g.depthDesc = g.motionDesc = D3D12_RESOURCE_DESC{};
            *problem = "menu mode's depth snapshot could not be created";
            return kMenuGuidesRefused;
        }
        g.sets[0].depth = made[0];
        g.sets[1].depth = made[1];
        g.zeroMotion = made[2];
        g.device = device;
        g.depthDesc = dd;
        g.motionDesc = md;
        char text[200];
        std::snprintf(text, sizeof(text), "menu guides: depth snapshots (fmt %d, %llux%u) and a zero motion (fmt %d, %llux%u)", int(dd.Format),
                      static_cast<unsigned long long>(dd.Width), dd.Height, int(md.Format), static_cast<unsigned long long>(md.Width), md.Height);
        Log(text);
    }
    const int next = g.current == 0 ? 1 : 0;
    Set &s = g.sets[next];
    // Busy: a pass reserved or still reads it, or the host list of its previous copy has not passed its fence yet.
    if (s.readUntil == kReserved || (g.f2 && g.f2->GetCompletedValue() < s.readUntil) || ReadableGuidesHoldBusy(s.depth.Get()))
        return kMenuGuidesBusy;
    if (!ReadableGuidesHoldWrite(hostList, s.depth.Get())) {
        *problem = "menu mode cannot follow the game's NR command list (no tag or fence for it)";
        return kMenuGuidesRefused;
    }
    Copy(hostList, depth, s.depth.Get());
    g.current = next;
    return g.epoch * 2 + next;
}

bool MenuGuidesFor(int set, ID3D12Resource **depth, ID3D12Resource **motion) {
    auto &g = G();
    std::lock_guard lock(g.mutex);
    if (set < 0 || set / 2 != g.epoch) return false;
    Set &s = g.sets[set % 2];
    if (!s.depth || !g.zeroMotion || s.readUntil == kReserved) return false;
    g.reserved = set % 2;
    g.reservedEpoch = g.epoch;
    g.reservedBefore = s.readUntil;
    s.readUntil = kReserved;
    *depth = s.depth.Get();
    *motion = g.zeroMotion.Get();
    return true;
}

void MenuGuidesUsed(ID3D12Fence *f2, UINT64 value2) {
    auto &g = G();
    std::lock_guard lock(g.mutex);
    if (g.f2.Get() != f2) { // a rebuilt pipeline: the old fence's values mean nothing to the new one (it was drained)
        for (int i = 0; i < 2; ++i) if (!(i == g.reserved && g.reservedEpoch == g.epoch)) g.sets[i].readUntil = 0;
        g.f2 = f2;
        g.lastRead = 0;
    }
    g.lastRead = std::max(g.lastRead, value2);
    if (g.reserved >= 0 && g.reservedEpoch == g.epoch) g.sets[g.reserved].readUntil = value2;
    RetireDeferred(g, value2);
    g.reserved = -1;
}

void MenuGuidesUnused() {
    auto &g = G();
    std::lock_guard lock(g.mutex);
    if (g.reserved >= 0 && g.reservedEpoch == g.epoch) g.sets[g.reserved].readUntil = g.reservedBefore;
    RetireDeferred(g, g.lastRead);
    g.reserved = -1;
}

void MenuGuidesRelease() {
    auto &g = G();
    std::lock_guard lock(g.mutex);
    RetireAll(g);
    g.depthDesc = g.motionDesc = D3D12_RESOURCE_DESC{};
}

} // namespace ofps::reshade
