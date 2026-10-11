#pragma once
// --sky-reflections: the CPU side of the sky's image-based specular (RASTER_REFLECTIONS.md). The math is the shaders' own
// (shaders/sky_reflection_math.hlsli through hlsl_compat.h), so the DFG table the scene pass reads, the GPU prefilter and the
// checks (tests_reflections.cpp) share one definition.
#include "hlsl_compat.h"
#include <vector>
namespace hlsl {
#include "shaders/sky_reflection_math.hlsli"
}

// The split sum's DFG table: SkyDfgSize^2 texels of (A, B), n.v = (i + 0.5) / size across, roughness = (j + 0.5) / size down,
// row-major, as the scene pass samples it (uv = (n.v, roughness), clamped).
inline std::vector<float> BuildDfgTable(unsigned size=hlsl::SkyDfgSize, unsigned samples=hlsl::SkyDfgSamples)
{
    std::vector<float> table(size_t(size)*size*2);
    for(unsigned j=0;j<size;++j) for(unsigned i=0;i<size;++i) {
        const hlsl::float2 ab=hlsl::IntegrateDfg((float(i)+0.5f)/float(size),(float(j)+0.5f)/float(size),samples);
        table[(size_t(j)*size+i)*2]=ab.x; table[(size_t(j)*size+i)*2+1]=ab.y;
    }
    return table;
}
