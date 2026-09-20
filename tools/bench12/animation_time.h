#pragma once
#include <algorithm>
#include <cmath>

inline float LoopAnimation(double time, float duration)
{
    if(duration<=0) return 0;
    double wrapped=std::fmod(time,double(duration));
    return float(wrapped<0 ? wrapped+duration : wrapped);
}
struct AnimationClock {
    double time=0;
    float duration=0;
    float Current() const { return LoopAnimation(time,duration); }
    void Seek(float value) { time=LoopAnimation(value,duration); }
    void Advance(double delta, float speed, bool playing, bool step)
    {
        if(step) time+=double(speed)/60;
        else if(playing) time+=delta*double(speed);
        // Keep the double clock bounded without rounding each frame to float.
        if(duration>0) { time=std::fmod(time,double(duration)); if(time<0) time+=duration; }
    }
};
inline double AnimationDelta(bool interactive, double elapsed)
{
    return interactive ? std::clamp(elapsed,0.0,1.0/15) : 1.0/60;
}
