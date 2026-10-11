#pragma once
// A stand-in for ReShade's proxy device (tests/test_gpu_queue_waits.cpp): another object than the device it wraps, which
// forwards every call to it and so answers with the same adapter LUID. A stack object: reference counting is a no-op.
#include <d3d12.h>

namespace proxytest {

class ProxyDevice final : public ID3D12Device {
public:
    explicit ProxyDevice(ID3D12Device *real) : real_(real) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12Device)) { *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *size, void *data) override { return real_->GetPrivateData(guid, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void *data) override { return real_->SetPrivateData(guid, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *data) override { return real_->SetPrivateDataInterface(guid, data); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override { return real_->SetName(name); }
    UINT STDMETHODCALLTYPE GetNodeCount() override { return real_->GetNodeCount(); }
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *desc, REFIID riid, void **out) override { return real_->CreateCommandQueue(desc, riid, out); }
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **out) override { return real_->CreateCommandAllocator(type, riid, out); }
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **out) override {
        return real_->CreateGraphicsPipelineState(desc, riid, out);
    }
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **out) override {
        return real_->CreateComputePipelineState(desc, riid, out);
    }
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *allocator, ID3D12PipelineState *state,
                                                REFIID riid, void **out) override {
        return real_->CreateCommandList(mask, type, allocator, state, riid, out);
    }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE feature, void *data, UINT size) override { return real_->CheckFeatureSupport(feature, data, size); }
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid, void **out) override { return real_->CreateDescriptorHeap(desc, riid, out); }
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE type) override { return real_->GetDescriptorHandleIncrementSize(type); }
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT mask, const void *blob, SIZE_T bytes, REFIID riid, void **out) override {
        return real_->CreateRootSignature(mask, blob, bytes, riid, out);
    }
    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dst) override { real_->CreateConstantBufferView(desc, dst); }
    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource *r, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dst) override {
        real_->CreateShaderResourceView(r, desc, dst);
    }
    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource *r, ID3D12Resource *counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE dst) override {
        real_->CreateUnorderedAccessView(r, counter, desc, dst);
    }
    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource *r, const D3D12_RENDER_TARGET_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dst) override {
        real_->CreateRenderTargetView(r, desc, dst);
    }
    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource *r, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dst) override {
        real_->CreateDepthStencilView(r, desc, dst);
    }
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dst) override { real_->CreateSampler(desc, dst); }
    void STDMETHODCALLTYPE CopyDescriptors(UINT dstCount, const D3D12_CPU_DESCRIPTOR_HANDLE *dstStarts, const UINT *dstSizes, UINT srcCount,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE *srcStarts, const UINT *srcSizes, D3D12_DESCRIPTOR_HEAP_TYPE type) override {
        real_->CopyDescriptors(dstCount, dstStarts, dstSizes, srcCount, srcStarts, srcSizes, type);
    }
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type) override {
        real_->CopyDescriptorsSimple(count, dst, src, type);
    }
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(UINT mask, UINT count, const D3D12_RESOURCE_DESC *descs) override {
        return real_->GetResourceAllocationInfo(mask, count, descs);
    }
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT mask, D3D12_HEAP_TYPE type) override { return real_->GetCustomHeapProperties(mask, type); }
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC *desc,
                                                      D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out) override {
        return real_->CreateCommittedResource(heap, flags, desc, state, clear, riid, out);
    }
    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC *desc, REFIID riid, void **out) override { return real_->CreateHeap(desc, riid, out); }
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
                                                   const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out) override {
        return real_->CreatePlacedResource(heap, offset, desc, state, clear, riid, out);
    }
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear,
                                                     REFIID riid, void **out) override {
        return real_->CreateReservedResource(desc, state, clear, riid, out);
    }
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild *object, const SECURITY_ATTRIBUTES *attributes, DWORD access, LPCWSTR name,
                                                 HANDLE *handle) override {
        return real_->CreateSharedHandle(object, attributes, access, name, handle);
    }
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE handle, REFIID riid, void **out) override { return real_->OpenSharedHandle(handle, riid, out); }
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR name, DWORD access, HANDLE *handle) override { return real_->OpenSharedHandleByName(name, access, handle); }
    HRESULT STDMETHODCALLTYPE MakeResident(UINT count, ID3D12Pageable *const *objects) override { return real_->MakeResident(count, objects); }
    HRESULT STDMETHODCALLTYPE Evict(UINT count, ID3D12Pageable *const *objects) override { return real_->Evict(count, objects); }
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64 value, D3D12_FENCE_FLAGS flags, REFIID riid, void **out) override { return real_->CreateFence(value, flags, riid, out); }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return real_->GetDeviceRemovedReason(); }
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC *desc, UINT first, UINT count, UINT64 offset,
                                                 D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows, UINT64 *rowBytes, UINT64 *total) override {
        real_->GetCopyableFootprints(desc, first, count, offset, layouts, rows, rowBytes, total);
    }
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *desc, REFIID riid, void **out) override { return real_->CreateQueryHeap(desc, riid, out); }
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL enable) override { return real_->SetStablePowerState(enable); }
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC *desc, ID3D12RootSignature *root, REFIID riid, void **out) override {
        return real_->CreateCommandSignature(desc, root, riid, out);
    }
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource *r, UINT *tiles, D3D12_PACKED_MIP_INFO *packed, D3D12_TILE_SHAPE *shape, UINT *count,
                                             UINT first, D3D12_SUBRESOURCE_TILING *tilings) override {
        real_->GetResourceTiling(r, tiles, packed, shape, count, first, tilings);
    }
    LUID STDMETHODCALLTYPE GetAdapterLuid() override { return real_->GetAdapterLuid(); }

private:
    ID3D12Device *real_;
};

} // namespace proxytest
