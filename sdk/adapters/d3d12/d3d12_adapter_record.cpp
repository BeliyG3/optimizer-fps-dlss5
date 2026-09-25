#include "d3d12_adapter_private.h"

namespace ofps::sdk {
namespace {
bool ValidHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept { return handle.ptr != 0; }
} // namespace

AdapterStatus D3D12Adapter::Impl::Record(
    ID3D12GraphicsCommandList *list, std::uint32_t constantSet,
    ID3D12PipelineState *pipeline, const D3D12_CPU_DESCRIPTOR_HANDLE *targets,
    UINT targetCount, std::uint32_t width, std::uint32_t height,
    D3D12_GPU_DESCRIPTOR_HANDLE sourceTable, DiagnosticOutlineFlags diagnosticOutlines) noexcept
{
        if (list == nullptr || pipeline == nullptr || constantSet >= sourceSets ||
            !ValidDiagnosticOutlineFlags(diagnosticOutlines))
            return AdapterStatus::InvalidArgument;
        ID3D12DescriptorHeap *heaps[] = {sourceHeap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(rootSignature.Get());
        list->SetPipelineState(pipeline);
        list->SetGraphicsRootConstantBufferView(0, constants->GetGPUVirtualAddress());
        list->SetGraphicsRootConstantBufferView(
            1, inputConstants->GetGPUVirtualAddress() + static_cast<UINT64>(constantSet) * 256u);
        list->SetGraphicsRootDescriptorTable(2, sourceTable);
        const std::uint32_t diagnosticConstants[4]{
            static_cast<std::uint32_t>(diagnosticOutlines), outputGainBits, outputInvGammaBits, 0};
        list->SetGraphicsRoot32BitConstants(3, 4, diagnosticConstants, 0);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->OMSetRenderTargets(targetCount, targets, FALSE, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
        return AdapterStatus::Ok;
}

AdapterStatus D3D12Adapter::RecordPack(ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
                                       const D3D12TargetHandles &targets) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (frameSlot >= impl_->framesInFlight) return AdapterStatus::InvalidArgument;
    return RecordPackFromSet(commandList, frameSlot, frameSlot, targets);
}

AdapterStatus D3D12Adapter::RecordPack(ID3D12GraphicsCommandList *commandList,
                                       std::uint32_t frameSlot) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (frameSlot >= impl_->framesInFlight) return AdapterStatus::InvalidArgument;
    return RecordPackFromSet(commandList, frameSlot, frameSlot, impl_->packedFrames[frameSlot].rtvs);
}

AdapterStatus D3D12Adapter::RecordPackFromSet(ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
                                              std::uint32_t sourceSet, const D3D12TargetHandles &targets) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (!ValidHandle(targets.color) || !ValidHandle(targets.depth) ||
        !ValidHandle(targets.motion) || !ValidHandle(targets.confidence)) return AdapterStatus::InvalidArgument;
    if (frameSlot >= impl_->framesInFlight || sourceSet >= impl_->sourceSets) return AdapterStatus::InvalidArgument;
    if ((impl_->sourceExtents[sourceSet] & Impl::SourceExtentNative) == 0)
        return AdapterStatus::ResourceMismatch;
    const D3D12_CPU_DESCRIPTOR_HANDLE handles[] = {targets.color, targets.depth, targets.motion, targets.confidence};
    (void) frameSlot; // the targets already name the slot's textures
    return impl_->Record(commandList, sourceSet, impl_->packPipeline.Get(), handles, 4,
                         impl_->layout.workWidth, impl_->layout.workHeight,
                         impl_->ExternalSourceTable(sourceSet));
}

AdapterStatus D3D12Adapter::RecordPackFromSet(ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
                                              std::uint32_t sourceSet) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (frameSlot >= impl_->framesInFlight) return AdapterStatus::InvalidArgument;
    return RecordPackFromSet(commandList, frameSlot, sourceSet, impl_->packedFrames[frameSlot].rtvs);
}

AdapterStatus D3D12Adapter::RecordUnpack(ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
                                         const D3D12TargetHandles &targets) noexcept
{
    return RecordUnpack(commandList, frameSlot, targets, DiagnosticOutlineNone);
}

AdapterStatus D3D12Adapter::RecordUnpack(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
    const D3D12TargetHandles &targets, DiagnosticOutlineFlags diagnosticOutlines) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (!ValidDiagnosticOutlineFlags(diagnosticOutlines)) return AdapterStatus::InvalidArgument;
    if (!ValidHandle(targets.color) || !ValidHandle(targets.depth) ||
        !ValidHandle(targets.motion) || !ValidHandle(targets.confidence)) return AdapterStatus::InvalidArgument;
    if (frameSlot >= impl_->framesInFlight ||
        (impl_->sourceExtents[frameSlot] & Impl::SourceExtentWork) == 0)
        return AdapterStatus::ResourceMismatch;
    const D3D12_CPU_DESCRIPTOR_HANDLE handles[] = {targets.color, targets.depth, targets.motion, targets.confidence};
    return impl_->Record(commandList, frameSlot, impl_->unpackPipeline.Get(), handles, 4,
                         impl_->layout.nativeWidth, impl_->layout.nativeHeight,
                         impl_->ExternalSourceTable(frameSlot), diagnosticOutlines);
}

AdapterStatus D3D12Adapter::RecordUnpackColor(ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
                                              D3D12_CPU_DESCRIPTOR_HANDLE target) noexcept
{
    return RecordUnpackColor(commandList, frameSlot, target, DiagnosticOutlineNone);
}

AdapterStatus D3D12Adapter::RecordUnpackColor(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
    D3D12_CPU_DESCRIPTOR_HANDLE target, DiagnosticOutlineFlags diagnosticOutlines) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (frameSlot >= impl_->framesInFlight) return AdapterStatus::InvalidArgument;
    return RecordUnpackColorFromSet(commandList, frameSlot, frameSlot, target, diagnosticOutlines);
}

AdapterStatus D3D12Adapter::RecordUnpackColorFromSet(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot, std::uint32_t sourceSet,
    D3D12_CPU_DESCRIPTOR_HANDLE target, DiagnosticOutlineFlags diagnosticOutlines) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (!ValidDiagnosticOutlineFlags(diagnosticOutlines)) return AdapterStatus::InvalidArgument;
    if (!ValidHandle(target)) return AdapterStatus::InvalidArgument;
    if (frameSlot >= impl_->framesInFlight || sourceSet >= impl_->sourceSets) return AdapterStatus::InvalidArgument;
    if ((impl_->sourceExtents[sourceSet] & Impl::SourceExtentWork) == 0)
        return AdapterStatus::ResourceMismatch;
    (void) frameSlot;
    return impl_->Record(commandList, sourceSet, impl_->unpackColorPipeline.Get(), &target, 1,
                         impl_->layout.nativeWidth, impl_->layout.nativeHeight,
                         impl_->ExternalSourceTable(sourceSet), diagnosticOutlines);
}

AdapterStatus D3D12Adapter::RecordUnpackOwned(ID3D12GraphicsCommandList *commandList,
                                              std::uint32_t frameSlot,
                                              const D3D12TargetHandles &targets) noexcept
{
    return RecordUnpackOwned(commandList, frameSlot, targets, DiagnosticOutlineNone);
}

AdapterStatus D3D12Adapter::RecordUnpackOwned(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
    const D3D12TargetHandles &targets, DiagnosticOutlineFlags diagnosticOutlines) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (!impl_->packedConfidenceAllocated) return AdapterStatus::ResourceMismatch;
    if (!ValidDiagnosticOutlineFlags(diagnosticOutlines)) return AdapterStatus::InvalidArgument;
    if (frameSlot >= impl_->framesInFlight || !ValidHandle(targets.color) ||
        !ValidHandle(targets.depth) || !ValidHandle(targets.motion) ||
        !ValidHandle(targets.confidence))
        return AdapterStatus::InvalidArgument;
    const D3D12_CPU_DESCRIPTOR_HANDLE handles[] = {
        targets.color, targets.depth, targets.motion, targets.confidence};
    return impl_->Record(commandList, frameSlot, impl_->unpackPipeline.Get(), handles, 4,
                         impl_->layout.nativeWidth, impl_->layout.nativeHeight,
                         impl_->packedFrames[frameSlot].shaderResourceTable,
                         diagnosticOutlines);
}

AdapterStatus D3D12Adapter::RecordUnpackOwnedColor(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
    D3D12_CPU_DESCRIPTOR_HANDLE target) noexcept
{
    return RecordUnpackOwnedColor(commandList, frameSlot, target, DiagnosticOutlineNone);
}

AdapterStatus D3D12Adapter::RecordUnpackOwnedColor(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
    D3D12_CPU_DESCRIPTOR_HANDLE target, DiagnosticOutlineFlags diagnosticOutlines) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (frameSlot >= impl_->framesInFlight) return AdapterStatus::InvalidArgument;
    return RecordUnpackOwnedColorFromSet(commandList, frameSlot, frameSlot, target, diagnosticOutlines);
}

AdapterStatus D3D12Adapter::RecordUnpackOwnedColorFromSet(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot, std::uint32_t sourceSet,
    D3D12_CPU_DESCRIPTOR_HANDLE target, DiagnosticOutlineFlags diagnosticOutlines) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (!ValidDiagnosticOutlineFlags(diagnosticOutlines)) return AdapterStatus::InvalidArgument;
    if (frameSlot >= impl_->framesInFlight || sourceSet >= impl_->sourceSets || !ValidHandle(target))
        return AdapterStatus::InvalidArgument;
    return impl_->Record(commandList, sourceSet, impl_->unpackColorPipeline.Get(), &target, 1,
                         impl_->layout.nativeWidth, impl_->layout.nativeHeight,
                         impl_->packedFrames[frameSlot].shaderResourceTable,
                         diagnosticOutlines);
}

AdapterStatus D3D12Adapter::RecordOutlines(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
    D3D12_CPU_DESCRIPTOR_HANDLE target,
    DiagnosticOutlineFlags diagnosticOutlines) noexcept
{
    if (!impl_->device) return AdapterStatus::NotInitialized;
    if (!ValidDiagnosticOutlineFlags(diagnosticOutlines)) return AdapterStatus::InvalidArgument;
    if (!impl_->outlinePipeline) return AdapterStatus::ShaderBytecodeMissing;
    if (frameSlot >= impl_->framesInFlight || !ValidHandle(target))
        return AdapterStatus::InvalidArgument;
    if (diagnosticOutlines == DiagnosticOutlineNone) return AdapterStatus::Ok;
    return impl_->Record(commandList, frameSlot, impl_->outlinePipeline.Get(), &target, 1,
                         impl_->layout.nativeWidth, impl_->layout.nativeHeight,
                         impl_->ExternalSourceTable(frameSlot), diagnosticOutlines);
}

AdapterStatus D3D12Adapter::RecordCenterOutline(
    ID3D12GraphicsCommandList *commandList, std::uint32_t frameSlot,
    D3D12_CPU_DESCRIPTOR_HANDLE target) noexcept
{
    return RecordOutlines(commandList, frameSlot, target, DiagnosticOutlineCenter);
}

} // namespace ofps::sdk
