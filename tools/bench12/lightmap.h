#pragma once
#include "device.h"
#include "lamps.h"
#include "options.h"
#include <cstdint>
#include <string>
#include <vector>

// --lightmap: the raster mode's baked lighting of static geometry (RASTER.md, "Baked lighting"). The JSON and its
// Radiance atlases are written by tools/bench/blender/bake_lab_lightmaps.py.

// What the JSON says (lightmap_file.cpp).
struct LightmapBake {
    // File names relative to the JSON's folder; texels per side. rest (optional, all atlases or none): the denoised
    // full - sun, which replaces full on the GPU so static pixels need no subtraction. lamp (optional, all atlases or
    // none, only with rest; bakes from 2026-10-07 on): the lamp groups' direct light, and rest is then full - sun - lamp.
    struct Atlas { std::string full, sun, rest, lamp; unsigned size=0; };
    std::vector<Atlas> atlases;
    Vec3 sunDir{}; float sunStrength=0, sunAngle=0; // the baked sun: travel direction (bench space, as --sun-dir), strength, angular diameter (degrees)
    float scaleFull=1, scaleSun=1; // the two maps' calibration against the path tracer
};
// Throws std::runtime_error naming the lightmap for malformed JSON or a missing or mistyped field.
LightmapBake ParseLightmapJson(const std::string &text);

// The loaded lightmap (lightmap.cpp): atlas i as full[i] and sun[i], R9G9B9E5_SHAREDEXP without mips, readable by pixel
// and non-pixel shaders. full[i] holds the rest map when the JSON has one (rest true). SRVs: full i at srvBase+2i,
// sun i at srvBase+2i+1 (shaders/raster_lightmap.hlsli). A JSON with "lamp" maps: with --lamp-shadows they load as
// lamp[i] (SRV srvBase+2n+i, n atlases); without it full[i] holds rest + lamp, so the light is the bake's full - sun.
struct LightmapSet {
    LightmapBake bake; // scaleFull, scaleSun and the baked sun
    std::vector<ComPtr<ID3D12Resource>> full, sun, lamp;
    bool rest=false;
    unsigned srvBase=0;
};
// sceneAtlases: the atlases the scene's lightmap UVs address (Scene::lightmapAtlases), checked against the JSON before
// any atlas is read. lampMaps (--lamp-shadows): load the "lamp" maps apart; the JSON must list them. Every failure reads
// "Cannot open lightmap <file>: <reason>" (missing file, bad JSON, too few atlases, a wrong atlas size, an invalid Radiance
// file, no "lamp" maps for --lamp-shadows).
LightmapSet LoadLightmap(Device &device, const std::string &jsonPath, unsigned sceneAtlases, bool lampMaps=false);

// Empty when the run's sun matches the baked one within 1e-3 (directions normalised), else one warning line:
// "lightmap baked for sun ...; this run uses ...".
std::string LightmapSunWarning(const LightmapSet &lightmap, const Options &options);
// Empty while every lamp group keeps the baked intensity 1 and white tint, else a warning naming the changed groups.
std::string LightmapLampWarning(const std::vector<LampGroup> &groups);
// One DXGI_FORMAT_R9G9B9E5_SHAREDEXP texel, as DirectXMath's XMStoreFloat3SE: negatives and NaN become 0.
uint32_t PackR9G9B9E5(float r, float g, float b);
