// Task 8 fix round 1 (review I1 / Codex C1): menu mode registers its private queue with the core under the NATIVE
// device, never under the identity the host's recordings are keyed on (the host list's device: ReShade's proxy). The
// core's Submission::Untrack CPU-cancels every unsubmitted recording of a device when the last queue tracked under that
// device goes; the private queue's unregister at a swap chain's teardown must never be that last queue.
//   - control (WARP): the last queue of the recording's own identity cancels it (the hazard of the old keying);
//   - fix (HARDWARE-ONLY CHECK: two device identities need a second adapter; two WARP devices are one object): a queue
//     tracked under another identity leaves the recording pending. Without a hardware adapter it prints "SKIP: ...".
#include "test_core_api_gpu.h"
#include "core/gpu/submission.h"
#include <cstdio>

using ofps::core::gpu::Submission;
using coretest::ComPtr;
namespace {
int failures = 0;
void Check(bool condition, const char *what) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
ComPtr<ID3D12GraphicsCommandList> ClosedList(ID3D12Device *device, ComPtr<ID3D12CommandAllocator> &allocator) {
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(list->Close()))
        return nullptr;
    return list;
}
} // namespace

int main() {
    coretest::WarpDevice proxy; // the identity the host's list (and so its recording) answers GetDevice with
    if (!coretest::CreateWarpDevice(proxy)) { std::fprintf(stderr, "FAIL: WARP device\n"); return 1; }
    Submission submission;
    ComPtr<ID3D12CommandAllocator> allocatorA, allocatorB;
    auto controlList = ClosedList(proxy.device.Get(), allocatorA), hostList = ClosedList(proxy.device.Get(), allocatorB);
    Check(controlList && hostList, "lists");
    if (!controlList || !hostList) return 1;

    const OfpsFencePoint control = submission.UsePoint(controlList.Get()); // recorded, never submitted
    Check(control.fence && control.fence->GetCompletedValue() == 0, "control: the host recording is pending");
    Check(submission.Track(proxy.device.Get(), proxy.queue.Get()), "control: a queue tracked under the recording's identity");
    submission.Untrack(proxy.queue.Get());
    Check(control.fence && control.fence->GetCompletedValue() >= 1,
          "control: the last queue of the recording's identity going away cancels the unsubmitted recording (the old keying)");

    // Task 10 fix round 2: the D3D11 menu bridge reports every private pass itself, and ReShade may report the same list
    // too. An exact duplicate report (same queue, same list) is harmless: the recording is claimed once, both entries
    // name the one real submission, and the recording is signalled on that queue behind the list.
    {
        ComPtr<ID3D12CommandAllocator> allocatorC;
        auto passList = ClosedList(proxy.device.Get(), allocatorC);
        Check(passList != nullptr, "duplicate: list");
        const OfpsFencePoint recorded = passList ? submission.UsePoint(passList.Get()) : OfpsFencePoint{};
        Check(recorded.fence && recorded.fence->GetCompletedValue() == 0, "duplicate: the pass's recording is pending");
        Check(submission.Track(proxy.device.Get(), proxy.queue.Get()), "duplicate: the private queue tracked");
        if (passList && recorded.fence) {
            const ComPtr<ID3D12Fence> held = recorded.fence; // borrowed until the next Drain, which releases its reference
            ID3D12CommandList *lists[] = {passList.Get()};
            proxy.queue->ExecuteCommandLists(1, lists);
            submission.Push(proxy.queue.Get(), passList.Get()); // the pipeline's report
            submission.Push(proxy.queue.Get(), passList.Get()); // the duplicate (ReShade's)
            ofps::core::gpu::SubmissionEntry entries[ofps::core::gpu::kSubmissionRing];
            const std::uint32_t count = submission.Drain(entries, ofps::core::gpu::kSubmissionRing, Submission::Signal::All);
            Check(count == 2 && entries[0].list == passList.Get() && entries[1].list == passList.Get() &&
                      entries[0].queue == proxy.queue.Get() && entries[1].queue == proxy.queue.Get(),
                  "duplicate: two entries naming the one submission");
            Check(coretest::WaitForQueue(proxy.device.Get(), proxy.queue.Get()), "duplicate: queue idle");
            Check(held->GetCompletedValue() == 1, "duplicate: the recording is signalled once, behind the list");
            Check(submission.Dropped() == 0, "duplicate: nothing dropped");
        }
        submission.Untrack(proxy.queue.Get());
    }

    coretest::WarpDevice native;
    if (!coretest::CreateWarpDevice(native, true)) {
        std::printf("SKIP: the second device identity needs a hardware D3D12 adapter (hardware-only check)\n");
    } else {
        const OfpsFencePoint pending = submission.UsePoint(hostList.Get());
        Check(pending.fence && pending.fence->GetCompletedValue() == 0, "fix: a new host recording is pending");
        // The private queue, keyed on the native identity, registered and unregistered (FinishGpu at destroy_swapchain).
        Check(submission.Track(native.device.Get(), native.queue.Get()), "fix: the private queue tracked under the native identity");
        submission.Untrack(native.queue.Get());
        Check(pending.fence && pending.fence->GetCompletedValue() == 0,
              "fix: unregistering a queue of another identity leaves the host's unsubmitted recording pending");
        Check(!coretest::HasDebugErrors(native.device.Get()), "no debug-layer errors on the native device");
    }
    Check(!coretest::HasDebugErrors(proxy.device.Get()), "no debug-layer errors");
    if (failures == 0) std::printf("submission identity: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
