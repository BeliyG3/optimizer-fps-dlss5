#include "device.h"

void Device::Timestamp(unsigned index)
{
    if(!timestamps) {
        D3D12_QUERY_HEAP_DESC desc{}; desc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP; desc.Count=7;
        Check(gpu->CreateQueryHeap(&desc,IID_PPV_ARGS(&timestamps)),"Create timestamp heap");
        timingReadback=Buffer(7*sizeof(UINT64),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        Check(queue->GetTimestampFrequency(&timestampFrequency),"Get timestamp frequency");
        if(!timestampFrequency) throw std::runtime_error("GPU timestamp frequency is zero");
    }
    list->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index);
}
void Device::ResolveTimings()
{
    list->ResolveQueryData(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,7,timingReadback.Get(),0);
}
void Device::CollectTimings()
{
    // Submit waits for the fence, so all timestamps belong to the completed frame.
    void *data=nullptr; D3D12_RANGE range{0,7*sizeof(UINT64)};
    Check(timingReadback->Map(0,&range,&data),"Map GPU timings");
    const auto *ticks=static_cast<const UINT64 *>(data);
    lastFrameMs=double(ticks[3]-ticks[0])*1000/double(timestampFrequency);
    lastTraceMs=double(ticks[2]-ticks[1])*1000/double(timestampFrequency);
    lastUpscalerMs=double(ticks[4]-ticks[2])*1000/double(timestampFrequency);
    lastBlasMs=double(ticks[6]-ticks[5])*1000/double(timestampFrequency);
    frameMilliseconds+=lastFrameMs; traceMilliseconds+=lastTraceMs;
    D3D12_RANGE empty{0,0}; timingReadback->Unmap(0,&empty); ++timedFrames;
}
void Device::ReportTimings() const
{
    if(timedFrames) std::printf("[info] gpu frame time: avg %.3f ms (path trace %.3f ms)\n",
        frameMilliseconds/double(timedFrames),traceMilliseconds/double(timedFrames));
}
