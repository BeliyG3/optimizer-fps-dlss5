// pw_bench: a tiny D3D11 "game" with native DLSS through the driver's NGX core. Put Reshade64.dll as
// dxgi.dll, the DLSS 5 bridge, renodx-dlss5, optimizer-fps-dlss5.addon64 (+ forwarder and shaders), the
// DLSS/DLSSNR snippets and the configs beside it, and the whole BG3 chain runs here without the game.
// Exit code: 0 after N frames with a live device, 2 when the D3D11 device is removed, 1 on setup errors,
// 3 when the bridge/ReShade logs show the bridge stopped or the D3D12 device was removed.
//
// Usage: pw_bench [frames] [options]
//   --scene 3d|flat        3D: textured ground, boxes, a figure at the orbit centre, third-person camera
//                          (default); flat: the old moving checkerboard.
//   --yaw-speed D        camera yaw rate in degrees per second for the yaw phases (default 45)
//   --fps-cap N          pace the frames to N per second on the CPU (spin-wait), giving the GPU idle time as a game would
//   --frametime          print statistics of the Present-to-Present intervals (frames 60..end): avg, min, max,
//                        standard deviation, share of frames longer than 1.5x the average
//   --gltf FILE.glb      render a glTF scene (Blender export of tools/bench/assets/bench_scene.blend) instead of the
//                        procedural one; node animations drive the objects and the file's camera drives the view
//                        (use --camera to override with the scripted path). Blender's stripe materials
//                        ("Palette_*") keep their stripes.
//   --camera face          (--gltf) close-up orbit around the head: the bounding box of the meshes under the node
//                          --face-node NAME (default "CharacterAnchor", else the first node whose name contains
//                          "girl"/"character"/"head"), recomputed every frame; a slow +-25 deg sweep, 35 deg FOV;
//                          --face-yaw D turns the sweep's centre (degrees, 0 = in front of a face looking along +Z),
//                          --face-angle D the camera's horizontal offset from that axis so the head reads slightly
//                          turned (default -15: turned right-to-left; +15 mirrors it), --face-radius R the
//                          distance (scene units; 0 = auto, the head's height filling 75% of the frame),
//                          --face-sweep A the sweep amplitude (0 = still)
//   --camera blender       the look tuned in Blender for the cutegirl head, baked in: --cam-pos/-target/--fov,
//                          --cam-shift, --sun-dir/--sun-strength, --scene-scale 100, --hdri-yaw -90 and
//                          --scene-yaw 180 as defaults (each still overridable). Nothing is read from disk.
//   --cam-pos x,y,z        fixed camera, glTF Y-up, in the units --scene-scale converts from (metres by
//   --cam-target x,y,z     default). Overrides every --camera path when both are given.
//   --fov D                vertical field of view in degrees for the camera override (Blender's 57 mm lens
//                          on a 16:9 frame is 23.7773).
//   --cam-shift x,y        Blender's camera shift_x,shift_y for the camera override, in units of the frame's
//                          height (its "VERTICAL" sensor fit). Off-centre framing without turning the camera.
//   --scene-scale S        multiplies --cam-pos/--cam-target into the scene's own units (default 1; the
//                          cutegirl .glb is authored in centimetres, so metres x 100 -> S = 100).
//   --scene-yaw D          the loaded asset is turned D degrees about +Y relative to the scene the camera,
//                          sun and HDRI numbers were authored in; the camera override, --sun-dir, --hdri-yaw
//                          and --camera face's orbit are all turned by D to compensate (default 0; 180 for
//                          --camera blender, because cutegirl.glb was re-exported facing glTF +Z while the
//                          .blend the look was tuned in has it facing -Z).
//   --sun-dir x,y,z        directional light, glTF Y-up, the direction the light TRAVELS (Blender's own
//                          convention); the shader's lightDir is its negation. Bypasses the HDRI sun search,
//                          and is NOT turned by --hdri-yaw (it is already in world space).
//   --sun-strength E       sun irradiance in the HDRI's radiance units (Blender's W/m^2 at HDRI strength 1);
//                          the shader gets E/pi, white. Bypasses --sun-scale / kSunOverSky.
//   --anim-speed S         glTF animation playback factor (default 1).
//   --shadow-soft W        penumbra half-width as a fraction of the shadow frustum's width (default 0.006);
//                          16-tap Poisson PCF with a per-pixel rotation, radius clamped to 1..48 texels.
//   --camera script|static|yaw|forward|strafe|combo   camera path for the 3D scene (script = all
//                          phases, 120 frames each, looping).
//   --moving-boxes         two boxes move on their own (object motion vectors).
//   --reference            no DLSS/NR: the 3D scene is rendered straight at 3840x2160 with 8 jitter
//                          samples averaged; --dump writes these frames (ground truth for metrics).
//   --hdri FILE.hdr|none   environment lighting for the 3D scene from a Radiance equirect HDRI: sky
//                          background, diffuse ambient and the sun's direction/colour come from it
//                          (default: assets/external/golden_gate_hills_2k.hdr when it exists).
//   --hdri-yaw D|auto      turn the environment (sky, ambient and the extracted sun alike) about the vertical
//                          axis. "auto" (default) puts the sun 40 deg to the side of the face camera's axis, a
//                          front-side key light; it resolves to 0 for every other camera and to 90 for
//                          --camera blender (before --scene-yaw is added to it). BLENDER PARITY:
//                          Blender's equirect world is ours turned a quarter turn - u_blender = u_ours - 0.25,
//                          v identical, no mirror - and the Mapping node turns the LOOKUP VECTOR while our
//                          yaw turns the MAP, so the senses are opposite:
//                                            ourYaw = -90 deg - blenderYaw.
//                          Measured, not guessed: an equirect probe image carrying R = u, G = v was put in
//                          the .blend's world and rendered through its own camera with Cycles (shift zeroed,
//                          view transform Raw). Reading Blender's (u,v) off every pixel and comparing it with
//                          EquirectUV for the same world direction gives du = -0.2512 +- 0.0016 (-90.44 deg,
//                          the spread is RGBE quantisation) and dv = -0.002 +- 0.001. u grows to the right in
//                          both, so the mappings are a pure rotation apart. The sun lamp is NOT a witness
//                          here: the user aimed it by eye, ~100 deg off the HDRI's own sun.
//   --sun-scale F          sun irradiance as a multiple of the HDRI's mean radiance (default 1.2: a soft,
//                          low-contrast key). The measured sun disk is normalised to this.
//   --exposure F           linear multiplier on the scene radiance (sky, ambient, sun) before the
//                          Khronos PBR Neutral tone mapper (default 1.45).
//   --switch N             every N frames cycle the NR add-on's layout Off -> Uniform -> Peripheral.
//   --temporal M[,N]       NR add-on temporal mode M with a full pass every N frames (from frame 10).
//   --dump A[,B,...]       write frame A, B, ... of the output as full-size BMP (dump_<n>.bmp).
//   --measure N            every N frames print the mean luminance of the output.
//   --mv-scale S           MV_Scale handed to DLSS (BG3 uses -1); stored vectors are divided by it.
//   --mv-dir +1|-1         sign of the stored vectors: +1 = NGX convention (current -> previous).
//   --present WxH          swap chain size (the DLSS output stays 4K).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wincodec.h> // decoding the textures embedded in a .glb (WIC)
#include <wrl/client.h>

#include "nvsdk_ngx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

struct LayoutStateV1 {
    unsigned int structSize, mode, filter;
    float centerX, workX, centerY, workY, globalScalePercent;
    unsigned int generation;
};
using PFN_SetLayout = unsigned int(__cdecl *)(const LayoutStateV1 *);

static bool LogContains(const char *path, const char *const *needles, int count, std::string *hit)
{
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        for (int i = 0; i < count; ++i) {
            if (line.find(needles[i]) != std::string::npos) { *hit = std::string(path) + ": " + line; return true; }
        }
    }
    return false;
}

// Scans the chain's own logs: the bridge does not remove the D3D11 device when its D3D12 side dies.
static int VerdictFromLogs()
{
    static const char *const bad[] = {"stopped:", "DEVICE REMOVED", "device was removed", "during stage",
                                      "did not retire", "OBJECT_DELETED_WHILE_STILL_IN_USE"};
    std::string hit;
    if (LogContains("dlss5-dx11-bridge.log", bad, 6, &hit) || LogContains("ReShade.log", bad, 6, &hit)) {
        std::printf("[fail] %s\n", hit.c_str());
        return 3;
    }
    return 0;
}

using Microsoft::WRL::ComPtr;

static const UINT kRenderW = 2258, kRenderH = 1270, kOutW = 3840, kOutH = 2160;

using PFN_Init = NVSDK_NGX_Result(NVSDK_CONV *)(unsigned long long, const wchar_t *, ID3D11Device *, const NVSDK_NGX_FeatureCommonInfo *, NVSDK_NGX_Version);
using PFN_Alloc = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter **);
using PFN_Create = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D11DeviceContext *, NVSDK_NGX_Feature, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
using PFN_Evaluate = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D11DeviceContext *, const NVSDK_NGX_Handle *, NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback);
using PFN_Release = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Handle *);
using PFN_Shutdown1 = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D11Device *);

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------------------------
// Minimal matrix math: row-major storage m[r][c], column-vector convention (clip = P * V * world),
// matching `mul(M, v)` in HLSL with row_major packing.
// ---------------------------------------------------------------------------------------------
struct Vec3 { float x, y, z; };
static Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static Vec3 Normalize(Vec3 a) { const float l = std::sqrt(Dot(a, a)); return l > 0 ? a * (1.0f / l) : a; }

struct Mat4 { float m[4][4]; };
static Mat4 Identity() { Mat4 r{}; for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f; return r; }
static Mat4 Mul(const Mat4 &a, const Mat4 &b)
{
    Mat4 r{};
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
// Left-handed look-at (x right, y up, z into the screen).
static Mat4 LookAt(Vec3 eye, Vec3 target)
{
    const Vec3 f = Normalize(target - eye);
    const Vec3 r = Normalize(Cross({0, 1, 0}, f));
    const Vec3 u = Cross(f, r);
    Mat4 v = Identity();
    v.m[0][0] = r.x; v.m[0][1] = r.y; v.m[0][2] = r.z; v.m[0][3] = -Dot(r, eye);
    v.m[1][0] = u.x; v.m[1][1] = u.y; v.m[1][2] = u.z; v.m[1][3] = -Dot(u, eye);
    v.m[2][0] = f.x; v.m[2][1] = f.y; v.m[2][2] = f.z; v.m[2][3] = -Dot(f, eye);
    return v;
}
// Reverse-Z perspective (depth 1 at the near plane, 0 at the far plane) with a sub-pixel jitter in
// pixels of the target (x right, y down), as games do for DLSS.
static Mat4 Perspective(float fovYDeg, float aspect, float n, float f, float jx, float jy, float w, float h)
{
    const float ys = 1.0f / std::tan(fovYDeg * 3.14159265f / 360.0f);
    const float xs = ys / aspect;
    Mat4 p{};
    p.m[0][0] = xs;
    p.m[1][1] = ys;
    p.m[0][2] = 2.0f * jx / w;
    p.m[1][2] = -2.0f * jy / h;
    p.m[2][2] = n / (n - f);
    p.m[2][3] = n * f / (f - n);
    p.m[3][2] = 1.0f;
    return p;
}

// Orthographic projection with a normal 0..1 depth (the shadow pass, unlike the reverse-Z main pass).
static Mat4 Ortho(float l, float r, float b, float t, float zn, float zf)
{
    Mat4 p{};
    p.m[0][0] = 2.0f / (r - l); p.m[0][3] = -(r + l) / (r - l);
    p.m[1][1] = 2.0f / (t - b); p.m[1][3] = -(t + b) / (t - b);
    p.m[2][2] = 1.0f / (zf - zn); p.m[2][3] = -zn / (zf - zn);
    p.m[3][3] = 1.0f;
    return p;
}

// General 4x4 inverse (Gauss-Jordan): the sky pass needs the inverse view-projection to turn a
// pixel back into a world-space view ray.
static Mat4 Invert(const Mat4 &src)
{
    double a[4][8]{};
    for (int r = 0; r < 4; ++r) { for (int c = 0; c < 4; ++c) a[r][c] = src.m[r][c]; a[r][4 + r] = 1.0; }
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        if (std::fabs(a[piv][c]) < 1e-20) return Identity();
        if (piv != c) for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[piv][k]);
        const double inv = 1.0 / a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] *= inv;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; if (f != 0.0) for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k]; }
    }
    Mat4 out{};
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out.m[r][c] = (float) a[r][4 + c];
    return out;
}

// ---------------------------------------------------------------------------------------------
// Radiance (.hdr) RGBE image: header lines, "-Y h +X w", then new-style RLE scanlines (marker
// 2 2 hi lo, four run-length encoded channel planes) with the flat non-RLE layout as fallback.
// Row 0 is the top of the image (-Y), matching v = 0 = straight up in the equirect mapping.
// ---------------------------------------------------------------------------------------------
struct HdrImage { int w = 0, h = 0; std::vector<float> rgb; }; // 3 floats per pixel, row-major from the top

static bool LoadHdr(const char *path, HdrImage &img)
{
    std::vector<unsigned char> data;
    {
        FILE *fp = nullptr; fopen_s(&fp, path, "rb");
        if (!fp) return false;
        fseek(fp, 0, SEEK_END); const long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (n <= 0) { fclose(fp); return false; }
        data.resize((size_t) n);
        const size_t got = fread(data.data(), 1, data.size(), fp);
        fclose(fp);
        if (got != data.size()) return false;
    }
    size_t p = 0;
    auto line = [&]() { std::string s; while (p < data.size() && data[p] != '\n') s.push_back((char) data[p++]); if (p < data.size()) ++p; return s; };
    const std::string magic = line();
    if (magic.rfind("#?", 0) != 0) return false;
    for (;;) { const std::string l = line(); if (l.empty()) break; if (p >= data.size()) return false; }
    int w = 0, h = 0;
    if (sscanf_s(line().c_str(), "-Y %d +X %d", &h, &w) != 2 || w <= 0 || h <= 0) return false;
    img.w = w; img.h = h; img.rgb.assign((size_t) w * h * 3, 0.0f);
    std::vector<unsigned char> scan((size_t) w * 4);
    auto store = [&](int y) {
        float *dst = img.rgb.data() + (size_t) y * w * 3;
        for (int x = 0; x < w; ++x) {
            const int e = scan[(size_t) w * 3 + x];
            const float s = e ? std::ldexp(1.0f / 256.0f, e - 128) : 0.0f;
            dst[x * 3 + 0] = (scan[x] + 0.5f) * s;
            dst[x * 3 + 1] = (scan[(size_t) w + x] + 0.5f) * s;
            dst[x * 3 + 2] = (scan[(size_t) w * 2 + x] + 0.5f) * s;
        }
    };
    for (int y = 0; y < h; ++y) {
        if (p + 4 > data.size()) return false;
        const unsigned char *m = &data[p];
        if (m[0] == 2 && m[1] == 2 && ((m[2] << 8) | m[3]) == w && w >= 8 && w < 0x8000) {
            p += 4;
            for (int c = 0; c < 4; ++c) {
                int x = 0;
                while (x < w) {
                    if (p >= data.size()) return false;
                    int count = data[p++];
                    if (count > 128) { // a run of one value
                        count -= 128;
                        if (p >= data.size() || x + count > w) return false;
                        const unsigned char v = data[p++];
                        while (count-- > 0) scan[(size_t) c * w + x++] = v;
                    } else {           // literal bytes
                        if (count == 0 || p + count > data.size() || x + count > w) return false;
                        while (count-- > 0) scan[(size_t) c * w + x++] = data[p++];
                    }
                }
            }
        } else { // flat RGBE, one 4-byte pixel after another (old-style runs are not produced by 2k HDRIs)
            if (p + (size_t) w * 4 > data.size()) return false;
            for (int x = 0; x < w; ++x) for (int c = 0; c < 4; ++c) scan[(size_t) c * w + x] = data[p + (size_t) x * 4 + c];
            p += (size_t) w * 4;
        }
        store(y);
    }
    return true;
}

// --hdri-yaw: the environment is turned about the vertical axis in exactly one place, the equirect
// mapping below (mirrored by EquirectUV in HLSL, which gets cos/sun through env.xy): a world
// direction is rotated by -yaw into HDRI space, so the map appears turned by +yaw. Everything that
// comes *out* of the map in HDRI space - only the extracted sun direction - is turned by +yaw with
// RotateHdriYaw, so the shadows stay consistent with the sky.
static float g_hdriYawCos = 1.0f, g_hdriYawSin = 0.0f;
static float g_hdriMeanL = 1.0f; // solid-angle weighted mean radiance (luma) of the loaded HDRI
static bool g_hdriMirror = false; // --hdri-mirror: u -> 1 - u (the bench world is the glTF/Blender world mirrored in Z)
static Vec3 RotateHdriYaw(Vec3 m) { return {m.x * g_hdriYawCos + m.z * g_hdriYawSin, m.y, -m.x * g_hdriYawSin + m.z * g_hdriYawCos}; } // HDRI space -> world

// The one equirect mapping used by the sky, the ambient lookup and the sun search (mirrored in HLSL).
static void HdrDirToUv(Vec3 w, float &u, float &v)
{
    const Vec3 d{w.x * g_hdriYawCos - w.z * g_hdriYawSin, w.y, w.x * g_hdriYawSin + w.z * g_hdriYawCos}; // world -> HDRI space
    u = std::atan2(d.x, -d.z) * (1.0f / 6.2831853f) + 0.5f;
    if (g_hdriMirror) u = 1.0f - u;
    v = std::acos(std::max(-1.0f, std::min(1.0f, d.y))) * (1.0f / 3.14159265f);
}
// The plain inverse, in HDRI space: the sun search works on the map and rotates its result once.
static Vec3 HdrUvToDir(float u, float v)
{
    const float theta = v * 3.14159265f, phi = (u - 0.5f) * 6.2831853f;
    const float st = std::sin(theta);
    return {st * std::sin(phi), std::cos(theta), -st * std::cos(phi)};
}

static unsigned short FloatToHalf(float f)
{
    unsigned int x; memcpy(&x, &f, 4);
    const unsigned int sign = (x >> 16) & 0x8000u;
    int exp = (int) ((x >> 23) & 0xFF) - 127 + 15;
    unsigned int mant = x & 0x7FFFFFu;
    if (exp <= 0) return (unsigned short) sign;                       // underflow -> 0
    if (exp >= 31) return (unsigned short) (sign | 0x7BFFu);          // clamp to the largest finite half
    return (unsigned short) (sign | ((unsigned int) exp << 10) | (mant >> 13));
}

#include "pw_gltf.h" // .glb scenes (--gltf): meshes, node animation, camera

// ---------------------------------------------------------------------------------------------
// Shaders. The flat scene is the historical moving checkerboard; the 3D scene renders a textured
// ground, instanced boxes and a figure with game-style motion vectors (previous unjittered
// view-projection) and hardware reverse-Z depth.
// ---------------------------------------------------------------------------------------------
static const char *kShaderFlat = R"(
cbuffer C : register(b0) { float time; float vx; float vy; float invMvScale; float mvDir; float3 pad; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS(uint id : SV_VertexID) { VSOut o; float2 p = float2((id << 1) & 2, id & 2); o.uv = p; o.pos = float4(p * 2 - 1, 0.5, 1); o.pos.y = -o.pos.y; return o; }
struct PSOut { float4 color : SV_Target0; float2 motion : SV_Target1; float depth : SV_Depth; };
PSOut PS(VSOut i) {
    PSOut o;
    float2 uv = i.uv + float2(vx, vy) * time;
    float c = step(0.5, frac(uv.x * 24)) != step(0.5, frac(uv.y * 14)) ? 1.0 : 0.15;
    float ring = 0.5 + 0.5 * sin(length(i.uv - 0.5) * 60 - time * 6);
    o.color = float4(c * float3(0.9, 0.6, 0.3) + ring * 0.2, 1);
    o.motion = float2(vx, vy) / 60.0 * float2(2258, 1270) * mvDir * invMvScale; // per-frame motion; stored * MV_Scale = mvDir * (current -> previous) in pixels
    o.depth = 0.55 + 0.4 * sin(uv.x * 9.0) * cos(uv.y * 7.0); // inverted depth, moves with the content
    return o;
}
struct BlitOut { float4 color : SV_Target0; };
Texture2D src : register(t0); SamplerState smp : register(s0);
BlitOut PSBlit(VSOut i) { BlitOut o; o.color = src.Sample(smp, i.uv); return o; }
)";

static const char *kShader3D = R"(
#pragma pack_matrix(row_major)
cbuffer Frame : register(b0) {
    float4x4 vp;          // jittered view-projection (rendering)
    float4x4 vpNoJitter;  // unjittered, this frame (motion vectors)
    float4x4 vpPrev;      // unjittered, previous frame
    float4x4 lightVp;     // directional shadow map: orthographic light view-projection (0..1 depth)
    float4x4 invVp;       // inverse of vp: pixel -> world-space view ray (sky pass)
    float4 renderSize;    // width, height, mvDir, invMvScale
    float4 misc;          // sample weight (reference accumulation), time, debug mode, exposure
    float4 eyePos;        // camera world position (two-sided lighting), w = one shadow texel in world units
    float4 lightDir;      // xyz: direction towards the sun (from the HDRI when one is loaded), w: 1 = HDRI active
    float4 sunColor;      // rgb: sun irradiance / pi (linear radiance units), w: exposure
    float4 hdriParams;    // x: ambient radiance clamp (the sun disk must not leak into the diffuse lookup), yzw unused
    float4 env;           // xy: cos/sin of the HDRI yaw (world -> HDRI space), z: shadow-map normalized depth
                          //     per world unit (bias in world units -> ndc), w: PCF penumbra radius in shadow texels
};
cbuffer Instances : register(b1) {
    float4 boxPos[64];    // world position (centre), w = unused
    float4 boxPrev[64];   // previous-frame position
    float4 boxSize[64];   // half extents, w = colour index
};
struct VSOut { float4 pos : SV_Position; float3 world : TEXCOORD0; float3 worldPrev : TEXCOORD1; float3 normal : TEXCOORD2; float material : TEXCOORD3; float localY : TEXCOORD4; float instanceId : TEXCOORD5; float3 vcolor : TEXCOORD6; float2 uv : TEXCOORD7; float texFlags : TEXCOORD8; };
// Vertices of a loaded glTF scene (--gltf): world positions now / previous frame, material -1 ground, -2 flat colour, -3 colour + stripes.
struct MeshIn { float3 pos : POSITION; float3 prev : PREVPOS; float3 normal : NORMAL; float3 colour : COLOR; float3 misc : TEXCOORD0; float2 uv : TEXCOORD1; };
Texture2D meshTex : register(t1); SamplerState meshSmp : register(s1); // the primitive's base-colour texture (--gltf), white when none
Texture2D shadowMap : register(t2); SamplerComparisonState shadowSmp : register(s2); // 4096^2 directional shadow map
Texture2D hdri : register(t3); SamplerState hdriSmp : register(s3); // equirect HDRI, full mip chain (wrap U, clamp V)

// Khronos PBR Neutral tone mapper (glTF Sample Viewer): compresses the highlights towards white
// while keeping hue and saturation of the mid tones, which is what the grey Reinhard curve lost.
// In: linear scene radiance (already multiplied by the exposure). Out: linear 0..1 display values.
float3 PBRNeutralToneMapping(float3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    color = max(color, 0.0);
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(color, newPeak.xxx, g);
}

// The single equirect mapping (mirrored by HdrDirToUv on the C++ side), and the single place the
// environment's yaw (--hdri-yaw, env.xy = cos/sin) is applied: world direction -> HDRI space.
float2 EquirectUV(float3 w) {
    float3 d = float3(w.x * env.x - w.z * env.y, w.y, w.x * env.y + w.z * env.x);
    float u = atan2(d.x, -d.z) * 0.15915494 + 0.5;
    if (lightDir.w > 1.5) u = 1.0 - u; // --hdri-mirror (lightDir.w = 2)
    return float2(u, acos(clamp(d.y, -1, 1)) * 0.31830989);
}

static const float3 kCorner[8] = { float3(-1,-1,-1), float3(1,-1,-1), float3(1,1,-1), float3(-1,1,-1), float3(-1,-1,1), float3(1,-1,1), float3(1,1,1), float3(-1,1,1) };
static const uint kIndex[36] = { 0,2,1, 0,3,2,  4,5,6, 4,6,7,  0,1,5, 0,5,4,  3,7,6, 3,6,2,  0,4,7, 0,7,3,  1,2,6, 1,6,5 };
static const float3 kNormal[6] = { float3(0,0,-1), float3(0,0,1), float3(0,-1,0), float3(0,1,0), float3(-1,0,0), float3(1,0,0) };

VSOut VSBox(uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    VSOut o;
    float3 local = kCorner[kIndex[vid]] * boxSize[inst].xyz;
    o.world = boxPos[inst].xyz + local;
    o.worldPrev = boxPrev[inst].xyz + local;
    o.normal = kNormal[vid / 6];
    o.material = boxSize[inst].w;
    o.localY = local.y + boxSize[inst].y; // height above the box's own base: the stripes stay on the surface when the box moves
    o.instanceId = inst;
    o.vcolor = 0; o.uv = 0; o.texFlags = 0;
    o.pos = mul(vp, float4(o.world, 1));
    return o;
}
VSOut VSMesh(MeshIn v) {
    VSOut o;
    o.world = v.pos; o.worldPrev = v.prev; o.normal = normalize(v.normal);
    o.material = v.misc.x; o.localY = v.misc.y; o.instanceId = 0; o.vcolor = v.colour;
    o.uv = v.uv; o.texFlags = v.misc.z; // 1 textured, 2 + cutoff textured cut-out
    o.pos = mul(vp, float4(v.pos, 1));
    return o;
}
VSOut VSGround(uint vid : SV_VertexID) {
    VSOut o;
    float2 q = float2((vid << 1) & 2, vid & 2) * 2 - 1; // one big triangle covering the ground
    float3 w = float3(q.x * 300, 0, q.y * 300);
    o.world = w; o.worldPrev = w; o.normal = float3(0,1,0); o.material = -1; o.localY = 0; o.instanceId = 63; o.vcolor = 0; o.uv = 0; o.texFlags = 0;
    o.pos = mul(vp, float4(w, 1));
    return o;
}
// Depth-only shadow pass: the same geometry through the light's orthographic projection.
struct ShadowOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float texFlags : TEXCOORD1; };
ShadowOut VSMeshShadow(MeshIn v) { ShadowOut o; o.pos = mul(lightVp, float4(v.pos, 1)); o.uv = v.uv; o.texFlags = v.misc.z; return o; }
// The cut-out geometry of a character (eyelashes, brow and hair cards, the eye-occlusion and tearline
// overlays) is made of alpha planes. Writing them into the shadow map as solid quads is what painted
// the bluish blotches on the cheeks and under the eyes, so the shadow pass runs the same alpha test
// as the main pass instead of a null pixel shader.
void PSMeshShadow(ShadowOut i) {
    if (i.texFlags > 1.5) { float4 t = meshTex.Sample(meshSmp, i.uv); if (t.a < i.texFlags - 2.0) discard; }
}
float4 VSBoxShadow(uint vid : SV_VertexID, uint inst : SV_InstanceID) : SV_Position {
    return mul(lightVp, float4(boxPos[inst].xyz + kCorner[kIndex[vid]] * boxSize[inst].xyz, 1));
}
float4 VSGroundShadow(uint vid : SV_VertexID) : SV_Position {
    float2 q = float2((vid << 1) & 2, vid & 2) * 2 - 1;
    return mul(lightVp, float4(q.x * 300, 0, q.y * 300, 1));
}
// Soft shadows: a 16-tap Poisson disk, rotated per pixel, instead of the old fixed 5x5 box.
// The kernel RADIUS is a world-space penumbra half-width (--shadow-soft, a fraction of the shadow
// frustum's width) converted to texels on the C++ side and handed over in env.w (clamped 1..48), so
// "soft" means the same thing on a 25 cm head as on a 700 m landscape. The per-pixel rotation angle
// comes from interleaved-gradient noise: a fixed wide kernel bands, a rotated one dithers, and the
// dither is what DLSS/TAA (and the 8-sample --reference accumulation) resolve into a smooth gradient.
// Both biases are expressed in SHADOW TEXELS, i.e. in the scene's own world units (eyePos.w = one
// texel), and both scale with the kernel radius: a tap `radius` texels away looks at a surface up to
// radius * texel * tan(acos(ndl)) deeper, which no constant bias can cover. env.z converts world
// units along the light axis into normalized depth. The border colour 1 leaves everything outside
// the map lit.
static const float2 kPoisson16[16] = {
    float2(-0.94201624,-0.39906216), float2( 0.94558609,-0.76890725), float2(-0.09418410,-0.92938870), float2( 0.34495938, 0.29387760),
    float2(-0.91588581, 0.45771432), float2(-0.81544232,-0.87912464), float2(-0.38277543, 0.27676845), float2( 0.97484398, 0.75648379),
    float2( 0.44323325,-0.97511554), float2( 0.53742981,-0.47373420), float2(-0.26496911,-0.41893023), float2( 0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590), float2( 0.19984126, 0.78641367), float2( 0.14383161,-0.14100790) };
float ShadowFactor(float3 world, float3 n, float ndl, float2 px) {
    float texel = eyePos.w;
    float radius = max(env.w, 1.0);          // penumbra half-width in shadow texels
    float slope = 1.0 - saturate(ndl);
    world += n * texel * (1.0 + radius * (0.35 + 1.20 * slope));
    float4 lp = mul(lightVp, float4(world, 1));
    float3 ndc = lp.xyz / lp.w;
    float d = ndc.z - texel * (1.0 + radius * (0.50 + 1.50 * slope)) * env.z;
    if (d <= 0 || d >= 1) return 1.0;
    float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
    float ign = frac(52.9829189 * frac(0.06711056 * px.x + 0.00583715 * px.y)); // interleaved gradient noise
    float sa, ca; sincos(ign * 6.2831853, sa, ca);
    const float kDiskMax = 0.81; // the classic Poisson set reaches 1.234 units: normalise its max to `radius`
    float scale = radius * kDiskMax / 4096.0;
    float s = 0;
    [unroll] for (int k = 0; k < 16; ++k) {
        float2 p = kPoisson16[k];
        float2 o = float2(p.x * ca - p.y * sa, p.x * sa + p.y * ca);
        s += shadowMap.SampleCmpLevelZero(shadowSmp, uv + o * scale, d);
    }
    return s / 16.0;
}
float3 Palette(float k) {
    // Explicit selection: a dynamically indexed local array produced per-row garbage on some drivers.
    uint i = (uint)(k + 0.5) % 6;
    if (i == 0) return float3(0.85,0.25,0.2);
    if (i == 1) return float3(0.2,0.7,0.3);
    if (i == 2) return float3(0.25,0.4,0.9);
    if (i == 3) return float3(0.9,0.8,0.2);
    if (i == 4) return float3(0.8,0.3,0.8);
    return float3(0.9,0.9,0.9);
}
struct PSOut { float4 color : SV_Target0; float2 motion : SV_Target1; };
PSOut PS(VSOut i) {
    PSOut o;
    float3 albedo;
    if (i.material < -1.5) {
        // glTF mesh: the material's base colour, optionally with the 0.2 m stripes by local height.
        float stripe = i.material < -2.5 ? (fmod(abs(floor(i.localY * 5.0)), 2.0) < 1.0 ? 1.0 : 0.55) : 1.0;
        albedo = i.vcolor * stripe;
        if (i.texFlags > 0.5) {
            float4 t = meshTex.Sample(meshSmp, i.uv); // sRGB texture: the view converts to linear
            if (i.texFlags > 1.5 && t.a < i.texFlags - 2.0) discard;
            albedo *= t.rgb;
        }
    } else if (i.material < 0) {
        // Ground: 1 m checkerboard with colour bands every 4 m and thin bright grid lines.
        float2 g = floor(i.world.xz);
        float check = fmod(abs(g.x + g.y), 2.0) < 1.0 ? 1.0 : 0.35;
        float3 band = lerp(float3(0.55,0.5,0.45), float3(0.3,0.5,0.7), fmod(abs(floor(i.world.x / 4)), 2.0));
        float2 fr = frac(i.world.xz);
        float gridLine = (min(fr.x, 1 - fr.x) < 0.03 || min(fr.y, 1 - fr.y) < 0.03) ? 1.6 : 1.0;
        albedo = band * check * gridLine;
    } else {
        // Boxes: palette colour with a stripe pattern along the height for high-contrast edges.
        float stripe = fmod(abs(floor(i.localY * 5.0)), 2.0) < 1.0 ? 1.0 : 0.55;
        albedo = Palette(i.material) * stripe;
    }
    // Two-sided shading (the meshes are drawn with cull NONE) plus the shadow map; a fully lit
    // surface lands near 1.05, one in shadow near 0.25 (the old flat term was 0.35..1.15).
    float3 n = normalize(i.normal);
    if (dot(n, eyePos.xyz - i.world) < 0) n = -n;
    float3 L = normalize(lightDir.xyz);
    float ndl = saturate(dot(n, L));
    float3 ambient, sun;
    bool hdriLit = lightDir.w > 0.5;
    if (hdriLit) {
        // Everything below is linear radiance. Lambert: Lo = albedo/pi * E. Both terms carry E/pi,
        // so the shader multiplies the albedo by (E_sun/pi * NdL * shadow + E_ambient/pi):
        //   ambient: mip 8 (8x4 texels of the 2048x1024 map) is the average radiance around n, and
        //            E_ambient/pi == that average radiance for a cosine lobe - no extra factor.
        //   sun:     sunColor.rgb is already the sun irradiance divided by pi (see the C++ side).
        // No clamp: the HDRI is used at its raw radiance and the tone mapper handles the range.
        ambient = min(hdri.SampleLevel(hdriSmp, EquirectUV(n), 8.0).rgb, hdriParams.x) * sunColor.w; // the clamp removes the sun's hot spot from the diffuse term (it painted a false shadow edge on the cheek)
        sun = sunColor.rgb * sunColor.w;
    } else {
        ambient = lerp(float3(0.10,0.11,0.13), float3(0.20,0.26,0.36), n.y * 0.5 + 0.5) * 1.5; // hemisphere: ground bounce / sky
        sun = 0.85;
    }
    float3 radiance = albedo * (ndl * ShadowFactor(i.world, n, ndl, i.pos.xy) * sun + ambient);
    // The --hdri none path keeps its historical 0..~1.15 look; the HDRI path is tone mapped.
    o.color = float4(hdriLit ? PBRNeutralToneMapping(radiance) : radiance, 1);
    if (misc.z > 0.5 && misc.z < 1.5) o.color = float4(i.instanceId / 64.0, i.instanceId / 64.0, i.instanceId / 64.0, 1); // debug: instance id as grey
    if (misc.z > 1.5 && misc.z < 2.5) o.color = float4(i.normal * 0.5 + 0.5, 1); // debug: normal
    if (misc.z > 2.5) o.color = float4(i.material / 8.0, i.material / 8.0, i.material / 8.0, 1); // debug: material index
    // Motion vectors: where this surface point was on screen last frame minus where it is now
    // (current -> previous), in render pixels of the unjittered projections.
    float4 cur = mul(vpNoJitter, float4(i.world, 1));
    float4 prev = mul(vpPrev, float4(i.worldPrev, 1));
    float2 curPix = (cur.xy / cur.w * float2(0.5, -0.5) + 0.5) * renderSize.xy;
    float2 prevPix = (prev.xy / prev.w * float2(0.5, -0.5) + 0.5) * renderSize.xy;
    o.motion = (prevPix - curPix) * renderSize.z * renderSize.w;
    return o;
}
struct BlitOut { float4 color : SV_Target0; };
struct BlitIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
BlitIn VSBlit(uint id : SV_VertexID) { BlitIn o; float2 p = float2((id << 1) & 2, id & 2); o.uv = p; o.pos = float4(p * 2 - 1, 0.5, 1); o.pos.y = -o.pos.y; return o; }
// Sky: a fullscreen triangle drawn before the scene with depth off, so the cleared far depth stays.
// Colour = the HDRI along the view ray, motion vectors = a point far along that ray reprojected
// through vpPrev (the same convention as the mesh PS).
PSOut PSSky(BlitIn i) {
    PSOut o;
    float2 ndc = float2(i.uv.x * 2 - 1, 1 - i.uv.y * 2);
    float4 h = mul(invVp, float4(ndc, 1, 1)); // reverse-Z: z = 1 is the near plane
    float3 dir = normalize(h.xyz / h.w - eyePos.xyz);
    o.color = float4(PBRNeutralToneMapping(hdri.SampleLevel(hdriSmp, EquirectUV(dir), 0).rgb * sunColor.w), 1);
    float3 world = eyePos.xyz + dir * 1e4;
    float4 cur = mul(vpNoJitter, float4(world, 1));
    float4 prev = mul(vpPrev, float4(world, 1));
    float2 curPix = (cur.xy / cur.w * float2(0.5, -0.5) + 0.5) * renderSize.xy;
    float2 prevPix = (prev.xy / prev.w * float2(0.5, -0.5) + 0.5) * renderSize.xy;
    o.motion = (prevPix - curPix) * renderSize.z * renderSize.w;
    return o;
}
Texture2D src : register(t0); SamplerState smp : register(s0);
BlitOut PSBlit(BlitIn i) { BlitOut o; o.color = src.Sample(smp, i.uv); return o; }
BlitOut PSAccum(BlitIn i) { BlitOut o; o.color = src.Sample(smp, i.uv) * misc.x; return o; }
)";

static ComPtr<ID3DBlob> Compile(const char *source, const char *entry, const char *target)
{
    ComPtr<ID3DBlob> code, err;
    if (FAILED(D3DCompile(source, strlen(source), "bench", nullptr, nullptr, entry, target, 0, 0, &code, &err))) {
        std::printf("[fail] shader %s: %s\n", entry, err ? (const char *) err->GetBufferPointer() : "?");
        return nullptr;
    }
    return code;
}

struct FrameConstants {
    Mat4 vp, vpNoJitter, vpPrev, lightVp, invVp;
    float renderSize[4];
    float misc[4];
    float eyePos[4];
    float lightDir[4];
    float sunColor[4];
    float hdriParams[4];
    float env[4];
};
struct InstanceConstants {
    float pos[64][4];
    float prev[64][4];
    float size[64][4];
};

float g_yawSpeed = 45.0f; // --yaw-speed, degrees per second

// The camera path: third person around a figure at the origin. Phases of 120 frames.
struct CameraState { Vec3 eye, target; };
static CameraState CameraAt(int frame, const char *mode)
{
    const int phaseLength = 120;
    int phase;
    if (strcmp(mode, "static") == 0) phase = 0;
    else if (strcmp(mode, "yaw") == 0) phase = 1;
    else if (strcmp(mode, "forward") == 0) phase = 2;
    else if (strcmp(mode, "strafe") == 0) phase = 3;
    else if (strcmp(mode, "combo") == 0) phase = 4;
    else phase = (frame / phaseLength) % 5;
    // Integrate the scripted motion up to this frame so phases chain continuously.
    float yaw = 0.0f, forward = 0.0f, strafe = 0.0f;
    const float yawRate = g_yawSpeed / 60.0f * 3.14159265f / 180.0f; // deg/s -> rad/frame
    const float moveRate = 2.0f / 60.0f;                        // 2 m/s
    auto step = [&](int p) {
        if (p == 1) yaw += yawRate;
        else if (p == 2) forward += moveRate;
        else if (p == 3) strafe += moveRate;
        else if (p == 4) { yaw += yawRate * 0.6f; forward += moveRate * 0.7f; }
    };
    if (strcmp(mode, "script") == 0) {
        for (int f = 0; f < frame; ++f) step((f / phaseLength) % 5);
    } else {
        for (int f = 0; f < frame; ++f) step(phase);
    }
    // Yaw rotates the camera around the figure; forward/strafe move camera and figure together
    // (the figure walks), so the boxes flow past.
    const Vec3 dir{std::sin(yaw), 0.0f, std::cos(yaw)};
    const Vec3 right{std::cos(yaw), 0.0f, -std::sin(yaw)};
    const Vec3 base = dir * forward + right * strafe;
    CameraState c;
    c.target = base + Vec3{0.0f, 1.0f, 0.0f};
    c.eye = c.target - dir * 5.0f + Vec3{0.0f, 0.9f, 0.0f};
    return c;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: the last line survives a crash
    int frames = 600, switchEvery = 0;
    int temporalMode = -1, temporalEvery = 2;
    std::vector<int> dumpFrames;
    float mvScale = 1.0f;
    float mvDir = 1.0f;
    int measureEvery = 0;
    UINT presentW = kOutW, presentH = kOutH;
    const char *scene = "3d";
    const char *cameraMode = "script";
    bool movingBoxes = false;
    bool frametime = false; // --frametime: statistics of the intervals between Present calls
    double fpsCap = 0.0;    // --fps-cap N: CPU-paced frame budget (emulates a game with idle GPU time)
    const char *gltfPath = nullptr; // --gltf <file.glb>: replaces the procedural scene (camera from the file unless --camera is given)
    bool cameraGiven = false;
    float faceYaw = 180.0f, faceRadius = 0.0f, faceSweep = 25.0f; // --camera face (180: in front of a character that faces glTF -Z); radius 0 = auto (see kFaceFill)
    float faceAngle = -15.0f;       // --face-angle: the camera's horizontal offset from the face axis, so the head reads slightly turned
                                    // (negative: the head reads turned right-to-left, the mirror of the old +15)
    const char *faceNode = "CharacterAnchor";
    bool reference = false;
    const char *hdriPath = nullptr; // --hdri <file.hdr|none>; empty = look for the default beside the assets
    // --hdri-yaw: environment rotation about the vertical axis; "auto" (the default) aims the sun at
    // kSunOffCameraDeg to the side of the face camera and resolves to 0 for every other camera.
    float hdriYaw = 0.0f; bool hdriYawAuto = true;
    float sunScale = -1.0f;         // --sun-scale: overrides kSunOverSky (negative = use the constant)
    float exposure = 1.45f;         // --exposure: plain multiplier on the scene radiance (sky, ambient and sun alike) before the tone mapper
                                    // (1.45 with the soft kSunOverSky key: lit skin lands near sRGB luma 209, shadowed near 143, the sky unclipped at 203)
    int hdriMirrorArg = -1;          // --hdri-mirror 0|1 (-1 = preset decides)
    float animSpeed = 1.0f;         // --anim-speed: glTF animation playback factor (2.5 = the cutegirl clip plays 2.5x faster)
    float shadowSoft = 0.006f;      // --shadow-soft: penumbra half-width as a fraction of the shadow frustum's width
    // ---- Blender parity overrides (see --camera blender). Unset = NaN-free sentinels below. ----
    bool sunDirGiven = false, sunStrengthGiven = false, camPosGiven = false, camTargetGiven = false, fovGiven = false, sceneScaleGiven = false, sceneYawGiven = false, camShiftGiven = false;
    Vec3 sunDirTravel{0, -1, 0};    // --sun-dir: the direction the light TRAVELS (Blender's convention); lightDir = -this
    float sunStrength = 0.0f;       // --sun-strength: sun irradiance in the HDRI's radiance units (Blender's W/m^2 with HDRI strength 1)
    Vec3 camPosM{0, 0, 0}, camTargetM{0, 0, 0}; // --cam-pos / --cam-target, in the units the user typed (metres); x sceneScale -> scene units
    float camFov = 0.0f;            // --fov: vertical FOV in degrees
    Vec3 camShift{0, 0, 0};         // --cam-shift: Blender's camera shift_x / shift_y (z unused)
    float sceneScale = 1.0f;        // --scene-scale: multiplies the camera override positions (cutegirl is authored in cm: 100)
    float sceneYaw = 0.0f;          // --scene-yaw: the loaded asset is turned this many degrees about +Y relative to the
                                    // scene the camera/sun/HDRI numbers were authored in; camera, sun and sky are turned with it
    auto parseVec3 = [](const char *s, Vec3 &out) { float a = 0, b = 0, c = 0; if (sscanf_s(s, "%f,%f,%f", &a, &b, &c) == 3) { out = {a, b, c}; return true; } return false; };
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--switch") == 0 && i + 1 < argc) switchEvery = atoi(argv[++i]);
        else if (strcmp(argv[i], "--temporal") == 0 && i + 1 < argc) { unsigned m = 0, n = 2; if (sscanf_s(argv[++i], "%u,%u", &m, &n) >= 1) { temporalMode = (int) m; temporalEvery = (int) n; } }
        else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) { for (char *tok = strtok(argv[++i], ","); tok; tok = strtok(nullptr, ",")) dumpFrames.push_back(atoi(tok)); }
        else if (strcmp(argv[i], "--mv-scale") == 0 && i + 1 < argc) mvScale = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--mv-dir") == 0 && i + 1 < argc) mvDir = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--measure") == 0 && i + 1 < argc) measureEvery = atoi(argv[++i]);
        else if (strcmp(argv[i], "--present") == 0 && i + 1 < argc) { unsigned w = 0, h = 0; if (sscanf_s(argv[++i], "%ux%u", &w, &h) == 2 && w && h) { presentW = w; presentH = h; } }
        else if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc) scene = argv[++i];
        else if (strcmp(argv[i], "--camera") == 0 && i + 1 < argc) { cameraMode = argv[++i]; cameraGiven = true; }
        else if (strcmp(argv[i], "--face-yaw") == 0 && i + 1 < argc) faceYaw = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--face-radius") == 0 && i + 1 < argc) faceRadius = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--face-sweep") == 0 && i + 1 < argc) faceSweep = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--face-angle") == 0 && i + 1 < argc) faceAngle = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--face-node") == 0 && i + 1 < argc) faceNode = argv[++i];
        else if (strcmp(argv[i], "--gltf") == 0 && i + 1 < argc) gltfPath = argv[++i];
        else if (strcmp(argv[i], "--frametime") == 0) frametime = true;
        else if (strcmp(argv[i], "--fps-cap") == 0 && i + 1 < argc) fpsCap = atof(argv[++i]);
        else if (strcmp(argv[i], "--yaw-speed") == 0 && i + 1 < argc) g_yawSpeed = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--moving-boxes") == 0) movingBoxes = true;
        else if (strcmp(argv[i], "--reference") == 0) reference = true;
        else if (strcmp(argv[i], "--hdri") == 0 && i + 1 < argc) hdriPath = argv[++i];
        else if (strcmp(argv[i], "--hdri-yaw") == 0 && i + 1 < argc) { const char *v = argv[++i]; if (strcmp(v, "auto") == 0) hdriYawAuto = true; else { hdriYawAuto = false; hdriYaw = (float) atof(v); } }
        else if (strcmp(argv[i], "--sun-scale") == 0 && i + 1 < argc) sunScale = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--exposure") == 0 && i + 1 < argc) exposure = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--shadow-soft") == 0 && i + 1 < argc) shadowSoft = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--anim-speed") == 0 && i + 1 < argc) animSpeed = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--hdri-mirror") == 0 && i + 1 < argc) { hdriMirrorArg = atoi(argv[++i]); }
        else if (strcmp(argv[i], "--sun-dir") == 0 && i + 1 < argc) sunDirGiven = parseVec3(argv[++i], sunDirTravel);
        else if (strcmp(argv[i], "--sun-strength") == 0 && i + 1 < argc) { sunStrength = (float) atof(argv[++i]); sunStrengthGiven = true; }
        else if (strcmp(argv[i], "--cam-pos") == 0 && i + 1 < argc) camPosGiven = parseVec3(argv[++i], camPosM);
        else if (strcmp(argv[i], "--cam-target") == 0 && i + 1 < argc) camTargetGiven = parseVec3(argv[++i], camTargetM);
        else if (strcmp(argv[i], "--fov") == 0 && i + 1 < argc) { camFov = (float) atof(argv[++i]); fovGiven = true; }
        else if (strcmp(argv[i], "--cam-shift") == 0 && i + 1 < argc) { float a = 0, b = 0; if (sscanf_s(argv[++i], "%f,%f", &a, &b) == 2) { camShift = {a, b, 0}; camShiftGiven = true; } }
        else if (strcmp(argv[i], "--scene-scale") == 0 && i + 1 < argc) { sceneScale = (float) atof(argv[++i]); sceneScaleGiven = true; }
        else if (strcmp(argv[i], "--scene-yaw") == 0 && i + 1 < argc) { sceneYaw = (float) atof(argv[++i]); sceneYawGiven = true; }
        else frames = atoi(argv[i]);
    }
    // ---- --camera blender: the user's Blender setup for the cutegirl head, baked in as defaults ----
    // Nothing is read from disk; every constant below is still overridable by its own flag. The
    // numbers come from the .blend the look was tuned in (glTF Y-up, metres, "Standard" view
    // transform, HDRI strength 1, 0 EV, 57 mm lens on a 3840x2160 frame).
    const bool blenderCam = strcmp(cameraMode, "blender") == 0;
    if (blenderCam) {
        if (!sunDirGiven) { sunDirTravel = {0.522451f, -0.654712f, 0.546258f}; sunDirGiven = true; }
        if (!sunStrengthGiven) { sunStrength = 3.01f; sunStrengthGiven = true; }
        if (!camPosGiven) { camPosM = {-0.19384f, 1.487888f, -0.669449f}; camPosGiven = true; }
        if (!camTargetGiven) { camTargetM = {-0.004746f, 1.552911f, -0.010353f}; camTargetGiven = true; }
        if (!fovGiven) { camFov = 23.7773f; fovGiven = true; }
        if (!camShiftGiven) { camShift = {-0.08f, -0.09f, 0.0f}; camShiftGiven = true; } // the .blend's camera lens shift_x / shift_y, Blender units (fraction of the frame WIDTH)

        if (!sceneScaleGiven) { sceneScale = 100.0f; sceneScaleGiven = true; } // the cutegirl .glb is authored in centimetres
        // The bench's world is the glTF world with Z negated (the head faces +Z here and -Z in Blender's
        // Y-up export), so everything authored in the .blend is MIRRORED in Z, not turned: a 180 deg
        // yaw gave the mirrored head turn. Verified against the user's Blender render (2026-09-08).
        if (!sceneYawGiven) { sceneYaw = 0.0f; sceneYawGiven = true; }
        camPosM.z = -camPosM.z; camTargetM.z = -camTargetM.z; sunDirTravel.z = -sunDirTravel.z; // the lamp mirrors with the camera (the user confirmed: key from the viewer's right)
        if (hdriMirrorArg < 0) hdriMirrorArg = 1; // the environment is mirrored with the world
        // Blender's equirect world is ours turned a quarter turn: ourYaw = -90 deg - blenderYaw
        // (measured to 0.4 deg, see the note at --hdri-yaw). blenderYaw is 0 in this .blend.
        if (hdriYawAuto) { hdriYaw = -90.0f; hdriYawAuto = false; } // with the Z mirror: matched to the Blender render's background (hills far left, ridge right)
        std::printf("[info] camera blender: pos (%.5f, %.5f, %.5f) target (%.5f, %.5f, %.5f) fovY %.4f deg, scene scale %.0f\n",
                    camPosM.x, camPosM.y, camPosM.z, camTargetM.x, camTargetM.y, camTargetM.z, camFov, sceneScale);
    }
    // --scene-yaw: one rotation about +Y applied to everything that was authored in the other scene -
    // the camera override, the sun direction and the environment (the sky's yaw simply adds).
    if (sceneYaw != 0.0f) {
        const float a = sceneYaw * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
        auto rotY = [&](Vec3 v) { return Vec3{v.x * c + v.z * s, v.y, -v.x * s + v.z * c}; };
        if (camPosGiven) camPosM = rotY(camPosM);
        if (camTargetGiven) camTargetM = rotY(camTargetM);
        if (sunDirGiven) sunDirTravel = rotY(sunDirTravel);
        if (!hdriYawAuto) hdriYaw += sceneYaw; // "auto" picks it up through faceYaw / the else branch below
        faceYaw += sceneYaw; // the face camera orbits the asset, so it follows the same turn
        std::printf("[info] scene yaw %.1f deg: camera (%.5f, %.5f, %.5f) -> (%.5f, %.5f, %.5f), sun travel (%.4f, %.4f, %.4f), hdri yaw %.1f\n",
                    sceneYaw, camPosM.x, camPosM.y, camPosM.z, camTargetM.x, camTargetM.y, camTargetM.z,
                    sunDirTravel.x, sunDirTravel.y, sunDirTravel.z, hdriYaw);
    }
    g_hdriMirror = hdriMirrorArg > 0;
    if (mvScale == 0.0f) mvScale = 1.0f;
    const bool flat = strcmp(scene, "flat") == 0;
    if (reference && flat) { std::printf("[fail] --reference needs the 3D scene\n"); return 1; }
    pwgltf::Scene gscene;
    if (gltfPath) {
        if (flat) { std::printf("[fail] --gltf needs the 3D scene\n"); return 1; }
        if (!pwgltf::Load(gltfPath, gscene)) { std::printf("[fail] glTF: %s (%s)\n", gscene.error.c_str(), gltfPath); return 1; }
        size_t tris = 0; for (const auto &m : gscene.meshes) for (const auto &p : m.primitives) tris += p.indices.size() / 3;
        std::printf("[info] glTF %s: %zu nodes, %zu meshes, %zu triangles, %zu animation channels (%.2f s), camera %s\n", gltfPath, gscene.nodes.size(),
                    gscene.meshes.size(), tris, gscene.channels.size(), gscene.animationEnd, gscene.cameraNode >= 0 ? gscene.nodes[gscene.cameraNode].name.c_str() : "none");
        if (!cameraGiven && gscene.cameraNode >= 0) cameraMode = "file";
        if (!cameraGiven && gscene.cameraNode < 0) std::printf("[info] glTF has no camera: the scripted camera path is used\n");
    }
    std::printf("[info] scene %s, camera %s, exposure %.2f, shadow-soft %.4f, DLSS MV_Scale = %.3f, mv-dir %+.0f%s%s\n", flat ? "flat" : "3d", cameraMode,
                exposure, shadowSoft, mvScale, mvDir,
                movingBoxes ? ", moving boxes" : "", reference ? ", REFERENCE (no DLSS/NR, 8 jitter samples at 4K)" : "");
    wchar_t exeDir[MAX_PATH]{}; GetModuleFileNameW(nullptr, exeDir, MAX_PATH); if (wchar_t *s = wcsrchr(exeDir, L'\\')) *s = 0;
    SetCurrentDirectoryW(exeDir);

    if (GetEnvironmentVariableA("PW_BENCH_DEBUG_LAYER", nullptr, 0) != 0) {
        ID3D12Debug *debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) && debug) { debug->EnableDebugLayer(); debug->Release(); std::printf("[info] D3D12 debug layer enabled\n"); }
    }
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"pw_bench";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(L"pw_bench", L"PeripheralWarp bench", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 1600, 900, nullptr, nullptr, wc.hInstance, nullptr);

    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; ComPtr<IDXGISwapChain> sc;
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Width = presentW; sd.BufferDesc.Height = presentH; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx))) {
        std::printf("[fail] D3D11 device\n"); return 1;
    }
    std::printf("[info] D3D11 device created\n");

    // Render-resolution targets (or the reference's 4K targets), the DLSS output, the swap chain.
    const UINT sceneW = reference ? kOutW : kRenderW, sceneH = reference ? kOutH : kRenderH;
    auto tex = [&](UINT w, UINT h, DXGI_FORMAT f, UINT bind) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
        ComPtr<ID3D11Texture2D> t; dev->CreateTexture2D(&d, nullptr, &t); return t; };
    auto color = tex(sceneW, sceneH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto motion = tex(sceneW, sceneH, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto depth = tex(sceneW, sceneH, DXGI_FORMAT_R24G8_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    auto output = tex(kOutW, kOutH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    if (!color || !motion || !depth || !output) { std::printf("[fail] textures\n"); return 1; }
    ComPtr<ID3D11RenderTargetView> colorRtv, motionRtv, outputRtv; ComPtr<ID3D11DepthStencilView> dsv; ComPtr<ID3D11ShaderResourceView> outputSrv, colorSrv;
    dev->CreateRenderTargetView(color.Get(), nullptr, &colorRtv);
    dev->CreateRenderTargetView(motion.Get(), nullptr, &motionRtv);
    dev->CreateRenderTargetView(output.Get(), nullptr, &outputRtv);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{}; dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dev->CreateDepthStencilView(depth.Get(), &dd, &dsv);
    dev->CreateShaderResourceView(output.Get(), nullptr, &outputSrv);
    dev->CreateShaderResourceView(color.Get(), nullptr, &colorSrv);
    ComPtr<ID3D11Texture2D> back; sc->GetBuffer(0, IID_PPV_ARGS(&back));
    ComPtr<ID3D11RenderTargetView> backRtv; dev->CreateRenderTargetView(back.Get(), nullptr, &backRtv);
    ComPtr<ID3D11Texture2D> staging;
    if (measureEvery > 0 || !dumpFrames.empty()) {
        D3D11_TEXTURE2D_DESC sdesc{}; sdesc.Width = kOutW; sdesc.Height = kOutH; sdesc.MipLevels = 1; sdesc.ArraySize = 1;
        sdesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; sdesc.SampleDesc.Count = 1; sdesc.Usage = D3D11_USAGE_STAGING; sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dev->CreateTexture2D(&sdesc, nullptr, &staging);
        if (!staging) { std::printf("[fail] staging texture\n"); return 1; }
    }
    auto halfToFloat = [](unsigned short h) {
        unsigned int sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
        float v;
        if (exp == 0) v = std::ldexp((float) mant, -24);
        else if (exp == 31) v = mant ? NAN : INFINITY;
        else v = std::ldexp((float) (mant | 0x400u), (int) exp - 25);
        return sign ? -v : v;
    };
    double sumAll = 0, sumCenter = 0, sumPeriphery = 0; int measurements = 0;
    auto measure = [&](int frameIndex) {
        ctx->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) { std::printf("[warn] measure map failed\n"); return; }
        double all = 0, center = 0, periphery = 0; long long nAll = 0, nCenter = 0, nPer = 0;
        const UINT cx0 = kOutW / 5, cx1 = kOutW - kOutW / 5, cy0 = kOutH / 5, cy1 = kOutH - kOutH / 5;
        for (UINT y = 0; y < kOutH; y += 4) {
            const unsigned short *row = (const unsigned short *) ((const char *) map.pData + (size_t) y * map.RowPitch);
            for (UINT x = 0; x < kOutW; x += 4) {
                const float r = halfToFloat(row[x * 4 + 0]), g = halfToFloat(row[x * 4 + 1]), b = halfToFloat(row[x * 4 + 2]);
                const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                all += luma; ++nAll;
                if (x >= cx0 && x < cx1 && y >= cy0 && y < cy1) { center += luma; ++nCenter; } else { periphery += luma; ++nPer; }
            }
        }
        ctx->Unmap(staging.Get(), 0);
        all /= nAll; center /= nCenter; periphery /= nPer;
        sumAll += all; sumCenter += center; sumPeriphery += periphery; ++measurements;
        std::printf("[measure] frame %d: mean luma all=%.5f center=%.5f periphery=%.5f\n", frameIndex, all, center, periphery);
    };
    // --dump N: the output of frame N, full size, as dump_N.bmp beside the exe. The scene shaders
    // already tone map to linear 0..1, so the only step left here is the linear -> sRGB encode.
    auto dump = [&](int frameIndex) {
        ctx->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) { std::printf("[warn] dump map failed\n"); return; }
        const UINT w = kOutW, h = kOutH;
        const UINT rowBytes = ((w * 3 + 3) / 4) * 4;
        std::vector<unsigned char> pixels((size_t) rowBytes * h);
        for (UINT y = 0; y < h; ++y) {
            const unsigned short *row = (const unsigned short *) ((const char *) map.pData + (size_t) y * map.RowPitch);
            unsigned char *out = pixels.data() + (size_t) (h - 1 - y) * rowBytes;
            for (UINT x = 0; x < w; ++x) {
                for (int ch = 0; ch < 3; ++ch) {
                    float v = halfToFloat(row[x * 4 + ch]);
                    v = v < 0 ? 0 : (v > 1 ? 1 : v);
                    v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; // linear -> sRGB
                    out[x * 3 + (2 - ch)] = (unsigned char) (std::min(1.0f, v) * 255.0f + 0.5f);
                }
            }
        }
        ctx->Unmap(staging.Get(), 0);
        char name[64]; std::snprintf(name, sizeof(name), "dump_%d.bmp", frameIndex);
        FILE *fp = nullptr; fopen_s(&fp, name, "wb");
        if (!fp) { std::printf("[warn] dump open failed\n"); return; }
        const unsigned int fileSize = 54 + (unsigned int) pixels.size();
        unsigned char header[54] = {'B', 'M'};
        memcpy(header + 2, &fileSize, 4); const unsigned int off = 54; memcpy(header + 10, &off, 4);
        const unsigned int dib = 40; memcpy(header + 14, &dib, 4); memcpy(header + 18, &w, 4); memcpy(header + 22, &h, 4);
        const unsigned short planes = 1, bpp = 24; memcpy(header + 26, &planes, 2); memcpy(header + 28, &bpp, 2);
        const unsigned int imageSize = (unsigned int) pixels.size(); memcpy(header + 34, &imageSize, 4);
        fwrite(header, 1, 54, fp); fwrite(pixels.data(), 1, pixels.size(), fp); fclose(fp);
        std::printf("[info] frame %d dumped to %s (%ux%u)\n", frameIndex, name, w, h);
    };

    // ---- shaders and state ----
    const char *source = flat ? kShaderFlat : kShader3D;
    ComPtr<ID3D11VertexShader> vsFlat, vsBox, vsGround, vsBlit, vsMesh, vsBoxShadow, vsGroundShadow, vsMeshShadow;
    ComPtr<ID3D11InputLayout> meshLayout;
    ComPtr<ID3D11PixelShader> psScene, psBlit, psAccum, psMeshShadow;
    {
        ComPtr<ID3DBlob> b;
        if (flat) {
            if (!(b = Compile(source, "VS", "vs_5_0"))) return 1; dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vsFlat);
            vsBlit = vsFlat;
        } else {
            if (!(b = Compile(source, "VSBox", "vs_5_0"))) return 1; dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vsBox);
            if (!(b = Compile(source, "VSGround", "vs_5_0"))) return 1; dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vsGround);
            if (!(b = Compile(source, "VSBlit", "vs_5_0"))) return 1; dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vsBlit);
            if (!(b = Compile(source, "VSBoxShadow", "vs_5_0"))) return 1; dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vsBoxShadow);
            if (!(b = Compile(source, "VSGroundShadow", "vs_5_0"))) return 1; dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vsGroundShadow);
            if (!(b = Compile(source, "VSMeshShadow", "vs_5_0"))) return 1; dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vsMeshShadow);
            if (!(b = Compile(source, "VSMesh", "vs_5_0"))) return 1; dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vsMesh);
            const D3D11_INPUT_ELEMENT_DESC layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"PREVPOS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 60, D3D11_INPUT_PER_VERTEX_DATA, 0}};
            if (FAILED(dev->CreateInputLayout(layout, 6, b->GetBufferPointer(), b->GetBufferSize(), &meshLayout))) { std::printf("[fail] mesh input layout\n"); return 1; }
        }
        if (!(b = Compile(source, "PS", "ps_5_0"))) return 1; dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psScene);
        if (!(b = Compile(source, "PSBlit", "ps_5_0"))) return 1; dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psBlit);
        if (!flat) {
            if (!(b = Compile(source, "PSAccum", "ps_5_0"))) return 1; dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psAccum);
            if (!(b = Compile(source, "PSMeshShadow", "ps_5_0"))) return 1; dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psMeshShadow);
        }
    }
    D3D11_BUFFER_DESC bd{}; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = flat ? 32 : sizeof(FrameConstants);
    ComPtr<ID3D11Buffer> cb; dev->CreateBuffer(&bd, nullptr, &cb);
    bd.ByteWidth = sizeof(InstanceConstants);
    ComPtr<ID3D11Buffer> cbInst; dev->CreateBuffer(&bd, nullptr, &cbInst);
    D3D11_DEPTH_STENCIL_DESC dsd{}; dsd.DepthEnable = GetEnvironmentVariableA("PW_BENCH_NO_DEPTH", nullptr, 0) ? FALSE : TRUE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; // PW_BENCH_NO_DEPTH: debug
    dsd.DepthFunc = flat ? D3D11_COMPARISON_ALWAYS : D3D11_COMPARISON_GREATER_EQUAL; // reverse-Z
    ComPtr<ID3D11DepthStencilState> dss; dev->CreateDepthStencilState(&dsd, &dss);
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rs; dev->CreateRasterizerState(&rd, &rs);
    D3D11_BLEND_DESC bld{}; bld.RenderTarget[0].BlendEnable = TRUE; bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bld.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    bld.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> additive; dev->CreateBlendState(&bld, &additive);
    D3D11_SAMPLER_DESC smd{}; smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> smp; dev->CreateSamplerState(&smd, &smp);

    // ---- directional shadow map (4096^2, normal 0..1 depth, comparison sampling) ----
    const UINT kShadowSize = 4096;
    ComPtr<ID3D11Texture2D> shadowTex; ComPtr<ID3D11DepthStencilView> shadowDsv; ComPtr<ID3D11ShaderResourceView> shadowSrv;
    ComPtr<ID3D11SamplerState> shadowSmp; ComPtr<ID3D11DepthStencilState> dssShadow;
    if (!flat) {
        D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = kShadowSize; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32_TYPELESS;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &shadowTex))) { std::printf("[fail] shadow map\n"); return 1; }
        D3D11_DEPTH_STENCIL_VIEW_DESC sdv{}; sdv.Format = DXGI_FORMAT_D32_FLOAT; sdv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        dev->CreateDepthStencilView(shadowTex.Get(), &sdv, &shadowDsv);
        D3D11_SHADER_RESOURCE_VIEW_DESC ssv{}; ssv.Format = DXGI_FORMAT_R32_FLOAT; ssv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; ssv.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(shadowTex.Get(), &ssv, &shadowSrv);
        D3D11_SAMPLER_DESC ssd{}; ssd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        ssd.AddressU = ssd.AddressV = ssd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER; ssd.BorderColor[0] = ssd.BorderColor[1] = ssd.BorderColor[2] = ssd.BorderColor[3] = 1.0f;
        ssd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL; ssd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&ssd, &shadowSmp);
        D3D11_DEPTH_STENCIL_DESC sd2{}; sd2.DepthEnable = TRUE; sd2.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; sd2.DepthFunc = D3D11_COMPARISON_LESS;
        dev->CreateDepthStencilState(&sd2, &dssShadow);
        if (!shadowDsv || !shadowSrv || !shadowSmp || !dssShadow) { std::printf("[fail] shadow map views\n"); return 1; }
    }

    // ---- environment lighting from an equirect HDRI (--hdri): sky, ambient, sun ----
    ComPtr<ID3D11ShaderResourceView> hdriSrv; ComPtr<ID3D11SamplerState> hdriSampler;
    ComPtr<ID3D11PixelShader> psSky;
    bool hdriActive = false;
    Vec3 sunDir = Normalize(Vec3{0.4f, 0.8f, -0.35f}); // fallback: the old hardcoded direction
    float sunCol[3] = {0.85f, 0.85f, 0.85f};
    const float hdriScale = 1.0f; // the HDRI is used at its raw radiance; --exposure is the only scale
    if (!flat) {
        std::string chosen;
        if (hdriPath && strcmp(hdriPath, "none") == 0) chosen.clear();
        else if (hdriPath) chosen = hdriPath;
        else {
            static const char *const candidates[] = {"..\\assets\\external\\golden_gate_hills_2k.hdr",
                                                     "assets\\external\\golden_gate_hills_2k.hdr",
                                                     "..\\..\\assets\\external\\golden_gate_hills_2k.hdr"};
            for (const char *c : candidates) { FILE *f = nullptr; fopen_s(&f, c, "rb"); if (f) { fclose(f); chosen = c; break; } }
        }
        HdrImage hdr;
        if (!chosen.empty() && !LoadHdr(chosen.c_str(), hdr)) std::printf("[warn] hdri %s could not be read; the flat sky/ambient is used\n", chosen.c_str());
        else if (!chosen.empty()) {
            // The map is used at its raw radiance (hdriScale stays 1): the mean luminance below is
            // only printed for information and used as the fallback reference for the sun strength.
            double sumL = 0, sumW = 0;
            std::vector<float> luma((size_t) hdr.w * hdr.h);
            for (int y = 0; y < hdr.h; ++y) {
                const double sw = std::sin((y + 0.5) / hdr.h * 3.14159265);
                for (int x = 0; x < hdr.w; ++x) {
                    const float *p = &hdr.rgb[((size_t) y * hdr.w + x) * 3];
                    const float l = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
                    luma[(size_t) y * hdr.w + x] = l;
                    sumL += l * sw; sumW += sw;
                }
            }
            const double meanL = sumW > 0 ? sumL / sumW : 1.0; // solid-angle weighted mean radiance (luma)
            g_hdriMeanL = (float) meanL;
            // Sun: centroid of the brightest 0.01% of the pixels, weighted by luminance.
            std::vector<float> sorted = luma;
            const size_t topN = std::max<size_t>(16, luma.size() / 10000);
            std::nth_element(sorted.begin(), sorted.end() - topN, sorted.end());
            const float threshold = *(sorted.end() - topN);
            // Their radiance integrated over their solid angle is the sun irradiance E_sun; the
            // shader wants E_sun / pi (see the Lambert convention in the PS), so divide it there.
            Vec3 acc{0, 0, 0}; double irrR = 0, irrG = 0, irrB = 0; size_t hits = 0;
            const double dPhi = 6.2831853 / hdr.w, dTheta = 3.14159265 / hdr.h;
            for (int y = 0; y < hdr.h; ++y) {
                const double dOmega = std::sin((y + 0.5) / hdr.h * 3.14159265) * dPhi * dTheta;
                for (int x = 0; x < hdr.w; ++x) {
                    const float l = luma[(size_t) y * hdr.w + x];
                    if (l < threshold) continue;
                    const Vec3 d = HdrUvToDir(g_hdriMirror ? 1.0f - (x + 0.5f) / hdr.w : (x + 0.5f) / hdr.w, (y + 0.5f) / hdr.h);
                    acc = acc + d * l;
                    const float *p = &hdr.rgb[((size_t) y * hdr.w + x) * 3];
                    irrR += p[0] * dOmega; irrG += p[1] * dOmega; irrB += p[2] * dOmega; ++hits;
                }
            }
            if (hits > 0 && Dot(acc, acc) > 0) {
                sunDir = Normalize(acc); // still in HDRI space; rotated by the yaw below
                // E_sun / pi, i.e. the same units as the ambient average radiance the shader reads.
                float r = (float) (irrR / 3.14159265), g = (float) (irrG / 3.14159265), b = (float) (irrB / 3.14159265);
                const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                // The measured disk is unusable as an absolute: a 2k .hdr clips it (under-reads) while a
                // wide bright sky over-reads it. Keep the measured chroma and normalise the sun to
                // kSunOverSky x the sky's mean radiance, which is the sun/sky ratio knob: lit surfaces
                // land ~(1 + kSunOverSky)x above shadowed ones. 1.2 is a soft, nearly shadowless key
                // (the old 3.0 gave a hard sunlit look with black shadow sides). --sun-scale overrides it.
                const float kSunOverSky = 1.2f;
                const float ratio = sunScale > 0.0f ? sunScale : kSunOverSky;
                const float target = (float) (ratio * meanL);
                const float k = l > 1e-6f ? target / l : 1.0f;
                r *= k; g *= k; b *= k;
                std::printf("[info] hdri sun: measured E/pi luma %.3f -> %.3f (%.2fx sky mean %.3f)\n", l, target, ratio, meanL);
                sunCol[0] = r; sunCol[1] = g; sunCol[2] = b;
            }
            // --hdri-yaw: pick the rotation, then turn the sun with the sky (the only value that comes
            // out of the map in HDRI space). Auto aims the sun kSunOffCameraDeg to the side of the face
            // camera's axis: a front-side key light instead of whatever the map happened to point at.
            {
                const float kSunOffCameraDeg = 40.0f; // sun azimuth - camera azimuth, for --camera face
                const float sunAz = std::atan2(sunDir.x, sunDir.z) * (180.0f / 3.14159265f);
                const float sunEl = std::asin(std::max(-1.0f, std::min(1.0f, sunDir.y))) * (180.0f / 3.14159265f);
                const bool faceCam = strcmp(cameraMode, "face") == 0;
                if (hdriYawAuto) hdriYaw = faceCam ? faceYaw + faceAngle + kSunOffCameraDeg - sunAz : sceneYaw;
                const float rad = hdriYaw * 3.14159265f / 180.0f;
                g_hdriYawCos = std::cos(rad); g_hdriYawSin = std::sin(rad);
                sunDir = RotateHdriYaw(sunDir);
                std::printf("[info] hdri yaw %.1f deg (%s): sun azimuth %.1f -> %.1f deg, elevation %.1f deg", hdriYaw, hdriYawAuto ? "auto" : "given", sunAz, sunAz + hdriYaw, sunEl);
                if (faceCam) std::printf("; face camera axis %.1f deg, sun %.1f deg to its side", faceYaw + faceAngle, sunAz + hdriYaw - (faceYaw + faceAngle));
                std::printf("\n");
            }
            // R16G16B16A16_FLOAT with a full mip chain: mip 8 (8x4) is the diffuse irradiance lookup.
            std::vector<unsigned short> half((size_t) hdr.w * hdr.h * 4);
            for (size_t i = 0; i < (size_t) hdr.w * hdr.h; ++i) {
                half[i * 4 + 0] = FloatToHalf(hdr.rgb[i * 3 + 0]);
                half[i * 4 + 1] = FloatToHalf(hdr.rgb[i * 3 + 1]);
                half[i * 4 + 2] = FloatToHalf(hdr.rgb[i * 3 + 2]);
                half[i * 4 + 3] = FloatToHalf(1.0f);
            }
            D3D11_TEXTURE2D_DESC td{}; td.Width = hdr.w; td.Height = hdr.h; td.MipLevels = 0; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            ComPtr<ID3D11Texture2D> hdriTex;
            if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &hdriTex))) {
                ctx->UpdateSubresource(hdriTex.Get(), 0, nullptr, half.data(), hdr.w * 8, 0);
                if (SUCCEEDED(dev->CreateShaderResourceView(hdriTex.Get(), nullptr, &hdriSrv))) ctx->GenerateMips(hdriSrv.Get());
            }
            D3D11_SAMPLER_DESC hsd{}; hsd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            hsd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP; hsd.AddressV = hsd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; hsd.MaxLOD = D3D11_FLOAT32_MAX;
            dev->CreateSamplerState(&hsd, &hdriSampler);
            ComPtr<ID3DBlob> b = Compile(source, "PSSky", "ps_5_0");
            if (b) dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psSky);
            hdriActive = hdriSrv && hdriSampler && psSky;
            if (!hdriActive) std::printf("[warn] hdri resources could not be created; the flat sky/ambient is used\n");
            else {
                std::printf("[info] hdri %s: %dx%d, mean radiance (luma) %.3f, scale %.3f, exposure %.2f\n", chosen.c_str(), hdr.w, hdr.h, meanL, hdriScale, exposure);
                std::printf("[info] hdri sun dir (%.3f, %.3f, %.3f) colour (%.3f, %.3f, %.3f)\n", sunDir.x, sunDir.y, sunDir.z, sunCol[0], sunCol[1], sunCol[2]);
            }
        }
    }
    // ---- --sun-dir / --sun-strength: the Blender lamp instead of the sun extracted from the HDRI ----
    // Both bypass the extraction above (and kSunOverSky / --sun-scale) completely: the direction is
    // already in world space, so it is NOT turned by --hdri-yaw, and the strength is an irradiance E
    // in the same radiance units as the HDRI at strength 1, which the shader wants as E / pi.
    if (sunDirGiven) {
        sunDir = Normalize(Vec3{-sunDirTravel.x, -sunDirTravel.y, -sunDirTravel.z}); // travels toward -> points at the light
        std::printf("[info] sun dir override: travels (%.4f, %.4f, %.4f) -> lightDir (%.4f, %.4f, %.4f), azimuth %.1f deg, elevation %.1f deg\n",
                    sunDirTravel.x, sunDirTravel.y, sunDirTravel.z, sunDir.x, sunDir.y, sunDir.z,
                    std::atan2(sunDir.x, sunDir.z) * (180.0f / 3.14159265f),
                    std::asin(std::max(-1.0f, std::min(1.0f, sunDir.y))) * (180.0f / 3.14159265f));
    }
    if (sunStrengthGiven) {
        const float e = sunStrength / 3.14159265f;
        sunCol[0] = sunCol[1] = sunCol[2] = e;
        std::printf("[info] sun strength override: E %.3f -> E/pi %.4f (white)\n", sunStrength, e);
    }
    // Depth off for the sky pass (the cleared far depth stays, the scene draws over it).
    ComPtr<ID3D11DepthStencilState> dssNoDepth;
    { D3D11_DEPTH_STENCIL_DESC nd{}; nd.DepthEnable = FALSE; nd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; nd.DepthFunc = D3D11_COMPARISON_ALWAYS; dev->CreateDepthStencilState(&nd, &dssNoDepth); }

    // ---- the glTF scene: one dynamic vertex buffer rebuilt per frame (world positions now / previous) ----
    struct MeshVertex { float pos[3], prev[3], normal[3], colour[3], misc[3], uv[2]; };
    std::vector<pwgltf::Vertex> gVerts; std::vector<Vec3> gPrev; std::vector<MeshVertex> gGpu; std::vector<pwgltf::DrawRange> gRanges;
    Vec3 gLo{0, 0, 0}, gHi{0, 0, 0}; // world bounding box of the glTF scene: the shadow map is fitted to it
    ComPtr<ID3D11Buffer> meshVb;
    // Base-colour textures of the .glb, decoded with WIC into sRGB textures with a full mip chain.
    std::vector<ComPtr<ID3D11ShaderResourceView>> meshTextures;
    ComPtr<ID3D11ShaderResourceView> whiteTexture;
    ComPtr<ID3D11SamplerState> meshSampler;
    if (gltfPath) {
        D3D11_SAMPLER_DESC msd{}; msd.Filter = D3D11_FILTER_ANISOTROPIC; msd.MaxAnisotropy = 8; msd.AddressU = msd.AddressV = msd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP; msd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&msd, &meshSampler);
        auto makeTexture = [&](const uint8_t *rgba, UINT w, UINT h, bool srgb) -> ComPtr<ID3D11ShaderResourceView> {
            D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 0; td.ArraySize = 1;
            td.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            ComPtr<ID3D11Texture2D> tex; if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) return nullptr;
            ctx->UpdateSubresource(tex.Get(), 0, nullptr, rgba, w * 4, 0);
            ComPtr<ID3D11ShaderResourceView> srv; if (FAILED(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return nullptr;
            ctx->GenerateMips(srv.Get());
            return srv;
        };
        const uint8_t white[4] = {255, 255, 255, 255};
        whiteTexture = makeTexture(white, 1, 1, false);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IWICImagingFactory> wic;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
            for (const pwgltf::Image &img : gscene.images) {
                ComPtr<ID3D11ShaderResourceView> srv;
                ComPtr<IWICStream> stream; ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame0; ComPtr<IWICFormatConverter> conv;
                if (!img.bytes.empty() && SUCCEEDED(wic->CreateStream(&stream)) &&
                    SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE *>(img.bytes.data()), (DWORD) img.bytes.size())) &&
                    SUCCEEDED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
                    SUCCEEDED(decoder->GetFrame(0, &frame0)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
                    SUCCEEDED(conv->Initialize(frame0.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                    UINT w = 0, h = 0; conv->GetSize(&w, &h);
                    std::vector<uint8_t> rgba((size_t) w * h * 4);
                    if (w && h && SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT) rgba.size(), rgba.data()))) srv = makeTexture(rgba.data(), w, h, true);
                }
                if (!srv) std::printf("[warn] glTF image (%s, %zu bytes) could not be decoded; white is used\n", img.mime.c_str(), img.bytes.size());
                meshTextures.push_back(srv);
            }
        } else std::printf("[warn] WIC unavailable: glTF textures are not used\n");
        std::printf("[info] glTF textures: %zu image(s), %zu decoded\n", gscene.images.size(), (size_t) std::count_if(meshTextures.begin(), meshTextures.end(), [](const ComPtr<ID3D11ShaderResourceView> &s) { return s != nullptr; }));
        pwgltf::BuildVertices(gscene, 0.0f, 1.0f / 60.0f, gVerts, gPrev, &gRanges);
        if (gVerts.empty()) { std::printf("[fail] glTF: no triangles\n"); return 1; }
        gLo = gHi = gVerts[0].pos;
        for (const pwgltf::Vertex &v : gVerts) {
            gLo.x = std::min(gLo.x, v.pos.x); gLo.y = std::min(gLo.y, v.pos.y); gLo.z = std::min(gLo.z, v.pos.z);
            gHi.x = std::max(gHi.x, v.pos.x); gHi.y = std::max(gHi.y, v.pos.y); gHi.z = std::max(gHi.z, v.pos.z);
        }
        const Vec3 c = (gLo + gHi) * 0.5f, e = (gHi - gLo) * 0.6f; // 20% margin: morphs and animation move vertices
        gLo = c - e; gHi = c + e;
        std::printf("[info] glTF bounds (%.1f, %.1f, %.1f) .. (%.1f, %.1f, %.1f)\n", gLo.x, gLo.y, gLo.z, gHi.x, gHi.y, gHi.z);
        D3D11_BUFFER_DESC vbd{}; vbd.Usage = D3D11_USAGE_DYNAMIC; vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER; vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        vbd.ByteWidth = (UINT) (gVerts.size() * sizeof(MeshVertex));
        if (FAILED(dev->CreateBuffer(&vbd, nullptr, &meshVb))) { std::printf("[fail] glTF vertex buffer\n"); return 1; }
        gGpu.resize(gVerts.size());
    }
    double skinMsTotal = 0.0; int skinFrames = 0;
    auto uploadMesh = [&](float t) {
        const auto t0 = std::chrono::high_resolution_clock::now();
        pwgltf::BuildVertices(gscene, t, animSpeed / 60.0f, gVerts, gPrev, &gRanges);
        skinMsTotal += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
        ++skinFrames;
        for (size_t i = 0; i < gVerts.size() && i < gGpu.size(); ++i) {
            const pwgltf::Vertex &v = gVerts[i]; MeshVertex &g = gGpu[i];
            g.pos[0] = v.pos.x; g.pos[1] = v.pos.y; g.pos[2] = v.pos.z;
            g.prev[0] = gPrev[i].x; g.prev[1] = gPrev[i].y; g.prev[2] = gPrev[i].z;
            g.normal[0] = v.normal.x; g.normal[1] = v.normal.y; g.normal[2] = v.normal.z;
            memcpy(g.colour, v.colour, sizeof(g.colour));
            const bool textured = v.texture >= 0 && (size_t) v.texture < meshTextures.size() && meshTextures[v.texture];
            g.misc[0] = v.material < -1.5f && v.stripe > 0.5f ? -3.0f : v.material; g.misc[1] = v.localY; g.misc[2] = textured ? (v.alphaMask ? 2.0f + v.alphaCutoff : 1.0f) : 0.0f;
            g.uv[0] = v.uv[0]; g.uv[1] = v.uv[1];
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(meshVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, gGpu.data(), gGpu.size() * sizeof(MeshVertex)); ctx->Unmap(meshVb.Get(), 0); }
    };

    // ---- the 3D scene: instances (boxes + the figure at the origin) ----
    InstanceConstants inst{};
    int instanceCount = 0;
    auto addBox = [&](float x, float y, float z, float hx, float hy, float hz, float material) {
        if (instanceCount >= 64) return;
        inst.pos[instanceCount][0] = x; inst.pos[instanceCount][1] = y; inst.pos[instanceCount][2] = z;
        inst.size[instanceCount][0] = hx; inst.size[instanceCount][1] = hy; inst.size[instanceCount][2] = hz; inst.size[instanceCount][3] = material;
        ++instanceCount;
    };
    // The figure: legs, torso, head, arms.
    addBox(-0.18f, 0.45f, 0.0f, 0.14f, 0.45f, 0.14f, 2); addBox(0.18f, 0.45f, 0.0f, 0.14f, 0.45f, 0.14f, 2);
    addBox(0.0f, 1.25f, 0.0f, 0.34f, 0.36f, 0.2f, 0); addBox(0.0f, 1.82f, 0.0f, 0.16f, 0.18f, 0.16f, 3);
    addBox(-0.48f, 1.25f, 0.0f, 0.11f, 0.34f, 0.11f, 5); addBox(0.48f, 1.25f, 0.0f, 0.11f, 0.34f, 0.11f, 5);
    // Boxes and columns on rings around the origin (deterministic pseudo-random sizes).
    unsigned seed = 12345u;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
    for (int ring = 0; ring < 4 && instanceCount < 62; ++ring) {
        const float radius = 6.0f + ring * 7.0f;
        const int count = 8 + ring * 4;
        for (int k = 0; k < count && instanceCount < 62; ++k) {
            const float a = (k + 0.37f * ring) / count * 6.2831853f;
            const float hx = 0.4f + rnd() * 1.2f, hz = 0.4f + rnd() * 1.2f, hy = 0.6f + rnd() * 2.4f;
            addBox(std::sin(a) * radius + (rnd() - 0.5f) * 2.0f, hy, std::cos(a) * radius + (rnd() - 0.5f) * 2.0f, hx, hy, hz, (float) (k % 6));
        }
    }
    const int movingA = instanceCount - 1, movingB = instanceCount - 2; // the last two boxes may move
    if (GetEnvironmentVariableA("PW_BENCH_LIST_BOXES", nullptr, 0)) {
        for (int i = 0; i < instanceCount; ++i)
            std::printf("[box] %2d pos %7.2f %5.2f %7.2f half %5.2f %5.2f %5.2f colour %g\n", i, inst.pos[i][0], inst.pos[i][1], inst.pos[i][2], inst.size[i][0], inst.size[i][1], inst.size[i][2], inst.size[i][3]);
        for (int i = 0; i < instanceCount; ++i) for (int j = i + 1; j < instanceCount; ++j)
            if (std::fabs(inst.pos[i][0] - inst.pos[j][0]) < inst.size[i][0] + inst.size[j][0] && std::fabs(inst.pos[i][2] - inst.pos[j][2]) < inst.size[i][2] + inst.size[j][2])
                std::printf("[box] overlap %d (colour %g) with %d (colour %g)\n", i, inst.size[i][3], j, inst.size[j][3]);
    }
    for (int i = 0; i < instanceCount; ++i) for (int c = 0; c < 4; ++c) inst.prev[i][c] = inst.pos[i][c];

    // ---- DLSS feature ----
    HMODULE ngx = nullptr; PFN_Evaluate ngxEvaluate = nullptr; NVSDK_NGX_Parameter *params = nullptr; NVSDK_NGX_Handle *feature = nullptr;
    if (!reference) {
        ngx = LoadLibraryW(L"C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_a3944b54ff18b284\\_nvngx.dll");
        if (!ngx) { std::printf("[fail] _nvngx.dll\n"); return 1; }
        auto ngxInit = (PFN_Init) GetProcAddress(ngx, "NVSDK_NGX_D3D11_Init");
        auto ngxAlloc = (PFN_Alloc) GetProcAddress(ngx, "NVSDK_NGX_D3D11_AllocateParameters");
        auto ngxCreate = (PFN_Create) GetProcAddress(ngx, "NVSDK_NGX_D3D11_CreateFeature");
        ngxEvaluate = (PFN_Evaluate) GetProcAddress(ngx, "NVSDK_NGX_D3D11_EvaluateFeature");
        if (!ngxInit || !ngxAlloc || !ngxCreate || !ngxEvaluate) { std::printf("[fail] NGX exports\n"); return 1; }
        NVSDK_NGX_Result r = ngxInit(0x24480451ull, exeDir, dev.Get(), nullptr, NVSDK_NGX_Version_API);
        std::printf("[info] NGX D3D11 init -> 0x%08X\n", (unsigned) r);
        if (NVSDK_NGX_FAILED(r)) return 1;
        r = ngxAlloc(&params);
        if (NVSDK_NGX_FAILED(r) || !params) { std::printf("[fail] params 0x%08X\n", (unsigned) r); return 1; }
        params->Set(NVSDK_NGX_Parameter_Width, kRenderW);
        params->Set(NVSDK_NGX_Parameter_Height, kRenderH);
        params->Set(NVSDK_NGX_Parameter_OutWidth, kOutW);
        params->Set(NVSDK_NGX_Parameter_OutHeight, kOutH);
        params->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int) NVSDK_NGX_PerfQuality_Value_Balanced);
        params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, (int) (NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_DoSharpening | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure));
        params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        r = ngxCreate(ctx.Get(), NVSDK_NGX_Feature_SuperSampling, params, &feature);
        std::printf("[info] DLSS create -> 0x%08X\n", (unsigned) r);
        if (NVSDK_NGX_FAILED(r)) return 1;
    }

    // ---- add-on control exports ----
    PFN_SetLayout setLayout = nullptr;
    if (switchEvery > 0) {
        HMODULE addon = GetModuleHandleW(L"optimizer-fps-dlss5.addon64");
        setLayout = addon ? (PFN_SetLayout) GetProcAddress(addon, "PeripheralWarpSetLayoutV1") : nullptr;
        std::printf("[info] layout switching every %d frames: export %s\n", switchEvery, setLayout ? "found" : "MISSING");
        if (!setLayout) return 1;
    }
    using PFN_SetTemporal = unsigned int(__cdecl *)(unsigned int, unsigned int);
    PFN_SetTemporal setTemporal = nullptr;
    if (temporalMode >= 0) {
        HMODULE addon = GetModuleHandleW(L"optimizer-fps-dlss5.addon64");
        setTemporal = addon ? (PFN_SetTemporal) GetProcAddress(addon, "PeripheralWarpSetTemporalV1") : nullptr;
        std::printf("[info] temporal mode %d every %d: export %s\n", temporalMode, temporalEvery, setTemporal ? "found" : "MISSING");
        if (!setTemporal) return 1;
    }
    static const unsigned int kCycle[] = {0, 1, 2};
    int cycleIndex = 0;
    bool switchPending = false;
    int switches = 0;
    const float vx = 0.30f, vy = 0.17f; // flat scene: uv per second

    // Jitter sequence (8 samples, the same for all frames of an 8-cycle).
    auto jitter = [](int index, float *jx, float *jy) {
        *jx = (float) (((index % 8) + 0.5) / 8.0 - 0.5);
        *jy = (float) ((((index * 3) % 8) + 0.5) / 8.0 - 0.5);
    };
    Mat4 vpPrevNoJitter = Identity();
    bool havePrev = false;

    // Renders the 3D scene once into colour/motion/depth with the given jitter (and weight for the
    // reference accumulation).
    auto renderScene3D = [&](int frame, float jx, float jy, float weight, bool additiveColor, UINT w, UINT h) {
        CameraState cam;
        float fovY = 60.0f;
        if (camPosGiven && camTargetGiven) {
            // --cam-pos / --cam-target / --fov (and the --camera blender preset): a fixed camera in
            // glTF Y-up metres, scaled into the scene's own units by --scene-scale.
            cam.eye = camPosM * sceneScale;
            cam.target = camTargetM * sceneScale;
            if (fovGiven && camFov > 0.0f) fovY = camFov;
            static bool s_logCam = false;
            if (!s_logCam) { s_logCam = true; std::printf("[info] camera override: eye (%.2f, %.2f, %.2f) target (%.2f, %.2f, %.2f) in scene units (scale %.0f), fovY %.4f deg, shift (%.3f, %.3f)\n",
                                                          cam.eye.x, cam.eye.y, cam.eye.z, cam.target.x, cam.target.y, cam.target.z, sceneScale, fovY, camShift.x, camShift.y); }
        } else if (gltfPath && strcmp(cameraMode, "file") == 0) {
            if (!pwgltf::CameraAt(gscene, frame * animSpeed / 60.0f, cam.eye, cam.target, fovY)) cam = CameraAt(frame, "script");
        } else if (gltfPath && strcmp(cameraMode, "face") == 0) {
            // Head = bounding box of the meshes under the face node at this instant (morphs and node animation
            // included), sweeping slowly around its centre; the radius follows the head's size unless given.
            static int s_anchor = -2;
            if (s_anchor == -2) {
                s_anchor = -1;
                for (size_t i = 0; i < gscene.nodes.size() && s_anchor < 0; ++i) if (gscene.nodes[i].name == faceNode) s_anchor = (int) i;
                for (size_t i = 0; i < gscene.nodes.size() && s_anchor < 0; ++i) { const std::string &n = gscene.nodes[i].name; if (n.find("girl") != std::string::npos || n.find("haracter") != std::string::npos || n.find("head") != std::string::npos) s_anchor = (int) i; }
                if (s_anchor >= 0) std::printf("[info] face camera: node %d (%s)\n", s_anchor, gscene.nodes[s_anchor].name.c_str());
                else std::printf("[warn] face camera: node %s not found; the scripted camera is used\n", faceNode);
            }
            bool placed = false;
            if (s_anchor >= 0) {
                const float t = frame * animSpeed / 60.0f;
                Vec3 lo{0, 0, 0}, hi{0, 0, 0};
                pwgltf::MeshBounds(gscene, t, s_anchor, lo, hi); // morphs + skinning applied
                if (lo.x < hi.x) {
                    fovY = 35.0f;
                    const Vec3 head = (lo + hi) * 0.5f;
                    const float height = hi.y - lo.y, depth = hi.z - lo.z;
                    // Auto radius (--face-radius 0): the distance at which the head's height fills
                    // kFaceFill of the frame (frame height at distance d is 2*d*tan(fovY/2), so
                    // d = 0.5*height/tan(fovY/2)/kFaceFill). The silhouette extremes - the top of the
                    // hair and the chin - sit near the box's centre plane, which is what the camera
                    // orbits, so that is the plane the fill is computed for; half the box's depth is
                    // the floor, so the near cheek can never end up behind the camera on a wide head.
                    // kFaceAim then drops the aim point below the box centre until the eye line lands
                    // at ~40% from the top of the frame (the eye line itself is near centre + 0.03 x
                    // height on this box, which reaches well below the chin).
                    const float kFaceFill = 0.75f, kFaceAim = -0.10f;
                    const float fit = 0.5f * height / std::tan(fovY * 3.14159265f / 360.0f) / kFaceFill;
                    const float radius0 = faceRadius > 0.0f ? faceRadius : std::max(fit, 0.5f * depth);
                    static bool s_logged = false;
                    if (!s_logged) { s_logged = true; std::printf("[info] face camera: head %.2f x %.2f x %.2f, centre (%.1f, %.1f, %.1f), radius %.2f (%s), fill %.0f%%, angle %.1f deg\n",
                                                                  hi.x - lo.x, height, depth, head.x, head.y, head.z, radius0, faceRadius > 0.0f ? "given" : "auto", kFaceFill * 100.0f, faceAngle); }
                    // --face-angle turns the camera off the face axis so the head reads slightly turned.
                    const float a = (faceYaw + faceAngle + faceSweep * std::sin(t * 0.6f)) * 3.14159265f / 180.0f; // slow sweep (0 = still)
                    const float r = radius0 * (1.0f + (faceSweep > 0.0f ? 0.08f : 0.0f) * std::sin(t * 0.37f)); // and a slight dolly
                    const Vec3 aim = head + Vec3{0.0f, kFaceAim * height, 0.0f};
                    cam.eye = aim + Vec3{std::sin(a) * r, 0.0f, std::cos(a) * r}; // level with the eye line
                    cam.target = aim;
                    placed = true;
                }
            }
            if (!placed) cam = CameraAt(frame, "script");
        } else {
            cam = CameraAt(frame, cameraMode);
        }
        const float aspect = (float) w / (float) h;
        const Mat4 view = LookAt(cam.eye, cam.target);
        // --cam-shift: Blender's camera shift_x / shift_y, in units of the FITTED sensor dimension
        // (the frame's height for the "VERTICAL" sensor fit the .blend uses). A shift moves the frame,
        // so the content moves the other way; measured against Blender: content_dx = -shift_x * height
        // and content_dy(down) = +shift_y * height pixels, which is exactly what the projection's
        // jitter offset expresses. It goes into the unjittered matrices too - it is part of the
        // camera, not of the sampling pattern - so the motion vectors stay correct.
        const float shiftX = -camShift.x * (float) w, shiftY = camShift.y * (float) w; // Blender: shift is a fraction of the larger frame dimension
        const Mat4 projJ = Perspective(fovY, aspect, 0.1f, 5000.0f, jx + shiftX, jy + shiftY, (float) w, (float) h);
        const Mat4 proj = Perspective(fovY, aspect, 0.1f, 5000.0f, shiftX, shiftY, (float) w, (float) h);
        FrameConstants fc{};
        fc.vp = Mul(projJ, view);
        fc.vpNoJitter = Mul(proj, view);
        fc.vpPrev = havePrev ? vpPrevNoJitter : fc.vpNoJitter;
        // Shadow map: an orthographic light frustum fitted to the glTF scene box (a fixed box around
        // the camera for the procedural scene), the same light direction as the pixel shader uses.
        {
            const Vec3 lightDir = sunDir; // the same variable the pixel shader gets through FrameConstants
            Vec3 lo = gLo, hi = gHi;
            if (gltfPath && !gVerts.empty()) {
                // Fit to THIS frame's deformed vertices (gVerts is rebuilt before the shadow pass): the load-time
                // box is the bind pose and a turned/animated head left it, which cut the shadow map off in a
                // straight line across the cheek (2026-09-08). 15% margin plus a floor for thin scenes.
                lo = hi = gVerts[0].pos;
                for (const pwgltf::Vertex &v : gVerts) {
                    lo.x = std::min(lo.x, v.pos.x); lo.y = std::min(lo.y, v.pos.y); lo.z = std::min(lo.z, v.pos.z);
                    hi.x = std::max(hi.x, v.pos.x); hi.y = std::max(hi.y, v.pos.y); hi.z = std::max(hi.z, v.pos.z);
                }
                const Vec3 c = (lo + hi) * 0.5f; Vec3 e = (hi - lo) * 0.575f;
                const float floorPad = 0.02f * std::max(e.x, std::max(e.y, e.z));
                e.x = std::max(e.x, floorPad); e.y = std::max(e.y, floorPad); e.z = std::max(e.z, floorPad);
                lo = c - e; hi = c + e;
            }
            if (!gltfPath) { const Vec3 c{cam.target.x, 0.0f, cam.target.z}; lo = c - Vec3{60, 5, 60}; hi = c + Vec3{60, 40, 60}; }
            const Vec3 centre = (lo + hi) * 0.5f, half = (hi - lo) * 0.5f;
            const float radius = std::sqrt(Dot(half, half));
            const Mat4 lightView = LookAt(centre + lightDir * (radius * 2.0f), centre); // lightDir points at the light, as in the PS
            float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
            for (int k = 0; k < 8; ++k) {
                const Vec3 p{k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z};
                for (int r = 0; r < 3; ++r) {
                    const float v = lightView.m[r][0] * p.x + lightView.m[r][1] * p.y + lightView.m[r][2] * p.z + lightView.m[r][3];
                    mn[r] = std::min(mn[r], v); mx[r] = std::max(mx[r], v);
                }
            }
            const float pad = 0.02f * radius + 1e-3f;
            // The near/far planes are the box's own extent along the light axis (plus the same 2% pad),
            // so the normalized depth stays tight: env.z below turns a world-unit bias into ndc.
            const float zn = std::max(0.01f, mn[2] - pad), zf = mx[2] + pad;
            fc.lightVp = Mul(Ortho(mn[0] - pad, mx[0] + pad, mn[1] - pad, mx[1] + pad, zn, zf), lightView);
            const float texel = std::max((mx[0] - mn[0]) + 2 * pad, (mx[1] - mn[1]) + 2 * pad) / (float) kShadowSize;
            fc.eyePos[3] = texel; // one shadow texel in world units
            fc.env[2] = zf > zn ? 1.0f / (zf - zn) : 0.0f; // normalized depth per world unit along the light axis
            // --shadow-soft: penumbra half-width as a fraction of the frustum's width. The frustum's
            // width IS kShadowSize texels, so the radius in texels is simply soft * kShadowSize;
            // clamp 1..48 (below 1 the Poisson disk degenerates, above 48 the 16 taps under-sample).
            const float softTexels = std::min(48.0f, std::max(1.0f, shadowSoft * (float) kShadowSize));
            fc.env[3] = softTexels;
            static bool s_logShadow = false;
            if (!s_logShadow) {
                s_logShadow = true;
                std::printf("[info] shadow map %ux%u: ortho %.2f x %.2f world units, depth %.2f .. %.2f (range %.2f), texel %.5f world units\n",
                            kShadowSize, kShadowSize, (mx[0] - mn[0]) + 2 * pad, (mx[1] - mn[1]) + 2 * pad, zn, zf, zf - zn, texel);
                std::printf("[info] soft shadows: 16-tap rotated Poisson, --shadow-soft %.4f -> radius %.1f texels = %.4f world units penumbra\n",
                            shadowSoft, softTexels, softTexels * texel);
            }
        }
        fc.renderSize[0] = (float) w; fc.renderSize[1] = (float) h; fc.renderSize[2] = mvDir; fc.renderSize[3] = 1.0f / mvScale;
        fc.invVp = Invert(fc.vp);
        fc.lightDir[0] = sunDir.x; fc.lightDir[1] = sunDir.y; fc.lightDir[2] = sunDir.z; fc.lightDir[3] = hdriActive ? (g_hdriMirror ? 2.0f : 1.0f) : 0.0f;
        fc.sunColor[0] = sunCol[0]; fc.sunColor[1] = sunCol[1]; fc.sunColor[2] = sunCol[2]; fc.sunColor[3] = hdriScale * exposure;
        fc.hdriParams[0] = 2.5f * g_hdriMeanL; fc.hdriParams[1] = fc.hdriParams[2] = fc.hdriParams[3] = 0.0f;
        fc.env[0] = g_hdriYawCos; fc.env[1] = g_hdriYawSin; // env.z / env.w are set with the shadow frustum above
        fc.misc[0] = weight; fc.misc[1] = frame / 60.0f; fc.misc[3] = exposure;
        fc.eyePos[0] = cam.eye.x; fc.eyePos[1] = cam.eye.y; fc.eyePos[2] = cam.eye.z;
        static const bool idColours = GetEnvironmentVariableA("PW_BENCH_ID_COLOURS", nullptr, 0) != 0; // debug
        static const float dbgMode = [] { char v[8] = {}; return GetEnvironmentVariableA("PW_BENCH_ID_COLOURS", v, sizeof(v)) ? (float) std::max(1, atoi(v)) : 0.0f; }();
        fc.misc[2] = dbgMode;
        D3D11_MAPPED_SUBRESOURCE m{};
        ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m); memcpy(m.pData, &fc, sizeof(fc)); ctx->Unmap(cb.Get(), 0);
        // Object motion: the two last boxes bob and circle; previous positions carry the object MVs.
        if (movingBoxes) {
            const float t = frame / 60.0f, tp = (frame - 1) / 60.0f;
            auto place = [&](int i, float time, float (*dst)[4]) {
                const float baseX = inst.pos[i][0], baseZ = inst.pos[i][2];
                dst[i][0] = baseX + std::sin(time * 1.3f) * 1.5f; dst[i][1] = inst.size[i][1] + 0.5f + 0.5f * std::sin(time * 2.1f); dst[i][2] = baseZ + std::cos(time * 1.3f) * 1.5f;
            };
            static float basePos[2][4] = {}; static bool baseSaved = false;
            if (!baseSaved) { memcpy(basePos[0], inst.pos[movingA], 16); memcpy(basePos[1], inst.pos[movingB], 16); baseSaved = true; }
            memcpy(inst.pos[movingA], basePos[0], 16); memcpy(inst.pos[movingB], basePos[1], 16);
            place(movingA, tp, inst.prev); place(movingB, tp, inst.prev);
            place(movingA, t, inst.pos); place(movingB, t, inst.pos);
        }
        ctx->Map(cbInst.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m); memcpy(m.pData, &inst, sizeof(inst)); ctx->Unmap(cbInst.Get(), 0);
        if (movingBoxes) { // restore base positions for the next frame's "previous"
            static float base2[2][4]; (void) base2;
        }
        (void) additiveColor;
        if (gltfPath) uploadMesh(frame * animSpeed / 60.0f);
        // ---- shadow pass: depth only, from the light, same geometry, cull NONE ----
        {
            ID3D11ShaderResourceView *noShadow = nullptr; ctx->PSSetShaderResources(2, 1, &noShadow); // the SRV cannot stay bound while we write
            ctx->ClearDepthStencilView(shadowDsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
            ctx->OMSetRenderTargets(0, nullptr, shadowDsv.Get());
            ctx->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
            ctx->OMSetDepthStencilState(dssShadow.Get(), 0);
            ctx->RSSetState(rs.Get());
            D3D11_VIEWPORT svp{0, 0, (float) kShadowSize, (float) kShadowSize, 0, 1}; ctx->RSSetViewports(1, &svp);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11Buffer *scbs[] = {cb.Get(), cbInst.Get()};
            ctx->VSSetConstantBuffers(0, 2, scbs);
            ctx->PSSetShader(nullptr, nullptr, 0);
            if (gltfPath) {
                const UINT stride = sizeof(MeshVertex), offset = 0;
                ID3D11Buffer *vb = meshVb.Get();
                ctx->IASetInputLayout(meshLayout.Get());
                ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
                ctx->VSSetShader(vsMeshShadow.Get(), nullptr, 0);
                // Alpha-tested cut-outs (eyelashes, hair cards) need the texture here too, or they
                // cast solid quads and blotch the face.
                ctx->PSSetShader(psMeshShadow.Get(), nullptr, 0);
                ctx->PSSetSamplers(1, 1, meshSampler.GetAddressOf());
                for (const pwgltf::DrawRange &r : gRanges) {
                    ID3D11ShaderResourceView *srv = (r.texture >= 0 && (size_t) r.texture < meshTextures.size() && meshTextures[r.texture]) ? meshTextures[r.texture].Get() : whiteTexture.Get();
                    ctx->PSSetShaderResources(1, 1, &srv);
                    ctx->Draw(r.count, r.start);
                }
                ID3D11ShaderResourceView *noTex0 = nullptr; ctx->PSSetShaderResources(1, 1, &noTex0);
                ctx->PSSetShader(nullptr, nullptr, 0);
                ctx->IASetInputLayout(nullptr);
                ID3D11Buffer *noVb = nullptr; const UINT zero0 = 0;
                ctx->IASetVertexBuffers(0, 1, &noVb, &zero0, &zero0);
            } else {
                ctx->VSSetShader(vsGroundShadow.Get(), nullptr, 0); ctx->Draw(3, 0);
                ctx->VSSetShader(vsBoxShadow.Get(), nullptr, 0); ctx->DrawInstanced(36, instanceCount, 0, 0);
            }
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
        }
        // Without an HDRI the flat sky colour is the clear; with one the sky pass below covers every pixel.
        const float sky[4] = {hdriActive ? 0.0f : 0.18f, hdriActive ? 0.0f : 0.28f, hdriActive ? 0.0f : 0.45f, 1.0f}, zero[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(colorRtv.Get(), sky);
        ctx->ClearRenderTargetView(motionRtv.Get(), zero);
        ID3D11RenderTargetView *rts[] = {colorRtv.Get(), motionRtv.Get()};
        ctx->OMSetRenderTargets(2, rts, dsv.Get());
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        ctx->OMSetDepthStencilState(dss.Get(), 0);
        ctx->RSSetState(rs.Get());
        D3D11_VIEWPORT vp{0, 0, (float) w, (float) h, 0, 1}; ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer *cbs[] = {cb.Get(), cbInst.Get()};
        ctx->VSSetConstantBuffers(0, 2, cbs);
        ctx->PSSetConstantBuffers(0, 2, cbs);
        if (hdriActive) { ctx->PSSetShaderResources(3, 1, hdriSrv.GetAddressOf()); ctx->PSSetSamplers(3, 1, hdriSampler.GetAddressOf()); }
        // Sky first, depth off: the fullscreen triangle fills colour and motion, the scene draws over it.
        if (hdriActive) {
            ctx->OMSetDepthStencilState(dssNoDepth.Get(), 0);
            ctx->IASetInputLayout(nullptr);
            ctx->VSSetShader(vsBlit.Get(), nullptr, 0);
            ctx->PSSetShader(psSky.Get(), nullptr, 0);
            ctx->Draw(3, 0);
            ctx->OMSetDepthStencilState(dss.Get(), 0);
        }
        ctx->PSSetShader(psScene.Get(), nullptr, 0);
        ctx->PSSetShaderResources(2, 1, shadowSrv.GetAddressOf());
        ctx->PSSetSamplers(2, 1, shadowSmp.GetAddressOf());
        if (gltfPath) {
            const UINT stride = sizeof(MeshVertex), offset = 0;
            ID3D11Buffer *vb = meshVb.Get();
            ctx->IASetInputLayout(meshLayout.Get());
            ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            ctx->VSSetShader(vsMesh.Get(), nullptr, 0);
            ctx->PSSetSamplers(1, 1, meshSampler.GetAddressOf());
            for (const pwgltf::DrawRange &r : gRanges) {
                ID3D11ShaderResourceView *srv = (r.texture >= 0 && (size_t) r.texture < meshTextures.size() && meshTextures[r.texture]) ? meshTextures[r.texture].Get() : whiteTexture.Get();
                ctx->PSSetShaderResources(1, 1, &srv);
                ctx->Draw(r.count, r.start);
            }
            ID3D11ShaderResourceView *noTex = nullptr; ctx->PSSetShaderResources(1, 1, &noTex);
            ctx->IASetInputLayout(nullptr);
            ID3D11Buffer *noVb = nullptr; const UINT zero0 = 0;
            ctx->IASetVertexBuffers(0, 1, &noVb, &zero0, &zero0);
        } else {
            static const bool noGround = GetEnvironmentVariableA("PW_BENCH_NO_GROUND", nullptr, 0) != 0; // debug
            if (!noGround) { ctx->VSSetShader(vsGround.Get(), nullptr, 0); ctx->Draw(3, 0); }
            ctx->VSSetShader(vsBox.Get(), nullptr, 0);
            ctx->DrawInstanced(36, instanceCount, 0, 0);
        }
        ID3D11RenderTargetView *none[2] = {}; ctx->OMSetRenderTargets(2, none, nullptr);
        ID3D11ShaderResourceView *noShadow = nullptr; ctx->PSSetShaderResources(2, 1, &noShadow);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        vpPrevNoJitter = fc.vpNoJitter;
        havePrev = true;
    };

    int frame = 0;
    for (; frame < frames; ++frame) {
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); if (msg.message == WM_QUIT) frame = frames; }
        if (setLayout && frame > 0 && frame % switchEvery == 0) switchPending = true;
        if (switchPending) {
            LayoutStateV1 st{};
            st.structSize = sizeof(st); st.mode = kCycle[cycleIndex % 3]; st.filter = 1;
            st.centerX = st.centerY = 80.0f; st.workX = st.workY = 90.0f; st.globalScalePercent = 100.0f;
            const unsigned int status = setLayout(&st);
            if (status != 3) {
                std::printf("[info] frame %d: layout -> mode %u (status %u)\n", frame, st.mode, status);
                switchPending = false; ++cycleIndex; ++switches;
            }
        }
        if (setTemporal && frame == 10) {
            const unsigned int status = setTemporal((unsigned) temporalMode, (unsigned) temporalEvery);
            std::printf("[info] frame %d: temporal mode -> %d every %d (status %u)\n", frame, temporalMode, temporalEvery, status);
        }
        float jx = 0.0f, jy = 0.0f;
        jitter(frame, &jx, &jy);
        const float clearDepth = flat ? 1.0f : 0.0f;
        ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, clearDepth, 0);
        if (flat) {
            const float time = frame / 60.0f;
            D3D11_MAPPED_SUBRESOURCE m{}; ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
            float c[8] = {time, vx, vy, 1.0f / mvScale, mvDir, 0, 0, 0}; memcpy(m.pData, c, 32); ctx->Unmap(cb.Get(), 0);
            ID3D11RenderTargetView *rts[] = {colorRtv.Get(), motionRtv.Get()};
            ctx->OMSetRenderTargets(2, rts, dsv.Get());
            ctx->OMSetDepthStencilState(dss.Get(), 0);
            ctx->RSSetState(rs.Get());
            D3D11_VIEWPORT vp{0, 0, (float) kRenderW, (float) kRenderH, 0, 1}; ctx->RSSetViewports(1, &vp);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vsFlat.Get(), nullptr, 0); ctx->PSSetShader(psScene.Get(), nullptr, 0);
            ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
            ctx->Draw(3, 0);
            ID3D11RenderTargetView *none[2] = {}; ctx->OMSetRenderTargets(2, none, nullptr);
        } else if (reference) {
            // Eight jittered renders averaged into the 4K colour target (additive blend, weight 1/8);
            // depth is cleared per sample so each sample resolves its own visibility.
            const float clear[4] = {0, 0, 0, 0};
            ctx->ClearRenderTargetView(outputRtv.Get(), clear);
            static const int refSamples = [] { char v[16] = {}; return GetEnvironmentVariableA("PW_BENCH_REF_SAMPLES", v, sizeof(v)) ? std::max(1, atoi(v)) : 8; }();
            for (int s = 0; s < refSamples; ++s) {
                float sx, sy; jitter(s, &sx, &sy);
                ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
                renderScene3D(frame, sx, sy, 1.0f / refSamples, true, kOutW, kOutH);
                ctx->OMSetRenderTargets(1, outputRtv.GetAddressOf(), nullptr);
                ctx->OMSetBlendState(additive.Get(), nullptr, 0xffffffffu);
                ctx->OMSetDepthStencilState(nullptr, 0);
                D3D11_VIEWPORT vpa{0, 0, (float) kOutW, (float) kOutH, 0, 1}; ctx->RSSetViewports(1, &vpa);
                ctx->VSSetShader(vsBlit.Get(), nullptr, 0);
                ctx->PSSetShader(psAccum.Get(), nullptr, 0);
                ctx->PSSetShaderResources(0, 1, colorSrv.GetAddressOf());
                ctx->PSSetSamplers(0, 1, smp.GetAddressOf());
                ctx->Draw(3, 0);
                ID3D11ShaderResourceView *nosrv = nullptr; ctx->PSSetShaderResources(0, 1, &nosrv);
                ctx->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
                ctx->OMSetRenderTargets(0, nullptr, nullptr);
            }
        } else {
            renderScene3D(frame, jx, jy, 1.0f, false, kRenderW, kRenderH);
        }

        if (!reference) {
            params->Set(NVSDK_NGX_Parameter_Color, (ID3D11Resource *) color.Get());
            params->Set(NVSDK_NGX_Parameter_Depth, (ID3D11Resource *) depth.Get());
            params->Set(NVSDK_NGX_Parameter_MotionVectors, (ID3D11Resource *) motion.Get());
            params->Set(NVSDK_NGX_Parameter_Output, (ID3D11Resource *) output.Get());
            params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, jx);
            params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, jy);
            params->Set(NVSDK_NGX_Parameter_MV_Scale_X, mvScale);
            params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, mvScale);
            params->Set(NVSDK_NGX_Parameter_Sharpness, 0.15f);
            params->Set(NVSDK_NGX_Parameter_Reset, frame == 0 ? 1 : 0);
            params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, kRenderW);
            params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, kRenderH);
            params->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
            params->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
            const NVSDK_NGX_Result r = ngxEvaluate(ctx.Get(), feature, params, nullptr);
            if (NVSDK_NGX_FAILED(r)) std::printf("[warn] frame %d evaluate 0x%08X\n", frame, (unsigned) r);
        }

        ctx->OMSetRenderTargets(1, backRtv.GetAddressOf(), nullptr);
        ctx->OMSetDepthStencilState(nullptr, 0);
        D3D11_VIEWPORT vpo{0, 0, (float) presentW, (float) presentH, 0, 1}; ctx->RSSetViewports(1, &vpo);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vsBlit.Get(), nullptr, 0);
        ctx->PSSetShader(psBlit.Get(), nullptr, 0);
        ctx->PSSetShaderResources(0, 1, outputSrv.GetAddressOf());
        ctx->PSSetSamplers(0, 1, smp.GetAddressOf());
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView *nosrv = nullptr; ctx->PSSetShaderResources(0, 1, &nosrv);
        sc->Present(0, 0);
        if (fpsCap > 0.0) {
            static LARGE_INTEGER capStart{}; static LARGE_INTEGER capFreq{};
            if (capFreq.QuadPart == 0) QueryPerformanceFrequency(&capFreq);
            const long long budget = (long long) (capFreq.QuadPart / fpsCap);
            if (capStart.QuadPart != 0) { LARGE_INTEGER now; do { QueryPerformanceCounter(&now); } while (now.QuadPart - capStart.QuadPart < budget); capStart.QuadPart += budget; if (now.QuadPart - capStart.QuadPart > budget) capStart = now; }
            else QueryPerformanceCounter(&capStart);
        }
        {
            static LARGE_INTEGER lastPresent{}; static std::vector<double> intervals; static LARGE_INTEGER qpf{};
            if (qpf.QuadPart == 0) QueryPerformanceFrequency(&qpf);
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            if (lastPresent.QuadPart != 0 && frame >= 60) intervals.push_back((double) (now.QuadPart - lastPresent.QuadPart) * 1000.0 / (double) qpf.QuadPart);
            lastPresent = now;
            if (frametime && frame == frames - 1 && !intervals.empty()) {
                double sum = 0, mn = 1e9, mx = 0; for (double v : intervals) { sum += v; mn = std::min(mn, v); mx = std::max(mx, v); }
                const double avg = sum / intervals.size(); double var = 0; int slow = 0;
                for (double v : intervals) { var += (v - avg) * (v - avg); if (v > 1.5 * avg) ++slow; }
                std::vector<double> sorted = intervals; std::sort(sorted.begin(), sorted.end());
                std::printf("[frametime] frames %zu: avg %.2f ms (%.1f fps), min %.2f, max %.2f, p99 %.2f, stddev %.2f ms (%.0f%% of avg), frames > 1.5x avg: %.1f%%\n",
                            intervals.size(), avg, 1000.0 / avg, mn, mx, sorted[(size_t) (sorted.size() * 0.99)], std::sqrt(var / intervals.size()),
                            100.0 * std::sqrt(var / intervals.size()) / avg, 100.0 * slow / intervals.size());
            }
        }

        if (measureEvery > 0 && frame > 0 && frame % measureEvery == 0) measure(frame);
        for (int d : dumpFrames) if (frame == d) dump(frame);
        const HRESULT removed = dev->GetDeviceRemovedReason();
        if (FAILED(removed)) { std::printf("[fail] frame %d: D3D11 device removed 0x%08lX\n", frame, (unsigned long) removed); std::fflush(stdout); TerminateProcess(GetCurrentProcess(), 2); }
        if (frame % 120 == 0) std::printf("[info] frame %d ok\n", frame);
    }
    if (measurements > 0)
        std::printf("[measure] average over %d samples: all=%.5f center=%.5f periphery=%.5f\n", measurements,
                    sumAll / measurements, sumCenter / measurements, sumPeriphery / measurements);
    int verdict = reference ? 0 : VerdictFromLogs();
    if (skinFrames > 0) std::printf("[info] glTF deform (morphs + CPU skinning + upload prep): %.2f ms/frame over %d frames, %zu vertices\n", skinMsTotal / skinFrames, skinFrames, gVerts.size());
    if (verdict == 0) std::printf("[info] bench finished after %d frames (%d layout switches), device ok\n", frame, switches);
    else std::printf("[fail] bench finished after %d frames (%d layout switches) but the chain broke (see above)\n", frame, switches);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), verdict);
    return verdict;
}
