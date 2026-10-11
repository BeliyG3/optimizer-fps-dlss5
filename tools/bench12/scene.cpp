#include "scene.h"
#include <thread>
#include <chrono>
#include "image.h"
#include "animation.h"
#include "gpu_skinning.h"
#include "lightmap_uv.h"
#include "wind.h"
#include "scene_instances.h"
#include <numeric>
#include "startup_profile.h"
#include "texture_load.h"
#include "texture_cache.h"
#include <atomic>

Scene::Scene(Device &d, const Options &o)
{
    animation=std::make_unique<Animation>(); auto &source=animation->source;
    const auto t0=std::chrono::steady_clock::now(); auto lap=[&](const char *what) { StartupMark(what); std::printf("[time] %s: %.1f s\n",what,std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count()); };
    if(!pwgltf::Load(o.gltf.c_str(),source)) throw std::runtime_error("glTF: "+source.error+" ("+o.gltf+")");
    lap("scene: glb parsed");
    if(o.wind) { // veg_ nodes become dynamic: the CPU sways their vertices every frame (wind.h)
        const size_t veg=MarkWindNodes(source);
        if(!veg) std::fputs("[warn] --wind: the scene has no veg_ nodes, wind has no effect\n",stderr);
        else { windActive=o.windStrength!=0; animation->windStrength=o.windStrength; std::printf("[info] wind: %zu veg_ nodes, strength %g\n",veg,double(o.windStrength)); }
    }
    std::vector<pwgltf::Vertex> raw; std::vector<Vec3> previous;
    std::vector<pwgltf::DrawRange> ranges; std::vector<pwgltf::Light> sourceLights;
    // Meshes shared by static nodes are stored once (scene_instances.h); the rest is flattened into world space.
    const InstancePlan plan=PlanInstances(source,o.instancing && o.lightmap.empty());
    std::vector<pwgltf::PrimitiveRef> flattened;
    for(const auto &primitive:pwgltf::Primitives(source)) if(!plan.instanced[primitive.node]) flattened.push_back(primitive);
    pwgltf::VertexWorkspace scratch;
    pwgltf::BuildVerticesSubset(source,0,0,flattened,scratch,raw,previous,&ranges,&sourceLights);
    lap("scene: vertices built");
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
    // wind: the triangle's node sways with --wind (TriMaterial flag 8, the probes' sky visibility); vegClass: flags bits 16..18.
    const FlatTriangles flat=CollectFlatTriangles(source,plan);
    const auto &[roughness,materialNames,dynamic,wind,dynamicIndex,vegClass]=flat;
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
    auto material=[&](const pwgltf::Vertex &a, float rough) {
        TriMaterial mat{}; mat.lampGroup=NoLamp;
        mat.albedo={a.colour[0],a.colour[1],a.colour[2]}; mat.roughness=rough;
        mat.texture=a.texture; mat.alphaCutoff=a.alphaCutoff; mat.flags=a.alphaMask ? 1u : 0u;
        // Allocate UNORM copies only for data slots. Reusing a colour image as data
        // must not inherit its SRGB decoding or its linear-light colour mip chain.
        mat.metallicRoughnessTexture=dataSlot(a.metallicRoughnessTexture);
        mat.normalTexture=dataSlot(a.normalTexture);
        mat.metallic=std::clamp(a.metallic,0.0f,1.0f); mat.normalScale=a.normalScale;
        if(a.material==-4) { mat.emission=mat.albedo; mat.albedo={0,0,0}; mat.flags|=2; }
        if(a.material==-5) mat.flags|=4;
        return mat;
    };
    const InstancedGeometry instanced=BuildInstancedGeometry(source,plan,triangleCount);
    v.reserve(raw.size()+instanced.vertices.size()); m.reserve(triangleCount+instanced.roughness.size());
    for(unsigned t:order) {
        const auto &a=raw[size_t(t)*3]; TriMaterial mat=material(a,roughness[t]);
        if(a.material==-4 && !dynamic[t]) mat.lampGroup=FindLampGroup(groups,materialNames[t]);
        if(wind[t]) mat.flags|=8;
        mat.flags=WithVegClass(mat.flags,vegClass[t]);
        unsigned triangle=unsigned(m.size()); m.push_back(mat);
        if(dynamic[t]) dynamicOrder.push_back(dynamicIndex[t]);
        for(unsigned k=0;k<3;++k) { const auto &r=raw[size_t(t)*3+k]; v.push_back({r.pos,r.pos,r.normal,{r.uv[0],r.uv[1]}}); }
        Vec3 e1=v[size_t(triangle)*3+1].pos-a.pos, e2=v[size_t(triangle)*3+2].pos-a.pos;
        float area=0.5f*std::sqrt(Dot(Cross(e1,e2),Cross(e1,e2)));
        float power=area*Dot(mat.emission,{0.2126f,0.7152f,0.0722f});
        if(power>0 && !dynamic[t]) {
            lampSources.push_back({triangle,mat.lampGroup,area,mat.emission});
            lampCentroids.push_back((v[size_t(triangle)*3].pos+v[size_t(triangle)*3+1].pos+v[size_t(triangle)*3+2].pos)*(1.0f/3.0f));
            lampTriangles.push_back({v[size_t(triangle)*3].pos,v[size_t(triangle)*3+1].pos,v[size_t(triangle)*3+2].pos});
        }
    }
    // The instanced meshes after the flattened triangles, in object space (their instances transform them in the shaders).
    flatTriangles=triangleCount;
    for(size_t t=0;t<instanced.roughness.size();++t) {
        TriMaterial mat=material(instanced.vertices[t*3],instanced.roughness[t]);
        mat.flags=WithVegClass(mat.flags,instanced.vegClass[t]); m.push_back(mat);
        for(unsigned k=0;k<3;++k) { const auto &r=instanced.vertices[t*3+k]; v.push_back({r.pos,r.pos,r.normal,{r.uv[0],r.uv[1]}}); }
    }
    triangleCount+=unsigned(instanced.roughness.size());
    instancedMeshes=instanced.meshes; instances=instanced.instances; instanceBoxes=instanced.boxes;
    // Lightmap UVs in the same triangle order as `v`, only for --lightmap: other runs of a TEXCOORD_1 scene get no buffer.
    const auto lightmap=o.lightmap.empty() ? std::vector<LightmapUv>() : BuildLightmapUvs(raw,order,dynamic);
    if(!lightmap.empty() && lightmap.size()!=v.size()) throw std::runtime_error("Lightmap UVs do not match the scene's vertices");
    for(const auto &entry:lightmap) if(entry.atlas>=0) lightmapAtlases=std::max(lightmapAtlases,unsigned(entry.atlas)+1);
    if(!o.lightmap.empty() && !lightmapAtlases)
        throw std::runtime_error("--lightmap needs a scene with lightmap UVs (TEXCOORD_1), e.g. lab_scene_baked.glb");
    boundsMin=boundsMax=v.front().pos;
    auto grow=[&](Vec3 p) {
        boundsMin={std::min(boundsMin.x,p.x),std::min(boundsMin.y,p.y),std::min(boundsMin.z,p.z)};
        boundsMax={std::max(boundsMax.x,p.x),std::max(boundsMax.y,p.y),std::max(boundsMax.z,p.z)};
    };
    for(size_t i=0;i<size_t(flatTriangles)*3;++i) grow(v[i].pos);
    if(!instances.empty()) { grow(instanced.bounds.lo); grow(instanced.bounds.hi); }
    lightPower=RebuildLampCdf(lampSources,groups,emitters);
    lightCount=unsigned(emitters.size());
    if(!lightCount) emitters.push_back({});
    textureCount=unsigned(source.images.size()+dataImages.size());
    srvBase=d.Allocate(5+std::max(1u,textureCount));
    // GPU skinning: the layout writes the dynamic UVs into `v`, and the pass writes the vertex buffer (a UAV).
    gpuSkinned=dynamicTriangles>0 && o.gpuSkinning;
    if(gpuSkinned) gpuSkinning=std::make_unique<GpuSkinning>(d,source,dynamicOrder,v.data()+size_t(staticTriangles)*3,staticTriangles*3,o.windStrength);
    vertices=d.UploadBuffer(v.data(),v.size()*sizeof(TriVertex),gpuSkinned ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    materials=d.UploadBuffer(m.data(),m.size()*sizeof(TriMaterial));
    lights=d.UploadBuffer(emitters.data(),emitters.size()*sizeof(EmissiveTriangle));
    d.BufferSrv(vertices.Get(),srvBase,unsigned(v.size()),sizeof(TriVertex));
    const GpuInstance none{}; // t4 always has an entry
    instanceBuffer=d.UploadBuffer(instances.empty() ? &none : instances.data(),std::max<size_t>(1,instances.size())*sizeof(GpuInstance));
    d.BufferSrv(instanceBuffer.Get(),srvBase+4,unsigned(std::max<size_t>(1,instances.size())),sizeof(GpuInstance));
    if(!instancedMeshes.empty()) {
        size_t drawn=flatTriangles; for(const auto &mesh:instancedMeshes) drawn+=size_t(mesh.triangles)*mesh.instances;
        std::printf("[info] instancing: %zu meshes shared by %zu static nodes, %u triangles stored once, %zu drawn\n",
                    instancedMeshes.size(),instances.size(),triangleCount-flatTriangles,drawn-flatTriangles);
    }
    std::printf("[info] scene buffers: vertices %.1f MB, materials %.1f MB, instances %zu\n",double(v.size()*sizeof(TriVertex))/1048576.0,
                double(m.size()*sizeof(TriMaterial))/1048576.0,instances.size());
    hasLightmapUvs=!lightmap.empty();
    if(hasLightmapUvs) {
        lightmapUvs=d.UploadBuffer(lightmap.data(),lightmap.size()*sizeof(LightmapUv));
        auto unbaked=UnbakedTriangles(lightmap);
        unbakedTriangleCount=unsigned(unbaked.size()); unbakedStaticTriangles=unbakedTriangleCount-dynamicTriangles;
        if(unbaked.empty()) unbaked.push_back(0);
        unbakedTriangles=d.UploadBuffer(unbaked.data(),unbaked.size()*sizeof(unsigned));
        std::printf("[info] lightmap UVs: %zu vertices; %u static triangles without them\n",lightmap.size(),unbakedStaticTriangles);
    }
    if(gpuSkinned) {
        Animate(animationTime,animationTime);
        d.Begin(); RecordAnimation(d); d.Submit();
    } else if(dynamicTriangles) {
        animationUpload=d.Buffer(UINT64(dynamicTriangles)*3*sizeof(TriVertex),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void *mapped=nullptr; D3D12_RANGE empty{0,0}; Check(animationUpload->Map(0,&empty,&mapped),"Map animation upload");
        mappedAnimation=static_cast<TriVertex *>(mapped);
        animation->Start(mappedAnimation,dynamicOrder);
        Animate(animationTime,animationTime);
        d.Begin(); RecordAnimation(d); d.Submit();
    }
    d.BufferSrv(materials.Get(),srvBase+1,unsigned(m.size()),sizeof(TriMaterial));
    d.BufferSrv(lights.Get(),srvBase+2,unsigned(emitters.size()),sizeof(EmissiveTriangle));
    lap("scene: triangles, buffers, skinning and the pose");
    HdrImage hdr;
    if(!o.hdri.empty()) hdr=LoadHdr(o.hdri);
    else { hdr.width=hdr.height=1; hdr.pixels={0.12f,0.16f,0.23f,1}; }
    environment=d.UploadTexture(hdr.width,hdr.height,DXGI_FORMAT_R32G32B32A32_FLOAT,16,hdr.pixels.data());
    lap("scene: HDRI loaded and uploaded");
    d.TextureSrv(environment.Get(),srvBase+3);
    // Decoding 190 JPEG/PNG images one after another was most of a 20 s start: decode, filter and stage each texture (its
    // resource and upload buffer, texture_load.h) on all cores, in any order; this thread copies them in image order, in
    // batches of about 256 MB with one wait each.
    const unsigned count=std::max(1u,textureCount), workers=std::max(2u,std::thread::hardware_concurrency());
    std::vector<double> decodeSeconds(count),mipSeconds(count); double copySeconds=0, megabytes=0; auto &profile=StartupProfile::Get();
    const double texturesStart=profile.Now();
    textures.resize(count);
    std::vector<StagedTexture> batch; UINT64 batchBytes=0;
    const std::filesystem::path cache=o.textureCache; std::atomic<unsigned> cached{0};
    BuildInOrder<StagedTexture>(count,workers,2*workers,[&](unsigned i) {
        const bool colour=i<source.images.size(), cacheable=textureCount && !cache.empty();
        static const std::vector<uint8_t> none;
        const auto &encoded=textureCount ? source.images[colour ? i : dataImages[i-source.images.size()]].bytes : none;
        const std::uint64_t key=cacheable ? MipCacheKey(encoded) : 0;
        std::vector<ImageMip> mips;
        if(cacheable && ReadMipCache(cache,key,encoded.size(),colour,mips)) ++cached;
        else {
            RgbaImage image; const double start=profile.Now();
            if(textureCount) image=DecodeImage(encoded);
            else { image.width=image.height=1; image.pixels={255,255,255,255}; }
            const double decoded=profile.Now(); decodeSeconds[i]=decoded-start;
            mips=BuildMips(image.width,image.height,4,image.pixels.data(),colour);
            mipSeconds[i]=profile.Now()-decoded;
            if(cacheable) WriteMipCache(cache,key,encoded.size(),colour,mips);
        }
        return d.StageMips(colour ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM,4,mips);
    },[&](unsigned i, StagedTexture &staged) {
        textures[i]=staged.texture; d.TextureSrv(textures[i].Get(),srvBase+5+i);
        batchBytes+=staged.bytes; megabytes+=double(staged.bytes)/1048576.0; batch.push_back(std::move(staged));
        if(batchBytes<(256ull<<20) && i+1<count) return;
        const double start=profile.Now();
        d.CopyStaged(batch); batch.clear(); batchBytes=0;
        copySeconds+=profile.Now()-start;
    });
    lap("scene: textures decoded and uploaded");
    std::printf("[info] textures: %.0f MB uploaded with their mips; %u of %u from the cache %s\n",megabytes,cached.load(),count,
                cache.string().c_str());
    profile.Add("textures: decode (thread seconds)",std::accumulate(decodeSeconds.begin(),decodeSeconds.end(),0.0));
    profile.Add("textures: mips (thread seconds)",std::accumulate(mipSeconds.begin(),mipSeconds.end(),0.0));
    profile.Add("textures: copies (main thread)",copySeconds);
    profile.Add("textures: main thread waiting for the workers",profile.Now()-texturesStart-copySeconds);
    UpdateLamps(d);
    StartupMark("scene: lamps");
    unsigned alpha=flatTriangles-opaqueTriangles-dynamicOpaqueTriangles; // the stored triangles: instanced meshes count once
    for(const auto &mesh:instancedMeshes) alpha+=mesh.triangles-mesh.opaqueTriangles;
    std::printf("[info] scene: %u triangles (%u dynamic, %u alpha-masked), %u textures, %u emissive triangles\n",
        triangleCount,dynamicTriangles,alpha,textureCount,lightCount);
}
