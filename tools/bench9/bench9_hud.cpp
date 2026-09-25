#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "bench9_hud.h"

#include <algorithm>
#include <vector>

namespace bench9 {
namespace {

struct Block {
    int x, y, w, h;      // panel rectangle in pixels
    int fontPx;          // cell height of the font
    bool panel;          // a translucent dark panel behind the text, as game menus have
    const wchar_t *text; // lines separated by \n
};

// Placed relative to a 1920x1080 frame and scaled to the real size.
const Block kBlocks[] = {
    {24, 110, 430, 190, 15, true,
     L"QUEST LOG\n- Find the missing scout near the old mill\n- Return the amulet to Brother Genitivi\n"
     L"- Speak with the quartermaster (0/3)\n- Optional: clear the cellar of spiders"},
    {1700, 140, 190, 360, 17, true,
     L"STR  24\nDEX  18\nCON  21\nMAG  12\nWIL  15\nCUN  14\n\nHP   482/510\nMP   120/140"},
    {460, 930, 1000, 70, 26, false,
     L"\"The darkspawn are coming. We hold the bridge until dawn, or not at all.\""},
    {860, 520, 200, 40, 13, false, L"Target: Genlock  [lvl 7]"},
    {24, 1010, 600, 50, 13, true, L"Autosave complete   |   Party: Alistair, Morrigan, Leliana   |   Gold 1 204"},
};

} // namespace

bool Hud::Create(IDirect3DDevice9 *dev, int width, int height)
{
    width_ = width;
    height_ = height;
    const float sx = width / 1920.0f;
    const float sy = height / 1080.0f;

    // Render the text with GDI, white on black, then turn brightness into alpha.
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { DeleteDC(dc); return false; }
    HGDIOBJ oldBitmap = SelectObject(dc, dib);
    std::fill_n(static_cast<DWORD *>(bits), static_cast<size_t>(width) * height, 0u);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    std::vector<unsigned char> panel(static_cast<size_t>(width) * height, 0);
    for (const Block &b : kBlocks) {
        RECT r{static_cast<LONG>(b.x * sx), static_cast<LONG>(b.y * sy), static_cast<LONG>((b.x + b.w) * sx),
               static_cast<LONG>((b.y + b.h) * sy)};
        if (b.panel)
            for (LONG y = std::max(0L, r.top); y < std::min(static_cast<LONG>(height), r.bottom); ++y)
                for (LONG x = std::max(0L, r.left); x < std::min(static_cast<LONG>(width), r.right); ++x)
                    panel[static_cast<size_t>(y) * width + x] = 1;
        HFONT font = CreateFontW(-static_cast<int>(b.fontPx * sy), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        RECT inner{r.left + 8, r.top + 6, r.right - 8, r.bottom - 6};
        DrawTextW(dc, b.text, -1, &inner, DT_LEFT | DT_TOP | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        DeleteObject(font);
    }
    GdiFlush();

    if (FAILED(dev->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture_, nullptr))) {
        SelectObject(dc, oldBitmap);
        DeleteObject(dib);
        DeleteDC(dc);
        return false;
    }
    D3DLOCKED_RECT lr{};
    texture_->LockRect(0, &lr, nullptr, 0);
    const auto *src = static_cast<const DWORD *>(bits);
    for (int y = 0; y < height; ++y) {
        auto *row = reinterpret_cast<DWORD *>(static_cast<BYTE *>(lr.pBits) + y * lr.Pitch);
        for (int x = 0; x < width; ++x) {
            const DWORD p = src[static_cast<size_t>(y) * width + x];
            const unsigned m = std::max({(p >> 16) & 255u, (p >> 8) & 255u, p & 255u}); // text coverage
            const unsigned back = panel[static_cast<size_t>(y) * width + x] ? 150u : 0u; // panel opacity
            // White text over a black panel: colour = coverage, alpha = text over panel.
            const unsigned alpha = m + back * (255u - m) / 255u;
            const unsigned colour = alpha == 0 ? 0 : std::min(255u, m * 255u / alpha);
            row[x] = D3DCOLOR_ARGB(alpha, colour, colour, colour);
        }
    }
    texture_->UnlockRect(0);
    SelectObject(dc, oldBitmap);
    DeleteObject(dib);
    DeleteDC(dc);
    return true;
}

void Hud::Draw(IDirect3DDevice9 *dev) const
{
    if (!texture_) return;
    struct V { float x, y, z, rhw, u, v; };
    // Pre-transformed, with Direct3D 9's half-pixel offset so texels land on pixels one to one.
    const float w = static_cast<float>(width_) - 0.5f, h = static_cast<float>(height_) - 0.5f;
    const V quad[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {w, -0.5f, 0, 1, 1, 0}, {-0.5f, h, 0, 1, 0, 1}, {w, h, 0, 1, 1, 1}};
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetTexture(0, texture_);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
}

void Hud::Release()
{
    if (texture_) texture_->Release();
    texture_ = nullptr;
}

} // namespace bench9
