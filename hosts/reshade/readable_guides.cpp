#include "hosts/reshade/readable_guides.h"

#include "hosts/reshade/ngx_params.h"
#include "hosts/reshade/shell_host.h"
#include "hosts/reshade/direct_host.h"
#include "hosts/reshade/addon/addon_context.h"
#include "core/api/ofps_settings_schema.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <vector>
#include <cstdio>

namespace ofps::reshade {

DXGI_FORMAT ReadableTwinFormat(DXGI_FORMAT format, bool depth)
{
    switch (format) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16_TYPELESS: return depth ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

namespace {
constexpr D3D12_RESOURCE_STATES kReadable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr unsigned kMaxTwinsPerKind = 4;
constexpr GUID kReadableGuideTag = {0xd38a670b, 0x6680, 0x44ef, {0x97, 0x58, 0x61, 0x0d, 0xf2, 0xcb, 0x38, 0x44}};

struct Twin {
    ID3D12Resource *resource = nullptr;
    ID3D12Device *device = nullptr;
    ID3D12Fence *fence = nullptr;
    std::uint64_t listTag = 0;
    ID3D12CommandQueue *queue = nullptr;       // owned while waiting to signal after submission
    DXGI_FORMAT sourceFormat = DXGI_FORMAT_UNKNOWN;
    UINT64 width = 0;
    UINT height = 0;
    UINT64 fenceValue = 0;
    unsigned kind = 0;
    bool pending = false;
    bool submitted = false;
    bool retired = false;
    bool gateFailed = false;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

std::mutex g_mutex;
std::vector<std::unique_ptr<Twin>> g_twins;
bool g_limitLogged[2] = {};
std::set<std::tuple<unsigned, DXGI_FORMAT, UINT64, UINT>> g_createFailures;
std::uint64_t g_nextListTag = 0;

std::uint64_t ListTag(ID3D12CommandList *list, bool create)
{
    std::uint64_t tag = 0;
    UINT size = sizeof(tag);
    if (SUCCEEDED(list->GetPrivateData(kReadableGuideTag, &size, &tag)) && size == sizeof(tag) && tag) return tag;
    if (!create) return 0;
    tag = ++g_nextListTag;
    return SUCCEEDED(list->SetPrivateData(kReadableGuideTag, sizeof(tag), &tag)) ? tag : 0;
}

void LogCopy(const char *guide, const D3D12_RESOURCE_DESC &desc, DXGI_FORMAT format)
{
    char text[256];
    std::snprintf(text, sizeof(text), "Optimizer FPS NGX hook: the host's %s guide (fmt %d, %llux%u) is not readable by the model as it is; the model gets a copy in fmt %d",
        guide, static_cast<int>(desc.Format), static_cast<unsigned long long>(desc.Width), desc.Height,
        static_cast<int>(format));
    Host().Log(OFPS_LOG_INFO, text);
}

void Barrier(ID3D12GraphicsCommandList *cmd, ID3D12Resource *resource,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, UINT subresource)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = subresource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &barrier);
}

// Matches HostDepthState's diagnostic override. ReadFrameInputs supplies kReadable for the
// regular path; the shell must use the same override before it calls into the core.
// Known limitation of the diagnostic key: the core also applies DebugDepthState 2/3/4 to the
// readable copy it receives (which always rests in NPSR). The key is off by default and only
// meant for games whose own depth state is unknown, so this is accepted rather than adding a
// shell-to-core side channel next to the frozen ABI.
D3D12_RESOURCE_STATES DepthState()
{
    const auto &values = DirectHostActive() && State().directValuesReady ? State().directValues : State().values;
    switch (values.v[OFPS_SET_DEBUG_DEPTH_STATE].i) {
    case 1: return kReadable;
    case 2: return D3D12_RESOURCE_STATE_DEPTH_READ | kReadable;
    case 3: return D3D12_RESOURCE_STATE_GENERIC_READ;
    case 4: return D3D12_RESOURCE_STATE_COMMON;
    default: return kReadable;
    }
}

bool Complete(const Twin &t)
{
    if (t.pending || t.submitted || t.gateFailed) return false;
    if (t.fenceValue == 0) return true;
    const UINT64 done = t.fence->GetCompletedValue();
    return done != std::numeric_limits<UINT64>::max() && done >= t.fenceValue;
}

void ReleaseTwin(Twin &t)
{
    if (t.resource) t.resource->Release();
    if (t.queue) t.queue->Release();
    if (t.fence) t.fence->Release();
    if (t.device) t.device->Release();
    t.resource = nullptr;
}

void SignalSubmitted(ID3D12CommandQueue *queue)
{
    for (auto &t : g_twins) {
        if (!t->submitted || t->queue != queue) continue;
        ++t->fenceValue;
        if (FAILED(queue->Signal(t->fence, t->fenceValue))) {
            t->gateFailed = true;
            Host().Log(OFPS_LOG_WARN, "Optimizer FPS NGX hook: readable guide retirement fence failed");
        }
        t->submitted = false;
        t->queue->Release();
        t->queue = nullptr;
    }
}

// The core takes its own reference and its graveyard also accounts for already submitted work.
// Without a core, the shell's fence retains the entire twin until the queue completes it.
void DrainRetired()
{
    for (auto it = g_twins.begin(); it != g_twins.end();) {
        Twin &t = **it;
        if (!t.retired || t.pending || t.submitted) { ++it; continue; }
        if (Core() && !t.gateFailed) {
            OfpsFencePoint point{sizeof(point), t.fence, t.fenceValue};
            Core()->RetireResource(t.resource, t.fenceValue ? &point : nullptr);
            ReleaseTwin(t);
            it = g_twins.erase(it);
        } else if (Complete(t)) {
            ReleaseTwin(t);
            it = g_twins.erase(it);
        } else {
            ++it;
        }
    }
}

Twin *CreateTwin(ID3D12Device *device, const D3D12_RESOURCE_DESC &source, DXGI_FORMAT format, unsigned kind)
{
    auto twin = std::make_unique<Twin>();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = source;
    desc.Format = format;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    desc.MipLevels = 1;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&twin->resource))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&twin->fence)))) {
        ReleaseTwin(*twin);
        return nullptr;
    }
    device->AddRef();
    twin->device = device;
    twin->sourceFormat = source.Format;
    twin->width = source.Width;
    twin->height = source.Height;
    twin->kind = kind;
    Twin *result = twin.get();
    g_twins.push_back(std::move(twin));
    LogCopy(kind == 0 ? "depth" : "motion", source, format);
    return result;
}

ID3D12Resource *Refresh(ID3D12GraphicsCommandList *cmd, ID3D12Resource *source,
                        unsigned kind, std::uint64_t listTag)
{
    const D3D12_RESOURCE_DESC sd = source->GetDesc();
    if (sd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sd.DepthOrArraySize != 1 ||
        sd.SampleDesc.Count != 1) return nullptr;
    const DXGI_FORMAT format = ReadableTwinFormat(sd.Format, kind == 0);
    if (format == DXGI_FORMAT_UNKNOWN) return nullptr;
    ID3D12Device *device = nullptr;
    if (FAILED(source->GetDevice(IID_PPV_ARGS(&device)))) return nullptr;
    for (auto &t : g_twins)
        if (t->kind == kind && (t->device != device || t->sourceFormat != sd.Format ||
            t->width != sd.Width || t->height != sd.Height)) t->retired = true;
    DrainRetired();
    Twin *twin = nullptr;
    for (auto &t : g_twins)
        if (!t->retired && t->kind == kind && t->device == device &&
            t->sourceFormat == sd.Format && t->width == sd.Width && t->height == sd.Height && Complete(*t)) {
            twin = t.get(); break;
        }
    if (!twin) {
        const auto count = std::count_if(g_twins.begin(), g_twins.end(),
            [kind](const auto &t) { return t->kind == kind; });
        const bool capped = count >= kMaxTwinsPerKind;
        if (!capped) {
            twin = CreateTwin(device, sd, format, kind);
        } else {
            // An unsubmitted list has no fence to wait on. Re-record the oldest matching
            // slot: on the same queue, each copy still precedes its evaluate's model read.
            for (auto &t : g_twins)
                if (t->kind == kind && t->pending && t->device == device &&
                    t->sourceFormat == sd.Format && t->width == sd.Width && t->height == sd.Height) {
                    twin = t.get(); twin->retired = false; break;
                }
            if (!g_limitLogged[kind]) {
                g_limitLogged[kind] = true;
                Host().Log(OFPS_LOG_WARN, "Optimizer FPS NGX hook: readable guide copy limit reached; reusing oldest compatible copy when available");
            }
        }
        if (!twin && !capped && g_createFailures.emplace(kind, sd.Format, sd.Width, sd.Height).second)
            Host().Log(OFPS_LOG_WARN, "Optimizer FPS NGX hook: readable guide copy could not be created");
    }
    device->Release();
    if (!twin) return nullptr;
    const UINT sourceSub = DepthPlaneSubresource(sd.Format);
    const UINT twinSub = DepthPlaneSubresource(format);
    const D3D12_RESOURCE_STATES sourceState = kind == 0 ? DepthState() : kReadable;
    const D3D12_RESOURCE_STATES twinState = twin->state;
    Barrier(cmd, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceSub);
    Barrier(cmd, twin->resource, twinState, D3D12_RESOURCE_STATE_COPY_DEST, twinSub);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = twin->resource; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = source; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(cmd, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState, sourceSub);
    Barrier(cmd, twin->resource, D3D12_RESOURCE_STATE_COPY_DEST, kReadable, twinSub);
    twin->state = kReadable;
    twin->pending = true;
    twin->listTag = listTag;
    return twin->resource;
}
} // namespace

ReadableGuides::ReadableGuides(ID3D12GraphicsCommandList *cmd, void *params) : params_(params)
{
    if (!cmd || !params) return;
    std::lock_guard lock(g_mutex);
    DrainRetired();
    std::uint64_t tag = 0;
    constexpr const char *names[] = {"DLSSNR.Depth", "DLSSNR.MVec"};
    for (unsigned i = 0; i < 2; ++i) {
        ID3D12Resource *source = GetResource(params, names[i]);
        if (!source) continue;
        if (ReadableTwinFormat(source->GetDesc().Format, i == 0) == DXGI_FORMAT_UNKNOWN) continue;
        if (!tag) tag = ListTag(cmd, true);
        if (!tag) break;
        ID3D12Resource *twin = Refresh(cmd, source, i, tag);
        if (!twin) continue;
        SetResource(params, names[i], twin);
        swaps_[count_++] = {names[i], source};
    }
}

ReadableGuides::~ReadableGuides()
{
    if (!count_) return;
    std::lock_guard lock(g_mutex);
    while (count_) { const Swap &swap = swaps_[--count_]; SetResource(params_, swap.name, swap.original); }
}

void ReadableGuidesExecuted(ID3D12CommandQueue *queue, ID3D12CommandList *list)
{
    std::lock_guard lock(g_mutex);
    const std::uint64_t tag = ListTag(list, false);
    if (!tag) return;
    for (auto &t : g_twins) {
        if (!t->pending || t->listTag != tag) continue;
        t->pending = false;
        t->listTag = 0;
        t->queue = queue;
        queue->AddRef();
        t->submitted = true;
    }
    DrainRetired();
}

void ReadableGuidesPresented()
{
    std::lock_guard lock(g_mutex);
    // ReShade invokes execute_command_list before the native ExecuteCommandLists call. By present,
    // that call has returned; signalling here puts the fence after the model's GPU work.
    std::vector<ID3D12CommandQueue *> queues;
    for (const auto &t : g_twins) if (t->submitted &&
        std::find(queues.begin(), queues.end(), t->queue) == queues.end()) queues.push_back(t->queue);
    for (auto *queue : queues) SignalSubmitted(queue);
    DrainRetired();
}

void ReadableGuidesQueueDestroyed(ID3D12CommandQueue *queue)
{
    std::lock_guard lock(g_mutex);
    SignalSubmitted(queue);
    // A queue's native device may differ in COM identity from the ReShade proxy device used
    // to create a twin. Retire every idle slot; each slot's own fence still gates release.
    for (auto &t : g_twins) if (!t->pending) t->retired = true;
    DrainRetired();
}

void ShutdownReadableGuides()
{
    std::lock_guard lock(g_mutex);
    std::vector<ID3D12CommandQueue *> queues;
    for (const auto &t : g_twins) if (t->submitted &&
        std::find(queues.begin(), queues.end(), t->queue) == queues.end()) queues.push_back(t->queue);
    for (auto *queue : queues) SignalSubmitted(queue);
    for (auto &t : g_twins) {
        if (!t->pending && !Complete(*t) && t->fence && t->fenceValue) {
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (event) {
                if (SUCCEEDED(t->fence->SetEventOnCompletion(t->fenceValue, event))) WaitForSingleObject(event, 3000);
                CloseHandle(event);
            }
        }
        if (t->pending || !Complete(*t)) {
            Host().Log(OFPS_LOG_WARN, "Optimizer FPS NGX hook: readable guide still awaits GPU completion at unload");
            continue;
        }
        ReleaseTwin(*t);
    }
    g_twins.clear();
}

} // namespace ofps::reshade
