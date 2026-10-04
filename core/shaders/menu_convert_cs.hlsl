// Menu mode: the model pass's copies between the back buffer and the host's NR colour/output. The menu frame goes to
// NR as it is (owner ruling 2026-09-29): the SRV/UAV views convert the storage format (for example RGBA8 UNORM to
// RGBA16F and back); the values are copied unchanged.
Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

[numthreads(8, 8, 1)]
void CSConvert(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    Dst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    Dst[id.xy] = Src.Load(int3(id.xy, 0));
}
