// The foliage class of a triangle, from its node and material names: --view vegmask shows it, and the foliage metrics
// split their errors by it. Stored in TriMaterial::flags bits 16..18 (scene.cpp); the shaders read (flags >> 16) & 7.
#pragma once
#include "wind.h"
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

enum class VegClass : uint32_t { None=0, Grass=1, Leaves=2, Fern=3, Trunk=4 };
constexpr uint32_t VegClassShift=16, VegClassMask=0x7u<<VegClassShift;

// True when `s` contains the lower-case `needle`, ignoring the case of `s`.
inline bool ContainsLower(std::string_view s, std::string_view needle)
{
    for(size_t i=0;i+needle.size()<=s.size();++i) {
        size_t k=0; while(k<needle.size() && char(std::tolower(static_cast<unsigned char>(s[i+k])))==needle[k]) ++k;
        if(k==needle.size()) return true;
    }
    return false;
}
// The nodes --wind moves (IsWindNode's case-sensitive "veg_" prefix); the trunk by IsTrunkMaterial; grass and fern by
// the node name (as the wind's grass rule); every other veg_ triangle is leaves.
inline VegClass VegClassOf(std::string_view node, std::string_view material)
{
    if(!node.starts_with("veg_")) return VegClass::None;
    if(IsTrunkMaterial(std::string(material))) return VegClass::Trunk;
    if(ContainsLower(node,"grass")) return VegClass::Grass;
    if(ContainsLower(node,"fern")) return VegClass::Fern;
    return VegClass::Leaves;
}
// `flags` with its class bits set to `c`, the other bits kept.
constexpr uint32_t WithVegClass(uint32_t flags, VegClass c) { return (flags&~VegClassMask)|(uint32_t(c)<<VegClassShift); }
