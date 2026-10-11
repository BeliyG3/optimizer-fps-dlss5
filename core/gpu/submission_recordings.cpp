#include "core/gpu/submission.h"
#include "core/context.h"
namespace ofps::core::gpu {
OfpsFencePoint Submission::UsePoint(ID3D12CommandList *list)
{
    const auto evalNow = Ctx().evalCounter;
    // Allocation/tag failure uses the same deferred lifetime as eval-tagged slots.
    OfpsFencePoint point{sizeof(OfpsFencePoint), nullptr, evalNow + kDeferredReleaseEvaluates};
    if (!list) return point;
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t tag = 0, previous = 0;
    UINT size = sizeof(tag);
    // One recording per list and evaluate. A list that comes back in a later evaluate still unclaimed gets a new one
    // that chains the old: a submission claims both (several evaluates on one list), and a list submitted where no
    // queue event sees it (renodx's Present hook point records into its own ring) cancels each recording
    // kDeferredReleaseEvaluates after ITS evaluate, not after the first one's: a shared recording freed the last
    // evaluates' descriptors one or two evaluates after their use.
    if (SUCCEEDED(list->GetPrivateData(kSubmissionTag, &size, &tag)) && size == sizeof(tag)) {
        for (const Recording &r : recordings_) {
            if (r.tag != tag || r.serial != 0 || r.signalled) continue;
            if (r.createdEval == evalNow) return {sizeof(point), r.fence, 1};
            previous = r.tag;
            break;
        }
    }
    Recording r;
    r.createdEval = evalNow;
    r.previousTag = previous;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&r.device)))) return point;
    if (FAILED(r.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&r.fence)))) {
        r.device->Release();
        return point;
    }
    r.tag = ++nextTag_;
    if (FAILED(list->SetPrivateData(kSubmissionTag, sizeof(r.tag), &r.tag))) {
        r.fence->Release();
        r.device->Release();
        return point;
    }
    recordings_.push_back(r);
    return {sizeof(point), r.fence, 1};
}

// Called under mutex_: signals must stay ordered with Untrack and other drains.
void Submission::SignalRecordings(const Tracked &queue, std::uint64_t target)
{
    for (Recording &r : recordings_)
        if (!r.signalled && r.queue == queue.queue && r.serial <= target)
            r.signalled = SUCCEEDED(queue.queue->Signal(r.fence, 1));
}
}
