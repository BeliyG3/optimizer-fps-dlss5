#pragma once
#include "options.h"

struct CameraState { Vec3 eye, target; };
inline CameraState CameraAt(int frame, const Options &o, Vec3 anchor)
{
    int phase=(o.camera=="static" || o.camera=="follow") ? 0 : o.camera=="yaw" ? 1 : o.camera=="forward" ? 2 : o.camera=="strafe" ? 3 : 4;
    float yaw=0, forward=0, strafe=0;
    float yawRate=o.yawSpeed/60*3.14159265f/180, moveRate=o.moveSpeed/60;
    for (int f=0; f<frame; ++f) {
        int p=o.camera=="script" ? (f/120)%5 : phase;
        if(p==1) yaw+=yawRate;
        else if(p==2) forward+=moveRate;
        else if(p==3) strafe+=moveRate;
        else if(p==4) { yaw+=yawRate*0.6f; forward+=moveRate*0.7f; }
    }
    Vec3 dir{std::sin(yaw),0,std::cos(yaw)}, right{std::cos(yaw),0,-std::sin(yaw)};
    CameraState c; c.target=dir*forward+right*strafe+Vec3{0,1,0}+anchor;
    c.eye=c.target-dir*5+Vec3{0,0.9f,0};
    if(o.cameraOverride) c={o.camPos,o.camTarget};
    Vec3 to=c.target-c.eye; float distance=std::sqrt(Dot(to,to));
    if(distance>1e-4f) c.eye=c.eye+to*(std::min(o.camDolly,distance*0.7f)/distance);
    c.eye.y+=o.camLift; c.target.y+=o.camLift;
    float ts=float(frame)/60;
    c.eye.x+=o.sway*(std::sin(ts*0.83f)+0.5f*std::sin(ts*1.91f));
    c.eye.y+=o.sway*0.6f*(std::sin(ts*0.57f+1.3f)+0.5f*std::sin(ts*1.37f));
    return c;
}
inline bool CameraMoved(const CameraState &a, const CameraState &b)
{
    // Jitter is intentionally excluded: it must not reset a static-camera reference.
    return Dot(a.eye-b.eye,a.eye-b.eye)>0 || Dot(a.target-b.target,a.target-b.target)>0;
}
inline bool CameraCut(const CameraState &a, const CameraState &b)
{
    // Continuous scripted motion keeps temporal history; jumps over 1 unit or 45 degrees reset it.
    return Dot(a.eye-b.eye,a.eye-b.eye)>1 ||
        Dot(Normalize(a.target-a.eye),Normalize(b.target-b.eye))<0.70710678f;
}
