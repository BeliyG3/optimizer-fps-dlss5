#include "d3d12_cases.h"

namespace d3d12_cases {

int failures = 0;

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<char> ReadBinary(const char *path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return {};
    const std::streamsize size = stream.tellg();
    if (size <= 0) return {};
    std::vector<char> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(bytes.data(), size);
    return stream ? bytes : std::vector<char>{};
}

ComPtr<ID3D12Resource> CreateTexture(
    ID3D12Device *device,
    std::uint32_t width,
    std::uint32_t height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initialState)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> result;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, initialState,
            nullptr, IID_PPV_ARGS(&result))))
        result.Reset();
    return result;
}

bool WaitForQueue(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    constexpr UINT64 value = 1;
    if (FAILED(queue->Signal(fence.Get(), value))) return false;
    if (fence->GetCompletedValue() >= value) return true;

    const HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (eventHandle == nullptr) return false;
    const HRESULT setResult = fence->SetEventOnCompletion(value, eventHandle);
    const DWORD waitResult = SUCCEEDED(setResult) ? WaitForSingleObject(eventHandle, 10000) : WAIT_FAILED;
    CloseHandle(eventHandle);
    return SUCCEEDED(setResult) && waitResult == WAIT_OBJECT_0;
}

bool HasDebugErrors(ID3D12Device *device)
{
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue)))) return false;
    bool found = false;
    const UINT64 count = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 index = 0; index < count; ++index) {
        SIZE_T size = 0;
        if (FAILED(infoQueue->GetMessage(index, nullptr, &size)) || size == 0) continue;
        std::vector<std::byte> storage(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        if (FAILED(infoQueue->GetMessage(index, message, &size))) continue;
        if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
            message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
            std::cerr << "D3D12 validation: " << message->pDescription << '\n';
            found = true;
        }
    }
    return found;
}

// Native x of the left edge of the 1:1 centre band and of the raw Work region: the outline shader
// paints the two pixels just inside each edge, so ceil(edge) lands on the band.

} // namespace d3d12_cases
