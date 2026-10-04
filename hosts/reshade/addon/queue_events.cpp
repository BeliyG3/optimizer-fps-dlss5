#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reshade.hpp>
#include <d3d12.h>
#include <wrl/client.h>

#include "queue_events.h"

#include "addon_context.h"
#include "config_store.h"
#include "../shell_host.h"
#include "../ngx_hook_api.h"
#include "../menu_pipeline.h"
#include "../menu_settings.h"
#include "../queue_submit_hook.h"
#include "../readable_guides.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

namespace ofps::reshade {
namespace {

// The NGX interposer waits for the host's D3D12 queues before dropping GPU objects; ReShade reports
// every queue created through its proxy device, natives included. Compute queues count too: a host
// that evaluates Neural Rendering on async compute (DLSS5-Reshade-AIO) records our work there.
// 26.7.4: queues are only NOTED while the game creates its device; the fence for GPU waits is created on the
// first present. Death Stranding DC exited within 25 ms of its D3D12 device creation with our add-on loaded and
// nothing else of ours running - the only work we did in that window was this registration on the fresh device.
// The submit hook's queues are noted the same way (from the host's submit thread) and registered on the next present.
struct PendingQueue {
    ID3D12Device *device;
    ID3D12CommandQueue *queue;
    bool hooked; // seen by the submit hook, not reported by ReShade
};
std::mutex g_pendingQueueMutex; // leaf: only the vectors below are touched under it (and AddRef)
std::vector<PendingQueue> g_pendingQueues;
std::vector<ID3D12CommandQueue *> g_reshadeQueues; // native queues ReShade reported (init .. destroy)
// Queues the submit hook saw outside ReShade, held for the session: the core holds the registered ones anyway, and a
// held pointer is never reused by another queue. Never destroyed from DllMain (Release at process exit is not safe).
auto *g_hookedQueues = new std::vector<Microsoft::WRL::ComPtr<ID3D12CommandQueue>>();
std::atomic<bool> g_hookTried{false};
std::atomic<ULONGLONG> g_hookInstalledAt{0};
std::atomic<bool> g_otherVtable{false}; // a queue the hook saw does not use the swapped vtable (logged at present)

// Completion tracking for one list (native pointers): the core's submission serials and the readable guides' fences.
void ListSubmitted(ID3D12CommandQueue *queue, ID3D12CommandList *list)
{
    if (Core()) Core()->OnCommandListExecuted(queue, list);
    ReadableGuidesExecuted(queue, list);
}

// The submit hook's first sight of a queue ReShade does not report: noted for the core (graphics and compute queues
// only, as OnInitCommandQueue), only while MenuMode is on (fix round 1: the hook exists for menu mode; a queue first
// seen with MenuMode off is noted once it is turned on). The native calls below take no lock of ours.
void NoteHookedQueue(ID3D12CommandQueue *queue)
{
    if (!MenuModeOn()) return;
    {
        std::lock_guard<std::mutex> lock(g_pendingQueueMutex);
        if (std::find(g_reshadeQueues.begin(), g_reshadeQueues.end(), queue) != g_reshadeQueues.end()) return; // registered by ReShade's event
        for (const auto &seen : *g_hookedQueues) if (seen.Get() == queue) return;
        g_hookedQueues->emplace_back(queue);
    }
    if (!SubmitHookCovers(queue)) g_otherVtable.store(true); // reached the hook through a vtable other than the swapped one
    const D3D12_COMMAND_LIST_TYPE type = queue->GetDesc().Type;
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return;
    ID3D12Device *device = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) return;
    device->Release(); // the held queue keeps its device alive
    std::lock_guard<std::mutex> lock(g_pendingQueueMutex);
    g_pendingQueues.push_back({device, queue, true});
}

// The submit hook's sinks (queue_submit_hook.h), on the host's submit thread. Fix round 1 (C1): BEFORE the native call
// only menu mode's observer, whose exit Wait must precede the list on its queue (and whose entry rule already allows for
// the call not having happened yet: an observation counts one present later). AFTER the native call returned the
// completion tracking and the registration: a fence the core or the guides signal on that queue then lands behind the
// list, never ahead of it.
void BeforeHookedExecute(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists)
{
    for (UINT i = 0; i < count; ++i)
        if (lists[i] != nullptr) MenuListExecuting(queue, lists[i]);
}
void AfterHookedExecute(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists)
{
    NoteHookedQueue(queue);
    for (UINT i = 0; i < count; ++i)
        if (lists[i] != nullptr) ListSubmitted(queue, lists[i]);
}

} // namespace

void FlushPendingQueues()
{
    if (g_otherVtable.exchange(false)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LogForNgxHook(true, "Optimizer FPS: a D3D12 queue reached the ExecuteCommandLists hook through another vtable than the one it swapped (logged once)");
        }
    }
    if (!Core()) return;
    std::vector<PendingQueue> pending;
    {
        std::lock_guard<std::mutex> lock(g_pendingQueueMutex);
        pending.swap(g_pendingQueues);
    }
    for (const auto &pq : pending) {
        Core()->RegisterQueue(pq.device, pq.queue);
        if (!pq.hooked) continue;
        char line[256];
        std::snprintf(line, sizeof(line), "Optimizer FPS: D3D12 queue %p (device %p) runs host lists outside ReShade; the submit hook reports it, handed to the core",
                      (void *) pq.queue, (void *) pq.device);
        LogForNgxHook(false, line);
    }
}

void OnInitCommandQueue(::reshade::api::command_queue *queue)
{
    ::reshade::api::device *device = queue != nullptr ? queue->get_device() : nullptr;
    if (device == nullptr || device->get_api() != ::reshade::api::device_api::d3d12) return;
    MenuQueueInitialized(); // the D3D11 menu bridge tells a queue of ReShade's wrapped device by this event
    constexpr std::uint32_t kRecordable =
        static_cast<std::uint32_t>(::reshade::api::command_queue_type::graphics) |
        static_cast<std::uint32_t>(::reshade::api::command_queue_type::compute);
    auto *native = reinterpret_cast<ID3D12CommandQueue *>(queue->get_native());
    std::lock_guard<std::mutex> lock(g_pendingQueueMutex);
    g_reshadeQueues.push_back(native);
    if ((static_cast<std::uint32_t>(queue->get_type()) & kRecordable) == 0) return;
    g_pendingQueues.push_back({reinterpret_cast<ID3D12Device *>(device->get_native()), native, false});
    // 26.7.4: no hook installation from inside the device/queue creation any more. Death Stranding DC
    // died in that window (the snippet had just been loaded by renodx during the game's D3D12 device
    // creation). The hooks go in on the first present; a model created before that is adopted.
}

void OnExecuteCommandList(::reshade::api::command_queue *queue, ::reshade::api::command_list *cmd_list)
{
    if (queue == nullptr || cmd_list == nullptr) return;
    ::reshade::api::device *device = queue->get_device();
    if (device == nullptr || device->get_api() != ::reshade::api::device_api::d3d12) return;
    auto *native = reinterpret_cast<ID3D12CommandQueue *>(queue->get_native());
    auto *list = reinterpret_cast<ID3D12CommandList *>(cmd_list->get_native());
    // ReShade fires this event BEFORE the native call, for all three (unchanged; see the hook's sinks for the split).
    ListSubmitted(native, list);
    // Menu mode: observes the queue the host's NR list runs on, and orders a menu exit list that runs on another queue.
    MenuListExecuting(native, list);
}

void OnDestroyCommandQueue(::reshade::api::command_queue *queue)
{
    ::reshade::api::device *device = queue != nullptr ? queue->get_device() : nullptr;
    if (device == nullptr || device->get_api() != ::reshade::api::device_api::d3d12) return;
    ID3D12CommandQueue *native = reinterpret_cast<ID3D12CommandQueue *>(queue->get_native());
    ReadableGuidesQueueDestroyed(native);
    {
        std::lock_guard<std::mutex> lock(g_pendingQueueMutex);
        g_pendingQueues.erase(std::remove_if(g_pendingQueues.begin(), g_pendingQueues.end(), [native](const auto &pq) { return pq.queue == native; }), g_pendingQueues.end());
        g_reshadeQueues.erase(std::remove(g_reshadeQueues.begin(), g_reshadeQueues.end(), native), g_reshadeQueues.end());
    }
    MenuQueueDestroyed(native);
    if (Core()) Core()->UnregisterQueue(native);
}

void EnsureHostSubmitHook(ID3D12GraphicsCommandList *hostList)
{
    if (hostList == nullptr || !MenuModeOn()) return; // fix round 1: only for menu mode, checked again here
    if (g_hookTried.load()) {
        // Asked again well after the install: the host's lists still go unreported, so its queue does not use the swapped
        // vtable (a debug layer or another wrapper of its own) or submits from a skipped module.
        const ULONGLONG at = g_hookInstalledAt.load();
        static std::atomic<bool> logged{false};
        if (at != 0 && GetTickCount64() - at > 2000 && !logged.exchange(true))
            LogForNgxHook(true, "Optimizer FPS: the ExecuteCommandLists hook is in, but the host's NR lists are still not reported (their "
                                "queue uses another vtable, or submits from a skipped module); menu mode keeps waiting (logged once)");
        return;
    }
    if (g_hookTried.exchange(true)) return;
    ID3D12Device *device = nullptr;
    if (FAILED(hostList->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) return;
    device->Release(); // the host keeps its device alive
    // Submissions the add-on and the core make are reported by their own code; those ReShade's proxies make, by its event.
    HMODULE core = nullptr;
    if (IOfpsCore *c = Core())
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(*reinterpret_cast<void **>(c)), &core); // its vtable lives in the core DLL
    const HMODULE skipped[] = {State().module, core, ::reshade::internal::get_reshade_module_handle()};
    const char *why = nullptr;
    SubmitSinks sinks;
    sinks.before = BeforeHookedExecute;
    sinks.after = AfterHookedExecute;
    const bool installed = SubmitHookInstall(device, sinks, skipped, 3, &why);
    if (installed) g_hookInstalledAt.store(GetTickCount64());
    char line[384];
    if (installed)
        std::snprintf(line, sizeof(line), "Optimizer FPS: no queue event reports the host's NR lists on device %p (a device ReShade does not wrap); "
                      "ExecuteCommandLists hooked (vtable entry) to observe them; the add-on module is pinned for the session", (void *) device);
    else
        std::snprintf(line, sizeof(line), "Optimizer FPS: the ExecuteCommandLists hook for device %p could not be installed (%s); menu mode keeps "
                      "waiting for the host's NR queue", (void *) device, why ? why : "unknown");
    LogForNgxHook(!installed, line);
}

void ShutdownHostSubmitHook()
{
    // Fix round 1 (C2): the module is pinned once the hook is in, so this runs at process exit only (or at an unload
    // before any install): the sinks go silent and the original pointer goes back; nothing to drain.
    SubmitHookRestore();
}

} // namespace ofps::reshade
