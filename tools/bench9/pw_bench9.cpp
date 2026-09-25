// pw_bench9: a 32-bit Direct3D 9 scene for the chain the 32-bit games use -
// D3D9 -> dgVoodoo (D3D11) -> ReShade x86 -> feed32 -> host64 (Neural Rendering + Optimizer FPS).
//
// Deterministic: the scene is a function of the frame number only, so two runs with different settings
// capture the same frames. Fixed-function pipeline, no shaders: a checkered floor and a ring of spinning
// textured cubes under a camera that circles and bobs, with a depth buffer the ReShade depth add-on picks
// up like any game's.
//
//   pw_bench9.exe [--frames N] [--size WxH] [--dump a,b,c] [--dump-dir DIR] [--speed S]
//                 [--offthread FROM,TO] [--no-hud] [--popup] [--pos X,Y]
//
// --dump writes the window's pixels as BMP after the frame was presented (a screen grab, so the picture
// is what the chain put on the screen, the neural pass included). --offthread presents frames FROM..TO
// from a second thread while the first keeps drawing into the same device, the way Dragon Age draws its
// loading screen; the device is created multithreaded, as that game creates it.
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bench9_hud.h"
#include "bench9_math.h"

namespace {

using namespace bench9;

struct Options {
    int frames = 600;
    int width = 1920;
    int height = 1080;
    float speed = 1.0f;
    std::vector<int> dumps;
    std::string dumpDir = ".";
    int offFrom = -1;
    int offTo = -1;
    bool hud = true;
    bool popup = false; // borderless and topmost, e.g. full screen on a second monitor
    int posX = 40;
    int posY = 40;
};

struct Vertex {
    float x, y, z;
    float u, v;
};
constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_TEX1;

void SetMat(IDirect3DDevice9 *dev, D3DTRANSFORMSTATETYPE t, const Mat &m)
{
    dev->SetTransform(t, reinterpret_cast<const D3DMATRIX *>(m.m));
}

// A texture with detail at several scales - fine checks, stripes, a few blobs - so a reprojection or
// a block-wise motion field shows up as broken edges rather than hiding in flat colour.
IDirect3DTexture9 *MakeTexture(IDirect3DDevice9 *dev, int kind)
{
    constexpr int kSize = 256;
    IDirect3DTexture9 *tex = nullptr;
    if (FAILED(dev->CreateTexture(kSize, kSize, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)))
        return nullptr;
    D3DLOCKED_RECT lr{};
    tex->LockRect(0, &lr, nullptr, 0);
    for (int y = 0; y < kSize; ++y) {
        auto *row = reinterpret_cast<DWORD *>(static_cast<BYTE *>(lr.pBits) + y * lr.Pitch);
        for (int x = 0; x < kSize; ++x) {
            int r, g, b;
            if (kind == 0) { // floor: checks of two sizes
                const bool coarse = ((x / 64) + (y / 64)) & 1;
                const bool fine = ((x / 8) + (y / 8)) & 1;
                const int v = coarse ? 190 : 70;
                r = g = b = fine ? v : v - 35;
                // Irregular fine detail on top, one texel apart: grass, gravel, foliage.
                const unsigned hsh = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(y) * 19349663u);
                const int grain = static_cast<int>((hsh >> 7) % 61u) - 30;
                r += grain; g += grain + 10; b += grain;
                r = r < 0 ? 0 : r; g = g < 0 ? 0 : g; b = b < 0 ? 0 : b;
            } else { // cubes: coloured stripes over a fine grid
                const int stripe = (x + y) / 16 % 3;
                r = stripe == 0 ? 220 : 60; g = stripe == 1 ? 200 : 60; b = stripe == 2 ? 230 : 70;
                if (x % 32 < 2 || y % 32 < 2) r = g = b = 20;
            }
            row[x] = D3DCOLOR_XRGB(r & 255, g & 255, b & 255);
        }
    }
    tex->UnlockRect(0);
    return tex;
}

void AppendQuad(std::vector<Vertex> &v, const float p[4][3], float uv)
{
    const int order[6] = {0, 1, 2, 0, 2, 3};
    const float tc[4][2] = {{0, 0}, {uv, 0}, {uv, uv}, {0, uv}};
    for (int i : order) v.push_back({p[i][0], p[i][1], p[i][2], tc[i][0], tc[i][1]});
}

std::vector<Vertex> MakeCube()
{
    std::vector<Vertex> v;
    const float s = 0.5f;
    const float faces[6][4][3] = {
        {{-s, -s, -s}, {-s, s, -s}, {s, s, -s}, {s, -s, -s}}, {{s, -s, s}, {s, s, s}, {-s, s, s}, {-s, -s, s}},
        {{-s, -s, s}, {-s, s, s}, {-s, s, -s}, {-s, -s, -s}}, {{s, -s, -s}, {s, s, -s}, {s, s, s}, {s, -s, s}},
        {{-s, s, -s}, {-s, s, s}, {s, s, s}, {s, s, -s}}, {{-s, -s, s}, {-s, -s, -s}, {s, -s, -s}, {s, -s, s}}};
    for (const auto &f : faces) AppendQuad(v, f, 1.0f);
    return v;
}

std::vector<Vertex> MakeFloor()
{
    std::vector<Vertex> v;
    const float e = 40.0f;
    const float f[4][3] = {{-e, 0, -e}, {-e, 0, e}, {e, 0, e}, {e, 0, -e}};
    AppendQuad(v, f, 16.0f);
    return v;
}

// Grab the window's client area from the screen and write it as a 24-bit BMP. The window is
// topmost for this reason; PrintWindow returns black for the flip-model swap chain dgVoodoo uses.
void DumpWindow(HWND hwnd, const std::string &path)
{
    RECT rc{};
    GetClientRect(hwnd, &rc);
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    const int w = rc.right, h = rc.bottom;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h; // bottom-up, as BMP stores it
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    void *bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    BitBlt(mem, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);
    const int stride = ((w * 3 + 3) / 4) * 4;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + stride * h;
    if (FILE *f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(&fh, sizeof(fh), 1, f);
        std::fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
        std::fwrite(bits, 1, static_cast<size_t>(stride) * h, f);
        std::fclose(f);
    }
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct Scene {
    IDirect3DDevice9 *dev = nullptr;
    IDirect3DTexture9 *floorTex = nullptr;
    IDirect3DTexture9 *cubeTex = nullptr;
    std::vector<Vertex> cube, floor;
    float aspect = 16.0f / 9.0f;
    float speed = 1.0f;
    const Hud *hud = nullptr;

    void Draw(int frame)
    {
        const float t = frame / 60.0f * speed;
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(96, 128, 170), 1.0f, 0);
        dev->BeginScene();
        const float camA = t * 0.35f;
        const Mat view = LookAt(std::sin(camA) * 9.0f, 2.2f + 0.6f * std::sin(t * 0.7f), -std::cos(camA) * 9.0f, 0.0f, 0.8f, 0.0f);
        SetMat(dev, D3DTS_VIEW, view);
        SetMat(dev, D3DTS_PROJECTION, Perspective(1.05f, aspect, 0.2f, 120.0f));
        dev->SetFVF(kFvf);
        dev->SetTexture(0, floorTex);
        SetMat(dev, D3DTS_WORLD, Identity());
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(floor.size() / 3), floor.data(), sizeof(Vertex));
        dev->SetTexture(0, cubeTex);
        for (int i = 0; i < 16; ++i) {
            const float a = i * 0.3927f;
            const float r = 3.0f + 1.5f * (i % 3);
            const Mat world = Mul(Mul(RotateX(t * 0.9f + i), RotateY(t * 1.3f + i * 0.5f)),
                                  Translate(std::cos(a) * r, 0.6f + 0.3f * (i % 4), std::sin(a) * r));
            SetMat(dev, D3DTS_WORLD, world);
            dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(cube.size() / 3), cube.data(), sizeof(Vertex));
        }
        if (hud) hud->Draw(dev);
        dev->EndScene();
    }
};

struct OffThread {
    Scene *scene = nullptr;
    HANDLE go = nullptr, done = nullptr;
    volatile LONG frame = 0;
    volatile LONG quit = 0;
};

// The loading-screen presenter: draws and presents whatever frame it is told to, from its own thread.
DWORD WINAPI OffThreadMain(void *param)
{
    auto *o = static_cast<OffThread *>(param);
    for (;;) {
        WaitForSingleObject(o->go, INFINITE);
        if (o->quit) return 0;
        o->scene->Draw(o->frame);
        o->scene->dev->Present(nullptr, nullptr, nullptr, nullptr);
        SetEvent(o->done);
    }
}

Options Parse(int argc, char **argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--frames") { o.frames = std::atoi(next); ++i; }
        else if (a == "--size") { std::sscanf(next, "%dx%d", &o.width, &o.height); ++i; }
        else if (a == "--no-hud") { o.hud = false; }
        else if (a == "--popup") { o.popup = true; }
        else if (a == "--pos") { std::sscanf(next, "%d,%d", &o.posX, &o.posY); ++i; }
        else if (a == "--speed") { o.speed = static_cast<float>(std::atof(next)); ++i; }
        else if (a == "--dump-dir") { o.dumpDir = next; ++i; }
        else if (a == "--offthread") { std::sscanf(next, "%d,%d", &o.offFrom, &o.offTo); ++i; }
        else if (a == "--dump") {
            std::string s = next;
            size_t pos = 0;
            while (pos < s.size()) {
                const size_t c = s.find(',', pos);
                o.dumps.push_back(std::atoi(s.substr(pos, c - pos).c_str()));
                if (c == std::string::npos) break;
                pos = c + 1;
            }
            ++i;
        }
    }
    return o;
}

} // namespace

int main(int argc, char **argv)
{
    const Options opt = Parse(argc, argv);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"pw_bench9";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    const DWORD style = opt.popup ? (WS_POPUP | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    RECT rc{0, 0, opt.width, opt.height};
    AdjustWindowRect(&rc, style, FALSE);
    HWND hwnd = CreateWindowW(L"pw_bench9", L"pw_bench9", style, opt.posX, opt.posY, rc.right - rc.left,
                              rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    // Captures are screen grabs of this window, so nothing may cover it.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { std::printf("[fail] Direct3DCreate9\n"); return 2; }
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = opt.width;
    pp.BackBufferHeight = opt.height;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    pp.hDeviceWindow = hwnd;
    Scene scene;
    const DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED;
    if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, flags, &pp, &scene.dev))) {
        std::printf("[fail] CreateDevice\n");
        return 2;
    }
    scene.aspect = static_cast<float>(opt.width) / opt.height;
    scene.speed = opt.speed;
    scene.floorTex = MakeTexture(scene.dev, 0);
    scene.cubeTex = MakeTexture(scene.dev, 1);
    scene.cube = MakeCube();
    scene.floor = MakeFloor();
    Hud hud;
    if (opt.hud && hud.Create(scene.dev, opt.width, opt.height)) scene.hud = &hud;
    scene.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    scene.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    scene.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    for (DWORD s = 0; s < 1; ++s) {
        scene.dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
        scene.dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        scene.dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        scene.dev->SetSamplerState(s, D3DSAMP_MAXANISOTROPY, 8);
    }

    OffThread off;
    off.scene = &scene;
    off.go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    off.done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE offThread = CreateThread(nullptr, 0, OffThreadMain, &off, 0, nullptr);

    std::printf("[info] pw_bench9 %dx%d, %d frames, off-thread presents %d..%d\n", opt.width, opt.height, opt.frames,
                opt.offFrom, opt.offTo);
    int result = 0;
    for (int frame = 0; frame < opt.frames; ++frame) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) frame = opt.frames;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const bool offThreadFrame = frame >= opt.offFrom && frame <= opt.offTo;
        if (offThreadFrame) {
            off.frame = frame;
            SetEvent(off.go);
            // Meanwhile the main thread keeps touching the device, as a loading game does.
            IDirect3DSurface9 *bb = nullptr;
            scene.dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
            if (bb) bb->Release();
            WaitForSingleObject(off.done, INFINITE);
        } else {
            scene.Draw(frame);
            const HRESULT hr = scene.dev->Present(nullptr, nullptr, nullptr, nullptr);
            if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICEREMOVED) {
                std::printf("[fail] frame %d: device lost (0x%08lx)\n", frame, static_cast<unsigned long>(hr));
                result = 3;
                break;
            }
        }
        for (int d : opt.dumps) {
            if (d != frame) continue;
            Sleep(60); // let the compositor show the frame that was just presented
            char name[64];
            std::snprintf(name, sizeof(name), "dump_%04d.bmp", frame);
            DumpWindow(hwnd, opt.dumpDir + "\\" + name);
        }
    }
    off.quit = 1;
    SetEvent(off.go);
    WaitForSingleObject(offThread, 2000);
    std::printf("[info] finished, exit %d\n", result);
    hud.Release();
    scene.dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    return result;
}
