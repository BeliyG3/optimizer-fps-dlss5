#pragma once

// D3D12 queue events forwarded to the core, the readable guides and menu mode (stage 27.D2, moved from producer.cpp).
//
// The interposer waits for the host's D3D12 graphics and compute queues before dropping GPU objects, so it needs
// to know which queues execute the host's command lists. ReShade reports every queue created through
// its proxy device; this module notes them while the game creates its device and hands them over on
// the first present (FlushPendingQueues), never from inside the creation itself - see the comment on
// OnInitCommandQueue in the .cpp.
//
// A host whose D3D12 device ReShade never wraps (OptiScaler's own device in a D3D11 game) gets no such event. For it the
// submit hook (../queue_submit_hook.h) reports the submissions instead: EnsureHostSubmitHook installs it once (MenuMode
// on), and its sinks feed what execute_command_list feeds - menu mode's observer before the native call, the core's and
// the guides' completion tracking after it - and note each new queue (MenuMode on), which the next present hands to
// the core.

struct ID3D12GraphicsCommandList;

namespace reshade::api {
struct command_queue;
struct command_list;
}

namespace ofps::reshade {

// Hands every queue noted since the last call to pw_ngx::RegisterQueue. Call from present.
void FlushPendingQueues();

void OnInitCommandQueue(::reshade::api::command_queue *queue);
void OnExecuteCommandList(::reshade::api::command_queue *queue, ::reshade::api::command_list *cmd_list);
void OnDestroyCommandQueue(::reshade::api::command_queue *queue);

// A host evaluate (off the present path), when menu mode saw the host's recordings go unreported for a while: installs
// the submit hook on the host list's device, once per session (one log line either way).
void EnsureHostSubmitHook(ID3D12GraphicsCommandList *hostList);
// DllMain detach (process exit once the hook pinned the module): silences the hook and restores the vtable entry.
void ShutdownHostSubmitHook();

} // namespace ofps::reshade
