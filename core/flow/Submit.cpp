#include "core/flow/OpticalFlowInternal.h"

namespace ofps::core::flow {
bool Session::Execute(ID3D12CommandQueue* queue)
{
    Impl& m = *_impl;
    if (queue == nullptr) return false;
    const std::uint64_t ticket = ++m.ticket;
    // The previous frame's image writes must precede the optical flow submission.
    if (FAILED(queue->Signal(m.fenceIn.Get(), ticket))) return false;

    NV_OF_FENCE_POINT ready = {m.fenceIn.Get(), ticket};
    NV_OF_FENCE_POINT done = {m.fenceOut.Get(), ticket};
    NV_OF_EXECUTE_INPUT_PARAMS_D3D12 input{};
    input.inputFrame = m.nowBuffer;
    input.referenceFrame = m.thenBuffer;
    input.disableTemporalHints = NV_OF_TRUE;
    input.numFencePoints = 1;
    input.fencePoint = &ready;
    NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 output{};
    output.outputBuffer = m.fieldBuffer;
    output.fencePoint = &done;
    if (m.api.nvOFExecuteD3D12(m.handle, &input, &output) != NV_OF_SUCCESS) return false;
    // No CPU wait (as in the fork): the field is used a frame later and the next host list
    // waits on the GPU for the engine to signal completion.
    return SUCCEEDED(queue->Wait(m.fenceOut.Get(), ticket));
}
} // namespace ofps::core::flow
