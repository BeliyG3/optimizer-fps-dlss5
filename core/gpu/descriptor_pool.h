#pragma once
// Not thread-safe: access only under Ctx().mutex.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include "core/api/ofps_core.h"
#include <cstdint>
#include <vector>
namespace ofps::core::gpu { constexpr DWORD kPoolWaitMilliseconds = 100; class DescriptorPool { public: static constexpr std::uint32_t kNone = 0xffffffffu; DescriptorPool() = default; ~DescriptorPool(); DescriptorPool(const DescriptorPool &) = delete; DescriptorPool &operator=(const DescriptorPool &) = delete; void Reset(std::uint32_t count);
std::uint32_t Count() const { return static_cast<std::uint32_t>(slots_.size()); } std::uint32_t Acquire(const OfpsFencePoint &use, std::uint64_t evalNow, DWORD waitMs); std::uint32_t InFlight(std::uint64_t evalNow) const; private: struct Slot { ID3D12Fence *fence = nullptr; std::uint64_t value = 0; std::uint64_t ticket = 0; }; static bool Passed(const Slot &slot, std::uint64_t evalNow); void Tag(Slot &slot, const OfpsFencePoint &use); std::vector<Slot> slots_; std::uint32_t cursor_ = 0; HANDLE event_ = nullptr; std::uint64_t ticket_ = 0; }; }
