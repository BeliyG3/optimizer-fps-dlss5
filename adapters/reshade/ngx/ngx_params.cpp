#include "ngx_params.h"

#include <cstdio>

namespace pwhook {

struct SizeKeys {
    const char *w;
    const char *h;
};

const SizeKeys kSizeKeys[] = {{"DLSSNR.Width", "DLSSNR.Height"},
                              {"DLSSNR.InputWidth", "DLSSNR.InputHeight"},
                              {"DLSSNR.OutputWidth", "DLSSNR.OutputHeight"}};

void WriteSizes(void *params, std::uint32_t w, std::uint32_t h)
{
    for (const auto &k : kSizeKeys) {
        unsigned int probe = 0;
        if (GetUInt(params, k.w, &probe)) SetUInt(params, k.w, w);
        if (GetUInt(params, k.h, &probe)) SetUInt(params, k.h, h);
    }
}

Subrect ReadSubrect(void *params, const char *name, unsigned int defaultW, unsigned int defaultH)
{
    char key[64];
    Subrect r;
    r.w = defaultW;
    r.h = defaultH;
    std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectWidth", name);
    if (GetUInt(params, key, &r.w) && r.w != 0) {
        r.present = true;
        std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectHeight", name);
        GetUInt(params, key, &r.h);
        if (r.h == 0) r.h = defaultH; // the shaders divide by the rect
        std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectBaseX", name);
        GetUInt(params, key, &r.x);
        std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectBaseY", name);
        GetUInt(params, key, &r.y);
    } else {
        r.w = defaultW;
        r.h = defaultH;
    }
    return r;
}

void WriteSubrect(void *params, const char *name, unsigned int x, unsigned int y, unsigned int w, unsigned int h)
{
    char key[64];
    std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectBaseX", name); SetUInt(params, key, x);
    std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectBaseY", name); SetUInt(params, key, y);
    std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectWidth", name); SetUInt(params, key, w);
    std::snprintf(key, sizeof(key), "DLSSNR.%sSubrectHeight", name); SetUInt(params, key, h);
}

} // namespace pwhook
