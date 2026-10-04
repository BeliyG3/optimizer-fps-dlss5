#pragma once
// Test doubles for tests/test_menu_host_queue.cpp: a command queue whose Signal or Wait fails and a command list whose
// private data cannot be set, to force the failure paths of menu_host_queue.h. Stack objects: reference counting is a
// no-op, and they are never handed to the D3D12 runtime (only to MenuHostQueue).
#include <d3d12.h>

namespace menutest {

// Forwards to a real queue, except that Signal and/or Wait can be made to fail without enqueueing anything. Records the
// fence and value of the last Signal and Wait, so tests can match them without timing.
class FailingQueue final : public ID3D12CommandQueue {
public:
    FailingQueue(ID3D12CommandQueue *real, bool failSignal, bool failWait) : real_(real), failSignal_(failSignal), failWait_(failWait) {}
    unsigned signals = 0, waits = 0; // calls that reached this queue
    ID3D12Fence *signalFence = nullptr, *waitFence = nullptr;
    UINT64 signalValue = 0, waitValue = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12CommandQueue)) { *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *size, void *data) override { return real_->GetPrivateData(guid, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void *data) override { return real_->SetPrivateData(guid, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *data) override { return real_->SetPrivateDataInterface(guid, data); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override { return real_->SetName(name); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override { return real_->GetDevice(riid, device); }
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource *resource, UINT regions, const D3D12_TILED_RESOURCE_COORDINATE *starts,
                                              const D3D12_TILE_REGION_SIZE *sizes, ID3D12Heap *heap, UINT ranges,
                                              const D3D12_TILE_RANGE_FLAGS *flags, const UINT *offsets, const UINT *counts,
                                              D3D12_TILE_MAPPING_FLAGS mappingFlags) override {
        real_->UpdateTileMappings(resource, regions, starts, sizes, heap, ranges, flags, offsets, counts, mappingFlags);
    }
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource *dst, const D3D12_TILED_RESOURCE_COORDINATE *dstStart, ID3D12Resource *src,
                                            const D3D12_TILED_RESOURCE_COORDINATE *srcStart, const D3D12_TILE_REGION_SIZE *size,
                                            D3D12_TILE_MAPPING_FLAGS flags) override {
        real_->CopyTileMappings(dst, dstStart, src, srcStart, size, flags);
    }
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT count, ID3D12CommandList *const *lists) override { real_->ExecuteCommandLists(count, lists); }
    void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data, UINT size) override { real_->SetMarker(metadata, data, size); }
    void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data, UINT size) override { real_->BeginEvent(metadata, data, size); }
    void STDMETHODCALLTYPE EndEvent() override { real_->EndEvent(); }
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence *fence, UINT64 value) override {
        ++signals;
        signalFence = fence;
        signalValue = value;
        return failSignal_ ? E_FAIL : real_->Signal(fence, value);
    }
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence *fence, UINT64 value) override {
        ++waits;
        waitFence = fence;
        waitValue = value;
        return failWait_ ? E_FAIL : real_->Wait(fence, value);
    }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64 *frequency) override { return real_->GetTimestampFrequency(frequency); }
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64 *gpu, UINT64 *cpu) override { return real_->GetClockCalibration(gpu, cpu); }
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return real_->GetDesc(); }

private:
    ID3D12CommandQueue *real_;
    bool failSignal_, failWait_;
};

// A fence of a removed device: it answers UINT64_MAX, and a completion event is set at once, as the runtime does after
// a removal (final review, Codex C1: never a completion).
class RemovedFence final : public ID3D12Fence {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **out) override { if (out) *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **device) override { if (device) *device = nullptr; return E_FAIL; }
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override { return UINT64_MAX; }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE event) override { return SetEvent(event) ? S_OK : E_FAIL; }
    HRESULT STDMETHODCALLTYPE Signal(UINT64) override { return DXGI_ERROR_DEVICE_REMOVED; }
};

// A command list that refuses private data: MenuHostQueue cannot tag it.
class UntaggableList final : public ID3D12CommandList {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12CommandList)) { *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *size, void *) override {
        if (size) *size = 0;
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override { return E_OUTOFMEMORY; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override { return E_OUTOFMEMORY; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **device) override {
        if (device) *device = nullptr;
        return E_NOINTERFACE;
    }
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return D3D12_COMMAND_LIST_TYPE_COMPUTE; }
};

} // namespace menutest
