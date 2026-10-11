#pragma once

// --hud: where the game-like HUD sits. The constants are a 1920x1080 layout; every size scales with height/1080.
// Left elements keep their distance to the left edge, right elements to the right edge, so a wider frame moves
// them apart as a game HUD does. Header-only and D3D-free, so the CPU checks use it (tests_raster.cpp).
struct HudRect { float x, y, w, h; };            // pixels, top-left origin
struct HudLayout { HudRect bar, labels[3], icons, minimap; };

inline HudLayout HudLayoutFor(unsigned width, unsigned height)
{
    const float s=float(height)/1080, right=float(width)-1920*s;
    auto left=[&](float x, float y, float w, float h) { return HudRect{x*s,y*s,w*s,h*s}; };
    auto fromRight=[&](float x, float y, float w, float h) { return HudRect{right+x*s,y*s,w*s,h*s}; };
    HudLayout l{};
    l.bar=left(40,40,360,40);
    l.labels[0]=fromRight(1300,40,240,28); l.labels[1]=fromRight(1300,76,240,28); l.labels[2]=fromRight(1300,112,240,28);
    l.icons=fromRight(1640,820,200,200);
    l.minimap=fromRight(1560,740,320,320);
    return l;
}
