// --hud: a game-like HUD alpha-blended into the render colour (u0, the trace's outputs[0]) before NGX/NR.
// Colours are in the colour's scene-linear units (after exposure): "white" 4.0 reads near-white after the default
// tonemap, the dark panels are 0.02. One thread per pixel; pixels outside every HUD rect leave at once.
cbuffer HudFrame : register(b0) // hud.h, Hud::Constants
{
    float4 bar; float4 labels[3]; float4 icons; float4 minimap; // x, y, w, h in pixels, top-left origin (hud_layout.h)
    float scale; uint labelVisible, width, height;               // scale = height/1080
};
RWTexture2D<float4> colour : register(u0);

static const float3 White=4.0, Panel=0.02;
static const float PanelAlpha=0.6, RingAlpha=0.5;

// A 5x7 bitmap font with only the letters of the three labels. One row per entry, top row first, bit 4 = left column.
static const uint Space=14;
static const uint GlyphRows[14*7]={
    0x0E,0x11,0x11,0x1F,0x11,0x11,0x11, //  0 A
    0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E, //  1 B
    0x0E,0x11,0x10,0x10,0x10,0x11,0x0E, //  2 C
    0x1E,0x11,0x11,0x11,0x11,0x11,0x1E, //  3 D
    0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F, //  4 E
    0x0E,0x11,0x10,0x17,0x11,0x11,0x0F, //  5 G
    0x0E,0x04,0x04,0x04,0x04,0x04,0x0E, //  6 I
    0x11,0x12,0x14,0x18,0x14,0x12,0x11, //  7 K
    0x10,0x10,0x10,0x10,0x10,0x10,0x1F, //  8 L
    0x11,0x19,0x15,0x13,0x11,0x11,0x11, //  9 N
    0x0E,0x11,0x11,0x11,0x11,0x11,0x0E, // 10 O
    0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E, // 11 S
    0x1F,0x04,0x04,0x04,0x04,0x04,0x04, // 12 T
    0x11,0x11,0x11,0x11,0x11,0x0A,0x04  // 13 V
};
// "EAGLE VISION", "ATTACK", "BLEND" in glyph indices, padded with Space.
static const uint Text[3][12]={
    {4,0,5,8,4,Space,13,6,11,6,10,9},
    {0,12,12,0,2,7,Space,Space,Space,Space,Space,Space},
    {1,8,4,9,3,Space,Space,Space,Space,Space,Space,Space}
};

bool Inside(float2 p, float4 r) { return all(p>=r.xy) && all(p<r.xy+r.zw); }
float3 Over(float3 c, float3 layer, float alpha) { return lerp(c,layer,alpha); }

float3 Bar(float3 c, float2 p)
{
    c=Over(c,Panel,PanelAlpha);
    // The fill: 72 % of the bar, 5 px inside the panel, opaque.
    const float inset=5*scale;
    const float4 fill=float4(bar.xy+inset,(bar.z-2*inset)*0.72,bar.w-2*inset);
    return Inside(p,fill) ? White : c;
}
float3 Label(float3 c, float2 p, uint label)
{
    const float4 r=labels[label];
    c=Over(c,Panel,PanelAlpha);
    // 3-px font texels at 1080p, 6 texels per character, the text 6 px from the left edge and centred vertically.
    const float texel=3*scale;
    const float2 origin=r.xy+float2(6*scale,0.5*(r.w-7*texel));
    const int2 t=int2(floor((p-origin)/texel));
    if(t.x<0 || t.y<0 || t.y>6) return c;
    const uint character=uint(t.x)/6, column=uint(t.x)%6;
    if(character>=12 || column>4) return c;
    const uint glyph=Text[label][character];
    if(glyph==Space) return c;
    return ((GlyphRows[glyph*7+uint(t.y)]>>(4-column))&1)!=0 ? White : c;
}
float3 Ring(float3 c, float2 p) // the minimap: a 6-px ring inscribed in its rect
{
    const float radius=0.5*minimap.z, r=length(p-(minimap.xy+0.5*minimap.zw));
    return r<radius && r>=radius-6*scale ? Over(c,White,RingAlpha) : c;
}
float3 Icons(float3 c, float2 p) // three opaque diamonds in a triangle
{
    const float2 centres[3]={float2(0.25,0.35),float2(0.75,0.35),float2(0.5,0.75)};
    const float size=0.14*icons.z; // centre to corner
    for(uint i=0;i<3;++i) {
        const float2 d=abs(p-(icons.xy+centres[i]*icons.zw));
        if(d.x+d.y<=size) return White;
    }
    return c;
}

[numthreads(8,8,1)]
void Hud(uint3 dispatch : SV_DispatchThreadID)
{
    const uint2 pixel=dispatch.xy; if(pixel.x>=width || pixel.y>=height) return;
    const float2 p=float2(pixel)+0.5;
    int label=-1;
    for(uint i=0;i<3;++i) if(Inside(p,labels[i]) && (i<2 || labelVisible!=0)) label=int(i);
    const bool inBar=Inside(p,bar), inMap=Inside(p,minimap), inIcons=Inside(p,icons);
    if(!inBar && label<0 && !inMap && !inIcons) return;
    const float4 before=colour[pixel];
    float3 c=before.rgb;
    if(inBar) c=Bar(c,p);
    if(label>=0) c=Label(c,p,uint(label));
    if(inMap) c=Ring(c,p);
    if(inIcons) c=Icons(c,p);
    colour[pixel]=float4(c,before.a);
}
