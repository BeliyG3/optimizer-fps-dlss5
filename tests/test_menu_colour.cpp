// Menu mode: the colour check and CSConvert's as-is copies on WARP (debug layer on).
// usage: ofps_menu_colour_tests <dir with menu_convert_cs.dxbc>
#include "test_menu_colour_images.h"
#include "hosts/reshade/menu_colour.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

using namespace ofps::reshade;
using namespace menutest;
using coretest::ComPtr;

namespace {
int g_failures = 0;
void Require(bool ok, const char *what) {
    if (!ok) { ++g_failures; std::fprintf(stderr, "FAIL: %s\n", what); }
}

void TestProblems() {
    using S = MenuColourSpace;
    struct Case { S space; DXGI_FORMAT format; bool ok; const char *what; };
    const Case cases[] = {
        {S::Srgb, DXGI_FORMAT_R8G8B8A8_UNORM, true, "sRGB RGBA8"},
        {S::Srgb, DXGI_FORMAT_B8G8R8A8_UNORM, true, "sRGB BGRA8"},
        {S::Srgb, DXGI_FORMAT_R10G10B10A2_UNORM, true, "sRGB RGB10A2"},
        {S::Srgb, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false, "sRGB-typed back buffer"},
        {S::Srgb, DXGI_FORMAT_R8G8B8A8_TYPELESS, false, "typeless back buffer"},
        {S::Srgb, DXGI_FORMAT_R16G16B16A16_FLOAT, false, "sRGB colour space on FP16"},
        {S::Scrgb, DXGI_FORMAT_R16G16B16A16_FLOAT, true, "scRGB FP16 goes as it is"},
        {S::Scrgb, DXGI_FORMAT_R10G10B10A2_UNORM, false, "scRGB without an FP16 back buffer"},
        {S::Hdr10Pq, DXGI_FORMAT_R10G10B10A2_UNORM, false, "HDR10 PQ"},
        {S::Hdr10Hlg, DXGI_FORMAT_R10G10B10A2_UNORM, false, "HDR10 HLG"},
        {S::Unknown, DXGI_FORMAT_R8G8B8A8_UNORM, false, "unknown colour space"},
    };
    for (const Case &c : cases) {
        const char *problem = MenuColourProblem(c.space, c.format);
        Require((problem == nullptr) == c.ok && (c.ok || problem[0] != '\0'), c.what);
    }
    Require(MenuConvertViewFormat(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) == DXGI_FORMAT_UNKNOWN, "no view format for sRGB-typed");
    Require(MenuConvertViewFormat(DXGI_FORMAT_R8G8B8A8_TYPELESS) == DXGI_FORMAT_UNKNOWN, "no view format for typeless");
    std::printf("colour check: %zu cases\n", std::size(cases));
}

// Every 8-bit level on R (x) and G (y) (x and y wrap at 256), a one-pixel checker on B, opaque.
Image Pattern(std::uint32_t width, std::uint32_t height) {
    Image image(size_t(width) * height);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            image[y * width + x] = Pixel{(x & 255u) / 255.0f, (y & 255u) / 255.0f, ((x ^ y) & 1u) ? 0.8f : 0.1f, 1.0f};
    return image;
}

// back buffer (format b) -> host colour (format h) -> back buffer format, both copies through CSConvert on WARP.
// `host` gets the intermediate host-format texture (the NR input copy), `back` the final back buffer.
bool RoundTrip(coretest::WarpDevice &w, const MenuConvertPipeline &pipe, DXGI_FORMAT b, DXGI_FORMAT h, const Image &source,
               std::uint32_t width, std::uint32_t height, Image *hostImage, Image *back) {
    constexpr auto kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   kCopy = D3D12_RESOURCE_STATE_COPY_DEST;
    const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto src = coretest::CreateTexture(w.device.Get(), width, height, b, D3D12_RESOURCE_FLAG_NONE, kCopy);
    auto host = coretest::CreateTexture(w.device.Get(), width, height, h, uav, kUav);
    auto dst = coretest::CreateTexture(w.device.Get(), width, height, b, uav, kUav);
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (!src || !host || !dst || FAILED(w.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return false;
    const UINT step = w.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = [&](UINT i) { auto c = heap->GetCPUDescriptorHandleForHeapStart(); c.ptr += SIZE_T(i) * step; return c; };
    auto gpu = [&](UINT i) { auto g = heap->GetGPUDescriptorHandleForHeapStart(); g.ptr += UINT64(i) * step; return g; };
    auto srv = [&](ID3D12Resource *res, UINT i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{}; v.Format = MenuConvertViewFormat(res->GetDesc().Format);
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1; w.device->CreateShaderResourceView(res, &v, cpu(i));
    };
    auto uavView = [&](ID3D12Resource *res, UINT i) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v{}; v.Format = MenuConvertViewFormat(res->GetDesc().Format);
        v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; w.device->CreateUnorderedAccessView(res, nullptr, &v, cpu(i));
    };
    srv(src.Get(), 0); uavView(host.Get(), 1); srv(host.Get(), 2); uavView(dst.Get(), 3);
    const auto backRead = coretest::CreateReadback(w.device.Get(), dst.Get());
    const auto hostRead = coretest::CreateReadback(w.device.Get(), host.Get());
    ComPtr<ID3D12Resource> upload;
    if (!coretest::BeginList(w) || !Upload(w, src.Get(), b, source, width, height, upload)) return false;
    coretest::Transition(w.list.Get(), src.Get(), kCopy, kSrv);
    MenuConvertRecord(w.list.Get(), pipe, heap.Get(), gpu(0), host.Get());
    coretest::Transition(w.list.Get(), host.Get(), kUav, kSrv);
    MenuConvertRecord(w.list.Get(), pipe, heap.Get(), gpu(2), dst.Get());
    coretest::Transition(w.list.Get(), host.Get(), kSrv, kUav);
    coretest::CaptureTarget(w, host.Get(), kUav, hostRead); // the NR input copy, read back on its own
    coretest::CaptureTarget(w, dst.Get(), kUav, backRead);
    if (!coretest::SubmitList(w) || !coretest::WaitForQueue(w.device.Get(), w.queue.Get())) return false;
    *hostImage = Read(hostRead, h, width, height);
    *back = Read(backRead, b, width, height);
    return !back->empty() && !hostImage->empty();
}

// The frame as it is: the NR input copy holds what the back buffer held (converted to the host's storage format), and
// after the round trip every channel equals the back buffer's again (host formats of the stand hosts: RGBA16F for
// renodx, R10G10B10A2 and RGBA8 for AIO). Checking the middle texture too catches a channel swap done by both copies.
void TestCopy(coretest::WarpDevice &w, const MenuConvertPipeline &pipe, DXGI_FORMAT b, DXGI_FORMAT h, std::uint32_t width = 256,
              std::uint32_t height = 256) {
    const Image source = Pattern(width, height);
    Image hostImage, back;
    const std::string what = "copy " + std::to_string(width) + "x" + std::to_string(height) + " fmt " + std::to_string(int(b)) +
                             " -> host fmt " + std::to_string(int(h)) + " -> back";
    if (!RoundTrip(w, pipe, b, h, source, width, height, &hostImage, &back)) { Require(false, (what + ": GPU run").c_str()); return; }
    const float hostTolerance = h == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1e-3f : 1e-6f; // half floats round the level
    size_t differ = 0, hostDiffer = 0;
    for (size_t i = 0; i < source.size(); ++i) {
        const Pixel stored = Stored(b, source[i]), expectedHost = Stored(h, stored);
        for (int c = 0; c < 4; ++c) {
            differ += std::abs(back[i][c] - stored[c]) > 1e-6f ? 1u : 0u;
            hostDiffer += std::abs(hostImage[i][c] - expectedHost[c]) > hostTolerance ? 1u : 0u;
        }
    }
    std::printf("%-48s %zu channel values differ (host texture: %zu)\n", what.c_str(), differ, hostDiffer);
    Require(differ == 0, (what + ": exact").c_str());
    Require(hostDiffer == 0, (what + ": host texture").c_str());
}
} // namespace

int main(int argc, char **argv) {
    TestProblems();
    if (argc < 2) { std::fprintf(stderr, "usage: ofps_menu_colour_tests <shader dir>\n"); return 2; }
    std::ifstream file(std::string(argv[1]) + "\\menu_convert_cs.dxbc", std::ios::binary);
    const std::string code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    coretest::WarpDevice w;
    MenuConvertPipeline pipe;
    if (code.empty() || !coretest::CreateWarpDevice(w) || !MenuConvertBuild(w.device.Get(), code.data(), code.size(), &pipe)) {
        std::fprintf(stderr, "FAIL: WARP device or menu_convert_cs.dxbc (%zu bytes)\n", code.size());
        return 1;
    }
    TestCopy(w, pipe, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT);
    TestCopy(w, pipe, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT);
    TestCopy(w, pipe, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT);
    TestCopy(w, pipe, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, 250, 130); // not a multiple of 8: the bounds check
    TestCopy(w, pipe, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT); // scRGB back buffer, FP16 NR input
    TestCopy(w, pipe, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM);
    TestCopy(w, pipe, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM);
    Require(!coretest::HasDebugErrors(w.device.Get()), "no D3D12 debug-layer errors");
    if (g_failures) { std::fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    std::printf("menu colour: all passed\n");
    return 0;
}
