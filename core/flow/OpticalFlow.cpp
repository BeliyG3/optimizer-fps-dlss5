#include "core/flow/OpticalFlowInternal.h"

#include <windows.h>

#include <mutex>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace ofps::core::flow
{
namespace
{
using CreateInstanceFn = NV_OF_STATUS(NVOFAPI*)(std::uint32_t, NV_OF_D3D12_API_FUNCTION_LIST*);

ComPtr<ID3D12Resource> CreateTexture(ID3D12Device* device, DXGI_FORMAT format, std::uint32_t width, std::uint32_t height,
                                     bool writable, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = writable ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&texture))))
        return nullptr;
    texture->SetName(name);
    return texture;
}
} // namespace

bool Session::Impl::Register(ID3D12Resource* resource, NvOFGPUBufferHandle* buffer)
{
        NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 params = {};
        params.resource = resource;
        params.hOFGpuBuffer = buffer;
        // Nothing pending on either side at registration: fences at their current values.
        params.inputFencePoint = { fenceIn.Get(), 0 };
        params.outputFencePoint = { fenceOut.Get(), 0 };
        return api.nvOFRegisterResourceD3D12(handle, &params) == NV_OF_SUCCESS;
}

Session::Impl::~Impl()
{
        for (NvOFGPUBufferHandle buffer : { nowBuffer, thenBuffer, fieldBuffer })
        {
            if (buffer == nullptr || api.nvOFUnregisterResourceD3D12 == nullptr)
                continue;
            NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 params = { buffer };
            api.nvOFUnregisterResourceD3D12(&params);
        }
        if (handle != nullptr && api.nvOFDestroy != nullptr)
            api.nvOFDestroy(handle);
        if (library != nullptr)
            FreeLibrary(library);
}

Session::~Session() = default;

Session* Session::Acquire(ID3D12Device* device, std::uint32_t width, std::uint32_t height, std::string& reason)
{
    if (!device || width < 4 || height < 4) {
        reason = "optical flow requires a device and at least a 4x4 image";
        return nullptr;
    }
    // Never destroyed, not even at exit (see the header): the list itself is leaked on purpose.
    static std::mutex mutex;
    static auto* sessions = new std::vector<std::unique_ptr<Session>>();
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& session : *sessions)
        if (session->_impl->device.Get() == device && session->Width() == (width & ~3u) && session->Height() == (height & ~3u))
            return session.get();
    std::unique_ptr<Session> created = Create(device, width, height, reason);
    if (!created)
        return nullptr;
    sessions->push_back(std::move(created));
    return sessions->back().get();
}

std::unique_ptr<Session> Session::Create(ID3D12Device* device, std::uint32_t width, std::uint32_t height,
                                         std::string& reason)
{
    auto impl = std::make_unique<Impl>();
    impl->device = device;
    impl->library = LoadLibraryW(L"nvofapi64.dll");
    if (impl->library == nullptr)
    {
        reason = "nvofapi64.dll is not present (the optical flow engine needs an NVIDIA driver)";
        return nullptr;
    }
    const auto createInstance = (CreateInstanceFn) GetProcAddress(impl->library, "NvOFAPICreateInstanceD3D12");
    if (createInstance == nullptr || createInstance(NV_OF_API_VERSION, &impl->api) != NV_OF_SUCCESS ||
        !impl->api.nvCreateOpticalFlowD3D12 || !impl->api.nvOFInit || !impl->api.nvOFDestroy ||
        !impl->api.nvOFRegisterResourceD3D12 || !impl->api.nvOFUnregisterResourceD3D12 ||
        !impl->api.nvOFExecuteD3D12)
    {
        reason = "this driver has no D3D12 optical flow interface";
        return nullptr;
    }
    if (impl->api.nvCreateOpticalFlowD3D12(device, &impl->handle) != NV_OF_SUCCESS)
    {
        reason = "the GPU has no optical flow engine (Turing or newer is required)";
        return nullptr;
    }

    // Sizes that hold a whole number of 4x4 blocks suit every grid. The finest grid the engine takes wins:
    // a vector per 4x4 of a half-size session is a vector per 8x8 of the frame, coarse for a silhouette.
    impl->width = width & ~3u;
    impl->height = height & ~3u;
    bool initialised = false;
    for (const std::uint32_t grid : { 1u, 2u, 4u })
    {
        NV_OF_INIT_PARAMS init = {};
        init.width = impl->width;
        init.height = impl->height;
        init.outGridSize = (NV_OF_OUTPUT_VECTOR_GRID_SIZE) grid;
        init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
        init.mode = NV_OF_MODE_OPTICALFLOW;
        init.perfLevel = NV_OF_PERF_LEVEL_MEDIUM;
        init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
        init.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
        if (impl->api.nvOFInit(impl->handle, &init) == NV_OF_SUCCESS)
        {
            impl->grid = grid;
            initialised = true;
            break;
        }
        // A refused init leaves the instance unusable: start over with a fresh one for the next size.
        impl->api.nvOFDestroy(impl->handle);
        impl->handle = nullptr;
        if (impl->api.nvCreateOpticalFlowD3D12(device, &impl->handle) != NV_OF_SUCCESS)
            break;
    }
    if (!initialised)
    {
        reason = "the optical flow engine refused an 8-bit greyscale session at this size";
        return nullptr;
    }

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl->fenceIn))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl->fenceOut))))
    {
        reason = "could not create the optical flow fences";
        return nullptr;
    }
    impl->now = CreateTexture(device, DXGI_FORMAT_R8_UNORM, impl->width, impl->height, true, L"OptimizerFps flow: this frame");
    impl->then = CreateTexture(device, DXGI_FORMAT_R8_UNORM, impl->width, impl->height, true, L"OptimizerFps flow: residual frame");
    impl->field = CreateTexture(device, DXGI_FORMAT_R16G16_SINT, impl->width / impl->grid, impl->height / impl->grid, false,
                                L"OptimizerFps flow field");
    if (!impl->now || !impl->then || !impl->field)
    {
        reason = "could not allocate the optical flow textures";
        return nullptr;
    }
    if (!impl->Register(impl->now.Get(), &impl->nowBuffer) || !impl->Register(impl->then.Get(), &impl->thenBuffer) ||
        !impl->Register(impl->field.Get(), &impl->fieldBuffer))
    {
        reason = "the optical flow engine refused the textures";
        return nullptr;
    }

    std::unique_ptr<Session> session(new Session());
    session->_impl = std::move(impl);
    return session;
}

std::uint32_t Session::Width() const { return _impl->width; }
std::uint32_t Session::Height() const { return _impl->height; }
std::uint32_t Session::Grid() const { return _impl->grid; }
ID3D12Resource* Session::Now() const { return _impl->now.Get(); }
ID3D12Resource* Session::Then() const { return _impl->then.Get(); }
ID3D12Resource* Session::Field() const { return _impl->field.Get(); }

} // namespace ofps::core::flow
