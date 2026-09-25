#pragma once

#include "core/api/ofps_core.h"

namespace ofps::core {
struct FeatureState;
struct CodecFrame;

bool PassesPending(FeatureState &st);
int CreationFrame(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                  const OfpsFrameInputs &frame, OfpsEvalResult &result);
int ModelGridFallback(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                      const OfpsFrameInputs &frame, const char *reason, int result);
void ModelGridInputs(const FeatureState &st, const CodecFrame &codec,
                     const OfpsFrameInputs &frame, OfpsModelInputs &inputs);
void ModelGridInitialInputs(const FeatureState &st, const CodecFrame &codec, OfpsModelInputs &inputs);
int RunModelGridDirect(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                       const OfpsFrameInputs &frame, CodecFrame &codec, OfpsModelInputs &inputs);
int RunUnfitModelPath(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                      const OfpsFrameInputs &frame, CodecFrame &codec, OfpsModelInputs &inputs);
int RunPlainModelPath(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                      const OfpsFrameInputs &frame, CodecFrame &codec, OfpsModelInputs &inputs);
void ReportModelGridPlan7Limit(const FeatureState &st);
} // namespace ofps::core
