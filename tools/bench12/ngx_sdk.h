#pragma once
#include "external/ngx/nvsdk_ngx.h"
#include "external/ngx/nvsdk_ngx_helpers_d3d.h"
// The supplied SDK subset omits helpers_cuda.h. Include the D3D helpers directly and
// suppress only their umbrella include; no CUDA helpers or vendor edits are needed.
#ifndef NVSDK_NGX_HELPERS_H
#define NVSDK_NGX_HELPERS_H
#endif
#include "external/ngx/nvsdk_ngx_helpers_dlssd_d3d.h"
