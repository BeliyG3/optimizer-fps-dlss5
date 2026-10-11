#pragma once

// Debug-only readers of what the GPU produced: the D3D12 debug layer's message queue
// ([PeripheralWarp] DebugLayer=1) and the centre-texel dump of the temporal machine
// (DebugTemporalReadback=1). Neither runs unless its key is set.

#include "core/frame/feature_state.h"

namespace ofps::core {

void DumpDebugLayer(ID3D12Device *device);
void TemporalDebugReadback(FeatureState &st, ID3D12GraphicsCommandList *cmd, const ofps::core::temporal::FrameInputs &tin);
// DebugTemporalDumpAcc=1 (acc_dump.h): on a full pass of temporal mode 1, right after the model's motion is recorded,
// the reasons for its markers are recorded and the model's motion, the chain and the reasons are copied aside; the poll (every carried frame, and before the next record) writes
// ofps_acc_<eval>.bin beside the host executable once the GPU has passed that frame. Never waits for the GPU.
// With DebugTemporalDumpAccFrom (tin.dumpAccFrom > 0) the frames are those of its window (AccDumpWanted), carried ones
// (`fullPass` false, called right after RecordAccumulate) with the chain in both planes and reasons 0.
void TemporalAccDumpRecord(FeatureState &st, ID3D12GraphicsCommandList *cmd, const ofps::core::temporal::FrameInputs &tin,
                           bool fullPass);
void TemporalAccDumpPoll(FeatureState &st);

} // namespace ofps::core
