#pragma once
#include <d3d12.h>
#include <cstdint>
#include <string>

namespace ofps::core::flow {
struct Backend {
    virtual ~Backend() = default;
    virtual void* Acquire(ID3D12Device*, std::uint32_t, std::uint32_t, std::string&) = 0;
    virtual std::uint32_t Width(void*) const = 0;
    virtual std::uint32_t Height(void*) const = 0;
    virtual std::uint32_t Grid(void*) const = 0;
    virtual ID3D12Resource* Now(void*) const = 0;
    virtual ID3D12Resource* Then(void*) const = 0;
    virtual ID3D12Resource* Field(void*) const = 0;
    virtual bool Execute(void*, ID3D12CommandQueue*) = 0;
    virtual void RetainQueue(ID3D12CommandQueue*) = 0;
    virtual void ReleaseQueue(ID3D12CommandQueue*) = 0;
};
Backend& DriverBackend();
}
