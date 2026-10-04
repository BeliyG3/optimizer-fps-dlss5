#include "bench_shaders.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------------------------
// Shaders. The flat scene is the historical moving checkerboard; the 3D scene renders a textured
// ground, instanced boxes and a figure with game-style motion vectors (previous unjittered
// view-projection) and hardware reverse-Z depth.
// ---------------------------------------------------------------------------------------------
const char *const kShaderFlat = R"(
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

const char *const kShader3D = R"(
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
    // Interior scenes (--gltf with emissive lamps): the lamps as point lights, and what a game hands DLSS.
    float4 lab;           // x: light count, y: 1 = --hdr (linear radiance out, no tone mapper), z: --boil amplitude, w: 1 = standard depth (far = 1)
    float4 lightPos[24];  // xyz world position
    float4 lightPower[24]; // rgb: radiance x area of the emissive surface (scaled by the exposure)
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
)" R"(
struct PSOut { float4 color : SV_Target0; float2 motion : SV_Target1; };
PSOut PS(VSOut i) {
    PSOut o;
    float3 albedo;
    if (i.material < -1.5) {
        // glTF mesh: the material's base colour, optionally with the 0.2 m stripes by local height.
        float stripe = (i.material < -2.5 && i.material > -3.5) ? (fmod(abs(floor(i.localY * 5.0)), 2.0) < 1.0 ? 1.0 : 0.55) : 1.0;
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
    if (i.material < -1.5 && !(i.material < -3.5 && i.material > -4.5)) albedo *= hdriParams.z; // --albedo: a dim interior's surfaces (the exporter drops a texture's darkening tint)
    ambient *= hdriParams.y; // --ambient: an interior lit by its lamps wants little of the environment
    float3 radiance = albedo * (ndl * ShadowFactor(i.world, n, ndl, i.pos.xy) * sun + ambient);
    // Lamps: every emissive surface of the scene is a point light (inverse square, no shadows). A
    // polished floor (material -5) also mirrors them as a tight highlight. The highlight slides over the
    // floor with the view while the floor's motion vectors describe the floor: what a game's reflections
    // do to anything that trusts the vectors.
    const bool emissive = i.material < -3.5 && i.material > -4.5;
    const bool glossy = i.material < -4.5 && i.material > -5.5;
    const float3 V = normalize(eyePos.xyz - i.world);
    [loop] for (int li = 0; li < (int) lab.x; ++li) {
        float3 toLight = lightPos[li].xyz - i.world;
        float d2 = max(dot(toLight, toLight), 0.04);
        float3 Ll = toLight * rsqrt(d2);
        float3 E = lightPower[li].rgb / d2; // irradiance / pi, like the sun term
        radiance += albedo * saturate(dot(n, Ll)) * E;
        float3 H = normalize(Ll + V);
        float gloss = glossy ? 900.0 : 24.0;
        float strength = glossy ? 0.25 : 0.04;
        radiance += strength * (gloss + 8.0) * 0.0398 * pow(saturate(dot(n, H)), gloss) * saturate(dot(n, Ll)) * E;
    }
    if (emissive) radiance = albedo * sunColor.w; // the lamp itself: its radiance, tens of units (a screen: times its picture)
    // --boil: what is left of path-tracing noise after the denoiser - the dim parts of the frame change a
    // little every frame (a hash of pixel and frame; relative, so the lamps stay clean).
    if (lab.z > 0.0) {
        float h = frac(sin(dot(floor(i.pos.xy), float2(12.9898, 78.233)) + misc.y * 61.7) * 43758.5453);
        radiance *= 1.0 + lab.z * (h - 0.5) * 2.0 * saturate(1.0 - dot(radiance, float3(0.2126, 0.7152, 0.0722)));
    }
    // The --hdri none path keeps its historical 0..~1.15 look; the HDRI path is tone mapped unless the
    // frame is asked for as a game hands it to the upscaler: linear radiance (--hdr).
    o.color = float4((hdriLit && lab.y < 0.5) ? PBRNeutralToneMapping(radiance) : radiance, 1);
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
    float4 h = mul(invVp, float4(ndc, lab.w > 0.5 ? 0.0 : 1.0, 1)); // the near plane: z = 1 in reverse-Z, 0 in standard depth
    float3 dir = normalize(h.xyz / h.w - eyePos.xyz);
    float3 sky = hdri.SampleLevel(hdriSmp, EquirectUV(dir), 0).rgb * sunColor.w;
    o.color = float4(lab.y < 0.5 ? PBRNeutralToneMapping(sky) : sky, 1);
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

ComPtr<ID3DBlob> Compile(const char *source, const char *entry, const char *target)
{
    ComPtr<ID3DBlob> code, err;
    if (FAILED(D3DCompile(source, strlen(source), "bench", nullptr, nullptr, entry, target, 0, 0, &code, &err))) {
        std::printf("[fail] shader %s: %s\n", entry, err ? (const char *) err->GetBufferPointer() : "?");
        return nullptr;
    }
    return code;
}
