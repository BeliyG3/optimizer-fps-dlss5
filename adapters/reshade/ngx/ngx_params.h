#pragma once

// Helpers over the NGX parameter block of feature 18: the extent keys the model was created with
// and the per-input sub-rects the host describes its textures with.

#include "hook_common.h"

#include <cstdint>

namespace pwhook {

inline const char *kSubrectNames[] = {"Color", "Depth", "MVec", "Output"};

void WriteSizes(void *params, std::uint32_t w, std::uint32_t h);
Subrect ReadSubrect(void *params, const char *name, unsigned int defaultW, unsigned int defaultH);
void WriteSubrect(void *params, const char *name, unsigned int x, unsigned int y, unsigned int w, unsigned int h);

} // namespace pwhook
