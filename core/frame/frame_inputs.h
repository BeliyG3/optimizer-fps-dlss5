#pragma once
#include "core/frame/common.h"
#include <cstdint>
namespace ofps::core
{
struct FeatureState;
OfpsResource FrameMotion(const FeatureState &, const OfpsResource &);
void ResolveFrameViews(OfpsFrameInputs &frame);
bool FrameHasModelInputs(const OfpsFrameInputs &frame);
OfpsResource MakeResource(ID3D12Resource *res, DXGI_FORMAT view, const OfpsRect &rect, D3D12_RESOURCE_STATES restState,
                          UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
OfpsModelInputs ModelInputsFrom(const OfpsFrameInputs &frame, std::uint32_t width, std::uint32_t height);
} // namespace ofps::core

namespace ofps::core
{
inline UINT CopySubresource(const OfpsResource &r)
{
    return r.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? 0 : r.subresource;
}
} // namespace ofps::core
