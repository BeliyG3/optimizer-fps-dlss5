#pragma once

// Text drawn into the frame before Present, the way a game draws its interface: a quest log in a
// corner, subtitles low in the centre, a column of numbers at the right edge, a small caption in the
// middle. It stays put while the scene moves behind it, so the neural pass and everything that carries
// its contribution between frames meet static, high-contrast edges - where games show artefacts first.

#include <d3d9.h>

namespace bench9 {

class Hud {
public:
    bool Create(IDirect3DDevice9 *dev, int width, int height);
    // Draws the text over whatever is in the back buffer; restores the render states it touches.
    void Draw(IDirect3DDevice9 *dev) const;
    void Release();

private:
    IDirect3DTexture9 *texture_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

} // namespace bench9
