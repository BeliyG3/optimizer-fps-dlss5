#pragma once
#include "animation_source.h"
#include <algorithm>
#include <vector>

// The second UV set (TEXCOORD_1) of a vertex and the lightmap atlas it addresses, for the raster mode's baked lighting.
// A buffer of its own, parallel to Scene::vertices: TriVertex (the path tracer's 44 bytes) does not carry it.
struct LightmapUv { float u, v; float atlas; float padding=0; };
static_assert(sizeof(LightmapUv)==16);

// One entry per vertex of `raw`, taken in the order of `order` (triangle indices, three vertices each: the order Scene
// stores its vertices in, static triangles first), so entry i belongs to vertices[i]. Empty when no vertex has TEXCOORD_1.
// Dynamic (animated, skinned) triangles are not baked: atlas -1, as for a static primitive without TEXCOORD_1.
inline std::vector<LightmapUv> BuildLightmapUvs(const std::vector<pwgltf::Vertex> &raw, const std::vector<unsigned> &order, const std::vector<bool> &dynamic)
{
    std::vector<LightmapUv> result;
    if(std::none_of(raw.begin(),raw.end(),[](const pwgltf::Vertex &v) { return v.lightmapAtlas>=0; })) return result;
    result.reserve(order.size()*3);
    for(unsigned t:order) for(unsigned k=0;k<3;++k) {
        const auto &r=raw[size_t(t)*3+k];
        if(dynamic[t] || r.lightmapAtlas<0) result.push_back({0,0,-1}); else result.push_back({r.uv2[0],r.uv2[1],float(r.lightmapAtlas)});
    }
    return result;
}
// The triangles (in the order of `vertices`, three entries of `lightmap` each) that take no baked light: dynamic ones and
// static ones without TEXCOORD_1 (the garden's veg_ foliage without --wind). They are not in the bake, so the baked mode's
// dynamic-only shadow maps draw them all (Scene::unbakedTriangles).
inline std::vector<unsigned> UnbakedTriangles(const std::vector<LightmapUv> &lightmap)
{
    std::vector<unsigned> result;
    for(size_t t=0;t*3<lightmap.size();++t) if(lightmap[t*3].atlas<0) result.push_back(unsigned(t));
    return result;
}
