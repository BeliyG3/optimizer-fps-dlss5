#include "scene.h"

void Scene::ApplyLamps(const std::vector<LampGroup> &settings)
{
    for(auto &g:groups) {
        g.intensity=1; g.tint={1,1,1};
        for(const auto &saved:settings) if(g.name==saved.name) { g.intensity=saved.intensity; g.tint=saved.tint; break; }
    }
}
void Scene::UpdateLamps(Device &d)
{
    // Submissions are fenced before descriptors or upload resources are replaced.
    std::vector<EmissiveTriangle> emitters;
    lightPower=RebuildLampCdf(lampSources,groups,emitters); lightCount=unsigned(emitters.size());
    if(emitters.empty()) emitters.push_back({});
    lights=d.UploadBuffer(emitters.data(),emitters.size()*sizeof(EmissiveTriangle));
    d.BufferSrv(lights.Get(),srvBase+2,unsigned(emitters.size()),sizeof(EmissiveTriangle));
    struct Factor { Vec3 rgb; float padding=0; };
    std::vector<Factor> factors;
    for(const auto &g:groups) factors.push_back({LampFactor(g)});
    if(factors.empty()) factors.push_back({{1,1,1}});
    groupBuffer=d.UploadBuffer(factors.data(),factors.size()*sizeof(Factor));
}
