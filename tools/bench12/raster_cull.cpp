#include "raster_cull.h"
#include "scene_instances.h"

void SmallCull::Classify(const std::vector<InstanceBox> &boxes)
{
    smallMask.assign(boxes.size(),0); centres.resize(boxes.size()); radii.resize(boxes.size()); dropped.assign(boxes.size(),0);
    smallCount=0;
    for(size_t i=0;i<boxes.size();++i) {
        centres[i]=(boxes[i].lo+boxes[i].hi)*0.5f; radii[i]=Length(boxes[i].hi-boxes[i].lo)*0.5f;
        if(radii[i]<radius) { smallMask[i]=1; ++smallCount; }
    }
}
void SmallCull::Update(Vec3 eye, Vec3 forward)
{
    droppedCount=0;
    for(size_t i=0;i<smallMask.size();++i) {
        dropped[i]=smallMask[i] && SphereOutsideCone(eye,forward,coneDegrees,distance,centres[i],radii[i]) ? 1 : 0;
        droppedCount+=dropped[i];
    }
}
