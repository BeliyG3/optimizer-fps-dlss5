#pragma once
#include "math.h"
struct TriVertex { Vec3 pos, prevPos, normal; float uv[2]; };
static_assert(sizeof(TriVertex)==44);
