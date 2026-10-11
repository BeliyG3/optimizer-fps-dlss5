#ifndef PW_TEMPORAL_LAYOUT_H
#define PW_TEMPORAL_LAYOUT_H
#define PW_TEMPORAL_THREADS_X 16
#define PW_TEMPORAL_THREADS_Y 8
// AccumulatePair's group (temporal_chain_pair.hlsli): its tile and a 2-texel apron are tested into groupshared memory for
// the 5x5 votes, so a larger tile tests fewer apron texels per texel.
#define PW_TEMPORAL_PAIR_THREADS_X 16
#define PW_TEMPORAL_PAIR_THREADS_Y 16
#define PW_TEMPORAL_SRV_COUNT 22
#define PW_TEMPORAL_UAV_COUNT 2
// DebugTemporalCarryChainRule 5/6 (temporal_chain_pair.hlsli). Rule 5: a colour fail restarts the carry chain where at
// least this many of the 25 texels of its 5x5 neighbourhood fail by colour too (a reflection or a light changes a whole
// area; swaying blades fail in specks). Rule 6: last frame's Refine correction of the main chain, averaged over 5x5,
// counts as a coherent disagreement with the game's vectors from this length on (native pixels: Refine's step is at
// most 0.8 of them), and when the mean is at least this share of the mean correction's length.
#define PW_TEMPORAL_COHERENT_VOTES 18
#define PW_TEMPORAL_COHERENT_SHIFT 0.5
#define PW_TEMPORAL_COHERENT_SHARE 0.7

#ifdef __cplusplus
#include <cstdint>
namespace pwtemporalcontract {
// The required passes, then the optional sky field's (temporal_sky_passes.def) and marker's (temporal_marker_passes.def).
enum Pass : std::uint32_t {
#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) k##name,
#include "temporal_passes.def"
#include "temporal_sky_passes.def"
#include "temporal_marker_passes.def"
#undef PW_TEMPORAL_PASS
    kPassCount
};
// Field: a level of the sky field's pyramid, sized by the caller.
enum class Extent { Native, Motion, Low, History, Stats, Flow, Field };
struct Description { const char *name; std::uint32_t reads, outputs; Extent extent; };
inline constexpr Description passes[] = {
#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) {#name, reads, outputs, Extent::extent},
#include "temporal_passes.def"
#include "temporal_sky_passes.def"
#include "temporal_marker_passes.def"
#undef PW_TEMPORAL_PASS
};
static_assert(sizeof(passes) / sizeof(passes[0]) == kPassCount);
struct Size { std::uint32_t width, height; };
inline Size DispatchSize(Pass pass, Size native, Size motion, Size low, Size custom)
{
    switch (passes[pass].extent) {
    case Extent::Native: return native;
    case Extent::Motion: return motion;
    case Extent::Low: return low;
    default: return custom; // history picture/geometry, optical flow session, CPU statistics grid or a sky field level
    }
}
}
#endif
#endif
