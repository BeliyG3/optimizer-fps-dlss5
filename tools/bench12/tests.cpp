#include "camera.h"
#include "image.h"
#include "device.h"
#include "ngx_contract.h"
#include "shaders/reservoir.hlsli"
#include "shaders/specular_guide.hlsli"
#pragma warning(push)
#pragma warning(disable: 4456 4457)
#include "../bench/pw_gltf.h"
#pragma warning(pop)
#include <fstream>
void MaterialChecks(const std::filesystem::path &directory);
void InteractiveChecks(const std::filesystem::path &directory);
void AnimationChecks(const std::filesystem::path &directory);

namespace {
void Require(bool condition, const char *message) { if(!condition) throw std::runtime_error(message); }
bool Near(float a, float b) { return std::abs(a-b)<1e-4f; }
Options Parse(std::vector<std::string> args)
{
    std::vector<char *> pointers; for(auto &arg:args) pointers.push_back(arg.data());
    return ParseOptions(int(pointers.size()),pointers.data());
}
void Reject(std::vector<std::string> args)
{
    bool rejected=false; try { (void)Parse(args); } catch(const std::exception &) { rejected=true; }
    Require(rejected,"Invalid options were accepted");
}
void CameraChecks()
{
    Options o; o.camera="static";
    auto camera=CameraAt(0,o,{3,0,7}); Require(Near(camera.eye.x,3) && Near(camera.eye.z,2),"Character anchor offset");
    Require(!CameraMoved(camera,CameraAt(100,o,{3,0,7})),"Static camera resets accumulation");
    o.sway=0.1f; Require(CameraMoved(camera,CameraAt(1,o,{3,0,7})),"Sway must reset accumulation"); o.sway=0;
    o.camera="script"; Require(!CameraMoved(CameraAt(0,o,{}),CameraAt(120,o,{})),"Script static phase");
    Require(CameraMoved(CameraAt(120,o,{}),CameraAt(121,o,{})),"Script yaw phase");
    o.camera="static"; o.camDolly=100;
    camera=CameraAt(0,o,{}); float distance=std::sqrt(Dot(camera.target-camera.eye,camera.target-camera.eye));
    Require(Near(distance,std::sqrt(25.81f)*0.3f),"Dolly must retain 30 percent of target distance");
    for(bool reverse:{false,true}) {
        auto projection=Perspective(60,16.0f/9,reverse);
        auto depth=[&](float z){ return (projection.m[2][2]*z+projection.m[2][3])/z; };
        Require(Near(depth(0.1f),reverse ? 1.0f : 0.0f),"Near depth convention");
        Require(Near(depth(300),reverse ? 0.0f : 1.0f),"Far depth convention");
    }
    // A camera moving right sends a stationary point left; current->previous motion is positive.
    Mat4 projection=Perspective(60,1,false);
    Mat4 now=Mul(projection,LookAt({1,0,0},{1,0,1})), old=Mul(projection,LookAt({0,0,0},{0,0,1}));
    auto pixelX=[](const Mat4 &m){ float x=m.m[0][2]*5+m.m[0][3], w=m.m[3][2]*5+m.m[3][3]; return (x/w*0.5f+0.5f)*100; };
    Require(pixelX(old)-pixelX(now)>0,"Current-to-previous pixel motion sign");
    Require(Near(Halton(1,2)-0.5f,0) && Near(Halton(1,3)-0.5f,-1.0f/6),"Halton jitter convention");
}
void HdrChecks(const std::filesystem::path &directory)
{
    auto path=directory/"test_rgbe.hdr";
    {
        std::ofstream file(path,std::ios::binary); file<<"#?RADIANCE\r\nFORMAT=32-bit_rle_rgbe\r\n\r\n-Y 1 +X 8\r\n";
        const unsigned char bytes[]={2,2,0,8,136,128,136,64,136,32,136,129};
        file.write(reinterpret_cast<const char *>(bytes),sizeof(bytes));
    }
    auto image=LoadHdr(path.string()); Require(image.width==8 && image.height==1,"HDR RLE dimensions");
    Require(Near(image.pixels[0],1.00390625f) && Near(image.pixels[1],0.50390625f),"HDR RLE radiance");
    {
        std::ofstream file(path,std::ios::binary); file<<"#?RADIANCE\n\n-Y 1 +X 1\n";
        const unsigned char bytes[]={128,64,32,129}; file.write(reinterpret_cast<const char *>(bytes),sizeof(bytes));
    }
    image=LoadHdr(path.string()); Require(Near(image.pixels[0],1.00390625f),"HDR flat RGBE radiance");
    { std::ofstream file(path,std::ios::binary); file<<"#?RADIANCE\n\n-Y 1 +X 8\n"; }
    bool rejected=false; try { (void)LoadHdr(path.string()); } catch(const std::exception &) { rejected=true; }
    Require(rejected,"Truncated HDR accepted"); std::filesystem::remove(path);
}
void NgxChecks()
{
    Require(NgxQuality(0.5f)==NVSDK_NGX_PerfQuality_Value_MaxPerf,"Half resolution must select Performance");
    Require(NgxQuality(1)==NVSDK_NGX_PerfQuality_Value_DLAA,"Native resolution must select DLAA");
    Require(NgxQuality(1.0f/3)==NVSDK_NGX_PerfQuality_Value_UltraPerformance,"Ultra Performance ratio");
    Require(NgxQuality(0.58f)==NVSDK_NGX_PerfQuality_Value_Balanced,"Balanced ratio");
    Require(NgxQuality(2.0f/3)==NVSDK_NGX_PerfQuality_Value_MaxQuality,"Quality ratio");
    Options o; o.camera="yaw";
    Require(!CameraCut(CameraAt(0,o,{}),CameraAt(1,o,{})),"Smooth yaw must preserve NGX history");
    CameraState pose{{2,3,4},{3,3,5}}, jump=pose; jump.eye.x+=2; jump.target.x+=2;
    Require(CameraCut(pose,jump),"Translation cut must reset NGX history");
    jump=pose; jump.target=pose.eye+Vec3{-1,0,-1};
    Require(CameraCut(pose,jump),"Rotation cut must reset NGX history");
    auto view=LookAt(pose.eye,pose.target), ngxView=NgxMatrix(view);
    // A world-space camera position must map to the origin when LEFT multiplied into NGX's matrix.
    const float eye[]={pose.eye.x,pose.eye.y,pose.eye.z,1};
    for(int col=0;col<4;++col) {
        float value=0; for(int row=0;row<4;++row) value+=eye[row]*ngxView.m[row][col];
        Require(Near(value,col==3 ? 1.0f : 0.0f),"NGX world-to-view row-vector convention");
    }
    for(bool reverse:{false,true}) {
        o.reverse=reverse; auto create=RrCreate(960,540,1920,1080,o);
        Require(create.InWidth==960 && create.InHeight==540 && create.InTargetWidth==1920 && create.InTargetHeight==1080,"RR dimensions");
        Require(create.InFeatureCreateFlags==(reverse ? 11 : 3),"RR HDR/low-res motion/reverse-depth flags only");
        Require(create.InRoughnessMode==NVSDK_NGX_DLSS_Roughness_Mode_Packed &&
            create.InUseHWDepth==NVSDK_NGX_DLSS_Depth_Type_HW &&
            create.InDenoiseMode==NVSDK_NGX_DLSS_Denoise_Mode_DLUnified && !create.InEnableOutputSubrects,"RR modes");
        Mat4 p=NgxMatrix(Perspective(60,16.0f/9,reverse));
        auto depth=[&](float z){ return (z*p.m[2][2]+p.m[3][2])/(z*p.m[2][3]+p.m[3][3]); };
        Require(Near(depth(0.1f),reverse ? 1.0f : 0.0f) && Near(depth(300),reverse ? 0.0f : 1.0f),"RR projection agrees with hardware-depth guide");
    }
    for(const char *mode:{"linear-negative","linear-positive"}) {
        auto linear=Parse({"bench","--depth",mode});
        Require(linear.linearDepth==(std::string(mode)=="linear-negative" ? -1 : 1),"Linear depth sign");
        Require(RrCreate(960,540,1920,1080,linear).InUseHWDepth==NVSDK_NGX_DLSS_Depth_Type_Linear,"RR linear depth contract");
    }
    NgxFrame frame; int resourceTags[8]{};
    for(unsigned i=0;i<7;++i) frame.inputs[i]=reinterpret_cast<ID3D12Resource *>(&resourceTags[i]);
    frame.output=reinterpret_cast<ID3D12Resource *>(&resourceTags[7]);
    frame.width=960; frame.height=540; frame.jitterX=-0.25f; frame.jitterY=1.0f/6; frame.reset=true;
    auto p=RrEvaluate(frame);
    Require(p.pInColor==frame.inputs[0] && p.pInDepth==frame.inputs[1] && p.pInMotionVectors==frame.inputs[2] && p.pInOutput==frame.output,"RR colour/depth/motion binding");
    Require(p.pInNormals==frame.inputs[3] && p.pInDiffuseAlbedo==frame.inputs[4] && p.pInSpecularAlbedo==frame.inputs[5] &&
        p.pInSpecularHitDistance==frame.inputs[6] && p.pInRoughness==nullptr,"RR guide binding and packed roughness");
    Require(p.InMVScaleX==1 && p.InMVScaleY==1 && p.InJitterOffsetX==frame.jitterX && p.InJitterOffsetY==frame.jitterY,"RR pixel-space motion and jitter");
    frame.mvScaleX=-1129; frame.mvScaleY=635;
    const auto ndc=RrEvaluate(frame);
    Require(ndc.InMVScaleX==-1129 && ndc.InMVScaleY==635,"RR preserves asymmetric NDC motion scale");
    Require(Parse({"bench","--motion","ndc"}).motion=="ndc","NDC motion option");
    Reject({"bench","--motion","invalid"});
    Require(p.InRenderSubrectDimensions.Width==960 && p.InRenderSubrectDimensions.Height==540 && p.InReset==1,"RR subrect/reset");
    Require(p.pInWorldToViewMatrix==&frame.worldToView.m[0][0] && p.pInViewToClipMatrix==&frame.viewToClip.m[0][0],"RR matrix lifetime");
    Require(Near(p.InFrameTimeDeltaInMsec,1000.0f/60) && p.InPreExposure==1 && p.InExposureScale==1,"RR fixed simulation time/exposure");
    Require(!p.pInExposureTexture && !p.pInDiffuseHitDistance && !p.pInMotionVectorsReflections &&
        !p.InColorSubrectBase.X && !p.InColorSubrectBase.Y && !p.InOutputSubrectBase.X && !p.InOutputSubrectBase.Y,"RR optional fields default to zero");
    frame.reset=false; Require(RrEvaluate(frame).InReset==0,"Continuous frames keep RR history");
}
void ReservoirChecks()
{
    Require(SpecularGuide(0.1f,0.1f)>SpecularGuide(0.1f,1),"Specular guide must include grazing Fresnel");
    Require(SpecularGuide(1,1)<SpecularGuide(0.1f,1),"Specular guide must integrate roughness");
    for(int r=0;r<=100;++r) for(int v=0;v<=100;++v) {
        float value=SpecularGuide(float(r)/100,float(v)/100);
        Require(std::isfinite(value) && value>=0,"Specular BRDF fit must remain finite/nonnegative");
    }
    LampReservoir empty{};
    Require(!UpdateLampReservoir(empty,0,0.2f,0) && LampNormalization(empty,8)==0,"Zero-target reservoir");
    LampReservoir one{}; Require(UpdateLampReservoir(one,3,0.25f,0.99f),"First nonzero RIS candidate");
    Require(Near(LampNormalization(one,1),4),"M=1 must recover inverse source PDF");
    Require(Near(LampNormalization(one,8),0.5f),"Zero candidates must still count in M");
    // Numerical integration over a discrete light domain: proposal != target, one black
    // emitter and one occluded emitter. Visibility must not affect reservoir selection.
    const float pdf[]={0.2f,0.3f,0.5f}, target[]={0,2,5}, contribution[]={0,2,0};
    unsigned rng=17;
    auto random=[&]() { rng=rng*1664525u+1013904223u; return float(rng>>8)*(1.0f/16777216); };
    for(unsigned candidates:{1u,8u}) {
        double sum=0;
        for(unsigned trial=0;trial<100000;++trial) {
            LampReservoir reservoir{}; unsigned selected=0;
            for(unsigned j=0;j<candidates;++j) {
                float u=random(); unsigned lamp=u<0.2f ? 0u : u<0.5f ? 1u : 2u;
                if(UpdateLampReservoir(reservoir,target[lamp],pdf[lamp],random())) selected=lamp;
            }
            sum+=contribution[selected]*LampNormalization(reservoir,candidates);
        }
        Require(std::abs(sum/100000-2)<0.04,"RIS must integrate visible contribution without bias");
    }
}
void SceneChecks(const char *path)
{
    pwgltf::Scene scene; Require(pwgltf::Load(path,scene),"Cannot load verification GLB");
    std::vector<pwgltf::Vertex> vertices; std::vector<Vec3> previous;
    pwgltf::BuildVertices(scene,0,0,vertices,previous);
    const auto dynamic=pwgltf::Primitives(scene,1);
    std::printf("[test] GLB: %zu dynamic primitives, animation duration %.3f s\n",dynamic.size(),double(scene.animationEnd));
    if(dynamic.empty()) {
        std::vector<pwgltf::Vertex> fixed; std::vector<Vec3> fixedPrevious; pwgltf::VertexWorkspace scratch;
        pwgltf::BuildVerticesSubset(scene,1,0.25f,pwgltf::Primitives(scene,0),scratch,fixed,fixedPrevious);
        Require(fixed.size()==vertices.size(),"Empty dynamic set preserves full triangle list");
        for(size_t i=0;i<fixed.size();++i) {
            Require(Dot(fixed[i].pos-vertices[i].pos,fixed[i].pos-vertices[i].pos)==0,"Static geometry unchanged at later time");
            Require(Dot(fixed[i].pos-fixedPrevious[i],fixed[i].pos-fixedPrevious[i])==0,"Static subset has zero object motion");
        }
    }
    Require(!vertices.empty() && vertices.size()%3==0 && previous.size()==vertices.size(),"GLB triangle list");
    size_t emitters=0, masked=0;
    for(size_t i=0;i<vertices.size();++i) {
        Require(Dot(vertices[i].pos-previous[i],vertices[i].pos-previous[i])==0,"Static scene has object motion");
        if(i%3==0) { emitters+=vertices[i].material==-4 ? 1 : 0; masked+=vertices[i].alphaMask ? 1 : 0; }
    }
    Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"Initialize WIC test");
    for(const auto &image:scene.images) { auto rgba=DecodeImage(image.bytes); Require(!rgba.pixels.empty(),"WIC returned empty image"); }
    CoUninitialize();
    std::printf("[test] GLB: %zu triangles, %zu masked, %zu emissive; %zu images decoded\n",vertices.size()/3,masked,emitters,scene.images.size());
}
}
void AnimationBenchmark(const std::filesystem::path &path);
void AnimationWriteCombinedBenchmark(const std::filesystem::path &path);
int main(int argc, char **argv)
{
    try {
        auto o=Parse({"bench","40","--camera","static","--cam-dolly","2.9","--sun-dir","-0.30,-0.80,0.52","--dump","0,30,39"});
        Require(o.frames==40 && o.dumps.size()==3 && Near(o.sunDir.z,0.52f),"Verification options");
        Require(o.bounces==4 && o.spp==1 && o.lightCandidates==8 && !o.vsync && o.upscaler=="none","Milestone 2 defaults");
        Require(Near(o.haze,0.012f) && Near(o.hazeG,0.6f) && Near(o.bloom,0.06f) && !o.autoExposure && o.tonemap=="aces","Milestone 3 defaults");
        auto display=Parse({"bench","--haze","0","--haze-g","-0.5","--bloom","0","--auto-exposure","1","--tonemap","neutral"});
        Require(display.haze==0 && display.hazeG==-0.5f && display.bloom==0 && display.autoExposure && display.tonemap=="neutral","Display/medium options");
        Reject({"bench","--haze","-1"}); Reject({"bench","--haze-g","1"}); Reject({"bench","--haze-g","-1"});
        Reject({"bench","--bloom","-1"}); Reject({"bench","--auto-exposure","2"}); Reject({"bench","--tonemap","bad"});
        for(const char *mode:{"none","sr","rr"}) {
            auto parsed=Parse({"bench","--upscaler",mode,"--light-candidates","1","--vsync","1"});
            Require(parsed.upscaler==mode && parsed.lightCandidates==1 && parsed.vsync,"New sampling/presentation options");
        }
        Reject({"bench","--upscaler","dlss"}); Reject({"bench","--vsync","2"}); Reject({"bench","--vsync","-1"});
        Reject({"bench","--light-candidates","0"}); Reject({"bench","--light-candidates","1025"});
        Reject({"bench","--upscaler"}); Reject({"bench","--light-candidates","1.5"});
        Reject({"bench","--jitter","2"}); Reject({"bench","--depth","invalid"}); Reject({"bench","--width","1920x"});
        Reject({"bench","--fov","nan"}); Reject({"bench","--cam-pos","1,2,3"}); Reject({"bench","--bounces","9"});
        Reject({"bench","40","--dump","40"}); Reject({"bench","--sun-dir","0,0,0"}); Reject({"bench","--spp"});
        CameraChecks(); NgxChecks(); ReservoirChecks(); HdrChecks(std::filesystem::absolute(argv[0]).parent_path());
        MaterialChecks(std::filesystem::absolute(argv[0]).parent_path());
        InteractiveChecks(std::filesystem::absolute(argv[0]).parent_path());
        AnimationChecks(std::filesystem::absolute(argv[0]).parent_path());
        auto animation=Parse({"bench","--camera","follow","--follow-node","hero","--anim-speed","-0.5","--anim-start","3","--blas-rebuild","10"});
        Require(animation.camera=="follow" && animation.followNode=="hero" && animation.animSpeed==-0.5f && animation.animStart==3 && animation.blasRebuild==10,"Animation CLI options");
        Reject({"bench","--anim-speed","nan"}); Reject({"bench","--anim-start","inf"});
        Reject({"bench","--blas-rebuild","0"}); Reject({"bench","--anim-playing","2"});
        const auto lab=argc>1 ? std::filesystem::path(argv[1]) : std::filesystem::absolute(argv[0]).parent_path()/"../../bench/assets/lab_scene.glb";
        AnimationBenchmark(lab); AnimationWriteCombinedBenchmark(lab);
        if(argc>1) SceneChecks(argv[1]);
        std::puts("[test] CPU checks passed"); return 0;
    } catch(const std::exception &error) { std::fprintf(stderr,"[test fail] %s\n",error.what()); return 1; }
}
