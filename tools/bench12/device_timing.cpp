#include "device.h"
#include <cstring>

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
void Device::Mark(const char *label)
{
    if(!marks) {
        D3D12_QUERY_HEAP_DESC desc{}; desc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP; desc.Count=MaxMarks;
        Check(gpu->CreateQueryHeap(&desc,IID_PPV_ARGS(&marks)),"Create stage timestamp heap");
        markReadback=Buffer(MaxMarks*sizeof(UINT64),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if(markLabels.size()>=MaxMarks) return;
    list->EndQuery(marks.Get(),D3D12_QUERY_TYPE_TIMESTAMP,UINT(markLabels.size()));
    markLabels.push_back(label);
}
void Device::ResolveTimings()
{
    list->ResolveQueryData(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,7,timingReadback.Get(),0);
    resolvedMarks=unsigned(markLabels.size());
    if(resolvedMarks) list->ResolveQueryData(marks.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,resolvedMarks,markReadback.Get(),0);
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
    if(resolvedMarks>1) {
        D3D12_RANGE markRange{0,resolvedMarks*sizeof(UINT64)};
        Check(markReadback->Map(0,&markRange,&data),"Map GPU stage timings");
        std::vector<UINT64> stamps(resolvedMarks); std::memcpy(stamps.data(),data,resolvedMarks*sizeof(UINT64));
        markReadback->Unmap(0,&empty);
        for(unsigned k=1;k<resolvedMarks;++k) {
            const double ms=double(stamps[k]-stamps[k-1])*1000/double(timestampFrequency);
            auto found=std::find_if(stageTotals.begin(),stageTotals.end(),[&](const auto &s) { return std::strcmp(s.first,markLabels[k])==0; });
            if(found==stageTotals.end()) { stageTotals.push_back({markLabels[k],{0.0,0u}}); found=stageTotals.end()-1; }
            found->second.first+=ms; ++found->second.second;
        }
    }
    markLabels.clear(); resolvedMarks=0;
}
void Device::ReportTimings() const
{
    if(timedFrames) std::printf("[info] gpu frame time: avg %.3f ms (path trace %.3f ms)\n",
        frameMilliseconds/double(timedFrames),traceMilliseconds/double(timedFrames));
    for(const auto &stage:stageTotals) if(stage.second.second)
        std::printf("[gpu stage] %-24s avg %7.3f ms (%u frames)\n",stage.first,stage.second.first/stage.second.second,stage.second.second);
}
