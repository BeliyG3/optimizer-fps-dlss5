#include "lamp_parts.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace {
unsigned Find(std::vector<unsigned> &parent, unsigned i)
{
    while(parent[i]!=i) { parent[i]=parent[parent[i]]; i=parent[i]; }
    return i;
}
// The eigenvectors of a symmetric 3x3 matrix (cyclic Jacobi rotations), as the columns of `v`.
void Eigenvectors(double m[3][3], double v[3][3])
{
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) v[i][j]=i==j ? 1 : 0;
    for(int sweep=0;sweep<64;++sweep) {
        const double off=m[0][1]*m[0][1]+m[0][2]*m[0][2]+m[1][2]*m[1][2], diagonal=m[0][0]*m[0][0]+m[1][1]*m[1][1]+m[2][2]*m[2][2];
        if(off<=1e-24*diagonal || off==0) return;
        for(int p=0;p<2;++p) for(int q=p+1;q<3;++q) {
            if(m[p][q]==0) continue;
            const double theta=(m[q][q]-m[p][p])/(2*m[p][q]);
            const double t=(theta>=0 ? 1 : -1)/(std::abs(theta)+std::sqrt(theta*theta+1)), c=1/std::sqrt(t*t+1), s=t*c;
            for(int k=0;k<3;++k) { const double kp=m[k][p], kq=m[k][q]; m[k][p]=c*kp-s*kq; m[k][q]=s*kp+c*kq; }
            for(int k=0;k<3;++k) { const double pk=m[p][k], qk=m[q][k]; m[p][k]=c*pk-s*qk; m[q][k]=s*pk+c*qk; }
            for(int k=0;k<3;++k) { const double kp=v[k][p], kq=v[k][q]; v[k][p]=c*kp-s*kq; v[k][q]=s*kp+c*kq; }
        }
    }
}
// The part's principal axes from its surface's second moment about the centroid, then its bounds along them.
void Shape(LampPart &part, const std::vector<LampSource> &sources, const std::vector<LampTriangle> &triangles)
{
    const Vec3 origin=triangles[part.sources.front()][0];
    double area=0, first[3]{}, second[3][3]{};
    for(unsigned s:part.sources) {
        const double weight=std::max(0.0f,sources[s].area);
        double p[3][3];
        for(int k=0;k<3;++k) { const Vec3 d=triangles[s][k]-origin; p[k][0]=d.x; p[k][1]=d.y; p[k][2]=d.z; }
        // A uniform triangle's E[x x^T] = (a a^T + b b^T + c c^T + (a+b+c)(a+b+c)^T) / 12.
        for(int i=0;i<3;++i) {
            const double si=p[0][i]+p[1][i]+p[2][i];
            first[i]+=weight*si/3;
            for(int j=0;j<3;++j) second[i][j]+=weight*(p[0][i]*p[0][j]+p[1][i]*p[1][j]+p[2][i]*p[2][j]+si*(p[0][j]+p[1][j]+p[2][j]))/12;
        }
        area+=weight;
    }
    if(!(area>0)) throw std::runtime_error("A lamp part has no area");
    double covariance[3][3], axes[3][3];
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) covariance[i][j]=second[i][j]/area-first[i]*first[j]/(area*area);
    Eigenvectors(covariance,axes);
    float low[3], high[3]; Vec3 axis[3];
    for(int k=0;k<3;++k) { axis[k]=Normalize({float(axes[0][k]),float(axes[1][k]),float(axes[2][k])}); low[k]=1e30f; high[k]=-1e30f; }
    for(unsigned s:part.sources) for(const Vec3 &corner:triangles[s]) for(int k=0;k<3;++k) {
        const float x=Dot(corner-origin,axis[k]); low[k]=std::min(low[k],x); high[k]=std::max(high[k],x);
    }
    int along=0;
    for(int k=1;k<3;++k) if(high[k]-low[k]>high[along]-low[along]) along=k;
    int wide=(along+1)%3, thin=(along+2)%3;
    if(high[thin]-low[thin]>high[wide]-low[wide]) std::swap(wide,thin);
    const float length=high[along]-low[along], extent0=high[wide]-low[wide], extent1=high[thin]-low[thin];
    Vec3 centre=origin;
    for(int k=0;k<3;++k) centre=centre+axis[k]*(0.5f*(low[k]+high[k]));
    part.a=centre-axis[along]*(0.5f*length); part.b=centre+axis[along]*(0.5f*length);
    part.radius=0.25f*(extent0+extent1);
    part.tube=length>0 && length>=TubeMinAspect*extent0 && extent0>=TubeMinFace*extent1;
    part.normal=axis[thin];
    part.faceShare=float(length*extent0/area); part.sideShare=float(length*extent1/area);
    part.twoSided=std::abs(part.normal.y)<0.70710678f;
    if(!part.twoSided && part.normal.y>0) part.normal=part.normal*-1.0f;
}
}

std::vector<LampPart> FindLampParts(const std::vector<LampSource> &sources, const std::vector<LampTriangle> &triangles)
{
    if(sources.size()!=triangles.size()) throw std::runtime_error("Lamp triangles do not match lamp sources");
    std::vector<unsigned> parent(sources.size());
    std::iota(parent.begin(),parent.end(),0u);
    std::map<std::tuple<unsigned,long long,long long,long long>,unsigned> owner; // a quantised position's first source
    for(unsigned i=0;i<sources.size();++i) for(const Vec3 &corner:triangles[i]) {
        const auto key=std::make_tuple(sources[i].group,std::llround(double(corner.x)*1e4),std::llround(double(corner.y)*1e4),
                                       std::llround(double(corner.z)*1e4));
        const auto [it,inserted]=owner.try_emplace(key,i);
        if(!inserted) { const unsigned a=Find(parent,i), b=Find(parent,it->second); if(a!=b) parent[std::max(a,b)]=std::min(a,b); }
    }
    std::vector<LampPart> parts; std::vector<unsigned> partOf(sources.size(),~0u);
    for(unsigned i=0;i<sources.size();++i) {
        const unsigned root=Find(parent,i); // the smallest index of its part, so parts come in the order of their first source
        if(partOf[root]==~0u) { partOf[root]=unsigned(parts.size()); parts.push_back({}); parts.back().group=sources[i].group; }
        parts[partOf[root]].sources.push_back(i);
    }
    for(auto &part:parts) Shape(part,sources,triangles);
    return parts;
}
