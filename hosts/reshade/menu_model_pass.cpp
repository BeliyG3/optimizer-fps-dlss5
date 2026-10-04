#include "hosts/reshade/menu_pipeline.h"
#include "hosts/reshade/menu_colour.h"
#include "hosts/reshade/menu_core_pass.h"
#include "hosts/reshade/menu_guides.h"
#include "hosts/reshade/ngx_forwarder_calls.h"
#include "hosts/reshade/ngx_hook_api.h"
#include "hosts/reshade/ngx_params.h"
#include "hosts/reshade/shell_host.h"
#include "hosts/reshade/addon/shell_settings.h"
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>

// Model pass of menu mode. Recorded on the private list (the host list's device): capture -> modelIn (CSConvert),
// the watched feature evaluated with menu mode's own block from the bound snapshot (menu_param_book.h) and the guide set
// copied at the same host evaluate (menu_guides.h), modelOut -> output. A model at the frame's size is called directly
// (stages 1-2); a compressed one (Mode Uniform/Peripheral) runs through the core's feature (menu_core_pass.h, stage 3).
namespace ofps::reshade {
namespace {
using Microsoft::WRL::ComPtr;
constexpr D3D12_RESOURCE_STATES kInput = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; // NGX inputs at evaluate
constexpr D3D12_RESOURCE_STATES kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;          // NGX output at evaluate
enum Descriptor : UINT { kCaptureSrv, kInUav, kOutSrv, kOutputUav, kDescriptors };

struct Model {
    MenuConvertPipeline convert; // root signature and PSO, size-independent, kept for the device
    ComPtr<ID3D12DescriptorHeap> heap; // size-bound: retired with the resources
    ComPtr<ID3D12Resource> in, out;    // modelIn (the host's colour format, rests in kInput), modelOut (rests in kWrite)
    ComPtr<ID3D12Device> device;
    UINT step = 0;
    ID3D12Resource *capture = nullptr, *output = nullptr; // what descriptors kCaptureSrv/kOutputUav describe
    std::shared_ptr<const MenuParamSnapshot> snapshot; // bound per present
    OwnParams block; // the menu evaluate's block (menu_param_book.h)
    unsigned long long passes = 0, refused = 0;
    double lastEvalMs = 0; // CPU of the last pass's evaluate call, taken by the pipeline's CPU record
    bool loggedKeys = false, loggedRefusal = false, loggedGuides = false;
};
Model &M() { static Model *model = new Model(); return *model; } // never destroyed from DllMain

void Log(const char *fmt, ...) {
    char text[1536]; va_list args; va_start(args, fmt); std::vsnprintf(text, sizeof(text), fmt, args); va_end(args);
    Host().Log(OFPS_LOG_INFO, text);
}

void Barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
}

// The views read and write stored values as they are; CSConvert copies them. sRGB-typed formats are refused: a typed
// sRGB resource can neither carry a UAV nor be viewed as UNORM. Typeless or unknown: no guess.
DXGI_FORMAT ViewFormat(DXGI_FORMAT format) { return MenuConvertViewFormat(format); }

// The device can store to a typed UAV of this format (CSConvert writes modelIn and the output through one).
bool TypedUavStore(ID3D12Device *device, DXGI_FORMAT format) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
           (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

// Root signature and PSO, cached for one device: a new device (after a drain) builds its own.
bool BuildPipeline(Model &m, ID3D12Device *device) {
    if (m.convert.pso && m.device.Get() == device) return true;
    m.convert = {}; // only reached after a drain: no list of the old device is in flight
    m.device = device;
    const std::string code = MenuShaderCode(L"menu_convert_cs.dxbc");
    if (code.empty()) { Log("menu model pass: optimizer-fps-dlss5\\menu_convert_cs.dxbc is missing"); return false; }
    return MenuConvertBuild(device, code.data(), code.size(), &m.convert);
}

D3D12_CPU_DESCRIPTOR_HANDLE Cpu(Model &m, UINT index) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = m.heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(index) * m.step; return h;
}
void Srv(Model &m, ID3D12Resource *resource, UINT index) {
    D3D12_SHADER_RESOURCE_VIEW_DESC v{}; v.Format = ViewFormat(resource->GetDesc().Format); v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; v.Texture2D.MipLevels = 1;
    m.device->CreateShaderResourceView(resource, &v, Cpu(m, index));
}
void Uav(Model &m, ID3D12Resource *resource, UINT index) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC v{}; v.Format = ViewFormat(resource->GetDesc().Format); v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m.device->CreateUnorderedAccessView(resource, nullptr, &v, Cpu(m, index));
}

// CSConvert from the SRV at `table` into the UAV after it, sized by the UAV's texture (heaps set again after the
// evaluate: NGX binds its own).
void Convert(Model &m, ID3D12GraphicsCommandList *list, UINT table, ID3D12Resource *target) {
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m.heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += UINT64(table) * m.step;
    MenuConvertRecord(list, m.convert, m.heap.Get(), gpu, target);
}

// Every key of the feature-18 contract (tools/bench12/ngx_nr_contract.h) the pass leaves as the host has it.
void LogContract(void *params) {
    char extents[1024] = {}, model[384] = {};
    DescribeExtents(params, extents, sizeof(extents));
    unsigned inverted = 0, enabled = 0; float ratio = 0, skin = 0, structure = 0;
    const bool hasInverted = GetUInt(params, "DLSSNR.DepthInverted", &inverted), hasRatio = GetFloat(params, "DLSSNR.ScalingRatio", &ratio);
    GetUInt(params, "DLSSNR.Enabled", &enabled);
    GetFloat(params, "DLSSNR.SkinStructureStrength", &skin); GetFloat(params, "DLSSNR.LocalStructureStrength", &structure);
    DescribeModelKeys(params, model, sizeof(model));
    Log("menu model pass: host keys left as the host has them: %s; DepthInverted %u (%s), ScalingRatio %.4f (%s), LocalStructure %.3f, "
        "SkinStructure %.3f, UI %p, UIAlpha %p, Backbuffer %p; %s",
        extents, inverted, hasInverted ? "set" : "unset", double(ratio), hasRatio ? "set" : "unset", double(structure), double(skin),
        (void *) GetResource(params, "DLSSNR.UI"), (void *) GetResource(params, "DLSSNR.UIAlpha"), (void *) GetResource(params, "DLSSNR.Backbuffer"), model);
}

bool Refuse(Model &m, const char *why, int result) {
    ++m.refused;
    if (!m.loggedRefusal) {
        m.loggedRefusal = true;
        Log("menu model pass: %s (result 0x%08X); the private list is not executed, the frame stays untouched (logged once)", why, unsigned(result));
    }
    return false;
}
} // namespace

std::string MenuShaderCode(const wchar_t *fileName) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&M), &module);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    if (wchar_t *slash = wcsrchr(path, L'\\')) slash[1] = 0;
    std::ifstream file(std::wstring(path) + L"optimizer-fps-dlss5\\" + fileName, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool MenuModelBuild(ID3D12Device *device, const D3D12_RESOURCE_DESC &back, const MenuShape &shape) {
    auto &m = M();
    const D3D12_RESOURCE_DESC cd = shape.colour, od = shape.output;
    const UINT w = UINT(back.Width), h = back.Height;
    const auto whole = [&](const UINT (&r)[4]) { return r[0] == 0 && r[1] == 0 && r[2] == w && r[3] == h; };
    if (!whole(shape.colourRect) || !whole(shape.outputRect) || ViewFormat(cd.Format) == DXGI_FORMAT_UNKNOWN ||
        ViewFormat(od.Format) == DXGI_FORMAT_UNKNOWN || ViewFormat(back.Format) == DXGI_FORMAT_UNKNOWN) {
        Log("menu model pass: the game's colour %u,%u %ux%u (fmt %d) / output %u,%u %ux%u (fmt %d) do not match the back buffer %ux%u (fmt %d)",
            shape.colourRect[0], shape.colourRect[1], shape.colourRect[2], shape.colourRect[3], int(cd.Format), shape.outputRect[0],
            shape.outputRect[1], shape.outputRect[2], shape.outputRect[3], int(od.Format), w, h, int(back.Format));
        return false;
    }
    const auto isFloat = [](DXGI_FORMAT f) { return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R11G11B10_FLOAT; };
    if (isFloat(back.Format) && !isFloat(cd.Format)) { // an HDR back buffer into an SDR NR input would clip it
        Log("menu model pass: the back buffer is float (fmt %d) but the game's NR colour is not (fmt %d); menu mode unavailable",
            int(back.Format), int(cd.Format));
        return false;
    }
    for (DXGI_FORMAT format : {cd.Format, back.Format}) // modelIn and the output are written through typed UAVs
        if (!TypedUavStore(device, format)) {
            Log("menu model pass: format %d has no typed UAV store on this device; menu mode unavailable", int(format));
            return false;
        }
    if (!BuildPipeline(m, device)) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kDescriptors, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h; td.DepthOrArraySize = 1;
    td.MipLevels = 1; td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_RESOURCE_DESC ind = td, outd = td; ind.Format = cd.Format; outd.Format = od.Format;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m.heap))) ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &ind, kInput, nullptr, IID_PPV_ARGS(&m.in))) ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &outd, kWrite, nullptr, IID_PPV_ARGS(&m.out)))) return false;
    m.step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    Uav(m, m.in.Get(), kInUav);
    Srv(m, m.out.Get(), kOutSrv);
    m.capture = m.output = nullptr;
    Log("menu model pass: modelIn fmt %d, modelOut fmt %d, %ux%u; back buffer fmt %d; the frame as it is; motion zero",
        int(cd.Format), int(od.Format), w, h, int(back.Format));
    return true;
}

void MenuModelBind(std::shared_ptr<const MenuParamSnapshot> snapshot) { M().snapshot = std::move(snapshot); }

double MenuModelTakeEvaluateMs() { const double ms = M().lastEvalMs; M().lastEvalMs = 0; return ms; }

void MenuModelReleaseDevice() { auto &m = M(); m.convert = {}; m.device.Reset(); }

std::vector<ComPtr<IUnknown>> MenuModelRetire() {
    auto &m = M();
    std::vector<ComPtr<IUnknown>> retired;
    for (IUnknown *object : {static_cast<IUnknown *>(m.heap.Get()), static_cast<IUnknown *>(m.in.Get()), static_cast<IUnknown *>(m.out.Get())})
        if (object) retired.emplace_back(object);
    m.heap.Reset(); m.in.Reset(); m.out.Reset();
    m.capture = m.output = nullptr;
    return retired;
}

bool MenuModelPass(MenuFrame &frame) {
    auto &m = M();
    const std::shared_ptr<const MenuParamSnapshot> snapshot = m.snapshot;
    if (!m.in || !snapshot) return Refuse(m, "no snapshot or resources", 0);
    ID3D12Resource *depth = nullptr, *motion = nullptr;
    if (!MenuGuidesFor(snapshot->guideSet, &depth, &motion)) return Refuse(m, "the snapshot's guides are gone", 0);
    if (frame.capture != m.capture || frame.output != m.output) { // written once per build: no pass reads them now
        Srv(m, frame.capture, kCaptureSrv); Uav(m, frame.output, kOutputUav);
        m.capture = frame.capture; m.output = frame.output;
    }
    MenuPassBlock(*snapshot, m.in.Get(), m.out.Get(), depth, motion, m.block);
    if (!m.loggedKeys) { m.loggedKeys = true; LogContract(&m.block); }
    ID3D12GraphicsCommandList *list = frame.list;
    Barrier(list, frame.capture, D3D12_RESOURCE_STATE_COPY_SOURCE, kInput);
    Barrier(list, m.in.Get(), kInput, kWrite);
    Convert(m, list, kCaptureSrv, m.in.Get());
    Barrier(list, m.in.Get(), kWrite, kInput);
    Barrier(list, frame.capture, kInput, D3D12_RESOURCE_STATE_COPY_SOURCE);
    LARGE_INTEGER t0{}, t1{}, frequency{};
    QueryPerformanceCounter(&t0);
    // Stage 3: the user's layout (and sync temporal cadence) through the core.
    if (MenuUsesCore(snapshot->shape, frame.width, frame.height, MenuCadenceMode())) {
        const MenuCoreFrame recorded = MenuCorePass(snapshot->tag.hostHandle, m.block, list);
        QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&frequency);
        m.lastEvalMs = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(frequency.QuadPart); // the pipeline logs it per present
        if (recorded == MenuCoreFrame::Untouched) return Refuse(m, "the core did not take the menu frame", 0);
        // Fix round 1: the list runs (the core recorded work), frame.output keeps the last pass's output, shown again
        // (or, before this run's first pass, not written back at all).
        if (recorded == MenuCoreFrame::LastShownAgain) { ++m.passes; frame.wrote = false; return true; }
    } else { // stages 1-2: the model directly, at the frame's size
        int result = 0;
        bool threw = false, current = false;
        char what[160] = "the evaluate threw";
        // Final review C1: the snapshot's model must still be the feature's model. Checked and called under the feature
        // call lock (try-lock, never waits), so neither a release nor a re-creation can come in between; a model that
        // changed (or a creation the core owes, MenuCorePassCreateBlocked) skips the pass.
        const bool called = !MenuCorePassCreateBlocked() && WithFeatureForMenu(snapshot->tag.hostHandle, [&](IOfpsFeature &feature, ModelHostNgx &) {
            current = feature.CurrentModelHandle() == snapshot->tag.realHandle;
            if (!current) return false;
            try { result = CallEvaluate(list, snapshot->tag.realHandle, &m.block, nullptr); } // the pass's resources and Reset=0
            catch (const std::exception &e) { threw = true; std::snprintf(what, sizeof(what), "the evaluate threw: %.120s", e.what()); }
            catch (...) { threw = true; }
            return true;
        });
        QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&frequency);
        m.lastEvalMs = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(frequency.QuadPart);
        if (!called) return Refuse(m, current ? "the feature is busy or gone" : "the snapshot's model is no longer the feature's (busy, gone or re-created)", 0);
        if (threw) return Refuse(m, what, 0);
        if (result != kNgxSuccess) return Refuse(m, "the evaluate refused", result);
    }
    Barrier(list, m.out.Get(), kWrite, kInput);
    Barrier(list, frame.output, D3D12_RESOURCE_STATE_COPY_DEST, kWrite);
    Convert(m, list, kOutSrv, frame.output);
    Barrier(list, frame.output, kWrite, D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(list, m.out.Get(), kInput, kWrite);
    ++m.passes;
    return true;
}

} // namespace ofps::reshade
