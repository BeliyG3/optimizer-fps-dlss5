#pragma once
#include "core/gpu/queues.h"
#include "core/api/ofps_core.h"
#include "sdk/adapters/d3d12/d3d12_adapter.h"
#include <cstdint>
#include <memory>
#include <vector>
namespace ofps::core::gpu {
struct Disposable {
    virtual ~Disposable() = default;
};
struct Grave {
    std::unique_ptr<ofps::sdk::D3D12Adapter> adapter12;
    std::vector<std::unique_ptr<Disposable>> disposables;
    std::vector<IUnknown *> objects;
    void *modelHandle = nullptr;
    IOfpsModelHost *modelHost = nullptr;
    std::uint64_t dueEval = 0;
    bool timeoutLogged = false;
    GateSet gate;
};
class Graveyard {
public:
    void Add(Grave &&grave, std::uint64_t now);
    void Drain(bool everything, std::uint64_t now, DWORD waitMs = kGpuWaitMilliseconds);
    bool References(const IOfpsModelHost *host) const;
    std::size_t Size() const { return graves_.size(); }
    bool Empty() const { return graves_.empty(); }

private:
    std::vector<Grave> graves_;
};
} // namespace ofps::core::gpu
