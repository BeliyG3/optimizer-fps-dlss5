#pragma once
#include "core/api/ofps_core.h"

namespace ofps::core {
struct FeatureState;
struct CodecFrame {
    OfpsFrameInputs frame{};
    OfpsResource modelColor{}, frameBefore{}, answer{};
    bool identity = true;
};
int SnapshotFrameColor(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                       const OfpsResource &source, OfpsResource &copy);
int BeginCodecFrame(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                    const OfpsFrameInputs &frame, CodecFrame &codec);
int ResolveCodecFrame(FeatureState &st, ID3D12GraphicsCommandList *cmd, CodecFrame &codec);
} // namespace ofps::core
