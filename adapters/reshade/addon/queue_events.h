#pragma once

// ReShade device/queue events forwarded to the NGX interposer (stage 27.D2, moved from producer.cpp).
//
// The interposer waits for the host's D3D12 graphics queue before dropping GPU objects, so it needs
// to know which queues execute the host's command lists. ReShade reports every queue created through
// its proxy device; this module notes them while the game creates its device and hands them over on
// the first present (FlushPendingQueues), never from inside the creation itself - see the comment on
// OnInitCommandQueue in the .cpp.

namespace reshade::api {
struct command_queue;
struct command_list;
}

namespace pw_addon {

// Hands every queue noted since the last call to pw_ngx::RegisterQueue. Call from present.
void FlushPendingQueues();

void OnInitCommandQueue(reshade::api::command_queue *queue);
void OnExecuteCommandList(reshade::api::command_queue *queue, reshade::api::command_list *cmd_list);
void OnDestroyCommandQueue(reshade::api::command_queue *queue);

} // namespace pw_addon
