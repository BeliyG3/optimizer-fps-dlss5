#pragma once
// Menu mode's colour handling. The menu frame goes to NR as it is (owner ruling 2026-09-29): an SDR back buffer is
// copied into the host's NR colour format and the model's output back, the typed views converting the storage format
// (every supported host encodes an SDR-range sRGB proxy, spike 0b-2b). Other swap chains are not processed; the tab
// says why and menu mode retries when the swap chain changes back.
#include <d3d12.h>
#include <wrl/client.h>
#include <cstddef>

namespace ofps::reshade {

// The swap chain's colour space (ReShade's swapchain::get_color_space(), mapped by the pipeline).
enum class MenuColourSpace { Unknown, Srgb, Scrgb, Hdr10Pq, Hdr10Hlg };
const char *MenuColourSpaceName(MenuColourSpace space);

// Null when the back buffer can go to NR as it is; else why not, for the tab. scRGB (FP16) goes as it is too, unscaled:
// the game's NR white level is unknown, so the menu's tone may differ from the game's NR frames (owner ruling: as is).
const char *MenuColourProblem(MenuColourSpace space, DXGI_FORMAT backBuffer);

// The format the copy's SRV/UAV views use; UNKNOWN for formats the pass does not handle (sRGB-typed, typeless, others).
DXGI_FORMAT MenuConvertViewFormat(DXGI_FORMAT format);

// Root signature (one SRV/UAV descriptor table) and PSO of CSConvert (core/shaders/menu_convert_cs.hlsl).
struct MenuConvertPipeline {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
};
bool MenuConvertBuild(ID3D12Device *device, const void *code, size_t size, MenuConvertPipeline *pipeline);
// Copies the SRV at `table` into the UAV at the next descriptor of `heap` (shader-visible) over the target's size.
// Sets the heap too: after an NGX evaluate the list has NGX's heaps bound.
void MenuConvertRecord(ID3D12GraphicsCommandList *list, const MenuConvertPipeline &pipeline, ID3D12DescriptorHeap *heap,
                       D3D12_GPU_DESCRIPTOR_HANDLE table, ID3D12Resource *target);

} // namespace ofps::reshade
