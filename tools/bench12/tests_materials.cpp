#include "math.h"
#include "image_mips.h"
#include "texture_cache.h"
#include "shaders/sampling_math.hlsli"
#include "shaders/reservoir.hlsli"
#include "shaders/display_math.hlsli"
#include "display.h"
#include "shaders/specular_guide.hlsli"
#pragma warning(push)
#pragma warning(disable: 4456 4457)
#include "../bench/pw_gltf.h"
#pragma warning(pop)
#include <filesystem>
#include <random>
#include <fstream>
#include <stdexcept>
#include <cstddef>

namespace {
void Require(bool value, const char *message) { if(!value) throw std::runtime_error(message); }
bool Near(float a, float b) { return std::abs(a-b)<0.0001f; }
void LoaderChecks(const std::filesystem::path &directory)
{
    // The D3D11 host reads this exact prefix; all new fields must remain appended.
    struct LegacyVertex { Vec3 pos, normal; float material, localY, stripe, colour[3], uv[2]; int texture; bool alphaMask; float alphaCutoff; };
    static_assert(offsetof(pwgltf::Vertex,texture)==offsetof(LegacyVertex,texture));
    static_assert(offsetof(pwgltf::Vertex,alphaCutoff)==offsetof(LegacyVertex,alphaCutoff));
    pwgltf::Primitive defaults; pwgltf::Vertex defaultVertex{};
    Require(defaults.metallic==0 && defaults.normalTexture==-1 && defaults.metallicRoughnessTexture==-1 &&
        defaultVertex.metallic==0 && defaultVertex.normalTexture==-1 && defaultVertex.metallicRoughnessTexture==-1,"Legacy material defaults");
    std::string json=R"({"asset":{"version":"2.0"},"bufferViews":[{"byteOffset":0,"byteLength":36},{"byteOffset":36,"byteLength":24}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}],
        "images":[{},{}],"textures":[{"source":1},{"source":0}],
        "materials":[{"pbrMetallicRoughness":{"baseColorFactor":[0.2,0.4,0.8,1],"metallicFactor":0.75,"roughnessFactor":0.2,
            "baseColorTexture":{"index":1},"metallicRoughnessTexture":{"index":0}},"normalTexture":{"index":1,"scale":0.5}}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"material":0}]}],"nodes":[{"mesh":0}]})";
    while(json.size()%4) json+=' ';
    const float geometry[]={0,0,0,1,0,0,0,1,0,0,0,1,0,0,1};
    const uint32_t header[]={0x46546c67,2,uint32_t(28+json.size()+sizeof(geometry)),uint32_t(json.size()),0x4e4f534a};
    const uint32_t bin[]={sizeof(geometry),0x004e4942};
    const auto path=directory/"test_material.glb";
    {
        std::ofstream file(path,std::ios::binary); file.write(reinterpret_cast<const char *>(header),sizeof(header));
        file.write(json.data(),std::streamsize(json.size())); file.write(reinterpret_cast<const char *>(bin),sizeof(bin));
        file.write(reinterpret_cast<const char *>(geometry),sizeof(geometry));
    }
    pwgltf::Scene scene; Require(pwgltf::Load(path.string().c_str(),scene),"Synthetic material GLB load");
    const auto &p=scene.meshes[0].primitives[0];
    Require(p.metallic==0.75f && p.metallicRoughnessTexture==1 && p.normalTexture==0 && p.normalScale==0.5f,"glTF factor and image-index mapping");
    std::vector<pwgltf::Vertex> vertices; std::vector<Vec3> previous;
    std::vector<pwgltf::DrawRange> ranges; std::vector<pwgltf::Light> lamps;
    pwgltf::BuildVertices(scene,0,0,vertices,previous,&ranges,&lamps);
    Require(vertices.size()==3 && ranges.size()==1 && lamps.empty(),"Legacy BuildVertices signature/ranges");
    Require(vertices[0].material==-5 && vertices[0].texture==0 && Near(vertices[0].colour[2],0.8f),"Legacy material category/colour unchanged");
    Require(vertices[0].metallic==0.75f && vertices[0].normalTexture==0 && vertices[0].metallicRoughnessTexture==1,"New fields propagated");
    std::filesystem::remove(path);
}
void MipChecks()
{
    const uint8_t pixels[]={0,0,0,255,255,255,255,255};
    auto colour=BuildMips(2,1,4,pixels,true), data=BuildMips(2,1,4,pixels,false);
    Require(colour.size()==2 && colour.back().bytes[0]>=187 && colour.back().bytes[0]<=188,"sRGB mips must average linear light");
    Require(data.back().bytes[0]==128 && data.back().bytes[3]==255,"Data mip channels must stay linear");
    const float odd[]={0,0,0,1,0,0,0,1,3,6,9,1};
    auto hdr=BuildMips(3,1,16,odd,false); float result[4]{};
    std::memcpy(result,hdr.back().bytes.data(),sizeof(result));
    Require(hdr.size()==2 && Near(result[0],1) && Near(result[2],3),"Odd HDR mip includes edge pixel");
    // The 8-bit table path is the generic loop's bit for bit: FNV-1a of every level of a 37x23 noise image (mt19937
    // seed 11), the hashes of the loop before the table (image_mips.cpp, 2026-10-10).
    std::mt19937 noise(11); std::vector<uint8_t> image(37*23*4); for(auto &b:image) b=uint8_t(noise());
    for(int srgb=0;srgb<2;++srgb) {
        unsigned long long h=1469598103934665603ull;
        for(const auto &level:BuildMips(37,23,4,image.data(),srgb!=0)) for(auto v:level.bytes) { h^=v; h*=1099511628211ull; }
        Require(h==(srgb ? 0xCF2A1E516222AB59ull : 0xB9772D23660B1AA8ull),"8-bit mips equal the generic loop's bytes");
    }
    // --texture-cache: a chain written and read back is the same; another key, size or colour flag misses.
    const auto chain=BuildMips(37,23,4,image.data(),true); const std::filesystem::path dir="obj/mipcache_test";
    WriteMipCache(dir,MipCacheKey(image),image.size(),true,chain);
    std::vector<ImageMip> back, untouched;
    Require(ReadMipCache(dir,MipCacheKey(image),image.size(),true,back) && back.size()==chain.size(),"cached mips read back");
    for(size_t k=0;k<chain.size();++k) Require(back[k].width==chain[k].width && back[k].bytes==chain[k].bytes,"cached mips equal");
    Require(!ReadMipCache(dir,MipCacheKey(image),image.size(),false,untouched) && !ReadMipCache(dir,MipCacheKey(image)^1,image.size(),true,untouched) &&
            !ReadMipCache(dir,MipCacheKey(image),image.size()+1,true,untouched) && untouched.empty(),"cache misses on another key");
}
void SamplingChecks()
{
    Require(LampMis(0,1)==0 && Near(LampMis(2,3)+LampMis(3,2),1),"MIS weights partition unity");
    Require(1-LampMis(1,100000)>0.9999f,"Near-mirror emitter hits retain full radiance");
    // Integrate two complementary estimators with deliberately different source
    // and BSDF PDFs. RIS's random normalization must not be mistaken for its PDF.
    const float proposal[]={0.2f,0.3f,0.5f}, bsdf[]={0.6f,0.3f,0.1f}, target[]={0,2,5}, visible[]={0,2,0};
    unsigned rng=819;
    auto random=[&]() { rng=rng*1664525u+1013904223u; return float(rng>>8)*(1.0f/16777216); };
    for(unsigned candidates:{1u,8u,32u}) {
        double sum=0;
        for(unsigned trial=0;trial<100000;++trial) {
            LampReservoir reservoir{}; unsigned selected=0;
            for(unsigned i=0;i<candidates;++i) {
                float u=random(); unsigned index=u<0.2f ? 0u : u<0.5f ? 1u : 2u;
                if(UpdateLampReservoir(reservoir,target[index],proposal[index],random())) selected=index;
            }
            sum+=visible[selected]*LampMis(proposal[selected],bsdf[selected])*LampNormalization(reservoir,candidates);
            float u=random(); unsigned index=u<0.6f ? 0u : u<0.9f ? 1u : 2u;
            sum+=visible[index]*(1-LampMis(proposal[index],bsdf[index]))/bsdf[index];
        }
        Require(std::abs(sum/100000-2)<0.025,"RIS plus BSDF MIS must integrate without double counting");
    }
    Require(Near(MediumTransmittance(0,300),1) && Near(MediumDistance(0,10,0.25f),2.5f),"Disabled medium");
    const float density=0.012f, distance=40;
    for(int i=0;i<100;++i) {
        float u=float(i)/100, t=MediumDistance(density,distance,u);
        Require(t>=0 && t<=distance && Near((1-MediumTransmittance(density,t))/(1-MediumTransmittance(density,distance)),u),"Truncated exponential inverse CDF");
    }
    for(float g:{-0.6f,0.0f,0.6f}) {
        double integral=0;
        for(int i=0;i<20000;++i) integral+=PhaseHG(-1+(float(i)+0.5f)/10000,g)*6.28318530718/10000;
        Require(std::abs(integral-1)<0.0001,"HG phase integrates to unity");
    }
    Require(PhaseHG(1,0.6f)>PhaseHG(-1,0.6f),"HG forward-scattering sign");
    // The raster fog's sun transmittance against the trace's estimator (random distances, the same boundary), at the
    // garden's haze: towards, across and away from the sun. 16 steps within 2.4 % (the worst: across the sun at the boundary); across the sun it is well above the old
    // constant MediumTransmittance(0.008, 300) = 0.091.
    for(float cosine:{0.95f,0.3f,0.0f,-0.5f,-1.0f}) for(float reach:{20.0f,120.0f,300.0f}) {
        double mc=0; unsigned state=12345;
        for(int i=0;i<200000;++i) {
            state=state*1664525u+1013904223u; const float u=float(state>>8)/16777216.0f;
            const float t=MediumDistance(0.008f,reach,u), projected=t*cosine;
            mc+=MediumTransmittance(0.008f,std::max(0.0f,-projected+std::sqrt(std::max(0.0f,300*300-t*t+projected*projected))));
        }
        mc/=200000;
        Require(std::abs(HazeSunTransmittance(0.008f,reach,cosine,300,16)/float(mc)-1)<0.03f,"Raster haze sun transmittance matches the trace");
    }
    Require(HazeSunTransmittance(0.008f,300,0,300,16)>1.3f*MediumTransmittance(0.008f,300),"Haze across the sun is brighter than the old constant");
    Require(SpecularGuideF0(0.2f,1,0.8f)>SpecularGuide(0.2f,1),"Metal F0 reaches RR guide");
    // Auto exposure in stops: towards a darker scene (target scale 2, one stop up) at the darker speed, a brighter one
    // (target 0.5) at the brighter speed; one second at 60 and at 35 frames per second closes the same 1 - exp(-speed).
    float darker=1, brighter=1, darker35=1;
    for(int i=0;i<60;++i) { darker=AdaptExposure(darker,2,1.0f/60,3,1.5f); brighter=AdaptExposure(brighter,0.5f,1.0f/60,3,1.5f); }
    for(int i=0;i<35;++i) darker35=AdaptExposure(darker35,2,1.0f/35,3,1.5f);
    Require(std::abs(std::log2(darker)-(1-std::exp(-1.5f)))<0.0005f,"Exposure one-second response towards a darker scene");
    Require(std::abs(std::log2(brighter)+(1-std::exp(-3.0f)))<0.0005f,"Exposure one-second response towards a brighter scene");
    Require(std::abs(darker35-darker)<0.0005f,"Exposure response does not depend on the frame rate");
    float settle=1; int frames=0; // 90 % of a 4-stop step outdoors (the scale falls) at 35 fps
    while(std::log2(settle)>-3.6f && frames<1000) { settle=AdaptExposure(settle,1.0f/16,1.0f/35,3,1.5f); ++frames; }
    Require(frames/35.0f>0.7f && frames/35.0f<0.85f,"Exposure settles to 90 % in about 0.8 s towards a brighter scene");
    Require(ExposureSeconds(true,1)==0.1f && ExposureSeconds(true,-1)==0 && ExposureSeconds(false,1)==1.0f/60,"Exposure seconds");
    // Metering: the log-average to 0.18 unless the highlight percentile would pass HIGHLIGHT_KEY; ev on top.
    unsigned bins[METER_BINS]={};
    bins[MeterBin(0.18f)]=80; bins[MeterBin(8.0f)]=20; // 20 % of the weight at 8 (log2 3): the 90th percentile in that bin
    const float p90=MeterPercentile(bins,0.9f);
    Require(p90>3 && p90<3+METER_STEP && MeterBin(1e-40f)==0 && MeterBin(1e30f)==METER_BINS-1,"Metering histogram bins and percentile");
    Require(Near(MeterTarget(std::log(0.18f),-10,1),1) && Near(MeterTarget(std::log(0.18f),-10,2),2),"Average-based target and compensation");
    Require(Near(MeterTarget(std::log(0.18f),3,1),HIGHLIGHT_KEY/8),"Highlights cap the target");
    Require(Near(AdaptExposure(1,1,1,3,1.5f),1) && Filmic(0)==0 && Filmic(1)>Filmic(0.18f),"Exposure midpoint and filmic ordering");
    // The firefly clamp follows the auto exposure. Garden picnic view, --exposure 1.45: sunlit sand writes a luminance of
    // about 125 and the settled scale is 0.00225, so it is 0.28 on screen. It must not be clamped (the old fixed limit 50
    // flattened every sunlit surface to one luminance); a firefly 50 on screen still is.
    const ExposureReading garden{0.00225f,0.0064f,0.00225f,true}, none{};
    Require(FireflyLimit(50,false,garden)==50 && FireflyLimit(50,false,none)==50,"Firefly limit without auto exposure is --firefly");
    Require(Near(FireflyLimit(50,true,garden),50/0.00225f) && FireflyLimit(50,true,garden)>125 && 60/0.00225f>FireflyLimit(50,true,garden),
            "Firefly limit under auto exposure is --firefly on screen");
    Require(FireflyLimit(50,true,none)>1e37f && FireflyLimit(50,true,{0,1,1,true})>1e37f,"No firefly limit before the first exposure reading");
}
}
void MaterialChecks(const std::filesystem::path &directory) { LoaderChecks(directory); MipChecks(); SamplingChecks(); }
