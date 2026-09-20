#include "scene.h"
#include <thread>
#include <chrono>
#include "image.h"
#include "animation.h"
#include <numeric>

Scene::Scene(Device &d, const Options &o)
{
    animation=std::make_unique<Animation>(); auto &source=animation->source;
    const auto t0=std::chrono::steady_clock::now(); auto lap=[&](const char *what) { std::printf("[time] %s: %.1f s\n",what,std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count()); };
    if(!pwgltf::Load(o.gltf.c_str(),source)) throw std::runtime_error("glTF: "+source.error+" ("+o.gltf+")");
    lap("glb parsed");
    std::vector<pwgltf::Vertex> raw; std::vector<Vec3> previous;
    std::vector<pwgltf::DrawRange> ranges; std::vector<pwgltf::Light> sourceLights;
    pwgltf::BuildVertices(source,0,0,raw,previous,&ranges,&sourceLights);
    lap("vertices built");
    if(raw.empty() || raw.size()%3 || raw.size()>UINT_MAX) throw std::runtime_error("glTF has no valid triangle list or is too large");
    triangleCount=unsigned(raw.size()/3);
    animationDuration=source.animationEnd; animationTime=LoopAnimation(o.animStart,animationDuration);
    for(size_t i=0;i<source.nodes.size();++i) {
        const auto &name=source.nodes[i].name;
        if(o.followNode.empty() ? (name.find("hero")!=std::string::npos || name.find("girl")!=std::string::npos) : name==o.followNode) {
            followNode=int(i); break;
        }
    }
    if(!o.followNode.empty() && followNode<0) throw std::runtime_error("Follow node not found: "+o.followNode);
    for(size_t i=0;i<source.nodes.size();++i) {
        const auto &n=source.nodes[i];
        if(n.mesh>=0 && (n.name.find("girl")!=std::string::npos || n.name.find("haracter")!=std::string::npos)) {
            Vec3 lo{},hi{};
            if(pwgltf::MeshBounds(source,0,int(i),lo,hi) && lo.x<hi.x) {
                cameraAnchor={(lo.x+hi.x)*0.5f,0,(lo.z+hi.z)*0.5f};
                std::printf("[info] scripted camera follows %s at (%.2f, %.2f)\n",n.name.c_str(),cameraAnchor.x,cameraAnchor.z);
            }
            break;
        }
    }
    // BuildVertices deliberately exposes only a material category. Preserve exact roughness from
    // its primitive traversal before reordering triangles for the two BLAS geometries.
    std::vector<float> roughness;
    std::vector<std::string> materialNames;
    std::vector<bool> dynamic;
    std::vector<unsigned> dynamicIndex;
    unsigned dynamicOffset=0;
    for(size_t node=0;node<source.nodes.size();++node) {
        const auto &n=source.nodes[node];
        if(n.mesh<0 || size_t(n.mesh)>=source.meshes.size()) continue;
        const bool moving=pwgltf::DynamicNode(source,node);
        for(const auto &p:source.meshes[n.mesh].primitives) {
            size_t valid=0; for(auto index:p.indices) if(index<p.positions.size()) ++valid;
            if(valid%3) throw std::runtime_error("Invalid glTF primitive triangle indices");
            roughness.insert(roughness.end(),valid/3,std::clamp(p.roughness,0.0f,1.0f));
            materialNames.insert(materialNames.end(),valid/3,p.materialName);
            dynamic.insert(dynamic.end(),valid/3,moving);
            for(size_t k=0;k<valid/3;++k) dynamicIndex.push_back(moving ? dynamicOffset++ : 0);
        }
    }
    if(roughness.size()!=triangleCount) throw std::runtime_error("glTF material traversal mismatch");
    std::vector<unsigned> order(triangleCount); std::iota(order.begin(),order.end(),0u);
    auto boundary=std::stable_partition(order.begin(),order.end(),[&](unsigned t){ return !dynamic[t]; });
    staticTriangles=unsigned(boundary-order.begin()); dynamicTriangles=triangleCount-staticTriangles;
    auto opaque=[&](unsigned t){ return !raw[size_t(t)*3].alphaMask; };
    auto split=std::stable_partition(order.begin(),boundary,opaque);
    opaqueTriangles=unsigned(split-order.begin());
    dynamicOpaqueTriangles=unsigned(std::stable_partition(boundary,order.end(),opaque)-boundary);
    std::vector<TriVertex> v; std::vector<TriMaterial> m; std::vector<EmissiveTriangle> emitters;
    std::vector<int> dataIndices(source.images.size(),-1); std::vector<unsigned> dataImages;
    auto dataSlot=[&](int image) {
        if(image<0) return -1;
        if(dataIndices[size_t(image)]<0) {
            dataIndices[size_t(image)]=int(source.images.size()+dataImages.size()); dataImages.push_back(unsigned(image));
        }
        return dataIndices[size_t(image)];
    };
    v.reserve(raw.size()); m.reserve(triangleCount);
    for(unsigned t:order) {
        const auto &a=raw[size_t(t)*3]; TriMaterial mat{}; mat.lampGroup=NoLamp;
        mat.albedo={a.colour[0],a.colour[1],a.colour[2]}; mat.roughness=roughness[t];
        mat.texture=a.texture; mat.alphaCutoff=a.alphaCutoff; mat.flags=a.alphaMask ? 1u : 0u;
        // Allocate UNORM copies only for data slots. Reusing a colour image as data
        // must not inherit its SRGB decoding or its linear-light colour mip chain.
        mat.metallicRoughnessTexture=dataSlot(a.metallicRoughnessTexture);
        mat.normalTexture=dataSlot(a.normalTexture);
        mat.metallic=std::clamp(a.metallic,0.0f,1.0f); mat.normalScale=a.normalScale;
        if(a.material==-4) {
            mat.emission=mat.albedo; mat.albedo={0,0,0}; mat.flags|=2;
            if(!dynamic[t]) mat.lampGroup=FindLampGroup(groups,materialNames[t]);
        }
        if(a.material==-5) mat.flags|=4;
        unsigned triangle=unsigned(m.size()); m.push_back(mat);
        if(dynamic[t]) dynamicOrder.push_back(dynamicIndex[t]);
        for(unsigned k=0;k<3;++k) { const auto &r=raw[size_t(t)*3+k]; v.push_back({r.pos,r.pos,r.normal,{r.uv[0],r.uv[1]}}); }
        Vec3 e1=v[size_t(triangle)*3+1].pos-a.pos, e2=v[size_t(triangle)*3+2].pos-a.pos;
        float area=0.5f*std::sqrt(Dot(Cross(e1,e2),Cross(e1,e2)));
        float power=area*Dot(mat.emission,{0.2126f,0.7152f,0.0722f});
        if(power>0 && !dynamic[t]) lampSources.push_back({triangle,mat.lampGroup,area,mat.emission});
    }
    lightPower=RebuildLampCdf(lampSources,groups,emitters);
    lightCount=unsigned(emitters.size());
    if(!lightCount) emitters.push_back({});
    textureCount=unsigned(source.images.size()+dataImages.size());
    srvBase=d.Allocate(4+std::max(1u,textureCount));
    vertices=d.UploadBuffer(v.data(),v.size()*sizeof(TriVertex)); materials=d.UploadBuffer(m.data(),m.size()*sizeof(TriMaterial));
    lights=d.UploadBuffer(emitters.data(),emitters.size()*sizeof(EmissiveTriangle));
    d.BufferSrv(vertices.Get(),srvBase,unsigned(v.size()),sizeof(TriVertex));
    if(dynamicTriangles) {

        animationUpload=d.Buffer(UINT64(dynamicTriangles)*3*sizeof(TriVertex),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void *mapped=nullptr; D3D12_RANGE empty{0,0}; Check(animationUpload->Map(0,&empty,&mapped),"Map animation upload");
        mappedAnimation=static_cast<TriVertex *>(mapped);
        animation->Start(mappedAnimation,dynamicOrder);
        Animate(animationTime,animationTime);
        d.Begin(); CopyAnimation(d); d.Submit();
    }
    d.BufferSrv(materials.Get(),srvBase+1,unsigned(m.size()),sizeof(TriMaterial));
    d.BufferSrv(lights.Get(),srvBase+2,unsigned(emitters.size()),sizeof(EmissiveTriangle));
    HdrImage hdr;
    if(!o.hdri.empty()) hdr=LoadHdr(o.hdri);
    else { hdr.width=hdr.height=1; hdr.pixels={0.12f,0.16f,0.23f,1}; }
    environment=d.UploadTexture(hdr.width,hdr.height,DXGI_FORMAT_R32G32B32A32_FLOAT,16,hdr.pixels.data());
    lap("buffers uploaded");
    d.TextureSrv(environment.Get(),srvBase+3);
    // Decoding 190 JPEG/PNG images one after another was most of a 20 s start: decode a batch on all
    // cores, upload it, drop it (a batch bounds the memory: a 4k image is 64 MB decoded).
    const unsigned count=std::max(1u,textureCount), workers=std::max(2u,std::thread::hardware_concurrency());
    for(unsigned first=0;first<count;first+=workers) {
        const unsigned batch=std::min(workers,count-first);
        std::vector<std::vector<ImageMip>> images(batch); std::vector<std::string> failures(batch); std::vector<std::thread> threads;
        for(unsigned k=0;k<batch;++k) threads.emplace_back([&,k]() {
            const unsigned i=first+k;
            try {
                RgbaImage image;
                if(textureCount) image=DecodeImage(source.images[i<source.images.size() ? i : dataImages[i-source.images.size()]].bytes);
                else { image.width=image.height=1; image.pixels={255,255,255,255}; }
                images[k]=BuildMips(image.width,image.height,4,image.pixels.data(),i<source.images.size());
            } catch(const std::exception &error) { failures[k]=error.what(); }
        });
        for(auto &thread:threads) thread.join();
        for(unsigned k=0;k<batch;++k) {
            if(!failures[k].empty()) throw std::runtime_error(failures[k]);
            const unsigned i=first+k;
            auto format=i<source.images.size() ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            auto texture=d.UploadMips(format,4,images[k]);
            d.TextureSrv(texture.Get(),srvBase+4+i); textures.push_back(texture);
        }
    }
    lap("textures decoded and uploaded");
    UpdateLamps(d);
    std::printf("[info] scene: %u triangles (%u dynamic, %u alpha-masked), %u textures, %u emissive triangles\n",
        triangleCount,dynamicTriangles,triangleCount-opaqueTriangles-dynamicOpaqueTriangles,textureCount,lightCount);
}
