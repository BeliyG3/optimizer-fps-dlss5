#include "ngx_common.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>

namespace pwngx {
namespace {

LogFn g_log = nullptr;

// Parameter block. Declared vtable: Set(ULL, float, double, uint, int, D3D11 res, D3D12 res,
// void*) = 0..7, Get(same) = 8..15. The compiler emits adjacent overloads in reverse order, so the
// float pair is found by round-tripping a value; on driver 310.8 it is setter 6 / getter 14. The
// 64-bit slots 0 / 8 answer every pointer-typed key.
constexpr int kSetPointer = 0;
constexpr int kSetUInt = 3;
constexpr int kGetPointer = 8;
constexpr int kGetUInt = 11;
using PFN_SetULL = void (*)(void *, const char *, unsigned long long);
using PFN_SetFloatT = void (*)(void *, const char *, float);
using PFN_SetUIntT = void (*)(void *, const char *, unsigned int);
using PFN_GetULL = int (*)(void *, const char *, unsigned long long *);
using PFN_GetUIntT = int (*)(void *, const char *, unsigned int *);
int g_floatSlot = -1;
int g_floatGetterSlot = -1;

void FindFloatSlot(void *p)
{
    if (g_floatSlot >= 0) return;
    static const int setters[] = {6, 5, 1, 2, 7, 4};
    static const int getters[] = {14, 13, 9, 10, 15, 12};
    const float expected = 0.375f;
    for (int setter : setters) {
        reinterpret_cast<PFN_SetFloatT>(VTable(p)[setter])(p, "PeripheralWarp.FloatProbe", expected);
        for (int getter : getters) {
            float readBack = 0.0f;
            if (GetFloatVia(p, getter, "PeripheralWarp.FloatProbe", &readBack) && readBack == expected) {
                g_floatSlot = setter;
                g_floatGetterSlot = getter;
                return;
            }
        }
    }
    g_floatSlot = 1;
    g_floatGetterSlot = 9;
}

// Forwarder
using PFN_FwdCreate = int(__cdecl *)(PFN_Create, void *, int, void *, void **);
using PFN_FwdEvaluate = int(__cdecl *)(PFN_Evaluate, void *, void *, void *, void *);
using PFN_FwdRelease = int(__cdecl *)(PFN_Release, void *);
PFN_FwdCreate g_fwdCreate = nullptr;
PFN_FwdEvaluate g_fwdEvaluate = nullptr;
PFN_FwdRelease g_fwdRelease = nullptr;

// Queues
struct QueueEntry {
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE event = nullptr;
    UINT64 value = 0;
};
std::mutex g_queueMutex;
std::vector<QueueEntry> g_queues;
bool g_noQueueLogged = false;
bool g_queueRegistrationSuppressed = false;
constexpr DWORD kGpuWaitMilliseconds = 3000;

bool ReadWholeFile(const std::wstring &path, std::vector<char> &out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !out.empty();
}

} // namespace

// ---- logging
void SetLog(LogFn log) { g_log = log; }

void Log(bool warning, const char *fmt, ...)
{
    if (g_log == nullptr) return;
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    g_log(warning, buffer);
}

// ---- parameter block
void **VTable(void *params) { return *reinterpret_cast<void ***>(params); }

void SetUInt(void *p, const char *name, unsigned int v)
{
    reinterpret_cast<PFN_SetUIntT>(VTable(p)[kSetUInt])(p, name, v);
}

bool GetUInt(void *p, const char *name, unsigned int *v)
{
    return reinterpret_cast<PFN_GetUIntT>(VTable(p)[kGetUInt])(p, name, v) == kNgxSuccess;
}

void SetPointer(void *p, const char *name, void *v)
{
    reinterpret_cast<PFN_SetULL>(VTable(p)[kSetPointer])(p, name, reinterpret_cast<unsigned long long>(v));
}

void *GetPointer(void *p, const char *name)
{
    unsigned long long v = 0;
    if (reinterpret_cast<PFN_GetULL>(VTable(p)[kGetPointer])(p, name, &v) != kNgxSuccess) return nullptr;
    return reinterpret_cast<void *>(v);
}

bool GetFloatVia(void *p, int slot, const char *name, float *v)
{
    unsigned long long raw = 0;
    if (reinterpret_cast<PFN_GetULL>(VTable(p)[slot])(p, name, &raw) != kNgxSuccess) return false;
    if ((raw >> 32) == 0) {
        float f = 0.0f;
        std::memcpy(&f, &raw, sizeof(f));
        *v = f;
    } else {
        double d = 0.0;
        std::memcpy(&d, &raw, sizeof(d));
        *v = static_cast<float>(d);
    }
    return true;
}

void SetFloat(void *p, const char *name, float v)
{
    FindFloatSlot(p);
    reinterpret_cast<PFN_SetFloatT>(VTable(p)[g_floatSlot])(p, name, v);
}

bool GetFloat(void *p, const char *name, float *v)
{
    FindFloatSlot(p);
    return GetFloatVia(p, g_floatGetterSlot >= 0 ? g_floatGetterSlot : 9, name, v);
}

int FloatSetterSlot() { return g_floatSlot; }
int FloatGetterSlot() { return g_floatGetterSlot; }

void ProbeKey(void *p, const char *name, char *out, std::size_t outSize)
{
    std::size_t used = 0;
    for (int slot = 8; slot <= 15; ++slot) {
        unsigned long long raw = 0;
        const int rc = reinterpret_cast<PFN_GetULL>(VTable(p)[slot])(p, name, &raw);
        float f = 0.0f;
        double d = 0.0;
        std::memcpy(&f, &raw, sizeof(f));
        std::memcpy(&d, &raw, sizeof(d));
        const int n = std::snprintf(out + used, outSize > used ? outSize - used : 0,
                                    "[%d rc=%d raw=%016llx f=%.4g d=%.4g u=%llu] ", slot, rc, raw, f, d, raw);
        if (n < 0) break;
        used += static_cast<std::size_t>(n);
        if (used >= outSize) break;
    }
}

// ---- forwarder
bool LoadForwarder(const std::wstring &directory, std::string *error)
{
    if (g_fwdCreate != nullptr) return true;
    const std::wstring path = directory + L"\\nvngx.dll_optimizerfps.dll";
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        if (error) *error = "nvngx.dll_optimizerfps.dll is missing beside the add-on";
        return false;
    }
    g_fwdCreate = reinterpret_cast<PFN_FwdCreate>(GetProcAddress(module, "pw_ngx_call_create"));
    g_fwdEvaluate = reinterpret_cast<PFN_FwdEvaluate>(GetProcAddress(module, "pw_ngx_call_evaluate"));
    g_fwdRelease = reinterpret_cast<PFN_FwdRelease>(GetProcAddress(module, "pw_ngx_call_release"));
    if (g_fwdCreate == nullptr || g_fwdEvaluate == nullptr || g_fwdRelease == nullptr) {
        if (error) *error = "nvngx.dll_optimizerfps.dll lacks its exports";
        g_fwdCreate = nullptr;
        g_fwdEvaluate = nullptr;
        g_fwdRelease = nullptr;
        return false;
    }
    return true;
}

bool ForwarderLoaded() { return g_fwdCreate != nullptr; }

int ForwardCreate(PFN_Create real, void *context, int featureId, void *params, void **outHandle)
{
    return g_fwdCreate(real, context, featureId, params, outHandle);
}

int ForwardEvaluate(PFN_Evaluate real, void *context, void *handle, void *params, void *callback)
{
    return g_fwdEvaluate(real, context, handle, params, callback);
}

int ForwardRelease(PFN_Release real, void *handle) { return g_fwdRelease(real, handle); }

// ---- queues
void SetQueueRegistrationSuppressed(bool suppressed) { g_queueRegistrationSuppressed = suppressed; }

void RegisterQueue(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    if (device == nullptr || queue == nullptr || g_queueRegistrationSuppressed) return;
    QueueEntry e;
    e.device = device;
    e.queue = queue;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&e.fence)))) {
        Log(true, "Optimizer FPS NGX: could not create a fence for queue %p; GPU waits unavailable on it", (void *) queue);
        return;
    }
    e.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (e.event == nullptr) {
        e.fence->Release();
        return;
    }
    device->AddRef();
    queue->AddRef();
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_queues.push_back(e);
    }
    Log(false, "Optimizer FPS NGX: D3D12 graphics queue %p (device %p) registered for GPU waits", (void *) queue,
        (void *) device);
}

void UnregisterQueue(ID3D12CommandQueue *queue)
{
    QueueEntry gone{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        for (std::size_t i = 0; i < g_queues.size(); ++i) {
            if (g_queues[i].queue != queue) continue;
            gone = g_queues[i];
            g_queues.erase(g_queues.begin() + static_cast<std::ptrdiff_t>(i));
            found = true;
            break;
        }
    }
    if (!found) return;
    CloseHandle(gone.event);
    gone.fence->Release();
    gone.queue->Release();
    gone.device->Release();
}

// 26.21: a CPU wait that survives a stale event registration. A wait that timed out leaves its
// SetEventOnCompletion armed; when that older value completes the auto-reset event is left signalled
// and the NEXT wait would return WAIT_OBJECT_0 at once, before its own value was reached. So the
// event is reset before it is armed and every wake re-checks the fence, waiting on until the deadline.
bool WaitFenceValue(ID3D12Fence *fence, UINT64 value, HANDLE event, DWORD milliseconds)
{
    if (fence == nullptr) return false;
    if (fence->GetCompletedValue() >= value) return true;
    if (event == nullptr) return false;
    ResetEvent(event);
    if (fence->GetCompletedValue() >= value) return true; // completed while the event was being reset
    if (FAILED(fence->SetEventOnCompletion(value, event))) return false;
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    for (;;) {
        if (fence->GetCompletedValue() >= value) return true;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        const DWORD waited = WaitForSingleObject(event, static_cast<DWORD>(deadline - now));
        if (waited == WAIT_OBJECT_0) continue;  // may be a stale registration: the loop re-checks
        if (waited == WAIT_TIMEOUT) return fence->GetCompletedValue() >= value;
        return false; // WAIT_FAILED / abandoned
    }
}

bool WaitForGpu(ID3D12Device *device, ID3D12Device *proxyDevice)
{
    if (device != nullptr && FAILED(device->GetDeviceRemovedReason())) return false;
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_queues.empty()) {
        if (!g_noQueueLogged) {
            g_noQueueLogged = true;
            Log(true, "Optimizer FPS NGX: no D3D12 queue registered; releases are deferred by %llu evaluates instead of a GPU wait",
                static_cast<unsigned long long>(kDeferredReleaseEvaluates));
        }
        return false;
    }
    auto onDevice = [&](const QueueEntry &e) {
        return (device != nullptr && e.device == device) || (proxyDevice != nullptr && e.device == proxyDevice);
    };
    bool matched = false;
    for (const QueueEntry &e : g_queues) matched |= onDevice(e);
    bool ok = true;
    for (QueueEntry &e : g_queues) {
        if (matched && !onDevice(e)) continue;
        const UINT64 value = ++e.value;
        if (FAILED(e.queue->Signal(e.fence, value))) { ok = false; continue; }
        if (!WaitFenceValue(e.fence, value, e.event, kGpuWaitMilliseconds)) ok = false;
    }
    if (!ok) Log(true, "Optimizer FPS NGX: GPU wait failed or timed out; the release is deferred");
    return ok;
}

bool SignalRegisteredQueues(ID3D12Device *device, ID3D12Device *proxyDevice, ID3D12Fence *fence, UINT64 value)
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    bool any = false;
    for (const QueueEntry &e : g_queues)
        if (e.device == device || e.device == proxyDevice) { any |= SUCCEEDED(e.queue->Signal(fence, value)); }
    return any;
}

void GateSet::Add(ID3D12Fence *fence, UINT64 value)
{
    if (fence == nullptr) return;
    fence->AddRef();
    gates_.push_back(FenceGate{fence, value});
}

void GateSet::Assign(const GateSet &other)
{
    gates_ = other.gates_;
    for (FenceGate &g : gates_) if (g.fence) g.fence->AddRef();
}

void GateSet::Clear()
{
    for (FenceGate &g : gates_) if (g.fence) g.fence->Release();
    gates_.clear();
}

bool GateSet::Completed() const
{
    for (const FenceGate &g : gates_)
        if (g.fence && g.fence->GetCompletedValue() < g.value) return false;
    return true;
}

void GateSet::Wait(DWORD milliseconds) const
{
    if (gates_.empty()) return;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev == nullptr) return;
    for (const FenceGate &g : gates_)
        if (g.fence) WaitFenceValue(g.fence, g.value, ev, milliseconds);
    CloseHandle(ev);
}

GateSet SignalGate(ID3D12Device *device, ID3D12Device *proxyDevice)
{
    GateSet set;
    if (device == nullptr) return set;
    // One fence per queue: a single fence signalled from several queues completes at the max of the
    // values, so the first queue to finish would open the gate for all of them.
    std::vector<ID3D12CommandQueue *> queues;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        for (const QueueEntry &e : g_queues)
            if (e.device == device || e.device == proxyDevice) { e.queue->AddRef(); queues.push_back(e.queue); }
    }
    for (ID3D12CommandQueue *queue : queues) {
        ID3D12Fence *fence = nullptr;
        if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence != nullptr) {
            if (SUCCEEDED(queue->Signal(fence, 1))) set.Add(fence, 1);
            fence->Release();
        }
        queue->Release();
    }
    return set;
}

bool WaitRegisteredQueues(ID3D12Device *device, ID3D12Device *proxyDevice, ID3D12Fence *fence, UINT64 value)
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    bool any = false;
    for (const QueueEntry &e : g_queues)
        if (e.device == device || e.device == proxyDevice) { any |= SUCCEEDED(e.queue->Wait(fence, value)); }
    return any;
}

std::size_t CountQueues(ID3D12Device *device, ID3D12Device *proxyDevice, std::size_t *total)
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    std::size_t matching = 0;
    for (const QueueEntry &e : g_queues) matching += (e.device == device || e.device == proxyDevice) ? 1 : 0;
    if (total) *total = g_queues.size();
    return matching;
}

bool TimestampFrequency(ID3D12Device *device, ID3D12Device *proxyDevice, UINT64 *frequency)
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    const QueueEntry *pick = nullptr;
    for (const QueueEntry &e : g_queues) {
        if (e.device == device || e.device == proxyDevice) { pick = &e; break; }
        if (pick == nullptr) pick = &e;
    }
    if (pick == nullptr) return false;
    return SUCCEEDED(pick->queue->GetTimestampFrequency(frequency));
}

// ---- graveyard
void Graveyard::Add(Grave &&grave, std::uint64_t now)
{
    grave.dueEval = now + kDeferredReleaseEvaluates;
    graves_.push_back(std::move(grave));
}

void Graveyard::Drain(bool everything, std::uint64_t now)
{
    for (std::size_t i = 0; i < graves_.size();) {
        Grave &g = graves_[i];
        if (!everything && g.dueEval > now) { ++i; continue; }
        if (!g.gate.Empty()) {
            // Every gate of the set must be open: one fence per queue that may still read the objects.
            if (!g.gate.Completed()) {
                if (!everything) { ++i; continue; }
                // Teardown: wait once on the CPU (3 s cap) so the passes in flight are over before their objects go.
                g.gate.Wait(kGpuWaitMilliseconds);
            }
            g.gate.Clear();
        }
        g.adapter12.reset();
        g.adapter11.reset();
        g.disposables.clear();
        for (IUnknown *o : g.objects) if (o) o->Release();
        g.objects.clear();
        if (g.realHandle && g.releaseFn) ForwardRelease(g.releaseFn, g.realHandle);
        graves_.erase(graves_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// ---- shaders
pw::ShaderSet Shaders::Set() const
{
    return pw::ShaderSet{{vertex.data(), vertex.size()}, {pack.data(), pack.size()}, {unpack.data(), unpack.size()},
                         {outline.data(), outline.size()}};
}

bool LoadShaders(const std::wstring &addonDirectory, Shaders &s)
{
    if (s.tried) return s.loaded;
    s.tried = true;
    const std::wstring dir = addonDirectory + L"\\optimizer-fps-dlss5\\";
    s.loaded = ReadWholeFile(dir + L"fullscreen_vs.dxbc", s.vertex) && ReadWholeFile(dir + L"pack_ps.dxbc", s.pack) &&
               ReadWholeFile(dir + L"unpack_ps.dxbc", s.unpack);
    ReadWholeFile(dir + L"outline_ps.dxbc", s.outline);
    ReadWholeFile(dir + L"temporal_residual_ps.dxbc", s.temporalResidual);
    ReadWholeFile(dir + L"temporal_accumulate_ps.dxbc", s.temporalAccumulate);
    ReadWholeFile(dir + L"temporal_reproject_ps.dxbc", s.temporalReproject);
    ReadWholeFile(dir + L"temporal_downsample_ps.dxbc", s.temporalDownsample);
    ReadWholeFile(dir + L"temporal_compose_ps.dxbc", s.temporalCompose);
    if (!s.loaded) Log(true, "Optimizer FPS NGX: shaders were not found in optimizer-fps-dlss5\\ beside the add-on");
    return s.loaded;
}

// ---- formats, barriers, textures
DXGI_FORMAT TypedView(DXGI_FORMAT format, bool depth)
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: return depth ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    default: return format;
    }
}

pw::ColorEncoding EncodingFor(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return pw::ColorEncoding::Srgb;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return pw::ColorEncoding::LinearHdr;
    default:
        return pw::ColorEncoding::LinearLdr;
    }
}

void Barrier(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES &tracked,
             D3D12_RESOURCE_STATES to)
{
    if (res == nullptr || tracked == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = tracked;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
    tracked = to;
}

bool PlanarDepthFormat(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return true;
    default: return false;
    }
}

UINT DepthBarrierSubresource(ID3D12Resource *depth)
{
    if (depth == nullptr) return D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    const D3D12_RESOURCE_DESC d = depth->GetDesc();
    return PlanarDepthFormat(d.Format) ? 0u : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
}

void BarrierExternal(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES from,
                     D3D12_RESOURCE_STATES to, UINT subresource)
{
    if (res == nullptr || from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = subresource;
    cmd->ResourceBarrier(1, &b);
}

void BarrierExternal(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES from,
                     D3D12_RESOURCE_STATES to)
{
    BarrierExternal(cmd, res, from, to, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
}

bool CreateTexture(ID3D12Device *device, UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                   ID3D12Resource **out)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(out)));
}

// ---- structured exceptions
LONG RecordCrash(EXCEPTION_POINTERS *info, int stage, CrashInfo &out)
{
    out.code = info != nullptr && info->ExceptionRecord != nullptr ? info->ExceptionRecord->ExceptionCode : 0;
    out.address = info != nullptr && info->ExceptionRecord != nullptr ? info->ExceptionRecord->ExceptionAddress : nullptr;
    out.stage = stage;
    out.module[0] = 0;
    HMODULE module = nullptr;
    if (out.address != nullptr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(out.address), &module)) {
        GetModuleFileNameA(module, out.module, MAX_PATH);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---- export prologue description
bool DescribeExportPrologue(const char *hookName, const char *exportName, void *fn)
{
    const auto *bytes = static_cast<const unsigned char *>(fn);
    if (bytes[0] != 0xE9) return false;
    std::int32_t rel = 0;
    std::memcpy(&rel, bytes + 1, sizeof(rel));
    void *target = static_cast<unsigned char *>(fn) + 5 + rel;
    HMODULE module = nullptr;
    char owner[MAX_PATH] = "?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(target), &module) && GetModuleFileNameA(module, owner, MAX_PATH)) {
        const char *slash = std::strrchr(owner, '\\');
        if (slash) std::memmove(owner, slash + 1, std::strlen(slash + 1) + 1);
    }
    Log(false, "%s: %s already starts with a jump to %p (%s); that hook will run inside ours", hookName, exportName, target,
        owner);
    return true;
}

} // namespace pwngx
