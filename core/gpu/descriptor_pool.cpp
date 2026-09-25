#include "core/gpu/descriptor_pool.h"
#include "core/gpu/queues.h"
namespace ofps::core::gpu
{
DescriptorPool::~DescriptorPool()
{
    Reset(0);
    if (event_)
        CloseHandle(event_);
}
void DescriptorPool::Reset(std::uint32_t count)
{
    for (Slot &s : slots_)
        if (s.fence)
            s.fence->Release();
    slots_.assign(count, Slot{});
    cursor_ = 0;
}
bool DescriptorPool::Passed(const Slot &slot, std::uint64_t evalNow)
{
    if (slot.fence == nullptr)
        return evalNow >= slot.value;
    const UINT64 done = slot.fence->GetCompletedValue();
    return done != UINT64_MAX && done >= slot.value;
}
void DescriptorPool::Tag(Slot &slot, const OfpsFencePoint &use)
{
    if (use.fence)
        use.fence->AddRef();
    if (slot.fence)
        slot.fence->Release();
    slot.fence = use.fence;
    slot.value = use.value;
    slot.ticket = ++ticket_;
}
std::uint32_t DescriptorPool::Acquire(const OfpsFencePoint &use, std::uint64_t evalNow, DWORD waitMs)
{
    const std::uint32_t n = Count();
    if (n == 0)
        return kNone;
    for (std::uint32_t k = 0; k < n; ++k)
    {
        const std::uint32_t i = (cursor_ + k) % n;
        if (!Passed(slots_[i], evalNow))
            continue;
        Tag(slots_[i], use);
        cursor_ = (i + 1) % n;
        return i;
    }
    std::uint32_t oldest = kNone;
    for (std::uint32_t i = 0; i < n; ++i)
    {
        if (slots_[i].fence == nullptr)
            continue;
        if (oldest == kNone || slots_[i].ticket < slots_[oldest].ticket)
            oldest = i;
    }
    if (oldest == kNone)
        return kNone;
    if (event_ == nullptr)
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!WaitFenceValue(slots_[oldest].fence, slots_[oldest].value, event_, waitMs))
        return kNone;
    Tag(slots_[oldest], use);
    cursor_ = (oldest + 1) % n;
    return oldest;
}
std::uint32_t DescriptorPool::InFlight(std::uint64_t evalNow) const
{
    std::uint32_t count = 0;
    for (const Slot &s : slots_)
        if (!Passed(s, evalNow))
            ++count;
    return count;
}
} // namespace ofps::core::gpu
