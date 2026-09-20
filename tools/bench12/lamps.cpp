#include "lamps.h"
#include <stdexcept>

std::string LampName(std::string name)
{
    if(name.size()>=4 && name[name.size()-4]=='.' &&
        name[name.size()-3]>='0' && name[name.size()-3]<='9' &&
        name[name.size()-2]>='0' && name[name.size()-2]<='9' &&
        name.back()>='0' && name.back()<='9') name.resize(name.size()-4);
    return name.empty() ? "(unnamed)" : name;
}
unsigned FindLampGroup(std::vector<LampGroup> &groups, const std::string &name)
{
    auto canonical=LampName(name);
    for(unsigned i=0;i<groups.size();++i) if(groups[i].name==canonical) return i;
    groups.push_back({canonical}); return unsigned(groups.size()-1);
}
Vec3 LampFactor(const LampGroup &g) { return g.tint*g.intensity; }
float RebuildLampCdf(const std::vector<LampSource> &sources, std::vector<LampGroup> &groups,
                     std::vector<EmissiveTriangle> &lights)
{
    lights.clear();
    for(auto &g:groups) { g.power=0; g.triangles=0; }
    float total=0;
    for(const auto &s:sources) {
        auto &g=groups.at(s.group); Vec3 f=LampFactor(g);
        float power=s.area*Dot({s.emission.x*f.x,s.emission.y*f.y,s.emission.z*f.z},{0.2126f,0.7152f,0.0722f});
        if(!std::isfinite(power) || power<0) throw std::runtime_error("Invalid lamp power");
        ++g.triangles; g.power+=power;
        if(power>0) { total+=power; lights.push_back({s.triangle,s.area,total,power}); }
    }
    if(!std::isfinite(total)) throw std::runtime_error("Lamp power overflow");
    if(total>0) { for(auto &light:lights) light.cdf/=total; lights.back().cdf=1; }
    return total;
}
