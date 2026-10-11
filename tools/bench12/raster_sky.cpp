// --render raster --raster-sky-ambient: the ambient term from the --hdri sky (sky_sh.h, shaders/raster_common.hlsli PW_SKYAMB).
#include "raster.h"
#include "sky_sh.h"
#include <cstring>

void Raster::UseSkyAmbient(Device &d, const Options &o)
{
    RequireCreated();
    // The panorama the path tracer's rays see (Scene loads the same file), projected on the CPU once: the sun is gone from the
    // prepared sky (tools/bench/hdri_prepare.py), so the SH holds the dome and the ground, the same light --hdri gives a trace.
    const HdrImage image=LoadHdr(o.hdri);
    const auto sh=SkyIrradianceSH(image,1);
    float data[9*4]{};
    for(unsigned k=0;k<9;++k) { data[k*4]=sh[k].x; data[k*4+1]=sh[k].y; data[k*4+2]=sh[k].z; }
    auto next=std::make_unique<Sky>();
    next->constants=d.Buffer(256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void *p=nullptr; D3D12_RANGE empty{0,0}; Check(next->constants->Map(0,&empty,&p),"Map raster sky constants");
    std::memcpy(p,data,sizeof(data));
    next->constants->Unmap(0,nullptr);
    sky=std::move(next); // only complete: the Create*Pipelines functions and DrawScene test `sky` alone
    CreateAmbientPipelines(d);
    // The ambient the shading gets (irradiance / pi, before exposure) for an upward, a sideways and a downward normal.
    const float inverse=1/3.14159265f;
    const Vec3 up=EvalSH(sh,{0,1,0})*inverse, side=EvalSH(sh,{1,0,0})*inverse, down=EvalSH(sh,{0,-1,0})*inverse;
    std::printf("[raster] sky ambient: SH9 of %s (%ux%u); ambient up %.2f %.2f %.2f, side %.2f %.2f %.2f, down %.2f %.2f %.2f\n",
                o.hdri.c_str(),image.width,image.height,up.x,up.y,up.z,side.x,side.y,side.z,down.x,down.y,down.z);
}
