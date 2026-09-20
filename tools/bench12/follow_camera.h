#pragma once
#include "camera.h"

class FollowCamera {
public:
    void Reset() { initialized=false; }
    CameraState Update(Vec3 position,Vec3 heading,const Options &options,float delta)
    {
        CameraState desired{{0,1.9f,-5},{0,1,0}};
        const Vec3 to=desired.target-desired.eye;
        const float distance=std::sqrt(Dot(to,to));
        desired.eye=desired.eye+to*(std::min(options.camDolly,distance*0.7f)/distance);
        desired.eye.y+=options.camLift; desired.target.y+=options.camLift;
        const Vec3 right=Normalize(Cross({0,1,0},heading));
        auto world=[&](Vec3 local) { return position+right*local.x+Vec3{0,local.y,0}+heading*local.z; };
        desired={world(desired.eye),world(desired.target)};
        if(!initialized) pose=desired;
        else {
            float blend=1-std::exp(-8*delta);
            pose.eye=pose.eye+(desired.eye-pose.eye)*blend;
            pose.target=pose.target+(desired.target-pose.target)*blend;
            if(Dot(desired.eye-pose.eye,desired.eye-pose.eye)+Dot(desired.target-pose.target,desired.target-pose.target)<1e-8f) pose=desired;
        }
        initialized=true; return pose;
    }
private:
    CameraState pose{};
    bool initialized=false;
};
