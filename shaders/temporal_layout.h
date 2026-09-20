#ifndef PW_TEMPORAL_LAYOUT_H
#define PW_TEMPORAL_LAYOUT_H
#define PW_TEMPORAL_THREADS_X 16
#define PW_TEMPORAL_THREADS_Y 8
#define PW_TEMPORAL_SRV_COUNT 20
#define PW_TEMPORAL_UAV_COUNT 2

#ifdef __cplusplus
#include <cstdint>
namespace pwtemporalcontract {
enum Pass : std::uint32_t {
#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) k##name,
#include "temporal_passes.def"
#undef PW_TEMPORAL_PASS
    kPassCount
};
enum class Extent { Native, Motion, Low, History, Stats, Flow };
struct Description { const char *name; std::uint32_t reads, outputs; Extent extent; };
inline constexpr Description passes[] = {
#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) {#name, reads, outputs, Extent::extent},
#include "temporal_passes.def"
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
    default: return custom; // history picture/geometry, optical flow session or CPU statistics grid
    }
}
}
#endif
#endif
