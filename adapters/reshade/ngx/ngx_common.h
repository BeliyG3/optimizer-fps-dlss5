#pragma once

// Shared machinery of the NGX interposers (feature-18 Neural Rendering hook and the DLSS SR hook):
// parameter-block access, the call forwarder, the D3D12 queue registry with GPU waits, the deferred
// release list, shader loading, format helpers, barriers and the structured-exception record. None
// of it knows a feature's parameter names; the hooks layer those on top.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>

#include "../../d3d11/d3d11_adapter.h"
#include "../../d3d12/d3d12_adapter.h"
#include "peripheral_warp/input_v2.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pwngx {

constexpr int kNgxSuccess = 1;

// ---- logging (the add-on routes this to ReShade's log)
using LogFn = void (*)(bool warning, const char *message);
void SetLog(LogFn log);
void Log(bool warning, const char *fmt, ...);

// ---- NGX parameter block (driver object driven through its vtable; slots found by probing)
void **VTable(void *params);
void SetUInt(void *params, const char *name, unsigned int value);
bool GetUInt(void *params, const char *name, unsigned int *value);
// Pointers (D3D11/D3D12 resources, handles) go through the 64-bit slots, which is what the block
// answers with for every pointer-typed key on the drivers seen so far.
void SetPointer(void *params, const char *name, void *value);
void *GetPointer(void *params, const char *name);
void SetFloat(void *params, const char *name, float value);
bool GetFloat(void *params, const char *name, float *value);
bool GetFloatVia(void *params, int getterSlot, const char *name, float *value);
int FloatSetterSlot();
int FloatGetterSlot();
// Debug: every getter slot's answer for a key, rendered into `out`.
void ProbeKey(void *params, const char *name, char *out, std::size_t outSize);

// ---- calls into the snippet through nvngx.dll_optimizerfps.dll (the snippet checks the caller's
// module name). The context argument is an ID3D12GraphicsCommandList* or an ID3D11DeviceContext*.
using PFN_Create = int(__cdecl *)(void *context, int featureId, void *params, void **outHandle);
using PFN_Evaluate = int(__cdecl *)(void *context, void *handle, void *params, void *callback);
using PFN_Release = int(__cdecl *)(void *handle);
bool LoadForwarder(const std::wstring &directory, std::string *error);
bool ForwarderLoaded();
int ForwardCreate(PFN_Create real, void *context, int featureId, void *params, void **outHandle);
int ForwardEvaluate(PFN_Evaluate real, void *context, void *handle, void *params, void *callback);
int ForwardRelease(PFN_Release real, void *handle);

// ---- D3D12 queue registry and GPU waits. The host's queue executes several frames behind the
// CPU, so nothing the GPU may still read is released before a fence signalled on the registered
// queues has completed. The registry has its own lock: a queue may be created inside an NGX call
// that already holds a hook's mutex, and ReShade reports that creation synchronously.
void RegisterQueue(ID3D12Device *device, ID3D12CommandQueue *queue);
// While set, RegisterQueue ignores new queues (the add-on's own background queue is created through
// the ReShade proxy and would otherwise announce itself as a host queue to wait on).
void SetQueueRegistrationSuppressed(bool suppressed);
void UnregisterQueue(ID3D12CommandQueue *queue);
// Waits for the queues on `device` or `proxyDevice` (all queues when neither matches). False when
// nothing could be waited for (no queue, device removed, timeout): the caller defers the release.
bool WaitForGpu(ID3D12Device *device, ID3D12Device *proxyDevice);
// Queue-side synchronisation with the registered graphics queues of a device (no CPU wait): every
// matching queue signals / waits on `fence` at `value`. False when no queue matched.
bool SignalRegisteredQueues(ID3D12Device *device, ID3D12Device *proxyDevice, ID3D12Fence *fence, UINT64 value);
// A gate on one queue: the fence that queue signalled and the value it will reach.
struct FenceGate {
    ID3D12Fence *fence = nullptr;
    UINT64 value = 0;
};

// 26.21: an owning AND over several queues. One fence signalled from several queues completes at the
// MAX of the values written to it, so the first queue to finish would open a single-fence gate while
// the others still read the resources; a set holds one (fence, value) per queue and is open only when
// every one of them has completed.
class GateSet {
public:
    GateSet() = default;
    GateSet(const GateSet &other) { Assign(other); }
    GateSet(GateSet &&other) noexcept : gates_(std::move(other.gates_)) { other.gates_.clear(); }
    GateSet &operator=(const GateSet &other)
    {
        if (this != &other) { Clear(); Assign(other); }
        return *this;
    }
    GateSet &operator=(GateSet &&other) noexcept
    {
        if (this != &other) { Clear(); gates_ = std::move(other.gates_); other.gates_.clear(); }
        return *this;
    }
    ~GateSet() { Clear(); }

    void Add(ID3D12Fence *fence, UINT64 value); // takes its own reference on the fence
    void Clear();                               // releases every reference
    bool Empty() const { return gates_.empty(); }
    std::size_t Size() const { return gates_.size(); }
    bool Completed() const;                     // every gate reached its value
    void Wait(DWORD milliseconds) const;        // CPU wait for all of them (teardown only)
private:
    void Assign(const GateSet &other);
    std::vector<FenceGate> gates_;
};

// 26.18/26.21: a fresh fence per registered queue of the device, each signalled on its own queue - the
// gate for a deferred release that never waits on the CPU. The returned set is empty when no queue of
// that device is registered.
GateSet SignalGate(ID3D12Device *device, ID3D12Device *proxyDevice);
bool WaitRegisteredQueues(ID3D12Device *device, ID3D12Device *proxyDevice, ID3D12Fence *fence, UINT64 value);
std::size_t CountQueues(ID3D12Device *device, ID3D12Device *proxyDevice, std::size_t *total);
// Timestamp frequency of a registered queue on `device`/`proxyDevice` (any queue when none matches).
bool TimestampFrequency(ID3D12Device *device, ID3D12Device *proxyDevice, UINT64 *frequency);
constexpr std::uint64_t kDeferredReleaseEvaluates = 16;

// ---- deferred releases: objects whose release could not be synchronised wait here until enough
// evaluates have passed (or until the host releases the feature). The caller serialises access.
// Anything else whose destructor must wait for the GPU (the temporal machine, for example).
struct Disposable {
    virtual ~Disposable() = default;
};

struct Grave {
    std::unique_ptr<pw::D3D12Adapter> adapter12;
    std::unique_ptr<pw::D3D11Adapter> adapter11;
    std::vector<std::unique_ptr<Disposable>> disposables;
    std::vector<IUnknown *> objects; // released with Release()
    void *realHandle = nullptr;      // released with releaseFn through the forwarder
    PFN_Release releaseFn = nullptr;
    std::uint64_t dueEval = 0;
    // Optional gate: nothing in this grave is released before every gate in the set has completed (a
    // background queue, or another of the device's queues, may still be using the objects); the grave
    // holds a reference on each fence.
    GateSet gate;
};
class Graveyard {
public:
    void Add(Grave &&grave, std::uint64_t now);
    void Drain(bool everything, std::uint64_t now);
    bool Empty() const { return graves_.empty(); }
private:
    std::vector<Grave> graves_;
};

// ---- shaders (the SDK's own DXBC beside the add-on in optimizer-fps-dlss5\)
struct Shaders {
    std::vector<char> vertex, pack, unpack, outline;
    // Temporal NR passes (optional: absent files leave these empty and the temporal modes off).
    std::vector<char> temporalResidual, temporalAccumulate, temporalReproject;
    std::vector<char> temporalDownsample; // optional: without it the hole fill is off
    std::vector<char> temporalCompose;    // optional: without it the guided smoothing of the addition is off (26.6.X)
    bool loaded = false;
    bool tried = false;
    pw::ShaderSet Set() const;
    bool TemporalLoaded() const { return !temporalResidual.empty() && !temporalAccumulate.empty() && !temporalReproject.empty(); }
};
bool LoadShaders(const std::wstring &addonDirectory, Shaders &shaders);

// ---- formats, barriers, textures (D3D12)
DXGI_FORMAT TypedView(DXGI_FORMAT format, bool depth);
pw::ColorEncoding EncodingFor(DXGI_FORMAT format);
void Barrier(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES &tracked,
             D3D12_RESOURCE_STATES to);
// 26.7.3: planar depth-stencil formats (R24G8 / R32G8X24 / D24S8 / D32S8): plane 0 = depth, plane 1 = stencil, each with
// its own state. Barriers and copies on such a guide address plane 0 only (we never read the stencil).
bool PlanarDepthFormat(DXGI_FORMAT format);
UINT DepthBarrierSubresource(ID3D12Resource *depth); // 0 for planar depth-stencil resources, ALL_SUBRESOURCES otherwise
void BarrierExternal(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES from,
                     D3D12_RESOURCE_STATES to, UINT subresource);
void BarrierExternal(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES from,
                     D3D12_RESOURCE_STATES to);
bool CreateTexture(ID3D12Device *device, UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                   ID3D12Resource **out);

// ---- structured exceptions inside a warped evaluate
struct CrashInfo {
    DWORD code = 0;
    void *address = nullptr;
    char module[MAX_PATH] = {};
    int stage = 0;
};
LONG RecordCrash(EXCEPTION_POINTERS *info, int stage, CrashInfo &out);

// ---- hook installation helper: describes who already patched an export (Detours chains: the
// latest attach is the outermost) and returns whether the prologue was already a jump.
bool DescribeExportPrologue(const char *hookName, const char *exportName, void *fn);

} // namespace pwngx
