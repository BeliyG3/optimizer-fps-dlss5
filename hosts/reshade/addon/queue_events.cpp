#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reshade.hpp>
#include <d3d12.h>

#include "queue_events.h"

#include "../shell_host.h"
#include "../ngx_hook_api.h"
#include "../readable_guides.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace ofps::reshade {
namespace {

// The NGX interposer waits for the host's D3D12 graphics queue before dropping GPU objects; ReShade
// reports every queue created through its proxy device, natives included.
// 26.7.4: queues are only NOTED while the game creates its device; the fence for GPU waits is created on the
// first present. Death Stranding DC exited within 25 ms of its D3D12 device creation with our add-on loaded and
// nothing else of ours running - the only work we did in that window was this registration on the fresh device.
std::mutex g_pendingQueueMutex;
std::vector<std::pair<ID3D12Device *, ID3D12CommandQueue *>> g_pendingQueues;

} // namespace

void FlushPendingQueues()
{
    if (!Core()) return;
    std::vector<std::pair<ID3D12Device *, ID3D12CommandQueue *>> pending;
    {
        std::lock_guard<std::mutex> lock(g_pendingQueueMutex);
        pending.swap(g_pendingQueues);
    }
    for (const auto &pq : pending) Core()->RegisterQueue(pq.first, pq.second);
}

void OnInitCommandQueue(::reshade::api::command_queue *queue)
{
    ::reshade::api::device *device = queue != nullptr ? queue->get_device() : nullptr;
    if (device == nullptr || device->get_api() != ::reshade::api::device_api::d3d12) return;
    if ((static_cast<std::uint32_t>(queue->get_type()) &
         static_cast<std::uint32_t>(::reshade::api::command_queue_type::graphics)) == 0) return;
    {
        std::lock_guard<std::mutex> lock(g_pendingQueueMutex);
        g_pendingQueues.emplace_back(reinterpret_cast<ID3D12Device *>(device->get_native()),
                                     reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()));
    }
    // 26.7.4: no hook installation from inside the device/queue creation any more. Death Stranding DC
    // died in that window (the snippet had just been loaded by renodx during the game's D3D12 device
    // creation). The hooks go in on the first present; a model created before that is adopted.
}

void OnExecuteCommandList(::reshade::api::command_queue *queue, ::reshade::api::command_list *cmd_list)
{
    if (queue == nullptr || cmd_list == nullptr) return;
    ::reshade::api::device *device = queue->get_device();
    if (device == nullptr || device->get_api() != ::reshade::api::device_api::d3d12) return;
    if (Core()) Core()->OnCommandListExecuted(reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()),
                                  reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list->get_native()));
    ReadableGuidesExecuted(reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()),
                           reinterpret_cast<ID3D12CommandList *>(cmd_list->get_native()));
}

void OnDestroyCommandQueue(::reshade::api::command_queue *queue)
{
    ::reshade::api::device *device = queue != nullptr ? queue->get_device() : nullptr;
    if (device == nullptr || device->get_api() != ::reshade::api::device_api::d3d12) return;
    ReadableGuidesQueueDestroyed(reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()));
    {
        std::lock_guard<std::mutex> lock(g_pendingQueueMutex);
        ID3D12CommandQueue *native = reinterpret_cast<ID3D12CommandQueue *>(queue->get_native());
        g_pendingQueues.erase(std::remove_if(g_pendingQueues.begin(), g_pendingQueues.end(), [native](const auto &pq) { return pq.second == native; }), g_pendingQueues.end());
    }
    if (Core()) Core()->UnregisterQueue(reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()));
}

} // namespace ofps::reshade
