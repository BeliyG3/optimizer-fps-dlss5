#include "core/flow/FrameState.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace ofps::core::flow;
namespace {
struct Fake final : Backend {
    int creates = 0, executes = 0, retained = 0;
    bool createOk = true, executeOk = true;
    void* Acquire(ID3D12Device*, std::uint32_t, std::uint32_t, std::string& reason) override {
        ++creates;
        if (!createOk) { reason = "nvofapi64.dll is missing"; return nullptr; }
        return this;
    }
    std::uint32_t Width(void*) const override { return 32; }
    std::uint32_t Height(void*) const override { return 32; }
    std::uint32_t Grid(void*) const override { return 2; }
    ID3D12Resource* Now(void*) const override { return nullptr; }
    ID3D12Resource* Then(void*) const override { return nullptr; }
    ID3D12Resource* Field(void*) const override { return nullptr; }
    bool Execute(void*, ID3D12CommandQueue*) override { ++executes; return executeOk; }
    void RetainQueue(ID3D12CommandQueue*) override { ++retained; }
    void ReleaseQueue(ID3D12CommandQueue*) override { --retained; }
};
}

int main() {
    Fake backend;
    auto* queue = reinterpret_cast<ID3D12CommandQueue*>(&backend); // identity only; fake never dereferences it
    {
        FrameState flow(&backend);
        assert(flow.Acquire(nullptr, 64, 64));
        assert(flow.Acquire(nullptr, 64, 64) && backend.creates == 1);
        flow.NowWritten(true);
        assert(flow.ArmTag() == 0); // no residual reference yet
        flow.ThenWritten(true);
        const auto first = flow.ArmTag();
        assert(first != 0 && !flow.Ready());
        flow.Submitted(first + 1, queue);
        assert(!flow.Ready());
        flow.Submitted(first, queue);
        assert(flow.Ready() && backend.retained == 1);
        assert(flow.Execute() && backend.executes == 1);
        flow.Consumed();
        assert(!flow.Ready() && backend.retained == 0);
        flow.Reset(); // switching to game vectors discards the old frame and its queue
        assert(!flow.Ready());
        assert(flow.Acquire(nullptr, 64, 64) && backend.creates == 1);
        flow.NowWritten(true); flow.ThenWritten(true);
        const auto second = flow.ArmTag();
        assert(second != first);
        flow.Submitted(first, queue);
        assert(!flow.Ready());
        flow.Submitted(second, queue);
        backend.executeOk = false; // fence timeout or driver failure
        assert(!flow.Execute());
        flow.Disable("optical flow fence timed out");
        assert(!flow.Acquire(nullptr, 64, 64) && !flow.Ready());
    }
    assert(backend.retained == 0);
    Fake missing;
    missing.createOk = false;
    FrameState flow(&missing);
    assert(!flow.Acquire(nullptr, 64, 64));
    assert(flow.Problem() == "nvofapi64.dll is missing");
}
