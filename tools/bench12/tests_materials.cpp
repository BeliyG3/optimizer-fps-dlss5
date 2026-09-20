#include "math.h"
#include "image_mips.h"
#include "shaders/sampling_math.hlsli"
#include "shaders/reservoir.hlsli"
#include "shaders/display_math.hlsli"
#include "shaders/specular_guide.hlsli"
#pragma warning(push)
#pragma warning(disable: 4456 4457)
#include "../bench/pw_gltf.h"
#pragma warning(pop)
#include <filesystem>
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
    Require(SpecularGuideF0(0.2f,1,0.8f)>SpecularGuide(0.2f,1),"Metal F0 reaches RR guide");
    float adapted=1;
    for(int i=0;i<60;++i) adapted=AdaptExposure(adapted,std::log(0.09f),1.0f/60);
    Require(std::abs(adapted-(2-std::exp(-1.0f)))<0.0001f,"Exposure one-second response");
    Require(Near(AdaptExposure(1,std::log(0.18f),1),1) && Filmic(0)==0 && Filmic(1)>Filmic(0.18f),"Exposure midpoint and filmic ordering");
}
}
void MaterialChecks(const std::filesystem::path &directory) { LoaderChecks(directory); MipChecks(); SamplingChecks(); }
