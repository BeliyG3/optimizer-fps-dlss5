#include "core/flow/Backend.h"
#include "core/flow/OpticalFlow.h"

namespace ofps::core::flow {
namespace {
class Driver final : public Backend {
public:
    void* Acquire(ID3D12Device* d, std::uint32_t w, std::uint32_t h, std::string& reason) override {
        return Session::Acquire(d, w, h, reason);
    }
    std::uint32_t Width(void* s) const override { return static_cast<Session*>(s)->Width(); }
    std::uint32_t Height(void* s) const override { return static_cast<Session*>(s)->Height(); }
    std::uint32_t Grid(void* s) const override { return static_cast<Session*>(s)->Grid(); }
    ID3D12Resource* Now(void* s) const override { return static_cast<Session*>(s)->Now(); }
    ID3D12Resource* Then(void* s) const override { return static_cast<Session*>(s)->Then(); }
    ID3D12Resource* Field(void* s) const override { return static_cast<Session*>(s)->Field(); }
    bool Execute(void* s, ID3D12CommandQueue* q) override { return static_cast<Session*>(s)->Execute(q); }
    void RetainQueue(ID3D12CommandQueue* q) override { q->AddRef(); }
    void ReleaseQueue(ID3D12CommandQueue* q) override { q->Release(); }
};
}
Backend& DriverBackend() { static Driver driver; return driver; }
}
