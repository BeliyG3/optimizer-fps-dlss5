#include "core/gpu/submission.h"
#include "core/context.h"
#include "core/log.h"
namespace ofps::core::gpu
{
Submission::~Submission()
{
    while (!queues_.empty()) Untrack(queues_.back().queue);
    for (const Recording &r : recordings_) {
        if (r.serial == 0) r.fence->Signal(1); // No submission ever claimed this recording.
        r.fence->Release();
        r.device->Release();
    }
}
Submission::Tracked *Submission::Find(ID3D12CommandQueue *queue)
{
    for (Tracked &t : queues_)
        if (t.queue == queue)
            return &t;
    return nullptr;
}
const Submission::Tracked *Submission::Find(ID3D12CommandQueue *queue) const
{
    for (const Tracked &t : queues_)
        if (t.queue == queue)
            return &t;
    return nullptr;
}
bool Submission::Track(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    if (device == nullptr || queue == nullptr)
        return false;
    Tracked t;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&t.fence))))
        return false;
    t.device = device;
    device->AddRef();
    t.queue = queue;
    queue->AddRef();
    std::lock_guard<std::mutex> lock(mutex_);
    if (Find(queue) != nullptr)
    {
        t.fence->Release();
        t.queue->Release();
        t.device->Release();
        return true;
    }
    queues_.push_back(t);
    return true;
}
void Submission::Untrack(ID3D12CommandQueue *queue)
{
    std::unique_lock<std::mutex> lock(mutex_);
    Tracked *t = Find(queue);
    if (!t) return;
    const Tracked gone = *t;
    // The owned queue reference keeps it alive. A terminal GPU signal also covers
    // outstanding next-serial gates; never CPU-complete work still in flight.
    SignalRecordings(gone, gone.pushed);
    gone.queue->Signal(gone.fence, UINT64_MAX - 1);
    queues_.erase(queues_.begin() + (t - queues_.data()));
    bool sameDevice = false;
    for (const Tracked &other : queues_) sameDevice |= other.device == gone.device;
    if (!sameDevice) {
        for (Recording &r : recordings_)
            if (r.device == gone.device && r.serial == 0)
                r.signalled = SUCCEEDED(r.fence->Signal(1)); // Cancel an unsubmitted list.
    }
    // Remove borrowed identities before the owning queue reference is released.
    std::uint32_t kept = 0;
    for (std::uint32_t i = 0; i < count_; ++i) {
        const auto e = ring_[(head_ + i) % kSubmissionRing];
        if (e.queue != queue) ring_[(head_ + kept++) % kSubmissionRing] = e;
    }
    count_ = kept;
    lock.unlock();
    gone.fence->Release();
    gone.queue->Release();
    gone.device->Release();
}

void Submission::Push(ID3D12CommandQueue *queue, ID3D12CommandList *list)
{
    if (queue == nullptr || list == nullptr)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    Tracked *t = Find(queue);
    if (!t) return;
    SubmissionEntry e;
    e.queue = queue;
    e.list = list;
    e.serial = ++t->pushed;
    UINT size = sizeof(e.asyncTag);
    if (FAILED(list->GetPrivateData(kAsyncKickTag, &size, &e.asyncTag)) || size != sizeof(e.asyncTag))
        e.asyncTag = 0;
    size = sizeof(e.flowTag);
    if (FAILED(list->GetPrivateData(kTemporalFlowTag, &size, &e.flowTag)) || size != sizeof(e.flowTag))
        e.flowTag = 0;
    std::uint64_t tag = 0;
    size = sizeof(tag);
    if (SUCCEEDED(list->GetPrivateData(kSubmissionTag, &size, &tag)) && size == sizeof(tag))
        for (Recording &r : recordings_) if (r.tag == tag && r.serial == 0 && !r.signalled) {
            r.queue = queue;
            r.serial = e.serial;
        }
    if (count_ == kSubmissionRing)
    {
        head_ = (head_ + 1) % kSubmissionRing;
        --count_;
        ++dropped_;
    }
    ring_[(head_ + count_) % kSubmissionRing] = e;
    ++count_;
}
std::uint32_t Submission::Drain(SubmissionEntry *out, std::uint32_t capacity, Signal policy)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint32_t n = 0;
    while (n < capacity && count_ > 0) {
        out[n++] = ring_[head_];
        head_ = (head_ + 1) % kSubmissionRing;
        --count_;
    }
    for (Tracked &t : queues_) {
        const auto target = policy == Signal::All ? t.pushed : t.settled;
        SignalRecordings(t, target);
        if (target > t.signalled) {
            const HRESULT result = t.queue->Signal(t.fence, target);
            if (SUCCEEDED(result)) t.signalled = target;
            else Log(true,
                "Optimizer FPS core: submission signal device error 0x%08lX; serial %llu remains pending",
                static_cast<unsigned long>(result), static_cast<unsigned long long>(target));
        }
        t.settled = t.pushed;
    }
    // Only Drain releases recording references: it runs under Ctx().mutex, so
    // Untrack cannot invalidate a borrowed UsePoint during an evaluate.
    for (auto it = recordings_.begin(); it != recordings_.end();) {
        if (it->serial == 0 && !it->signalled &&
            Ctx().evalCounter - it->createdEval >= kDeferredReleaseEvaluates)
            it->signalled = SUCCEEDED(it->fence->Signal(1)); // Cancel an abandoned recording, as in Untrack.
        if (!it->signalled) { ++it; continue; }
        it->fence->Release();
        it->device->Release();
        it = recordings_.erase(it);
    }
    return n;
}

bool Submission::NextSubmission(ID3D12CommandQueue *queue, OfpsFencePoint *point) const
{
    if (point == nullptr)
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const Tracked *t = queue ? Find(queue) : nullptr;
    if (t == nullptr)
        return false;
    point->size = sizeof(*point);
    point->fence = t->fence;
    point->value = t->pushed + 1;
    return true;
}
void Submission::PendingGate(ID3D12Device *device, ID3D12Device *proxyDevice, GateSet *gate) const
{
    if (gate == nullptr)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Tracked &t : queues_)
    {
        if (t.device != device && t.device != proxyDevice)
            continue;
        if (t.pushed == 0)
            continue;
        gate->Add(t.fence, t.pushed);
    }
    for (const Recording &r : recordings_)
        if (r.device == device || r.device == proxyDevice) gate->Add(r.fence, 1);
}
std::uint64_t Submission::Dropped() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}
void Submission::PendingGateAll(GateSet *gate) const
{
    if (!gate)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Tracked &t : queues_)
        if (t.pushed != 0)
            gate->Add(t.fence, t.pushed);
    for (const Recording &r : recordings_) gate->Add(r.fence, 1);
}
} // namespace ofps::core::gpu
