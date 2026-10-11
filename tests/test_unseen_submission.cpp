// A host list the core records into but no queue event reports submitted (renodx 8.5's NRHookPoint=2 and the SF build's
// DirectNeuralRenderingHookStage=3 record NR into their own Present ring and submit it themselves). Its recordings get no
// GPU fence; each one is cancelled kDeferredReleaseEvaluates after ITS evaluate, so a descriptor slot it tags is freed
// that many evaluates after its use, never earlier, and the temporal pool holds that window without running dry.
//   - per use: the list comes back every evaluate; the slot of evaluate 10 stays in flight until evaluate 10 + 16
//     (before the fix the first evaluate's recording served them all and freed the later uses an evaluate or two after);
//   - capacity: kTableSlots tables at the temporal mode's rate never exhaust (before the fix: frame 17 in the bench);
//   - claimed: two evaluates on one list before a reported submission are both released by that submission's fence.
#include "test_core_api_gpu.h"
#include "core/context.h"
#include "core/gpu/descriptor_pool.h"
#include "core/gpu/submission.h"
#include "core/temporal/resources.h"
#include <cstdio>

using coretest::ComPtr;
using ofps::core::Ctx;
using ofps::core::gpu::DescriptorPool;
using ofps::core::gpu::kDeferredReleaseEvaluates;
using ofps::core::gpu::Submission;
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
// What Feature::Evaluate does before recording: drain the reported submissions, then count the evaluate.
void NextEvaluate(Submission &submission) {
    ofps::core::gpu::SubmissionEntry entries[ofps::core::gpu::kSubmissionRing];
    submission.Drain(entries, ofps::core::gpu::kSubmissionRing, Submission::Signal::All);
    ++Ctx().evalCounter;
}

void TestPerUse(ID3D12GraphicsCommandList *list) {
    Submission submission;
    DescriptorPool pool;
    pool.Reset(64);
    constexpr std::uint64_t kWatched = 10;
    ComPtr<ID3D12Fence> watched;
    std::uint64_t watchedValue = 0, freedAt = 0;
    bool exhausted = false;
    for (std::uint64_t e = 1; e <= 40; ++e) {
        NextEvaluate(submission);
        if (watched && freedAt == 0 && watched->GetCompletedValue() >= watchedValue) freedAt = e;
        const OfpsFencePoint use = submission.UsePoint(list);
        for (int k = 0; k < 2; ++k) exhausted |= pool.Acquire(use, Ctx().evalCounter, 0) == DescriptorPool::kNone;
        if (e == kWatched) { watched = use.fence; watchedValue = use.value; }
    }
    Check(watched != nullptr, "per use: the unsubmitted list's evaluate has a recording fence");
    Check(freedAt >= kWatched + kDeferredReleaseEvaluates,
          "per use: an evaluate's recording is not freed before kDeferredReleaseEvaluates evaluates after its own use");
    Check(freedAt != 0 && freedAt <= kWatched + kDeferredReleaseEvaluates + 1, "per use: and is freed by then");
    Check(!exhausted, "per use: two slots per evaluate never exhaust 64");
    Check(pool.InFlight(Ctx().evalCounter) <= 2 * (kDeferredReleaseEvaluates + 1), "per use: at most the window is in flight");
}

void TestCapacity(ID3D12GraphicsCommandList *list) {
    Submission submission;
    DescriptorPool pool;
    pool.Reset(ofps::core::temporal::kTableSlots);
    constexpr int kPerEvaluate = 16; // the synchronous temporal mode's tables per evaluate, with a margin (bench: 15)
    bool exhausted = false;
    for (int e = 0; e < 64; ++e) {
        NextEvaluate(submission);
        const OfpsFencePoint use = submission.UsePoint(list);
        for (int k = 0; k < kPerEvaluate; ++k) exhausted |= pool.Acquire(use, Ctx().evalCounter, 0) == DescriptorPool::kNone;
    }
    Check(!exhausted, "capacity: the temporal table pool holds the fence-less release window");
}

void TestClaimed(coretest::WarpDevice &w, ID3D12GraphicsCommandList *list) {
    Submission submission;
    Check(submission.Track(w.device.Get(), w.queue.Get()), "claimed: the queue is tracked");
    NextEvaluate(submission);
    const ComPtr<ID3D12Fence> a = submission.UsePoint(list).fence;
    NextEvaluate(submission);
    const ComPtr<ID3D12Fence> b = submission.UsePoint(list).fence;
    Check(a && b && a != b, "claimed: one recording per evaluate");
    ID3D12CommandList *lists[] = {list};
    w.queue->ExecuteCommandLists(1, lists);
    submission.Push(w.queue.Get(), list);
    NextEvaluate(submission);
    Check(coretest::WaitForQueue(w.device.Get(), w.queue.Get()), "claimed: queue idle");
    Check(a && b && a->GetCompletedValue() >= 1 && b->GetCompletedValue() >= 1,
          "claimed: the reported submission releases both evaluates' recordings on its fence");
    submission.Untrack(w.queue.Get());
}
} // namespace

int main() {
    coretest::WarpDevice w;
    if (!coretest::CreateWarpDevice(w)) { std::fprintf(stderr, "FAIL: WARP device\n"); return 1; }
    ComPtr<ID3D12CommandAllocator> allocatorA, allocatorB, allocatorC;
    auto listA = ClosedList(w.device.Get(), allocatorA), listB = ClosedList(w.device.Get(), allocatorB);
    auto listC = ClosedList(w.device.Get(), allocatorC);
    Check(listA && listB && listC, "lists");
    if (!listA || !listB || !listC) return 1;
    TestPerUse(listA.Get());
    TestCapacity(listB.Get());
    TestClaimed(w, listC.Get());
    Check(!coretest::HasDebugErrors(w.device.Get()), "no debug-layer errors");
    if (failures == 0) std::printf("unseen submission: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
